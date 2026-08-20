// Live socket tests for adapters/uds_doip, run only when
// -DSOVD_ADAPTER_UDS_DOIP=ON. Uses FakeDoipServer (fake_doip_server.hpp) —
// an in-repo test fixture, not the user's separate DoIP_ECU_Simulator repo.
//
// These use real timeouts (configured short on the transport side) rather
// than sleeping in the test itself — same spirit as LockManager's injectable
// clock: the thing under test does the waiting, not the test code.
#include <atomic>
#include <chrono>
#include <thread>

#include <cstdio>
#include <fstream>
#include <sstream>

#include "fake_doip_server.hpp"
#include "httplib.h"
#include "json.hpp"
#include "sovd/entity_registry.hpp"
#include "sovd/lock_manager.hpp"
#include "sovd/server/routes.hpp"
#include "sovd/uds_doip/doip_transport.hpp"
#include "sovd/uds_doip/nrc_map.hpp"
#include "sovd/uds_doip/session_manager.hpp"
#include "sovd/uds_doip/uds_doip_adapter.h"
#include "sovd/uds_doip/uds_services.hpp"
#include "test_framework.hpp"

using namespace sovd::uds_doip;
using namespace sovd::uds_doip::uds;
using sovd::uds_doip::test::FakeDoipServer;
using sovd::uds_doip::test::FaultMode;

// ---------------------------------------------------------------------
// session_manager: driven by an injected fake SendFn, no sockets needed.
// Its heartbeat/idle-timeout behavior is genuinely wall-clock-driven (a
// real background thread, not a lazily-checked TTL like LockManager), so
// unlike the rest of this project's tests, these use short real intervals
// with bounded polling rather than an injectable clock — see CLAUDE.md for
// why that tradeoff was made here specifically.
// ---------------------------------------------------------------------

namespace {

struct FakeEcu {
    std::atomic<int> session_control_calls{0};
    std::atomic<int> heartbeat_calls{0};
    std::atomic<bool> fail_next_session_control{false};

    bool send(const std::vector<uint8_t> &req, std::vector<uint8_t> &resp) {
        if (req.empty()) return false;
        if (req[0] == 0x10) { // DiagnosticSessionControl
            session_control_calls++;
            if (fail_next_session_control.exchange(false)) return false;
            uint8_t session_type = req.size() > 1 ? req[1] : 0x01;
            resp = {0x50, session_type, 0x00, 0x32, 0x01, 0xF4};
            return true;
        }
        if (req[0] == 0x3E) { // TesterPresent
            heartbeat_calls++;
            resp = {0x7E, 0x00};
            return true;
        }
        return false;
    }
};

template <typename Pred>
bool wait_until(Pred pred, int timeout_ms, int poll_ms = 5) {
    int waited = 0;
    while (waited < timeout_ms) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
        waited += poll_ms;
    }
    return pred();
}

} // namespace

void test_session_manager_escalate_and_noop_repeat() {
    FakeEcu ecu;
    SessionManagerConfig cfg;
    cfg.heartbeat_interval_ms = 200;
    cfg.idle_timeout_ms = 5000;
    SessionManager sm([&](const auto &req, auto &resp) { return ecu.send(req, resp); }, cfg);

    ASSERT_EQ(sm.current_session_type(), static_cast<uint8_t>(0x01));
    ASSERT_TRUE(sm.ensure_session(0x03));
    ASSERT_EQ(sm.current_session_type(), static_cast<uint8_t>(0x03));
    ASSERT_EQ(ecu.session_control_calls.load(), 1);

    ASSERT_TRUE(sm.ensure_session(0x03)); // already there -> no redundant request
    ASSERT_EQ(ecu.session_control_calls.load(), 1);
}

void test_session_manager_escalation_failure_leaves_default() {
    FakeEcu ecu;
    ecu.fail_next_session_control = true;
    SessionManager sm([&](const auto &req, auto &resp) { return ecu.send(req, resp); }, {});

    ASSERT_FALSE(sm.ensure_session(0x03));
    ASSERT_EQ(sm.current_session_type(), static_cast<uint8_t>(0x01));
}

void test_session_manager_revert_to_default() {
    FakeEcu ecu;
    SessionManagerConfig cfg;
    cfg.heartbeat_interval_ms = 200;
    cfg.idle_timeout_ms = 5000;
    SessionManager sm([&](const auto &req, auto &resp) { return ecu.send(req, resp); }, cfg);

    ASSERT_TRUE(sm.ensure_session(0x03));
    ASSERT_TRUE(sm.revert_to_default());
    ASSERT_EQ(sm.current_session_type(), static_cast<uint8_t>(0x01));
    ASSERT_EQ(ecu.session_control_calls.load(), 2); // escalate + revert
}

