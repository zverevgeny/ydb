# QYT Topic Client — Code Review

## 1. Overview

The QYT Topic Client is a new module that allows YDB's Platform Queue (PQ) infrastructure to communicate with YTsaurus queues instead of native YDB topics. It implements the [`ITopicClient`](ydb/library/yql/providers/pq/gateway/abstract/yql_pq_topic_client.h:12) interface, emulating the YDB topic push (event-driven) model on top of the YTsaurus queue pull API.

The gateway layer ([`TYtPqGateway`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_yt_gateway.cpp:34)) remains unchanged and uses the QYT Topic Client as its underlying implementation.

### Files

| File | Purpose |
|------|---------|
| [`yql_qyt_topic_client.h`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.h:1) | Topic client settings and factory function |
| [`yql_qyt_topic_client.cpp`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:1) | Read/write session implementations over YT queue |
| [`yql_qyt_blocking_queue.h`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_blocking_queue.h:1) | Bounded thread-safe queue with backpressure |
| [`ut/yql_qyt_blocking_queue_ut.cpp`](ydb/library/yql/providers/pq/gateway/clients/qyt/ut/yql_qyt_blocking_queue_ut.cpp:1) | Unit tests for the blocking queue |
| [`ya.make`](ydb/library/yql/providers/pq/gateway/clients/qyt/ya.make:1) | Build file |

---

## 2. Architecture

### Topic Client Layer ([`TQytTopicClient`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:475))

- Wraps YT queue operations into the YDB topic client interface
- **Read**: Creates [`TQytTopicReadSession`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:93) that runs a background poller thread calling `pull_queue_consumer`
- **Write**: Creates [`TQytTopicWriteSession`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:290) that runs a background writer thread using a YT producer session
- **CommitOffset**: Uses `advance_queue_consumer` in a tablet transaction

### Blocking Queue ([`TBlockingEQueue`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_blocking_queue.h:15))

- Template bounded queue with byte-size-based backpressure
- Producer blocks when accumulated size reaches `MaxSize`
- Uses two condition variables (`CanPush`, `CanPop`) for efficient signaling
- `Stop()` unblocks all waiters

### Read Session ([`TQytTopicReadSession`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:93))

```
YT Queue (pull)  →  PollLoop thread  →  TBlockingEQueue  →  WaitEvent()/GetEvent() (SDK push API)
```

- A dedicated thread continuously polls `pull_queue_consumer` starting from the current consumer offset
- Each row is converted to a [`TReadSessionEvent::TDataReceivedEvent`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:258)
- When no rows are available, sleeps for `PollPeriod` (default 50ms)
- Offset is tracked in an `atomic<i64>` and updated after each successful pull

### Write Session ([`TQytTopicWriteSession`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:290))

```
Write()  →  TBlockingEQueue (messages)  →  WriteLoop thread  →  ProducerSession->Write() + Flush()
```

- Incoming messages are pushed into a bounded queue (4 MB limit)
- A writer thread drains the queue, converts messages to unversioned rows, and writes via the producer session
- After each flush, emits `TAcksEvent` followed by `TReadyToAcceptEvent` with a new continuation token
- Sequence numbers are auto-incremented starting from the producer's last sequence number

---

## 3. Problems and Risks

### 3.1 Thread Safety Issues

#### CRITICAL: [`Offset`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:275) is `atomic<i64>` but used unsafely

The [`Offset`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:275) field in [`TQytTopicReadSession`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:93) is declared as `std::atomic<i64>` but:
1. It's read in [`PollLoop()`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:217) (line 225) and written at line 256 — both from the same poller thread, so atomicity is unnecessary there
2. However, if `Offset` is ever read from another thread (e.g., for diagnostics), there's no synchronization mechanism beyond atomic load/store
3. More importantly, the offset is passed by value to `PullQueueConsumer` at creation time. If the session is closed and recreated, the offset could be stale

**Risk**: Data loss if the poller crashes between consuming rows and updating `Offset` — the next pull will re-deliver already processed rows (duplicates, not loss). This is actually acceptable for at-least-once semantics but should be documented.

