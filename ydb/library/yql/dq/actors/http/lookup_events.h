#pragma once

#include <ydb/library/actors/core/event_local.h>
#include <ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h>

#include <memory>

namespace NYql::NDq::NHttpEgress {

// Lookup request event: sent by THttpLookupSource to THttpLookupReceiver.
// This carries the request map from the source to the receiver actor.
struct TEvLookupRequest : public NActors::TEventLocal<TEvLookupRequest, 1000100> {
    using TUnboxedValueMap = NYql::NDq::IDqAsyncLookupSource::TUnboxedValueMap;

    explicit TEvLookupRequest(std::shared_ptr<TUnboxedValueMap> request)
        : Request(std::move(request))
    {}

    std::shared_ptr<TUnboxedValueMap> Request;
};

} // namespace NYql::NDq::NHttpEgress
