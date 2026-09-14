# План: пропуск чтения данных колонок parquet по min/max

Поддержать отсечение чтения column chunk’ов parquet-файла в S3 (Federated Query / YQL S3 provider) на основе статистик min/max при выполнении SQL с `WHERE`.

## 1. Цель

При запросе вида

```sql
PRAGMA s3.UsePredicatePushdown = "true";

SELECT fruit, ts
FROM connection.`/data.parquet`
WITH (FORMAT = "parquet", SCHEMA = (ts Timestamp NOT NULL, fruit Utf8 NOT NULL))
WHERE Timestamp("2024-06-14T00:00:00Z") <= ts AND ts < Timestamp("2024-06-15T00:00:00Z");
```

не скачивать и не декодировать данные колонок тех row group’ов, для которых по footer-статистикам (min/max колонки) предикат заведомо ложен.

Ожидаемый эффект:

- меньше HTTP range-запросов к S3 (`IngressBytes`);
- меньше CPU на decode parquet;
- результат запроса тот же, что без pushdown.

Гранулярность **первой версии**: целый row group (все выбранные column chunk’и группы). Не отдельная колонка внутри группы и не отдельная data page.

Почему не колонка внутри группы: для `SELECT *` / проекции всё равно нужны все запрошенные колонки совпавшего row group. Экономия I/O появляется, когда группа целиком отбрасывается и range-read её chunk’ов не делается.

## 2. Текущая архитектура

Цепочка уже существует и работает для `DATE` / `TIMESTAMP`. Задача — расширить её, а не строить новый механизм.

```
SQL WHERE
  → logical opt (PushFilterTo*)
    → FilterPredicate (TCoLambda) на TS3ParseSettings / TS3ReadObject
      → SerializeFilterPredicate → NYql.NConnector.NApi.TPredicate (proto)
        → S3 read actor
          → parquet FileMetaData (footer)
            → MatchedRowGroups(metadata, predicate)
              → WillNeedRowGroups / DecodeRowGroups только для совпавших групп
```

Включается прагмой `s3.UsePredicatePushdown` (по умолчанию `false`):

- настройка: `ydb/library/yql/providers/s3/provider/yql_s3_settings.h` (`UsePredicatePushdown`);
- регистрация: `yql_s3_settings.cpp`.

### 2.1. Оптимизатор

Файл: `ydb/library/yql/providers/s3/provider/yql_s3_logical_opt.cpp`

- `PushFilterToS3ReadObject` — путь `TCoFlatMap` над `TS3ReadObject`.
- `PushFilterToDqSourceWrap` — путь `TCoFlatMap` над `TDqSourceWrap` / `TS3ParseSettings` (основной для DQ/FQ).

Оба хендлера:

1. Проверяют `UsePredicatePushdown`.
2. Ищут `TCoOptionalIf` в теле лямбды `FlatMap`.
3. Собирают предикаты через `NPushdown::CollectPredicates` с `TPushdownSettings`.
4. Частично пушат `AND` (`SplitForPartialPushdown`): неподдерживаемые конъюнкты остаются в `FlatMap`.
5. Записывают поддерживаемую часть в `FilterPredicate`.

`FlatMap` с исходным фильтром **не удаляется**: если runtime не смог применить предикат, фильтрация на CPU всё равно верная.

Текущие флаги `TPushdownSettings` для S3:

```
ExpressionAsPredicate | ArithmeticalExpressions | ImplicitConversionToInt64
| DateTimeTypes | TimestampCtor
```

`StringTypes` и `UuidType` **не включены**. Без них сравнения по `Utf8`/`String` и `Uuid` в `CollectPredicates` отбрасываются (`collection.cpp`: `IsUuidType` / `IsStringType`). Даже если рантайм научится матчить UUID-статистики, предикат `WHERE id = Uuid("...")` не дойдёт до actor, пока не включить `UuidType`.

Сравнение: generic-провайдер включает ещё `StringTypes`, `LikeOperator`, `DecimalType`, `DateCtor` и др. (`yql_generic_physical_opt.cpp`). `UuidType` там тоже не включён.

### 2.2. Сериализация предиката

- Proto: `NYql.NConnector.NApi.TPredicate` в `ydb/library/yql/providers/s3/proto/source.proto` (поле `Predicate = 15`).
- Сериализация: `SerializeFilterPredicate` в `ydb/library/yql/providers/generic/provider/yql_generic_predicate_pushdown.cpp`.
- Вызов: `ydb/library/yql/providers/s3/provider/yql_s3_dq_integration.cpp` (при заполнении `srcDesc` из `TS3ParseSettings`).

Runtime получает предикат в `TReadSpec::Predicate` (`yql_s3_read_actor.cpp`).

