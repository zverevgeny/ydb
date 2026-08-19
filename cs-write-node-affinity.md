# ColumnShard Write Node Affinity


---

## 1. Формулировка проблемы.

При записи в колонную таблицу при CTAS / FILL / INSERT) создаётся **один** WriteActor
на весь Sink Stage:

```
ComputeActor
    ↓ (все строки)
TKqpDirectWriteActor  ← один актор на все шарды
    ↓
TShardedWriteController
    ├─ Hash(PK) → Shard[0] → Buffer[0] → отправка на CS[0] (другая нода!)
    ├─ Hash(PK) → Shard[1] → Buffer[1] → отправка на CS[1] (другая нода!)
    └─ Hash(PK) → Shard[N] → Buffer[N] → отправка на CS[N] (другая нода!)
```

**Недостатки:**

1. **Нет локальности** — WriteActor может быть на любой ноде, данные всегда идут по сети
2. **Bottleneck** — один WriteActor обрабатывает все записи последовательно
3. **Память** — все per-shard буферы сосредоточены в одном месте
4. **Нет affinity** — планировщик не учитывает расположение шардов

### 1.1 Текущая(origin/main) реализация CTAS

#### 1.1.1 Декомпозиция CTAS на стейтменты

С `EnablePerStatementQueryExecution=true` CTAS компилируется как три независимых стейтмента:

1. **CREATE TABLE** — создаёт temp-таблицу `/.tmp/sessions/.../Destination_uuid`
2. **FILL** — записывает данные в temp-таблицу (стейтмент с sink mode `MODE_FILL`)
3. **MOVE** — атомарно переименовывает temp-таблицу в `/Root/Destination`

Именно стейтмент **FILL** является объектом оптимизации.

#### 1.1.2 Оптимизатор: [`BuildFillTableEffect`](ydb/core/kqp/opt/kqp_opt_effects.cpp:162)

`BuildFillTableEffect(node, ctx, effect, order)` — сигнатура без `TKqpOptimizeContext`.

Для Union-input (типичный CTAS с источником из другой таблицы) строится **один** `TDqStage`:
- входной канал: `TDqCnMap` из upstream stage
- программа: `ToFlow(row)`
- выход: `TDqSink` (sink внутри того же stage)

Sink settings (`TKqpTableSinkSettings`):
- `Type = MODE_FILL`, `Table.Path` = путь temp-таблицы назначения
- `InputColumns` = список имён колонок из плана
- `InconsistentWrite = true`, `StreamWrite = true`
- `OriginalPath` (путь destination) хранится как атом в settings самого stage — в proto sink settings не передаётся

#### 1.1.3 Компилятор: [`FillCreateTableAs`](ydb/core/kqp/query_compiler/kqp_query_compiler.cpp:2456)

Заполняет proto `TKqpTableSinkSettings`:
- `MODE_FILL`, `Table.Path`, `InputColumns`
- `Columns`, `KeyColumns`, `WriteIndexes` не заполняются — добавляются table resolver'ом во время выполнения

#### 1.1.4 Table Resolver: [`kqp_table_resolver.cpp`](ydb/core/kqp/executer_actor/kqp_table_resolver.cpp)

**Проход 1 — `HandleResolveNames` (Navigate by path)**:
- Навигирует temp-таблицу по `settings.GetTable().GetPath()`
- `AFL_ENSURE(settings.GetType() == MODE_FILL)` — обрабатывает только FILL
- Заполняет `stageMeta.ResolvedSinkSettings`: `TableId`, `IsOlap`, `KeyColumns`, `Columns`, `WriteIndexes`
- Создаёт `stageMeta.ShardKey = ExtractKey(tableId, keyTypes, Update)`

**Проход 2 — `HandleResolveKeys` (Navigate by TableId)**:
- `stageMeta.ColumnTableInfoPtr = entry.ColumnTableInfo`
- Резолвинг `ShardKey->Partitioning` через `TEvResolveKeySetResult`

#### 1.1.5 Executer: [`kqp_executer_impl.h`](ydb/core/kqp/executer_actor/kqp_executer_impl.h)

