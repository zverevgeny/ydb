# Анализ экспорта/импорта колоночных таблиц с настроенным TTL/Tiering в S3

Проведён детальный анализ кода операций Export/Import для колоночных таблиц (column tables) с настроенным TTL и tiering (эвикция данных в external storage/S3). Ниже — выявленные проблемы и риски.

---

## Ключевая архитектура

### Export (экспорт данных)

Export состоит из нескольких этапов ([`schemeshard_export__create.cpp`](ydb/core/tx/schemeshard/schemeshard_export__create.cpp:106)):
1. **CreateExportDir** — создание директории для экспорта
2. **CopyTables** — создание консистентных копий таблиц через `ConsistentCopyTables` ([`schemeshard_export_flow_proposals.cpp:43`](ydb/core/tx/schemeshard/schemeshard_export_flow_proposals.cpp:43))
3. **Transferring** — бэкап каждой копии в S3/YT/FS через `BackupPropose` ([`schemeshard_export_flow_proposals.cpp:238`](ydb/core/tx/schemeshard/schemeshard_export_flow_proposals.cpp:238))
4. **Dropping** — удаление временных копий

Export использует `TBackupTask` ([`schemeshard_export_flow_proposals.cpp:250`](ydb/core/tx/schemeshard/schemeshard_export_flow_proposals.cpp:250)) который сериализует схему таблицы но **не включает TTL settings** в экспортируемые данные.

### Import (импорт данных)

Import состоит из нескольких этапов ([`schemeshard_import__create.cpp`](ydb/core/tx/schemeshard/schemeshard_import__create.cpp:466)):
1. **GetScheme** — загрузка схемы таблицы из S3/FS ([`schemeshard_import_getters.cpp:1019`](ydb/core/tx/schemeshard/schemeshard_import_getters.cpp:1019))
2. **CreateSchemeObject** — создание таблицы через `CreateTablePropose` ([`schemeshard_import_flow_proposals.cpp:47`](ydb/core/tx/schemeshard/schemeshard_import_flow_proposals.cpp:47))
3. **Transferring** — восстановление данных через `RestoreTableDataPropose` ([`schemeshard_import_flow_proposals.cpp:215`](ydb/core/tx/schemeshard/schemeshard_import_flow_proposals.cpp:215))
4. **BuildIndexes** — построение индексов (для row-таблиц)

### TTL/Tiering в контексте Export/Import

TTL настройки хранятся в:
- **In-memory**: `TColumnTableInfo::Description.GetTtlSettings()` (тип `NKikimrSchemeOp::TColumnDataLifeCycle`)
- **Persistent storage**: TTL settings сериализуются вместе с `Description` в `Schema::ColumnTables::Description`

Tiering использует tiers из TTL settings. Каждый tier с `EvictToExternalStorage` указывает путь к external data source (например, `/Root/s3-storage`).

---

## Проблема 1: TTL/Tiering настройки не экспортируются

### Как работает экспорт схемы

При экспорте схема таблицы сохраняется через `FillTableDescription` ([`schemeshard_export_flow_proposals.cpp:198`](ydb/core/tx/schemeshard/schemeshard_export_flow_proposals.cpp:198)):
```cpp
void FillTableDescription(TSchemeShard* ss, NKikimrSchemeOp::TBackupTask& task, const TPath& sourcePath, const TPath& exportItemPath) {
    // ...
    task.MutableTable()->CopyFrom(sourceDescription);  // копирует описание таблицы
}
```

Для колоночных таблиц используется `DescribePath` который возвращает `TColumnTableDescription`. Однако:

### Что не экспортируется

1. **TTL settings** — настройки жизненного цикла данных не включаются в экспорт
2. **External data source references** — ссылки на внешние хранилища не сохраняются
3. **Tiering configuration** — конфигурация эвикции в S3 не переносится

**Результат**: После импорта таблица теряет TTL/tiering конфигурацию. Данные, которые должны были эвиктироваться в S3, останутся в горячем хранилище.

