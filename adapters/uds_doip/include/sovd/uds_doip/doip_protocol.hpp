// doip_protocol — DoIP (ISO 13400-2) header/payload framing. Pure byte
// encode/decode, no sockets. Shared by the real transport (doip_transport)
// and the in-repo fault-injecting test server (tests/fake_doip_server.hpp)
// so both speak the exact same wire format — a test fixture that frames
// bytes differently from the real client would test nothing useful.
//
// Deliberately NOT implemented here: UDP vehicle identification/announcement
// (0x0001/0x0004) — the topology config gives gateway_ip/port/logical_address
// directly, so discovery is unnecessary. Also not ISO-TP (ISO 15765-2): that's
// a CAN-side concern the physical gateway handles internally when relaying a
// DoIP diagnostic message onto the bus. A DoIP client just needs correct
// length-prefixed framing over TCP, which is what this covers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sovd::uds_doip::protocol {

// ISO 13400-2:2012. A real multi-version client would negotiate; fixed here
// since we control both ends of every test and the topology config doesn't
// carry a protocol version field (Phase 4 concern if it ever needs to).
constexpr uint8_t kProtocolVersion = 0x02;
constexpr size_t kHeaderSize = 8; // version(1) + inverse(1) + type(2) + length(4)

enum class PayloadType : uint16_t {
    RoutingActivationRequest = 0x0005,
    RoutingActivationResponse = 0x0006,
    DiagnosticMessage = 0x8001,
    DiagnosticMessagePositiveAck = 0x8002,
    DiagnosticMessageNegativeAck = 0x8003,
};

struct Header {
    PayloadType payload_type;
    uint32_t payload_length = 0;
};

// Appends an 8-byte DoIP header (version/inverse-version/type/length) to out.
void append_header(std::vector<uint8_t> &out, PayloadType type, uint32_t payload_length);

// Parses exactly kHeaderSize bytes at data. Returns false if the protocol
// version/inverse-version pair doesn't check out (malformed-frame case) or
// the payload type is unrecognized.
bool decode_header(const uint8_t *data, size_t len, Header &out);

std::vector<uint8_t> encode_routing_activation_request(uint16_t source_address, uint8_t activation_type);

struct RoutingActivationResponse {
    uint16_t tester_logical_address = 0;
    uint16_t entity_logical_address = 0;
    uint8_t response_code = 0; // 0x10 = success; see ISO 13400-2 Table 32
};
bool decode_routing_activation_response(const uint8_t *payload, size_t len, RoutingActivationResponse &out);

std::vector<uint8_t> encode_diagnostic_message(uint16_t source_address, uint16_t target_address,
                                                const std::vector<uint8_t> &uds_payload);

struct DiagnosticMessage {
    uint16_t source_address = 0;
    uint16_t target_address = 0;
    std::vector<uint8_t> uds_payload;
};
bool decode_diagnostic_message(const uint8_t *payload, size_t len, DiagnosticMessage &out);

struct DiagnosticMessageAck {
    uint16_t source_address = 0;
    uint16_t target_address = 0;
    uint8_t ack_code = 0; // 0x00 = positive
};
bool decode_diagnostic_message_ack(const uint8_t *payload, size_t len, DiagnosticMessageAck &out);

} // namespace sovd::uds_doip::protocol
