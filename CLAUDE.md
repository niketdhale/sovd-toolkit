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

### Deliberate non-goals
- Software update / flash orchestration (large, demonstrates nothing new)
- Async long-running operations with job polling
- OTX runtime (especially Scenario B server-side triggering)
- Full spec coverage of every SOVD resource class

### Two decisions STILL OPEN (settle before Phase 2)
1. **API versioning scheme** — path prefix (`/v1/`) vs `Accept` header.
   Path prefix is uglier but unambiguous. *Trivial now, breaks every deployed
   tester later.*
2. **Session manager ownership** — per-adapter or per-lock? Per-lock is
   conceptually cleaner (session lifetime = lock lifetime) but unlocked
   read-only requests then need a transient session path. **This shapes the
   whole DoIP module.**

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
│   └── mock/               # in-memory, extern "C" only (supplier pattern)
├── server/                 # C++ HTTP layer — NO diagnostic logic
│   ├── include/sovd/server/routes.hpp
│   └── src/{main.cpp, routes.cpp}
├── tests/
│   ├── test_framework.hpp  # minimal harness, no external dep
│   └── test_core.cpp       # 73 assertions
└── third_party/            # vendored single headers
    ├── httplib.h           # cpp-httplib v0.18.3 (MIT)
    └── json.hpp            # nlohmann/json v3.11.3 (MIT)
```

**Layering rule:** `server/` translates HTTP ⇄ core calls and nothing more.
`core/` holds no I/O. All backend complexity hides behind `adapter.h`.

---

## Build & test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
./build/test_core                      # 73 assertions
./build/sovd_server 20002 domain       # port, role
cd build && ctest --output-on-failure
```

Adapter selection at **configure** time:
```bash
cmake -S . -B build -DSOVD_ADAPTER_MOCK=ON -DSOVD_ADAPTER_UDS_DOIP=OFF
```

Builds clean under `-Wall -Wextra -Wpedantic` with zero warnings. Keep it that
way.

---

## Phase 0 — COMPLETE ✅

Verified: clean warning-free build, 73 assertions passing, end-to-end HTTP run
hitting every expected status code.

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
| GET | `/` | self-description, role |
| GET | `/entities` | flat listing with `has_backend` |
| GET | `/entities/{path}/faults` | |
| DELETE | `/entities/{path}/faults` | lock-gated |
| GET | `/entities/{path}/data/{did}` | hex DID e.g. `F190` |
| PUT | `/entities/{path}/data/{did}` | lock-gated |
| POST | `/entities/{path}/modes` | lock-gated, UDS session control |
| POST | `/entities/{path}/operations/{op}` | lock-gated, UDS RoutineControl |
| POST | `/entities/{path}/locks` | returns `lock_id` |
| DELETE | `/entities/{path}/locks/{lock_id}` | |

`{path}` is a full multi-segment entity path (`vehicle/body/bcm`).
Lock-gated ops require `X-SOVD-Lock-Id` header when a lock is held.

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

### ⚠ KNOWN CARRY-OVER BUG — fix early in Phase 2
`server/src/routes.cpp` calls adapters with the **leaf** id (`bcm`) via
`leaf_of(path)`, while the registry keys on the **full path**
(`vehicle/body/bcm`). Fine for UDS adapters; **wrong for the Phase 4 proxy
adapter**, which needs the full path to forward correctly.
**Fix: pass the full path, let each adapter decide what to do with it.**
Do this *before* a second adapter depends on current behaviour.

---

## NEXT UP: Phase 1 — Self-description & usability

**Do this before Phase 2.** The instinct is to reach for real hardware next,
but `/docs` + typed catalogs + batch read is what makes the client generic.
Build the DoIP adapter first and you'll write a client hardcoded to the mock's
DIDs, then have to rewrite it.

- [ ] **`sovd_capability_t`** extension to the adapter vtable — each backend
      declares what it supports
- [ ] **DID catalog parser** — YAML → typed definitions (`type`, `encoding`,
      `scale`, `unit`, enum `values`, `access`, `io_control`,
      `requires_session`)
- [ ] **`GET /entities/{path}/docs`** — capability description, serialized from
      the catalog. *This is the answer to "how does the client know what to
      call" and the highest-value remaining feature.*
- [ ] **Named data paths** — `/data/battery_voltage` primary, raw hex DID as
      fallback
- [ ] **Batch data read** — `GET /data?ids=a,b,c`. Without it, reading 30 live
      values is 30 HTTP round trips (unusable for real diagnostics)
- [ ] **Fault filtering** — `?status=confirmed|pending|testFailed`
- [ ] **API versioning** — settle the open decision above, then implement
- [ ] **Correlation IDs** — request ID surviving the proxy hop. Clocks may not
      be synced across gateway/domain HPC, so correlate by ID, **never** by
      timestamp

**Exit criteria:** a client that has never seen the vehicle before can
enumerate entities, discover every readable value with its type and unit, and
read them in one batched call.

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

## Phase 2 — UDS/DoIP adapter (real hardware path)

All complexity hides behind the vtable; `core/` and route handlers never learn
UDS exists.

```
adapters/uds_doip/
├── doip_transport    — socket, routing activation, ISO-TP segmentation
├── session_manager   — per-ECU session state + 0x3E TesterPresent heartbeat
├── nrc_map           — UDS negative response → sovd_result_t
└── did_catalog       — shared with Phase 1 (encode/decode)
```

