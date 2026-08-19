#include "sovd/server/routes.hpp"

#include <chrono>
#include <cstdint>
#include <cctype>
#include <iostream>
#include <vector>

#include "httplib.h"
#include "json.hpp"

namespace sovd::server {

using json = nlohmann::json;

namespace {

void emit_event(const std::string &type, json fields) {
    fields["event"] = type;
    fields["ts_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    std::cout << fields.dump() << std::endl;
}

int http_status_for(sovd_result_t r) {
    switch (r) {
        case SOVD_OK: return 200;
        case SOVD_NOT_FOUND: return 404;
        case SOVD_LOCKED: return 423;
        case SOVD_BAD_REQUEST: return 400;
        case SOVD_UNSUPPORTED: return 501;
        case SOVD_BUSY: return 503;
        case SOVD_TRANSPORT:
        case SOVD_NEGATIVE_RESPONSE: return 502;
        case SOVD_INTERNAL: return 500;
    }
    return 500;
}

void write_error(httplib::Response &res, int status, const std::string &code, const std::string &message) {
    res.status = status;
    res.set_content(json{{"error", code}, {"message", message}}.dump(), "application/json");
}

std::string to_hex(const uint8_t *data, size_t len) {
    static const char *digits = "0123456789ABCDEF";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0x0F]);
    }
    return out;
}

