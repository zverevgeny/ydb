# CPU-учёт и лимитирование чтения PQ через resource pools (WLM / HDRF)

**Ветка:** `feat/pq-read-cpu-accounting-wlm`  
**План / исходная постановка:** `cpu-accounting.md`  
**Статус:** реализовано в рабочей копии; ключевые UT зелёные (см. раздел «Тесты»).

## 1. Краткое резюме

Доработка закрывает пробел HDRF-планировщика для долгоживущих streaming-запросов, читающих/пишущих Topics (PQ):

1. **Idle** — задача не резервирует FairShare, пока ждёт данные от источников.
2. **External CPU** — CPU Topic SDK (декомпрессия / компрессия) учитывается через `GetCpuTime()` и попадает в Usage пула.
3. **Backpressure при throttle** — при троттлинге CA источники подавляют `NotifyCA` / pull, чтобы не раздувать буферы.
4. **Метрики** — Idle / ThrottleEvents / IdleTimeUs на уровне пула.
5. **SQL-тест** — пул с 10% CPU, обработка топика и контроль потребления по счётчикам планировщика.

---

## 2. Архитектура

### 2.1 Участники

```
Resource Pool (TOTAL_CPU_LIMIT_PERCENT_PER_NODE)
        │
        ▼
TComputeSchedulerService ── UpdateFairShare() ──► HDRF tree
        │                                              │
        │                                              ▼
        │                                    schedulerPool/<poolId>/{Limit,Usage,...}
        ▼
TKqpComputeActor : TSchedulableComputeActorBase
        │  EnterIdle / ExitIdle
        │  AccountBurstUsage(GetCpuTime delta)
        │  SetSchedulerThrottled(true|false) → sources
        │
        ├── TDqPqReadActor      (SDK ReadSession + TCpuAccountingExecutor)
        ├── TDqPqWriteActor     (SDK WriteSession + TCpuAccountingExecutor)
        └── TDqPqRdReadActor    (Row Dispatcher; GetCpuTime уже был, throttle добавлен)
```

### 2.2 Поток управления для topic read

```
Topic SDK threads
  └─ decompress (TCpuAccountingExecutor) ──► atomic CpuMicros
           │
           ▼
TDqPqReadActor::WaitEvent / GetEvents
  │  if SchedulerThrottled → suppress NotifyCA, remember pending
  ▼
TEvNewAsyncInputDataArrived → CA
  │  OnBeforeContinueExecuteFromNewAsyncInput → ExitIdle
  │  DoExecuteImpl → StartExecution / MiniKQL / StopExecution
  │  PollAsyncInput → GetCpuTime() delta → OnSourceCpuTimeAccounted → AccountBurstUsage
  │  if PendingInput & no inflight outputs → EnterIdle
  └─ if FairShare denied → SetSchedulerThrottled(true) + delayed Resume
```

### 2.3 Модель HDRF: Demand / Idle / Throttle

| Состояние задачи | Demand | Usage | Смысл |
|---|---|---|---|
| Active (исполняется / конкурирует) | +1 | может быть > 0 | участвует в FairShare |
| Idle (ждёт данных от source) | 0 | 0 | не «держит» долю CPU |
| Throttled (FairShare не дал слот) | +1 (не Idle) | 0 | отложенный Resume; sources приглушены |

Раньше `Demand` рос в конструкторе `TSchedulableTask` и падал только в деструкторе. Для бесконечного streaming это означало постоянную резервацию FairShare даже при отсутствии данных.

### 2.4 Учёт external CPU

Планировщик по-прежнему меряет wall-time `DoExecuteImpl` через `StartExecution` / `StopExecution`. Дополнительно:

- `IDqAsyncInput::GetCpuTime()` / sink-аналог отдают накопленное время SDK/RD.
- CA считает дельту между опросами и вызывает `AccountBurstUsage`.
- Тип usage отделён в API задачи (`CPU_DEFAULT` и т.п.), чтобы burst Usage попадал в те же пуловые счётчики FairShare.

Декомпрессия/компрессия Topic SDK обёрнуты в `TCpuAccountingExecutor` (decorator над `NYdb::IExecutor`): wall-time `Post()` накапливается в `std::atomic<ui64>` и экспонируется через `GetCpuTime()`.

### 2.5 Backpressure при scheduler throttle

Когда CA не получает FairShare:

1. CA вызывает `SetSchedulerThrottled(true)` на всех async sources.
2. PQ read: не шлёт `NotifyCA`, не тянет лишние события в ReadyBuffer (pending запоминается).
3. При снятии throttle: re-`SubscribeOnNextEvent()` + `NotifyCA`, если были pending / непустой ReadyBuffer (иначе CA мог «уснуть» после WaitEvent без re-arm).

Аналогичный флаг/логика добавлены для RD-read.

---

## 3. Инженерные решения

### 3.1 Idle вместо «вечного Demand»