### 2.3. Runtime: HTTP parquet reader

Файл: `ydb/library/yql/providers/s3/actors/yql_s3_read_actor.cpp`

Метод `RunCoroBlockArrowParserOverHttp`:

1. Открывает `parquet::arrow::FileReader` над `THttpRandomAccessFile` (HTTP range `ReadAt`).
2. Берёт `fileMetadata`.
3. Если предикат задан — `MatchedRowGroups(fileMetadata, ReadSpec->Predicate)`.
4. Строит `columnIndices` по схеме запроса (`BuildColumnConverters`) — проекция колонок уже есть.
5. Для каждой (совпавшей) группы: `WillNeedRowGroups` → Arrow `PreBuffer` нужных column chunk’ов, затем `DecodeRowGroups`.

`WillNeedRowGroups` (`contrib/libs/apache/arrow/.../parquet/arrow/reader.cc`) делает `parquet_reader()->PreBuffer(row_groups, column_indices, ...)`. Именно здесь экономится сеть: несовпавшие группы не префетчатся.

Есть отдельный путь `RunCoroBlockArrowParserOverFile` (локальный `file://`): предикат **не применяется**, читаются все row group’ы.

### 2.4. Сопоставление статистик с предикатом

Заголовок: `ydb/library/yql/providers/s3/actors/yql_arrow_push_down.h`  
Реализация: `ydb/library/yql/providers/s3/actors/yql_arrow_push_down.cpp`

```
MatchedRowGroups(FileMetaData, TPredicate) → TVector<ui64>  // индексы групп
```

`MatchRowGroup`:

- для каждой колонки row group, если `is_stats_set()`;
- по `logical_type()` извлекает min/max **только** для `DATE` и `TIMESTAMP`;
- остальные logical types (`INT`, `STRING`, `UUID`, `NONE`, `DECIMAL`, …) — `break`, статистика не попадает в map;
- вызывает `NYql::NGenericPushDown::MatchPredicate(columns, predicate)`.

Семантика `MatchPredicate` (`ydb/library/yql/providers/generic/pushdown/yql_generic_match_predicate.cpp`):

- трёхзначная логика: `True` / `False` / `Unknown`;
- `And` / `Or` / `Neg` / `Comparison` / `Between`;
- итоговый ответ: `result != False` → **оставить** row group;
- `Unknown` (нет статистик, неподдерживаемый тип, сложное выражение) → читать группу.

Это правильная консервативная модель: ложноотрицательный skip хуже, чем лишнее чтение.

Компараторы реализованы **только для TIMESTAMP / DATETIME / DATE** через `TTimestampColumnStatsData`. Для остальных типов в коде явно стоит `// TODO: other types`.

Модель статистик уже шире рантайма (`yql_generic_column_statistics.h`):

| Поле `TColumnStatistics` | Смысл | Используется в матчере |
|---|---|---|
| `Timestamp` | min/max `TInstant` | да |
| `LongStats` | min/max `i64` | нет |
| `DoubleStats` | min/max `double` | нет |
| `BooleanStats` | numTrues / numFalses / numNulls | нет |
| `StringStats` | только длины (`maxColLen`, `avgColLen`), **нет min/max** | нет |
| `BinaryStats` | только длины, **нет min/max** | нет |
| `DecimalStats` | min/max decimal | нет |
| отдельного `UuidStats` нет | UUID — 16 байт `FIXED_LEN_BYTE_ARRAY` | нет |

### 2.5. Что уже покрыто тестами

Юнит:

- `ydb/library/yql/providers/s3/actors/ut/yql_arrow_push_down_ut.cpp` — timestamp, один/несколько row group, полное отсечение.
- `ydb/library/yql/providers/generic/pushdown/ut/match_predicate_ut.cpp` — timestamp comparison / between.

Интеграция FQ:

- `ydb/tests/fq/s3/test_format_setting.py` → `test_s3_push_down_parquet`:
  - parquet с `row_group_size=2`;
  - фильтр по `Timestamp`;
  - с прагмой и без;
  - одинаковый результат + `IngressBytes` с pushdown строго меньше.

## 3. Пробелы и дефекты

### 3.1. Только DATE/TIMESTAMP

Предикат `WHERE Price > 10` (Int) или `WHERE id = Uuid("...")` может быть семантически простым, но:

- Int: флаги оптимизатора позволяют пушить, но в `MatchRowGroup` INT-статистики не извлекаются;
- Uuid: `UuidType` выключен, плюс нет сериализации `TCoUuid`.

В обоих случаях матчер/рантайм не отсекает группу. Pushdown формально «включён», экономии нет.

### 3.2. Строки и UUID не пушатся с оптимизатора

`TPushdownSettings` без `StringTypes` и без `UuidType`.

