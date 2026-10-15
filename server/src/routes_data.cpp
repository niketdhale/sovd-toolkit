#include "sovd/server/routes.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <regex>
#include <sstream>
#include <variant>
#include <vector>

#include "httplib.h"
#include "json.hpp"
#include "routes_internal.hpp"
#include "sovd/server/oauth2.hpp"

namespace sovd::server {

using json = nlohmann::json;
using namespace detail;


void Router::handle_get_data(const httplib::Request &req, httplib::Response &res, const std::string &path,
                              const std::string &id_or_did) {
    correlation_id_for(req, res);
    const Entity *e = require_entity(res, path);
    if (!e) return;
    if (try_forward(req, res, path)) return;
    if (!e->vtable || !e->vtable->read_data) {
        write_error(res, 501, "UNSUPPORTED", "entity has no diagnostic backend");
        return;
    }

    auto outcome = read_one_data_item(*e, path, id_or_did, find_catalog(path));
    if (!outcome.ok) {
        write_error(res, outcome.status, outcome.error_code, outcome.message);
        return;
    }
    res.set_content(outcome.body.dump(), "application/json");
}


void Router::handle_get_data_batch(const httplib::Request &req, httplib::Response &res, const std::string &path) {
    correlation_id_for(req, res);
    const Entity *e = require_entity(res, path);
    if (!e) return;
    if (try_forward(req, res, path)) return;
    if (!e->vtable || !e->vtable->read_data) {
        write_error(res, 501, "UNSUPPORTED", "entity has no diagnostic backend");
        return;
    }

    std::string ids_param = req.get_param_value("ids");
    std::vector<std::string> ids;
    std::istringstream iss(ids_param);
    for (std::string token; std::getline(iss, token, ','); ) {
        if (!token.empty()) ids.push_back(token);
    }
    if (ids.empty()) {
        write_error(res, 400, "BAD_REQUEST", "expected non-empty ?ids=a,b,c");
        return;
    }

    // Partial failure doesn't fail the whole batch — matches the "one
    // unreachable ECU must not fail the whole entity listing" graceful-
    // degradation principle (docs/DESIGN.md, Phase 4), applied one level down:
    // one stale/removed id in a 30-item batch shouldn't force 30 retries.
    const catalog::Catalog *cat = find_catalog(path);
    json items = json::array();
    for (auto &id : ids) {
        auto outcome = read_one_data_item(*e, path, id, cat);
        if (outcome.ok) {
            items.push_back(outcome.body);
        } else {
            items.push_back({{"id", id}, {"error", outcome.error_code}, {"message", outcome.message}});
        }
    }
    res.set_content(json{{"items", items}}.dump(), "application/json");
}

void Router::handle_put_data(const httplib::Request &req, httplib::Response &res, const std::string &path,
                              const std::string &id_or_did) {
    std::string corr = correlation_id_for(req, res);
    const Entity *e = require_entity(res, path);
    if (!e) return;
    if (try_forward(req, res, path)) return;
    if (!e->vtable || !e->vtable->write_data) {
        write_error(res, 501, "UNSUPPORTED", "entity has no diagnostic backend");
        return;
    }
    if (!check_lock_header(req, res, path)) return;

    const catalog::Catalog *cat = find_catalog(path);
    const catalog::DataItem *item = cat ? cat->find_by_id(id_or_did) : nullptr;
    std::string did_hex = item ? to_hex_u16(item->did) : id_or_did;

    if (item && item->access == catalog::Access::Read) {
        write_error(res, 400, "BAD_REQUEST", "data item '" + item->id + "' is read-only");
        return;
    }

    // Phase 8 (D2): the OAuth2-scope half of SecurityAccess gating -- the
    // ECU-demand half (uds_doip's ensure_security_for_did, driven by this
    // same requires_security_level field) runs unconditionally inside the
    // adapter regardless of what happens here; this is the "is this client
    // even allowed to ask" check D2 required alongside it.
    if (item && item->requires_security_level && !has_oauth2_scope(req, "execute:security_access")) {
        emit_event(event_sink_, "security_access_scope_denied",
                   {{"entity", path}, {"id", item->id}, {"correlation_id", corr}});
        write_error(res, 403, "FORBIDDEN",
                    "token lacks execute:security_access scope required for '" + item->id + "'");
        return;
    }

    json parsed;
    try {
        parsed = json::parse(req.body);
    } catch (...) {
        write_error(res, 400, "BAD_REQUEST", "invalid JSON body");
        return;
    }
    if (!parsed.contains("value")) {
        write_error(res, 400, "BAD_REQUEST", R"(expected {"value": ...})");
        return;
    }

    // B1 (Phase 7 blocker): a named catalog id gets a typed value on the
    // wire (a JSON string label for enum/string/raw, a JSON number for
    // float) and goes through Catalog::encode() -- symmetric with how a
    // named GET already returns a typed, decoded value instead of raw hex.
    // An unnamed/raw DID has no catalog entry to type-check against, so it
    // keeps the original hex-bytes-over-the-wire contract unchanged.
    std::vector<uint8_t> bytes;
    if (item) {
        try {
            std::variant<std::string, double> typed_value;
            if (item->type == catalog::DataType::Float) {
                if (!parsed["value"].is_number()) {
                    write_error(res, 400, "BAD_REQUEST", "value must be a number for this data item");
                    return;
                }
                typed_value = parsed["value"].get<double>();
            } else {
                if (!parsed["value"].is_string()) {
                    write_error(res, 400, "BAD_REQUEST", "value must be a string for this data item");
                    return;
                }
                typed_value = parsed["value"].get<std::string>();
            }
            bytes = catalog::Catalog::encode(*item, typed_value);
        } catch (const catalog::CatalogError &ex) {
            write_error(res, 400, "BAD_REQUEST", ex.what());
            return;
        }
    } else {
        if (!parsed["value"].is_string()) {
            write_error(res, 400, "BAD_REQUEST", R"(expected {"value": "<hex>"})");
            return;
        }
        if (!from_hex(parsed["value"].get<std::string>(), bytes)) {
            write_error(res, 400, "BAD_REQUEST", "value must be hex-encoded bytes");
            return;
        }
    }

    sovd_result_t r =
        e->vtable->write_data(e->adapter_ctx, path.c_str(), did_hex.c_str(), bytes.data(), bytes.size());
    if (r != SOVD_OK) {
        write_error(res, http_status_for(r), "ADAPTER_ERROR", "write_data failed");
        return;
    }

    emit_event(event_sink_, "data_written",
               {{"entity", path}, {"id", item ? item->id : id_or_did}, {"did", did_hex}, {"correlation_id", corr}});
    res.status = 204;
}
} // namespace sovd::server
