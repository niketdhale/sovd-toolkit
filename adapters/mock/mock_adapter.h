// mock_adapter — in-memory backend, extern "C" only across the boundary.
// Internally it's C++ (unordered_map, mutex); nothing but this vtable
// accessor crosses the seam.
#pragma once

#include "sovd/adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

const sovd_vtable_t *sovd_mock_adapter_vtable(void);

#ifdef __cplusplus
}
#endif
