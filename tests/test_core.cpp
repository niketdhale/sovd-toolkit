#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>
#include <thread>

#include "httplib.h"
#include "json.hpp"
#include "mock_adapter.h"
#include "sovd/catalog/did_catalog.hpp"
#include "sovd/entity_registry.hpp"
#include "sovd/lock_manager.hpp"
#include "sovd/server/config_loader.hpp"
#include "sovd/server/mqtt_publisher.hpp"
#include "sovd/server/routes.hpp"
#include "test_framework.hpp"

using namespace sovd;
using namespace sovd::catalog;
using json = nlohmann::json;

// ---------------------------------------------------------------------
// EntityRegistry
// ---------------------------------------------------------------------

void test_registry_add_and_find_root() {
    EntityRegistry reg;
    ASSERT_TRUE(reg.add_entity("vehicle", EntityType::Vehicle));
    const Entity *e = reg.find("vehicle");
    ASSERT_TRUE(e != nullptr);
    ASSERT_EQ(e->path, "vehicle");
    ASSERT_TRUE(e->type == EntityType::Vehicle);
    ASSERT_TRUE(e->parent.empty());
    ASSERT_FALSE(e->has_backend());
}

void test_registry_child_requires_existing_parent() {
    EntityRegistry reg;
    // parent "vehicle" not yet registered -> orphan rejection
    ASSERT_FALSE(reg.add_entity("vehicle/body", EntityType::Area));
    ASSERT_TRUE(reg.find("vehicle/body") == nullptr);

    ASSERT_TRUE(reg.add_entity("vehicle", EntityType::Vehicle));
    ASSERT_TRUE(reg.add_entity("vehicle/body", EntityType::Area));
    const Entity *e = reg.find("vehicle/body");
    ASSERT_TRUE(e != nullptr);
    ASSERT_EQ(e->parent, "vehicle");
}

void test_registry_rejects_duplicate_path() {
    EntityRegistry reg;
    ASSERT_TRUE(reg.add_entity("vehicle", EntityType::Vehicle));
    ASSERT_FALSE(reg.add_entity("vehicle", EntityType::Vehicle));
}

void test_registry_has_backend_reflects_vtable() {
    EntityRegistry reg;
    const sovd_vtable_t *mock = sovd_mock_adapter_vtable();
    ASSERT_TRUE(reg.add_entity("vehicle", EntityType::Vehicle));
    ASSERT_TRUE(reg.add_entity("vehicle/body", EntityType::Area)); // grouping node
    sovd_adapter_ctx *ctx = mock->create(nullptr);
    ASSERT_TRUE(reg.add_entity("vehicle/body/bcm", EntityType::Component, mock, ctx));

    ASSERT_FALSE(reg.find("vehicle/body")->has_backend());
    ASSERT_TRUE(reg.find("vehicle/body/bcm")->has_backend());
}

void test_registry_list_all_preserves_insertion_order() {
    EntityRegistry reg;
    ASSERT_TRUE(reg.add_entity("vehicle", EntityType::Vehicle));
    ASSERT_TRUE(reg.add_entity("vehicle/body", EntityType::Area));
    ASSERT_TRUE(reg.add_entity("vehicle/adas", EntityType::Area));

    auto all = reg.list_all();
    ASSERT_EQ(all.size(), static_cast<size_t>(3));
    ASSERT_EQ(all[0]->path, "vehicle");
    ASSERT_EQ(all[1]->path, "vehicle/body");
    ASSERT_EQ(all[2]->path, "vehicle/adas");
}

void test_registry_remove_rejects_node_with_children() {
    EntityRegistry reg;
    ASSERT_TRUE(reg.add_entity("vehicle", EntityType::Vehicle));
    ASSERT_TRUE(reg.add_entity("vehicle/body", EntityType::Area));

    ASSERT_FALSE(reg.remove_entity("vehicle")); // has a child -> orphan rejection
    ASSERT_TRUE(reg.remove_entity("vehicle/body")); // leaf -> ok
    ASSERT_TRUE(reg.find("vehicle/body") == nullptr);
    ASSERT_TRUE(reg.remove_entity("vehicle"));
}

void test_registry_unknown_path_not_found() {
    EntityRegistry reg;
    ASSERT_TRUE(reg.find("vehicle/nope") == nullptr);
    ASSERT_FALSE(reg.remove_entity("vehicle/nope"));
}

// ---------------------------------------------------------------------
// LockManager (fake, controllable clock — never sleeps)
// ---------------------------------------------------------------------

struct FakeClock {
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point operator()() const { return now; }
    void advance(int seconds) { now += std::chrono::seconds(seconds); }
};

void test_lock_acquire_and_conflict() {
    FakeClock clock;
    LockManager locks([&clock] { return clock(); });

    auto id1 = locks.acquire("vehicle/body/bcm", 60);
    ASSERT_TRUE(id1.has_value());
    ASSERT_FALSE(id1->empty());

    auto id2 = locks.acquire("vehicle/body/bcm", 60);
    ASSERT_FALSE(id2.has_value()); // conflict: already locked, unexpired
}

void test_lock_expires_after_ttl() {
    FakeClock clock;
    LockManager locks([&clock] { return clock(); });

    auto id1 = locks.acquire("vehicle/body/bcm", 10);
    ASSERT_TRUE(id1.has_value());
    ASSERT_TRUE(locks.is_locked("vehicle/body/bcm"));

    clock.advance(11);
    ASSERT_FALSE(locks.is_locked("vehicle/body/bcm"));

    auto id2 = locks.acquire("vehicle/body/bcm", 10);
    ASSERT_TRUE(id2.has_value()); // previous holder expired, so this succeeds
}

void test_lock_check_lock_semantics() {
    FakeClock clock;
    LockManager locks([&clock] { return clock(); });

    ASSERT_TRUE(locks.check_lock("vehicle/body/bcm", "")); // unlocked: anything passes

    auto id = locks.acquire("vehicle/body/bcm", 60);
    ASSERT_TRUE(locks.check_lock("vehicle/body/bcm", *id));
    ASSERT_FALSE(locks.check_lock("vehicle/body/bcm", ""));
    ASSERT_FALSE(locks.check_lock("vehicle/body/bcm", "wrong-id"));
}

void test_lock_release_wrong_id_keeps_lock() {
    FakeClock clock;
    LockManager locks([&clock] { return clock(); });

    auto id = locks.acquire("vehicle/body/bcm", 60);
    ASSERT_TRUE(locks.release("vehicle/body/bcm", "wrong-id") == LockReleaseResult::WrongId);
    ASSERT_TRUE(locks.is_locked("vehicle/body/bcm")); // still held

    ASSERT_TRUE(locks.release("vehicle/body/bcm", *id) == LockReleaseResult::Released);
    ASSERT_FALSE(locks.is_locked("vehicle/body/bcm"));
}

