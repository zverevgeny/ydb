# ColumnShard Write Node Affinity

**Сводный документ.** Объединяет четыре ранее раздельных документа:
`ctas-data-routing-design.md`, `ctas-generalized-shard-assignment.md`,
`ctas-implementation-plan.md`, `ctas-writeactor-per-shard-design.md`.

**Цель**: Реализовать node affinity для записи в колонные таблицы (CTAS/FILL/INSERT),
где каждый WriteActor выполняется на ноде своих ColumnShard'ов, минимизируя сетевые
передачи.

**Механизм включения**: `PRAGMA ydb.EnableCsWriteAffinity` (по умолчанию **включено**).
- Явно выключить: `PRAGMA ydb.EnableCsWriteAffinity = "false";`

---

## 1. Проблема

При записи в колонную таблицу в исходной архитектуре создаётся **один** WriteActor
на весь Sink Stage, который:
1. Получает все строки от ComputeActor
2. Для каждой строки вычисляет hash от PK
3. Определяет target shard через `Partitioning->GetPartitionByHash(hash)`
4. Кладёт строку в per-shard буфер
5. Периодически отправляет буферы на соответствующие ColumnShard'ы

```
ComputeActor
    ↓ (все строки)
TKqpDirectWriteActor
    ↓
TShardedWriteController
    ├─ Hash(PK) → Shard[0] → Buffer[0] → Send to CS[0]
    ├─ Hash(PK) → Shard[1] → Buffer[1] → Send to CS[1]
    └─ Hash(PK) → Shard[N] → Buffer[N] → Send to CS[N]
```

Файл: [`kqp_write_actor.cpp`](ydb/core/kqp/runtime/kqp_write_actor.cpp:485)

### Недостатки

1. **Нет локальности**: WriteActor может быть на любой ноде, данные всегда идут по сети
2. **Все буферы в одном месте**: Память всех per-shard буферов в одном акторе
3. **Один актор = bottleneck**: Один WriteActor обрабатывает все записи
4. **Нет affinity**: Planner не учитывает расположение шардов при планировании

---

## 2. Решение: node affinity

Разбить единый Sink Stage на несколько задач, каждая из которых пишет в свой набор
шардов и выполняется на ноде этих шардов.

```
До:                              После:

ComputeActor (Stage N)           ComputeActor (Stage N)
      ↓                                ↓
┌─────────────────────┐          ┌──────────┐  ┌──────────┐  ┌──────────┐
│  TKqpDirectWriteActor│         │WriteActor│  │WriteActor│  │WriteActor│
│  (все шарды)         │          │ Node A   │  │ Node B   │  │ Node C   │
└─────────────────────┘          └────┬─────┘  └────┬─────┘  └────┬─────┘
      ↓ (всё по сети)                 ↓ local       ↓ local       ↓ local
┌──────┬──────┬──────┐            ┌──────┐     ┌──────┐     ┌──────┐
│CS[0] │CS[1] │CS[N] │            │CS[0] │     │CS[1] │     │CS[2] │
└──────┴──────┴──────┘            │CS[3] │     │CS[4] │     │CS[5] │
 (разные ноды)                    └──────┘     └──────┘     └──────┘
                                   Node A       Node B       Node C
```

---

## 3. Обобщённая модель shard assignment

**Shard Assignment** — отображение от пишущего стейджа к набору шардов:

```
ShardAssignment: WriteStage → Set<ShardId>
```

### Формальное определение

```
Given:
  S = {s₁, s₂, ..., sₙ} — множество всех шардов целевой таблицы
  P: S → NodeId — маппинг шарда на ноду (ShardIdToNodeId)
  A: S → {0, 1, ..., K-1} — assignment функция

Define:
  StageShards[i] = {s ∈ S | A(s) = i}          — шарды i-го стейджа
  StageNode[i]   = f({P(s) | s ∈ StageShards[i]}) — нода i-го стейджа

Constraints:
  1. ∀s ∈ S: ∃!i такое, что s ∈ StageShards[i]  (каждый шард ровно в одном стейдже)
  2. StageNode[i] выбирается для максимизации локальности
```

