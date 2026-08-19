#include "sovd/server/routes.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <random>
#include <sstream>
#include <variant>
#include <vector>

#include "httplib.h"
#include "json.hpp"

namespace sovd::server {

using json = nlohmann::json;

namespace {

void emit_event(const Router::EventSink &sink, const std::string &type, json fields) {
    fields["event"] = type;
    fields["ts_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    sink(fields.dump());
}

// Not a counter: a counter is predictable and collides across restarts and
// (eventually) across gateway/domain-HPC instances. thread_local avoids
// locking a shared generator under httplib's worker-thread pool.
std::string generate_correlation_id() {
    thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist;
    char buf[24];
    std::snprintf(buf, sizeof(buf), "req-%016llx", static_cast<unsigned long long>(dist(rng)));
    return std::string(buf);
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

std::string to_hex_u16(uint16_t v) {
    char buf[5];
    std::snprintf(buf, sizeof(buf), "%04X", v);
    return std::string(buf);
}

// Shared by the single-item and batch data-read handlers: resolves
// id_or_did against the catalog (if any), calls the adapter, and decodes.
// Doesn't touch `res` — callers decide what to do with the outcome (a
// single 4xx/5xx for the single-item path, an inline per-item error that
// doesn't fail the rest of the batch for the batch path).
struct DataReadOutcome {
    bool ok = false;
    int status = 200;
    std::string error_code;
    std::string message;
    json body; // populated when ok
};

DataReadOutcome read_one_data_item(const Entity &e, const std::string &path, const std::string &id_or_did,
                                    const catalog::Catalog *cat) {
    const catalog::DataItem *item = cat ? cat->find_by_id(id_or_did) : nullptr;
    std::string did_hex = item ? to_hex_u16(item->did) : id_or_did;

    sovd_buffer_t buf{};
    sovd_result_t r = e.vtable->read_data(e.adapter_ctx, path.c_str(), did_hex.c_str(), &buf);
    if (r != SOVD_OK) {
        return DataReadOutcome{false, http_status_for(r), "ADAPTER_ERROR", "read_data failed", {}};
    }
    std::vector<uint8_t> bytes(buf.data, buf.data + buf.len);
    if (e.vtable->free_buffer) e.vtable->free_buffer(&buf);

    DataReadOutcome out;
    out.ok = true;
    if (item) {
        out.body["id"] = item->id;
        out.body["did"] = did_hex;
        try {
            auto decoded = catalog::Catalog::decode(*item, bytes);
            if (std::holds_alternative<std::string>(decoded)) {
                out.body["value"] = std::get<std::string>(decoded);
            } else {
                out.body["value"] = std::get<double>(decoded);
            }
        } catch (const catalog::CatalogError &ex) {
            return DataReadOutcome{false, 502, "ADAPTER_ERROR", std::string("catalog decode failed: ") + ex.what(),
                                    {}};
        }
        if (!item->encoding.unit.empty()) out.body["unit"] = item->encoding.unit;
    } else {
        out.body["id"] = id_or_did;
        out.body["value"] = to_hex(bytes.data(), bytes.size());
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
    : registry_(registry), locks_(locks), server_id_(std::move(server_id)), role_(std::move(role)),
      event_sink_([](const std::string &line) { std::cout << line << std::endl; }) {}

void Router::set_event_sink(EventSink sink) { event_sink_ = std::move(sink); }

std::string Router::correlation_id_for(const httplib::Request &req, httplib::Response &res) const {
    std::string id = req.get_header_value("X-SOVD-Correlation-Id");
    if (id.empty()) id = generate_correlation_id();
    res.set_header("X-SOVD-Correlation-Id", id);
    return id;
}

void Router::register_routes(httplib::Server &svr) {
    // Root is deliberately unversioned: a client that has never seen this
    // server before hits / first to learn what's available (api_versions)
    // before it knows which prefix to use. Everything else is versioned —
    // path prefix over an Accept header, settled in CLAUDE.md: uglier, but
    // unambiguous, and safety-adjacent APIs shouldn't leave version
    // negotiation implicit.
    svr.Get("/", [this](const httplib::Request &req, httplib::Response &res) { handle_root(req, res); });
    svr.Get("/v1/entities",
            [this](const httplib::Request &req, httplib::Response &res) { handle_list_entities(req, res); });

    svr.Get(R"(/v1/entities/(.+)/faults)", [this](const httplib::Request &req, httplib::Response &res) {
        handle_get_faults(req, res, req.matches[1]);
    });
    svr.Delete(R"(/v1/entities/(.+)/faults)", [this](const httplib::Request &req, httplib::Response &res) {
        handle_clear_faults(req, res, req.matches[1]);
    });

    // No trailing id: batch read via ?ids=a,b,c. Registered before the
    // single-item pattern for readability; the two never actually collide
    // since one requires a further "/<id>" segment and the other forbids it.
    svr.Get(R"(/v1/entities/(.+)/data)", [this](const httplib::Request &req, httplib::Response &res) {
        handle_get_data_batch(req, res, req.matches[1]);
    });
    svr.Get(R"(/v1/entities/(.+)/data/(.+))", [this](const httplib::Request &req, httplib::Response &res) {
        handle_get_data(req, res, req.matches[1], req.matches[2]);
    });
    svr.Put(R"(/v1/entities/(.+)/data/(.+))", [this](const httplib::Request &req, httplib::Response &res) {
        handle_put_data(req, res, req.matches[1], req.matches[2]);
    });

    svr.Post(R"(/v1/entities/(.+)/modes)", [this](const httplib::Request &req, httplib::Response &res) {
        handle_post_mode(req, res, req.matches[1]);
    });
    svr.Post(R"(/v1/entities/(.+)/operations/(.+))", [this](const httplib::Request &req, httplib::Response &res) {
        handle_post_operation(req, res, req.matches[1], req.matches[2]);
    });

    svr.Post(R"(/v1/entities/(.+)/locks)", [this](const httplib::Request &req, httplib::Response &res) {
        handle_post_lock(req, res, req.matches[1]);
    });
    svr.Delete(R"(/v1/entities/(.+)/locks/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handle_delete_lock(req, res, req.matches[1], req.matches[2]);
    });

    svr.Get(R"(/v1/entities/(.+)/docs)", [this](const httplib::Request &req, httplib::Response &res) {
        handle_get_docs(req, res, req.matches[1]);
    });
}

void Router::attach_catalog(const std::string &entity_path, catalog::Catalog cat) {
    catalogs_.insert_or_assign(entity_path, std::move(cat));
}

const catalog::Catalog *Router::find_catalog(const std::string &entity_path) const {
    auto it = catalogs_.find(entity_path);
    if (it == catalogs_.end()) return nullptr;
    return &it->second;
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

void Router::handle_root(const httplib::Request &req, httplib::Response &res) {
    correlation_id_for(req, res);
    json body = {
        {"server_id", server_id_},
        {"role", role_},
        {"sovd_version", "phase0-demo"},
        {"api_versions", json::array({"v1"})},
    };
    res.set_content(body.dump(), "application/json");
}

void Router::handle_list_entities(const httplib::Request &req, httplib::Response &res) {
    correlation_id_for(req, res);
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

void Router::handle_get_faults(const httplib::Request &req, httplib::Response &res, const std::string &path) {
    correlation_id_for(req, res);
    const Entity *e = require_entity(res, path);
    if (!e) return;
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

void Router::handle_get_data(const httplib::Request &req, httplib::Response &res, const std::string &path,
                              const std::string &id_or_did) {
    correlation_id_for(req, res);
    const Entity *e = require_entity(res, path);
    if (!e) return;
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
    // degradation principle (CLAUDE.md, Phase 4), applied one level down:
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

    // The catalog resolves named ids -> DIDs above; it doesn't yet encode
    // typed values -> bytes (no caller needed that before this), so the
    // wire format stays hex bytes regardless of whether the path was named.
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

void Router::handle_post_mode(const httplib::Request &req, httplib::Response &res, const std::string &path) {
    std::string corr = correlation_id_for(req, res);
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

    emit_event(event_sink_, "mode_changed", {{"entity", path}, {"mode", mode}, {"correlation_id", corr}});
    res.status = 204;
}

void Router::handle_post_operation(const httplib::Request &req, httplib::Response &res, const std::string &path,
                                    const std::string &op) {
    std::string corr = correlation_id_for(req, res);
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

void Router::handle_post_lock(const httplib::Request &req, httplib::Response &res, const std::string &path) {
    std::string corr = correlation_id_for(req, res);
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
        emit_event(event_sink_, "lock_denied", {{"entity", path}, {"correlation_id", corr}});
        write_error(res, 423, "LOCKED", "entity is already locked");
        return;
    }

    emit_event(event_sink_, "lock_acquired", {{"entity", path}, {"lock_id", *lock_id}, {"correlation_id", corr}});
    res.status = 201;
    res.set_content(json{{"lock_id", *lock_id}, {"ttl_seconds", ttl}}.dump(), "application/json");
}

void Router::handle_delete_lock(const httplib::Request &req, httplib::Response &res, const std::string &path,
                                 const std::string &lock_id) {
    std::string corr = correlation_id_for(req, res);
    const Entity *e = require_entity(res, path);
    if (!e) return;

    switch (locks_.release(path, lock_id)) {
        case LockReleaseResult::Released:
            emit_event(event_sink_, "lock_released", {{"entity", path}, {"lock_id", lock_id}, {"correlation_id", corr}});
            res.status = 204;
            break;
        case LockReleaseResult::WrongId:
            emit_event(event_sink_, "lock_release_mismatch",
                       {{"entity", path}, {"lock_id", lock_id}, {"correlation_id", corr}});
            write_error(res, 403, "FORBIDDEN", "lock_id does not match holder");
            break;
        case LockReleaseResult::NotFound:
            write_error(res, 404, "NOT_FOUND", "no active lock on entity");
            break;
    }
}

void Router::handle_get_docs(const httplib::Request &req, httplib::Response &res, const std::string &path) {
    correlation_id_for(req, res);
    const Entity *e = require_entity(res, path);
    if (!e) return;

    json body = {
        {"path", path},
        {"type", entity_type_to_string(e->type)},
        {"has_backend", e->has_backend()},
        {"data", json::array()},
        {"operations", json::array()},
    };

    if (e->vtable) {
        body["capabilities"] = {
            {"supports_batch_read", e->vtable->capabilities.supports_batch_read},
            {"supports_async_operations", e->vtable->capabilities.supports_async_operations},
            {"supports_io_control", e->vtable->capabilities.supports_io_control},
        };
    }

    const catalog::Catalog *cat = find_catalog(path);
    if (!cat) {
        res.set_content(body.dump(), "application/json");
        return;
    }

    for (auto &item : cat->data()) {
        json d = {
            {"id", item.id},
            {"did", to_hex_u16(item.did)},
            {"type", catalog::data_type_to_string(item.type)},
            {"access", catalog::access_to_string(item.access)},
        };
        if (item.io_control) d["io_control"] = true;
        if (item.requires_session) d["requires_session"] = *item.requires_session;

        switch (item.type) {
            case catalog::DataType::String:
                if (item.length > 0) d["length"] = item.length;
                break;
            case catalog::DataType::Float:
                d["unit"] = item.encoding.unit;
                d["scale"] = item.encoding.scale;
                break;
            case catalog::DataType::Enum: {
                json values = json::object();
                for (auto &ev : item.values) values[std::to_string(ev.raw)] = ev.label;
                d["values"] = values;
                break;
            }
            case catalog::DataType::Raw:
                break;
        }
        body["data"].push_back(d);
    }

    for (auto &op : cat->operations()) {
        json o = {
            {"id", op.id},
            {"routine_id", to_hex_u16(op.routine_id)},
            {"async", op.async},
        };
        if (op.requires_session) o["requires_session"] = *op.requires_session;
        body["operations"].push_back(o);
    }

    res.set_content(body.dump(), "application/json");
}

} // namespace sovd::server
