#include "sovd/uds_doip/doip_transport.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "sovd/uds_doip/doip_protocol.hpp"

namespace sovd::uds_doip {

using namespace protocol;

// Diagnostic messages are never anywhere near this size in practice; caps a
// malicious/malfunctioning peer's claimed payload_length from driving an
// unbounded allocation before we've even validated anything about the frame.
constexpr size_t kMaxPayloadLength = 64 * 1024;

std::string transport_error_to_string(TransportError e) {
    switch (e) {
        case TransportError::None: return "none";
        case TransportError::ConnectFailed: return "connect_failed";
        case TransportError::RoutingActivationFailed: return "routing_activation_failed";
        case TransportError::Timeout: return "timeout";
        case TransportError::MalformedFrame: return "malformed_frame";
        case TransportError::ConnectionClosed: return "connection_closed";
        case TransportError::NegativeAck: return "negative_ack";
    }
    return "unknown";
}

DoipTransport::DoipTransport(TransportConfig config) : config_(std::move(config)) {}

DoipTransport::~DoipTransport() { disconnect(); }

bool DoipTransport::is_connected() const { return sock_ >= 0; }

void DoipTransport::disconnect() {
    if (sock_ >= 0) {
        ::close(sock_);
        sock_ = -1;
    }
}

bool DoipTransport::connect(TransportError *err) {
    if (is_connected()) return true;
    if (err) *err = TransportError::None;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (err) *err = TransportError::ConnectFailed;
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);
    if (::inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        if (err) *err = TransportError::ConnectFailed;
        return false;
    }

    // Non-blocking connect with an explicit timeout, rather than trusting
    // the OS default (which can be tens of seconds) — a hung gateway
    // shouldn't hang the caller past what it configured.
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        ::close(fd);
        if (err) *err = TransportError::ConnectFailed;
        return false;
    }

    if (rc < 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        timeval tv{};
        tv.tv_sec = config_.connect_timeout_ms / 1000;
        tv.tv_usec = (config_.connect_timeout_ms % 1000) * 1000;

        rc = ::select(fd + 1, nullptr, &wfds, nullptr, &tv);
        if (rc <= 0) {
            ::close(fd);
            if (err) *err = TransportError::ConnectFailed;
            return false;
        }
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (so_error != 0) {
            ::close(fd);
            if (err) *err = TransportError::ConnectFailed;
            return false;
        }
    }

    fcntl(fd, F_SETFL, flags); // back to blocking; SO_RCVTIMEO/SO_SNDTIMEO cover subsequent I/O

    timeval rtv{};
    rtv.tv_sec = config_.read_timeout_ms / 1000;
    rtv.tv_usec = (config_.read_timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &rtv, sizeof(rtv));

    sock_ = fd;

    auto activation = encode_routing_activation_request(config_.tester_logical_address, 0x00);
    if (::send(sock_, activation.data(), activation.size(), 0) != static_cast<ssize_t>(activation.size())) {
        disconnect();
        if (err) *err = TransportError::ConnectFailed;
        return false;
    }

    uint8_t header_buf[kHeaderSize];
    if (read_exact(header_buf, kHeaderSize) != ReadResult::Ok) {
        disconnect();
        if (err) *err = TransportError::Timeout;
        return false;
    }
    Header header{};
    if (!decode_header(header_buf, kHeaderSize, header) ||
        header.payload_type != PayloadType::RoutingActivationResponse || header.payload_length > kMaxPayloadLength) {
        disconnect();
        if (err) *err = TransportError::MalformedFrame;
        return false;
    }
    std::vector<uint8_t> payload(header.payload_length);
    if (!payload.empty() && read_exact(payload.data(), payload.size()) != ReadResult::Ok) {
        disconnect();
        if (err) *err = TransportError::Timeout;
        return false;
    }

    RoutingActivationResponse resp{};
    if (!decode_routing_activation_response(payload.data(), payload.size(), resp)) {
        disconnect();
        if (err) *err = TransportError::MalformedFrame;
        return false;
    }
    if (resp.response_code != 0x10) {
        disconnect();
        if (err) *err = TransportError::RoutingActivationFailed;
        return false;
    }

    return true;
}

