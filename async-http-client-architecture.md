# Async HTTP access for YDB KQP/DQ compute: task, DoD and solution options

> Status: design draft (options review, no option selected yet).
> Scope: ability for a YDB query to call an external HTTP service during execution
> (enrichment, external UDF-like calls, HTTP-backed table reads).

## 1. Task

### 1.1 Problem statement

Today a KQP query can only work with data that already lives inside YDB tables or
with the fixed set of external data sources wired through the DQ provider stack.
There is no supported way to, **from within a running query**, call an arbitrary
external HTTP service and use its response as part of the computation.

We want to add **asynchronous HTTP access** to query execution so that:

- a query can enrich rows by calling an external HTTP endpoint (per-row or per-batch);
- a query can invoke an external "function" exposed over HTTP (ML inference,
  scoring, geocoding, etc.);
- a query can read a dataset exposed by an HTTP endpoint as if it were a table.

"Asynchronous" is a hard requirement: an in-flight HTTP call must **never block**
an actor-system thread. YDB is built on the actor model; a blocking network call
inside a compute thread starves unrelated work on the same mailbox/executor pool.

### 1.2 Non-goals (for the first iteration)

- Arbitrary user-supplied C++/network code (only declarative HTTP calls).
- Streaming request bodies of unbounded size.
- Acting as an HTTP *server* (this is client-side only).
- Cross-cluster distributed transactions over HTTP.

### 1.3 Key constraints imposed by the platform

These constraints come from how KQP/DQ actually work and must shape any design.

1. **No blocking in MiniKQL compute.** MiniKQL computation nodes are pull-based:
   the runtime calls
   [`IComputationNode::GetValue()`](yql/essentials/minikql/computation/mkql_computation_node.h:185)
   and stream nodes return
   [`EFetchResult`](yql/essentials/minikql/computation/mkql_computation_node.h:245)
   (`Finish` / `Yield` / `One`). There is **no** "suspend this node until my
   future resolves" primitive. `Yield` means "no data available *right now*, the
   whole task will be re-run"; it is not a per-node await. Therefore async I/O
   must be driven **outside** the MiniKQL graph, at the DQ compute-actor level.

2. **Async external I/O already has a home.** DQ has a first-class async I/O
   layer: [`IDqComputeActorAsyncInput`](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:68),
   [`IDqAsyncLookupSource`](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:236)
   and the factory
   [`IDqAsyncIoFactory`](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:252)
   with `CreateDqSource` / `CreateDqLookupSource` / `CreateDqInputTransform` /
   `CreateDqOutputTransform`. These actors already integrate with backpressure
   (`GetAsyncInputData(..., freeSpace)`), memory quotas (`IMemoryQuotaManager`),
   checkpointing (`SaveState`/`LoadState`), stats and secure params.

3. **There is already an `ExternalFunction` provider.** HTTP-invoked functions
   are partially modeled by the function provider under
   [`ydb/library/yql/providers/function`](ydb/library/yql/providers/function/provider/dq_function_provider.cpp:1):
   a `TDqTransform` carries an `InvokeUrl`, see
   [`TDqFunctionDqIntegration`](ydb/library/yql/providers/function/provider/dq_function_dq_integration.cpp:24).
   New work should extend this, not fork a parallel stack.

4. **There is already an actor HTTP client.** [`ydb/library/actors/http`](ydb/library/actors/http/http.h)
   provides non-blocking outgoing HTTP/HTTPS (proxy actor, TLS, HPACK, keep-alive).
   We should reuse it rather than build a new connection pool / TLS stack.

5. **Stage re-execution.** DQ stages can be retried/restarted. HTTP calls are
   side effects and are frequently non-idempotent (POST). Any design must define
   what happens on retry.

## 2. Definition of Done (DoD)

The feature is considered done when **all** of the following hold.

### 2.1 Functional

- [ ] A query can call an external HTTP endpoint and consume its response, using a
      documented, stable surface (SQL/UDF syntax or external data source object).
- [ ] Requests are fully non-blocking: no HTTP call ever blocks an actor executor
      thread (verified by design + a stress test that saturates in-flight calls).
- [ ] Request shaping is supported: method, URL/path, per-call timeout, and
      **arbitrary user-supplied headers and body** that may be computed per row
      (dynamic keys/values), with reserved auth/transport headers protected and
      header/body validated against injection and size limits.
- [ ] Response is delivered as typed data (at minimum: status code, headers, body;
      body parseable as JSON into declared columns).
- [ ] Per-row / per-batch enrichment works end to end on a multi-node cluster.

### 2.2 Resource safety & correctness

- [ ] Response bytes are accounted against the compute actor memory quota
      (via `IMemoryQuotaManager`), with configurable max request/response size.
- [ ] Backpressure works: a slow endpoint slows the query instead of unbounded
      buffering or OOM.
- [ ] Cancellation is clean: aborting a query / killing a compute actor cancels
      in-flight requests and leaks no actors, promises or connections.
- [ ] Retry semantics are explicit and documented (what is retried, idempotency
      expectations, at-least-once vs at-most-once for side effects).
