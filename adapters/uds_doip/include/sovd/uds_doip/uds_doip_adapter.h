// uds_doip_adapter — real UDS/DoIP backend, extern "C" only across the
// boundary. Internally C++ (transport, session manager, catalog); nothing
// but this vtable accessor crosses the seam, matching adapters/mock's
// supplier pattern.
#pragma once

#include "sovd/adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

const sovd_vtable_t *sovd_uds_doip_adapter_vtable(void);

#ifdef __cplusplus
}
#endif
