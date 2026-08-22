#include "sovd/server/stream_ticket.hpp"

#include <cstdio>
#include <random>

namespace sovd::server {

std::chrono::steady_clock::time_point StreamTicketStore::default_clock() { return std::chrono::steady_clock::now(); }

StreamTicketStore::StreamTicketStore(SteadyClock clock) : clock_(std::move(clock)) {}

std::string StreamTicketStore::issue(const std::string &entity_path, const std::string &data_id) {
    thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist;
    char buf[40];
    // Two 64-bit draws, not one -- this token grants temporary read access,
    // unlike the correlation id's single draw which only needs to be
    // unpredictable enough to not collide, not resistant to guessing.
    std::snprintf(buf, sizeof(buf), "tkt-%016llx%016llx", static_cast<unsigned long long>(dist(rng)),
                  static_cast<unsigned long long>(dist(rng)));

    std::lock_guard<std::mutex> lock(mtx_);
    Entry entry{entity_path, data_id, clock_() + std::chrono::seconds(kStreamTicketTtlSeconds)};
    std::string ticket(buf);
    tickets_[ticket] = std::move(entry);
    return ticket;
}

bool StreamTicketStore::redeem(const std::string &ticket, const std::string &entity_path,
                                const std::string &data_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = tickets_.find(ticket);
    if (it == tickets_.end()) return false;
    // Single-use regardless of outcome: erase before checking anything else,
    // so a wrong-entity/expired ticket can't be retried against a different
    // (path, id) guess.
    Entry entry = std::move(it->second);
    tickets_.erase(it);
    if (clock_() >= entry.expires_at) return false;
    return entry.entity_path == entity_path && entry.data_id == data_id;
}

} // namespace sovd::server
