# ColumnShard Write Node Affinity


---

## 1. Формулировка проблемы.

При записи в колонную таблицу при CTAS / FILL / INSERT создаётся **один** WriteActor
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

### 1.1 Цепочка преобразований плана выполнения запроса

Запрос на запись в колоночную таблицу проходит несколько стадий преобразования — от SQL до исполнения. Ниже описана полная цепочка для CTAS (FILL) и INSERT.

#### 1.1.1 Общая схема

```
SQL (CREATE TABLE AS / INSERT / REPLACE)
   │
   ▼
[1] Парсинг + построение AST (YQL)
   │
   ▼
[2] Логический план (TKqlFillTable / TKqlWriteTable)
   │
   ▼
[3] Оптимизация (KQP optimizer) → физический план (TDqStage / TDqSink)
   │
   ▼
[4] Компиляция (KqpQueryCompiler) → TKqpPhyTx (proto)
   │
   ▼
[5] Исполнение (KqpExecuter) → TasksGraph → ComputeActor / WriteActor
```

#### 1.1.2 Стадия 1 — Парсинг и AST

SQL-запрос парсится в AST YQL. Для записи формируются узлы:
- **CTAS** → `TKqlFillTable` (с `OriginalPath` = путь destination)
- **INSERT/REPLACE** → `TKqlWriteTable`

#### 1.1.3 Стадия 2 — Логический план

Логический план содержит узел записи с описанием целевой таблицы и источника данных. На этом этапе нет информации о шардировании — только логическая структура.

#### 1.1.4 Стадия 3 — Оптимизация (KQP optimizer)

[`BuildFillTableEffect`](ydb/core/kqp/opt/kqp_opt_effects.cpp:162) преобразует логический узел в физический план:

**origin/main (без `EnableCsWriteAffinity`)**:
- Один `TDqStage`: вход `TDqCnMap` из upstream, программа `ToFlow(row)`, выход `TDqSink`
- Sink settings: `MODE_FILL`, `Table.Path`, `InputColumns`, `InconsistentWrite=true`, `StreamWrite=true`

**Ветка (с `EnableCsWriteAffinity`)**:
- Два `TDqStage`: Transform (вход `TDqCnMap`, программа `ToFlow`) + Sink (вход `TDqCnBroadcast`, выход `TDqSink`)
- `TDqCnBroadcast` позволяет Sink-стадии иметь независимое число задач (M) от Transform (1)

#### 1.1.5 Стадия 4 — Компиляция (KqpQueryCompiler)

[`FillCreateTableAs`](ydb/core/kqp/query_compiler/kqp_query_compiler.cpp:2456) заполняет proto `TKqpTableSinkSettings`:
- `MODE_FILL`, `Table.Path`, `InputColumns`
- `CtasDestinationPath` (путь destination) — в ветке
- `Columns`, `KeyColumns`, `WriteIndexes` не заполняются — добавляются table resolver'ом во время выполнения

Результат — `TKqpPhyTx` (proto), содержащий стадии, соединения и sink settings.

#### 1.1.6 Стадия 5 — Исполнение (KqpExecuter)

Executer строит `TKqpTasksGraph` из `TKqpPhyTx`:

1. **`FillStages`** — создаёт `TStageInfo` для каждой стадии, заполняет `TablePath`, `TableId`, `ShardOperations` из sink settings
2. **Table Resolver** — резолвит таблицу по пути/TableId, заполняет `ResolvedSinkSettings`, `ColumnTableInfoPtr`, `ShardKey`
3. **`CountComputeTasks`** — определяет число задач на стадию (в origin/main: 1 задача для sink)
4. **`BuildKqpStageChannels`** — строит каналы между задачами (Map/Broadcast/HashShuffle)
5. **`BuildInternalSinks`** — сериализует sink settings в task output
6. **`AssignTasksToNodes`** — планировщик назначает задачи нодам
7. **ComputeActor / WriteActor** — исполнение: WriteActor шардифицирует строки и отправляет в ColumnShards

#### 1.1.7 Ключевые точки, где в ветке вносятся изменения

