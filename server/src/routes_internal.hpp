// Private to server/src: helpers shared by the routes*.cpp translation units.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "json.hpp"
#include "sovd/catalog/did_catalog.hpp"
#include "sovd/entity_registry.hpp"
#include "sovd/server/routes.hpp"

namespace sovd::server::detail {

using json = nlohmann::json;

void emit_event(const Router::EventSink &sink, const std::string &type, json fields);
int http_status_for(sovd_result_t r);
void write_error(httplib::Response &res, int status, const std::string &code, const std::string &message);
std::string to_hex(const uint8_t *data, size_t len);
std::string to_hex_u16(uint16_t v);
bool from_hex(std::string s, std::vector<uint8_t> &out);

// Shared by the single-item and batch data-read handlers: resolves id_or_did
// against the catalog (if any), calls the adapter, and decodes. Does not touch
// `res` -- callers decide how to report a failure.
struct DataReadOutcome {
    bool ok = false;
    int status = 200;
    std::string error_code;
    std::string message;
    json body; // populated when ok
};

DataReadOutcome read_one_data_item(const Entity &e, const std::string &path, const std::string &id_or_did,
                                    const catalog::Catalog *cat);

} // namespace sovd::server::detail
