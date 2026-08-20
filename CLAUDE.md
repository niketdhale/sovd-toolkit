# CLAUDE.md — sovd-toolkit

Context file for Claude Code. Read this first before touching the repo.

---

## What this project is

A modular **SOVD (Service-Oriented Vehicle Diagnostics, ASAM / ISO 17978-3)**
server and client stack for Linux, written in C++17 with a C-ABI adapter seam.

SOVD replaces the ECU-centric UDS model with a REST/HTTP+JSON API. This project
implements the in-vehicle server side (gateway / domain HPC) plus a client SDK.

**Guiding constraint:** build the *shape* of a production architecture at a
scale one person can finish. A partial implementation of the full spec is worth
less than a complete implementation of a coherent subset.

### Owner context
Automotive cybersecurity engineer (AUTOSAR SecOC, IDS/IDSM pipelines,
ISO/SAE 21434, WP.29/UN-R155 CSMS, ECU pentesting, DoIP/SOME/IP fuzzing).
This is a portfolio + interview project, so *architectural seriousness matters
more than feature count*. Existing assets to reuse:
- `DoIP_ECU_Simulator` — target for the Phase 2 UDS/DoIP adapter
- `autosar-idsm-toolkit` — the IDS pipeline this feeds in Phase 3
- IDSM pipeline already built: MQTT → Telegraf → InfluxDB → Grafana OSS
- Vue 3 + Vite + Tailwind experience (from the Verso project) → reuse in Phase 7

---

## Role model (get this right — it is commonly inverted)

```
External tester  ──SOVD/HTTP──►  Gateway / Domain HPC
                                 ├─ SOVD server role (answers the tester)
                                 └─ UDS client role  (interrogates ECUs)
                                          │
                                     UDS/DoIP/CAN
                                          ▼
                                 Classic ECUs — SOVD-unaware, unchanged
```

- **Classic ECUs have NO SOVD client and no SOVD awareness.** They speak plain
  UDS. The gateway is a protocol translator wearing two hats.
- **Nodes that run SOVD servers:** gateway/central HPC, domain HPCs (ADAS,
  body, etc.), and native SDV apps on HPCs.
- **The only in-vehicle SOVD *client*** is the vehicle-level gateway acting as
  a client toward domain servers — i.e. the `sovd_proxy` adapter (Phase 4).
  It is upstream-facing, never on the ECUs.
- The server never calls an API on the client. Client always initiates.
  (Streaming exists but is client-subscribed.)

### Multi-server topology (Phase 4 target)
```
        Vehicle-level SOVD server (gateway) ── "public", proxy-only,
                │                              restricted build
        ┌───────┴───────┐
        ▼               ▼
  Domain server    Domain server      ── "private", full capability,
  (ADAS HPC)       (Body HPC)            vehicle-internal network only
        │               │
   UDS/DoIP        UDS/DoIP
        ▼               ▼
   camera/radar     bcm/door_ctrl
```

---

## Architectural decisions (SETTLED — do not relitigate mid-build)

| Decision | Choice | Reasoning |
|---|---|---|
| Language | C++17, C-ABI at adapter seam | RAII/containers for server; portable ABI where suppliers plug in |
| Adapter loading | **Static**, CMake configure-time | `dlopen` on a privileged in-vehicle process is a code-exec primitive; simplifies cross-compile + static analysis |
| Deployment model | **One binary, role by config** | Domain vs gateway differ only in bound adapters |
| Gateway build | Compiled **without** ECU-facing adapters | If it cannot emit a UDS frame, compromise cannot reach the bus. Capability reduction by linkage > runtime checks |
| Lock authority | At the server **owning** the entity | Two servers both believing they hold a lock during a flash is a hazard |
| Client config | Connection only — **never** entity paths or DIDs | Those come from `/entities` + `/docs`. A DID list in client config = rebuilding ODX distribution |
| DID catalog | Separate file from topology | Topology is per-vehicle; catalogs are per-ECU-software-version, shared across vehicles |
| Catalog `id` scope | Per-ECU, **not** cached at proxy | Caching diagnostic reads is wrong — the point is current vehicle state |
| Streaming | SSE, not WebSocket, initially | One-directional, simpler, proxy-friendly |
| OTX | **Deferred, possibly forever** | No mature OSS runtime; ISO 13209-2 interpreter is a project in itself |
| API versioning | **Path prefix** `/v1/`, not `Accept` header | Uglier but unambiguous — trivial to add now, breaks every deployed tester later if skipped. `/` itself stays unversioned (version-discovery: a client hits it first to learn `api_versions` before it knows which prefix to use); everything else is under `/v1/`. Settled 2026-08-19, implemented in `server/src/routes.cpp`. |
| Session manager ownership | **Object per-adapter, lifetime per-lock** | Not strictly either option as originally framed — see below. Settled 2026-08-19. |

### Deliberate non-goals
- Software update / flash orchestration (large, demonstrates nothing new)
- Async long-running operations with job polling
- OTX runtime (especially Scenario B server-side triggering)
- Full spec coverage of every SOVD resource class

### Session manager ownership — resolved
The `SessionManager` *object* is owned per-adapter-instance (one per ECU,
created alongside the adapter context, tracking whatever sessions that ECU's
DIDs/operations need) — not per-lock, since a per-lock object would mean
recreating heartbeat-thread machinery on every lock acquisition for no
benefit. What actually varies with the lock is the session's **operational
state** (default vs. escalated):

- Session escalation only ever happens from a **lock-gated** call
  (`write_data`, `set_mode`, `execute_operation`) checking the DID/operation's
  catalog `requires_session` field — never from an unlocked `read_data`. This
  isn't just convenience: escalating an ECU's session as a side effect of an
  *unauthenticated-feeling* read would be a real gap in a safety-adjacent
  interface, so reads always run in whatever session already happens to be
  active and never request escalation themselves.
- Teardown is driven by the **existing `set_mode` vtable hook** — no new
  vtable surface needed. `routes.cpp`'s `handle_delete_lock` calls
  `vtable->set_mode(ctx, path, "default")` on a successful release, and the
  adapter's `set_mode("default")` implementation is what actually reverts the
  UDS session and stops the heartbeat. `core/`/`server/` never learn a UDS
  session exists; they're just calling the same mode-change entry point a
  client could call directly.
- A silently **expired lock** (TTL elapses, no explicit `DELETE /locks`) has
  no event to hook — `LockManager` doesn't push expiry notifications. The
  session manager instead runs its own idle-timeout safety net: if no
  lock-gated call touches a given ECU's session within a configured window,
  it reverts to default and stops heartbeating on its own. This mirrors how
  real ECUs already behave (UDS S3 session timeout) rather than inventing new
  cross-module plumbing for a case the protocol already handles.

