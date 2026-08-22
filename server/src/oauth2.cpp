#include "sovd/server/oauth2.hpp"

#include <chrono>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "json.hpp"

namespace sovd::server::oauth2 {

using json = nlohmann::json;

namespace {

std::string base64url_encode(const unsigned char *data, size_t len) {
    std::string out(4 * ((len + 2) / 3) + 1, '\0');
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(&out[0]), data, static_cast<int>(len));
    out.resize(static_cast<size_t>(n));
    for (char &c : out) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!out.empty() && out.back() == '=') out.pop_back();
    return out;
}

std::string base64url_encode(const std::string &data) {
    return base64url_encode(reinterpret_cast<const unsigned char *>(data.data()), data.size());
}

// EVP_DecodeBlock's returned length includes the zero bytes implied by
// padding -- the well-known OpenSSL gotcha where the caller must trim one
// byte off the output for every '=' the (re-padded) input carried.
bool base64url_decode(const std::string &in, std::string &out) {
    std::string padded = in;
    for (char &c : padded) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (padded.size() % 4 != 0) padded.push_back('=');
    if (padded.empty()) return false;

    size_t pad_count = 0;
    for (auto it = padded.rbegin(); it != padded.rend() && *it == '='; ++it) pad_count++;

    out.assign(padded.size() / 4 * 3, '\0');
    int n = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(&out[0]),
                             reinterpret_cast<const unsigned char *>(padded.data()), static_cast<int>(padded.size()));
    if (n < 0 || static_cast<size_t>(n) < pad_count) return false;
    out.resize(static_cast<size_t>(n) - pad_count);
    return true;
}

std::string hmac_sha256(const std::string &secret, const std::string &data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char *>(data.data()), data.size(), digest, &digest_len);
    return std::string(reinterpret_cast<char *>(digest), digest_len);
}

// A timing side channel on signature comparison is exactly the kind of
// thing a project claiming security seriousness must not hand-wave.
bool constant_time_equal(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

long now_seconds() {
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

} // namespace

bool has_scope(const TokenClaims &claims, const std::string &scope) {
    for (auto &s : claims.scopes) {
        if (s == scope) return true;
    }
    return false;
}

std::string mint_token(const std::string &secret, const std::vector<std::string> &scopes, long ttl_seconds) {
    json header = {{"alg", "HS256"}, {"typ", "JWT"}};
    json payload = {{"scopes", scopes}, {"exp", now_seconds() + ttl_seconds}};

    std::string signing_input = base64url_encode(header.dump()) + "." + base64url_encode(payload.dump());
    std::string sig = hmac_sha256(secret, signing_input);
    return signing_input + "." + base64url_encode(reinterpret_cast<const unsigned char *>(sig.data()), sig.size());
}

bool verify_token(const std::string &token, const std::string &secret, TokenClaims &out) {
    size_t first_dot = token.find('.');
    if (first_dot == std::string::npos) return false;
    size_t second_dot = token.find('.', first_dot + 1);
    if (second_dot == std::string::npos) return false;

    std::string signing_input = token.substr(0, second_dot);
    std::string sig_b64 = token.substr(second_dot + 1);

    std::string expected_sig = hmac_sha256(secret, signing_input);
    std::string expected_sig_b64 =
        base64url_encode(reinterpret_cast<const unsigned char *>(expected_sig.data()), expected_sig.size());
    if (!constant_time_equal(sig_b64, expected_sig_b64)) return false;

    // SOVD_REVIEW_FEEDBACK.md Task 6: not currently exploitable -- this
    // verifier always recomputes HS256 unconditionally and never branches
    // on the header, so alg:none / RS256-vs-HS256 confusion (both of which
    // require the verifier to *switch* on alg) don't apply today. Checked
    // anyway, after the signature (never before it -- see the ordering note
    // above), as defense in depth: a future refactor that adds real
    // algorithm selection can't silently reintroduce that bug class if this
    // is already here.
    std::string header_b64 = token.substr(0, first_dot);
    std::string header_json;
    if (!base64url_decode(header_b64, header_json)) return false;
    json header;
    try {
        header = json::parse(header_json);
    } catch (...) {
        return false;
    }
    if (!header.contains("alg") || header["alg"] != "HS256") return false;

    std::string payload_b64 = token.substr(first_dot + 1, second_dot - first_dot - 1);
    std::string payload_json;
    if (!base64url_decode(payload_b64, payload_json)) return false;

    json payload;
    try {
        payload = json::parse(payload_json);
    } catch (...) {
        return false;
    }
    if (!payload.contains("exp") || !payload["exp"].is_number_integer()) return false;
    if (!payload.contains("scopes") || !payload["scopes"].is_array()) return false;

    long exp = payload["exp"].get<long>();
    if (now_seconds() >= exp) return false;

    out.exp = exp;
    out.scopes.clear();
    for (auto &s : payload["scopes"]) {
        if (s.is_string()) out.scopes.push_back(s.get<std::string>());
    }
    return true;
}

} // namespace sovd::server::oauth2
