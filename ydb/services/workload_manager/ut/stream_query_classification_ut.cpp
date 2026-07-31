#include <ydb/services/workload_manager/ut/common/workload_service_ut_common.h>

#include <ydb/core/kqp/counters/kqp_counters.h>
#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/core/tx/scheme_cache/scheme_cache.h>

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/query/client.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/scheme/scheme.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/topic/client.h>

#include <library/cpp/testing/unittest/registar.h>

#include <fmt/format.h>

#include <chrono>
#include <thread>

namespace NKikimr::NWorkloadManager {

using namespace NWorkloadManager;
using namespace NYdb;


namespace {

TIntrusivePtr<IYdbSetup> MakeStreamingYdb() {
    return TYdbSetupSettings()
        .EnableHasPredicatesInResourcePoolClassifiers(true)
        .Create([](auto) {});
}

void CreateTopic(TIntrusivePtr<IYdbSetup> ydb, TString name) {
    const auto& result = ydb->ExecuteQuery(TStringBuilder() << "CREATE TOPIC " << name);
    UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToOneLineString());
}

NYdb::TDriver MakeTopicDriver(TIntrusivePtr<IYdbSetup> ydb) {
    return NYdb::TDriver(NYdb::TDriverConfig()
        .SetEndpoint(TStringBuilder() << "localhost:" << ydb->GetGrpcPort())
        .SetDatabase(TStringBuilder() << "/" << ydb->GetSettings().DomainName_));
}

void WriteTopicMessages(TIntrusivePtr<IYdbSetup> ydb, const TString& topic, const std::vector<TString>& messages) {
    auto driver = MakeTopicDriver(ydb);
    NYdb::NTopic::TTopicClient topicClient(driver);
    auto writeSession = topicClient.CreateSimpleBlockingWriteSession(
        NYdb::NTopic::TWriteSessionSettings()
            .Path(topic)
            .PartitionId(0));
    for (const auto& message : messages) {
        UNIT_ASSERT(writeSession->Write(NYdb::NTopic::TWriteMessage(message)));
    }
    UNIT_ASSERT(writeSession->Close(TDuration::Seconds(5)));
    // Non-blocking stop: sync Stop(true) can hang forever in kikimr UT runtimes.
    driver.Stop(false);
}

bool TryReadTopicMessages(
    TIntrusivePtr<IYdbSetup> ydb,
    const TString& topic,
    ui64 expectedCount,
    TDuration timeout)
{
    auto driver = MakeTopicDriver(ydb);
    NYdb::NTopic::TTopicClient topicClient(driver);
    NYdb::NTopic::TReadSessionSettings readSettings;
    readSettings
        .WithoutConsumer()
        .Decompress(true)
        .AppendTopics(
            NYdb::NTopic::TTopicReadSettings(topic)
                .AppendPartitionIds(0));

    auto readSession = topicClient.CreateReadSession(readSettings);
    ui64 received = 0;
    const TInstant deadline = TInstant::Now() + timeout;
    while (TInstant::Now() < deadline && received < expectedCount) {
        auto future = readSession->WaitEvent();
        if (!future.Wait(deadline)) {
            break;
        }
        auto event = readSession->GetEvent(/*block=*/false);
        if (!event) {
            continue;
        }
        if (auto* data = std::get_if<NYdb::NTopic::TReadSessionEvent::TDataReceivedEvent>(&*event)) {
            received += data->GetMessages().size();
        } else if (auto* start = std::get_if<NYdb::NTopic::TReadSessionEvent::TStartPartitionSessionEvent>(&*event)) {
            start->Confirm(/*readFromCommittedOffset=*/0);
        } else if (auto* stop = std::get_if<NYdb::NTopic::TReadSessionEvent::TStopPartitionSessionEvent>(&*event)) {
            stop->Confirm();
        }
    }
    readSession->Close(TDuration::Seconds(1));
    driver.Stop(false);
    return received >= expectedCount;
}

struct TSchedulerPoolCpuSnapshot {
    i64 Usage = 0;
    i64 Limit = 0;
    i64 Queries = 0;
    i64 IdleTimeUs = 0;
    i64 ThrottleEvents = 0;
    i64 UpdateFairShare = 0;
};

TSchedulerPoolCpuSnapshot ReadSchedulerPoolCpu(TIntrusivePtr<IYdbSetup> ydb, const TString& poolId) {
    NKqp::TKqpCounters counters(ydb->GetRuntime()->GetAppData().Counters);
    auto kqp = counters.GetKqpCounters();
    auto poolGroup = kqp->GetSubgroup("schedulerPool", poolId);
    return TSchedulerPoolCpuSnapshot{
        .Usage = poolGroup->GetCounter("Usage", true)->Val(),
        .Limit = poolGroup->GetCounter("Limit", false)->Val(),
        .Queries = poolGroup->GetCounter("Queries", false)->Val(),
        .IdleTimeUs = poolGroup->GetCounter("IdleTimeUs", true)->Val(),
        .ThrottleEvents = poolGroup->GetCounter("ThrottleEvents", true)->Val(),
        .UpdateFairShare = kqp->GetCounter("scheduler/UpdateFairShare", true)->Val(),
    };
}

void CreateAndWaitStreamingQuery(TIntrusivePtr<IYdbSetup> ydb, const TString& createSql, const TString& poolId) {
    using namespace std::chrono_literals;
    const auto& result = ydb->ExecuteQuery(createSql);
    UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToOneLineString());
    ydb->WaitPoolState({.DelayedRequests = 0, .RunningRequests = 1}, poolId);
    std::this_thread::sleep_for(5s);
    ydb->WaitPoolState({.DelayedRequests = 0, .RunningRequests = 1}, poolId);
}

}  // anonymous namespace