---

## Проблема 2: Import создает таблицу без TTL/tiering

### Как работает создание таблицы при импорте

`CreateTablePropose` ([`schemeshard_import_flow_proposals.cpp:47`](ydb/core/tx/schemeshard/schemeshard_import_flow_proposals.cpp:47)) создает таблицу:
```cpp
if (isColumnTable) {
    auto& tableDesc = *modifyScheme.MutableCreateColumnTable();
    tableDesc.SetName(wdAndPath.second);
    tableDesc.SetIsRestore(true);

    Ydb::StatusIds::StatusCode status;
    if (!FillColumnTableDescription(modifyScheme, *item.Table, status, error)) {
        return nullptr;
    }
}
```

`FillColumnTableDescription` заполняет описание таблицы из импортированной схемы, но:
- Не восстанавливает TTL settings
- Не устанавливает tiering конфигурацию
- Не создает ссылки на external data sources

**Результат**: Импортированная таблица работает без tiering, что может привести к:
- Переполнению горячего хранилища
- Росту затрат на хранение
- Потере ожидаемого поведения эвикции данных

---

## Проблема 3: External data source references не устанавливаются при импорте

### Как работает при Create (нормальный случай)

При создании таблицы с tiering ([`create_table.cpp:819`](ydb/core/tx/schemeshard/olap/operations/create_table.cpp:819)):
```cpp
for (const auto& tier : tableInfo->GetUsedTiers()) {
    auto tierPath = TPath::Resolve(tier, context.SS);
    AFL_VERIFY(tierPath.IsResolved())("path", tier);
    context.SS->PersistExternalDataSourceReference(db, tierPath->PathId, dstPath);
}
```

### Что происходит при Import

В [`schemeshard_import_flow_proposals.cpp`](ydb/core/tx/schemeshard/schemeshard_import_flow_proposals.cpp) **нет ни одной операции** с `PersistExternalDataSourceReference`.

**Результат**:
- Даже если TTL settings каким-то образом сохранятся в схеме, ссылки на external data sources не будут установлены
- Tiering не активируется для импортированной таблицы
- ColumnShard не сможет найти external data source для эвикции

---

## Проблема 4: Данные в external storage не мигрируются при export/import

### Сценарий

1. Исходная таблица имеет данные в S3 (эвиктированные через tiering)
2. Export копирует только «горячие» данные из таблицы
3. Import восстанавливает данные в новую таблицу

### Проблема

- Export через `BackupPropose` бэкапит данные из копии таблицы
- Эвиктированные данные в external storage **не включаются** в экспорт
- После импорта данные из S3 теряются

**Результат**: Потеря данных, которые были эвиктированы в external storage перед экспортом.

---

## Проблема 5: Export с ConsistentCopy не переносит tiering

### Как работает CopyTables

`CopyTablesPropose` ([`schemeshard_export_flow_proposals.cpp:43`](ydb/core/tx/schemeshard/schemeshard_export_flow_proposals.cpp:43)) создает копии таблиц:
```cpp
auto& desc = *copyTables.Add();
desc.SetSrcPath(item.SourcePathName);
desc.SetDstPath(ExportItemPathName(ss, exportInfo, itemIdx));
desc.SetOmitIndexes(!exportInfo.IncludeIndexData);
desc.SetOmitFollowers(true);
desc.SetIsBackup(true);
```

### Проблема

- `ConsistentCopyTables` создает копию таблицы без TTL/tiering настроек
- Копия используется для бэкапа, но не наследует конфигурацию tiering
- После экспорта и импорта tiering не восстанавливается

---

## Проблема 6: Restore не активирует tiering для импортированных данных

### Как работает Restore