Executer собирает `shardIds` для резолвинга нод только по стадиям с `TableOps` (scan-источники). Для стадий без TableOps (в т.ч. FILL-sink) — ветка `else` с TODO-комментариями, без кода. Шарды temp-таблицы в `shardIds` не попадают. Если источник данных не читает никаких таблиц, `shardIds` пуст, и `TasksGraph.ResolveShards({})` вызывается сразу с пустой картой — `ShardIdToNodeId` пуст.

#### 1.1.6 `CountComputeTasks`: [`kqp_tasks_graph.cpp:4002`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp:4002)

Для sink-стейджа FILL входной канал — `TDqCnMap`:
- `inputTypeCase == kMap` → `stageType = COPY`, `partitionsCount = upstream_tasks_count`
- Upstream имеет 1 задачу → `partitionsCount = 1`
- Результат: **1 задача** FILL, выполняется на executer-ноде

#### 1.1.7 `BuildKqpStageChannels` (kMap)

`TDqCnMap` обрабатывается как стандартный **Map-канал** 1:1 между upstream-задачей и единственной задачей FILL.

#### 1.1.8 `BuildInternalSinks`: [`kqp_tasks_graph.cpp:3327`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp:3327)

Берёт `ResolvedSinkSettings`, вызывает `FillKqpTableSinkSettings`, пакует в `output.SinkSettings`:
```cpp
output.SinkSettings.ConstructInPlace();
output.SinkSettings->PackFrom(settings);
```
Поля `TargetShardIds` в proto и в коде нет.

#### 1.1.9 WriteActor: [`kqp_write_table.cpp`](ydb/core/kqp/runtime/kqp_write_table.cpp)

`TShardedWriteController::ShardAndFlushBatch`:
```cpp
void ShardAndFlushBatch(TRecordBatchPtr&& unshardedBatch, bool force) {
    for (auto [shardId, shardBatch] : Sharding->SplitByShardsToArrowBatches(...)) {
        ShardIds.insert(shardId);
        auto& unpreparedBatch = UnpreparedBatches[shardId];
        ...
        FlushUnpreparedBatch(shardId, unpreparedBatch, force);
    }
}
```
Единственный WriteActor шардирует все строки и отправляет каждый батч в соответствующий ColumnShard по сети.

#### 1.1.10 Итоговая схема в origin/main

```
Upstream ComputeActor
    ↓ TDqCnMap (1:1, COPY)
TKqpDirectWriteActor (1 задача, executer-нода)
    └── TShardedWriteController
            ├── Hash(PK) → CS[0]  (сеть)
            ├── Hash(PK) → CS[1]  (сеть)
            └── Hash(PK) → CS[N]  (сеть)
```

---

### 1.2 Другие (не CTAS) сценарии записи

out of scope текущего документа


## 2. Целевая картина

### 2.1 Архитектура

Sink Stage разбивается на **M задач** — по одной на каждую ноду (или шард), каждая пишет
только в свои шарды и выполняется на ноде этих шардов:

```
До:                                    После (Per-Shard):

ComputeActor (Stage N)                 ComputeActor (Stage N)
      ↓                                      ↓ ColumnShardHashV1 HashShuffle
┌─────────────────────┐              ┌──────────┐  ┌──────────┐  ┌──────────┐
│  WriteActor          │              │WriteActor│  │WriteActor│  │WriteActor│
│  (все шарды)         │              │ Node A   │  │ Node B   │  │ Node C   │
└─────────────────────┘              └────┬─────┘  └────┬─────┘  └────┬─────┘
      ↓ (всё по сети)                     ↓local        ↓local        ↓local
   CS[0] CS[1] CS[N]                   CS[0]CS[3]    CS[1]CS[4]    CS[2]CS[5]
```

### 2.2 Ключевые требования

1. **Точный routing**: каждая задача получает строки **только своих** шардов (тех, что
   в её `TargetShardIds`). Фильтрация в WriteActor — **неправильный** подход
   (M× сетевой трафик и избыточная работа). При Per-Node разбивке одна задача обслуживает
   все шарды данной ноды — их может быть несколько.

2. **Mechanism**: DQ-канал Transform→Sink использует `ColumnShardHashV1` HashShuffle.
   `TaskIndexByHash[bucket]` = индекс задачи, владеющей шардом bucket'а `bucket`.

