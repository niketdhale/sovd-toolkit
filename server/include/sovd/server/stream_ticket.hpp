// StreamTicketStore — short-lived, single-use tickets for the SSE stream
// endpoint. Pure logic: no HTTP, no sockets, no JSON, no sleeping (clock is
// injectable, same shape as core/lock_manager.hpp).
//
// Why this exists (SOVD_REVIEW_FEEDBACK.md Task 1c): the browser's
// EventSource API cannot set an Authorization header, so once OAuth2 is
// enabled the SSE stream route can't be gated the same way every other route
// is. A client instead POSTs (with a normal bearer token) to mint a ticket
// bound to one (entity path, data id) pair, then opens the EventSource with
// that ticket in the query string. The ticket is burned on first redeem and
// expires in kStreamTicketTtlSeconds regardless — short enough that a
// captured ticket is useless once the stream has actually opened, long
// enough to cover realistic connection latency.
#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace sovd::server {

using SteadyClock = std::function<std::chrono::steady_clock::time_point()>;

inline constexpr int kStreamTicketTtlSeconds = 30;

class StreamTicketStore {
public:
    explicit StreamTicketStore(SteadyClock clock = default_clock);

    // Mints a new opaque ticket bound to exactly (entity_path, data_id).
    // Always succeeds -- the caller already passed the bearer/scope check
    // before calling this.
    std::string issue(const std::string &entity_path, const std::string &data_id);

    // Single-use: true and consumes the ticket only if it exists, hasn't
    // expired, and matches (entity_path, data_id) exactly. Anything else
    // (unknown, expired, wrong entity/id, already redeemed) is false --
    // caller can't distinguish reasons from the outside, same shape as
    // LockManager's release()/renew().
    bool redeem(const std::string &ticket, const std::string &entity_path, const std::string &data_id);

private:
    static std::chrono::steady_clock::time_point default_clock();

    struct Entry {
        std::string entity_path;
        std::string data_id;
        std::chrono::steady_clock::time_point expires_at;
    };

    mutable std::mutex mtx_;
    std::unordered_map<std::string, Entry> tickets_;
    SteadyClock clock_;
    unsigned long long next_id_ = 1;
};

} // namespace sovd::server
