#pragma once

#include "events.h"
#include "http_egress_counters.h"
#include "http_egress_security.h"

#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/hfunc.h>
#include <ydb/library/actors/http/http_proxy.h>

#include <util/generic/map.h>
#include <util/generic/set.h>
#include <util/system/hp_timer.h>

namespace NYql::NDq::NHttpEgress {

// A pending HTTP request tracked by the egress actor.
struct TPendingRequest {
    TPendingRequest(
        ui64 callerRequestId,
        ui64 cookie,
        NActors::TActorId responseActor,
        NHttp::THttpOutgoingRequestPtr request,
        TDuration timeout,
        TString pinnedIp = {})
        : CallerRequestId(callerRequestId)
        , Cookie(cookie)
        , ResponseActor(responseActor)
        , Request(std::move(request))
        , Timeout(timeout)
        , PinnedIP(std::move(pinnedIp))
    {
        Timer.Reset();
    }

    /// RequestId provided by the caller (used in all responses/errors).
    const ui64 CallerRequestId;
    /// Cookie used to match the proxy response back to this pending request.
    const ui64 Cookie;
    const NActors::TActorId ResponseActor;
    const NHttp::THttpOutgoingRequestPtr Request;
    const TDuration Timeout;
    /// Pinned IP address for DNS rebinding protection (empty if not pinned).
    const TString PinnedIP;
    THPTimer Timer;
};

// THttpEgressActor is a per-node singleton actor that performs non-blocking HTTP
// requests using the existing ydb/library/actors/http infrastructure.
//
// It enforces:
// - Allow/deny host list + SSRF protection
// - Request/response size limits
// - Header injection prevention
// - Global and per-host concurrency limits
// - Timeout enforcement
// - Monitoring counters
//
// Protocol:
// - Caller sends TEvHttpRequest → actor sends TEvHttpResponse or TEvHttpError back
// - The actor uses the existing HTTP proxy from ydb/library/actors/http
class THttpEgressActor : public NActors::TActorBootstrapped<THttpEgressActor> {
public:
    THttpEgressActor(TEgressSecurityConfig config, TEgressCounters* counters);

    void Bootstrap(const NActors::TActorContext& ctx);

private:
    STFUNC(StateFunc);

    // Event handlers.
    void HandleHttpRequest(TEvHttpEgress::TEvHttpRequest::TPtr& ev, const NActors::TActorContext& ctx);
    void HandleHttpIncomingResponse(NHttp::TEvHttpProxy::TEvHttpIncomingResponse::TPtr& ev, const NActors::TActorContext& ctx);
    void HandleWakeup(NActors::TEvents::TEvWakeup::TPtr& ev, const NActors::TActorContext& ctx);

    // Cleanup all pending requests (called on PassAway).
    void CancelAllPendingRequests();

    // Override PassAway to cancel pending requests before shutdown.
    void PassAway() override;

    // Request validation and dispatch.
    bool ValidateRequest(const TEvHttpEgress::TEvHttpRequest* req);
    void DispatchRequest(TPendingRequest* pending, NHttp::THttpOutgoingRequestPtr request, const NActors::TActorContext& ctx);

    // Extract host from the request URL for per-host tracking.
    TString ExtractHost(TStringBuf url);

    // Check if we can accept a new request (concurrency limits).
    bool CanAcceptRequest(TStringBuf host);

    // Send error response to the caller.
    void SendError(TPendingRequest* pending, TStringBuf message);

    // Get or create the HTTP proxy actor ID.
    NActors::TActorId GetHttpProxyId(const NActors::TActorContext& ctx);

    // Configuration.
    const TEgressSecurityConfig Config;

    // Counters reference.
    TEgressCounters* Counters;

    // Pending requests: keyed by cookie for response matching.
    THashMap<ui64, TPendingRequest*> PendingRequests;

    // Per-host in-flight counter.
    THashMap<TString, ui64> InFlightPerHost;

    // Global in-flight counter (single-threaded actor, no atomic needed).
    ui64 GlobalInFlight{0};

    // Next cookie for response matching.
    ui64 NextCookie{1};

    // HTTP proxy actor ID (lazy-initialized).
    NActors::TActorId HttpProxyId;
};

// Factory function to create the egress actor.
NActors::IActor* CreateHttpEgressActor(TEgressSecurityConfig config, TEgressCounters* counters);

} // namespace NYql::NDq::NHttpEgress