void test_lock_release_not_found() {
    FakeClock clock;
    LockManager locks([&clock] { return clock(); });
    ASSERT_TRUE(locks.release("vehicle/body/bcm", "anything") == LockReleaseResult::NotFound);
}

void test_lock_renew_extends_ttl_and_keeps_id() {
    FakeClock clock;
    LockManager locks([&clock] { return clock(); });

    auto id = locks.acquire("vehicle/body/bcm", 10);
    clock.advance(8); // not yet expired
    ASSERT_TRUE(locks.renew("vehicle/body/bcm", *id, 10) == LockRenewResult::Renewed);

    clock.advance(8); // would have expired under the original TTL, not the renewed one
    ASSERT_TRUE(locks.is_locked("vehicle/body/bcm"));
    ASSERT_TRUE(locks.check_lock("vehicle/body/bcm", *id)); // same id, not a new lock
}

void test_lock_renew_wrong_id_leaves_ttl_unchanged() {
    FakeClock clock;
    LockManager locks([&clock] { return clock(); });

    auto id = locks.acquire("vehicle/body/bcm", 10);
    ASSERT_TRUE(locks.renew("vehicle/body/bcm", "wrong-id", 60) == LockRenewResult::WrongId);

    clock.advance(11); // original TTL elapses -- wrong-id renew didn't extend it
    ASSERT_FALSE(locks.is_locked("vehicle/body/bcm"));
}

void test_lock_renew_not_found_after_expiry() {
    FakeClock clock;
    LockManager locks([&clock] { return clock(); });

    auto id = locks.acquire("vehicle/body/bcm", 10);
    clock.advance(11);
    ASSERT_TRUE(locks.renew("vehicle/body/bcm", *id, 10) == LockRenewResult::NotFound);
}

// ---------------------------------------------------------------------
// Mock adapter (direct vtable calls)
// ---------------------------------------------------------------------

void test_mock_adapter_faults_roundtrip() {
    const sovd_vtable_t *v = sovd_mock_adapter_vtable();
    sovd_adapter_ctx *ctx = v->create(nullptr);

    sovd_fault_t *faults = nullptr;
    size_t count = 0;
    ASSERT_TRUE(v->read_faults(ctx, "vehicle/body/bcm", &faults, &count) == SOVD_OK);
    ASSERT_EQ(count, static_cast<size_t>(2));
    ASSERT_EQ(std::string(faults[0].code), "P0A0F-16");
    ASSERT_EQ(std::string(faults[0].status), "confirmed");
    ASSERT_EQ(std::string(faults[1].code), "P0420-14");
    ASSERT_EQ(std::string(faults[1].status), "pending");
    v->free_faults(faults, count);

    ASSERT_TRUE(v->clear_faults(ctx, "vehicle/body/bcm") == SOVD_OK);
    faults = nullptr;
    count = 1;
    ASSERT_TRUE(v->read_faults(ctx, "vehicle/body/bcm", &faults, &count) == SOVD_OK);
    ASSERT_EQ(count, static_cast<size_t>(0));

    v->destroy(ctx);
}

void test_mock_adapter_data_read_write() {
    const sovd_vtable_t *v = sovd_mock_adapter_vtable();
    sovd_adapter_ctx *ctx = v->create(nullptr);

    sovd_buffer_t buf{};
    ASSERT_TRUE(v->read_data(ctx, "vehicle/body/bcm", "010A", &buf) == SOVD_OK);
    ASSERT_EQ(buf.len, static_cast<size_t>(2));
    ASSERT_EQ(buf.data[0], 0x32);
    ASSERT_EQ(buf.data[1], 0xC8);
    v->free_buffer(&buf);

    sovd_buffer_t missing{};
    ASSERT_TRUE(v->read_data(ctx, "vehicle/body/bcm", "FFFF", &missing) == SOVD_NOT_FOUND);

    uint8_t new_value[2] = {0x00, 0x64};
    ASSERT_TRUE(v->write_data(ctx, "vehicle/body/bcm", "010A", new_value, 2) == SOVD_OK);

    sovd_buffer_t after{};
    ASSERT_TRUE(v->read_data(ctx, "vehicle/body/bcm", "010A", &after) == SOVD_OK);
    ASSERT_EQ(after.len, static_cast<size_t>(2));
    ASSERT_EQ(after.data[0], 0x00);
    ASSERT_EQ(after.data[1], 0x64);
    v->free_buffer(&after);

    v->destroy(ctx);
}

void test_mock_adapter_mode_and_operation() {
    const sovd_vtable_t *v = sovd_mock_adapter_vtable();
    sovd_adapter_ctx *ctx = v->create(nullptr);

    ASSERT_TRUE(v->set_mode(ctx, "vehicle/body/bcm", "extended") == SOVD_OK);

    char *result = nullptr;
    ASSERT_TRUE(v->execute_operation(ctx, "vehicle/body/bcm", "self_test", "{}", &result) == SOVD_OK);
    ASSERT_TRUE(result != nullptr);
    json parsed = json::parse(result);
    ASSERT_EQ(parsed["operation"].get<std::string>(), "self_test");
    ASSERT_EQ(parsed["status"].get<std::string>(), "completed");
    v->free_string(result);

    v->destroy(ctx);
}

// ---------------------------------------------------------------------
// DID catalog (YAML -> typed definitions)
// ---------------------------------------------------------------------

const char *kSampleCatalogYaml = R"(
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

void test_catalog_parses_data_and_operations() {
    Catalog cat = Catalog::load_from_string(kSampleCatalogYaml);
    ASSERT_EQ(cat.data().size(), static_cast<size_t>(3));
    ASSERT_EQ(cat.operations().size(), static_cast<size_t>(1));

    const DataItem *vin = cat.find_by_id("vin");
    ASSERT_TRUE(vin != nullptr);
    ASSERT_EQ(vin->did, static_cast<uint16_t>(0xF190));
    ASSERT_TRUE(vin->type == DataType::String);
    ASSERT_TRUE(vin->access == Access::Read);
    ASSERT_EQ(vin->length, 17);

    const DataItem *by_did = cat.find_by_did(0x010A);
    ASSERT_TRUE(by_did != nullptr);
    ASSERT_EQ(by_did->id, "battery_voltage");

    ASSERT_TRUE(cat.find_by_id("nonexistent") == nullptr);

    const Operation *op = cat.find_operation("self_test");
    ASSERT_TRUE(op != nullptr);
    ASSERT_EQ(op->routine_id, static_cast<uint16_t>(0x0203));
    ASSERT_TRUE(op->requires_session.has_value());
    ASSERT_EQ(*op->requires_session, "extended");
    ASSERT_TRUE(op->async);
}

void test_catalog_float_encoding_fields() {
    Catalog cat = Catalog::load_from_string(kSampleCatalogYaml);
    const DataItem *v = cat.find_by_id("battery_voltage");
    ASSERT_TRUE(v != nullptr);
    ASSERT_EQ(v->encoding.bytes, 2);
    ASSERT_EQ(v->encoding.endian, "big");
    ASSERT_EQ(v->encoding.scale, 0.001);
    ASSERT_EQ(v->encoding.unit, "V");
}

