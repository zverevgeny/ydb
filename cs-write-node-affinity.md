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

| Этап | Описание | Статус | Файлы |
|------|----------|--------|-------|
| **1** | PRAGMA `EnableCsWriteAffinity` + proto-поле `TKqpPhyTx.EnableCsWriteAffinity` | ✅ | [`yql_kikimr_settings.h:127`](ydb/core/kqp/provider/yql_kikimr_settings.h:127), [`kqp_physical.proto`](ydb/core/protos/kqp_physical.proto:745) |
| **2** | Отдельный Sink Stage (Transform→Sink через `TDqCnBroadcast`) | ✅ | [`kqp_opt_effects.cpp:162`](ydb/core/kqp/opt/kqp_opt_effects.cpp:162) |
| **3** | Поля `TargetShardIds = 30`, `ExpectedNodeId = 31`, `CtasDestinationPath = 32` в proto | ✅ | [`kqp.proto:929`](ydb/core/protos/kqp.proto:929) |
| **4** | Table resolver запрашивает `ColumnTableInfo` целевой таблицы для OLAP sink-стейджей | ✅ | [`kqp_table_resolver.cpp`](ydb/core/kqp/executer_actor/kqp_table_resolver.cpp) |
| **5** | `CsShardingColumns` и `ColumnTableInfoPtr` sink-стейджа заполняются из целевой таблицы | ✅ | [`kqp_table_resolver.cpp:280`](ydb/core/kqp/executer_actor/kqp_table_resolver.cpp) |
| **6** | Per-shard задачи для non-CTAS OLAP сinks (`ResolvedSinkSettings != null`) | ✅ | [`kqp_tasks_graph.cpp:4473`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp) |
| **7** | `TargetShardIds` назначаются только для multi-task стейджей | ✅ | [`kqp_tasks_graph.cpp:3628`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp) |
| **8** | `ColumnShardHashV1` routing для non-CTAS OLAP (kBroadcast/kMap → HashShuffle) | ✅ | [`kqp_tasks_graph.cpp:1492`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp) |
| **8'** | CTAS FILL: 1 задача, Broadcast → одна задача пишет все шарды (без фильтрации) | 🔶 workaround | [`kqp_tasks_graph.cpp:4473`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp) |
| **9** | Планировщик `AssignTasksToNodes()` использует `ExpectedNodeId` | ✅ (без изменений) | [`kqp_planner.cpp:346`](ydb/core/kqp/executer_actor/kqp_planner.cpp) |
| **10** | Тесты | ✅ | [`kqp_query_ut.cpp`](ydb/core/kqp/ut/query/kqp_query_ut.cpp), [`kqp_write_affinity_ut.cpp`](ydb/core/kqp/ut/query/kqp_write_affinity_ut.cpp) |

### 3.2 Результаты тестов

```bash
KqpQuery::CTAS_WriteAffinity_Twin*   — 2/2  GOOD
KqpWriteAffinity::*                  — 5/5  GOOD
*CreateAsSelect*                     — 17/17 GOOD
ydb/core/kqp/ut/olap/operations/     — 13/13 GOOD
```

### 3.3 Архитектура CTAS в ветке (workaround)

С `EnablePerStatementQueryExecution=true`, CTAS компилируется как 3 отдельных стейтмента:
1. **CREATE TABLE** → создаёт temp-таблицу `/.tmp/sessions/.../Destination_uuid`
2. **FILL** → записывает данные в temp-таблицу (1 задача, все шарды)
3. **MOVE** → переименовывает temp-таблицу в `/Root/Destination`

Sink-стейдж FILL имеет `ResolvedSinkSettings != null` (temp-таблица резолвится), но создаётся
**одна задача** (не per-shard), поскольку `ColumnTableInfoPtr` для CTAS FILL на момент
`CountComputeTasks` может быть нечётко привязана к нодам шардов. `TargetShardIds` для
одной задачи не назначаются — задача пишет все шарды сразу.

