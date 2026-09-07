# Threat Analysis and Risk Assessment — sovd-toolkit

Structured per **ISO/SAE 21434:2021**: clause 9 (concept phase — item
definition, cybersecurity goals) frames the analysis; clause 15 (TARA methods)
provides the method for the body.

**Scope note.** This is a portfolio project, not a production item, and no
distributed cybersecurity activities, supplier agreements, or independent review
have taken place. What is claimed here is that the analysis method is applied
correctly and that every stated mitigation is traceable to code in this
repository. Risk values are the author's judgement.

| | |
|---|---|
| Item | sovd-toolkit SOVD server, adapters, and client SDK |
| Version | *(fill in: commit SHA or tag)* |
| Method | ISO/SAE 21434 clause 15, CAL not assigned (no vehicle programme) |
| Author / date | *(fill in)* |

---

## 1. Item definition (clause 9.3)

### 1.1 Item boundary

Inside the boundary:

- SOVD server process (HTTP layer, entity registry, lock manager)
- UDS/DoIP adapter and its session manager
- SOVD-to-SOVD proxy forwarding path at the gateway tier
- Client SDK, CLI, and Vue web UI
- DID catalog parsing and value decode/encode

Outside the boundary, but interacting with it:

- Classic ECUs (SOVD-unaware, plain UDS)
- The OAuth2 authorization server issuing tokens
- The MQTT broker and the monitoring pipeline behind it
- The host OS, its TLS library, and the vehicle network stack

### 1.2 Operational environment and assumptions

State each assumption explicitly — an assumption that turns out false is a
finding, and reviewers look for this section specifically.

- **A1** — The domain-tier server is reachable only from the vehicle-internal
  network; the gateway is the sole externally reachable tier.
- **A2** — The host provides a trusted boot chain and a read-only root
  filesystem for the server binary.
- **A3** — Private keys and the OAuth2 secret are provisioned out of band and
  are not stored in the repository or in any topology YAML.
- **A4** — ECUs enforce their own SecurityAccess; the server's checks are
  defence in depth, not the primary control.
- **A5** — Physical access to the vehicle bus is out of scope for this item.

### 1.3 Assets and cybersecurity properties (clause 15.3)

| ID | Asset | Property at stake | Rationale |
|---|---|---|---|
| AS-01 | Write access to ECU data (`0x2E`) | Integrity | A written DID can change vehicle configuration |
| AS-02 | Routine execution (`0x31`) | Integrity, availability | Routines can actuate |
| AS-03 | SecurityAccess unlock state (`0x27`) | Integrity | Gates the privileged UDS surface |
| AS-04 | Diagnostic session / lock state | Availability, integrity | Lock exhaustion denies legitimate testers |
| AS-05 | VIN and diagnostic telemetry | Confidentiality | Personal data under GDPR |
| AS-06 | OAuth2 tokens and stream tickets | Confidentiality | Bearer credentials |
| AS-07 | mTLS private keys | Confidentiality | Identity of a tier |
| AS-08 | Audit log and MQTT event stream | Integrity, availability | Loss defeats detection |
| AS-09 | Server availability | Availability | A DoS'd diagnostic tier blocks service work |

### 1.4 Cybersecurity goals (clause 9.4)

Derived from the risks in section 3; listed here because clause 9 places them in
the concept phase.

- **CG-01** — Only an authenticated, appropriately scoped client shall cause a
  write, routine execution, or SecurityAccess request to reach an ECU.
- **CG-02** — A compromised gateway tier shall not be able to emit UDS frames.
- **CG-03** — Diagnostic session and lock state shall not be exhaustible by an
  unauthenticated client.
- **CG-04** — Security-relevant events shall be observable off-host.
- **CG-05** — Credentials shall not be recoverable from the repository, the
  configuration files, or the logs.

---

## 2. Threat scenarios and attack paths (clause 15.4, 15.6)

One row per threat scenario. Damage scenario first (what the road user
experiences), then the threat scenario (what the attacker does to cause it).

