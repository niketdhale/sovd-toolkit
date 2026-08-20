// Router — translates HTTP <-> core calls. Nothing else lives here: no
// diagnostic logic, no adapter-specific behavior.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "sovd/catalog/did_catalog.hpp"
#include "sovd/entity_registry.hpp"
#include "sovd/lock_manager.hpp"
#include "sovd/server/stream_hub.hpp"

namespace httplib {
class Server;
class Request;
class Response;
class Client;
} // namespace httplib

namespace sovd::server {

// Phase 4: a local entity path can be backed by a remote SOVD server
// instead of a local adapter. CLAUDE.md's original sketch ("adapters/
// sovd_proxy — same vtable") doesn't survive Phase 1's typed /docs and
// named-id decoding: the vtable's read_data only carries raw
// sovd_buffer_t bytes, but a proxy has to pass through the remote's
// already-decoded JSON (battery_voltage -> 13.0 V) verbatim — and it must,
// since "gateway needs no DID catalogs at all" (CLAUDE.md) rules out
// decoding locally. So this is a Router-level HTTP-forwarding table, not a
// vtable adapter: same one-lookup-then-dispatch shape as find_catalog(),
// just forwarding the whole request/response instead of decoding bytes.
// Locks are always forwarded too (never a toggle) — CLAUDE.md: "forward
// must NEVER cache lock state locally."
struct ProxyTarget {
    std::string base_url;   // e.g. "http://127.0.0.1:20003"
    std::string remote_path; // full path on the remote server
};

// Phase 8: one persistent httplib::Client per proxied entity (created
// alongside its ProxyTarget in attach_proxy(), before svr.listen() starts),
// reused across requests instead of paying a fresh TCP handshake every
// forward -- the proxy-forwarding analogue of uds_doip's DoipTransport
// already being a persistent per-adapter-instance connection. Defined in
// routes.cpp (needs httplib::Client's complete type); forward-declared here
// so routes.hpp doesn't have to include httplib.h just for this member.
class ProxyConnection;

class Router {
public:
    // Receives one already-serialized JSON event line. Default sink writes
    // it to stdout (matching Phase 0/3's existing behavior); tests swap it
    // to capture events without redirecting the real stdout, and Phase 3's
    // MQTT transport is a drop-in replacement for the same seam.
    using EventSink = std::function<void(const std::string &)>;

    Router(EntityRegistry &registry, LockManager &locks, std::string server_id, std::string role);
    // Out-of-line: proxy_connections_ holds unique_ptr<ProxyConnection>, and
    // ProxyConnection is only a complete type in routes.cpp.
    ~Router();

    // Phase 4: the config loader needs a Router to exist before it can
    // attach catalogs/proxies while parsing entities, but server_id/role
    // only become known once that same parse reaches the `server:` block —
    // so main.cpp constructs Router with placeholders and corrects them
    // here once load_topology_from_file() returns. Only handle_root() (a
    // per-request, lazily-called handler) ever reads these, so this just
    // has to land before svr.listen(), not before construction.
    void set_server_id(std::string id);
    void set_role(std::string role);

    void register_routes(httplib::Server &svr);
    void set_event_sink(EventSink sink);

    // Phase 3: operational telemetry (per-request latency) is a *separate*
    // sink from set_event_sink's security-relevant events — "an IDS should
    // not be your APM" (CLAUDE.md). Same stdout-by-default seam either way.
    void set_telemetry_sink(EventSink sink);

    // Associates a DID catalog with an entity path, enabling named data
    // paths (/data/battery_voltage) and typed /docs output for it. Catalogs
    // are per-ECU-software-version, kept separate from topology (see
    // CLAUDE.md) — an entity with no attached catalog still works, just
    // falls back to raw hex DIDs and an empty /docs data/operations list.
    void attach_catalog(const std::string &entity_path, catalog::Catalog cat);

    // Phase 4: marks entity_path as proxied to a remote SOVD server. See
    // ProxyTarget's comment for why this bypasses the vtable entirely.
    void attach_proxy(const std::string &entity_path, ProxyTarget target);

    // B2 (Phase 7 blocker): an explicit allow-list, never "*" -- this is a
    // diagnostic interface on a safety-adjacent device. Empty (the default)
    // means no CORS headers are ever sent, i.e. every browser origin is
    // denied by the browser's own same-origin policy; CORS is opt-in
    // per-deployment, not on by default.
    void set_cors_allowed_origins(std::vector<std::string> origins);

