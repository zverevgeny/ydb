# Code Review: QYT — YTsaurus Queue Topic Client

## 1. Задача

**Интеграция YTsaurus queues как backend для YDB Platform Queue (PQ).**

Решается задача использования очередей YTsaurus в качестве transport layer для YDB Topics, позволяя читать/писать сообщения через стандартный YDB topic client API, но с фактическим хранением данных в YTsaurus queue.

Это позволяет:
- Использовать YTsaurus queues как альтернативу YDB Topics для определённых workload'ов
- Подключить внешние потребители/продюсеры через YTsaurus
- Обеспечить interoperability между YDB и YTsaurus экосистемами

---

## 2. Обзор изменений (18 файлов, +1742 строк)

### 2.1. QYT Topic Client — ядро решения

| Файл | Роль |
|------|------|
| [`yql_qyt_topic_client.h`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.h:1) | Settings + factory |
| [`yql_qyt_topic_client.cpp`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:1) | TQytTopicClient, Read/Write session |
| [`yql_qyt_blocking_queue.h`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_blocking_queue.h:1) | Bounded thread-safe queue |

**Почему нужно:** Это адаптер, который реализует `ITopicClient` интерфейс поверх YTsaurus queue API. Ключевая сложность — эмуляция push-модели YDB Topics поверх pull-модели YT queue.

### 2.2. Gateway Layer

| Файл | Роль |
|------|------|
| [`yql_yt_gateway.h`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_yt_gateway.h:1) | Standalone gateway settings |
| [`yql_yt_gateway.cpp`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_yt_gateway.cpp:1) | TYtPqGateway implementation |

**Почему нужно:** Standalone gateway для прямого использования QYT без native PQ gateway. Позволяет тестировать и использовать QYT изолированно.

### 2.3. Интеграция в Native PQ Gateway

| Файл | Изменение |
|------|----------|
| [`yql_pq_gateway.cpp`](ydb/library/yql/providers/pq/gateway/native/yql_pq_gateway.cpp:110) | +`TryCreateYtTopicClient()`, +`YtClients` cache |
| [`native/ya.make`](ydb/library/yql/providers/pq/gateway/native/ya.make:16) | +peerdir `clients/qyt`, +`yt/yt/client` |

**Почему нужно:** Позволяет native PQ gateway автоматически использовать QYT client когда cluster type = `CT_YT`. Это основной путь интеграции — пользователь настраивает cluster как YT type, и gateway автоматически маршрутизирует трафик через QYT.

### 2.4. Build System

| Файл | Изменение |
|------|----------|
| [`clients/ya.make`](ydb/library/yql/providers/pq/gateway/clients/ya.make:6) | +`qyt` в RECURSE |
| [`clients/qyt/ya.make`](ydb/library/yql/providers/pq/gateway/clients/qyt/ya.make:1) | Build file для QYT library |
| [`clients/qyt/ut/ya.make`](ydb/library/yql/providers/pq/gateway/clients/qyt/ut/ya.make:1) | Unit tests build |

**Почему нужно:** Стандартная интеграция нового модуля в build system YaMake.

### 2.5. Тесты

| Файл | Роль |
|------|------|
| [`yql_qyt_blocking_queue_ut.cpp`](ydb/library/yql/providers/pq/gateway/clients/qyt/ut/yql_qyt_blocking_queue_ut.cpp:1) | Unit tests для blocking queue |
| [`tests_docker/test_qyt_integration.py`](ydb/tests/stress/yt_queue/tests_docker/test_qyt_integration.py:1) | Integration test с Docker YT |
| [`docker-compose.yml`](ydb/tests/stress/yt_queue/docker-compose.yml:1) | YT cluster в Docker |
| [`docker/Dockerfile`](ydb/tests/stress/yt_queue/docker/Dockerfile:1) | Image для YT |
| [`docker/start.sh`](ydb/tests/stress/yt_queue/docker/start.sh:1) | Startup script |

**Почему нужно:** Покрытие unit-тестами критического компонента (blocking queue) и integration test для валидации полного цикла write→read.

### 2.6. Документация

| Файл | Роль |
|------|------|
| [`qyt_gw.md`](ydb/library/yql/providers/pq/qyt_gw.md:1) | Code review + architecture doc |

**Почему нужно:** Документация архитектуры, проблем и рекомендаций.

---

## 3. Детальный анализ ключевых решений

### 3.1. Эмуляция push модели поверх pull API

**Проблема:** YDB Topics использует push-модель (сервер отправляет события клиенту), а YTsaurus queue — pull-модель (клиент запрашивает данные).

**Решение:** [`TQytTopicReadSession`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:93) запускает background thread (`PollLoop`), который:
1. Периодически вызывает `pull_queue_consumer`
2. Конвертирует rows в `TReadSessionEvent::TDataReceivedEvent`
3. Помещает события в bounded queue (`TBlockingEQueue`)
4. Consumer вызывает `WaitEvent()`/`GetEvent()` для получения событий

**Обоснование:** Это стандартный pattern для адаптации pull→push. Альтернатива — переписать весь PQ stack на pull-модель, что было бы значительно более инвазивно.

