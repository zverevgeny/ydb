# Анализ переименования колоночных таблиц с настроенным TTL/Tiering в S3

Проведён детальный анализ кода операции `MoveTable` для колоночных таблиц (column tables) с настроенным TTL и tiering (эвикция данных в external storage/S3). Ниже — выявленные проблемы и риски.

---

## Ключевая архитектура

### TTL/Tiering в SchemeShard

TTL настройки для колоночных таблиц хранятся в:
- **In-memory**: `TColumnTableInfo::Description.GetTtlSettings()` (тип `NKikimrSchemeOp::TColumnDataLifeCycle`)
- **Persistent storage**: TTL settings сериализуются вместе с `Description` в `Schema::ColumnTables::Description`

### Tiering (эвикция в S3)

Tiering использует tiers из TTL settings:
```cpp
// table.cpp:41
THashSet<TString> TColumnTableInfo::GetUsedTiers() const {
    THashSet<TString> tiers;
    for (const auto& tier : Description.GetTtlSettings().GetEnabled().GetTiers()) {
        if (tier.HasEvictToExternalStorage()) {
            tiers.emplace(tier.GetEvictToExternalStorage().GetStorage());
        }
    }
    return tiers;
}
```

Каждый tier с `EvictToExternalStorage` указывает путь к external data source (например, `/Root/s3-storage`). При создании/изменении таблицы устанавливается ссылка на external data source через `PersistExternalDataSourceReference`.

### TTL версии в ColumnShard

В columnshard TTL версии хранятся в `TableVersionInfo` таблице, ключенной по **InternalPathId** (не меняется при rename):
```cpp
// columnshard_schema.h:249
struct TableVersionInfo: Table<(ui32)ECommonTables::TableVersionInfo> {
    struct PathId: Column<1, NScheme::NTypeIds::Uint64> {};  // InternalPathId
    struct SinceStep: Column<2, NScheme::NTypeIds::Uint64> {};
    struct SinceTxId: Column<3, NScheme::NTypeIds::Uint64> {};
    struct InfoProto: Column<4, NScheme::NTypeIds::String> {};   // TTableVersionInfo с TtlSettings
    using TKey = TableKey<PathId, SinceStep, SinceTxId>;
};
```

---

## Проблема 1: Ссылки на External Data Sources не мигрируются при MoveTable

### Как работает при Create

При создании таблицы с tiering ([`create_table.cpp:819`](ydb/core/tx/schemeshard/olap/operations/create_table.cpp:819)):
```cpp
for (const auto& tier : tableInfo->GetUsedTiers()) {
    auto tierPath = TPath::Resolve(tier, context.SS);
    AFL_VERIFY(tierPath.IsResolved())("path", tier);
    context.SS->PersistExternalDataSourceReference(db, tierPath->PathId, dstPath);
}
```

### Как работает при Drop

При удалении таблицы ([`drop_table.cpp:325`](ydb/core/tx/schemeshard/olap/operations/drop_table.cpp:325)):
```cpp
for (const auto& tier : tableInfo->GetUsedTiers()) {
    auto tierPath = TPath::Resolve(tier, context.SS);
    AFL_VERIFY(tierPath.IsResolved())("path", tier);
    context.SS->PersistRemoveExternalDataSourceReference(db, tierPath->PathId, txState->TargetPathId);
}
```

### Что происходит при Move

В [`schemeshard__operation_move_table.cpp`](ydb/core/tx/schemeshard/schemeshard__operation_move_table.cpp) **нет ни одной операции** с `PersistExternalDataSourceReference` или `PersistRemoveExternalDataSourceReference`.

**Результат**:
- Ссылка на external data source остаётся привязанной к **старому** path id
- Новый path id **не получает** ссылку на external data source
- После ребута SchemeShard таблица не сможет найти external data source для tiering
- **Tiering в S3 перестанет работать** для переименованной таблицы

---

## Проблема 2: TTL settings сохраняются, но tiering не активируется на destination path

TTL settings сериализуются в `Description` и копируются при move (через `PersistColumnTable`), поэтому после ребута TTL настройки восстановятся. Однако:

1. `GetUsedTiers()` вернёт те же tiers, но ссылка на external data source не будет установлена для нового path
2. ColumnShard при загрузке попытается активировать tiers через `ActivateTiering()`, но external data source не будет найден по новому path id

---

## Проблема 3: TTL версии в ColumnShard привязаны к InternalPathId

Это **положительный момент**: TTL версии в columnshard ключены по `InternalPathId`, который не меняется при rename. Поэтому:
- TTL история версий сохраняется
- ColumnShard продолжит работать с правильными TTL настройками

Но это не решает проблему на уровне SchemeShard (Проблема 1).

---

## Проблема 4: GenerateInternalPathId и MoveTable в ColumnShard

В columnshard при [`MoveTablePropose()`](ydb/core/tx/columnshard/tables_manager.cpp:663):
```cpp
const auto& internalPathId = ResolveInternalPathId(srcSchemeShardLocalPathId, false);
AFL_VERIFY(internalPathId);
```

При `GenerateInternalPathId = false`, `ResolveInternalPathId` может вернуть `nullopt`, что приведёт к AFL-падению.

---

## Проблема 5: Shared pointer aliasing

При move column table используется `shared_ptr` копирование:
```cpp
auto tableInfo = context.SS->ColumnTables.BuildNew(dstPath.Base()->PathId, srcTable.GetPtr());
```

Оба path указывают на один объект `TColumnTableInfo`. Concurrent alter может затронуть оба пути.

---

## Итоговая таблица рисков

| Риск | Влияние | Вероятность |
|------|---------|-------------|
| External data source reference не мигрирован | Критическое — tiering в S3 перестанет работать | Высокая |
| TTL settings сохраняются но без ссылки на storage | Критическое — эвикция данных не работает | Высокая |
| AFL при GenerateInternalPathId=false | Падение columnshard | Средняя |
| Shared pointer aliasing | Коррупция при concurrent alter | Низкая |

---

## Рекомендация

В [`TPropose::HandleReply(TEvOperationPlan)`](ydb/core/tx/schemeshard/schemeshard__operation_move_table.cpp:238) для column tables необходимо добавить:

### 1. Установить ссылки на external data sources для destination path
```cpp
if (srcPath->IsColumnTable()) {
    auto srcTable = context.SS->ColumnTables.GetVerified(srcPath.Base()->PathId);
    for (const auto& tier : srcTable->GetUsedTiers()) {
        auto tierPath = TPath::Resolve(tier, context.SS);
        AFL_VERIFY(tierPath.IsResolved())("path", tier);
        context.SS->PersistExternalDataSourceReference(db, tierPath->PathId, dstPath);
    }
}
```

### 2. Удалить ссылки на external data sources для source path
В `MarkSrcDropped()` добавить для column tables:
```cpp
if (srcPath->IsColumnTable()) {
    auto srcTable = context.SS->ColumnTables.GetVerified(srcPath->PathId);
    for (const auto& tier : srcTable->GetUsedTiers()) {
        auto tierPath = TPath::Resolve(tier, context.SS);
        AFL_VERIFY(tierPath.IsResolved())("path", tier);
        context.SS->PersistRemoveExternalDataSourceReference(db, tierPath->PathId, srcPath->PathId);
    }
}
```
