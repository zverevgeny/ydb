# YT (YTsaurus) External Data Source Support — Staged Changes Review

## Overview

This change adds support for connecting to **YTsaurus (YT)** queues through YDB External Data Sources (EDS). When a query references an EDS with `SOURCE_TYPE="YT"`, the system creates a YTsaurus RPC proxy client and adapts the YT queue API to the YDB Topics session model.

**Statistics**: 23 files changed, 1587 insertions(+), 4 deletions(-)

## Architecture

The implementation routes YT queues through the existing PQ (Persistent Queue / Topics) provider pipeline rather than using the native YT provider directly. The flow is:

```
EDS SOURCE_TYPE="YT" → PQ Provider AddCluster(CT_YT) → Native Gateway TryCreateYtTopicClient()
    → NYT::NApi::IClientPtr (RPC proxy) → TYtTopicClient (ITopicClient adapter)
```

## Changed Files by Category

### 1. Proto Definition

| File | Change |
|------|--------|
| [`yql/essentials/providers/common/proto/gateways_config.proto`](yql/essentials/providers/common/proto/gateways_config.proto:302) | Added `CT_YT = 3` enum value to `TPqClusterConfig.EClusterType`. Added descriptive comments to all enum values (`CT_UNSPECIFIED`, `CT_PERS_QUEUE`, `CT_DATA_STREAMS`, `CT_YT`). |

### 2. PQ Provider — Cluster Registration

| File | Change |
|------|--------|
| [`ydb/library/yql/providers/pq/provider/yql_pq_datasource.cpp`](ydb/library/yql/providers/pq/provider/yql_pq_datasource.cpp:317) | `AddCluster()` now detects `source_type="yt"` (case-insensitive) and sets `ClusterType = CT_YT`. Maps EDS properties: `location` → `Endpoint`, `token` → `Token`, `database_name` → `Database`. |
| [`ydb/library/yql/providers/pq/provider/yql_pq_dq_integration.cpp`](ydb/library/yql/providers/pq/provider/yql_pq_dq_integration.cpp:394) | `ToClusterType()` handles `CT_YT` → `NPq::NProto::Unspecified` (YT queues reuse the topic read pipeline; no dedicated DQ cluster type). |
| [`ydb/library/yql/providers/pq/provider/ya.make`](ydb/library/yql/providers/pq/provider/ya.make:1) | Added `RECURSE_FOR_TESTS(ut)` to enable unit test discovery. |

### 3. Native PQ Gateway — YT Client Creation

| File | Change |
|------|--------|
| [`ydb/library/yql/providers/pq/gateway/native/yql_pq_gateway.cpp`](ydb/library/yql/providers/pq/gateway/native/yql_pq_gateway.cpp:109) | `GetTopicClient()` first calls `TryCreateYtTopicClient(settings)`. New private methods: `TryCreateYtTopicClient()` — locates a `CT_YT` cluster config and lazily creates/caches `NYT::NApi::IClientPtr`; `FindYtClusterConfig()` — matches by `Endpoint` or `Database`; `CreateYtClient()` — builds RPC proxy connection + client with optional token. New member: `THashMap<TString, NYT::NApi::IClientPtr> YtClients` guarded by `Mutex`. |
| [`ydb/library/yql/providers/pq/gateway/native/ya.make`](ydb/library/yql/providers/pq/gateway/native/ya.make:1) | Added PEERDIR `.../clients/yt`, `yt/yt/client`; added `YQL_LAST_ABI_VERSION()`. |

### 4. New YT Topic Client Library

