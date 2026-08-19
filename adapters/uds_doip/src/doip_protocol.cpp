#include "sovd/uds_doip/doip_protocol.hpp"

namespace sovd::uds_doip::protocol {

namespace {

void append_u16(std::vector<uint8_t> &out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

uint16_t read_u16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

} // namespace

void append_header(std::vector<uint8_t> &out, PayloadType type, uint32_t payload_length) {
    out.push_back(kProtocolVersion);
    out.push_back(static_cast<uint8_t>(~kProtocolVersion));
    append_u16(out, static_cast<uint16_t>(type));
    out.push_back(static_cast<uint8_t>((payload_length >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((payload_length >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((payload_length >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(payload_length & 0xFF));
}

bool decode_header(const uint8_t *data, size_t len, Header &out) {
    if (len < kHeaderSize) return false;
    if (data[0] != kProtocolVersion) return false;
    if (data[1] != static_cast<uint8_t>(~kProtocolVersion)) return false;

    uint16_t type = read_u16(data + 2);
    switch (type) {
        case static_cast<uint16_t>(PayloadType::RoutingActivationRequest):
        case static_cast<uint16_t>(PayloadType::RoutingActivationResponse):
        case static_cast<uint16_t>(PayloadType::DiagnosticMessage):
        case static_cast<uint16_t>(PayloadType::DiagnosticMessagePositiveAck):
        case static_cast<uint16_t>(PayloadType::DiagnosticMessageNegativeAck):
            out.payload_type = static_cast<PayloadType>(type);
            break;
        default:
            return false;
    }

    out.payload_length = (static_cast<uint32_t>(data[4]) << 24) | (static_cast<uint32_t>(data[5]) << 16) |
                          (static_cast<uint32_t>(data[6]) << 8) | static_cast<uint32_t>(data[7]);
    return true;
}

std::vector<uint8_t> encode_routing_activation_request(uint16_t source_address, uint8_t activation_type) {
    std::vector<uint8_t> payload;
    append_u16(payload, source_address);
    payload.push_back(activation_type);
    payload.insert(payload.end(), {0x00, 0x00, 0x00, 0x00}); // reserved by ISO

    std::vector<uint8_t> out;
    append_header(out, PayloadType::RoutingActivationRequest, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

bool decode_routing_activation_response(const uint8_t *payload, size_t len, RoutingActivationResponse &out) {
    if (len < 5) return false;
    out.tester_logical_address = read_u16(payload);
    out.entity_logical_address = read_u16(payload + 2);
    out.response_code = payload[4];
    return true;
}

std::vector<uint8_t> encode_diagnostic_message(uint16_t source_address, uint16_t target_address,
                                                const std::vector<uint8_t> &uds_payload) {
    std::vector<uint8_t> payload;
    append_u16(payload, source_address);
    append_u16(payload, target_address);
    payload.insert(payload.end(), uds_payload.begin(), uds_payload.end());

    std::vector<uint8_t> out;
    append_header(out, PayloadType::DiagnosticMessage, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

bool decode_diagnostic_message(const uint8_t *payload, size_t len, DiagnosticMessage &out) {
    if (len < 4) return false;
    out.source_address = read_u16(payload);
    out.target_address = read_u16(payload + 2);
    out.uds_payload.assign(payload + 4, payload + len);
    return true;
}

bool decode_diagnostic_message_ack(const uint8_t *payload, size_t len, DiagnosticMessageAck &out) {
    if (len < 5) return false;
    out.source_address = read_u16(payload);
    out.target_address = read_u16(payload + 2);
    out.ack_code = payload[4];
    return true;
}

} // namespace sovd::uds_doip::protocol
