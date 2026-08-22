# sovd-toolkit — Review Feedback & Action Items

**For:** the implementing agent (Claude Code)
**Reviewed at:** commit `4f7f8ed`
**Method:** built and ran every configuration; all findings reproduced live, not
inferred from reading.

Read this alongside `CLAUDE.md`. Nothing here changes a settled architectural
decision — these are integration gaps and hardening items on top of work that is
otherwise sound.

---

## Before you start: what is already correct

Do **not** "fix" any of these. They were verified working and several are subtle
enough that a well-meaning refactor could break them.

| Verified | Evidence |
|---|---|
| `test_core` 542, `test_client` 32, `test_uds_doip` 197 | all pass |
| B3 restricted build (`MOCK=OFF UDS_DOIP=ON`) | `nm` shows **zero** mock symbols linked |
| Token verification order | signature checked **before** payload parse — keep it that way |
| Constant-time signature compare | `constant_time_equal()` in `oauth2.cpp` |
| 401 vs 403 distinction | unauthenticated → 401, wrong scope → 403 |
| `alg=none` forgery, expired token | both → 401 |
| CORS allow-list | disallowed origin gets **no** `ACAO`; never `*` |
| Proxy does **not** forward `Authorization` | deliberate, per CLAUDE.md's mTLS rule — **do not "fix" this** |
| Two-tier typed read | `{"value":13.0,"unit":"V"}` through a gateway with no catalog |
| SSE through proxy → 501 | documented behaviour |
| B1 validation | bad enum → 400 w/ `/docs` pointer; read-only → 400; **raw hex write still 204** |
| Resource limits | 3 MB body → 413; TTL 99999 → clamped to 3600 |
| Repo hygiene | `certs/`, `.cache/` untracked; no private keys committed |

---

## TASK 1 — [HIGH] Make the web UI work with OAuth2 enabled

**Problem:** enabling `SOVD_OAUTH2_SECRET` breaks the entire Phase 7 UI.
Reproduced:

```
$ SOVD_OAUTH2_SECRET=demo-secret ./sovd_server config/domain_body.yaml
GET /v1/entities   (as the UI sends it)  -> 401
GET .../faults     (as the UI sends it)  -> 401

Preflight carrying Authorization:
  Access-Control-Allow-Headers: Content-Type, X-SOVD-Lock-Id, X-SOVD-Correlation-Id
  ^ 'authorization' absent => browser blocks the request before it is sent
```

**Root cause:** Phase 7 was browser-verified 2026-08-20; Phase 8 added OAuth2 the
same day, *after*. Both were verified honestly, in isolation. The combination
never was.

### 1a. Add `Authorization` to CORS `Allow-Headers`
- **File:** `server/src/routes.cpp:386`
- **Change:** append `Authorization` to the `Access-Control-Allow-Headers` list.
- Update the adjacent comment — it currently explains only the B2-era rationale
  for the three `X-SOVD-*`/`Content-Type` headers and predates bearer tokens.
- **Acceptance:** an `OPTIONS` preflight with
  `Access-Control-Request-Headers: authorization` returns a list containing
  `Authorization`.

### 1b. Give the UI a token field
- **Files:** `web/src/api/sovdClient.ts`, `web/src/App.vue`
- **Change:** optional bearer token on `SovdClient`, sent as
  `Authorization: Bearer …` on every `request()`. Add an input next to the
  existing base-URL field in the Connect bar. Empty token ⇒ send no header, so
  the current no-auth demo keeps working unchanged.
- Surface `401` and `403` distinctly in the UI — "not authenticated" and "your
  token lacks the required scope" are different user problems and the server
  already distinguishes them.
- **Acceptance:** with `SOVD_OAUTH2_SECRET` set, pasting a token from
  `sovd_mint_token` makes screens 1–3 work; leaving it blank yields a clear 401
  message rather than a silent failure.

### 1c. SSE cannot carry a bearer token — decide and implement
**This is a browser API limitation, not a bug.** `LiveChart.vue` uses
`new EventSource(url)`, and `EventSource` **cannot set request headers**. Screen
4 stays broken under auth even after 1a and 1b.

**Do not reach for `?access_token=…`.** This server logs every request `path`
into its telemetry sink (`routes.cpp:441`), so a token in the query string is
written straight into the audit trail — worse here than in a typical web app.

