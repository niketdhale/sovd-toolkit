#include "sovd/uds_doip/nrc_map.hpp"

namespace sovd::uds_doip {

sovd_result_t nrc_to_sovd_result(uint8_t nrc) {
    switch (nrc) {
        case 0x10: return SOVD_INTERNAL;    // generalReject
        case 0x11: return SOVD_UNSUPPORTED; // serviceNotSupported
        case 0x12: return SOVD_UNSUPPORTED; // subFunctionNotSupported
        case 0x13: return SOVD_BAD_REQUEST; // incorrectMessageLengthOrInvalidFormat
        case 0x14: return SOVD_INTERNAL;    // responseTooLong
        case 0x21: return SOVD_BUSY;        // busyRepeatRequest
        case 0x22: return SOVD_CONFLICT;    // conditionsNotCorrect
        case 0x24: return SOVD_CONFLICT;    // requestSequenceError
        case 0x31: return SOVD_BAD_REQUEST; // requestOutOfRange
        case 0x33: return SOVD_FORBIDDEN;   // securityAccessDenied
        case 0x35: return SOVD_FORBIDDEN;   // invalidKey
        case 0x36: return SOVD_BUSY;        // exceedNumberOfAttempts (try later)
        case 0x37: return SOVD_BUSY;        // requiredTimeDelayNotExpired (try later)
        case 0x70: return SOVD_UNSUPPORTED; // uploadDownloadNotAccepted
        case 0x78: return SOVD_BUSY;        // responsePending leaking through unexpectedly
        case 0x7E: return SOVD_UNSUPPORTED; // subFunctionNotSupportedInActiveSession
        case 0x7F: return SOVD_UNSUPPORTED; // serviceNotSupportedInActiveSession
        default: return SOVD_INTERNAL;
    }
}

} // namespace sovd::uds_doip