- API: `TSchedulableTask::EnterIdle` / `ExitIdle`, прокси в `TSchedulableActorBase`.
- CA входит в Idle после `StopExecution`, если run status = `PendingInput`, нет inflight outputs и нет уже запланированного Resume.
- CA выходит из Idle **до** `ContinueExecute` на `TEvNewAsyncInputDataArrived` и в начале успешного `DoExecuteImpl`.
- В PassAway: сброс Idle/Throttle-уведомлений sources, чтобы не оставить source в throttled после смерти CA.

### 3.2 Decorator executor вместо изменений Topic SDK API

Вместо расширения контракта Read/Write session выбран локальный wrapper `TCpuAccountingExecutor`:

- не требует изменений публичного SDK API;
- измеряет именно задачи, которые SDK постит на compression/decompression executor;
- погрешность — wall-time потока executor’а, не cgroup CPU; для HDRF достаточно как upper-bound proxy нагрузки.

### 3.3 Throttle как сигнал в IDqAsyncInput

Добавлен виртуальный `SetSchedulerThrottled(bool)` в `IDqAsyncInput` (default no-op). Это проще, чем отдельные события mailbox’а, и локализует политику в source.

Важный нюанс unthrottle: пока throttled, `GetEvents`/subscribe-цикл может «разоружить» WaitEvent; при снятии throttle нужна явная переподписка, иначе данные в SDK-буфере не разбудят CA.

### 3.4 Метрики пула

На динамическом пуле HDRF:

- `Idle`, `Active`, `IdleTimeUs`, `ThrottleEvents` (+ существующие `Usage`, `Limit`, …).
- В дереве: атомики `CpuIdle`, `CpuBurstIdle`, `CpuThrottleEvents`.

Публикуются в FairShare snapshot (`TakeSnapshot`).

### 3.5 Наблюдаемость в UT: AppData counters

В `ydb/core/testlib/test_client.cpp` планировщик раньше создавался на **отдельном** `TDynamicCounters`, не связанном с `AppData::Counters`. Тесты читали `AppData` и всегда видели нули.

Исправление: `CreateKqpComputeScheduler(AppData.Counters, …)` — как в production (`driver_lib/run`). Без этого SQL-контроль потребления невозможен.

### 3.6 SQL-тест: 10% CPU, не zero-CPU cancel

Zero-CPU + `QueryCancelAfter` сознательно не используются (по требованию): нужен сценарий с реальной квотой и контролем потребления.

Сценарий `TopicReadSqlCpuConsumptionUnderTenPercentPool`:

1. Пул `TOTAL_CPU_LIMIT_PERCENT_PER_NODE = 10`.
2. Navigate properties → `"10"`.
3. Streaming query `INSERT … SELECT * FROM topic` с `RESOURCE_POOL`.
4. Запись сообщений → чтение output topic.
5. Ожидание `Limit >= 1e6` и роста Usage / IdleTimeUs / ThrottleEvents.

Хелперы topic SDK в тесте используют `driver.Stop(false)`: sync `Stop(true)` в kikimr UT может зависнуть навсегда.

---

## 4. Изменённые компоненты (карта файлов)

| Область | Файлы |
|---|---|
| Scheduler task/actor | `kqp_schedulable_task.{h,cpp}`, `kqp_schedulable_actor.{h,cpp}` |
| Schedulable CA | `kqp_compute_actor.h` |
| HDRF tree / counters | `tree/common.h`, `tree/dynamic.{h,cpp}` |
| DQ CA base | `dq_compute_actor_impl.h`, `dq_compute_actor_async_io.h` |
| PQ IO | `dq_pq_read_actor.cpp`, `dq_pq_write_actor.cpp`, `dq_pq_rd_read_actor.cpp` |
| CPU executor | `dq_pq_cpu_accounting_executor.h` (+ UT) |
| Testlib | `ydb/core/testlib/test_client.cpp` |
| Tests | scheduler UT, pq_async_io UT, `stream_query_classification_ut.cpp` |

---

## 5. Тесты

### 5.1 Unit / component

| Тест | Что проверяет | Статус |
|---|---|---|
| `SchedulableTaskIdle*` / throttle events в `kqp_compute_scheduler_ut` | EnterIdle/ExitIdle, Demand/CpuIdle, AccountBurstUsage, ThrottleEvents | реализовано |
| `TCpuAccountingExecutor` UT | накопление micros вокруг `Post()` | реализовано |
| `SchedulerThrottledSuppressesNotifyAndExposesCpuTime` (`dq_pq_read_actor_ut`) | throttle глушит notify; unthrottle снова будит CA; `GetCpuTime` доступен | **GOOD** |

### 5.2 SQL / WLM integration

| Тест | Что проверяет | Статус |
|---|---|---|
| `StreamingTopicCpuLimit::TopicReadSqlCpuConsumptionUnderTenPercentPool` | пул 10%, streaming read/write topic, Limit/Usage (idle/throttle) | **GOOD** |

Запуск:

```bash
./ya make --build relwithdebinfo ydb/services/workload_manager/ut -tA \
  -F '*TopicReadSqlCpuConsumptionUnderTenPercentPool*'

./ya make --build relwithdebinfo ydb/tests/fq/pq_async_io/ut -tA \
  -F '*SchedulerThrottled*'

./ya make --build relwithdebinfo ydb/core/kqp/runtime/scheduler -tA \
  -F '*SchedulableTaskIdle*'
```

