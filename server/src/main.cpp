#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

#include "httplib.h"
#include "sovd/catalog/did_catalog.hpp"
#include "sovd/entity_registry.hpp"
#include "sovd/lock_manager.hpp"
#include "sovd/server/config_loader.hpp"
#include "sovd/server/mqtt_publisher.hpp"
#include "sovd/server/routes.hpp"

#ifdef SOVD_HAVE_MDNS
#include "sovd/server/mdns_advertise.hpp"
#endif

#ifdef SOVD_HAVE_MOCK
#include "mock_adapter.h"
#endif

using namespace sovd;

namespace {

// Hardcoded zero-config demo topology, used when argv[1] isn't a config
// file (see main()). Phase 4 adds the YAML-driven path alongside this, not
// instead of it -- `./sovd_server` with no args still needs to just work.
//
// B3: only compiled against the mock adapter when it's actually linked in
// (SOVD_HAVE_MOCK) -- a restricted build with SOVD_ADAPTER_MOCK=OFF has no
// mock symbols to call. That build has no zero-config demo to offer either
// way: an operator building a mock-less binary is building it to run real
// hardware, which needs a topology YAML (config/, `kind: uds_doip`)
// regardless of whether main.cpp *could* fall back to something else.
#ifdef SOVD_HAVE_MOCK
void build_topology(EntityRegistry &registry) {
    const sovd_vtable_t *mock = sovd_mock_adapter_vtable();

    registry.add_entity("vehicle", EntityType::Vehicle);

    registry.add_entity("vehicle/adas", EntityType::Area);
    registry.add_entity("vehicle/adas/camera_ecu", EntityType::Component, mock, mock->create(nullptr));
    registry.add_entity("vehicle/adas/radar_ecu", EntityType::Component, mock, mock->create(nullptr));

    registry.add_entity("vehicle/body", EntityType::Area);
    registry.add_entity("vehicle/body/bcm", EntityType::Component, mock, mock->create(nullptr));
    registry.add_entity("vehicle/body/door_ctrl", EntityType::Component, mock, mock->create(nullptr));
}
#else
void build_topology(EntityRegistry &registry) {
    registry.add_entity("vehicle", EntityType::Vehicle);
    std::cerr << "warning: this binary was built without the mock adapter (SOVD_ADAPTER_MOCK=OFF); "
              << "the zero-config demo topology has nothing to attach. Pass a topology YAML instead "
              << "(see config/), e.g. `./sovd_server config/domain_body.yaml` with `kind: uds_doip`."
              << std::endl;
}
#endif

} // namespace

