# Обогащение данных

Обогащение данных — добавление к событиям из потока дополнительной информации из справочника. Например, событие содержит только идентификатор, а справочник позволяет добавить к нему название или другие атрибуты.

В [потоковых запросах](../../concepts/streaming-query.md) справочник подключают с помощью `JOIN`: слева остаётся поток (чтение из топика или промежуточный результат), справа — таблица в {{ ydb-short-name }} или данные из [объектного хранилища S3](#enrichment-s3).

## Обогащение из локальной таблицы {#enrichment-local-table}

Справочником может служить [таблица](../../concepts/datamodel/table.md) в текущей базе данных. Таблица должна существовать до запуска потокового запроса и содержать поля для соединения с потоком (например, общий идентификатор).

Ниже поток читает JSON из `input_topic`, подставляет название сервиса из таблицы `services_dict` по полю `ServiceId` и пишет результат в `output_topic`. Запрос создаётся через [CREATE STREAMING QUERY](../../yql/reference/syntax/create-streaming-query.md).

Подробнее о форматах данных в топике: [{#T}](streaming-query-formats.md).

```sql
CREATE STREAMING QUERY query_with_table_join AS
DO BEGIN

-- События из топика
$topic_data = SELECT
    *
FROM
    input_topic
WITH (
    FORMAT = json_each_row,
    SCHEMA = (
        Time String NOT NULL,
        ServiceId Uint32 NOT NULL,
        Message String NOT NULL
    )
);

-- Присоединение справочника из локальной таблицы
$joined_data = SELECT
    s.Name AS Name,
    t.*
FROM
    $topic_data AS t
LEFT JOIN
    services_dict AS s
ON
    t.ServiceId = s.ServiceId;

-- Запись в выходной топик (JSON)
INSERT INTO
    output_topic
SELECT
    ToBytes(Unwrap(Yson::SerializeJson(Yson::From(TableRow()))))
FROM
    $joined_data;

END DO
```

Концептуально: [{#T}](../../concepts/streaming-query.md#enrichment-ydb-tables).

Подробнее о функциях сериализации в JSON:

- [TableRow](../../yql/reference/builtins/basic#tablerow)
- [Yson::From](../../yql/reference/udf/list/yson#ysonfrom)
- [Yson::SerializeJson](../../yql/reference/udf/list/yson#ysonserializejson)
- [Unwrap](../../yql/reference/builtins/basic#unwrap)
- [ToBytes](../../yql/reference/builtins/basic#to-from-bytes).

## Обогащение из S3 {#enrichment-s3}

Справочник задаётся файлом в S3; в `JOIN` поток остаётся слева, выборка из файла через [внешнюю таблицу](../../concepts/query_execution/federated_query/s3/external_table.md) — справа.

{% note warning %}

Справочник из S3 полностью загружается в память при запуске запроса. Если данные в S3 изменились, для получения актуальной версии справочника необходимо перезапустить запрос — удалить его с помощью [DROP STREAMING QUERY](../../yql/reference/syntax/drop-streaming-query.md) и создать заново с помощью [CREATE STREAMING QUERY](../../yql/reference/syntax/create-streaming-query.md).

{% endnote %}

Справочник хранится в S3 и подключается через [внешний источник данных](../../concepts/query_execution/federated_query/s3/external_data_source.md).

### Подготовка источника данных для S3

Создайте [внешний источник данных](../../yql/reference/syntax/create-external-data-source.md) для чтения справочника из S3:

```sql
CREATE EXTERNAL DATA SOURCE s3_source WITH (
    SOURCE_TYPE = "ObjectStorage",
    LOCATION = "<s3_endpoint>",
    AUTH_METHOD = "NONE"
)
```

Где:

- `<s3_endpoint>` — URL S3-хранилища, например `https://storage.yandexcloud.net/<bucket>` для Yandex Cloud.

Входной и выходной [топики](../../concepts/datamodel/topic.md) (`input_topic`, `output_topic`) создаются в текущей базе данных обычным [CREATE TOPIC](../../yql/reference/syntax/create-topic.md).

### Потоковый запрос со справочником в S3

Запрос читает события из входного топика, присоединяет к каждому событию название сервиса из справочника по `ServiceId` и записывает результат в выходной топик.

```sql
CREATE STREAMING QUERY query_with_join AS
DO BEGIN

-- Чтение событий из входного топика
$topic_data = SELECT
    *
FROM
    input_topic
WITH (
    FORMAT = json_each_row,
    SCHEMA = (
        Time String NOT NULL,
        ServiceId Uint32 NOT NULL,
        Message String NOT NULL
    )
);

-- Чтение справочника сервисов из S3
$s3_data = SELECT
    *
FROM
    s3_source.`file.csv`
WITH (
    FORMAT = csv_with_names,
    SCHEMA = (
        ServiceId Uint32,
        Name Utf8
    )
);

-- Присоединение справочника к потоку по ServiceId
$joined_data = SELECT
    s.Name AS Name,
    t.*
FROM
    $topic_data AS t
LEFT JOIN
    $s3_data AS s
ON
    t.ServiceId = s.ServiceId;

-- Запись результата в выходной топик в формате JSON
INSERT INTO
    output_topic
SELECT
    ToBytes(Unwrap(Yson::SerializeJson(Yson::From(TableRow()))))
FROM
    $joined_data;

END DO
```

Те же функции сериализации, что и в [разделе про локальную таблицу](#enrichment-local-table).