- Строки: даже после поддержки BYTE_ARRAY min/max в рантайме `WHERE fruit = "Apple"` не дойдёт до actor, пока не включат `StringTypes` (этап 2).
- UUID: то же для `WHERE id = Uuid("...")`. В этапе 1 нужно **включить `UuidType`**, не дожидаясь строк.

Дополнительно `SerializeExpression` в `yql_generic_predicate_pushdown.cpp` не сериализует литерал `TCoUuid` (нет `MATCH_ATOM` / аналога `SerializeDecimal`). Даже с флагом `UuidType` сериализация упадёт с `unknown expression`, пока не добавить запись `Ydb::Type::UUID` + `low_128` / `high_128`.

### 3.3. File-path без skip

`RunCoroBlockArrowParserOverFile` игнорирует `ReadSpec->Predicate`. Для `file://` и тестов на локальных файлах отсечения нет.

### 3.4. Баг prefetch следующего row group

В `RunCoroBlockArrowParserOverHttp` при постановке в очередь следующей группы:

```cpp
readers[readyReaderIndex]->WillNeedRowGroups({
    hasPredicate ? static_cast<int>(nextGroup) : static_cast<int>(nextGroup)
}, columnIndices);
```

Обе ветки используют `nextGroup` (индекс в списке совпавших), а не `matchedRowGroups[nextGroup]` (индекс в файле).

Следствие: при совпавших группах `{1, 3}` после первой качается группа `1` файла вместо `3`. Лишний I/O и возможное рассогласование prefetch/decode. При decode ниже по коду индекс берётся из `matchedRowGroups[readyGroupIndex]` — качается одно, декодируется другое.

Исправление: всегда

```cpp
int rg = hasPredicate ? static_cast<int>(matchedRowGroups[nextGroup])
                      : static_cast<int>(nextGroup);
readers[readyReaderIndex]->WillNeedRowGroups({rg}, columnIndices);
```

Аналогично проверить все остальные места, где в `WillNeedRowGroups` / `DecodeRowGroups` подставляется индекс группы.

### 3.5. Нет проверки `HasMinMax()`

`GetDateStatistics` / `GetTimestampStatistics` сразу вызывают `typedStatistics->min()` / `max()`. Если статистика есть, но min/max не выставлены (все NULL, writer не записал), поведение не определено. Нужно `HasMinMax()`; иначе не добавлять колонку в map (`Unknown`).

### 3.6. Page-level skip отсутствует и в текущем Arrow недоступен

Parquet Column Index / Offset Index (min/max **страницы** колонки) позволяют не читать отдельные data page внутри row group.

- В `contrib/libs/apache/arrow` (peerdir S3 actors) **нет** `page_index.h` / `PageIndexReader`.
- API есть в `contrib/libs/apache/arrow_next`.

Для первой версии page skip **не делать**. Это отдельный этап после смены Arrow или своей читалки ColumnIndex по `column_index_offset` в `ColumnChunk`.

### 3.7. Не покрытые типы и операторы (сознательно отложить)

- DECIMAL (INT32 / INT64 / FIXED_LEN_BYTE_ARRAY, scale/precision).
- UINT через widening в INT64 — осторожно с переполнением.
- `IN (...)`, `IS NULL` / `IS NOT NULL`.
- Касты в предикате (`CAST(col AS ...)`).
- `LIKE` / `CONTAINS` (по min/max в общем случае нельзя).
- Nested / LIST / MAP — parquet stats на листьях, текущий reader LIST и так не читает (`test_btc`).

## 4. Принципы реализации

1. **Консервативность.** Skip только при доказанном `False`. Любое сомнение → читать.
2. **Не менять семантику SQL.** `FlatMap` с исходным `WHERE` остаётся.
3. **Не скачивать данные для решения skip.** Только footer `FileMetaData` (уже в памяти после `FileReader::Open`).
4. **Не включать pushdown по умолчанию.** Прагма как сейчас; поведение без прагмы не меняется.
5. **Общий матчер.** Расширять `NYql::NGenericPushDown::MatchPredicate` — его же может использовать generic-провайдер.
6. **Не трогать Arrow.** Только выбор subset row group’ов уже существующим API `WillNeedRowGroups` / `ReadRowGroup`.

## 5. Декомпозиция на задачи

Правила:

- Одна задача = один проверяемый инкремент. Без тестов задача не считается сделанной.
- Тесты пишутся **в той же задаче**, что и код. Регрессия существующих timestamp-тестов обязательна в каждой задаче, которая трогает общий матчер или `MatchedRowGroups`.
- Skip только при доказанном `False`. Тесты покрывают и skip, и «оставить группу».
- T0–T11 — минимальный вертикальный срез (этап 1). T12–T14 — строки. T15 — page-level, не в том же круге PR.

