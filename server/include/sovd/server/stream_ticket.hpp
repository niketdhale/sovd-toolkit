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
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace sovd::server {

using SteadyClock = std::function<std::chrono::steady_clock::time_point()>;

inline constexpr int kStreamTicketTtlSeconds = 30;

// SOVD_REVIEW_ROUND2.md Task 10: this project's stated Phase 8 position is
// that unbounded anything is a DoS on a safety-adjacent interface --
// LockManager purges stale entries on access and routes.cpp caps concurrent
// locks at kMaxConcurrentLocks (64). This mirrors that same cap so the
// ticket store isn't the one exception to it.
inline constexpr std::size_t kMaxOutstandingTickets = 64;

class StreamTicketStore {
public:
    explicit StreamTicketStore(SteadyClock clock = default_clock);

    // Mints a new opaque ticket bound to exactly (entity_path, data_id).
    // Sweeps expired entries first (so the cap is reached only by
    // genuinely concurrent outstanding tickets, not historical churn), then
    // nullopt if still at kMaxOutstandingTickets -- caller maps that to a
    // 503, same shape as the lock route's own over-cap path.
    std::optional<std::string> issue(const std::string &entity_path, const std::string &data_id);

    // Single-use: true and consumes the ticket only if it exists, hasn't
    // expired, and matches (entity_path, data_id) exactly. Anything else
    // (unknown, expired, wrong entity/id, already redeemed) is false --
    // caller can't distinguish reasons from the outside, same shape as
    // LockManager's release()/renew().
    bool redeem(const std::string &ticket, const std::string &entity_path, const std::string &data_id);

    // Test-only: count of currently unswept (not necessarily unexpired)
    // outstanding tickets. Doesn't force a sweep itself -- callers that
    // want to observe post-sweep state call issue() first, same as the
    // real capacity check does.
    std::size_t outstanding_count() const;

private:
    static std::chrono::steady_clock::time_point default_clock();
    void sweep_expired(); // caller holds mtx_

    struct Entry {
        std::string entity_path;
        std::string data_id;
        std::chrono::steady_clock::time_point expires_at;
    };

    mutable std::mutex mtx_;
    std::unordered_map<std::string, Entry> tickets_;
    SteadyClock clock_;
};

} // namespace sovd::server