3. **Совместимость hash-функций** (доказана):

   | Компонент | Реализация |
   |-----------|-----------|
   | DQ `TColumnShardHashV1` ([`dq_output_consumer.cpp:136`](ydb/library/yql/dq/runtime/dq_output_consumer.cpp:136)) | `NXX64::TStreamStringHashCalcer(seed=0)` + `Update(raw_bytes)` per column |
   | ColumnShard `TXX64::Execute()` ([`calcer.cpp:106`](ydb/core/formats/arrow/hash/calcer.cpp:106)) | `NXX64::TStreamStringHashCalcer(seed=0)` + `Update(raw_bytes)` per column |

   hash(row.pk) и bucket mapping `min(h/(Max/N), N-1)` совпадают → функции **совместимы**.

4. **Единство порядка шардов**: `CountComputeTasks`, `BuildInternalSinks` и `BuildKqpStageChannels`
   используют один порядок `GetSharding().GetColumnShards()`:

   ```
   строка → hash(pk) → bucket i → TaskIndexByHash[i] → task i → пишет только в ColumnShards[i]
   ```

5. **`AFL_VERIFY` в WriteActor**: если строка чужого шарда попала в задачу — это баг routing'а.

### 2.3 Модели shard assignment

Поддерживаются два варианта. Оба требуют точного routing'а: строки попадают
**только в задачу-владельца** нужного шарда.

**Вариант A: Per-Shard** K = N

**PRAGMA**: `PRAGMA ydb.EnableCsWriteAffinity` (по умолчанию **включено**).


```
StageShards[i] = {sᵢ}            — ровно один шард на задачу
StageNode[i]   = P(sᵢ)           — нода шарда sᵢ
TargetShardIds = {sᵢ}            — один шард
TaskIndexByHash[bucket] = i       — bucket = hash(pk) / (Max/N)
```

**Вариант B: Per-Node** (K = M, рекомендуется для минимума акторов)
```
StageShards[j] = {s ∈ S | P(s) = Nodeⱼ}  — все шарды ноды j
StageNode[j]   = Nodeⱼ
TargetShardIds = StageShards[j]            — несколько шардов!
TaskIndexByHash[bucket] = j                — bucket → нода шарда sᵢ
```

При Per-Node каждая задача обслуживает **несколько** шардов. Hash-routing должен
направлять строку в задачу, чья нода владеет целевым шардом.

---

## 3. Текущее состояние в ветке

### 3.1 Что реализовано