Свойства:
- `Shards[i] ∩ Shards[j] = ∅` для `i ≠ j` (дисъюнктные множества)
- `∪ Shards[i] = AllShards` (покрытие всех шардов)
- `K` — количество пишущих стейджей

### Функция маршрутизации строк

```cpp
ui32 RouteRowToStage(const TCellVec& pk,
                     const TKeyDesc& partitioning,
                     const TShardAssignment& assignment) {
    ui64 hash = ConsistencyHash64(pk);
    ui64 shardId = partitioning.GetPartitionByHash(hash);
    return assignment.GetStageForShard(shardId);
}
```

---

## 4. Варианты assignment: Per-Shard vs Per-Node

### Вариant A: Per-Shard (K = N)

```
A(s) = shard_index(s)
K = N (количество шардов)
StageShards[i] = {sᵢ}       (один шард)
StageNode[i]   = P(sᵢ)      (нода шарда)
```

```
Previous Stage
    ↓  TDqCnPartitionByKey (PartitionCount = N)
  Stage[0] Stage[1] ... Stage[N-1]
    ↓        ↓              ↓
   CS[0]    CS[1]        CS[N-1]
   Node A   Node B        Node C
```

### Вариант B: Per-Node (K = M) — рекомендуется

```
A(s) = P(s)  (нода шарда)
K = M (количество уникальных нод)
StageShards[i] = {s ∈ S | P(s) = Nodeᵢ}   (все шарды на ноде i)
StageNode[i]   = Nodeᵢ
```

```
Previous Stage
    ↓  TDqCnPartitionByNode (PartitionCount = M)
  Stage[0]     Stage[1]     Stage[M-1]
    ↓            ↓            ↓
  ┌──────┐    ┌──────┐    ┌──────┐
  │CS[0] │    │CS[1] │    │CS[N-1]│
  │CS[2] │    │CS[3] │    │       │
  └──────┘    └──────┘    └──────┘
  Node A       Node B      Node C
  (local)      (local)     (local)
```

### Сравнение

| Критерий | Per-Shard (A) | Per-Node (B) |
|----------|---------------|--------------|
| **Assignment** | `A(s) = shard_index(s)` | `A(s) = P(s)` |
| **K (стейджей)** | N (шардов) | M (нод) |
| **StageShards[i]** | `{sᵢ}` | `{s \| P(s) = Nodeᵢ}` |
| **StageNode[i]** | `P(sᵢ)` | `Nodeᵢ` |
| **Локальность** | 100% | 100% |
| **Акторов** | N (может быть 1000+) | M (обычно 10-100) |
| **Сложность** | Высокая | Средняя |
| **Overhead** | Высокий | Низкий |
| **Внутристейджевая маршрутизация** | Не нужна | Нужна (но локальная) |

**Рекомендация**: Per-Node как частный случай обобщённой модели — минимум акторов,
100% локальность внутри ноды, использует существующий `ShardIdToNodeId`.

---

## 5. Дизайн маршрутизации данных

На этапе компиляции partitioning целевой таблицы может быть **неизвестен** (для CTAS
таблица ещё не создана). Ключевые подходы:

### 5.1 Абстракция assignment

```cpp
class TShardAssignment {
public:
    virtual ui32 GetStageCount() const = 0;
    virtual TVector<ui64> GetStageShards(ui32 stageIdx) const = 0;
    virtual ui64 GetStageNode(ui32 stageIdx) const = 0;
    virtual ui32 GetStageForShard(ui64 shardId) const = 0;
};
```

**Per-Node реализация** строит `NodeToShards` и `NodeToStageIdx` из `ShardIdToNodeId`:

```cpp
class TPerNodeAssignment : public TShardAssignment {
    THashMap<ui64, TVector<ui64>> NodeToShards;
    THashMap<ui64, ui32> NodeToStageIdx;
public:
    TPerNodeAssignment(const TKeyDesc& p, const TShardIdToNodeIdMap& s) {
        for (ui32 i = 0; i < p.Size(); ++i) {
            ui64 shardId = p.GetShardId(i);
            NodeToShards[s.at(shardId)].push_back(shardId);
        }
        ui32 idx = 0;
        for (auto& [nodeId, _] : NodeToShards) {
            NodeToStageIdx[nodeId] = idx++;
        }
    }
    ui32 GetStageForShard(ui64 shardId) const override {
        return NodeToStageIdx.at(ShardToNode.at(shardId));
    }
    // GetStageCount / GetStageShards / GetStageNode — по NodeToShards / NodeToStageIdx
};
```