### 5.3 Что тест **не** доказывает

- Точную долю «не больше 10% wall-CPU процесса» (счётчики — модель HDRF, не cgroup).
- Изоляцию между двумя пулами под конкурентной нагрузкой (нет comparative throughput-теста 10% vs 100%).
- Полный учёт gRPC/network CPU вне compression executor.
- Корректность FairShare при многоузловом кластере.

---

## 6. Проблемы и риски

### 6.1 Известные технические риски

1. **Неполный охват SDK CPU.** Учитывается wall-time задач на compression/decompression executor. Приём по сети, внутренние SDK-потоки вне этого executor’а, pure network stack — по-прежнему вне Usage.
2. **Wall-time ≠ CPU-time.** `THPTimer` меряет прошедшее время потока; при contention на executor Usage может завышаться.
3. **Row Dispatcher shared path.** RD делит CPU сессии между клиентами своим механизмом; throttle на `TDqPqRdReadActor` ограничивает notify в CA, но shared decompress на стороне RD не останавливается полностью одним потребителем.
4. **Idle heuristics.** Idle ставится только при `PendingInput` + пустой output inflight. Ошибочный Idle при кратковременной пустоте / наоборот отсутствие Idle при «тихом» source может искажать FairShare.
5. **Unthrottle race.** Без re-subscribe после throttle CA может не получить событие о данных (баг был пойман UT’ом). Регрессии в соседних путях (federated clusters, reconnect) требуют внимания.
6. **Memory under throttle.** Подавление NotifyCA снижает рост ReadyBuffer CA, но SDK/internal buffers всё ещё могут расти до своих лимитов; жёсткий cap размера буфера не вводился.
7. **Метрики в UT.** До фикса `test_client` любые проверки `schedulerPool/*` в kikimr-тестах были ложноотрицательными. Другие стенды/хелперы с отдельным counters root могут иметь ту же проблему.

### 6.2 Продуктовые / операционные риски

1. После включения scheduler для streaming пулы с низким `%` могут заметно снизить latency/throughput topic pipelines — это ожидаемо, но нужна ясная документация и дашборды Idle/ThrottleEvents.
2. Пул с `TOTAL_CPU_LIMIT_PERCENT_PER_NODE = 0` по-прежнему опасен для streaming (cancel / голодание); отдельный e2e на cancel не входит в текущий набор.
3. Изменение Demand через Idle меняет FairShare относительно batch-запросов на том же узле: batch могут получить больше, streaming — меньше в периоды ожидания данных (желаемый эффект), но при частых Idle/ExitIdle возможны осцилляции.

### 6.3 Проблемы, встреченные при разработке тестов

| Проблема | Причина | Решение |
|---|---|---|
| Счётчики Usage/Limit всегда 0 | scheduler на orphan counters в testlib | wiring на `AppData.Counters` |
| Hang 600s в SQL-тесте | `TDriver::Stop(true)` в UT | `Stop(false)` |
| Digest::Sha256 в streaming | усложнение / риск type mismatch | упрощение до `SELECT *` |
| Notify не приходит после unthrottle | WaitEvent disarmed while throttled | re-subscribe + notify if pending |
| Zero-CPU cancel сценарий | не соответствует запросу «10% + контроль» | заменён на 10% consumption test |

### 6.4 Открытые follow-up

- Comparative тест: одинаковая нагрузка в пулах 10% и 100%, сравнение throughput / ThrottleEvents.
- Явные SDK buffer occupancy metrics в мониторинге (план в `cpu-accounting.md`).
- Проверка multi-node / serverless tenants.
- Возможный hard limit на ReadyBuffer при длительном throttle.
- Убедиться, что schemeshard/other test envs тоже не создают scheduler на orphan counters.

---

## 7. Связь с исходным планом (`cpu-accounting.md`)

| Пункт плана | Статус в этой доработке |
|---|---|
| PoolId для streaming / классификатор | опираемся на уже существующий WLM + `RESOURCE_POOL` (отдельные classification UT уже были) |
| Idle Demand для long-lived | **сделано** |
| CPU compression/decompression → Usage | **сделано** через executor wrapper + GetCpuTime |
| Throttle → backpressure sources | **сделано** для PQ read / RD read (+ write executor accounting) |
| Мониторинг Idle/Throttle/… | **частично** (pool counters); buffer occupancy — не полностью |
| Zero-CPU cancel e2e | **не делаем** (по решению); вместо этого 10% + consumption |

---

## 8. Команды быстрой проверки

```bash
# SQL 10% CPU + consumption
./ya make --build relwithdebinfo ydb/services/workload_manager/ut -tA \
  -F '*TopicReadSqlCpuConsumptionUnderTenPercentPool*'

# PQ throttle / notify
./ya make --build relwithdebinfo ydb/tests/fq/pq_async_io/ut -tA \
  -F '*SchedulerThrottled*'

# Scheduler idle/throttle unit
./ya make --build relwithdebinfo ydb/core/kqp/runtime/scheduler -tA \
  -F '*SchedulableTaskIdle*'
```