| № | Описание | Статус | Файл |
|---|----------|--------|------|
| 1 | PRAGMA `EnableCsWriteAffinity` → флаг в `TKqpPhyTx.EnableCsWriteAffinity` (proto) | ✅ | [`yql_kikimr_settings.h`](ydb/core/kqp/provider/yql_kikimr_settings.h), [`kqp_physical.proto`](ydb/core/protos/kqp_physical.proto) |
| 2 | `BuildFillTableEffect`: при `enableCsWriteAffinity` строятся **два** stage — Transform + отдельный Sink, соединённые `TDqCnBroadcast` | ✅ | [`kqp_opt_effects.cpp:238`](ydb/core/kqp/opt/kqp_opt_effects.cpp:238) |
| 3 | Proto-поля `TargetShardIds = 30`, `ExpectedNodeId = 31`, `CtasDestinationPath = 32` в `TKqpTableSinkSettings` | ✅ | [`kqp.proto`](ydb/core/protos/kqp.proto) |
| 4 | `FillCreateTableAs`: сохраняет `CtasDestinationPath` (путь destination) в `TKqpTableSinkSettings` | ✅ | [`kqp_query_compiler.cpp:2456`](ydb/core/kqp/query_compiler/kqp_query_compiler.cpp:2456) |
| 5 | Table resolver `HandleResolveNames`: принимает OLAP сinks (не только MODE_FILL); `ResolvedSinkSettings` заполняется для всех типов | ✅ | [`kqp_table_resolver.cpp`](ydb/core/kqp/executer_actor/kqp_table_resolver.cpp) |
| 6 | Table resolver `HandleResolveKeys`: при `EnableCsWriteAffinity` + OLAP sink заполняет `stageMeta.CsShardingColumns` и `ShardKey->Partitioning` из `ColumnTableInfo.GetColumnShards()` | ✅ | [`kqp_table_resolver.cpp:303`](ydb/core/kqp/executer_actor/kqp_table_resolver.cpp:303) |
| 7 | `kqp_executer_impl.h`: для FILL-sink стадий с `EnableCsWriteAffinity` добавляет шарды temp-таблицы в `shardIds` → они попадают в `ShardIdToNodeId` | ✅ | [`kqp_executer_impl.h:319`](ydb/core/kqp/executer_actor/kqp_executer_impl.h:319) |
| 8 | `CountComputeTasks`: при наличии OLAP sink (`GetIsOlap()`) создаёт per-shard задачи из `ColumnTableInfoPtr->GetColumnShards()`, пиннит к ноде шарда через `ShardIdToNodeId` | ✅ | [`kqp_tasks_graph.cpp:4408`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp:4408) |
| 9 | `BuildKqpStageChannels` (kBroadcast): при `CsShardingColumns` + N>1 задач строит `ColumnShardHashV1` HashShuffle вместо Broadcast | ✅ | [`kqp_tasks_graph.cpp:1472`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp:1472) |
| 10 | `BuildKqpStageChannels` (kMap): при OLAP sink с N>1 задачами аналогично заменяет Map на `ColumnShardHashV1` HashShuffle | ✅ | [`kqp_tasks_graph.cpp:1590`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp:1590) |
| 11 | `BuildInternalSinks`: при `IsOlap` + N>1 задач назначает `TargetShardIds = {shard_i}` задаче i по индексу в `GetColumnShards()` | ✅ | [`kqp_tasks_graph.cpp:3545`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp:3545) |
| 12 | `AssignTasksToNodes`: планировщик использует `ExpectedNodeId` для пиннинга задач к нодам шардов | ✅ | [`kqp_planner.cpp`](ydb/core/kqp/executer_actor/kqp_planner.cpp) |
| 13 | `ShardAndFlushBatch`: `AFL_VERIFY(TargetShardIds->contains(shardId))` — строгая валидация routing'а | ✅ | [`kqp_write_table.cpp:522`](ydb/core/kqp/runtime/kqp_write_table.cpp:522) |
| 14 | Тесты: `KqpWriteAffinity::*`, `KqpQuery::CTAS_WriteAffinity_Twin*`, `*CreateAsSelect*`, olap/operations | ✅ | [`kqp_write_affinity_ut.cpp`](ydb/core/kqp/ut/query/kqp_write_affinity_ut.cpp) |

### 3.2 Итоговая схема в ветке (CTAS с `EnableCsWriteAffinity`)

```
Upstream ComputeActor
    ↓ TDqCnMap (1:1)
Transform Stage (1 задача, любая нода)
    ↓ ColumnShardHashV1 HashShuffle (hash(PK) → task i)
Sink Stage (N задач, по одной на шард)
    WriteActor[0] на Node(CS[0]) → CS[0]  (local)
    WriteActor[1] на Node(CS[1]) → CS[1]  (local)
    ...
    WriteActor[N] на Node(CS[N]) → CS[N]  (local)
```

---

## 4. Список доработок

### 4.1 **Вариант A: Per-Shard** K = N

#### Статус: бо́льшая часть реализована. Остаются уточнения и cleanup.

---

##### 4.1.1 [БЛОКЕР] Ретрай Navigate при `ColumnTableInfoPtr == null` — баг: CRASH

**Когда `ColumnTableInfoPtr == null`:**

`ResolveKeys()` (второй вызов после `HandleResolveNames`) одновременно посылает два запроса в SchemeCache:
- `TEvNavigateKeySet` by TableId → ответ `HandleResolveKeys(TEvNavigateKeySetResult)` — ставит `ColumnTableInfoPtr + CsShardingColumns + ShardKey->Partitioning`
- `TEvResolveKeySet` → ответ `HandleResolveKeys(TEvResolveKeySetResult)` — перезаписывает `ShardKey`

Оба ответа обрабатываются в `ResolveKeysState` и могут прийти в **любом порядке**. Если Navigate by TableId вернул `entry.ColumnTableInfo == nullptr` (например, SchemeCache ещё не обновился до ColumnTableInfo для только что созданной temp-таблицы), то `ColumnTableInfoPtr` остаётся null, `CsShardingColumns` остаётся пустым.

**Что происходит при `ColumnTableInfoPtr == null`:**

