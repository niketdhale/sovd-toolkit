#include "sovd/catalog/did_catalog.hpp"

#include <cmath>
#include <cstdint>

#include <yaml-cpp/yaml.h>

namespace sovd::catalog {

namespace {

std::string require_string(const YAML::Node &node, const std::string &key, const std::string &context) {
    YAML::Node v = node[key];
    if (!v || !v.IsScalar()) {
        throw CatalogError(context + " is missing required field '" + key + "'");
    }
    return v.as<std::string>();
}

std::string optional_string(const YAML::Node &node, const std::string &key, const std::string &def = "") {
    YAML::Node v = node[key];
    if (!v || !v.IsScalar()) return def;
    return v.as<std::string>();
}

template <typename T>
T optional_scalar(const YAML::Node &node, const std::string &key, T def) {
    YAML::Node v = node[key];
    if (!v || !v.IsScalar()) return def;
    try {
        return v.as<T>();
    } catch (const YAML::BadConversion &) {
        return def;
    }
}

uint16_t parse_hex_u16(const std::string &text, const std::string &context) {
    std::string s = text;
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    if (s.empty()) throw CatalogError(context + " has an empty hex identifier");

    size_t consumed = 0;
    unsigned long value = 0;
    try {
        value = std::stoul(s, &consumed, 16);
    } catch (...) {
        throw CatalogError(context + " has an invalid hex identifier '" + text + "'");
    }
    if (consumed != s.size() || value > 0xFFFFu) {
        throw CatalogError(context + " has an invalid hex identifier '" + text + "'");
    }
    return static_cast<uint16_t>(value);
}

DataType parse_data_type(const std::string &s) {
    if (s == "string") return DataType::String;
    if (s == "float") return DataType::Float;
    if (s == "enum") return DataType::Enum;
    return DataType::Raw; // unrecognized type: treat as opaque bytes
}

Access parse_access(const std::string &s, const std::string &context) {
    if (s == "read") return Access::Read;
    if (s == "write") return Access::Write;
    if (s == "read_write") return Access::ReadWrite;
    throw CatalogError(context + " has invalid access '" + s + "' (expected read|write|read_write)");
}

DataItem parse_data_item(const YAML::Node &item) {
    DataItem d;
    d.id = require_string(item, "id", "a data item");

    std::string context = "data item '" + d.id + "'";
    d.did = parse_hex_u16(require_string(item, "did", context), context);
    d.type = parse_data_type(require_string(item, "type", context));
    d.access = parse_access(require_string(item, "access", context), context);
    d.io_control = optional_scalar<bool>(item, "io_control", false);

    std::string session = optional_string(item, "requires_session");
    if (!session.empty()) d.requires_session = session;

    int security_level = optional_scalar<int>(item, "requires_security_level", 0);
    if (security_level > 0) d.requires_security_level = security_level;

    switch (d.type) {
        case DataType::String:
            d.length = optional_scalar<int>(item, "length", 0);
            break;

        case DataType::Float: {
            YAML::Node enc = item["encoding"];
            if (!enc || !enc.IsMap()) {
                throw CatalogError(context + " has type float but no 'encoding' block");
            }
            d.encoding.bytes = optional_scalar<int>(enc, "bytes", 0);
            d.encoding.endian = optional_string(enc, "endian", "big");
            d.encoding.scale = optional_scalar<double>(enc, "scale", 1.0);
            d.encoding.unit = optional_string(enc, "unit");
            if (d.encoding.bytes <= 0 || d.encoding.bytes > 8) {
                throw CatalogError(context + " has an invalid encoding.bytes (must be 1-8)");
            }
            if (d.encoding.endian != "big" && d.encoding.endian != "little") {
                throw CatalogError(context + " has an invalid encoding.endian (must be big|little)");
            }
            break;
        }

        case DataType::Enum: {
            YAML::Node values = item["values"];
            if (!values || !values.IsMap()) {
                throw CatalogError(context + " has type enum but no 'values' map");
            }
            for (const auto &entry : values) {
                EnumValue ev;
                std::string key = entry.first.as<std::string>();
                try {
                    size_t consumed = 0;
                    ev.raw = std::stoi(key, &consumed);
                    if (consumed != key.size()) throw std::invalid_argument(key);
                } catch (...) {
                    throw CatalogError(context + " has a non-integer enum key '" + key + "'");
                }
                ev.label = entry.second.as<std::string>();
                d.values.push_back(ev);
            }
            break;
        }

        case DataType::Raw:
            break;
    }

    return d;
}

Operation parse_operation(const YAML::Node &item) {
    Operation op;
    op.id = require_string(item, "id", "an operation");

    std::string context = "operation '" + op.id + "'";
    op.routine_id = parse_hex_u16(require_string(item, "routine_id", context), context);

    std::string session = optional_string(item, "requires_session");
    if (!session.empty()) op.requires_session = session;
    op.async = optional_scalar<bool>(item, "async", false);

    return op;
}

std::vector<DataItem> parse_data_section(const YAML::Node &root) {
    std::vector<DataItem> data;
    YAML::Node data_node = root["data"];
    if (!data_node) return data;
    if (!data_node.IsSequence()) throw CatalogError("'data' must be a YAML sequence");
    for (const auto &item : data_node) {
        data.push_back(parse_data_item(item));
    }
    return data;
}

std::vector<Operation> parse_operations_section(const YAML::Node &root) {
    std::vector<Operation> operations;
    YAML::Node ops_node = root["operations"];
    if (!ops_node) return operations;
    if (!ops_node.IsSequence()) throw CatalogError("'operations' must be a YAML sequence");
    for (const auto &item : ops_node) {
        operations.push_back(parse_operation(item));
    }
    return operations;
}

} // namespace

std::string access_to_string(Access a) {
    switch (a) {
        case Access::Read: return "read";
        case Access::Write: return "write";
        case Access::ReadWrite: return "read_write";
    }
    return "read";
}

std::string data_type_to_string(DataType t) {
    switch (t) {
        case DataType::String: return "string";
        case DataType::Float: return "float";
        case DataType::Enum: return "enum";
        case DataType::Raw: return "raw";
    }
    return "raw";
}

Catalog Catalog::load_from_string(const std::string &yaml_text) {
    YAML::Node root;
    try {
        root = YAML::Load(yaml_text);
    } catch (const YAML::Exception &e) {
        throw CatalogError(std::string("YAML parse error: ") + e.what());
    }

    Catalog cat;
    if (root.IsDefined() && !root.IsNull()) {
        cat.data_ = parse_data_section(root);
        cat.operations_ = parse_operations_section(root);
    }
    return cat;
}

Catalog Catalog::load_from_file(const std::string &path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception &e) {
        throw CatalogError("YAML parse error in " + path + ": " + e.what());
    }

