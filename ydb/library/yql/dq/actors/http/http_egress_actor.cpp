#include "http_egress_actor.h"

#include <ydb/library/actors/core/log.h>
#include <ydb/library/actors/http/http.h>
#include <ydb/library/actors/http/http_proxy.h>

#include <util/generic/string.h>
#include <util/string/builder.h>

namespace NYql::NDq::NHttpEgress {

THttpEgressActor::THttpEgressActor(TEgressSecurityConfig config, TEgressCounters* counters)
    : NActors::TActorBootstrapped<THttpEgressActor>()
    , Config(std::move(config))
    , Counters(counters)
{
}

void THttpEgressActor::Bootstrap(const NActors::TActorContext& /* ctx */) {
    // Switch to the main state function after bootstrap.
    this->Become(&THttpEgressActor::StateFunc);
}

STFUNC(THttpEgressActor::StateFunc) {
    switch (ev->GetTypeRewrite()) {
        HFunc(TEvHttpEgress::TEvHttpRequest, HandleHttpRequest);
        HFunc(NHttp::TEvHttpProxy::TEvHttpIncomingResponse, HandleHttpIncomingResponse);
        HFunc(NActors::TEvents::TEvWakeup, HandleWakeup);
        cFunc(NActors::TEvents::TEvPoison::EventType, PassAway);
        default: {
            Y_UNUSED(ev);
        }
    }
}

void THttpEgressActor::HandleHttpRequest(TEvHttpEgress::TEvHttpRequest::TPtr& ev, const NActors::TActorContext& ctx) {
    auto* req = ev->Get();

    // Validate the request.
    if (!ValidateRequest(req)) {
        // Validation already sent error and updated counters.
        return;
    }

    // Check concurrency limits.
    TString host = ExtractHost(req->Url);
    if (!CanAcceptRequest(host)) {
        Counters->IncrementConcurrencyRejected();
        this->Send(req->ResponseActor,
            new TEvHttpEgress::TEvHttpError(req->RequestId, "Concurrency limit exceeded"));
        return;
    }

    // Build the outgoing HTTP request.
    auto outgoingRequest = NHttp::THttpOutgoingRequest::CreateRequest(
        req->Method,
        req->Url,
        {}, // content-type will be set from headers
        req->Body);

    // Apply user headers, filtering out reserved headers.
    for (const auto& [name, value] : req->Headers.Data) {
        if (!IsReservedHeader(name)) {
            outgoingRequest->Set(name, value);
        }
    }

    // Generate a unique cookie for matching the response back.
    const ui64 cookie = NextCookie++;

    // Create pending request tracker.
    auto* pending = new TPendingRequest(
        req->RequestId,   // Use caller's RequestId for all responses
        cookie,
        req->ResponseActor,
        outgoingRequest->Duplicate(), // Keep a copy for host extraction on timeout.
        req->Timeout);

    // Track the pending request by cookie for response matching.
    PendingRequests[cookie] = pending;
    InFlightPerHost[host]++;
    GlobalInFlight++;
    Counters->IncrementRequestsSent();
    Counters->IncrementActiveRequests();
    Counters->AddRequestBytes(req->Body.size());

    // Schedule timeout wakeup with cookie as Tag for O(1) lookup.
    ctx.Schedule(req->Timeout, new NActors::TEvents::TEvWakeup(cookie));

    // Dispatch the request.
    DispatchRequest(pending, std::move(outgoingRequest), ctx);
}

void THttpEgressActor::HandleHttpIncomingResponse(NHttp::TEvHttpProxy::TEvHttpIncomingResponse::TPtr& ev, const NActors::TActorContext& /* ctx */) {
    auto* httpEv = ev->Get();

    // Find the pending request by cookie.
    ui64 cookie = ev->Cookie;
    auto it = PendingRequests.find(cookie);
    if (it == PendingRequests.end()) {
        // Late response after timeout — request already cleaned up, ignore silently.
        return;
    }

    auto* pending = it->second;
    PendingRequests.erase(it);

    // Update tracking.
    TString host = ExtractHost(pending->Request->URL);
    if (auto hostIt = InFlightPerHost.find(host); hostIt != InFlightPerHost.end()) {
        if (hostIt->second > 0) {
            hostIt->second--;
            if (hostIt->second == 0) {
                InFlightPerHost.erase(hostIt);
            }
        } else {
            InFlightPerHost.erase(hostIt);
        }
    }
    GlobalInFlight--;
    Counters->DecrementActiveRequests();

    // Check for error.
    if (!httpEv->GetError().empty()) {
        Counters->IncrementErrors();
        SendError(pending, httpEv->GetError());
        delete pending;
        return;
    }

    // Check response size limit.
    TString body(httpEv->Response->Body);
    if (body.size() > Config.MaxResponseBodySize) {
        Counters->IncrementSizeLimitExceeded();
        SendError(pending, TStringBuilder() << "Response body exceeds limit: "
                     << body.size() << " > " << Config.MaxResponseBodySize);
        delete pending;
        return;
    }

    Counters->AddResponseBytes(body.size());
    Counters->IncrementResponsesReceived();

    // Parse status code from string.
    ui32 statusCode = 0;
    try {
        statusCode = static_cast<ui32>(std::stoul(TString(httpEv->Response->Status)));
    } catch (...) {
        statusCode = 0;
    }

    // Send response back to the caller using the caller's RequestId.
    this->Send(pending->ResponseActor,
        new TEvHttpEgress::TEvHttpResponse(
            pending->CallerRequestId,
            statusCode,
            httpEv->Response->Headers,
            body));

    delete pending;
}

void THttpEgressActor::HandleWakeup(NActors::TEvents::TEvWakeup::TPtr& ev, const NActors::TActorContext& /* ctx */) {
    // O(1) lookup: the cookie is passed as the wakeup Tag.
    ui64 cookie = ev->Get()->Tag;
    auto it = PendingRequests.find(cookie);
    if (it == PendingRequests.end()) {
        // Request already completed (proxy responded before timeout).
        return;
    }

    auto* p = it->second;
    PendingRequests.erase(it);

    TString host = ExtractHost(p->Request->URL);
    if (auto hostIt = InFlightPerHost.find(host); hostIt != InFlightPerHost.end()) {
        if (hostIt->second > 0) {
            hostIt->second--;
            if (hostIt->second == 0) {
                InFlightPerHost.erase(hostIt);
            }
        } else {
            InFlightPerHost.erase(hostIt);
        }
    }
    GlobalInFlight--;
    Counters->DecrementActiveRequests();
    Counters->IncrementTimeouts();
    Counters->IncrementErrors();

    SendError(p, "Request timed out");
    delete p;
}

bool THttpEgressActor::ValidateRequest(const TEvHttpEgress::TEvHttpRequest* req) {
    // Check scheme and SSRF protection.
    auto ssrfResult = CheckSSRFProtection(req->Url, Config);
    if (ssrfResult != ESSRFResult::Allowed) {
        if (ssrfResult == ESSRFResult::BlockedIP) {
            Counters->IncrementSSRFBlocks();
        } else {
            // ESSRFResult::BlockedPolicy — host not in allowlist.
            Counters->IncrementDeniedHosts();
        }
        this->Send(req->ResponseActor,
            new TEvHttpEgress::TEvHttpError(req->RequestId, "URL blocked by SSRF protection"));
        return false;
    }

    // Check request body size.
    if (req->Body.size() > Config.MaxRequestBodySize) {
        Counters->IncrementSizeLimitExceeded();
        this->Send(req->ResponseActor,
            new TEvHttpEgress::TEvHttpError(req->RequestId,
                TStringBuilder() << "Request body exceeds limit: " << req->Body.size()
                                 << " > " << Config.MaxRequestBodySize));
        return false;
    }

    // Validate headers.
    ui64 totalHeadersSize = 0;
    for (const auto& [name, value] : req->Headers.Data) {
        totalHeadersSize += name.size() + value.size();
        if (!ValidateHeader(name, value)) {
            Counters->IncrementHeaderInjectionBlocks();
            // Redact sensitive header values in error messages.
            TString redactedName = IsSensitiveHeader(name) ? "[REDACTED_NAME]" : name;
            this->Send(req->ResponseActor,
                new TEvHttpEgress::TEvHttpError(req->RequestId,
                    TStringBuilder() << "Invalid header: " << redactedName));
            return false;
        }
        // Note: reserved headers are counted toward MaxHeadersSize but will be
        // filtered out when building the outgoing request in HandleHttpRequest.
    }
    if (totalHeadersSize > Config.MaxHeadersSize) {
        Counters->IncrementSizeLimitExceeded();
        this->Send(req->ResponseActor,
            new TEvHttpEgress::TEvHttpError(req->RequestId,
                TStringBuilder() << "Total headers size exceeds limit: " << totalHeadersSize
                                 << " > " << Config.MaxHeadersSize));
        return false;
    }

    // Check timeout bounds.
    if (req->Timeout > Config.MaxTimeout) {
        this->Send(req->ResponseActor,
            new TEvHttpEgress::TEvHttpError(req->RequestId,
                TStringBuilder() << "Timeout exceeds maximum: " << req->Timeout
                                 << " > " << Config.MaxTimeout));
        return false;
    }

    return true;
}

void THttpEgressActor::DispatchRequest(TPendingRequest* pending, NHttp::THttpOutgoingRequestPtr request, const NActors::TActorContext& ctx) {
    // Get or create the HTTP proxy.
    NActors::TActorId proxyId = GetHttpProxyId(ctx);

    // Send the request through the HTTP proxy with a cookie for response matching.
    auto* proxyEvent = new NHttp::TEvHttpProxy::TEvHttpOutgoingRequest(std::move(request));
    this->Send(proxyId, proxyEvent, 0, pending->Cookie);
}

TString THttpEgressActor::ExtractHost(TStringBuf url) {
    TStringBuf scheme, host, uri;
    if (NHttp::CrackURL(url, scheme, host, uri)) {
        return TString(host);
    }
    return TString(url);
}

bool THttpEgressActor::CanAcceptRequest(TStringBuf host) {
    ui64 currentGlobal = GlobalInFlight;
    if (currentGlobal >= Config.MaxInFlightRequests) {
        return false;
    }

    auto it = InFlightPerHost.find(TString(host));
    if (it != InFlightPerHost.end() && it->second >= Config.MaxInFlightRequestsPerHost) {
        return false;
    }

    return true;
}

void THttpEgressActor::SendError(TPendingRequest* pending, TStringBuf message) {
    this->Send(pending->ResponseActor,
        new TEvHttpEgress::TEvHttpError(pending->CallerRequestId, message));
}

NActors::TActorId THttpEgressActor::GetHttpProxyId(const NActors::TActorContext& /* ctx */) {
    if (!HttpProxyId) {
        // Create an HTTP proxy actor for outgoing connections.
        HttpProxyId = this->Register(NHttp::CreateHttpProxy());
    }
    return HttpProxyId;
}

void THttpEgressActor::PassAway() {
    // Cancel all pending requests before the actor shuts down.
    // This prevents memory leaks when the egress actor is destroyed
    // while requests are still in flight.
    CancelAllPendingRequests();
    NActors::TActorBootstrapped<THttpEgressActor>::PassAway();
}

void THttpEgressActor::CancelAllPendingRequests() {
    // Cancel all pending requests when the actor is shutting down.
    // Each waiting caller receives a TEvHttpError so it does not hang forever.
    while (!PendingRequests.empty()) {
        auto it = PendingRequests.begin();
        auto* pending = it->second;
        PendingRequests.erase(it);
        
        TString host = ExtractHost(pending->Request->URL);
        if (auto hostIt = InFlightPerHost.find(host); hostIt != InFlightPerHost.end()) {
            if (hostIt->second > 0) {
                hostIt->second--;
                if (hostIt->second == 0) {
                    InFlightPerHost.erase(hostIt);
                }
            } else {
                InFlightPerHost.erase(hostIt);
            }
        }
        GlobalInFlight--;
        Counters->DecrementActiveRequests();
        
        // Notify the waiting caller that the egress actor is shutting down.
        this->Send(pending->ResponseActor,
            new TEvHttpEgress::TEvHttpError(pending->CallerRequestId,
                "Egress actor is shutting down"));
        
        delete pending;
    }
    // Clear any remaining per-host entries.
    InFlightPerHost.clear();
}

NActors::IActor* CreateHttpEgressActor(TEgressSecurityConfig config, TEgressCounters* counters) {
    return new THttpEgressActor(std::move(config), counters);
}

} // namespace NYql::NDq::NHttpEgress
