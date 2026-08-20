// session_manager — tracks whether the ONE ECU a uds_doip adapter instance
// talks to is in its default UDS session or an escalated one, keeping an
// escalated session alive with a 0x3E TesterPresent heartbeat. One adapter
// instance = one entity = one ECU (each gets its own `adapter:` config with
// its own logical_address), so this deliberately does not track multiple
// ECUs — no map, just one piece of session state.
//
// See CLAUDE.md's "Session manager ownership" for the full reasoning this
// implements: escalation only ever happens from a lock-gated call
// (ensure_session), teardown is driven by set_mode("default") calling
// revert_to_default(), and a silently expired SOVD lock (no explicit
// DELETE /locks) is covered by this class's own idle timeout rather than
// new cross-module plumbing back to LockManager.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace sovd::uds_doip {

struct SessionManagerConfig {
    int heartbeat_interval_ms = 2000; // 0x3E cadence while escalated
    int idle_timeout_ms = 6000;       // revert on its own if untouched this long
};

class SessionManager {
public:
    // How to actually perform one UDS request/response over the transport.
    // Injected so this class has zero transport/socket knowledge of its
    // own — the adapter wires this to something that also serializes
    // access to the (not-thread-safe-by-itself) DoipTransport, so the
    // heartbeat thread and foreground adapter calls never race on the
    // socket. Returns false on any transport-level failure.
    using SendFn = std::function<bool(const std::vector<uint8_t> &request, std::vector<uint8_t> &response)>;

    explicit SessionManager(SendFn send, SessionManagerConfig config = {});
    ~SessionManager();

    SessionManager(const SessionManager &) = delete;
    SessionManager &operator=(const SessionManager &) = delete;

    // Ensures the given UDS session type (e.g. 0x03 extended) is active,
    // escalating via DiagnosticSessionControl and starting the heartbeat if
    // it wasn't already. A no-op returning true if already in that session.
    // Returns false only if the escalation request itself failed (caller
    // treats that like any other failed UDS request).
    bool ensure_session(uint8_t session_type);

    // Reverts to the default session (0x01) and stops the heartbeat.
    // Always updates local bookkeeping and stops the heartbeat even if the
    // UDS-level revert request fails — the caller (lock release) is giving
    // up control either way, and a redundant future escalation is harmless.
    // The bool return is informational only (did the ECU confirm).
    bool revert_to_default();

    uint8_t current_session_type() const;

    // Phase 8 (D2): ensures the given SecurityAccess level is unlocked,
    // performing the 0x27 requestSeed/sendKey exchange if not already at
    // this level or higher -- a no-op returning true otherwise. Odd `level`
    // is the requestSeed sub-function per ISO 14229-1's convention; the
    // matching sendKey sub-function (level+1) is derived internally, not a
    // separate parameter. Real key derivation is OEM-proprietary and
    // secret (CLAUDE.md); this calls uds_services.hpp's clearly-labeled
    // derive_key_DEMO_ONLY_NOT_SECURE stand-in -- never use this outside
    // this project's own tests/demo. Reuses send_, the same serialized
    // transport-access path ensure_session already uses, so this and the
    // heartbeat thread never race on the socket either.
    bool ensure_security_level(uint8_t level);
    uint8_t current_security_level() const;

private:
    void heartbeat_loop();
    void start_heartbeat_locked();
    void stop_heartbeat_locked();

    SendFn send_;
    SessionManagerConfig config_;

    mutable std::mutex mtx_;
    uint8_t current_session_type_ = 0x01;
    uint8_t current_security_level_ = 0; // 0 = locked/default, matches ISO 14229-1's "no level unlocked"
    std::chrono::steady_clock::time_point last_touch_;

    std::thread heartbeat_thread_;
    std::atomic<bool> heartbeat_running_{false};
};

} // namespace sovd::uds_doip