| File | Description |
|------|-------------|
| [`ydb/library/yql/providers/pq/gateway/clients/yt/yql_yt_topic_client.h`](ydb/library/yql/providers/pq/gateway/clients/yt/yql_yt_topic_client.h:1) | `TYtTopicClientSettings` struct (holds `NYT::NApi::IClientPtr`, `PathPrefix`, `DataColumn`, `MaxRowCount`, `MaxDataWeight`, `PollPeriodMs`). Entry point `CreateYtTopicClient()`. |
| [`ydb/library/yql/providers/pq/gateway/clients/yt/yql_yt_topic_client.cpp`](ydb/library/yql/providers/pq/gateway/clients/yt/yql_yt_topic_client.cpp:1) | Full `ITopicClient` implementation: `TYtTopicReadSession` (polling loop via `PullQueueConsumer` → topic events), `TYtTopicWriteSession` (producer session), `CommitOffset` via `AdvanceQueueConsumer`. Adapts YT queue pull API to YDB topic push/session model using a background polling thread + bounded blocking event queue with backpressure. |
| [`ydb/library/yql/providers/pq/gateway/clients/yt/yql_yt_blocking_queue.h`](ydb/library/yql/providers/pq/gateway/clients/yt/yql_yt_blocking_queue.h:1) | `TBlockingEQueue<TEvent>` — bounded thread-safe queue with backpressure for the polling adapter. |
| [`ydb/library/yql/providers/pq/gateway/clients/yt/yql_yt_gateway.h`](ydb/library/yql/providers/pq/gateway/clients/yt/yql_yt_gateway.h:1) | `TYtPqGatewaySettings` and `CreateYtPqGateway()` entry point. |
| [`ydb/library/yql/providers/pq/gateway/clients/yt/yql_yt_gateway.cpp`](ydb/library/yql/providers/pq/gateway/clients/yt/yql_yt_gateway.cpp:1) | Gateway implementation (214 lines). |
| [`ydb/library/yql/providers/pq/gateway/clients/yt/ya.make`](ydb/library/yql/providers/pq/gateway/clients/yt/ya.make:1) | LIBRARY with SRCS `yql_yt_topic_client.cpp`, `yql_yt_gateway.cpp`; PEERDIRs include `yt/yt/client`, topic SDK. |
| [`ydb/library/yql/providers/pq/gateway/clients/ya.make`](ydb/library/yql/providers/pq/gateway/clients/ya.make:1) | Added `yt` to RECURSE. |

### 5. Unit Tests

| File | Description |
|------|-------------|
| [`ydb/library/yql/providers/pq/provider/ut/yql_pq_datasource_ut.cpp`](ydb/library/yql/providers/pq/provider/ut/yql_pq_datasource_ut.cpp:1) | `PqDataSourceAddClusterTest` suite (4 tests): `AddYtClusterSetsClusterTypeYt`, `AddYtClusterCaseInsensitive`, `AddDataStreamsClusterByDefault`, `AddClusterUnknownSourceTypeFallsBackToDataStreams`. Uses `CreatePqFileGateway()` and inspects `state->Configuration->ClustersConfigurationSettings`. |
| [`ydb/library/yql/providers/pq/provider/ut/ya.make`](ydb/library/yql/providers/pq/provider/ut/ya.make:1) | Unit test build target. |
| [`ydb/core/external_sources/external_data_source_ut.cpp`](ydb/core/external_sources/external_data_source_ut.cpp:1) | `YtExternalDataSourceTest` suite — validates YT EDS name, auth, location, token, no-extra-properties, hostname patterns. |

### 6. Stress Tests

| File | Description |
|------|-------------|
| [`ydb/tests/stress/yt_queue/`](ydb/tests/stress/ya.make:1) | Python stress workload for YT queue operations. Includes `__main__.py`, `workload/__init__.py`, `tests/test_workload.py` and corresponding `ya.make` files. |
| [`ydb/tests/stress/ya.make`](ydb/tests/stress/ya.make:1) | Added `yt_queue` entry. |

## Key Design Decisions

1. **Reuse PQ pipeline**: YT queues are routed through the existing PQ provider rather than creating a separate provider path. This reduces code duplication and leverages the existing read/write infrastructure.

2. **Lazy client creation**: The `NYT::NApi::IClientPtr` is created on-demand in `TryCreateYtTopicClient()` and cached in `YtClients` by cluster name, avoiding unnecessary connections.

