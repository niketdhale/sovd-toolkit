#include "sovd/lock_manager.hpp"

#include <sstream>

namespace sovd {

std::chrono::steady_clock::time_point LockManager::default_clock() {
    return std::chrono::steady_clock::now();
}

LockManager::LockManager(SteadyClock clock) : clock_(std::move(clock)) {}

bool LockManager::locked_unexpired(const std::string &entity_path) const {
    auto it = locks_.find(entity_path);
    if (it == locks_.end()) return false;
    return clock_() < it->second.expires_at;
}

std::optional<std::string> LockManager::acquire(const std::string &entity_path, int ttl_seconds) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (locked_unexpired(entity_path)) {
        return std::nullopt;
    }
    std::ostringstream oss;
    oss << "lock-" << next_id_++;
    std::string id = oss.str();

    LockEntry entry;
    entry.lock_id = id;
    entry.expires_at = clock_() + std::chrono::seconds(ttl_seconds);
    locks_[entity_path] = entry;
    return id;
}

LockReleaseResult LockManager::release(const std::string &entity_path, const std::string &lock_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!locked_unexpired(entity_path)) {
        locks_.erase(entity_path); // purge a stale expired entry, if any
        return LockReleaseResult::NotFound;
    }
    auto it = locks_.find(entity_path);
    if (it->second.lock_id != lock_id) {
        return LockReleaseResult::WrongId;
    }
    locks_.erase(it);
    return LockReleaseResult::Released;
}

LockRenewResult LockManager::renew(const std::string &entity_path, const std::string &lock_id, int ttl_seconds) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!locked_unexpired(entity_path)) {
        locks_.erase(entity_path); // purge a stale expired entry, if any
        return LockRenewResult::NotFound;
    }
    auto it = locks_.find(entity_path);
    if (it->second.lock_id != lock_id) {
        return LockRenewResult::WrongId;
    }
    it->second.expires_at = clock_() + std::chrono::seconds(ttl_seconds);
    return LockRenewResult::Renewed;
}

bool LockManager::check_lock(const std::string &entity_path, const std::string &supplied_lock_id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!locked_unexpired(entity_path)) return true;
    return locks_.find(entity_path)->second.lock_id == supplied_lock_id;
}

bool LockManager::is_locked(const std::string &entity_path) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return locked_unexpired(entity_path);
}

} // namespace sovd
