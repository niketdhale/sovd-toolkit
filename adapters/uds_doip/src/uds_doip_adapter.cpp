// uds_doip_adapter — wires transport + session manager + UDS services +
// NRC map + (optionally) a catalog into a sovd_vtable_t. All UDS/DoIP
// complexity stays behind this file; core/ and server/ never learn it
// exists (CLAUDE.md's layering rule).
//
// Expected create() config_json shape (Phase 4's config loader will
// eventually build this from the topology YAML's `adapter:` block):
//   {
//     "logical_address": "0x0E80",      // required: this ECU's address
//     "gateway_ip": "192.168.1.10",     // required
//     "port": 13400,                     // optional, default 13400
//     "tester_logical_address": "0x0E00",// optional, default 0x0E00
//     "did_catalog": "catalogs/bcm.yaml",// optional; resolved relative to
//                                         // CWD, same convention main.cpp
//                                         // already uses for the mock demo
//     "connect_timeout_ms": 2000,
//     "read_timeout_ms": 2000,
//     "heartbeat_interval_ms": 2000,
//     "session_idle_timeout_ms": 6000
//   }
// Missing gateway_ip/logical_address (or unparsable JSON) makes create()
// return NULL. Phase 4's loader is expected to check for that and skip
// registering the entity — core's EntityRegistry doesn't itself guard
// against a NULL adapter_ctx with a non-NULL vtable, a pre-existing gap
// that's out of scope to fix here since nothing calls create() with
// untrusted config yet (this adapter isn't wired into main.cpp's demo
// topology — there's no live target to point it at by default).
#include "sovd/uds_doip/uds_doip_adapter.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#include "json.hpp"
#include "sovd/catalog/did_catalog.hpp"
#include "sovd/uds_doip/doip_transport.hpp"
#include "sovd/uds_doip/nrc_map.hpp"
#include "sovd/uds_doip/session_manager.hpp"
#include "sovd/uds_doip/uds_services.hpp"

namespace {

using json = nlohmann::json;
using namespace sovd::uds_doip;

bool parse_hex_u16(const char *text, uint16_t &out) {
    if (!text) return false;
    std::string s = text;
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    if (s.empty()) return false;
    size_t consumed = 0;
    unsigned long value = 0;
    try {
        value = std::stoul(s, &consumed, 16);
    } catch (...) {
        return false;
    }
    if (consumed != s.size() || value > 0xFFFFu) return false;
    out = static_cast<uint16_t>(value);
    return true;
}

char *malloc_dup_string(const std::string &s) {
    char *buf = static_cast<char *>(std::malloc(s.size() + 1));
    std::memcpy(buf, s.c_str(), s.size() + 1);
    return buf;
}

struct UdsDoipContext {
    DoipTransport transport;
    uint16_t ecu_logical_address;
    sovd::catalog::Catalog catalog_data; // default-constructed (empty) if none configured
    bool has_catalog = false;
    std::mutex transport_mtx;
    std::unique_ptr<SessionManager> session_mgr;

    UdsDoipContext(TransportConfig tcfg, uint16_t ecu_addr, SessionManagerConfig scfg)
        : transport(std::move(tcfg)), ecu_logical_address(ecu_addr) {
        session_mgr = std::make_unique<SessionManager>(
            [this](const std::vector<uint8_t> &req, std::vector<uint8_t> &resp) { return send_and_resolve(req, resp); },
            scfg);
    }