void test_catalog_enum_values_parsed() {
    Catalog cat = Catalog::load_from_string(kSampleCatalogYaml);
    const DataItem *d = cat.find_by_id("door_lock_state");
    ASSERT_TRUE(d != nullptr);
    ASSERT_TRUE(d->io_control);
    ASSERT_TRUE(d->access == Access::ReadWrite);
    ASSERT_EQ(d->values.size(), static_cast<size_t>(3));
    bool found_locked = false;
    for (auto &ev : d->values) {
        if (ev.raw == 1) {
            ASSERT_EQ(ev.label, "locked");
            found_locked = true;
        }
    }
    ASSERT_TRUE(found_locked);
}

void test_catalog_decode_float_matches_worked_example() {
    // Matches CLAUDE.md's path-mapping example: 0x32C8 * 0.001 == 13.0
    Catalog cat = Catalog::load_from_string(kSampleCatalogYaml);
    const DataItem *v = cat.find_by_id("battery_voltage");
    auto decoded = Catalog::decode(*v, {0x32, 0xC8});
    ASSERT_TRUE(std::holds_alternative<double>(decoded));
    ASSERT_TRUE(std::abs(std::get<double>(decoded) - 13.0) < 1e-9);
}

void test_catalog_decode_enum_known_and_unknown() {
    Catalog cat = Catalog::load_from_string(kSampleCatalogYaml);
    const DataItem *d = cat.find_by_id("door_lock_state");

    auto known = Catalog::decode(*d, {0x01});
    ASSERT_TRUE(std::holds_alternative<std::string>(known));
    ASSERT_EQ(std::get<std::string>(known), "locked");

    // Undocumented raw value: degrade to its numeric text, don't throw.
    auto unknown = Catalog::decode(*d, {0x09});
    ASSERT_TRUE(std::holds_alternative<std::string>(unknown));
    ASSERT_EQ(std::get<std::string>(unknown), "9");
}

void test_catalog_decode_string_and_raw() {
    Catalog cat = Catalog::load_from_string(kSampleCatalogYaml);
    const DataItem *vin = cat.find_by_id("vin");
    std::vector<uint8_t> bytes = {'A', 'B', 'C'};
    auto decoded = Catalog::decode(*vin, bytes);
    ASSERT_TRUE(std::holds_alternative<std::string>(decoded));
    ASSERT_EQ(std::get<std::string>(decoded), "ABC");

    DataItem raw_item;
    raw_item.id = "unknown_item";
    raw_item.type = DataType::Raw;
    auto raw_decoded = Catalog::decode(raw_item, {0xDE, 0xAD});
    ASSERT_EQ(std::get<std::string>(raw_decoded), "DEAD");
}

void test_catalog_decode_float_wrong_length_throws() {
    Catalog cat = Catalog::load_from_string(kSampleCatalogYaml);
    const DataItem *v = cat.find_by_id("battery_voltage");
    bool threw = false;
    try {
        Catalog::decode(*v, {0x01}); // encoding says 2 bytes
    } catch (const CatalogError &) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

void test_catalog_rejects_missing_required_field() {
    bool threw = false;
    try {
        Catalog::load_from_string("data:\n  - did: 0xF190\n    type: string\n    access: read\n");
    } catch (const CatalogError &) {
        threw = true;
    }
    ASSERT_TRUE(threw); // missing 'id'
}

void test_catalog_rejects_float_without_encoding() {
    bool threw = false;
    try {
        Catalog::load_from_string("data:\n  - id: x\n    did: 0x1234\n    type: float\n    access: read\n");
    } catch (const CatalogError &) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

void test_catalog_rejects_invalid_hex_did() {
    bool threw = false;
    try {
        Catalog::load_from_string("data:\n  - id: x\n    did: not-hex\n    type: raw\n    access: read\n");
    } catch (const CatalogError &) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

void test_catalog_empty_document_is_valid_empty_catalog() {
    Catalog cat = Catalog::load_from_string("");
    ASSERT_EQ(cat.data().size(), static_cast<size_t>(0));
    ASSERT_EQ(cat.operations().size(), static_cast<size_t>(0));
}

void test_catalog_load_from_file() {
    std::string path = std::string(SOVD_SOURCE_DIR) + "/catalogs/bcm.yaml";
    Catalog cat = Catalog::load_from_file(path);
    ASSERT_EQ(cat.data().size(), static_cast<size_t>(3));
    ASSERT_TRUE(cat.find_by_id("vin") != nullptr);
}

void test_catalog_access_and_type_to_string() {
    ASSERT_EQ(access_to_string(Access::Read), "read");
    ASSERT_EQ(access_to_string(Access::Write), "write");
    ASSERT_EQ(access_to_string(Access::ReadWrite), "read_write");
    ASSERT_EQ(data_type_to_string(DataType::String), "string");
    ASSERT_EQ(data_type_to_string(DataType::Float), "float");
    ASSERT_EQ(data_type_to_string(DataType::Enum), "enum");
    ASSERT_EQ(data_type_to_string(DataType::Raw), "raw");
}

// ---------------------------------------------------------------------
// End-to-end HTTP (real httplib::Server + httplib::Client, no real network)
// ---------------------------------------------------------------------

struct TestServer {
    EntityRegistry registry;
    LockManager locks;
    httplib::Server svr;
    sovd::server::Router router;
    std::thread thread;
    int port = 0;

    TestServer() : router(registry, locks, "test-server", "domain") {
        const sovd_vtable_t *mock = sovd_mock_adapter_vtable();
        registry.add_entity("vehicle", EntityType::Vehicle);
        registry.add_entity("vehicle/body", EntityType::Area); // grouping node, no backend
        registry.add_entity("vehicle/body/bcm", EntityType::Component, mock, mock->create(nullptr));

        router.register_routes(svr);
        // kSampleCatalogYaml's DIDs (F190/010A/0200) match the mock adapter's
        // seeded data, so named paths resolve to real values in these tests.
        router.attach_catalog("vehicle/body/bcm", Catalog::load_from_string(kSampleCatalogYaml));

        port = svr.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { svr.listen_after_bind(); });
        svr.wait_until_ready();
    }

    ~TestServer() {
        svr.stop();
        thread.join();
    }
};

// Phase 4: a second, smaller live-server fixture whose topology comes from
// load_topology_from_string() instead of being hardcoded -- kept separate
// from TestServer rather than adding a YAML-or-hardcoded mode to it, since
// only the config_loader tests below need this shape.
struct ConfigLoadedServer {
    EntityRegistry registry;
    LockManager locks;
    httplib::Server svr;
    sovd::server::Router router;
    std::thread thread;
    int port = 0;

    explicit ConfigLoadedServer(const std::string &yaml_text) : router(registry, locks, "placeholder", "domain") {
        router.register_routes(svr);
        sovd::server::load_topology_from_string(yaml_text, registry, router);
        port = svr.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { svr.listen_after_bind(); });
        svr.wait_until_ready();
    }

    ~ConfigLoadedServer() {
        svr.stop();
        thread.join();
    }
};

void test_http_root_and_entities() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    auto root = cli.Get("/");
    ASSERT_TRUE(root != nullptr);
    ASSERT_EQ(root->status, 200);
    json root_body = json::parse(root->body);
    ASSERT_EQ(root_body["role"].get<std::string>(), "domain");
    ASSERT_EQ(root_body["api_versions"].size(), static_cast<size_t>(1));
    ASSERT_EQ(root_body["api_versions"][0].get<std::string>(), "v1");

    auto ents = cli.Get("/v1/entities");
    ASSERT_TRUE(ents != nullptr);
    ASSERT_EQ(ents->status, 200);
    json ents_body = json::parse(ents->body);
    ASSERT_EQ(ents_body["items"].size(), static_cast<size_t>(3));
}

