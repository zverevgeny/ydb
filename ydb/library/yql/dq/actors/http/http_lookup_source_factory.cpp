#include "http_lookup_source_factory.h"
#include "http_lookup_source.h"
#include "http_egress_actor.h"
#include "http_egress_security.h"

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
                // Map from TDqHttpEgressSettings to TEgressSecurityConfig.
                const auto& egressSettings = settings.egress_settings();
                for (const auto& host : egressSettings.allowed_hosts()) {
                    securityConfig.AllowedHosts.insert(host);
                }
                for (const auto& host : egressSettings.denied_hosts()) {
                    securityConfig.DeniedHosts.insert(host);
                }
                if (egressSettings.has_max_request_body_size()) {
                    securityConfig.MaxRequestBodySize = egressSettings.max_request_body_size();
                }
                if (egressSettings.has_max_response_body_size()) {
                    securityConfig.MaxResponseBodySize = egressSettings.max_response_body_size();
                }
                if (egressSettings.has_max_in_flight_requests()) {
                    securityConfig.MaxInFlightRequests = egressSettings.max_in_flight_requests();
                }
                if (egressSettings.has_max_in_flight_requests_per_host()) {
                    securityConfig.MaxInFlightRequestsPerHost =
                        egressSettings.max_in_flight_requests_per_host();
                }
            }

            // Create the shared egress actor for this lookup source.
            // The egress actor handles the actual HTTP requests with SSRF protection,
            // concurrency limits, and security config.
            NActors::TActorId egressActorId = args.ParentId.NewChild<NHttpEgress::THttpEgressActor>(
                securityConfig,
                nullptr  // counters (can be wired later)
            );

            // Create the lookup source and paired receiver actor.
            // The receiver actor sends HTTP requests to the egress actor
            // and collects responses before emitting TEvLookupResult.
            return NHttpEgress::CreateHttpLookupSourcePair(
                std::move(settings),
                egressActorId,
                args.ParentId,  // resultTarget — send results back to compute actor
                std::move(args)
            );
        }
    );
}

} // namespace NYql::NDq