Текущее состояние `ShardAndFlushBatch` в [`kqp_write_table.cpp:519`](ydb/core/kqp/runtime/kqp_write_table.cpp:519):
```cpp
// workaround: continue вместо AFL_VERIFY для одной задачи
if (TargetShardIds.has_value() && !TargetShardIds->contains(shardId)) {
    continue;  // временно; должен быть AFL_VERIFY при правильном routing
}
```

---

## 4. Список доработок

Реализация разбита на два этапа:
- **Этап 1 (Per-Shard)**: каждая задача обслуживает ровно один шард, пинится к его ноде.
  Задачи определены ниже.
- **Этап 2 (Per-Node)**: задачи группируются по нодам (несколько шардов на задачу),
  снижает количество акторов с N до M. Задачи будут уточнены позже.

---

### Этап 1: Per-Shard routing

#### Задача 1.1: Per-shard задачи для CTAS через temp-таблицу

**Статус**: ⬜ Не реализовано
**Проблема**: Для CTAS FILL создаётся 1 задача вместо N (по числу шардов temp-таблицы),
т.к. шарды temp-таблицы могут не быть в `ShardIdToNodeId` на момент `CountComputeTasks`.

**Что нужно сделать**:
1. В [`kqp_executer_impl.h`](ydb/core/kqp/executer_actor/kqp_executer_impl.h) добавить
   шарды MODE_FILL temp-таблицы в `shardIds` для резолвинга нод (по аналогии с строками 319–338).
2. В [`CountComputeTasks`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp:4473) разрешить
   per-shard задачи для CTAS FILL (`ResolvedSinkSettings != null`), когда шарды temp-таблицы
   присутствуют в `ShardIdToNodeId`.
3. В [`BuildInternalSinks`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp:3628) назначать
   `TargetShardIds = {sᵢ}` для каждой задачи.
4. Убрать workaround: вернуть `AFL_VERIFY` вместо `continue` в
   [`ShardAndFlushBatch`](ydb/core/kqp/runtime/kqp_write_table.cpp:519).

---

#### Задача 1.2: ColumnShardHashV1 routing для CTAS Transform→Sink

**Статус**: ⬜ Не реализовано (зависит от 1.1)
**Проблема**: После появления N per-shard задач для CTAS нужно настроить `ColumnShardHashV1`
HashShuffle на канале Transform→Sink, чтобы каждая строка попадала ровно в одну задачу.

**Что нужно сделать**:
1. В [`BuildKqpStageChannels`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp:1492) для
   CTAS FILL sink-стейджа с N>1 задачами: заменить `TDqCnBroadcast` на `ColumnShardHashV1`
   HashShuffle (как уже сделано для INSERT).
2. Убедиться, что `TaskIndexByHash[bucket_i] = i` совпадает с `TargetShardIds[i]`
   (порядок `GetColumnShards()` одинаков в обоих местах).

---

#### Задача 1.3: CTAS без EnablePerStatementQueryExecution

**Статус**: ⬜ Не исследовано
**Проблема**: Без `EnablePerStatementQueryExecution` CTAS может компилироваться иначе
(без temp-таблицы). Нужно исследовать, доступны ли шарды целевой таблицы через
`ShardIdToNodeId` в этом сценарии, и применить аналогичный подход.

---

#### Задача 1.4: Удалить временные workaround'ы

**Статус**: ⬜ Выполнять после 1.1–1.2
**Что нужно сделать**:
- Удалить поле `CtasDestinationPath = 32` из [`kqp.proto`](ydb/core/protos/kqp.proto:929)
  если оно больше не нужно.
- Удалить условие `stageInfo.Meta.ResolvedSinkSettings` из `CountComputeTasks` (перейти
  к явной проверке типа операции).

---

#### Задача 1.5: Бенчмарк и тесты корректности

**Статус**: ⬜ Не реализовано
**Что нужно сделать**:
- Тест: каждый WriteActor получает строки только своих шардов (нет пересечений).
- Тест: split/merge шардов во время записи — корректный fallback.
- Бенчмарк: трафик affinity vs baseline (ожидается снижение в N раз).

---

### Этап 2: Per-Node routing

> Задачи для этапа 2 будут уточнены после завершения этапа 1.

