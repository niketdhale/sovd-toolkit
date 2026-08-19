#include <chrono>
#include <cstring>
#include <thread>

#include "httplib.h"
#include "json.hpp"
#include "mock_adapter.h"
#include "sovd/entity_registry.hpp"
#include "sovd/lock_manager.hpp"
#include "sovd/server/routes.hpp"
#include "test_framework.hpp"

using namespace sovd;
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

// ---------------------------------------------------------------------
// Mock adapter (direct vtable calls)
// ---------------------------------------------------------------------

void test_mock_adapter_faults_roundtrip() {
    const sovd_vtable_t *v = sovd_mock_adapter_vtable();
    sovd_adapter_ctx *ctx = v->create(nullptr);

    sovd_fault_t *faults = nullptr;
    size_t count = 0;
    ASSERT_TRUE(v->read_faults(ctx, "vehicle/body/bcm", &faults, &count) == SOVD_OK);
    ASSERT_EQ(count, static_cast<size_t>(1));
    ASSERT_EQ(std::string(faults[0].code), "P0A0F-16");
    ASSERT_EQ(std::string(faults[0].status), "confirmed");
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

        port = svr.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { svr.listen_after_bind(); });
        svr.wait_until_ready();
    }

    ~TestServer() {
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

    auto ents = cli.Get("/entities");
    ASSERT_TRUE(ents != nullptr);
    ASSERT_EQ(ents->status, 200);
    json ents_body = json::parse(ents->body);
    ASSERT_EQ(ents_body["items"].size(), static_cast<size_t>(3));
}

void test_http_unknown_entity_404() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    auto res = cli.Get("/entities/vehicle/nope/faults");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 404);
}

void test_http_grouping_node_501() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    auto res = cli.Get("/entities/vehicle/body/data/010A");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 501);
}

void test_http_faults_read_and_clear() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    auto get1 = cli.Get("/entities/vehicle/body/bcm/faults");
    ASSERT_TRUE(get1 != nullptr);
    ASSERT_EQ(get1->status, 200);
    ASSERT_EQ(json::parse(get1->body)["faults"].size(), static_cast<size_t>(1));

    // No lock held yet -> clear proceeds without a lock header.
    auto del = cli.Delete("/entities/vehicle/body/bcm/faults");
    ASSERT_TRUE(del != nullptr);
    ASSERT_EQ(del->status, 204);

    auto get2 = cli.Get("/entities/vehicle/body/bcm/faults");
    ASSERT_EQ(json::parse(get2->body)["faults"].size(), static_cast<size_t>(0));
}

void test_http_write_data_locked_without_header_423() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    auto lock_res = cli.Post("/entities/vehicle/body/bcm/locks", "{}", "application/json");
    ASSERT_TRUE(lock_res != nullptr);
    ASSERT_EQ(lock_res->status, 201);
    std::string lock_id = json::parse(lock_res->body)["lock_id"].get<std::string>();

    // Write without the lock header while locked -> 423.
    auto put_unheadered = cli.Put("/entities/vehicle/body/bcm/data/010A", R"({"value":"0064"})", "application/json");
    ASSERT_TRUE(put_unheadered != nullptr);
    ASSERT_EQ(put_unheadered->status, 423);

    // Write with the correct header -> succeeds.
    httplib::Headers headers = {{"X-SOVD-Lock-Id", lock_id}};
    auto put_ok = cli.Put("/entities/vehicle/body/bcm/data/010A", headers, R"({"value":"0064"})", "application/json");
    ASSERT_TRUE(put_ok != nullptr);
    ASSERT_EQ(put_ok->status, 204);

    auto get_after = cli.Get("/entities/vehicle/body/bcm/data/010A");
    ASSERT_EQ(json::parse(get_after->body)["value"].get<std::string>(), "0064");
}

void test_http_lock_conflict_and_release() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    auto first = cli.Post("/entities/vehicle/body/bcm/locks", "{}", "application/json");
    ASSERT_EQ(first->status, 201);
    std::string lock_id = json::parse(first->body)["lock_id"].get<std::string>();

    auto second = cli.Post("/entities/vehicle/body/bcm/locks", "{}", "application/json");
    ASSERT_TRUE(second != nullptr);
    ASSERT_EQ(second->status, 423); // conflict: already locked

    auto wrong_release = cli.Delete("/entities/vehicle/body/bcm/locks/not-the-holder");
    ASSERT_TRUE(wrong_release != nullptr);
    ASSERT_EQ(wrong_release->status, 403);

    auto right_release = cli.Delete(("/entities/vehicle/body/bcm/locks/" + lock_id).c_str());
    ASSERT_TRUE(right_release != nullptr);
    ASSERT_EQ(right_release->status, 204);

    // Now unlocked -> a fresh lock can be acquired.
    auto third = cli.Post("/entities/vehicle/body/bcm/locks", "{}", "application/json");
    ASSERT_EQ(third->status, 201);
}

void test_http_mode_and_operation() {
    TestServer ts;
    httplib::Client cli("127.0.0.1", ts.port);

    auto lock_res = cli.Post("/entities/vehicle/body/bcm/locks", "{}", "application/json");
    std::string lock_id = json::parse(lock_res->body)["lock_id"].get<std::string>();
    httplib::Headers headers = {{"X-SOVD-Lock-Id", lock_id}};

    auto mode = cli.Post("/entities/vehicle/body/bcm/modes", headers, R"({"mode":"extended"})", "application/json");
    ASSERT_TRUE(mode != nullptr);
    ASSERT_EQ(mode->status, 204);

    auto op = cli.Post("/entities/vehicle/body/bcm/operations/self_test", headers, "{}", "application/json");
    ASSERT_TRUE(op != nullptr);
    ASSERT_EQ(op->status, 200);
    ASSERT_EQ(json::parse(op->body)["status"].get<std::string>(), "completed");
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

    RUN_TEST(test_mock_adapter_faults_roundtrip);
    RUN_TEST(test_mock_adapter_data_read_write);
    RUN_TEST(test_mock_adapter_mode_and_operation);

    RUN_TEST(test_http_root_and_entities);
    RUN_TEST(test_http_unknown_entity_404);
    RUN_TEST(test_http_grouping_node_501);
    RUN_TEST(test_http_faults_read_and_clear);
    RUN_TEST(test_http_write_data_locked_without_header_423);
    RUN_TEST(test_http_lock_conflict_and_release);
    RUN_TEST(test_http_mode_and_operation);

    return testfw::summary();
}