#### HIGH: Race in gateway's [`GetOrCreateClient`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_yt_gateway.cpp:178)

The method holds the mutex while creating the client. Client creation involves network I/O (creating RPC connection). This means all threads waiting for any cluster operation will block during client creation.

**Risk**: Under load, if multiple sessions open simultaneously for different clusters, they serialize on client creation, causing latency spikes.

### 3.2 Resource Management

#### CRITICAL: No graceful shutdown of poller/writer threads

In [`TQytTopicReadSession::Cleanup()`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:262):
```cpp
EventsQ.Stop();
Pool.Stop();
if (Poller.joinable()) Poller.join();
```

The `Pool.Stop()` stops the thread pool, but [`WaitEvent()`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:132) submits work to this pool via `NThreading::Async`. If a `WaitEvent` future is pending when `Pool.Stop()` is called, the async callback may never execute, causing the future to never resolve.

**Risk**: Calling code waiting on `WaitEvent()` futures will hang indefinitely if the session is closed while futures are pending.

#### HIGH: [`TThreadPool`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:280) with single thread

Both read and write sessions create a single-threaded pool. The [`WaitEvent()`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:132) method submits a lambda to this pool that calls `EventsQ.BlockUntilEvent()`. This means:
- The pool thread is blocked waiting for events
- If multiple `WaitEvent()` calls are made, they queue up in the pool
- Each queued `WaitEvent` blocks until an event arrives, but only the first one actually drains the queue

**Risk**: Accumulation of pending `WaitEvent` tasks in the pool. Each one blocks waiting for events, but events are consumed by only one task at a time. The others wake up, find no events, and block again.

#### MEDIUM: No timeout on thread joins

[`Poller.join()`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:266) and [`Writer.join()`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:454) have no timeout. If the background thread is stuck (e.g., waiting on a blocked YT RPC call), the destructor hangs.

**Risk**: Process shutdown hangs if a session is being destroyed while the background thread is blocked on YT operations.

### 3.3 Error Handling

#### HIGH: Poll loop exits on first error

In [`PollLoop()`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:217), any exception from `PullQueueConsumer` causes the loop to push a `TSessionClosedEvent` and break:
```cpp
} catch (const std::exception& ex) {
    EventsQ.Push(TSessionClosedEvent(...), 0);
    break;
}
```

Transient errors (network blips, temporary YT unavailability) permanently kill the read session.

**Risk**: Single transient failure kills the entire read session. No retry logic, no circuit breaker.

#### HIGH: Write loop similarly exits on first error

Same pattern in [`WriteLoop()`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:408) — any write/flush error terminates the writer thread.

**Risk**: Single write failure kills the write session.

#### MEDIUM: Silenced exceptions in destructors

Both [`~TQytTopicReadSession()`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:125) and [`~TQytTopicWriteSession()`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:322) wrap cleanup in try-catch with empty handler:
```cpp
} catch (...) {
}
```

**Risk**: Resource leaks or cleanup failures are silently ignored, making debugging difficult.

### 3.4 Data Model Limitations

#### HIGH: Single-column string data extraction

The [`ExtractRowData()`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:199) method only handles `String` type values:
```cpp
if (value.Type == NYT::NTableClient::EValueType::String) {
    data << TStringBuf(value.Data.String, value.Length);
}
```

Non-string columns are silently skipped. If the queue has binary data, the message payload is lost.

**Risk**: Data corruption or silent data loss for non-string queue columns.

#### MEDIUM: No message grouping support

The [`MakeMessage()`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:180) method always sets `messageGroupId` to empty string. YDB topics support message groups for ordering guarantees, but this is not mapped from YT queue partitions.

**Risk**: Message group ordering semantics from YDB side are not preserved.

#### MEDIUM: [`CommitOffset`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:618) without CAS

The commit offset operation passes `oldOffset = std::nullopt`:
```cpp
/* oldOffset */ std::nullopt,
```

This means the offset is advanced unconditionally, without comparing-and-swap protection. If multiple consumers commit offsets concurrently, later commits can overwrite earlier ones.

**Risk**: Offset regression if multiple readers commit concurrently for the same consumer.