`RestoreTableDataPropose` ([`schemeshard_import_flow_proposals.cpp:215`](ydb/core/tx/schemeshard/schemeshard_import_flow_proposals.cpp:215)) восстанавливает данные:
```cpp
auto& task = *modifyScheme.MutableRestore();
task.SetTableName(dstPath.LeafName());
*task.MutableTableDescription() = RebuildTableDescription(GetTableDescription(ss, item.DstPathId), *item.Table);
```

### Проблема

- Restore загружает данные в таблицу, но не активирует tiering
- Даже если таблица была создана с TTL settings (что само по себе не происходит), tiering не будет работать без ссылок на external data sources
- Импортированные данные останутся в горячем хранилище

---

## Проблема 7: SchemaMapping не включает TTL/tiering метаданные

### Как работает SchemaMapping

При экспорте в S3/FS создается `SchemaMapping` с метаданными таблиц. Однако:
- TTL settings не включаются в schema mapping
- External data source references не сохраняются
- Tiering configuration не сериализуется

**Результат**: При импорте с использованием schema mapping, информация о tiering полностью теряется.

---

## Итоговая таблица рисков

| Риск | Влияние | Вероятность |
|------|---------|-------------|
| TTL/tiering настройки не экспортируются | Критическое — tiering теряется после импорта | Высокая |
| Import создает таблицу без tiering | Критическое — данные не эвиктируются | Высокая |
| External data source references не устанавливаются | Критическое — tiering не активируется | Высокая |
| Данные в external storage не мигрируются | Критическое — потеря эвиктированных данных | Средняя |
| ConsistentCopy не переносит tiering | Среднее — промежуточные копии без tiering | Высокая |
| Restore не активирует tiering | Критическое — импортированные данные без tiering | Высокая |
| SchemaMapping без TTL метаданных | Среднее — информация о tiering теряется | Высокая |

---

## Рекомендации

### Для Export

1. **Включить TTL settings в экспорт схемы** — добавить `TtlSettings` в `TBackupTask` при экспорте
2. **Сохранить external data source references** — добавить сериализацию ссылок на external storage
3. **Экспортировать эвиктированные данные** — обеспечить включение данных из external storage в бэкап

### Для Import

1. **Восстановить TTL settings при создании таблицы** — добавить десериализацию `TtlSettings` в `CreateTablePropose`
2. **Установить external data source references** — добавить вызов `PersistExternalDataSourceReference` после создания таблицы:
   ```cpp
   if (isColumnTable) {
       auto tableInfo = ss->ColumnTables.GetVerified(item.DstPathId);
       for (const auto& tier : tableInfo->GetUsedTiers()) {
           auto tierPath = TPath::Resolve(tier, ss);
           if (tierPath.IsResolved()) {
               ss->PersistExternalDataSourceReference(db, tierPath->PathId, item.DstPathId);
           }
       }
   }
   ```
3. **Активировать tiering после restore** — убедиться что tiering работает для импортированных данных

### Для SchemaMapping

1. **Добавить TTL/tiering метаданные** — расширить schema mapping формат для включения tiering конфигурации
2. **Сохранить ссылки на external storage** — добавить пути к external data sources в метаданные

---

## Применимость проблем к TTL без тиринга

Ниже приведён анализ того, какие из выявленных проблем относятся **только к тирингу** (эвикции в external storage), а какие затрагивают **любые настройки TTL**, включая простые настройки на удаление данных.

### Структура TTL settings

TTL настройки хранятся в `TColumnDataLifeCycle` ([`flat_scheme_op.proto:847`](ydb/core/protos/flat_scheme_op.proto:847)):

```protobuf
message TColumnDataLifeCycle {
    message TTtl {
        optional string ColumnName = 1;
        oneof Expire {
            uint32 ExpireAfterSeconds = 2;  // удаление по времени
            uint64 ExpireAfterBytes = 4;     // удаление по объёму
        }
        repeated TTTLSettings.TTier Tiers = 5;
    }
}
```

Каждый `TTier` ([`flat_scheme_op.proto:271`](ydb/core/protos/flat_scheme_op.proto:271)) может иметь одно из двух действий:

