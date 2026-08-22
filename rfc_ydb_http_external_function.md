# External Function (HTTP UDF) — вызов внешних HTTP сервисов из запросов YDB

| Поле | Значение |
|------|----------|
| **Статус** | Draft |
| **Авторы** | @fomichev, @slonn |
| **Создан** | 2026-08-19 |
| **Обсуждение** | — |

> Этот документ является частью RFC «Вызов внешних HTTP сервисов из запросов YDB» и описывает
> **External Function (HTTP UDF)** — вызов HTTP-сервиса для каждой строки (или батча строк) потока данных.
> Второй режим — **External Source (HTTP Table Function)** — описан в отдельном документе
> [rfc_ydb_http_external_source.md](rfc_ydb_http_external_source.md).

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

Данный документ описывает механизм вызова внешних HTTP-сервисов из YQL-запросов в режиме
**External Function (HTTP UDF)**. Это позволяет пользователям расширять функциональность YDB,
интегрируя внешние API без необходимости изменять ядро базы данных или выгружать данные из кластера.

**External Function (HTTP UDF)** — вызов HTTP-сервиса для каждой строки (или батча строк) потока данных.
Аналог пользовательских функций, но с выполнением во внешнем сервисе. Реализуется через
`Http::Call(...)` и опирается на варианты **Option A (Input/Output Transform)** и
**Option B (Async Lookup Source)** из архитектурного документа.

---

## Use Cases

### Use Case 1: Прототипирование новой функциональности (@fomichev)

Пользователь разрабатывает новую фичу и ему требуется реализовать функциональность, которую сложно или невозможно выразить на YQL. Предлагается для ускорения разработки реализовать внешний сервис, доступный по HTTP, который реализует нужную функцию.

**Характеристики:**
- Разрабатываемый сервис контролируется пользователем/командой.
- Протокол взаимодействия может быть фиксированным и оптимизированным под конкретную задачу.
- Типичный протокол: бинарный (Protobuf) или JSON с известной схемой.
- Авторизация: может использовать внутренние механизмы (IAM token, mTLS).
- Батчинг: может быть оптимизирован под конкретный use case — сервис знает структуру данных и может принимать батчи.
- Жизненный цикл: сервис живёт параллельно с разработкой фичи; после стабилизации может быть портирован в нативный YQL или оставлен как внешний.

**Пример:** Команда хочет добавить ML-скоринг в запрос, но модель ещё не интегрирована в YDB. Вместо ожидания релиша они поднимают локальный сервис с моделью и вызывают его из запроса.

### Use Case 2: Интеграция с существующим сервисом (@slonn)

У пользователя уже есть сервис, который реализует нужную функцию. Нужно обеспечить вызов функций из существующего сервиса.

**Характеристики:**
- Существующий сервис может иметь произвольный протокол (REST API, GraphQL и т.д.).
- Авторизация: OAuth 2.0, API keys, базовая авторизация — зависит от сервиса.
- Батчинг: может не поддерживаться внешним сервисом; требуется поддержка построчных вызовов.
- Жизненный цикл: сервис независим от YDB; изменения в его API могут потребовать обновления запросов.
- SLA внешнего сервиса может отличаться от SLA YDB.

**Пример:** Компания использует внешний сервис для проверки адреса или обогащения данных о клиенте. Нужно вызывать этот сервис при обработке транзакций.

### Сравнение use case

| Параметр | Use Case 1 (Прототип) | Use Case 2 (Существующий сервис) |
|----------|----------------------|----------------------------------|
| **Протокол** | Фиксированный, оптимизированный (Protobuf/JSON) | Произвольный (REST, GraphQL, и др.) |
| **Авторизация** | Внутренняя (IAM, mTLS, без авторизации) | OAuth 2.0, API keys, Basic Auth |
| **Батчинг** | Поддерживается, оптимизирован | Может не поддерживаться |
| **Контроль над сервисом** | Полный (своя разработка) | Ограниченный (внешний сервис) |
| **SLA** | Контролируемый | Зависит от провайдера |
| **Типичный объём данных** | Большой (внутренние данные) | Умеренный (обращение к внешнему API) |
| **Требования к отказоустойчивости** | Высокие | Зависит от критичности сервиса |

