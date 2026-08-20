# CLAUDE.md — sovd-toolkit

Context file for Claude Code. Read this first before touching the repo.

---

## STATUS AT A GLANCE

**Phases 0–6: COMPLETE.** 381 assertions in the default build (`test_core`
381 includes B1/B2 coverage, `test_client` 32), plus 180 in `test_uds_doip`
when `-DSOVD_ADAPTER_UDS_DOIP=ON`. Clean under `-Wall -Wextra -Wpedantic` in
every documented configuration, including the previously-broken
`-DSOVD_ADAPTER_MOCK=OFF -DSOVD_ADAPTER_UDS_DOIP=ON` restricted combination
(B3, fixed).

**B1, B2, and B3 are all DONE. Phase 7 (Vue web UI, all four screens) is
built (2026-08-20)** — `web/`, Vue 3 + Vite + Tailwind, pointed at a domain
server per D1, typed widgets from `/docs` per B1, CORS per B2, 10s
lock-heartbeat lifecycle per D3. `npm run build` type-checks clean and every
API call pattern was verified live with curl against a real server, but
**no browser automation was available to visually exercise the rendered
UI** — that's the one remaining check before calling this fully done, not
just built.

**Phase 8 (hardening) is in progress, started 2026-08-20.** Done so far:
default-deny route whitelist at the gateway tier, and all four resource
limits (entity-tree depth cap, lock TTL ceiling, max-concurrent-locks as the
UDS-session-cap proxy, max request body) — both live-verified against the
real two-tier demo, not just unit-tested. 509 assertions in `test_core` (up
from 381), `test_uds_doip` 180, `test_client` 32 — all passing. Remaining:
mTLS, OAuth2, SecurityAccess wiring (needs OAuth2 first, per D2), per-adapter
bounded queue, per-adapter connection pooling, explicit persistence
write-up.

### Blockers (server-side, must land before UI code)
| id | What | Blocks | Where it's specified |
|---|---|---|---|
| **B1** ✅ | Catalog `encode()` (typed value → bytes) + pre-adapter validation | DONE 2026-08-20 | Phase 7 § Blockers |
| **B2** ✅ | CORS in `routes.cpp` (allow-list, `OPTIONS`, `X-SOVD-*` headers, SSE verified separately) | DONE 2026-08-20 | Phase 7 § Blockers |
| **B3** ✅ | Fix `SOVD_ADAPTER_MOCK=OFF` + `UDS_DOIP=ON` build break | DONE 2026-08-20 | Phase 8, first item |

### Decisions — SETTLED 2026-08-20 (owner confirmed, not agent-picked)
| id | Decision | Resolution |
|---|---|---|
| **D1** | UI points at gateway or domain server? | **Domain server directly.** All four screens work; the two-tier topology stays a curl/CLI demo (Phase 4), not part of the UI. |
| **D2** | SecurityAccess gating needs **both** a catalog field and an OAuth2 scope | **Recorded, implemented in Phase 8** alongside the scope work — nothing to build for Phase 7. |
| **D3** | Browser lock lifecycle | **Short TTL (10s) + JS heartbeat + best-effort `beforeunload` release.** Not the CLI's 60s default — different failure mode (a closed tab has no RAII destructor). |

Full reasoning for each: **OPEN DECISIONS** section below.

### Known-good deviations from the original plan (already settled, don't revisit)
- Phase 4 proxying is a **Router-level HTTP forwarding table**, not a vtable
  adapter — the vtable carries raw bytes, a proxy must pass through decoded
  JSON, and `/docs` isn't reachable through the vtable at all.
- Session manager is **object per-adapter, lifetime per-lock** — not per-lock
  objects, and unlocked reads never escalate.
- API versioning is a **path prefix** (`/v1/`), with `/` deliberately
  unversioned for version discovery.

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

---

## OPEN DECISIONS — SETTLED 2026-08-20

These were **not** for an implementing agent to pick silently — each had a
real consequence and a wrong default. All three were put to the project
owner explicitly (not defaulted) and confirmed before any Phase 7 code.

### D1. Where does the web UI point — gateway or domain server? → **domain server directly**
Streaming through a `sovd_proxy` entity is a deliberate `501` (Phase 6:
bidirectional stream proxying was never built, and `try_forward()`'s one-shot
semantics would silently truncate a stream). So **Phase 7 screen 4 (live
chart via SSE) cannot run against the gateway tier.**

The options were:
- **(a) [CHOSEN]** UI points at a domain server directly → all four screens
  work, the two-tier topology isn't in the UI demo (it stays a curl/CLI demo,
  already verified live in Phase 4).
