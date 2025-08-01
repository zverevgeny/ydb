# DateTime

В модуле DateTime существует два внутренних формата представления: `Resource<TM>` для базовых типов дат и `Resource<TM64>` для расширенных.

`Resource<TM>` и `Resource<TM64>` хранят следующие компоненты дат:
* Year (для `Resource<TM>` 12 бит, беззнаковый; для `Resource<TM64>` 19 бит, знаковый);
* Month (4 бита);
* Day (5 бит);
* Hour (5 бит);
* Minute (6 бит);
* Second (6 бит);
* Microsecond (20 бит);
* TimezoneId (16 бит);
* DayOfYear (9 бит) — день от начала года;
* WeekOfYear (6 бит) — неделя от начала года, 1 января всегда относится к первой неделе;
* WeekOfYearIso8601 (6 бит) — неделя года согласно ISO 8601 (первой неделей считается та, в которой 4 января);
* DayOfWeek (3 бита) — день недели.

Если таймзона не GMT, то в компонентах хранится локальное время в соответствующей таймзоне.

## Split {#split}

Преобразование из простого типа во внутреннее представление. Всегда успешно при непустом входе.

#### Список функций

* `DateTime::Split(Date/TzDate/Datetime/TzDatetime/Timestamp/TzTimestamp{Flags:AutoMap}) -> Resource<TM>`
* `DateTime::Split(Date32/TzDate32/Datetime64/TzDatetime64/Timestamp64/TzTimestamp64{Flags:AutoMap}) -> Resource<TM64>`

Функции, принимающие на вход `Resource<TM>` или `Resource<TM64>`, могут быть вызваны непосредственно от простого типа даты/времени. В этом случае будет сделано неявное преобразование через вызов соответствующей функции `Split`.

## Make... {#make}

Сборка простого типа из внутреннего представления. Всегда успешна при непустом входе.

#### Список функций

* `DateTime::MakeDate(Resource<TM>{Flags:AutoMap}) -> Date`
* `DateTime::MakeTzDate(Resource<TM>{Flags:AutoMap}) -> TzDate`
* `DateTime::MakeDate32(Resource<TM64>{Flags:AutoMap}) -> Date32`
* `DateTime::MakeTzDate32(Resource<TM64>{Flags:AutoMap}) -> TzDate32`
* `DateTime::MakeDatetime(Resource<TM>{Flags:AutoMap}) -> Datetime`
* `DateTime::MakeTzDatetime(Resource<TM>{Flags:AutoMap}) -> TzDatetime`
* `DateTime::MakeDatetime64(Resource<TM64>{Flags:AutoMap}) -> Datetime64`
* `DateTime::MakeTzDatetime64(Resource<TM64>{Flags:AutoMap}) -> TzDatetime64`
* `DateTime::MakeTimestamp(Resource<TM>{Flags:AutoMap}) -> Timestamp`
* `DateTime::MakeTzTimestamp(Resource<TM>{Flags:AutoMap}) -> TzTimestamp`
* `DateTime::MakeTimestamp64(Resource<TM64>{Flags:AutoMap}) -> Timestamp64`
* `DateTime::MakeTzTimestamp64(Resource<TM64>{Flags:AutoMap}) -> TzTimestamp64`

#### Примеры

```yql
SELECT
    DateTime::MakeTimestamp(DateTime::Split(Datetime("2019-01-01T15:30:00Z"))),
      -- 2019-01-01T15:30:00.000000Z
    DateTime::MakeDate(Datetime("2019-01-01T15:30:00Z")),
      -- 2019-01-01
    DateTime::MakeTimestamp(DateTime::Split(TzDatetime("2019-01-01T00:00:00,Europe/Moscow"))),
      -- 2018-12-31T21:00:00Z (конвертация в UTC)
    DateTime::MakeDate(TzDatetime("2019-01-01T12:00:00,GMT"))
      -- 2019-01-01 (Datetime -> Date с неявным Split)
```

## Get... {#get}

Взятие компоненты внутреннего представления.

#### Список функций

