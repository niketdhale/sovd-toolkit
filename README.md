# sovd-toolkit

A modular **SOVD** (Service-Oriented Vehicle Diagnostics, ASAM / ISO 17978-3)
server and client stack for Linux, written in **C++17** with a C-ABI adapter
seam, a Vue 3 web UI, and a MQTT-fed security monitoring pipeline.

SOVD replaces the ECU-centric UDS diagnostic model with a REST/HTTP+JSON API.
This project implements the in-vehicle server side (gateway / domain HPC),
a client SDK + CLI, and the security-monitoring plumbing around it — the
shape of a production automotive diagnostics stack, built end to end at a
scale one person can finish and defend in an interview.

> Full phase-by-phase build log, every settled architectural decision (and
> why), and the complete design rationale live in [`CLAUDE.md`](CLAUDE.md).
> This README is the "what is this and how do I run it" front door.

---

## What it does

- Serves vehicle diagnostic data over plain HTTP+JSON instead of raw UDS —
  browse entities, read/write typed data points, read/clear DTCs, run
  diagnostic routines, and stream live values over SSE, all self-described
  through a `/docs` endpoint so a client never needs to hardcode a DID.
- Talks **real UDS/DoIP** to classic ECUs on the other side, translating
  every SOVD call into the matching UDS service (`0x22`/`0x2E`/`0x2F`
  ReadDataByIdentifier/WriteDataByIdentifier/IOControl, `0x19`/`0x14`
  ReadDTC/ClearDiagnosticInformation, `0x10` DiagnosticSessionControl,
  `0x31` RoutineControl, `0x27` SecurityAccess) — the ECUs themselves stay
  completely SOVD-unaware.
- Chains into a **multi-server topology**: a vehicle-level gateway proxies
  to per-domain servers (ADAS, body, …), each of which owns its own ECUs —
  matching how a real E/E architecture is actually laid out, not a single
  flat server pretending to be the whole vehicle.
- Feeds every security-relevant event (lock contention, auth failures,
  mode changes) into **MQTT → Telegraf → InfluxDB → Grafana**, because a
  diagnostic interface is a privileged interface and deserves the same
  monitoring as the CAN bus.
- Ships a **Vue 3 web UI** and a **CLI + C++ client SDK**, both built purely
  from server self-description — neither has any hardcoded knowledge of any
  specific ECU or DID.

## What it deliberately doesn't do

Software/firmware update orchestration, OTX runtime, async job polling —
all named non-goals. See CLAUDE.md's "Deliberate non-goals" for the reasoning;
the point of this project is architectural depth on a coherent subset, not
partial coverage of the entire SOVD spec.

---

## Architecture

### Role model

Classic ECUs speak plain UDS and have no idea SOVD exists. The gateway/domain
HPC is a protocol translator wearing two hats — an SOVD **server** toward the
tester, a UDS **client** toward the ECUs:

```
External tester  ──SOVD/HTTP──►  Gateway / Domain HPC
                                 ├─ SOVD server role (answers the tester)
                                 └─ UDS client role  (interrogates ECUs)
                                          │
                                     UDS/DoIP/CAN
                                          ▼
                                 Classic ECUs — SOVD-unaware, unchanged
```

### Multi-server topology

```
        Vehicle-level SOVD server (gateway) ── proxy-only, restricted build
                │                              (no ECU-facing adapters linked in)
        ┌───────┴───────┐
        ▼               ▼
  Domain server    Domain server      ── full capability, vehicle-internal
  (ADAS HPC)       (Body HPC)            network only
        │               │
   UDS/DoIP        UDS/DoIP
        ▼               ▼
   camera/radar     bcm/door_ctrl
```

The gateway never links the UDS/DoIP adapter at all — if the binary can't
emit a UDS frame, a compromised gateway can't reach the bus. Capability
reduction happens by **linkage**, not a runtime flag.

### Module layout

