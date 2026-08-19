// Phase 3: minimal MQTT 3.1.1 publisher — the transport behind
// Router::EventSink. QoS0, no TLS, no subscribe: this only ever fire-and-
// forgets already-serialized JSON lines onto the existing MQTT -> Telegraf
// -> InfluxDB -> Grafana pipeline (CLAUDE.md). That narrow a slice of the
// protocol doesn't justify pulling in libmosquitto/paho (neither is even
// installed on this box) — same call the project already made for DoIP
// (adapters/uds_doip/doip_transport.hpp): hand-roll the wire format, split
// pure encode logic (unit-testable, no socket) from the socket transport.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sovd::server::mqtt {

// Pure packet encoding — no I/O, exercised directly by tests.
std::vector<uint8_t> encode_connect(const std::string &client_id, uint16_t keep_alive_sec = 60);
std::vector<uint8_t> encode_publish(const std::string &topic, const std::string &payload);
std::vector<uint8_t> encode_disconnect();

// Fire-and-forget publisher bound to one host/port/topic. Connects lazily on
// first publish, reconnects on failure. Never throws: an unreachable
// telemetry/security bus must not take down request handling (the same
// graceful-degradation spirit as Phase 4's "one unreachable ECU must not
// fail the whole entity listing") — publish() silently drops on failure.
class MqttPublisher {
public:
    MqttPublisher(std::string host, uint16_t port, std::string client_id, std::string topic);
    ~MqttPublisher();

    MqttPublisher(const MqttPublisher &) = delete;
    MqttPublisher &operator=(const MqttPublisher &) = delete;

    void publish(const std::string &payload);

private:
    bool ensure_connected();
    void disconnect();

    std::string host_;
    uint16_t port_;
    std::string client_id_;
    std::string topic_;
    int sock_ = -1;
};

} // namespace sovd::server::mqtt