- [ ] Concurrency limits enforced: per-query, per-host, per-node.

### 2.3 Security

- [ ] Egress is controlled by an allow/deny host list (default: deny-all or
      explicit allowlist, decided with security).
- [ ] SSRF protection: block RFC1918 / loopback / link-local (incl. cloud metadata
      `169.254.169.254`), validate resolved IPs (DNS-rebinding), reject non-http(s)
      schemes. Enforced **before** the first request, not as a later phase.
- [ ] Secrets/authorization tokens are sourced from the secrets subsystem
      (`SecureParams` / TokenAccessor), never from plaintext SQL, and are redacted
      from logs.
- [ ] TLS verification on by default; opting out is explicit and audited.

### 2.4 Observability & ops

- [ ] Per-query plan stats: request count, bytes in/out, latency, error count.
- [ ] Service-level monitoring counters (active requests/connections, timeouts,
      errors, denied hosts).
- [ ] Feature is behind a config flag, off by default, with clear enable path.

### 2.5 Quality gates

- [ ] Unit tests for each layer (mock HTTP), actor tests via `TTestActorRuntime`,
      and Python E2E tests against a real test HTTP server (incl. timeout,
      connection-refused, 5xx, oversized response).
- [ ] Docs: user guide (SQL surface, limits, security) + operator guide (config).

## 3. User-facing surface (what the end user gets)

The concrete syntax depends on the chosen option (§4/§5), but the **user
experience** goal is fixed:

### 3.1 Enrichment / external function (per-row or per-batch)

`Http::Call` accepts, in addition to the endpoint reference, an **arbitrary
user-supplied request body** and **arbitrary user-supplied headers**, both of
which may be built from row data and therefore vary per row.

**Signature.**

```
Http::Call(
    endpoint    : String,          -- name of the EXTERNAL DATA SOURCE (URL + auth + TLS)
    body        : String?,         -- arbitrary request body (any bytes); NULL/omitted => no body
    headers     : Dict<String,String>? | Struct?,  -- arbitrary user headers
    options     : Struct?          -- optional: Method, Path, QueryParams, Timeout
) -> Struct<
    StatusCode : Uint32,
    Headers    : Dict<String,String>,
    Body       : String
>
```

**Basic call with a JSON body and one custom header:**

```sql
SELECT
    t.id,
    t.name,
    Http::Call(
        'my_scoring_endpoint',              -- external data source object (holds URL + auth)
        FormatJson(AsStruct(t.f1 AS f1, t.f2 AS f2)),   -- arbitrary body from row data
        AsStruct('application/json' AS `Content-Type`)  -- custom header
    ) AS response
FROM my_table AS t;
```

**Arbitrary per-row headers and body.** Headers may be passed either as a
`Struct` (static, human-readable literal keys) or as a `Dict<String,String>`
(fully dynamic, computed from row data). Both the body bytes and the header
values can depend on the row:

```sql
SELECT
    t.id,
    Http::Call(
        'my_scoring_endpoint',
        t.raw_payload,                         -- arbitrary body straight from a column
        AsDict(
            AsTuple('Content-Type', 'application/json'),
            AsTuple('X-Request-Id', CAST(t.id AS String)),   -- per-row header value
            AsTuple('X-Tenant',     t.tenant),                -- dynamic key/value from data
            AsTuple('Accept-Language', COALESCE(t.lang, 'en'))
        ),
        AsStruct(
            'POST'          AS Method,
            '/v2/predict'   AS Path,           -- appended to the data source LOCATION
            AsDict(AsTuple('model', t.model_name)) AS QueryParams,
            Interval("PT5S") AS Timeout
        )
    ) AS response
FROM my_table AS t;
```

Semantics of user-supplied headers and body:

- **Body** is opaque bytes. The caller is responsible for its encoding; typically
  paired with a `Content-Type` header. `NULL` or an omitted argument means "no
  body" (e.g. for `GET`).
- **Headers** provided by the user are merged onto the request. Multiple headers
  with the same name are supported via a `List<Tuple<String,String>>` form when
  duplicates are required.
- **Precedence & reserved headers.** Headers injected by the platform for the
  configured `AUTH_METHOD` of the data source (e.g. `Authorization`) and
  transport-managed headers (`Host`, `Content-Length`, `Connection`,
  `Transfer-Encoding`) are **reserved**: user values for them are ignored (or, by
  config, rejected with an error) so that a query cannot override or exfiltrate
  the endpoint's credentials. All other header names are passed through verbatim.
- **Validation.** Header names/values are validated (no CR/LF — prevents header
  injection/response splitting); total header bytes and body bytes are bounded by
  the same size limits enforced in the egress layer (§2.2/§5 foundation).
- **Secrets in headers.** To place a secret into a *custom* header (not covered by
  a built-in `AUTH_METHOD`), the value is referenced from the secrets subsystem,
  never inlined:

  ```sql
  AsDict(AsTuple('X-Api-Key', Secret('scoring_api_key')))
  ```

  `Secret(...)` resolves via `SecureParams`; the literal never appears in the
  query text, logs or plan.