```protobuf
message TTier {
    oneof Action {
        google.protobuf.Empty Delete = 2;                          // удаление данных
        TEvictionToExternalStorageSettings EvictToExternalStorage = 3;  // тиринг в S3
    }
}
```

Таким образом, TTL может работать **без тиринга** — с tiers, содержащими только `Delete` action.

### Таблица применимости проблем

Проблема | Только тиринг | TTL без тиринга | Пояснение |
|----------|:-------------:|:---------------:|-----------|
**П1**: TTL settings не экспортируются | ❌ | ✅ | [`FillTableDescription()`](ydb/core/tx/schemeshard/schemeshard_export_flow_proposals.cpp:198) копирует полное `TPathDescription` включая `TtlSettings` в `TBackupTask.Table`. Но [`BuildScheme()`](ydb/core/tx/schemeshard/schemeshard_scheme_builders.cpp:202) не обрабатывает `EPathTypeColumnTable`, и схема для column tables не генерируется через стандартный путь экспорта. **Все** TTL settings теряются. |
**П2**: Import создает таблицу без TTL | ❌ | ✅ | [`FillColumnTableDescription()`](ydb/core/ydb_convert/table_description.cpp:2827) → [`FillCreateTableSettingsDesc()`](ydb/core/ydb_convert/table_settings.cpp:246) обрабатывает `ttl_settings` (строки 281-285) **если** они присутствуют в `CreateTableRequest`. Но так как схема не экспортируется правильно, TTL settings не будут в импортированных данных. |
**П3**: External data source references не устанавливаются | ✅ | ❌ | `PersistExternalDataSourceReference` требуется **только** для tiers с `EvictToExternalStorage`. Чистое удаление (`Delete`) не нуждается в external data source. |
**П4**: Данные в external storage не мигрируются | ✅ | ❌ | Относится только к данным, эвиктированным в S3. При чистом TTL-удалении данные не хранятся в external storage. |
**П5**: ConsistentCopy не переносит tiering | ❌ | ✅ | Копия таблицы не наследует **все** TTL settings, включая настройки на удаление. |
**П6**: Restore не активирует tiering | Частично | Частично | Если TTL settings каким-то образом сохранятся, то чистое удаление (`Delete`) будет работать без дополнительных действий. Но тиринг не активируется без `PersistExternalDataSourceReference`. |
**П7**: SchemaMapping без TTL метаданных | ❌ | ✅ | SchemaMapping не включает **все** TTL settings, включая настройки на удаление. |

### Вывод

- **Проблемы П1, П2, П5, П7** затрагивают **все** настройки TTL, включая простые настройки на удаление без тиринга.
- **Проблемы П3, П4** относятся **только** к тирингу с эвикцией в external storage.
- **Проблема П6** частично затрагивает оба случая: чистое удаление может работать, если TTL settings сохранены, но тиринг требует дополнительных шагов.

### Критичность для TTL без тиринга

Для таблиц с TTL на удаление (без тиринга):
- После export/import таблица **потеряет** TTL настройки на удаление (П1, П2)
- Данные **не будут автоматически удаляться** по истечении срока хранения
- Это может привести к **неограниченному росту** таблицы и **переполнению** хранилища

---

## Связанные файлы

- [`schemeshard_export__create.cpp`](ydb/core/tx/schemeshard/schemeshard_export__create.cpp) — создание экспорта
- [`schemeshard_export_flow_proposals.cpp`](ydb/core/tx/schemeshard/schemeshard_export_flow_proposals.cpp) — proposals для этапов экспорта
- [`schemeshard_import__create.cpp`](ydb/core/tx/schemeshard/schemeshard_import__create.cpp) — создание импорта
- [`schemeshard_import_flow_proposals.cpp`](ydb/core/tx/schemeshard/schemeshard_import_flow_proposals.cpp) — proposals для этапов импорта
- [`schemeshard_import_getters.cpp`](ydb/core/tx/schemeshard/schemeshard_import_getters.cpp) — загрузка схемы из S3/FS
- [`olap/operations/create_table.cpp`](ydb/core/tx/schemeshard/olap/operations/create_table.cpp) — создание таблицы с tiering (референс)
- [`olap/operations/drop_table.cpp`](ydb/core/tx/schemeshard/olap/operations/drop_table.cpp) — удаление таблицы с tiering (референс)

