#!/usr/bin/env bash
# Task 9 (docs/reviews/round-1.md; docs/DESIGN.md's Working conventions): runs
# the cross-feature combinations the review's closing note named, not just
# each feature in isolation. Task 1 (OAuth2 breaking the whole web UI) was
# exactly this failure mode -- Phase 7 and Phase 8's OAuth2 were each
# verified honestly on their own, and the combination was never tried. This
# is a smoke/integration script, not a unit test suite: it boots real
# `sovd_server` processes and curls them, meant to be re-run before calling
# a change done, not on every keystroke.
#
# Usage:
#   scripts/cross_phase_check.sh                    # everything, incl. the
#                                                     # 4-way build matrix
#                                                     # (a few minutes)
#   scripts/cross_phase_check.sh --skip-build-matrix # just the runtime
#                                                     # combinations (fast,
#                                                     # assumes the default
#                                                     # config is already
#                                                     # built)
set -uo pipefail
cd "$(dirname "$0")/.."

PASS=0
FAIL=0
SERVER_PIDS=()

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

start_server() {
    # start_server <log-file> <command...>
    local log="$1"
    shift
    "$@" >"$log" 2>&1 &
    SERVER_PIDS+=("$!")
}

stop_all() {
    for pid in "${SERVER_PIDS[@]:-}"; do
        kill "$pid" >/dev/null 2>&1 || true
    done
    wait >/dev/null 2>&1 || true
    SERVER_PIDS=()
    sleep 0.3
}
trap stop_all EXIT

wait_for_port() {
    local port="$1"
    for _ in $(seq 1 50); do
        curl -s -o /dev/null "http://127.0.0.1:$port/" && return 0
        sleep 0.1
    done
    return 1
}

status_of() { curl -s -o /dev/null -w '%{http_code}' "$@"; }

BIN=./build/sovd_server
MINT=./build/sovd_mint_token
NPROC=$(nproc 2>/dev/null || echo 4)

if [ "${1:-}" != "--skip-build-matrix" ]; then
    echo "== Build matrix: restricted-build x each config (Task 5, B3) =="
    for combo in "ON:OFF" "OFF:ON" "ON:ON" "OFF:OFF"; do
        mock="${combo%%:*}"
        uds="${combo##*:}"
        label="MOCK=$mock UDS_DOIP=$uds"
        if cmake -S . -B build -DSOVD_ADAPTER_MOCK="$mock" -DSOVD_ADAPTER_UDS_DOIP="$uds" \
            >/tmp/cross_phase_cmake.log 2>&1 &&
            cmake --build build -j"$NPROC" >/tmp/cross_phase_build.log 2>&1; then
            if grep -qi "warning" /tmp/cross_phase_build.log; then
                fail "$label builds but with warnings (see /tmp/cross_phase_build.log)"
            else
                pass "$label builds clean"
            fi
        else
            fail "$label failed to build (see /tmp/cross_phase_cmake.log, /tmp/cross_phase_build.log)"
        fi
    done
    # Every section below assumes the default (mock-only) build.
    cmake -S . -B build -DSOVD_ADAPTER_MOCK=ON -DSOVD_ADAPTER_UDS_DOIP=OFF >/dev/null 2>&1
    cmake --build build -j"$NPROC" >/dev/null 2>&1
else
    echo "== Build matrix skipped (--skip-build-matrix) =="
fi

if [ ! -x "$BIN" ]; then
    echo "build/sovd_server not found -- build the default config first (see README)"
    exit 1
fi

echo "== UI x auth (Task 1): OAuth2 must not break the web UI's own request patterns =="
start_server /tmp/cross_phase_domain_auth.log env SOVD_OAUTH2_SECRET=demo-secret "$BIN" config/domain_body_auth.yaml
wait_for_port 20003 || fail "domain_body_auth.yaml server never came up"
[ "$(status_of http://127.0.0.1:20003/v1/entities)" = "401" ] &&
    pass "no token -> 401, not a silent pass-through" || fail "no token should be 401"
[ "$(status_of -X OPTIONS -H 'Origin: http://localhost:5173' \
    -H 'Access-Control-Request-Headers: authorization' -H 'Access-Control-Request-Method: GET' \
    http://127.0.0.1:20003/v1/entities)" = "204" ] || fail "CORS preflight should be 204"
if curl -s -D - -o /dev/null -X OPTIONS -H 'Origin: http://localhost:5173' \
    -H 'Access-Control-Request-Headers: authorization' -H 'Access-Control-Request-Method: GET' \
    http://127.0.0.1:20003/v1/entities | grep -qi "access-control-allow-headers.*authorization"; then
    pass "preflight lists Authorization back (Task 1a)"
else
    fail "preflight must list Authorization or a browser blocks the real request (Task 1a)"
fi
TOKEN=$("$MINT" demo-secret read:data,execute:routines,read:faults 3600)
[ "$(curl -s -H "Authorization: Bearer $TOKEN" -o /dev/null -w '%{http_code}' \
    http://127.0.0.1:20003/v1/entities/vehicle/body/bcm/data/battery_voltage)" = "200" ] &&
    pass "valid token -> 200" || fail "valid token should reach data"