* `DateTime::GetYear(Resource<TM>{Flags:AutoMap}) -> Uint16`
* `DateTime::GetYear(Resource<TM64>{Flags:AutoMap}) -> Int32`
* `DateTime::GetDayOfYear(Resource<TM>/Resource<TM64>{Flags:AutoMap}) -> Uint16`
* `DateTime::GetMonth(Resource<TM>/Resource<TM64>{Flags:AutoMap}) -> Uint8`
* `DateTime::GetMonthName(Resource<TM>/Resource<TM64>{Flags:AutoMap}) -> String`
* `DateTime::GetWeekOfYear(Resource<TM>/Resource<TM64>{Flags:AutoMap}) -> Uint8`
* `DateTime::GetWeekOfYearIso8601(Resource<TM>/Resource<TM64>{Flags:AutoMap}) -> Uint8`
* `DateTime::GetDayOfMonth(Resource<TM>/Resource<TM64>{Flags:AutoMap}) -> Uint8`
* `DateTime::GetDayOfWeek(Resource<TM>/Resource<TM64>{Flags:AutoMap}) -> Uint8`
* `DateTime::GetDayOfWeekName(Resource<TM>/Resource<TM64>{Flags:AutoMap}) -> String`
* `DateTime::GetHour(Resource<TM>/Resource<TM64>{Flags:AutoMap}) -> Uint8`
* `DateTime::GetMinute(Resource<TM>/Resource<TM64>{Flags:AutoMap}) -> Uint8`
* `DateTime::GetSecond(Resource<TM>/Resource<TM64>{Flags:AutoMap}) -> Uint8`
* `DateTime::GetMillisecondOfSecond(Resource<TM>/Resource<TM64>{Flags:AutoMap}) -> Uint32`
* `DateTime::GetMicrosecondOfSecond(Resource<TM>/Resource<TM64>{Flags:AutoMap}) -> Uint32`
* `DateTime::GetTimezoneId(Resource<TM>/Resource<TM64>{Flags:AutoMap}) -> Uint16`
* `DateTime::GetTimezoneName(Resource<TM>/Resource<TM64>{Flags:AutoMap}) -> String`

#### Примеры

```yql
$tm = DateTime::Split(TzDatetime("2019-01-09T00:00:00,Europe/Moscow"));

SELECT
    DateTime::GetDayOfMonth($tm) as Day, -- 9
    DateTime::GetMonthName($tm) as Month, -- "January"
    DateTime::GetYear($tm) as Year, -- 2019
    DateTime::GetTimezoneName($tm) as TzName, -- "Europe/Moscow"
    DateTime::GetDayOfWeekName($tm) as WeekDay; -- "Wednesday"
```

## Update {#update}

Обновление одной или нескольких компонент во внутреннем представлении. Возвращает либо обновлённую копию, либо NULL, если после обновления получается некорректная дата или возникают другие противоречия.

#### Список функций

```yql
DateTime::Update(Resource<TM>{Flags:AutoMap}, [ Year:Uint16?, Month:Uint8?, Day:Uint8?, Hour:Uint8?, Minute:Uint8?, Second:Uint8?, Microsecond:Uint32?, Timezone:String? ]) -> Resource<TM>?
DateTime::Update(Resource<TM64>{Flags:AutoMap}, [ Year:Int32?, Month:Uint8?, Day:Uint8?, Hour:Uint8?, Minute:Uint8?, Second:Uint8?, Microsecond:Uint32?, Timezone:String? ]) -> Resource<TM64>?
```

#### Примеры

```yql
$tm = DateTime::Split(Timestamp("2019-01-01T01:02:03.456789Z"));

SELECT
    DateTime::MakeDate(DateTime::Update($tm, 2012)), -- 2012-01-01
    DateTime::MakeDate(DateTime::Update($tm, 2000, 6, 6)), -- 2000-06-06
    DateTime::MakeDate(DateTime::Update($tm, NULL, 2, 30)), -- NULL (30 февраля)
    DateTime::MakeDatetime(DateTime::Update($tm, NULL, NULL, 31)), -- 2019-01-31T01:02:03Z
    DateTime::MakeDatetime(DateTime::Update($tm, 15 as Hour, 30 as Minute)), -- 2019-01-01T15:30:03Z
    DateTime::MakeTimestamp(DateTime::Update($tm, 999999 as Microsecond)), -- 2019-01-01T01:02:03.999999Z
    DateTime::MakeTimestamp(DateTime::Update($tm, "Europe/Moscow" as Timezone)), -- 2018-12-31T22:02:03.456789Z (конвертация в UTC)
    DateTime::MakeTzTimestamp(DateTime::Update($tm, "Europe/Moscow" as Timezone)); -- 2019-01-01T01:02:03.456789,Europe/Moscow
```

## From... {#from}

Получение Timestamp из количества секунд/миллисекунд/микросекунд от начала эпохи в UTC. При выходе за границы Timestamp возвращается NULL.

#### Список функций

