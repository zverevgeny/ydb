#include <fmt/format.h>
#include <ydb/core/kqp/ut/common/kqp_ut_common.h>

namespace NKikimr {
namespace NKqp {

using namespace NYdb;
using namespace NYdb::NQuery;


namespace {

/*
 * With EnableCsWriteAffinity=true, the pure-expr REPLACE INTO is split into:
 *   Sink Stage (olap, N tasks, one per shard)
 *     Broadcast  ← HashShuffle connection, routes rows to correct shard tasks
 *       Transform Stage (compute, 1 task, generates rows)
 *
 * With EnableCsWriteAffinity=false, the sink is inlined into the transform stage
 * (no separate Sink stage, no Broadcast).
 */
void VerifyPlanWithAffinity(const NJson::TJsonValue& plan, TString planStr, bool enableCsWriteAffinity) {
    const ui32 expectedStages = enableCsWriteAffinity ? 3 : 2;
    const auto stages = FindPlanStages(plan);
    UNIT_ASSERT_VALUES_EQUAL_C(stages.size(), expectedStages,
        "Expected " << expectedStages << " stages (EnableCsWriteAffinity="
        << enableCsWriteAffinity << "), got " << stages.size()
        << ". Plan: " << planStr);

    if (enableCsWriteAffinity) {
        // 1. A Broadcast connection exists (Transform→Sink link).
        const auto broadcastNode = FindPlanNodeByKv(plan, "Node Type", "Broadcast");
        UNIT_ASSERT_C(broadcastNode.IsDefined(),
            "Expected a 'Broadcast' connection in plan with EnableCsWriteAffinity=true. "
            "Plan: " << planStr);

        // 2. A Sink stage node exists.
        const auto sinkNode = FindPlanNodeByKv(plan, "Node Type", "Sink");
        UNIT_ASSERT_C(sinkNode.IsDefined(),
            "Expected a 'Sink' stage in plan with EnableCsWriteAffinity=true. "
            "Plan: " << planStr);

        // 3. Broadcast has PlanNodeType=Connection.
        const auto& broadcastMap = broadcastNode.GetMapSafe();
        const auto planNodeTypeIt = broadcastMap.find("PlanNodeType");
        UNIT_ASSERT_C(planNodeTypeIt != broadcastMap.end()
                && planNodeTypeIt->second.GetStringSafe() == "Connection",
            "Expected 'Broadcast' to have PlanNodeType=Connection. "
            "Plan: " << planStr);

        // 4. Exactly 1 Broadcast.
        const ui32 broadcastCount = CountPlanNodesByKv(plan, "Node Type", "Broadcast");
        UNIT_ASSERT_VALUES_EQUAL_C(broadcastCount, 1,
            "Expected exactly 1 Broadcast connection. "
            "Plan: " << planStr);

        // 5. Inner compute stage exists (Node Type = "Stage").
        const auto innerStageNode = FindPlanNodeByKv(plan, "Node Type", "Stage");
        UNIT_ASSERT_C(innerStageNode.IsDefined(),
            "Expected an inner 'Stage' (compute) in plan with EnableCsWriteAffinity=true. "
            "Plan: " << planStr);
    } else {
        // Without affinity, the sink is inlined — no separate Sink stage, no Broadcast.
        const auto broadcastNode = FindPlanNodeByKv(plan, "Node Type", "Broadcast");
        UNIT_ASSERT_C(!broadcastNode.IsDefined(),
            "Expected NO 'Broadcast' with EnableCsWriteAffinity=false. "
            "Plan: " << planStr);
    }
}

void RunReplaceTest(bool enableCsWriteAffinity) {
    NKikimrConfig::TFeatureFlags featureFlags;
    featureFlags.SetEnableMoveColumnTable(true);
    auto settings = TKikimrSettings().SetFeatureFlags(featureFlags).SetWithSampleTables(false);
    TKikimrRunner kikimr(settings);

    auto client = kikimr.GetQueryClient();

    {
        auto result = client.ExecuteQuery(R"(
            CREATE TABLE `/Root/Source` (
                Col1 Uint64 NOT NULL,
                Col2 Int32,
                PRIMARY KEY (Col1)
            )
            PARTITION BY HASH(Col1)
            WITH (STORE = COLUMN, AUTO_PARTITIONING_MIN_PARTITIONS_COUNT = 8);
        )", NYdb::NQuery::TTxControl::NoTx()).ExtractValueSync();
        UNIT_ASSERT_C(result.GetStatus() == NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
    }

    const TString pragmaPrefix = enableCsWriteAffinity
        ? "PRAGMA ydb.EnableCsWriteAffinity = \"true\";\n"
        : "PRAGMA ydb.EnableCsWriteAffinity = \"false\";\n";

    const int insertedRowsCount = 80;

    const TString query = TStringBuilder()
        << pragmaPrefix
        << "$data = ListMap(ListFromRange(0, " << insertedRowsCount << "), ($x) -> { "
        << "RETURN AsStruct($x AS Col1, $x AS Col2); });"
        << "REPLACE INTO `/Root/Source` "
        << "SELECT Unwrap(CAST(Col1 AS Uint64)) AS Col1, Unwrap(CAST(Col2 AS Int32)) AS Col2 "
        << "FROM AS_TABLE($data);";

    {
        auto result = client.ExecuteQuery(
            query,
            NYdb::NQuery::TTxControl::NoTx(),
            NYdb::NQuery::TExecuteQuerySettings().ExecMode(NQuery::EExecMode::Explain)
        ).ExtractValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());

        UNIT_ASSERT_C(result.GetStats().has_value(), "Expected query stats to be present");
        const auto planStr = result.GetStats()->GetPlan();
        UNIT_ASSERT_C(planStr.has_value(), "Expected query plan to be present");

        NJson::TJsonValue plan;
        UNIT_ASSERT_C(NJson::ReadJsonTree(TString(*planStr), &plan, true),
            "Failed to parse query plan: " << *planStr);

        VerifyPlanWithAffinity(plan, TString(*planStr), enableCsWriteAffinity);
    }

    {
        auto result = client.ExecuteQuery(query,
            NYdb::NQuery::TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
    }

    // Verify data was written correctly
    {
        auto it = client.StreamExecuteQuery(R"(
            SELECT Col1, Col2 FROM `/Root/Source` ORDER BY Col1 ASC;
        )", NYdb::NQuery::TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(it.GetStatus(), EStatus::SUCCESS, it.GetIssues().ToString());
        TString output = StreamResultToYson(it);

        // Build expected YSON dynamically from all inserted rows (0..79).
        // Each row is [Col1u;[Col2]] where Col1 = Col2 = i.
        TString expected = "[";
        for (int i = 0; i < insertedRowsCount; ++i) {
            if (i > 0) {
                expected += ";";
            }
            expected += TStringBuilder() << "[" << i << "u;[" << i << "]]";
        }
        expected += "]";
        CompareYson(output, expected);
    }
}

void RunInsertTest(bool enableCsWriteAffinity) {
    NKikimrConfig::TFeatureFlags featureFlags;
    featureFlags.SetEnableMoveColumnTable(true);
    auto settings = TKikimrSettings().SetFeatureFlags(featureFlags).SetWithSampleTables(false);
    TKikimrRunner kikimr(settings);

    auto client = kikimr.GetQueryClient();

    {
        auto result = client.ExecuteQuery(R"(
            CREATE TABLE `/Root/Source` (
                Col1 Uint64 NOT NULL,
                Col2 Int32,
                PRIMARY KEY (Col1)
            )
            PARTITION BY HASH(Col1)
            WITH (STORE = COLUMN, AUTO_PARTITIONING_MIN_PARTITIONS_COUNT = 8);
        )", NYdb::NQuery::TTxControl::NoTx()).ExtractValueSync();
        UNIT_ASSERT_C(result.GetStatus() == NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
    }

    const TString pragmaPrefix = enableCsWriteAffinity
        ? "PRAGMA ydb.EnableCsWriteAffinity = \"true\";\n"
        : "PRAGMA ydb.EnableCsWriteAffinity = \"false\";\n";

    const int insertedRowsCount = 80;

    {
        const TString query = TStringBuilder()
            << pragmaPrefix
            << "$data = ListMap(ListFromRange(0, " << insertedRowsCount << "), ($x) -> { "
            << "RETURN AsStruct($x AS Col1, $x AS Col2); });"
            << "INSERT INTO `/Root/Source` "
            << "SELECT Unwrap(CAST(Col1 AS Uint64)) AS Col1, Unwrap(CAST(Col2 AS Int32)) AS Col2 "
            << "FROM AS_TABLE($data);";
        auto result = client.ExecuteQuery(query,
            NYdb::NQuery::TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
    }

    // Verify data was written correctly
    {
        auto it = client.StreamExecuteQuery(R"(
            SELECT Col1, Col2 FROM `/Root/Source` ORDER BY Col1 ASC;
        )", NYdb::NQuery::TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(it.GetStatus(), EStatus::SUCCESS, it.GetIssues().ToString());
        TString output = StreamResultToYson(it);

        // Build expected YSON dynamically from all inserted rows (0..79).
        TString expected = "[";
        for (int i = 0; i < insertedRowsCount; ++i) {
            if (i > 0) {
                expected += ";";
            }
            expected += TStringBuilder() << "[" << i << "u;[" << i << "]]";
        }
        expected += "]";
        CompareYson(output, expected);
    }
}

void RunUpdateTest(bool enableCsWriteAffinity) {
    NKikimrConfig::TFeatureFlags featureFlags;
    featureFlags.SetEnableMoveColumnTable(true);
    auto settings = TKikimrSettings().SetFeatureFlags(featureFlags).SetWithSampleTables(false);
    TKikimrRunner kikimr(settings);

    auto client = kikimr.GetQueryClient();

    {
        auto result = client.ExecuteQuery(R"(
            CREATE TABLE `/Root/Source` (
                Col1 Uint64 NOT NULL,
                Col2 Int32,
                PRIMARY KEY (Col1)
            )
            PARTITION BY HASH(Col1)
            WITH (STORE = COLUMN, AUTO_PARTITIONING_MIN_PARTITIONS_COUNT = 8);
        )", NYdb::NQuery::TTxControl::NoTx()).ExtractValueSync();
        UNIT_ASSERT_C(result.GetStatus() == NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
    }

    // Insert initial data
    {
        auto result = client.ExecuteQuery(R"(
            REPLACE INTO `/Root/Source` (Col1, Col2)
            VALUES (1u, 10), (2u, 20), (3u, 30);
        )", NYdb::NQuery::TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
    }

    const TString pragmaPrefix = enableCsWriteAffinity
        ? "PRAGMA ydb.EnableCsWriteAffinity = \"true\";\n"
        : "PRAGMA ydb.EnableCsWriteAffinity = \"false\";\n";

    // UPDATE: set Col2 = CAST(Col1 * 2 AS Int32) for all rows
    const TString query = TStringBuilder()
        << pragmaPrefix
        << "UPDATE `/Root/Source` SET Col2 = CAST(Col1 * 2 AS Int32);";

    // NOTE: UPDATE has a source (TableFullScan) and uses Map connection, not Broadcast.
    // The plan structure differs from pure OLAP stages, so we don't call VerifyPlanWithAffinity.
    // TODO: Add proper plan verification for UPDATE when CS Write Affinity supports it.

    {
        auto result = client.ExecuteQuery(query,
            NYdb::NQuery::TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
    }

    // Verify data was updated correctly
    {
        auto it = client.StreamExecuteQuery(R"(
            SELECT Col1, Col2 FROM `/Root/Source` ORDER BY Col1 ASC;
        )", NYdb::NQuery::TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(it.GetStatus(), EStatus::SUCCESS, it.GetIssues().ToString());
        TString output = StreamResultToYson(it);
        // Expected: (1, 2), (2, 4), (3, 6)
        CompareYson(output, "[[1u;[2]];[2u;[4]];[3u;[6]]]");
    }
}

void RunDeleteTest(bool enableCsWriteAffinity) {
    NKikimrConfig::TFeatureFlags featureFlags;
    featureFlags.SetEnableMoveColumnTable(true);
    auto settings = TKikimrSettings().SetFeatureFlags(featureFlags).SetWithSampleTables(false);
    TKikimrRunner kikimr(settings);

    auto client = kikimr.GetQueryClient();

    {
        auto result = client.ExecuteQuery(R"(
            CREATE TABLE `/Root/Source` (
                Col1 Uint64 NOT NULL,
                Col2 Int32,
                PRIMARY KEY (Col1)
            )
            PARTITION BY HASH(Col1)
            WITH (STORE = COLUMN, AUTO_PARTITIONING_MIN_PARTITIONS_COUNT = 8);
        )", NYdb::NQuery::TTxControl::NoTx()).ExtractValueSync();
        UNIT_ASSERT_C(result.GetStatus() == NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
    }

    // Insert initial data
    {
        auto result = client.ExecuteQuery(R"(
            REPLACE INTO `/Root/Source` (Col1, Col2)
            VALUES (1u, 10), (2u, 20), (3u, 30);
        )", NYdb::NQuery::TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
    }

    const TString pragmaPrefix = enableCsWriteAffinity
        ? "PRAGMA ydb.EnableCsWriteAffinity = \"true\";\n"
        : "PRAGMA ydb.EnableCsWriteAffinity = \"false\";\n";

    // DELETE: remove rows where Col1 > 1
    const TString query = TStringBuilder()
        << pragmaPrefix
        << "DELETE FROM `/Root/Source` WHERE Col1 > 1u;";

    // NOTE: DELETE has a source (TableFullScan) and uses Map connection, not Broadcast.
    // The plan structure differs from pure OLAP stages, so we don't call VerifyPlanWithAffinity.
    // TODO: Add proper plan verification for DELETE when CS Write Affinity supports it.

    {
        auto result = client.ExecuteQuery(query,
            NYdb::NQuery::TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
    }

    // Verify only row with Col1=1 remains
    {
        auto it = client.StreamExecuteQuery(R"(
            SELECT Col1, Col2 FROM `/Root/Source` ORDER BY Col1 ASC;
        )", NYdb::NQuery::TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(it.GetStatus(), EStatus::SUCCESS, it.GetIssues().ToString());
        TString output = StreamResultToYson(it);
        CompareYson(output, "[[1u;[10]]]");
    }
}

void RunCtasTest(bool enableCsWriteAffinity) {
    // Verify CTAS produces identical results with EnableCsWriteAffinity=true/false
    // and checks that the query plan has different number of stages:
    // - Without pragma: single stage (transform + sink together)
    // - With pragma: two stages (transform stage + separate sink stage)
    NKikimrConfig::TFeatureFlags featureFlags;
    featureFlags.SetEnableMoveColumnTable(true);
    auto settings = TKikimrSettings().SetFeatureFlags(featureFlags).SetWithSampleTables(false);
    settings.AppConfig.MutableTableServiceConfig()->SetEnableOlapSink(true);
    settings.AppConfig.MutableTableServiceConfig()->SetEnableCreateTableAs(true);
    settings.AppConfig.MutableTableServiceConfig()->SetEnablePerStatementQueryExecution(true);
    TKikimrRunner kikimr(settings);

    auto client = kikimr.GetQueryClient();

    {
        auto result = client.ExecuteQuery(R"(
            CREATE TABLE `/Root/Source` (
                Col1 Uint64 NOT NULL,
                Col2 Int32,
                PRIMARY KEY (Col1)
            )
            PARTITION BY HASH(Col1)
            WITH (STORE = COLUMN, AUTO_PARTITIONING_MIN_PARTITIONS_COUNT = 8);
        )", NYdb::NQuery::TTxControl::NoTx()).ExtractValueSync();
        UNIT_ASSERT_C(result.GetStatus() == NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
    }

    // Build the CTAS query with the pragma set explicitly in both branches.
    // NOTE: the feature is enabled by default, so the "off" branch must set
    // the pragma to "false" explicitly rather than relying on the default.
    const TString pragmaPrefix = enableCsWriteAffinity
        ? "PRAGMA ydb.EnableCsWriteAffinity = \"true\";\n"
        : "PRAGMA ydb.EnableCsWriteAffinity = \"false\";\n";

    const int insertedRowsCount = 80;

    {
        const TString insertQuery = TStringBuilder()
            << pragmaPrefix
            << "$data = ListMap(ListFromRange(0, " << insertedRowsCount << "), ($x) -> { "
            << "RETURN AsStruct($x AS Col1, $x AS Col2); });"
            << "REPLACE INTO `/Root/Source` "
            << "SELECT Unwrap(CAST(Col1 AS Uint64)) AS Col1, Unwrap(CAST(Col2 AS Int32)) AS Col2 "
            << "FROM AS_TABLE($data);";
        auto result = client.ExecuteQuery(insertQuery
            , NYdb::NQuery::TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
    }

    const TString ctasQuery = TStringBuilder()
        << pragmaPrefix
        << R"(
            CREATE TABLE `/Root/Destination` (
                PRIMARY KEY (Col1)
            )
            PARTITION BY HASH(Col1)
            WITH (STORE = COLUMN, AUTO_PARTITIONING_MIN_PARTITIONS_COUNT = 2)
            AS SELECT * FROM `/Root/Source`;
        )";

    // Explain the CTAS query and inspect the physical plan. With the pragma
    // enabled the WriteActor (sink) lives in its own TDqStage (connected via Broadcast),
    // so the plan contains exactly one more stage than without the pragma.
    // Measured: 3 stages without the pragma, 4 stages with the pragma.
    {
        auto result = client.ExecuteQuery(
            ctasQuery,
            NYdb::NQuery::TTxControl::NoTx(),
            NYdb::NQuery::TExecuteQuerySettings().ExecMode(NQuery::EExecMode::Explain)
        ).ExtractValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());

        UNIT_ASSERT_C(result.GetStats().has_value(), "Expected query stats to be present");
        const auto planStr = result.GetStats()->GetPlan();
        UNIT_ASSERT_C(planStr.has_value(), "Expected query plan to be present");

        NJson::TJsonValue plan;
        UNIT_ASSERT_C(NJson::ReadJsonTree(TString(*planStr), &plan, true),
            "Failed to parse query plan: " << *planStr);

        const ui32 expectedStages = enableCsWriteAffinity ? 4 : 3;
        const auto stages = FindPlanStages(plan);
        UNIT_ASSERT_VALUES_EQUAL_C(stages.size(), expectedStages,
            "Expected " << expectedStages << " stages (EnableCsWriteAffinity="
            << enableCsWriteAffinity << "), got " << stages.size()
            << ". Plan: " << *planStr);

        if (enableCsWriteAffinity) {
            // With affinity, the sink stage must be connected via Broadcast (not Map),
            // so it can be independently placed (N tasks, one per target shard).

            // 1. Broadcast connection exists somewhere in the plan.
            const auto broadcastNode = FindPlanNodeByKv(plan, "Node Type", "Broadcast");
            UNIT_ASSERT_C(broadcastNode.IsDefined(),
                "Expected a 'Broadcast' connection node in plan with EnableCsWriteAffinity=true."
                " Plan: " << *planStr);

            // 2. A Sink node exists.
            const auto sinkNode = FindPlanNodeByKv(plan, "Node Type", "Sink");
            UNIT_ASSERT_C(sinkNode.IsDefined(),
                "Expected a 'Sink' stage node in plan with EnableCsWriteAffinity=true."
                " Plan: " << *planStr);

            // 3. The Broadcast node must have PlanNodeType=Connection.
            const auto& broadcastMap = broadcastNode.GetMapSafe();
            const auto planNodeTypeIt = broadcastMap.find("PlanNodeType");
            UNIT_ASSERT_C(planNodeTypeIt != broadcastMap.end()
                    && planNodeTypeIt->second.GetStringSafe() == "Connection",
                "Expected 'Broadcast' node to have PlanNodeType=Connection."
                " Plan: " << *planStr);

            // 4. Verify there is exactly 1 Broadcast connection (the Transform→Sink link).
            const ui32 broadcastCount = CountPlanNodesByKv(plan, "Node Type", "Broadcast");
            UNIT_ASSERT_VALUES_EQUAL_C(broadcastCount, 1,
                "Expected exactly 1 Broadcast connection in plan with EnableCsWriteAffinity=true."
                " Plan: " << *planStr);
        } else {
            // Without affinity, the sink is inlined into the transform stage (no separate
            // sink stage). Therefore there must be NO Broadcast connection in the plan.
            const auto broadcastNode = FindPlanNodeByKv(plan, "Node Type", "Broadcast");
            UNIT_ASSERT_C(!broadcastNode.IsDefined(),
                "Expected NO 'Broadcast' connection in plan with EnableCsWriteAffinity=false"
                " (sink should be inlined into transform stage). Plan: " << *planStr);
        }
    }

    // Execute CTAS query
    {
        auto result = client.ExecuteQuery(ctasQuery, NYdb::NQuery::TTxControl::NoTx()).ExtractValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
    }

    // Verify data was written correctly — result must be identical regardless of pragma value
    {
        auto it = client.StreamExecuteQuery(R"(
            SELECT Col1, Col2 FROM `/Root/Destination` ORDER BY Col1 ASC;
        )", NYdb::NQuery::TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(it.GetStatus(), EStatus::SUCCESS, it.GetIssues().ToString());
        TString output = StreamResultToYson(it);
        // Build expected YSON dynamically from all inserted rows.
        TString expected = "[";
        for (int i = 0; i < insertedRowsCount; ++i) {
            if (i > 0) {
                expected += ";";
            }
            expected += TStringBuilder() << "[" << i << "u;[" << i << "]]";
        }
        expected += "]";
        CompareYson(output, expected);
    }
}

} // anonymous namespace

Y_UNIT_TEST_SUITE(CS_WriteAffinity) {

    Y_UNIT_TEST(Replace_EnableCsWriteAffinity_True) {
        RunReplaceTest(true);
    }

    Y_UNIT_TEST(Replace_EnableCsWriteAffinity_False) {
        RunReplaceTest(false);
    }

    Y_UNIT_TEST(Insert_EnableCsWriteAffinity_True) {
        RunInsertTest(true);
    }

    Y_UNIT_TEST(Insert_EnableCsWriteAffinity_False) {
        RunInsertTest(false);
    }

    Y_UNIT_TEST(Update_EnableCsWriteAffinity_True) {
        RunUpdateTest(true);
    }

    Y_UNIT_TEST(Update_EnableCsWriteAffinity_False) {
        RunUpdateTest(false);
    }

    Y_UNIT_TEST(Delete_EnableCsWriteAffinity_True) {
        RunDeleteTest(true);
    }

    Y_UNIT_TEST(Delete_EnableCsWriteAffinity_False) {
        RunDeleteTest(false);
    }

    Y_UNIT_TEST(Ctas_EnableCsWriteAffinity_True) {
        RunCtasTest(true);
    }

    Y_UNIT_TEST(Ctas_EnableCsWriteAffinity_False) {
        RunCtasTest(false);
    }

} // Y_UNIT_TEST_SUITE(CS_WriteAffinity)

} // namespace NKqp
} // namespace NKikimr