- The endpoint, base URL, TLS settings and **auth secret** live in an
  `EXTERNAL DATA SOURCE` object created by an admin, not inline in the query.
- The query author references it by name; they never see or paste secrets.

### 3.2 HTTP-backed table (read a dataset over HTTP)

```sql
SELECT id, metadata
FROM my_external_source.`/entities?since=2024-01-01`
WITH (FORMAT = "json", SCHEMA = (id Int64, metadata String));
```

### 3.3 Administration

```sql
CREATE EXTERNAL DATA SOURCE my_scoring_endpoint WITH (
    SOURCE_TYPE = "Http",
    LOCATION    = "https://api.internal.example.com",
    AUTH_METHOD = "TOKEN",
    TOKEN_SECRET_NAME = "scoring_token"
);
```

Operator-level knobs (allow/deny hosts, global concurrency, size limits, default
timeout) are set in cluster config and are **not** query-author controllable.

### 3.4 Where the functionality can be used in a query

`Http::Call` is a scalar expression that returns a struct, so syntactically it can
appear anywhere a scalar expression is allowed. Semantically, however, it is an
**async side-effecting call**, which constrains *how* it is planned in each clause.
The table summarizes support; details follow.

| SQL clause | Supported | How it is planned | Notes / caveats |
|------------|-----------|-------------------|-----------------|
| **Projection** (`SELECT`) | Yes — primary case | HTTP transform / lookup applied to the stream, output column added | The natural, recommended place. One call per row (or per batch). |
| **Filter** (`WHERE`/`HAVING`) | Yes, with care | Call materialized in a preceding stage, then the predicate reads its result column | The call is **not** re-evaluated per predicate; the optimizer must pull it out so it runs once per row and short-circuits are disabled around it. |
| **JOIN** (`... ON`, HTTP-backed side) | Yes — via Option B | Streaming lookup join: build side is the HTTP endpoint, probe keys are batched | Best fit for key→value enrichment; response cache dedupes repeated keys. Not allowed in arbitrary non-equi `ON` predicates that would fan out unboundedly. |
| **Aggregation** (`GROUP BY`, agg args) | Yes, but restricted | Call runs in a **pre-aggregation** projection stage; its result then feeds the aggregate | Cannot be called *inside* a running aggregate combiner (e.g. per merge step). Calling in `GROUP BY` keys is discouraged (one call per input row before grouping). |
| **Window functions** (`OVER (...)`) | Partially | Call runs in a projection **before/after** the window operator; not inside frame evaluation | Must not be invoked per-frame or per-peer-group iteration (would explode call count and be non-deterministic across frame re-scans). |

Common rules across all clauses:

- **Evaluated exactly where planned, once per row/batch.** The optimizer isolates
  `Http::Call` into a dedicated stage so it is never re-executed by an enclosing
  operator that may re-scan its input (aggregation combiners, window frames,
  join inner loops). This guarantees a predictable number of external calls.
- **No short-circuiting relies on it.** Because the call is materialized upfront,
  constructs like `col IS NULL OR Http::Call(...)` do **not** skip the call; if
  conditional invocation is required, guard it explicitly (see below).
- **Conditional invocation.** To avoid calling for rows that don't need it, filter
  first in an earlier stage, or pass a sentinel that the transform treats as
  "skip" (returns a NULL response without egress). Example: `WHERE score > 0.5`
  placed so it runs before the `Http::Call` projection.
- **Determinism.** `Http::Call` is non-deterministic (external state, retries).
  It is treated as a barrier for optimizations that assume purity (common
  subexpression elimination will **not** collapse two textually identical calls
  unless explicitly marked cacheable via the lookup cache in Option B).
- **Cost/limits.** Per-query `MaxHttpRequestsPerQuery` counts calls across all
  clauses; a query using the call in projection *and* filter *and* join pays for
  all of them.

Usage examples per clause:

```sql
-- Projection: enrich each row
SELECT id, Http::Call('ep', FormatJson(AsStruct(x AS x))) AS resp FROM t;

-- Filter: call once per row, then filter on its result
SELECT * FROM (
    SELECT t.*, Http::Call('ep', t.payload) AS resp FROM t
) WHERE CAST(Yson::LookupString(resp.Body, 'allow') AS Bool);

-- Join: HTTP-backed lookup (Option B)
SELECT t.id, e.Body
FROM t LEFT JOIN my_scoring_endpoint AS e ON e.key = t.entity_id;

-- Aggregation: call before grouping, aggregate the result
SELECT region, COUNT(*)
FROM (SELECT region, Http::Call('ep', payload) AS resp FROM t)
WHERE resp.StatusCode = 200
GROUP BY region;

-- Window: call in projection, then window over the materialized column
SELECT id,
       AVG(CAST(resp.Body AS Double)) OVER (PARTITION BY region) AS avg_score
FROM (SELECT id, region, Http::Call('ep', payload) AS resp FROM t);
```

## 4. Solution options — summary