This resolves the tension in the original framing ("unlocked read-only
requests need a transient session path"): they don't need one, because they
never escalate in the first place.

---

## Repo layout

```
sovd-toolkit/
├── CMakeLists.txt          # static adapters via configure-time options
├── README.md
├── CLAUDE.md               # this file
├── core/                   # pure logic: NO HTTP, NO sockets, NO JSON
│   ├── include/sovd/
│   │   ├── adapter.h           # ★ THE SEAM — C-ABI vtable
│   │   ├── entity_registry.hpp
│   │   └── lock_manager.hpp
│   └── src/{adapter.c, entity_registry.cpp, lock_manager.cpp}
├── adapters/
│   ├── mock/               # in-memory, extern "C" only (supplier pattern)
│   └── uds_doip/           # real UDS/DoIP backend, built via -DSOVD_ADAPTER_UDS_DOIP=ON
│       ├── include/sovd/uds_doip/
│       │   ├── doip_protocol.hpp    # DoIP header/payload framing, pure logic
│       │   ├── doip_transport.hpp   # TCP transport: routing activation, send/ack/receive
│       │   ├── uds_services.hpp     # UDS request/response encode/decode, pure logic
│       │   ├── nrc_map.hpp          # NRC -> sovd_result_t
│       │   ├── session_manager.hpp  # session escalation + 0x3E heartbeat
│       │   └── uds_doip_adapter.h   # extern "C" vtable accessor (supplier pattern)
│       └── src/{same names}.cpp
├── catalog/                 # DID/operation YAML defs — shared by server & adapters
│   ├── include/sovd/catalog/did_catalog.hpp
│   └── src/did_catalog.cpp
├── catalogs/                 # actual per-ECU catalog YAML files (data, not code)
│   └── bcm.yaml
├── config/                  # Phase 4: topology YAML for the two-tier demo
│   ├── domain_body.yaml         # domain tier, port 20003, mock-backed
│   └── gateway.yaml             # gateway tier, port 20002, sovd_proxy-backed
├── server/                 # C++ HTTP layer — NO diagnostic logic
│   ├── include/sovd/server/{routes.hpp, config_loader.hpp, mqtt_publisher.hpp, mdns_advertise.hpp, stream_hub.hpp}
│   └── src/{main.cpp, routes.cpp, config_loader.cpp, mqtt_publisher.cpp, mdns_advertise.cpp, stream_hub.cpp}
├── client/                  # Phase 5: SDK — typed wrappers, RAII locks, mDNS discovery
│   ├── include/sovd/client/{sovd_client.hpp, lock_guard.hpp, mdns_discovery.hpp}
│   └── src/{sovd_client.cpp, lock_guard.cpp, mdns_discovery.cpp}
├── cli/                      # Phase 5: sovd_cli, built on client/ only (no server/ dep)
│   └── src/main.cpp
├── monitoring/              # Phase 3: Grafana dashboard/alerts, Telegraf input
│   ├── grafana/dashboards/sovd_security.json
│   ├── grafana/provisioning/alerting/sovd_alerts.yaml
│   └── telegraf/sovd_mqtt_input.conf.example
├── tests/
│   ├── test_framework.hpp     # minimal harness, no external dep
│   ├── test_core.cpp          # 342 assertions (mock path, default build)
│   ├── test_client.cpp        # 32 assertions — SovdClient/LockGuard/subscribe_data vs. a real live server
│   ├── fake_doip_server.hpp   # in-repo fault-injecting DoIP/UDS test fixture
│   └── test_uds_doip.cpp      # 180 assertions, built only when SOVD_ADAPTER_UDS_DOIP=ON
└── third_party/            # vendored single headers
    ├── httplib.h           # cpp-httplib v0.18.3 (MIT)
    └── json.hpp            # nlohmann/json v3.11.3 (MIT)
```

avahi-client (LGPL 2.1, dynamically linked) is an optional build-time
dependency for mDNS (`SOVD_CLIENT_MDNS`, auto-detected via `pkg_check_modules`)
— not vendored, not required: every other target builds and passes with it
off.

yaml-cpp (MIT) is a build-time dependency fetched via CMake `FetchContent`
(pinned to `yaml-cpp-0.9.0`), not vendored as a single header — the catalog
schema below uses flow-style YAML (`{ bytes: 2, ... }`), which ruled out
header-only minimal parsers (mini-yaml was tried first and dropped: no
flow-style support at all).

**Layering rule:** `server/` translates HTTP ⇄ core calls and nothing more.
`core/` holds no I/O. All backend complexity hides behind `adapter.h`.
`catalog/` is data/logic only (no HTTP, no adapter-specific behavior) —
shared between `server/` (Phase 1 `/docs`, named data paths) and
`adapters/uds_doip/` (Phase 2: each adapter instance loads its own `Catalog`
to decide session escalation and `0x2E`-vs-`0x2F` dispatch — a second,
independent use of the same module for a different question, not a shared
runtime instance; see Phase 2 below).

---

## Build & test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
./build/test_core                      # 342 assertions
./build/test_client                    # 32 assertions (Phase 5 SDK, always built)
./build/sovd_server 20002 domain       # port, role — hardcoded zero-config demo
cd build && ctest --output-on-failure
```

Phase 5 CLI, same server:
```bash
./build/sovd_cli http://127.0.0.1:20002 entities
./build/sovd_cli http://127.0.0.1:20002 docs vehicle/body/bcm
./build/sovd_cli http://127.0.0.1:20002 read vehicle/body/bcm battery_voltage
./build/sovd_cli http://127.0.0.1:20002 watch vehicle/body/bcm battery_voltage 1000   # Phase 6: SSE-backed, not polling
./build/sovd_cli discover                          # mDNS; needs SOVD_CLIENT_MDNS + a running avahi-daemon
```

Phase 6 raw SSE, same server (`interval_ms` is the server's poll cadence,
shared by every subscriber at that value — see StreamHub):
```bash
curl -N "http://127.0.0.1:20002/v1/entities/vehicle/body/bcm/data/battery_voltage/stream?interval_ms=1000"
```

Phase 4: `./sovd_server <path-to-existing-file>` loads a YAML topology
instead — the two are told apart by whether the argument is an openable
file, not a flag. Two-tier demo, from the repo root (relative
`did_catalog:`/catalog paths resolve against CWD):
```bash
./build/sovd_server config/domain_body.yaml &   # domain tier, :20003
./build/sovd_server config/gateway.yaml &       # gateway tier, :20002
curl http://127.0.0.1:20002/v1/entities/vehicle/body/bcm/data/battery_voltage
```

Adapter selection at **configure** time:
```bash
cmake -S . -B build -DSOVD_ADAPTER_MOCK=ON -DSOVD_ADAPTER_UDS_DOIP=OFF
```

With the DoIP adapter on, a second test binary builds and runs socket-level
tests against the in-repo fake DoIP server:
```bash
cmake -S . -B build -DSOVD_ADAPTER_UDS_DOIP=ON
cmake --build build -j4
./build/test_uds_doip                  # 180 assertions
```
`SOVD_ADAPTER_MOCK=OFF` alongside `SOVD_ADAPTER_UDS_DOIP=ON` builds the
adapter library and its tests fine on their own, but currently breaks
`sovd_server`/`test_core` — they call `sovd_mock_adapter_vtable()`
unconditionally regardless of the flag. Pre-existing since Phase 0, not a
Phase 2 regression; matches Phase 8's still-unbuilt "restricted gateway
build (no ECU adapters linked)" item.

Builds clean under `-Wall -Wextra -Wpedantic` with zero warnings in every
configuration above. Keep it that way.

---

## Phase 0 — COMPLETE ✅

Verified: clean warning-free build, 245 assertions passing (`test_core`),
end-to-end HTTP run hitting every expected status code (curl against a live
`sovd_server`).

- [x] `EntityRegistry` — hierarchical paths, parent validation, orphan rejection
- [x] `LockManager` — TTL, **injectable clock** (no sleeping in tests)
- [x] `adapter.h` C-ABI vtable — **NULL fn-ptr ⇒ automatic `501 UNSUPPORTED`**
      (how a read-only/restricted build declares reduced capability with no
      runtime config)
- [x] `adapters/mock` — C++ internally, `extern "C"` externally
- [x] Routes, error mapping, lock gating, structured event emission
- [x] Verified behaviours: lock conflict → 423, unheadered write on locked
      entity → 423, wrong lock-id release → 403, grouping node → 501,
      unknown entity → 404

### Current API surface
| Method | Path | Notes |
|---|---|---|
| GET | `/` | self-description, role, `api_versions` — deliberately unversioned |
| GET | `/v1/entities` | flat listing with `has_backend` |
| GET | `/v1/entities/{path}/faults` | optional `?status=confirmed\|pending\|testFailed` filter |
| DELETE | `/v1/entities/{path}/faults` | lock-gated |
| GET | `/v1/entities/{path}/data?ids=a,b,c` | batch read; per-item failure doesn't fail the batch |
| GET | `/v1/entities/{path}/data/{id}` | catalog `id` (e.g. `battery_voltage`) if attached, else raw hex DID |
| PUT | `/v1/entities/{path}/data/{id}` | lock-gated; same `id`-or-DID resolution; wire value is still hex bytes even for a named `id` (catalog `encode()` not built yet) |
| GET | `/v1/entities/{path}/data/{id}/stream?interval_ms=N` | Phase 6: SSE, shared poller per `(path,id,interval_ms)`; `501` if the entity is proxied (Phase 4) — streaming through a proxy isn't supported |
| POST | `/v1/entities/{path}/modes` | lock-gated, UDS session control |
| POST | `/v1/entities/{path}/operations/{op}` | lock-gated, UDS RoutineControl |
| POST | `/v1/entities/{path}/locks` | returns `lock_id` |
| PUT | `/v1/entities/{path}/locks/{lock_id}` | Phase 5: renews TTL without changing `lock_id` — what the client SDK's RAII lock heartbeat calls |
| DELETE | `/v1/entities/{path}/locks/{lock_id}` | |
| GET | `/v1/entities/{path}/docs` | capability description from the attached catalog (empty `data`/`operations` if none attached); works on any entity, not just ones with a backend |

`{path}` is a full multi-segment entity path (`vehicle/body/bcm`).
Lock-gated ops require `X-SOVD-Lock-Id` header when a lock is held.
Every request gets an `X-SOVD-Correlation-Id` response header — echoed if
supplied, generated otherwise — regardless of success or error status.
A named `/data/{id}` GET returns a catalog-decoded typed `value` (string,
scaled number, or enum label) plus `unit` when applicable; the raw-DID
fallback returns hex, same as Phase 0. PUT to a catalog `id` with
`access: read` is rejected with 400 before it reaches the adapter.

### Error mapping (implemented)
`NOT_FOUND`→404 · `LOCKED`→423 · `BAD_REQUEST`→400 · `UNSUPPORTED`→501 ·
`BUSY`→503 · `TRANSPORT`/`NEGATIVE_RESPONSE`→502 · `INTERNAL`→500

### Demo topology (hardcoded in `main.cpp`, replaced by config in Phase 4)
```
vehicle
├── adas   (area, no backend → 501 on data ops)
│   ├── camera_ecu
│   └── radar_ecu
└── body   (area, no backend)
    ├── bcm
    └── door_ctrl
```

### Carry-over bug from an earlier attempt — already avoided
An earlier pass at this codebase called adapters with the **leaf** id
(`bcm`) instead of the full registry path (`vehicle/body/bcm`), which would
have broken the Phase 4 proxy adapter. This rebuild passes the **full path**
to every `sovd_vtable_t` call from day one (`server/src/routes.cpp`); each
adapter decides what to do with it. Nothing left to fix here — just don't
regress it when Phase 2/4 adapters are added.

---

## Phase 1 — Self-description & usability — COMPLETE ✅

Verified: clean warning-free build, 245 assertions passing (`test_core`),
plus curl smoke tests against a live `sovd_server` for every item below
(batch read reproducing the exit criteria exactly, `/docs`, named paths,
fault filtering, correlation-id echo/generation on both success and error
responses).

- [x] **`sovd_capability_t`** extension to the adapter vtable — each backend
      declares what it supports. A static field on `sovd_vtable_t`
      (`supports_batch_read`, `supports_async_operations`,
      `supports_io_control`), not a callback — same "declare via linkage,
      not a runtime query" philosophy as the NULL-fn-ptr capability check.
      Mock declares all three `false` (it's honest: fully synchronous, no
      native multi-DID read, no distinct IOControl path). Surfaced in
      `GET .../docs` as a `capabilities` object when the entity has a
      backend at all.
- [x] **DID catalog parser** — YAML → typed definitions (`type`, `encoding`,
      `scale`, `unit`, enum `values`, `access`, `io_control`,
      `requires_session`). `catalog/` module, backed by yaml-cpp. Includes
      catalog-driven `decode()` (bytes → typed value: string/float-scaled/
      enum-label).
- [x] **`GET /entities/{path}/docs`** — capability description, serialized from
      the catalog. Works on any entity (not just ones with a backend or a
      catalog) — it's self-description, not a live diagnostic call, so it
      returns 200 with empty `data`/`operations` rather than 501. Deviates
      from the original phrasing above (which implied 501-on-no-backend like
      every other endpoint); this was a deliberate call, not an oversight.
- [x] **Named data paths** — `/data/battery_voltage` primary, raw hex DID as
      fallback. `Router` owns a `path -> Catalog` map (`attach_catalog()`),
      kept separate from `EntityRegistry`/topology per the settled "DID
      catalog separate from topology" decision. `catalogs/bcm.yaml` is
      attached to `vehicle/body/bcm` in `main.cpp`; other demo entities have
      no catalog and just fall back to raw hex, same as Phase 0. PUT still
      takes hex bytes over the wire regardless of `id` vs DID — catalog
      `encode()` (typed value → bytes) remains unbuilt; nothing needs it yet.
- [x] **Batch data read** — `GET /v1/entities/{path}/data?ids=a,b,c`. Shares
      the single-item resolve/decode logic (`read_one_data_item` helper in
      `routes.cpp`) with `GET .../data/{id}`. Partial failure doesn't fail
      the whole batch — one bad id returns an inline `{"id","error"}` entry
      alongside the successful ones, same graceful-degradation spirit as
      Phase 4's "one unreachable ECU must not fail the whole entity listing."
- [x] **Fault filtering** — `?status=confirmed|pending|testFailed`; invalid
      value is 400, valid value with no matching faults is 200 + empty
      array. Mock now seeds two faults with different statuses so this is
      actually exercised (was one before).
- [x] **API versioning** — settled: path prefix (`/v1/`), see the settled
      decisions table above. `/` stays unversioned for version discovery.
- [x] **Correlation IDs** — `X-SOVD-Correlation-Id`: echoed if the client
      supplies one, generated (thread-local random, not a counter — a
      counter is predictable and collides across restarts and eventually
      across gateway/domain-HPC instances) if absent. Stamped on every
      response, success or error. Threaded into every emitted structured
      event as `correlation_id`. Emission itself is now pluggable
      (`Router::set_event_sink`, defaults to stdout) — added for testability
      here, but it's also exactly the seam Phase 3's MQTT transport needs;
      no rework expected when that lands.

**Exit criteria — met:** `GET /v1/entities/vehicle/body/bcm/data?ids=vin,battery_voltage,door_lock_state`
against the live demo server returns all three, typed and decoded, in one
call — a client that has never seen the vehicle before can enumerate
entities (`/v1/entities`), discover every readable value with its type and
unit (`/docs`), and read them in one batched call.

### Target config schemas (Phase 1 catalog / Phase 4 topology)

Topology:
```yaml
server:
  id: sovd-body-domain
  port: 20003
  role: domain          # domain | gateway
entities:
  - path: vehicle
    type: vehicle       # no adapter → grouping node
  - path: vehicle/body
    type: area
  - path: vehicle/body/bcm
    type: component
    adapter:
      kind: uds_doip
      logical_address: 0x0E80
      gateway_ip: 192.168.1.10
      port: 13400
      did_catalog: catalogs/bcm.yaml
```

Gateway tier — identical schema, only adapter kind changes:
```yaml
  - path: vehicle/body
    type: area
    adapter:
      kind: sovd_proxy
      base_url: http://192.168.1.20:20003
      remote_path: vehicle/body
      forward_locks: true   # NEVER cache lock state locally
```

DID catalog (separate file, per-ECU-software-version):
```yaml
data:
  - id: vin
    did: 0xF190
    type: string
    length: 17
    access: read
  - id: battery_voltage
    did: 0x010A
    type: float
    encoding: { bytes: 2, endian: big, scale: 0.001, unit: V }
    access: read
  - id: door_lock_state
    did: 0x0200
    type: enum
    values: { 0: unlocked, 1: locked, 2: deadlocked }
    access: read_write
    io_control: true          # use 0x2F not 0x2E
operations:
  - id: self_test
    routine_id: 0x0203
    requires_session: extended
    async: true
```

Client config — connection only, **no paths, no DIDs**:
```yaml
connection:
  base_url: https://192.168.1.10:20002
  discovery: mdns              # _sovd._tcp.local
auth:
  mode: oauth2                 # oauth2 | mtls | none
  scopes: [read:faults, read:data, execute:routines]
client:
  requester_id: workshop-tester-42
  lock_ttl_seconds: 60
  lock_heartbeat: true         # auto-refresh at ttl/2
  retry: { on: [503, 504], max: 3, backoff_ms: 200 }
  # deliberately absent: 423 — retrying a lock conflict hammers another tester
```

### Path mapping, end to end
```
GET /entities/vehicle/body/bcm/data/battery_voltage
             └────────┬────────┘      └──────┬─────┘
               entity path            named data id
  route regex → registry.find() → Entity + adapter
  adapter → did_catalog: battery_voltage → DID 0x010A, 2B BE, ×0.001, "V"
  session_manager: ensure extended session on 0x0E80 (+ 0x3E heartbeat)
  UDS: 22 01 0A  →  62 01 0A 32 C8
  decode: 0x32C8 = 13000 × 0.001 = 13.0
  → {"id": "battery_voltage", "value": 13.0, "unit": "V"}
```

---

## Phase 2 — UDS/DoIP adapter (real hardware path) — COMPLETE ✅

Verified: clean warning-free build (both `-DSOVD_ADAPTER_UDS_DOIP=ON` and the
default mock-only config — the two don't interfere with each other, and
`SOVD_ADAPTER_UDS_DOIP=ON`/`SOVD_ADAPTER_MOCK=OFF` builds and links the
adapter fine in isolation too), 425 total assertions passing across
`test_core` (245) and `test_uds_doip` (180, only built/run when the option is
on), including a true end-to-end test: real HTTP → `server/routes.cpp` → the
real `uds_doip` adapter → the fake DoIP server, reproducing the Phase 1
worked example (`battery_voltage` → 13.0 V) through production code instead
of the mock, plus a full lock → write (IOControl) → operation (session
escalation) → release (session teardown) sequence over real HTTP.

All complexity hides behind the vtable; `core/` and route handlers never learn
UDS exists.

```
adapters/uds_doip/
├── doip_protocol      — DoIP header/payload framing, pure logic, no sockets
├── doip_transport     — TCP socket, routing activation, message send/ack/receive
├── uds_services        — UDS request/response encode/decode, pure logic
├── session_manager    — per-ECU session state + 0x3E TesterPresent heartbeat
├── nrc_map            — UDS negative response → sovd_result_t
└── uds_doip_adapter   — wires the above + a Catalog instance into sovd_vtable_t
```

Not literally the file layout originally sketched (`did_catalog` isn't a
separate file here — the adapter loads its own `sovd::catalog::Catalog`
instance via `catalog/`, already built in Phase 1, rather than duplicating
that logic) and `uds_services`/`doip_protocol` weren't in the original sketch
at all — pure encode/decode logic split out from the transport so it's
unit-testable without a socket, same instinct as the rest of this project.

### SOVD → UDS service mapping — implemented as documented, with two corrections
| SOVD request | UDS service |
|---|---|
| `GET /data/{id}` | `0x22` ReadDataByIdentifier |
| `PUT /data/{id}` | `0x2E` WriteDataByIdentifier, or `0x2F` IOControlByIdentifier if the catalog's `io_control: true` |
| `GET /faults` | `0x19` ReadDTCInformation (sub `0x02`) |
| `DELETE /faults` | `0x14` ClearDiagnosticInformation |
| `POST /modes` | `0x10` DiagnosticSessionControl — also how `routes.cpp` tears a session down on lock release (`set_mode(ctx, path, "default")`), not just a client-visible operation |
| `POST /operations/{id}` | `0x31` RoutineControl (`Start` only — `RequestResults`-backed async polling is a stated non-goal, see below) |
| software update | `0x34`/`0x36`/`0x37` (deferred, unchanged) |
| `POST /locks` | **nothing** — pure SOVD concept |
| *(none)* | `0x3E` TesterPresent — heartbeat thread, **not** suppressed-response: the DoIP-level ack alone isn't a liveness signal, and the transport already waits for a follow-up message regardless, so a real `0x7E` back is free and doubles as a dead-ECU check |

### The four hard problems
1. **Stateless HTTP vs stateful UDS sessions** — done, but the actual shape
   is "object per-adapter, lifetime per-lock," not literally "keyed off the
   SOVD lock" as first framed — see "Session manager ownership — resolved"
   above for the full reasoning and why unlocked reads never escalate.
2. **`0x27` SecurityAccess** — the *mechanism* is done and tested in
   isolation (`uds_services`: requestSeed/sendKey encode/decode, plus
   `derive_key_DEMO_ONLY_NOT_SECURE` — a clearly-labeled stand-in, since real
   UDS key derivation is OEM-proprietary and secret). **Not wired into the
   adapter's live read/write path** — deliberately, not an oversight: there
   is no HTTP scope concept anywhere in this codebase yet (Phase 8, unbuilt)
   to translate from, and the catalog schema has no field analogous to
   `requires_session` for "requires this security level" to trigger it from.
   Wiring it up for real means picking one of those two things first, which
   is a design decision bigger than "finish Phase 2," not a buried
   implementation detail to decide here.
3. **Semantic gap** — Phase 1's catalog-driven `decode()` already handles
   scaling/endianness/enums; Phase 2's addition is dispatching `0x2E` vs
   `0x2F` off the catalog's `io_control` flag, which is real and tested.
4. **`0x78` response-pending** — done: a bounded retry loop (`send_and_
   resolve` in `uds_doip_adapter.cpp`, capped at 10 attempts) waits for
   follow-up diagnostic messages with no new request sent, matching how a
   real ECU actually behaves after `0x78`. Capped rather than unbounded — a
   perpetually-pending ECU is itself a fault worth surfacing (as `SOVD_BUSY`)
   rather than hanging the caller forever.
   **Correction to the original framing:** "ISO-TP multi-frame ... below the
   SOVD line" doesn't apply to a DoIP-over-TCP client. ISO-TP (ISO 15765-2)
   segments messages for CAN's 8-byte frames; DoIP's own length-prefixed
   framing already carries arbitrarily-sized messages natively over TCP, and
   ISO-TP segmentation (if it happens at all) is internal to the physical
   gateway when it relays onto the CAN bus — transparent to a DoIP client.
   `doip_transport` does need correct length-prefixed TCP reassembly (built,
   tested against truncated/malformed frames), just not ISO-TP itself.

### NRC → HTTP
`0x33` securityAccessDenied → 403 · `0x31` requestOutOfRange → 400 ·
`0x22` conditionsNotCorrect → 409 · `0x78` responsePending → handled
internally, **never surfaced** · timeout/no response → 502 — implemented in
`nrc_map`, extended to the rest of the standard NRC table (`0x11`/`0x12`→501,
`0x13`→400, `0x24`→409, `0x35`→403, `0x36`/`0x37`→503, unenumerated→500).
**Required extending `sovd_result_t`** (`core/include/sovd/adapter.h`): the
Phase 0 enum had no way to produce 403 or 409 at all — added `SOVD_FORBIDDEN`
and `SOVD_CONFLICT`. A real, necessary, backward-compatible seam extension,
not scope creep — the mock never needed this vocabulary, real UDS does.

### Testing prerequisite — done, with one clarification
**Build a fault-injecting DoIP simulator BEFORE the session manager.** Done in
that order. `tests/fake_doip_server.hpp` is a minimal in-repo test fixture
(routing activation, configurable request→response table, and injectable
fault modes: no-response, malformed frame, truncated frame, NRC storm,
response-pending storm, routing-activation-denied) — speaking the same
`doip_protocol` framing the real transport uses, so it can't drift from what
the client actually parses. **This is not `DoIP_ECU_Simulator`** — that's a
separate repo of the owner's that wasn't available to this build; the fixture
exists so the "build the simulator first" constraint is satisfiable without
it. If `DoIP_ECU_Simulator` is later extended with equivalent fault
injection, it's a straightforward drop-in replacement for socket-level
testing — `doip_transport`'s tests only depend on it speaking correct DoIP
framing, not on anything fixture-specific.

### What's deliberately not built
- Multi-DID `ReadDataByIdentifier` (UDS supports requesting several DIDs in
  one `0x22` message) — Phase 1's batch read already works correctly against
  this adapter, just as N separate requests; a real optimization, not
  attempted here.
- `RequestResults`-backed async routine polling — matches the project's
  stated non-goal on async job polling.
- SecurityAccess wired into the live path — see hard problem #2 above.
- Wiring this adapter into `main.cpp`'s demo topology — there's no live (or
  appropriate-to-ship-alongside) DoIP target for the default `sovd_server`
  binary to point at. Phase 4's config loader is the natural place a real
  deployment wires this in against a real or per-deployment-configured
  target.

---

## Phase 3 — Security monitoring (THE DIFFERENTIATOR) — COMPLETE ✅

Nobody ships SOVD with native IDS integration. Most differentiated part of the
project. Diagnostic interfaces are privileged *by design*, so they belong under
the same monitoring as the CAN bus.

Verified: clean warning-free build, 271 assertions passing (`test_core`, up
from 245 — new coverage: pure MQTT CONNECT/PUBLISH/DISCONNECT packet framing,
and that the telemetry sink is independent from the event sink), plus a real
end-to-end smoke test — `sovd_server` against a live `eclipse-mosquitto`
container, `mosquitto_sub` observing real `lock_acquired`/`http_request`
events arrive on their respective topics — and the Grafana dashboard +
alert-rule YAML validated by actually importing them into a live
`grafana-oss` container against real InfluxDB data (not just hand-checked
JSON): the dashboard's three InfluxQL panel queries return real rows through
Grafana's own datasource proxy, and both alert rules load into Grafana's
unified alerting engine via file provisioning with no errors.

Events already emitted, each carrying `correlation_id` (Phase 1), via
`Router::EventSink` in `routes.cpp` — defaults to stdout, swap with
`set_event_sink()`: `lock_acquired`, `lock_denied`, `lock_release_mismatch`,
`lock_released`, `mode_changed`, `data_written`, `operation_executed`,
`faults_cleared`

- [x] **MQTT transport** — `server/src/mqtt_publisher.cpp`: hand-rolled MQTT
      3.1.1 CONNECT/PUBLISH(QoS0)/DISCONNECT framing, same call this project
      already made for DoIP (`adapters/uds_doip/doip_transport`) — neither
      libmosquitto nor paho is installed on this box, and a fire-and-forget
      one-way publish onto an internal telemetry bus doesn't justify vendoring
      a full client for QoS1/2, subscribe, and TLS this project never uses.
      `MqttPublisher` connects lazily, reconnects on failure, never throws —
      a down broker degrades to silently-dropped telemetry, not a crashed
      server. No `routes.cpp` call site changed; it's a drop-in `EventSink`,
      exactly as designed in Phase 1.
- [x] **Feed existing pipeline: MQTT → Telegraf → InfluxDB → Grafana OSS** —
      `monitoring/telegraf/sovd_mqtt_input.conf.example` documents the
      `mqtt_consumer` input for both topics, matching the tag/field shape the
      dashboard and alert queries below actually query (verified against
      real InfluxDB line-protocol writes, not guessed).
- [x] **Grafana panels: session timeline, lock contention, auth failure
      rate** — `monitoring/grafana/dashboards/sovd_security.json`. Three
      panels, one each: a table of `lock_acquired`/`lock_released`/
      `lock_release_mismatch` events (session timeline), a bar chart of
      `lock_denied` count by entity (contention), a time series of
      `lock_denied` + `lock_release_mismatch` per interval (auth failure
      rate). Uses Grafana's standard `${DS_INFLUXDB}` input-variable
      convention so it imports against whatever InfluxDB datasource already
      exists — verified importable, not just schema-plausible.
- [x] **Alert signatures** — `monitoring/grafana/provisioning/alerting/sovd_alerts.yaml`.
      `lock_release_mismatch` (any occurrence) and repeated `lock_denied` on
      one entity (>5 in 5 min) as Grafana OSS unified-alerting provisioning
      — native alerting the stack already has (ladder: use the platform
      feature, don't hand-roll a rule engine). `<INFLUXDB_DATASOURCE_UID>`
      placeholder needs the real datasource uid filled in at deploy time.
- [x] **Separate operational telemetry sink** — `Router::set_telemetry_sink`,
      independent `EventSink` instance from `set_event_sink`, wired via one
      `httplib` pre-routing-handler + logger hook pair in `register_routes()`
      (no per-handler changes). Emits `http_request` events (`method`,
      `path`, `status`, `duration_ms`, `correlation_id`) on its own MQTT
      topic (`sovd/<server_id>/telemetry` vs `sovd/<server_id>/events`) — *an
      IDS should not be your APM* satisfied by topic separation, matching how
      the SOVD server itself never mixes the two. **Narrower than the
      original wording** ("latency per adapter call, UDS timeout rate,
      session-open failures, queue depth"): this instruments per-*request*
      latency at the HTTP boundary, not per-adapter-call inside the
      `uds_doip` transport. Per-adapter-call granularity (UDS timeout rate,
      session-open failures, queue depth) needs instrumentation inside
      `adapters/uds_doip` itself — a real, separate task, not done here;
      HTTP-level latency was the piece this phase's monitoring pipeline
      (MQTT/Telegraf/Grafana) actually needed to prove out end-to-end.
- [x] **Error verbosity by role — deliberately not built, and here's why:**
      every `write_error()` call site in `routes.cpp` already emits a fixed,
      generic message (`"read_data failed"`, `"entity has no diagnostic
      backend"`, …) — none of them carry adapter-internal detail (an NRC
      name, a DID, an address) today. The vtable itself only returns a
      `sovd_result_t` enum to `routes.cpp`, no descriptive text. So there is
      currently *nothing for a role-based filter to filter* — building the
      mechanism now would be guarding a leak that doesn't exist yet. This
      matters once descriptive NRC text (from `adapters/uds_doip/nrc_map`)
      gets threaded up into HTTP error messages for technician-facing
      builds; CLAUDE.md's original framing ("add before the response schema
      is fixed") still holds — revisit at that point, not before.

