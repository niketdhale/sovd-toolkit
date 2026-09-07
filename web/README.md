# SOVD Toolkit — Web UI (Phase 7)

Vue 3 + Vite + Tailwind, hosted separately from the vehicle (docs/DESIGN.md's
design constraint). **Zero knowledge of any specific ECU is baked into this
code** — every entity, data id, type, and value in the four screens below
comes from `GET /entities` and `GET /entities/{path}/docs` at runtime.
Hardcoding a DID here would throw away the actual reason SOVD beats ODX.

Points at a **domain server directly**, not the gateway (D1, settled
2026-08-20) — streaming through a Phase 4 `sovd_proxy` is a deliberate
`501`, so screen 4 can't run against the gateway tier.

## Run it

```bash
# 1. Start a domain server with CORS open for this dev server's origin.
#    config/domain_body.yaml already has cors_allowed_origins:
#    [http://localhost:5173] -- the Vite default port.
cd .. && ./build/sovd_server config/domain_body.yaml &

# 2. Start the UI.
cd web
npm install
npm run dev
```

Open <http://localhost:5173>, enter the domain server's base URL (defaults
to `http://localhost:20003`, editable — see `.env.example` for
`VITE_SOVD_BASE_URL`), click **Connect**.

Pointing this at a different server? Its `cors_allowed_origins` (or
`SOVD_CORS_ORIGINS` for the zero-config demo path) needs to include this
dev server's actual origin, or the browser will block every request —
that's the browser's same-origin policy doing its job, not a bug here.

## The four screens, then stop (docs/DESIGN.md's own scope line)

1. **Entities** — flat list from `/entities`.
2. **Faults** — read + status filter + clear (one-shot lock, no heartbeat —
   same `heartbeat=false` shape the CLI's `faults-clear` uses, since the
   whole operation is one quick round trip, never held open).
3. **Data** — typed read/write widgets built purely from `/docs`' `type`/
   `access`/`values` fields. Needs B1 (`Catalog::encode()`) — before that,
   `PUT` only took hex. Editing acquires a lock via `useLock` (D3: 10s TTL,
   JS heartbeat at ~5s, best-effort `beforeunload` release — not the CLI's
   60s default, since a closed tab has no RAII destructor).
4. **Live** — `EventSource` against `.../data/{id}/stream`; a hand-rolled
   SVG polyline for a float item (no charting dependency for one screen),
   a scrolling text log otherwise.

## What wasn't verified

No browser automation was available in the session that built this — every
API call pattern each screen makes (docs, batch read, typed write, the full
lock/heartbeat/release cycle, the SSE stream) was verified live with curl
using the real `Origin: http://localhost:5173` header against a real
running domain server, and `npm run build`/`vue-tsc` type-check clean. The
actual rendered UI (layout, click-through, console errors) has **not** been
visually exercised in a browser — do that before treating this as done.