---

## Как задача решается в других системах

### Apache Flink

Flink поддерживает вызов внешних сервисов через:

- **UDF (User-Defined Functions)** — пользователь может написать Java/Python функцию, которая делает HTTP-запрос внутри `eval()`. Функция выполняется в процессе TaskManager.
- **Async I/O** — Flink предоставляет `AsyncFunction` для асинхронных вызовов внешних систем. Позволяет отправлять несколько запросов параллельно и собирать ответы без блокировки потока.

**Пример (Java UDF с HTTP-вызовом):**

```java
// UDF, вызывающий внешний HTTP-сервис для каждой строки
public class HttpEnrichment extends RichMapFunction<Row, Row> {
    private transient HttpClient client;

    @Override
    public void open(Configuration parameters) {
        client = HttpClients.createDefault();
    }

    @Override
    public Row map(Row row) throws Exception {
        // POST JSON-тело во внешний сервис и чтение ответа
        HttpPost post = new HttpPost("https://api.example.com/predict");
        post.setEntity(new StringEntity("{\"feature\": " + row.getField(0) + "}"));
        HttpResponse resp = client.execute(post);
        String body = EntityUtils.toString(resp.getEntity());
        return Row.of(row.getField(0), body);
    }
}

// Использование в Table API / SQL
tableEnv.createTemporaryView("my_table", sourceTable);
Table result = tableEnv.sqlQuery(
    "SELECT id, name FROM my_table WHERE score > 0.5");
```

**Пример (Async I/O для параллельных HTTP-вызовов):**

```java
// AsyncFunction для неблокирующих HTTP-вызовов
public class AsyncHttpRequest extends RichAsyncFunction<Row, Row> {
    @Override
    public void asyncInvoke(Row row, ResultFuture<Row> resultFuture) {
        // асинхронный HTTP-запрос, результат через resultFuture.complete()
    }
}
```

**Ограничения Flink:**
- HTTP-запросы выполняются в процессе TaskManager — утечка памяти в UDF влияет на весь кластер.
- Нет встроенной защиты от SSRF.
- Нет встроенного connection pooling для HTTP.

