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

namespace {



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



// Phase 8: default-deny whitelist for gateway-role servers. Same patterns as
// register_routes() below -- kept as a second, explicit list rather than
// derived from httplib's internal handler table (it doesn't expose one to
// introspect) so this is an auditable security policy, not an accident of
// whatever got registered. A gateway forwards the same resource surface a
// domain server answers directly (docs/DESIGN.md's role model), so today this
// mirrors every real route; the point is a *future* route added to
// register_routes() without a matching entry here is unreachable through a
// gateway by default instead of silently exposed. test_core has a drift
// check that walks every real route through this list with role=gateway.
const std::vector<std::pair<std::string, std::regex>> &gateway_route_whitelist() {
    static const std::vector<std::pair<std::string, std::regex>> routes = {
        {"GET", std::regex(R"(^/$)")},
        {"GET", std::regex(R"(^/v1/entities$)")},
        {"GET", std::regex(R"(^/v1/entities/.+/faults$)")},
        {"DELETE", std::regex(R"(^/v1/entities/.+/faults$)")},
        {"GET", std::regex(R"(^/v1/entities/.+/data$)")},
        {"GET", std::regex(R"(^/v1/entities/.+/data/.+/stream$)")},
        {"GET", std::regex(R"(^/v1/entities/.+/data/.+$)")},
        {"PUT", std::regex(R"(^/v1/entities/.+/data/.+$)")},
        {"POST", std::regex(R"(^/v1/entities/.+/modes$)")},
        {"POST", std::regex(R"(^/v1/entities/.+/operations/.+$)")},
        {"POST", std::regex(R"(^/v1/entities/.+/locks$)")},
        {"PUT", std::regex(R"(^/v1/entities/.+/locks/[^/]+$)")},
        {"DELETE", std::regex(R"(^/v1/entities/.+/locks/[^/]+$)")},
        {"GET", std::regex(R"(^/v1/entities/.+/docs$)")},
    };
    return routes;
}

bool gateway_route_allowed(const std::string &method, const std::string &path) {
    for (auto &[m, pattern] : gateway_route_whitelist()) {
        if (m == method && std::regex_match(path, pattern)) return true;
    }
    return false;
}

// Phase 8: scope required per route, matching exactly the three example
// scopes docs/DESIGN.md's client config schema already sketched in Phase 1
// (read:faults, read:data, execute:routines) rather than inventing finer-
// grained ones. A null scope means "any authenticated caller" -- topology
// listing and /docs are self-description, not diagnostic data (the same
// reasoning /docs already uses to return 200 with no backend rather than
// 501), so they're gated on having *a* valid token but no specific scope.
// PUT data, POST modes/operations, and every lock verb are all mutating/
// privileged actions and share execute:routines rather than each getting
// its own scope -- docs/DESIGN.md's sketch never listed more than these three.
struct ScopedRoute {
    const char *method;
    std::regex pattern;
    const char *required_scope; // nullptr = authenticated, no specific scope
};

const std::vector<ScopedRoute> &oauth2_scope_table() {
    static const std::vector<ScopedRoute> table = {
        {"GET", std::regex(R"(^/v1/entities$)"), nullptr},
        {"GET", std::regex(R"(^/v1/entities/.+/docs$)"), nullptr},
        {"GET", std::regex(R"(^/v1/entities/.+/faults$)"), "read:faults"},
        {"DELETE", std::regex(R"(^/v1/entities/.+/faults$)"), "execute:routines"},
        {"GET", std::regex(R"(^/v1/entities/.+/data$)"), "read:data"},
        // The SSE stream route is deliberately absent here -- EventSource
        // can't set an Authorization header, so it can't be gated by this
        // table at all. check_oauth2() bypasses it explicitly; it does its
        // own bearer-or-ticket check inside handle_stream_data instead. The
        // ticket-minting POST *is* a normal bearer route, scoped the same
        // as everything else that reads data.
        {"POST", std::regex(R"(^/v1/entities/.+/data/.+/stream-ticket$)"), "read:data"},
        {"GET", std::regex(R"(^/v1/entities/.+/data/.+$)"), "read:data"},
        {"PUT", std::regex(R"(^/v1/entities/.+/data/.+$)"), "execute:routines"},
        {"POST", std::regex(R"(^/v1/entities/.+/modes$)"), "execute:routines"},
        {"POST", std::regex(R"(^/v1/entities/.+/operations/.+$)"), "execute:routines"},
        {"POST", std::regex(R"(^/v1/entities/.+/locks$)"), "execute:routines"},
        {"PUT", std::regex(R"(^/v1/entities/.+/locks/[^/]+$)"), "execute:routines"},
        {"DELETE", std::regex(R"(^/v1/entities/.+/locks/[^/]+$)"), "execute:routines"},
    };
    return table;
}

enum class OAuth2Result { Ok, Unauthorized, Forbidden };

// "/" (version discovery) is the one route deliberately outside this table
// entirely -- a client must be able to learn api_versions before it has a
// token to use. The SSE stream GET is also bypassed here (see the comment
// on its stream-ticket table entry above): it does its own bearer-or-ticket
// check inside handle_stream_data, where the entity path/id are already
// parsed out of the URL.
//
// docs/reviews/round-1.md Task 2: everything else is default-DENY, not
// default-allow. Originally an unmatched (method, path) returned Ok --
// meaning a route added to register_routes() later without a matching
// entry here was silently reachable with no token at all, the opposite
// posture from the gateway whitelist's fail-closed default a few lines up.
// Now an unmatched route just falls through to "must present *a* valid
// token, no specific scope" (required_scope stays nullptr) instead of
// short-circuiting -- same treatment `/v1/entities` and `.../docs` already
// get deliberately. This can only make more routes require a token than
// before, never fewer, so it's safe against every route this project has
// today; test_http_oauth2_every_real_route_requires_auth_when_enabled
// (test_core.cpp) is the drift guard -- it walks every route
// register_routes() actually registers and asserts none of them are
// reachable with zero credentials while OAuth2 is on.
OAuth2Result check_oauth2(const std::string &secret, const std::string &method, const std::string &path,
                           const std::string &auth_header) {
    if (path == "/") return OAuth2Result::Ok;
    static const std::regex stream_pattern(R"(^/v1/entities/.+/data/.+/stream$)");
    if (method == "GET" && std::regex_match(path, stream_pattern)) return OAuth2Result::Ok;

    const char *required_scope = nullptr;
    for (auto &route : oauth2_scope_table()) {
        if (route.method == method && std::regex_match(path, route.pattern)) {
            required_scope = route.required_scope;
            break;
        }
    }

    static const std::string prefix = "Bearer ";
    if (auth_header.size() <= prefix.size() || auth_header.compare(0, prefix.size(), prefix) != 0) {
        return OAuth2Result::Unauthorized;
    }
    sovd::server::oauth2::TokenClaims claims;
    if (!sovd::server::oauth2::verify_token(auth_header.substr(prefix.size()), secret, claims)) {
        return OAuth2Result::Unauthorized;
    }
    if (required_scope && !sovd::server::oauth2::has_scope(claims, required_scope)) {
        return OAuth2Result::Forbidden;
    }
    return OAuth2Result::Ok;
}




// httplib runs pre_routing_handler and set_logger sequentially on the same
// worker thread for a given request (routing() -> process_request() is
// synchronous per-connection), so a thread_local timestamp is enough to
// carry request start time through to the logger without threading it
// through every handler or touching response headers.
std::chrono::steady_clock::time_point &request_start() {
    thread_local std::chrono::steady_clock::time_point t;
    return t;
}


} // namespace