### 5.2 Роутинг через DQ

DQ framework автоматически маршрутизирует данные через каналы между задачами. Если
sink stage имеет M задач, предыдущий stage тоже должен иметь M задач, а канал между
ними использует partition routing.

- **Подвариант A**: `TDqCnPartitionByKey` с key = hash(PK) → nodeId
- **Подвариант B** (рекомендуется): если sink stage имеет M задач, DQ framework
  автоматически создаёт M задач для предыдущего stage через COPY механизм в
  `TMaxTasksGraph`.

---

## 6. План реализации и статус

**Принцип**: После каждого этапа при **выключенной** прагме все существующие тесты
проходят; при **включённой** — новые тесты подтверждают поведение.

| Этап | Название | Статус |
|------|----------|--------|
| 1 | PRAGMA + proto-поле `TKqpPhyTx.EnableCsWriteAffinity` | ✅ Выполнено |
| 2 | Выделение WriteActor в отдельный stage (по прагме) | ✅ Выполнено |
| 3 | Поля `TargetShardIds` / `ExpectedNodeId` в `TKqpTableSinkSettings` | ✅ Выполнено |
| 4 | Пометка sink для affinity в `BuildFillTableEffect()` | ✅ Выполнено |
| 5 | Множественные задачи sink stage в TasksGraph | ✅ Выполнено |
| 6 | Планировщик (проверка, изменений не требуется) | ✅ Выполнено |
| 7 | Фильтрация шардов в WriteActor | ✅ Выполнено |
| 8 | Роутинг данных (одна задача, single-node) | ✅ Выполнено |
| 9 | Комплексное тестирование | ✅ Выполнено |

### Этап 1: PRAGMA + protobuf поле

**Механизм**: используется `TKikimrConfiguration` (`NCommon::TConfSetting` +
`REGISTER_SETTING`), прагма читается двумя способами:
- **на этапе оптимизации** — из `kqpCtx.Config->EnableCsWriteAffinity` (Этап 2);
- **в TasksGraph/runtime** — из proto-поля `TKqpPhyTx.EnableCsWriteAffinity` (Этапы 4–5).

**Файлы**:
- [`yql_kikimr_settings.h`](ydb/core/kqp/provider/yql_kikimr_settings.h:127) — объявление
- [`yql_kikimr_settings.cpp`](ydb/core/kqp/provider/yql_kikimr_settings.cpp:173) — регистрация
- [`kqp_physical.proto`](ydb/core/protos/kqp_physical.proto:740) — поле в `TKqpPhyTx`
- [`kqp_query_compiler.cpp`](ydb/core/kqp/query_compiler/kqp_query_compiler.cpp:1211) — передача в proto

```cpp
// yql_kikimr_settings.h
NCommon::TConfSetting<bool, Static> EnableCsWriteAffinity;
// yql_kikimr_settings.cpp
REGISTER_SETTING(*this, EnableCsWriteAffinity);
// kqp_query_compiler.cpp:1211
txProto.SetEnableCsWriteAffinity(Config->EnableCsWriteAffinity.Get().GetOrElse(true));
```

> **Дефолт — `true`.** Значение читается в двух местах и должно совпадать:
> [`kqp_opt_effects.cpp:238`](ydb/core/kqp/opt/kqp_opt_effects.cpp:238) и
> [`kqp_query_compiler.cpp:1211`](ydb/core/kqp/query_compiler/kqp_query_compiler.cpp:1211).

### Этап 2: Выделение WriteActor в отдельный stage

Разделение на два stage выполняется **только при включённой прагме**, чтобы
гарантировать неизменность плана по умолчанию.

**Файл**: [`kqp_opt_effects.cpp`](ydb/core/kqp/opt/kqp_opt_effects.cpp:240) — `BuildFillTableEffect()`

