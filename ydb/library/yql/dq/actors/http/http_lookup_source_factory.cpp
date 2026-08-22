#include "http_lookup_source_factory.h"
#include "http_lookup_actor.h"
#include "http_egress_actor.h"
#include "http_egress_security.h"

#include <ydb/library/actors/core/actor.h>

namespace NYql::NDq {

void RegisterHttpLookupSourceFactory(TDqAsyncIoFactory& factory) {
    factory.RegisterLookupSource<Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings>(
        "HttpLookup",
        [](Ydb::Dq::HttpLookup::TDqHttpLookupSourceSettings&& settings,
           IDqAsyncIoFactory::TLookupSourceArguments&& args)
        {
            // Build egress security config from lookup source settings.
            NHttpEgress::TEgressSecurityConfig securityConfig;
            if (settings.has_egress_settings()) {
                const auto& egressSettings = settings.egress_settings();
                for (const auto& host : egressSettings.allowed_hosts()) {
                    securityConfig.AllowedHosts.insert(host);
                }
                for (const auto& host : egressSettings.denied_hosts()) {
                    securityConfig.DeniedHosts.insert(host);
                }
                if (egressSettings.max_request_body_size()) {
                    securityConfig.MaxRequestBodySize = egressSettings.max_request_body_size();
                }
                if (egressSettings.max_response_body_size()) {
                    securityConfig.MaxResponseBodySize = egressSettings.max_response_body_size();
                }
                if (egressSettings.max_in_flight_requests()) {
                    securityConfig.MaxInFlightRequests = egressSettings.max_in_flight_requests();
                }
                if (egressSettings.max_in_flight_requests_per_host()) {
                    securityConfig.MaxInFlightRequestsPerHost =
                        egressSettings.max_in_flight_requests_per_host();
                }
            }

            // Create the egress actor as a child of the parent (compute actor).
            auto* egressActor = NHttpEgress::CreateHttpEgressActor(securityConfig, nullptr);
            NActors::TActorId egressActorId = NActors::TActivationContext::Register(
                egressActor, args.ParentId);

            // Create the unified lookup actor.
            auto* actor = new NHttpEgress::THttpLookupActor(
                args.ParentId,
                egressActorId,
                std::move(settings),
                args.Alloc,
                args.PayloadType,
                args.HolderFactory,
                args.SecureParams,
                args.MaxKeysInRequest);

            return std::make_pair<IDqAsyncLookupSource*, NActors::IActor*>(actor, actor);
        }
    );
}

} // namespace NYql::NDq
