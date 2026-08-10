#pragma once

#include <ydb/library/yql/providers/pq/gateway/abstract/yql_pq_gateway.h>

#include <util/generic/string.h>

namespace NYql {

// Settings used to build a YT-queue PQ gateway. Each configured cluster maps a
// logical cluster name to a YTsaurus proxy endpoint and an authentication token.
struct TYtPqGatewaySettings {
    struct TCluster {
        TString Name;
        TString Endpoint; // YT proxy url (e.g. hahn.yt.yandex.net)
        TString Token;    // OAuth token; may be empty for local clusters
        TString PathPrefix;
        TString DataColumn = "data";
    };

    TVector<TCluster> Clusters;
};

// Creates a PQ gateway that talks to YTsaurus queues instead of YDB topics.
IPqGateway::TPtr CreateYtPqGateway(const TYtPqGatewaySettings& settings);

} // namespace NYql