DoipTransport::ReadResult DoipTransport::read_exact(uint8_t *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(sock_, buf + got, len - got, 0);
        if (n > 0) {
            got += static_cast<size_t>(n);
            continue;
        }
        if (n == 0) return ReadResult::Closed;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return ReadResult::Timeout;
        return ReadResult::Error;
    }
    return ReadResult::Ok;
}

bool DoipTransport::read_diagnostic_message(uint16_t expected_source_address, std::vector<uint8_t> &uds_response,
                                             TransportError &err) {
    uint8_t header_buf[kHeaderSize];
    auto rr = read_exact(header_buf, kHeaderSize);
    if (rr == ReadResult::Timeout) {
        err = TransportError::Timeout;
        return false;
    }
    if (rr != ReadResult::Ok) {
        err = TransportError::ConnectionClosed;
        return false;
    }

    Header header{};
    if (!decode_header(header_buf, kHeaderSize, header) || header.payload_length > kMaxPayloadLength) {
        err = TransportError::MalformedFrame;
        return false;
    }
    if (header.payload_type != PayloadType::DiagnosticMessage) {
        err = TransportError::MalformedFrame;
        return false;
    }

    std::vector<uint8_t> payload(header.payload_length);
    if (!payload.empty()) {
        rr = read_exact(payload.data(), payload.size());
        if (rr == ReadResult::Timeout) {
            err = TransportError::Timeout;
            return false;
        }
        if (rr != ReadResult::Ok) {
            err = TransportError::ConnectionClosed;
            return false;
        }
    }

    DiagnosticMessage msg{};
    if (!decode_diagnostic_message(payload.data(), payload.size(), msg)) {
        err = TransportError::MalformedFrame;
        return false;
    }
    if (msg.source_address != expected_source_address || msg.target_address != config_.tester_logical_address) {
        err = TransportError::MalformedFrame;
        return false;
    }

    uds_response = std::move(msg.uds_payload);
    return true;
}

bool DoipTransport::send_and_receive(uint16_t target_address, const std::vector<uint8_t> &uds_request,
                                      std::vector<uint8_t> &uds_response, TransportError *err) {
    TransportError local_err = TransportError::None;
    if (!err) err = &local_err;
    *err = TransportError::None;

    if (!is_connected()) {
        *err = TransportError::ConnectionClosed;
        return false;
    }

    auto frame = encode_diagnostic_message(config_.tester_logical_address, target_address, uds_request);
    if (::send(sock_, frame.data(), frame.size(), 0) != static_cast<ssize_t>(frame.size())) {
        *err = TransportError::ConnectionClosed;
        return false;
    }

    // DoIP-level ack/nack for the request itself, before the actual UDS
    // response (a separate, later diagnostic message).
    uint8_t header_buf[kHeaderSize];
    auto rr = read_exact(header_buf, kHeaderSize);
    if (rr == ReadResult::Timeout) {
        *err = TransportError::Timeout;
        return false;
    }
    if (rr != ReadResult::Ok) {
        *err = TransportError::ConnectionClosed;
        return false;
    }
    Header header{};
    if (!decode_header(header_buf, kHeaderSize, header) || header.payload_length > kMaxPayloadLength) {
        *err = TransportError::MalformedFrame;
        return false;
    }
    std::vector<uint8_t> ack_payload(header.payload_length);
    if (!ack_payload.empty()) {
        rr = read_exact(ack_payload.data(), ack_payload.size());
        if (rr == ReadResult::Timeout) {
            *err = TransportError::Timeout;
            return false;
        }
        if (rr != ReadResult::Ok) {
            *err = TransportError::ConnectionClosed;
            return false;
        }
    }
    if (header.payload_type == PayloadType::DiagnosticMessageNegativeAck) {
        *err = TransportError::NegativeAck;
        return false;
    }
    if (header.payload_type != PayloadType::DiagnosticMessagePositiveAck) {
        *err = TransportError::MalformedFrame;
        return false;
    }

    return read_diagnostic_message(target_address, uds_response, *err);
}

bool DoipTransport::receive_pending_response(uint16_t expected_source_address, std::vector<uint8_t> &uds_response,
                                              TransportError *err) {
    TransportError local_err = TransportError::None;
    if (!err) err = &local_err;
    *err = TransportError::None;

    if (!is_connected()) {
        *err = TransportError::ConnectionClosed;
        return false;
    }
    return read_diagnostic_message(expected_source_address, uds_response, *err);
}

} // namespace sovd::uds_doip