### SOVD → UDS service mapping
| SOVD request | UDS service |
|---|---|
| `GET /data/{id}` | `0x22` ReadDataByIdentifier |
| `PUT /data/{id}` | `0x2E` WriteDataByIdentifier |
| `PUT /data/{id}` (actuator) | `0x2F` InputOutputControlByIdentifier |
| `GET /faults` | `0x19` ReadDTCInformation (sub `0x02`) |
| `DELETE /faults` | `0x14` ClearDiagnosticInformation |
| `POST /modes` | `0x10` DiagnosticSessionControl |
| `POST /operations/{id}` | `0x31` RoutineControl |
| software update | `0x34`/`0x36`/`0x37` (deferred) |
| `POST /locks` | **nothing** — pure SOVD concept |
| *(none)* | `0x3E` TesterPresent — adapter generates internally on a timer |

### The four hard problems
1. **Stateless HTTP vs stateful UDS sessions** — session state machine per ECU
   keyed off the SOVD lock: acquire lock → open extended session → spawn `0x3E`
   heartbeat → hold until release/TTL. Client never sees it.
   *Most common way a real SOVD gateway breaks.*
2. **`0x27` SecurityAccess** — no SOVD equivalent. Translate HTTP scope
   (`flash:write`) → seed/key exchange. Gateway becomes holder of ECU
   credentials → reinforces the restricted-build decision.
3. **Semantic gap** — `0x22` returns opaque bytes; SOVD returns typed values.
   Catalog-driven decode (scaling, endianness, bit layout, enums). Least
   glamorous, most laborious part of any real implementation.
4. **`0x78` response-pending** — loop internally, surface HTTP only once
   resolved. Plus ISO-TP multi-frame entirely below the SOVD line.

### NRC → HTTP
`0x33` securityAccessDenied → 403 · `0x31` requestOutOfRange → 400 ·
`0x22` conditionsNotCorrect → 409 · `0x78` responsePending → handled
internally, **never surfaced** · timeout/no response → 502

### Testing prerequisite
**Build a fault-injecting DoIP simulator BEFORE the session manager.** Extend
`DoIP_ECU_Simulator` to misbehave on demand (timeouts, `0x78` storms, truncated
frames, NRCs). Otherwise you debug against a simulator that never reproduces
the failure.

---

## Phase 3 — Security monitoring (THE DIFFERENTIATOR)

Nobody ships SOVD with native IDS integration. Most differentiated part of the
project. Diagnostic interfaces are privileged *by design*, so they belong under
the same monitoring as the CAN bus.

Events already emitted (currently stdout via `emit_event()` in `routes.cpp`):
`lock_acquired`, `lock_denied`, `lock_release_mismatch`, `lock_released`,
`mode_changed`, `data_written`, `operation_executed`, `faults_cleared`

- [ ] MQTT transport for existing structured events
- [ ] Feed existing pipeline: MQTT → Telegraf → InfluxDB → Grafana OSS
- [ ] Grafana panels: session timeline, lock contention, auth failure rate
- [ ] **Alert signatures:** `lock_release_mismatch`, and repeated `lock_denied`
      on one entity → client misbehaving or probing
- [ ] **Separate operational telemetry sink** — latency per adapter call, UDS
      timeout rate, session-open failures, queue depth.
      *An IDS should not be your APM.*
- [ ] **Error verbosity by role** — technician sees `conditionsNotCorrect`;
      remote fleet client must not learn internal addressing.
      *Add before the response schema is fixed.*

---

## Phase 4 — Multi-server topology

- [ ] **Config loader** — YAML → entity/adapter map, replacing hardcoded
      `build_topology()`. Delivers the "same binary, different config" property.
- [ ] **`adapters/sovd_proxy`** — same vtable, emits HTTP to a child SOVD server
      instead of UDS. Gateway code path becomes identical to an ECU-facing one.
- [ ] `forward_locks: true` — proxy must **never** cache lock state
- [ ] Demonstrate two-tier topology on one host, two ports
- [ ] **Backpressure through the proxy tier** — a saturated domain server must
      be able to slow the gateway, else the gateway queues until it falls over
- [ ] **Graceful degradation** — one unreachable ECU must not fail the whole
      entity listing
- [ ] Note: gateway needs **no DID catalogs at all** (domain server decodes) —
      another argument for the restricted build

---

## Phase 5 — Client SDK + CLI

**CLI before UI.** The CLI forces a clean SDK boundary and is trivially
scriptable for testing. UI-first ends with API logic tangled into components.

- [ ] Typed wrappers over every resource
- [ ] **Discovery-driven** — build callable surface from `/entities` + `/docs`
      at runtime, not hardcoded
- [ ] **RAII lock lifecycle** — acquire, background heartbeat at TTL/2, release
      on scope exit. *A tester that crashes holding a 3600s lock bricks the
      entity until expiry.*
- [ ] Retry/backoff on `503`/`504` — **explicitly not on `423`**
- [ ] mDNS discovery (`_sovd._tcp.local`)
- [ ] CLI: browse entities, read/clear faults, read/write data, live watch

---

## Phase 6 — Streaming

Clearest "why SOVD over UDS" demonstration.

- [ ] SSE subscription endpoint for live data
- [ ] Client-side subscription handling
- [ ] Backed by adapter-level periodic read, not per-request polling

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
- Tests must not sleep — use the injectable clock
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