| Стадия | Функция | Изменение в ветке |
|--------|---------|-------------------|
| 3 (оптимизация) | `BuildFillTableEffect` | Разделение Transform/Sink на два stage + Broadcast |
| 4 (компиляция) | `FillCreateTableAs` | Сохранение `CtasDestinationPath` |
| 5 (исполнение) | Table Resolver | Заполнение `CsShardingColumns` |
| 5 (исполнение) | `CountComputeTasks` | Per-shard задачи (M=N) |
| 5 (исполнение) | `BuildKqpStageChannels` | ColumnShardHashV1 HashShuffle вместо Broadcast/Map |
| 5 (исполнение) | `BuildInternalSinks` | Заполнение `TargetShardIds` |
| 5 (исполнение) | WriteActor | `AFL_VERIFY(TargetShardIds->contains(shardId))` |

---

### 1.2 Текущая(origin/main) реализация записи в колоночные шарды

Ниже описано поведение каждой изменённой в ветке функции в origin/main.

#### 1.2.1 Оптимизатор: [`BuildFillTableEffect`](ydb/core/kqp/opt/kqp_opt_effects.cpp:162)

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

**Гарантии**: один stage, один sink, Map-канал из upstream.
**Ограничения**: нет разделения Transform и Sink на разные stage; нет Broadcast-канала.

#### 1.2.2 Компилятор: [`FillCreateTableAs`](ydb/core/kqp/query_compiler/kqp_query_compiler.cpp:2456)

Заполняет proto `TKqpTableSinkSettings`:
- `MODE_FILL`, `Table.Path`, `InputColumns`
- `Columns`, `KeyColumns`, `WriteIndexes` не заполняются — добавляются table resolver'ом во время выполнения

**Гарантии**: базовые настройки sink заполнены.
**Ограничения**: схема таблицы (columns, key columns) отсутствует на этапе компиляции.

#### 1.2.3 Table Resolver: [`kqp_table_resolver.cpp`](ydb/core/kqp/executer_actor/kqp_table_resolver.cpp)

**Проход 1 — `HandleResolveNames` (Navigate by path)**:
- Навигирует temp-таблицу по `settings.GetTable().GetPath()`
- `AFL_ENSURE(settings.GetType() == MODE_FILL)` — обрабатывает **только** MODE_FILL (не INSERT и др.)
- Заполняет `stageMeta.ResolvedSinkSettings`: `TableId`, `IsOlap`, `KeyColumns`, `Columns`, `WriteIndexes`
- Создаёт `stageMeta.ShardKey = ExtractKey(tableId, keyTypes, Update)`
- `CsShardingColumns` не заполняется

**Проход 2 — `HandleResolveKeys` (Navigate by TableId)**:
- `stageMeta.ColumnTableInfoPtr = entry.ColumnTableInfo`
- Резолвинг `ShardKey->Partitioning` через `TEvResolveKeySetResult`

**Гарантии**: `ResolvedSinkSettings` и `ColumnTableInfoPtr` заполнены для MODE_FILL.
**Ограничения**: `CsShardingColumns` не заполняется — нет информации о sharding-колоночках для routing'а. Обрабатывается только MODE_FILL, не INSERT.

#### 1.2.4 Executer: [`kqp_executer_impl.h`](ydb/core/kqp/executer_actor/kqp_executer_impl.h)

Executer собирает `shardIds` для резолвинга нод только по стадиям с `TableOps` (scan-источники). Для стадий без TableOps (в т.ч. FILL-sink) — ветка `else` с TODO-комментариями, без кода. Шарды temp-таблицы в `shardIds` не попадают. Если источник данных не читает никаких таблиц, `shardIds` пуст, и `TasksGraph.ResolveShards({})` вызывается сразу с пустой картой — `ShardIdToNodeId` пуст.

**Гарантии**: шарды source-таблиц резолвятся.
**Ограничения**: шарды destination (temp) таблицы не резолвятся — `ShardIdToNodeId` не содержит нод destination-шардов.

#### 1.2.5 `CountComputeTasks`: [`kqp_tasks_graph.cpp`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp)

Для sink-стейджа FILL входной канал — `TDqCnMap`:
- `inputTypeCase == kMap` → `stageType = COPY`, `partitionsCount = upstream_tasks_count`
- Upstream имеет 1 задачу → `partitionsCount = 1`
- Результат: **1 задача** FILL, выполняется на executer-ноде

**Гарантии**: одна задача на sink stage.
**Ограничения**: нет per-shard задач; нет node affinity; задача выполняется на executer-ноде.

#### 1.2.6 `BuildKqpStageChannels`: [`kqp_tasks_graph.cpp`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp)