void test_session_manager_heartbeat_fires_while_escalated() {
    FakeEcu ecu;
    SessionManagerConfig cfg;
    cfg.heartbeat_interval_ms = 30;
    cfg.idle_timeout_ms = 5000;
    SessionManager sm([&](const auto &req, auto &resp) { return ecu.send(req, resp); }, cfg);

    ASSERT_TRUE(sm.ensure_session(0x03));
    ASSERT_TRUE(wait_until([&] { return ecu.heartbeat_calls.load() >= 2; }, 800));
}

void test_session_manager_idle_timeout_reverts_on_its_own() {
    FakeEcu ecu;
    SessionManagerConfig cfg;
    cfg.heartbeat_interval_ms = 20;
    cfg.idle_timeout_ms = 60;
    SessionManager sm([&](const auto &req, auto &resp) { return ecu.send(req, resp); }, cfg);

    ASSERT_TRUE(sm.ensure_session(0x03));
    ASSERT_TRUE(wait_until([&] { return sm.current_session_type() == 0x01; }, 1000));
}

// Regression test: escalating straight from one non-default session to
// another used to join() the still-running heartbeat thread while holding
// the same mutex that thread needs to acquire to notice the stop signal —
// a real deadlock this exercises directly (it would hang, not fail).
void test_session_manager_escalate_across_non_default_sessions_no_deadlock() {
    FakeEcu ecu;
    SessionManagerConfig cfg;
    cfg.heartbeat_interval_ms = 30;
    cfg.idle_timeout_ms = 5000;
    SessionManager sm([&](const auto &req, auto &resp) { return ecu.send(req, resp); }, cfg);

    ASSERT_TRUE(sm.ensure_session(0x03)); // extended
    ASSERT_TRUE(sm.ensure_session(0x02)); // programming, straight from extended
    ASSERT_EQ(sm.current_session_type(), static_cast<uint8_t>(0x02));
}

// ---------------------------------------------------------------------
// uds_services: pure encode/decode, no sockets
// ---------------------------------------------------------------------

void test_uds_read_data_by_identifier_roundtrip() {
    auto req = encode_read_data_by_identifier(0x010A);
    ASSERT_EQ(req.size(), static_cast<size_t>(3));
    ASSERT_EQ(req[0], 0x22);
    ASSERT_EQ(req[1], 0x01);
    ASSERT_EQ(req[2], 0x0A);

    std::vector<uint8_t> data;
    ASSERT_TRUE(decode_read_data_by_identifier({0x62, 0x01, 0x0A, 0x32, 0xC8}, 0x010A, data));
    ASSERT_EQ(data.size(), static_cast<size_t>(2));
    ASSERT_EQ(data[0], 0x32);
    ASSERT_EQ(data[1], 0xC8);

    // Wrong DID echoed back -> reject, even though the SID matches.
    ASSERT_FALSE(decode_read_data_by_identifier({0x62, 0x01, 0x0B, 0x32, 0xC8}, 0x010A, data));

    uint8_t nrc = 0;
    ASSERT_TRUE(is_negative_response({0x7F, 0x22, 0x31}, nrc));
    ASSERT_EQ(nrc, 0x31);
    ASSERT_FALSE(decode_read_data_by_identifier({0x7F, 0x22, 0x31}, 0x010A, data));
}

void test_uds_write_data_by_identifier_roundtrip() {
    auto req = encode_write_data_by_identifier(0x0200, {0x02});
    ASSERT_EQ(req.size(), static_cast<size_t>(4));
    ASSERT_EQ(req[0], 0x2E);
    ASSERT_EQ(req[3], 0x02);

    ASSERT_TRUE(decode_write_data_by_identifier({0x6E, 0x02, 0x00}, 0x0200));
    ASSERT_FALSE(decode_write_data_by_identifier({0x6E, 0x02, 0x01}, 0x0200));
}

void test_uds_io_control_roundtrip() {
    auto req = encode_io_control(0x0200, kIoControlShortTermAdjustment, {0x02});
    ASSERT_EQ(req.size(), static_cast<size_t>(5));
    ASSERT_EQ(req[0], 0x2F);
    ASSERT_EQ(req[3], kIoControlShortTermAdjustment);

    ASSERT_TRUE(decode_io_control({0x6F, 0x02, 0x00, 0x03}, 0x0200));
}

void test_uds_read_dtc_by_status_mask_decodes_multiple_entries() {
    auto req = encode_read_dtc_by_status_mask();
    ASSERT_EQ(req, (std::vector<uint8_t>{0x19, 0x02, 0xFF}));

    // Two DTCs: one confirmed, one pending. b0's top 2 bits = category (00=P),
    // next 2 bits + low nibble form the display digits.
    std::vector<uint8_t> response = {
        0x59, 0x02, 0xFF,             // SID+0x40, sub, availability mask
        0x0A, 0x0F, 0x16, 0x08,       // DTC bytes -> "P0A0F-16", status 0x08 = confirmed
        0x04, 0x20, 0x14, 0x04,       // DTC bytes -> "P0420-14", status 0x04 = pending
    };
    std::vector<DtcEntry> entries;
    ASSERT_TRUE(decode_read_dtc_by_status_mask(response, entries));
    ASSERT_EQ(entries.size(), static_cast<size_t>(2));
    ASSERT_EQ(entries[0].code, "P0A0F-16");
    ASSERT_EQ(entries[0].status, "confirmed");
    ASSERT_EQ(entries[1].code, "P0420-14");
    ASSERT_EQ(entries[1].status, "pending");
}