---

## Phase 4 — Multi-server topology — COMPLETE ✅

Verified: clean warning-free build in both configs, 304 assertions passing
(`test_core`, up from 271 — new coverage: config loader structural
validation, and a real two-live-server proxy-forwarding test using dynamic
ports), plus a genuine two-process demo on one host (`config/domain_body.yaml`
on :20003, `config/gateway.yaml` on :20002, both against the real
`sovd_server` binary): `/docs` and named-id data reads return the domain's
fully typed values through the gateway with curl, a lock acquired through
the gateway is provably held on the domain server (the domain itself then
refuses a second lock with 423), and killing the domain server mid-demo
produces a clean 502 from the gateway on that entity while `/v1/entities`
and every other entity keep working.

- [x] **Config loader** — `server/src/config_loader.cpp` (`sovd_config_loader`
      target, shared by `sovd_server` and `test_core`). YAML → entity/adapter
      map, replacing hardcoded `build_topology()` — but not *instead of* it:
      `main.cpp` keeps the old hardcoded demo path unchanged for `./sovd_server`
      with no args or `./sovd_server <port> <role>`; `./sovd_server <path-to-
      existing-file>` is the new config-driven path. Delivers "same binary,
      different config" without breaking the zero-config quick-test path
      Phases 0–3 already relied on. A single misconfigured entity (bad
      adapter config, unknown adapter kind, adapter not compiled into this
      binary) degrades to a plain grouping node with a stderr warning rather
      than aborting the whole topology load or — the real hazard flagged in
      `uds_doip_adapter.cpp`'s own doc comment — pairing a non-NULL vtable
      with a NULL adapter context. A genuine structural error (duplicate
      path, child listed before its parent) still throws `ConfigError`.