`TDqCnMap` обрабатывается как стандартный **Map-канал** 1:1 между upstream-задачей и единственной задачей FILL.

`TDqCnBroadcast` вызывает `BuildBroadcastChannels` — все строки отправляются всем задачам. Специальной логики для OLAP sink нет.

**Гарантии**: данные доходят до sink задачи.
**Ограничения**: нет ColumnShardHashV1 routing'а; Broadcast шлёт все данные всем задачам.

#### 1.2.7 `BuildInternalSinks`: [`kqp_tasks_graph.cpp`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp)

Берёт `ResolvedSinkSettings`, вызывает `FillKqpTableSinkSettings`, пакует в `output.SinkSettings`:
```cpp
output.SinkSettings.ConstructInPlace();
output.SinkSettings->PackFrom(settings);
```
Поля `TargetShardIds` в proto и в коде нет.

**Гарантии**: sink settings сериализованы в task output.
**Ограничения**: нет `TargetShardIds` — WriteActor не знает, какие шарды ему принадлежат.

#### 1.2.8 WriteActor: [`kqp_write_table.cpp`](ydb/core/kqp/runtime/kqp_write_table.cpp)

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
Единственный WriteActor шардирует все строки и отправляет каждый батч в соответствующий ColumnShard по сети. `TargetShardIds` отсутствует — фильтрации по шардам нет.

**Гарантии**: все строки записаны в правильные шарды.
**Ограничения**: один актор, все данные по сети, нет node affinity.

#### 1.2.9 Декомпозиция CTAS на стейтменты

С `EnablePerStatementQueryExecution=true` CTAS компилируется как три независимых стейтмента:

1. **CREATE TABLE** — создаёт temp-таблицу `/.tmp/sessions/.../Destination_uuid`
2. **FILL** — записывает данные в temp-таблицу (стейтмент с sink mode `MODE_FILL`)
3. **MOVE** — атомарно переименовывает temp-таблицу в `/Root/Destination`

Именно стейтмент **FILL** является объектом оптимизации.

#### 1.2.10 Итоговая схема в origin/main

```
Upstream ComputeActor
    ↓ TDqCnMap (1:1, COPY)
TKqpDirectWriteActor (1 задача, executer-нода)
    └── TShardedWriteController
            ├── Hash(PK) → CS[0]  (сеть)
            ├── Hash(PK) → CS[1]  (сеть)
            └── Hash(PK) → CS[N]  (сеть)
```

**Резюме origin/main**:
| Функция | Поведение | Гарантия | Ограничение |
|---------|-----------|----------|-------------|
| `BuildFillTableEffect` | Один stage с Map-каналом | Sink создан | Нет разделения Transform/Sink |
| `FillCreateTableAs` | Базовые sink settings | Path, columns | Нет key columns |
| Table Resolver | ResolvedSinkSettings + ColumnTableInfo | Схема резолвлена | `CsShardingColumns` пусто, только MODE_FILL |
| Executer | ShardIds только из TableOps | Source shards резолвлены | Destination shards не резолвлены |
| `CountComputeTasks` | 1 задача (COPY от Map) | Задача создана | Нет per-shard задач, нет affinity |
| `BuildKqpStageChannels` | Map 1:1 или Broadcast | Данные доходят | Нет ColumnShardHashV1 routing'а |
| `BuildInternalSinks` | PackFrom(settings) | Settings сериализованы | Нет TargetShardIds |
| WriteActor | Шардифицирует все строки | Все строки записаны | Один актор, всё по сети |

---

### 1.3 Другие (не CTAS) сценарии записи в origin/main

**INSERT/REPLACE INTO** в колоночные таблицы идёт тем же путём:
- Table Resolver резолвит таблицу по TableId (есть в sink settings)
- `ResolvedSinkSettings` заполняется аналогично FILL
- `CountComputeTasks` создаёт 1 задачу
- WriteActor шардифицирует и отправляет все данные по сети

Отличие от CTAS: у INSERT есть `TableConstInfo` из компиляции (таблица существует до выполнения), тогда как у CTAS FILL temp-таблица создаётся во время выполнения и `TableConstInfo` отсутствует — используется `ColumnTableInfo` из SchemeCache.

**PRAGMA `EnableCsWriteAffinity`** в origin/main отсутствует.


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

### 2.4 Функции, которые нужно изменить

