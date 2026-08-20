// Phase 6: the shared poller behind every SSE data subscription. "Backed by
// adapter-level periodic read, not per-request polling" (CLAUDE.md) means
// N browser tabs watching the same data point must not turn into N
// independent timers each hitting the adapter/UDS bus — they share ONE
// poller, keyed by (path, id, interval_ms); the last unsubscribe stops it.
// Pure logic + threading, no HTTP/SSE framing here — routes.cpp owns that.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace sovd::server {

class StreamHub {
public:
    // Returns the current JSON-encoded value (or an inline {"error":...}
    // body, same shape read_one_data_item already produces) -- called by
    // the poller thread, never by an HTTP-handling thread directly.
    using ReadFn = std::function<std::string()>;

    class Subscription {
    public:
        Subscription() = default;
        ~Subscription();
        Subscription(Subscription &&other) noexcept;
        Subscription &operator=(Subscription &&other) noexcept;
        Subscription(const Subscription &) = delete;
        Subscription &operator=(const Subscription &) = delete;

        // Blocks up to timeout_ms for a value newer than the last one this
        // subscription observed. Returns true + fills out_json on a new
        // value; false on timeout (caller sends an SSE keep-alive comment
        // and calls again) -- this is what keeps an idle SSE connection
        // alive and bounds how long an HTTP worker thread ever blocks in
        // one call, so a client disconnect is noticed promptly.
        bool wait_next(std::string &out_json, int timeout_ms);

    private:
        friend class StreamHub;
        Subscription(StreamHub *hub, std::string key, std::shared_ptr<struct PollerState> poller);

        StreamHub *hub_ = nullptr;
        std::string key_;
        std::shared_ptr<struct PollerState> poller_;
        uint64_t last_seen_version_ = 0;
    };

    // interval_ms is part of the sharing key on purpose: two subscribers
    // asking for the same (path, id) at the same cadence share one poller;
    // different cadences get independent ones rather than silently forcing
    // one subscriber's rate onto another.
    Subscription subscribe(const std::string &path, const std::string &id, int interval_ms, ReadFn read_fn);

private:
    void release(const std::string &key);

    std::mutex map_mtx_;
    std::unordered_map<std::string, std::shared_ptr<struct PollerState>> pollers_;
};

} // namespace sovd::server
