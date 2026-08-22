# Вызов внешних HTTP сервисов из запросов YDB

| Поле | Значение |
|------|----------|
| **Статус** | Draft |
| **Авторы** | @fomichev, @slonn |
| **Создан** | 2026-08-19 |
| **Обсуждение** | — |

## Содержание

1. [Введение](#введение)
2. [Два режима работы](#два-режима-работы)
3. [Общая архитектура](#общая-архитектура)
4. [Общие компоненты](#общие-компоненты)
5. [Безопасность](#безопасность)
6. [Observability](#observability)
7. [Конфигурация](#конфигурация)
8. [Ссылки](#ссылки)

---

## Введение

Данный RFC описывает механизм вызова внешних HTTP-сервисов непосредственно из YQL-запросов. Это позволяет пользователям расширять функциональность YDB, интегрируя внешние API без необходимости изменять ядро базы данных или выгружать данные из кластера.

Механизм поддерживает два основных режима работы, каждый из которых описан в отдельном документе:

- **External Function (HTTP UDF)** — вызов HTTP-сервиса для каждой строки (или батча строк) потока данных. Аналог пользовательских функций, но с выполнением во внешнем сервисе.
- **External Source (HTTP Table Function)** — чтение данных из HTTP-эндпоинта как из таблицы. Позволяет использовать внешний API как источник данных в `FROM`/`JOIN`.

---

## Два режима работы

### 1. External Function (HTTP UDF)

Вызов внешнего HTTP-сервиса для обогащения каждой строки (или батча строк) потока данных.
Реализуется через `Http::Call(...)` и опирается на варианты **Option A (Input/Output Transform)**
и **Option B (Async Lookup Source)**.

**Подробное описание:** [rfc_ydb_http_external_function.md](rfc_ydb_http_external_function.md)

**Ключевые особенности:**
- `Http::Call(endpoint, body, headers, options)` — скалярное выражение, возвращающее struct.
- Произвольное тело запроса и произвольные заголовки, вычисляемые из данных строки.
- Секреты через `SecureParams` / `Secret()`, никогда не инлайнятся в SQL.
- Может использоваться в `SELECT`, `WHERE`, `JOIN`, `GROUP BY`, `OVER`.

### 2. External Source (HTTP Table Function)

Чтение данных из HTTP-эндпоинта как из таблицы. Реализуется через
**Option C (External Source / Sink)**.

**Подробное описание:** [rfc_ydb_http_external_source.md](rfc_ydb_http_external_source.md)

**Ключевые особенности:**
- Чтение датасета из HTTP-эндпоинта как из таблицы в `FROM`/`JOIN`.
- Пагинация/партиционирование для больших датасетов.
- `EXTERNAL DATA SOURCE` типа `Http` для административного управления.

---

## Общая архитектура

Оба режима делят одни и те же нижние слои:

```
        DQ async I/O actor (Transform / LookupSource / Source)
                          │  (non-blocking, backpressured)
                          ▼
        THttpEgressActor  (thin wrapper over ydb/library/actors/http)
          - per-node singleton, owns the actor HTTP proxy
          - connection reuse / keep-alive (предоставляется http lib)
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

### Варианты решения

| # | Вариант | Режим | Точка интеграции | Рекомендация |
|---|---------|-------|------------------|--------------|
| A | **Input/Output Transform** | External Function | `CreateDqInputTransform` / `CreateDqOutputTransform` | **Рекомендуется как основной** |
| B | **Async Lookup Source** | External Function | `CreateDqLookupSource` / `IDqAsyncLookupSource` | **Рекомендуется для обогащения/JOIN** |
| C | **External Source / Sink** | External Source | `CreateDqSource` / `CreateDqSink` | **Рекомендуется для table-style доступа** |
| D | **HTTP client внутри MiniKQL** | — | `IHttpClient` в `TComputationContext` | **Отклонено** |

Варианты A, B и C **дополняют друг друга**, а не исключают: они нацелены
на разные пользовательские сценарии и могут делить одни и те же нижние слои (HTTP egress actor,
безопасность, секреты, counters). D — это базовая линия, которую мы заменяем.

---

## Общие компоненты

Поперечные компоненты, переиспользуемые каждым вариантом:

- **Egress actor** над [`ydb/library/actors/http`](ydb/library/actors/http/http.h)
  — нет нового TLS/connection-pool кода.
- **Secrets** через `SecureParams` (уже прокинуты в
  [`TSourceArguments`](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:262)
  и `TInputTransformArguments`).
- **Memory** через `IMemoryQuotaManager`
  ([async_io.h:39](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:39)).
- **Errors** через `TEvAsyncInputError`
  ([async_io.h:76](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:76)).
- **Security** (allow/deny + SSRF) enforced внутри egress actor.

### Resource Safety & Correctness

- **Backpressure** — медленный эндпоинт замедляет запрос вместо unbounded буферизации или OOM.
- **Memory Quota** — байты ответа учитываются против memory quota compute actor через `IMemoryQuotaManager`.
- **Cancellation** — abort запроса / kill compute actor отменяет in-flight запросы без утечек.
- **Idempotency и retry** — Option A (Transform) at-least-once; Option B (Lookup) retry-safe; Option C (Source) retry-safe.

---

## Безопасность

### Host Allow/Deny List

Каждый HTTP-запрос проходит проверку по списку разрешённых/запрещённых хостов.
По умолчанию: **deny-all** или явный allowlist (решается с security командой).

### SSRF Protection

1. **Блокировка внутренних IP** — запрет запросов к диапазонам RFC 1918, link-local, loopback,
   включая cloud metadata `169.254.169.254`.
2. **Защита от DNS rebinding** — валидация resolved IP на соответствие разрешённому хосту.
3. **Валидация URL** — отклонение схем `javascript:`, `data:`, `file:`.
4. **Лимиты редиректов** — максимум 3 редиректа, по умолчанию без cross-host редиректов.

Применяется **до** первого запроса, а не как более поздний этап.

### Rate Limiting

```
Per-query:   MaxHttpRequestsPerQuery (default: 10000)
Per-host:    MaxRequestsPerHostPerSecond (default: 100)
Per-node:    MaxConcurrentRequests (default: 500)
```

### Авторизация и секреты

Секреты/токены авторизации берутся из подсистемы секретов
(`SecureParams` / TokenAccessor), никогда из plaintext SQL, и redact'ятся
из логов.

| Метод | Описание | Use Case |
|-------|----------|----------|
| **Bearer Token** | JWT/API token в заголовке `Authorization` через `TOKEN_SECRET_NAME` | Большинство REST API |
| **Basic Auth** | Базовая HTTP-авторизация через секреты | Простые сервисы |
| **API Key** | Ключ в заголовке или query параметре через `Secret()` | Cloud API |
| **mTLS** | Взаимная TLS-аутентификация | Внутренние сервисы |
| **OAuth 2.0** | Полноценный OAuth flow | Внешние сервисы |
| **YDB IAM** | Использование IAM токенов YDB | Внутренние сервисы Yandex |

TLS verification включена по умолчанию; отключение — явное и аудируемое.

---

## Observability

### Метрики на уровне сервиса (THttpCounters)

| Метрика | Описание |
|---------|----------|
| `TotalRequests` | Общее количество HTTP-запросов |
| `ActiveRequests` | Текущее количество активных запросов |
| `ActiveConnections` | Текущее количество активных соединений |
| `TotalConnections` | Общее количество созданных соединений |
| `RequestTimeouts` | Количество таймаутов |
| `ConnectionErrors` | Количество ошибок соединения |
| `HostDenyErrors` | Количество запросов к запрещённым хостам |
| `ResponseSizeBytes` | Общий размер ответов в байтах |
| `RequestSizeBytes` | Общий размер запросов в байтах |

### Метрики на уровне задачи (THttpTaskCounters)

| Метрика | Описание |
|---------|----------|
| `RequestCount` | Количество запросов в рамках задачи |
| `ResponseBytes` | Байты ответов в рамках задачи |
| `RequestBytes` | Байты запросов в рамках задачи |
| `TotalLatencyUs` | Суммарная задержка в микросекундах |
| `ErrorCount` | Количество ошибок в рамках задачи |

### Query Plan Stats

В план выполнения запроса добавляются следующие статистики:

```
HttpRequestsCount       - Total HTTP requests made
HttpRequestBytes        - Total bytes sent in requests
HttpResponseBytes       - Total bytes received in responses
HttpRequestLatencyUs    - Total time spent waiting for HTTP responses
HttpErrorsCount         - Total HTTP errors (network, timeout, denied)
```

---

## Конфигурация

### System-Level Configuration

```protobuf
message THttpServiceConfig {
    bool enable = false;

    // Connection pool settings (управляется ydb/library/actors/http)
    uint32 max_total_connections = 100;
    uint32 max_connections_per_host = 20;
    uint32 connection_idle_timeout_sec = 60;

    // Request limits
    uint32 max_concurrent_requests = 500;
    uint64 max_request_size_bytes = 10485760;   // 10MB
    uint64 max_response_size_bytes = 52428800;  // 50MB
    uint32 default_timeout_sec = 30;

    // Security
    repeated string allowed_hosts = [];    // Empty = deny all (решается с security)
    repeated string denied_hosts = [];
    bool allow_insecure = false;           // Allow HTTP (non-TLS)

    // Proxy
    string proxy_url = "";

    // Batching
    uint32 default_batch_size = 100;
    uint32 max_batch_size = 10000;
    uint32 batch_timeout_ms = 10;
}
```

### Query-Level Settings

```cpp
NCommon::TConfSetting<bool, Static> _KqpEnableHttpClient;
NCommon::TConfSetting<TString, Static> AllowedHttpHosts;     // Comma-separated
NCommon::TConfSetting<ui32, Static> MaxHttpRequestsPerQuery;
NCommon::TConfSetting<ui32, Static> HttpRequestTimeoutSec;
```

### Node-Level Settings

```cpp
enum class EEnabledHttpNodes : ui64 {
    None             = 0ULL,
    ExternalFunction = 1ULL,  // HTTP UDF calls (Option A - Transform)
    ExternalSource   = 2ULL,  // HTTP table functions (Option C - Source)
    LookupSource     = 4ULL,  // HTTP lookup enrichment (Option B - Lookup)
    All              = ~0ULL,
};
```

---

## Ссылки

- [External Function (HTTP UDF)](rfc_ydb_http_external_function.md) — вызов HTTP-сервиса как функции
- [External Source (HTTP Table Function)](rfc_ydb_http_external_source.md) — чтение HTTP как таблицы
- [Async HTTP Client Architecture](async-http-client-architecture.md) — детальное описание архитектуры и вариантов решения
- [DQ Compute Actor Async I/O](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h) — интерфейс async I/O
- [Existing HTTP Actor Library](ydb/library/actors/http/http.h) — существующий non-blocking HTTP клиент
- [Function Provider](ydb/library/yql/providers/function/provider/dq_function_provider.cpp) — существующий ExternalFunction провайдер
- [Federated Query Documentation](ydb/docs/en/core/concepts/query_execution/federated_query/) — существующая интеграция с внешними источниками
- [Snowflake External Functions](https://docs.snowflake.com/en/user-guide/external-functions) — референсная реализация
- [BigQuery Remote Functions](https://cloud.google.com/bigquery/docs/remote-functions) — референсная реализация