# Test Plan: HTTP Egress Actor (Phase 1)

## 1. Scope

This test plan covers the testing of `THttpEgressActor` and related components implemented in `ydb/library/yql/dq/actors/http/`:

- **Events**: `TEvHttpRequest`, `TEvHttpResponse`, `TEvHttpError`
- **Counters**: `TEgressCounters`
- **Security**: `TEgressSecurityConfig`, SSRF protection, header injection prevention
- **Actor**: `THttpEgressActor` — YDB actor for HTTP egress requests

---

## 2. Unit Tests

### 2.1 SSRF Protection (`CheckSSRFProtection`)

| Test ID | Test Case | Input | Expected Result |
|---------|-----------|-------|-----------------|
| SSRF-001 | Block loopback IPv4 | `http://127.0.0.1/test` | `false` |
| SSRF-002 | Block localhost hostname | `http://localhost/test` | `false` |
| SSRF-003 | Block RFC1918 10.0.0.0/8 | `http://10.0.0.1/test` | `false` |
| SSRF-004 | Block RFC1918 172.16.0.0/12 | `http://172.16.0.1/test` | `false` |
| SSRF-005 | Block RFC1918 192.168.0.0/16 | `http://192.168.1.1/test` | `false` |
| SSRF-006 | Block cloud metadata | `http://169.254.169.254/latest/meta-data/` | `false` |
| SSRF-007 | Block link-local 169.254.0.0/16 | `http://169.254.0.1/test` | `false` |
| SSRF-008 | Block non-HTTP schemes (FTP) | `ftp://example.com/test` | `false` |
| SSRF-009 | Block non-HTTP schemes (file) | `file:///etc/passwd` | `false` |
| SSRF-010 | Block non-HTTP schemes (gopher) | `gopher://example.com/test` | `false` |
| SSRF-011 | Allow external HTTP URL | `http://example.com/test` (with allowed host) | `true` |
| SSRF-012 | Allow external HTTPS URL | `https://example.com/test` (with allowed host) | `true` |
| SSRF-013 | Block when host not in allowlist | `http://evil.com/test` (no allowed hosts) | `false` |
| SSRF-014 | Allow when host in allowlist | `http://example.com/test` (allowed) | `true` |
| SSRF-015 | Block 0.0.0.0 | `http://0.0.0.0/test` | `false` |
| SSRF-016 | Block IPv6 loopback | `http://[::1]/test` | `false` |
| SSRF-017 | Block IPv6 link-local | `http://[fe80::1]/test` | `false` |
| SSRF-018 | Allow public IPv6 | `http://[2001:4860:4860:4860::8888]/test` (with allowed host) | `true` |

### 2.2 Host Policy (`CheckHostPolicy`)

| Test ID | Test Case | Input | Expected Result |
|---------|-----------|-------|-----------------|
| HOST-001 | Deny all by default | No allowed hosts, any host | `false` |
| HOST-002 | Exact match allow | `example.com` in allowlist | `true` |
| HOST-003 | Exact match deny | `evil.com` not in allowlist | `false` |
| HOST-004 | Wildcard subdomain | `*.example.com` matches `api.example.com` | `true` |
| HOST-005 | Wildcard deep subdomain | `*.example.com` matches `foo.bar.example.com` | `true` |
| HOST-006 | Wildcard no bare domain | `*.example.com` does NOT match `example.com` | `false` |
| HOST-007 | Wildcard no suffix match | `*.example.com` does NOT match `notexample.com` | `false` |
| HOST-008 | Denylist overrides allowlist | Host in both lists | `false` |
| HOST-009 | Multiple allowed hosts | Any from list | `true` |
| HOST-010 | Case-insensitive host match | `Example.COM` vs `example.com` | `true` |

### 2.3 Header Validation (`ValidateHeader`)

