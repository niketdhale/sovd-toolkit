// Phase 5: SovdClient/LockGuard tested against a real live sovd_server
// (same "test the real path, not a mock of the SDK's own dependency"
// standard as the rest of this repo) rather than mocking httplib::Client.
#include <chrono>
#include <thread>

#include "httplib.h"
#include "mock_adapter.h"
#include "sovd/catalog/did_catalog.hpp"
#include "sovd/client/lock_guard.hpp"
#include "sovd/client/sovd_client.hpp"
#include "sovd/entity_registry.hpp"
#include "sovd/lock_manager.hpp"
#include "sovd/server/routes.hpp"
#include "test_framework.hpp"

using namespace sovd;
using namespace sovd::client;

namespace {

const char *kCatalogYaml = R"(
data:
  - id: vin
    did: 0xF190
    type: string
    length: 17
    access: read
  - id: battery_voltage
    did: 0x010A
    type: float
    encoding: { bytes: 2, endian: big, scale: 0.001, unit: V }
    access: read
  - id: door_lock_state
    did: 0x0200
    type: enum
    values: { 0: unlocked, 1: locked, 2: deadlocked }
    access: read_write
    io_control: true
operations:
  - id: self_test
    routine_id: 0x0203
    requires_session: extended
    async: true
)";

struct LiveServer {
    EntityRegistry registry;
    LockManager locks;
    httplib::Server svr;
    sovd::server::Router router;
    std::thread thread;
    int port = 0;

    LiveServer() : router(registry, locks, "test-server", "domain") {
        const sovd_vtable_t *mock = sovd_mock_adapter_vtable();
        registry.add_entity("vehicle", EntityType::Vehicle);
        registry.add_entity("vehicle/body", EntityType::Area);
        registry.add_entity("vehicle/body/bcm", EntityType::Component, mock, mock->create(nullptr));

        router.register_routes(svr);
        router.attach_catalog("vehicle/body/bcm", catalog::Catalog::load_from_string(kCatalogYaml));

        port = svr.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { svr.listen_after_bind(); });
        svr.wait_until_ready();
    }

    ~LiveServer() {
        svr.stop();
        thread.join();
    }

    std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port); }
};

void test_client_list_entities_and_docs() {
    LiveServer server;
    SovdClient cli(server.base_url());

    auto entities = cli.list_entities();
    bool found_bcm = false;
    for (auto &e : entities) {
        if (e.path == "vehicle/body/bcm") {
            found_bcm = true;
            ASSERT_TRUE(e.has_backend);
        }
    }
    ASSERT_TRUE(found_bcm);

    DocsResult docs = cli.get_docs("vehicle/body/bcm");
    ASSERT_TRUE(docs.has_backend);
    ASSERT_TRUE(docs.capabilities.has_value());
    bool found_battery = false;
    for (auto &d : docs.data) {
        if (d.id == "battery_voltage") {
            found_battery = true;
            ASSERT_EQ(d.type, std::string("float"));
            ASSERT_TRUE(d.unit.has_value());
            ASSERT_EQ(*d.unit, std::string("V"));
        }
    }
    ASSERT_TRUE(found_battery);
    ASSERT_EQ(docs.operations.size(), static_cast<size_t>(1));
    ASSERT_EQ(docs.operations[0].id, std::string("self_test"));
}

void test_client_read_single_and_batch() {
    LiveServer server;
    SovdClient cli(server.base_url());

    DataValue v = cli.get_data("vehicle/body/bcm", "battery_voltage");
    ASSERT_FALSE(v.error.has_value());
    ASSERT_TRUE(std::abs(v.value.get<double>() - 13.0) < 1e-9);
    ASSERT_EQ(*v.unit, std::string("V"));

    auto batch = cli.get_data_batch("vehicle/body/bcm", {"vin", "battery_voltage", "not_a_real_id"});
    ASSERT_EQ(batch.size(), static_cast<size_t>(3));
    ASSERT_FALSE(batch[0].error.has_value());
    ASSERT_FALSE(batch[1].error.has_value());
    ASSERT_TRUE(batch[2].error.has_value()); // per-item failure, doesn't throw or drop the others
}

