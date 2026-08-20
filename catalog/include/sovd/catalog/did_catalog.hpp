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
    // Phase 8 (D2): what the ECU demands, parallel to requires_session --
    // an odd UDS SecurityAccess requestSeed level (ISO 14229-1), e.g. 1 for
    // "level 1" (0x01 requestSeed / 0x02 sendKey). Gating on this is one
    // half of D2; the other half (whether *this client* is permitted to
    // request that level) is an OAuth2 scope check in routes.cpp -- this
    // field only ever answers "what does the ECU require," never "who's
    // allowed to ask for it."
    std::optional<int> requires_security_level;

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

    // B1 (Phase 7 blocker): the inverse of decode() — a human-facing typed
    // value in, wire bytes out. Takes the same variant shape decode()
    // returns, for the same reason decode() returns it: String/Enum/Raw are
    // std::string (Enum = the label, Raw = uppercase hex, matching decode()
    // exactly), Float is double (the already-scaled human value, e.g.
    // 13.0 for a battery_voltage write). Throws CatalogError — mapped to
    // HTTP 400 by the caller, never reaching the adapter — for: the wrong
    // variant alternative for item.type, an enum label not present in
    // item.values (no raw-number fallback on write, unlike decode()'s
    // read-side fallback: silently accepting an undocumented value on
    // write is a real correctness hazard decode()'s read-side leniency
    // isn't), a scaled float value that doesn't fit in encoding.bytes, a
    // string longer than item.length (when length > 0), or invalid hex for
    // a Raw item.
    static std::vector<uint8_t> encode(const DataItem &item, const std::variant<std::string, double> &value);

private:
    std::vector<DataItem> data_;
    std::vector<Operation> operations_;
};

} // namespace sovd::catalog