| # | Option | Integration point | Best fit | Reuses existing | Idempotency / retry story | Effort | Recommendation |
|---|--------|--------------------|----------|-----------------|---------------------------|--------|----------------|
| A | **Input/Output Transform** over DQ async I/O, extending the `ExternalFunction` provider | `CreateDqInputTransform` / `CreateDqOutputTransform` ([async_io.h:367](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:367)) | Per-batch enrichment / external function on a stream | High: DQ transforms + `ydb/library/actors/http` + function provider | Transform re-runs on stage restart → needs dedup/at-least-once note | Medium | **Recommended primary** |
| B | **Async Lookup Source** | `CreateDqLookupSource` / [`IDqAsyncLookupSource`](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:236) | Key-based per-row enrichment / "HTTP JOIN" with batching & caching | High: lookup infra already batches keys, has fullscan limit | Read-mostly (GET) → naturally retry-safe | Medium | **Recommended for enrichment/JOIN** |
| C | **External Source / Sink** (HTTP data source) | `CreateDqSource` / `CreateDqSink` ([async_io.h:352](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:352)) | Reading/writing whole datasets over HTTP as a table | High: mirrors S3/PQ providers | Source reads retry-safe; sink needs care | Medium-High | **Recommended for table-style access** |
| D | **HTTP client inside MiniKQL compute node** (original proposal) | `IHttpClient` in `TComputationContext` | — | Low: new HTTP stack, forks provider | Broken: no await primitive in MiniKQL | High | **Rejected** |

Options A, B and C are **complementary**, not mutually exclusive: they target
different user scenarios and can share the same lower layers (HTTP egress actor,
security, secrets, counters). D is the baseline we are replacing.

The pragmatic plan is: build the **shared HTTP egress layer** once, then expose it
through B (lookup enrichment) and/or A (transform) first, and add C (table
source/sink) when table-style HTTP access is required.

## 5. Solution options — detailed architecture

### Shared foundation (used by A, B, C)

Regardless of the chosen surface, all viable options share the same bottom layers:

```
        DQ async I/O actor (Transform / LookupSource / Source)
                          │  (non-blocking, backpressured)
                          ▼
        THttpEgressActor  (thin wrapper over ydb/library/actors/http)
          - per-node singleton, owns the actor HTTP proxy
          - connection reuse / keep-alive (provided by the http lib)
          - global concurrency limit, per-host rate limit
          - allow/deny list + SSRF + DNS-rebinding checks
          - TLS config, proxy config
          - service-level monitoring counters
                          │
                          ▼
                 ydb/library/actors/http  (existing)
                          │
                          ▼
                   External HTTP servers
```

Cross-cutting pieces reused by every option:

- **Egress actor** over [`ydb/library/actors/http`](ydb/library/actors/http/http.h)
  — no new TLS/connection-pool code.
- **Secrets** via `SecureParams` (already threaded into
  [`TSourceArguments`](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:262)
  and `TInputTransformArguments`).
- **Memory** via `IMemoryQuotaManager`
  ([async_io.h:39](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:39)).
- **Errors** via `TEvAsyncInputError`
  ([async_io.h:76](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:76)).
- **Security** (allow/deny + SSRF) enforced inside the egress actor.

---

### Option A — Input/Output Transform (extend `ExternalFunction`)

**Idea.** Model an HTTP call as a **DQ input transform**: a stream of input rows
goes in, an HTTP call is made per row or per batch, and a stream of enriched rows
comes out. This maps directly onto the existing transform mechanism and onto the
existing `ExternalFunction`/`TDqTransform` concept.

**Integration point.**
[`IDqAsyncIoFactory::CreateDqInputTransform`](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:367)
returning a `{IDqComputeActorAsyncInput*, IActor*}` pair, fed by
[`TInputTransformArguments`](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:310)
(has `TransformInput`, `SecureParams`, `TaskParams`, `ComputeActorId`, type env).

**Architecture.**

```
Stage program (MiniKQL)  ──►  input rows (TransformInput)
                                    │
                          ┌─────────▼───────────────────────────┐
                          │  THttpTransformActor                 │
                          │  (IDqComputeActorAsyncInput)         │
                          │   - pulls input rows                 │
                          │   - builds requests (batch of N)     │
                          │   - sends to THttpEgressActor        │
                          │   - buffers responses within freeSpace│
                          │   - GetAsyncInputData() emits enriched│
                          │     rows; TEvNewAsyncInputDataArrived │
                          └─────────┬───────────────────────────┘
                                    ▼
                            THttpEgressActor ─► actors/http ─► endpoint
```

**Compilation path.** Extend the existing function provider
([`dq_function_physical_optimize.cpp`](ydb/library/yql/providers/function/provider/dq_function_physical_optimize.cpp:1),
[`dq_function_dq_integration.cpp`](ydb/library/yql/providers/function/provider/dq_function_dq_integration.cpp:24))
so `ExternalFunction('http', ...)` / `Http::Call(...)` lowers to a `TDqTransform`.
The transform settings carry only the **static** part of the request (endpoint
reference, default method/path, timeout, batch size, size limits). The **dynamic,
per-row** parts — arbitrary body and arbitrary headers (§3.1) — are *not* baked
into the transform settings: they are computed by the MiniKQL graph and arrive as
regular columns in `TransformInput`. Concretely, the row fed into the transform is
a struct like `<body: String, headers: Dict<String,String>, path: String,
query: Dict<String,String>>`; the transform actor reads these fields per row,
merges platform-reserved auth/transport headers, validates them, and issues the
request. This keeps secret/header injection and validation on the trusted actor
side while letting header values and body depend on data. No new MiniKQL node
type; the transform is applied by the compute actor, outside the MiniKQL graph.

