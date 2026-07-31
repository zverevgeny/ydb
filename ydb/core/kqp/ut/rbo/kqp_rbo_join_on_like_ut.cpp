#include <ydb/core/kqp/ut/olap/helpers/get_value.h>
#include <ydb/core/kqp/ut/olap/helpers/query_executor.h>
#include <ydb/core/kqp/ut/olap/helpers/local.h>
#include <ydb/core/kqp/ut/olap/helpers/writer.h>
#include <ydb/core/kqp/ut/olap/helpers/aggregation.h>
#include <ydb/core/kqp/common/events/events.h>
#include <ydb/core/kqp/common/simple/services.h>
#include <ydb/core/kqp/executer_actor/kqp_executer.h>
#include <ydb/core/statistics/ut_common/ut_common.h>
#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/library/actors/testlib/test_runtime.h>
#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <yql/essentials/parser/pg_catalog/catalog.h>
#include <yql/essentials/parser/pg_wrapper/interface/codec.h>
#include <yql/essentials/utils/log/log.h>
#include <ydb/public/lib/ut_helpers/ut_helpers_query.h>
#include <ydb/public/lib/ydb_cli/common/format.h>
#include <util/system/env.h>
#include <ydb/public/lib/ydb_cli/common/format.h>
#include <ydb/public/lib/yson_value/ydb_yson_value.h>

#include <algorithm>
#include <ctime>
#include <regex>
#include <fstream>

using namespace NKikimr;
using namespace NKikimr::NKqp;
using namespace NYdb;
using namespace NYdb::NTable;
using namespace NYql::NNodes;
using namespace NStat;