Y_UNIT_TEST_SUITE(StreamingQueryClassification) {
    using namespace std::chrono_literals;

    void CreateStreamingPoolAndClassifier(TIntrusivePtr<IYdbSetup> ydb, const TString& poolId, const TString& classifierSql) {
        const auto& result = ydb->ExecuteQuery(TStringBuilder() << R"(
            CREATE RESOURCE POOL )" << poolId << R"( WITH (
                CONCURRENT_QUERY_LIMIT = 10,
                QUEUE_SIZE = 100,
                TOTAL_CPU_LIMIT_PERCENT_PER_NODE = 10,
                QUERY_CPU_LIMIT_PERCENT_PER_NODE = 1
            );
            )" << classifierSql);
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToOneLineString());
        ydb->WaitForClassifierPropagation();
    }

    Y_UNIT_TEST(TestStreamingQueryClassificationByPath) {
        auto ydb = MakeStreamingYdb();

        const TString& poolId = "streaming_pool";
        CreateTopic(ydb, "input_topic");
        CreateTopic(ydb, "output_topic");

        CreateStreamingPoolAndClassifier(ydb, poolId, TStringBuilder() << R"(
            CREATE RESOURCE POOL CLASSIFIER streaming_classifier WITH (
                RESOURCE_POOL=")" << poolId << R"(",
                HAS_PATH = "*input_topic*"
            );
        )");

        CreateAndWaitStreamingQuery(ydb, R"(
            CREATE STREAMING QUERY MyStreamingQuery
            AS DO BEGIN
                INSERT INTO output_topic SELECT * FROM input_topic;
            END DO
        )", poolId);
    }

    Y_UNIT_TEST(TestClassifierMatchesStreamingQuery) {
        auto ydb = MakeStreamingYdb();

        const TString& poolId = "streaming_pool";
        CreateTopic(ydb, "input_topic");
        CreateTopic(ydb, "output_topic");

        CreateStreamingPoolAndClassifier(ydb, poolId, TStringBuilder() << R"(
            CREATE RESOURCE POOL CLASSIFIER streaming_classifier WITH (
                RESOURCE_POOL=")" << poolId << R"(",
                HAS_STREAM = "true"
            );
        )");

        CreateAndWaitStreamingQuery(ydb, R"(
            CREATE STREAMING QUERY MyStreamingQuery
            AS DO BEGIN
                INSERT INTO output_topic SELECT * FROM input_topic;
            END DO
        )", poolId);
    }

    Y_UNIT_TEST(TestStreamingQueryUsesExplicitResourcePool) {
        auto ydb = MakeStreamingYdb();

        const TString& classifierPoolId = "classifier_pool";
        const TString& explicitPoolId = "explicit_pool";
        CreateTopic(ydb, "input_topic");
        CreateTopic(ydb, "output_topic");

        {
            const auto& result = ydb->ExecuteQuery(TStringBuilder() << R"(
                CREATE RESOURCE POOL )" << classifierPoolId << R"( WITH (
                    CONCURRENT_QUERY_LIMIT = 10,
                    QUEUE_SIZE = 100,
                    TOTAL_CPU_LIMIT_PERCENT_PER_NODE = 10,
                    QUERY_CPU_LIMIT_PERCENT_PER_NODE = 1
                );
                CREATE RESOURCE POOL )" << explicitPoolId << R"( WITH (
                    CONCURRENT_QUERY_LIMIT = 10,
                    QUEUE_SIZE = 100,
                    TOTAL_CPU_LIMIT_PERCENT_PER_NODE = 10,
                    QUERY_CPU_LIMIT_PERCENT_PER_NODE = 1
                );
                CREATE RESOURCE POOL CLASSIFIER streaming_classifier WITH (
                    RESOURCE_POOL=")" << classifierPoolId << R"(",
                    HAS_STREAM = "true"
                );
            )");
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToOneLineString());
        }
        ydb->WaitForClassifierPropagation();

        CreateAndWaitStreamingQuery(ydb, TStringBuilder() << R"(
            CREATE STREAMING QUERY MyStreamingQuery WITH (
                RESOURCE_POOL = ")" << explicitPoolId << R"("
            ) AS DO BEGIN
                INSERT INTO output_topic SELECT * FROM input_topic;
            END DO
        )", explicitPoolId);

        ydb->WaitPoolState({.DelayedRequests = 0, .RunningRequests = 0}, classifierPoolId);
    }
}

