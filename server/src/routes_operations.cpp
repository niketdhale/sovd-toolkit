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


void Router::handle_post_mode(const httplib::Request &req, httplib::Response &res, const std::string &path) {
    std::string corr = correlation_id_for(req, res);
    const Entity *e = require_entity(res, path);
    if (!e) return;
    if (try_forward(req, res, path)) return;
    if (!e->vtable || !e->vtable->set_mode) {
        write_error(res, 501, "UNSUPPORTED", "entity has no diagnostic backend");
        return;
    }
    if (!check_lock_header(req, res, path)) return;

    json parsed;
    try {
        parsed = json::parse(req.body);
    } catch (...) {
        write_error(res, 400, "BAD_REQUEST", "invalid JSON body");
        return;
    }
    if (!parsed.contains("mode") || !parsed["mode"].is_string()) {
        write_error(res, 400, "BAD_REQUEST", R"(expected {"mode": "<name>"})");
        return;
    }

    std::string mode = parsed["mode"].get<std::string>();
    sovd_result_t r = e->vtable->set_mode(e->adapter_ctx, path.c_str(), mode.c_str());
    if (r != SOVD_OK) {
        write_error(res, http_status_for(r), "ADAPTER_ERROR", "set_mode failed");
        return;
    }

    emit_event(event_sink_, "mode_changed", {{"entity", path}, {"mode", mode}, {"correlation_id", corr}});
    res.status = 204;
}

void Router::handle_post_operation(const httplib::Request &req, httplib::Response &res, const std::string &path,
                                    const std::string &op) {
    std::string corr = correlation_id_for(req, res);
    const Entity *e = require_entity(res, path);
    if (!e) return;
    if (try_forward(req, res, path)) return;
    if (!e->vtable || !e->vtable->execute_operation) {
        write_error(res, 501, "UNSUPPORTED", "entity has no diagnostic backend");
        return;
    }
    if (!check_lock_header(req, res, path)) return;

    std::string params_json = req.body.empty() ? "{}" : req.body;
    char *out_result = nullptr;
    sovd_result_t r =
        e->vtable->execute_operation(e->adapter_ctx, path.c_str(), op.c_str(), params_json.c_str(), &out_result);
    if (r != SOVD_OK) {
        if (out_result && e->vtable->free_string) e->vtable->free_string(out_result);
        write_error(res, http_status_for(r), "ADAPTER_ERROR", "operation failed");
        return;
    }

    emit_event(event_sink_, "operation_executed", {{"entity", path}, {"operation", op}, {"correlation_id", corr}});

    json result = json::object();
    if (out_result) {
        try {
            result = json::parse(out_result);
        } catch (...) {
            // leave result as {} if the adapter returned non-JSON
        }
        if (e->vtable->free_string) e->vtable->free_string(out_result);
    }
    res.set_content(result.dump(), "application/json");
}
} // namespace sovd::server
