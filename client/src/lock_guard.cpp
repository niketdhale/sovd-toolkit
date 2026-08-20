#include "sovd/client/lock_guard.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>

namespace sovd::client {

LockGuard::LockGuard(SovdClient &client, std::string entity_path, int ttl_seconds, bool heartbeat)
    : client_(client), path_(std::move(entity_path)), ttl_seconds_(ttl_seconds) {
    lock_id_ = client_.acquire_lock(path_, ttl_seconds_);

    if (heartbeat) {
        heartbeat_thread_ = std::thread([this] {
            // Millisecond precision, not std::chrono::seconds(ttl/2): for a
            // short TTL (e.g. 1s, common in tests and for fast-expiring
            // locks) integer-second division rounds ttl/2 down to 0, and a
            // naive floor of "at least 1 second" then makes the heartbeat
            // fire *at* the TTL instead of ahead of it -- a race the lock
            // can lose. 100ms floor instead, which stays well clear of any
            // TTL this project's lock_manager would sanely be asked for.
            auto interval = std::chrono::milliseconds(std::max(100, ttl_seconds_ * 500));
            std::unique_lock<std::mutex> lk(cv_mtx_);
            while (!cv_.wait_for(lk, interval, [this] { return stop_.load(); })) {
                try {
                    client_.renew_lock(path_, lock_id_, ttl_seconds_);
                } catch (const SovdError &ex) {
                    // Best-effort, matching session_manager's heartbeat: a
                    // single missed renew doesn't abandon the lock (could be
                    // a transient blip) -- it just means this beat didn't
                    // extend the TTL. If the lock is genuinely gone (someone
                    // else broke in, or it already expired), every
                    // subsequent op the caller makes will surface that on
                    // its own via a 423/403 from the server, which is the
                    // right place to fail loudly -- not this thread.
                    std::cerr << "warning: lock renew failed for " << path_ << ": " << ex.what() << std::endl;
                }
            }
        });
    }
}

LockGuard::~LockGuard() {
    stop_.store(true);
    cv_.notify_one();
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();

    try {
        client_.release_lock(path_, lock_id_);
    } catch (const SovdError &) {
        // Best-effort: a destructor can't throw, and a release that fails
        // (e.g. the lock already expired server-side) leaves nothing worse
        // than what an expired lock already implies.
    }
}

} // namespace sovd::client
