#include "sovd/server/routes.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <regex>
#include <sstream>
#include <variant>
#include <vector>

#include "httplib.h"
#include "json.hpp"
#include "routes_internal.hpp"
#include "sovd/server/oauth2.hpp"

namespace sovd::server {

using json = nlohmann::json;

namespace detail {

void emit_event(const Router::EventSink &sink, const std::string &type, json fields) {
    fields["event"] = type;
    fields["ts_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    sink(fields.dump());
}

int http_status_for(sovd_result_t r) {
    switch (r) {
        case SOVD_OK: return 200;
        case SOVD_NOT_FOUND: return 404;
        case SOVD_LOCKED: return 423;
        case SOVD_BAD_REQUEST: return 400;
        case SOVD_UNSUPPORTED: return 501;
        case SOVD_BUSY: return 503;
        case SOVD_TRANSPORT:
        case SOVD_NEGATIVE_RESPONSE: return 502;
        case SOVD_INTERNAL: return 500;
        case SOVD_FORBIDDEN: return 403;
        case SOVD_CONFLICT: return 409;
    }
    return 500;
}

void write_error(httplib::Response &res, int status, const std::string &code, const std::string &message) {
    res.status = status;
    res.set_content(json{{"error", code}, {"message", message}}.dump(), "application/json");
}

std::string to_hex(const uint8_t *data, size_t len) {
    static const char *digits = "0123456789ABCDEF";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0x0F]);
    }
    return out;
}

std::string to_hex_u16(uint16_t v) {
    char buf[5];
    std::snprintf(buf, sizeof(buf), "%04X", v);
    return std::string(buf);
}

DataReadOutcome read_one_data_item(const Entity &e, const std::string &path, const std::string &id_or_did,
                                    const catalog::Catalog *cat) {
    const catalog::DataItem *item = cat ? cat->find_by_id(id_or_did) : nullptr;
    std::string did_hex = item ? to_hex_u16(item->did) : id_or_did;

    sovd_buffer_t buf{};
    sovd_result_t r = e.vtable->read_data(e.adapter_ctx, path.c_str(), did_hex.c_str(), &buf);
    if (r != SOVD_OK) {
        return DataReadOutcome{false, http_status_for(r), "ADAPTER_ERROR", "read_data failed", {}};
    }
    std::vector<uint8_t> bytes(buf.data, buf.data + buf.len);
    if (e.vtable->free_buffer) e.vtable->free_buffer(&buf);

    DataReadOutcome out;
    out.ok = true;
    if (item) {
        out.body["id"] = item->id;
        out.body["did"] = did_hex;
        try {
            auto decoded = catalog::Catalog::decode(*item, bytes);
            if (std::holds_alternative<std::string>(decoded)) {
                out.body["value"] = std::get<std::string>(decoded);
            } else {
                out.body["value"] = std::get<double>(decoded);
            }
        } catch (const catalog::CatalogError &ex) {
            return DataReadOutcome{false, 502, "ADAPTER_ERROR", std::string("catalog decode failed: ") + ex.what(),
                                    {}};
        }
        if (!item->encoding.unit.empty()) out.body["unit"] = item->encoding.unit;
    } else {
        out.body["id"] = id_or_did;
        out.body["value"] = to_hex(bytes.data(), bytes.size());
    }
    return out;
}

bool from_hex(std::string s, std::vector<uint8_t> &out) {
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    if (s.empty() || s.size() % 2 != 0) return false;
    out.clear();
    out.reserve(s.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = nibble(s[i]);
        int lo = nibble(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}
} // namespace detail

} // namespace sovd::server
