#include "sovd/uds_doip/uds_services.hpp"

#include <cstdio>

namespace sovd::uds_doip::uds {

namespace {

constexpr uint8_t kSidReadDataByIdentifier = 0x22;
constexpr uint8_t kSidWriteDataByIdentifier = 0x2E;
constexpr uint8_t kSidIoControl = 0x2F;
constexpr uint8_t kSidReadDtcInformation = 0x19;
constexpr uint8_t kSidClearDiagnosticInformation = 0x14;
constexpr uint8_t kSidDiagnosticSessionControl = 0x10;
constexpr uint8_t kSidRoutineControl = 0x31;
constexpr uint8_t kSidTesterPresent = 0x3E;
constexpr uint8_t kSidSecurityAccess = 0x27;

constexpr uint8_t kSubReportDtcByStatusMask = 0x02;

void append_u16(std::vector<uint8_t> &out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

uint16_t read_u16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

std::string format_dtc(uint8_t b0, uint8_t b1, uint8_t b2) {
    static const char kCategories[4] = {'P', 'C', 'B', 'U'};
    char category = kCategories[(b0 >> 6) & 0x3];
    int digit1 = (b0 >> 4) & 0x3;
    int digit2 = b0 & 0xF;
    int digit3 = (b1 >> 4) & 0xF;
    int digit4 = b1 & 0xF;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%c%d%X%X%X-%02X", category, digit1, digit2, digit3, digit4, b2);
    return std::string(buf);
}

std::string dtc_status_to_string(uint8_t status_byte) {
    if (status_byte & 0x08) return "confirmed"; // confirmedDTC
    if (status_byte & 0x04) return "pending";   // pendingDTC
    if (status_byte & 0x01) return "testFailed";
    return "pending";
}

} // namespace

bool is_negative_response(const std::vector<uint8_t> &response, uint8_t &out_nrc) {
    if (response.size() < 3 || response[0] != 0x7F) return false;
    out_nrc = response[2];
    return true;
}

std::vector<uint8_t> encode_read_data_by_identifier(uint16_t did) {
    std::vector<uint8_t> req = {kSidReadDataByIdentifier};
    append_u16(req, did);
    return req;
}

bool decode_read_data_by_identifier(const std::vector<uint8_t> &response, uint16_t expected_did,
                                     std::vector<uint8_t> &out_data) {
    if (response.size() < 3 || response[0] != kSidReadDataByIdentifier + 0x40) return false;
    if (read_u16(response.data() + 1) != expected_did) return false;
    out_data.assign(response.begin() + 3, response.end());
    return true;
}

std::vector<uint8_t> encode_write_data_by_identifier(uint16_t did, const std::vector<uint8_t> &data) {
    std::vector<uint8_t> req = {kSidWriteDataByIdentifier};
    append_u16(req, did);
    req.insert(req.end(), data.begin(), data.end());
    return req;
}

bool decode_write_data_by_identifier(const std::vector<uint8_t> &response, uint16_t expected_did) {
    if (response.size() < 3 || response[0] != kSidWriteDataByIdentifier + 0x40) return false;
    return read_u16(response.data() + 1) == expected_did;
}

std::vector<uint8_t> encode_io_control(uint16_t did, uint8_t control_param, const std::vector<uint8_t> &data) {
    std::vector<uint8_t> req = {kSidIoControl};
    append_u16(req, did);
    req.push_back(control_param);
    req.insert(req.end(), data.begin(), data.end());
    return req;
}

bool decode_io_control(const std::vector<uint8_t> &response, uint16_t expected_did) {
    if (response.size() < 3 || response[0] != kSidIoControl + 0x40) return false;
    return read_u16(response.data() + 1) == expected_did;
}

std::vector<uint8_t> encode_read_dtc_by_status_mask(uint8_t status_mask) {
    return {kSidReadDtcInformation, kSubReportDtcByStatusMask, status_mask};
}

bool decode_read_dtc_by_status_mask(const std::vector<uint8_t> &response, std::vector<DtcEntry> &out_entries) {
    if (response.size() < 3 || response[0] != kSidReadDtcInformation + 0x40 ||
        response[1] != kSubReportDtcByStatusMask) {
        return false;
    }
    // response[2] is the DTCStatusAvailabilityMask; records follow as
    // 4-byte groups (3 DTC bytes + 1 status byte).
    out_entries.clear();
    size_t i = 3;
    while (i + 4 <= response.size()) {
        DtcEntry entry;
        entry.code = format_dtc(response[i], response[i + 1], response[i + 2]);
        entry.status = dtc_status_to_string(response[i + 3]);
        out_entries.push_back(std::move(entry));
        i += 4;
    }
    return true;
}

std::vector<uint8_t> encode_clear_diagnostic_information(uint32_t group) {
    return {kSidClearDiagnosticInformation, static_cast<uint8_t>((group >> 16) & 0xFF),
            static_cast<uint8_t>((group >> 8) & 0xFF), static_cast<uint8_t>(group & 0xFF)};
}

bool decode_clear_diagnostic_information(const std::vector<uint8_t> &response) {
    return !response.empty() && response[0] == kSidClearDiagnosticInformation + 0x40;
}

bool session_name_to_type(const std::string &name, uint8_t &out_type) {
    if (name == "default") {
        out_type = 0x01;
    } else if (name == "programming") {
        out_type = 0x02;
    } else if (name == "extended") {
        out_type = 0x03;
    } else {
        return false;
    }
    return true;
}

std::vector<uint8_t> encode_diagnostic_session_control(uint8_t session_type) {
    return {kSidDiagnosticSessionControl, session_type};
}

bool decode_diagnostic_session_control(const std::vector<uint8_t> &response, uint8_t expected_session_type) {
    if (response.size() < 2 || response[0] != kSidDiagnosticSessionControl + 0x40) return false;
    return response[1] == expected_session_type;
}

std::vector<uint8_t> encode_routine_control(RoutineSubfunction subfunction, uint16_t routine_id,
                                             const std::vector<uint8_t> &option_record) {
    std::vector<uint8_t> req = {kSidRoutineControl, static_cast<uint8_t>(subfunction)};
    append_u16(req, routine_id);
    req.insert(req.end(), option_record.begin(), option_record.end());
    return req;
}

bool decode_routine_control(const std::vector<uint8_t> &response, uint16_t expected_routine_id,
                             std::vector<uint8_t> &out_result_record) {
    if (response.size() < 4 || response[0] != kSidRoutineControl + 0x40) return false;
    if (read_u16(response.data() + 2) != expected_routine_id) return false;
    out_result_record.assign(response.begin() + 4, response.end());
    return true;
}

std::vector<uint8_t> encode_tester_present() { return {kSidTesterPresent, 0x00}; }

bool decode_tester_present(const std::vector<uint8_t> &response) {
    return response.size() >= 2 && response[0] == kSidTesterPresent + 0x40 && response[1] == 0x00;
}

std::vector<uint8_t> encode_security_access_request_seed(uint8_t level) { return {kSidSecurityAccess, level}; }

bool decode_security_access_seed(const std::vector<uint8_t> &response, uint8_t expected_level,
                                  std::vector<uint8_t> &out_seed) {
    if (response.size() < 2 || response[0] != kSidSecurityAccess + 0x40 || response[1] != expected_level) {
        return false;
    }
    out_seed.assign(response.begin() + 2, response.end());
    return true;
}

std::vector<uint8_t> encode_security_access_send_key(uint8_t level, const std::vector<uint8_t> &key) {
    std::vector<uint8_t> req = {kSidSecurityAccess, static_cast<uint8_t>(level + 1)};
    req.insert(req.end(), key.begin(), key.end());
    return req;
}

bool decode_security_access_key_accepted(const std::vector<uint8_t> &response, uint8_t expected_level) {
    return response.size() >= 2 && response[0] == kSidSecurityAccess + 0x40 &&
           response[1] == static_cast<uint8_t>(expected_level + 1);
}

std::vector<uint8_t> derive_key_DEMO_ONLY_NOT_SECURE(const std::vector<uint8_t> &seed, uint8_t level) {
    std::vector<uint8_t> key;
    key.reserve(seed.size());
    for (uint8_t b : seed) {
        key.push_back(static_cast<uint8_t>(b ^ 0xA5 ^ level));
    }
    return key;
}

} // namespace sovd::uds_doip::uds