void test_uds_clear_diagnostic_information() {
    auto req = encode_clear_diagnostic_information();
    ASSERT_EQ(req, (std::vector<uint8_t>{0x14, 0xFF, 0xFF, 0xFF}));
    ASSERT_TRUE(decode_clear_diagnostic_information({0x54}));
    ASSERT_FALSE(decode_clear_diagnostic_information({0x7F, 0x14, 0x22}));
}

void test_uds_diagnostic_session_control_roundtrip() {
    uint8_t type = 0;
    ASSERT_TRUE(session_name_to_type("extended", type));
    ASSERT_EQ(type, static_cast<uint8_t>(0x03));
    ASSERT_TRUE(session_name_to_type("default", type));
    ASSERT_EQ(type, static_cast<uint8_t>(0x01));
    ASSERT_FALSE(session_name_to_type("bogus", type));

    auto req = encode_diagnostic_session_control(0x03);
    ASSERT_EQ(req, (std::vector<uint8_t>{0x10, 0x03}));
    ASSERT_TRUE(decode_diagnostic_session_control({0x50, 0x03, 0x00, 0x32, 0x01, 0xF4}, 0x03));
    ASSERT_FALSE(decode_diagnostic_session_control({0x50, 0x01, 0x00, 0x32, 0x01, 0xF4}, 0x03));
}

void test_uds_routine_control_roundtrip() {
    auto req = encode_routine_control(RoutineSubfunction::Start, 0x0203);
    ASSERT_EQ(req, (std::vector<uint8_t>{0x31, 0x01, 0x02, 0x03}));

    std::vector<uint8_t> result;
    ASSERT_TRUE(decode_routine_control({0x71, 0x01, 0x02, 0x03, 0x00}, 0x0203, result));
    ASSERT_EQ(result.size(), static_cast<size_t>(1));
    ASSERT_FALSE(decode_routine_control({0x71, 0x01, 0x02, 0x04, 0x00}, 0x0203, result));
}

void test_uds_tester_present_roundtrip() {
    ASSERT_EQ(encode_tester_present(), (std::vector<uint8_t>{0x3E, 0x00}));
    ASSERT_TRUE(decode_tester_present({0x7E, 0x00}));
    ASSERT_FALSE(decode_tester_present({0x7E, 0x01}));
}

void test_uds_security_access_seed_key_roundtrip() {
    auto seed_req = encode_security_access_request_seed(0x01);
    ASSERT_EQ(seed_req, (std::vector<uint8_t>{0x27, 0x01}));

    std::vector<uint8_t> seed;
    ASSERT_TRUE(decode_security_access_seed({0x67, 0x01, 0x12, 0x34}, 0x01, seed));
    ASSERT_EQ(seed.size(), static_cast<size_t>(2));

    auto key = derive_key_DEMO_ONLY_NOT_SECURE(seed, 0x01);
    ASSERT_EQ(key.size(), seed.size());

    auto key_req = encode_security_access_send_key(0x01, key);
    ASSERT_EQ(key_req[0], 0x27);
    ASSERT_EQ(key_req[1], 0x02); // level + 1

    ASSERT_TRUE(decode_security_access_key_accepted({0x67, 0x02}, 0x01));
    ASSERT_FALSE(decode_security_access_key_accepted({0x7F, 0x27, 0x35}, 0x01)); // invalidKey
}

namespace {

TransportConfig fast_config(int port) {
    TransportConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = static_cast<uint16_t>(port);
    cfg.tester_logical_address = 0x0E00;
    cfg.connect_timeout_ms = 500;
    cfg.read_timeout_ms = 300;
    return cfg;
}

} // namespace

void test_transport_connect_and_routing_activation() {
    FakeDoipServer server;
    DoipTransport transport(fast_config(server.port()));

    TransportError err = TransportError::None;
    ASSERT_TRUE(transport.connect(&err));
    ASSERT_TRUE(transport.is_connected());
    ASSERT_TRUE(err == TransportError::None);
}

void test_transport_routing_activation_denied() {
    FakeDoipServer server;
    server.set_routing_activation_response_code(0x00); // "unknown source address"
    DoipTransport transport(fast_config(server.port()));

    TransportError err = TransportError::None;
    ASSERT_FALSE(transport.connect(&err));
    ASSERT_TRUE(err == TransportError::RoutingActivationFailed);
    ASSERT_FALSE(transport.is_connected());
}

