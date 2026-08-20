// Phase 5: typed SDK over every SOVD resource. One deliberate simplification
// vs. a fully hand-rolled type per field: a data item's decoded `value` is
// carried as nlohmann::json rather than a custom variant -- the wire format
// is already JSON and every other module in this repo (routes.cpp included)
// treats it the same way, so a bespoke variant would just be a second
// encoding of the same three cases (string/number/absent-on-error) for no
// real type safety gain. "Typed wrappers over every resource" is about the
// *resource surface* (one method per SOVD operation, structured results,
// not hand-building JSON per call site) -- not about eliminating json::json
// from every leaf field.
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "httplib.h"
#include "json.hpp"

namespace sovd::client {

// Thrown by every typed wrapper on a non-2xx response (after any retry is
// exhausted) or a transport failure (no response at all -- unreachable
// server, timeout). status == 0 means transport failure, not a server error
// code -- callers that care about the distinction check that first.
class SovdError : public std::runtime_error {
public:
    SovdError(int status, std::string code, std::string message)
        : std::runtime_error(code + ": " + message), status(status), code(std::move(code)),
          message(std::move(message)) {}
    int status;
    std::string code;
    std::string message;
};

struct EntityInfo {
    std::string path;
    std::string type;
    bool has_backend = false;
};

struct FaultInfo {
    std::string code;
    std::string status;
};

struct DataItemDoc {
    std::string id;
    std::string did;
    std::string type; // "string" | "float" | "enum" | "raw"
    std::string access; // "read" | "write" | "read_write"
    bool io_control = false;
    std::optional<std::string> requires_session;
    std::optional<int> length; // type == string
    std::optional<double> scale; // type == float
    std::optional<std::string> unit; // type == float
    nlohmann::json values; // type == enum: {"0": "unlocked", ...}, else null
};

struct OperationDoc {
    std::string id;
    std::string routine_id;
    bool async = false;
    std::optional<std::string> requires_session;
};

struct Capabilities {
    bool supports_batch_read = false;
    bool supports_async_operations = false;
    bool supports_io_control = false;
};

struct DocsResult {
    std::string path;
    std::string type;
    bool has_backend = false;
    std::optional<Capabilities> capabilities;
    std::vector<DataItemDoc> data;
    std::vector<OperationDoc> operations;
};

// One entry from a single or batch data read. `error` is set (and `value`
// empty) for the per-item failure case a batch read can return without
// failing the whole batch (Phase 1).
struct DataValue {
    std::string id;
    std::optional<std::string> did;
    nlohmann::json value;
    std::optional<std::string> unit;
    std::optional<std::string> error;
    std::optional<std::string> error_message;
};

// Retry/backoff on 503/504 only -- explicitly never on 423 (CLAUDE.md:
// "retrying a lock conflict hammers another tester"). Defaults match the
// client config schema documented in CLAUDE.md.
struct RetryPolicy {
    int max_retries = 3;
    int backoff_ms = 200; // doubles each retry
};

class SovdClient {
public:
    explicit SovdClient(std::string base_url, RetryPolicy retry = {});

    std::vector<EntityInfo> list_entities();
    DocsResult get_docs(const std::string &path);

    std::vector<FaultInfo> get_faults(const std::string &path, const std::string &status_filter = "");
    void clear_faults(const std::string &path, const std::string &lock_id);

    DataValue get_data(const std::string &path, const std::string &id);
    std::vector<DataValue> get_data_batch(const std::string &path, const std::vector<std::string> &ids);
    void put_data(const std::string &path, const std::string &id, const std::string &hex_value,
                  const std::string &lock_id);

    void set_mode(const std::string &path, const std::string &mode, const std::string &lock_id);
    nlohmann::json execute_operation(const std::string &path, const std::string &op, const std::string &params_json,
                                      const std::string &lock_id);

    // Low-level lock primitives -- LockGuard (lock_guard.hpp) is the
    // intended way to use these; call directly only for scripts that want
    // manual control.
    std::string acquire_lock(const std::string &path, int ttl_seconds);
    void renew_lock(const std::string &path, const std::string &lock_id, int ttl_seconds);
    void release_lock(const std::string &path, const std::string &lock_id);

private:
    nlohmann::json request(const std::string &method, const std::string &url_path, const std::string &body = "",
                            const std::string &lock_id = "");

    httplib::Client cli_;
    RetryPolicy retry_;
};

} // namespace sovd::client