- [x] **Proxying to a child SOVD server — NOT `adapters/sovd_proxy` behind
      the vtable, as originally sketched. Corrected during the build, same
      as several Phase 2 items, for a real reason:** the vtable's
      `read_data` only carries raw `sovd_buffer_t` bytes for local decoding.
      A proxy has to pass through the remote's *already-decoded* JSON
      (`battery_voltage` → `13.0 V`) verbatim — and must, since "gateway
      needs no DID catalogs at all" (below) rules out decoding locally. Worse,
      `/docs` isn't even reachable through the vtable at all (it's a
      Router+Catalog concern), so a vtable-shaped proxy would have silently
      broken typed self-description through the gateway — arguably the
      whole point of Phase 1. The actual implementation is a Router-level
      HTTP-forwarding table (`ProxyTarget`, `Router::attach_proxy`/
      `find_proxy`/`try_forward` in `routes.hpp`/`routes.cpp`): every
      handler calls `try_forward()` right after `require_entity()`, and if
      the path is proxied, the *entire* request (method, sub-path suffix,
      query params, body, `X-SOVD-Lock-Id`/`X-SOVD-Correlation-Id` headers)
      is forwarded verbatim to `base_url + /v1/entities/<remote_path><suffix>`
      and the remote's response copied back — one lookup-then-dispatch
      shape, same as `find_catalog()`, just forwarding instead of decoding.
      "Gateway code path becomes identical to an ECU-facing one" is
      preserved in spirit (one table lookup selects the dispatch path) even
      though it's not literally the same C-ABI struct. A config-loader
      consequence: `kind: sovd_proxy` is attached **per leaf entity**, not
      per-area as CLAUDE.md's original schema sketch showed (each leaf gets
      its own `base_url`/`remote_path`) — the per-entity vtable/dispatch
      model was never going to support "one adapter covers an entire
      subtree," sketch or no sketch.