| Место | Ветка | Результат |
|-------|-------|-----------|
| [`CountComputeTasks`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp:4447) | `ColumnTableInfoPtr == null`, `ShardKey` есть → `ShardKey->GetPartitions()` | N задач создаётся |
| [`BuildKqpStageChannels`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp:1486) | `CsShardingColumns.empty()` → условие fail | Остаётся **Broadcast** (не HashShuffle!) |
| [`BuildInternalSinks`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp:3588) | `ColumnTableInfoPtr == null`, `ShardKey` → `ShardKey->GetPartitions()` | `TargetShardIds = {shard_i}` по порядку ShardKey |
| WriteActor [`ShardAndFlushBatch`](ydb/core/kqp/runtime/kqp_write_table.cpp:522) | Broadcast → все строки → шардифицирует → `AFL_VERIFY(TargetShardIds->contains(shardId))` | **CRASH** для строк чужих шардов |

**Итог**: `ColumnTableInfoPtr == null` ведёт к **crash** (`AFL_VERIFY` в WriteActor) из-за несоответствия: N задач + Broadcast + `TargetShardIds`. Broadcast шлёт все строки во все задачи, но каждая задача ожидает только «свои» строки.

**Дополнительно**: даже если бы `AFL_VERIFY` не было — порядок `ShardKey->GetPartitions()` из `TEvResolveKeySetResult` **не гарантирован** для OLAP таблиц, что может привести к неверному routing'у.

**Что нужно сделать:**

Navigate by TableId для существующей колонной таблицы (`entry.Kind == KindColumnTable`) обязан возвращать заполненный `entry.ColumnTableInfo` — таблица создана, схема должна быть доступна. Если он `nullptr` — SchemeCache ещё не проgatewayed создание таблицы (гонка между CREATE TABLE и Navigate).

**Решение: ретрай Navigate в `HandleResolveKeys`**

`TKqpTableResolver` — актор в `ResolveKeysState`, который ждёт двух независимых ответов:
- `TEvNavigateKeySetResult` (Navigate by TableId) → флаг `NavigationFinished`
- `TEvResolveKeySetResult` (ResolveKeySet) → флаг `ResolvingFinished`

`TryFinish()` вызывается только когда оба флага выставлены. Если Navigate вернул `ColumnTableInfo == nullptr` для OLAP sink — не выставлять `NavigationFinished`, а послать новый `TEvNavigateKeySet` в SchemeCache и дождаться повторного ответа. Актор остаётся в `ResolveKeysState` и продолжит принимать оба типа ответов.

```cpp
// В HandleResolveKeys (TEvNavigateKeySetResult):
bool needRetry = false;
for (auto stageId : stageIds) {
    auto& stageMeta = TasksGraph.GetStageInfo(stageId).Meta;
    stageMeta.ColumnTableInfoPtr = entry.ColumnTableInfo;
    if (!entry.ColumnTableInfo
            && stageMeta.ResolvedSinkSettings
            && stageMeta.ResolvedSinkSettings->GetIsOlap()) {
        needRetry = true;
        TableRequestIds[entry.TableId].emplace_back(stageId);
    }
    ...
}
if (needRetry) {
    // Переотправить Navigate by TableId, не выставляя NavigationFinished
    auto requestNavigate = ...;
    for (auto& [tableId, stageIds] : TableRequestIds) { ... }
    Send(MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvNavigateKeySet(...));
    return; // NavigationFinished остаётся false
}
NavigationFinished = true;
TryFinish();
```

**Ограничения**:
- Нужен счётчик ретраев + таймаут — если SchemeCache никогда не вернёт `ColumnTableInfo`, запрос должен завершиться с ошибкой через N попыток
- `TableRequestIds` нужно восстановить перед ретраем (он был `erase`-нут при первой обработке)

Дополнительно: убрать fallback-ветку `else if (ShardKey)` в `CountComputeTasks` и `BuildInternalSinks` для OLAP sinks — она не должна достигаться после надёжного получения `ColumnTableInfo`.

**Статус**: ⬜ Не реализовано

---

##### 4.1.2 [БЛОКЕР] Починить write stats — двойной счёт при per-shard задачах

**Статус**: ⬜ Workaround, требует нормального фикса

