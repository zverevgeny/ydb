# Покрытие тестами: HTTP Egress Actor (Phase 0 + Phase 1)

## Общая статистика

| Категория | Реализовано тестов | Статус |
|-----------|-------------------|--------|
| SSRF Protection | 12 | ✅ Расширенное покрытие (P0) |
| Host Policy | 6 | ✅ Расширенное покрытие |
| Header Validation | 3 | ✅ Расширенное покрытие (P0) |
| Reserved Headers | 1 | ✅ Базовое покрытие |
| Counters | 3 | ✅ Расширенное покрытие |
| Concurrency Limits | 2 | ✅ Расширенное покрытие |
| Actor Integration (SSRF block) | 1 | ✅ Реализован |
| Actor Integration (timeout) | 1 | ✅ Реализован |
| Actor Integration (counters) | 2 | ✅ Реализован |
| Actor Integration (header injection) | 1 | ✅ P0 |
| HTTP Proxy Infrastructure | 4 | ✅ Интеграционные тесты |
| HTTP Request Construction | 3 | ✅ Базовое покрытие |
| Sensitive Header Detection (`IsSensitiveHeader`) | 1 | ✅ Реализован |
| Sensitive Header Redaction (`RedactSensitiveHeaders`) | 4 | ✅ Реализован |
| **ВСЕГО** | **44** | |

---

## Детальное покрытие по категориям

### 1. SSRF Protection (`CheckSSRFProtection`)

| Тест | Что проверяет | Статус |
|------|--------------|--------|
| `TestSSRFProtectionBlocksLoopback` | 127.0.0.1, localhost | ✅ |
| `TestSSRFProtectionBlocksRFC1918` | 10.x, 192.168.x, 172.16.x | ✅ |
| `TestSSRFProtectionBlocksCloudMetadata` | 169.254.169.254 | ✅ |
| `TestSSRFProtectionBlocksNonHttpSchemes` | ftp://, file:// | ✅ |
| `TestSSRFProtectionBlocksIPv6` (P0) | ::1, fe80::, FE80:: | ✅ |
| `TestSSRFProtectionBlocksIPv6WithAllowlist` | IPv6 ::1 с allowlist всё равно блокируется | ✅ |
| `TestSSRFProtectionBlocksIPv4MappedIPv6` | ::ffff:127.0.0.1 (IPv4-mapped IPv6) | ✅ |
| `TestSSRFProtectionBlocksZeroAddress` (P0) | 0.0.0.0 | ✅ |
| `TestSSRFProtectionBlocksDangerousSchemes` (P0) | gopher://, dict://, ssh://, telnet:// | ✅ |
| `TestSSRFProtectionCaseInsensitiveScheme` (P0) | HTTP://, Https://, FTP:// | ✅ |

| `TestSSRFProtectionBlocksLinkLocalRange` | 169.254.0.0/16 (SSRF-007) | ✅ |
| `TestSSRFProtectionAllowsExternalURLs` | http/https через allowlist (SSRF-011/012) | ✅ |
| `TestOctalAndHexIPLimitation` | Документация ограничения для octal/hex IP | ✅ |

**Не покрыто:**
- DNS rebinding защита (планируется в Phase 6)

### 2. Host Policy (`CheckHostPolicy`)

| Тест | Что проверяет | Статус |
|------|--------------|--------|
| `TestHostPolicyDenyAllByDefault` | Пустой allowlist = deny all | ✅ |
| `TestHostPolicyAllowlist` | Точное совпадение в allowlist | ✅ |
| `TestHostPolicyWildcard` | `*.example.com` wildcard | ✅ |
| `TestHostPolicyDenylist` | DeniedHosts переопределяет AllowedHosts | ✅ |
| `TestHostPolicyCaseInsensitive` | `Example.COM` vs `example.com` | ✅ |
| `TestHostPolicyCaseInsensitiveDenylistAndWildcard` | Case-insensitive denylist + wildcard | ✅ |

