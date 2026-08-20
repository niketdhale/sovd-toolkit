#include "sovd/uds_doip/session_manager.hpp"

#include "sovd/uds_doip/uds_services.hpp"

namespace sovd::uds_doip {

SessionManager::SessionManager(SendFn send, SessionManagerConfig config)
    : send_(std::move(send)), config_(config), last_touch_(std::chrono::steady_clock::now()) {}

SessionManager::~SessionManager() {
    heartbeat_running_.store(false);
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
}

uint8_t SessionManager::current_session_type() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return current_session_type_;
}

uint8_t SessionManager::current_security_level() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return current_security_level_;
}

// Signals the heartbeat thread to stop and joins it. NEVER call this while
// holding mtx_: heartbeat_loop() acquires mtx_ itself on every wake-up to
// check the stop flag, so joining under the same lock can deadlock against
// a thread that's genuinely still running (not just one that already exited
// via the idle timeout, where join() would return immediately).
void SessionManager::stop_heartbeat_locked() {
    heartbeat_running_.store(false);
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
}

bool SessionManager::ensure_session(uint8_t session_type) {
    std::unique_lock<std::mutex> lk(mtx_);
    last_touch_ = std::chrono::steady_clock::now();

    if (current_session_type_ == session_type) {
        return true;
    }
    lk.unlock();

    auto req = uds::encode_diagnostic_session_control(session_type);
    std::vector<uint8_t> resp;
    if (!send_(req, resp) || !uds::decode_diagnostic_session_control(resp, session_type)) {
        return false;
    }

    // Stop whatever heartbeat might already be running (e.g. this call is
    // escalating straight from one non-default session to another) before
    // starting the new one. Outside the lock: see stop_heartbeat_locked's
    // comment on why joining under mtx_ is unsafe.
    stop_heartbeat_locked();

    lk.lock();
    current_session_type_ = session_type;
    heartbeat_running_.store(true);
    heartbeat_thread_ = std::thread([this] { heartbeat_loop(); });
    return true;
}

bool SessionManager::revert_to_default() {
    std::unique_lock<std::mutex> lk(mtx_);
    if (current_session_type_ == 0x01) {
        lk.unlock();
        stop_heartbeat_locked(); // defensive: make sure it's actually stopped even if bookkeeping says default
        return true;
    }
    lk.unlock();

    auto req = uds::encode_diagnostic_session_control(0x01);
    std::vector<uint8_t> resp;
    bool confirmed = send_(req, resp) && uds::decode_diagnostic_session_control(resp, 0x01);

    // Local bookkeeping reverts regardless of confirmation: the caller is
    // relinquishing the lock either way, and a future escalation attempt
    // (a redundant DiagnosticSessionControl if the ECU was actually already
    // reverted, or a real one if it wasn't) is harmless.
    stop_heartbeat_locked();
    lk.lock();
    current_session_type_ = 0x01;
    // Real ECUs tie SecurityAccess to the session it was unlocked in --
    // reverting the session drops it too, matching that behavior rather
    // than leaving stale bookkeeping that claims a level is still unlocked
    // against an ECU that no longer agrees.
    current_security_level_ = 0;
    return confirmed;
}

bool SessionManager::ensure_security_level(uint8_t level) {
    std::unique_lock<std::mutex> lk(mtx_);
    last_touch_ = std::chrono::steady_clock::now();

    if (current_security_level_ >= level) {
        return true;
    }
    lk.unlock();

    auto seed_req = uds::encode_security_access_request_seed(level);
    std::vector<uint8_t> seed_resp;
    if (!send_(seed_req, seed_resp)) return false;
    std::vector<uint8_t> seed;
    if (!uds::decode_security_access_seed(seed_resp, level, seed)) return false;

    auto key = uds::derive_key_DEMO_ONLY_NOT_SECURE(seed, level);
    auto send_key_level = static_cast<uint8_t>(level + 1);
    auto key_req = uds::encode_security_access_send_key(send_key_level, key);
    std::vector<uint8_t> key_resp;
    if (!send_(key_req, key_resp)) return false;
    if (!uds::decode_security_access_key_accepted(key_resp, send_key_level)) return false;

    lk.lock();
    current_security_level_ = level;
    return true;
}

void SessionManager::heartbeat_loop() {
    while (heartbeat_running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(config_.heartbeat_interval_ms));
        if (!heartbeat_running_.load()) return;

        std::unique_lock<std::mutex> lk(mtx_);
        if (!heartbeat_running_.load()) return;

        auto idle_for = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                last_touch_)
                             .count();
        if (idle_for >= config_.idle_timeout_ms) {
            current_session_type_ = 0x01;
            current_security_level_ = 0; // same reasoning as revert_to_default(): tied to the session
            heartbeat_running_.store(false);
            return; // thread exits; left joinable for the next stop_heartbeat_locked()/destructor
        }
        lk.unlock();

        // Best-effort: a single missed heartbeat doesn't itself tear down
        // the session — that would let one transient blip discard a session
        // the client still legitimately holds a lock for. It just means
        // this beat didn't refresh the ECU-side session timer.
        auto req = uds::encode_tester_present();
        std::vector<uint8_t> resp;
        send_(req, resp);
    }
}

} // namespace sovd::uds_doip