| Test ID | Test Case | Input | Expected Result |
|---------|-----------|-------|-----------------|
| HDR-001 | Reject CR in name | `"X-Test\r\nInjected: true"`, `"value"` | `false` |
| HDR-002 | Reject LF in name | `"X-Test\nInjected"`, `"value"` | `false` |
| HDR-003 | Reject CR in value | `"X-Test"`, `"value\r\nInjected"` | `false` |
| HDR-004 | Reject LF in value | `"X-Test"`, `"value\nInjected"` | `false` |
| HDR-005 | Accept valid header | `"X-Custom-Header"`, `"some-value"` | `true` |
| HDR-006 | Accept standard header | `"Content-Type"`, `"application/json"` | `true` |
| HDR-007 | Reject null byte in name | `"X-Test\x00"`, `"value"` | `false` |
| HDR-008 | Reject null byte in value | `"X-Test"`, `"value\x00"` | `false` |
| HDR-009 | Empty name | `""`, `"value"` | `false` |
| HDR-010 | Empty value (should pass) | `"X-Empty"`, `""` | `true` |

### 2.4 Reserved Headers (`IsReservedHeader`)

| Test ID | Test Case | Input | Expected Result |
|---------|-----------|-------|-----------------|
| RES-001 | Authorization | `"Authorization"` | `true` |
| RES-002 | Authorization lowercase | `"authorization"` | `true` |
| RES-003 | Host | `"Host"` | `true` |
| RES-004 | Content-Length | `"Content-Length"` | `true` |
| RES-005 | Connection | `"Connection"` | `true` |
| RES-006 | Transfer-Encoding | `"Transfer-Encoding"` | `true` |
| RES-007 | User-Agent | `"User-Agent"` | `false` (not reserved in v1) |
| RES-008 | X-Custom (not reserved) | `"X-Custom-Header"` | `false` |
| RES-009 | Content-Type (not reserved) | `"Content-Type"` | `false` |
| RES-010 | Accept (not reserved) | `"Accept"` | `false` |

### 2.5 Counters (`TEgressCounters`)

| Test ID | Test Case | Operations | Expected Result |
|---------|-----------|------------|-----------------|
| CNT-001 | Increment requests sent | `IncrementRequestsSent()` x2 | `GetRequestsSent() == 2` |
| CNT-002 | Increment errors | `IncrementErrors()` | `GetErrors() == 1` |
| CNT-003 | Add request bytes | `AddRequestBytes(100)`, `AddRequestBytes(200)` | `GetRequestBytes() == 300` |
| CNT-004 | Add response bytes | `AddResponseBytes(500)` | `GetResponseBytes() == 500` |
| CNT-005 | Active requests (increment) | `IncrementActiveRequests()` x2 | `GetActiveRequests() == 2` |
| CNT-006 | Active requests (decrement) | `DecrementActiveRequests()` | `GetActiveRequests() == 1` |
| CNT-007 | Reset all counters | `Reset()` | All counters == 0 |
| CNT-008 | SSRF blocks counter | `IncrementSSRFBlocks()` | `GetSSRFBlocks() == 1` |
| CNT-009 | Timeouts counter | `IncrementTimeouts()` | `GetTimeouts() == 1` |
| CNT-010 | Size limit exceeded | `IncrementSizeLimitExceeded()` | `GetSizeLimitExceeded() == 1` |
| CNT-011 | Header injection blocks | `IncrementHeaderInjectionBlocks()` | `GetHeaderInjectionBlocks() == 1` |
| CNT-012 | Concurrency rejected | `IncrementConcurrencyRejected()` | `GetConcurrencyRejected() == 1` |
| CNT-013 | Thread safety | Concurrent increments from multiple threads | Final count == sum of all increments |

### 2.6 Concurrency Limits

| Test ID | Test Case | Configuration | Expected Result |
|---------|-----------|---------------|-----------------|
| CONC-001 | Accept under global limit | `MaxInFlightRequests=2`, current=1 | `true` |
| CONC-002 | Reject at global limit | `MaxInFlightRequests=2`, current=2 | `false` |
| CONC-003 | Accept under per-host limit | `MaxInFlightRequestsPerHost=1`, current=0 | `true` |
| CONC-004 | Reject at per-host limit | `MaxInFlightRequestsPerHost=1`, current=1 | `false` |
| CONC-005 | Release and re-accept | Decrement, then check | `true` |

---

## 3. Integration Tests (Actor Lifecycle)

### 3.1 Actor Creation and Bootstrap