Y_UNIT_TEST_SUITE(StreamingTopicCpuLimit) {
    /*
        Scenario:
        - Resource pool with TOTAL_CPU_LIMIT_PERCENT_PER_NODE = 10.
        - CREATE STREAMING QUERY that reads from a topic under that pool.
        - Verify pool CPU cap is configured, topic messages are processed,
          and scheduler publishes Limit / Usage for the pool (CPU accounting).
     */
    Y_UNIT_TEST(TopicReadSqlCpuConsumptionUnderTenPercentPool) {
        auto ydb = TYdbSetupSettings()
            .EnableResourcePools(true)
            .EnableResourcePoolsScheduler(true)
            .EnableStreamingQueries(true)
            .EnableHasPredicatesInResourcePoolClassifiers(true)
            .Create([](auto) {});

        const TString& poolId = "topic_cpu_10pct";
        CreateTopic(ydb, "cpu10_input");
        CreateTopic(ydb, "cpu10_output");

        {
            const auto& result = ydb->ExecuteQuery(TStringBuilder() << R"(
                CREATE RESOURCE POOL )" << poolId << R"( WITH (
                    CONCURRENT_QUERY_LIMIT = 10,
                    QUEUE_SIZE = 100,
                    TOTAL_CPU_LIMIT_PERCENT_PER_NODE = 10,
                    QUERY_CPU_LIMIT_PERCENT_PER_NODE = 10
                );
            )");
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToOneLineString());
        }

        {
            auto desc = ydb->Navigate(TStringBuilder() << ".metadata/workload_manager/pools/" << poolId);
            UNIT_ASSERT(desc->ResultSet.at(0).ResourcePoolInfo);
            const auto& properties = desc->ResultSet.at(0).ResourcePoolInfo->Description.GetProperties().GetProperties();
            UNIT_ASSERT_VALUES_EQUAL_C(
                properties.at("total_cpu_limit_percent_per_node"), "10",
                "Pool must be configured with 10% CPU limit");
        }

        CreateAndWaitStreamingQuery(ydb, TStringBuilder() << R"(
            CREATE STREAMING QUERY TopicCpuTenPercentQuery WITH (
                RESOURCE_POOL = ")" << poolId << R"("
            ) AS DO BEGIN
                INSERT INTO cpu10_output
                SELECT * FROM cpu10_input;
            END DO
        )", poolId);

        const auto cpuBefore = ReadSchedulerPoolCpu(ydb, poolId);

        constexpr ui64 messageCount = 20;
        std::vector<TString> messages;
        messages.reserve(messageCount);
        for (ui64 i = 0; i < messageCount; ++i) {
            messages.push_back(TStringBuilder() << "cpu10-msg-" << i);
        }
        WriteTopicMessages(ydb, "cpu10_input", messages);

        UNIT_ASSERT_C(
            TryReadTopicMessages(ydb, "cpu10_output", messageCount, TDuration::Seconds(30)),
            "Streaming query under 10% CPU pool must process topic messages");

        // Limit / Usage are published by FairShare snapshots. After real topic work
        // the pool must expose Limit for the 10% cap and some accounted CPU activity.
        IYdbSetup::WaitFor(TDuration::Seconds(30), "scheduler pool CPU accounting", [&](TString& error) {
            const auto snap = ReadSchedulerPoolCpu(ydb, poolId);
            error = TStringBuilder()
                << "usage=" << snap.Usage
                << " (before=" << cpuBefore.Usage << ")"
                << ", limit=" << snap.Limit
                << ", queries=" << snap.Queries
                << ", idleTimeUs=" << snap.IdleTimeUs
                << ", throttleEvents=" << snap.ThrottleEvents
                << ", updateFairShare=" << snap.UpdateFairShare;
            // CpuLimit for 10% is at least 1 CPU => Limit counter is CpuLimit * 1e6.
            const bool limitApplied = snap.Limit >= 1'000'000;
            const bool usageObserved = snap.Usage > cpuBefore.Usage
                || snap.IdleTimeUs > cpuBefore.IdleTimeUs
                || snap.ThrottleEvents > cpuBefore.ThrottleEvents;
            return limitApplied && usageObserved;
        });

        const auto cpuAfter = ReadSchedulerPoolCpu(ydb, poolId);
        UNIT_ASSERT_GE_C(cpuAfter.Limit, 1'000'000, "10% pool must publish Limit >= 1 CPU * 1e6");
        ydb->WaitPoolState({.DelayedRequests = 0, .RunningRequests = 1}, poolId);
    }
}