- **(b)** UI points at the gateway → screens 1–3 work, screen 4 must be
  disabled or fall back to polling for proxied entities.
- **(c)** Build stream proxying in `try_forward()` → a real feature, not a
  Phase 7 item. Out of scope unless deliberately promoted.

Chosen for the reason recommended: screen 4 is the "why SOVD over UDS" demo;
degrading it to prove a topology point would be the wrong trade. Phase 7's
demo server is therefore a **domain-tier** config (mock- or uds_doip-backed),
not `config/gateway.yaml`.

### D2. SecurityAccess gating — needs BOTH halves, not one → **recorded, deferred to Phase 8**
Phase 2 framed this as *either* an HTTP scope concept *or* a catalog field.
That framing is wrong: they answer different questions and both are required.
- **Catalog field** (`requires_security_level`, sitting parallel to the
  existing `requires_session`) = what the **ECU** demands.
- **OAuth2 scope** (Phase 8) = whether this **client** is permitted to request
  that level.

Gate on both. Add the catalog field alongside the Phase 8 scope work, not
before — nothing for Phase 7 to build here; this section exists so the
one-without-the-other framing doesn't quietly recur when Phase 8 starts.

### D3. Browser lock lifecycle → **10s TTL + JS heartbeat + best-effort `beforeunload`**
`client/lock_guard.hpp`'s RAII heartbeat has **no browser equivalent**. A
closed tab or crashed page leaves a lock held until TTL expiry, blocking every
other tester on that entity.

Chosen mechanism (owner confirmed, 10s over the 5s alternative — more
heartbeat slack, still short enough that a crashed tab self-heals in
single-digit seconds):
- **`lock_ttl_seconds = 10`** when Phase 7's UI acquires a lock — not the
  CLI's 60s default. Different failure mode, different correct value.
- JS heartbeat calling `PUT /v1/entities/{path}/locks/{lock_id}` at roughly
  ttl/2 (~5s), the same ratio `LockGuard`'s C++ heartbeat already uses.
