// FakeDoipServer — a minimal, in-repo, fault-injecting DoIP+UDS test
// double. NOT the user's separate DoIP_ECU_Simulator project (that's a
// different repo, not available here) — this is a lightweight fixture that
// speaks just enough of the wire protocol (via doip_protocol.hpp, the same
// framing code the real transport uses) to drive real socket-level tests of
// adapters/uds_doip, including fault modes a real simulator would need to be
// coaxed into: no response, malformed/truncated frames, NRC storms, and
// 0x78 response-pending storms.
//
// Per docs/DESIGN.md: "Build a fault-injecting DoIP simulator BEFORE the session
// manager. Otherwise you debug against a simulator that never reproduces
// the failure." This exists so that constraint is satisfiable without
// access to the external DoIP_ECU_Simulator asset.
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "sovd/uds_doip/doip_protocol.hpp"

namespace sovd::uds_doip::test {

enum class FaultMode {
    None,                    // respond via the configured request/response table
    NoResponse,              // ack the request, then go silent (never send the real response)
    MalformedFrame,          // ack, then send bytes that don't parse as a DoIP frame
    TruncatedFrame,          // ack, send a header claiming more payload than actually follows, then close
    NrcStorm,                // ack, then always answer with a fixed NRC regardless of request
    ResponsePendingStorm,    // ack, then NRC 0x78 some number of times, then the real (or a fallback) response
    RoutingActivationDenied, // fail routing activation itself
};

class FakeDoipServer {
public:
    explicit FakeDoipServer(uint16_t ecu_logical_address = 0x0E80) : ecu_logical_address_(ecu_logical_address) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

        socklen_t len = sizeof(addr);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        ::listen(listen_fd_, 4);

        running_ = true;
        thread_ = std::thread([this] { accept_loop(); });
    }

    ~FakeDoipServer() {
        running_ = false;
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        int client = current_client_fd_.load();
        if (client >= 0) {
            ::shutdown(client, SHUT_RDWR);
            ::close(client);
        }
        if (thread_.joinable()) thread_.join();
    }

    FakeDoipServer(const FakeDoipServer &) = delete;
    FakeDoipServer &operator=(const FakeDoipServer &) = delete;

    int port() const { return port_; }

    void set_response(std::vector<uint8_t> request, std::vector<uint8_t> response) {
        std::lock_guard<std::mutex> lk(mtx_);
        responses_[std::move(request)] = std::move(response);
    }

    void set_fault_mode(FaultMode mode, uint8_t nrc = 0x22, int pending_count = 0) {
        std::lock_guard<std::mutex> lk(mtx_);
        fault_mode_ = mode;
        fault_nrc_ = nrc;
        fault_pending_count_ = pending_count;
    }

    void set_routing_activation_response_code(uint8_t code) {
        std::lock_guard<std::mutex> lk(mtx_);
        routing_response_code_ = code;
    }

private:
    static void append_u16(std::vector<uint8_t> &out, uint16_t v) {
        out.push_back(static_cast<uint8_t>(v >> 8));
        out.push_back(static_cast<uint8_t>(v & 0xFF));
    }

    static bool read_exact(int fd, uint8_t *buf, size_t len) {
        size_t got = 0;
        while (got < len) {
            ssize_t n = ::recv(fd, buf + got, len - got, 0);
            if (n <= 0) return false;
            got += static_cast<size_t>(n);
        }
        return true;
    }

    void accept_loop() {
        while (running_) {
            int client = ::accept(listen_fd_, nullptr, nullptr);
            if (client < 0) {
                if (!running_) return;
                continue;
            }
            current_client_fd_.store(client);
            handle_connection(client);
            current_client_fd_.store(-1);
            ::close(client);
        }
    }

    void send_ack(int client, uint16_t source, uint16_t target, bool positive) {
        std::vector<uint8_t> payload;
        append_u16(payload, source);
        append_u16(payload, target);
        payload.push_back(0x00);

        std::vector<uint8_t> out;
        protocol::append_header(out,
                                 positive ? protocol::PayloadType::DiagnosticMessagePositiveAck
                                          : protocol::PayloadType::DiagnosticMessageNegativeAck,
                                 static_cast<uint32_t>(payload.size()));
        out.insert(out.end(), payload.begin(), payload.end());
        ::send(client, out.data(), out.size(), 0);
    }

    void send_diagnostic_response(int client, uint16_t from_addr, uint16_t to_addr,
                                   const std::vector<uint8_t> &uds_bytes) {
        auto frame = protocol::encode_diagnostic_message(from_addr, to_addr, uds_bytes);
        ::send(client, frame.data(), frame.size(), 0);
    }

    void handle_diagnostic_request(int client, uint16_t req_source, uint16_t req_target,
                                    const std::vector<uint8_t> &uds_request) {
        FaultMode mode;
        uint8_t nrc;
        int pending_count;
        std::vector<uint8_t> configured_response;
        bool has_configured = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            mode = fault_mode_;
            nrc = fault_nrc_;
            pending_count = fault_pending_count_;
            auto it = responses_.find(uds_request);
            if (it != responses_.end()) {
                configured_response = it->second;
                has_configured = true;
            }
        }