void test_transport_read_data_by_identifier_roundtrip() {
    FakeDoipServer server;
    // 22 01 0A -> 62 01 0A 32 C8 (matches the CLAUDE.md worked example bytes)
    server.set_response({0x22, 0x01, 0x0A}, {0x62, 0x01, 0x0A, 0x32, 0xC8});

    DoipTransport transport(fast_config(server.port()));
    ASSERT_TRUE(transport.connect());

    std::vector<uint8_t> response;
    TransportError err = TransportError::None;
    ASSERT_TRUE(transport.send_and_receive(0x0E80, {0x22, 0x01, 0x0A}, response, &err));
    ASSERT_TRUE(err == TransportError::None);
    ASSERT_EQ(response.size(), static_cast<size_t>(5));
    ASSERT_EQ(response[0], 0x62);
    ASSERT_EQ(response[3], 0x32);
    ASSERT_EQ(response[4], 0xC8);
}

void test_transport_unconfigured_request_gets_negative_response() {
    FakeDoipServer server; // no responses configured
    DoipTransport transport(fast_config(server.port()));
    ASSERT_TRUE(transport.connect());

    std::vector<uint8_t> response;
    ASSERT_TRUE(transport.send_and_receive(0x0E80, {0x22, 0xFF, 0xFF}, response));
    ASSERT_EQ(response.size(), static_cast<size_t>(3));
    ASSERT_EQ(response[0], 0x7F); // negative response marker
    ASSERT_EQ(response[2], 0x11); // serviceNotSupported fallback
}

void test_transport_no_response_times_out() {
    FakeDoipServer server;
    server.set_fault_mode(FaultMode::NoResponse);
    DoipTransport transport(fast_config(server.port()));
    ASSERT_TRUE(transport.connect());

    std::vector<uint8_t> response;
    TransportError err = TransportError::None;
    ASSERT_FALSE(transport.send_and_receive(0x0E80, {0x22, 0x01, 0x0A}, response, &err));
    ASSERT_TRUE(err == TransportError::Timeout);
}

void test_transport_malformed_frame_detected() {
    FakeDoipServer server;
    server.set_fault_mode(FaultMode::MalformedFrame);
    DoipTransport transport(fast_config(server.port()));
    ASSERT_TRUE(transport.connect());

    std::vector<uint8_t> response;
    TransportError err = TransportError::None;
    ASSERT_FALSE(transport.send_and_receive(0x0E80, {0x22, 0x01, 0x0A}, response, &err));
    ASSERT_TRUE(err == TransportError::MalformedFrame);
}

void test_transport_truncated_frame_detected() {
    FakeDoipServer server;
    server.set_fault_mode(FaultMode::TruncatedFrame);
    DoipTransport transport(fast_config(server.port()));
    ASSERT_TRUE(transport.connect());

    std::vector<uint8_t> response;
    TransportError err = TransportError::None;
    ASSERT_FALSE(transport.send_and_receive(0x0E80, {0x22, 0x01, 0x0A}, response, &err));
    // Header claims 20 bytes, only 3 arrive before the connection closes ->
    // the payload read either times out or the socket closes first,
    // depending on scheduling; both are legitimate transport-level failures.
    ASSERT_TRUE(err == TransportError::Timeout || err == TransportError::ConnectionClosed);
}

void test_transport_nrc_storm() {
    FakeDoipServer server;
    server.set_fault_mode(FaultMode::NrcStorm, 0x31); // requestOutOfRange
    DoipTransport transport(fast_config(server.port()));
    ASSERT_TRUE(transport.connect());

    std::vector<uint8_t> response;
    ASSERT_TRUE(transport.send_and_receive(0x0E80, {0x2E, 0x01, 0x0A, 0x00}, response));
    ASSERT_EQ(response.size(), static_cast<size_t>(3));
    ASSERT_EQ(response[0], 0x7F);
    ASSERT_EQ(response[2], 0x31);
}

void test_transport_response_pending_then_receive_pending_response() {
    FakeDoipServer server;
    server.set_response({0x22, 0x01, 0x0A}, {0x62, 0x01, 0x0A, 0x32, 0xC8});
    server.set_fault_mode(FaultMode::ResponsePendingStorm, 0x00, 3); // 3x 0x78 then the real response

    DoipTransport transport(fast_config(server.port()));
    ASSERT_TRUE(transport.connect());

    std::vector<uint8_t> response;
    ASSERT_TRUE(transport.send_and_receive(0x0E80, {0x22, 0x01, 0x0A}, response));
    ASSERT_EQ(response.size(), static_cast<size_t>(3));
    ASSERT_EQ(response[0], 0x7F);
    ASSERT_EQ(response[2], 0x78); // first response is pending

    // Caller (adapter layer, in real use) loops on receive_pending_response
    // until it gets something other than 0x78.
    int guard = 0;
    while (response.size() == 3 && response[0] == 0x7F && response[2] == 0x78 && guard < 10) {
        ASSERT_TRUE(transport.receive_pending_response(0x0E80, response));
        ++guard;
    }
    ASSERT_EQ(response.size(), static_cast<size_t>(5));
    ASSERT_EQ(response[0], 0x62);
}

