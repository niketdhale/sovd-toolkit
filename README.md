# sovd-toolkit

[![CI](https://github.com/niketdhale/sovd-toolkit/actions/workflows/ci.yml/badge.svg)](https://github.com/niketdhale/sovd-toolkit/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Platform: Linux](https://img.shields.io/badge/platform-Linux-lightgrey.svg)

A modular **SOVD** (Service-Oriented Vehicle Diagnostics, ASAM / ISO 17978-3)
server and client stack for Linux — C++17, a C-ABI adapter seam, real UDS/DoIP
translation, a Vue 3 web UI, and an MQTT-fed security monitoring pipeline.

SOVD replaces the ECU-centric UDS diagnostic model with a REST/HTTP+JSON API.
This project implements the in-vehicle server side (gateway / domain HPC), a
client SDK and CLI, and the security plumbing around them — the shape of a
production automotive diagnostics stack, built end to end with security design
treated as a first-class requirement rather than an afterthought.

**Standards touched:** ASAM / ISO 17978-3 (SOVD) · ISO 14229 / ISO 13400 (UDS,
DoIP) · ISO/SAE 21434 (see [`docs/TARA.md`](docs/TARA.md)) · UN-R155 / R156
(see [`docs/COMPLIANCE.md`](docs/COMPLIANCE.md))

<!-- TODO: screenshot of the Live Chart / Data Table screen goes here -->
![Web UI — live data streaming over SSE](docs/images/web-ui-live-chart.png)

> Design rationale, the phase-by-phase build log, and every settled
> architectural decision (with the reasoning, including the ones that changed
> mid-build) live in [`docs/DESIGN.md`](docs/DESIGN.md). This README is the
> "what is this and how do I run it" front door.

---

## What it does

- Serves vehicle diagnostic data over plain HTTP+JSON instead of raw UDS —
  browse entities, read/write typed data points, read/clear DTCs, run
  diagnostic routines, and stream live values over SSE, all self-described
  through a `/docs` endpoint so a client never needs to hardcode a DID.
- Talks **real UDS/DoIP** to classic ECUs on the other side, translating every
  SOVD call into the matching UDS service (`0x22`/`0x2E`/`0x2F`, `0x19`/`0x14`,
  `0x10`, `0x31`, `0x27`) — the ECUs themselves stay completely SOVD-unaware.
- Chains into a **multi-server topology**: a vehicle-level gateway proxies to
  per-domain servers (ADAS, body, …), each owning its own ECUs — matching how a
  real E/E architecture is laid out, not one flat server pretending to be the
  whole vehicle.
- Feeds every security-relevant event (lock contention, auth failures, mode
  changes) into **MQTT → Telegraf → InfluxDB → Grafana**, because a diagnostic
  interface is a privileged interface and deserves the same monitoring as the
  CAN bus.
- Ships a **Vue 3 web UI** and a **CLI + C++ client SDK**, both built purely
  from server self-description — neither has hardcoded knowledge of any
  specific ECU or DID.

## What it deliberately doesn't do

Software/firmware update orchestration, OTX runtime, and async job polling are
named non-goals. The reasoning is in `docs/DESIGN.md` — briefly, architectural
depth on a coherent subset is worth more than partial coverage of the whole
SOVD spec, and update orchestration in particular pulls in UN-R156 / ISO 24089
scope that deserves its own project rather than a stub here.

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

The gateway never links the UDS/DoIP adapter at all — if the binary cannot emit
a UDS frame, a compromised gateway cannot reach the bus. Capability reduction
happens by **linkage**, not a runtime flag.

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
├── catalogs/       per-ECU catalog YAML files (data, not code)
├── config/         topology YAML for the multi-server demo
├── server/         HTTP layer (cpp-httplib + nlohmann/json): routing, CORS,
│                   OAuth2, mTLS, SSE streaming, MQTT event/telemetry sinks,
│                   config-driven topology loading
├── client/         C++ SDK: typed wrappers, RAII lock heartbeat, mDNS
│                   discovery, SSE subscription
├── cli/            sovd_cli — every SOVD operation, built on client/ only
├── monitoring/     Grafana dashboard + alert rules, Telegraf MQTT input
├── web/            Vue 3 + Vite + Tailwind UI, own frontend project
├── tools/          sovd_mint_token — standalone OAuth2 demo-token minter
└── tests/          700+ assertions across three binaries, no external test
                    framework — fake_doip_server.hpp is an in-repo
                    fault-injecting DoIP fixture
```

**Layering rule:** `server/` translates HTTP ⇄ core calls and nothing more.
`core/` holds no I/O at all. Every backend-specific detail — real UDS, mocked,
proxied to another SOVD server — hides behind `core/include/sovd/adapter.h`, a
small C-ABI vtable. A NULL function pointer in that vtable is how a
reduced-capability build declares what it doesn't support, and the server turns
that into a clean `501` automatically.

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

Two implementations sit behind this interface: `adapters/mock` (in-memory) and
`adapters/uds_doip` (real DoIP transport + UDS services). Proxying to a child
SOVD server is handled instead by a Router-level HTTP-forwarding table — a
proxy has to pass through the child's already-decoded JSON, which a raw-bytes
vtable cannot carry. `docs/DESIGN.md` (Phase 4) covers why that plan changed
mid-build.

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

---

## Security posture

Threats, mitigations, and a threat-to-code traceability table are in
[`docs/TARA.md`](docs/TARA.md) (ISO/SAE 21434 clause 9 structure). Mapping to
UN-R155 Annex 5 threat identifiers is in
[`docs/COMPLIANCE.md`](docs/COMPLIANCE.md). Vulnerability disclosure:
[`SECURITY.md`](SECURITY.md).

- **OAuth2 bearer tokens** at the HTTP boundary, scope-gated per route
  (`read:faults`, `read:data`, `execute:routines`, `execute:security_access`),
  **opt-in via `SOVD_OAUTH2_SECRET`** — unset in every config this repo ships.
  Default-deny: a route with no matching scope-table entry still requires *a*
  valid token, never a silent bypass.
- **Mutual TLS** between gateway and domain servers — both directions verified
  against a private demo CA, not the system trust store.
- **SecurityAccess (`0x27`)** gated on *both* what the ECU demands (catalog
  `requires_security_level`) and what the client's token is allowed to ask for
  (`execute:security_access` scope) — either alone isn't enough. The second half
  **only runs when OAuth2 is enabled**; with it off (every shipped config's
  default), a `requires_security_level` item is reachable by any caller who can
  reach the server at all. Enable OAuth2 wherever that matters.
- **Default-deny route whitelist** at the gateway tier — a route added to the
  server without an explicit whitelist entry is unreachable through the gateway
  rather than silently exposed.
- **Resource limits** as a security property, not just robustness: entity tree
  depth cap, lock TTL ceiling, max concurrent locks (doubling as the UDS-session
  cap, since escalation is always lock-gated), max request body.
- **Audit log** — every security event optionally persisted to disk
  (`SOVD_AUDIT_LOG_PATH`), independent of and in addition to the MQTT feed.
- Every event carries a `correlation_id`, echoed in `X-SOVD-Correlation-Id` on
  every HTTP response, success or error.

### Diagnostic data and GDPR

A VIN identifies a vehicle and, in practice, its keeper — under GDPR that makes
VIN and much diagnostic telemetry personal data. Two consequences are visible in
this codebase: the audit log and MQTT event feed carry a `correlation_id` rather
than a VIN wherever an identifier is only needed for correlation, and
`docs/COMPLIANCE.md` records what each sink retains and why. A production
deployment would need a retention policy and a lawful basis per data category;
that analysis is out of scope here but the hooks are deliberately in the right
places.

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
compiled in — the "restricted build" capability-reduction story above.

Builds clean under `-Wall -Wextra -Wpedantic` in every configuration listed, and
CI covers all four adapter combinations. Required system deps: OpenSSL (TLS +
OAuth2 HMAC), yaml-cpp (fetched via CMake `FetchContent`); `avahi-client` is
optional. cpp-httplib and nlohmann/json are vendored single-header
(`third_party/`). A CycloneDX SBOM is generated per CI run and attached to the
build artifacts.

---

## Quick start

### 1. Zero-config demo server

```bash
./build/sovd_server 20002 domain
curl http://127.0.0.1:20002/v1/entities
curl http://127.0.0.1:20002/v1/entities/vehicle/body/bcm/docs
curl "http://127.0.0.1:20002/v1/entities/vehicle/body/bcm/data?ids=vin,battery_voltage,door_lock_state"
```

### 2. CLI — discovery-driven, no hardcoded paths or DIDs

```bash
./build/sovd_cli http://127.0.0.1:20002 entities
./build/sovd_cli http://127.0.0.1:20002 docs  vehicle/body/bcm
./build/sovd_cli http://127.0.0.1:20002 read  vehicle/body/bcm battery_voltage
./build/sovd_cli http://127.0.0.1:20002 write vehicle/body/bcm door_lock_state unlocked
./build/sovd_cli http://127.0.0.1:20002 watch vehicle/body/bcm battery_voltage 1000
./build/sovd_cli discover                       # mDNS, needs avahi-daemon
```

### 3. Two-tier gateway + domain topology

```bash
./build/sovd_server config/domain_body.yaml &   # domain tier, :20003
./build/sovd_server config/gateway.yaml   &     # gateway tier, :20002, proxy-only

curl http://127.0.0.1:20002/v1/entities/vehicle/body/bcm/data/battery_voltage
```

Mutual TLS, OAuth2, SSE streaming, the web UI, the MQTT monitoring pipeline, and
running the UDS/DoIP adapter against a real DoIP target are all covered in
[`docs/USAGE.md`](docs/USAGE.md).

---

## API surface

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

Full request/response shapes and error-code mapping: `docs/DESIGN.md`.

---

## Status & roadmap

All eight build phases are complete and tested — 700+ assertions across three
binaries, plus a real-browser click-through of the web UI. Two full external
code-review cycles have been folded in; the findings and fixes are recorded in
[`docs/reviews/`](docs/reviews/).

Current work, in order:

- [ ] Live verification against a real DoIP target on `vcan0`, with the captured
      session published here
- [ ] ISO/SAE 21434 clause-9 TARA with threat-to-code traceability
- [ ] UN-R155 Annex 5 control mapping
- [ ] Rendered architecture diagram replacing the ASCII ones above

---

## License

MIT — see [`LICENSE`](LICENSE).