// Phase 8: see routes.hpp's ProxyConnection forward-declaration comment.
// try_lock, not lock, in try_forward(): a second concurrent request against
// the *same* proxied entity while one is already in flight gets a clean 503
// instead of queueing behind a mutex -- "bounded queue -> 503" at depth 1,
// matching that a single entity is already effectively serialized by
// LockManager for every lock-gated operation anyway.
class ProxyConnection {
public:
    // Phase 8 (mTLS): httplib::Client's (url, client_cert_path,
    // client_key_path) constructor already builds an SSLClient internally
    // when base_url is https:// -- empty cert/key strings are a no-op for
    // a plain http:// target, so this one constructor covers both cases
    // without a separate SSLClient type in this file.
    explicit ProxyConnection(const ProxyTarget &target)
        : client(target.base_url, target.tls_client_cert, target.tls_client_key) {
        client.set_connection_timeout(2, 0);
        client.set_read_timeout(5, 0);
        client.set_keep_alive(true);
        if (!target.tls_ca_cert.empty()) {
            // Verify the domain server's certificate against this
            // project's own demo CA, not the system trust store -- these
            // are self-signed certs, and enable_server_certificate_
            // verification would otherwise reject them outright.
            client.set_ca_cert_path(target.tls_ca_cert);
            client.enable_server_certificate_verification(true);
        }
    }