### 5.1. Сводка и зависимости

| ID | Задача | Зависит от | Где тесты | Статус |
|---|---|---|---|---|
| T0 | Prefetch: индекс файла, не индекс списка | — | FQ `test_format_setting.py` | DONE |
| T1 | `HasMinMax()` для DATE/TIMESTAMP | — | `yql_arrow_push_down_ut.cpp` | DONE |
| T2 | Компараторы `LongStats` | — | `match_predicate_ut.cpp` | DONE |
| T3 | INT-статистики из footer | T2 | `yql_arrow_push_down_ut.cpp` | DONE |
| T4 | FLOAT/DOUBLE: матчер + footer | — | оба юнит-сьюта | DONE |
| T5 | BOOL: матчер + footer | — | оба юнит-сьюта | DONE |
| T6 | `UuidStats` + компараторы | — | `match_predicate_ut.cpp` | DONE |
| T7 | UUID-статистики из footer | T6 | `yql_arrow_push_down_ut.cpp` | DONE |
| T8 | `UuidType` + сериализация `TCoUuid` | T6 | `pushdown_ut.cpp` | DONE |
| T9 | Skip на file-path reader | T3 | юнит со счётчиком `ReadRowGroup` | DONE |
| T10 | FQ: INT + `IngressBytes` | T0, T3 | `test_format_setting.py` | DONE |
| T11 | FQ: UUID + endianness | T0, T7, T8 | `test_format_setting.py` | DONE |
| T12 | `StringStats` min/max + компараторы | этап 1 | `match_predicate_ut.cpp` | TODO |
| T13 | BYTE_ARRAY из footer + `StringTypes` | T12 | `yql_arrow_push_down_ut.cpp` | TODO |
| T14 | FQ: STRING + `IngressBytes` | T0, T12, T13 | `test_format_setting.py` | TODO |
| T15 | Page-level skip | T10–T14 в проде | эпик, не планировать сейчас | TODO |

```
T0 ──────────────────────────────────────────┐
T1                                           │
T2 ─► T3 ─► T9 ─┐                            │
T4              │                            │
T5              ├─► T10 FQ INT ──────────────┤
T6 ─► T7 ───────┤                            │
    └► T8 ─► T11 FQ UUID ────────────────────┤
                                             ▼
                                    этап 1 (T0–T11)

T12 ─► T13 ─► T14     этап 2
T15                   позже
```

Параллелить после знакомства с кодом: `{T0, T1, T2, T4, T5, T6}`.

Общая таблица компараторов min/max (T2, T4, T6, T12), колонка слева, константа `c`, диапазон `[lo, hi]`:

| Op | Оставить группу (`MatchPredicate == true`), если |
|---|---|
| EQ | `lo <= c <= hi` |
| NE | `c < lo \|\| hi < c`; если `c` строго внутри широкого диапазона — **true** (читать) |
| LE | `lo <= c` |
| L | `lo < c` |
| GE | `c <= hi` |
| G | `c < hi` |
| BETWEEN [a, b] | `lo <= b && hi >= a` |

Хелпер извлечения (завести в T1, наращивать в T3/T4/T5/T7/T13):

```cpp
TMaybe<TColumnStatistics> MakeStatistics(const parquet::ColumnDescriptor* col,
                                         const parquet::ColumnChunkMetaData* chunk);
```

- `if (!chunk->is_stats_set()) return {};`
- `auto st = chunk->statistics(); if (!st || !st->HasMinMax()) return {};`
- switch по `physical_type()` + `logical_type()`, имя `col->name()`.

### 5.2. Карта файлов

| Файл | Задачи |
|---|---|
| `ydb/library/yql/providers/s3/actors/yql_s3_read_actor.cpp` | T0, T9 |
| `ydb/library/yql/providers/s3/actors/yql_arrow_push_down.cpp` | T1, T3, T4, T5, T7, T13 |
| `ydb/library/yql/providers/s3/actors/ut/yql_arrow_push_down_ut.cpp` | T1, T3, T4, T5, T7, T13 |
| `ydb/library/yql/providers/generic/pushdown/yql_generic_column_statistics.h` | T6, T12 |
| `ydb/library/yql/providers/generic/pushdown/yql_generic_match_predicate.cpp` | T2, T4, T5, T6, T12 |
| `ydb/library/yql/providers/generic/pushdown/ut/match_predicate_ut.cpp` | T2, T4, T5, T6, T12 |
| `ydb/library/yql/providers/generic/provider/yql_generic_predicate_pushdown.cpp` | T8 |
| `ydb/library/yql/providers/generic/provider/ut/pushdown/pushdown_ut.cpp` | T8 |
| `ydb/library/yql/providers/s3/provider/yql_s3_logical_opt.cpp` | T8, T13 |
| `ydb/tests/fq/s3/test_format_setting.py` | T0, T10, T11, T14 |