3. **Pull-to-push adaptation**: Since YT uses a pull-based queue API but YDB Topics expects a push-based session model, `TYtTopicReadSession` runs a background polling thread that fetches batches via `PullQueueConsumer` and pushes them into a bounded `TBlockingEQueue` with backpressure.

4. **Cluster matching**: `FindYtClusterConfig` matches clusters by `Endpoint` or `Database` from the `TTopicClientSettings`, allowing flexible cluster resolution.

## Build & Test Status

- **Library build**: `ydb/library/yql/providers/pq/gateway/clients/yt` — OK
- **Native gateway build**: `ydb/library/yql/providers/pq/gateway/native` — OK
- **PQ provider unit tests**: 1 suite / 4 tests — ALL GOOD
- **External sources YT EDS tests**: 6 tests — ALL GOOD

## Usage

Create an EDS pointing to YTsaurus:

```sql
CREATE EXTERNAL DATA SOURCE yt_source
WITH (
    SOURCE_TYPE = 'YT',
    LOCATION = 'grpc://yt-cluster.example.com: DinnoPort',
    TOKEN = '...',
    DATABASE_NAME = '/Root'
);
```

Queries referencing `yt_source` will automatically create and cache a YTsaurus RPC proxy client.

## Comparison: YT Queues vs YDB Topics via EDS

This section compares the new YT queue support with the existing YDB Topics cross-database access (both use the PQ provider internally).

### 1. EDS Registration

| Aspect | YDB Topics (`SOURCE_TYPE="YdbTopics"`) | YT Queues (`SOURCE_TYPE="YT"`) |
|--------|----------------------------------------|--------------------------------|
| Factory entry | [`external_source_factory.cpp:207`](ydb/core/external_sources/external_source_factory.cpp:207) → `PqProviderName` | [`external_source_factory.cpp:163`](ydb/core/external_sources/external_source_factory.cpp:163) → `YtProviderName` |
| Auth methods | `NONE`, `BASIC`, `TOKEN`, `IAM` | `NONE`, `TOKEN` |
| Allowed properties | `database_name`, `use_tls`, `shared_reading` | *(none — YT uses `location` + `token` from base EDS fields)* |
| Cluster type | `CT_DATA_STREAMS` (default fallback) | `CT_YT` (explicitly set when `source_type="yt"`) |

**Note**: Although YT EDS is registered under `YtProviderName` in the factory, the actual data flow routes through the PQ provider. The `source_type` property (set to `"YT"`) is what triggers the `CT_YT` cluster type in [`yql_pq_datasource.cpp:321`](ydb/library/yql/providers/pq/provider/yql_pq_datasource.cpp:321).

### 2. Client Creation

| Aspect | YDB Topics | YT Queues |
|--------|------------|-----------|
| Entry point | `GetTopicClient()` → `CreateExternalTopicClient()` | `GetTopicClient()` → `TryCreateYtTopicClient()` |
| Client type | [`TTopicClient`](ydb/library/yql/providers/pq/gateway/clients/external/yql_pq_topic_client.cpp:57) (wraps YDB C++ SDK) | [`NYT::NApi::IClientPtr`](ydb/library/yql/providers/pq/gateway/native/yql_pq_gateway.cpp:202) (YT RPC proxy) |
| Connection | YDB driver with endpoint + credentials | RPC proxy connection to YT Dinno port |
| Caching | Per-session via `GetYdbPqClient()` | Per-cluster in `YtClients` hash map |
| Authentication | IAM, token, service account, basic | Token only (`TClientOptions::FromToken`) |

### 3. Read Model

| Aspect | YDB Topics | YT Queues |
|--------|------------|-----------|
| Session model | Push-based (`IReadSession` with `WaitEvent()`) | Pull-based → adapted to push via polling thread |
| Read API | `TTopicClient::CreateReadSession()` | `PullQueueConsumer()` in background loop |
| Backpressure | Built into YDB SDK (async events) | [`TBlockingEQueue`](ydb/library/yql/providers/pq/gateway/clients/yt/yql_yt_blocking_queue.h:15) bounded queue |
| Consumer | Named consumer on topic | Named queue consumer with partition offsets |
| Offset commit | `CommitOffset()` via YDB SDK | `AdvanceQueueConsumer()` via YT API |