| ID | Damage scenario | Threat scenario | Asset | Attack path (summary) |
|---|---|---|---|---|
| TS-01 | Unauthorised configuration change to a body ECU | Attacker on the vehicle network issues a `PUT .../data/{id}` without valid credentials | AS-01 | Reach domain tier directly, bypassing the gateway |
| TS-02 | Unintended actuation | Attacker executes a routine using a token scoped only for reads | AS-02 | Scope table missing an entry for a newly added route |
| TS-03 | Privileged UDS surface unlocked | Attacker reaches a `requires_security_level` item on a server running with OAuth2 disabled | AS-03 | Default shipped config has `SOVD_OAUTH2_SECRET` unset |
| TS-04 | Legitimate technician cannot service the vehicle | Attacker acquires locks until the cap is reached | AS-04, AS-09 | Repeated `POST .../locks` without release |
| TS-05 | Vehicle keeper identified from captured traffic | VIN observed in cleartext or in a log sink | AS-05 | Passive capture on an unencrypted hop |
| TS-06 | Session hijack | Stream ticket replayed by a second client | AS-06 | Ticket observed in a URL query string or a server log |
| TS-07 | Gateway impersonation | Attacker presents a certificate from the system trust store | AS-07 | Server validates against the public CA set rather than the private demo CA |
| TS-08 | Intrusion goes unnoticed | Event feed suppressed while an attack proceeds | AS-08 | MQTT broker unreachable, events dropped silently |
| TS-09 | Compromised gateway reaches the bus | Code execution in the gateway process used to send UDS | AS-01, AS-02 | Attacker loads or calls ECU-facing code |
| TS-10 | Resource exhaustion | Oversized request body or deeply nested entity path | AS-09 | Unbounded parse |

*(TS-11+ : add SSE poller exhaustion, catalog-parsing malformed YAML, NRC
mapping leaking ECU state, proxy trust of a forwarded lock ID.)*

---

## 3. Risk determination (clause 15.5, 15.7, 15.8)

Impact rated per clause 15.5 across Safety / Financial / Operational / Privacy
(S/F/O/P), each Negligible → Severe. Attack feasibility per clause 15.7 using the
attack-potential approach (elapsed time, expertise, knowledge of the item,
window of opportunity, equipment). Risk value 1–5 from the clause 15.8 matrix.

| ID | S | F | O | P | Feasibility | Risk | Treatment (15.9) |
|---|---|---|---|---|---|---|---|
| TS-01 | Moderate | Negligible | Major | Negligible | Medium | 4 | Reduce |
| TS-02 | Major | Negligible | Major | Negligible | Low | 4 | Reduce |
| TS-03 | Major | Negligible | Moderate | Negligible | High | 5 | Reduce |
| TS-04 | Negligible | Moderate | Major | Negligible | High | 3 | Reduce |
| TS-05 | Negligible | Negligible | Negligible | Major | Medium | 3 | Reduce |
| TS-06 | Moderate | Negligible | Moderate | Moderate | Low | 3 | Reduce |
| TS-07 | Major | Negligible | Major | Negligible | Low | 4 | Reduce |
| TS-08 | Negligible | Negligible | Moderate | Negligible | Medium | 2 | Reduce |
| TS-09 | Severe | Moderate | Major | Negligible | Low | 4 | Reduce |
| TS-10 | Negligible | Negligible | Moderate | Negligible | High | 3 | Reduce |

Fill these in with your own judgement rather than keeping mine — being able to
justify a rating out loud is the point of the exercise, and an interviewer will
ask about at least one row.

---

## 4. Traceability: threat → control → code

This is the section that distinguishes the document. Every row must point at a
file that exists.