**Документация:**
- [Flink Async I/O](https://nightlies.apache.org/flink/flink-docs-stable/docs/dev/datastream/operators/asyncio/) — асинхронные вызовы внешних систем
- [Flink Table API & SQL](https://nightlies.apache.org/flink/flink-docs-stable/docs/dev/table/tableapi/) — работа с таблицами и UDF

### Snowflake

- **External Functions** — вызов внешних HTTP-сервисов (AWS Lambda, Azure Functions) напрямую из SQL.
- Поддержка батчинга: Snowflake автоматически группирует строки в батчи перед отправкой.
- Встроенная обработка ошибок и таймаутов.
- Поддержка пользовательских заголовков для авторизации.

**Пример (External Function в SQL):**

```sql
-- Создание external function, указывающей на AWS Lambda / Azure Function
CREATE OR REPLACE EXTERNAL FUNCTION my_predict(feature FLOAT)
    RETURNS FLOAT
    API_INTEGRATION = my_api_integration
    AS 'https://my-account.execute-api.us-east-1.amazonaws.com/prod/predict';

-- Использование в запросе
SELECT
    id,
    my_predict(score) AS prediction
FROM my_table
WHERE score > 0.5;
```

**Пример (с пользовательскими заголовками авторизации):**

```sql
-- External function с заголовком авторизации
CREATE OR REPLACE EXTERNAL FUNCTION my_enrich(id INT)
    RETURNS VARIANT
    API_INTEGRATION = my_api_integration
    HEADERS = ('Authorization' = 'Bearer ${token}')
    AS 'https://api.example.com/enrich';
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

**Пример (Remote Function в SQL):**

```sql
-- Создание remote function, указывающей на Cloud Function
CREATE FUNCTION my_dataset.my_predict(feature FLOAT64)
RETURNS FLOAT64
REMOTE WITH CONNECTION `my-project.us.my_connection`
OPTIONS (
    endpoint = 'https://us-central1-my-project.cloudfunctions.net/predict'
);

-- Использование в запросе
SELECT
    id,
    my_dataset.my_predict(score) AS prediction
FROM my_table
WHERE score > 0.5;
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
CREATE EXTERNAL DATA SOURCE my_scoring_endpoint WITH (
    SOURCE_TYPE = "Http",
    LOCATION    = "https://api.internal.example.com",
    AUTH_METHOD = "TOKEN",
    TOKEN_SECRET_NAME = "scoring_token"
);
```

Запрос ссылается на эндпоинт по имени; автор запроса никогда не видит и не вставляет секреты.

#### External Function (HTTP UDF) — `Http::Call`

Вызов внешнего HTTP-сервиса для обогащения каждой строки (или батча строк). Функция `Http::Call` принимает произвольное тело запроса и произвольные заголовки, которые могут вычисляться из данных строки:

```
Http::Call(
    endpoint    : String,          -- имя EXTERNAL DATA SOURCE (URL + auth + TLS)
    body        : String?,         -- произвольное тело запроса; NULL/опущено => нет тела
    headers     : Dict<String,String>? | Struct?,  -- произвольные пользовательские заголовки
    options     : Struct?          -- опционально: Method, Path, QueryParams, Timeout
) -> Struct<
    StatusCode : Uint32,
    Headers    : Dict<String,String>,
    Body       : String
>
```

**Semantics of user-supplied headers and body:**

- **Body** — непрозрачные байты. Вызывающий отвечает за кодировку; обычно
  сопровождается заголовком `Content-Type`. `NULL` или опущенный аргумент означает "нет
  тела" (например, для `GET`).
- **Headers** предоставленные пользователем объединяются с запросом. Несколько заголовков
  с одинаковым именем поддерживаются через форму `List<Tuple<String,String>>` когда
  нужны дубликаты.
- **Приоритет и зарезервированные заголовки.** Заголовки, внедрённые платформой для
  настроенного `AUTH_METHOD` источника данных (например, `Authorization`) и
  транспортно-управляемые заголовки (`Host`, `Content-Length`, `Connection`,
  `Transfer-Encoding`) являются **зарезервированными**: пользовательские значения для них игнорируются (или, по
  конфигу, отклоняются с ошибкой) чтобы запрос не мог переопределить или извлечь
  креденшелы эндпоинта. Все остальные имена заголовков передаются без изменений.
- **Валидация.** Имена/значения заголовков валидируются (нет CR/LF — предотвращает
  инъекцию заголовков/response splitting); общие байты заголовков и байты тела ограничены
  теми же лимитами размера, что и в слое egress.
- **Секреты в заголовках.** Для помещения секрета в *пользовательский* заголовок (не покрытый
  встроенным `AUTH_METHOD`), значение ссылается на подсистему секретов,
  никогда не инлайнится:

  ```sql
  AsDict(AsTuple('X-Api-Key', Secret('scoring_api_key')))
  ```

  `Secret(...)` резолвится через `SecureParams`; литерал никогда не появляется в
  тексте запроса, логах или плане.

**Базовый вызов с JSON телом и одним пользовательским заголовком:**

```sql
SELECT
    t.id,
    t.name,
    Http::Call(
        'my_scoring_endpoint',              -- external data source object (holds URL + auth)
        FormatJson(AsStruct(t.f1 AS f1, t.f2 AS f2)),   -- произвольное тело из данных строки
        AsStruct('application/json' AS `Content-Type`)  -- пользовательский заголовок
    ) AS response
FROM my_table AS t;
```

**Произвольные построчные заголовки и тело.** Заголовки могут быть переданы либо как `Struct` (статические, читаемые литеральные ключи), либо как `Dict<String,String>` (полностью динамические, вычисляемые из данных строки). И байты тела, и значения заголовков могут зависеть от строки:

```sql
SELECT
    t.id,
    Http::Call(
        'my_scoring_endpoint',
        t.raw_payload,                         -- произвольное тело прямо из колонки
        AsDict(
            AsTuple('Content-Type', 'application/json'),
            AsTuple('X-Request-Id', CAST(t.id AS String)),   -- построчное значение заголовка
            AsTuple('X-Tenant',     t.tenant),                -- динамический ключ/значение из данных
            AsTuple('Accept-Language', COALESCE(t.lang, 'en'))
        ),
        AsStruct(
            'POST'          AS Method,
            '/v2/predict'   AS Path,           -- добавляется к LOCATION источника данных
            AsDict(AsTuple('model', t.model_name)) AS QueryParams,
            Interval("PT5S") AS Timeout
        )
    ) AS response
FROM my_table AS t;
```

#### Где функциональность может использоваться в запросе

`Http::Call` — это скалярное выражение, возвращающее struct, поэтому синтаксически оно может
появиться везде, где допускается скалярное выражение. Семантически, однако, это
**асинхронный вызов с побочными эффектами**, что ограничивает *как* он планируется в каждом clause.

| SQL clause | Поддержка | Как планируется | Примечания |
|------------|-----------|-----------------|------------|
| **Projection** (`SELECT`) | Да — основной случай | HTTP transform / lookup применяется к потоку, добавлена выходная колонка | Естественное, рекомендуемое место. Один вызов на строку (или на батч). |
| **Filter** (`WHERE`/`HAVING`) | Да, с осторожностью | Вызов материализуется в предыдущем stage, затем предикат читает его колонку результата | Вызов **не** переопределяется на предикат; оптимизатор должен вынести его так, чтобы он выполнялся один раз на строку и short-circuit отключён вокруг него. |
| **JOIN** (`... ON`, HTTP-backed сторона) | Да — через Option B | Streaming lookup join: build сторона — HTTP эндпоинт, probe ключи батчатся | Лучший вариант для key→value обогащения; response cache дедуплицирует повторяющиеся ключи. Не допускается в произвольных non-equi `ON` предикатах, которые могут бесконечно расширяться. |
| **Aggregation** (`GROUP BY`, agg args) | Да, но с ограничениями | Вызов выполняется в **pre-aggregation** projection stage; его результат затем питает агрегат | Нельзя вызывать *внутри* работающего aggregate combiner (например, на каждом шаге merge). Вызов в `GROUP BY` ключах не рекомендуется (один вызов на входную строку перед группировкой). |
| **Window functions** (`OVER (...)`) | Частично | Вызов выполняется в projection **до/после** window operator; не внутри frame evaluation | Не должен вызываться per-frame или per-peer-group iteration (это взорвёт количество вызовов и будет недетерминированным при пересканировании frame). |

Общие правила для всех clause:

- **Выполняется ровно там, где запланировано, один раз на строку/батч.** Оптимизатор изолирует
  `Http::Call` в dedicated stage чтобы он никогда не переисполнялся окружающим
  оператором, который может пересканировать ввод (aggregation combiners, window frames,
  join inner loops). Это гарантирует предсказуемое количество внешних вызовов.
- **Нет short-circuiting.** Поскольку вызов материализуется заранее,
  конструкции вроде `col IS NULL OR Http::Call(...)` **не** пропускают вызов; если
  нужна условная invocация, защитите явно (см. ниже).
- **Условная invocация.** Чтобы избежать вызова для строк, которым он не нужен, сначала отфильтруйте
  в предыдущем stage, или передайте sentinel который transform трактует как
  "skip" (возвращает NULL ответ без egress). Пример: `WHERE score > 0.5`
  размещён так, чтобы выполнялся до projection `Http::Call`.
- **Детерминизм.** `Http::Call` недетерминирован (внешнее состояние, ретраи).
  Трактруется как барьер для оптимизаций, которые предполагают чистоту (common
  subexpression elimination **не** схлопнёт два текстово идентичных вызова
  если явно не помечены как cacheable через lookup cache в Option B).
- **Cost/limits.** Per-query `MaxHttpRequestsPerQuery` считает вызовы across all
  clause; запрос, использующий вызов в projection *и* filter *и* join, платит за
  все.

Примеры использования по clause:

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

### 4.2 Архитектура

#### Варианты решения

| # | Вариант | Точка интеграции | Лучшее применение | Переиспользует существующее | Idempotency / retry | Усилия | Рекомендация |
|---|---------|------------------|-------------------|---------------------------|---------------------|--------|--------------|
| A | **Input/Output Transform** через DQ async I/O, расширение `ExternalFunction` провайдера | `CreateDqInputTransform` / `CreateDqOutputTransform` | Per-batch обогащение / внешняя функция на потоке | Высоко: DQ transforms + `ydb/library/actors/http` + function provider | Transform переисполняется при рестарте stage → нужен dedup/примечание at-least-once | Средне | **Рекомендуется как основной** |
| B | **Async Lookup Source** | `CreateDqLookupSource` / `IDqAsyncLookupSource` | Key-based построчное обогащение / "HTTP JOIN" с батчингом и кэшированием | Высоко: lookup инфра уже батчит ключи, имеет fullscan limit | Read-mostly (GET) → естественно retry-safe | Средне | **Рекомендуется для обогащения/JOIN** |
| D | **HTTP client внутри MiniKQL compute node** (первоначальное предложение) | `IHttpClient` в `TComputationContext` | — | Низко: новый HTTP стек, форкает provider | Сломано: нет await примитива в MiniKQL | Высоко | **Отклонено** |

Варианты A и B **дополняют друг друга**, а не исключают: они нацелены
на разные пользовательские сценарии и могут делить одни и те же нижние слои (HTTP egress actor,
безопасность, секреты, counters). D — это базовая линия, которую мы заменяем.

Прагматичный план: построить **общий HTTP egress слой** один раз, затем экспонировать его
через B (lookup обогащение) и/или A (transform).

#### Общая архитектура

Оба варианта делят одни и те же нижние слои:

```
        DQ async I/O actor (Transform / LookupSource)
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

#### Option A — Input/Output Transform (расширение `ExternalFunction`)

**Идея.** Смоделировать HTTP вызов как **DQ input transform**: поток входных строк
идёт в input, HTTP вызов делается на строку или на батч, и поток обогащённых строк
выходит. Это напрямую маппится на существующий механизм transform и на
существующую концепцию `ExternalFunction`/`TDqTransform`.

**Архитектура:**

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

**Путь компиляции.** Расширить существующий function provider
([`dq_function_physical_optimize.cpp`](ydb/library/yql/providers/function/provider/dq_function_physical_optimize.cpp:1),
[`dq_function_dq_integration.cpp`](ydb/library/yql/providers/function/provider/dq_function_dq_integration.cpp:24))
так чтобы `ExternalFunction('http', ...)` / `Http::Call(...)` lowering'ился в `TDqTransform`.
Transform settings несут только **статическую** часть запроса (endpoint
reference, default method/path, timeout, batch size, size limits). **Динамические,
построчные** части — произвольное тело и произвольные заголовки — *не* запекаются
в transform settings: они вычисляются MiniKQL graph и приходят как
обычные колонки в `TransformInput`. Конкретно, строка, подаваемая в transform, это
struct вроде `<body: String, headers: Dict<String,String>, path: String,
query: Dict<String,String>>`; transform actor читает эти поля на строку,
объединяет platform-reserved auth/transport заголовки, валидирует их, и делает
запрос. Это держит secret/header injection и валидацию на trusted actor
стороне позволяя значениям заголовков и телу зависеть от данных. Нет нового типа MiniKQL node;
transform применяется compute actor, вне MiniKQL graph.

#### Option B — Async Lookup Source

**Идея.** Переиспользовать **async lookup** механизмы (построенные для key→value обогащения,
например streaming lookup joins) для реализации "для каждого ключа, GET/POST HTTP
эндпоинт и вернуть ответ как payload".

**Архитектура:**

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

### 4.3 Компиляция запроса

Пайплайн компиляции SQL-запроса с HTTP-функциями следует существующему пути `ExternalFunction`:

```
SQL Query
"SELECT Http::Call('my_endpoint', ...) FROM t"
     │
     ▼
+------------------+
| 1. Parser        |  SQL -> AST (TExprNode tree)
|                  |  Http::Call -> TCoApply(TDqSqlExternalHttpFunction)
+--------+---------+
         │
         ▼
+------------------+
| 2. Type          |  Type annotation, validation
| Annotation       |  Resolve input/output types
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
| Stages Build     |  HTTP function -> TDqTransform через function provider
+--------+---------+
         │
         ▼
+------------------+
| 5. DQ Integration|  TDqTransform settings (endpoint, method, timeout, batch size)
|                  |  Динамические части (body, headers) остаются колонками в MiniKQL
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
- TransformInput/Output schema
- SecureParams (для секрета авторизации)
- TaskParams (endpoint reference, static config)
```

**Ключевое отличие от MiniKQL-внутреннего HTTP клиента:** Асинхронный I/O приводится
в движение **вне** MiniKQL graph, на уровне DQ compute actor.
В MiniKQL нет примитива "suspend this node until my future resolves".
`Yield` означает "нет данных *прямо сейчас*, весь task будет перепущен";
это не per-node await. Поэтому HTTP вызовы выполняются DQ async I/O actor'ами
(THttpTransformActor / THttpLookupSource), которые интегрируются с backpressure,
memory quotas и checkpointing.

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
- THttpTransformActor/THttpLookupSource ограничивает in-flight запросы
  на основе `freeSpace`.
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

- **Option A (Transform)** — Transform переисполняется при рестарте stage → **at-least-once**
  побочные эффекты. Нужно документировать это и направлять non-idempotent (POST)
  использование к явному opt-in / idempotency keys.
- **Option B (Lookup)** — Lookups read-oriented (GET) → естественно retry-safe.
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
    "url": "https://api.example.com/predict",
    "method": "POST",
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

Для упрощения отладки запросов с HTTP-функциями предусмотрен режим отладки:

```sql
-- Включение детального логирования HTTP-запросов
SET SETTINGS DEBUG_HTTP_REQUESTS = TRUE;

-- Ограничение количества запросов для отладки
SET SETTINGS DEBUG_HTTP_MAX_REQUESTS = 10;

-- Режим эмуляции (запросы не отправляются, возвращается фиктивный ответ)
SET SETTINGS DEBUG_HTTP_MOCK = TRUE;
SET SETTINGS DEBUG_HTTP_MOCK_RESPONSE = '{"prediction": 0.9, "confidence": 0.95}';
```

### Error Handling

Ошибки HTTP-запросов обрабатываются на нескольких уровнях:

1. **Сетевые ошибки** (connection refused, timeout) — автоматический retry с exponential backoff.
2. **HTTP 4xx** — ошибка возвращается в строку результата как `NULL` с флагом ошибки.
3. **HTTP 5xx** — автоматический retry (до лимита), затем ошибка.
4. **SSRF/Host denied** — немедленная ошибка, retry не выполняется.

```sql
-- Обработка ошибок в запросе
SELECT
    id,
    CASE
        WHEN resp.StatusCode >= 400
            THEN FORMAT('Error: status %d', resp.StatusCode)
        ELSE CAST(resp.Body AS Float64)
    END AS result
FROM (
    SELECT
        id,
        Http::Call('my_scoring_endpoint', FormatJson(AsStruct(feature1 AS f1, feature2 AS f2))) AS resp
    FROM my_table
);
```

### Поля ответа

Результат `Http::Call` — struct с полями:

| Поле | Тип | Описание |
|------|-----|----------|
| `StatusCode` | `Uint32` | HTTP статус-код ответа |
| `Headers` | `Dict<String,String>` | Заголовки ответа |
| `Body` | `String` | Тело ответа (opaque bytes) |

Для доступа к разпаршенным данным из JSON тела используйте `Json::` функции:

```sql
SELECT
    id,
    Json::LookupFloat(resp.Body, 'prediction') AS prediction,
    Json::LookupFloat(resp.Body, 'confidence') AS confidence
FROM (
    SELECT
        id,
        Http::Call('my_scoring_endpoint', FormatJson(AsStruct(feature1 AS f1, feature2 AS f2))) AS resp
    FROM my_table
);
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
    LookupSource     = 4ULL,  // HTTP lookup enrichment (Option B - Lookup)
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

### Phase 2: Option B — Async Lookup Source (приоритетный)

**Цель:** Реализовать key-based обогащение через lookup механизм.

4. **`THttpLookupSource`** — реализация `IDqAsyncLookupSource`
   - Maps keys → HTTP requests
   - Fan-out to THttpEgressActor (bounded concurrency)
   - Response cache (dedupe identical keys)
   - `TEvLookupResult` back to join operator

5. **`THttpLookupSourceFactory`** — фабрика в `IDqAsyncIoFactory::CreateDqLookupSource`

6. **Интеграция с streaming lookup join** в оптимизаторе

### Phase 3: Option A — Input/Output Transform (расширение ExternalFunction)

**Цель:** Поддержка per-batch обогащения через transform механизм.

7. **`THttpTransformActor`** — реализация `IDqComputeActorAsyncInput`
   - Pulls input rows, builds requests (batch of N)
   - Sends to THttpEgressActor
   - Buffers responses within freeSpace
   - `GetAsyncInputData()` emits enriched rows

8. **Расширение function provider** — `Http::Call(...)` → `TDqTransform`
   - Статическая часть в transform settings
   - Динамическая часть (body, headers) как колонки в `TransformInput`

### Phase 4: Безопасность, конфигурация и observability

**Цель:** Защита, управление и мониторинг.

9. **Host Allow/Deny List** — конфигурация на уровне кластера
10. **Rate limiting** — per-query, per-host, per-node
11. **Метрики и counters** — service-level и per-query
12. **Query plan stats** — request count, bytes, latency, errors
13. **Логирование** — с redacted секретами

### Phase 5: Тестирование и документация

**Цель:** Стабильность и качество.

14. **Unit tests** — mock HTTP для каждого слоя
15. **Actor tests** — через `TTestActorRuntime`
16. **Python E2E tests** — против реального test HTTP сервера (incl. timeout, connection-refused, 5xx, oversized response)
17. **Документация** — user guide (SQL surface, limits, security) + operator guide (config)

---

## Отказные сценарии и ограничения

### Ограничения

1. **Транзакционность** — HTTP-вызовы не являются частью транзакции YDB. Данные, полученные из внешнего сервиса, не участвуют в ACID-гарантиях.
2. **Детерминизм** — внешние сервисы могут возвращать разные результаты для одинаковых входных данных. `Http::Call` трактруется как барьер для оптимизаций, предполагающих чистоту.
3. **Производительность** — HTTP-запросы значительно медленнее локальных вычислений. Рекомендуется использовать батчинг и кэширование.
4. **Размер данных** — максимальный размер запроса/ответа ограничен конфигурацией (по умолчанию 10MB / 50MB).
5. **Поддержка протоколов** — на первом этапе поддерживаются только HTTP/1.1 и HTTP/2. gRPC-over-HTTP может быть добавлен в будущем.
6. **At-least-once для Option A** — Transform переисполняется при рестарте stage. Non-idempotent (POST) вызовы требуют явного opt-in / idempotency keys.

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

1. **Используйте батчинг** — группировка строк в батчи снижает количество HTTP-запросов.
2. **Настройте таймауты** — установите реалистичные таймауты для предотвращения зависаний.
3. **Обрабатывайте ошибки** — проверяйте `StatusCode` в ответе и используйте условную логику.
4. **Кэшируйте результаты** — для повторяющихся запросов рассмотрите кэширование на стороне внешнего сервиса или используйте Option B (lookup) с response cache.
5. **Мониторьте метрики** — следите за задержками и количеством ошибок через observability.
6. **Используйте idempotency keys** — для non-idempotent (POST) вызовов через Option A.
7. **Предпочитайте GET для enrichment** — Option B (lookup) естественно retry-safe для read-oriented операций.

---

## Ссылки

- [Async HTTP Client Architecture](async-http-client-architecture.md) — детальное описание архитектуры и вариантов решения
- [External Source (HTTP Table Function)](rfc_ydb_http_external_source.md) — второй режим работы (чтение HTTP как таблицы)
- [DQ Compute Actor Async I/O](ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h) — интерфейс async I/O
- [Existing HTTP Actor Library](ydb/library/actors/http/http.h) — существующий non-blocking HTTP клиент
- [Function Provider](ydb/library/yql/providers/function/provider/dq_function_provider.cpp) — существующий ExternalFunction провайдер
- [Federated Query Documentation](ydb/docs/en/core/concepts/query_execution/federated_query/) — существующая интеграция с внешними источниками
- [Snowflake External Functions](https://docs.snowflake.com/en/user-guide/external-functions) — референсная реализация
- [BigQuery Remote Functions](https://cloud.google.com/bigquery/docs/remote-functions) — референсная реализация