    httplib::Client client;
    std::mutex mtx;
};

Router::Router(EntityRegistry &registry, LockManager &locks, std::string server_id, std::string role)
    : registry_(registry), locks_(locks), server_id_(std::move(server_id)), role_(std::move(role)),
      event_sink_([](const std::string &line) { std::cout << line << std::endl; }),
      telemetry_sink_([](const std::string &line) { std::cout << line << std::endl; }) {}

Router::~Router() = default;

void Router::set_event_sink(EventSink sink) { event_sink_ = std::move(sink); }
void Router::set_telemetry_sink(EventSink sink) { telemetry_sink_ = std::move(sink); }
void Router::set_cors_allowed_origins(std::vector<std::string> origins) { cors_allowed_origins_ = std::move(origins); }
void Router::set_oauth2_secret(std::string secret) { oauth2_secret_ = std::move(secret); }
void Router::set_web_root(std::string dir) { web_root_ = std::move(dir); }

bool Router::has_oauth2_scope(const httplib::Request &req, const std::string &scope) const {
    if (oauth2_secret_.empty()) return true;
    static const std::string prefix = "Bearer ";
    std::string auth_header = req.get_header_value("Authorization");
    if (auth_header.size() <= prefix.size() || auth_header.compare(0, prefix.size(), prefix) != 0) return false;
    sovd::server::oauth2::TokenClaims claims;
    if (!sovd::server::oauth2::verify_token(auth_header.substr(prefix.size()), oauth2_secret_, claims)) return false;
    return sovd::server::oauth2::has_scope(claims, scope);
}