## Реализованный fix

### Проблема

Для таблиц с TTL (как с тирингом, так и без — чистое удаление): после экспорта/импорта таблица теряет настройки TTL (P1, P2), данные не будут автоматически удаляться после истечения срока, что может привести к неограниченному росту таблицы и переполнению хранилища.

### Причина

Функция `BuildScheme()` в [`schemeshard_scheme_builders.cpp`](ydb/core/tx/schemeshard/schemeshard_scheme_builders.cpp) не обрабатывала случай `EPathTypeColumnTable`, из-за чего файлы схемы для колоночных таблиц не генерировались при экспорте.

### Решение

1. **Добавлена функция `BuildColumnTableScheme()`** в [`schemeshard_scheme_builders.cpp`](ydb/core/tx/schemeshard/schemeshard_scheme_builders.cpp:202):
   - Конвертирует внутренний `TColumnTableDescription` (с `TColumnDataLifeCycle` TTL) в публичный `Ydb::Table::CreateTableRequest` (с `Ydb::Table::TtlSettings`)
   - Использует существующую функцию `FillTtlSettings()` из [`table_settings.cpp`](ydb/core/ydb_convert/table_settings.cpp:820) для конвертации TTL настроек
   - Копирует столбцы, первичный ключ, настройки партиционирования и TTL

2. **Добавлен случай `EPathTypeColumnTable`** в `BuildScheme()` switch statement ([`schemeshard_scheme_builders.cpp`](ydb/core/tx/schemeshard/schemeshard_scheme_builders.cpp:274))

3. **Добавлен тип файла `ColumnTableScheme`** в [`files.cpp`](ydb/public/lib/ydb_cli/dump/files/files.cpp) и [`files.h`](ydb/public/lib/ydb_cli/dump/files/files.h):
   - Файл схемы: `column_table_scheme.pb`
   - Содержит `CreateTableRequest` в TextFormat с TTL настройками

4. **Добавлен `ColumnTableSchema`** в enum `EBackupFileType` в [`encryption.h`](ydb/core/backup/common/encryption.h:79)

5. **Зарегистрирована колоночная таблица** в `GetXxportProperties()` в [`schemeshard_xxport__helpers.cpp`](ydb/core/tx/schemeshard/schemeshard_xxport__helpers.cpp:56):
   - Связывает файл `column_table_scheme.pb` с типом `EPathTypeColumnTable`

6. **Добавлен include** для [`table_settings.h`](ydb/core/ydb_convert/table_settings.h) в [`schemeshard_scheme_builders.cpp`](ydb/core/tx/schemeshard/schemeshard_scheme_builders.cpp:10)

### Тест

Добавлен тест `ExportImportColumnTableWithTtlDeleteOnly` в [`ut_export.cpp`](ydb/core/tx/schemeshard/ut_export/ut_export.cpp:5911):
- Создает колоночную таблицу с TTL настройками (только удаление, без тиринга)
- Экспортирует таблицу
- Проверяет, что файл схемы содержит TTL настройки
- Импортирует таблицу
- Проверяет, что TTL настройки сохранены после импорта

### Поток экспорта/импорта

1. **Экспорт**: `BuildScheme()` → `BuildColumnTableScheme()` → serialize `CreateTableRequest` TextFormat → записать в `column_table_scheme.pb`
2. **Импорт**: чтение `column_table_scheme.pb` как `CreateTableRequest` → `FillColumnTableDescription()` → `FillCreateTableSettingsDesc()` → `FillTtlSettings()` (public → internal)
