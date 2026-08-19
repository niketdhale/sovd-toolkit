// doip_transport — one TCP connection to a DoIP entity (gateway/ECU),
// handling routing activation and diagnostic-message send/ack/receive.
// Deliberately UDS-agnostic: it moves opaque UDS byte payloads and doesn't
// know what a negative response or 0x78 response-pending means — that's
// uds_services'/the adapter's job, one layer up. Not thread-safe: one
// request in flight at a time per instance, which matches real UDS/DoIP
// semantics anyway (an ECU processes one diagnostic request at a time).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sovd::uds_doip {

struct TransportConfig {
    std::string host;
    uint16_t port = 13400;
    uint16_t tester_logical_address = 0x0E00; // our own address as the tester
    int connect_timeout_ms = 2000;
    int read_timeout_ms = 2000;
};

enum class TransportError {
    None,
    ConnectFailed,
    RoutingActivationFailed,
    Timeout,
    MalformedFrame,
    ConnectionClosed,
    NegativeAck,
};

std::string transport_error_to_string(TransportError e);

class DoipTransport {
public:
    explicit DoipTransport(TransportConfig config);
    ~DoipTransport();

    DoipTransport(const DoipTransport &) = delete;
    DoipTransport &operator=(const DoipTransport &) = delete;

    // Connects and performs routing activation (activation type 0x00,
    // "default"). Idempotent: a no-op returning true if already connected.
    bool connect(TransportError *err = nullptr);
    void disconnect();
    bool is_connected() const;

    // Sends a UDS request to target_address, waits for the DoIP-level ack
    // (positive/negative), then waits for the follow-up diagnostic message
    // carrying the actual UDS response bytes (which may itself be a UDS
    // negative response, including 0x78 response-pending — this layer
    // doesn't interpret that, it just hands the bytes back).
    bool send_and_receive(uint16_t target_address, const std::vector<uint8_t> &uds_request,
                           std::vector<uint8_t> &uds_response, TransportError *err = nullptr);

    // Waits for a further diagnostic message without sending a new request
    // — used by the 0x78 response-pending retry loop, where the ECU sends
    // the real response unprompted once it's ready.
    bool receive_pending_response(uint16_t expected_source_address, std::vector<uint8_t> &uds_response,
                                   TransportError *err = nullptr);

private:
    enum class ReadResult { Ok, Timeout, Closed, Error };
    ReadResult read_exact(uint8_t *buf, size_t len);
    bool read_diagnostic_message(uint16_t expected_source_address, std::vector<uint8_t> &uds_response,
                                  TransportError &err);

    TransportConfig config_;
    int sock_ = -1;
};

} // namespace sovd::uds_doip