void test_http_correlation_id_generated_and_echoed() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    // No header supplied -> server generates one.
    auto generated = cli.Get("/");
    ASSERT_TRUE(generated != nullptr);
    std::string gen_id = generated->get_header_value("X-SOVD-Correlation-Id");
    ASSERT_FALSE(gen_id.empty());

    // Header supplied -> echoed back verbatim, not replaced.
    httplib::Headers headers = {{"X-SOVD-Correlation-Id", "my-test-id-42"}};
    auto echoed = cli.Get("/", headers);
    ASSERT_TRUE(echoed != nullptr);
    ASSERT_EQ(echoed->get_header_value("X-SOVD-Correlation-Id"), "my-test-id-42");

    // Present on error responses too (404), not just success -- correlation
    // has to survive a failed request just as much as a successful one.
    auto not_found = cli.Get("/v1/entities/vehicle/nope/faults", headers);
    ASSERT_TRUE(not_found != nullptr);
    ASSERT_EQ(not_found->status, 404);
    ASSERT_EQ(not_found->get_header_value("X-SOVD-Correlation-Id"), "my-test-id-42");
}

void test_http_correlation_id_appears_in_emitted_events() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    std::vector<std::string> captured;
    ts.router.set_event_sink([&captured](const std::string &line) { captured.push_back(line); });

    httplib::Headers headers = {{"X-SOVD-Correlation-Id", "corr-event-test"}};
    auto del = cli.Delete("/v1/entities/vehicle/body/bcm/faults", headers);
    ASSERT_TRUE(del != nullptr);
    ASSERT_EQ(del->status, 204);

    ASSERT_EQ(captured.size(), static_cast<size_t>(1));
    json evt = json::parse(captured[0]);
    ASSERT_EQ(evt["event"].get<std::string>(), "faults_cleared");
    ASSERT_EQ(evt["correlation_id"].get<std::string>(), "corr-event-test");
}

void test_http_unknown_entity_404() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    auto res = cli.Get("/v1/entities/vehicle/nope/faults");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 404);
}

void test_http_grouping_node_501() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    auto res = cli.Get("/v1/entities/vehicle/body/data/010A");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 501);
}

void test_http_faults_read_and_clear() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    auto get1 = cli.Get("/v1/entities/vehicle/body/bcm/faults");
    ASSERT_TRUE(get1 != nullptr);
    ASSERT_EQ(get1->status, 200);
    ASSERT_EQ(json::parse(get1->body)["faults"].size(), static_cast<size_t>(2));

    // No lock held yet -> clear proceeds without a lock header.
    auto del = cli.Delete("/v1/entities/vehicle/body/bcm/faults");
    ASSERT_TRUE(del != nullptr);
    ASSERT_EQ(del->status, 204);

    auto get2 = cli.Get("/v1/entities/vehicle/body/bcm/faults");
    ASSERT_EQ(json::parse(get2->body)["faults"].size(), static_cast<size_t>(0));
}

void test_http_faults_status_filter() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    auto confirmed = cli.Get("/v1/entities/vehicle/body/bcm/faults?status=confirmed");
    ASSERT_TRUE(confirmed != nullptr);
    ASSERT_EQ(confirmed->status, 200);
    json c_body = json::parse(confirmed->body);
    ASSERT_EQ(c_body["faults"].size(), static_cast<size_t>(1));
    ASSERT_EQ(c_body["faults"][0]["code"].get<std::string>(), "P0A0F-16");

    auto pending = cli.Get("/v1/entities/vehicle/body/bcm/faults?status=pending");
    ASSERT_TRUE(pending != nullptr);
    json p_body = json::parse(pending->body);
    ASSERT_EQ(p_body["faults"].size(), static_cast<size_t>(1));
    ASSERT_EQ(p_body["faults"][0]["code"].get<std::string>(), "P0420-14");

    // Valid enum value, just none seeded with it -> empty, not an error.
    auto test_failed = cli.Get("/v1/entities/vehicle/body/bcm/faults?status=testFailed");
    ASSERT_TRUE(test_failed != nullptr);
    ASSERT_EQ(test_failed->status, 200);
    ASSERT_EQ(json::parse(test_failed->body)["faults"].size(), static_cast<size_t>(0));

    auto invalid = cli.Get("/v1/entities/vehicle/body/bcm/faults?status=bogus");
    ASSERT_TRUE(invalid != nullptr);
    ASSERT_EQ(invalid->status, 400);
}

void test_http_write_data_locked_without_header_423() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    auto lock_res = cli.Post("/v1/entities/vehicle/body/bcm/locks", "{}", "application/json");
    ASSERT_TRUE(lock_res != nullptr);
    ASSERT_EQ(lock_res->status, 201);
    std::string lock_id = json::parse(lock_res->body)["lock_id"].get<std::string>();

    // Write without the lock header while locked -> 423.
    auto put_unheadered = cli.Put("/v1/entities/vehicle/body/bcm/data/010A", R"({"value":"0064"})", "application/json");
    ASSERT_TRUE(put_unheadered != nullptr);
    ASSERT_EQ(put_unheadered->status, 423);

    // Write with the correct header -> succeeds.
    httplib::Headers headers = {{"X-SOVD-Lock-Id", lock_id}};
    auto put_ok = cli.Put("/v1/entities/vehicle/body/bcm/data/010A", headers, R"({"value":"0064"})", "application/json");
    ASSERT_TRUE(put_ok != nullptr);
    ASSERT_EQ(put_ok->status, 204);

    auto get_after = cli.Get("/v1/entities/vehicle/body/bcm/data/010A");
    ASSERT_EQ(json::parse(get_after->body)["value"].get<std::string>(), "0064");
}

