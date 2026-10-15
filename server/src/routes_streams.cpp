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


void Router::handle_stream_data(const httplib::Request &req, httplib::Response &res, const std::string &path,
                                 const std::string &id_or_did) {
    correlation_id_for(req, res);

    // Task 1c: this route is exempted from the generic pre-routing oauth2
    // check (see check_oauth2()'s comment on stream_pattern) because a
    // browser EventSource can't set an Authorization header. Accept either
    // a normal bearer token (curl, the CLI, any non-browser client) or a
    // single-use ticket minted by POST .../stream-ticket -- what the web
    // UI's EventSource actually opens the connection with. has_oauth2_scope
    // already no-ops to true when OAuth2 is disabled, so this whole check
    // collapses to "always authorized" in that case, same as every other
    // route.
    bool authorized = has_oauth2_scope(req, "read:data");
    if (!authorized) {
        std::string ticket = req.get_param_value("ticket");
        authorized = !ticket.empty() && stream_tickets_.redeem(ticket, path, id_or_did);
    }
    if (!authorized) {
        std::string corr = correlation_id_for(req, res);
        emit_event(event_sink_, "oauth2_denied",
                   {{"method", "GET"}, {"path", req.path}, {"status", 401}, {"correlation_id", corr}});
        res.set_header("WWW-Authenticate", "Bearer");
        write_error(res, 401, "UNAUTHORIZED", "missing or invalid bearer token or stream ticket");
        return;
    }

    const Entity *e = require_entity(res, path);
    if (!e) return;
    if (find_proxy(path)) {
        // try_forward()'s one-shot request/response round trip can't carry
        // a held-open SSE stream through -- that's a real second feature
        // (bidirectional stream proxying), not attempted here. Failing
        // clearly beats silently mis-forwarding a truncated response.
        write_error(res, 501, "UNSUPPORTED", "streaming through a Phase 4 proxy is not supported");
        return;
    }
    if (!e->vtable || !e->vtable->read_data) {
        write_error(res, 501, "UNSUPPORTED", "entity has no diagnostic backend");
        return;
    }

    int interval_ms = 1000;
    if (req.has_param("interval_ms")) {
        interval_ms = std::max(100, std::atoi(req.get_param_value("interval_ms").c_str()));
    }

    const catalog::Catalog *cat = find_catalog(path);
    auto read_fn = [e, path, id_or_did, cat]() -> std::string {
        auto outcome = read_one_data_item(*e, path, id_or_did, cat);
        json body = outcome.ok ? outcome.body
                                : json{{"id", id_or_did}, {"error", outcome.error_code}, {"message", outcome.message}};
        return body.dump();
    };

    // shared_ptr, not the Subscription itself: std::function (which
    // set_chunked_content_provider's callback is) requires its target to
    // be copy-constructible, and Subscription is deliberately move-only
    // (RAII unsubscribe must happen exactly once).
    auto sub = std::make_shared<StreamHub::Subscription>(stream_hub_.subscribe(path, id_or_did, interval_ms, read_fn));

    res.set_header("Cache-Control", "no-cache");
    res.set_chunked_content_provider("text/event-stream", [sub](size_t /*offset*/, httplib::DataSink &sink) {
        std::string json_line;
        // wait_next's own timeout (well under any sane read-timeout on the
        // client side) is what turns an idle stream into periodic
        // ": keep-alive" comments instead of a connection that looks dead.
        if (sub->wait_next(json_line, 15000)) {
            std::string frame = "data: " + json_line + "\n\n";
            if (!sink.is_writable() || !sink.write(frame.data(), frame.size())) return false;
        } else {
            static const char ka[] = ": keep-alive\n\n";
            if (!sink.is_writable() || !sink.write(ka, sizeof(ka) - 1)) return false;
        }
        return sink.is_writable();
    });
}

// Task 1c: mints a StreamTicketStore ticket for (path, id_or_did). A normal
// bearer-authenticated route (see its oauth2_scope_table() entry, read:data)
// -- the whole reason it exists is that the *stream itself* can't be gated
// the same way. Doesn't validate that id_or_did actually resolves against
// the catalog/adapter; the stream handler still does that real work, this
// just proves "an authorized caller asked for this (path, id) recently".
void Router::handle_post_stream_ticket(const httplib::Request &req, httplib::Response &res, const std::string &path,
                                        const std::string &id_or_did) {
    std::string corr = correlation_id_for(req, res);
    const Entity *e = require_entity(res, path);
    if (!e) return;
    // docs/reviews/round-2.md Task 10: capacity-checked the same shape as
    // handle_post_lock's kMaxConcurrentLocks -- 503/BUSY is transient and
    // retry-worthy, distinct from a hard rejection.
    auto ticket = stream_tickets_.issue(path, id_or_did);
    if (!ticket) {
        emit_event(event_sink_, "stream_ticket_denied",
                   {{"entity", path}, {"id", id_or_did}, {"correlation_id", corr}, {"reason", "capacity"}});
        write_error(res, 503, "BUSY", "server-wide stream-ticket capacity reached");
        return;
    }
    json body;
    body["ticket"] = *ticket;
    body["ttl_seconds"] = kStreamTicketTtlSeconds;
    res.status = 201;
    res.set_content(body.dump(), "application/json");
}
} // namespace sovd::server