void test_nrc_map_matches_claude_md_table() {
    ASSERT_TRUE(nrc_to_sovd_result(0x33) == SOVD_FORBIDDEN);   // securityAccessDenied -> 403
    ASSERT_TRUE(nrc_to_sovd_result(0x31) == SOVD_BAD_REQUEST); // requestOutOfRange -> 400
    ASSERT_TRUE(nrc_to_sovd_result(0x22) == SOVD_CONFLICT);    // conditionsNotCorrect -> 409
}

void test_nrc_map_exhaustive() {
    ASSERT_TRUE(nrc_to_sovd_result(0x10) == SOVD_INTERNAL);
    ASSERT_TRUE(nrc_to_sovd_result(0x11) == SOVD_UNSUPPORTED);
    ASSERT_TRUE(nrc_to_sovd_result(0x12) == SOVD_UNSUPPORTED);
    ASSERT_TRUE(nrc_to_sovd_result(0x13) == SOVD_BAD_REQUEST);
    ASSERT_TRUE(nrc_to_sovd_result(0x14) == SOVD_INTERNAL);
    ASSERT_TRUE(nrc_to_sovd_result(0x21) == SOVD_BUSY);
    ASSERT_TRUE(nrc_to_sovd_result(0x24) == SOVD_CONFLICT);
    ASSERT_TRUE(nrc_to_sovd_result(0x35) == SOVD_FORBIDDEN);
    ASSERT_TRUE(nrc_to_sovd_result(0x36) == SOVD_BUSY);
    ASSERT_TRUE(nrc_to_sovd_result(0x37) == SOVD_BUSY);
    ASSERT_TRUE(nrc_to_sovd_result(0x70) == SOVD_UNSUPPORTED);
    ASSERT_TRUE(nrc_to_sovd_result(0x78) == SOVD_BUSY); // shouldn't leak through in practice, safe fallback
    ASSERT_TRUE(nrc_to_sovd_result(0x7E) == SOVD_UNSUPPORTED);
    ASSERT_TRUE(nrc_to_sovd_result(0x7F) == SOVD_UNSUPPORTED);
    ASSERT_TRUE(nrc_to_sovd_result(0xAB) == SOVD_INTERNAL); // unenumerated NRC -> safe default
}

// ---------------------------------------------------------------------
// uds_doip_adapter: full sovd_vtable_t stack against the fake server.
// ---------------------------------------------------------------------

namespace {

// A DID requiring extended session (write path exercises session
// escalation), an io_control DID (write path exercises 0x2F vs 0x2E
// dispatch), and an operation — enough to cover every vtable entry point
// without depending on the real catalogs/bcm.yaml Phase 1 owns.
const char *kAdapterTestCatalogYaml = R"(
data:
  - id: needs_extended
    did: 0x0300
    type: raw
    access: read_write
    requires_session: extended
  - id: actuator
    did: 0x0400
    type: raw
    access: read_write
    io_control: true
operations:
  - id: self_test
    routine_id: 0x0203
)";

std::string write_temp_catalog() {
    char path_template[] = "/tmp/sovd_uds_doip_test_catalog_XXXXXX";
    int fd = mkstemp(path_template);
    ASSERT_TRUE(fd >= 0);
    std::string path(path_template);
    std::ofstream out(path);
    out << kAdapterTestCatalogYaml;
    out.close();
    ::close(fd);
    return path;
}

std::string adapter_config_json(int port, const std::string &catalog_path, int heartbeat_ms = 2000,
                                 int idle_timeout_ms = 6000) {
    std::ostringstream oss;
    oss << R"({"gateway_ip":"127.0.0.1","logical_address":"0x0E80","port":)" << port
        << R"(,"connect_timeout_ms":500,"read_timeout_ms":300)"
        << R"(,"heartbeat_interval_ms":)" << heartbeat_ms << R"(,"session_idle_timeout_ms":)" << idle_timeout_ms;
    if (!catalog_path.empty()) oss << R"(,"did_catalog":")" << catalog_path << R"(")";
    oss << "}";
    return oss.str();
}

} // namespace

void test_adapter_create_rejects_bad_config() {
    const sovd_vtable_t *v = sovd_uds_doip_adapter_vtable();
    ASSERT_TRUE(v->create("{}") == nullptr);                              // missing required fields
    ASSERT_TRUE(v->create("not json") == nullptr);
    ASSERT_TRUE(v->create(R"({"gateway_ip":"127.0.0.1"})") == nullptr);   // missing logical_address
}

void test_adapter_capabilities_are_honest() {
    const sovd_vtable_t *v = sovd_uds_doip_adapter_vtable();
    ASSERT_FALSE(v->capabilities.supports_batch_read);
    ASSERT_FALSE(v->capabilities.supports_async_operations);
    ASSERT_TRUE(v->capabilities.supports_io_control);
}

