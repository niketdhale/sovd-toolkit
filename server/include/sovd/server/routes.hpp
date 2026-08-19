// Router — translates HTTP <-> core calls. Nothing else lives here: no
// diagnostic logic, no adapter-specific behavior.
#pragma once

#include <string>

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
    Router(EntityRegistry &registry, LockManager &locks, std::string server_id, std::string role);

    void register_routes(httplib::Server &svr);

private:
    void handle_root(httplib::Response &res);
    void handle_list_entities(httplib::Response &res);
    void handle_get_faults(const httplib::Request &req, httplib::Response &res, const std::string &path);
    void handle_clear_faults(const httplib::Request &req, httplib::Response &res, const std::string &path);
    void handle_get_data(const httplib::Request &req, httplib::Response &res, const std::string &path,
                          const std::string &did);
    void handle_put_data(const httplib::Request &req, httplib::Response &res, const std::string &path,
                          const std::string &did);
    void handle_post_mode(const httplib::Request &req, httplib::Response &res, const std::string &path);
    void handle_post_operation(const httplib::Request &req, httplib::Response &res, const std::string &path,
                                const std::string &op);
    void handle_post_lock(const httplib::Request &req, httplib::Response &res, const std::string &path);
    void handle_delete_lock(const httplib::Request &req, httplib::Response &res, const std::string &path,
                             const std::string &lock_id);

    const Entity *require_entity(httplib::Response &res, const std::string &path);
    bool check_lock_header(const httplib::Request &req, httplib::Response &res, const std::string &path);

    EntityRegistry &registry_;
    LockManager &locks_;
    std::string server_id_;
    std::string role_;
};

} // namespace sovd::server