### 4. Write Model

| Aspect | YDB Topics | YT Queues |
|--------|------------|-----------|
| Write API | `IWriteSession::Write()` (push) | `TYtTopicWriteSession` → YT producer client |
| Federated writes | `TFederatedTopicClient::CreateWriteSession()` | Not supported (`GetFederatedTopicClient()` returns error) |
| Transaction support | Yes (via `TTransactionBase*`) | No |

### 5. Path & Database Handling

| Aspect | YDB Topics | YT Queues |
|--------|------------|-----------|
| Path format | `/domain/topic` (YDB scheme path) | `/path/to/queue` (YT path) |
| Database | Explicit `database_name` property | From EDS `database_name`, defaults to `/Root` |
| Path remapping | None | `JoinYtPath()` prepends `PathPrefix` from settings |
| RTMR compat | PersQueue: remaps `/queue/topic` → `/logbroker-federation/queue` | N/A |

### 6. DQ (Data Queue) Integration

| Aspect | YDB Topics | YT Queues |
|--------|------------|-----------|
| DQ cluster type | `NPq::NProto::DataStreams` | `NPq::NProto::Unspecified` (reuses topic pipeline) |
| Consumer required | No | No (handled by queue API internally) |
| Watermark support | Yes | Yes (via `TSourceWatermarksSettings`) |

### 7. Architecture Diagram

```
YDB Topics (Cross-Database):
┌──────────────┐     EDS: YdbTopics      ┌─────────────────┐
│  KQP Host    │ ── SOURCE_TYPE ──>      │  PQ Provider    │
│              │                          │  AddCluster()   │
└──────────────┘                          │  CT_DATA_STREAMS│
                                          └────────┬────────┘
                                                   │
                                          ┌────────▼────────┐
                                          │ Native Gateway   │
                                          │ GetTopicClient() │
                                          └────────┬────────┘
                                                   │
                                          ┌────────▼────────┐
                                          │ TTopicClient     │
                                          │ (YDB C++ SDK)    │
                                          └────────┬────────┘
                                                   │
                                          ┌────────▼────────┐
                                          │  Remote YDB     │
                                          │  Topic Service   │
                                          └─────────────────┘

YT Queues:
┌──────────────┐     EDS: YT             ┌─────────────────┐
│  KQP Host    │ ── SOURCE_TYPE ──>      │  PQ Provider    │
│              │                          │  AddCluster()   │
└──────────────┘                          │  CT_YT          │
                                          └────────┬────────┘
                                                   │
                                          ┌────────▼────────┐
                                          │ Native Gateway   │
                                          │ TryCreateYt...() │
                                          └────────┬────────┘
                                                   │
                                          ┌────────▼────────┐
                                          │ NYT::NApi::     │
                                          │ IClientPtr      │
                                          │ (RPC Proxy)     │
                                          └────────┬────────┘
                                                   │
                                          ┌────────▼────────┐
                                          │ TYtTopicClient   │
                                          │ (Pull→Push       │
                                          │  Adapter)        │
                                          └────────┬────────┘
                                                   │
                                          ┌────────▼────────┐
                                          │  YTsaurus       │
                                          │  Queue Service   │
                                          └─────────────────┘
```

### 8. Key Differences Summary

| Dimension | YDB Topics | YT Queues |
|-----------|------------|-----------|
| **Transport** | gRPC to YDB server | RPC proxy to YT Dinno |
| **Session model** | Native push (async) | Polling loop → emulated push |
| **Client caching** | Per-session | Per-cluster (shared) |
| **Auth** | IAM, token, SA, basic | Token only |
| **Write support** | Full (push + federated) | Basic (producer, no federated) |
| **Transaction** | Supported | Not supported |
| **Code complexity** | Thin wrapper (~60 lines) | Full adapter (~700 lines) |
| **Performance char.** | Async, event-driven | Thread-per-session polling |