        send_ack(client, req_source, req_target, true);
        if (mode == FaultMode::NoResponse) return;

        if (mode == FaultMode::MalformedFrame) {
            // A full header's worth of bytes (not fewer) with an invalid
            // protocol-version byte, so decode_header() actually rejects it
            // instead of the client just timing out waiting for the rest of
            // a header that will never arrive.
            std::vector<uint8_t> garbage = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            ::send(client, garbage.data(), garbage.size(), 0);
            return;
        }

        if (mode == FaultMode::TruncatedFrame) {
            std::vector<uint8_t> out;
            protocol::append_header(out, protocol::PayloadType::DiagnosticMessage, 20); // claims 20, sends 3
            out.insert(out.end(), {0x01, 0x02, 0x03});
            ::send(client, out.data(), out.size(), 0);
            return;
        }

        uint8_t sid = uds_request.empty() ? 0x00 : uds_request[0];
        uint16_t my_addr = req_target;   // we (the ECU) respond from the address the request targeted
        uint16_t their_addr = req_source; // back to whoever sent the request

        if (mode == FaultMode::NrcStorm) {
            send_diagnostic_response(client, my_addr, their_addr, {0x7F, sid, nrc});
            return;
        }

        if (mode == FaultMode::ResponsePendingStorm) {
            for (int i = 0; i < pending_count; ++i) {
                send_diagnostic_response(client, my_addr, their_addr, {0x7F, sid, 0x78});
            }
            send_diagnostic_response(client, my_addr, their_addr,
                                      has_configured ? configured_response
                                                      : std::vector<uint8_t>{0x7F, sid, 0x22});
            return;
        }

        // FaultMode::None
        send_diagnostic_response(client, my_addr, their_addr,
                                  has_configured ? configured_response : std::vector<uint8_t>{0x7F, sid, 0x11});
    }

    void handle_connection(int client) {
        uint8_t header_buf[protocol::kHeaderSize];
        if (!read_exact(client, header_buf, sizeof(header_buf))) return;
        protocol::Header header{};
        if (!protocol::decode_header(header_buf, sizeof(header_buf), header)) return;
        if (header.payload_type != protocol::PayloadType::RoutingActivationRequest) return;

        std::vector<uint8_t> req_payload(header.payload_length);
        if (!req_payload.empty() && !read_exact(client, req_payload.data(), req_payload.size())) return;
        uint16_t tester_addr =
            req_payload.size() >= 2 ? static_cast<uint16_t>((req_payload[0] << 8) | req_payload[1]) : 0;

        FaultMode mode;
        uint8_t routing_code;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            mode = fault_mode_;
            routing_code = routing_response_code_;
        }
        if (mode == FaultMode::RoutingActivationDenied) routing_code = 0x00; // unknown source address, etc.

        std::vector<uint8_t> resp_payload;
        append_u16(resp_payload, tester_addr);
        append_u16(resp_payload, ecu_logical_address_);
        resp_payload.push_back(routing_code);
        resp_payload.insert(resp_payload.end(), {0x00, 0x00, 0x00, 0x00});

        std::vector<uint8_t> out;
        protocol::append_header(out, protocol::PayloadType::RoutingActivationResponse,
                                 static_cast<uint32_t>(resp_payload.size()));
        out.insert(out.end(), resp_payload.begin(), resp_payload.end());
        ::send(client, out.data(), out.size(), 0);

        if (routing_code != 0x10) return; // failed activation: real entities close the connection here

        while (running_) {
            uint8_t hbuf[protocol::kHeaderSize];
            if (!read_exact(client, hbuf, sizeof(hbuf))) return;
            protocol::Header h{};
            if (!protocol::decode_header(hbuf, sizeof(hbuf), h)) return;
            if (h.payload_type != protocol::PayloadType::DiagnosticMessage) return;

            std::vector<uint8_t> payload(h.payload_length);
            if (!payload.empty() && !read_exact(client, payload.data(), payload.size())) return;
            protocol::DiagnosticMessage msg{};
            if (!protocol::decode_diagnostic_message(payload.data(), payload.size(), msg)) return;

            handle_diagnostic_request(client, msg.source_address, msg.target_address, msg.uds_payload);
        }
    }

    uint16_t ecu_logical_address_;
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> current_client_fd_{-1};
    std::thread thread_;

    std::mutex mtx_;
    std::map<std::vector<uint8_t>, std::vector<uint8_t>> responses_;
    FaultMode fault_mode_ = FaultMode::None;
    uint8_t fault_nrc_ = 0x22;
    int fault_pending_count_ = 0;
    uint8_t routing_response_code_ = 0x10;
};

} // namespace sovd::uds_doip::test