Ниже для каждой функции, изменяемой в ветке, описаны: текущее поведение (origin/main),
новая задача, которую она получает, и гарантии, которые она должна давать после изменения.

#### 2.4.1 [`BuildFillTableEffect`](ydb/core/kqp/opt/kqp_opt_effects.cpp:162) — оптимизатор

**Текущее (origin/main)**: строит один `TDqStage` (Transform + Sink вместе), вход `TDqCnMap`.

**Новая задача**: при `EnableCsWriteAffinity` разделить на два stage — Transform (вход `TDqCnMap`, программа `ToFlow`) и Sink (вход `TDqCnBroadcast`, выход `TDqSink`). Broadcast позволяет Sink-стадии иметь независимое число задач (M) от Transform (1).

**Гарантии**:
- Sink-стадия может быть разбита на M задач независимо от числа задач Transform
- Broadcast-канал доставляет все строки во все Sink-задачи (дальнейший точный routing — задача `BuildKqpStageChannels`)

#### 2.4.2 [`FillCreateTableAs`](ydb/core/kqp/query_compiler/kqp_query_compiler.cpp:2456) — компилятор

**Текущее (origin/main)**: заполняет `MODE_FILL`, `Table.Path`, `InputColumns`.

**Новая задача**: сохранить `CtasDestinationPath` (путь destination) в `TKqpTableSinkSettings`, чтобы table resolver мог навигировать правильную (destination) таблицу для per-shard affinity.

**Гарантии**:
- Table resolver получает путь destination-таблицы, а не source
- `CtasDestinationPath` доступен в runtime (в `TKqpTableSinkSettings`)

#### 2.4.3 Table Resolver: [`kqp_table_resolver.cpp`](ydb/core/kqp/executer_actor/kqp_table_resolver.cpp)

**Текущее (origin/main)**: `HandleResolveNames` обрабатывает только `MODE_FILL`; `CsShardingColumns` не заполняется.

**Новая задача**:
- `HandleResolveNames`: принимать OLAP sinks всех типов (не только MODE_FILL), заполнять `ResolvedSinkSettings`
- `HandleResolveKeys`: при `EnableCsWriteAffinity` + OLAP sink заполнять `stageMeta.CsShardingColumns` и `ShardKey->Partitioning` из `ColumnTableInfo.GetColumnShards()`