namespace {

using namespace fmt::literals;

void CheckObjectProperties(TTestActorRuntime& runtime, const TString& path, const std::unordered_map<TString, TString>& expectedProperties) {
    auto streamingQueryDesc = NKqp::Navigate(runtime, runtime.AllocateEdgeActor(), path, NSchemeCache::TSchemeCacheNavigate::EOp::OpUnknown);
    const auto& streamingQuery = streamingQueryDesc->ResultSet.at(0);
    UNIT_ASSERT_VALUES_EQUAL(streamingQuery.Kind, NSchemeCache::TSchemeCacheNavigate::EKind::KindStreamingQuery);
    UNIT_ASSERT(streamingQuery.StreamingQueryInfo);
    UNIT_ASSERT_VALUES_EQUAL(streamingQuery.StreamingQueryInfo->Description.GetName(), SplitPath(path).back());
    const auto& properties = streamingQuery.StreamingQueryInfo->Description.GetProperties().GetProperties();
    UNIT_ASSERT_GE(properties.size(), expectedProperties.size());
    for (const auto& [key, value] : expectedProperties) {
        UNIT_ASSERT_C(properties.contains(key), key);
        UNIT_ASSERT_VALUES_EQUAL(properties.at(key), value);
    }
}

std::unique_ptr<NKqp::TKikimrRunner> SetupStreamingSource(bool enableStreamingQueries = true) {
    NKikimrConfig::TAppConfig config;
    auto& featureFlags = *config.MutableFeatureFlags();
    featureFlags.SetEnableStreamingQueries(enableStreamingQueries);
    featureFlags.SetEnableExternalDataSources(true);
    featureFlags.SetEnableResourcePools(true);
    featureFlags.SetEnableStreamingQueryDisposition(true);
    config.MutableTableServiceConfig()->SetDqChannelVersion(1u);

    auto kikimr = std::make_unique<NKqp::TKikimrRunner>(NKqp::TKikimrSettings(config)
        .SetEnableStreamingQueries(enableStreamingQueries)
        .SetEnableExternalDataSources(true)
        .SetEnableResourcePools(true)
        .SetInitFederatedQuerySetupFactory(true));

    const auto result = kikimr->GetQueryClient().ExecuteQuery(fmt::format(R"(
        CREATE TOPIC MyTopic;
        CREATE EXTERNAL DATA SOURCE MySource WITH (
            SOURCE_TYPE = "Ydb",
            LOCATION = "localhost:{port}",
            DATABASE_NAME = "/Root",
            AUTH_METHOD = "NONE"
        );)",
        "port"_a = kikimr->GetTestServer().GetGRpcServer().GetPort()),
        NQuery::TTxControl::NoTx()).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToOneLineString());

    return kikimr;
}

}  // anonymous namespace