**Recommended: a short-lived single-use stream ticket.**
1. `POST /v1/entities/{path}/data/{id}/stream-ticket` — normal bearer auth,
   `read:data` scope, returns an opaque ticket with a ~30 s TTL.
2. Client opens `EventSource(…/stream?ticket=…)`.
3. Server burns the ticket on connect; tickets are single-use and never logged
   (add the ticket param to a redaction list in the telemetry sink).

Alternatives, if you prefer, with their costs — pick one and record the choice
in CLAUDE.md as a settled decision:
- `fetch()` + `ReadableStream` instead of `EventSource`: can set headers, loses
  the automatic reconnect that motivated `EventSource`.
- Cookie auth for the stream endpoint only: simplest, adds CSRF surface to a
  diagnostic interface.

**Acceptance:** with OAuth2 enabled, screen 4 streams; no token or ticket value
appears anywhere in the telemetry/audit output; a replayed ticket is rejected.

### 1d. Documentation
- **File:** `README.md`
- Today §6 shows the UI against `config/domain_body.yaml` (no OAuth2) and §7
  shows OAuth2 against the built-in topology (no `cors_allowed_origins`). The
  two are never combined, so the docs don't currently promise something broken —
  but once 1a–1c land, **add one worked example that does combine them.**

---

## TASK 2 — [MEDIUM] Invert the OAuth2 scope table to default-deny

**File:** `server/src/routes.cpp`, `check_oauth2()`

```cpp
if (!matched) return OAuth2Result::Ok;   // <-- unmatched route = no auth
```

I diffed all 14 registered routes against the 13-entry scope table: **every route
is currently covered, so there is no live bypass.** The issue is posture. The
gateway route whitelist is default-deny; this is default-allow. A route added
later without a table entry is silently unauthenticated, and nothing complains.

**Change:** default-deny, with an explicit unauthenticated allow-list:
- `GET /` — version discovery must work before a client has a token
- `OPTIONS` (any path) — CORS preflight must stay unauthenticated, or 1a is moot

**The important half:** add a test that enumerates every route registered on the
`httplib::Server` and asserts each has a scope-table entry. Without that test
this regresses the first time someone adds an endpoint.

**Acceptance:** a deliberately-added unlisted test route returns 401 rather than
200; the coverage test fails if a route is added without a table entry.

---

## TASK 3 — [MEDIUM] OAuth2 is off in every shipped config, so D2's second half never runs

`has_oauth2_scope()` returns `true` when `oauth2_secret_` is empty
(`routes.cpp:324`) — correct for "auth disabled," but the consequence is that in
**all four shipped configs plus the built-in topology**, the
`execute:security_access` scope check never executes. A catalog item marked
`requires_security_level` is reachable by any unauthenticated caller.

The feature is correct and unit-tested. The *default posture* is the problem.

**Changes:**
1. Add `config/domain_body_auth.yaml` demonstrating the authenticated path.
2. In README's security-posture section, state plainly that OAuth2 is opt-in and
   that `requires_security_level` gating **depends on it being enabled**.
3. Document in CLAUDE.md that `SOVD_OAUTH2_SECRET`, `SOVD_AUDIT_LOG_PATH`,
   `SOVD_MQTT_HOST`, and the `SOVD_TLS_*` paths are **env-var only, with no YAML
   keys**. Env-var for the secret is the right call — secrets don't belong in a
   config file — but the config schema in CLAUDE.md doesn't mention it, so
   someone reading only the YAML concludes auth doesn't exist.

---

## TASK 4 — [MEDIUM] Document the domain-tier-auth dead end

The proxy forwards `X-SOVD-Lock-Id` but deliberately **not** `Authorization`
(`routes.cpp:545`). **This is correct** per CLAUDE.md's rule that the internal
hop verifies the gateway's certificate rather than trusting a forwarded external
token. Leave the behaviour alone.

But the consequence is undocumented: if a **domain** server also sets
`SOVD_OAUTH2_SECRET`, every gateway-proxied request to it fails 401 with no
recourse. The supported topology is *OAuth2 at the gateway, mTLS on the internal
hop*.

**Change:** state this explicitly in CLAUDE.md (Phase 8 section) and README. The
failure mode is confusing and looks like a proxy bug.

---

## TASK 5 — [LOW] Fix `-Wunused-function` in the adapter-free gateway build

```
$ cmake -DSOVD_ADAPTER_MOCK=OFF -DSOVD_ADAPTER_UDS_DOIP=OFF
config_loader.cpp:73:6: warning: 'attach_router_catalog_if_present' defined but not used
```

