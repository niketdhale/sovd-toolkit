// Router — translates HTTP <-> core calls. Nothing else lives here: no
// diagnostic logic, no adapter-specific behavior.
#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include "sovd/catalog/did_catalog.hpp"
#include "sovd/entity_registry.hpp"
#include "sovd/lock_manager.hpp"

namespace httplib {
class Server;
class Request;
class Response;
} // namespace httplib

namespace sovd::server {

class Router {
public:
    // Receives one already-serialized JSON event line. Default sink writes
    // it to stdout (matching Phase 0/3's existing behavior); tests swap it
    // to capture events without redirecting the real stdout, and Phase 3's
    // MQTT transport is a drop-in replacement for the same seam.
    using EventSink = std::function<void(const std::string &)>;

    Router(EntityRegistry &registry, LockManager &locks, std::string server_id, std::string role);

    void register_routes(httplib::Server &svr);
    void set_event_sink(EventSink sink);

    // Associates a DID catalog with an entity path, enabling named data
    // paths (/data/battery_voltage) and typed /docs output for it. Catalogs
    // are per-ECU-software-version, kept separate from topology (see
    // CLAUDE.md) — an entity with no attached catalog still works, just
    // falls back to raw hex DIDs and an empty /docs data/operations list.
    void attach_catalog(const std::string &entity_path, catalog::Catalog cat);

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
    void handle_delete_lock(const httplib::Request &req, httplib::Response &res, const std::string &path,
                             const std::string &lock_id);
    void handle_get_docs(const httplib::Request &req, httplib::Response &res, const std::string &path);

    const Entity *require_entity(httplib::Response &res, const std::string &path);
    bool check_lock_header(const httplib::Request &req, httplib::Response &res, const std::string &path);
    const catalog::Catalog *find_catalog(const std::string &entity_path) const;

    // Echoes X-SOVD-Correlation-Id if the client supplied one, generates one
    // otherwise; always stamps it on the response header (success or error)
    // and returns it so the handler can attach it to any emitted event.
    // Clocks may not be synced across a gateway/domain-HPC hop — this is
    // what lets an event get correlated to the request that caused it
    // instead, which timestamps alone can't guarantee (CLAUDE.md).
    std::string correlation_id_for(const httplib::Request &req, httplib::Response &res) const;

    EntityRegistry &registry_;
    LockManager &locks_;
    std::string server_id_;
    std::string role_;
    std::unordered_map<std::string, catalog::Catalog> catalogs_;
    EventSink event_sink_;
};

} // namespace sovd::server