    // Phase 8: bearer-token + scope validation at the external boundary.
    // Opt-in (empty = disabled, matching CORS/the audit log's opt-in
    // shape) -- unset means no auth check at all, same "off by default"
    // reasoning as everything else added in Phase 8. See oauth2.hpp for
    // what the token scheme does and deliberately doesn't cover.
    void set_oauth2_secret(std::string secret);

private:
    void handle_root(const httplib::Request &req, httplib::Response &res);
    void handle_list_entities(const httplib::Request &req, httplib::Response &res);
    void handle_get_faults(const httplib::Request &req, httplib::Response &res, const std::string &path);
    void handle_clear_faults(const httplib::Request &req, httplib::Response &res, const std::string &path);
    void handle_get_data(const httplib::Request &req, httplib::Response &res, const std::string &path,
                          const std::string &id_or_did);
    void handle_get_data_batch(const httplib::Request &req, httplib::Response &res, const std::string &path);
    void handle_put_data(const httplib::Request &req, httplib::Response &res, const std::string &path,
                          const std::string &id_or_did);
    void handle_post_mode(const httplib::Request &req, httplib::Response &res, const std::string &path);
    void handle_post_operation(const httplib::Request &req, httplib::Response &res, const std::string &path,
                                const std::string &op);
    void handle_post_lock(const httplib::Request &req, httplib::Response &res, const std::string &path);
    void handle_put_lock(const httplib::Request &req, httplib::Response &res, const std::string &path,
                          const std::string &lock_id);
    void handle_delete_lock(const httplib::Request &req, httplib::Response &res, const std::string &path,
                             const std::string &lock_id);
    void handle_get_docs(const httplib::Request &req, httplib::Response &res, const std::string &path);
    // Phase 6: SSE. Shares read_one_data_item's decode logic (routes.cpp's
    // anonymous namespace) with the plain GET, so a client switching from
    // polling to streaming sees byte-identical JSON per event -- only the
    // transport changes.
    void handle_stream_data(const httplib::Request &req, httplib::Response &res, const std::string &path,
                             const std::string &id_or_did);

    const Entity *require_entity(httplib::Response &res, const std::string &path);
    bool check_lock_header(const httplib::Request &req, httplib::Response &res, const std::string &path);
    const catalog::Catalog *find_catalog(const std::string &entity_path) const;
    const ProxyTarget *find_proxy(const std::string &entity_path) const;

    // If entity_path is proxied, forwards the whole request (method, the
    // URL suffix past .../entities/{entity_path}, query params, body, the
    // lock-id/correlation-id headers) to the remote server and copies its
    // response into res, returning true. Returns false (res untouched) if
    // there's no proxy for this path — callers fall through to local
    // vtable/catalog handling. Called right after require_entity(), before
    // any vtable/lock checks: those are meaningless against a path whose
    // real backend and lock authority live on a different server.
    bool try_forward(const httplib::Request &req, httplib::Response &res, const std::string &entity_path);

    // Echoes X-SOVD-Correlation-Id if the client supplied one, generates one
    // otherwise; always stamps it on the response header (success or error)
    // and returns it so the handler can attach it to any emitted event.
    // Clocks may not be synced across a gateway/domain-HPC hop — this is
    // what lets an event get correlated to the request that caused it
    // instead, which timestamps alone can't guarantee (CLAUDE.md).
    std::string correlation_id_for(const httplib::Request &req, httplib::Response &res) const;

    // B2: if req's Origin header is in the allow-list, stamps
    // Access-Control-Allow-Origin (+ Access-Control-Expose-Headers for
    // X-SOVD-Correlation-Id) on res. A no-op (res untouched) for a missing
    // or non-allow-listed Origin -- the browser's own same-origin policy is
    // what actually blocks the response in that case, not this method.
    void apply_cors_headers(const httplib::Request &req, httplib::Response &res) const;

    EntityRegistry &registry_;
    LockManager &locks_;
    std::string server_id_;
    std::string role_;
    std::unordered_map<std::string, catalog::Catalog> catalogs_;
    std::unordered_map<std::string, ProxyTarget> proxies_;
    std::unordered_map<std::string, std::unique_ptr<ProxyConnection>> proxy_connections_;
    EventSink event_sink_;
    EventSink telemetry_sink_;
    StreamHub stream_hub_;
    std::vector<std::string> cors_allowed_origins_;
    std::string oauth2_secret_;
};

} // namespace sovd::server