void test_adapter_read_data_roundtrip() {
    FakeDoipServer server;
    server.set_response({0x22, 0x01, 0x0A}, {0x62, 0x01, 0x0A, 0x32, 0xC8});

    const sovd_vtable_t *v = sovd_uds_doip_adapter_vtable();
    auto *ctx = v->create(adapter_config_json(server.port(), "").c_str());
    ASSERT_TRUE(ctx != nullptr);

    sovd_buffer_t buf{};
    ASSERT_TRUE(v->read_data(ctx, "vehicle/body/bcm", "010A", &buf) == SOVD_OK);
    ASSERT_EQ(buf.len, static_cast<size_t>(2));
    ASSERT_EQ(buf.data[0], 0x32);
    ASSERT_EQ(buf.data[1], 0xC8);
    v->free_buffer(&buf);

    v->destroy(ctx);
}

void test_adapter_read_data_maps_nrc_to_sovd_result() {
    FakeDoipServer server; // no response configured for this DID
    const sovd_vtable_t *v = sovd_uds_doip_adapter_vtable();
    auto *ctx = v->create(adapter_config_json(server.port(), "").c_str());

    sovd_buffer_t buf{};
    // Unconfigured request -> fake server's default fallback: 0x7F <sid> 0x11
    // (serviceNotSupported) -> SOVD_UNSUPPORTED.
    ASSERT_TRUE(v->read_data(ctx, "vehicle/body/bcm", "FFFF", &buf) == SOVD_UNSUPPORTED);

    v->destroy(ctx);
}

void test_adapter_read_data_transport_failure() {
    const sovd_vtable_t *v = sovd_uds_doip_adapter_vtable();
    // Nothing listening on this port.
    auto *ctx = v->create(adapter_config_json(1, "").c_str());
    ASSERT_TRUE(ctx != nullptr); // create() doesn't connect eagerly

    sovd_buffer_t buf{};
    ASSERT_TRUE(v->read_data(ctx, "vehicle/body/bcm", "010A", &buf) == SOVD_TRANSPORT);

    v->destroy(ctx);
}

void test_adapter_faults_read_and_clear() {
    FakeDoipServer server;
    server.set_response({0x19, 0x02, 0xFF}, {0x59, 0x02, 0xFF, 0x0A, 0x0F, 0x16, 0x08});
    server.set_response({0x14, 0xFF, 0xFF, 0xFF}, {0x54});

    const sovd_vtable_t *v = sovd_uds_doip_adapter_vtable();
    auto *ctx = v->create(adapter_config_json(server.port(), "").c_str());

    sovd_fault_t *faults = nullptr;
    size_t count = 0;
    ASSERT_TRUE(v->read_faults(ctx, "vehicle/body/bcm", &faults, &count) == SOVD_OK);
    ASSERT_EQ(count, static_cast<size_t>(1));
    ASSERT_EQ(std::string(faults[0].code), "P0A0F-16");
    ASSERT_EQ(std::string(faults[0].status), "confirmed");
    v->free_faults(faults, count);

    ASSERT_TRUE(v->clear_faults(ctx, "vehicle/body/bcm") == SOVD_OK);

    v->destroy(ctx);
}

void test_adapter_write_data_escalates_session_from_catalog() {
    std::string catalog_path = write_temp_catalog();
    FakeDoipServer server;
    server.set_response({0x10, 0x03}, {0x50, 0x03, 0x00, 0x32, 0x01, 0xF4}); // escalate to extended
    server.set_response({0x2E, 0x03, 0x00, 0x01}, {0x6E, 0x03, 0x00});      // write needs_extended = 0x01

    const sovd_vtable_t *v = sovd_uds_doip_adapter_vtable();
    auto *ctx = v->create(adapter_config_json(server.port(), catalog_path).c_str());
    ASSERT_TRUE(ctx != nullptr);

    uint8_t value = 0x01;
    ASSERT_TRUE(v->write_data(ctx, "vehicle/body/bcm", "0300", &value, 1) == SOVD_OK);

    v->destroy(ctx);
    std::remove(catalog_path.c_str());
}

void test_adapter_write_data_fails_when_escalation_rejected() {
    std::string catalog_path = write_temp_catalog();
    FakeDoipServer server; // no response configured for 0x10 -> default negative fallback
    const sovd_vtable_t *v = sovd_uds_doip_adapter_vtable();
    auto *ctx = v->create(adapter_config_json(server.port(), catalog_path).c_str());

    uint8_t value = 0x01;
    ASSERT_TRUE(v->write_data(ctx, "vehicle/body/bcm", "0300", &value, 1) == SOVD_TRANSPORT);

    v->destroy(ctx);
    std::remove(catalog_path.c_str());
}

