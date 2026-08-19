// LockManager — per-entity TTL locks. Pure logic: no HTTP, no sockets, no
// JSON, no sleeping. The clock is injectable so tests can advance time
// deterministically instead of waiting on a wall clock.
#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <mutex>

namespace sovd {

using SteadyClock = std::function<std::chrono::steady_clock::time_point()>;

enum class LockReleaseResult { Released, NotFound, WrongId };

class LockManager {
public:
    explicit LockManager(SteadyClock clock = default_clock);

    // nullopt means the entity is already locked and unexpired (conflict).
    std::optional<std::string> acquire(const std::string &entity_path, int ttl_seconds);

    LockReleaseResult release(const std::string &entity_path, const std::string &lock_id);

    // true if the entity is unlocked, or locked with lock_id == supplied_lock_id.
    bool check_lock(const std::string &entity_path, const std::string &supplied_lock_id) const;

    bool is_locked(const std::string &entity_path) const;

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