**Гарантии**:
- `CsShardingColumns` заполнен для OLAP sink с affinity — это обязательное условие для ColumnShardHashV1 routing'а
- `ShardKey->Partitioning` заполнен в порядке `GetColumnShards()` (канонический порядок bucket'ов)

#### 2.4.4 Executer: [`kqp_executer_impl.h`](ydb/core/kqp/executer_actor/kqp_executer_impl.h)

**Текущее (origin/main)**: `shardIds` собираются только из стадий с `TableOps` (scan-источники). Шарды destination-таблицы не резолвятся.

**Новая задача**: для FILL-sink стадий с `EnableCsWriteAffinity` добавлять шарды temp-таблицы в `shardIds`, чтобы они попали в `ShardIdToNodeId`.

**Гарантии**:
- `ShardIdToNodeId` содержит ноды destination-шардов → `CountComputeTasks` может пиннить задачи к нодам шардов
- Node affinity достижим (задача выполняется на ноде своего шарда)

#### 2.4.5 [`CountComputeTasks`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp) — число задач

**Текущее (origin/main)**: 1 задача на sink stage (COPY от Map).

**Новая задача**: при OLAP sink (`GetIsOlap()`) создавать per-shard задачи из `ColumnTableInfoPtr->GetColumnShards()`, пиннить к ноде шарда через `ShardIdToNodeId`.

**Гарантии**:
- Число задач = числу шардов (Per-Shard, K=N)
- Каждая задача пиннится к ноде своего шарда (через `ExpectedNodeId`)
- Порядок задач совпадает с порядком `GetColumnShards()` — критично для `TaskIndexByHash`

#### 2.4.6 [`BuildKqpStageChannels`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp) — каналы

**Текущее (origin/main)**: `TDqCnMap` → Map 1:1; `TDqCnBroadcast` → Broadcast всем.

**Новая задача**: при `CsShardingColumns` + N>1 задач строить `ColumnShardHashV1` HashShuffle вместо Broadcast/Map. `TaskIndexByHash[bucket]` = индекс задачи, владеющей шардом bucket'а.

**Гарантии**:
- Каждая строка направляется ровно в одну задачу — владельца целевого шарда
- `TaskIndexByHash` построен по `GetColumnShards()` (канонический порядок), совпадает с порядком задач из `CountComputeTasks`
- Нет M× сетевого трафика (в отличие от Broadcast)

#### 2.4.7 [`BuildInternalSinks`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp) — sink settings

**Текущее (origin/main)**: `PackFrom(settings)` без `TargetShardIds`.

**Новая задача**: при `IsOlap` + N>1 задач назначать `TargetShardIds = {shard_i}` задаче i по индексу в `GetColumnShards()`.

**Гарантии**:
- Каждая задача знает, какие шарды ей принадлежат (`TargetShardIds`)
- Порядок `TargetShardIds` совпадает с порядком задач из `CountComputeTasks`

#### 2.4.8 WriteActor: [`kqp_write_table.cpp`](ydb/core/kqp/runtime/kqp_write_table.cpp)

**Текущее (origin/main)**: `ShardAndFlushBatch` шардифицирует все строки, `TargetShardIds` отсутствует.

**Новая задача**: `AFL_VERIFY(TargetShardIds->contains(shardId))` — строгая валидация, что строка принадлежит задаче.

**Гарантии**:
- Если строка чужого шарда попала в задачу — это баг routing'а, и он детектируется (crash)
- Корректность записи: каждая задача пишет только в свои шарды

#### 2.4.9 Сводная таблица изменений

| Функция | Новая задача | Гарантия |
|---------|--------------|----------|
| `BuildFillTableEffect` | Разделить Transform/Sink на два stage + Broadcast | Sink-стадия независимо параллелизуется |
| `FillCreateTableAs` | Сохранить `CtasDestinationPath` | Resolver навигирует destination-таблицу |
| Table Resolver | Заполнить `CsShardingColumns` | Обязательное условие ColumnShardHashV1 |
| Executer | Добавить destination-шарды в `ShardIdToNodeId` | Node affinity достижим |
| `CountComputeTasks` | Per-shard задачи (K=N) | Задача на шард, пиннинг к ноде |
| `BuildKqpStageChannels` | ColumnShardHashV1 HashShuffle | Точный routing, нет M× трафика |
| `BuildInternalSinks` | Заполнить `TargetShardIds` | Задача знает свои шарды |
| WriteActor | `AFL_VERIFY` routing'а | Детекция багов routing'а |

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

### 3.2 Текущий статус: краш на INSERT-фазе

**Краш подтверждён** в тесте `KqpQuery::CTAS_WriteAffinity_Twin+EnableCsWriteAffinity`, но **не на CTAS, а на INSERT-фазе** (заполнение source-таблицы).

**Диагностика через AFL_VERIFY** (добавлен в [`CountComputeTasks`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp:4469)):
```
VERIFY failed: verification=!stageInfo.Meta.CsShardingColumns.empty();
stageId=[0,0]; shardNodesCount=8; isOlapSink=1;
hasColumnTableInfo=1; hasShardKey=1; shardsResolved=1;
```

**Корневая причина**: `CountComputeTasks` создаёт per-shard задачи для INSERT-стадии (8 задач на 8 шардов), но `CsShardingColumns` **пуст** для INSERT. Из-за этого `BuildKqpStageChannels` не может построить `ColumnShardHashV1` HashShuffle и **откатывается на Broadcast**. Broadcast шлёт все строки во все задачи, но каждая задача ожидает только «свои» шарды (`TargetShardIds`), что приводит к `AFL_VERIFY`-крашу в WriteActor.

**Почему `CsShardingColumns` пуст для INSERT**: table resolver заполняет `CsShardingColumns` только для CTAS FILL (через `ColumnTableInfo.GetSharding().GetHashSharding().GetColumns()`), но для обычного INSERT этот путь не срабатывает.

### 3.3 Итоговая схема в ветке (CTAS с `EnableCsWriteAffinity`)

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

##### 4.1.1 [БЛОКЕР] Добавить флаг компиляции `QP_FORCE_CS_WRITE_AFFINITY`

**Статус**: ⬜ Не реализовано

**Задача**: добавить флаг компиляции `QP_FORCE_CS_WRITE_AFFINITY`. Если флаг выставлен — использовать новый режим (per-shard affinity) **независимо** от значения PRAGMA `EnableCsWriteAffinity`.

**Мотивация**: PRAGMA управляется пользователем и может быть выключена. Для тестирования и отладки нового режима нужен способ принудительно включить его на уровне сборки, не зависящий от прагмы.

**Реализация**:
- Определить макрос `QP_FORCE_CS_WRITE_AFFINITY` в [`ya.make`](ydb/core/kqp/runtime/ya.make) (или в общем `ya.make` KQP)
- Во всех местах, где проверяется `EnableCsWriteAffinity`, добавить условие `|| QP_FORCE_CS_WRITE_AFFINITY`

**Инварианты и `AFL_VERIFY` под флагом**:

В каждую изменённую функцию под флагом `QP_FORCE_CS_WRITE_AFFINITY` добавить `AFL_VERIFY` на проверку инвариантов:

| Функция | Инвариант | `AFL_VERIFY` |
|---------|-----------|--------------|
| [`BuildFillTableEffect`](ydb/core/kqp/opt/kqp_opt_effects.cpp:162) | При флаге строится два stage (Transform + Sink) | `AFL_VERIFY(enableCsWriteAffinity)` — флаг форсирует режим |
| [`FillCreateTableAs`](ydb/core/kqp/query_compiler/kqp_query_compiler.cpp:2456) | `CtasDestinationPath` заполнен | `AFL_VERIFY(!settingsProto.GetCtasDestinationPath().empty())` |
| Table Resolver | `CsShardingColumns` заполнен для OLAP sink | `AFL_VERIFY(!stageMeta.CsShardingColumns.empty())` |
| Executer | destination-шарды в `ShardIdToNodeId` | `AFL_VERIFY(!GetMeta().ShardIdToNodeId.empty())` |
| [`CountComputeTasks`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp) | per-shard задачи созданы | `AFL_VERIFY(stageInfo.Tasks.size() == shardNodes.size())` |
| [`BuildKqpStageChannels`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp) | ColumnShardHashV1 построен | `AFL_VERIFY(transformParams.TaskIndexByHash != nullptr)` |
| [`BuildInternalSinks`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp) | `TargetShardIds` заполнен | `AFL_VERIFY(!settings.TargetShardIds().empty())` |
| WriteActor | routing корректен | `AFL_VERIFY(TargetShardIds->contains(shardId))` |

**Гарантии**: при `QP_FORCE_CS_WRITE_AFFINITY` новый режим включается принудительно, а `AFL_VERIFY` детектируют нарушение инвариантов на ранней стадии.

---

##### 4.1.2 [БЛОКЕР] INSERT-фаза: `CsShardingColumns` пуст → per-shard задачи + Broadcast → CRASH

**Статус**: 🔴 Краш подтверждён, корневая причина выявлена

**Краш** (в тесте `KqpQuery::CTAS_WriteAffinity_Twin+EnableCsWriteAffinity`, INSERT-фаза):
```
VERIFY failed: verification=!stageInfo.Meta.CsShardingColumns.empty();
stageId=[0,0]; shardNodesCount=8; isOlapSink=1;
hasColumnTableInfo=1; hasShardKey=1; shardsResolved=1;
```

**Корневая причина**: `CountComputeTasks` создаёт per-shard задачи для INSERT-стадии (8 задач на 8 шардов), но `CsShardingColumns` **пуст** для INSERT. Из-за этого `BuildKqpStageChannels` не может построить `ColumnShardHashV1` HashShuffle и **откатывается на Broadcast**. Broadcast шлёт все строки во все задачи, но каждая задача ожидает только «свои» шарды (`TargetShardIds`), что приводит к `AFL_VERIFY`-крашу в WriteActor.

**Почему `CsShardingColumns` пуст для INSERT**: table resolver заполняет `CsShardingColumns` только для CTAS FILL (через `ColumnTableInfo.GetSharding().GetHashSharding().GetColumns()`), но для обычного INSERT этот путь не срабатывает.

**Что нужно сделать**:
1. **Вариант 1 (правильный)**: заполнять `CsShardingColumns` для INSERT-стадий в table resolver (аналогично CTAS FILL). Тогда `BuildKqpStageChannels` построит `ColumnShardHashV1` HashShuffle, и per-shard задачи будут работать корректно.
2. **Вариант 2 (fallback)**: в `CountComputeTasks` создавать per-shard задачи **только если** `CsShardingColumns` не пуст. Иначе — fallback на 1 задачу (все шарды в `TargetShardIds`), как в origin/main.

**Гарантия после фикса**: INSERT с `EnableCsWriteAffinity` либо использует корректный per-shard routing (HashShuffle), либо безопасно откатывается на 1 задачу без краша.

---

##### 4.1.3 [БЛОКЕР] Ретрай Navigate при `ColumnTableInfoPtr == null` — баг: CRASH

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

**Примечание**: Гонка `ColumnTableInfoPtr == null` была проверена через AFL_VERIFY в `HandleResolveKeys(TEvNavigateKeySetResult)` и `HandleResolveKeys(TEvResolveKeySetResult)` — **не подтверждена**. `ColumnTableInfoPtr` присутствует при обработке Navigate, и `ShardKey->Partitioning` уже установлена при обработке ResolveKeySet. Краш вызван другой причиной (см. 4.1.2).

**Статус**: ✅ Проверено, гонка не подтверждена

---

##### 4.1.4 [БЛОКЕР] Починить routing в BuildKqpStageChannels для CTAS sink

**Статус**: 🔴 Краш, требует немедленного фикса

`BuildKqpStageChannels` для sink stage с `EnableCsWriteAffinity` использует `kHashShuffle` путь (lines 1416-1469 в `kqp_tasks_graph.cpp`), который наследует `ColumnShardHashV1Params` из scan stage (source table). Это означает что `TaskIndexByHash` и `SourceShardCount` берутся из source table sharding, а не destination table.

**Решение**: В `BuildKqpStageChannels`, для sink stage с OLAP и `EnableCsWriteAffinity`, пропустить `kHashShuffle` путь и использовать логику из `kBroadcast`/`kMap` веток, которая строит `TaskIndexByHash` на основе `GetColumnShards()` destination table через `ColumnTableInfoPtr`.

**Код для фикса** (псевдокод в `BuildKqpStageChannels`, до switch по TypeCase):
```cpp
// For OLAP sink stages with CsWriteAffinity, override kHashShuffle path.
// The kHashShuffle path inherits ColumnShardHashV1Params from the scan stage
// (source table), which has wrong TaskIndexByHash for the destination table.
bool isOlapSinkWithAffinity = false;
if (stageInfo.Meta.ColumnTableInfoPtr && stageInfo.Meta.Tx.Body->EnableCsWriteAffinity()) {
    for (const auto& sink : stage.GetSinks()) {
        if (sink.HasInternalSink() && sink.GetInternalSink().GetSettings().Is<TKqpTableSinkSettings>()) {
            TKqpTableSinkSettings s;
            if (sink.GetInternalSink().GetSettings().UnpackTo(&s) && s.GetIsOlap()) {
                isOlapSinkWithAffinity = true;
                break;
            }
        }
    }
}
// In kHashShuffle case: if isOlapSinkWithAffinity, skip and fall through to
// the Broadcast/Map logic that builds correct TaskIndexByHash.
```

---

##### 4.1.5 [БЛОКЕР] Починить write stats — двойной счёт при per-shard задачах

**Статус**: ⬜ Workaround, требует нормального фикса

В [`kqp_cost_ut.cpp`](ydb/core/kqp/ut/cost/kqp_cost_ut.cpp) строгие ассерты на количество строк/байт в write stats заменены на `UNIT_ASSERT_GE + rows % N == 0`. Причина: при per-shard задачах каждая задача сообщает свою статистику, итого rows умножается на число задач. Это **побочный эффект**, а не ожидаемое поведение.

**Действия**:
- Выяснить, где агрегируется write stats (executer или session actor)
- Дедуплицировать/суммировать корректно: итоговое число строк должно быть равно реальному числу записанных строк, независимо от числа задач
- Вернуть строгие ассерты в тестах

---

##### 4.1.6 [БЛОКЕР] Разобраться с изменением в `kqp_session_actor.cpp`

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

##### 4.1.7 CTAS без `EnablePerStatementQueryExecution`

**Статус**: ⬜ Не исследовано

Без флага CTAS компилируется иначе (не через `TKqlFillTable`/FILL). Нужно выяснить, применяется ли тот же путь или требуется отдельная обработка.

**Действия**:
- Проверить, как компилируется CTAS при `EnablePerStatementQueryExecution=false`
- Определить, попадает ли plan в `BuildFillTableEffect` или идёт другим путём
- При необходимости применить аналогичный подход

---

##### 4.1.8 Удалить `CtasDestinationPath` если не используется

**Статус**: ⬜ Требует проверки

Поле `CtasDestinationPath = 32` добавлено в proto и заполняется в [`FillCreateTableAs`](ydb/core/kqp/query_compiler/kqp_query_compiler.cpp:2456). Table resolver получает sharding-информацию из temp-таблицы (через `ColumnTableInfo` по её `TableId`), а не из destination-таблицы. Если `CtasDestinationPath` нигде не читается — поле можно удалить.

**Действия**:
- Поиск всех мест использования `CtasDestinationPath` в runtime
- Удалить поле из proto и компилятора, если оно не нужно

---

##### 4.1.9 Удалить временные debug-флаги из `ya.make`

**Статус**: ⬜ Требует удаления перед мержем

В [`ya.make`](ydb/core/kqp/runtime/ya.make) добавлены временные флаги компиляции:
```
-DKQP_WRITE_TABLE_TARGET_SHARD_IDS_CHECK
-DKQP_WRITE_TABLE_TARGET_SHARD_IDS_EXPECTED_COUNT=1
```
и соответствующий `#ifdef`-блок в [`kqp_write_table.cpp:454`](ydb/core/kqp/runtime/kqp_write_table.cpp:454), который проверяет `TargetShardIds->size() == 1`. Эти флаги хардкодят Per-Shard (K=N) и сломают Per-Node (K=M), где `TargetShardIds.size() > 1`. Перед мержем удалить оба флага из `ya.make` и `#ifdef`-блок из `kqp_write_table.cpp`.

---

##### 4.1.10 DEBUG: Диагностика краша CTAS_WriteAffinity_Twin

**Статус**: 🔴 Краш подтверждён, корневая причина выявлена (INSERT-фаза)

**Краш** (первоначально на CTAS, после увеличения числа строк — на INSERT-фазе):
```
VERIFY failed: verification=TargetShardIds->contains(shardId);
shard_id=72075186224037890; target_shard_ids={72075186224037889};
```

**Диагностика (подтверждено через AFL_VERIFY и YDB_LOG_ERROR)**:
1. ✅ `HandleResolveKeys(Navigate)`: `ColumnTableInfoPtr` present, `CsShardingColumns` extracted correctly
2. ✅ `HandleResolveKeys(ResolveKeySet)`: `isOlapSinkWithAffinity=true`, `willOverwriteShardKey=false` — гонка из 4.1.2 НЕ подтверждена
3. ✅ Partition order: Navigate и ResolveKeySet имеют одинаковый порядок шардов `[88, 89, 90, 91]`
4. ✅ `BuildInternalSinks`: TargetShardIds назначены правильно (task 0→88, 1→89, 2→90, 3→91)
5. ❌ **Проблема**: Строка для shard 90 (bucket 2) попадает в задачу с TargetShardIds={89} (task 1)

**Корневая причина (уточнена)**: краш происходит на **INSERT-фазе** (заполнение source-таблицы), а не на CTAS. `CountComputeTasks` создаёт per-shard задачи для INSERT, но `CsShardingColumns` **пуст** для INSERT → `BuildKqpStageChannels` откатывается на Broadcast → все строки во все задачи → `AFL_VERIFY`-краш. Подробнее см. 4.1.1.

**Решение**: заполнять `CsShardingColumns` для INSERT-стадий в table resolver (аналогично CTAS FILL), либо в `CountComputeTasks` создавать per-shard задачи только при непустом `CsShardingColumns` (см. 4.1.1).

##### 4.1.11 Бенчмарк

**Статус**: ⬜ Не реализовано

Измерить: сетевой трафик, время выполнения, пиковое потребление памяти — affinity vs baseline (1 задача).

### 4.2 **Вариант B: Per-Node** K = M

> Задачи для этапа B будут уточнены после завершения и стабилизации этапа A (Per-Shard).

Общая идея: сгруппировать шарды по нодам (`P: Shard → Node`), создать M=|{Node}| задач вместо N шардов. Каждая задача обслуживает `{s | P(s) = Nodeⱼ}` шардов. `TargetShardIds` содержит несколько шардов. `ColumnShardHashV1` routing: `TaskIndexByHash[bucket] = j` (нода, не шард).