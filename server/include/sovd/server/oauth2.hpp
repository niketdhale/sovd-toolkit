// Phase 8: bearer-token + scope validation at the external boundary.
// Deliberately NOT a full OAuth2 authorization server (auth-code flow,
// client registration, refresh tokens) -- issuing tokens is a real IdP's
// job and out of scope for what this project demonstrates (SOVD
// diagnostics, not identity infrastructure); *verifying* them is the
// resource server's actual responsibility, which is what protects the
// diagnostic interface. Self-issued HMAC-SHA256 signed tokens
// (header.payload.signature, base64url each part -- a minimal JWT), not a
// full JWT library dependency: same call this project already made for
// MQTT (Phase 3) -- hand-roll a minimal protocol slice when a full
// implementation would be disproportionate, and a resource server that only
// ever verifies tokens it mints itself doesn't need algorithm negotiation.
#pragma once

#include <string>
#include <vector>

namespace sovd::server::oauth2 {

struct TokenClaims {
    std::vector<std::string> scopes;
    long exp = 0; // unix seconds
};

bool has_scope(const TokenClaims &claims, const std::string &scope);

// Verifies signature + expiry and extracts claims. Returns false on any
// failure (malformed token, bad signature, expired) without distinguishing
// which -- a resource server doesn't need to tell a caller *why* their
// token was rejected beyond "unauthorized". The header's "alg" field is
// never read back to select verification behavior (always HS256) --
// trusting an attacker-supplied alg is the classic JWT algorithm-confusion
// bug; this scheme has no such surface because there's only ever one
// algorithm to begin with.
bool verify_token(const std::string &token, const std::string &secret, TokenClaims &out);

// Testing/demo helper: mints a token for a given secret+scopes+ttl. A real
// deployment gets tokens from an actual IdP; this exists so the
// verification mechanism is exercisable (tests, live curl) without one.
std::string mint_token(const std::string &secret, const std::vector<std::string> &scopes, long ttl_seconds);

} // namespace sovd::server::oauth2