| Test ID | Test Case | Expected Result |
|---------|-----------|-----------------|
| ACT-001 | Create actor with valid config | Actor registered successfully |
| ACT-002 | Actor responds to TEvHttpRequest | Request processed without crash |
| ACT-003 | Actor handles PassAway | Actor shuts down cleanly |

### 3.2 Request Validation Flow

| Test ID | Test Case | Input | Expected Response |
|---------|-----------|-------|-------------------|
| VAL-001 | SSRF-blocked URL | `http://127.0.0.1/test` | `TEvHttpError` with "SSRF" message |
| VAL-002 | Body exceeds limit | Body > `MaxRequestBodySize` | `TEvHttpError` with "exceeds limit" |
| VAL-003 | Headers exceed size limit | Total headers > `MaxHeadersSize` | `TEvHttpError` with "exceeds limit" |
| VAL-004 | Header injection attempt | Header with CR/LF | `TEvHttpError` with "Invalid header" |
| VAL-005 | Timeout exceeds maximum | Timeout > `MaxTimeout` | `TEvHttpError` with "exceeds maximum" |
| VAL-006 | Concurrency limit exceeded | All slots occupied | `TEvHttpError` with "Concurrency limit" |

### 3.3 Response Handling

| Test ID | Test Case | Input | Expected Response | Status |
|---------|-----------|-------|-------------------|--------|
| RESP-001 | Successful HTTP response | 200 OK from proxy | `TEvHttpResponse` with status=200 | ⏳ Planned (requires end-to-end actor test) |
| RESP-002 | Error from proxy | Proxy returns error | `TEvHttpError` with error message | ⏳ Planned |
| RESP-003 | Response body exceeds limit | Body > `MaxResponseBodySize` | `TEvHttpError` with "exceeds limit" | ✅ `TestResponseBodySizeLimitThroughActor` |
| RESP-004 | HTTP error status (4xx/5xx) | 404/500 from proxy | `TEvHttpResponse` with correct status | ⏳ Planned |

### 3.4 Timeout Handling

| Test ID | Test Case | Configuration | Expected Result | Status |
|---------|-----------|---------------|-----------------|--------|
| TO-001 | Request times out | Timeout expires before response | `TEvHttpError` with "timed out" | ✅ `TestRealRequestTimeoutThroughActor` |
| TO-002 | Multiple pending requests timeout | Multiple requests with different timeouts | Each times out independently | ⏳ Planned |
| TO-003 | Timer counter incremented | After timeout | `GetTimeouts() >= 1` | ✅ (в TestRealRequestTimeoutThroughActor) |

---

## 4. Security Tests

### 4.1 SSRF Attack Vectors

| Test ID | Attack Vector | URL | Expected Result | Status |
|---------|---------------|-----|-----------------|--------|
| SSRF-ATK-001 | DNS rebinding | Host resolves to internal IP | Blocked by SSRF check | ⏳ Planned (requires DNS resolve-and-pin) |
| SSRF-ATK-002 | IPv4 in IPv6 | `http://[::ffff:127.0.0.1]/test` | Blocked | ✅ Implemented |
| SSRF-ATK-003 | Octal IP | `http://0177.0.0.1/test` | Blocked | ⏳ Planned |
| SSRF-ATK-004 | Hexadecimal IP | `http://0x7F.0x0.0x0.0x1/test` | Blocked | ⏳ Planned |
| SSRF-ATK-005 | URL encoding | `http://%31%32%37%2e%30%2e%30%2e%31/test` | Blocked | ⏳ Planned |
| SSRF-ATK-006 | Cloud metadata (AWS) | `http://169.254.169.254/latest/meta-data/` | Blocked | ✅ Implemented |
| SSRF-ATK-007 | Cloud metadata (GCP) | `http://metadata.google.internal/` | Blocked (if in denylist) | ⏳ Planned |
| SSRF-ATK-008 | Kubernetes API | `http://kubernetes.default.svc/` | Blocked (if in denylist) | ⏳ Planned |

### 4.2 Header Injection

