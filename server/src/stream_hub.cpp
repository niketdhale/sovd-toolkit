#include "sovd/server/stream_hub.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace sovd::server {

struct PollerState {
    std::mutex mtx;
    std::condition_variable cv;
    std::string latest_json;
    uint64_t version = 0;
    int subscriber_count = 0;
    bool stop = false;
    std::thread thread;
};

StreamHub::Subscription::Subscription(StreamHub *hub, std::string key, std::shared_ptr<PollerState> poller)
    : hub_(hub), key_(std::move(key)), poller_(std::move(poller)) {}

StreamHub::Subscription::Subscription(Subscription &&other) noexcept
    : hub_(other.hub_), key_(std::move(other.key_)), poller_(std::move(other.poller_)),
      last_seen_version_(other.last_seen_version_) {
    other.hub_ = nullptr;
}

StreamHub::Subscription &StreamHub::Subscription::operator=(Subscription &&other) noexcept {
    if (this != &other) {
        if (hub_) hub_->release(key_);
        hub_ = other.hub_;
        key_ = std::move(other.key_);
        poller_ = std::move(other.poller_);
        last_seen_version_ = other.last_seen_version_;
        other.hub_ = nullptr;
    }
    return *this;
}

StreamHub::Subscription::~Subscription() {
    if (hub_) hub_->release(key_);
}

bool StreamHub::Subscription::wait_next(std::string &out_json, int timeout_ms) {
    std::unique_lock<std::mutex> lk(poller_->mtx);
    bool updated = poller_->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                         [this] { return poller_->version != last_seen_version_ || poller_->stop; });
    if (!updated || poller_->stop) return false;
    out_json = poller_->latest_json;
    last_seen_version_ = poller_->version;
    return true;
}

StreamHub::Subscription StreamHub::subscribe(const std::string &path, const std::string &id, int interval_ms,
                                              ReadFn read_fn) {
    std::string key = path + "\x1f" + id + "\x1f" + std::to_string(interval_ms);

    std::shared_ptr<PollerState> poller;
    {
        std::lock_guard<std::mutex> lk(map_mtx_);
        auto it = pollers_.find(key);
        if (it != pollers_.end()) {
            poller = it->second;
            std::lock_guard<std::mutex> plk(poller->mtx);
            poller->subscriber_count++;
        } else {
            poller = std::make_shared<PollerState>();
            poller->subscriber_count = 1;
            pollers_[key] = poller;

            // First subscriber starts the poller; it runs until the last
            // subscriber for this key unsubscribes (release() below).
            std::weak_ptr<PollerState> weak_poller = poller;
            poller->thread = std::thread([weak_poller, interval_ms, read_fn = std::move(read_fn)] {
                while (true) {
                    auto p = weak_poller.lock();
                    if (!p) return; // hub already dropped the last shared_ptr
                    {
                        std::unique_lock<std::mutex> lk(p->mtx);
                        if (p->stop) return;
                    }
                    std::string value = read_fn();
                    {
                        std::lock_guard<std::mutex> lk(p->mtx);
                        if (p->stop) return;
                        p->latest_json = std::move(value);
                        p->version++;
                    }
                    p->cv.notify_all();

                    // Interruptible wait, not sleep_for(): sleep_for can't
                    // be woken by stop()+notify_all(), so unsubscribing
                    // from a long-interval stream would otherwise block
                    // whoever is destroying the last Subscription --
                    // possibly an HTTP worker thread -- for up to the full
                    // interval instead of returning immediately.
                    std::unique_lock<std::mutex> lk(p->mtx);
                    if (p->cv.wait_for(lk, std::chrono::milliseconds(interval_ms), [&p] { return p->stop; })) {
                        return;
                    }
                }
            });
        }
    }

    return Subscription(this, std::move(key), poller);
}

void StreamHub::release(const std::string &key) {
    std::shared_ptr<PollerState> to_join;
    {
        std::lock_guard<std::mutex> lk(map_mtx_);
        auto it = pollers_.find(key);
        if (it == pollers_.end()) return;
        auto &poller = it->second;

        bool last = false;
        {
            std::lock_guard<std::mutex> plk(poller->mtx);
            poller->subscriber_count--;
            last = poller->subscriber_count <= 0;
            if (last) poller->stop = true;
        }
        if (last) {
            poller->cv.notify_all(); // wake any wait_next() calls so they return false promptly
            to_join = poller;
            pollers_.erase(it);
        }
    }
    if (to_join && to_join->thread.joinable()) to_join->thread.join();
}

} // namespace sovd::server