- [x] `forward_locks: true` — accepted in the YAML for schema fidelity but
      not actually a toggle: a proxied entity's locks are **always**
      forwarded, never cached locally, matching "NEVER cache lock state" as
      a hard rule rather than a configurable option nothing should be
      allowed to turn off.
- [x] **Demonstrate two-tier topology on one host, two ports** —
      `config/domain_body.yaml` (mock-backed, matches Phase 2's own "no live
      DoIP target to ship alongside this demo" call — swap `kind: mock` for
      `kind: uds_doip` to point at a real ECU without touching the gateway
      config at all) + `config/gateway.yaml`. Both curl-verified live (see
      above) and covered by an automated test
      (`test_config_loader_proxy_forwards_docs_data_and_locks`) using two
      real `httplib::Server` instances on dynamic ports.
- [x] **Backpressure through the proxy tier** — verified as an *existing*
      property, not new code: `httplib::Server` already runs a bounded
      `ThreadPool` (not unbounded per-connection threads), and
      `try_forward()`'s remote call is a blocking `httplib::Client` request
      on the gateway's own worker thread. A saturated/slow domain server
      therefore blocks the gateway's finite worker pool directly — new
      gateway connections queue and eventually block at the accept layer,
      the standard backpressure shape for a synchronous thread-per-request
      proxy. Nothing further was built for this; a synthetic queue/limiter
      on top would duplicate what the thread pool already provides.
