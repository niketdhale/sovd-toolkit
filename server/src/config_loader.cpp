#include "sovd/server/config_loader.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

#include "json.hpp"
#include "sovd/catalog/did_catalog.hpp"
#include "yaml-cpp/yaml.h"

#ifdef SOVD_HAVE_UDS_DOIP
#include "sovd/uds_doip/uds_doip_adapter.h"
#endif

#ifdef SOVD_HAVE_MOCK
#include "mock_adapter.h"
#endif

namespace sovd::server {

namespace {

using json = nlohmann::json;

EntityType parse_entity_type(const std::string &s) {
    if (s == "vehicle") return EntityType::Vehicle;
    if (s == "area") return EntityType::Area;
    if (s == "component") return EntityType::Component;
    throw ConfigError("unknown entity type: " + s);
}

// add_entity() returning false means a structural topology error (duplicate
// path, or parent not registered yet -- entities must be listed
// parent-before-child), distinct from an adapter failing to construct.
// That's a broken config, not a degraded one: throw rather than silently
// dropping the entity.
void add_entity_or_throw(EntityRegistry &registry, const std::string &path, EntityType type,
                          const sovd_vtable_t *vtable = nullptr, sovd_adapter_ctx *ctx = nullptr) {
    if (!registry.add_entity(path, type, vtable, ctx)) {
        throw ConfigError("failed to register entity '" + path +
                           "' -- duplicate path, or its parent isn't registered yet "
                           "(entities must be listed parent-before-child)");
    }
}

#ifdef SOVD_HAVE_UDS_DOIP
// Builds the uds_doip adapter's expected config_json (see
// uds_doip_adapter.cpp's create() doc comment) straight from the YAML
// adapter block's known fields -- a small explicit allowlist, not a
// generic YAML->JSON converter (nothing else here needs one).
std::string uds_doip_config_json(const YAML::Node &adapter) {
    json j;
    if (adapter["logical_address"]) j["logical_address"] = adapter["logical_address"].as<std::string>();
    if (adapter["gateway_ip"]) j["gateway_ip"] = adapter["gateway_ip"].as<std::string>();
    if (adapter["port"]) j["port"] = adapter["port"].as<int>();
    if (adapter["tester_logical_address"])
        j["tester_logical_address"] = adapter["tester_logical_address"].as<std::string>();
    if (adapter["did_catalog"]) j["did_catalog"] = adapter["did_catalog"].as<std::string>();
    if (adapter["connect_timeout_ms"]) j["connect_timeout_ms"] = adapter["connect_timeout_ms"].as<int>();
    if (adapter["read_timeout_ms"]) j["read_timeout_ms"] = adapter["read_timeout_ms"].as<int>();
    if (adapter["heartbeat_interval_ms"]) j["heartbeat_interval_ms"] = adapter["heartbeat_interval_ms"].as<int>();
    if (adapter["session_idle_timeout_ms"])
        j["session_idle_timeout_ms"] = adapter["session_idle_timeout_ms"].as<int>();
    return j.dump();
}
#endif

// Router's own catalog attachment (for /docs + named data paths) is
// independent of whatever an adapter does with did_catalog internally (e.g.
// uds_doip's session-escalation/io_control decisions) -- CLAUDE.md: "not a
// shared runtime instance". Any adapter kind that owns real diagnostic data
// can have one; not just uds_doip.
void attach_router_catalog_if_present(const std::string &path, const YAML::Node &adapter, Router &router) {
    if (!adapter["did_catalog"]) return;
    try {
        router.attach_catalog(path, catalog::Catalog::load_from_file(adapter["did_catalog"].as<std::string>()));
    } catch (const catalog::CatalogError &ex) {
        std::cerr << "warning: failed to load catalog for " << path << ": " << ex.what() << std::endl;
    }
}

// Attaches whatever adapter kind the YAML names, or registers a plain
// grouping node (no vtable) if the kind is unknown, not compiled into this
// binary, or its create() fails on the given config -- one misconfigured
// entity degrades to a 501-on-diagnostics grouping node, not a crash or an
// aborted load of the whole topology (uds_doip_adapter.cpp's create() doc
// comment flags exactly this hazard: never pair a non-NULL vtable with a
// NULL ctx).
void attach_entity(const std::string &path, EntityType type, const YAML::Node &entity_node, EntityRegistry &registry,
                    Router &router) {
    if (!entity_node["adapter"]) {
        add_entity_or_throw(registry, path, type);
        return;
    }
    YAML::Node adapter = entity_node["adapter"];
    std::string kind = adapter["kind"] ? adapter["kind"].as<std::string>() : "";

    if (kind == "mock") {
#ifdef SOVD_HAVE_MOCK
        const sovd_vtable_t *vt = sovd_mock_adapter_vtable();
        add_entity_or_throw(registry, path, type, vt, vt->create(nullptr));
        attach_router_catalog_if_present(path, adapter, router);
#else
        std::cerr << "warning: entity " << path << " requests adapter kind 'mock', but this binary was built "
                  << "without SOVD_ADAPTER_MOCK; registering as a grouping node" << std::endl;
        add_entity_or_throw(registry, path, type);
#endif
        return;
    }

    if (kind == "uds_doip") {
#ifdef SOVD_HAVE_UDS_DOIP
        const sovd_vtable_t *vt = sovd_uds_doip_adapter_vtable();
        std::string config_json = uds_doip_config_json(adapter);
        sovd_adapter_ctx *ctx = vt->create(config_json.c_str());
        if (!ctx) {
            std::cerr << "warning: uds_doip adapter for " << path << " failed to initialize (bad config?); "
                      << "registering as a grouping node" << std::endl;
            add_entity_or_throw(registry, path, type);
            return;
        }
        add_entity_or_throw(registry, path, type, vt, ctx);
        attach_router_catalog_if_present(path, adapter, router);
#else
        std::cerr << "warning: entity " << path << " requests adapter kind 'uds_doip', but this binary was built "
                  << "without SOVD_ADAPTER_UDS_DOIP; registering as a grouping node" << std::endl;
        add_entity_or_throw(registry, path, type);
#endif
        return;
    }

    if (kind == "sovd_proxy") {
        add_entity_or_throw(registry, path, type); // no vtable: Router forwards this path over HTTP instead (see ProxyTarget)
        ProxyTarget target;
        target.base_url = adapter["base_url"] ? adapter["base_url"].as<std::string>() : "";
        target.remote_path = adapter["remote_path"] ? adapter["remote_path"].as<std::string>() : path;
        if (target.base_url.empty()) {
            throw ConfigError("sovd_proxy adapter for " + path + " is missing required field: base_url");
        }
        // forward_locks is accepted (so the YAML matches CLAUDE.md's
        // documented schema) but not branched on: locks are always
        // forwarded for a proxied entity, never cached locally, per
        // CLAUDE.md's "NEVER cache lock state locally" -- it's not a toggle.
        router.attach_proxy(path, std::move(target));
        return;
    }

    std::cerr << "warning: entity " << path << " requests unknown adapter kind '" << kind << "'; registering as a "
              << "grouping node" << std::endl;
    add_entity_or_throw(registry, path, type);
}

} // namespace

ServerConfig load_topology_from_string(const std::string &yaml_text, EntityRegistry &registry, Router &router) {
    YAML::Node root;
    try {
        root = YAML::Load(yaml_text);
    } catch (const YAML::Exception &ex) {
        throw ConfigError(std::string("YAML parse error: ") + ex.what());
    }

    if (!root["server"]) throw ConfigError("missing required top-level key: server");
    YAML::Node server_node = root["server"];
    if (!server_node["id"]) throw ConfigError("server.id is required");
    if (!server_node["port"]) throw ConfigError("server.port is required");
    if (!server_node["role"]) throw ConfigError("server.role is required");

    ServerConfig cfg;
    cfg.id = server_node["id"].as<std::string>();
    cfg.port = server_node["port"].as<int>();
    cfg.role = server_node["role"].as<std::string>();

    // B2 (Phase 7 blocker): optional, and deliberately an explicit
    // allow-list rather than a bare "cors: true" -- absent means CORS stays
    // off (every browser origin denied by the browser's own policy), never
    // a wildcard default on a safety-adjacent diagnostic interface.
    if (server_node["cors_allowed_origins"] && server_node["cors_allowed_origins"].IsSequence()) {
        std::vector<std::string> origins;
        for (const auto &o : server_node["cors_allowed_origins"]) origins.push_back(o.as<std::string>());
        router.set_cors_allowed_origins(std::move(origins));
    }

    if (!root["entities"] || !root["entities"].IsSequence()) {
        throw ConfigError("missing required top-level key: entities (must be a sequence)");
    }
    for (const auto &entity_node : root["entities"]) {
        if (!entity_node["path"]) throw ConfigError("an entity is missing required field: path");
        if (!entity_node["type"]) throw ConfigError("an entity is missing required field: type");
        std::string path = entity_node["path"].as<std::string>();
        EntityType type = parse_entity_type(entity_node["type"].as<std::string>());
        attach_entity(path, type, entity_node, registry, router);
    }

    return cfg;
}

ServerConfig load_topology_from_file(const std::string &path, EntityRegistry &registry, Router &router) {
    std::ifstream f(path);
    if (!f) throw ConfigError("cannot open config file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return load_topology_from_string(ss.str(), registry, router);
}

} // namespace sovd::server