Эталон FQ-теста: `TestS3.test_s3_push_down_parquet` — один SQL без прагмы и с `pragma s3.UsePredicatePushdown = "true"`, одинаковый результат, `IngressBytes` с прагмой строго меньше.

### 5.3. Задачи этапа 1

---

#### T0. Prefetch: индекс файла, не индекс списка совпавших

**Зачем.** `WillNeedRowGroups({nextGroup})` при `hasPredicate` качает не ту группу: обе ветки ternary одинаковые. Decode берёт `matchedRowGroups[i]`, prefetch — `i`. На `{1, 3}` качается группа `1` файла вместо `3`.

**Сделать.** `yql_s3_read_actor.cpp`, все постановки следующей группы в prefetch:

```cpp
int rg = hasPredicate ? static_cast<int>(matchedRowGroups[nextGroup])
                      : static_cast<int>(nextGroup);
```

Просмотреть цикл prefetch/decode целиком.

**Зависимости.** Нет.

**Тесты.** `ydb/tests/fq/s3/test_format_setting.py` (timestamp уже умеем скипать):

1. Parquet с **≥3** row group, предикат оставляет **несмежные** группы (0 и 2), середина заведомо не подходит.
2. Среднюю группу сделать заметно больше (больше строк / длиннее значения), чтобы экономия I/O была однозначной.
3. Без прагмы и с прагмой: одинаковые строки.
4. `IngressBytes` с прагмой **строго меньше**.

Существующий `test_s3_push_down_parquet` **недостаточен**: две группы, совпадает последняя, баг prefetch на смежных индексах не виден.

**DoD.** FQ на несмежных группах зелёный; `test_s3_push_down_parquet` не сломан.

---

#### T1. Не вызывать min/max без `HasMinMax()` (DATE/TIMESTAMP)

**Зачем.** Сейчас `GetDateStatistics` / `GetTimestampStatistics` зовут `min()`/`max()` без проверки. Все NULL / writer без min-max → UB или ложный skip.

**Сделать.** Не класть колонку в map, если `!is_stats_set()` или `!HasMinMax()`. Единицы DATE/TIMESTAMP не менять. Завести `MakeStatistics`.

**Зависимости.** Нет.

**Тесты.** `yql_arrow_push_down_ut.cpp`:

1. Row group **без** `SetMinMax` на timestamp + предикат `< константа` → группа **остаётся**.
2. Регрессия: `SimplePushDown`, `FilterEverything`, `MatchSeveralRowGroups` — те же ожидания.

**DoD.** Оба пункта зелёные. Нет `min()`/`max()` при `!HasMinMax()`.

---

#### T2. Компараторы `LongStats` в `MatchPredicate`

**Зачем.** Матчер не умеет Int*. Без этого извлечённые INT-статистики бесполезны.

**Сделать.** Ветки `INT8/16/32/64` (UINT*, если константа влезает в `i64`). Константа из `int32_value` / `int64_value` / `uint32_value` / `uint64_value`. Непредставимая константа → `Unknown`. Таблица ops в §5.1. Симметрия «константа слева».

**Зависимости.** Нет.

**Тесты.** `match_predicate_ut.cpp`, stats `[lo, hi] = [10, 20]`:

| Кейс | Предикат | Ожидание | Зачем |
|---|---|---|---|
| EQ внутри | `col = 15` | true | пересечение |
| EQ ниже | `col = 5` | false | skip |
| EQ на `lo` / `hi` | `= 10`, `= 20` | true | граница |
| L полностью левее | `col < 10` | false | skip |
| L пересекает | `col < 15` | true | читать |
| G полностью правее | `col > 20` | false | skip |
| GE на hi | `col >= 20` | true | |
| NE точка | stats `[5,5]`, `!= 5` | false | весь диапазон равен 5 |
| NE внутри широкого | `[10,20]`, `!= 15` | true | в группе есть и 15, и другие |
| BETWEEN мимо | `BETWEEN 1 AND 5` | false | |
| BETWEEN пересечение | `BETWEEN 15 AND 25` | true | |
| нет LongStats | пустая статистика | true | Unknown → читать |
| чужой тип | TIMESTAMP-константа vs LongStats | true | не скипать |

Регрессия существующих timestamp-тестов в том же suite.

**DoD.** Таблица зелёная. Integer больше не падает в `// TODO: other types`.

---

#### T3. INT-статистики из parquet footer → отсечение row group

**Зачем.** Связка footer → `LongStats` → матчер T2.

**Сделать.** Physical INT32/INT64, logical INT или NONE, **signed** sort order → `LongStats`. Unsigned не извлекать. В UT builder — `AddColumnInt64Statistics` / `AddColumnInt32Statistics`.