**Не покрыто:**
- Несколько wildcard правил
- Пустой хост

### 3. Header Validation (`ValidateHeader`)

| Тест | Что проверяет | Статус |
|------|--------------|--------|
| `TestValidateHeaderRejectsInjection` | CR/LF в имени и значении, валидные хедеры | ✅ |
| `TestValidateHeaderEdgeCases` (P0) | Пустое имя (отклоняется), пустое значение (допустимо) | ✅ |
| `TestHeaderInjectionBlockedByActor` (P0) | CR/LF в хедере через `THttpEgressActor` → `TEvHttpError` | ✅ |

**Не покрыто:**
- Null-байт (`\0`) в имени/значении хедера — невозможно тестировать через `TStringBuf` (null-terminated C-strings). Защита relies on HTTP parser rejecting malformed input.
- Очень длинные имена/значения хедеров
- Хедеры с пробелами в имени

### 4. Reserved Headers (`IsReservedHeader`)

| Тест | Что проверяет | Статус |
|------|--------------|--------|
| `TestIsReservedHeader` | authorization, host, content-length, connection, transfer-encoding | ✅ |

**Не покрыто:**
- `proxy-authorization`, `proxy-connection`, `proxy-host`, `proxy-port`
- `user-agent`, `accept`

### 5. Counters (`TEgressCounters`)

| Тест | Что проверяет | Статус |
|------|--------------|--------|
| `TestCounters` | requestsSent, errors, requestBytes, activeRequests | ✅ |
| `TestCountersReset` | Reset() обнуляет все счетчики | ✅ |
| `TestCountersIncrementedOnError` | SSRF blocks, denied hosts, errors | ✅ |

**Не покрыто:**
- `IncrementConcurrencyRejected`
- Thread safety (конкурентное обновление из нескольких потоков)

### 6. Concurrency Limits

| Тест | Что проверяет | Статус |
|------|--------------|--------|
| `TestConcurrencyLimit` | Логика проверки лимита (unit) | ✅ |

| `TestPerHostConcurrencyLimit` | Per-host лимит (`MaxInFlightRequestsPerHost`) через актор | ✅ |

**Не покрыто:**
- Освобождение слота после ответа

### 7. Actor Integration

| Тест | Что проверяет | Статус |
|------|--------------|--------|
| `TestActorCreationAndSSRFBlock` | Актор блокирует SSRF, отправляет TEvHttpError | ✅ |
| `TestTimeoutExceedsMaximum` | Актор отклоняет timeout > MaxTimeout | ✅ |
| `TestMultipleRequests` | Счетчики корректно считают множественные запросы | ✅ |
| `TestRequestSizeLimit` | Проверка лимита размера тела запроса | ✅ |
| `TestHeaderInjectionBlockedByActor` (P0) | Header injection через актор → TEvHttpError | ✅ |

**Не покрыто:**
- Успешный запрос через актор (полный цикл request → response)
- Обработка ответа с ошибкой (HTTP 4xx/5xx)
- [x] Превышение `MaxResponseBodySize` → `TestResponseBodySizeLimitThroughActor` (RESP-003)
- Превышение `MaxHeadersSize`
- Concurrency limit через актор

### 8. HTTP Proxy Infrastructure (Интеграционные тесты с локальным сервером)

| Тест | Что проверяет | Статус |
|------|--------------|--------|
| `TestLocalHttpServerGetRequest` | GET запрос → 200 OK с телом | ✅ |
| `TestLocalHttpServerPostRequest` | POST с JSON телом → 200 OK | ✅ |
| `TestLocalHttpServerErrorResponse` | GET → 404 Not Found | ✅ |
| `TestLocalHttpServerMultiplePaths` | Несколько путей на одном сервере | ✅ |

### 9. HTTP Request Construction

