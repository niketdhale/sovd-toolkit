#include "sovd/client/sovd_client.hpp"

#include <chrono>
#include <thread>

namespace sovd::client {

using json = nlohmann::json;

namespace {

DataValue parse_data_value(const json &d) {
    DataValue v;
    v.id = d.value("id", "");
    if (d.contains("did")) v.did = d["did"].get<std::string>();
    if (d.contains("value")) v.value = d["value"];
    if (d.contains("unit")) v.unit = d["unit"].get<std::string>();
    if (d.contains("error")) v.error = d["error"].get<std::string>();
    if (d.contains("message")) v.error_message = d["message"].get<std::string>();
    return v;
}

std::string join_ids(const std::vector<std::string> &ids) {
    std::string out;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i) out += ",";
        out += ids[i];
    }
    return out;
}

} // namespace

SovdClient::SovdClient(std::string base_url, RetryPolicy retry) : cli_(base_url), retry_(retry) {
    cli_.set_connection_timeout(3, 0);
    cli_.set_read_timeout(10, 0);
}

json SovdClient::request(const std::string &method, const std::string &url_path, const std::string &body,
                          const std::string &lock_id) {
    httplib::Headers headers;
    if (!lock_id.empty()) headers.emplace("X-SOVD-Lock-Id", lock_id);

    int backoff_ms = retry_.backoff_ms;
    for (int attempt = 0;; ++attempt) {
        httplib::Result res;
        if (method == "GET") {
            res = cli_.Get(url_path, headers);
        } else if (method == "POST") {
            res = cli_.Post(url_path, headers, body, "application/json");
        } else if (method == "PUT") {
            res = cli_.Put(url_path, headers, body, "application/json");
        } else if (method == "DELETE") {
            res = cli_.Delete(url_path, headers);
        } else {
            throw std::invalid_argument("unsupported method: " + method);
        }

        if (!res) {
            throw SovdError(0, "TRANSPORT", "request failed: " + httplib::to_string(res.error()));
        }

        // Retry only 503/504, never 423 -- retrying a lock conflict just
        // hammers whoever actually holds it (CLAUDE.md).
        if ((res->status == 503 || res->status == 504) && attempt < retry_.max_retries) {
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
            backoff_ms *= 2;
            continue;
        }

        if (res->status < 200 || res->status >= 300) {
            std::string code = "HTTP_" + std::to_string(res->status);
            std::string message = res->body;
            try {
                json err = json::parse(res->body);
                if (err.contains("error")) code = err["error"].get<std::string>();
                if (err.contains("message")) message = err["message"].get<std::string>();
            } catch (...) {
                // body wasn't the usual {"error","message"} shape -- keep the raw body as the message
            }
            throw SovdError(res->status, code, message);
        }

        if (res->body.empty()) return json::object();
        try {
            return json::parse(res->body);
        } catch (...) {
            throw SovdError(res->status, "BAD_RESPONSE", "server returned a non-JSON body");
        }
    }
}

std::vector<EntityInfo> SovdClient::list_entities() {
    json j = request("GET", "/v1/entities");
    std::vector<EntityInfo> out;
    for (auto &item : j.at("items")) {
        out.push_back({item.value("path", ""), item.value("type", ""), item.value("has_backend", false)});
    }
    return out;
}

DocsResult SovdClient::get_docs(const std::string &path) {
    json j = request("GET", "/v1/entities/" + path + "/docs");

    DocsResult out;
    out.path = j.value("path", path);
    out.type = j.value("type", "");
    out.has_backend = j.value("has_backend", false);

    if (j.contains("capabilities")) {
        const json &c = j["capabilities"];
        Capabilities cap;
        cap.supports_batch_read = c.value("supports_batch_read", false);
        cap.supports_async_operations = c.value("supports_async_operations", false);
        cap.supports_io_control = c.value("supports_io_control", false);
        out.capabilities = cap;
    }

    for (auto &d : j.value("data", json::array())) {
        DataItemDoc item;
        item.id = d.value("id", "");
        item.did = d.value("did", "");
        item.type = d.value("type", "");
        item.access = d.value("access", "");
        item.io_control = d.value("io_control", false);
        if (d.contains("requires_session")) item.requires_session = d["requires_session"].get<std::string>();
        if (d.contains("length")) item.length = d["length"].get<int>();
        if (d.contains("scale")) item.scale = d["scale"].get<double>();
        if (d.contains("unit")) item.unit = d["unit"].get<std::string>();
        if (d.contains("values")) item.values = d["values"];
        out.data.push_back(std::move(item));
    }

    for (auto &o : j.value("operations", json::array())) {
        OperationDoc op;
        op.id = o.value("id", "");
        op.routine_id = o.value("routine_id", "");
        op.async = o.value("async", false);
        if (o.contains("requires_session")) op.requires_session = o["requires_session"].get<std::string>();
        out.operations.push_back(std::move(op));
    }

    return out;
}

std::vector<FaultInfo> SovdClient::get_faults(const std::string &path, const std::string &status_filter) {
    std::string url = "/v1/entities/" + path + "/faults";
    if (!status_filter.empty()) url += "?status=" + status_filter;
    json j = request("GET", url);
    std::vector<FaultInfo> out;
    for (auto &f : j.at("faults")) out.push_back({f.value("code", ""), f.value("status", "")});
    return out;
}

void SovdClient::clear_faults(const std::string &path, const std::string &lock_id) {
    request("DELETE", "/v1/entities/" + path + "/faults", "", lock_id);
}

DataValue SovdClient::get_data(const std::string &path, const std::string &id) {
    return parse_data_value(request("GET", "/v1/entities/" + path + "/data/" + id));
}

std::vector<DataValue> SovdClient::get_data_batch(const std::string &path, const std::vector<std::string> &ids) {
    json j = request("GET", "/v1/entities/" + path + "/data?ids=" + join_ids(ids));
    std::vector<DataValue> out;
    for (auto &item : j.at("items")) out.push_back(parse_data_value(item));
    return out;
}

void SovdClient::put_data(const std::string &path, const std::string &id, const std::string &hex_value,
                           const std::string &lock_id) {
    request("PUT", "/v1/entities/" + path + "/data/" + id, json{{"value", hex_value}}.dump(), lock_id);
}

void SovdClient::set_mode(const std::string &path, const std::string &mode, const std::string &lock_id) {
    request("POST", "/v1/entities/" + path + "/modes", json{{"mode", mode}}.dump(), lock_id);
}

json SovdClient::execute_operation(const std::string &path, const std::string &op, const std::string &params_json,
                                    const std::string &lock_id) {
    return request("POST", "/v1/entities/" + path + "/operations/" + op, params_json.empty() ? "{}" : params_json,
                    lock_id);
}

std::string SovdClient::acquire_lock(const std::string &path, int ttl_seconds) {
    json j = request("POST", "/v1/entities/" + path + "/locks", json{{"ttl_seconds", ttl_seconds}}.dump());
    return j.at("lock_id").get<std::string>();
}

void SovdClient::renew_lock(const std::string &path, const std::string &lock_id, int ttl_seconds) {
    request("PUT", "/v1/entities/" + path + "/locks/" + lock_id, json{{"ttl_seconds", ttl_seconds}}.dump());
}

void SovdClient::release_lock(const std::string &path, const std::string &lock_id) {
    request("DELETE", "/v1/entities/" + path + "/locks/" + lock_id);
}

} // namespace sovd::client