bool from_hex(std::string s, std::vector<uint8_t> &out) {
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    if (s.empty() || s.size() % 2 != 0) return false;
    out.clear();
    out.reserve(s.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = nibble(s[i]);
        int lo = nibble(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

} // namespace

Router::Router(EntityRegistry &registry, LockManager &locks, std::string server_id, std::string role)
    : registry_(registry), locks_(locks), server_id_(std::move(server_id)), role_(std::move(role)) {}

void Router::register_routes(httplib::Server &svr) {
    svr.Get("/", [this](const httplib::Request &, httplib::Response &res) { handle_root(res); });
    svr.Get("/entities", [this](const httplib::Request &, httplib::Response &res) { handle_list_entities(res); });

    svr.Get(R"(/entities/(.+)/faults)", [this](const httplib::Request &req, httplib::Response &res) {
        handle_get_faults(req, res, req.matches[1]);
    });
    svr.Delete(R"(/entities/(.+)/faults)", [this](const httplib::Request &req, httplib::Response &res) {
        handle_clear_faults(req, res, req.matches[1]);
    });

    svr.Get(R"(/entities/(.+)/data/(.+))", [this](const httplib::Request &req, httplib::Response &res) {
        handle_get_data(req, res, req.matches[1], req.matches[2]);
    });
    svr.Put(R"(/entities/(.+)/data/(.+))", [this](const httplib::Request &req, httplib::Response &res) {
        handle_put_data(req, res, req.matches[1], req.matches[2]);
    });

    svr.Post(R"(/entities/(.+)/modes)", [this](const httplib::Request &req, httplib::Response &res) {
        handle_post_mode(req, res, req.matches[1]);
    });
    svr.Post(R"(/entities/(.+)/operations/(.+))", [this](const httplib::Request &req, httplib::Response &res) {
        handle_post_operation(req, res, req.matches[1], req.matches[2]);
    });

    svr.Post(R"(/entities/(.+)/locks)", [this](const httplib::Request &req, httplib::Response &res) {
        handle_post_lock(req, res, req.matches[1]);
    });
    svr.Delete(R"(/entities/(.+)/locks/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handle_delete_lock(req, res, req.matches[1], req.matches[2]);
    });
}

const Entity *Router::require_entity(httplib::Response &res, const std::string &path) {
    const Entity *e = registry_.find(path);
    if (!e) {
        write_error(res, 404, "NOT_FOUND", "unknown entity: " + path);
        return nullptr;
    }
    return e;
}

bool Router::check_lock_header(const httplib::Request &req, httplib::Response &res, const std::string &path) {
    std::string header = req.get_header_value("X-SOVD-Lock-Id");
    if (!locks_.check_lock(path, header)) {
        write_error(res, 423, "LOCKED", "entity is locked; supply X-SOVD-Lock-Id header");
        return false;
    }
    return true;
}

void Router::handle_root(httplib::Response &res) {
    json body = {
        {"server_id", server_id_},
        {"role", role_},
        {"sovd_version", "phase0-demo"},
    };
    res.set_content(body.dump(), "application/json");
}

void Router::handle_list_entities(httplib::Response &res) {
    json items = json::array();
    for (auto *e : registry_.list_all()) {
        items.push_back({
            {"path", e->path},
            {"type", entity_type_to_string(e->type)},
            {"has_backend", e->has_backend()},
        });
    }
    res.set_content(json{{"items", items}}.dump(), "application/json");
}

void Router::handle_get_faults(const httplib::Request &, httplib::Response &res, const std::string &path) {
    const Entity *e = require_entity(res, path);
    if (!e) return;
    if (!e->vtable || !e->vtable->read_faults) {
        write_error(res, 501, "UNSUPPORTED", "entity has no diagnostic backend");
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
        items.push_back({{"code", faults[i].code}, {"status", faults[i].status}});
    }
    if (faults && e->vtable->free_faults) e->vtable->free_faults(faults, count);

    res.set_content(json{{"faults", items}}.dump(), "application/json");
}

void Router::handle_clear_faults(const httplib::Request &req, httplib::Response &res, const std::string &path) {
    const Entity *e = require_entity(res, path);
    if (!e) return;
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

    emit_event("faults_cleared", {{"entity", path}});
    res.status = 204;
}

void Router::handle_get_data(const httplib::Request &, httplib::Response &res, const std::string &path,
                              const std::string &did) {
    const Entity *e = require_entity(res, path);
    if (!e) return;
    if (!e->vtable || !e->vtable->read_data) {
        write_error(res, 501, "UNSUPPORTED", "entity has no diagnostic backend");
        return;
    }

    sovd_buffer_t buf{};
    sovd_result_t r = e->vtable->read_data(e->adapter_ctx, path.c_str(), did.c_str(), &buf);
    if (r != SOVD_OK) {
        write_error(res, http_status_for(r), "ADAPTER_ERROR", "read_data failed");
        return;
    }

    json body = {{"id", did}, {"value", to_hex(buf.data, buf.len)}};
    if (e->vtable->free_buffer) e->vtable->free_buffer(&buf);

    res.set_content(body.dump(), "application/json");
}

void Router::handle_put_data(const httplib::Request &req, httplib::Response &res, const std::string &path,
                              const std::string &did) {
    const Entity *e = require_entity(res, path);
    if (!e) return;
    if (!e->vtable || !e->vtable->write_data) {
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
    if (!parsed.contains("value") || !parsed["value"].is_string()) {
        write_error(res, 400, "BAD_REQUEST", R"(expected {"value": "<hex>"})");
        return;
    }

    std::vector<uint8_t> bytes;
    if (!from_hex(parsed["value"].get<std::string>(), bytes)) {
        write_error(res, 400, "BAD_REQUEST", "value must be hex-encoded bytes");
        return;
    }

    sovd_result_t r = e->vtable->write_data(e->adapter_ctx, path.c_str(), did.c_str(), bytes.data(), bytes.size());
    if (r != SOVD_OK) {
        write_error(res, http_status_for(r), "ADAPTER_ERROR", "write_data failed");
        return;
    }

    emit_event("data_written", {{"entity", path}, {"id", did}});
    res.status = 204;
}

void Router::handle_post_mode(const httplib::Request &req, httplib::Response &res, const std::string &path) {
    const Entity *e = require_entity(res, path);
    if (!e) return;
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

    emit_event("mode_changed", {{"entity", path}, {"mode", mode}});
    res.status = 204;
}

void Router::handle_post_operation(const httplib::Request &req, httplib::Response &res, const std::string &path,
                                    const std::string &op) {
    const Entity *e = require_entity(res, path);
    if (!e) return;
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

    emit_event("operation_executed", {{"entity", path}, {"operation", op}});

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

void Router::handle_post_lock(const httplib::Request &req, httplib::Response &res, const std::string &path) {
    const Entity *e = require_entity(res, path);
    if (!e) return;

    int ttl = 60;
    if (!req.body.empty()) {
        try {
            json parsed = json::parse(req.body);
            if (parsed.contains("ttl_seconds") && parsed["ttl_seconds"].is_number_integer()) {
                ttl = parsed["ttl_seconds"].get<int>();
            }
        } catch (...) {
            write_error(res, 400, "BAD_REQUEST", "invalid JSON body");
            return;
        }
    }
    if (ttl <= 0) {
        write_error(res, 400, "BAD_REQUEST", "ttl_seconds must be positive");
        return;
    }

    auto lock_id = locks_.acquire(path, ttl);
    if (!lock_id) {
        emit_event("lock_denied", {{"entity", path}});
        write_error(res, 423, "LOCKED", "entity is already locked");
        return;
    }

    emit_event("lock_acquired", {{"entity", path}, {"lock_id", *lock_id}});
    res.status = 201;
    res.set_content(json{{"lock_id", *lock_id}, {"ttl_seconds", ttl}}.dump(), "application/json");
}

void Router::handle_delete_lock(const httplib::Request &, httplib::Response &res, const std::string &path,
                                 const std::string &lock_id) {
    const Entity *e = require_entity(res, path);
    if (!e) return;

    switch (locks_.release(path, lock_id)) {
        case LockReleaseResult::Released:
            emit_event("lock_released", {{"entity", path}, {"lock_id", lock_id}});
            res.status = 204;
            break;
        case LockReleaseResult::WrongId:
            emit_event("lock_release_mismatch", {{"entity", path}, {"lock_id", lock_id}});
            write_error(res, 403, "FORBIDDEN", "lock_id does not match holder");
            break;
        case LockReleaseResult::NotFound:
            write_error(res, 404, "NOT_FOUND", "no active lock on entity");
            break;
    }
}

} // namespace sovd::server