| Тест | Что проверяет | Статус |
|------|--------------|--------|
| `TestHttpOutgoingRequestCreation` | GET запрос, URL parsing | ✅ |
| `TestHttpOutgoingPostRequestCreation` | POST с body, content-type | ✅ |
| `TestHttpOutgoingRequestWithCustomMethod` | PUT запрос с custom method | ✅ |

### 10. Sensitive Header Detection (`IsSensitiveHeader`)

| Тест | Что проверяет | Статус |
|------|--------------|--------|
| `TestIsSensitiveHeader` | authorization, cookie, proxy-authorization, x-api-key, x-auth-token, x-access-token (case-insensitive) | ✅ |

### 11. Sensitive Header Redaction (`RedactSensitiveHeaders`)

| Тест | Что проверяет | Статус |
|------|--------------|--------|
| `TestRedactSensitiveHeaders` | Сensitive headers redacted, non-sensitive visible, secrets not in output | ✅ |
| `TestRedactSensitiveHeadersEmpty` | Empty headers → empty result | ✅ |
| `TestRedactSensitiveHeadersNone` | No sensitive headers → all values visible | ✅ |

### 12. Authorization/Secret Redaction in Error Messages

| Функция | Описание | Статус |
|---------|----------|--------|
| Header name redaction in validation errors | `IsSensitiveHeader(name)` → `"[REDACTED_NAME]"` в TEvHttpError | ✅ |
| `RedactSensitiveHeaders()` | Форматирует хедеры с `[REDACTED]` для чувствительных значений | ✅ |

---

## Phase 0 Deliverables

| Deliverable | Статус |
|-------------|--------|
| Directory + `ya.make` for `ydb/library/yql/dq/actors/http/` | ✅ |
| Proto package (`proto/http_egress.proto` с `TDqHttpEgressSettings`) | ✅ |
| Feature flag in KQP config | ✅ `EnableHttpEgress = 312` в `ydb/core/protos/feature_flags.proto` |
| Empty libraries compile | ✅ |

---

## Исправленные баги (из ревью Phase 0)

| Баг | Описание | Статус |
|-----|----------|--------|
| BUG-1 | Correlation ID mismatch — `CallerRequestId` вместо внутреннего `NextRequestId++` | ✅ Исправлен |
| BUG-2 | Reserved header bypass — пользовательские хедеры могли переопределить Host/Authorization | ✅ Исправлен |
| BUG-3 | `InFlightPerHost` memory leak — пустые записи не удалялись | ✅ Исправлен |
| BUG-4 | Pointer-based response matching — заменён на Cookie-based matching | ✅ Исправлен |
| BUG-5 | `0.0.0.0` не блокировался в `IsBlockedIP()` | ✅ Исправлен |

---

## Реализованные функции (без тестов)

Следующие функции реализованы но пока не покрыты unit-тестами. Требуют тестов в рамках P1.

| Функция | Файл | Описание | Статус тестов |
|---------|------|----------|---------------|
| `ResolveAndPinHost()` | `http_egress_security.cpp` | DNS resolve + IP валидация для защиты от DNS rebinding | ❌ Нет тестов |
| `PassAway()` | `http_egress_actor.cpp` | Очистка pending-запросов при смерти актора (caller-death cancellation) | ❌ Нет тестов |
| `CancelAllPendingRequests()` | `http_egress_actor.cpp` | Отмена всех ожидающих запросов с отправкой TEvHttpError | ❌ Нет тестов |

---

## Приоритеты дальнейшего покрытия тестами

### P0 — Критические (безопасность) — ЗАВЕРШЕНО

Все P0 тесты реализованы:
1. ✅ SSRF: IPv6 адреса (`::1`, `fe80::`)
2. ✅ SSRF: IPv6 с allowlist (регрессия — BUG-A)
3. ✅ SSRF: IPv4-mapped IPv6 (`::ffff:127.0.0.1`)
4. ✅ SSRF: `0.0.0.0`
5. ✅ SSRF: Дополнительные схемы (`gopher://`, `dict://`, `ssh://`, `telnet://`)
6. ✅ SSRF: Case-insensitive scheme handling
7. ✅ Header injection через актор
8. ✅ Edge cases в header validation (пустое имя/значение)