**Зависимости.** T2.

**Тесты.** `yql_arrow_push_down_ut.cpp`:

1. Одна группа INT64 `[100, 200]`, `col < 50` → 0 групп.
2. Та же группа, `col > 50` → `[0]`.
3. Две группы `[0, 10]` и `[100, 200]`, `col >= 150` → `[1]`.
4. INT32: хотя бы skip + keep.
5. Колонка без stats + INT-предикат → группа остаётся.
6. Регрессия timestamp-тестов.

**DoD.** INT-предикат отсекает группы на синтетическом `FileMetaData`, data pages не нужны.

---

#### T4. FLOAT/DOUBLE: матчер + footer

**Сделать.** `DoubleStats` в матчере (NaN в min/max → Unknown). `MatchRowGroup`: physical FLOAT/DOUBLE.

**Зависимости.** Нет.

**Тесты.**

`match_predicate_ut.cpp`:

1. `[1.0, 3.0]`, `= 2.0` → true; `= 0.0` → false.
2. `< 1.0` → false; `> 3.0` → false.
3. `low = NaN` или `high = NaN` → true (не скипать).

`yql_arrow_push_down_ut.cpp`:

4. DOUBLE min/max вне константы → 0 групп; пересечение → `[0]`.

**DoD.** Skip/keep для DOUBLE на обоих слоях. NaN не даёт skip.

---

#### T5. BOOL: матчер + footer

**Сделать.** `col = true` → skip, если `numTrues == 0`; `= false` → skip, если `numFalses == 0`. Иначе min/max 0/1. Physical BOOLEAN → `BooleanStats`.

**Зависимости.** Нет.

**Тесты.**

`match_predicate_ut.cpp`:

1. `numTrues=0`, `numFalses>0`, `= true` → false.
2. То же, `= false` → true.
3. Оба счётчика > 0 → любой EQ true (смешанная группа).
4. Нет BooleanStats → true.

`yql_arrow_push_down_ut.cpp`:

5. Синтетическая группа «все false», предикат `= true` → 0 групп.

**DoD.** Skip только если значение заведомо отсутствует в группе.

---

#### T6. Модель `UuidStats` и компараторы UUID

**Зачем.** 16-байтный тип отдельно от строк. Нужен до footer и до сериализации литералов.

**Сделать.** `TUuidColumnStatsData { lowValue, highValue }` (`TString` 16 байт), поле `UuidStats`. Константа: `type_id == UUID`, 16 байт = `low_128` (LE ui64, байты 0..7) + `high_128` (8..15), как `yql_kikimr_provider`. Сравнение `memcmp`. Длина ≠ 16 → Unknown.

**Зависимости.** Нет.

**Тесты.** `match_predicate_ut.cpp`, хелперы `BuildUuidStats` / `BuildUuidTypedValue`:

1. EQ внутри `[lo, hi]` → true; лексикографически `< lo` или `> hi` → false.
2. EQ на `lo` и на `hi` → true.
3. `< lo` → false; `> hi` → false.
4. NE: `lo==hi==c` → false; широкий диапазон, `c` внутри → true.
5. BETWEEN без пересечения → false; с пересечением → true.
6. `type_id != UUID` при UuidStats → true.

Зафиксировать в тесте конкретные `low_128`/`high_128` и 16 байт min — контракт для T7/T8.

**DoD.** Таблица UUID зелёная; layout константы зафиксирован ассертами.

---

#### T7. UUID-статистики из footer (только logical UUID)

**Сделать.** `logical UUID` + `FIXED_LEN_BYTE_ARRAY` length 16 + `HasMinMax()` → 16 сырых байт без перестановки. `NONE` + FLBA(16) тоже трактуем как Uuid: pyarrow 5 пишет Uuid как `fixed_size_binary(16)` без UUID logical type; DECIMAL отличается logical type DECIMAL.

**Зависимости.** T6.

**Тесты.** `yql_arrow_push_down_ut.cpp`:

1. UUID-поле, EQ вне диапазона → 0 групп.
2. EQ внутри → `[0]`.
3. Две группы с непересекающимися диапазонами; константа из второй → `[1]`.
4. `fixed_size_binary(16)` **без** UUID logical type + UUID-предикат вне диапазона → группа **отсекается** (совместимость с pyarrow 5).
5. Регрессия timestamp.

**DoD.** Skip по UUID на metadata; non-UUID FLBA не скипается.

---

#### T8. UUID-предикат от SQL до proto: `UuidType` + `TCoUuid`

**Зачем.** Без флага `CollectPredicates` режет Uuid; без сериализации `TCoUuid` — `unknown expression`.

**Сделать.**