| Test ID | Attack Vector | Header | Expected Result |
|---------|---------------|--------|-----------------|
| INJ-001 | CRLF in header name | `"X-Test\r\nSet-Cookie: evil=1"` | Rejected |
| INJ-002 | CRLF in header value | `"X-Test"`, `"val\r\nSet-Cookie: evil=1"` | Rejected |
| INJ-003 | Newline only | `"X-Test\nInjected"` | Rejected |
| INJ-004 | Carriage return only | `"X-Test\rInjected"` | Rejected |
| INJ-005 | Null byte injection | `"X-Test\x00Injected"` | Rejected |

---

## 5. Performance Tests

| Test ID | Test Case | Configuration | Expected Result |
|---------|-----------|---------------|-----------------|
| PERF-001 | Single request latency | 1 request, fast endpoint | Latency < timeout |
| PERF-002 | Concurrent requests | `MaxInFlightRequests` parallel | All processed within limits |
| PERF-003 | High throughput | 1000 sequential requests | All complete, counters accurate |
| PERF-004 | Memory usage under load | Sustained concurrent requests | No memory leak |
| PERF-005 | Counter thread safety | 100 threads, 1000 increments each | Final count == 100,000 |

---

## 6. Edge Cases

| Test ID | Test Case | Input | Expected Result |
|---------|-----------|-------|-----------------|
| EDGE-001 | Empty URL | `""` | Validation error |
| EDGE-002 | Malformed URL | `http:///test` | Validation error or handled gracefully |
| EDGE-003 | Very long URL | URL > 8KB | Handled (may be rejected by proxy) |
| EDGE-004 | Empty method | `""` | Validation error |
| EDGE-005 | Unknown method | `"FOOBAR"` | Passed to proxy (may fail) |
| EDGE-006 | Zero timeout | `TDuration::Zero()` | Immediate timeout |
| EDGE-007 | Very large timeout | `TDuration::Hours(24)` | Rejected if > `MaxTimeout` |
| EDGE-008 | Unicode in URL | `http://example.com/тест` | Handled (percent-encoded) |
| EDGE-009 | Multiple headers with same name | `"X-Custom"` x3 | All passed to proxy |
| EDGE-010 | Response with no body | 204 No Content | `TEvHttpResponse` with empty body |

---

## 7. Test Execution Priority

### P0 (Critical - Must Pass)
- All SSRF protection tests (SSRF-001 to SSRF-018)
- Header injection tests (HDR-001 to HDR-010, INJ-001 to INJ-005)
- SSRF attack vectors (SSRF-ATK-001 to SSRF-ATK-008)
- Request validation (VAL-001 to VAL-006)
- Counter correctness (CNT-001 to CNT-013)

### P1 (High - Should Pass)
- Host policy tests (HOST-001 to HOST-010)
- Reserved headers (RES-001 to RES-010)
- Concurrency limits (CONC-001 to CONC-005)
- Response handling (RESP-001 to RESP-004)
- Timeout handling (TO-001 to TO-003)

### P2 (Medium - Nice to Have)
- Actor lifecycle (ACT-001 to ACT-003)
- Performance tests (PERF-001 to PERF-005)
- Edge cases (EDGE-001 to EDGE-010)

---

## 8. Test Environment Requirements

- **Build**: `./ya make --build relwithdebinfo ydb/library/yql/dq/actors/http`
- **Tests**: `./ya make --build relwithdebinfo -tA ydb/library/yql/dq/actors/http/ut`
- **Dependencies**: YDB actor framework, HTTP proxy infrastructure
- **Isolation**: Unit tests should not require network access
- **Integration tests**: May require mock HTTP server or proxy stubs

---

## 9. Defect Criteria

| Severity | Criteria |
|----------|----------|
| **Critical** | SSRF bypass, header injection bypass, security counter not incremented |
| **High** | Request validation failure, timeout not enforced, concurrency limit bypass |
| **Medium** | Counter inaccuracy, edge case crash, memory leak |
| **Low** | Minor counter discrepancy, logging issues, documentation gaps |

---

## 10. Regression Testing

After any changes to:
- `http_egress_security.cpp` — Re-run all SSRF and header tests
- `http_egress_actor.cpp` — Re-run all actor and validation tests
- `http_egress_counters.h` — Re-run all counter tests
- `events.h` — Re-run all integration tests
