// Phase 4: YAML topology -> EntityRegistry + Router, replacing main.cpp's
// hardcoded build_topology(). Lives alongside main.cpp (compiled into the
// sovd_server executable target, not sovd_server_lib): it directly
// instantiates concrete adapters (mock, uds_doip), which is exactly the
// "backend complexity" CLAUDE.md's layering rule keeps out of server/'s own
// library — main.cpp already did this for the hardcoded demo, this is the
// same responsibility, just data-driven.
#pragma once

#include <stdexcept>
#include <string>

#include "sovd/entity_registry.hpp"
#include "sovd/server/routes.hpp"

namespace sovd::server {

class ConfigError : public std::runtime_error {
public:
    explicit ConfigError(const std::string &msg) : std::runtime_error(msg) {}
};

struct ServerConfig {
    std::string id;
    int port = 20002;
    std::string role = "domain";

    // Phase 8 (mTLS): tls_cert/tls_key present => main.cpp binds an
    // httplib::SSLServer instead of a plain Server. tls_client_ca present
    // on top of that => the server requires and verifies a client
    // certificate on every connection (mutual TLS) -- the gateway<->domain
    // hop CLAUDE.md's mTLS item is about, not the external tester-facing
    // boundary (that's OAuth2's job). All empty (the default) means plain
    // HTTP, unchanged from every config that predates this.
    std::string tls_cert;
    std::string tls_key;
    std::string tls_client_ca;
};

// Populates registry (entities + adapters) and router (catalogs + proxy
// targets) from topology YAML; returns the `server:` block. Throws
// ConfigError on malformed YAML structure (missing/invalid required
// fields). A single entity's adapter failing to construct (e.g. a uds_doip
// block with bad config) is not structural malformity -- that entity is
// registered as a plain grouping node (a warning goes to stderr) instead of
// throwing, so one misconfigured ECU doesn't take the whole topology down.
ServerConfig load_topology_from_string(const std::string &yaml_text, EntityRegistry &registry, Router &router);
ServerConfig load_topology_from_file(const std::string &path, EntityRegistry &registry, Router &router);

} // namespace sovd::server
