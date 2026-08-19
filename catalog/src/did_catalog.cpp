#include "sovd/catalog/did_catalog.hpp"

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

} // namespace sovd::catalog