* `DateTime::FromSeconds(Uint32{Flags:AutoMap}) -> Timestamp`
* `DateTime::FromSeconds64(Int64{Flags:AutoMap}) -> Timestamp64`
* `DateTime::FromMilliseconds(Uint64{Flags:AutoMap}) -> Timestamp`
* `DateTime::FromMilliseconds64(Int64{Flags:AutoMap}) -> Timestamp64`
* `DateTime::FromMicroseconds(Uint64{Flags:AutoMap}) -> Timestamp`
* `DateTime::FromMicroseconds64(Int64{Flags:AutoMap}) -> Timestamp64`

## To... {#to}

Получение количества секунд/миллисекунд/микросекунд от начала эпохи в UTC из простого типа.

#### Список функций

* `DateTime::ToSeconds(Date/Datetime/Timestamp/TzDate/TzDatetime/TzTimestamp{Flags:AutoMap}) -> Uint32`
* `DateTime::ToSeconds(Date32/Datetime64/Timestamp64/TzDate32/TzDatetime64/TzTimestamp64{Flags:AutoMap}) -> Int64`
* `DateTime::ToMilliseconds(Date/Datetime/Timestamp/TzDate/TzDatetime/TzTimestamp{Flags:AutoMap}) -> Uint64`
* `DateTime::ToMilliseconds(Date32/Datetime64/Timestamp64/TzDate32/TzDatetime64/TzTimestamp64{Flags:AutoMap}) -> Int64`
* `DateTime::ToMicroseconds(Date/Datetime/Timestamp/TzDate/TzDatetime/TzTimestamp{Flags:AutoMap}) -> Uint64`
* `DateTime::ToMicroseconds(Date32/Datetime64/Timestamp64/TzDate32/TzDatetime64/TzTimestamp64{Flags:AutoMap}) -> Int64`

#### Примеры

```yql
SELECT
    DateTime::FromSeconds(1546304523), -- 2019-01-01T01:02:03.000000Z
    DateTime::ToMicroseconds(Timestamp("2019-01-01T01:02:03.456789Z")); -- 1546304523456789
```

## Interval... {#interval}

Преобразования между `Interval` и различными единицами измерения времени.

#### Список функций

* `DateTime::ToDays(Interval{Flags:AutoMap}) -> Int32`
* `DateTime::ToDays(Interval64{Flags:AutoMap}) -> Int32`
* `DateTime::ToHours(Interval{Flags:AutoMap}) -> Int32`
* `DateTime::ToHours(Interval64{Flags:AutoMap}) -> Int64`
* `DateTime::ToMinutes(Interval{Flags:AutoMap}) -> Int32`
* `DateTime::ToMinutes(Interval64{Flags:AutoMap}) -> Int64`
* `DateTime::ToSeconds(Interval{Flags:AutoMap}) -> Int32` - до версии [2025.03](../../changelog/2025.03.md)
* `DateTime::ToSeconds(Interval{Flags:AutoMap}) -> Int64` - с версии [2025.03](../../changelog/2025.03.md)
* `DateTime::ToSeconds(Interval64{Flags:AutoMap}) -> Int64`
* `DateTime::ToMilliseconds(Interval{Flags:AutoMap}) -> Int64`
* `DateTime::ToMilliseconds(Interval64{Flags:AutoMap}) -> Int64`
* `DateTime::ToMicroseconds(Interval{Flags:AutoMap}) -> Int64`
* `DateTime::ToMicroseconds(Interval64{Flags:AutoMap}) -> Int64`
* `DateTime::IntervalFromDays(Int32{Flags:AutoMap}) -> Interval`
* `DateTime::Interval64FromDays(Int32{Flags:AutoMap}) -> Interval64`
* `DateTime::IntervalFromHours(Int32{Flags:AutoMap}) -> Interval`
* `DateTime::Interval64FromHours(Int64{Flags:AutoMap}) -> Interval64`
* `DateTime::IntervalFromMinutes(Int32{Flags:AutoMap}) -> Interval`
* `DateTime::Interval64FromMinutes(Int64{Flags:AutoMap}) -> Interval64`
* `DateTime::IntervalFromSeconds(Int32{Flags:AutoMap}) -> Interval` - до версии [2025.03](../../changelog/2025.03.md)
* `DateTime::IntervalFromSeconds(Int64{Flags:AutoMap}) -> Interval` - с версии [2025.03](../../changelog/2025.03.md)
* `DateTime::Interval64FromSeconds(Int64{Flags:AutoMap}) -> Interval64`
* `DateTime::IntervalFromMilliseconds(Int64{Flags:AutoMap}) -> Interval`
* `DateTime::Interval64FromMilliseconds(Int64{Flags:AutoMap}) -> Interval64`
* `DateTime::IntervalFromMicroseconds(Int64{Flags:AutoMap}) -> Interval`
* `DateTime::Interval64FromMicroseconds(Int64{Flags:AutoMap}) -> Interval64`

