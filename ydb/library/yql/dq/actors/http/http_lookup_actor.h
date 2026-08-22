#pragma once

#include "events.h"
#include "http_egress_actor.h"
#include "lookup_events.h"
#include <ydb/library/yql/dq/proto/http_lookup.pb.h>

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

// THttpLookupActor is a unified actor implementing both IDqAsyncLookupSource
// and the actor framework. It handles the async HTTP lookup flow:
// 1. Receives lookup request from compute actor
// 2. Sends HTTP requests to egress actor for each key
// 3. Collects responses and sends TEvLookupResult back
//
// This follows the pattern from TMockLookupActor in mock_lookup_factory.cpp.
class THttpLookupActor
    : public NYql::NDq::IDqAsyncLookupSource,
      public NActors::TActorBootstrapped<THttpLookupActor> {
    using TBase = NActors::TActorBootstrapped<THttpLookupActor>;

public:
    THttpLookupActor(
        NActors::TActorId parentId,
        NActors::TActorId egressActorId,
        Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings settings,
        std::shared_ptr<NKikimr::NMiniKQL::TScopedAlloc> alloc,
        const NKikimr::NMiniKQL::TStructType* payloadType,
        const NKikimr::NMiniKQL::THolderFactory& holderFactory,
        const THashMap<TString, TString>& secureParams,
        size_t maxKeysInRequest);

    ~THttpLookupActor();

public:
    void Bootstrap();
    static constexpr char ActorName[] = "HTTP_LOOKUP_ACTOR";

public: // IDqAsyncLookupSource
    size_t GetMaxSupportedKeysInRequest() const override;
    void AsyncLookup(std::weak_ptr<IDqAsyncLookupSource::TUnboxedValueMap> request) override;
    void PassAway() override;

private: // events
    STRICT_STFUNC(StateFunc,
        hFunc(TEvLookupRequest, Handle);
        hFunc(TEvHttpEgress::TEvHttpResponse, Handle);
        hFunc(TEvHttpEgress::TEvHttpError, Handle);
        hFunc(NActors::TEvents::TEvWakeup, Handle);
        hFunc(NActors::TEvents::TEvPoison, Handle);)

    void Handle(TEvLookupRequest::TPtr ev);
    void Handle(TEvHttpEgress::TEvHttpResponse::TPtr ev);
    void Handle(TEvHttpEgress::TEvHttpError::TPtr ev);
    void Handle(NActors::TEvents::TEvWakeup::TPtr ev);
    void Handle(NActors::TEvents::TEvPoison::TPtr);

private:
    void CreateRequest(std::shared_ptr<IDqAsyncLookupSource::TUnboxedValueMap> request);
    void SendHttpRequests();
    void SendResult();
    void SendError(TStringBuf message);
    void Free();

    // Build HTTP URL from key using path template.
    TString BuildUrl(TStringBuf key) const;

    // Build request body from key using body template.
    TString BuildBody(TStringBuf key) const;

    // Check cache for a key. Returns true if found.
    bool CheckCache(TStringBuf key, TLookupResponse& response) const;

    // Store response in cache.
    void CacheResponse(TStringBuf key, const TLookupResponse& response);

    // URL-encode a key value.
    static TString UrlEncode(TStringBuf input);

    // Convert HTTP response to TUnboxedValue using PayloadType schema.
    void ConvertResponseToValue(const TLookupResponse& response, NUdf::TUnboxedValue& value);

private:
    const NActors::TActorId ParentId;
    const NActors::TActorId EgressActorId;
    const Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings Settings;
    std::shared_ptr<NKikimr::NMiniKQL::TScopedAlloc> Alloc;
    const NKikimr::NMiniKQL::TStructType* PayloadType;
    const NKikimr::NMiniKQL::THolderFactory& HolderFactory;
    const THashMap<TString, TString> SecureParams;
    const size_t MaxKeysInRequest;

    // Current request being processed.
    std::shared_ptr<IDqAsyncLookupSource::TUnboxedValueMap> Request;

    // Pending HTTP requests: requestId -> key string.
    THashMap<ui64, TString> PendingRequests;
    ui64 NextRequestId{1};

    // Completed responses: key string -> response.
    THashMap<TString, TLookupResponse> CompletedResponses;

    // Cache: key -> response (for CACHE_ACROSS_BATCHES).
    mutable THashMap<TString, TLookupResponse> Cache;
    mutable TVector<TString> CacheAccessOrder;
    mutable TInstant CacheExpiry;
    mutable ui64 CacheSizeBytes{0};
    ui64 MaxCacheSizeBytes{10 * 1024 * 1024};
};

} // namespace NYql::NDq::NHttpEgress
