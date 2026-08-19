// nrc_map — UDS negative response code (ISO 14229-1 Table A.1) ->
// sovd_result_t. Pure logic, trivial to unit test exhaustively.
#pragma once

#include <cstdint>

#include "sovd/adapter.h"

namespace sovd::uds_doip {

// 0x78 responsePending is deliberately absorbed by the adapter's internal
// retry loop and should never reach this function in practice — if it ever
// does (a bug elsewhere, or an ECU sending it outside the expected
// sequence), it falls back to SOVD_BUSY rather than SOVD_INTERNAL: a stuck
// "still working" is closer to "try again" than to a hard failure.
sovd_result_t nrc_to_sovd_result(uint8_t nrc);

} // namespace sovd::uds_doip
