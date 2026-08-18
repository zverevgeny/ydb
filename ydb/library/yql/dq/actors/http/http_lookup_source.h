#pragma once

#include "events.h"
#include "http_egress_actor.h"
#include "lookup_events.h"
#include "proto/http_lookup.pb.h"

#include <ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>

#include <yql/essentials/minikql/computation/mkql_computation_node_holders.h>

#include <util/generic/hash.h>
#include <util/generic/map.h>
#include <util/generic/string.h>
#include <util/generic/vector.h>

#include <memory>

namespace NYql::NDq::NHttpEgress {

// Response from a single HTTP lookup request.
struct TLookupResponse {
    ui32 StatusCode{0};
    TString Body;
    TString Error;
    bool Success{false};
};

// THttpLookupReceiver is the actor that receives HTTP responses from the egress actor.
// It is created as a child actor paired with THttpLookupSource.
// The source's AsyncLookup() sends requests to the egress actor targeting this receiver,
// and the receiver assembles results and sends TEvLookupResult to the compute actor.
class THttpLookupReceiver : public NActors::TActorBootstrapped<THttpLookupReceiver> {
public:
    THttpLookupReceiver(
        NActors::TActorId egressActorId,
        NActors::TActorId resultTarget,
        Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings settings,
        IMemoryQuotaManager::TPtr memoryQuotaManager,
        const THashMap<TString, TString>& secureParams,
        std::shared_ptr<NKikimr::NMiniKQL::TScopedAlloc> alloc,
        const NKikimr::NMiniKQL::TStructType* payloadType,
        const NKikimr::NMiniKQL::THolderFactory& holderFactory);

private:
    STFUNC(StateFunc);

    void HandleLookupRequest(TEvLookupRequest::TPtr& ev, const NActors::TActorContext& ctx);
    void HandleHttpResponse(TEvHttpEgress::TEvHttpResponse::TPtr& ev, const NActors::TActorContext& ctx);
    void HandleHttpError(TEvHttpEgress::TEvHttpError::TPtr& ev, const NActors::TActorContext& ctx);
    void HandleWakeup(NActors::TEvents::TEvWakeup::TPtr& ev, const NActors::TActorContext& ctx);

    // Process a single lookup request batch.
    void ProcessLookup(std::shared_ptr<IDqAsyncLookupSource::TUnboxedValueMap> request);

    // Build HTTP URL from key using path template.
    TString BuildUrl(TStringBuf key) const;

    // Build request body from key using body template.
    TString BuildBody(TStringBuf key) const;

    // Check cache for a key. Returns true if found.
    bool CheckCache(TStringBuf key, TLookupResponse& response) const;

    // Store response in cache (with size limit enforcement).
    void CacheResponse(TStringBuf key, const TLookupResponse& response);

    // Evict oldest cache entries if cache exceeds size limit.
    void EvictCacheIfNeeded();

    // URL-encode a key value for safe inclusion in URLs.
    static TString UrlEncode(TStringBuf input);

    // Convert TLookupResponse to TUnboxedValue using PayloadType schema.
    NKikimr::NUdf::TUnboxedValue ConvertResponseToValue(const TLookupResponse& response);

    // Send final result to the target actor.
    void SendResult();

    // Free internal unboxed values (for PassAway).
    void Free();

    // Settings from proto.
    const Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings Settings;

    // Egress actor for making HTTP requests.
    const NActors::TActorId EgressActorId;

    // Target actor to send TEvLookupResult to.
    const NActors::TActorId ResultTarget;

    // Memory quota manager for accounting response bytes.
    IMemoryQuotaManager::TPtr MemoryQuotaManager;

    // Secure params for auth token resolution.
    const THashMap<TString, TString> SecureParams;

    // MKQL allocator for managing unboxed value lifetimes.
    std::shared_ptr<NKikimr::NMiniKQL::TScopedAlloc> Alloc;

    // Output payload type schema (Struct<StatusCode, Headers, Body>).
    const NKikimr::NMiniKQL::TStructType* PayloadType;

    // Holder factory for creating TUnboxedValue instances.
    const NKikimr::NMiniKQL::THolderFactory& HolderFactory;

    // Pending requests: requestId → key string.
    THashMap<ui64, TString> PendingRequests;
    ui64 NextRequestId{1};

    // Completed responses: key string → response (collected before SendResult).
    THashMap<TString, TLookupResponse> CompletedResponses;

    // Original request keys in order (for result assembly).
    std::shared_ptr<IDqAsyncLookupSource::TUnboxedValueMap> CurrentRequest;

    // Cache: key → response (for CACHE_ACROSS_BATCHES).
    // Uses TVector for access-order tracking (simple LRU).
    mutable THashMap<TString, TLookupResponse> Cache;
    mutable TVector<TString> CacheAccessOrder;  // For LRU eviction
    mutable TInstant CacheExpiry;
    mutable ui64 CacheSizeBytes{0};  // Total cache size in bytes

    // Maximum cache size in bytes (default 10MB).
    ui64 MaxCacheSizeBytes{10 * 1024 * 1024};

    // Accounted memory for quota tracking.
    ui64 AccountedMemory{0};
};

// THttpLookupSource implements IDqAsyncLookupSource for HTTP-based lookups.
// It is paired with a THttpLookupReceiver actor. The source initiates lookups
// by sending the request to the receiver actor, which handles the async HTTP flow.
class THttpLookupSource : public IDqAsyncLookupSource {
public:
    THttpLookupSource(
        Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings settings,
        NActors::TActorId receiverActorId);

    // IDqAsyncLookupSource interface
    size_t GetMaxSupportedKeysInRequest() const override;
    void AsyncLookup(std::weak_ptr<TUnboxedValueMap> request) override;

    // Returns the paired receiver actor.
    NActors::IActor* GetReceiverActor();

private:
    // Settings from proto.
    const Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings Settings;

    // Paired receiver actor that handles the async HTTP flow.
    NActors::TActorId ReceiverActorId;
};

// Factory function to create the lookup source and paired receiver actor.
// Takes the egress actor ID explicitly (created by the caller).
std::pair<IDqAsyncLookupSource*, NActors::IActor*> CreateHttpLookupSourcePair(
    Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings settings,
    NActors::TActorId egressActorId,
    NActors::TActorId resultTarget,
    IDqAsyncIoFactory::TLookupSourceArguments&& args);

} // namespace NYql::NDq::NHttpEgress