```
sovd-toolkit/
├── core/           pure logic: entity registry, lock manager, the C-ABI
│                   adapter vtable — NO HTTP, NO sockets, NO JSON
├── adapters/
│   ├── mock/       in-memory backend for tests and demos
│   └── uds_doip/   real UDS/DoIP client: DoIP framing, UDS service
│                   encode/decode, session management + 0x3E heartbeat,
│                   SecurityAccess (0x27), NRC → HTTP status mapping
├── catalog/        DID/operation YAML schema, decode()/encode() (bytes ⇄
│                   typed values) — shared by server/ and adapters/uds_doip
├── catalogs/        per-ECU catalog YAML files (data, not code)
├── config/          topology YAML for the multi-server demo
├── server/          HTTP layer (cpp-httplib + nlohmann/json): routing,
│                   CORS, OAuth2, mTLS, SSE streaming, MQTT event/telemetry
│                   sinks, config-driven topology loading
├── client/          C++ SDK: typed wrappers, RAII lock heartbeat, mDNS
│                   discovery, SSE subscription
├── cli/             sovd_cli — every SOVD operation, built on client/ only
├── monitoring/      Grafana dashboard + alert rules, Telegraf MQTT input
├── web/             Vue 3 + Vite + Tailwind UI, own npm project
├── tools/           sovd_mint_token — standalone OAuth2 demo-token minter
└── tests/           341+ assertions across three binaries, no external
                    test framework — fake_doip_server.hpp is an in-repo
                    fault-injecting DoIP fixture
```

**Layering rule:** `server/` translates HTTP ⇄ core calls and nothing more.
`core/` holds no I/O at all. Every backend-specific detail — real UDS,
mocked, proxied to another SOVD server — hides behind `core/include/sovd/adapter.h`,
a small C-ABI vtable. A NULL function pointer in that vtable is how a
reduced-capability build (e.g. a read-only gateway) declares what it doesn't
support, and the server turns that into a clean `501` automatically.

### The adapter seam, concretely

```c
// core/include/sovd/adapter.h (essence)
typedef struct {
    sovd_result_t (*read_data)(void* ctx, const char* path, ...);
    sovd_result_t (*write_data)(void* ctx, const char* path, ...);
    sovd_result_t (*read_faults)(void* ctx, const char* path, ...);
    sovd_result_t (*set_mode)(void* ctx, const char* path, const char* mode);
    sovd_result_t (*execute_operation)(void* ctx, const char* path, ...);
    /* ...NULL-able capability flags, NULL-able fn pointers... */
} sovd_vtable_t;
```

