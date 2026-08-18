# Design Decisions for Async HTTP Egress (Phase 0)

This document records the resolved open questions from §7 of the async HTTP client architecture design (`async-http-client-architecture.md`). These decisions were reflected in the Phase 0 / Phase 1 implementation.

---

## §7.1 — Primary user scenario (Question #1)

**Decision:** Option B (Async Lookup Source) first, then Option A (Input Transform).

**Rationale:** The lookup layer already models batched keys, max-keys-per-request bounds, and result assembly. GET-style lookups sidestep the idempotency problem. Option C (table reads) is deferred.

**Status:** Deferred to Phase 2 — the egress layer (Phase 0/1) is scenario-agnostic.

---

## §7.2 — Idempotency policy (Question #2)

**Decision:** v1 is restricted to **GET / read-only lookups** with **at-least-once** delivery semantics.

**Rationale:**
- Transform re-runs if the stage restarts, so non-idempotent calls (POST with side effects) would cause duplicates.
- GET-style lookups are naturally retry-safe.
- Non-idempotent methods can be added in a later phase with explicit idempotency keys.

**Reflected in code:**
- The actor accepts any HTTP method but the intended v1 usage is GET-only.
- Documentation notes at-least-once semantics.

---

## §7.3 — Egress security default (Question #3)

**Decision:** **Allowlist-only** model. Empty `AllowedHosts` means **deny all**.

**Rationale:**
- Deny-by-default is the secure baseline: no egress until explicitly allowed.
- The `DeniedHosts` set provides an override to block specific hosts even when a broader wildcard allowlist is present.
- Administered by the cluster operator (not tenant admin) in v1.

**Reflected in code:**
- [`TEgressSecurityConfig::AllowedHosts`](ydb/library/yql/dq/actors/http/http_egress_security.h:17) — empty set means deny all.
- [`CheckHostPolicy()`](ydb/library/yql/dq/actors/http/http_egress_security.cpp:109) returns `false` when no hosts are allowed.
- SSRF protection blocks RFC1918, loopback, link-local, cloud metadata, and `0.0.0.0`.

---

## §7.4 — Auth methods for v1 (Question #4)

**Decision:** **Static token from YDB secrets** only.

**Rationale:**
- Static tokens are simple and sufficient for v1 read-only lookups.
- Service-account / IAM token exchange is deferred to a later phase.
- The `Authorization` header is reserved — the egress actor injects it from the configured secret, and user-provided `Authorization` headers are filtered out.

**Reflected in code:**
- `Authorization` is in the [`IsReservedHeader`](ydb/library/yql/dq/actors/http/http_egress_security.h:46) list.
- User-provided reserved headers are skipped in [`HandleHttpRequest`](ydb/library/yql/dq/actors/http/http_egress_actor.cpp:35).

---

## §7.5 — Response formats for v1 (Question #5)

**Decision:** **JSON + raw bytes**.

**Rationale:**
- JSON covers the majority of enrichment / lookup use cases.
- Raw bytes allows binary protocols and non-JSON APIs without additional parsing overhead.
- CSV and other structured formats can be added later if needed.

**Reflected in code:**
- The response body is passed through as raw bytes (`TString`) without format-specific parsing.
- No format validation is performed at the egress layer.

---

## §7.6 — Checkpointing for streaming queries (Question #6)

**Decision:** **Not addressed in Phase 0/1.** In-flight HTTP calls are not checkpointed. On restore, pending requests are lost and will be retried (consistent with at-least-once semantics).

**Rationale:**
- Checkpointing HTTP state requires replay protocol, which adds significant complexity.
- At-least-once semantics already tolerate lost in-flight requests.
- Deferred to a later phase when streaming query support is added.

**Status:** Deferred.

---

## §7.7 — TLS verification default (Question #7)

**Decision:** **TLS verification enabled by default** (verify server certificate against system CA bundle).

**Rationale:**
- Disabling TLS verification opens the system to MITM attacks, defeating the purpose of HTTPS.
- The HTTP proxy actor (`NHttp::CreateHttpProxy()`) uses the system CA bundle by default, which is the secure baseline.
- If the operator needs to use self-signed certificates, they can configure a custom CA bundle at the proxy level (not at the egress actor level).
- Allowing `VerifyTLS = false` as a configuration option is possible but **not recommended** and should require explicit operator approval.

**Reflected in code:**
- The egress actor delegates to `NHttp::CreateHttpProxy()` which uses default TLS settings (verification enabled).
- No `VerifyTLS` flag is exposed in `TEgressSecurityConfig` in v1 — TLS verification is always on.
- If custom CA support is needed, it can be added via the HTTP proxy configuration in a later phase.

**Status:** Phase 0 ✅ — TLS verification is always enabled by default.

---

## Summary Table

| Question | Topic | Decision | Phase |
|----------|-------|----------|-------|
| §7.1 | Primary scenario | Option B (Lookup) first | Phase 2 |
| §7.2 | Idempotency | GET/read-only + at-least-once | Phase 0 ✅ |
| §7.3 | Security default | Allowlist-only, deny-by-default | Phase 0 ✅ |
| §7.4 | Auth methods | Static token from secrets | Phase 0 ✅ |
| §7.5 | Response formats | JSON + raw bytes | Phase 0 ✅ |
| §7.6 | Checkpointing | Deferred — no checkpoint for in-flight calls | Deferred |
| §7.7 | TLS verification | Always enabled (system CA bundle) | Phase 0 ✅ |

---

## Related Documents

- [Architecture Design](async-http-client-architecture.md) — full design document
- [COVERAGE.md](ydb/library/yql/dq/actors/http/COVERAGE.md) — test coverage status
- [TEST_PLAN.md](ydb/library/yql/dq/actors/http/TEST_PLAN.md) — test plan
