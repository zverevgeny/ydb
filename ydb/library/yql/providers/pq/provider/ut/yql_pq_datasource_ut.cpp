#include <ydb/library/yql/providers/pq/provider/yql_pq_provider.h>
#include <ydb/library/yql/providers/pq/provider/yql_pq_settings.h>
#include <ydb/library/yql/providers/pq/gateway/dummy/yql_pq_dummy_gateway.h>

#include <yql/essentials/core/yql_type_annotation.h>
#include <yql/essentials/providers/common/proto/gateways_config.pb.h>

#include <library/cpp/testing/unittest/registar.h>

namespace NYql {

namespace {

TPqState::TPtr MakeTestState(TTypeAnnotationContext& typeCtx) {
    auto state = MakeIntrusive<TPqState>("test_session");
    state->Types = &typeCtx;
    state->Configuration = MakeIntrusive<TPqConfiguration>();
    return state;
}

} // namespace

Y_UNIT_TEST_SUITE(PqDataSourceAddClusterTest) {
    Y_UNIT_TEST(AddYtClusterSetsClusterTypeYt) {
        TTypeAnnotationContext typeCtx;
        auto state = MakeTestState(typeCtx);
        auto gateway = CreatePqFileGateway();

        auto provider = CreatePqDataSource(state, gateway);

        THashMap<TString, TString> properties;
        properties["source_type"] = "yt";
        properties["location"] = "yt-cluster.example.com:8080";
        properties["token"] = "test-token";
        provider->AddCluster("yt_cluster", properties);

        const auto* clusterSettings = state->Configuration->ClustersConfigurationSettings.FindPtr("yt_cluster");
        UNIT_ASSERT(clusterSettings);
        UNIT_ASSERT_VALUES_EQUAL(static_cast<int>(clusterSettings->ClusterType), static_cast<int>(NYql::TPqClusterConfig::CT_YT));
        UNIT_ASSERT_VALUES_EQUAL(clusterSettings->Endpoint, "yt-cluster.example.com:8080");
    }

    Y_UNIT_TEST(AddYtClusterCaseInsensitive) {
        TTypeAnnotationContext typeCtx;
        auto state = MakeTestState(typeCtx);
        auto gateway = CreatePqFileGateway();

        auto provider = CreatePqDataSource(state, gateway);

        THashMap<TString, TString> properties;
        properties["source_type"] = "YT";
        provider->AddCluster("yt_cluster_upper", properties);

        const auto* clusterSettings = state->Configuration->ClustersConfigurationSettings.FindPtr("yt_cluster_upper");
        UNIT_ASSERT(clusterSettings);
        UNIT_ASSERT_VALUES_EQUAL(static_cast<int>(clusterSettings->ClusterType), static_cast<int>(NYql::TPqClusterConfig::CT_YT));
    }

    Y_UNIT_TEST(AddDataStreamsClusterByDefault) {
        TTypeAnnotationContext typeCtx;
        auto state = MakeTestState(typeCtx);
        auto gateway = CreatePqFileGateway();

        auto provider = CreatePqDataSource(state, gateway);

        THashMap<TString, TString> properties;
        properties["location"] = "ydb-cluster.example.com:2135";
        provider->AddCluster("ds_cluster", properties);

        const auto* clusterSettings = state->Configuration->ClustersConfigurationSettings.FindPtr("ds_cluster");
        UNIT_ASSERT(clusterSettings);
        UNIT_ASSERT_VALUES_EQUAL(static_cast<int>(clusterSettings->ClusterType), static_cast<int>(NYql::TPqClusterConfig::CT_DATA_STREAMS));
    }

    Y_UNIT_TEST(AddClusterUnknownSourceTypeFallsBackToDataStreams) {
        TTypeAnnotationContext typeCtx;
        auto state = MakeTestState(typeCtx);
        auto gateway = CreatePqFileGateway();

        auto provider = CreatePqDataSource(state, gateway);

        THashMap<TString, TString> properties;
        properties["source_type"] = "ydb";
        provider->AddCluster("other_cluster", properties);

        const auto* clusterSettings = state->Configuration->ClustersConfigurationSettings.FindPtr("other_cluster");
        UNIT_ASSERT(clusterSettings);
        UNIT_ASSERT_VALUES_EQUAL(static_cast<int>(clusterSettings->ClusterType), static_cast<int>(NYql::TPqClusterConfig::CT_DATA_STREAMS));
    }
}

} // namespace NYql
