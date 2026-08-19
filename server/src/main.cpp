#include <cstdlib>
#include <iostream>
#include <memory>

#include "httplib.h"
#include "mock_adapter.h"
#include "sovd/catalog/did_catalog.hpp"
#include "sovd/entity_registry.hpp"
#include "sovd/lock_manager.hpp"
#include "sovd/server/mqtt_publisher.hpp"
#include "sovd/server/routes.hpp"

using namespace sovd;

namespace {

// Hardcoded demo topology. Replaced by the YAML config loader in Phase 4.
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

} // namespace

int main(int argc, char **argv) {
    int port = 20002;
    std::string role = "domain";
    if (argc > 1) port = std::atoi(argv[1]);
    if (argc > 2) role = argv[2];

    EntityRegistry registry;
    build_topology(registry);
    LockManager locks;

    httplib::Server svr;
    std::string server_id = "sovd-demo";
    sovd::server::Router router(registry, locks, server_id, role);
    router.register_routes(svr);

    // Phase 3: MQTT is opt-in via env var, not config-loaded (Phase 4's
    // topology loader doesn't exist yet) — unset means stdout, same as
    // before, so `./sovd_server` still runs with no broker required.
    // Two publishers, two topics: security events stay separate from
    // per-request telemetry ("an IDS should not be your APM", CLAUDE.md).
    // server_id in the topic path means Phase 4's multi-server topology
    // needs no rework here — each server already publishes under its own
    // name.
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

    // Catalog is per-ECU-software-version data, loaded independently of
    // topology (see CLAUDE.md). Hardcoded to bcm for this demo topology;
    // Phase 4's config loader will read `did_catalog:` per entity instead.
    try {
        router.attach_catalog("vehicle/body/bcm", catalog::Catalog::load_from_file("catalogs/bcm.yaml"));
    } catch (const catalog::CatalogError &ex) {
        std::cerr << "warning: failed to load catalogs/bcm.yaml: " << ex.what() << std::endl;
    }

    std::cout << "sovd_server listening on :" << port << " role=" << role << std::endl;
    if (!svr.listen("0.0.0.0", port)) {
        std::cerr << "failed to bind port " << port << std::endl;
        return 1;
    }
    return 0;
}