void test_client_faults_read_and_clear() {
    LiveServer server;
    SovdClient cli(server.base_url());

    auto faults = cli.get_faults("vehicle/body/bcm");
    ASSERT_FALSE(faults.empty());

    LockGuard lock(cli, "vehicle/body/bcm", 60, /*heartbeat=*/false);
    cli.clear_faults("vehicle/body/bcm", lock.lock_id());

    auto after = cli.get_faults("vehicle/body/bcm");
    ASSERT_TRUE(after.empty());
}

void test_client_write_requires_lock_and_throws_sovd_error() {
    LiveServer server;
    SovdClient cli(server.base_url());

    // An *unlocked* entity accepts a write regardless of the lock-id header
    // (the header is only checked once something actually holds the lock) --
    // so to see a genuine 423, someone else has to hold the lock first.
    SovdClient holder(server.base_url());
    std::string real_lock_id = holder.acquire_lock("vehicle/body/bcm", 60);

    bool threw = false;
    try {
        cli.put_data("vehicle/body/bcm", "door_lock_state", "02", "not-the-holders-lock-id");
    } catch (const SovdError &ex) {
        threw = true;
        ASSERT_EQ(ex.status, 423);
    }
    ASSERT_TRUE(threw);

    holder.release_lock("vehicle/body/bcm", real_lock_id);
}

void test_client_lock_guard_acquires_and_releases_on_scope_exit() {
    LiveServer server;
    SovdClient cli(server.base_url());

    {
        LockGuard lock(cli, "vehicle/body/bcm", 60, /*heartbeat=*/false);
        ASSERT_FALSE(lock.lock_id().empty());

        // Held: a second acquire from a fresh client conflicts.
        SovdClient other(server.base_url());
        bool threw = false;
        try {
            other.acquire_lock("vehicle/body/bcm", 60);
        } catch (const SovdError &ex) {
            threw = true;
            ASSERT_EQ(ex.status, 423);
        }
        ASSERT_TRUE(threw);
    }

    // Released on scope exit -> a fresh acquire now succeeds.
    std::string id = cli.acquire_lock("vehicle/body/bcm", 5);
    ASSERT_FALSE(id.empty());
    cli.release_lock("vehicle/body/bcm", id);
}

void test_client_lock_guard_heartbeat_keeps_lock_alive_past_original_ttl() {
    LiveServer server;
    SovdClient cli(server.base_url());

    // 1s TTL with heartbeat on -> renewed at ~0.5s. Without heartbeat this
    // lock would be gone by the time we check at 1.4s.
    LockGuard lock(cli, "vehicle/body/bcm", 1, /*heartbeat=*/true);
    std::this_thread::sleep_for(std::chrono::milliseconds(1400));

    SovdClient other(server.base_url());
    bool threw = false;
    try {
        other.acquire_lock("vehicle/body/bcm", 1);
    } catch (const SovdError &ex) {
        threw = true;
        ASSERT_EQ(ex.status, 423); // still held -> heartbeat renewed it
    }
    ASSERT_TRUE(threw);
}

void test_client_mode_and_operation() {
    LiveServer server;
    SovdClient cli(server.base_url());

    LockGuard lock(cli, "vehicle/body/bcm", 60, /*heartbeat=*/false);
    cli.set_mode("vehicle/body/bcm", "extended", lock.lock_id());
    nlohmann::json result = cli.execute_operation("vehicle/body/bcm", "self_test", "{}", lock.lock_id());
    (void)result; // mock's op result shape isn't the point here -- just that it doesn't throw
}

} // namespace

int main() {
    RUN_TEST(test_client_list_entities_and_docs);
    RUN_TEST(test_client_read_single_and_batch);
    RUN_TEST(test_client_faults_read_and_clear);
    RUN_TEST(test_client_write_requires_lock_and_throws_sovd_error);
    RUN_TEST(test_client_lock_guard_acquires_and_releases_on_scope_exit);
    RUN_TEST(test_client_lock_guard_heartbeat_keeps_lock_alive_past_original_ttl);
    RUN_TEST(test_client_mode_and_operation);
    return testfw::summary();
}