    // The one place any UDS request/response crosses the wire. Holds
    // transport_mtx for the whole exchange, including the 0x78
    // response-pending retry loop — that loop does further reads on the
    // same connection with no new request sent, so nothing else may
    // interleave a request on it in the meantime. Used by every vtable
    // call AND by SessionManager's heartbeat, so the heartbeat and
    // foreground calls never race on the socket.
    bool send_and_resolve(const std::vector<uint8_t> &req, std::vector<uint8_t> &resp) {
        std::lock_guard<std::mutex> lk(transport_mtx);
        if (!transport.is_connected() && !transport.connect()) return false;

        std::vector<uint8_t> current;
        if (!transport.send_and_receive(ecu_logical_address, req, current)) return false;

        // Bounded: a perpetually-pending ECU is itself a fault worth
        // surfacing (as SOVD_BUSY, via nrc_to_sovd_result(0x78)) rather
        // than hanging the caller forever.
        constexpr int kMaxPendingAttempts = 10;
        int attempts = 0;
        uint8_t nrc = 0;
        while (sovd::uds_doip::uds::is_negative_response(current, nrc) && nrc == 0x78 &&
               attempts < kMaxPendingAttempts) {
            if (!transport.receive_pending_response(ecu_logical_address, current)) return false;
            ++attempts;
        }
        resp = std::move(current);
        return true;
    }

    // Escalates to the DID's catalog-declared session, if any. Best-effort:
    // no catalog, no entry, or an unrecognized requires_session name all
    // just proceed without escalating — that's a config/catalog problem to
    // flag at review time, not something to fail a live request over.
    bool ensure_session_for_did(uint16_t did) {
        if (!has_catalog) return true;
        const sovd::catalog::DataItem *item = catalog_data.find_by_did(did);
        if (!item || !item->requires_session) return true;
        uint8_t session_type = 0;
        if (!uds::session_name_to_type(*item->requires_session, session_type)) return true;
        return session_mgr->ensure_session(session_type);
    }

