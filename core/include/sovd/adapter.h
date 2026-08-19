/* sovd/adapter.h — the C-ABI seam between core/server and any backend.
 *
 * A backend (mock, uds_doip, sovd_proxy, ...) implements a sovd_vtable_t and
 * hands it to the entity registry. Any function pointer left NULL means the
 * backend does not support that operation; the server layer maps that to
 * HTTP 501 without the backend having to write a stub. This is how a
 * restricted build (e.g. the gateway, which links no ECU-facing adapter at
 * all) declares reduced capability through linkage rather than runtime
 * config.
 */
#ifndef SOVD_ADAPTER_H
#define SOVD_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum sovd_result_t {
    SOVD_OK = 0,
    SOVD_NOT_FOUND,
    SOVD_LOCKED,
    SOVD_BAD_REQUEST,
    SOVD_UNSUPPORTED,
    SOVD_BUSY,
    SOVD_TRANSPORT,
    SOVD_NEGATIVE_RESPONSE,
    SOVD_INTERNAL,
    /* Added for Phase 2 (adapters/uds_doip/nrc_map): the mock never needed
     * these, but real UDS NRCs do — securityAccessDenied and
     * conditionsNotCorrect have no honest fit among the codes above. */
    SOVD_FORBIDDEN, /* -> HTTP 403, e.g. UDS 0x33 securityAccessDenied */
    SOVD_CONFLICT   /* -> HTTP 409, e.g. UDS 0x22 conditionsNotCorrect */
} sovd_result_t;

/* Owned by whoever the vtable's free_buffer says owns it (the adapter, via
 * its free_* functions) until that function is called. */
typedef struct sovd_buffer_t {
    uint8_t *data;
    size_t len;
} sovd_buffer_t;

/* One DTC-equivalent entry. Fixed-size to keep the ABI simple in Phase 0;
 * revisit if a backend needs longer codes/status strings than this holds. */
typedef struct sovd_fault_t {
    char code[16];   /* e.g. "P0A0F-16" */
    char status[16]; /* "confirmed" | "pending" | "testFailed" */
} sovd_fault_t;

/* Opaque per-instance adapter state. Only the adapter that created it may
 * interpret the bytes behind this pointer. */
typedef struct sovd_adapter_ctx sovd_adapter_ctx;

/* Coarse backend capability flags. Declared statically per adapter type
 * (a field on the vtable, not a runtime query) — same philosophy as the
 * NULL-fn-ptr capability declaration below: "capability reduction by
 * linkage, not runtime checks." These don't gate whether an operation
 * works at all (that's still the NULL-fn-ptr checks); they describe how
 * well, so the server can decide things like whether a batch read gets a
 * native multi-DID call or a per-DID loop. */
typedef struct sovd_capability_t {
    bool supports_batch_read;      /* native multi-DID read, not just looped read_data */
    bool supports_async_operations;
    bool supports_io_control;      /* distinguishes 0x2F IOControl from 0x2E write */
} sovd_capability_t;

typedef struct sovd_vtable_t {
    /* config_json is adapter-specific connection/config data (Phase 4
     * topology loader will pass the entity's `adapter:` block here). May be
     * NULL for adapters that need no config, such as the mock. */
    sovd_adapter_ctx *(*create)(const char *config_json);
    void (*destroy)(sovd_adapter_ctx *ctx);

    sovd_result_t (*read_faults)(sovd_adapter_ctx *ctx, const char *entity_path,
                                  sovd_fault_t **out_faults, size_t *out_count);
    sovd_result_t (*clear_faults)(sovd_adapter_ctx *ctx, const char *entity_path);

    sovd_result_t (*read_data)(sovd_adapter_ctx *ctx, const char *entity_path,
                                const char *did, sovd_buffer_t *out);
    sovd_result_t (*write_data)(sovd_adapter_ctx *ctx, const char *entity_path,
                                 const char *did, const uint8_t *data, size_t len);

    sovd_result_t (*set_mode)(sovd_adapter_ctx *ctx, const char *entity_path,
                               const char *mode);

    /* params_json/out_result_json are opaque JSON text; core never parses
     * them, only server does. *out_result_json may be left NULL. */
    sovd_result_t (*execute_operation)(sovd_adapter_ctx *ctx, const char *entity_path,
                                        const char *op, const char *params_json,
                                        char **out_result_json);

    /* Deallocators for the out-params above. An adapter using malloc/new[]
     * internally can just point these at the sovd_default_free_* helpers. */
    void (*free_faults)(sovd_fault_t *faults, size_t count);
    void (*free_buffer)(sovd_buffer_t *buf);
    void (*free_string)(char *str);

    sovd_capability_t capabilities;
} sovd_vtable_t;

/* Shared malloc-based deallocators, usable by any adapter that allocates its
 * out-params with malloc — avoids every adapter reimplementing free(). */
void sovd_default_free_faults(sovd_fault_t *faults, size_t count);
void sovd_default_free_buffer(sovd_buffer_t *buf);
void sovd_default_free_string(char *str);

#ifdef __cplusplus
}
#endif

#endif /* SOVD_ADAPTER_H */