namespace NKikimr::NKqp {

Y_UNIT_TEST_SUITE(JoinOnLike) {

    Y_UNIT_TEST(Simple) {
        // Default path: the `ydb.EnableLikeJoinHyperscan` pragma is NOT set, so
        // the LIKE join is executed as a standard cross join followed by a
        // per-row filter (the double loop), not the Hyperscan automaton.
        NKikimrConfig::TAppConfig appConfig;
        // {
        //     auto* entry = appConfig.MutableLogConfig()->AddEntry();
        //     entry->SetComponent(NKikimrServices::EServiceKikimr_Name(NKikimrServices::EServiceKikimr::KQP_COMPILE_ACTOR));
        //     entry->SetLevel(NActors::NLog::PRI_DEBUG);
        // }
        appConfig.MutableLogConfig()->SetDefaultLevel(NActors::NLog::PRI_DEBUG);

        appConfig.MutableTableServiceConfig()->SetEnableNewRBO(true);
        TKikimrRunner kikimr(NKqp::TKikimrSettings(appConfig).SetWithSampleTables(false));
        auto queryClient = kikimr.GetQueryClient();
        auto querySession = queryClient.GetSession().GetValueSync().GetSession();
        auto db = kikimr.GetTableClient();
        {
            querySession.ExecuteQuery(R"(
            CREATE TABLE `/Root/events` (
                id	Int64	NOT NULL,
                name	String,
                primary key(id)
            );

            CREATE TABLE `/Root/lookup` (
                mask String	NOT NULL,
                id	Int64,
                primary key(id)
            );
            )",
            NYdb::NQuery::TTxControl::NoTx(),
            {}
            ).GetValueSync();

            // Insert test data into events table.
            {
                NYdb::TValueBuilder rows;
                rows.BeginList();
                rows.AddListItem().BeginStruct().AddMember("id").Int64(1).AddMember("name").String("apple").EndStruct();
                rows.AddListItem().BeginStruct().AddMember("id").Int64(2).AddMember("name").String("banana").EndStruct();
                rows.AddListItem().BeginStruct().AddMember("id").Int64(3).AddMember("name").String("cherry").EndStruct();
                rows.AddListItem().BeginStruct().AddMember("id").Int64(4).AddMember("name").String("date").EndStruct();
                rows.EndList();
                auto resultUpsert = db.BulkUpsert("/Root/events", rows.Build()).GetValueSync();
                UNIT_ASSERT_C(resultUpsert.IsSuccess(), resultUpsert.GetIssues().ToString());
            }

            // Insert test data into lookup table with LIKE masks.
            {
                NYdb::TValueBuilder rows;
                rows.BeginList();
                rows.AddListItem().BeginStruct().AddMember("mask").String("%app%").AddMember("id").Int64(10).EndStruct();
                rows.AddListItem().BeginStruct().AddMember("mask").String("ban%").AddMember("id").Int64(20).EndStruct();
                rows.AddListItem().BeginStruct().AddMember("mask").String("%rry").AddMember("id").Int64(30).EndStruct();
                rows.EndList();
                auto resultUpsert = db.BulkUpsert("/Root/lookup", rows.Build()).GetValueSync();
                UNIT_ASSERT_C(resultUpsert.IsSuccess(), resultUpsert.GetIssues().ToString());
            }
        }

        // Execute the LEFT JOIN query with LIKE condition.
        // This test verifies that the RBO can handle LEFT JOIN with LIKE predicate
        // (a non-equi join that requires multi-consumer handling for row storage sources).
        //
        // Expected matches:
        //   apple  LIKE %app%  -> (e.id=1,  e.name="apple",  l.mask="%app%", l.id=10)
        //   banana LIKE ban%   -> (e.id=2,  e.name="banana", l.mask="ban%",  l.id=20)
        //   cherry LIKE %rry   -> (e.id=3,  e.name="cherry", l.mask="%rry",  l.id=30)
        //   date   (no match)  -> (e.id=4,  e.name="date",   l.mask=NULL,    l.id=NULL)
        const auto result = querySession.ExecuteQuery(R"(
                PRAGMA ydb.HashJoinMode = 'map';
                SELECT e.id, e.name, l.mask, l.id as mask_id
                FROM `/Root/events` as e
                LEFT JOIN `/Root/lookup` as l
                ON e.name LIKE l.mask
                ORDER BY e.id
            )",
            NYdb::NQuery::TTxControl::NoTx(),
            NYdb::NQuery::TExecuteQuerySettings().ExecMode(NQuery::EExecMode::Execute)
        ).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);

        // Verify the result set by comparing YSON representation.
        // Expected:
        //   apple  LIKE %app%  -> (1, "apple",  "%app%", 10)
        //   banana LIKE ban%   -> (2, "banana", "ban%",  20)
        //   cherry LIKE %rry   -> (3, "cherry", "%rry",  30)
        //   date   (no match)  -> (4, "date",   NULL,    NULL)
        TString expectedYson = R"([[1;["apple"];["%app%"];[10]];[2;["banana"];["ban%"];[20]];[3;["cherry"];["%rry"];[30]];[4;["date"];#;#]])";
        TString actualYson = FormatResultSetYson(result.GetResultSet(0));
        Cout << "Expected: " << expectedYson << Endl;
        Cout << "Actual:   " << actualYson << Endl;
        UNIT_ASSERT_VALUES_EQUAL(expectedYson, actualYson);
    }

    Y_UNIT_TEST(SimpleHyperscan) {
        // Hyperscan path: the `ydb.EnableLikeJoinHyperscan` pragma IS set, so the
        // LIKE join is lowered to a single Hyperscan multi-pattern automaton pass.
        // The result must be identical to the default double-loop path (Simple).
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableLogConfig()->SetDefaultLevel(NActors::NLog::PRI_DEBUG);

        appConfig.MutableTableServiceConfig()->SetEnableNewRBO(true);
        TKikimrRunner kikimr(NKqp::TKikimrSettings(appConfig).SetWithSampleTables(false));
        auto queryClient = kikimr.GetQueryClient();
        auto querySession = queryClient.GetSession().GetValueSync().GetSession();
        auto db = kikimr.GetTableClient();
        {
            querySession.ExecuteQuery(R"(
            CREATE TABLE `/Root/events` (
                id	Int64	NOT NULL,
                name	String,
                primary key(id)
            );

            CREATE TABLE `/Root/lookup` (
                mask String	NOT NULL,
                id	Int64,
                primary key(id)
            );
            )",
            NYdb::NQuery::TTxControl::NoTx(),
            {}
            ).GetValueSync();

            {
                NYdb::TValueBuilder rows;
                rows.BeginList();
                rows.AddListItem().BeginStruct().AddMember("id").Int64(1).AddMember("name").String("apple").EndStruct();
                rows.AddListItem().BeginStruct().AddMember("id").Int64(2).AddMember("name").String("banana").EndStruct();
                rows.AddListItem().BeginStruct().AddMember("id").Int64(3).AddMember("name").String("cherry").EndStruct();
                rows.AddListItem().BeginStruct().AddMember("id").Int64(4).AddMember("name").String("date").EndStruct();
                rows.EndList();
                auto resultUpsert = db.BulkUpsert("/Root/events", rows.Build()).GetValueSync();
                UNIT_ASSERT_C(resultUpsert.IsSuccess(), resultUpsert.GetIssues().ToString());
            }

            {
                NYdb::TValueBuilder rows;
                rows.BeginList();
                rows.AddListItem().BeginStruct().AddMember("mask").String("%app%").AddMember("id").Int64(10).EndStruct();
                rows.AddListItem().BeginStruct().AddMember("mask").String("ban%").AddMember("id").Int64(20).EndStruct();
                rows.AddListItem().BeginStruct().AddMember("mask").String("%rry").AddMember("id").Int64(30).EndStruct();
                rows.EndList();
                auto resultUpsert = db.BulkUpsert("/Root/lookup", rows.Build()).GetValueSync();
                UNIT_ASSERT_C(resultUpsert.IsSuccess(), resultUpsert.GetIssues().ToString());
            }
        }

        const auto result = querySession.ExecuteQuery(R"(
                PRAGMA ydb.HashJoinMode = 'map';
                PRAGMA ydb.EnableLikeJoinHyperscan = 'true';
                SELECT e.id, e.name, l.mask, l.id as mask_id
                FROM `/Root/events` as e
                LEFT JOIN `/Root/lookup` as l
                ON e.name LIKE l.mask
                ORDER BY e.id
            )",
            NYdb::NQuery::TTxControl::NoTx(),
            NYdb::NQuery::TExecuteQuerySettings().ExecMode(NQuery::EExecMode::Execute)
        ).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS,
            result.GetIssues().ToString());

        TString expectedYson = R"([[1;["apple"];["%app%"];[10]];[2;["banana"];["ban%"];[20]];[3;["cherry"];["%rry"];[30]];[4;["date"];#;#]])";
        TString actualYson = FormatResultSetYson(result.GetResultSet(0));
        Cout << "Expected: " << expectedYson << Endl;
        Cout << "Actual:   " << actualYson << Endl;
        UNIT_ASSERT_VALUES_EQUAL(expectedYson, actualYson);
    }

    Y_UNIT_TEST(Explain) {
        // Regression test for PrunedPartitions crash in task graph builder.
        // When using EXPLAIN mode with RBO-enabled JOIN LIKE queries, the
        // PrunedPartitions metadata was empty, causing std::out_of_range in
        // CountScanTasksFromSource(). The fix adds an early return guard when
        // PrunedPartitions is empty.
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableLogConfig()->SetDefaultLevel(NActors::NLog::PRI_DEBUG);

        appConfig.MutableTableServiceConfig()->SetEnableNewRBO(true);
        TKikimrRunner kikimr(NKqp::TKikimrSettings(appConfig).SetWithSampleTables(false));
        auto queryClient = kikimr.GetQueryClient();
        auto querySession = queryClient.GetSession().GetValueSync().GetSession();
        auto db = kikimr.GetTableClient();
        {
            querySession.ExecuteQuery(R"(
            CREATE TABLE `/Root/events` (
                id	Int64	NOT NULL,
                name	String,
                primary key(id)
            );

            CREATE TABLE `/Root/lookup` (
                mask String	NOT NULL,
                id	Int64,
                primary key(id)
            );
            )",
            NYdb::NQuery::TTxControl::NoTx(),
            {}
            ).GetValueSync();

            // Insert test data into events table.
            {
                NYdb::TValueBuilder rows;
                rows.BeginList();
                rows.AddListItem().BeginStruct().AddMember("id").Int64(1).AddMember("name").String("apple").EndStruct();
                rows.AddListItem().BeginStruct().AddMember("id").Int64(2).AddMember("name").String("banana").EndStruct();
                rows.EndList();
                auto resultUpsert = db.BulkUpsert("/Root/events", rows.Build()).GetValueSync();
                UNIT_ASSERT_C(resultUpsert.IsSuccess(), resultUpsert.GetIssues().ToString());
            }

            // Insert test data into lookup table with LIKE masks.
            {
                NYdb::TValueBuilder rows;
                rows.BeginList();
                rows.AddListItem().BeginStruct().AddMember("mask").String("%app%").AddMember("id").Int64(10).EndStruct();
                rows.AddListItem().BeginStruct().AddMember("mask").String("ban%").AddMember("id").Int64(20).EndStruct();
                rows.EndList();
                auto resultUpsert = db.BulkUpsert("/Root/lookup", rows.Build()).GetValueSync();
                UNIT_ASSERT_C(resultUpsert.IsSuccess(), resultUpsert.GetIssues().ToString());
            }
        }

        // Execute EXPLAIN on the JOIN LIKE query.
        // Before the fix, this would crash with std::out_of_range in
        // TKqpTasksGraph::CountScanTasksFromSource() due to empty PrunedPartitions.
        const auto result = querySession.ExecuteQuery(R"(
                PRAGMA ydb.HashJoinMode = 'map';
                PRAGMA ydb.EnableLikeJoinHyperscan = 'true';
                SELECT e.id, e.name, l.mask, l.id as mask_id
                FROM `/Root/events` as e
                LEFT JOIN `/Root/lookup` as l
                ON e.name LIKE l.mask
                ORDER BY e.id
            )",
            NYdb::NQuery::TTxControl::NoTx(),
            NYdb::NQuery::TExecuteQuerySettings().ExecMode(NQuery::EExecMode::Explain)
        ).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
    }

    Y_UNIT_TEST(NotNullSchema) {
        // Regression test reproducing the std::out_of_range crash in the task
        // graph builder (TKqpTasksGraph::CountScanTasksFromSource ->
        // PrunedPartitions.at(0)) reported in db_start_err.log.
        //
        // Unlike JoinOnLike::Simple (which uses nullable `name`/`id` columns and
        // does NOT crash), this test mirrors the join_like_workload stress schema
        // exactly: both join-relevant columns are declared NOT NULL
        //   events(id Int64 NOT NULL, name String NOT NULL, PK(id))
        //   lookup(mask String NOT NULL, id Int64 NOT NULL, PK(id))
        // With NOT NULL columns the RBO produces a different LIKE-join plan shape
        // in which the row-storage source stage is fanned out through the
        // multi-consumer Switch handler (BuildMultiConsumerHandler). One of the
        // resulting kReadRangesSource stages is iterated by BuildAllTasks but is
        // NOT the stage that the resolve loop populated PrunedPartitions for, so
        // PrunedPartitions stays empty and the unguarded .at(0) throws
        // std::out_of_range, terminating the whole ydbd process.
        //
        // A well-behaved server must complete the query (or at worst reply with
        // an error) instead of crashing.
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableLogConfig()->SetDefaultLevel(NActors::NLog::PRI_DEBUG);

        appConfig.MutableTableServiceConfig()->SetEnableNewRBO(true);
        TKikimrRunner kikimr(NKqp::TKikimrSettings(appConfig).SetWithSampleTables(false));
        auto queryClient = kikimr.GetQueryClient();
        auto querySession = queryClient.GetSession().GetValueSync().GetSession();
        auto db = kikimr.GetTableClient();
        {
            querySession.ExecuteQuery(R"(
            CREATE TABLE `/Root/events` (
                id	Int64	NOT NULL,
                name	String	NOT NULL,
                primary key(id)
            );

            CREATE TABLE `/Root/lookup` (
                mask String	NOT NULL,
                id	Int64	NOT NULL,
                primary key(id)
            );
            )",
            NYdb::NQuery::TTxControl::NoTx(),
            {}
            ).GetValueSync();

            // Insert test data into events table.
            {
                NYdb::TValueBuilder rows;
                rows.BeginList();
                rows.AddListItem().BeginStruct().AddMember("id").Int64(1).AddMember("name").String("apple").EndStruct();
                rows.AddListItem().BeginStruct().AddMember("id").Int64(2).AddMember("name").String("banana").EndStruct();
                rows.AddListItem().BeginStruct().AddMember("id").Int64(3).AddMember("name").String("cherry").EndStruct();
                rows.AddListItem().BeginStruct().AddMember("id").Int64(4).AddMember("name").String("date").EndStruct();
                rows.EndList();
                auto resultUpsert = db.BulkUpsert("/Root/events", rows.Build()).GetValueSync();
                UNIT_ASSERT_C(resultUpsert.IsSuccess(), resultUpsert.GetIssues().ToString());
            }

            // Insert test data into lookup table with LIKE masks.
            {
                NYdb::TValueBuilder rows;
                rows.BeginList();
                rows.AddListItem().BeginStruct().AddMember("mask").String("%app%").AddMember("id").Int64(10).EndStruct();
                rows.AddListItem().BeginStruct().AddMember("mask").String("ban%").AddMember("id").Int64(20).EndStruct();
                rows.AddListItem().BeginStruct().AddMember("mask").String("%rry").AddMember("id").Int64(30).EndStruct();
                rows.EndList();
                auto resultUpsert = db.BulkUpsert("/Root/lookup", rows.Build()).GetValueSync();
                UNIT_ASSERT_C(resultUpsert.IsSuccess(), resultUpsert.GetIssues().ToString());
            }
        }

        // Execute the LEFT JOIN LIKE query in Execute mode (reaches the data
        // executer, where the crash occurs). Before the fix this terminates the
        // whole ydbd process with std::out_of_range.
        const auto result = querySession.ExecuteQuery(R"(
                PRAGMA ydb.HashJoinMode = 'map';
                PRAGMA ydb.EnableLikeJoinHyperscan = 'true';
                SELECT e.id, e.name, l.mask, l.id as mask_id
                FROM `/Root/events` as e
                LEFT JOIN `/Root/lookup` as l
                ON e.name LIKE l.mask
                ORDER BY e.id
            )",
            NYdb::NQuery::TTxControl::NoTx(),
            NYdb::NQuery::TExecuteQuerySettings().ExecMode(NQuery::EExecMode::Execute)
        ).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS,
            result.GetIssues().ToString());

        TString expectedYson = R"([[1;"apple";["%app%"];[10]];[2;"banana";["ban%"];[20]];[3;"cherry";["%rry"];[30]];[4;"date";#;#]])";
        TString actualYson = FormatResultSetYson(result.GetResultSet(0));
        Cout << "Expected: " << expectedYson << Endl;
        Cout << "Actual:   " << actualYson << Endl;
        UNIT_ASSERT_VALUES_EQUAL(expectedYson, actualYson);
    }

    Y_UNIT_TEST(MultiConsumerLikeJoin) {
        // Regression test reproducing the std::out_of_range crash in the task
        // graph builder (TKqpTasksGraph::CountScanTasksFromSource ->
        // PrunedPartitions.at(0)) reported in db_start_err.log.
        //
        // JoinOnLike::Simple (a single-branch LEFT JOIN ... ON ... LIKE) does NOT
        // crash because every row-storage source has exactly one consumer, so the
        // multi-consumer Switch handler (BuildMultiConsumerHandler) is never used.
        //
        // This test forces a *multi-consumer* row-storage source (a CTE that is
        // referenced twice, mirroring KqpRboYql::TestMultiConsumer) and combines
        // it with a LIKE join under HashJoinMode='map'. On this plan shape the
        // shared source is fanned out through the multi-consumer Switch, and one
        // of the resulting kReadRangesSource stages reaches BuildAllTasks without
        // the resolve loop having populated its PrunedPartitions - so the
        // unguarded .at(0) throws std::out_of_range and terminates ydbd.
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableLogConfig()->SetDefaultLevel(NActors::NLog::PRI_DEBUG);

        appConfig.MutableTableServiceConfig()->SetEnableNewRBO(true);
        TKikimrRunner kikimr(NKqp::TKikimrSettings(appConfig).SetWithSampleTables(false));
        auto queryClient = kikimr.GetQueryClient();
        auto querySession = queryClient.GetSession().GetValueSync().GetSession();
        auto db = kikimr.GetTableClient();
        {
            querySession.ExecuteQuery(R"(
            CREATE TABLE `/Root/events` (
                id	Int64	NOT NULL,
                name	String	NOT NULL,
                primary key(id)
            );

            CREATE TABLE `/Root/lookup` (
                mask String	NOT NULL,
                id	Int64	NOT NULL,
                primary key(id)
            );
            )",
            NYdb::NQuery::TTxControl::NoTx(),
            {}
            ).GetValueSync();

            {
                NYdb::TValueBuilder rows;
                rows.BeginList();
                rows.AddListItem().BeginStruct().AddMember("id").Int64(1).AddMember("name").String("apple").EndStruct();
                rows.AddListItem().BeginStruct().AddMember("id").Int64(2).AddMember("name").String("banana").EndStruct();
                rows.AddListItem().BeginStruct().AddMember("id").Int64(3).AddMember("name").String("cherry").EndStruct();
                rows.EndList();
                auto resultUpsert = db.BulkUpsert("/Root/events", rows.Build()).GetValueSync();
                UNIT_ASSERT_C(resultUpsert.IsSuccess(), resultUpsert.GetIssues().ToString());
            }
            {
                NYdb::TValueBuilder rows;
                rows.BeginList();
                rows.AddListItem().BeginStruct().AddMember("mask").String("%app%").AddMember("id").Int64(10).EndStruct();
                rows.AddListItem().BeginStruct().AddMember("mask").String("ban%").AddMember("id").Int64(20).EndStruct();
                rows.EndList();
                auto resultUpsert = db.BulkUpsert("/Root/lookup", rows.Build()).GetValueSync();
                UNIT_ASSERT_C(resultUpsert.IsSuccess(), resultUpsert.GetIssues().ToString());
            }
        }

        // The CTE `$ev` is consumed twice (once per join side), which makes its
        // row-storage source multi-consumer. Combined with the LIKE join this
        // exercises the multi-consumer handler path that the crash originates in.
        const auto result = querySession.ExecuteQuery(R"(
                PRAGMA ydb.HashJoinMode = 'map';
                PRAGMA ydb.EnableLikeJoinHyperscan = 'true';
                $ev = (SELECT id, name FROM `/Root/events`);
                SELECT e.id, e.name, l.mask, l.id AS mask_id
                FROM $ev AS e
                LEFT JOIN `/Root/lookup` AS l
                  ON e.name LIKE l.mask
                WHERE e.id IN (SELECT id FROM $ev)
                ORDER BY e.id
            )",
            NYdb::NQuery::TTxControl::NoTx(),
            NYdb::NQuery::TExecuteQuerySettings().ExecMode(NQuery::EExecMode::Execute)
        ).ExtractValueSync();
        // Reaching this assertion at all (rather than the process terminating
        // with std::out_of_range) is the essence of the regression check.
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS,
            result.GetIssues().ToString());
    }
}

} // namespace NKikimr::NKqp