Общая идея: сгруппировать шарды по нодам (`P: Shard → Node`), создать M=|{Node}| задач
вместо N шардов. Каждая задача обслуживает `{s | P(s) = Nodeⱼ}` шардов.
`TargetShardIds` содержит несколько шардов. `ColumnShardHashV1` routing строится
как `TaskIndexByHash[bucket] = j` (нода, а не шард).

---

## 5. Ключевые файлы

| Файл | Роль |
|------|------|
| [`yql_kikimr_settings.h:127`](ydb/core/kqp/provider/yql_kikimr_settings.h:127) | Настройка `EnableCsWriteAffinity` |
| [`kqp_physical.proto:745`](ydb/core/protos/kqp_physical.proto:745) | Поле `TKqpPhyTx.EnableCsWriteAffinity` |
| [`kqp.proto:929`](ydb/core/protos/kqp.proto:929) | `TargetShardIds`, `ExpectedNodeId`, `CtasDestinationPath` |
| [`kqp_query_compiler.cpp:2456`](ydb/core/kqp/query_compiler/kqp_query_compiler.cpp:2456) | `FillCreateTableAs` — сохраняет `CtasDestinationPath` |
| [`kqp_opt_effects.cpp:162`](ydb/core/kqp/opt/kqp_opt_effects.cpp:162) | `BuildFillTableEffect()` — разделение stage, `TDqCnBroadcast` |
| [`kqp_table_resolver.cpp`](ydb/core/kqp/executer_actor/kqp_table_resolver.cpp) | Резолвинг целевой таблицы, `CsShardingColumns`, `ColumnTableInfoPtr` |
| [`kqp_tasks_graph.cpp:4473`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp) | `CountComputeTasks` — per-shard задачи для non-CTAS |
| [`kqp_tasks_graph.cpp:3628`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp) | `BuildInternalSinks` — `TargetShardIds` для multi-task |
| [`kqp_tasks_graph.cpp:1492`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp) | `BuildKqpStageChannels` — `ColumnShardHashV1` HashShuffle |
| [`kqp_planner.cpp:346`](ydb/core/kqp/executer_actor/kqp_planner.cpp) | `AssignTasksToNodes` — планирование с affinity |
| [`kqp_write_table.cpp:519`](ydb/core/kqp/runtime/kqp_write_table.cpp) | `ShardAndFlushBatch` — фильтрация/валидация шардов |
| [`dq_output_consumer.cpp:136`](ydb/library/yql/dq/runtime/dq_output_consumer.cpp) | `TColumnShardHashV1` — DQ hash routing |
| [`calcer.cpp:106`](ydb/core/formats/arrow/hash/calcer.cpp) | `TXX64::Execute` — ColumnShard hash (совместим с DQ) |

---

## 6. Тесты

```bash
# TWIN-тест (обе ветки прагмы)
./ya make --build relwithdebinfo -tA ydb/core/kqp/ut/query -F 'KqpQuery::CTAS_WriteAffinity_Twin*'

# Affinity suite
./ya make --build relwithdebinfo -tA ydb/core/kqp/ut/query -F 'KqpWriteAffinity::*'

# CreateAsSelect регрессия
./ya make --build relwithdebinfo -tA ydb/core/kqp/ut/query -F '*CreateAsSelect*'

# OLAP write интеграция
./ya make --build relwithdebinfo -tA ydb/core/kqp/ut/olap/operations
```

| Тест | Что проверяет |
|------|---------------|
| `CTAS_WriteAffinity_Twin` | 3 stage без / 4 stage с прагмой; Broadcast+Sink в плане; идентичность данных |
| `KqpWriteAffinity::CTAS_WriteAffinity_LargeData` | CTAS 100 строк, несколько flush'ей |
| `KqpWriteAffinity::CTAS_WriteAffinity_MultiNode` | Кластер из 3 нод; per-shard задачи |
| `KqpWriteAffinity::CTAS_WriteAffinity_JoinSource` | CTAS с JOIN |
| `KqpWriteAffinity::CTAS_WriteAffinity_EmptySource` | Пустой источник |
| `KqpWriteAffinity::CTAS_WriteAffinity_CompositeKey` | Composite primary key |
