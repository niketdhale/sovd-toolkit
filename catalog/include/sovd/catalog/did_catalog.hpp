// DID catalog — YAML -> typed data/operation definitions, shared between the
// server (Phase 1 self-description) and the UDS/DoIP adapter (Phase 2
// encode/decode). Deliberately independent of core/, server/, and any
// adapter: it's per-ECU-software-version data, not transport logic.
//
// No JSON here on purpose — server/ is the only layer that knows JSON
// exists; this module hands back plain C++ types.
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace sovd::catalog {

class CatalogError : public std::runtime_error {
public:
    explicit CatalogError(const std::string &msg) : std::runtime_error(msg) {}
};

enum class Access { Read, Write, ReadWrite };
std::string access_to_string(Access a);

enum class DataType { String, Float, Enum, Raw };
std::string data_type_to_string(DataType t);

struct Encoding {
    int bytes = 0;
    std::string endian = "big"; // "big" | "little"
    double scale = 1.0;
    std::string unit;
};

struct EnumValue {
    int raw = 0;
    std::string label;
};

struct DataItem {
    std::string id;
    uint16_t did = 0;
    DataType type = DataType::Raw;
    Access access = Access::Read;
    bool io_control = false; // true => 0x2F IOControl instead of 0x2E Write
    std::optional<std::string> requires_session;

    int length = 0;                  // type == String
    Encoding encoding;                // type == Float
    std::vector<EnumValue> values;    // type == Enum
};

struct Operation {
    std::string id;
    uint16_t routine_id = 0;
    std::optional<std::string> requires_session;
    bool async = false;
};

class Catalog {
public:
    static Catalog load_from_file(const std::string &path);
    static Catalog load_from_string(const std::string &yaml_text);

    const std::vector<DataItem> &data() const { return data_; }
    const std::vector<Operation> &operations() const { return operations_; }

    const DataItem *find_by_id(const std::string &id) const;
    const DataItem *find_by_did(uint16_t did) const;
    const Operation *find_operation(const std::string &id) const;

    // Decodes raw backend bytes into a human-facing value per item.type:
    // String -> std::string (the bytes as text), Float -> double (scaled),
    // Enum -> std::string (the matched label, or the raw number as text if
    // no label matches — an undocumented ECU value shouldn't turn into a
    // 500), Raw -> std::string (uppercase hex). Throws CatalogError only for
    // a genuine mismatch against the catalog (e.g. wrong byte count for a
    // Float item).
    static std::variant<std::string, double> decode(const DataItem &item, const std::vector<uint8_t> &bytes);

private:
    std::vector<DataItem> data_;
    std::vector<Operation> operations_;
};

} // namespace sovd::catalog