```cpp
if (enableCsWriteAffinity) {
    // Transform stage: без явного .Outputs()
    auto transformStage = Build<TDqStage>(ctx, node.Pos())
        .Inputs().Add(mapCn).Build()
        .Program().Args({rowArgument})
            .Body<TCoToFlow>().Input(rowArgument).Build().Build()
        .Settings().Build().Done();

    // Соединение ссылается на выход transform stage напрямую
    auto sinkInput = Build<TDqCnMap>(ctx, node.Pos())
        .Output<TDqOutput>().Stage(transformStage).Index().Build("0").Build()
        .Done();

    // Отдельный аргумент для лямбды sink stage
    const auto sinkRowArgument = Build<TCoArgument>(ctx, node.Pos())
        .Name("sinkRow").Done();

    auto sinkStage = Build<TDqStage>(ctx, node.Pos())
        .Inputs().Add(sinkInput).Build()
        .Program().Args({sinkRowArgument})
            .Body<TCoToFlow>().Input(sinkRowArgument).Build().Build()
        .Outputs<TDqStageOutputsList>().Add(sink).Build()
        .Settings().Build().Done();
    // ...
}
```

**⚠️ Уроки реализации**:
1. **Разделение условное, не безусловное** — только по прагме, чтобы не трогать
   canondata-эталоны планов.
2. **Нет метода `transformStage.Output(0)`** — соединение через
   `TDqCnMap.Output<TDqOutput>().Stage(transformStage).Index("0")`; transform stage
   объявляется **без** `.Outputs()`.
3. **Нельзя переиспользовать один `TCoArgument` в двух лямбдах** — аборт на
   `CheckArguments()` (code 1060). Для sink stage — отдельный аргумент `sinkRow`.
4. **`.Add<TDqOutput>()` в `.Outputs()` не работает** — билдер требует ссылку на stage.

### Этап 3: Поля в TKqpTableSinkSettings

**Файл**: [`kqp.proto`](ydb/core/protos/kqp.proto:889) — `TKqpTableSinkSettings`

```protobuf
message TKqpTableSinkSettings {
    // ... поля 3..29 ...
    // Target shard IDs for per-node shard affinity.
    repeated uint64 TargetShardIds = 30;
    // Expected node ID for task scheduling affinity.
    optional uint64 ExpectedNodeId = 31;
}
```

### Этап 4: Пометка sink для affinity

На этапе оптимизации `ShardIdToNodeId` **недоступен**. Поэтому sink лишь помечается
как требующий affinity; конкретные шарды и `ExpectedNodeId` проставляются в TasksGraph
(Этап 5). Явный «маркер» не нужен — используется `EnableCsWriteAffinity` +
признак `fill_table`-режима sink.

**Файл**: [`kqp_opt_effects.cpp`](ydb/core/kqp/opt/kqp_opt_effects.cpp:238)

```cpp
// Stage 4: Affinity marker for sink settings
//
// At optimization time, ShardIdToNodeId is NOT available. Therefore, we cannot
// populate TargetShardIds or ExpectedNodeId here. Instead:
//   - The EnableCsWriteAffinity flag is already in TKqpPhyTx (Stage 1).
//   - The sink mode "fill_table" identifies this as a CTAS sink.
//   - In TasksGraph (Stage 5), the combination triggers multi-task creation.
```

### Этап 5: TasksGraph — множественные задачи

**Файлы**:
- [`kqp_tasks_graph.cpp`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp:3307) — `BuildInternalSinks()`
- [`kqp_prepared_query.h`](ydb/core/kqp/query_data/kqp_prepared_query.h:63) — accessor `EnableCsWriteAffinity()`

Алгоритм: сгруппировать шарды по нодам через `ShardIdToNodeId`, создать по одной
задаче на каждую ноду, установить `ExpectedNodeId` и `TargetShardIds`.