**Pros**
- Uses the DQ mechanism actually designed for async external I/O.
- Natural batching (`freeSpace`-driven) and backpressure for free.
- Extends existing `ExternalFunction` provider instead of forking it.
- Memory quota, stats, checkpoint hooks already present on the interface.

**Cons / risks**
- Transform re-runs if the stage restarts ⇒ **at-least-once** side effects.
  Must document this and steer non-idempotent (POST) usage toward explicit
  opt-in / idempotency keys.
- Ordering: if output must preserve input order, the actor must reorder
  out-of-order responses (extra buffering).

**Best for.** Per-batch enrichment and HTTP "external function" calls on a stream.

---

### Option B — Async Lookup Source

**Idea.** Reuse the **async lookup** machinery (built for key→value enrichment,
e.g. streaming lookup joins) to implement "for each key, GET/POST an HTTP
endpoint and return the response as the payload".

**Integration point.**
[`IDqAsyncLookupSource`](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:236):
`GetMaxSupportedKeysInRequest()` + `AsyncLookup(request)` → `TEvLookupResult`,
created via `CreateDqLookupSource`
([async_io.h:357](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:357)).

**Architecture.**

```
Streaming Lookup Join / enrichment operator
        │  batch of keys (≤ GetMaxSupportedKeysInRequest)
        ▼
  THttpLookupSource (IDqAsyncLookupSource)
    - maps each key → HTTP request (template)
    - fan-out to THttpEgressActor (bounded concurrency)
    - assembles TUnboxedValueMap key→payload
    - optional response cache (dedupe identical keys)
    - TEvLookupResult back to the join operator
        ▼
  THttpEgressActor ─► actors/http ─► endpoint
```

**Pros**
- The lookup layer already models **batched keys**, a max-keys-per-request
  bound, and result assembly — exactly the shape of per-row enrichment.
- Lookups are read-oriented (GET) ⇒ retry-safe, sidestepping the idempotency
  problem of Option A.
- Fits "HTTP JOIN": `FROM t LEFT JOIN http_source ON t.k = http_source.key`.
- Natural place for a response cache keyed by request.

**Cons / risks**
- Constrained to a key→value shape; not a fit for "stream body once, get a
  paginated dataset".
- Requires the streaming-lookup-join plan path; more moving parts in the
  optimizer than a plain transform.

**Best for.** Per-row/key enrichment and HTTP-backed lookup joins.

---

### Option C — External Source / Sink (HTTP data source)

**Idea.** Treat an HTTP endpoint as an external **table**: a source reads pages
of data from it; a sink POSTs rows to it. This mirrors how the S3 / PQ / generic
providers are structured.

**Integration point.**
`CreateDqSource` / `CreateDqSink`
([async_io.h:352](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:352),
[async_io.h:362](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:362)),
driven by an `EXTERNAL DATA SOURCE` of type `Http`, plus a new provider that plans
partitioned/paginated reads (`ReadRanges` in
[`TSourceArguments`](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:256)).

**Architecture.**

```
Query planner → HTTP provider → partitions (page ranges / URL shards)
        │
        ▼
  THttpSourceActor (IDqComputeActorAsyncInput, one per partition)
    - fetches page, parses (JSON/CSV) into rows
    - follows pagination (next-cursor / Link header) up to a limit
    - honors freeSpace backpressure; accounts bytes to memory quota
    - emits rows via GetAsyncInputData()
        ▼
  THttpEgressActor ─► actors/http ─► endpoint
```

**Pros**
- Clean "HTTP as a table" model; composes with the rest of SQL (filters,
  joins, aggregation) like any external source.
- Parallelizable across partitions/pages.
- Reuses the well-trodden source/sink provider pattern (S3-like).

**Cons / risks**
- Most work of the three: needs a full provider (type annotation, physical
  planning, partitioning, pagination protocol handling).
- Sink side (writing rows out via HTTP) re-raises idempotency/at-least-once
  concerns like Option A.
- Pagination protocols are endpoint-specific; needs a pluggable strategy.

**Best for.** Reading (and optionally writing) whole datasets exposed over HTTP.

---

### Option D — HTTP client inside a MiniKQL compute node (rejected baseline)

**Idea (original proposal).** Add `IHttpClient` / `IHttpClientFactory` to
[`TComputationContext`](yql/essentials/minikql/computation/mkql_computation_node.h:116)
next to `SpillerFactory`, and have a MiniKQL node call
`HttpClientFactory->CreateClient()->Send(request)`, "yielding" until the future
resolves.

**Why it is rejected.**