{% note warning %}

Функция `DateTime::ToSeconds` до `2025.03` не поддерживает работу с интервалами с длительностью большей чем 68 лет, в этом случае можно использовать выражение `DateTime::ToMilliseconds(x) / 1000`

{% endnote %}

AddTimezone никак не влияет на вывод ToSeconds(), поскольку ToSeconds() всегда возвращают время в таймзоне GMT.

Interval также можно создавать из строкового литерала в формате [ISO 8601](https://en.wikipedia.org/wiki/ISO_8601%23Durations).

{% note warning %}

На данный момент поддержаны только литералы для детерминированных интервалов. Все размерности больше недели -- M (месяц) и Y (год) -- не фиксированы, поскольку интервал может быть определен только с помощью внешнего контекста (месяц, год).

{% endnote %}

#### Примеры

```yql
SELECT
    DateTime::ToDays(Interval("PT3000M")), -- 2
    DateTime::IntervalFromSeconds(1000000), -- 11 days 13 hours 46 minutes 40 seconds
    DateTime::ToDays(cast('2018-01-01' as date) - cast('2017-12-31' as date)); --1
```

## StartOf... / EndOf... / TimeOfDay {#startof}

Получить начало (конец) периода, содержащего дату/время. При некорректном результате возвращается NULL. Если таймзона не GMT, то начало (конец) периода будет в указанной временной зоне.

#### Список функций

* `DateTime::StartOfYear(Resource<TM>{Flags:AutoMap}) -> Resource<TM>?`
* `DateTime::StartOfYear(Resource<TM64>{Flags:AutoMap}) -> Resource<TM64>?`
* `DateTime::EndOfYear(Resource<TM>{Flags:AutoMap}) -> Resource<TM>?`
* `DateTime::EndOfYear(Resource<TM64>{Flags:AutoMap}) -> Resource<TM64>?`
* `DateTime::StartOfQuarter(Resource<TM>{Flags:AutoMap}) -> Resource<TM>?`
* `DateTime::StartOfQuarter(Resource<TM64>{Flags:AutoMap}) -> Resource<TM64>?`
* `DateTime::EndOfQuarter(Resource<TM>{Flags:AutoMap}) -> Resource<TM>?`
* `DateTime::EndOfQuarter(Resource<TM64>{Flags:AutoMap}) -> Resource<TM64>?`
* `DateTime::StartOfMonth(Resource<TM>{Flags:AutoMap}) -> Resource<TM>?`
* `DateTime::StartOfMonth(Resource<TM64>{Flags:AutoMap}) -> Resource<TM64>?`
* `DateTime::EndOfMonth(Resource<TM>{Flags:AutoMap}) -> Resource<TM>?`
* `DateTime::EndOfMonth(Resource<TM64>{Flags:AutoMap}) -> Resource<TM64>?`
* `DateTime::StartOfWeek(Resource<TM>{Flags:AutoMap}) -> Resource<TM>?`
* `DateTime::StartOfWeek(Resource<TM64>{Flags:AutoMap}) -> Resource<TM64>?`
* `DateTime::EndOfWeek(Resource<TM>{Flags:AutoMap}) -> Resource<TM>?`
* `DateTime::EndOfWeek(Resource<TM64>{Flags:AutoMap}) -> Resource<TM64>?`
* `DateTime::StartOfDay(Resource<TM>{Flags:AutoMap}) -> Resource<TM>?`
* `DateTime::StartOfDay(Resource<TM64>{Flags:AutoMap}) -> Resource<TM64>?`
* `DateTime::EndOfDay(Resource<TM>{Flags:AutoMap}) -> Resource<TM>?`
* `DateTime::EndOfDay(Resource<TM64>{Flags:AutoMap}) -> Resource<TM64>?`
* `DateTime::StartOf(Resource<TM>{Flags:AutoMap}, Interval{Flags:AutoMap}) -> Resource<TM>?`
* `DateTime::StartOf(Resource<TM64>{Flags:AutoMap}, Interval64{Flags:AutoMap}) -> Resource<TM64>?`
* `DateTime::EndOf(Resource<TM>{Flags:AutoMap}, Interval{Flags:AutoMap}) -> Resource<TM>?`
* `DateTime::EndOf(Resource<TM64>{Flags:AutoMap}, Interval64{Flags:AutoMap}) -> Resource<TM64>?`

Функции `StartOf`/`EndOf` предназначены для группировки в пределах суток по произвольному периоду. Результат отличается от входного значения только компонентами времени. Период более суток трактуется как сутки (эквивалентно `StartOfDay`/`EndOfDay`). Если в сутках не содержится целого числа периодов, производится округление к ближайшему времени от начала суток, кратному указанному периоду. При нулевом интервале выход совпадает со входом. Отрицательный интервал трактуется как положительный.

Функции `EndOf...` предназначены для получения последнего момента времени, находящегося в том же выбранном интервале, что и заданный.

Поведение функций с периодами больше дня отличается от поведения одноимённых функций в старой библиотеке. Компоненты времени всегда обнуляются (это логично, поскольку эти функции в основном используются для группировки по периоду). Отдельно существует возможность выделить время в пределах суток:

* `DateTime::TimeOfDay(Resource<TM>{Flags:AutoMap}) -> Interval`
* `DateTime::TimeOfDay(Resource<TM64>{Flags:AutoMap}) -> Interval64`

#### Примеры

```yql
SELECT
    DateTime::MakeDate(DateTime::StartOfYear(Date("2019-06-06"))),
      -- 2019-01-01 (неявный Split здесь и дальше)
    DateTime::MakeDatetime(DateTime::StartOfQuarter(Datetime("2019-06-06T01:02:03Z"))),
      -- 2019-04-01T00:00:00Z (компоненты времени обнулены)
    DateTime::MakeDate(DateTime::StartOfMonth(Timestamp("2019-06-06T01:02:03.456789Z"))),
      -- 2019-06-01
    DateTime::MakeDate(DateTime::StartOfWeek(Date("1970-01-01"))),
      -- NULL (начало эпохи - четверг, начало недели - 1969-12-29, выход за границы)
    DateTime::MakeTimestamp(DateTime::StartOfWeek(Date("2019-01-01"))),
      -- 2018-12-31T00:00:00Z
    DateTime::MakeDatetime(DateTime::StartOfDay(Datetime("2019-06-06T01:02:03Z"))),
      -- 2019-06-06T00:00:00Z
    DateTime::MakeTzDatetime(DateTime::StartOfDay(TzDatetime("1970-01-01T05:00:00,Europe/Moscow"))),
      -- NULL (в GMT выход за эпоху)
    DateTime::MakeTzTimestamp(DateTime::StartOfDay(TzTimestamp("1970-01-02T05:00:00.000000,Europe/Moscow"))),
      -- 1970-01-02T00:00:00,Europe/Moscow (начало дня по Москве)
    DateTime::MakeDatetime(DateTime::StartOf(Datetime("2019-06-06T23:45:00Z"), Interval("PT7H"))),
      -- 2019-06-06T21:00:00Z
    DateTime::MakeDatetime(DateTime::StartOf(Datetime("2019-06-06T23:45:00Z"), Interval("PT20M"))),
      -- 2019-06-06T23:40:00Z
    DateTime::TimeOfDay(Timestamp("2019-02-14T01:02:03.456789Z"));
      -- 1 hour 2 minutes 3 seconds 456789 microseconds
```

## Shift... {#shift}

Прибавить/вычесть заданное количество единиц к компоненте во внутреннем представлении и обновить остальные поля.
Возвращает либо обновлённую копию, либо NULL, если после обновления получается некорректная дата или возникают другие противоречия.

#### Список функций

* `DateTime::ShiftYears(Resource<TM>{Flags:AutoMap}, Int32) -> Resource<TM>?`
* `DateTime::ShiftYears(Resource<TM64>{Flags:AutoMap}, Int32) -> Resource<TM64>?`
* `DateTime::ShiftQuarters(Resource<TM>{Flags:AutoMap}, Int32) -> Resource<TM>?`
* `DateTime::ShiftQuarters(Resource<TM64>{Flags:AutoMap}, Int32) -> Resource<TM64>?`
* `DateTime::ShiftMonths(Resource<TM>{Flags:AutoMap}, Int32) -> Resource<TM>?`
* `DateTime::ShiftMonths(Resource<TM64>{Flags:AutoMap}, Int32) -> Resource<TM64>?`

Если в результате номер дня в месяце превышает максимально возможный, то в поле `Day` будет записан последний день месяца,
время при этом не изменится (см. примеры).

#### Примеры

```yql
$tm1 = DateTime::Split(Datetime("2019-01-31T01:01:01Z"));
$tm2 = DateTime::Split(TzDatetime("2049-05-20T12:34:50,Europe/Moscow"));

SELECT
    DateTime::MakeDate(DateTime::ShiftYears($tm1, 10)), -- 2029-01-31T01:01:01
    DateTime::MakeDate(DateTime::ShiftYears($tm2, -10000)), -- NULL (выход за границы)
    DateTime::MakeDate(DateTime::ShiftQuarters($tm2, 0)), -- 2049-05-20T12:34:50,Europe/Moscow
    DateTime::MakeDate(DateTime::ShiftQuarters($tm1, -3)), -- 2018-04-30T01:01:01
    DateTime::MakeDate(DateTime::ShiftMonths($tm1, 1)), -- 2019-02-28T01:01:01
    DateTime::MakeDate(DateTime::ShiftMonths($tm1, -35)), -- 2016-02-29T01:01:01
```

## Format {#format}

Получить строковое представление момента времени, используя произвольную строку форматирования.

#### Список функций

* `DateTime::Format(String, alwaysWriteFractionalSeconds:Bool?) -> (Resource<TM64>{Flags:AutoMap}) -> String`

Для строки форматирования реализовано множество спецификаторов:

* `%%` - символ %;
* `%Y` - год 1-6 цифр и знак, для дат до н.э.;
* `%m` - месяц 2 цифры;
* `%d` - день 2 цифры;
* `%H` - час 2 цифры;
* `%M` - минуты 2 цифры;
* `%S` - секунды 2 цифры или `XX.XXXXXX` в случае непустых микросекунд (и только если флаг `alwaysWriteFractionalSeconds` не выставлен в `True`);
* `%z` - +hhmm or -hhmm;
* `%Z` - IANA имя таймзоны;
* `%b` - короткое трехбуквенное английское название месяца (Jan);
* `%B` - полное английское название месяца (January).

Все остальные символы строки форматирования переносятся без изменений.

#### Примеры

```yql
$format = DateTime::Format("%Y-%m-%d %H:%M:%S %Z");

SELECT
    $format(DateTime::Split(TzDatetime("2019-01-01T01:02:03,Europe/Moscow")));
      -- "2019-01-01 01:02:03 Europe/Moscow"
```

## Parse/Parse64 {#parse}

Распарсить строку во внутреннее представление, используя произвольную строку форматирования. Для незаполненных полей используются значения по умолчанию. При возникновении ошибок возвращается NULL.

#### Список функций

* `DateTime::Parse(String) -> (String{Flags:AutoMap}) -> Resource<TM>?`
* `DateTime::Parse64(String) -> (String{Flags:AutoMap}) -> Resource<TM64>?`

Реализованные спецификаторы:

* `%%` - символ %;
* `%Y` - год 4 цифры - при использовании `Parse`, или 1-6 цифр и знак для дат до н.э. - при использовани `Parse64`;
* `%m` - месяц 2 цифры;
* `%d` - день 2 цифры;
* `%H` - час 2 цифры;
* `%M` - минуты 2 цифры;
* `%S` - секунды, может принимать и микросекунды в форматах от `XX` до `XX.XXXXXX`;
* `%Z` - IANA имя таймзоны (GMT);
* `%b` - короткое трехбуквенное регистронезависимое английское название месяца (Jan);
* `%B` - полное регистронезависимое английское название месяца (January).

#### Примеры

```yql
$parse1 = DateTime::Parse("%H:%M:%S");
$parse2 = DateTime::Parse("%S");
$parse3 = DateTime::Parse("%m/%d/%Y");
$parse4 = DateTime::Parse("%Z");
$parse5 = DateTime::Parse64("%m/%d/%Y");

SELECT
    DateTime::MakeDatetime($parse1("01:02:03")), -- 1970-01-01T01:02:03Z
    DateTime::MakeTimestamp($parse2("12.3456")), -- 1970-01-01T00:00:12.345600Z
    DateTime::MakeTimestamp($parse3("02/30/2000")), -- NULL (Feb 30)
    DateTime::MakeTimestamp($parse4("Canada/Central")), -- 1970-01-01T06:00:00Z (конвертация в UTC)
    DateTime::MakeTimestamp64($parse5("02/10/1931")), -- 1931-02-10T00:00:00Z (конвертация в UTC)
```

## Parse конкретных форматов

Для распространённых форматов есть врапперы вокруг соответствующих методов util. Можно получить только TM с компонентами в UTC таймзоне.

#### Список функций

* `DateTime::ParseRfc822(String{Flags:AutoMap}) -> Resource<TM>?`
* `DateTime::ParseIso8601(String{Flags:AutoMap}) -> Resource<TM>?`
* `DateTime::ParseHttp(String{Flags:AutoMap}) -> Resource<TM>?`
* `DateTime::ParseX509(String{Flags:AutoMap}) -> Resource<TM>?`

#### Примеры

```yql
SELECT
    DateTime::MakeTimestamp(DateTime::ParseRfc822("Fri, 4 Mar 2005 19:34:45 EST")),
      -- 2005-03-05T00:34:45Z
    DateTime::MakeTimestamp(DateTime::ParseIso8601("2009-02-14T02:31:30+0300")),
      -- 2009-02-13T23:31:30Z
    DateTime::MakeTimestamp(DateTime::ParseHttp("Sunday, 06-Nov-94 08:49:37 GMT")),
      -- 1994-11-06T08:49:37Z
    DateTime::MakeTimestamp(DateTime::ParseX509("20091014165533Z"))
      -- 2009-10-14T16:55:33Z
```

## Типовые сценарии

### Преобразования между строками и секундами

Преобразование строковой даты (в таймзоне Москвы) в секунды (в таймзоне GMT):

```yql
$datetime_parse = DateTime::Parse("%Y-%m-%d %H:%M:%S");
$datetime_parse_tz = DateTime::Parse("%Y-%m-%d %H:%M:%S %Z");

SELECT
    DateTime::ToSeconds(TzDatetime("2019-09-16T00:00:00,Europe/Moscow")) AS md_us1, -- 1568581200
    DateTime::ToSeconds(DateTime::MakeDatetime($datetime_parse_tz("2019-09-16 00:00:00" || " Europe/Moscow"))),  -- 1568581200
    DateTime::ToSeconds(DateTime::MakeDatetime(DateTime::Update($datetime_parse("2019-09-16 00:00:00"), "Europe/Moscow" as Timezone))), -- 1568581200

    -- НЕПРАВИЛЬНО (Date импортирует время как GMT, а AddTimezone никак не влияет на ToSeconds, которая всегда возвращает время в таймзоне GMT)
    DateTime::ToSeconds(AddTimezone(Date("2019-09-16"), 'Europe/Moscow')) AS md_us2, -- 1568592000
```

Преобразование строковой даты (в таймзоне Москвы) в секунды (в таймзоне Москвы). Поскольку `DateTime::ToSeconds()` экспортирует только GMT, придется временно забыть о таймзонах и работать только в GMT (выглядит это так, как будто временно мы считаем, что в Москве таймзона GMT):

```yql
$date_parse = DateTime::Parse("%Y-%m-%d");
$datetime_parse = DateTime::Parse("%Y-%m-%d %H:%M:%S");
$datetime_parse_tz = DateTime::Parse("%Y-%m-%d %H:%M:%S %Z");

SELECT
    DateTime::ToSeconds(Datetime("2019-09-16T00:00:00Z")) AS md_ms1, -- 1568592000
    DateTime::ToSeconds(Date("2019-09-16")) AS md_ms2, -- 1568592000
    DateTime::ToSeconds(DateTime::MakeDatetime($date_parse("2019-09-16"))) AS md_ms3, -- 1568592000
    DateTime::ToSeconds(DateTime::MakeDatetime($datetime_parse("2019-09-16 00:00:00"))) AS md_ms4, -- 1568592000
    DateTime::ToSeconds(DateTime::MakeDatetime($datetime_parse_tz("2019-09-16 00:00:00 GMT"))) AS md_ms5, -- 1568592000

    -- НЕПРАВИЛЬНО (импортирует время в таймзоне Москвы, а RemoveTimezone никак не влияет на ToSeconds)
    DateTime::ToSeconds(RemoveTimezone(TzDatetime("2019-09-16T00:00:00,Europe/Moscow"))) AS md_ms6, -- 1568581200
    DateTime::ToSeconds(DateTime::MakeDatetime($datetime_parse_tz("2019-09-16 00:00:00 Europe/Moscow"))) AS md_ms7 -- 1568581200
```

Преобразование секунд (в таймзоне GMT) в строковую дату (в таймзоне Москвы):

```yql
$date_format = DateTime::Format("%Y-%m-%d %H:%M:%S %Z");
SELECT
    $date_format(AddTimezone(DateTime::FromSeconds(1568592000), 'Europe/Moscow')) -- "2019-09-16 03:00:00 Europe/Moscow"
```

Преобразование секунд (в таймзоне Москвы) в строковую дату (в таймзоне Москвы). Здесь таймзона `%Z` выводится для справки - обычно выводить ее не нужно, потому что она будет равна "GMT" и может сбить с толку.

```yql
$date_format = DateTime::Format("%Y-%m-%d %H:%M:%S %Z");
SELECT
    $date_format(DateTime::FromSeconds(1568592000)) -- "2019-09-16 00:00:00 GMT"
```

Преобразование секунд (в таймзоне GMT) в трехбуквенное название дня недели (в таймзоне Москвы):

```yql
SELECT
    SUBSTRING(DateTime::GetDayOfWeekName(AddTimezone(DateTime::FromSeconds(1568581200), "Europe/Moscow")), 0, 3) -- "Mon"
```

### Форматирование даты и времени

Обычно для форматирования времени используется отдельное именованное выражение, но можно обойтись и без него:

```yql
$date_format = DateTime::Format("%Y-%m-%d %H:%M:%S %Z");

SELECT

   -- Вариант с именованным выражением

   $date_format(AddTimezone(DateTime::FromSeconds(1568592000), 'Europe/Moscow')),

   -- Вариант без именованного выражения

   DateTime::Format("%Y-%m-%d %H:%M:%S %Z")
       (AddTimezone(DateTime::FromSeconds(1568592000), 'Europe/Moscow'))
;
```

### Преобразование типов

Так можно преобразовывать только константы:

```yql
SELECT
    TzDatetime("2019-09-16T00:00:00,Europe/Moscow"), -- 2019-09-16T00:00:00,Europe/Moscow
    Date("2019-09-16") -- 2019-09-16
```

А так - константу, именованное выражение или поле таблицы:

```yql
SELECT
    CAST("2019-09-16T00:00:00,Europe/Moscow" AS TzDatetime), -- 2019-09-16T00:00:00,Europe/Moscow
    CAST("2019-09-16" AS Date) -- 2019-09-16
```

### Преобразование времени в дату

`CAST` в `Date` или `TzDate` дает такую дату в таймзоне GMT, в которую происходит полночь по локальному времени (например, для московского времени 2019-10-22 00:00:00, будет возвращена дата 2019-10-21). Для получения даты в локальной таймзоне можно использовать DateTime::Format.

```yql
$x = Datetime("2019-10-21T21:00:00Z");
select
    AddTimezone($x, "Europe/Moscow"), -- 2019-10-22T00:00:00,Europe/Moscow
    cast($x as TzDate), -- 2019-10-21,GMT
    cast(AddTimezone($x, "Europe/Moscow") as TzDate), -- 2019-10-21,Europe/Moscow
    cast(AddTimezone($x, "Europe/Moscow") as Date), -- 2019-10-21
  DateTime::Format("%Y-%m-%d %Z")(AddTimezone($x, "Europe/Moscow")), -- 2019-10-22 Europe/Moscow
```

Стоит отметить, что некоторые значения в типaх TzDatetime и TzTimestamp, имеющие положительное смещение таймзоны, не могут быть преобразованы в тип TzDate. Рассмотрим следующий пример:

```yql
SELECT CAST(TzDatetime("1970-01-01T23:59:59,Europe/Moscow") as TzDate);
/* Fatal: Timestamp 1970-01-01T23:59:59.000000,Europe/Moscow cannot be casted to TzDate */
```

Не существует такого значения, начиная с Unix epoch, которым можно представить полночь 01.01.1970 для московского времени. В результате, такое преобразование считается невозможным и генерирует ошибку времени выполнения.

В то же время, значения, имеющие отрицательное смещение таймзоны, возвращают корректный результат:

```yql
SELECT CAST(TzDatetime("1970-01-01T23:59:59,America/Los_Angeles") as TzDate);
/* 1970-01-01,America/Los_Angeles */
```

### Летнее время

Обратите внимание, что летнее время зависит от года:

```yql
SELECT
    RemoveTimezone(TzDatetime("2019-09-16T10:00:00,Europe/Moscow")) as DST1, -- 2019-09-16T07:00:00Z
    RemoveTimezone(TzDatetime("2008-12-03T10:00:00,Europe/Moscow")) as DST2, -- 2008-12-03T07:00:00Z
    RemoveTimezone(TzDatetime("2008-07-03T10:00:00,Europe/Moscow")) as DST3, -- 2008-07-03T06:00:00Z (DST)
```
