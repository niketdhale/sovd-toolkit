// uds_services — UDS (ISO 14229-1) request/response byte encode/decode for
// exactly the services the SOVD -> UDS mapping table (docs/DESIGN.md) needs.
// Pure logic: no sockets, no DoIP framing (that's doip_transport/protocol),
// fully unit-testable on its own.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sovd::uds_doip::uds {

// True if response is a negative response (0x7F <sid> <nrc>), with the NRC
// byte extracted. False (and *out_nrc left untouched) for anything else,
// including a too-short/malformed buffer.
bool is_negative_response(const std::vector<uint8_t> &response, uint8_t &out_nrc);

// --- 0x22 ReadDataByIdentifier ---
std::vector<uint8_t> encode_read_data_by_identifier(uint16_t did);
// out_data receives everything after the echoed DID. False on any mismatch
// (wrong SID, wrong DID, too short) — including a negative response; the
// caller uses is_negative_response separately when it needs the NRC.
bool decode_read_data_by_identifier(const std::vector<uint8_t> &response, uint16_t expected_did,
                                     std::vector<uint8_t> &out_data);

// --- 0x2E WriteDataByIdentifier ---
std::vector<uint8_t> encode_write_data_by_identifier(uint16_t did, const std::vector<uint8_t> &data);
bool decode_write_data_by_identifier(const std::vector<uint8_t> &response, uint16_t expected_did);

// --- 0x2F InputOutputControlByIdentifier ---
// shortTermAdjustment (0x03) is the control-parameter byte that maps to "set
// this actuator to a value now", which is what a SOVD PUT on an io_control
// DID means. returnControlToECU (0x00) exists in the standard but nothing
// in this project's SOVD surface currently triggers it.
constexpr uint8_t kIoControlShortTermAdjustment = 0x03;
std::vector<uint8_t> encode_io_control(uint16_t did, uint8_t control_param, const std::vector<uint8_t> &data);
bool decode_io_control(const std::vector<uint8_t> &response, uint16_t expected_did);

// --- 0x19 ReadDTCInformation, sub 0x02 reportDTCByStatusMask ---
struct DtcEntry {
    std::string code;   // e.g. "P0A0F-16": letter+4hex from the standard SAE
                         // J2012 2-byte encoding, "-XX" is the raw 3rd DTC
                         // byte (OEM-specific extension in some UDS stacks) —
                         // matches the convention the Phase 0 mock already
                         // used for its seed data, kept consistent here.
    std::string status; // "confirmed" | "pending" | "testFailed"
};
std::vector<uint8_t> encode_read_dtc_by_status_mask(uint8_t status_mask = 0xFF);
bool decode_read_dtc_by_status_mask(const std::vector<uint8_t> &response, std::vector<DtcEntry> &out_entries);

// --- 0x14 ClearDiagnosticInformation ---
std::vector<uint8_t> encode_clear_diagnostic_information(uint32_t group = 0xFFFFFF);
bool decode_clear_diagnostic_information(const std::vector<uint8_t> &response);

// --- 0x10 DiagnosticSessionControl ---
// Only the session names this project actually names anywhere (catalog
// requires_session, POST /modes body). An unrecognized name is a
// config/catalog problem, not something to guess a UDS session type for.
bool session_name_to_type(const std::string &name, uint8_t &out_type);
std::vector<uint8_t> encode_diagnostic_session_control(uint8_t session_type);
// Ignores the P2/P2* timing parameters in the response (bytes after the
// echoed session type) — the session manager uses its own configured
// heartbeat interval rather than parsing ECU-advertised timing, which is a
// deliberate simplification for this project's scope.
bool decode_diagnostic_session_control(const std::vector<uint8_t> &response, uint8_t expected_session_type);

// --- 0x31 RoutineControl ---
enum class RoutineSubfunction : uint8_t { Start = 0x01, Stop = 0x02, RequestResults = 0x03 };
std::vector<uint8_t> encode_routine_control(RoutineSubfunction subfunction, uint16_t routine_id,
                                             const std::vector<uint8_t> &option_record = {});
bool decode_routine_control(const std::vector<uint8_t> &response, uint16_t expected_routine_id,
                             std::vector<uint8_t> &out_result_record);

// --- 0x3E TesterPresent ---
// Deliberately NOT the suppress-response subfunction (0x3E 0x80): the DoIP-
// level ack (0x8002) only confirms routing accepted the request, not that
// the ECU is alive, and our transport's send_and_receive already waits for
// a follow-up diagnostic message regardless — so there's no round-trip cost
// to getting a real 0x7E back, and it doubles as an actual liveness check.
std::vector<uint8_t> encode_tester_present();
bool decode_tester_present(const std::vector<uint8_t> &response);

// --- 0x27 SecurityAccess ---
std::vector<uint8_t> encode_security_access_request_seed(uint8_t level);
bool decode_security_access_seed(const std::vector<uint8_t> &response, uint8_t expected_level,
                                  std::vector<uint8_t> &out_seed);
std::vector<uint8_t> encode_security_access_send_key(uint8_t level, const std::vector<uint8_t> &key);
bool decode_security_access_key_accepted(const std::vector<uint8_t> &response, uint8_t expected_level);

// A clearly-labeled stand-in, NOT a real security algorithm. Real UDS
// SecurityAccess key derivation is OEM-proprietary and secret; this exists
// only so the seed/key exchange *mechanism* (the actual "four hard
// problems" item — translating an HTTP scope into a seed/key round trip) is
// testable end-to-end without one. Never use this outside this project's
// own tests/fake server.
std::vector<uint8_t> derive_key_DEMO_ONLY_NOT_SECURE(const std::vector<uint8_t> &seed, uint8_t level);

} // namespace sovd::uds_doip::uds