1. **No await primitive.** MiniKQL nodes cannot suspend on a future. The runtime
   contract is `GetValue()` /
   [`FetchValues() → EFetchResult`](yql/essentials/minikql/computation/mkql_computation_node.h:245);
   `Yield` is "no data now / re-run", not "resume me later". The proposed
   `bool Consume()` "return true to wait" method does not exist and cannot be
   emulated without changing the execution model.
2. **Wrong layer for side effects.** Mixing network egress into pure MiniKQL
   evaluation breaks determinism, cancellation, memory accounting and stage
   restart handling.
3. **Reinvents infrastructure.** It specifies a new connection pool / TLS stack,
   duplicating [`ydb/library/actors/http`](ydb/library/actors/http/http.h), and a
   parallel HTTP function path that ignores the existing
   [`ExternalFunction` provider](ydb/library/yql/providers/function/provider/dq_function_dq_integration.cpp:1).
4. **Lifetime hazards.** Raw owning pointers across async completions
   (`IDqHttpActor*`, `TConnectionPool*`) are use-after-free/leaks waiting to
   happen in the actor model.

Kept here only to document why the approach is not pursued.

## 6. Comparison against the spilling analogy

The original document justified Option D by analogy to spilling
([`ISpiller`](yql/essentials/minikql/computation/mkql_spiller.h:8),
[`ISpillerFactory`](yql/essentials/minikql/computation/mkql_spiller_factory.h:11)).
The analogy is misleading: spilling is **local, synchronous-from-the-graph's-view
disk I/O** with a `TFuture` that the runtime is architected to await at specific
spill points, and it has explicit memory reporting (`ReportAlloc`/`ReportFree`).
HTTP is **remote, high-latency, failure-prone, security-sensitive** I/O — exactly
the profile the DQ async I/O layer (sources/transforms/lookups) was built for.
Hence Options A/B/C follow the async-I/O pattern, not the spiller pattern.

## 7. Open questions (to resolve before picking an option)

1. **Primary user scenario first?** Enrichment (→ B) vs external function on a
   stream (→ A) vs table reads (→ C). This decides sequencing.
2. **Idempotency policy.** Do we allow non-idempotent (POST with side effects)
   calls at all in v1, or restrict v1 to GET/read-only + at-least-once with
   idempotency keys?
3. **Egress security default.** Allowlist-only vs deny-list, and who administers
   it (cluster operator vs tenant admin).
4. **Auth methods for v1.** Static token from secrets only, or also
   service-account/IAM token exchange.
5. **Response formats for v1.** JSON only, or also raw bytes / CSV.
6. **Checkpointing.** For streaming queries, what does `SaveState`/`LoadState`
   store for in-flight HTTP calls (nothing / replay-on-restore)?

## 8. Detailed implementation plan (recommended path)

**Recommended path.** Build the shared HTTP egress layer once, then ship the
user-facing feature through **Option B (Async Lookup Source)** as the primary
surface (per-row/key enrichment and HTTP JOIN — read-oriented, retry-safe), with
**Option A (Input Transform)** added on the same egress layer for the
external-function-on-a-stream case. **Option C** is deferred.

Rationale for B-first: the lookup layer already models batched keys, a
max-keys-per-request bound and result assembly
([`IDqAsyncLookupSource`](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:236)),
and GET-style lookups sidestep the idempotency problem. It also has a direct
precedent to copy: the generic provider registers a lookup source via
`factory.RegisterLookupSource<...>(name, lookupActorFactory)`
([`yql_generic_provider_factories.cpp:65`](ydb/library/yql/providers/generic/actors/yql_generic_provider_factories.cpp:65)),
and the stream-lookup input transform
([`dq_input_transform_lookup.cpp`](ydb/library/yql/dq/actors/input_transforms/dq_input_transform_lookup.cpp:1176))
is exactly the operator that will drive our HTTP lookup source.

### Guiding conventions

- **Mock-first:** every layer gets an interface + a mock so it is testable without
  a network. Real actors come after the mock-backed tests are green.
- **Tests include build**, no `-j`, no force rebuild; run with
  `./ya make --build relwithdebinfo -tA <folder> 2>&1 | tail`.
- **C++20 or earlier.**
- Each phase ends with a green test target — nothing merges without tests.

### Phase 0 — Design lock-in & scaffolding

**Goal:** resolve blocking decisions and create empty build units.

**Tasks**
1. Resolve Open questions §7 #2 (idempotency: v1 = GET/read-only lookups +
   at-least-once), #3 (egress security default: allowlist-only), #4 (auth: static
   token from secrets in v1), #5 (formats: JSON + raw bytes).
2. Create directory + `ya.make` for `ydb/library/yql/dq/actors/http/` (egress),
   and a proto package for settings.
3. Add a feature flag (off by default) in KQP config.

**Deliverable:** empty libraries compile; flag exists.
**Tests:** build-only.

---

### Phase 1 — Shared HTTP egress actor (foundation for A/B/C)

**Goal:** a per-node singleton actor that performs non-blocking HTTP with
security, limits, secrets and counters — the single choke point for all egress.

