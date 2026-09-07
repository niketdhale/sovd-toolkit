# Usage examples

The main [`README.md`](../README.md)'s Quick start covers the three most
common flows in simplified form. This is the full set — every one of these
has been curl/CLI-verified live against a real running server (see
`docs/DESIGN.md`'s per-phase write-ups for the verification detail).

### 1. Zero-config demo server

```bash
./build/sovd_server 20002 domain
curl http://127.0.0.1:20002/v1/entities
curl http://127.0.0.1:20002/v1/entities/vehicle/body/bcm/docs
curl "http://127.0.0.1:20002/v1/entities/vehicle/body/bcm/data?ids=vin,battery_voltage,door_lock_state"
```

### 2. CLI (client SDK, discovery-driven — no hardcoded paths/DIDs anywhere)

```bash
./build/sovd_cli http://127.0.0.1:20002 entities
./build/sovd_cli http://127.0.0.1:20002 docs vehicle/body/bcm
./build/sovd_cli http://127.0.0.1:20002 read vehicle/body/bcm battery_voltage
./build/sovd_cli http://127.0.0.1:20002 write vehicle/body/bcm door_lock_state unlocked
./build/sovd_cli http://127.0.0.1:20002 watch vehicle/body/bcm battery_voltage 1000   # SSE-backed live stream
./build/sovd_cli discover                                                            # mDNS, needs avahi-daemon
```

### 3. Two-tier gateway + domain topology

```bash
./build/sovd_server config/domain_body.yaml &   # domain tier, :20003, mock- or uds_doip-backed
./build/sovd_server config/gateway.yaml   &     # gateway tier, :20002, proxy-only, no ECU adapters linked

curl http://127.0.0.1:20002/v1/entities/vehicle/body/bcm/data/battery_voltage   # typed data through the gateway
```

`config/domain_body_mtls.yaml` + `config/gateway_mtls.yaml` are the same
pair with mutual TLS turned on — regenerate demo certs first with
`scripts/generate_demo_certs.sh` (writes to `certs/`, gitignored).

### 4. Real UDS/DoIP adapter

```bash
cmake -S . -B build -DSOVD_ADAPTER_UDS_DOIP=ON
cmake --build build -j4
./build/test_uds_doip     # socket-level tests against an in-repo fault-injecting DoIP fixture
```
Point a topology YAML's entity `adapter: { kind: uds_doip, logical_address, gateway_ip, port, did_catalog }`
at a real ECU / DoIP simulator to use it live.

### 5. Live streaming (SSE)

```bash
curl -N "http://127.0.0.1:20003/v1/entities/vehicle/body/bcm/data/battery_voltage/stream?interval_ms=1000"
```

### 6. Web UI

```bash
./build/sovd_server config/domain_body.yaml &   # cors_allowed_origins already includes :5173
cd web && npm install && npm run dev
# open http://localhost:5173, enter the server's base URL, click Connect
```
Four screens: Entity Browser, Fault Viewer (read + filter + clear), Data
Table (typed read/write widgets built purely from `/docs` — an enum
`<select>`, a numeric input with its unit, a length-capped text field,
nothing hardcoded per-entity), Live Chart (SSE + a hand-rolled SVG polyline).

**Combined with OAuth2** (the two are independent examples above; this is
what running them together actually looks like):
```bash
SOVD_OAUTH2_SECRET=demo-secret ./build/sovd_server config/domain_body_auth.yaml &
./build/sovd_mint_token demo-secret read:data,execute:routines,read:faults 3600
cd web && npm run dev
# open http://localhost:5173, paste the minted token into the token field
# next to the base URL, click Connect
```
The token field sends `Authorization: Bearer <token>` on every request. The
live-chart screen can't do that itself — a browser `EventSource` has no way
to set request headers — so it mints a short-lived, single-use ticket
through a normal bearer-authenticated `POST .../stream-ticket` call first
and opens the stream with that instead; this happens automatically the
moment a token is configured, nothing to do differently on that screen.

### 7. Security monitoring

```bash
# broker + dashboards assumed running (see monitoring/)
SOVD_MQTT_HOST=127.0.0.1 SOVD_OAUTH2_SECRET=demo-secret ./build/sovd_server 20002 domain
mosquitto_sub -t 'sovd/#' -v
```
Import `monitoring/grafana/dashboards/sovd_security.json` and provision
`monitoring/grafana/provisioning/alerting/sovd_alerts.yaml` for the session
timeline / lock contention / auth failure panels and alert rules; Telegraf
input example in `monitoring/telegraf/sovd_mqtt_input.conf.example`.

### 8. OAuth2-protected server

```bash
SOVD_OAUTH2_SECRET=demo-secret ./build/sovd_server config/domain_body_auth.yaml
TOKEN=$(./build/sovd_mint_token demo-secret read:data,execute:routines 3600)
curl -H "Authorization: Bearer $TOKEN" http://127.0.0.1:20003/v1/entities/vehicle/body/bcm/data
```
OAuth2 has **no YAML config key at all** — it's `SOVD_OAUTH2_SECRET` or
nothing, same as `SOVD_MQTT_HOST` and `SOVD_AUDIT_LOG_PATH`. A secret has no
business sitting in a topology file that might get committed or handed to
someone debugging an unrelated issue. `config/domain_body_auth.yaml` is the
identical topology to `config/domain_body.yaml`, kept as a separate file for
exactly this reason — see its own header comment.

**One topology this project deliberately doesn't support**: setting
`SOVD_OAUTH2_SECRET` on a **domain**-tier server sitting behind a gateway
proxy. The proxy forwards `X-SOVD-Lock-Id` but never `Authorization`
(mTLS on that hop verifies the gateway's own certificate instead of
trusting a forwarded external bearer token — see the mTLS section in
`docs/DESIGN.md`); a domain server that also demands a bearer token then
rejects every gateway-proxied request with a 401 it has no way to satisfy.
The supported split is **OAuth2 at the gateway, mTLS on the internal
gateway↔domain hop** — set `SOVD_OAUTH2_SECRET` on the gateway tier's
config, not the domain tier's, in a two-server topology.
