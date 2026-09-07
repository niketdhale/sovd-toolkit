#include "sovd/server/stream_ticket.hpp"

#include <openssl/rand.h>

namespace sovd::server {

namespace {

std::string hex16(const unsigned char *bytes) {
    static const char *digits = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (int i = 0; i < 16; ++i) {
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 0x0F]);
    }
    return out;
}

} // namespace

std::chrono::steady_clock::time_point StreamTicketStore::default_clock() { return std::chrono::steady_clock::now(); }

StreamTicketStore::StreamTicketStore(SteadyClock clock) : clock_(std::move(clock)) {}

void StreamTicketStore::sweep_expired() {
    auto now = clock_();
    for (auto it = tickets_.begin(); it != tickets_.end();) {
        if (now >= it->second.expires_at) {
            it = tickets_.erase(it);
        } else {
            ++it;
        }
    }
}

std::optional<std::string> StreamTicketStore::issue(const std::string &entity_path, const std::string &data_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    sweep_expired();
    if (tickets_.size() >= kMaxOutstandingTickets) return std::nullopt;

    // docs/reviews/round-2.md Task 11: mt19937_64 (what generate_correlation_id
    // uses) is not a CSPRNG -- its internal state is recoverable from ~312
    // consecutive 64-bit outputs. A correlation id only needs to avoid
    // collision; this token grants temporary read access, so it needs to be
    // unguessable. OpenSSL is already linked for the OAuth2 HMAC, so
    // RAND_bytes costs nothing new to reach for.
    unsigned char raw[16];
    if (RAND_bytes(raw, sizeof raw) != 1) return std::nullopt; // fail closed, not a weak fallback
    std::string ticket = "tkt-" + hex16(raw);

    Entry entry{entity_path, data_id, clock_() + std::chrono::seconds(kStreamTicketTtlSeconds)};
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

std::size_t StreamTicketStore::outstanding_count() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return tickets_.size();
}

} // namespace sovd::server
