# External Source (HTTP Table Function) — чтение данных из HTTP-эндпоинта как из таблицы

| Поле | Значение |
|------|----------|
| **Статус** | Draft |
| **Авторы** | @fomichev, @slonn |
| **Создан** | 2026-08-19 |
| **Обсуждение** | — |

> Этот документ является частью RFC «Вызов внешних HTTP сервисов из запросов YDB» и описывает
> **External Source (HTTP Table Function)** — чтение данных из HTTP-эндпоинта как из таблицы.
> Второй режим — **External Function (HTTP UDF)** — описан в отдельном документе
> [rfc_ydb_http_external_function.md](rfc_ydb_http_external_function.md).

## Содержание

1. [Введение](#введение)
2. [Use Cases](#use-cases)
3. [Как задача решается в других системах](#как-задача-решается-в-других-системах)
4. [Предложение по расширению](#предложение-по-расширению)
   - [4.1 SQL синтаксис](#41-sql-синтаксис)
   - [4.2 Архитектура](#42-архитектура)
   - [4.3 Компиляция запроса](#43-компиляция-запроса)
   - [4.4 Безопасность](#44-безопасность)
5. [Resource Safety & Correctness](#resource-safety--correctness)
   - [5.1 Backpressure](#51-backpressure)
   - [5.2 Memory Quota](#52-memory-quota)
   - [5.3 Cancellation](#53-cancellation)
   - [5.4 Idempotency и retry](#54-idempotency-и-retry)
6. [Observability](#observability)
7. [Debugging](#debugging)
8. [Конфигурация](#конфигурация)
9. [План реализации](#план-реализации)
10. [Отказные сценарии и ограничения](#отказные-сценарии-и-ограничения)

---

## Введение

Данный документ описывает механизм чтения данных из внешних HTTP-эндпоинтов как из таблиц
в YQL-запросах. Это позволяет использовать внешний API как источник данных в `FROM`/`JOIN`
без необходимости выгружать данные из кластера.

**External Source (HTTP Table Function)** — чтение данных из HTTP-эндпоинта как из таблицы.
Позволяет использовать внешний API как источник данных в `FROM`/`JOIN`. Реализуется через
**Option C (External Source / Sink)** из архитектурного документа.

---

## Use Cases

### Use Case: Чтение датасета из внешнего HTTP-сервиса

Пользователю требуется читать данные из внешнего HTTP-эндпоинта как из таблицы — например,
список сущностей, метаданные, справочники, которые обновляются во внешнем сервисе.

**Характеристики:**
- Эндпоинт отдаёт структурированные данные (JSON, Protobuf) с известной схемой.
- Данные могут быть большими и требовать пагинации/партиционирования.
- Авторизация: может использовать внутренние механизмы (IAM token, mTLS).
- Жизненный цикл: сервис независим от YDB; изменения в его API могут потребовать обновления запросов.
- SLA внешнего сервиса может отличаться от SLA YDB.

**Пример:** Компания использует внешний сервис, который отдаёт справочник клиентов или
список сущностей. Нужно читать этот справочник как таблицу и JOIN'ить с локальными данными.

---

## Как задача решается в других системах

### Apache Flink

- **Table API** — поддержка `EXTERNAL TABLE` через connectors (JDBC, Kafka, HTTP).

**Пример (чтение HTTP-источника как таблицы через Table API):**

```sql
-- Регистрация HTTP-источника как внешней таблицы
CREATE TABLE http_entities (
    id      INT,
    metadata STRING
) WITH (
    'connector' = 'http',
    'url'       = 'https://api.example.com/entities',
    'format'    = 'json'
);

-- Чтение из внешней таблицы
SELECT id, metadata
FROM http_entities
WHERE id > 100;
```

**Ограничения Flink:**
- HTTP-запросы выполняются в процессе TaskManager — утечка памяти в UDF влияет на весь кластер.
- Нет встроенной защиты от SSRF.
- Нет встроенного connection pooling для HTTP.

**Документация:**
- [Flink Table API & SQL](https://nightlies.apache.org/flink/flink-docs-stable/docs/dev/table/tableapi/) — работа с таблицами и connectors
- [Flink Connectors](https://nightlies.apache.org/flink/flink-docs-stable/docs/connectors/table/overview/) — обзор connectors

### PostgreSQL

- **`postgres_fdw`** — Foreign Data Wrapper для подключения к другим PostgreSQL базам.
- **`dblink`** — выполнение запросов к удалённым PostgreSQL базам.

**Пример (чтение удалённой таблицы через `postgres_fdw`):**

```sql
-- Создание foreign server и foreign table
CREATE SERVER remote_server
    FOREIGN DATA WRAPPER postgres_fdw
    OPTIONS (host 'remote.example.com', dbname 'mydb');

CREATE USER MAPPING FOR current_user
    SERVER remote_server
    OPTIONS (user 'remote_user', password 'secret');

CREATE FOREIGN TABLE remote_entities (
    id       INT,
    metadata TEXT
) SERVER remote_server
  OPTIONS (schema_name 'public', table_name 'entities');

-- Чтение из foreign table
SELECT id, metadata
FROM remote_entities
WHERE id > 100;
```

**Ограничения PostgreSQL:**
- HTTP-запросы блокирующие (нет асинхронности).
- Требуется установка расширений на сервер.
- Нет встроенного connection pooling.

**Документация:**
- [PostgreSQL postgres_fdw](https://www.postgresql.org/docs/current/postgres-fdw.html) — Foreign Data Wrapper
- [PostgreSQL dblink](https://www.postgresql.org/docs/current/dblink.html) — выполнение запросов к удалённым БД

### Snowflake

- **External Functions** — вызов внешних HTTP-сервисов (AWS Lambda, Azure Functions) напрямую из SQL.
- Поддержка батчинга: Snowflake автоматически группирует строки в батчи перед отправкой.
- Встроенная обработка ошибок и таймаутов.

**Пример (External Function для чтения данных из внешнего сервиса):**

```sql
-- External function, возвращающая данные из внешнего HTTP-сервиса
CREATE OR REPLACE EXTERNAL FUNCTION get_entity_metadata(id INT)
    RETURNS VARIANT
    API_INTEGRATION = my_api_integration
    AS 'https://api.example.com/entities/{id}';

-- Использование в запросе
SELECT
    t.id,
    get_entity_metadata(t.entity_id) AS metadata
FROM my_table t;
```

**Преимущества Snowflake:**
- Прозрачный батчинг.
- Интеграция с cloud-функциями.
- Хорошая обработка ошибок.

**Документация:**
- [Snowflake External Functions](https://docs.snowflake.com/en/sql-reference/external-functions) — справочник по SQL
- [Snowflake External Functions — Introduction](https://docs.snowflake.com/en/user-guide/external-functions-introduction) — введение и ограничения

### BigQuery

- **Remote Functions** — вызов Google Cloud Functions из SQL-запросов.
- Поддержка SQL-типов на входе/выходе.
- Автоматическое масштабирование через Cloud Functions.

**Пример (Remote Function для чтения данных из внешнего сервиса):**

```sql
-- Remote function, возвращающая данные из внешнего HTTP-сервиса
CREATE FUNCTION my_dataset.get_entity_metadata(id INT64)
RETURNS STRING
REMOTE WITH CONNECTION `my-project.us.my_connection`
OPTIONS (
    endpoint = 'https://us-central1-my-project.cloudfunctions.net/entity_metadata'
);

-- Использование в запросе
SELECT
    t.id,
    my_dataset.get_entity_metadata(t.entity_id) AS metadata
FROM my_table t;
```

**Ограничения BigQuery:**
- Работает только с Google Cloud Functions.
- Дополнительные затраты на вызовы функций.

**Документация:**
- [BigQuery Remote Functions](https://cloud.google.com/bigquery/docs/remote-functions) — руководство
- [BigQuery CREATE FUNCTION](https://cloud.google.com/bigquery/docs/reference/standard-sql/data-definition-language#create_function_statement) — синтаксис

---

## Предложение по расширению

### 4.1 SQL синтаксис

#### Administration: Регистрация HTTP-эндпоинта

Эндпоинт, базовый URL, TLS-настройки и **секрет авторизации** хранятся в объекте `EXTERNAL DATA SOURCE`, создаваемом администратором, а не встраиваются в запрос:

```sql
CREATE EXTERNAL DATA SOURCE my_external_source WITH (
    SOURCE_TYPE = "Http",
    LOCATION    = "https://api.internal.example.com",
    AUTH_METHOD = "TOKEN",
    TOKEN_SECRET_NAME = "external_source_token"
);
```

Запрос ссылается на эндпоинт по имени; автор запроса никогда не видит и не вставляет секреты.

#### External Source (HTTP Table Function)

Чтение данных из HTTP-эндпоинта как из таблицы. Эндпоинт трактуется как внешняя **таблица**:
source читает страницы данных из него; sink POST'ит строки в него. Это зеркалит то, как
структурированы S3 / PQ / generic провайдеры.

```sql
SELECT id, metadata
FROM my_external_source.`/entities?since=2024-01-01`
WITH (FORMAT = "json", SCHEMA = (id Int64, metadata String));
```

**Параметры:**

| Параметр | Описание |
|----------|----------|
| `FORMAT` | Формат данных: `json`, `protobuf` и др. |
| `SCHEMA` | Схема колонок результата |
| `Path` (в backticks) | Путь и query-параметры, добавляемые к `LOCATION` источника данных |

#### Использование в JOIN

HTTP-источник может использоваться как build-сторона в JOIN:

```sql
SELECT
    t.id,
    t.name,
    ext.metadata
FROM my_table t
LEFT JOIN my_external_source.`/entities` AS ext
    ON t.entity_id = ext.id
WITH (FORMAT = "json", SCHEMA = (id Int64, metadata String));
```

### 4.2 Архитектура

#### Вариант решения

| # | Вариант | Точка интеграции | Лучшее применение | Переиспользует существующее | Idempotency / retry | Усилия | Рекомендация |
|---|---------|------------------|-------------------|---------------------------|---------------------|--------|--------------|
| C | **External Source / Sink** (HTTP data source) | `CreateDqSource` / `CreateDqSink` | Чтение/запись целых датасетов по HTTP как таблицы | Высоко: зеркалит S3/PQ провайдеры | Source reads retry-safe; sink нужен care | Средне-Высоко | **Рекомендуется для table-style доступа** |

#### Общая архитектура

Вариант C делит нижние слои с External Function (HTTP UDF):

```
        DQ async I/O actor (Source / Sink)
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

Поперечные компоненты, переиспользуемые вариантом C:

- **Egress actor** над [`ydb/library/actors/http`](ydb/library/actors/http/http.h)
  — нет нового TLS/connection-pool кода.
- **Secrets** через `SecureParams` (уже прокинуты в
  [`TSourceArguments`](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:262)).
- **Memory** через `IMemoryQuotaManager`
  ([async_io.h:39](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:39)).
- **Errors** через `TEvAsyncInputError`
  ([async_io.h:76](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:76)).
- **Security** (allow/deny + SSRF) enforced внутри egress actor.

#### Option C — External Source / Sink (HTTP data source)

**Идея.** Трактровать HTTP эндпоинт как внешнюю **таблицу**: source читает страницы
данных из него; sink POST'ит строки в него. Это зеркалит то, как структурированы
S3 / PQ / generic провайдеры.

**Интеграция.** `CreateDqSource` / `CreateDqSink`
([async_io.h:352](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:352),
[async_io.h:362](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:362)),
driven by an `EXTERNAL DATA SOURCE` of type `Http`, plus a new provider that plans
partitioned/paginated reads (`ReadRanges` in
[`TSourceArguments`](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h:256)).

**Архитектура:**

```
Query planner → HTTP provider → partitions (page ranges / URL shards)
        │
        ▼
  THttpSourceActor (IDqComputeActorAsyncInput)
    - reads pages of data from the endpoint
    - parses rows (JSON/Protobuf) into declared schema
    - backpressure via freeSpace
        ▼
  THttpEgressActor ─► actors/http ─► endpoint
```

### 4.3 Компиляция запроса

Пайплайн компиляции SQL-запроса с HTTP-источником следует существующему пути внешних источников
(S3 / PQ / generic провайдеры):

```
SQL Query
"SELECT ... FROM my_external_source.`/entities` WITH (...)"
     │
     ▼
+------------------+
| 1. Parser        |  SQL -> AST (TExprNode tree)
|                  |  External source -> TDataSource / TRead
+--------+---------+
         │
         ▼
+------------------+
| 2. Type          |  Type annotation, validation
| Annotation       |  Resolve schema, FORMAT
+--------+---------+
         │
         ▼
+------------------+
| 3. Logical       |  Optimize logical plan
| Optimization     |  Push predicates, eliminate redundant ops
+--------+---------+
         │
         ▼
+------------------+
| 4. Physical      |  Build physical plan (stages, connections)
| Stages Build     |  HTTP source -> TDqSource через HTTP provider
+--------+---------+
         │
         ▼
+------------------+
| 5. DQ Integration|  TSourceArguments (ReadRanges, SecureParams, TaskParams)
|                  |  Partitioned/paginated reads
+--------+---------+
         │
         ▼
+------------------+
| 6. Serialize     |  SerializeNode() -> bytecode string
| Program          |  Pack into NKqpProto::TProgram
+--------+---------+
         │
         ▼
Compiled Stage Proto (NKqpProto::TStage)
- Program (bytecode)
- Source schema
- SecureParams (для секрета авторизации)
- TaskParams (endpoint reference, static config)
```

**Ключевое отличие от MiniKQL-внутреннего HTTP клиента:** Асинхронный I/O приводится
в движение **вне** MiniKQL graph, на уровне DQ compute actor.
В MiniKQL нет примитива "suspend this node until my future resolves".
`Yield` означает "нет данных *прямо сейчас*, весь task будет перепущен";
это не per-node await. Поэтому HTTP чтение выполняется DQ async I/O actor'ом
(THttpSourceActor), который интегрируется с backpressure, memory quotas и checkpointing.

### 4.4 Безопасность

#### Host Allow/Deny List

Каждый HTTP-запрос проходит проверку по списку разрешённых/запрещённых хостов:

```
Request → HostAllowList Check → Denied? → Error: "Host not allowed"
                        → Allowed? → Proceed to DNS resolution
```

По умолчанию: **deny-all** или явный allowlist (решается с security командой).

#### SSRF Protection

1. **Блокировка внутренних IP** — запрет запросов к диапазонам RFC 1918, link-local, loopback,
   включая cloud metadata `169.254.169.254`.
2. **Защита от DNS rebinding** — валидация resolved IP на соответствие разрешённому хосту.
3. **Валидация URL** — отклонение схем `javascript:`, `data:`, `file:`.
4. **Лимиты редиректов** — максимум 3 редиректа, по умолчанию без cross-host редиректов.

Применяется **до** первого запроса, а не как более поздний этап.

#### Rate Limiting

```
Per-query:   MaxHttpRequestsPerQuery (default: 10000)
Per-host:    MaxRequestsPerHostPerSecond (default: 100)
Per-node:    MaxConcurrentRequests (default: 500)
```

#### Авторизация и секреты

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

## Resource Safety & Correctness

### 5.1 Backpressure

Backpressure работает: медленный эндпоинт замедляет запрос вместо unbounded
буферизации или OOM.

- DQ async I/O интерфейс предоставляет `GetAsyncInputData(..., freeSpace)` который
  сообщает actor'у сколько места доступно в буфере compute actor.
- THttpSourceActor ограничивает in-flight запросы на основе `freeSpace`.
- Конкуренция лимиты enforced: per-query, per-host, per-node.

### 5.2 Memory Quota

Байты ответа учитываются против memory quota compute actor
(через `IMemoryQuotaManager`), с конфигурируемым max request/response size.

- Каждый ответ HTTP запроса регистрируется в `IMemoryQuotaManager` перед доставкой
  в compute actor.
- Если quota исчерпана, backpressure останавливает новые запросы.
- Максимальный размер запроса/ответа ограничен конфигурацией (по умолчанию 10MB / 50MB).

### 5.3 Cancellation

Cancellation чистая: abort запроса / kill compute actor отменяет
in-flight запросы и не утекает actor'ов, promises или соединений.

- При уничтожении compute actor, все связанные async I/O actor'ы получают
  cancel сигнал.
- THttpEgressActor отменяет pending запросы и закрывает связанные ресурсы.
- Connection pool возвращает соединения или закрывает их при timeout.

### 5.4 Idempotency и retry

Retry семантика явная и документированная:

- **Option C (Source)** — Source reads retry-safe; sink нужен care.
- Сетевые ошибки (connection refused, timeout) — автоматический retry с exponential backoff.
- HTTP 4xx — ошибка возвращается в строку результата как `NULL` с флагом ошибки.
- HTTP 5xx — автоматический retry (до лимита), затем ошибка.
- SSRF/Host denied — немедленная ошибка, retry не выполняется.

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

### Логирование

Каждый HTTP-запрос логируется с контекстом:

```json
{
    "level": "INFO",
    "component": "DqHttpClient",
    "message": "HTTP request completed",
    "request_id": "42",
    "url": "https://api.example.com/entities",
    "method": "GET",
    "status_code": 200,
    "latency_us": 15234,
    "request_size": 256,
    "response_size": 1024,
    "query_id": "abc-123",
    "stage_id": 1,
    "task_id": 0
}
```

---

## Debugging

### Режим отладки

Для упрощения отладки запросов с HTTP-источниками предусмотрен режим отладки:

```sql
-- Включение детального логирования HTTP-запросов
SET SETTINGS DEBUG_HTTP_REQUESTS = TRUE;

-- Ограничение количества запросов для отладки
SET SETTINGS DEBUG_HTTP_MAX_REQUESTS = 10;

-- Режим эмуляции (запросы не отправляются, возвращается фиктивный ответ)
SET SETTINGS DEBUG_HTTP_MOCK = TRUE;
SET SETTINGS DEBUG_HTTP_MOCK_RESPONSE = '[{"id": 1, "metadata": "..."}]';
```

### Error Handling

Ошибки HTTP-запросов обрабатываются на нескольких уровнях:

1. **Сетевые ошибки** (connection refused, timeout) — автоматический retry с exponential backoff.
2. **HTTP 4xx** — ошибка возвращается в строку результата как `NULL` с флагом ошибки.
3. **HTTP 5xx** — автоматический retry (до лимита), затем ошибка.
4. **SSRF/Host denied** — немедленная ошибка, retry не выполняется.

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
    ExternalSource   = 2ULL,  // HTTP table functions (Option C - Source)
    All              = ~0ULL,
};
```

---

## План реализации

### Phase 1: Shared HTTP Egress слой

**Цель:** Построить общий HTTP egress слой, который переиспользуется всеми вариантами.

1. **`THttpEgressActor`** — per-node singleton actor, тонкий wrapper над `ydb/library/actors/http`
   - Connection reuse / keep-alive (предоставляется http lib)
   - Global concurrency limit, per-host rate limit
   - Allow/deny list + SSRF + DNS-rebinding checks
   - TLS config, proxy config
   - Service-level monitoring counters

2. **Security module** — Host allow/deny list, SSRF protection, DNS rebinding validation

3. **Secrets интеграция** — `SecureParams` поток в egress actor

### Phase 2: Option C — External Source / Sink

**Цель:** Поддержка table-style HTTP доступа.

4. **HTTP provider** — планирует partitioned/paginated reads
5. **`CreateDqSource` / `CreateDqSink`** интеграция
6. **`EXTERNAL DATA SOURCE` типа `Http`** — административный интерфейс
7. **`THttpSourceActor`** — реализация `IDqComputeActorAsyncInput`
   - Reads pages of data from the endpoint
   - Parses rows (JSON/Protobuf) into declared schema
   - Backpressure via freeSpace

### Phase 3: Безопасность, конфигурация и observability

**Цель:** Защита, управление и мониторинг.

8. **Host Allow/Deny List** — конфигурация на уровне кластера
9. **Rate limiting** — per-query, per-host, per-node
10. **Метрики и counters** — service-level и per-query
11. **Query plan stats** — request count, bytes, latency, errors
12. **Логирование** — с redacted секретами

### Phase 4: Тестирование и документация

**Цель:** Стабильность и качество.

13. **Unit tests** — mock HTTP для каждого слоя
14. **Actor tests** — через `TTestActorRuntime`
15. **Python E2E tests** — против реального test HTTP сервера (incl. timeout, connection-refused, 5xx, oversized response)
16. **Документация** — user guide (SQL surface, limits, security) + operator guide (config)

---

## Отказные сценарии и ограничения

### Ограничения

1. **Транзакционность** — HTTP-чтение не является частью транзакции YDB. Данные, полученные из внешнего сервиса, не участвуют в ACID-гарантиях.
2. **Детерминизм** — внешние сервисы могут возвращать разные результаты для одинаковых входных данных. Это может привести к недетерминированным результатам запроса.
3. **Производительность** — HTTP-запросы значительно медленнее локальных вычислений. Рекомендуется использовать батчинг и кэширование.
4. **Размер данных** — максимальный размер запроса/ответа ограничен конфигурацией (по умолчанию 10MB / 50MB).
5. **Поддержка протоколов** — на первом этапе поддерживаются только HTTP/1.1 и HTTP/2. gRPC-over-HTTP может быть добавлен в будущем.
6. **Sink (запись)** — запись строк в HTTP-эндпоинт требует особого внимания к идемпотентности (POST не идемпотентен).

### Отказные сценарии

| Сценарий | Поведение |
|----------|-----------|
| Внешний сервис недоступен | Retry с exponential backoff, затем ошибка в строке |
| Таймаут запроса | Retry (если не исчерпан лимит), затем `NULL` с ошибкой |
| Превышение rate limit | Запрос ставится в очередь или отклоняется с ошибкой |
| Ошибка парсинга ответа | Ошибка в строке результата |
| SSRF обнаружен | Немедленная ошибка, запрос не выполняется |
| Исчерпание connection pool | Запрос ставится в очередь с таймаутом |
| Memory quota исчерпана | Backpressure останавливает новые запросы |
| Query cancellation | In-flight запросы отменяются, ресурсы освобождаются |

### Рекомендации по использованию

1. **Используйте пагинацию/партиционирование** — для больших датасетов разбивайте чтение на страницы.
2. **Настройте таймауты** — установите реалистичные таймауты для предотвращения зависаний.
3. **Обрабатывайте ошибки** — проверяйте статус-коды и используйте условную логику.
4. **Кэшируйте результаты** — для повторяющихся запросов рассмотрите кэширование на стороне внешнего сервиса.
5. **Мониторьте метрики** — следите за задержками и количеством ошибок через observability.
6. **Используйте idempotency keys** — для sink (POST) операций.

---

## Ссылки

- [Async HTTP Client Architecture](async-http-client-architecture.md) — детальное описание архитектуры и вариантов решения
- [External Function (HTTP UDF)](rfc_ydb_http_external_function.md) — второй режим работы (вызов HTTP как функции)
- [DQ Compute Actor Async I/O](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h) — интерфейс async I/O
- [Existing HTTP Actor Library](ydb/library/actors/http/http.h) — существующий non-blocking HTTP клиент
- [Federated Query Documentation](ydb/docs/en/core/concepts/query_execution/federated_query/) — существующая интеграция с внешними источниками
- [Snowflake External Functions](https://docs.snowflake.com/en/user-guide/external-functions) — референсная реализация
- [BigQuery Remote Functions](https://cloud.google.com/bigquery/docs/remote-functions) — референсная реализация