void Router::apply_cors_headers(const httplib::Request &req, httplib::Response &res) const {
    std::string origin = req.get_header_value("Origin");
    if (origin.empty()) return;
    for (auto &allowed : cors_allowed_origins_) {
        if (allowed == origin) {
            res.set_header("Access-Control-Allow-Origin", origin);
            res.set_header("Access-Control-Expose-Headers", "X-SOVD-Correlation-Id");
            return;
        }
    }
    // Not on the allow-list: no header at all, never "*" -- the browser
    // enforces the actual block, this just declines to open the door.
}
void Router::set_server_id(std::string id) { server_id_ = std::move(id); }
void Router::set_role(std::string role) { role_ = std::move(role); }

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
    // path prefix over an Accept header, settled in docs/DESIGN.md: uglier, but
    // unambiguous, and safety-adjacent APIs shouldn't leave version
    // negotiation implicit.
    // Phase 3: per-request latency on a separate telemetry sink, not mixed
    // into the security event stream (docs/DESIGN.md: "an IDS should not be your
    // APM"). One hook pair here covers every route without instrumenting
    // each handler individually.
    //
    // B2 (Phase 7 blocker): CORS lives in the same pre-routing hook for the
    // same reason -- one place that runs before every route (including
    // handle_stream_data's chunked/SSE response, which never touches CORS
    // headers itself) beats threading Access-Control-* into every handler.
    // OPTIONS preflight requests aren't registered on any route, so they're
    // answered directly here and marked Handled rather than falling through
    // to a 404.
    svr.set_pre_routing_handler([this](const httplib::Request &req, httplib::Response &res) {
        request_start() = std::chrono::steady_clock::now();

        if (req.method == "OPTIONS") {
            apply_cors_headers(req, res);
            if (res.has_header("Access-Control-Allow-Origin")) {
                res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
                // X-SOVD-Lock-Id/X-SOVD-Correlation-Id explicitly listed: a
                // preflight that only allows Content-Type silently breaks
                // every lock-gated write and every correlated request from
                // the browser, since those headers wouldn't pass preflight.
                // Authorization added by docs/reviews/round-1.md Task 1a --
                // this list predates Phase 8's OAuth2 (B2 was Phase 7, mid-
                // 2026-08-20; OAuth2 landed the same day, after). Without it
                // a preflight carrying `Authorization: Bearer ...` never
                // lists the header back, so the browser blocks the real
                // request before it's even sent -- the whole web UI 401s
                // the instant SOVD_OAUTH2_SECRET is set.
                res.set_header("Access-Control-Allow-Headers",
                                "Content-Type, Authorization, X-SOVD-Lock-Id, X-SOVD-Correlation-Id");
                res.set_header("Access-Control-Max-Age", "600");
            }
            res.status = 204;
            return httplib::Server::HandlerResponse::Handled;
        }

        if (!web_root_.empty() && serve_static_file(web_root_, req, res)) return httplib::Server::HandlerResponse::Handled;

        apply_cors_headers(req, res);

        // Phase 8: default-deny at the gateway tier. Checked here, before
        // any route handler runs, so a request that doesn't match the
        // whitelist never reaches entity lookup / adapter dispatch at all --
        // "capability reduction by linkage > runtime checks" (docs/DESIGN.md)
        // applied at the HTTP layer, since role can't be enforced by
        // linkage alone when gateway and domain share one binary.
        if (role_ == "gateway" && !gateway_route_allowed(req.method, req.path)) {
            std::string corr = correlation_id_for(req, res);
            emit_event(event_sink_, "gateway_route_denied",
                       {{"method", req.method}, {"path", req.path}, {"correlation_id", corr}});
            write_error(res, 403, "FORBIDDEN", "path/method not permitted on a gateway-tier server");
            return httplib::Server::HandlerResponse::Handled;
        }

        // Phase 8: bearer-token + scope validation at the external
        // boundary. Opt-in (oauth2_secret_ empty means disabled, checked
        // every existing test's default TestServer never wires this) --
        // when set, every scoped route (see oauth2_scope_table()) needs a
        // valid, unexpired token, and mutating/privileged routes need the
        // matching scope on top of that.
        if (!oauth2_secret_.empty()) {
            OAuth2Result result =
                check_oauth2(oauth2_secret_, req.method, req.path, req.get_header_value("Authorization"));
            if (result != OAuth2Result::Ok) {
                std::string corr = correlation_id_for(req, res);
                int status = (result == OAuth2Result::Unauthorized) ? 401 : 403;
                emit_event(event_sink_, "oauth2_denied",
                           {{"method", req.method}, {"path", req.path}, {"status", status}, {"correlation_id", corr}});
                if (status == 401) {
                    res.set_header("WWW-Authenticate", "Bearer");
                    write_error(res, 401, "UNAUTHORIZED", "missing or invalid bearer token");
                } else {
                    write_error(res, 403, "FORBIDDEN", "token lacks the required scope");
                }
                return httplib::Server::HandlerResponse::Handled;
            }
        }

        return httplib::Server::HandlerResponse::Unhandled;
    });
    svr.set_logger([this](const httplib::Request &req, const httplib::Response &res) {
        double duration_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - request_start()).count();
        emit_event(telemetry_sink_, "http_request",
                   {{"method", req.method},
                    {"path", req.path},
                    {"status", res.status},
                    {"duration_ms", duration_ms},
                    {"correlation_id", res.get_header_value("X-SOVD-Correlation-Id")}});
    });

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
    // Phase 6: registered *before* the plain .../data/(.+) pattern below --
    // otherwise that route's greedy (.+) id-capture would swallow the
    // trailing "/stream" as part of the id instead of this one matching.
    svr.Get(R"(/v1/entities/(.+)/data/(.+)/stream)", [this](const httplib::Request &req, httplib::Response &res) {
        handle_stream_data(req, res, req.matches[1], req.matches[2]);
    });
    // Task 1c: mints the ticket a browser EventSource passes to the GET
    // .../stream route above in lieu of an Authorization header. A distinct
    // POST method, so no ambiguity with the GET .../stream pattern despite
    // the shared prefix.
    svr.Post(R"(/v1/entities/(.+)/data/(.+)/stream-ticket)",
             [this](const httplib::Request &req, httplib::Response &res) {
                 handle_post_stream_ticket(req, res, req.matches[1], req.matches[2]);
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
    // Phase 5: extends an already-held lock's TTL -- what the client SDK's
    // RAII lock heartbeat calls at ttl/2, so a crashed tester's lock lapses
    // on the original short TTL instead of needing one requested upfront.
    svr.Put(R"(/v1/entities/(.+)/locks/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handle_put_lock(req, res, req.matches[1], req.matches[2]);
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

void Router::attach_proxy(const std::string &entity_path, ProxyTarget target) {
    // Created here, before svr.listen() starts (config loading is
    // single-threaded), not lazily in try_forward() -- lets try_forward()
    // do a plain, concurrency-safe map lookup with no insertion-time race.
    proxy_connections_[entity_path] = std::make_unique<ProxyConnection>(target);
    proxies_.insert_or_assign(entity_path, std::move(target));
}

const ProxyTarget *Router::find_proxy(const std::string &entity_path) const {
    auto it = proxies_.find(entity_path);
    if (it == proxies_.end()) return nullptr;
    return &it->second;
}

bool Router::try_forward(const httplib::Request &req, httplib::Response &res, const std::string &entity_path) {
    const ProxyTarget *proxy = find_proxy(entity_path);
    if (!proxy) return false;

    // attach_proxy() always creates the connection alongside the
    // ProxyTarget, both before svr.listen() starts -- a proxy with no entry
    // here would be an attach_proxy() bug, not a runtime condition.
    ProxyConnection &conn = *proxy_connections_.at(entity_path);
    std::unique_lock<std::mutex> lk(conn.mtx, std::try_to_lock);
    if (!lk.owns_lock()) {
        write_error(res, 503, "BUSY", "another request to this proxied entity is already in flight");
        return true;
    }

    std::string prefix = "/v1/entities/" + entity_path;
    std::string suffix = req.path.size() > prefix.size() ? req.path.substr(prefix.size()) : "";
    std::string remote_url_path = "/v1/entities/" + proxy->remote_path + suffix;

    httplib::Headers headers;
    std::string lock_id = req.get_header_value("X-SOVD-Lock-Id");
    if (!lock_id.empty()) headers.emplace("X-SOVD-Lock-Id", lock_id);
    std::string corr = res.get_header_value("X-SOVD-Correlation-Id"); // stamped by correlation_id_for() already
    if (!corr.empty()) headers.emplace("X-SOVD-Correlation-Id", corr);

    httplib::Client &cli = conn.client;

    httplib::Result remote;
    if (req.method == "GET") {
        remote = cli.Get(remote_url_path, req.params, headers);
    } else if (req.method == "PUT") {
        remote = cli.Put(remote_url_path, headers, req.body, "application/json");
    } else if (req.method == "POST") {
        remote = cli.Post(remote_url_path, headers, req.body, "application/json");
    } else if (req.method == "DELETE") {
        remote = cli.Delete(remote_url_path, headers);
    } else {
        write_error(res, 501, "UNSUPPORTED", "method not proxied: " + req.method);
        return true;
    }

    if (!remote) {
        // Unreachable/timed-out remote -> 502, the same meaning TRANSPORT
        // already carries for a dead UDS backend, just over HTTP instead of
        // DoIP. This is also this phase's graceful-degradation guarantee at
        // the request level: a saturated/down domain server fails only
        // requests aimed at its own entities, nothing else on this gateway.
        write_error(res, 502, "TRANSPORT", "upstream SOVD server unreachable: " + proxy->base_url);
        return true;
    }

    res.status = remote->status;
    res.set_content(remote->body, remote->get_header_value("Content-Type", "application/json"));
    std::string remote_corr = remote->get_header_value("X-SOVD-Correlation-Id");
    if (!remote_corr.empty()) res.set_header("X-SOVD-Correlation-Id", remote_corr);
    return true;
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
    // Phase 8 follow-up (docs/reviews/round-1.md Task 8, settled as D4 in
    // docs/DESIGN.md): a POST/PUT .../locks response already echoes the actual
    // *granted* TTL when a request exceeds kMaxLockTtlSeconds, but a caller
    // that never reads the response body has no way to know 3600 was ever a
    // ceiling rather than "whatever I asked for was honored". Advertising it
    // here -- self-description, discovered once, same spot api_versions
    // already lives -- lets a well-behaved client clamp its own request
    // instead of finding out after the fact. Chosen over rejecting an
    // over-ceiling request with 400: clamping-and-telling-the-truth was
    // already the settled design (docs/DESIGN.md's original resource-limits
    // writeup), this only fixes the "telling" part for a client that isn't
    // reading response bodies.
    json body = {
        {"server_id", server_id_},
        {"role", role_},
        {"sovd_version", "phase0-demo"},
        {"api_versions", json::array({"v1"})},
        {"limits", {{"lock_ttl_ceiling_seconds", kMaxLockTtlSeconds}}},
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
            {"has_backend", e->has_backend() || find_proxy(e->path) != nullptr},
        });
    }
    res.set_content(json{{"items", items}}.dump(), "application/json");
}

void Router::handle_get_docs(const httplib::Request &req, httplib::Response &res, const std::string &path) {
    correlation_id_for(req, res);
    const Entity *e = require_entity(res, path);
    if (!e) return;
    if (try_forward(req, res, path)) return;

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