# -d "" (not a bare -X POST): curl sends neither Content-Length nor
# Transfer-Encoding for a truly bodiless POST, and httplib's server then
# waits out its ~5s read timeout deciding there's no body to read before
# proceeding -- a real, measured curl/httplib interaction, not a server
# bug (a POST with an actual body, e.g. every httplib::Client::Post call in
# test_core.cpp, never hits it). -d "" sends Content-Length: 0 and avoids it.
TICKET_RESP=$(curl -s -X POST -d "" -H "Authorization: Bearer $TOKEN" \
    http://127.0.0.1:20003/v1/entities/vehicle/body/bcm/data/battery_voltage/stream-ticket)
TICKET=$(printf '%s' "$TICKET_RESP" | sed -n 's/.*"ticket":"\([^"]*\)".*/\1/p')
STREAM_CODE=$(status_of --max-time 2 \
    "http://127.0.0.1:20003/v1/entities/vehicle/body/bcm/data/battery_voltage/stream?interval_ms=200&ticket=$TICKET")
[ "$STREAM_CODE" = "200" ] && pass "stream ticket mint + redeem -> 200 (Task 1c, what EventSource actually does)" ||
    fail "stream ticket flow broken (got $STREAM_CODE)"
stop_all

echo "== UI x gateway (D1 boundary): streaming through a proxy stays a clean 501 =="
start_server /tmp/cross_phase_domain.log "$BIN" config/domain_body.yaml
wait_for_port 20003 || fail "domain_body.yaml server never came up"
start_server /tmp/cross_phase_gateway.log "$BIN" config/gateway.yaml
wait_for_port 20002 || fail "gateway.yaml server never came up"
[ "$(status_of http://127.0.0.1:20002/v1/entities/vehicle/body/bcm/data/battery_voltage)" = "200" ] &&
    pass "typed read through the gateway" || fail "gateway proxy read broken"
[ "$(status_of --max-time 2 'http://127.0.0.1:20002/v1/entities/vehicle/body/bcm/data/battery_voltage/stream?interval_ms=200')" = "501" ] &&
    pass "stream through the gateway -> 501, not silently mis-forwarded" || fail "gateway stream should be 501"
stop_all

echo "== auth x proxy (Task 4): supported (gateway-tier) vs. documented dead end (domain-tier) =="
start_server /tmp/cross_phase_domain2.log "$BIN" config/domain_body.yaml
wait_for_port 20003 || fail "domain server never came up (auth x proxy, supported case)"
start_server /tmp/cross_phase_gateway_auth.log env SOVD_OAUTH2_SECRET=demo-secret "$BIN" config/gateway.yaml
wait_for_port 20002 || fail "gateway (auth) never came up"
[ "$(status_of http://127.0.0.1:20002/v1/entities/vehicle/body/bcm/data/battery_voltage)" = "401" ] &&
    pass "gateway-tier OAuth2 gates proxied requests without a token" ||
    fail "gateway auth should 401 an unauthenticated proxied request"
TOKEN2=$("$MINT" demo-secret read:data 3600)
[ "$(curl -s -H "Authorization: Bearer $TOKEN2" -o /dev/null -w '%{http_code}' \
    http://127.0.0.1:20002/v1/entities/vehicle/body/bcm/data/battery_voltage)" = "200" ] &&
    pass "gateway-tier OAuth2 + a valid token still reaches the domain through the proxy" ||
    fail "gateway auth + valid token should succeed"
stop_all

start_server /tmp/cross_phase_domain_auth2.log env SOVD_OAUTH2_SECRET=demo-secret "$BIN" config/domain_body_auth.yaml
wait_for_port 20003 || fail "domain (auth) never came up (dead-end check)"
start_server /tmp/cross_phase_gateway2.log "$BIN" config/gateway.yaml
wait_for_port 20002 || fail "gateway never came up (dead-end check)"
DEAD_END_CODE=$(status_of http://127.0.0.1:20002/v1/entities/vehicle/body/bcm/data/battery_voltage)
[ "$DEAD_END_CODE" != "200" ] &&
    pass "domain-tier OAuth2 behind an unauthenticated gateway can't succeed (documented dead end, got $DEAD_END_CODE)" ||
    fail "domain-tier OAuth2 unexpectedly succeeded through the proxy -- Task 4's documented dead end may have silently changed"
stop_all

echo "== mTLS x proxy: both directions of mutual TLS through the gateway =="
if [ ! -f certs/ca.crt ]; then
    echo "  (no certs/ yet -- generating demo certs first)"
    ./scripts/generate_demo_certs.sh >/dev/null 2>&1
fi
start_server /tmp/cross_phase_domain_mtls.log "$BIN" config/domain_body_mtls.yaml
sleep 0.5 # HTTPS-only listener; wait_for_port's plain-HTTP probe doesn't apply
start_server /tmp/cross_phase_gateway_mtls.log "$BIN" config/gateway_mtls.yaml
wait_for_port 20012 || fail "gateway_mtls.yaml server never came up"
[ "$(status_of http://127.0.0.1:20012/v1/entities/vehicle/body/bcm/data/battery_voltage)" = "200" ] &&
    pass "typed read through an mTLS-proxied gateway" || fail "mTLS proxy read broken"
stop_all

echo
echo "== $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
