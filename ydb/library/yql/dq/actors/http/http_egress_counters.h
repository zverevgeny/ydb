#pragma once

#include <util/generic/string.h>

#include <atomic>

namespace NYql::NDq::NHttpEgress {

// Simple in-memory counters for HTTP egress monitoring.
// In production, these would be wired to NMonitoring::TDynamicCounter.
class TEgressCounters {
public:
    TEgressCounters() = default;

    void IncrementRequestsSent() { RequestsSent.fetch_add(1, std::memory_order_relaxed); }
    void IncrementResponsesReceived() { ResponsesReceived.fetch_add(1, std::memory_order_relaxed); }
    void IncrementErrors() { Errors.fetch_add(1, std::memory_order_relaxed); }
    void IncrementTimeouts() { Timeouts.fetch_add(1, std::memory_order_relaxed); }
    void IncrementDeniedHosts() { DeniedHosts.fetch_add(1, std::memory_order_relaxed); }
    void IncrementSSRFBlocks() { SSRFBlocks.fetch_add(1, std::memory_order_relaxed); }
    void IncrementHeaderInjectionBlocks() { HeaderInjectionBlocks.fetch_add(1, std::memory_order_relaxed); }
    void IncrementSizeLimitExceeded() { SizeLimitExceeded.fetch_add(1, std::memory_order_relaxed); }
    void AddRequestBytes(ui64 bytes) { RequestBytes.fetch_add(bytes, std::memory_order_relaxed); }
    void AddResponseBytes(ui64 bytes) { ResponseBytes.fetch_add(bytes, std::memory_order_relaxed); }
    void IncrementActiveRequests() { ActiveRequests.fetch_add(1, std::memory_order_relaxed); }
    void DecrementActiveRequests() { ActiveRequests.fetch_sub(1, std::memory_order_relaxed); }
    void IncrementConcurrencyRejected() { ConcurrencyRejected.fetch_add(1, std::memory_order_relaxed); }

    void Reset() {
        RequestsSent.store(0, std::memory_order_relaxed);
        ResponsesReceived.store(0, std::memory_order_relaxed);
        Errors.store(0, std::memory_order_relaxed);
        Timeouts.store(0, std::memory_order_relaxed);
        DeniedHosts.store(0, std::memory_order_relaxed);
        SSRFBlocks.store(0, std::memory_order_relaxed);
        HeaderInjectionBlocks.store(0, std::memory_order_relaxed);
        SizeLimitExceeded.store(0, std::memory_order_relaxed);
        RequestBytes.store(0, std::memory_order_relaxed);
        ResponseBytes.store(0, std::memory_order_relaxed);
        ActiveRequests.store(0, std::memory_order_relaxed);
        ConcurrencyRejected.store(0, std::memory_order_relaxed);
    }

    ui64 GetRequestsSent() const { return RequestsSent.load(std::memory_order_relaxed); }
    ui64 GetResponsesReceived() const { return ResponsesReceived.load(std::memory_order_relaxed); }
    ui64 GetErrors() const { return Errors.load(std::memory_order_relaxed); }
    ui64 GetTimeouts() const { return Timeouts.load(std::memory_order_relaxed); }
    ui64 GetDeniedHosts() const { return DeniedHosts.load(std::memory_order_relaxed); }
    ui64 GetSSRFBlocks() const { return SSRFBlocks.load(std::memory_order_relaxed); }
    ui64 GetHeaderInjectionBlocks() const { return HeaderInjectionBlocks.load(std::memory_order_relaxed); }
    ui64 GetSizeLimitExceeded() const { return SizeLimitExceeded.load(std::memory_order_relaxed); }
    ui64 GetRequestBytes() const { return RequestBytes.load(std::memory_order_relaxed); }
    ui64 GetResponseBytes() const { return ResponseBytes.load(std::memory_order_relaxed); }
    ui64 GetActiveRequests() const { return ActiveRequests.load(std::memory_order_relaxed); }
    ui64 GetConcurrencyRejected() const { return ConcurrencyRejected.load(std::memory_order_relaxed); }

private:
    std::atomic<ui64> RequestsSent{0};
    std::atomic<ui64> ResponsesReceived{0};
    std::atomic<ui64> Errors{0};
    std::atomic<ui64> Timeouts{0};
    std::atomic<ui64> DeniedHosts{0};
    std::atomic<ui64> SSRFBlocks{0};
    std::atomic<ui64> HeaderInjectionBlocks{0};
    std::atomic<ui64> SizeLimitExceeded{0};
    std::atomic<ui64> RequestBytes{0};
    std::atomic<ui64> ResponseBytes{0};
    std::atomic<ui64> ActiveRequests{0};
    std::atomic<ui64> ConcurrencyRejected{0};
};

} // namespace NYql::NDq::NHttpEgress