Three implementations exist behind this same interface: `adapters/mock`
(in-memory, for tests/demos), `adapters/uds_doip` (real DoIP transport +
UDS services against actual ECU hardware), and a Router-level HTTP-forwarding
table for proxying to a child SOVD server (not vtable-shaped — a proxy has
to pass through the child's already-decoded JSON, which the raw-bytes vtable
can't carry; see CLAUDE.md Phase 4 for why that plan changed mid-build).

### Request flow, end to end

```
GET /v1/entities/vehicle/body/bcm/data/battery_voltage
             └────────┬────────┘      └──────┬─────┘
               entity path            named data id
  routes.cpp → EntityRegistry.find() → Entity + adapter
  adapter → DID catalog: battery_voltage → DID 0x010A, 2 bytes BE, ×0.001, "V"
  session_manager: ensure correct UDS session on ECU 0x0E80 (+ 0x3E heartbeat)
  UDS over DoIP: 22 01 0A  →  62 01 0A 32 C8
  decode: 0x32C8 = 13000 × 0.001 = 13.0
  → {"id": "battery_voltage", "value": 13.0, "unit": "V"}
```

### Security posture

- **OAuth2 bearer tokens** at the HTTP boundary, scope-gated per route
  (`read:faults`, `read:data`, `execute:routines`, `execute:security_access`),
  **opt-in via `SOVD_OAUTH2_SECRET`** — unset in every config this repo
  ships, including the default demo. Default-deny: a route with no matching
  scope-table entry still requires *a* valid token, never a silent bypass.
- **Mutual TLS** between gateway and domain servers — both directions
  verified against a private demo CA, not the system trust store.
- **SecurityAccess (`0x27`)** gated on *both* what the ECU demands (catalog
  `requires_security_level`) and what the client's token is allowed to ask
  for (`execute:security_access` scope) — either alone isn't enough. This
  second half **only actually runs when OAuth2 is enabled**; with it off
  (every shipped config's default), a `requires_security_level` item is
  reachable by any caller who can reach the server at all — enable OAuth2
  (usage example 8) wherever that matters.
- **Default-deny route whitelist** at the gateway tier — a route added to
  the server without an explicit whitelist entry is unreachable through the
  gateway rather than silently exposed.
- **Resource limits** as a security property, not just robustness: entity
  tree depth cap, lock TTL ceiling, max concurrent locks (doubling as the
  UDS-session cap, since escalation is always lock-gated), max request body.
- **Audit log** — every security event optionally persisted to disk
  (`SOVD_AUDIT_LOG_PATH`), independent of and in addition to the MQTT feed.
- Every event carries a `correlation_id`, echoed in
  `X-SOVD-Correlation-Id` on every HTTP response, success or error.

See CLAUDE.md's Phase 8 section for the full list with live-verification
notes for each item.

---

## Build & test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4

./build/test_core                      # 542 assertions — core + server + HTTP
./build/test_client                    # 32 assertions — SDK/CLI, live server
cd build && ctest --output-on-failure
```

Build options (`cmake -S . -B build -D<option>=<ON|OFF>`):

| Option | Default | Adds |
|---|---|---|
| `SOVD_ADAPTER_MOCK` | `ON` | In-memory adapter; `test_core`/`test_client` only build when this is on |
| `SOVD_ADAPTER_UDS_DOIP` | `OFF` | Real UDS/DoIP adapter + `./build/test_uds_doip` (197 assertions, socket-level, against an in-repo fake DoIP server) |
| `SOVD_CLIENT_MDNS` | auto-detected | `_sovd._tcp.local` discovery via `avahi-client`, if present |

Turning `SOVD_ADAPTER_MOCK` off and `SOVD_ADAPTER_UDS_DOIP` on builds a
gateway-style binary with zero mock symbols and zero ECU-facing code paths
compiled out — the "restricted build" capability-reduction story above.

Builds clean under `-Wall -Wextra -Wpedantic` in every configuration listed.
Required system deps: OpenSSL (TLS + OAuth2 HMAC), yaml-cpp (fetched via
CMake `FetchContent`); `avahi-client` is optional. cpp-httplib and
nlohmann/json are vendored single-header (`third_party/`).

---

## Usage examples

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
trusting a forwarded external bearer token — see the mTLS section above);
a domain server that also demands a bearer token then rejects every
gateway-proxied request with a 401 it has no way to satisfy. The supported
split is **OAuth2 at the gateway, mTLS on the internal gateway↔domain hop**
— set `SOVD_OAUTH2_SECRET` on the gateway tier's config, not the domain
tier's, in a two-server topology.

---

## API surface (summary)

| Method | Path | Notes |
|---|---|---|
| GET | `/` | self-description, role, `api_versions` — unversioned, hit first |
| GET | `/v1/entities` | flat entity listing |
| GET/DELETE | `/v1/entities/{path}/faults` | optional `?status=` filter; DELETE is lock-gated |
| GET | `/v1/entities/{path}/data?ids=a,b,c` | batch read, partial failure doesn't fail the batch |
| GET/PUT | `/v1/entities/{path}/data/{id}` | catalog `id` or raw hex DID; PUT is lock-gated + typed JSON body |
| GET | `/v1/entities/{path}/data/{id}/stream?interval_ms=N` | SSE, one shared poller per (path, id, interval) |
| POST | `/v1/entities/{path}/modes` | lock-gated, UDS session control |
| POST | `/v1/entities/{path}/operations/{op}` | lock-gated, UDS RoutineControl |
| POST/PUT/DELETE | `/v1/entities/{path}/locks[/{lock_id}]` | acquire / renew (heartbeat) / release |
| GET | `/v1/entities/{path}/docs` | capability + data/operation self-description |

Full request/response shapes, error-code mapping, and every settled design
decision behind this surface: see `CLAUDE.md`.

---

## Status

All phases (0 through 8) are complete, tested (700+ assertions across three
binaries), and live-verified — including a real-browser click-through of
the web UI. See `CLAUDE.md`'s "STATUS AT A GLANCE" for the current build
health and phase-by-phase detail.
