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


void Router::handle_get_faults(const httplib::Request &req, httplib::Response &res, const std::string &path) {
    correlation_id_for(req, res);
    const Entity *e = require_entity(res, path);
    if (!e) return;
    if (try_forward(req, res, path)) return;
    if (!e->vtable || !e->vtable->read_faults) {
        write_error(res, 501, "UNSUPPORTED", "entity has no diagnostic backend");
        return;
    }

    std::string status_filter = req.get_param_value("status");
    if (!status_filter.empty() && status_filter != "confirmed" && status_filter != "pending" &&
        status_filter != "testFailed") {
        write_error(res, 400, "BAD_REQUEST", "status must be confirmed|pending|testFailed");
        return;
    }

    sovd_fault_t *faults = nullptr;
    size_t count = 0;
    sovd_result_t r = e->vtable->read_faults(e->adapter_ctx, path.c_str(), &faults, &count);
    if (r != SOVD_OK) {
        write_error(res, http_status_for(r), "ADAPTER_ERROR", "read_faults failed");
        return;
    }

    json items = json::array();
    for (size_t i = 0; i < count; ++i) {
        if (!status_filter.empty() && status_filter != faults[i].status) continue;
        items.push_back({{"code", faults[i].code}, {"status", faults[i].status}});
    }
    if (faults && e->vtable->free_faults) e->vtable->free_faults(faults, count);

    res.set_content(json{{"faults", items}}.dump(), "application/json");
}

void Router::handle_clear_faults(const httplib::Request &req, httplib::Response &res, const std::string &path) {
    std::string corr = correlation_id_for(req, res);
    const Entity *e = require_entity(res, path);
    if (!e) return;
    if (try_forward(req, res, path)) return;
    if (!e->vtable || !e->vtable->clear_faults) {
        write_error(res, 501, "UNSUPPORTED", "entity has no diagnostic backend");
        return;
    }
    if (!check_lock_header(req, res, path)) return;

    sovd_result_t r = e->vtable->clear_faults(e->adapter_ctx, path.c_str());
    if (r != SOVD_OK) {
        write_error(res, http_status_for(r), "ADAPTER_ERROR", "clear_faults failed");
        return;
    }

    emit_event(event_sink_, "faults_cleared", {{"entity", path}, {"correlation_id", corr}});
    res.status = 204;
}
} // namespace sovd::server
