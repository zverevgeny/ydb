#include "http_lookup_source.h"

#include "lookup_events.h"

#include <yql/essentials/minikql/mkql_string_util.h>

#include <util/generic/string.h>
#include <util/string/builder.h>
#include <util/system/time.h>

#include <algorithm>
#include <cctype>

namespace NYql::NDq::NHttpEgress {

///////////////////////////////////////////////////////////////////////////////
// THttpLookupReceiver
///////////////////////////////////////////////////////////////////////////////

THttpLookupReceiver::THttpLookupReceiver(
    NActors::TActorId egressActorId,
    NActors::TActorId resultTarget,
    Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings settings,
    IMemoryQuotaManager::TPtr memoryQuotaManager,
    const THashMap<TString, TString>& secureParams,
    std::shared_ptr<NKikimr::NMiniKQL::TScopedAlloc> alloc,
    const NKikimr::NMiniKQL::TStructType* payloadType,
    const NKikimr::NMiniKQL::THolderFactory& holderFactory)
    : Settings(std::move(settings))
    , EgressActorId(egressActorId)
    , ResultTarget(resultTarget)
    , MemoryQuotaManager(std::move(memoryQuotaManager))
    , SecureParams(secureParams)
    , Alloc(std::move(alloc))
    , PayloadType(payloadType)
    , HolderFactory(holderFactory)
{
}

STFUNC(THttpLookupReceiver::StateFunc) {
    switch (ev->GetTypeRewrite()) {
        HFunc(TEvLookupRequest, HandleLookupRequest);
        HFunc(TEvHttpEgress::TEvHttpResponse, HandleHttpResponse);
        HFunc(TEvHttpEgress::TEvHttpError, HandleHttpError);
        HFunc(NActors::TEvents::TEvWakeup, HandleWakeup);
        cFunc(NActors::TEvents::TEvPoison::EventType, PassAway);
        default: {
            Y_UNUSED(ev);
        }
    }
}

void THttpLookupReceiver::HandleLookupRequest(
    TEvLookupRequest::TPtr& ev,
    const NActors::TActorContext& ctx)
{
    ProcessLookup(std::move(ev->Request));
}

TString THttpLookupReceiver::BuildUrl(TStringBuf key) const {
    TString path = Settings.path_template();
    auto pos = path.find("{key}");
    if (pos != TString::npos) {
        // URL-encode the key value for safe inclusion in the URL.
        TString encodedKey = UrlEncode(key);
        path.replace(pos, 5, encodedKey);
    }
    return TStringBuilder() << Settings.endpoint() << path;
}

TString THttpLookupReceiver::BuildBody(TStringBuf key) const {
    if (!Settings.has_body_template() || Settings.body_template().empty()) {
        return {};
    }
    TString body = Settings.body_template();
    auto pos = body.find("{key}");
    if (pos != TString::npos) {
        body.replace(pos, 5, key);
    }
    return body;
}

bool THttpLookupReceiver::CheckCache(TStringBuf key, TLookupResponse& response) const {
    if (Settings.cache_policy() != Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings::CACHE_ACROSS_BATCHES) {
        return false;
    }
    if (TInstant::Now() > CacheExpiry) {
        // Cache expired — clear all entries.
        Cache.clear();
        CacheAccessOrder.clear();
        CacheSizeBytes = 0;
        return false;
    }
    auto it = Cache.find(TString(key));
    if (it != Cache.end()) {
        response = it->second;
        // Update access order for LRU (move to end = most recently used).
        auto orderIt = std::find(CacheAccessOrder.begin(), CacheAccessOrder.end(), it->first);
        if (orderIt != CacheAccessOrder.end()) {
            CacheAccessOrder.erase(orderIt);
        }
        CacheAccessOrder.push_back(it->first);
        return true;
    }
    return false;
}

void THttpLookupReceiver::EvictCacheIfNeeded() {
    // Evict oldest (least recently used) entries until cache is within size limit.
    while (CacheSizeBytes > MaxCacheSizeBytes && !CacheAccessOrder.empty()) {
        // Remove the oldest entry (front of access order = least recently used).
        const TString& oldestKey = CacheAccessOrder.front();
        auto it = Cache.find(oldestKey);
        if (it != Cache.end()) {
            CacheSizeBytes -= it->second.Body.size() + oldestKey.size();
            Cache.erase(it);
        }
        CacheAccessOrder.erase(CacheAccessOrder.begin());
    }
}

void THttpLookupReceiver::CacheResponse(TStringBuf key, const TLookupResponse& response) {
    if (Settings.cache_policy() != Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings::CACHE_ACROSS_BATCHES) {
        return;
    }
    
    TString keyStr(key);
    ui64 entrySize = response.Body.size() + keyStr.size();
    
    // If single entry exceeds max cache size, skip caching.
    if (entrySize > MaxCacheSizeBytes) {
        return;
    }
    
    // Check if key already exists — update in place.
    auto existingIt = Cache.find(keyStr);
    if (existingIt != Cache.end()) {
        CacheSizeBytes -= existingIt->second.Body.size();
        existingIt->second = response;
        CacheSizeBytes += response.Body.size();
        // Update access order.
        auto orderIt = std::find(CacheAccessOrder.begin(), CacheAccessOrder.end(), keyStr);
        if (orderIt != CacheAccessOrder.end()) {
            CacheAccessOrder.erase(orderIt);
        }
        CacheAccessOrder.push_back(keyStr);
    } else {
        // Evict if needed before adding new entry.
        CacheSizeBytes += entrySize;
        EvictCacheIfNeeded();
        Cache[keyStr] = response;
        CacheAccessOrder.push_back(keyStr);
    }
    
    CacheExpiry = TInstant::Now() + TDuration::Seconds(Settings.cache_ttl_seconds());
}

TString THttpLookupReceiver::UrlEncode(TStringBuf input) {
    TString result;
    result.reserve(input.size());
    for (auto c : input) {
        // Safe characters: alphanumeric and -._~
        if (std::isalnum(static_cast<unsigned char>(c)) ||
            c == '-' || c == '.' || c == '_' || c == '~') {
            result += c;
        } else {
            // Encode as %XX
            result += '%';
            result += static_cast<char>("0123456789ABCDEF"[(static_cast<unsigned char>(c) >> 4) & 0xF]);
            result += static_cast<char>("0123456789ABCDEF"[static_cast<unsigned char>(c) & 0xF]);
        }
    }
    return result;
}

void THttpLookupReceiver::ProcessLookup(
    std::shared_ptr<IDqAsyncLookupSource::TUnboxedValueMap> request)
{
    using TUnboxedValue = NKikimr::NMiniKQL::TUnboxedValue;

    // Guard against concurrent lookups — the receiver is designed for sequential processing.
    // If the compute actor sends overlapping lookups, the second will fail fast.
    Y_ASSERT(PendingRequests.empty(),
        "THttpLookupReceiver: concurrent lookups not supported — previous lookup still in progress");

    if (!request || request->empty()) {
        // Empty request — send empty result immediately.
        CurrentRequest = std::move(request);
        SendResult();
        return;
    }

    CurrentRequest = request;
    PendingRequests.clear();
    CompletedResponses.clear();
    NextRequestId = 1;
    AccountedMemory = 0;

    // Collect unique keys and check cache.
    THashSet<TString> seenKeys;

    for (const auto& [key, value] : *request) {
        TString keyStr = key.ToString();

        // Skip duplicates within this batch.
        if (!seenKeys.insert(keyStr).second) {
            continue;
        }

        // Check cross-batch cache.
        TLookupResponse cachedResponse;
        if (CheckCache(keyStr, cachedResponse)) {
            // Cache hit — store directly.
            CompletedResponses[keyStr] = cachedResponse;
            continue;
        }

        // Build URL and body.
        TString url = BuildUrl(keyStr);
        TString body = BuildBody(keyStr);

        // Assign request ID.
        ui64 requestId = NextRequestId++;
        PendingRequests[requestId] = keyStr;

        // Build headers.
        NHttp::THeadersBuilder headers;
        for (const auto& header : Settings.headers()) {
            headers.Set(header.name(), header.value());
        }

        // Add auth token if configured.
        if (!Settings.auth_token_secret_name().empty()) {
            auto it = SecureParams.find(Settings.auth_token_secret_name());
            if (it != SecureParams.end()) {
                headers.Set("Authorization", TStringBuilder() << "Bearer " << it->second);
            }
        }

        // Calculate timeout.
        TDuration timeout = TDuration::Milliseconds(Settings.timeout_ms());

        // Set wakeup for timeout.
        ctx.ScheduleTimeout(+ctx, new NActors::TEvents::TEvWakeup(requestId), timeout);

        // Send request to egress actor, targeting self as the response actor.
        auto* req = new TEvHttpEgress::TEvHttpRequest(
            requestId,
            Settings.method(),
            url,
            headers,
            body,
            timeout,
            NActors::self()->ID());

        NActors::Send(EgressActorId, req);
    }

    // If all keys were cache hits (no pending requests), send result immediately.
    if (PendingRequests.empty()) {
        SendResult();
    }
}

void THttpLookupReceiver::HandleHttpResponse(
    TEvHttpEgress::TEvHttpResponse::TPtr& ev,
    const NActors::TActorContext& ctx)
{
    ui64 requestId = ev->RequestId;
    auto it = PendingRequests.find(requestId);
    if (it == PendingRequests.end()) {
        // Stale response (already timed out), ignore.
        return;
    }

    const TString& keyStr = it->second;
    PendingRequests.erase(it);

    // Build response object.
    TLookupResponse response;
    response.StatusCode = ev->StatusCode;
    response.Body = ev->Body;
    response.Success = ev->StatusCode >= 200 && ev->StatusCode < 300;

    // Check response body size limit.
    if (Settings.has_max_response_size() && response.Body.size() > Settings.max_response_size()) {
        response.Success = false;
        response.Error = "Response body exceeds maximum size";
    }

    // Account memory for response body.
    if (MemoryQuotaManager && response.Success) {
        if (!MemoryQuotaManager->AllocateQuota(response.Body.size())) {
            response.Success = false;
            response.Error = "Memory quota exceeded";
        } else {
            AccountedMemory += response.Body.size();
        }
    }

    // Store in cross-batch cache.
    if (response.Success) {
        CacheResponse(keyStr, response);
    }

    // Store completed response.
    CompletedResponses[keyStr] = response;

    // Check if all requests completed.
    if (PendingRequests.empty()) {
        SendResult();
    }
}

void THttpLookupReceiver::HandleHttpError(
    TEvHttpEgress::TEvHttpError::TPtr& ev,
    const NActors::TActorContext& ctx)
{
    ui64 requestId = ev->RequestId;
    auto it = PendingRequests.find(requestId);
    if (it == PendingRequests.end()) {
        // Stale error (already timed out), ignore.
        return;
    }

    const TString& keyStr = it->second;
    PendingRequests.erase(it);

    // Store error response.
    TLookupResponse response;
    response.Success = false;
    response.Error = ev->Message;
    CompletedResponses[keyStr] = response;

    // Check if all requests completed.
    if (PendingRequests.empty()) {
        SendResult();
    }
}

void THttpLookupReceiver::HandleWakeup(
    NActors::TEvents::TEvWakeup::TPtr& ev,
    const NActors::TActorContext& ctx)
{
    ui64 requestId = ev->Tag;
    auto it = PendingRequests.find(requestId);
    if (it != PendingRequests.end()) {
        const TString& keyStr = it->second;
        PendingRequests.erase(it);

        // Request timed out.
        TLookupResponse response;
        response.Success = false;
        response.Error = "Request timeout";
        CompletedResponses[keyStr] = response;

        // Check if all requests completed.
        if (PendingRequests.empty()) {
            SendResult();
        }
    }
}

NKikimr::NUdf::TUnboxedValue THttpLookupReceiver::ConvertResponseToValue(
    const TLookupResponse& response)
{
    using TUnboxedValue = NKikimr::NUdf::TUnboxedValue;

    if (!response.Success) {
        // Error response — return null.
        return TUnboxedValue();
    }

    if (!PayloadType) {
        // No payload type schema — return body as string.
        return NKikimr::NMiniKQL::MakeString(NUdf::TStringRef(response.Body));
    }

    // Validate PayloadType has at least 3 members: StatusCode, Headers, Body.
    Y_ASSERT(PayloadType->GetMembersCount() >= 3,
        "THttpLookupReceiver: PayloadType must have at least 3 members (StatusCode, Headers, Body), "
        "got %lu", static_cast<unsigned long>(PayloadType->GetMembersCount()));

    // Build Struct<StatusCode, Headers, Body> using HolderFactory.
    // The PayloadType is expected to be a struct with 3 members:
    //   0: StatusCode (ui32)
    //   1: Headers (string)
    //   2: Body (string)
    NUdf::TUnboxedValue* valueItems = nullptr;
    auto result = HolderFactory.CreateDirectArrayHolder(3, valueItems);

    // Field 0: StatusCode (ui32)
    valueItems[0] = NKikimr::NMiniKQL::MakeEmbeddedValue(response.StatusCode);

    // Field 1: Headers (string) — empty for now, could be populated from response headers
    valueItems[1] = NKikimr::NMiniKQL::MakeString(NUdf::TStringRef());

    // Field 2: Body (string)
    valueItems[2] = NKikimr::NMiniKQL::MakeString(NUdf::TStringRef(response.Body));

    return result;
}

void THttpLookupReceiver::Free() {
    if (Alloc) {
        auto guard = NKikimr::NMiniKQL::Guard(*Alloc);
        CurrentRequest.reset();
        CompletedResponses.clear();
        Cache.clear();
    }
}

void THttpLookupReceiver::SendResult() {
    using TUnboxedValue = NKikimr::NMiniKQL::TUnboxedValue;
    using TUnboxedValueMap = IDqAsyncLookupSource::TUnboxedValueMap;

    // Build result map from completed responses.
    TUnboxedValueMap results;

    if (CurrentRequest) {
        for (const auto& [key, value] : *CurrentRequest) {
            TString keyStr = key.ToString();
            auto it = CompletedResponses.find(keyStr);
            if (it != CompletedResponses.end()) {
                // Convert TLookupResponse to TUnboxedValue using PayloadType schema.
                results[key] = ConvertResponseToValue(it->second);
            }
        }
    }

    // Free accounted memory.
    if (MemoryQuotaManager && AccountedMemory > 0) {
        MemoryQuotaManager->FreeQuota(AccountedMemory);
        AccountedMemory = 0;
    }

    // Send result to the target actor.
    auto sharedResults = std::make_shared<TUnboxedValueMap>(std::move(results));
    NActors::Send(ResultTarget,
        new IDqAsyncLookupSource::TEvLookupResult(sharedResults, sharedResults->size()));

    // Note: Do NOT call PassAway() here. The receiver actor must persist to handle
    // subsequent AsyncLookup() calls from the same THttpLookupSource instance.
    // The receiver will be destroyed when the parent (compute actor) shuts down,
    // which will deliver a poison pill or trigger parent-based cleanup.
}

void THttpLookupReceiver::PassAway() {
    Free();
    TActorBootstrapped<THttpLookupReceiver>::PassAway();
}

///////////////////////////////////////////////////////////////////////////////
// THttpLookupSource
///////////////////////////////////////////////////////////////////////////////

THttpLookupSource::THttpLookupSource(
    Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings settings,
    NActors::TActorId receiverActorId)
    : Settings(std::move(settings))
    , ReceiverActorId(std::move(receiverActorId))
{
}

size_t THttpLookupSource::GetMaxSupportedKeysInRequest() const {
    return Settings.max_batch_size();
}

void THttpLookupSource::AsyncLookup(std::weak_ptr<TUnboxedValueMap> request) {
    auto sharedRequest = request.lock();
    if (!sharedRequest) {
        // Request already expired — nothing to do.
        return;
    }

    // Send lookup request to the receiver actor.
    NActors::Send(ReceiverActorId, new TEvLookupRequest(std::move(sharedRequest)));
}

NActors::IActor* THttpLookupSource::GetReceiverActor() {
    // The actual actor pointer is managed by the factory.
    // This method is a placeholder for future use.
    return nullptr;
}

///////////////////////////////////////////////////////////////////////////////
// Factory
///////////////////////////////////////////////////////////////////////////////

std::pair<IDqAsyncLookupSource*, NActors::IActor*> CreateHttpLookupSourcePair(
    Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings settings,
    NActors::TActorId egressActorId,
    NActors::TActorId resultTarget,
    IDqAsyncIoFactory::TLookupSourceArguments&& args)
{
    // Create the receiver actor.
    // The receiver is both the IDqAsyncLookupSource collaborator and the NActors::IActor.
    //
    // KNOWN LIMITATION: IMemoryQuotaManager is passed as nullptr because
    // TLookupSourceArguments does not include a MemoryQuotaManager field
    // (only TSourceArguments does, see dq_compute_actor_async_io.h:271).
    // This means response body bytes are NOT accounted against the memory quota.
    //
    // TO FIX: Extend TLookupSourceArguments with IMemoryQuotaManager::TPtr field,
    // wire it from the compute actor, and pass it here instead of nullptr.
    auto receiver = args.ParentId.NewChild<THttpLookupReceiver>(
        egressActorId,
        resultTarget,
        Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings(settings),
        nullptr,  // IMemoryQuotaManager — not in TLookupSourceArguments (see NOTE above)
        args.SecureParams,
        args.Alloc,
        args.PayloadType,
        args.HolderFactory);

    // Create the source object (heap-allocated, lifetime managed by caller).
    auto* source = new THttpLookupSource(
        std::move(settings),
        receiver);

    return {source, receiver};
}

} // namespace NYql::NDq::NHttpEgress