В [`kqp_cost_ut.cpp`](ydb/core/kqp/ut/cost/kqp_cost_ut.cpp) строгие ассерты на количество строк/байт в write stats заменены на `UNIT_ASSERT_GE + rows % N == 0`. Причина: при per-shard задачах каждая задача сообщает свою статистику, итого rows умножается на число задач. Это **побочный эффект**, а не ожидаемое поведение.

**Действия**:
- Выяснить, где агрегируется write stats (executer или session actor)
- Дедуплицировать/суммировать корректно: итоговое число строк должно быть равно реальному числу записанных строк, независимо от числа задач
- Вернуть строгие ассерты в тестах

---

##### 4.1.3 [БЛОКЕР] Разобраться с изменением в `kqp_session_actor.cpp`

**Статус**: ⬜ Требует проверки

В [`kqp_session_actor.cpp:1147`](ydb/core/kqp/session_actor/kqp_session_actor.cpp:1147) добавлено:
```cpp
if (stageInfo.Meta.ColumnTableInfoPtr && ...) {
    for (const auto& shardId : ...GetColumnShards())
        shardIds.insert(shardId);
}
```
Это добавление OLAP shard'ов в `shardIds` в контексте session actor'а (вероятно, для `TxLocksCleanup` или commit). Нужно выяснить, почему оно нужно, и не дублирует ли оно логику из [`kqp_executer_impl.h`](ydb/core/kqp/executer_actor/kqp_executer_impl.h).

**Действия**:
- Установить, зачем session actor собирает shard'ы OLAP-таблиц
- Убедиться, что это изменение не является побочным эффектом, который нужно убрать

---

##### 4.1.4 CTAS без `EnablePerStatementQueryExecution`

**Статус**: ⬜ Не исследовано

Без флага CTAS компилируется иначе (не через `TKqlFillTable`/FILL). Нужно выяснить, применяется ли тот же путь или требуется отдельная обработка.

**Действия**:
- Проверить, как компилируется CTAS при `EnablePerStatementQueryExecution=false`
- Определить, попадает ли plan в `BuildFillTableEffect` или идёт другим путём
- При необходимости применить аналогичный подход

---

##### 4.1.5 Удалить `CtasDestinationPath` если не используется

**Статус**: ⬜ Требует проверки

Поле `CtasDestinationPath = 32` добавлено в proto и заполняется в [`FillCreateTableAs`](ydb/core/kqp/query_compiler/kqp_query_compiler.cpp:2456). Table resolver получает sharding-информацию из temp-таблицы (через `ColumnTableInfo` по её `TableId`), а не из destination-таблицы. Если `CtasDestinationPath` нигде не читается — поле можно удалить.

**Действия**:
- Поиск всех мест использования `CtasDestinationPath` в runtime
- Удалить поле из proto и компилятора, если оно не нужно

---

##### 4.1.6 Удалить временные debug-флаги из `ya.make`

**Статус**: ⬜ Требует удаления перед мержем

В [`ya.make`](ydb/core/kqp/runtime/ya.make) добавлены временные флаги компиляции:
```
-DKQP_WRITE_TABLE_TARGET_SHARD_IDS_CHECK
-DKQP_WRITE_TABLE_TARGET_SHARD_IDS_EXPECTED_COUNT=1
```
и соответствующий `#ifdef`-блок в [`kqp_write_table.cpp:454`](ydb/core/kqp/runtime/kqp_write_table.cpp:454), который проверяет `TargetShardIds->size() == 1`. Эти флаги хардкодят Per-Shard (K=N) и сломают Per-Node (K=M), где `TargetShardIds.size() > 1`. Перед мержем удалить оба флага из `ya.make` и `#ifdef`-блок из `kqp_write_table.cpp`.

---

##### 4.1.7 Бенчмарк

**Статус**: ⬜ Не реализовано

Измерить: сетевой трафик, время выполнения, пиковое потребление памяти — affinity vs baseline (1 задача).

### 4.2 **Вариант B: Per-Node** K = M

> Задачи для этапа B будут уточнены после завершения и стабилизации этапа A (Per-Shard).

Общая идея: сгруппировать шарды по нодам (`P: Shard → Node`), создать M=|{Node}| задач вместо N шардов. Каждая задача обслуживает `{s | P(s) = Nodeⱼ}` шардов. `TargetShardIds` содержит несколько шардов. `ColumnShardHashV1` routing: `TaskIndexByHash[bucket] = j` (нода, не шард).