void test_http_lock_conflict_and_release() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    auto first = cli.Post("/v1/entities/vehicle/body/bcm/locks", "{}", "application/json");
    ASSERT_EQ(first->status, 201);
    std::string lock_id = json::parse(first->body)["lock_id"].get<std::string>();

    auto second = cli.Post("/v1/entities/vehicle/body/bcm/locks", "{}", "application/json");
    ASSERT_TRUE(second != nullptr);
    ASSERT_EQ(second->status, 423); // conflict: already locked

    auto wrong_release = cli.Delete("/v1/entities/vehicle/body/bcm/locks/not-the-holder");
    ASSERT_TRUE(wrong_release != nullptr);
    ASSERT_EQ(wrong_release->status, 403);

    auto right_release = cli.Delete(("/v1/entities/vehicle/body/bcm/locks/" + lock_id).c_str());
    ASSERT_TRUE(right_release != nullptr);
    ASSERT_EQ(right_release->status, 204);

    // Now unlocked -> a fresh lock can be acquired.
    auto third = cli.Post("/v1/entities/vehicle/body/bcm/locks", "{}", "application/json");
    ASSERT_EQ(third->status, 201);
}

void test_http_lock_renew() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    auto acquired = cli.Post("/v1/entities/vehicle/body/bcm/locks", R"({"ttl_seconds":10})", "application/json");
    ASSERT_EQ(acquired->status, 201);
    std::string lock_id = json::parse(acquired->body)["lock_id"].get<std::string>();

    auto renewed =
        cli.Put(("/v1/entities/vehicle/body/bcm/locks/" + lock_id).c_str(), R"({"ttl_seconds":60})", "application/json");
    ASSERT_TRUE(renewed != nullptr);
    ASSERT_EQ(renewed->status, 200);
    ASSERT_EQ(json::parse(renewed->body)["ttl_seconds"].get<int>(), 60);

    auto wrong_id_renew =
        cli.Put("/v1/entities/vehicle/body/bcm/locks/not-the-holder", R"({"ttl_seconds":60})", "application/json");
    ASSERT_TRUE(wrong_id_renew != nullptr);
    ASSERT_EQ(wrong_id_renew->status, 403);

    auto no_lock_renew =
        cli.Put("/v1/entities/vehicle/body/door_ctrl/locks/anything", R"({"ttl_seconds":60})", "application/json");
    ASSERT_TRUE(no_lock_renew != nullptr);
    ASSERT_EQ(no_lock_renew->status, 404);
}

void test_http_mode_and_operation() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    auto lock_res = cli.Post("/v1/entities/vehicle/body/bcm/locks", "{}", "application/json");
    std::string lock_id = json::parse(lock_res->body)["lock_id"].get<std::string>();
    httplib::Headers headers = {{"X-SOVD-Lock-Id", lock_id}};

    auto mode = cli.Post("/v1/entities/vehicle/body/bcm/modes", headers, R"({"mode":"extended"})", "application/json");
    ASSERT_TRUE(mode != nullptr);
    ASSERT_EQ(mode->status, 204);

    auto op = cli.Post("/v1/entities/vehicle/body/bcm/operations/self_test", headers, "{}", "application/json");
    ASSERT_TRUE(op != nullptr);
    ASSERT_EQ(op->status, 200);
    ASSERT_EQ(json::parse(op->body)["status"].get<std::string>(), "completed");
}

void test_http_docs_lists_catalog_data_and_operations() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    auto res = cli.Get("/v1/entities/vehicle/body/bcm/docs");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 200);
    json body = json::parse(res->body);

    ASSERT_TRUE(body["has_backend"].get<bool>());
    ASSERT_EQ(body["data"].size(), static_cast<size_t>(3));
    ASSERT_EQ(body["operations"].size(), static_cast<size_t>(1));

    bool found_voltage = false;
    for (auto &d : body["data"]) {
        if (d["id"].get<std::string>() == "battery_voltage") {
            ASSERT_EQ(d["did"].get<std::string>(), "010A");
            ASSERT_EQ(d["type"].get<std::string>(), "float");
            ASSERT_EQ(d["unit"].get<std::string>(), "V");
            found_voltage = true;
        }
    }
    ASSERT_TRUE(found_voltage);
    ASSERT_EQ(body["operations"][0]["id"].get<std::string>(), "self_test");

    // Mock declares its capabilities honestly: fully synchronous, no
    // native batch, no distinct IOControl path.
    ASSERT_TRUE(body.contains("capabilities"));
    ASSERT_FALSE(body["capabilities"]["supports_batch_read"].get<bool>());
    ASSERT_FALSE(body["capabilities"]["supports_async_operations"].get<bool>());
    ASSERT_FALSE(body["capabilities"]["supports_io_control"].get<bool>());
}

void test_http_docs_empty_for_uncataloged_entity() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    // vehicle/body is a grouping node with no catalog attached; /docs still
    // succeeds (it's self-description, not a live diagnostic op), just with
    // empty data/operations.
    auto res = cli.Get("/v1/entities/vehicle/body/docs");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 200);
    json body = json::parse(res->body);
    ASSERT_FALSE(body["has_backend"].get<bool>());
    ASSERT_EQ(body["data"].size(), static_cast<size_t>(0));
    ASSERT_EQ(body["operations"].size(), static_cast<size_t>(0));
    ASSERT_FALSE(body.contains("capabilities")); // no backend -> nothing to declare
}

void test_http_named_data_path_decodes_typed_value() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    auto voltage = cli.Get("/v1/entities/vehicle/body/bcm/data/battery_voltage");
    ASSERT_TRUE(voltage != nullptr);
    ASSERT_EQ(voltage->status, 200);
    json v_body = json::parse(voltage->body);
    ASSERT_EQ(v_body["id"].get<std::string>(), "battery_voltage");
    ASSERT_EQ(v_body["did"].get<std::string>(), "010A");
    ASSERT_TRUE(std::abs(v_body["value"].get<double>() - 13.0) < 1e-9);
    ASSERT_EQ(v_body["unit"].get<std::string>(), "V");

    auto lock_state = cli.Get("/v1/entities/vehicle/body/bcm/data/door_lock_state");
    ASSERT_TRUE(lock_state != nullptr);
    ASSERT_EQ(lock_state->status, 200);
    ASSERT_EQ(json::parse(lock_state->body)["value"].get<std::string>(), "locked");

    auto vin = cli.Get("/v1/entities/vehicle/body/bcm/data/vin");
    ASSERT_TRUE(vin != nullptr);
    ASSERT_EQ(vin->status, 200);
    ASSERT_EQ(json::parse(vin->body)["value"].get<std::string>(), "SOVDTOOLKITMOCK01");
}

void test_http_data_path_falls_back_to_raw_hex_did() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    // "010A" isn't a catalog id, so it's treated as a raw DID (Phase 0
    // behavior) instead of a 404.
    auto res = cli.Get("/v1/entities/vehicle/body/bcm/data/010A");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 200);
    json body = json::parse(res->body);
    ASSERT_EQ(body["id"].get<std::string>(), "010A");
    ASSERT_EQ(body["value"].get<std::string>(), "32C8");
    ASSERT_FALSE(body.contains("unit"));
}