    // Phase 8 (D2): the ECU-demands half of SecurityAccess gating, parallel
    // to ensure_session_for_did above. The OAuth2-scope half (is *this
    // client* allowed to ask) already ran in routes.cpp before this adapter
    // was ever called -- this function has no HTTP/auth knowledge of its
    // own, same layering as everything else in this file.
    bool ensure_security_for_did(uint16_t did) {
        if (!has_catalog) return true;
        const sovd::catalog::DataItem *item = catalog_data.find_by_did(did);
        if (!item || !item->requires_security_level) return true;
        return session_mgr->ensure_security_level(static_cast<uint8_t>(*item->requires_security_level));
    }
};

UdsDoipContext *context(sovd_adapter_ctx *ctx) { return reinterpret_cast<UdsDoipContext *>(ctx); }

sovd_result_t negative_or_internal(const std::vector<uint8_t> &resp) {
    uint8_t nrc = 0;
    if (uds::is_negative_response(resp, nrc)) return nrc_to_sovd_result(nrc);
    return SOVD_INTERNAL;
}

sovd_adapter_ctx *uds_doip_create(const char *config_json) {
    try {
        json j = json::parse(config_json ? config_json : "{}");
        if (!j.contains("gateway_ip") || !j.contains("logical_address")) return nullptr;

        uint16_t ecu_addr = 0;
        std::string ecu_addr_str = j.at("logical_address").get<std::string>();
        if (!parse_hex_u16(ecu_addr_str.c_str(), ecu_addr)) return nullptr;

        TransportConfig tcfg;
        tcfg.host = j.at("gateway_ip").get<std::string>();
        tcfg.port = j.value("port", 13400);
        tcfg.connect_timeout_ms = j.value("connect_timeout_ms", 2000);
        tcfg.read_timeout_ms = j.value("read_timeout_ms", 2000);

        uint16_t tester_addr = 0x0E00;
        std::string tester_addr_str = j.value("tester_logical_address", std::string("0x0E00"));
        parse_hex_u16(tester_addr_str.c_str(), tester_addr); // keeps the 0x0E00 default on bad input
        tcfg.tester_logical_address = tester_addr;

        SessionManagerConfig scfg;
        scfg.heartbeat_interval_ms = j.value("heartbeat_interval_ms", 2000);
        scfg.idle_timeout_ms = j.value("session_idle_timeout_ms", 6000);

        auto *ctx = new UdsDoipContext(std::move(tcfg), ecu_addr, scfg);

        std::string catalog_path = j.value("did_catalog", std::string(""));
        if (!catalog_path.empty()) {
            try {
                ctx->catalog_data = sovd::catalog::Catalog::load_from_file(catalog_path);
                ctx->has_catalog = true;
            } catch (const sovd::catalog::CatalogError &) {
                // Not fatal: session-escalation/io_control decisions just
                // fall back to best-effort without a catalog to consult.
            }
        }
        return reinterpret_cast<sovd_adapter_ctx *>(ctx);
    } catch (...) {
        return nullptr;
    }
}

void uds_doip_destroy(sovd_adapter_ctx *ctx) { delete context(ctx); }

sovd_result_t uds_doip_read_faults(sovd_adapter_ctx *raw_ctx, const char *entity_path, sovd_fault_t **out_faults,
                                    size_t *out_count) {
    (void)entity_path;
    auto *c = context(raw_ctx);
    auto req = uds::encode_read_dtc_by_status_mask();
    std::vector<uint8_t> resp;
    if (!c->send_and_resolve(req, resp)) return SOVD_TRANSPORT;

    std::vector<uds::DtcEntry> entries;
    if (!uds::decode_read_dtc_by_status_mask(resp, entries)) return negative_or_internal(resp);

    *out_count = entries.size();
    if (entries.empty()) {
        *out_faults = nullptr;
        return SOVD_OK;
    }
    auto *arr = static_cast<sovd_fault_t *>(std::malloc(sizeof(sovd_fault_t) * entries.size()));
    for (size_t i = 0; i < entries.size(); ++i) {
        std::memset(&arr[i], 0, sizeof(sovd_fault_t));
        std::strncpy(arr[i].code, entries[i].code.c_str(), sizeof(arr[i].code) - 1);
        std::strncpy(arr[i].status, entries[i].status.c_str(), sizeof(arr[i].status) - 1);
    }
    *out_faults = arr;
    return SOVD_OK;
}

sovd_result_t uds_doip_clear_faults(sovd_adapter_ctx *raw_ctx, const char *entity_path) {
    (void)entity_path;
    auto *c = context(raw_ctx);
    auto req = uds::encode_clear_diagnostic_information();
    std::vector<uint8_t> resp;
    if (!c->send_and_resolve(req, resp)) return SOVD_TRANSPORT;
    if (uds::decode_clear_diagnostic_information(resp)) return SOVD_OK;
    return negative_or_internal(resp);
}

sovd_result_t uds_doip_read_data(sovd_adapter_ctx *raw_ctx, const char *entity_path, const char *did_str,
                                  sovd_buffer_t *out) {
    (void)entity_path;
    auto *c = context(raw_ctx);
    uint16_t did = 0;
    if (!parse_hex_u16(did_str, did)) return SOVD_BAD_REQUEST;

    auto req = uds::encode_read_data_by_identifier(did);
    std::vector<uint8_t> resp;
    if (!c->send_and_resolve(req, resp)) return SOVD_TRANSPORT;

    std::vector<uint8_t> data;
    if (!uds::decode_read_data_by_identifier(resp, did, data)) return negative_or_internal(resp);

    out->len = data.size();
    out->data = data.empty() ? nullptr : static_cast<uint8_t *>(std::malloc(data.size()));
    if (!data.empty()) std::memcpy(out->data, data.data(), data.size());
    return SOVD_OK;
}

sovd_result_t uds_doip_write_data(sovd_adapter_ctx *raw_ctx, const char *entity_path, const char *did_str,
                                   const uint8_t *data, size_t len) {
    (void)entity_path;
    auto *c = context(raw_ctx);
    uint16_t did = 0;
    if (!parse_hex_u16(did_str, did)) return SOVD_BAD_REQUEST;

    if (!c->ensure_session_for_did(did)) return SOVD_TRANSPORT;
    // After session escalation: real ECUs typically only accept
    // SecurityAccess outside the default session, so session-first ordering
    // matches how a real UDS stack actually behaves, not just convenience.
    if (!c->ensure_security_for_did(did)) return SOVD_FORBIDDEN;

    bool io_control = false;
    if (c->has_catalog) {
        const sovd::catalog::DataItem *item = c->catalog_data.find_by_did(did);
        if (item) io_control = item->io_control;
    }

    std::vector<uint8_t> payload(data, data + len);
    auto req = io_control ? uds::encode_io_control(did, uds::kIoControlShortTermAdjustment, payload)
                           : uds::encode_write_data_by_identifier(did, payload);

    std::vector<uint8_t> resp;
    if (!c->send_and_resolve(req, resp)) return SOVD_TRANSPORT;

    bool ok = io_control ? uds::decode_io_control(resp, did) : uds::decode_write_data_by_identifier(resp, did);
    if (ok) return SOVD_OK;
    return negative_or_internal(resp);
}

sovd_result_t uds_doip_set_mode(sovd_adapter_ctx *raw_ctx, const char *entity_path, const char *mode) {
    (void)entity_path;
    auto *c = context(raw_ctx);
    if (!mode) return SOVD_BAD_REQUEST;

    // "default" is also how routes.cpp tears a session down on lock
    // release (CLAUDE.md's "Session manager ownership") — same path either
    // way, whether a client asked for it or the lock lifecycle did.
    if (std::strcmp(mode, "default") == 0) {
        return c->session_mgr->revert_to_default() ? SOVD_OK : SOVD_TRANSPORT;
    }

    uint8_t session_type = 0;
    if (!uds::session_name_to_type(mode, session_type)) return SOVD_BAD_REQUEST;
    return c->session_mgr->ensure_session(session_type) ? SOVD_OK : SOVD_TRANSPORT;
}

sovd_result_t uds_doip_execute_operation(sovd_adapter_ctx *raw_ctx, const char *entity_path, const char *op,
                                          const char *params_json, char **out_result_json) {
    (void)entity_path;
    (void)params_json;
    auto *c = context(raw_ctx);
    if (!c->has_catalog || !op) return SOVD_NOT_FOUND;

    const sovd::catalog::Operation *operation = c->catalog_data.find_operation(op);
    if (!operation) return SOVD_NOT_FOUND;

    if (operation->requires_session) {
        uint8_t session_type = 0;
        if (uds::session_name_to_type(*operation->requires_session, session_type)) {
            if (!c->session_mgr->ensure_session(session_type)) return SOVD_TRANSPORT;
        }
    }

    auto req = uds::encode_routine_control(uds::RoutineSubfunction::Start, operation->routine_id);
    std::vector<uint8_t> resp;
    if (!c->send_and_resolve(req, resp)) return SOVD_TRANSPORT;

    std::vector<uint8_t> result;
    if (!uds::decode_routine_control(resp, operation->routine_id, result)) return negative_or_internal(resp);

    std::string result_str = std::string(R"({"operation":")") + op + R"(","status":"completed"})";
    *out_result_json = malloc_dup_string(result_str);
    return SOVD_OK;
}

const sovd_vtable_t g_uds_doip_vtable = {
    uds_doip_create,
    uds_doip_destroy,
    uds_doip_read_faults,
    uds_doip_clear_faults,
    uds_doip_read_data,
    uds_doip_write_data,
    uds_doip_set_mode,
    uds_doip_execute_operation,
    sovd_default_free_faults,
    sovd_default_free_buffer,
    sovd_default_free_string,
    // capabilities: honest about what this pass actually implements. UDS
    // does support multi-DID ReadDataByIdentifier requests in one message,
    // and RoutineControl's RequestResults subfunction could back true async
    // job polling — both real, both not built here (Phase 1's batch read
    // already works correctly, just as N separate 0x22 calls; async job
    // polling is a stated non-goal for this project). io_control is real:
    // the adapter genuinely dispatches 0x2E vs 0x2F off the catalog.
    {false, false, true},
};

} // namespace

extern "C" const sovd_vtable_t *sovd_uds_doip_adapter_vtable(void) { return &g_uds_doip_vtable; }