    Catalog cat;
    if (root.IsDefined() && !root.IsNull()) {
        cat.data_ = parse_data_section(root);
        cat.operations_ = parse_operations_section(root);
    }
    return cat;
}

const DataItem *Catalog::find_by_id(const std::string &id) const {
    for (auto &d : data_) {
        if (d.id == id) return &d;
    }
    return nullptr;
}

const DataItem *Catalog::find_by_did(uint16_t did) const {
    for (auto &d : data_) {
        if (d.did == did) return &d;
    }
    return nullptr;
}

const Operation *Catalog::find_operation(const std::string &id) const {
    for (auto &o : operations_) {
        if (o.id == id) return &o;
    }
    return nullptr;
}

namespace {

uint64_t bytes_to_uint(const std::vector<uint8_t> &bytes, bool big_endian) {
    uint64_t value = 0;
    if (big_endian) {
        for (uint8_t b : bytes) value = (value << 8) | b;
    } else {
        for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) value = (value << 8) | *it;
    }
    return value;
}

std::string to_hex(const std::vector<uint8_t> &bytes) {
    static const char *digits = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0F]);
    }
    return out;
}

std::vector<uint8_t> uint_to_bytes(uint64_t value, int nbytes, bool big_endian) {
    std::vector<uint8_t> out(static_cast<size_t>(nbytes));
    for (int i = 0; i < nbytes; ++i) {
        int shift = big_endian ? (nbytes - 1 - i) * 8 : i * 8;
        out[static_cast<size_t>(i)] = static_cast<uint8_t>((value >> shift) & 0xFF);
    }
    return out;
}