void test_http_batch_data_read_mixed_success_and_failure() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    // battery_voltage: named/decoded. 010A: raw DID fallback (same DID,
    // different resolution path). FFFF: valid hex, but not seeded -> 404
    // inline, doesn't abort the rest of the batch.
    auto res = cli.Get("/v1/entities/vehicle/body/bcm/data?ids=battery_voltage,010A,FFFF");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 200);
    json body = json::parse(res->body);
    ASSERT_EQ(body["items"].size(), static_cast<size_t>(3));

    ASSERT_EQ(body["items"][0]["id"].get<std::string>(), "battery_voltage");
    ASSERT_TRUE(std::abs(body["items"][0]["value"].get<double>() - 13.0) < 1e-9);
    ASSERT_EQ(body["items"][0]["unit"].get<std::string>(), "V");

    ASSERT_EQ(body["items"][1]["id"].get<std::string>(), "010A");
    ASSERT_EQ(body["items"][1]["value"].get<std::string>(), "32C8");

    ASSERT_EQ(body["items"][2]["id"].get<std::string>(), "FFFF");
    ASSERT_TRUE(body["items"][2].contains("error"));
    ASSERT_FALSE(body["items"][2].contains("value"));
}

void test_http_batch_data_read_requires_ids_param() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    auto missing = cli.Get("/v1/entities/vehicle/body/bcm/data");
    ASSERT_TRUE(missing != nullptr);
    ASSERT_EQ(missing->status, 400);

    auto empty = cli.Get("/v1/entities/vehicle/body/bcm/data?ids=");
    ASSERT_TRUE(empty != nullptr);
    ASSERT_EQ(empty->status, 400);
}

void test_http_batch_data_read_501_on_no_backend() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    auto res = cli.Get("/v1/entities/vehicle/body/data?ids=anything");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 501);
}

void test_http_named_data_path_put_readonly_guard_and_write() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    auto lock_res = cli.Post("/v1/entities/vehicle/body/bcm/locks", "{}", "application/json");
    std::string lock_id = json::parse(lock_res->body)["lock_id"].get<std::string>();
    httplib::Headers headers = {{"X-SOVD-Lock-Id", lock_id}};

    // vin is access: read in the catalog -> write is rejected before it
    // ever reaches the adapter.
    auto readonly = cli.Put("/v1/entities/vehicle/body/bcm/data/vin", headers, R"({"value":"00"})", "application/json");
    ASSERT_TRUE(readonly != nullptr);
    ASSERT_EQ(readonly->status, 400);

    // door_lock_state is read_write -> named write succeeds, and a
    // subsequent named read observes it (raw hex wire format, per the
    // catalog's still-hex-only PUT contract).
    auto write = cli.Put("/v1/entities/vehicle/body/bcm/data/door_lock_state", headers, R"({"value":"02"})",
                          "application/json");
    ASSERT_TRUE(write != nullptr);
    ASSERT_EQ(write->status, 204);

    auto after = cli.Get("/v1/entities/vehicle/body/bcm/data/door_lock_state");
    ASSERT_EQ(json::parse(after->body)["value"].get<std::string>(), "deadlocked");
}

void test_http_telemetry_sink_separate_from_event_sink() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    std::vector<std::string> events;
    std::vector<std::string> telemetry;
    ts.router.set_event_sink([&events](const std::string &line) { events.push_back(line); });
    ts.router.set_telemetry_sink([&telemetry](const std::string &line) { telemetry.push_back(line); });

    auto res = cli.Get("/v1/entities");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 200);

    // httplib's set_logger() (which telemetry_sink_ is wired through) fires
    // *after* the response body is already written to the socket -- unlike
    // set_event_sink(), which handlers call before res.status is even set.
    // So the client can legitimately observe the response before the
    // server-side logger call runs; a bounded poll (not a blind sleep) is
    // the same real-async-hazard tradeoff CLAUDE.md documents for
    // session_manager's heartbeat tests, applied to a different cause.
    for (int waited_ms = 0; telemetry.empty() && waited_ms < 200; waited_ms += 5) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // A plain GET emits no security event, but does emit telemetry -- the
    // two sinks are independent (CLAUDE.md: "an IDS should not be your APM").
    ASSERT_EQ(events.size(), static_cast<size_t>(0));
    ASSERT_EQ(telemetry.size(), static_cast<size_t>(1));

    json evt = json::parse(telemetry[0]);
    ASSERT_EQ(evt["event"].get<std::string>(), "http_request");
    ASSERT_EQ(evt["method"].get<std::string>(), "GET");
    ASSERT_EQ(evt["path"].get<std::string>(), "/v1/entities");
    ASSERT_EQ(evt["status"].get<int>(), 200);
    ASSERT_TRUE(evt["duration_ms"].get<double>() >= 0.0);
}

// Pure MQTT packet framing -- no socket, no broker. Matches the
// doip_protocol precedent: wire-format encode/decode is unit-testable on
// its own, independent of the transport that carries it.
void test_mqtt_encode_connect_packet() {
    auto pkt = sovd::server::mqtt::encode_connect("client-42", 60);
    ASSERT_EQ(pkt[0], static_cast<uint8_t>(0x10)); // CONNECT
    // Variable header + payload: "MQTT" (2+4) + level(1) + flags(1) + keepalive(2) + "client-42" (2+9) = 21
    ASSERT_EQ(pkt[1], static_cast<uint8_t>(21));
    ASSERT_EQ(pkt.size(), static_cast<size_t>(23));
    ASSERT_EQ(pkt[2], static_cast<uint8_t>(0x00));
    ASSERT_EQ(pkt[3], static_cast<uint8_t>(0x04));
    ASSERT_EQ(std::string(pkt.begin() + 4, pkt.begin() + 8), "MQTT");
    ASSERT_EQ(pkt[8], static_cast<uint8_t>(0x04)); // protocol level 3.1.1
    ASSERT_EQ(pkt[9], static_cast<uint8_t>(0x02)); // clean session
    ASSERT_EQ(pkt[10], static_cast<uint8_t>(0x00));
    ASSERT_EQ(pkt[11], static_cast<uint8_t>(60)); // keep-alive
    ASSERT_EQ(std::string(pkt.end() - 9, pkt.end()), "client-42");
}

void test_mqtt_encode_publish_packet() {
    std::string topic_str = "sovd/demo/events"; // 16 chars
    std::string payload_str = "{\"event\":\"x\"}"; // 13 chars
    auto pkt = sovd::server::mqtt::encode_publish(topic_str, payload_str);
    ASSERT_EQ(pkt[0], static_cast<uint8_t>(0x30)); // PUBLISH, QoS0
    std::string topic(pkt.begin() + 2 + 2, pkt.begin() + 2 + 2 + static_cast<long>(topic_str.size()));
    ASSERT_EQ(topic, topic_str);
    std::string payload(pkt.end() - static_cast<long>(payload_str.size()), pkt.end());
    ASSERT_EQ(payload, payload_str);
}