- `beforeunload` handler issuing `DELETE /locks/{lock_id}` as a best-effort
  extra — it will not always fire (that's exactly why the TTL must stay
  short rather than being relied on).

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
├── web/                      # Phase 7: Vue 3 + Vite + Tailwind UI, own npm project
│   ├── src/api/sovdClient.ts        # discovery-driven, retry/backoff, mirrors client/'s SDK shape
│   ├── src/composables/useLock.ts   # D3: 10s TTL + heartbeat + beforeunload
│   └── src/views/{EntityBrowser,FaultViewer,DataTable,LiveChart}.vue
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
`SOVD_ADAPTER_MOCK=OFF` alongside `SOVD_ADAPTER_UDS_DOIP=ON` — **fixed,
2026-08-20 (Phase 8's B3)**: `sovd_server` and `sovd_config_loader` now
guard every mock reference behind `SOVD_HAVE_MOCK` (defined per-target when
`SOVD_ADAPTER_MOCK` is on, same shape as `SOVD_HAVE_UDS_DOIP`); `test_core`/
`test_client` are conditional on `SOVD_ADAPTER_MOCK` in `CMakeLists.txt`
(same shape `test_uds_doip` already has for `SOVD_ADAPTER_UDS_DOIP`) rather
than gated function-by-function. `sovd_server` builds and runs clean in this
combination with zero mock symbols linked in; see Phase 8's B3 writeup for
the live verification and the reasoning on why the test binaries are
conditional rather than partially compiled.

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

## Phase 7 — Web UI — COMPLETE ✅ (pending a human's visual pass)

**Design constraint: ZERO knowledge of any specific ECU.** Every control
rendered from `/docs`. Hardcoding DIDs in the frontend throws away the thing
that makes SOVD better than ODX.

Verified: `npm run build` (`vue-tsc -b && vite build`) compiles clean, zero
type errors. No browser automation was available in the session that built
this, so **every API call pattern each screen makes was verified live with
curl** using the real `Origin: http://localhost:5173` header against a real
running domain server instead — version discovery (`GET /`), `/docs`, batch
read, the full lock/heartbeat/renew/release cycle with a typed enum write
riding on it, and the SSE stream with its CORS headers — and the Vite dev
server itself confirmed serving `index.html` and every `.vue`/`.ts` module
in the graph with no transform errors. **The actual rendered UI has not
been visually exercised in a real browser** (layout, click-through, console
errors) — flagged here rather than claimed, matching this project's own
standard (Phase 5/6 found real bugs specifically by running things live;
this phase couldn't complete that same step). Both `sovd_server
config/domain_body.yaml` and `npm run dev` were left running for a human
to check at <http://localhost:5173>.

### ⚠ BLOCKERS — server-side work that must land BEFORE any UI code

These are not optional prep. Two of the four screens cannot be built without
them. **Do these first, in this order.**

- [x] **B1. Catalog `encode()` — DONE, 2026-08-20.** Typed value → bytes,
      the inverse of `decode()` (`catalog/src/did_catalog.cpp`), mirroring
      its three cases exactly and using only existing catalog metadata — no
      new schema field, as scoped. Live-verified: `PUT .../door_lock_state
      {"value":"unlocked"}` → 204, an invalid label → clean 400 with a
      message naming the bad value, a raw/unnamed DID PUT with hex `value`
      unaffected. Round-trip-tested (`decode(encode(x)) == x`) for float and
      enum, the strongest single check for a codec pair. One real scope
      note: enum items have no width field in the schema (Float has
      `encoding.bytes`, Enum doesn't, and this item's own scope said "no new
      schema needed") — `encode()` produces a single byte, matching every
      enum DID actually used in this project's catalogs; documented in
      `did_catalog.cpp` as a real limit, not silently assumed.
      **Required updating the wire contract**, not just adding a function:
      `PUT` to a *named* id now sends a typed JSON `value` (a number for
      float, a string otherwise) instead of hex — the raw/unnamed-DID path
      is untouched. This broke two existing end-to-end tests
      (`test_core.cpp`, `test_uds_doip.cpp`) that PUT hex to a named enum
      id; fixed to send the label, not silently left passing against stale
      behavior. `SovdClient::put_data()` signature changed to take
      `nlohmann::json` instead of a hex string; a new `put_typed_data()`
      looks the id up via `/docs` first (one extra request) to decide
      number-vs-string for callers, like the CLI's `write` command, that
      only have an untyped string argument.

- [x] **B2. CORS support in `routes.cpp` — DONE, 2026-08-20.** One
      `set_pre_routing_handler` hook (already existed for Phase 3 telemetry
      timing; extended rather than adding a second hook) covers every route
      uniformly, including `handle_stream_data`'s chunked/SSE response —
      that handler never touches CORS headers itself, they're already on
      `res` by the time it runs. Scope, all delivered:
      - `Access-Control-Allow-Origin` from an explicit allow-list
        (`Router::set_cors_allowed_origins`), **never `*`** — empty (the
        default) means no CORS headers are ever sent, so a deployment with
        nothing configured stays same-origin-only by default, not
        accidentally open. Wired through both paths: `cors_allowed_origins`
        in a topology YAML's `server:` block (`config_loader.cpp`), and
        `SOVD_CORS_ORIGINS` (comma-separated) as an opt-in env var for the
        hardcoded zero-config demo path, matching the MQTT/mDNS opt-in
        pattern already established.
      - Preflight `OPTIONS` answered directly in the pre-routing hook (not
        registered per-route) — `OPTIONS` isn't on any route, so this is
        what makes every route covered without N registrations. Denied
        origins still get a clean `204`, just without
        `Access-Control-Allow-Origin` — the browser enforces the actual
        block, matching how a non-allow-listed origin behaves on real
        requests too.
      - `Access-Control-Allow-Headers` explicitly includes `X-SOVD-Lock-Id`
        and `X-SOVD-Correlation-Id`; `Access-Control-Expose-Headers` carries
        `X-SOVD-Correlation-Id` on every real response.
      - **SSE verified separately, as required** — not assumed covered by
        the plain-GET fix. `test_http_cors_applies_to_sse_stream_endpoint`
        asserts the header on the stream endpoint's actual response
        directly, and a live curl against `.../stream` confirmed it outside
        the test suite too.
      - Not built: `Access-Control-Allow-Credentials` — this project has no
        cookie/credentialed auth yet (Phase 8), so there's nothing that
        needs it; add only alongside real auth, not speculatively now.

### Inherited constraints (recorded here so they aren't rediscovered late)

- [x] **Streaming through a proxy is `501`** — see **D1** above (settled:
      domain server directly, so this doesn't bite screen 4). Constraint
      originates in Phase 6; repeated here because nobody building the UI
      reads Phase 6.
- [x] **Discover the API version, don't hardcode `/v1/`** —
      `SovdClient.ensureApiBase()` (`web/src/api/sovdClient.ts`) hits `/`
      once, reads `api_versions[0]`, and builds every subsequent call from
      that; no `/v1/` literal anywhere else in the frontend. Live-verified
      via curl exactly as the client does it.
- [x] **Browser lock lifecycle** — see **D3** above (settled: 10s TTL + JS
      heartbeat + best-effort `beforeunload`). `web/src/composables/
      useLock.ts`. One real design point: it takes a *getter* for the
      entity path, not a static string — Vue's `onUnmounted` can only be
      registered once per component, but the entity being locked (screen
      3's selected path) can change across the composable's lifetime, so
      `useLock()` itself is called once at `setup()` and always reads
      `getPath()` fresh when `acquire()`/`release()` run; `DataTable.vue`
      forces a `release()` via a `watch` before the path changes under it.
      **Not every lock in this UI uses `useLock`**: the fault-clear button
      is a one-shot acquire→DELETE→release within a single click, matching
      the CLI's own `faults-clear` (`heartbeat=false`) — a heartbeat has
      nothing to do across a sub-second round trip; `useLock`'s heartbeat
      machinery is specifically for screen 3's held-open edit sessions.
- [ ] **Error verbosity by role** — not touched. No NRC detail was threaded
      into any HTTP response in this phase (every error the UI shows is
      whatever generic string `routes.cpp` already emitted), so per Phase
      3's own resolution this still isn't needed yet — noted, not
      silently skipped.

### The UI itself

- [x] Stack: Vue 3 + Vite + Tailwind — `web/`. Tailwind 4's Vite plugin
      (`@tailwindcss/vite`), no separate `postcss.config`/`tailwind.config`
      needed. No Verso source was available to actually reuse in this
      session; Tailwind utility classes only, no custom design system,
      matching "value is functional, not aesthetic." No router, no state
      library (Pinia/Vuex), no charting library, no HTTP client library —
      four screens sharing a little state fit in plain `ref`s and native
      `fetch`/`EventSource`; adding any of those would be solving a
      problem this UI doesn't have.
- [x] **Hosted separately from the vehicle**, CORS-configured API (B2) —
      `web/` is its own Vite project/dev-server/build, no static files
      served from `sovd_server` at all. `config/domain_body.yaml` carries
      `cors_allowed_origins: [http://localhost:5173]` (Vite's default port)
      so the existing two-tier demo config just works with `npm run dev`
      out of the box.
- [ ] Self-signed cert warnings / mixed content / mDNS `.local` resolution
      — not applicable to this session's plain-`http://localhost` setup;
      genuinely deferred to whoever deploys this against a real TLS
      endpoint, not silently dropped.

**Scope: four screens, then STOP — delivered.** `web/src/views/`:
`EntityBrowser.vue` (1), `FaultViewer.vue` (2, read + status filter +
clear), `DataTable.vue` (3, typed widgets purely from `item.type`/
`item.access`/`item.values` — an enum `<select>`, a `<input type=number>`
with its unit shown, a text input capped at `item.length` for a bounded
string, nothing hardcoded per-entity), `LiveChart.vue` (4, native
`EventSource`, a hand-rolled SVG polyline for a float item since one screen
doesn't justify a charting dependency, a scrolling text log for anything
else streamed).
*Update orchestration and OTX UIs are where projects go to die.*

---

## Phase 8 — Hardening

**Order matters here — the first item is a prerequisite for the rest.**

- [x] **B3 / FIRST: fix the restricted-build breakage — DONE, 2026-08-20.**
      `sovd_server`'s `build_topology()` and `config_loader.cpp`'s `kind:
      mock` handling are now both `#ifdef SOVD_HAVE_MOCK`-guarded, defined
      per-target in `CMakeLists.txt` exactly like `SOVD_HAVE_UDS_DOIP`
      already was — same established pattern, applied symmetrically.
      Verified live, not just linked: `-DSOVD_ADAPTER_MOCK=OFF
      -DSOVD_ADAPTER_UDS_DOIP=ON` now builds `sovd_server` clean with zero
      mock symbols in the binary (checked via `nm`), the zero-config demo
      path degrades to a warning + a bare `vehicle` root instead of failing
      to link, a topology YAML that names `kind: mock` degrades those
      specific entities to grouping nodes with a clear per-entity warning
      (mirroring the existing "adapter not compiled in" pattern uds_doip
      already had), and `test_uds_doip`'s 180 assertions still pass
      unchanged in that config.
      **`test_core`/`test_client` are not gated function-by-function** —
      they're made conditional on `SOVD_ADAPTER_MOCK` in `CMakeLists.txt`
      instead (same shape `test_uds_doip` already has for
      `SOVD_ADAPTER_UDS_DOIP`), so they simply aren't built in the
      restricted config rather than half-compiling. Deliberate: ~50 of
      `test_core`'s test functions exercise the mock adapter directly or
      via its `TestServer` fixture, and gating each individually would be a
      large, low-value mechanical diff — the HTTP-layer coverage that
      config combination would otherwise lose already exists redundantly in
      `test_uds_doip.cpp`, a real end-to-end test through `routes.cpp`
      against the real (non-mock) adapter. The actual security-relevant
      artifact (`sovd_server` compiling and running with no ECU adapter
      dead code) is what's fixed and verified; test-binary build hygiene in
      that exact configuration was the smaller concern.
      Restricted gateway build itself (Phase 8's broader item, `main.cpp`
      wired to require a config file with no ECU-facing adapters linked at
      all) is still unbuilt — this item was specifically the linkage
      breakage blocking it, now cleared.
- [ ] **mTLS gateway ↔ domain servers** — internal hop must verify the
      gateway's certificate, **not** trust a forwarded external bearer token
      (otherwise a leaked token becomes lateral movement)
- [x] **OAuth2 / token auth at the external boundary — DONE, 2026-08-20,
      scoped narrower than the phrase suggests.** Not a full OAuth2
      authorization server (auth-code flow, client registration, refresh
      tokens) — issuing tokens is a real IdP's job and a distinct large
      subsystem this project was never going to build; *verifying* bearer
      tokens and their scopes is the resource server's actual
      responsibility, and that's what actually protects the diagnostic
      interface, so that's what's built. Self-issued HMAC-SHA256 signed
      tokens (`server/include/sovd/server/oauth2.hpp`,
      `server/src/oauth2.cpp`) — a minimal JWT (header.payload.signature,
      base64url each part via OpenSSL's `EVP_EncodeBlock`/`DecodeBlock`,
      HMAC via OpenSSL's `HMAC()`), the same "hand-roll a minimal protocol
      slice" call already made for MQTT in Phase 3, and a resource server
      that only ever verifies tokens it minted itself doesn't need
      algorithm negotiation (so the header's `alg` field is never read back
      to select behavior — no algorithm-confusion surface, unlike a general
      JWT library). Signature comparison is constant-time. Opt-in via
      `SOVD_OAUTH2_SECRET` (same shape as `SOVD_MQTT_HOST`/`SOVD_CORS_
      ORIGINS`/`SOVD_AUDIT_LOG_PATH`) — unset means no auth check at all.
      **Scope-to-route mapping** (`routes.cpp`'s `oauth2_scope_table()`)
      uses exactly the three scopes CLAUDE.md's own Phase 1 client-config
      sketch already named (`read:faults`, `read:data`, `execute:routines`)
      rather than inventing finer-grained ones: reads of faults/data need
      the matching `read:*` scope; every mutating or privileged route (PUT
      data, POST modes/operations, all three lock verbs, DELETE faults)
      needs `execute:routines`; `GET /v1/entities` and `GET .../docs` need
      only *a* valid token, no specific scope (self-description, not
      diagnostic data — the same reasoning `/docs` already uses to return
      200 with no backend rather than 501); `GET /` needs no token at
      all — a client must be able to learn `api_versions` before it has one
      to use. Checked in the same pre-routing hook as CORS/the gateway
      whitelist, after both, so an unauthenticated caller never reaches
      routing logic. Denials emit an `oauth2_denied` event (method, path,
      status, correlation_id) for the same IDS-signal reason
      `gateway_route_denied` does.
      **`tools/mint_token.cpp` → `sovd_mint_token`**: a tiny standalone tool
      (secret, comma-separated scopes, ttl → prints a token), added because
      there's no real IdP in this project to get a token from otherwise,
      and this feature needed to be live-verifiable the same way every
      other Phase 8 item was. Not a production tool — a real deployment's
      tokens come from wherever it manages secrets/identity.
      OpenSSL is now a **required**, not auto-detected, dependency
      (`find_package(OpenSSL REQUIRED)`), needed here for HMAC and also by
      the mTLS item below — confirmed present in this environment (3.6.2).
      `CPPHTTPLIB_OPENSSL_SUPPORT` is defined **globally** (every target,
      not per-target) since it changes `httplib::Server`/`Client`'s class
      layout via conditional members — a per-target mismatch would be an
      ODR violation across the static-library boundaries this project
      already has (`sovd_server_lib`, `sovd_client`).
      **Tests**: `test_http_oauth2_root_exempt_and_missing_token_rejected`,
      `test_http_oauth2_wrong_secret_and_expired_token_rejected`,
      `test_http_oauth2_valid_token_missing_scope_gets_403`,
      `test_http_oauth2_valid_token_with_scope_succeeds` (read, discovery,
      and a real write all through their correct scopes) — 531 assertions
      passing in `test_core` (up from 512). A standalone smoke test
      (mint → verify, plus wrong-secret/tampered/expired all correctly
      rejected) was run directly against `oauth2.cpp` before wiring it into
      `routes.cpp`, to isolate crypto-correctness from HTTP-layer wiring.
      **Verified live** against a real running server
      (`SOVD_OAUTH2_SECRET=demo-secret`): `GET /` with no token succeeds;
      a scoped read with no token gets `401` + `WWW-Authenticate: Bearer`;
      the same read with a `read:faults`-only token gets `403`; with a
      `read:data` token (minted via `sovd_mint_token`) it succeeds — all
      four cases, plus both `oauth2_denied` events landing in the server's
      event stream with correlation ids, confirmed against the real binary.
- [ ] **SecurityAccess (`0x27`) wiring — needs BOTH halves, see D2.** The
      mechanism is already built and tested in isolation in Phase 2
      (`uds_services` requestSeed/sendKey, plus the clearly-labelled
      `derive_key_DEMO_ONLY_NOT_SECURE` stand-in). It is unwired because
      there was nothing to gate on. Wiring it means:
      - adding a catalog field (`requires_security_level`, parallel to the
        existing `requires_session`) = what the ECU demands, **and**
      - an OAuth2 scope check = whether this client may request that level.
      One without the other is incomplete. Real key derivation stays
      OEM-proprietary; the stand-in must remain visibly labelled.
- [x] **Default-deny path/method whitelist at the gateway tier — DONE,
      2026-08-20.** `routes.cpp`'s existing pre-routing hook (already home to
      CORS and per-request timing) gained one more check: when `role_ ==
      "gateway"`, every request's (method, path) is matched against
      `gateway_route_whitelist()` — a small explicit `(method, regex)` table
      mirroring the patterns `register_routes()` itself registers — *before*
      entity lookup or adapter dispatch runs. No match → `403 FORBIDDEN`,
      never reaching routing logic at all. Domain-role servers are untouched
      (the check is gated on role, so `vehicle/body`'s domain server still
      404s on an unknown path exactly as before). A deliberate, explicit
      second list rather than introspecting httplib's internal handler
      table (it doesn't expose one) — an auditable security policy, not an
      accident of whatever got registered; the real payoff is that a
      *future* route added to `register_routes()` without a matching
      whitelist entry is unreachable through the gateway by default instead
      of silently exposed, matching "capability reduction by linkage >
      runtime checks" applied at the HTTP layer (linkage alone can't gate
      this since gateway and domain share one binary/route table). Denials
      emit a `gateway_route_denied` event (method, path, correlation_id) —
      exactly the kind of signal Phase 3's alerting cares about (a client
      probing the gateway for unexpected paths).
      **Drift check, not just a positive test:** `test_core.cpp` has
      `test_http_gateway_whitelist_allows_every_real_route()`, which walks
      every actual registered route (root, entities, faults get/delete,
      data batch/item/put, modes, operations, locks post/put/delete, docs,
      the SSE stream) through a `role=gateway` `TestServer` and asserts none
      of them get the whitelist's specific denial message — if a future
      route is added to `register_routes()` without a matching whitelist
      entry, this test fails as a spurious denial rather than the drift
      going unnoticed. `test_http_gateway_whitelist_denies_unlisted_path()`
      confirms an unlisted path gets `403`/`FORBIDDEN`.
      **Verified live** against the real two-tier demo
      (`config/domain_body.yaml` + `config/gateway.yaml`): legitimate
      proxied routes (`/v1/entities`, `/v1/entities/vehicle/body/bcm/docs`)
      return `200` through the gateway; `GET /v1/admin/debug` (not a real
      route, standing in for "some future route") returns `403 FORBIDDEN`
      through the gateway but a plain `404` on the domain server on the same
      path — confirming the whitelist is gateway-only, not a global change;
      the `gateway_route_denied` event fired with a correlation id in the
      gateway's event stream for the denied request.
- [x] **Resource limits as a security property — DONE, 2026-08-20.**
      Corrects a stale claim in this file: `SOVD_MAX_DATA_LEN` did **not**
      actually exist anywhere in the codebase before this pass — all four
      limits below are new.
      - **Entity-tree depth ceiling**: `kMaxEntityPathDepth = 16` in
        `entity_registry.hpp`; `add_entity()` rejects (returns `false`, same
        failure path as orphan/duplicate rejection) any path deeper than
        that. Far beyond any real vehicle topology; bounds recursive/listing
        work an attacker-controlled config or proxy chain could otherwise
        force unboundedly deep.
      - **Lock TTL ceiling**: `kMaxLockTtlSeconds = 3600` in
        `lock_manager.hpp`. **Clamped, not rejected** — `acquire()` and
        `renew()` both cap the requested TTL — so an over-generous request
        degrades to "as long as we'll allow" instead of failing outright.
        Comfortably above both the CLI's 60s default and D3's 10s browser
        TTL, resolving the noted D3 interaction: the ceiling had to sit
        above every legitimate client's real TTL, and 3600s does. `routes.cpp`
        now echoes the actually-*granted* (clamped) `ttl_seconds` in the
        `POST`/`PUT .../locks` response body, not the raw requested value —
        a client asking for an absurd TTL sees the true grant, not a lie.
      - **Max concurrent locks, doubling as the session cap**: no new
        tracking needed — `LockManager::held_lock_count()` counts currently
        unexpired locks, and since UDS session escalation is *always*
        lock-gated (the settled session-manager-ownership decision above),
        "how many entities are locked right now" already bounds "how many
        entities could have an escalated session right now." `routes.cpp`
        enforces `kMaxConcurrentLocks = 64` in `handle_post_lock`, emitting
        `lock_denied` (`reason: "capacity"`) and `503 BUSY` at the cap — the
        policy decision lives in `Router`, `LockManager` just answers the
        query. Reused an existing mechanism instead of inventing new session
        tracking, matching the same instinct as the Phase 2 session-manager
        design.
      - **Max request body**: `svr.set_payload_max_length(64 * 1024)` in
        `main.cpp` — a native `httplib` feature, not hand-rolled. Every
        legitimate SOVD write body is a few bytes of JSON; 64KiB is generous
        headroom, not a real ceiling on anything valid.
      **Tests**: `test_registry_rejects_excessive_depth` (boundary-exact:
      depth 16 allowed, 17 rejected), `test_lock_acquire_clamps_excessive_ttl`
      / `test_lock_renew_clamps_excessive_ttl` (via `FakeClock`, proving the
      raw huge TTL was never actually honored), `test_lock_held_lock_count_
      tracks_active_locks_only`, `test_http_lock_post_rejects_at_server_
      wide_capacity` (real HTTP through `TestServer`, pre-filling 64
      synthetic locks directly via `LockManager::acquire()` then asserting a
      real `POST .../locks` gets `503`, then that releasing one filler
      unblocks a subsequent `201`). 509 assertions passing in `test_core`
      (up from 492), `test_uds_doip` (180) and `test_client` (32) unaffected.
      **Verified live** against `config/domain_body.yaml`: a normal small
      write still succeeds (`204`); a 70KB oversized `PUT` body is rejected
      with `413`; `POST .../locks -d '{"ttl_seconds":999999999}'` returns
      `{"lock_id":"lock-2","ttl_seconds":3600}` — both the internal clamp and
      the corrected response-echo confirmed end-to-end, not just at the unit
      level.
- [x] **Per-adapter bounded request queue + connection pooling — DONE,
      2026-08-20, one mechanism for both.** uds_doip already had this by
      construction (`UdsDoipContext` holds one `DoipTransport` for the
      adapter's whole lifetime — routing activation happens once, not per
      request); the real gap was `sovd_proxy` forwarding, where
      `try_forward()` built a fresh `httplib::Client` (fresh TCP handshake)
      on every single forwarded request. Fixed with `ProxyConnection`
      (`routes.cpp`): one persistent `httplib::Client` per proxied entity,
      created in `attach_proxy()` alongside its `ProxyTarget` (both before
      `svr.listen()` starts, so the lookup in `try_forward()` needs no
      locking of its own). A `std::try_lock` on the connection's mutex
      doubles as the bounded queue: a second concurrent request against the
      *same* proxied entity while one is already in flight gets a clean
      `503`, depth 1, rather than queueing behind the connection — matching
      that a single entity is already effectively serialized by
      `LockManager` for every lock-gated operation anyway. `routes.hpp`
      forward-declares both `httplib::Client` and `ProxyConnection` (an
      out-of-line `~Router()` handles the resulting incomplete-type member)
      so this doesn't pull `httplib.h` into the header.
      **Tests**: `test_http_proxy_bounded_queue_returns_503_on_concurrent_
      request` — a synthetic "slow domain" double blocks its handler on a
      condition variable (no sleep) until explicitly released, giving
      deterministic control over a real two-thread race: a background
      request occupies the entity's one connection, a second concurrent
      request from a separate `httplib::Client` gets `503`, then releasing
      the first proves it still completes normally (`200`). Existing proxy
      tests (`test_config_loader_proxy_forwards_docs_data_and_locks`) are
      unchanged and still pass, proving the pooling refactor didn't change
      correctness. 512 assertions passing (up from 509).
      **Verified live** against the real two-tier demo: sequential repeated
      reads to the same proxied entity all return `200` (the persistent
      connection survives reuse, doesn't go stale); firing 20 real parallel
      curls at the same proxied entity produced a mix of `503`s and `200`s
      matching the depth-1 bounded-queue behavior exactly, live, not just
      in the synthetic unit test.
- [x] **Explicit persistence decisions — DONE, 2026-08-20.** Locks: already
      correct by construction, no code needed — `LockManager` is pure
      in-memory with no persistence layer at all, so a restart already
      releases every held lock. Job status: nothing to decide — async job
      polling is a stated non-goal, so there's no job-status concept in this
      codebase to persist. Audit logs (the actual gap): security events
      (`Router::set_event_sink`) previously only ever reached stdout or an
      opt-in MQTT publish — stdout is lost with the process, and a down or
      unreachable broker means MQTT never durably lands an event at all, so
      neither actually satisfied "audit logs should survive a restart."
      Fixed with an opt-in `SOVD_AUDIT_LOG_PATH` env var (`main.cpp`, same
      shape as `SOVD_MQTT_HOST`/`SOVD_CORS_ORIGINS`): when set, every
      security event is also appended (`std::ios::app`, never truncated) to
      that file, mutex-serialized since `std::ofstream` isn't safe for
      concurrent writers and `event_sink_` is invoked from httplib's
      worker-thread pool. Composes with MQTT rather than replacing it —
      both fire per event if both are configured; deliberately scoped to
      `event_sink_` only, not `telemetry_sink_` (per-request latency),
      matching the existing "an IDS should not be your APM" separation.
      **Verified live**: with `SOVD_AUDIT_LOG_PATH=/tmp/sovd_audit.log` and
      no MQTT configured, a real `POST .../locks` produced a `lock_acquired`
      line in the audit file and *not* on stdout (stdout carried only the
      `http_request` telemetry line, confirming `event_sink_` was correctly
      replaced while `telemetry_sink_` stayed on its own default) — the
      separation and the file-sink wiring both confirmed against the real
      server, not just read from the code.

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
- **Any background timer must be interruptible — `condition_variable::wait_for`
  with a stop predicate, never `std::this_thread::sleep_for`.** An
  uninterruptible sleep cannot be woken by `stop` + `notify_all()`, so the
  thread destroying the object blocks for up to a full interval — and that
  thread is often an HTTP worker. This has now been hit **twice**: `LockGuard`'s
  lock heartbeat (Phase 5) and `StreamHub`'s shared poller (Phase 6), the second
  caught only because a test legitimately hung for a 60-second poll interval.
  Two instances is a pattern, not a coincidence; Phase 7/8 will add more timers
  (browser lock heartbeat, session idle-timeout, connection pooling), so treat
  this as a rule rather than rediscovering it a third time.
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

Phases 1 → 2 → 3 were the credible, finishable core — **done**.
Phases 4 → 6 made it architecturally serious — **done**.
Phase 7 is built; Phase 8 is polish and production posture — **remaining**.

**Immediate sequence from here:**
1. ~~Settle **D1**, **D2**, **D3**~~ — **done**, see OPEN DECISIONS above
2. ~~**B1** catalog `encode()` + validation~~ — **done**
3. ~~**B2** CORS, with SSE verified separately via `EventSource`~~ — **done**
4. ~~**B3** restricted-build fix~~ — **done**
5. ~~Phase 7 screens 1 → 2 → 3 → 4~~ — **built, 2026-08-20**; give it a real
   browser pass (visual/click-through) before treating it as fully verified,
   not just built — no browser automation was available in the session that
   wrote it
6. Phase 8, restricted build first

For interviews, Phases 1, 3, and 6 show understanding of *why* SOVD exists
rather than just an ability to serve JSON over HTTP. Self-description and
streaming are the actual advances over UDS/ODX; everything else is a nicer
transport for what UDS already did.