```cpp
THashMap<ui64, TVector<ui64>> NodeToShards;
for (const auto& [shardId, nodeId] : GetMeta().ShardIdToNodeId) {
    NodeToShards[nodeId].push_back(shardId);
}
if (IsCtasWriteAffinityEnabled(stageInfo)) {
    for (const auto& [nodeId, shards] : NodeToShards) {
        auto& task = AddTask(stageInfo);
        task.Meta.ExpectedNodeId = nodeId;
        task.Meta.TaskParams["target_shard_ids"] = SerializeShards(shards);
    }
}
```

### Этап 6: Планировщик — affinity уже поддерживается

**Файл**: [`kqp_planner.cpp`](ydb/core/kqp/executer_actor/kqp_planner.cpp:346) — изменения не нужны.

```cpp
for (const auto& task : TasksGraph.GetTasks()) {
    if (task.Meta.ExpectedNodeId) {
        TasksPerNode[*task.Meta.ExpectedNodeId].emplace_back(task.Id);
    } else {
        UnassignedTasks.emplace_back(task.Id);
    }
}
```

### Этап 7: WriteActor — фильтрация шардов

**Файл**: [`kqp_write_actor.cpp`](ydb/core/kqp/runtime/kqp_write_actor.cpp:475)

При наличии `TargetShardIds` WriteActor пишет только в указанные шарды и валидирует,
что resolved-шарды входят в целевой набор.

```cpp
for (const auto& shard : ResolvedShards) {
    YQL_ENSURE(Contains(TargetShardIds, shard.ShardId),
        "Shard " << shard.ShardId << " not in target_shard_ids");
}
```

### Этап 8: Роутинг данных

**Ключевой инсайт**: если sink stage имеет M задач, DQ framework автоматически создаёт
M задач для предыдущего stage через COPY механизм в `TMaxTasksGraph`. На текущий момент
реализован single-task routing (multi-node deferred).

### Этап 9: Комплексное тестирование

- TWIN-тест `CTAS_WriteAffinity_Twin` — проверяет обе ветки прагмы (3 stage без /
  4 stage с) и идентичность записанных данных.
- Регрессия: CTAS 30/30, Olap 12/12, CreateAsSelect 17/17 GOOD.

Подробный план — в разделе 7.

---

## 7. План тестирования

### 7.1 Принципы

1. **Двухрежимность.** Каждая фича проверяется при `EnableCsWriteAffinity = "false"`
   (регрессия — поведение не меняется) и `"true"` (новое поведение подтверждается).
   Инструмент — `Y_UNIT_TEST_TWIN` / `Y_UNIT_TEST_QUAD`.
2. **Целостность прежде всего.** Ни одна строка не потеряна и не продублирована;
   каждая строка попадает в правильный шард (проверяется `AFL_VERIFY` в WriteActor).
3. **План запроса.** Explain-план проверяет структуру stage'ов (число stage, тип
   соединения sink — Broadcast/HashShuffle).

### 7.2 Существующие тесты (baseline)

| Тест | Файл | Что проверяет |
|------|------|---------------|
| `CTAS_WriteAffinity_Twin` | [`kqp_query_ut.cpp:2353`](ydb/core/kqp/ut/query/kqp_query_ut.cpp:2353) | Обе ветки прагмы: 3 stage без / 4 stage с; наличие Broadcast+Sink; идентичность данных |
| `KqpWriteAffinity::CTAS_WriteAffinity_LargeData` | [`kqp_write_affinity_ut.cpp:50`](ydb/core/kqp/ut/query/kqp_write_affinity_ut.cpp:50) | CTAS 100 строк через несколько flush'ей; COUNT(*) и выборочные строки |
| `KqpWriteAffinity::CTAS_WriteAffinity_MultiNode` | [`kqp_write_affinity_ut.cpp:114`](ydb/core/kqp/ut/query/kqp_write_affinity_ut.cpp:114) | Кластер из 3 нод; per-shard задачи пиннуются к нодам своих шардов |
| `KqpWriteAffinity::CTAS_WriteAffinity_JoinSource` | [`kqp_write_affinity_ut.cpp:165`](ydb/core/kqp/ut/query/kqp_write_affinity_ut.cpp:165) | CTAS с JOIN-источником и affinity |
| `KqpWriteAffinity::CTAS_WriteAffinity_EmptySource` | [`kqp_write_affinity_ut.cpp:233`](ydb/core/kqp/ut/query/kqp_write_affinity_ut.cpp:233) | Пустой источник — таблица создана, строк нет |
| `KqpWriteAffinity::CTAS_WriteAffinity_CompositeKey` | [`kqp_write_affinity_ut.cpp:275`](ydb/core/kqp/ut/query/kqp_write_affinity_ut.cpp:275) | Sharding hash по нескольким ключевым колонкам |
| `KqpOlapWrite::TestInsertDataIntegrityViolation` | [`write_ut.cpp:555`](ydb/core/kqp/ut/olap/operations/write_ut.cpp:555) | AFL_VERIFY ловит попадание строки в чужой шард |
| `kqp_data_integrity_trails_ut.cpp` | [`kqp_data_integrity_trails_ut.cpp:129`](ydb/core/kqp/ut/data_integrity/kqp_data_integrity_trails_ut.cpp:129) | Per-shard WriteActor: логи масштабируются с числом шардов |
| `kqp_cost_ut.cpp` | [`kqp_cost_ut.cpp:1008`](ydb/core/kqp/ut/cost/kqp_cost_ut.cpp:1008) | Статистика rows/bytes отчитывается per-shard |