### 3.5 Configuration & Path Resolution

#### MEDIUM: Duplicate path joining logic

Path joining is implemented in three places:
1. [`JoinPath()`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_yt_gateway.cpp:157) in gateway
2. [`JoinYtPath()`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:40) in topic client
3. [`ResolvePath()`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:644) lambda in topic client

The logic is nearly identical but not shared.

**Risk**: Divergence if one is fixed and others are not.

#### LOW: Consumer path resolution

In [`CreateReadSession()`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:559), the consumer name is resolved as a path:
```cpp
NYT::NYPath::TRichYPath consumerPath(ResolvePath(TString(settings.ConsumerName_)));
```

If the consumer name doesn't include the full YT path prefix, it may resolve to the wrong location.

**Risk**: Misconfigured consumer paths silently read from wrong queue.

### 3.6 Testing Gaps

#### HIGH: No integration tests for topic client

The unit tests only cover [`TBlockingEQueue`](ydb/library/yql/providers/pq/gateway/clients/qyt/ut/yql_qyt_blocking_queue_ut.cpp:11). There are no unit tests for:
- `TQytTopicClient`
- `TQytTopicReadSession`
- `TQytTopicWriteSession`

The stress test directory [`ydb/tests/stress/yt_queue/`](ydb/tests/stress/yt_queue/) appears to be empty (no files found).

**Risk**: Undetected regressions in the core read/write session logic.

#### MEDIUM: No mock for YT client

Without a mock YT client, unit testing the topic client requires a real YT cluster.

**Risk**: Tests cannot run in CI without YT infrastructure.

---

## 4. Code Quality Observations

### 4.1 Good Practices
- Clean separation between gateway, client, and session layers
- Template-based blocking queue is reusable and well-designed
- Backpressure mechanism prevents unbounded memory growth
- Unit tests for the blocking queue cover push/pop, backpressure, blocking, and stop scenarios

### 4.2 Areas for Improvement
- **Missing includes**: [`yql_qyt_blocking_queue.h`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_blocking_queue.h:1) only has 94 lines including guards — the implementation is entirely header-only which is fine for templates, but the file is referenced in the `ya.make` peer dir rather than as a direct source
- **No logging**: There are no LOG() calls anywhere in the implementation. Debugging production issues will be difficult
- **Magic numbers**: Poll period (50ms), max row count (1000), max data weight (16MB), queue sizes (4MB, 128KB) are hardcoded defaults with no documentation on tuning
- **`Y_UNUSED` proliferation**: Many parameters are marked unused, suggesting the interface contract is broader than needed for this implementation

---

## 5. Recommendations

### Priority 1 (Critical)
1. **Add retry logic** to poll and write loops with exponential backoff for transient errors
2. **Add timeout to thread joins** to prevent destructor hangs
3. **Fix `WaitEvent()` future resolution** — ensure pending futures resolve when session closes
4. **Add CAS protection** to `CommitOffset` or document the at-most-once semantics

### Priority 2 (High)
5. **Add comprehensive unit tests** with mocked YT client for all session operations
6. **Add logging** at key points (session create/close, errors, offset commits)
7. **Support binary data** in `ExtractRowData()` or document string-only limitation
8. **Deduplicate path joining** into a single utility

### Priority 3 (Medium)
9. **Document configuration** — especially the relationship between YDB topic paths and YT queue paths
10. **Add configuration validation** — reject invalid cluster settings at startup
11. **Consider connection pooling** or async client creation to avoid blocking the gateway mutex during network I/O
12. **Add metrics/counters** — currently `GetCounters()` returns `nullptr`

---

## 6. Summary

The QYT Topic Client provides a functional bridge between YDB's PQ infrastructure and YTsaurus queues. The architecture is sound — using background threads to emulate push semantics over a pull API is the right approach. The blocking queue with backpressure is well-designed.

However, the implementation has significant gaps in error resilience (single failure kills sessions), resource cleanup (potential hangs on shutdown), and test coverage (only the blocking queue is tested). These should be addressed before the feature is considered production-ready.

The solution is appropriate for initial integration and testing but carries operational risks in production until the critical items above are resolved.