- [x] **Graceful degradation** — one unreachable ECU must not fail the whole
      entity listing. `GET /v1/entities` never calls any backend (proxied or
      not) — true since Phase 0, unchanged — so this was already satisfied
      structurally; verified explicitly by
      `test_config_loader_unreachable_proxy_degrades_gracefully` (an
      unreachable proxy target fails only its own entity with a clean 502;
      the listing and a second, independent mock-backed entity are
      unaffected) and by the live demo (killing the domain server mid-run).
- [x] Gateway needs **no DID catalogs at all** — true by construction: the
      config loader never calls `router.attach_catalog()` for a
      `sovd_proxy`-kind entity, and `try_forward()` bypasses `find_catalog()`
      entirely. Demonstrated live: the gateway process never reads
      `catalogs/bcm.yaml`, yet `/docs` and named-id reads through it are
      fully typed (see the curl output in this phase's demo).

---

## Phase 5 — Client SDK + CLI — COMPLETE ✅

**CLI before UI.** The CLI forces a clean SDK boundary and is trivially
scriptable for testing. UI-first ends with API logic tangled into components.

Verified: clean warning-free build in three configs (default mock-only,
`SOVD_ADAPTER_UDS_DOIP=ON`, and `SOVD_ADAPTER_UDS_DOIP=ON -DSOVD_CLIENT_MDNS=OFF`
— mDNS must not be load-bearing for the rest of the project to build on a
box without avahi), 345 assertions total (up from 318 — `test_core`'s new
lock-renew coverage plus a new `test_client` binary: 27 assertions,
`SovdClient`/`LockGuard` exercised against a real live `sovd_server`, not a
mock of the SDK's own HTTP dependency). Every CLI subcommand curl-equivalent
verified live against a real running server (`entities`, `docs`, `faults`,
`faults-clear`, `read` single/batch, `write`, `mode`, `op`, `watch`) — output
included below in each bullet. Two real bugs were found and fixed by this
live testing, not by inspection — see the RAII and CLI bullets.

- [x] **Typed wrappers over every resource** —
      `client/include/sovd/client/sovd_client.hpp` / `sovd_client.cpp`.
      One deliberate simplification: a decoded data value is carried as
      `nlohmann::json` rather than a hand-built variant (documented in the
      header) — the wire format is already JSON and every other module in
      this repo treats it the same way; "typed" is about the *resource
      surface* (one method per SOVD operation, structured results), not
      about eliminating `json::json` from every leaf field.
- [x] **Discovery-driven** — the CLI (`cli/src/main.cpp`) has zero
      hardcoded entity/DID knowledge anywhere: every path/id is a runtime
      argument, and `sovd-cli <url> docs <path>` is how a user or script
      discovers what's actually callable — the CLI-scale version of the
      same constraint Phase 7 restates for the web UI.
- [x] **RAII lock lifecycle** — `client/include/sovd/client/lock_guard.hpp`.
      Required a real, necessary server-side addition that didn't exist
      before this phase: `PUT /v1/entities/{path}/locks/{lock_id}`
      (`LockManager::renew`, `Router::handle_put_lock`) — there was no way
      to extend a held lock's TTL at all, so "heartbeat at ttl/2" had
      nothing to call. Emits its own `lock_renew_mismatch` event (not
      `lock_release_mismatch` — it wasn't a release) on a wrong-id renew;
      Phase 3's alert-rule query was extended to match both, same signature.
      **Live bug found and fixed**: the heartbeat's interval was computed
      as `std::chrono::seconds(std::max(1, ttl_seconds/2))` — for a 1s TTL,
      integer division rounds `ttl/2` to 0, and the `max(1, …)` floor then
      fired the heartbeat *at* the TTL instead of ahead of it, racing (and
      sometimes losing to) expiry. Fixed to millisecond precision
      (`ttl_seconds * 500`, 100ms floor) — caught by
      `test_client_lock_guard_heartbeat_keeps_lock_alive_past_original_ttl`
      actually failing on the first run, not by code review.
- [x] Retry/backoff on `503`/`504`, **never** `423` — `SovdClient::request()`
      in `sovd_client.cpp`; defaults (`max_retries=3`, `backoff_ms=200`,
      doubling) match the `retry:` block CLAUDE.md's client config schema
      already documented in Phase 1.
- [x] **mDNS discovery (`_sovd._tcp.local`)** — real `avahi-client`, not
      hand-rolled DNS-SD packet parsing (unlike DoIP/MQTT's "hand-roll the
      protocol slice" precedent: avahi-client is genuinely already
      installed here and a full DNS-SD implementation is a much bigger slice
      than QoS0 MQTT PUBLISH framing was). `client/src/mdns_discovery.cpp`
      (browse+resolve, `AvahiSimplePoll`, one-shot with a timeout) and
      `server/src/mdns_advertise.cpp` (`MdnsAdvertiser`, RAII, its own
      `AvahiThreadedPoll` so it doesn't compete with `httplib::Server`'s
      accept loop) — both gated behind `SOVD_CLIENT_MDNS`, auto-detected via
      `pkg_check_modules(avahi-client)` so the rest of the project builds
      unchanged on a box without avahi. **Honest limitation, not swept
      under a green checkmark**: this dev sandbox has avahi-client
      *libraries* installed but no functioning `avahi-daemon`/D-Bus service
      activation (`systemctl start avahi-daemon` itself times out — no
      working init/service manager in this sandbox, confirmed, not a code
      issue) — so unlike MQTT/Grafana (Phase 3) and the two-process proxy
      demo (Phase 4), a live advertise→discover round trip could not be
      verified here. What *was* verified: real compilation and linking
      against the real avahi-client headers/libs (not a stub), and correct
      graceful-failure behavior when no daemon is reachable (`sovd-cli
      discover` returns cleanly with "no servers found" within its timeout,
      no hang, no crash). Needs a live avahi-daemon (any normal Linux
      desktop or the project's actual target hardware) to verify the full
      round trip — flagged here rather than claimed.
- [x] **CLI: browse entities, read/clear faults, read/write data, live
      watch** — `cli/src/main.cpp`, plus `mode`/`op`/`discover` beyond the
      literal checklist (needed for "typed wrappers over every resource" to
      actually mean *every* resource). **Live bug found and fixed**: `watch`
      produced zero output when its stdout wasn't a tty (e.g. under
      `timeout` sending `SIGTERM`, or any real piped/redirected use) —
      C++'s stdout is fully buffered, not line-buffered, off a tty, so
      nothing reached the terminal until a clean process exit, which is
      exactly what "live" output must not depend on. Fixed with explicit
      `std::cout.flush()` after each printed line and a `SIGTERM` handler
      alongside `SIGINT`'s (`timeout N ... watch` sends `SIGTERM`, not
      `SIGINT`) — verified live afterward: initial value printed
      immediately, a background write's new value appeared within one
      poll interval, confirmed under `timeout`.

---

## Phase 6 — Streaming — COMPLETE ✅

Clearest "why SOVD over UDS" demonstration.

Verified: clean warning-free build in three configs (default, `+uds_doip`,
`+uds_doip -mdns`), 374 assertions total (up from 345 — `test_core` 342,
`test_client` 32). Live-verified with curl and `sovd-cli`: raw SSE frames,
`watch` showing an initial value then a pushed update after a background
write (with output correctly flushed under `timeout`/SIGTERM, matching
Phase 5's fix), a clean 501 (not a silent mis-forward) streaming through a
Phase 4 gateway, and two concurrent curl subscribers to the same data point
landing the same frame count on the same cadence. **Two real threading bugs
found and fixed by actually running the tests, not by review** — see the
first two bullets.

- [x] **SSE subscription endpoint for live data** —
      `GET /v1/entities/{path}/data/{id}/stream?interval_ms=N`
      (`Router::handle_stream_data`, registered *before* the plain
      `.../data/(.+)` pattern so that route's greedy capture doesn't
      swallow the trailing `/stream`). Reuses `read_one_data_item` — the
      exact same decode path the plain GET uses — so a client switching
      from polling to streaming sees byte-identical JSON per event, only
      the transport changes. Streaming through a Phase 4 `sovd_proxy` is a
      real second feature (bidirectional stream proxying) not attempted
      here; explicitly guarded to a clean `501` rather than letting
      `try_forward()`'s one-shot semantics silently truncate a stream.
      **Live bug found and fixed**: the shared poller's interval used
      `std::this_thread::sleep_for()`, which can't be woken by a
      `stop`+`notify_all()` — so unsubscribing from a long-interval stream
      blocked the thread destroying the last `Subscription` (potentially an
      HTTP worker thread) for up to the full interval. Caught by a test
      that legitimately hung for the length of a 60-second poll interval,
      not by inspection. Fixed to an interruptible `condition_variable::
      wait_for` with a `stop` predicate, mirroring the identical fix
      already applied to `LockGuard`'s heartbeat in Phase 5 — this project's
      second instance of "a background timer must be interruptible, not a
      blind sleep," now clearly a pattern rather than a one-off.
- [x] **Backed by adapter-level periodic read, not per-request polling** —
      `server/src/stream_hub.cpp`'s `StreamHub`: one poller thread per
      `(path, id, interval_ms)` key, shared by every subscriber at that
      cadence (refcounted; last unsubscribe stops it), fanning out via a
      version-numbered condition variable rather than each SSE connection
      independently hitting the adapter on its own timer. Proven, not just
      built: a unit test subscribes twice with a shared read-call counter
      and asserts the count matches *one* poller's expected ticks, not two
      — the precise way to catch a regression back to per-subscriber
      polling that a live demo alone wouldn't reliably reveal. Different
      `interval_ms` values for the same `(path, id)` deliberately get
      independent pollers rather than one subscriber's cadence silently
      overriding another's.
- [x] **Client-side subscription handling** —
      `SovdClient::subscribe_data()` (`client/src/sovd_client.cpp`):
      blocking, runs the SSE receive loop on the caller's own thread via
      `httplib::Client`'s `ContentReceiver`, invoking a callback per event
      until the server ends the stream or a caller-supplied
      `std::atomic<bool>* stop_flag` is set — deliberately not
      thread-managed by the SDK itself, since every consumer so far already
      has a natural thread for this. The CLI's `watch` command was switched
      from polling to this (`interval_ms` now genuinely means "how often
      the *server* reads the adapter," matching this phase's title, not
      "how often the client asks"). **Live bug found and fixed**: `watch`
      produced zero output when piped/redirected — already fixed in Phase 5
      for the polling version (`std::cout.flush()`, SIGTERM handling); the
      SSE rewrite inherited the fix since it reuses the same print path,
      confirmed live rather than assumed.

---

## Phase 7 — Web UI

**Design constraint: ZERO knowledge of any specific ECU.** Every control
rendered from `/docs`. Hardcoding DIDs in the frontend throws away the thing
that makes SOVD better than ODX.

- [ ] Stack: Vue 3 + Vite + Tailwind (reuse Verso patterns; don't rebuild a
      design system — value is functional, not aesthetic)
- [ ] **Hosted separately from the vehicle**, CORS-configured API. Serving
      static files from a safety-adjacent gateway adds attack surface and TLS
      pain for no benefit.
- [ ] Budget time for: self-signed cert warnings, mixed content, inconsistent
      browser mDNS `.local` resolution

**Scope: four screens, then STOP.** (1) entity browser, (2) fault viewer
read/clear, (3) data table read/write with typed widgets from catalog,
(4) live chart via SSE.
*Update orchestration and OTX UIs are where projects go to die.*

---

## Phase 8 — Hardening

- [ ] **mTLS gateway ↔ domain servers** — internal hop must verify the
      gateway's certificate, **not** trust a forwarded external bearer token
      (otherwise a leaked token becomes lateral movement)
- [ ] OAuth2 / token auth at the external boundary
- [ ] **Default-deny path/method whitelist** at the gateway tier
- [ ] Restricted gateway build (no ECU adapters linked)
- [ ] **Resource limits as a security property** — max concurrent sessions, max
      request body, max entity-tree depth, lock TTL ceiling. Unbounded any of
      these is a DoS on a safety-adjacent interface. (`SOVD_MAX_DATA_LEN`
      exists; the rest don't.)
- [ ] Per-adapter bounded request queue → `503` rather than piling up requests
      the bus cannot service
- [ ] Per-adapter connection pooling — DoIP routing activation is expensive,
      don't redo it per request
- [ ] **Explicit persistence decisions:** locks should *not* survive restart
      (a restart should release them); audit logs and job status should.
      "All in memory" is a decision, not a default.

---

## Deferred / optional

- [ ] `Function` entity endpoint — design the seam, leave unimplemented
- [ ] OTX Scenario A — OTX runtime acts as an SOVD client via `sovd_client`
      (the pragmatic starting point if OTX is ever revisited)
- [ ] OTX Scenario B — server launches OTX procedures via the `Function`
      entity; not fully supported in reference implementations yet
- [ ] Software update orchestration (`0x34`/`0x36`/`0x37`)
- [ ] Bulk log / freeze-frame retrieval

---

## Certification posture (if this heads toward production)

- UN-R155 CSMS wants demonstrable threat analysis on the diagnostic interface
- ISO 21434 wants traceability from requirement to test
- Both are much cheaper because `core/` is I/O-free — unit-level evidence is easy

---

## Working conventions

- Keep the build warning-free under `-Wall -Wextra -Wpedantic`
- `core/` must stay free of HTTP, sockets, and JSON — if a change needs I/O in
  core, the design is wrong
- New adapters implement `adapter.h` and nothing else; leave unsupported
  function pointers NULL rather than returning stub data
- Tests must not sleep — use the injectable clock. One narrow, documented
  exception: `session_manager`'s heartbeat/idle-timeout is a genuine
  wall-clock-driven background thread (not a lazily-checked TTL like
  `LockManager`), so its tests use short real intervals with bounded polling
  instead. Don't extend that exception to anything that could use an
  injectable clock instead — it's a real tradeoff made once for a case that
  needed it, not a precedent.
- Env note: owner's other projects are Windows/PowerShell (`;` not `&&`) and
  Bun-based; **this project is Linux/CMake/C++ and does not use Bun**

## Recommended order

**Phases 1 → 2 → 3 is the credible, finishable core.**
Phases 4 → 6 make it architecturally serious.
Phases 7 → 8 are polish and production posture.

For interviews, Phases 1, 3, and 6 show understanding of *why* SOVD exists
rather than just an ability to serve JSON over HTTP. Self-description and
streaming are the actual advances over UDS/ODX; everything else is a nicer
transport for what UDS already did.