void test_mqtt_encode_disconnect_packet() {
    auto pkt = sovd::server::mqtt::encode_disconnect();
    ASSERT_EQ(pkt.size(), static_cast<size_t>(2));
    ASSERT_EQ(pkt[0], static_cast<uint8_t>(0xE0));
    ASSERT_EQ(pkt[1], static_cast<uint8_t>(0x00));
}

// ---------------------------------------------------------------------
// Phase 4: config_loader (YAML -> registry/router) and proxy forwarding.

using sovd::server::ConfigError;
using sovd::server::load_topology_from_string;
using sovd::server::ServerConfig;

void test_config_loader_basic_topology_and_mock_adapter() {
    EntityRegistry registry;
    LockManager locks;
    sovd::server::Router router(registry, locks, "placeholder", "domain");

    ServerConfig cfg = load_topology_from_string(R"(
server: {id: sovd-test-domain, port: 20099, role: domain}
entities:
  - path: vehicle
    type: vehicle
  - path: vehicle/body
    type: area
  - path: vehicle/body/bcm
    type: component
    adapter: { kind: mock }
)",
                                                  registry, router);
    ASSERT_EQ(cfg.id, "sovd-test-domain");
    ASSERT_EQ(cfg.port, 20099);
    ASSERT_EQ(cfg.role, "domain");

    const Entity *bcm = registry.find("vehicle/body/bcm");
    ASSERT_TRUE(bcm != nullptr);
    ASSERT_TRUE(bcm->has_backend());

    const Entity *body = registry.find("vehicle/body");
    ASSERT_TRUE(body != nullptr);
    ASSERT_FALSE(body->has_backend()); // grouping node: no adapter block
}

void test_config_loader_rejects_missing_server_block() {
    EntityRegistry registry;
    LockManager locks;
    sovd::server::Router router(registry, locks, "placeholder", "domain");
    bool threw = false;
    try {
        load_topology_from_string("entities: []", registry, router);
    } catch (const ConfigError &) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

void test_config_loader_rejects_orphan_entity() {
    EntityRegistry registry;
    LockManager locks;
    sovd::server::Router router(registry, locks, "placeholder", "domain");
    bool threw = false;
    try {
        // "vehicle" (the parent) is never listed -- orphan rejection.
        load_topology_from_string(R"(
server: {id: x, port: 1, role: domain}
entities:
  - path: vehicle/body
    type: area
)",
                                   registry, router);
    } catch (const ConfigError &) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

void test_config_loader_unknown_adapter_kind_falls_back_to_grouping_node() {
    EntityRegistry registry;
    LockManager locks;
    sovd::server::Router router(registry, locks, "placeholder", "domain");
    load_topology_from_string(R"(
server: {id: x, port: 1, role: domain}
entities:
  - path: vehicle
    type: vehicle
  - path: vehicle/thing
    type: component
    adapter: { kind: nonexistent_kind }
)",
                               registry, router);
    const Entity *e = registry.find("vehicle/thing");
    ASSERT_TRUE(e != nullptr);
    ASSERT_FALSE(e->has_backend()); // degrades to 501-on-diagnostics, not a crash or aborted load
}

void test_config_loader_sovd_proxy_requires_base_url() {
    EntityRegistry registry;
    LockManager locks;
    sovd::server::Router router(registry, locks, "placeholder", "domain");
    bool threw = false;
    try {
        load_topology_from_string(R"(
server: {id: x, port: 1, role: domain}
entities:
  - path: vehicle
    type: vehicle
  - path: vehicle/bcm
    type: component
    adapter: { kind: sovd_proxy, remote_path: vehicle/bcm }
)",
                                   registry, router);
    } catch (const ConfigError &) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

// The real Phase 4 exit criterion: two live servers, gateway config-loaded
// with a sovd_proxy pointing at the domain server's dynamic port. Proves
// the properties CLAUDE.md's original vtable-based sketch couldn't have
// (see ProxyTarget's comment in routes.hpp): typed /docs and named-id
// decode pass through with zero catalog on the gateway, and locks acquired
// through the gateway are actually held on the domain server, not cached
// locally.
void test_config_loader_proxy_forwards_docs_data_and_locks() {
    ConfigLoadedServer domain(R"(
server: {id: sovd-domain-test, port: 0, role: domain}
entities:
  - path: vehicle
    type: vehicle
  - path: vehicle/body
    type: area
  - path: vehicle/body/bcm
    type: component
    adapter: { kind: mock }
)");
    domain.router.attach_catalog("vehicle/body/bcm", Catalog::load_from_string(kSampleCatalogYaml));

    std::ostringstream gw_yaml;
    gw_yaml << "server: {id: sovd-gateway-test, port: 0, role: gateway}\n"
               "entities:\n"
               "  - path: vehicle\n"
               "    type: vehicle\n"
               "  - path: vehicle/body\n"
               "    type: area\n"
               "  - path: vehicle/body/bcm\n"
               "    type: component\n"
               "    adapter:\n"
               "      kind: sovd_proxy\n"
               "      base_url: http://127.0.0.1:"
            << domain.port
            << "\n"
               "      remote_path: vehicle/body/bcm\n"
               "      forward_locks: true\n";
    ConfigLoadedServer gateway(gw_yaml.str());

    httplib::Client cli("127.0.0.1", gateway.port);

    // /docs has no local catalog to render from -- it's forwarded, and the
    // domain's real typed catalog comes back through the gateway.
    auto docs = cli.Get("/v1/entities/vehicle/body/bcm/docs");
    ASSERT_TRUE(docs != nullptr);
    ASSERT_EQ(docs->status, 200);
    json docs_body = json::parse(docs->body);
    bool found_battery = false;
    for (auto &item : docs_body["data"]) {
        if (item["id"] == "battery_voltage") found_battery = true;
    }
    ASSERT_TRUE(found_battery);

    // Named-id read through the gateway returns the domain's typed decode
    // (not raw hex -- the gateway has no catalog to decode with itself).
    auto read = cli.Get("/v1/entities/vehicle/body/bcm/data/battery_voltage");
    ASSERT_TRUE(read != nullptr);
    ASSERT_EQ(read->status, 200);
    json read_body = json::parse(read->body);
    ASSERT_TRUE(std::abs(read_body["value"].get<double>() - 13.0) < 1e-9);
    ASSERT_EQ(read_body["unit"].get<std::string>(), "V");

    // A lock acquired through the gateway is held on the domain server --
    // proof forward_locks isn't a local no-op. The domain server itself
    // now refuses a second lock on the same entity.
    auto lock_res = cli.Post("/v1/entities/vehicle/body/bcm/locks", "{}", "application/json");
    ASSERT_TRUE(lock_res != nullptr);
    ASSERT_EQ(lock_res->status, 201);
    std::string lock_id = json::parse(lock_res->body)["lock_id"].get<std::string>();

    httplib::Client domain_cli("127.0.0.1", domain.port);
    auto conflict = domain_cli.Post("/v1/entities/vehicle/body/bcm/locks", "{}", "application/json");
    ASSERT_TRUE(conflict != nullptr);
    ASSERT_EQ(conflict->status, 423);

    httplib::Headers unlock_headers = {{"X-SOVD-Lock-Id", lock_id}};
    auto unlock = cli.Delete("/v1/entities/vehicle/body/bcm/locks/" + lock_id, unlock_headers);
    ASSERT_TRUE(unlock != nullptr);
    ASSERT_EQ(unlock->status, 204);
}

