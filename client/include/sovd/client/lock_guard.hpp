// Phase 5: RAII lock lifecycle. Acquires on construction, releases on scope
// exit (including via an exception), and — the part that needs real
// server-side support, not just client-side bookkeeping — a background
// thread renews the lock at ttl/2 via the new PUT /locks/{id} endpoint
// (routes.cpp), so a short TTL doesn't expire out from under a long-running
// operation. docs/DESIGN.md's stated reason: "a tester that crashes holding a
// 3600s lock bricks the entity until expiry" — the fix is a short TTL kept
// alive by heartbeat, not one long TTL requested upfront, so a crash (which
// stops the heartbeat with it) still self-heals on the *original* short
// timeout.
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "sovd/client/sovd_client.hpp"

namespace sovd::client {

class LockGuard {
public:
    // Throws SovdError if the lock can't be acquired (e.g. 423 -- someone
    // else holds it) -- there's no such thing as a half-constructed guard.
    LockGuard(SovdClient &client, std::string entity_path, int ttl_seconds = 60, bool heartbeat = true);
    ~LockGuard();

    LockGuard(const LockGuard &) = delete;
    LockGuard &operator=(const LockGuard &) = delete;

    const std::string &lock_id() const { return lock_id_; }

private:
    SovdClient &client_;
    std::string path_;
    std::string lock_id_;
    int ttl_seconds_;
    std::atomic<bool> stop_{false};
    std::mutex cv_mtx_;
    std::condition_variable cv_; // lets the destructor wake the heartbeat thread immediately, not after up to ttl/2
    std::thread heartbeat_thread_;
};

} // namespace sovd::client