### 7.3 Матрица покрытия по этапам

| Этап | Проверка | Тест |
|------|----------|------|
| 1 PRAGMA + proto | Прагма попадает в `TKqpPhyTx` | `CTAS_WriteAffinity_Twin` (Explain) |
| 2 Отдельный stage | 3 stage без / 4 stage с прагмой | `CTAS_WriteAffinity_Twin` |
| 3 Поля sink settings | `TargetShardIds` / `ExpectedNodeId` в proto | покрыто планом + runtime |
| 4 Пометка sink | Sink помечен для affinity | `CTAS_WriteAffinity_Twin` |
| 5 Множественные задачи | M задач с `ExpectedNodeId` | `CTAS_WriteAffinity_MultiNode` |
| 6 Планировщик | Задачи на нодах своих шардов | `CTAS_WriteAffinity_MultiNode` |
| 7 Фильтрация шардов | WriteActor не пишет чужие шарды | `TestInsertDataIntegrityViolation` |
| 8 Роутинг данных | Каждая строка → правильный шард | `CTAS_WriteAffinity_LargeData`, `_CompositeKey` |
| 9 Комплекс | Целостность + план + регрессия | весь набор выше |

### 7.4 Функциональные сценарии

**Целостность данных:**
- Все строки записаны (`COUNT(*)` совпадает с источником)
- Нет дублей и потерь (выборочная проверка строк по PK)
- `EnableCsWriteAffinity=true`/`false` дают идентичные данные (TWIN)

**Топологии таблиц:**
- Один шард (1 задача, single-node)
- Несколько шардов на одной ноде
- Несколько шардов на нескольких нодах (affinity, локальность)
- Композитный ключ шардирования