**Proposed files**
- `ydb/library/yql/dq/actors/http/http_egress_actor.{h,cpp}`
- `ydb/library/yql/dq/actors/http/http_egress_security.{h,cpp}` (allow/deny, SSRF)
- `ydb/library/yql/dq/actors/http/http_egress_counters.h`
- `ydb/library/yql/dq/actors/http/events.h`

**Tasks**
1. Define events: `TEvHttpRequest{ id, method, url, headers, body, timeout }` →
   `TEvHttpResponse{ id, status, headers, body }` / `TEvHttpError{ id, issues }`.
2. Implement the actor over
   [`ydb/library/actors/http`](ydb/library/actors/http/http.h), building requests
   with [`THttpOutgoingRequest::CreateRequest`](ydb/library/actors/http/http.h:1076)
   and owning one HTTP proxy (keep-alive/TLS handled by the lib — **no custom
   connection pool**).
3. Enforce **before egress**: allow/deny host list; SSRF checks (block RFC1918,
   loopback, link-local, `169.254.169.254`); reject non-`http(s)` schemes;
   validate header names/values (no CR/LF); enforce request/response size limits.
4. Concurrency control: global in-flight cap + per-host rate limit; queue with
   backpressure signaling (reject/queue when saturated).
5. Merge reserved auth/transport headers on the trusted side; redact `Authorization`
   and secret headers from logs.
6. Wire monitoring counters (active requests/connections, timeouts, errors,
   denied hosts, bytes in/out).
7. DNS-rebinding guard: resolve, validate resolved IP against policy, pin for the
   connection.

**Tests** (`http_egress_actor_ut.cpp`, `TTestActorRuntime` + local test HTTP server)
- Happy path GET/POST; large response truncated at limit; timeout → `TEvHttpError`.
- SSRF: loopback/RFC1918/metadata blocked; scheme rejection; CR/LF header rejected.
- Concurrency cap respected; per-host rate limit; denied-host counter increments.
- Cancellation: destroying the requesting actor cancels in-flight, no leaks
  (verified with LSAN).

**DoD mapping:** §2.1 (non-blocking), §2.2 (limits/backpressure/cancel), §2.3
(SSRF/allow-deny/redaction), §2.4 (service counters).

---

### Phase 2 — HTTP lookup source (Option B core), mock-first

**Goal:** an `IDqAsyncLookupSource` that turns a batch of keys into HTTP calls via
the Phase-1 egress actor and returns key→response.

**Proposed files**
- `ydb/library/yql/dq/actors/http/http_lookup_source.{h,cpp}`
- proto: `TDqHttpLookupSourceSettings` (endpoint ref, method, path/body templates,
  header spec, timeout, batch size, cache policy, size limits).

**Tasks**
1. Implement
   [`IDqAsyncLookupSource`](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:236):
   `GetMaxSupportedKeysInRequest()` (from settings/limits) and
   `AsyncLookup(request)` that fans out to the egress actor, assembles
   `TUnboxedValueMap` key→payload, then emits `TEvLookupResult`.
2. Account response bytes to
   [`IMemoryQuotaManager`](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:39)
   from `TLookupSourceArguments`.
3. Resolve secrets from `SecureParams`; build reserved auth header here.
4. Optional response cache keyed by canonical request (dedupe identical keys in a
   batch and across batches within limits).
5. Error mapping to `TEvAsyncInputError`
   ([async_io.h:76](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:76)).
6. Register via `factory.RegisterLookupSource<TDqHttpLookupSourceSettings>("Http", ...)`
   mirroring
   [`yql_generic_provider_factories.cpp:65`](ydb/library/yql/providers/generic/actors/yql_generic_provider_factories.cpp:65).

**Tests** (`http_lookup_source_ut.cpp`, egress mocked)
- Batch of N keys → N requests; dedupe hits cache; max-keys bound honored.
- Memory accounting increments/decrements; oversized response rejected.
- Error from egress surfaces as `TEvAsyncInputError`.

**DoD mapping:** §2.1 (per-row enrichment), §2.2 (memory/limits), §2.3 (secrets).

---

### Phase 3 — YQL/DQ wiring for lookup (Option B compilation)

**Goal:** the streaming lookup-join operator can target the HTTP lookup source,
driven by an `EXTERNAL DATA SOURCE` of type `Http`.

**Tasks**
1. Add `SOURCE_TYPE = "Http"` external data source (LOCATION, AUTH_METHOD,
   TOKEN_SECRET_NAME) — schema object, metadata loader, validation.
2. Teach the optimizer to route a lookup join whose right side is an `Http` data
   source to the stream-lookup input transform
   ([`dq_input_transform_lookup.cpp`](ydb/library/yql/dq/actors/input_transforms/dq_input_transform_lookup.cpp:1176)),
   with `TDqInputTransformLookupSettings` referencing our `"Http"` lookup source.
3. Type annotation: validate key type, output struct
   `Struct<StatusCode,Headers,Body>`, header/body specs, timeout.
4. Thread `SecureParams` (secret name → value) through compilation to runtime.

**Tests**
- Compile `FROM t LEFT JOIN http_src AS e ON e.key = t.id`; assert plan contains
  the HTTP stream-lookup transform and the `Http` lookup source settings.