int main(int argc, char **argv) {
    // Phase 4: `./sovd_server <config.yaml>` loads a topology; anything
    // else (no args, or `./sovd_server <port> [role]`) keeps the original
    // hardcoded demo unchanged — an existing-regular-file check is enough
    // to tell them apart without an argv-parsing library for one flag's
    // worth of ambiguity.
    bool config_mode = false;
    if (argc > 1) {
        std::ifstream probe(argv[1]);
        config_mode = probe.good();
    }

    EntityRegistry registry;
    LockManager locks;
    httplib::Server svr;
    // Phase 8: max request body -- native httplib feature (rung 4: don't
    // hand-roll what the platform already does), not opt-in like MQTT/CORS
    // since this is a security property, not a demo convenience. Every
    // legitimate SOVD write body is a few bytes of JSON ({"value": ...});
    // 64KiB is generous headroom, not a real ceiling on anything valid.
    svr.set_payload_max_length(64 * 1024);

    // Router needs a server_id/role at construction, but in config mode
    // those only become known once load_topology_from_file() below has
    // parsed the `server:` block -- placeholders here, corrected via
    // set_server_id()/set_role() before svr.listen() starts accepting
    // requests (see routes.hpp's comment on those setters).
    std::string server_id = "sovd-demo";
    std::string role = "domain";
    int port = 20002;
    sovd::server::Router router(registry, locks, server_id, role);
    router.register_routes(svr);

    if (config_mode) {
        try {
            sovd::server::ServerConfig cfg = sovd::server::load_topology_from_file(argv[1], registry, router);
            server_id = cfg.id;
            role = cfg.role;
            port = cfg.port;
            router.set_server_id(server_id);
            router.set_role(role);
        } catch (const sovd::server::ConfigError &ex) {
            std::cerr << "config error: " << ex.what() << std::endl;
            return 1;
        }
    } else {
        build_topology(registry);
        if (argc > 1) port = std::atoi(argv[1]);
        if (argc > 2) role = argv[2];
        router.set_role(role);

        // Catalog is per-ECU-software-version data, loaded independently of
        // topology (see CLAUDE.md). Hardcoded to bcm for this demo
        // topology; config mode reads `did_catalog:` per entity instead.
        try {
            router.attach_catalog("vehicle/body/bcm", catalog::Catalog::load_from_file("catalogs/bcm.yaml"));
        } catch (const catalog::CatalogError &ex) {
            std::cerr << "warning: failed to load catalogs/bcm.yaml: " << ex.what() << std::endl;
        }
    }

    // MQTT is opt-in via env var regardless of mode — unset means stdout,
    // so `./sovd_server` still runs with no broker required. Two
    // publishers, two topics: security events stay separate from
    // per-request telemetry ("an IDS should not be your APM", CLAUDE.md).
    // server_id in the topic path means multi-server topology needs no
    // rework here — each server already publishes under its own name.
    if (const char *mqtt_host = std::getenv("SOVD_MQTT_HOST")) {
        int mqtt_port = 1883;
        if (const char *p = std::getenv("SOVD_MQTT_PORT")) mqtt_port = std::atoi(p);

        auto events_pub = std::make_shared<sovd::server::mqtt::MqttPublisher>(
            mqtt_host, static_cast<uint16_t>(mqtt_port), server_id + "-events", "sovd/" + server_id + "/events");
        auto telemetry_pub = std::make_shared<sovd::server::mqtt::MqttPublisher>(
            mqtt_host, static_cast<uint16_t>(mqtt_port), server_id + "-telemetry",
            "sovd/" + server_id + "/telemetry");

        router.set_event_sink([events_pub](const std::string &line) { events_pub->publish(line); });
        router.set_telemetry_sink([telemetry_pub](const std::string &line) { telemetry_pub->publish(line); });
        std::cout << "MQTT event/telemetry publishing to " << mqtt_host << ":" << mqtt_port << std::endl;
    }

    // B2: CORS in the hardcoded-demo path is the same opt-in-via-env-var
    // shape as MQTT/mDNS -- unset means no CORS headers at all (browser
    // origins denied by same-origin policy), so `./sovd_server` still runs
    // exactly as before with no config file. Comma-separated origins, e.g.
    // SOVD_CORS_ORIGINS="http://localhost:5173,https://tester.example".
    if (const char *cors_origins = std::getenv("SOVD_CORS_ORIGINS")) {
        std::vector<std::string> origins;
        std::istringstream iss(cors_origins);
        for (std::string origin; std::getline(iss, origin, ','); ) {
            if (!origin.empty()) origins.push_back(origin);
        }
        router.set_cors_allowed_origins(origins);
        std::cout << "CORS allowed for " << origins.size() << " origin(s)" << std::endl;
    }

    // mDNS advertising is opt-in the same way MQTT is -- unset means no
    // avahi traffic at all, so `./sovd_server` still runs on a box with no
    // avahi-daemon (or a build without SOVD_CLIENT_MDNS) unchanged.
#ifdef SOVD_HAVE_MDNS
    std::unique_ptr<sovd::server::MdnsAdvertiser> mdns_advertiser;
    if (std::getenv("SOVD_MDNS_ADVERTISE")) {
        mdns_advertiser = std::make_unique<sovd::server::MdnsAdvertiser>(server_id, static_cast<uint16_t>(port));
        std::cout << "advertising _sovd._tcp.local as " << server_id << std::endl;
    }
#endif

    std::cout << "sovd_server listening on :" << port << " role=" << role << " id=" << server_id << std::endl;
    if (!svr.listen("0.0.0.0", port)) {
        std::cerr << "failed to bind port " << port << std::endl;
        return 1;
    }
    return 0;
}
