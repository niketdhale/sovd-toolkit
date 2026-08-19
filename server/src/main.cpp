#include <cstdlib>
#include <iostream>

#include "httplib.h"
#include "mock_adapter.h"
#include "sovd/entity_registry.hpp"
#include "sovd/lock_manager.hpp"
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
    sovd::server::Router router(registry, locks, "sovd-demo", role);
    router.register_routes(svr);

    std::cout << "sovd_server listening on :" << port << " role=" << role << std::endl;
    if (!svr.listen("0.0.0.0", port)) {
        std::cerr << "failed to bind port " << port << std::endl;
        return 1;
    }
    return 0;
}