// Graceful degradation: one entity's proxy target being unreachable fails
// only requests aimed at that entity -- not GET /entities (which never
// calls any backend, proxied or not) and not a second, independent entity.
void test_config_loader_unreachable_proxy_degrades_gracefully() {
    ConfigLoadedServer gateway(R"(
server: {id: sovd-gateway-degraded-test, port: 0, role: gateway}
entities:
  - path: vehicle
    type: vehicle
  - path: vehicle/body
    type: area
  - path: vehicle/body/bcm
    type: component
    adapter:
      kind: sovd_proxy
      base_url: http://127.0.0.1:1
      remote_path: vehicle/body/bcm
  - path: vehicle/body/door_ctrl
    type: component
    adapter: { kind: mock }
)");
    httplib::Client cli("127.0.0.1", gateway.port);

    // The unreachable entity's own request fails cleanly (502), not a hang
    // or a crash.
    auto bad = cli.Get("/v1/entities/vehicle/body/bcm/data/010A");
    ASSERT_TRUE(bad != nullptr);
    ASSERT_EQ(bad->status, 502);

    // Listing still works and still lists it (has_backend reflects the
    // proxy attachment even though the remote is down).
    auto list = cli.Get("/v1/entities");
    ASSERT_TRUE(list != nullptr);
    ASSERT_EQ(list->status, 200);
    json items = json::parse(list->body)["items"];
    bool bcm_listed = false;
    for (auto &item : items) {
        if (item["path"] == "vehicle/body/bcm") {
            bcm_listed = true;
            ASSERT_TRUE(item["has_backend"].get<bool>());
        }
    }
    ASSERT_TRUE(bcm_listed);

    // A second, independent entity (real mock backend, no proxy involved)
    // is completely unaffected.
    auto ok = cli.Get("/v1/entities/vehicle/body/door_ctrl/data/0200");
    ASSERT_TRUE(ok != nullptr);
    ASSERT_EQ(ok->status, 200);
}

// ---------------------------------------------------------------------

int main() {
    RUN_TEST(test_registry_add_and_find_root);
    RUN_TEST(test_registry_child_requires_existing_parent);
    RUN_TEST(test_registry_rejects_duplicate_path);
    RUN_TEST(test_registry_has_backend_reflects_vtable);
    RUN_TEST(test_registry_list_all_preserves_insertion_order);
    RUN_TEST(test_registry_remove_rejects_node_with_children);
    RUN_TEST(test_registry_unknown_path_not_found);

    RUN_TEST(test_lock_acquire_and_conflict);
    RUN_TEST(test_lock_expires_after_ttl);
    RUN_TEST(test_lock_check_lock_semantics);
    RUN_TEST(test_lock_release_wrong_id_keeps_lock);
    RUN_TEST(test_lock_release_not_found);
    RUN_TEST(test_lock_renew_extends_ttl_and_keeps_id);
    RUN_TEST(test_lock_renew_wrong_id_leaves_ttl_unchanged);
    RUN_TEST(test_lock_renew_not_found_after_expiry);

    RUN_TEST(test_mock_adapter_faults_roundtrip);
    RUN_TEST(test_mock_adapter_data_read_write);
    RUN_TEST(test_mock_adapter_mode_and_operation);

    RUN_TEST(test_catalog_parses_data_and_operations);
    RUN_TEST(test_catalog_float_encoding_fields);
    RUN_TEST(test_catalog_enum_values_parsed);
    RUN_TEST(test_catalog_decode_float_matches_worked_example);
    RUN_TEST(test_catalog_decode_enum_known_and_unknown);
    RUN_TEST(test_catalog_decode_string_and_raw);
    RUN_TEST(test_catalog_decode_float_wrong_length_throws);
    RUN_TEST(test_catalog_rejects_missing_required_field);
    RUN_TEST(test_catalog_rejects_float_without_encoding);
    RUN_TEST(test_catalog_rejects_invalid_hex_did);
    RUN_TEST(test_catalog_empty_document_is_valid_empty_catalog);
    RUN_TEST(test_catalog_load_from_file);
    RUN_TEST(test_catalog_access_and_type_to_string);

    RUN_TEST(test_http_root_and_entities);
    RUN_TEST(test_http_correlation_id_generated_and_echoed);
    RUN_TEST(test_http_correlation_id_appears_in_emitted_events);
    RUN_TEST(test_http_unknown_entity_404);
    RUN_TEST(test_http_grouping_node_501);
    RUN_TEST(test_http_faults_read_and_clear);
    RUN_TEST(test_http_faults_status_filter);
    RUN_TEST(test_http_write_data_locked_without_header_423);
    RUN_TEST(test_http_lock_conflict_and_release);
    RUN_TEST(test_http_lock_renew);
    RUN_TEST(test_http_mode_and_operation);

    RUN_TEST(test_http_docs_lists_catalog_data_and_operations);
    RUN_TEST(test_http_docs_empty_for_uncataloged_entity);
    RUN_TEST(test_http_named_data_path_decodes_typed_value);
    RUN_TEST(test_http_data_path_falls_back_to_raw_hex_did);
    RUN_TEST(test_http_batch_data_read_mixed_success_and_failure);
    RUN_TEST(test_http_batch_data_read_requires_ids_param);
    RUN_TEST(test_http_batch_data_read_501_on_no_backend);
    RUN_TEST(test_http_named_data_path_put_readonly_guard_and_write);
    RUN_TEST(test_http_telemetry_sink_separate_from_event_sink);

    RUN_TEST(test_mqtt_encode_connect_packet);
    RUN_TEST(test_mqtt_encode_publish_packet);
    RUN_TEST(test_mqtt_encode_disconnect_packet);

    RUN_TEST(test_config_loader_basic_topology_and_mock_adapter);
    RUN_TEST(test_config_loader_rejects_missing_server_block);
    RUN_TEST(test_config_loader_rejects_orphan_entity);
    RUN_TEST(test_config_loader_unknown_adapter_kind_falls_back_to_grouping_node);
    RUN_TEST(test_config_loader_sovd_proxy_requires_base_url);
    RUN_TEST(test_config_loader_proxy_forwards_docs_data_and_locks);
    RUN_TEST(test_config_loader_unreachable_proxy_degrades_gracefully);

    return testfw::summary();
}