Y_UNIT_TEST_SUITE(WorkloadManagerScheme) {
    Y_UNIT_TEST(StreamingQueriesWithResourcePools) {
        auto kikimr = SetupStreamingSource();
        auto& runtime = *kikimr->GetTestServer().GetRuntime();
        auto db = kikimr->GetQueryClient();

        {
            const auto result = kikimr->GetQueryClient().ExecuteQuery(R"(
                CREATE RESOURCE POOL my_pool WITH (
                    CONCURRENT_QUERY_LIMIT = 0
                ))",
                NQuery::TTxControl::NoTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToOneLineString());
        }

        {
            const auto result = db.ExecuteQuery(R"(
                CREATE STREAMING QUERY `MyFolder/MyStreamingQuery` WITH (
                    RUN = TRUE,
                    RESOURCE_POOL = "my_pool"
                ) AS DO BEGIN INSERT INTO MySource.MyTopic SELECT * FROM MySource.MyTopic END DO)",
                NQuery::TTxControl::NoTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::PRECONDITION_FAILED, result.GetIssues().ToOneLineString());
            UNIT_ASSERT_STRING_CONTAINS(result.GetIssues().ToString(), "Resource pool my_pool was disabled due to zero concurrent query limit");

            CheckObjectProperties(runtime, "/Root/MyFolder/MyStreamingQuery", {});
        }

        {
            const auto result = db.ExecuteQuery(R"(
                CREATE STREAMING QUERY `MyFolder/OtherQuery` WITH (
                    RUN = FALSE
                ) AS DO BEGIN INSERT INTO MySource.MyTopic SELECT * FROM MySource.MyTopic END DO)",
                NQuery::TTxControl::NoTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToOneLineString());

            CheckObjectProperties(runtime, "/Root/MyFolder/OtherQuery", {});
        }

        {
            const auto result = db.ExecuteQuery(R"(
                ALTER STREAMING QUERY `MyFolder/OtherQuery` SET (
                    RUN = TRUE,
                    RESOURCE_POOL = "my_pool"
                );)",
                NQuery::TTxControl::NoTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::PRECONDITION_FAILED, result.GetIssues().ToOneLineString());
            UNIT_ASSERT_STRING_CONTAINS(result.GetIssues().ToString(), "Resource pool my_pool was disabled due to zero concurrent query limit");
        }
    }
}

}  // namespace NKikimr::NWorkloadManager