- Negative: HTTP source used in an unbounded non-equi predicate is rejected (§3.4).

**DoD mapping:** §3.1/§3.4 surface, §2.5 (compile tests).

---

### Phase 4 — `Http::Call` scalar surface + Input Transform (Option A)

**Goal:** the `Http::Call(...)` expression and the stream input-transform path for
external-function-on-a-stream, reusing Phase-1 egress.

**Proposed files**
- `ydb/library/yql/dq/actors/http/http_input_transform.{h,cpp}`
- extend function provider
  ([`dq_function_physical_optimize.cpp`](ydb/library/yql/providers/function/provider/dq_function_physical_optimize.cpp:1),
  [`dq_function_dq_integration.cpp`](ydb/library/yql/providers/function/provider/dq_function_dq_integration.cpp:24)).

**Tasks**
1. Register the `Http::Call` scalar (module function) with the signature from §3.1
   (`endpoint, body?, headers?, options?` → `Struct<StatusCode,Headers,Body>`).
2. Lower `Http::Call` to a `TDqTransform`: **static** parts (endpoint, default
   method/path, timeout, batch size, limits) go into transform settings; **dynamic**
   per-row body/headers/path/query are emitted by the MiniKQL graph as input
   columns (struct `<body, headers, path, query>`) — per §5 Option A.
3. Implement the transform actor as `IDqComputeActorAsyncInput` (model after
   [`dq_input_transform_lookup.cpp`](ydb/library/yql/dq/actors/input_transforms/dq_input_transform_lookup.cpp:26)):
   pull rows, batch, call egress, buffer within `freeSpace`, emit enriched rows,
   raise `TEvNewAsyncInputDataArrived`. Preserve input order when required.
4. Register via `factory.RegisterInputTransform<TDqHttpTransformSettings>("HttpCall", ...)`
   (see the registration precedent in
   [`dq_input_transform_lookup_factory.cpp:9`](ydb/library/yql/dq/actors/input_transforms/dq_input_transform_lookup_factory.cpp:9)).
5. Enforce §3.4 planning rules: isolate `Http::Call` into its own stage so
   aggregation combiners / window frames / join inner loops never re-invoke it;
   mark it non-deterministic (barrier for CSE/short-circuit).

**Tests**
- Projection: `SELECT Http::Call(...)` compiles to a transform stage; runtime
  (egress mocked) returns the response struct; dynamic per-row headers/body honored.
- Filter/aggregation/window: assert the call is materialized once per row in a
  preceding stage (call count == row count), per §3.4.

**DoD mapping:** §3.1 (headers/body), §3.4 (clause placement), §2.5.

---

### Phase 5 — End-to-end integration & runtime enablement

**Goal:** full compile→execute on a multi-node cluster behind the feature flag.

**Tasks**
1. Register the `Http` async-io factories into the KQP compute actor's
   `TDqAsyncIoFactory` assembly (source/lookup/transform), gated by the flag.
2. Start the egress singleton on node startup when the flag is on.
3. Query plan stats: `HttpRequestsCount`, `HttpRequestBytes`, `HttpResponseBytes`,
   `HttpRequestLatencyUs`, `HttpErrorsCount` (§2.4).
4. Enforce `MaxHttpRequestsPerQuery` across all clauses (§3.4).

**Tests** (`TTestActorRuntime` full-stack, egress → local test server)
- E2E enrichment (JOIN) and E2E `Http::Call` projection return correct data.
- Query fails cleanly with feature flag off.
- Plan stats populated; per-query request cap enforced.

**DoD mapping:** §2.1, §2.4, §2.5.

---

### Phase 6 — Hardening, security review, Python E2E

**Goal:** production readiness.

**Tasks**
1. Retry policy for idempotent (GET) calls: bounded retries + backoff on 5xx/network;
   never retry non-idempotent unless an idempotency key is supplied.
2. Cancellation/restart semantics finalized and documented (at-least-once for A).
3. Security review sign-off on allow/deny defaults, SSRF, secret redaction, TLS.
4. Python E2E against a real HTTP server (echo/slow/error endpoints): success,
   timeout, connection-refused, 5xx, oversized response. Add to CI.
5. Stress: saturate in-flight calls; verify no executor-thread blocking, bounded
   memory, no leaks.
6. Docs: user guide (SQL surface, §3.1/§3.4, limits, security) + operator guide.

**DoD mapping:** §2.2 (retry/cancel), §2.3 (security sign-off), §2.4, §2.5.

---

### Sequencing & milestones

| Milestone | Phases | User-visible outcome |
|-----------|--------|----------------------|
| M1: Egress foundation | 0–1 | Internal, non-blocking, secure HTTP egress actor |
| M2: Enrichment (B) | 2–3 | HTTP JOIN / key enrichment works behind flag |
| M3: `Http::Call` (A) | 4 | Scalar `Http::Call` with dynamic headers/body |
| M4: GA-ready | 5–6 | Flag-gated feature, stats, docs, security sign-off |

Option C (HTTP-as-a-table source/sink) is a follow-up epic on the same egress
layer and is intentionally out of this plan's scope.