`attach_router_catalog_if_present()` is only called inside `#ifdef
SOVD_HAVE_MOCK` / `#ifdef SOVD_HAVE_UDS_DOIP` blocks, so with both off it's
orphaned.

Minor in itself — but this is the **proxy-only gateway**, precisely the
configuration Phase 8's "capability reduction by linkage" argument rests on, and
CLAUDE.md claims clean warnings "in every documented configuration."

**Change:** wrap the helper in
`#if defined(SOVD_HAVE_MOCK) || defined(SOVD_HAVE_UDS_DOIP)`.
**Then add this fourth configuration to the build matrix** so the claim stays
true — it is currently the only documented config nobody builds.

---

## TASK 6 — [LOW] Validate the JWT `alg` header

`verify_token()` recomputes HMAC-SHA256 unconditionally and never reads the
header.

**This is not exploitable** — `alg:none` and RS256→HS256 confusion both require
the verifier to *switch* on `alg`, and this one never does (confirmed: `alg=none`
→ 401). Add a `header.alg == "HS256"` check as defence in depth, so a future
refactor that introduces algorithm selection can't reintroduce the bug class.

---

## TASK 7 — [LOW] Add a writable numeric to the demo catalog

`test_core.cpp:499–512` covers out-of-range and negative float encoding. But
`catalogs/bcm.yaml` has no writable numeric — `battery_voltage` is `access:
read`, `door_lock_state` is an enum — so the **HTTP path** for numeric range
rejection is never exercised end to end, and neither is a typed numeric input
widget in the UI.

**Change:** add one `read_write` float (e.g. a calibration or setpoint DID).
Cheap, and it makes screen 3 demonstrate the scaled-number widget that B1 was
largely built for.

---

## TASK 8 — [LOW] Reconsider silent lock-TTL clamping

`ttl_seconds: 99999` → `201 {"ttl_seconds": 3600}`. Defensible — the response
tells the truth and clamping is friendlier than rejecting — but a caller that
doesn't read the body believes it holds a 27-hour lock.

**Change:** either return `400` for a TTL over the ceiling, or advertise the
ceiling in `/docs` so a client can clamp before asking. Pick one; record it.

---

## TASK 9 — [PROCESS] Add a cross-phase verification matrix

The failure mode behind Task 1 was **each phase verified honestly in isolation,
the combination never**. Both Phase 7 and Phase 8 claim live verification and
both claims were true.

**Change:** add to CLAUDE.md's Working Conventions —

> Features are now numerous enough that combinations matter more than individual
> features. Before marking a phase complete, run the cross-phase matrix: UI × auth,
> UI × gateway, auth × proxy, mTLS × proxy, restricted-build × each config. A
> feature that works alone and fails in combination is not complete.

Ideally encode this as a script (`scripts/cross_phase_check.sh`) that boots the
relevant server pairs and curls the combinations, so it's runnable rather than
aspirational.

---

## Suggested order

1. **Task 1a + 1b** (~1 hour) — restores screens 1–3 under auth
2. **Task 1c** (~half a day) — decide the SSE approach first, then implement
3. **Task 2** — default-deny + the route-coverage test
4. **Task 5** — one-line `#if` + fourth build config in the matrix
5. **Tasks 3, 4, 8** — documentation and one demo config
6. **Tasks 6, 7** — small hardening and demo-coverage items
7. **Task 9** — process guard so this class of gap doesn't recur

Tasks 1 and 2 are the ones that matter. Everything else is polish.

---

## Closing note for the agent

The architecture holds up under adversarial testing. The adapter seam does what
it was designed to do — a fully typed read through a gateway that has never
parsed a catalog is the proof that Phase 4's proxy correction was right. Phase
8's security work is real rather than decorative: the restricted build genuinely
excludes the adapters, the proxy genuinely refuses to forward tokens, and the
token verifier resists the attacks people usually get wrong.

Task 1 is a seam between two phases finished on the same day, not a design flaw.
But it is findable in five minutes by anyone who sets `SOVD_OAUTH2_SECRET` and
opens the UI, so fix it before this project is shown to anyone.

**When updating CLAUDE.md after this work:** record 1c's SSE choice and 8's TTL
choice as *settled decisions* with reasoning, in the same style as the existing
D1/D2/D3 entries — so they don't get relitigated later.