1. `TPushdownSettings` S3: `Enable(EFlag::UuidType)`.
2. `SerializeExpression`: не `MATCH_ATOM`, два поля proto:

```cpp
type->set_type_id(Ydb::Type::UUID);
value->set_low_128(...);
value->set_high_128(...);
```

Литерал — 16 байт, разложение как в T6. `CAST`/`ToString` над Uuid не пушить.

**Зависимости.** T6 (тот же layout). E2e — T11.

**Тесты.** `ydb/library/yql/providers/generic/provider/ut/pushdown/pushdown_ut.cpp`:

1. Фильтр `id = Uuid("...")` → proto `comparison`, `type_id: UUID`, `low_128`/`high_128` совпадают с контрактом T6.
2. Регрессия сериализации Int/Timestamp в том же UT.

Если в suite нет S3-оптимизатора: этого UT сериализации достаточно для T8; полноту цепочки закрывает T11.

**DoD.** Литерал Uuid сериализуется в ожидаемый proto; у S3 включён `UuidType`.

---

#### T9. Тот же skip на file-path reader

**Зачем.** `RunCoroBlockArrowParserOverFile` игнорирует `Predicate`.

**Сделать.** Итерировать только `MatchedRowGroups`. Общая функция выбора индексов с HTTP-путём (чтобы индекс из T0 не разъехался).

**Зависимости.** T3.

**Тесты.** FQ ходит в HTTP, file:// сам не закроет. Нужен юнит со **счётчиком** прочитанных групп:

1. Записать маленький parquet (arrow/parquet writer в UT) с 2+ row group INT.
2. Предикат отсекает одну группу.
3. Вызвать тот же API, что file-path (`FileReader::ReadRowGroup` по списку `MatchedRowGroups`).
4. Assert: число прочитанных групп = числу matched, не `num_row_groups`.

Если coro-UT слишком дорогой: вынести `RowGroupsToRead(...)` и тестировать её плюс assert в code review, что file-цикл не ходит `0..n-1` без фильтра. Предпочтителен счётчик `ReadRowGroup`.

**DoD.** Отсечённая группа не читается на file-path API (тест со счётчиком).

---

#### T10. FQ: INT + `IngressBytes`

**Зачем.** SQL `WHERE` по Int реально не качает лишние chunk’и.

**Сделать.** Склейка T0+T3, нового рантайма сверх них нет.

**Зависимости.** T0, T3.

**Тесты.** `test_format_setting.py`:

1. Pyarrow, маленький `row_group_size`, колонки `price Int32`, `fruit Utf8`.
2. Часть групп с `price < 15`, часть `>= 15`.
3. `WHERE price >= 15` без прагмы и с прагмой: одинаковые строки.
4. `IngressBytes` с прагмой **строго меньше**.
5. Предикат на все группы (`price >= 0`): результаты равны.
6. Регрессия `test_s3_push_down_parquet`.

**DoD.** INT-фильтр в FQ экономит I/O и не теряет строки.

---

#### T11. FQ: UUID + endianness

**Зачем.** Единственная проверка, что pyarrow UUID, YQL `Uuid("...")`, proto `low_128`/`high_128` и parquet min/max — один layout.

**Сделать.** Склейка T0+T7+T8.

**Зависимости.** T0, T7, T8.

**Тесты.** `test_format_setting.py`:

1. Pyarrow `pa.uuid()` (logical UUID), несколько row group с разными диапазонами.
2. `WHERE id = Uuid("<значение из второй группы>")` без прагмы и с прагмой.
3. **Одинаковый результат.** Если байты разъехались — без прагмы другой набор строк: **не скипать**, чинить layout, не ослаблять assert.
4. `IngressBytes` с прагмой меньше, если константа в одной группе.
5. UUID вне всех диапазонов → 0 строк в обоих режимах.

**DoD.** FQ UUID-фильтр корректен и экономит I/O. Сломанный endianness падает на п.3.

---

### 5.4. Задачи этапа 2 (строки)

После T0–T11.

---

#### T12. `StringStats` min/max и компараторы строк

**Сделать.** `lowValue`/`highValue` в `TStringColumnStatsData`. Компараторы для `STRING`/`UTF8`, unsigned byte order. `CONTAINS` / `LIKE %x%` → Unknown.

**Зависимости.** Этап 1 готов.

**Тесты.** `match_predicate_ut.cpp`:

1. `[apple, pear]`, `= "banana"` → true; `= "aardvark"` → false; `= "zebra"` → false.
2. EQ на `apple` / `pear` → true.
3. `< "apple"` → false; `> "pear"` → false.
4. Нет StringStats → true.

**DoD.** Строковый матчер покрыт skip/keep без parquet.

---

#### T13. BYTE_ARRAY из footer + `StringTypes`