**Источники данных:**
- Простой `SELECT * FROM`
- JOIN нескольких таблиц
- Пустой источник
- Большой объём (несколько flush'ей на шард)

**Типы операций:** CTAS, FILL, INSERT в колонную таблицу.

### 7.5 Негативные и краевые случаи

| Случай | Ожидание |
|--------|----------|
| Строка попала в чужой шард (баг маршрутизации) | `AFL_VERIFY` в [`kqp_write_table.cpp:523`](ydb/core/kqp/runtime/kqp_write_table.cpp:523) падает с диагностикой `shard_id`/`target_shard_ids` |
| Шард не резолвится в `ShardIdToNodeId` | Fallback: single-task путь (корректность сохранена, affinity отложена) |
| Прагма выключена | План идентичен прежнему (canondata-эталоны не меняются) |
| Пустой источник | Таблица создана, 0 строк |

### 7.6 Регрессия

При выключенной прагме поведение по умолчанию не меняется:

```bash
# CreateAsSelect регрессия
./ya make --build relwithdebinfo -tA ydb/core/kqp/ut/query -F '*CreateAsSelect*'

# OLAP write интеграция
./ya make --build relwithdebinfo -tA ydb/core/kqp/ut/olap/operations -F '*'

# Affinity suite
./ya make --build relwithdebinfo -tA ydb/core/kqp/ut/query -F 'KqpWriteAffinity::*'

# Integrity violation (AFL_VERIFY)
./ya make --build relwithdebinfo -tA ydb/core/kqp/ut/olap \
    -F 'KqpOlapWrite::TestInsertDataIntegrityViolation'
```

Целевые эталоны прохождения: CTAS 30/30, Olap 12/12, CreateAsSelect 17/17.

### 7.7 Пробелы и планируемые тесты

| Область | Статус | План |
|---------|--------|------|
| Multi-node роутинг (M задач на M нод) | ⬜ Отложено (Этап 8 — single-task) | Тест на реальную M-задачную маршрутизацию после включения multi-node |
| Производительность (локальность) | ⬜ | Бенчмарк меж-нодового трафика: affinity vs baseline |
| Перераспределение шардов (reshard) | ⬜ | Корректность маршрутизации при split/merge шардов |
| INSERT (не только CTAS/FILL) | 🔶 Частично | Расширить покрытие OLAP INSERT с affinity |

---

## 8. Ключевые файлы

| Файл | Роль |
|------|------|
| [`yql_kikimr_settings.h`](ydb/core/kqp/provider/yql_kikimr_settings.h:127) | Объявление настройки `EnableCsWriteAffinity` |
| [`kqp_physical.proto`](ydb/core/protos/kqp_physical.proto:740) | Поле `TKqpPhyTx.EnableCsWriteAffinity` |
| [`kqp.proto`](ydb/core/protos/kqp.proto:889) | `TargetShardIds` / `ExpectedNodeId` в `TKqpTableSinkSettings` |
| [`kqp_query_compiler.cpp`](ydb/core/kqp/query_compiler/kqp_query_compiler.cpp:1211) | Проброс прагмы в proto |
| [`kqp_opt_effects.cpp`](ydb/core/kqp/opt/kqp_opt_effects.cpp:162) | `BuildFillTableEffect()` — разделение stage, пометка sink |
| [`kqp_tasks_graph.cpp`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp:3307) | `BuildInternalSinks()` — множественные задачи, `ExpectedNodeId` |
| [`kqp_tasks_graph.cpp`](ydb/core/kqp/executer_actor/kqp_tasks_graph.cpp:3383) | `ResolveShards()` — `ShardIdToNodeId` маппинг |
| [`kqp_planner.cpp`](ydb/core/kqp/executer_actor/kqp_planner.cpp:346) | `AssignTasksToNodes()` — планирование с affinity |
| [`kqp_write_actor.cpp`](ydb/core/kqp/runtime/kqp_write_actor.cpp:475) | `TKqpDirectWriteActor` — фильтрация шардов |
| [`dq_opt_phy.cpp`](ydb/library/yql/dq/opt/dq_opt_phy.cpp:1514) | `DqBuildPartitionStage()` — partition stage |
| [`dq_channel_service_impl.h`](ydb/library/yql/dq/runtime/dq_channel_service_impl.h:269) | `TOutputDescriptor` — маршрутизация данных |

---

## 9. Риски и митигация

| Риск | Митигация |
|------|-----------|
| `ShardIdToNodeId` недоступен на этапе оптимизации | Создаём задачи в TasksGraph, где информация доступна |
| Несоответствие роутинга и assignment | DQ framework синхронизирует задачи через COPY |
| Перегрузка при большом количестве нод | Per-Node: K = M (нод), обычно 10-100 |
| Регрессия существующей функциональности | PRAGMA по умолчанию не меняет план (условное разделение) |
| Потеря данных | Валидация в WriteActor, тесты на целостность |
| Много акторов при per-shard (1000+ шардов) | Использовать per-node группировку |

---

## 10. Критерии завершения

1. Все существующие тесты проходят без прагмы
2. Все существующие тесты проходят с прагмой
3. Новые тесты покрывают все этапы с прагмой
4. Данные корректно записываются в целевую таблицу (без потерь и дублей)
5. WriteActor'ы выполняются на нодах своих шардов
6. Нет деградации производительности без прагмы
7. Улучшение производительности с прагмой (из-за локальности)