### 3.2. Backpressure через TBlockingEQueue

**Проблема:** Без ограничения producer может накопить бесконечно много данных в памяти.

**Решение:** [`TBlockingEQueue`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_blocking_queue.h:15) — bounded queue с byte-size backpressure. Producer блокируется когда accumulated size ≥ MaxSize.

**Обоснование:** Предотвращает OOM при медленном consumer'е. Два condition variables (`CanPush`, `CanPop`) обеспечивают эффективное signaling без busy-waiting.

### 3.3. Lazy client creation в gateway

**Решение:** [`TryCreateYtTopicClient()`](ydb/library/yql/providers/pq/gateway/native/yql_pq_gateway.cpp:120) lazily создает YT client при первом запросе для cluster'а и кэширует его в `YtClients` map.

**Обоснование:** Не все cluster'ы являются YT — создание клиента должно быть conditional. Кэширование избегает пересоздания соединения для каждого запроса.

### 3.4. Cluster routing по типу

**Решение:** [`FindYtClusterConfig()`](ydb/library/yql/providers/pq/gateway/native/yql_pq_gateway.cpp:137) ищет cluster с `CT_YT` type по endpoint или database.

**Обоснование:** Позволяет mixed deployment — некоторые cluster'ы могут быть YDB Topics, другие — YT queues. Routing прозрачен для caller'а.

---

## 4. Риски и проблемы

### 4.1. Thread Safety (CRITICAL)

**[`Offset`](ydb/library/yql/providers/pq/gateway/clients/qyt/yql_qyt_topic_client.cpp:275) как `atomic<i64>`:**
- Используется только из poller thread → atomic избыточен
- При crash poller между read и offset update → дубликаты (at-least-once, acceptable)
- **Риск:** Если Offset читается из другого thread для diagnostics — нет synchronization

**[`YtClients`](ydb/library/yql/providers/pq/gateway/native/yql_pq_gateway.cpp:244) map:**
- Protected by `Mutex`, но client creation (network I/O) выполняется под lock'ом
- **Риск:** Под нагрузкой все операции на gateway блокируются при создании нового YT client

### 4.2. Error Handling (HIGH)

**Poll loop terminates on first error:**
```cpp
} catch (const std::exception& ex) {
    EventsQ.Push(TSessionClosedEvent(...), 0);
    break;  // ← session dead
}
```
- Transient network error → permanent session death
- Нет retry logic, circuit breaker
- **Риск:** Single blip kills read session

### 4.3. Resource Cleanup (HIGH)

**No timeout on `thread::join()`:**
```cpp
if (Poller.joinable()) Poller.join();  // ← может hang forever
```
- Если background thread заблокирован на YT RPC call → destructor hangs
- **Риск:** Process shutdown hangs

**`WaitEvent()` futures могут не resolve:**
- `Pool.Stop()` останавливает thread pool
- Pending async callbacks от `WaitEvent()` могут не выполниться
- **Риск:** Caller, ожидающий future, зависает навсегда

### 4.4. Data Model (MEDIUM)

**String-only data extraction:**
```cpp
if (value.Type == NYT::NTableClient::EValueType::String) {
    data << TStringBuf(value.Data.String, value.Length);
}
```
- Binary/non-string columns silently skipped
- **Риск:** Data loss для non-string queue columns

**No CAS on CommitOffset:**
```cpp
/* oldOffset */ std::nullopt,
```
- Offset advanced unconditionally
- **Риск:** Offset regression при concurrent commit'ах

### 4.5. Test Coverage (HIGH)

- Unit tests только для `TBlockingEQueue`
- Нет unit tests для `TQytTopicClient`, `TQytTopicReadSession`, `TQytTopicWriteSession`
- Нет mock для YT client → тесты требуют реальный YT cluster
- Integration test gracefully skips когда `yt` недоступен

---

## 5. Рекомендации по приоритету

### Before Production (Critical)
1. Добавить retry logic с exponential backoff в poll/write loops
2. Добавить timeout на `thread::join()` в Cleanup()
3. Обеспечить resolution pending `WaitEvent()` futures при close
4. Добавить CAS protection или документировать at-most-once semantics для CommitOffset

### High Priority
5. Добавить unit tests с mocked YT client
6. Добавить logging в ключевых точках
7. Поддержать binary data или документировать string-only limitation
8. Дедуплицировать path joining logic (3 copy-paste)

### Medium Priority
9. Документировать configuration (mapping YDB paths → YT paths)
10. Добавить config validation при startup
11. Рассмотреть async client creation (не блокировать mutex на network I/O)
12. Добавить metrics/counters (сейчас `GetCounters()` → nullptr)

---

## 6. Итог

Решение архитектурно корректно — адаптация pull→push через background thread + bounded queue является правильным подходом. Интеграция в native PQ gateway через cluster type routing — чистое и non-invasive решение.

Основные риски связаны с error resilience (single failure kills session), resource cleanup (potential hangs) и test coverage. Решение подходит для initial integration и testing, но требует доработки перед production use.