**Сделать.** BYTE_ARRAY + STRING/NONE → StringStats. `Enable(StringTypes)` в S3. Не смешивать с UUID FLBA.

**Зависимости.** T12.

**Тесты.** `yql_arrow_push_down_ut.cpp`:

1. Две группы с разными строковыми диапазонами; EQ попадает в одну → один индекс.
2. UUID-колонка + строковый предикат не использует UuidStats как строки.

**DoD.** Footer-skip по STRING; оптимизатор пушит строковые сравнения.

---

#### T14. FQ: STRING + `IngressBytes`

**Зависимости.** T0, T12, T13.

**Тесты.** `test_format_setting.py`: как T10, `WHERE fruit = "Pear"`, равенство результата, меньший `IngressBytes`.

**DoD.** Строковый фильтр в FQ экономит I/O.

---

### 5.5. Позже / вне скоупа

#### T15. Page-level skip (эпик)

Column Index / Offset Index. В `contrib/libs/apache/arrow` нет `PageIndexReader` (есть в `arrow_next`). Нужен общий набор row ranges по колонкам группы.

**Тесты (когда дойдут):** parquet с column index, одна большая группа, фильтр отсекает часть страниц; `IngressBytes` меньше row-group-only skip; строки совпадают с полным чтением.

Браться, когда профили после T10/T11/T14 покажут I/O внутри оставшихся групп.

**Вне скоупа:** DECIMAL, UINT widening, `IN`, `IS NULL`, касты, `UsePredicatePushdown=true` по умолчанию, CSV/JSON.

### 5.6. Сборка в PR

| PR | Задачи | Можно вливать, когда |
|---|---|---|
| **PR-A** | T0 | зелёный FQ на несмежных timestamp-группах |
| **PR-B1** | T1, T2, T3 | юниты INT + HasMinMax |
| **PR-B2** | T4, T5 | юниты DOUBLE/BOOL |
| **PR-B3** | T6, T7, T8 | юниты UUID + сериализация |
| **PR-B4** | T9, T10, T11 | FQ INT и UUID, file-path |
| **PR-C** | T12–T14 | FQ STRING |

B1–B3 можно одним PR; B4 не смешивать с рефакторингом матчера.

Новых прагм нет: `s3.UsePredicatePushdown`.

---

## 6. Риски

| Риск | Митигация |
|---|---|
| Ложный skip (потеря строк) | только доказанный False; `Unknown` → читать; `FlatMap` остаётся |
| Несовпадение единиц DATE/TIMESTAMP | юнит на DATE; не менять текущий timestamp-путь без теста |
| Signed vs unsigned INT | unsigned без надёжного min/max → не извлекать |
| Double NaN / -0.0 | NaN → Unknown |
| UUID endianness (parquet RFC bytes vs YQL `low_128`/`high_128`) | сравнивать 16 байт в layout конвертера (identity FLBA); интеграционный тест pyarrow↔YQL; при расхождении — не скипать |
| Голый FLBA(16) без logical UUID | трактовать как Uuid (pyarrow 5); DECIMAL не задеть из-за DECIMAL logical type |
| Имя колонки в nested schema | пока только flat schema, как сейчас `column->name()` |
| Рост CPU на матчинг | footer уже прочитан; число групп обычно невелико; ранний выход на первом False в `AND` |
| Тест `IngressBytes` флапает | несколько row group заведомо разного размера; порог «строго меньше», не точное равенство байт |

## 7. Справка: как устроен skip на I/O

После открытия reader в памяти есть `FileMetaData`:

- число row group;
- для каждого chunk: `file_offset`, `total_compressed_size`, optional statistics min/max.

HTTP-читалка (`THttpRandomAccessFile::ReadAt`) качает только запрошенные диапазоны.

`WillNeedRowGroups({k}, columnIndices)` префетчит chunk’и **только группы k** и **только выбранных колонок**. Если `k` нет в `matchedRowGroups`, этих байт в `IngressBytes` не будет.

Поэтому достаточно корректного списка групп: отдельный «не читай колонку X» внутри оставшейся группы для filter-колонки не даёт выигрыша, если `X` есть в проекции, и ломает выравнивание строк, если `X` выкинуть.

## 8. Связанный существующий тест как эталон

`TestS3.test_s3_push_down_parquet` в `ydb/tests/fq/s3/test_format_setting.py`:

- пишет parquet с `row_group_size=2`;
- выполняет один и тот же SQL без прагмы и с `pragma s3.UsePredicatePushdown = "true"`;
- проверяет одну результирующую строку;
- `assert without_predicate_ingress_bytes > with_predicate_ingress_bytes`.

Новые интеграционные тесты копировать этот каркас, сменив тип колонки и предикат.