// Mirrors routes.cpp's from_hex (server/ can't be depended on from catalog/
// per the layering rule, so this is a small deliberate duplication rather
// than a cross-module reach) but throws instead of returning bool, matching
// every other validation failure in this file.
std::vector<uint8_t> from_hex(const std::string &item_id, const std::string &s_in) {
    std::string s = s_in;
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    if (s.empty() || s.size() % 2 != 0) {
        throw CatalogError("data item '" + item_id + "': value must be hex-encoded bytes");
    }
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = nibble(s[i]);
        int lo = nibble(s[i + 1]);
        if (hi < 0 || lo < 0) {
            throw CatalogError("data item '" + item_id + "': value must be hex-encoded bytes");
        }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

} // namespace

std::variant<std::string, double> Catalog::decode(const DataItem &item, const std::vector<uint8_t> &bytes) {
    switch (item.type) {
        case DataType::String:
            return std::string(bytes.begin(), bytes.end());

        case DataType::Float: {
            if (static_cast<int>(bytes.size()) != item.encoding.bytes) {
                throw CatalogError("data item '" + item.id + "': expected " +
                                    std::to_string(item.encoding.bytes) + " bytes, got " +
                                    std::to_string(bytes.size()));
            }
            uint64_t raw = bytes_to_uint(bytes, item.encoding.endian == "big");
            return static_cast<double>(raw) * item.encoding.scale;
        }

        case DataType::Enum: {
            uint64_t raw = bytes_to_uint(bytes, /*big_endian=*/true);
            for (auto &ev : item.values) {
                if (static_cast<uint64_t>(ev.raw) == raw) return ev.label;
            }
            return std::to_string(raw); // undocumented value: degrade, don't fail
        }

        case DataType::Raw:
            return to_hex(bytes);
    }
    return to_hex(bytes);
}

std::vector<uint8_t> Catalog::encode(const DataItem &item, const std::variant<std::string, double> &value) {
    switch (item.type) {
        case DataType::String: {
            if (!std::holds_alternative<std::string>(value)) {
                throw CatalogError("data item '" + item.id + "': expected a string value");
            }
            const std::string &s = std::get<std::string>(value);
            if (item.length > 0 && static_cast<int>(s.size()) > item.length) {
                throw CatalogError("data item '" + item.id + "': value exceeds max length " +
                                    std::to_string(item.length));
            }
            return std::vector<uint8_t>(s.begin(), s.end());
        }

        case DataType::Float: {
            if (!std::holds_alternative<double>(value)) {
                throw CatalogError("data item '" + item.id + "': expected a numeric value");
            }
            double raw_d = item.encoding.scale != 0.0 ? std::get<double>(value) / item.encoding.scale
                                                        : std::get<double>(value);
            double rounded = std::round(raw_d);

            uint64_t max_raw = (item.encoding.bytes >= 8) ? UINT64_MAX
                                                            : ((uint64_t{1} << (item.encoding.bytes * 8)) - 1);
            if (rounded < 0.0 || rounded > static_cast<double>(max_raw)) {
                throw CatalogError("data item '" + item.id + "': value out of range (0.." +
                                    std::to_string(static_cast<double>(max_raw) * item.encoding.scale) + ")");
            }
            return uint_to_bytes(static_cast<uint64_t>(rounded), item.encoding.bytes, item.encoding.endian == "big");
        }

        case DataType::Enum: {
            if (!std::holds_alternative<std::string>(value)) {
                throw CatalogError("data item '" + item.id + "': expected a string label");
            }
            const std::string &label = std::get<std::string>(value);
            for (auto &ev : item.values) {
                if (ev.label == label) {
                    // The schema has no width field for enum DIDs (Float's
                    // encoding.bytes has no Enum equivalent, and B1's own
                    // scope says "no new schema needed") -- every catalog
                    // in this project uses a single-byte enum DID, matching
                    // what the mock/uds_doip adapters actually read/write,
                    // so that's the one width encode() produces. A future
                    // multi-byte enum DID would need a real schema addition,
                    // not a guess baked in here.
                    return {static_cast<uint8_t>(ev.raw)};
                }
            }
            throw CatalogError("data item '" + item.id + "': '" + label + "' is not a valid value (see /docs)");
        }

        case DataType::Raw:
            if (!std::holds_alternative<std::string>(value)) {
                throw CatalogError("data item '" + item.id + "': expected a hex string value");
            }
            return from_hex(item.id, std::get<std::string>(value));
    }
    throw CatalogError("data item '" + item.id + "': unknown type");
}

} // namespace sovd::catalog