void test_adapter_write_data_uses_io_control_per_catalog() {
    std::string catalog_path = write_temp_catalog();
    FakeDoipServer server;
    // 0x0400 is catalog io_control:true -> expect 0x2F (IOControl), not 0x2E.
    server.set_response({0x2F, 0x04, 0x00, uds::kIoControlShortTermAdjustment, 0x07},
                         {0x6F, 0x04, 0x00, uds::kIoControlShortTermAdjustment});

    const sovd_vtable_t *v = sovd_uds_doip_adapter_vtable();
    auto *ctx = v->create(adapter_config_json(server.port(), catalog_path).c_str());

    uint8_t value = 0x07;
    ASSERT_TRUE(v->write_data(ctx, "vehicle/body/bcm", "0400", &value, 1) == SOVD_OK);

    v->destroy(ctx);
    std::remove(catalog_path.c_str());
}

void test_adapter_set_mode_extended_then_default() {
    FakeDoipServer server;
    server.set_response({0x10, 0x03}, {0x50, 0x03, 0x00, 0x32, 0x01, 0xF4});
    server.set_response({0x10, 0x01}, {0x50, 0x01, 0x00, 0x32, 0x01, 0xF4});

    const sovd_vtable_t *v = sovd_uds_doip_adapter_vtable();
    auto *ctx = v->create(adapter_config_json(server.port(), "").c_str());

    ASSERT_TRUE(v->set_mode(ctx, "vehicle/body/bcm", "extended") == SOVD_OK);
    ASSERT_TRUE(v->set_mode(ctx, "vehicle/body/bcm", "default") == SOVD_OK);
    ASSERT_TRUE(v->set_mode(ctx, "vehicle/body/bcm", "not_a_real_session") == SOVD_BAD_REQUEST);

    v->destroy(ctx);
}

void test_adapter_execute_operation_roundtrip() {
    std::string catalog_path = write_temp_catalog();
    FakeDoipServer server;
    server.set_response({0x31, 0x01, 0x02, 0x03}, {0x71, 0x01, 0x02, 0x03});

    const sovd_vtable_t *v = sovd_uds_doip_adapter_vtable();
    auto *ctx = v->create(adapter_config_json(server.port(), catalog_path).c_str());

    char *result = nullptr;
    ASSERT_TRUE(v->execute_operation(ctx, "vehicle/body/bcm", "self_test", "{}", &result) == SOVD_OK);
    ASSERT_TRUE(result != nullptr);
    ASSERT_TRUE(std::string(result).find("self_test") != std::string::npos);
    v->free_string(result);

    ASSERT_TRUE(v->execute_operation(ctx, "vehicle/body/bcm", "no_such_op", "{}", &result) == SOVD_NOT_FOUND);

    v->destroy(ctx);
    std::remove(catalog_path.c_str());
}

// ---------------------------------------------------------------------
// End to end: real HTTP -> server/routes.cpp -> the real uds_doip adapter
// -> FakeDoipServer. Ties Phase 0 (routes/locks), Phase 1 (catalog/named
// paths), and Phase 2 (this adapter) together through actual production
// code, not mock — the strongest evidence any of this actually works as a
// system rather than as isolated units.
// ---------------------------------------------------------------------