### P1 — Функциональные (корректность)

| # | Что добавить | Обоснование |
|---|-------------|-------------|
| 7 | **Полный цикл request → response через актор** | Ключевой сценарий: отправить запрос, получить ответ |
| 8 | **Per-host concurrency limit через актор** | Проверить `MaxInFlightRequestsPerHost` |
| 9 | **Глобальный concurrency limit через актор** | Проверить `MaxInFlightRequests` |
| 10 | **Response body size limit** | ✅ `TestResponseBodySizeLimitThroughActor` — E2E с локальным proxy |
| 11 | **Headers size limit** | Превышение `MaxHeadersSize` должно вернуть ошибку |
| 12 | **HTTP error status (4xx/5xx)** | Актор должен передавать статус-код в `TEvHttpResponse` |
| 13 | **Timeout через актор** | ✅ `TestRealRequestTimeoutThroughActor` — реальный таймаут 100ms |
| 14 | **Proxy-authorization, proxy-connection в IsReservedHeader** | Проверить все зарезервированные хедеры |
| 15 | **ResolveAndPinHost() unit-тесты** | Проверить DNS resolve, IP валидацию, обработку ошибок |
| 16 | **PassAway() / CancelAllPendingRequests()** | Проверить очистку pending-запросов при смерти актора |

### P2 — Желательные (полнота)

| # | Что добавить | Обоснование |
|---|-------------|-------------|
| 15 | **Thread safety счетчиков** | Конкурентное обновление из нескольких потоков |
| 16 | **Пустой хост** | `CheckHostPolicy("", config)` должен вернуть false |
| 17 | **Очень длинные URL** | Проверка обработки длинных URL |
| 18 | **Множественные wildcard правила** | Несколько `*.example.com` правил |
| 19 | **Chunked transfer encoding** | Ответы с chunked телом |
| 20 | **HTTP redirects (3xx)** | Обработка перенаправлений |
| 21 | **Connection errors** | Что происходит при невозможности подключения |
| 22 | **Empty body POST** | POST с пустым телом |

### P3 — Производительность

| # | Что добавить | Обоснование |
|---|-------------|-------------|
| 23 | **Бenchmarks: 1000 concurrent requests** | Проверить производительность при нагрузке |
| 24 | **Memory leak check** | Утечки памяти при длительной работе |
| 25 | **Request/Response allocation overhead** | Стоимость выделения памяти на запрос |

---

## Блокирующие факторы

1. **Полная интеграция актор → proxy → сервер** — `THttpEgressActor` создает внутренний HTTP proxy, который не маршрутизирует ответы обратно к тестеру в тестовом окружении. Для полного покрытия нужен либо mock proxy, либо более сложная настройка тестового окружения.

2. **Таймауты в тестах** — Реальные таймауты требуют `ctx.Schedule` и ожидания, что сложно в синхронных unit-тестах.

3. **TLS/mTLS тесты** — Требуют сертификатов и настройки TLS соединений.

4. **Feature flag** — ✅ Добавлен `EnableHttpEgress = 312` в `ydb/core/protos/feature_flags.proto`.

---

## Рекомендации

1. **P0 завершено** — Все критические тесты безопасности реализованы и проходят.
2. **Затем P1** — Добавить полный цикл request → response через актор (можно использовать mock или упрощенную интеграцию).
3. **Интеграционные тесты** — Расширить набор тестов с локальным HTTP сервером для покрытия путей: request size limit, header size limit, response size limit.
4. **Feature flag** — ✅ Добавлен `EnableHttpEgress = 312` в `ydb/core/protos/feature_flags.proto`.
5. **Документировать ограничения** — Указать, что полная интеграция актор → proxy → сервер требует дополнительной инфраструктуры.