| ID | Control | Implemented in | Verified by |
|---|---|---|---|
| TS-01 | OAuth2 bearer + per-route scope gate, default-deny | `server/src/oauth2.cpp` (`verify_token`/`has_scope` — signature + expiry check), `server/src/routes.cpp` (`check_oauth2()`, `oauth2_scope_table()` — per-route enforcement and default-deny fallthrough) | `tests/test_core.cpp` |
| TS-02 | Route with no scope-table entry still requires a valid token | `server/src/routes.cpp` (`check_oauth2()` — an unmatched route falls through to "any valid token", never to `Ok` with no token; `oauth2.cpp` only verifies token signatures and has no per-route logic) | `tests/test_core.cpp` |
| TS-03 | Dual gate: catalog `requires_security_level` **and** `execute:security_access` scope | `catalog/src/did_catalog.cpp` (field parsing), `adapters/uds_doip/src/uds_doip_adapter.cpp` (`ensure_security_for_did` — the ECU-demand half), `server/src/routes.cpp` (`has_oauth2_scope`/`handle_put_data` — the caller-permission half; `uds_services.cpp` only has the low-level `0x27` seed/key encode/decode primitives, not the gating decision) | `tests/test_uds_doip.cpp`, `tests/test_core.cpp` |
| TS-04 | Max concurrent locks, lock TTL ceiling, RAII heartbeat release | `server/src/routes.cpp` (`kMaxConcurrentLocks`, `handle_post_lock` — the cap is enforced here; `LockManager` only exposes the counter), `core/src/lock_manager.cpp` (`kMaxLockTtlSeconds` clamp), `client/src/lock_guard.cpp` (heartbeat + release) | `tests/test_core.cpp` |
| TS-05 | `correlation_id` in event sinks in place of VIN | `server/src/routes.cpp` (every `emit_event(...)` call site — `mqtt_publisher.cpp` is pure MQTT packet framing and never touches payload content, so it was the wrong file) | confirmed by direct code read, 2026-09-08: `git grep -i '\bvin\b' -- server/` returns nothing; every event carries `entity`/`path`/`correlation_id` only |
| TS-06 | CSPRNG-generated, single-use, burn-on-redeem stream tickets with expiry sweep | `server/src/stream_ticket.cpp` | `tests/test_core.cpp` |
| TS-07 | mTLS verified against a private CA, not the system trust store | `server/src/main.cpp` (`SSLServer` construction — server-side client-cert verification), `server/src/routes.cpp` (`ProxyConnection`'s `set_ca_cert_path`/`enable_server_certificate_verification` — gateway-side CA pinning), `scripts/generate_demo_certs.sh` (the private demo CA; `config_loader.cpp` only parses the YAML into these fields, it doesn't perform the verification) | manual, `config/*_mtls.yaml` |
| TS-08 | Audit log to disk independent of the MQTT feed | `server/src/main.cpp` (`SOVD_AUDIT_LOG_PATH` wiring — composes the audit-file sink with the MQTT sink; `mqtt_publisher.cpp` has no involvement, it's a separate feed entirely) | manual |
| TS-09 | Gateway build links no ECU-facing adapter; static selection at CMake configure time, no `dlopen` | `CMakeLists.txt` | CI matrix, `scripts/cross_phase_check.sh` |
| TS-10 | Max request body, entity tree depth cap | `server/src/main.cpp` (`set_payload_max_length(64 * 1024)`), `core/src/entity_registry.cpp` (`kMaxEntityPathDepth` check in `add_entity`; `routes.cpp` has neither of these) | `tests/test_core.cpp` |

---

## 5. Residual risk and accepted limitations

State these plainly. A TARA that claims everything is mitigated reads as
marketing; a reviewer trusts the document more for the honesty, and these are
already documented elsewhere in the repo.

- **TS-03 partially unmitigated by default.** The scope half of the
  SecurityAccess gate only runs when OAuth2 is enabled, and every shipped config
  leaves `SOVD_OAUTH2_SECRET` unset. On a default deployment a
  `requires_security_level` item is reachable by any caller who can reach the
  server. Accepted for a demo posture; unacceptable in a vehicle.
- **A1 is load-bearing.** Domain-tier network isolation is assumed, not
  enforced by this item.
- **No secure boot or runtime attestation** is provided by this project; A2 is
  delegated to the platform.
- **The OAuth2-on-domain-tier topology is unsupported**, because the proxy
  forwards `X-SOVD-Lock-Id` but never `Authorization`. Documented in the README.
- **No independent review or penetration test** has been performed.

---

## 6. Mapping to UN-R155

Kept in [`COMPLIANCE.md`](COMPLIANCE.md), which maps each threat above to the
relevant UN-R155 Annex 5 Part A threat identifier and the corresponding Part B
mitigation.
