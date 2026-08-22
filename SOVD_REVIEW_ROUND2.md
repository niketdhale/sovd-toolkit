# sovd-toolkit — Review Round 2

**For:** the implementing agent (Claude Code)
**Reviewed at:** commit `658d7a2`
**Previous round:** `SOVD_REVIEW_FEEDBACK.md` (commit `4f7f8ed`)
**Method:** rebuilt every configuration and re-ran every task's acceptance
criteria independently, rather than trusting the commit message.

---

## Round 1: all 9 tasks verified fixed

| Task | Claim | Independently verified |
|---|---|---|
| 1a | CORS allows `Authorization` | ✅ preflight returns `Content-Type, Authorization, X-SOVD-Lock-Id, X-SOVD-Correlation-Id` |
| 1b | UI token field, 401 vs 403 | ✅ `type="password"`, `autocomplete="off"`, **in-memory only** (no `localStorage`); distinct error copy per status |
| 1c | SSE works under auth via ticket | ✅ full flow below |
| 2 | Scope table default-deny | ✅ `/v1/futurefeature`, `/v1/entities/vehicle` → 401; `GET /` → 200; `OPTIONS` → 204 |
| 3 | Auth demo config + env-var docs | ✅ `config/domain_body_auth.yaml` present; docs correct (see note) |
| 4 | Domain-tier-auth dead end documented | ✅ README + CLAUDE.md |
| 5 | `MOCK=OFF UDS_DOIP=OFF` warning | ✅ clean; all 4 configs build warning-free |
| 6 | JWT `alg` check | ✅ present |
| 7 | Writable numeric in catalog | ✅ second `read_write` item added |
| 8 | TTL ceiling advertised | ✅ `GET /` → `limits.lock_ttl_ceiling_seconds: 3600` |
| 9 | Cross-phase script | ✅ **14 passed, 0 failed** |

**Assertions:** 583 + 197 + 32 = **812**, all passing.
**B3 not eroded:** `nm` on the restricted-build `sovd_server` still shows **zero**
mock symbols.

### Task 1c, verified in detail
```
mint without token            -> 401
mint with token               -> tkt-<32 hex>  (36 chars)
stream without ticket         -> 401
stream with ticket            -> 200, live SSE data flowing
replay same ticket            -> 401   (single-use holds)
battery_voltage ticket on vin -> 401   (bound to (path, id))
ticket value present in log?  -> no
```

`redeem()` erasing *before* validating is a good detail — a wrong-entity ticket
can't be retried against another guess.

### Correction accepted
Round 1's Task 3 claimed `SOVD_TLS_*` was env-var-only alongside
OAuth2/MQTT/audit. That was **wrong** — `server.tls: {cert, key, client_ca}` is
YAML-configurable and is used in `domain_body_mtls.yaml`. The new docs
distinguish the two cases correctly. Pushing back on the review rather than
accepting it wholesale was the right call.

---

## TASK 10 — [MEDIUM] `StreamTicketStore` is unbounded

**The one Phase 8 resource with neither a cap nor a reaper.**

`server/src/stream_ticket.cpp` only ever erases in `redeem()`. There is no
`reap_expired()` and no size limit, so tickets minted and never redeemed
accumulate **for the process lifetime**. The 30 s TTL makes them unusable but
never frees them.

Measured on a live server:

```
baseline RSS:        9436 KB
after 3000 tickets: 11216 KB   (last status 201)
```

~590 bytes per orphaned ticket, and minting still returns `201` — no cap at any
point. Every EventSource that fails to connect, every abandoned tab, every
retried connect leaks one.

**Why this matters here specifically:** it's inconsistent with its own
neighbours. `LockManager` purges stale entries on access, and `routes.cpp` caps
locks at `kMaxConcurrentLocks = 64`. This project's stated Phase 8 position is
that unbounded anything is a DoS on a safety-adjacent interface — the ticket
store is the exception.

Severity is genuinely **medium, not high**: minting requires a valid `read:data`
bearer token, so this is post-authentication.

**Fix** — both halves, mirroring patterns already in the codebase:
1. `kMaxOutstandingTickets` (64 is consistent with the lock cap). Over the cap,
   return `503` from `handle_post_stream_ticket`, the same shape as the lock
   route's existing over-cap path.
2. Opportunistic expiry sweep inside `issue()`, under the mutex it already
   holds. No background thread, and therefore no new timer — which keeps it
   clear of the `wait_for`-not-`sleep_for` convention entirely.

Sweeping first also means the cap is only reached by genuinely concurrent
outstanding tickets, not by historical churn.

**Acceptance:** minting past the cap returns 503; RSS is flat across repeated
mint-and-abandon cycles once the TTL has elapsed; a unit test with the injected
clock asserts expired tickets are dropped without a redeem.

---

## TASK 11 — [LOW] Ticket RNG is not cryptographic

`issue()` uses `thread_local std::mt19937_64` seeded from `std::random_device`.

The comment's reasoning is right as far as it goes — two 64-bit draws because
this token grants access, versus the correlation id's one draw that only needs
to avoid collision. But **Mersenne Twister is not a CSPRNG**: its internal state
is recoverable from ~312 consecutive 64-bit outputs, i.e. ~156 observed tickets
from the same thread, after which all subsequent tickets from that thread are
predictable.

Impact is limited — an attacker needs a valid `read:data` token to observe
tickets at all, and with that token they could already read the same data. So
this is defence-in-depth, not a live escalation path.

But OpenSSL is **already linked** for the OAuth2 HMAC, so the fix is two lines:

```cpp
#include <openssl/rand.h>
unsigned char raw[16];
if (RAND_bytes(raw, sizeof raw) != 1) { /* fail closed */ }
// hex-encode raw into the tkt- prefix
```

A credential generated from a non-cryptographic PRNG is the sort of detail
that's cheap to fix now and awkward to defend later.

**Note:** the correlation-id generator can stay on `mt19937_64` — the existing
comment correctly identifies that it needs collision-avoidance, not
unpredictability. Only the ticket needs the change.

---

## Nothing else outstanding

Round 1's findings are closed. The architecture and Phase 8 security work
continue to hold up under adversarial testing: the restricted build genuinely
excludes adapters, the proxy genuinely refuses to forward tokens, the token
verifier resists `alg=none`, expiry, and forgery, and the new ticket path
resists replay and cross-entity reuse.

Tasks 10 and 11 are both small, both post-authentication, and both in the same
file. They can land as a single commit.

**When updating CLAUDE.md:** Task 10's cap value and sweep strategy are worth a
line in the Phase 8 resource-limits list alongside the existing four, so the
"every resource is bounded" claim becomes true rather than nearly true.
