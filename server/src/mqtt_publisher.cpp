#include "sovd/server/mqtt_publisher.hpp"

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace sovd::server::mqtt {

namespace {

void append_varint(std::vector<uint8_t> &out, size_t len) {
    do {
        uint8_t byte = len % 128;
        len /= 128;
        if (len > 0) byte |= 0x80;
        out.push_back(byte);
    } while (len > 0);
}

void append_mqtt_string(std::vector<uint8_t> &out, const std::string &s) {
    out.push_back(static_cast<uint8_t>((s.size() >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(s.size() & 0xFF));
    out.insert(out.end(), s.begin(), s.end());
}

} // namespace

std::vector<uint8_t> encode_connect(const std::string &client_id, uint16_t keep_alive_sec) {
    std::vector<uint8_t> var_and_payload;
    append_mqtt_string(var_and_payload, "MQTT");
    var_and_payload.push_back(0x04); // protocol level 3.1.1
    var_and_payload.push_back(0x02); // connect flags: clean session, no will/user/pass
    var_and_payload.push_back(static_cast<uint8_t>((keep_alive_sec >> 8) & 0xFF));
    var_and_payload.push_back(static_cast<uint8_t>(keep_alive_sec & 0xFF));
    append_mqtt_string(var_and_payload, client_id);

    std::vector<uint8_t> packet;
    packet.push_back(0x10); // CONNECT
    append_varint(packet, var_and_payload.size());
    packet.insert(packet.end(), var_and_payload.begin(), var_and_payload.end());
    return packet;
}

std::vector<uint8_t> encode_publish(const std::string &topic, const std::string &payload) {
    std::vector<uint8_t> var_and_payload;
    append_mqtt_string(var_and_payload, topic);
    // QoS 0: no packet identifier.
    var_and_payload.insert(var_and_payload.end(), payload.begin(), payload.end());

    std::vector<uint8_t> packet;
    packet.push_back(0x30); // PUBLISH, QoS0, no DUP/RETAIN
    append_varint(packet, var_and_payload.size());
    packet.insert(packet.end(), var_and_payload.begin(), var_and_payload.end());
    return packet;
}

std::vector<uint8_t> encode_disconnect() { return {0xE0, 0x00}; }

MqttPublisher::MqttPublisher(std::string host, uint16_t port, std::string client_id, std::string topic)
    : host_(std::move(host)), port_(port), client_id_(std::move(client_id)), topic_(std::move(topic)) {}

MqttPublisher::~MqttPublisher() { disconnect(); }

void MqttPublisher::disconnect() {
    if (sock_ >= 0) {
        ::close(sock_);
        sock_ = -1;
    }
}

bool MqttPublisher::ensure_connected() {
    if (sock_ >= 0) return true;

    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    if (getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &res) != 0) return false;

    int fd = -1;
    for (auto *p = res; p != nullptr; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return false;

    auto connect_pkt = encode_connect(client_id_);
    if (::send(fd, connect_pkt.data(), connect_pkt.size(), 0) != static_cast<ssize_t>(connect_pkt.size())) {
        ::close(fd);
        return false;
    }
    // CONNACK is always 4 bytes; not parsed beyond that a reply arrived —
    // QoS0 publishes are best-effort regardless, so a malformed/rejected
    // CONNACK just means the next publish() will fail and retry the
    // connection, same as any other transport hiccup.
    uint8_t connack[4];
    if (::recv(fd, connack, sizeof(connack), 0) != static_cast<ssize_t>(sizeof(connack))) {
        ::close(fd);
        return false;
    }

    sock_ = fd;
    return true;
}

void MqttPublisher::publish(const std::string &payload) {
    if (!ensure_connected()) return;
    auto pkt = encode_publish(topic_, payload);
    if (::send(sock_, pkt.data(), pkt.size(), 0) != static_cast<ssize_t>(pkt.size())) {
        disconnect(); // next publish() reconnects
    }
}

} // namespace sovd::server::mqtt
