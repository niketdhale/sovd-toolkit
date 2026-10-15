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

// Phase 8: see the comment at its one call site (handle_post_lock) for why
// this is also the effective cap on concurrently escalated UDS sessions.
// Generous for any real vehicle's entity count, still a real ceiling.
constexpr size_t kMaxConcurrentLocks = 64;


void Router::handle_post_lock(const httplib::Request &req, httplib::Response &res, const std::string &path) {
    std::string corr = correlation_id_for(req, res);
    const Entity *e = require_entity(res, path);
    if (!e) return;
    if (try_forward(req, res, path)) return;

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

    // Phase 8: server-wide cap on concurrently held locks, which -- since
    // session escalation only ever happens from a lock-gated call
    // (docs/DESIGN.md's settled session-manager-ownership decision) -- doubles
    // as a cap on concurrently escalated UDS sessions with no separate
    // session-tracking needed. A capacity rejection is BUSY/503 (transient,
    // retry-worthy), distinct from LOCKED/423 (a specific conflict that
    // retrying won't resolve until someone else releases).
    if (locks_.held_lock_count() >= kMaxConcurrentLocks) {
        emit_event(event_sink_, "lock_denied", {{"entity", path}, {"correlation_id", corr}, {"reason", "capacity"}});
        write_error(res, 503, "BUSY", "server-wide lock capacity reached");
        return;
    }

    auto lock_id = locks_.acquire(path, ttl);
    if (!lock_id) {
        emit_event(event_sink_, "lock_denied", {{"entity", path}, {"correlation_id", corr}});
        write_error(res, 423, "LOCKED", "entity is already locked");
        return;
    }

    // ttl_seconds echoed back is what LockManager actually granted (it
    // clamps to kMaxLockTtlSeconds internally) -- not the raw request, which
    // would otherwise tell a client its 999999s lock was honored verbatim.
    int granted_ttl = std::min(ttl, kMaxLockTtlSeconds);
    emit_event(event_sink_, "lock_acquired", {{"entity", path}, {"lock_id", *lock_id}, {"correlation_id", corr}});
    res.status = 201;
    res.set_content(json{{"lock_id", *lock_id}, {"ttl_seconds", granted_ttl}}.dump(), "application/json");
}

void Router::handle_put_lock(const httplib::Request &req, httplib::Response &res, const std::string &path,
                              const std::string &lock_id) {
    std::string corr = correlation_id_for(req, res);
    const Entity *e = require_entity(res, path);
    if (!e) return;
    if (try_forward(req, res, path)) return;

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

    switch (locks_.renew(path, lock_id, ttl)) {
        case LockRenewResult::Renewed: {
            int granted_ttl = std::min(ttl, kMaxLockTtlSeconds); // echo what was actually granted, not the raw request
            emit_event(event_sink_, "lock_renewed", {{"entity", path}, {"lock_id", lock_id}, {"correlation_id", corr}});
            res.set_content(json{{"lock_id", lock_id}, {"ttl_seconds", granted_ttl}}.dump(), "application/json");
            break;
        }
        case LockRenewResult::WrongId:
            // Own event name (not lock_release_mismatch) since this wasn't
            // a release attempt -- same security signature though (wrong
            // lock_id against a held lock), so Phase 3's alert query below
            // is extended to match both rather than adding a second rule.
            emit_event(event_sink_, "lock_renew_mismatch",
                       {{"entity", path}, {"lock_id", lock_id}, {"correlation_id", corr}});
            write_error(res, 403, "FORBIDDEN", "lock_id does not match holder");
            break;
        case LockRenewResult::NotFound:
            write_error(res, 404, "NOT_FOUND", "no active lock on entity");
            break;
    }
}

void Router::handle_delete_lock(const httplib::Request &req, httplib::Response &res, const std::string &path,
                                 const std::string &lock_id) {
    std::string corr = correlation_id_for(req, res);
    const Entity *e = require_entity(res, path);
    if (!e) return;
    if (try_forward(req, res, path)) return;

    switch (locks_.release(path, lock_id)) {
        case LockReleaseResult::Released:
            // Session lifetime follows lock lifetime (docs/DESIGN.md's "Session
            // manager ownership"): tear down whatever session-like state
            // the backend opened for this lock, via the same set_mode
            // entry point a client could call directly. core/server never
            // learn a UDS session exists — this just fires the existing
            // mode-change hook. Best-effort: the lock is already released
            // either way, so the result isn't surfaced to the client (a
            // failed/slow backend teardown shouldn't turn a successful
            // unlock into an error response).
            if (e->vtable && e->vtable->set_mode) {
                e->vtable->set_mode(e->adapter_ctx, path.c_str(), "default");
            }
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
} // namespace sovd::server
