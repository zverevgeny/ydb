#pragma once

#include <ydb/library/actors/core/event_local.h>
#include <ydb/library/actors/core/actor.h>
#include <ydb/library/actors/http/http.h>

#include <util/generic/string.h>

namespace NYql::NDq::NHttpEgress {

// Forward declaration.
struct IDqAsyncLookupSource;

// Event IDs for the HTTP egress actor communication protocol.
struct TEvHttpEgress {
    enum {
        EvHttpRequest = 1000000,
        EvHttpResponse,
        EvHttpError,
    };

    // Request event: sent by the caller to the egress actor.
    struct TEvHttpRequest : public NActors::TEventLocal<TEvHttpRequest, EvHttpRequest> {
        TEvHttpRequest(
            ui64 requestId,
            TStringBuf method,
            TStringBuf url,
            NHttp::THeadersBuilder headers,
            TStringBuf body,
            TDuration timeout,
            NActors::TActorId responseActor)
            : RequestId(requestId)
            , Method(method)
            , Url(url)
            , Headers(std::move(headers))
            , Body(body)
            , Timeout(timeout)
            , ResponseActor(responseActor)
        {}

        const ui64 RequestId;
        const TString Method;
        const TString Url;
        const NHttp::THeadersBuilder Headers;
        const TString Body;
        const TDuration Timeout;
        const NActors::TActorId ResponseActor;
    };

    // Response event: sent by the egress actor back to the caller on success.
    struct TEvHttpResponse : public NActors::TEventLocal<TEvHttpResponse, EvHttpResponse> {
        TEvHttpResponse(
            ui64 requestId,
            ui32 statusCode,
            TStringBuf headers,
            TStringBuf body)
            : RequestId(requestId)
            , StatusCode(statusCode)
            , Headers(headers)
            , Body(body)
        {}

        const ui64 RequestId;
        const ui32 StatusCode;
        const TString Headers;
        const TString Body;
    };

    // Error event: sent by the egress actor back to the caller on failure.
    struct TEvHttpError : public NActors::TEventLocal<TEvHttpError, EvHttpError> {
        TEvHttpError(ui64 requestId, TStringBuf message)
            : RequestId(requestId)
            , Message(message)
        {}

        const ui64 RequestId;
        const TString Message;
    };
};

} // namespace NYql::NDq::NHttpEgress