void test_end_to_end_http_through_real_adapter() {
    using json = nlohmann::json;

    FakeDoipServer server;
    // battery_voltage: matches CLAUDE.md's worked example exactly.
    server.set_response({0x22, 0x01, 0x0A}, {0x62, 0x01, 0x0A, 0x32, 0xC8});
    // door_lock_state is io_control:true in catalogs/bcm.yaml -> 0x2F.
    server.set_response({0x2F, 0x02, 0x00, uds::kIoControlShortTermAdjustment, 0x02},
                         {0x6F, 0x02, 0x00, uds::kIoControlShortTermAdjustment});
    // self_test requires_session: extended in catalogs/bcm.yaml.
    server.set_response({0x10, 0x03}, {0x50, 0x03, 0x00, 0x32, 0x01, 0xF4});
    server.set_response({0x31, 0x01, 0x02, 0x03}, {0x71, 0x01, 0x02, 0x03});
    // Lock release drives set_mode("default") -> DiagnosticSessionControl 0x01.
    server.set_response({0x10, 0x01}, {0x50, 0x01, 0x00, 0x32, 0x01, 0xF4});

    std::string catalog_path = std::string(SOVD_SOURCE_DIR) + "/catalogs/bcm.yaml";

    sovd::EntityRegistry registry;
    sovd::LockManager locks;
    registry.add_entity("vehicle", sovd::EntityType::Vehicle);
    registry.add_entity("vehicle/body", sovd::EntityType::Area);

    const sovd_vtable_t *v = sovd_uds_doip_adapter_vtable();
    auto *ctx = v->create(adapter_config_json(server.port(), catalog_path).c_str());
    ASSERT_TRUE(ctx != nullptr);
    registry.add_entity("vehicle/body/bcm", sovd::EntityType::Component, v, ctx);

    sovd::server::Router router(registry, locks, "test-server", "domain");
    httplib::Server svr;
    router.register_routes(svr);
    router.attach_catalog("vehicle/body/bcm", sovd::catalog::Catalog::load_from_file(catalog_path));

    int port = svr.bind_to_any_port("127.0.0.1");
    std::thread server_thread([&] { svr.listen_after_bind(); });
    svr.wait_until_ready();

    httplib::Client cli("127.0.0.1", port);

    // (a) named GET, decoded, through the real adapter — the Phase 1 exit
    // criteria's worked example, now end to end through real UDS/DoIP code.
    auto voltage = cli.Get("/v1/entities/vehicle/body/bcm/data/battery_voltage");
    ASSERT_TRUE(voltage != nullptr);
    ASSERT_EQ(voltage->status, 200);
    json v_body = json::parse(voltage->body);
    ASSERT_TRUE(std::abs(v_body["value"].get<double>() - 13.0) < 1e-9);
    ASSERT_EQ(v_body["unit"].get<std::string>(), "V");

    // (b)-(e): lock -> write (io_control) -> operation (session escalation)
    // -> release (session teardown), all over real HTTP.
    auto lock_res = cli.Post("/v1/entities/vehicle/body/bcm/locks", "{}", "application/json");
    ASSERT_TRUE(lock_res != nullptr);
    ASSERT_EQ(lock_res->status, 201);
    std::string lock_id = json::parse(lock_res->body)["lock_id"].get<std::string>();
    httplib::Headers headers = {{"X-SOVD-Lock-Id", lock_id}};

    // B1 (Phase 7 blocker): named ids now take a typed value (the enum
    // label), not hex -- "deadlocked" is catalogs/bcm.yaml's label for 2,
    // same raw value "02" used to write directly.
    auto write_res = cli.Put("/v1/entities/vehicle/body/bcm/data/door_lock_state", headers,
                              R"({"value":"deadlocked"})", "application/json");
    ASSERT_TRUE(write_res != nullptr);
    ASSERT_EQ(write_res->status, 204);

    auto op_res = cli.Post("/v1/entities/vehicle/body/bcm/operations/self_test", headers, "{}", "application/json");
    ASSERT_TRUE(op_res != nullptr);
    ASSERT_EQ(op_res->status, 200);

    auto release_res = cli.Delete(("/v1/entities/vehicle/body/bcm/locks/" + lock_id).c_str());
    ASSERT_TRUE(release_res != nullptr);
    ASSERT_EQ(release_res->status, 204);

    svr.stop();
    server_thread.join();
}

int main() {
    RUN_TEST(test_end_to_end_http_through_real_adapter);

    RUN_TEST(test_adapter_create_rejects_bad_config);
    RUN_TEST(test_adapter_capabilities_are_honest);
    RUN_TEST(test_adapter_read_data_roundtrip);
    RUN_TEST(test_adapter_read_data_maps_nrc_to_sovd_result);
    RUN_TEST(test_adapter_read_data_transport_failure);
    RUN_TEST(test_adapter_faults_read_and_clear);
    RUN_TEST(test_adapter_write_data_escalates_session_from_catalog);
    RUN_TEST(test_adapter_write_data_fails_when_escalation_rejected);
    RUN_TEST(test_adapter_write_data_uses_io_control_per_catalog);
    RUN_TEST(test_adapter_set_mode_extended_then_default);
    RUN_TEST(test_adapter_execute_operation_roundtrip);

    RUN_TEST(test_session_manager_escalate_and_noop_repeat);
    RUN_TEST(test_session_manager_escalation_failure_leaves_default);
    RUN_TEST(test_session_manager_revert_to_default);
    RUN_TEST(test_session_manager_heartbeat_fires_while_escalated);
    RUN_TEST(test_session_manager_idle_timeout_reverts_on_its_own);
    RUN_TEST(test_session_manager_escalate_across_non_default_sessions_no_deadlock);

    RUN_TEST(test_nrc_map_matches_claude_md_table);
    RUN_TEST(test_nrc_map_exhaustive);

    RUN_TEST(test_uds_read_data_by_identifier_roundtrip);
    RUN_TEST(test_uds_write_data_by_identifier_roundtrip);
    RUN_TEST(test_uds_io_control_roundtrip);
    RUN_TEST(test_uds_read_dtc_by_status_mask_decodes_multiple_entries);
    RUN_TEST(test_uds_clear_diagnostic_information);
    RUN_TEST(test_uds_diagnostic_session_control_roundtrip);
    RUN_TEST(test_uds_routine_control_roundtrip);
    RUN_TEST(test_uds_tester_present_roundtrip);
    RUN_TEST(test_uds_security_access_seed_key_roundtrip);

    RUN_TEST(test_transport_connect_and_routing_activation);
    RUN_TEST(test_transport_routing_activation_denied);
    RUN_TEST(test_transport_read_data_by_identifier_roundtrip);
    RUN_TEST(test_transport_unconfigured_request_gets_negative_response);
    RUN_TEST(test_transport_no_response_times_out);
    RUN_TEST(test_transport_malformed_frame_detected);
    RUN_TEST(test_transport_truncated_frame_detected);
    RUN_TEST(test_transport_nrc_storm);
    RUN_TEST(test_transport_response_pending_then_receive_pending_response);

    return testfw::summary();
}
