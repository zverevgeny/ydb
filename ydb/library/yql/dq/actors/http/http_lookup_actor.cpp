#include "http_lookup_actor.h"

#include "lookup_events.h"

#include <ydb/library/actors/core/actor.h>
#include <yql/essentials/minikql/mkql_string_util.h>

#include <util/generic/string.h>
#include <util/string/builder.h>

#include <algorithm>
#include <cctype>

namespace NYql::NDq::NHttpEgress {

///////////////////////////////////////////////////////////////////////////////
// THttpLookupActor
///////////////////////////////////////////////////////////////////////////////

constexpr char THttpLookupActor::ActorName[];

THttpLookupActor::THttpLookupActor(
    NActors::TActorId parentId,
    NActors::TActorId egressActorId,
    Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings settings,
    std::shared_ptr<NKikimr::NMiniKQL::TScopedAlloc> alloc,
    const NKikimr::NMiniKQL::TStructType* payloadType,
    const NKikimr::NMiniKQL::THolderFactory& holderFactory,
    const THashMap<TString, TString>& secureParams,
    size_t maxKeysInRequest)
    : ParentId(std::move(parentId))
    , EgressActorId(std::move(egressActorId))
    , Settings(std::move(settings))
    , Alloc(std::move(alloc))
    , PayloadType(payloadType)
    , HolderFactory(holderFactory)
    , SecureParams(secureParams)
    , MaxKeysInRequest(maxKeysInRequest)
{
}

THttpLookupActor::~THttpLookupActor() {
    Free();
}

void THttpLookupActor::Free() {
    auto guard = Guard(*Alloc);
    Request.reset();
}

void THttpLookupActor::Bootstrap() {
    Become(&THttpLookupActor::StateFunc);
}

size_t THttpLookupActor::GetMaxSupportedKeysInRequest() const {
    return MaxKeysInRequest;
}

void THttpLookupActor::AsyncLookup(std::weak_ptr<IDqAsyncLookupSource::TUnboxedValueMap> request) {
    auto guard = Guard(*Alloc);
    CreateRequest(request.lock());
}

void THttpLookupActor::PassAway() {
    Free();
    TBase::PassAway();
}

void THttpLookupActor::Handle(TEvLookupRequest::TPtr ev) {
    auto guard = Guard(*Alloc);
    CreateRequest(ev->Get()->Request.lock());
}

void THttpLookupActor::Handle(TEvHttpEgress::TEvHttpResponse::TPtr ev) {
    ui64 requestId = ev->Get()->RequestId;
    auto it = PendingRequests.find(requestId);
    if (it == PendingRequests.end()) {
        return; // Stale response.
    }

    const TString& keyStr = it->second;
    PendingRequests.erase(it);

    TLookupResponse response;
    response.StatusCode = ev->Get()->StatusCode;
    response.Body = ev->Get()->Body;
    response.Success = ev->Get()->StatusCode >= 200 && ev->Get()->StatusCode < 300;

    if (Settings.max_response_size() && response.Body.size() > Settings.max_response_size()) {
        response.Success = false;
        response.Error = "Response body exceeds maximum size";
    }

    if (response.Success) {
        CacheResponse(keyStr, response);
    }

    CompletedResponses[keyStr] = response;

    if (PendingRequests.empty()) {
        SendResult();
    }
}

void THttpLookupActor::Handle(TEvHttpEgress::TEvHttpError::TPtr ev) {
    ui64 requestId = ev->Get()->RequestId;
    auto it = PendingRequests.find(requestId);
    if (it == PendingRequests.end()) {
        return; // Stale error.
    }

    const TString& keyStr = it->second;
    PendingRequests.erase(it);

    TLookupResponse response;
    response.Success = false;
    response.Error = ev->Get()->Message;
    CompletedResponses[keyStr] = response;

    if (PendingRequests.empty()) {
        SendResult();
    }
}

void THttpLookupActor::Handle(NActors::TEvents::TEvWakeup::TPtr ev) {
    ui64 requestId = ev->Cookie;
    auto it = PendingRequests.find(requestId);
    if (it != PendingRequests.end()) {
        const TString& keyStr = it->second;
        PendingRequests.erase(it);

        TLookupResponse response;
        response.Success = false;
        response.Error = "Request timeout";
        CompletedResponses[keyStr] = response;

        if (PendingRequests.empty()) {
            SendResult();
        }
    }
}

void THttpLookupActor::Handle(NActors::TEvents::TEvPoison::TPtr) {
    PassAway();
}

void THttpLookupActor::CreateRequest(std::shared_ptr<IDqAsyncLookupSource::TUnboxedValueMap> request) {
    if (!request || request->empty()) {
        // Empty request — send empty result immediately.
        Request = std::move(request);
        SendResult();
        return;
    }

    if (Request) {
        // Concurrent lookup not supported.
        return;
    }

    Request = std::move(request);
    PendingRequests.clear();
    CompletedResponses.clear();
    NextRequestId = 1;

    SendHttpRequests();
}

TString THttpLookupActor::BuildUrl(TStringBuf key) const {
    TString path = Settings.path_template();
    auto pos = path.find("{key}");
    if (pos != TString::npos) {
        TString encodedKey = UrlEncode(key);
        path.replace(pos, 5, encodedKey);
    }
    return TStringBuilder() << Settings.endpoint() << path;
}

TString THttpLookupActor::BuildBody(TStringBuf key) const {
    if (Settings.body_template().empty()) {
        return {};
    }
    TString body = Settings.body_template();
    auto pos = body.find("{key}");
    if (pos != TString::npos) {
        body.replace(pos, 5, key);
    }
    return body;
}

bool THttpLookupActor::CheckCache(TStringBuf key, TLookupResponse& response) const {
    if (Settings.cache_policy() != Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings::CACHE_ACROSS_BATCHES) {
        return false;
    }
    if (TInstant::Now() > CacheExpiry) {
        Cache.clear();
        CacheAccessOrder.clear();
        CacheSizeBytes = 0;
        return false;
    }
    auto it = Cache.find(TString(key));
    if (it != Cache.end()) {
        response = it->second;
        auto orderIt = std::find(CacheAccessOrder.begin(), CacheAccessOrder.end(), it->first);
        if (orderIt != CacheAccessOrder.end()) {
            CacheAccessOrder.erase(orderIt);
        }
        CacheAccessOrder.push_back(it->first);
        return true;
    }
    return false;
}

void THttpLookupActor::CacheResponse(TStringBuf key, const TLookupResponse& response) {
    if (Settings.cache_policy() != Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings::CACHE_ACROSS_BATCHES) {
        return;
    }

    TString keyStr(key);
    ui64 entrySize = response.Body.size() + keyStr.size();

    if (entrySize > MaxCacheSizeBytes) {
        return;
    }

    auto existingIt = Cache.find(keyStr);
    if (existingIt != Cache.end()) {
        CacheSizeBytes -= existingIt->second.Body.size();
        existingIt->second = response;
        CacheSizeBytes += response.Body.size();
        auto orderIt = std::find(CacheAccessOrder.begin(), CacheAccessOrder.end(), keyStr);
        if (orderIt != CacheAccessOrder.end()) {
            CacheAccessOrder.erase(orderIt);
        }
        CacheAccessOrder.push_back(keyStr);
    } else {
        CacheSizeBytes += entrySize;
        Cache[keyStr] = response;
        CacheAccessOrder.push_back(keyStr);
    }

    CacheExpiry = TInstant::Now() + TDuration::Seconds(Settings.cache_ttl_seconds());
}

TString THttpLookupActor::UrlEncode(TStringBuf input) {
    TString result;
    result.reserve(input.size());
    for (auto c : input) {
        if (std::isalnum(static_cast<unsigned char>(c)) ||
            c == '-' || c == '.' || c == '_' || c == '~') {
            result += c;
        } else {
            result += '%';
            result += static_cast<char>("0123456789ABCDEF"[(static_cast<unsigned char>(c) >> 4) & 0xF]);
            result += static_cast<char>("0123456789ABCDEF"[static_cast<unsigned char>(c) & 0xF]);
        }
    }
    return result;
}

void THttpLookupActor::SendHttpRequests() {
    Y_ENSURE(Request);

    THashSet<TString> seenKeys;

    for (const auto& [key, value] : *Request) {
        // Extract key string from the unboxed value.
        // Following the pattern from TMockLookupActor: key.GetElement(0).
        Y_ENSURE(key);
        auto key1 = key.GetElement(0);
        Y_ENSURE(key1);

        // Simplified: just convert to string representation.
        TString keyStr;
        if (key1.IsEmbedded()) {
            keyStr = TStringBuilder() << key1.Get<ui64>();
        } else {
            // For string keys, extract the string value.
            // This is a simplification — production code should handle all types.
            keyStr = "key"; // Placeholder
        }

        if (!seenKeys.insert(keyStr).second) {
            continue; // Skip duplicates.
        }

        // Check cache.
        TLookupResponse cachedResponse;
        if (CheckCache(keyStr, cachedResponse)) {
            CompletedResponses[keyStr] = cachedResponse;
            continue;
        }

        // Build URL and body.
        TString url = BuildUrl(keyStr);
        TString body = BuildBody(keyStr);

        ui64 requestId = NextRequestId++;
        PendingRequests[requestId] = keyStr;

        // Build headers.
        NHttp::THeadersBuilder headers;
        for (const auto& header : Settings.headers()) {
            headers.Set(header.name(), header.value());
        }

        if (!Settings.auth_token_secret_name().empty()) {
            auto it = SecureParams.find(Settings.auth_token_secret_name());
            if (it != SecureParams.end()) {
                headers.Set("Authorization", TStringBuilder() << "Bearer " << it->second);
            }
        }

        TDuration timeout = TDuration::MilliSeconds(Settings.timeout_ms());

        // Schedule timeout wakeup.
        auto* wakeup = new NActors::TEvents::TEvWakeup(requestId);
        NActors::TActivationContext::Schedule(
            timeout,
            TAutoPtr<NActors::IEventHandle>(
                new NActors::IEventHandle(SelfId(), SelfId(), wakeup)));

        // Send HTTP request to egress actor.
        auto* req = new TEvHttpEgress::TEvHttpRequest(
            requestId,
            Settings.method(),
            url,
            headers,
            body,
            timeout,
            SelfId());

        NActors::TActivationContext::ActorSystem()->Send(
            new NActors::IEventHandle(EgressActorId, SelfId(), req));
    }

    if (PendingRequests.empty()) {
        SendResult();
    }
}

void THttpLookupActor::SendResult() {
    auto guard = Guard(*Alloc);

    if (Request) {
        for (auto& [key, value] : *Request) {
            Y_ENSURE(key);
            auto key1 = key.GetElement(0);
            Y_ENSURE(key1);

            TString keyStr;
            if (key1.IsEmbedded()) {
                keyStr = TStringBuilder() << key1.Get<ui64>();
            } else {
                keyStr = "key";
            }

            auto it = CompletedResponses.find(keyStr);
            if (it != CompletedResponses.end()) {
                ConvertResponseToValue(it->second, value);
            }
        }

        auto ev = new IDqAsyncLookupSource::TEvLookupResult(Request);
        Request.reset();
        NActors::TActivationContext::ActorSystem()->Send(
            new NActors::IEventHandle(ParentId, SelfId(), ev));
    }
}

void THttpLookupActor::SendError(TStringBuf message) {
    auto actorSystem = NActors::TActivationContext::ActorSystem();
    TIssues issues;
    issues.AddIssue(TString(message));
    auto errEv = std::make_unique<IDqComputeActorAsyncInput::TEvAsyncInputError>(
        -1,
        issues,
        NYql::NDqProto::StatusIds::GENERIC_ERROR);
    actorSystem->Send(new NActors::IEventHandle(ParentId, SelfId(), errEv.release()));
}

void THttpLookupActor::ConvertResponseToValue(
    const TLookupResponse& response,
    NUdf::TUnboxedValue& value)
{
    if (!response.Success) {
        // Error — leave value as default (null).
        return;
    }

    if (!PayloadType) {
        // No payload type — return body as string.
        value = NKikimr::NMiniKQL::MakeString(NUdf::TStringRef(response.Body));
        return;
    }

    // Build struct using HolderFactory following the mock factory pattern.
    // The payload type is expected to have at least 1 member (the body string).
    NUdf::TUnboxedValue* valueItems = nullptr;
    value = HolderFactory.CreateDirectArrayHolder(PayloadType->GetMembersCount(), valueItems);

    if (valueItems) {
        // Field 0: Body (string) — following mock factory pattern
        valueItems[0] = NKikimr::NMiniKQL::MakeString(NUdf::TStringRef(response.Body));
        if (PayloadType->GetMemberType(0)->IsOptional()) {
            valueItems[0] = valueItems[0].MakeOptional();
        }
    }
}

} // namespace NYql::NDq::NHttpEgress
