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
#include "sovd/server/oauth2.hpp"

namespace sovd::server {

using json = nlohmann::json;

namespace {

// Phase 8: see the comment at its one call site (handle_post_lock) for why
// this is also the effective cap on concurrently escalated UDS sessions.
// Generous for any real vehicle's entity count, still a real ceiling.
constexpr size_t kMaxConcurrentLocks = 64;

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
        case SOVD_FORBIDDEN: return 403;
        case SOVD_CONFLICT: return 409;
    }
    return 500;
}

void write_error(httplib::Response &res, int status, const std::string &code, const std::string &message) {
    res.status = status;
    res.set_content(json{{"error", code}, {"message", message}}.dump(), "application/json");
}

// Phase 8: default-deny whitelist for gateway-role servers. Same patterns as
// register_routes() below -- kept as a second, explicit list rather than
// derived from httplib's internal handler table (it doesn't expose one to
// introspect) so this is an auditable security policy, not an accident of
// whatever got registered. A gateway forwards the same resource surface a
// domain server answers directly (CLAUDE.md's role model), so today this
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
// scopes CLAUDE.md's client config schema already sketched in Phase 1
// (read:faults, read:data, execute:routines) rather than inventing finer-
// grained ones. A null scope means "any authenticated caller" -- topology
// listing and /docs are self-description, not diagnostic data (the same
// reasoning /docs already uses to return 200 with no backend rather than
// 501), so they're gated on having *a* valid token but no specific scope.
// PUT data, POST modes/operations, and every lock verb are all mutating/
// privileged actions and share execute:routines rather than each getting
// its own scope -- CLAUDE.md's sketch never listed more than these three.
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
// SOVD_REVIEW_FEEDBACK.md Task 2: everything else is default-DENY, not
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

// httplib runs pre_routing_handler and set_logger sequentially on the same
// worker thread for a given request (routing() -> process_request() is
// synchronous per-connection), so a thread_local timestamp is enough to
// carry request start time through to the logger without threading it
// through every handler or touching response headers.
std::chrono::steady_clock::time_point &request_start() {
    thread_local std::chrono::steady_clock::time_point t;
    return t;
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
    // path prefix over an Accept header, settled in CLAUDE.md: uglier, but
    // unambiguous, and safety-adjacent APIs shouldn't leave version
    // negotiation implicit.
    // Phase 3: per-request latency on a separate telemetry sink, not mixed
    // into the security event stream (CLAUDE.md: "an IDS should not be your
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
                // Authorization added by SOVD_REVIEW_FEEDBACK.md Task 1a --
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

        apply_cors_headers(req, res);

        // Phase 8: default-deny at the gateway tier. Checked here, before
        // any route handler runs, so a request that doesn't match the
        // whitelist never reaches entity lookup / adapter dispatch at all --
        // "capability reduction by linkage > runtime checks" (CLAUDE.md)
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
    // Phase 8 follow-up (SOVD_REVIEW_FEEDBACK.md Task 8, settled as D4 in
    // CLAUDE.md): a POST/PUT .../locks response already echoes the actual
    // *granted* TTL when a request exceeds kMaxLockTtlSeconds, but a caller
    // that never reads the response body has no way to know 3600 was ever a
    // ceiling rather than "whatever I asked for was honored". Advertising it
    // here -- self-description, discovered once, same spot api_versions
    // already lives -- lets a well-behaved client clamp its own request
    // instead of finding out after the fact. Chosen over rejecting an
    // over-ceiling request with 400: clamping-and-telling-the-truth was
    // already the settled design (CLAUDE.md's original resource-limits
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
    // SOVD_REVIEW_ROUND2.md Task 10: capacity-checked the same shape as
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
    // (CLAUDE.md's settled session-manager-ownership decision) -- doubles
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
            // Session lifetime follows lock lifetime (CLAUDE.md's "Session
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
