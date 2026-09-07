// LockManager — per-entity TTL locks. Pure logic: no HTTP, no sockets, no
// JSON, no sleeping. The clock is injectable so tests can advance time
// deterministically instead of waiting on a wall clock.
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <mutex>

namespace sovd {

using SteadyClock = std::function<std::chrono::steady_clock::time_point()>;

enum class LockReleaseResult { Released, NotFound, WrongId };
enum class LockRenewResult { Renewed, NotFound, WrongId };

// Phase 8: an unbounded TTL is a DoS on a safety-adjacent interface (one
// client parks a lock for a year, every other tester is locked out for a
// year) -- clamped, not rejected, so an over-generous request degrades to
// "as long as we'll allow" instead of failing outright. Comfortably above
// both the CLI's 60s default and D3's 10s browser TTL; well below "may as
// well be forever."
inline constexpr int kMaxLockTtlSeconds = 3600;

class LockManager {
public:
    explicit LockManager(SteadyClock clock = default_clock);

    // nullopt means the entity is already locked and unexpired (conflict).
    // ttl_seconds is clamped to kMaxLockTtlSeconds, not rejected.
    std::optional<std::string> acquire(const std::string &entity_path, int ttl_seconds);

    LockReleaseResult release(const std::string &entity_path, const std::string &lock_id);

    // Phase 5: extends an already-held lock's TTL without changing its id —
    // what the client SDK's RAII lock heartbeat calls at ttl/2. Fails the
    // same two ways release() does (NotFound covers "never locked" and
    // "expired": the caller can't tell those apart from the outside, and
    // doesn't need to — both mean "you don't hold it, stop heartbeating").
    LockRenewResult renew(const std::string &entity_path, const std::string &lock_id, int ttl_seconds);

    // true if the entity is unlocked, or locked with lock_id == supplied_lock_id.
    bool check_lock(const std::string &entity_path, const std::string &supplied_lock_id) const;

    bool is_locked(const std::string &entity_path) const;

    // Phase 8: number of currently held (unexpired) locks server-wide.
    // Doubles as an upper bound on concurrently escalated UDS sessions
    // without LockManager needing to know sessions exist at all: escalation
    // only ever happens from a lock-gated call (docs/DESIGN.md's settled session-
    // manager-ownership decision), so "how many entities are locked right
    // now" already bounds "how many entities could have an escalated
    // session right now." The policy decision (what the cap is, what error
    // code a rejection maps to) lives in Router, not here -- this is just
    // the query.
    size_t held_lock_count() const;

private:
    static std::chrono::steady_clock::time_point default_clock();
    bool locked_unexpired(const std::string &entity_path) const; // caller holds mtx_

    struct LockEntry {
        std::string lock_id;
        std::chrono::steady_clock::time_point expires_at;
    };

    mutable std::mutex mtx_;
    std::unordered_map<std::string, LockEntry> locks_;
    SteadyClock clock_;
    unsigned long long next_id_ = 1;
};

} // namespace sovd
