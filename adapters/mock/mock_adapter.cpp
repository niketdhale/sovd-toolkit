#include "mock_adapter.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// Internal C++ state, hidden behind the opaque sovd_adapter_ctx*.
struct MockState {
    std::mutex mtx;
    std::vector<sovd_fault_t> faults;
    std::unordered_map<std::string, std::vector<uint8_t>> data; // DID (upper hex, no 0x) -> bytes
    std::string mode = "default";
};

sovd_fault_t make_fault(const char *code, const char *status) {
    sovd_fault_t f{};
    std::strncpy(f.code, code, sizeof(f.code) - 1);
    std::strncpy(f.status, status, sizeof(f.status) - 1);
    return f;
}

std::string normalize_did(const char *did) {
    std::string s = did ? did : "";
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return out;
}

MockState *state(sovd_adapter_ctx *ctx) { return reinterpret_cast<MockState *>(ctx); }

sovd_adapter_ctx *mock_create(const char *config_json) {
    (void)config_json;
    auto *st = new MockState();
    st->faults.push_back(make_fault("P0A0F-16", "confirmed"));
    st->faults.push_back(make_fault("P0420-14", "pending"));
    // Seeds match the worked example in docs/DESIGN.md's "path mapping" section:
    // DID 010A = 0x32C8 -> 13000 raw, decoded (Phase 1) as 13.0V.
    st->data["F190"] = {'S', 'O', 'V', 'D', 'T', 'O', 'O', 'L', 'K', 'I', 'T', 'M', 'O', 'C', 'K', '0', '1'};
    st->data["010A"] = {0x32, 0xC8};
    st->data["0200"] = {0x01};
    // 0x0210 = courtesy_light_delay (catalogs/bcm.yaml, Task 7): 0x0A -> 10s.
    st->data["0210"] = {0x0A};
    return reinterpret_cast<sovd_adapter_ctx *>(st);
}

void mock_destroy(sovd_adapter_ctx *ctx) { delete state(ctx); }

sovd_result_t mock_read_faults(sovd_adapter_ctx *ctx, const char *entity_path,
                                sovd_fault_t **out_faults, size_t *out_count) {
    (void)entity_path;
    auto *st = state(ctx);
    std::lock_guard<std::mutex> lk(st->mtx);
    *out_count = st->faults.size();
    if (*out_count == 0) {
        *out_faults = nullptr;
        return SOVD_OK;
    }
    auto *arr = static_cast<sovd_fault_t *>(std::malloc(sizeof(sovd_fault_t) * *out_count));
    std::copy(st->faults.begin(), st->faults.end(), arr);
    *out_faults = arr;
    return SOVD_OK;
}

sovd_result_t mock_clear_faults(sovd_adapter_ctx *ctx, const char *entity_path) {
    (void)entity_path;
    auto *st = state(ctx);
    std::lock_guard<std::mutex> lk(st->mtx);
    st->faults.clear();
    return SOVD_OK;
}

sovd_result_t mock_read_data(sovd_adapter_ctx *ctx, const char *entity_path,
                              const char *did, sovd_buffer_t *out) {
    (void)entity_path;
    auto *st = state(ctx);
    std::lock_guard<std::mutex> lk(st->mtx);
    auto it = st->data.find(normalize_did(did));
    if (it == st->data.end()) return SOVD_NOT_FOUND;
    out->len = it->second.size();
    out->data = static_cast<uint8_t *>(std::malloc(out->len));
    std::copy(it->second.begin(), it->second.end(), out->data);
    return SOVD_OK;
}

sovd_result_t mock_write_data(sovd_adapter_ctx *ctx, const char *entity_path,
                               const char *did, const uint8_t *data, size_t len) {
    (void)entity_path;
    auto *st = state(ctx);
    std::lock_guard<std::mutex> lk(st->mtx);
    st->data[normalize_did(did)] = std::vector<uint8_t>(data, data + len);
    return SOVD_OK;
}

sovd_result_t mock_set_mode(sovd_adapter_ctx *ctx, const char *entity_path, const char *mode) {
    (void)entity_path;
    auto *st = state(ctx);
    std::lock_guard<std::mutex> lk(st->mtx);
    st->mode = mode ? mode : "default";
    return SOVD_OK;
}

sovd_result_t mock_execute_operation(sovd_adapter_ctx *ctx, const char *entity_path,
                                      const char *op, const char *params_json,
                                      char **out_result_json) {
    (void)entity_path;
    (void)params_json;
    (void)state(ctx);
    std::string result = std::string(R"({"operation":")") + (op ? op : "") + R"(","status":"completed"})";
    *out_result_json = static_cast<char *>(std::malloc(result.size() + 1));
    std::memcpy(*out_result_json, result.c_str(), result.size() + 1);
    return SOVD_OK;
}

const sovd_vtable_t g_mock_vtable = {
    mock_create,
    mock_destroy,
    mock_read_faults,
    mock_clear_faults,
    mock_read_data,
    mock_write_data,
    mock_set_mode,
    mock_execute_operation,
    sovd_default_free_faults,
    sovd_default_free_buffer,
    sovd_default_free_string,
    // capabilities: fully synchronous, no native batch read, no distinct
    // IOControl path — honest about what the mock actually does, not what
    // a real ECU backend eventually will.
    {false, false, false},
};

} // namespace

extern "C" const sovd_vtable_t *sovd_mock_adapter_vtable(void) { return &g_mock_vtable; }
