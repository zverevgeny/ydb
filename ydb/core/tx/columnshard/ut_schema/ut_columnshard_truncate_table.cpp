#include <ydb/core/base/blobstorage.h>
#include <ydb/core/kqp/compute_actor/kqp_compute_events.h>
#include <ydb/core/protos/long_tx_service_config.pb.h>
#include <ydb/core/tx/columnshard/columnshard_impl.h>
#include <ydb/core/tx/columnshard/columnshard_schema.h>
#include <ydb/core/tx/columnshard/engines/changes/cleanup_portions.h>
#include <ydb/core/tx/columnshard/engines/changes/compaction.h>
#include <ydb/core/tx/columnshard/engines/changes/with_appended.h>
#include <ydb/core/tx/columnshard/engines/portions/portion_info.h>
#include <ydb/core/tx/columnshard/engines/scheme/objects_cache.h>
#include <ydb/core/tx/columnshard/hooks/abstract/abstract.h>
#include <ydb/core/tx/columnshard/hooks/testing/controller.h>
#include <ydb/core/tx/columnshard/operations/write_data.h>
#include <ydb/core/tx/columnshard/test_helper/columnshard_ut_common.h>
#include <ydb/core/tx/columnshard/test_helper/controllers.h>
#include <ydb/core/tx/columnshard/test_helper/shard_reader.h>
#include <ydb/core/tx/columnshard/test_helper/test_combinator.h>
#include <ydb/core/tx/long_tx_service/public/snapshot_registry.h>

#include <ydb/library/actors/protos/unittests.pb.h>
#include <ydb/library/yverify_stream/yverify_stream.h>

#include <arrow/api.h>
#include <arrow/ipc/reader.h>
#include <util/string/join.h>
#include <util/string/printf.h>

namespace NKikimr {

using namespace NColumnShard;
using namespace Tests;
using namespace NTxUT;

using TDefaultTestsController = NKikimr::NYDBTest::NColumnShard::TController;

namespace {

// Update a column in a RecordBatch to a constant value (seconds since epoch).
// Copied from ut_columnshard_schema.cpp.
std::shared_ptr<arrow::RecordBatch> UpdateColumn(std::shared_ptr<arrow::RecordBatch> batch, TString columnName, i64 seconds) {
    std::string name(columnName.c_str(), columnName.size());
    auto schema = batch->schema();
    int pos = schema->GetFieldIndex(name);
    UNIT_ASSERT(pos >= 0);
    auto colType = batch->GetColumnByName(name)->type_id();
    std::shared_ptr<arrow::Array> array;
    if (colType == arrow::Type::TIMESTAMP) {
        auto scalar = arrow::TimestampScalar(seconds * 1000 * 1000, arrow::timestamp(arrow::TimeUnit::MICRO));
        UNIT_ASSERT_VALUES_EQUAL(scalar.value, seconds * 1000 * 1000);
        auto res = arrow::MakeArrayFromScalar(scalar, batch->num_rows());
        UNIT_ASSERT(res.ok());
        array = *res;
    } else if (colType == arrow::Type::UINT16) {
        TInstant date(TInstant::Seconds(seconds));
        auto res = arrow::MakeArrayFromScalar(arrow::UInt16Scalar(date.Days()), batch->num_rows());
        UNIT_ASSERT(res.ok());
        array = *res;
    } else if (colType == arrow::Type::UINT32) {
        auto res = arrow::MakeArrayFromScalar(arrow::UInt32Scalar(seconds), batch->num_rows());
        UNIT_ASSERT(res.ok());
        array = *res;
    } else if (colType == arrow::Type::UINT64) {
        auto res = arrow::MakeArrayFromScalar(arrow::UInt64Scalar(seconds), batch->num_rows());
        UNIT_ASSERT(res.ok());
        array = *res;
    }
    UNIT_ASSERT(array);
    auto columns = batch->columns();
    columns[pos] = array;
    return arrow::RecordBatch::Make(schema, batch->num_rows(), columns);
}

constexpr auto TruncateTestMaxReadStaleness = TDuration::Seconds(1);

void SetupTruncateTestRuntime(TTestBasicRuntime& runtime) {
    TTester::Setup(runtime);
    // Use local scan snapshot guard so SetOverrideMaxReadStaleness controls the cleanup floor.
    runtime.GetAppData().FeatureFlags.SetEnableSnapshotsLocking(false);
}

auto RegisterTruncateTestController() {
    auto guard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
    guard->SetOverrideMaxReadStaleness(TruncateTestMaxReadStaleness);
    guard->DisableBackground(NKikimr::NYDBTest::ICSController::EBackground::Cleanup);
    return guard;
}

const TColumnShard* WaitForShard(TDefaultTestsController& controller, TTestBasicRuntime& runtime) {
    const TInstant deadline = TInstant::Now() + TDuration::Seconds(5);
    while (controller.GetShardActualsCount() == 0 && TInstant::Now() < deadline) {
        runtime.SimulateSleep(TDuration::MilliSeconds(50));
    }
    UNIT_ASSERT_VALUES_EQUAL(controller.GetShardActualsCount(), 1);
    return controller.GetShard();
}

bool IsInPathsToDrop(const TColumnShard& shard, const TInternalPathId& pathId) {
    for (const auto& [_, pathIds] : shard.GetTablesManager().GetPathsToDrop()) {
        if (pathIds.contains(pathId)) {
            return true;
        }
    }
    return false;
}

void AssertPathsToDropState(const TColumnShard& shard, const TInternalPathId& pathId, const bool expectedPresent) {
    UNIT_ASSERT_VALUES_EQUAL(IsInPathsToDrop(shard, pathId), expectedPresent);
}

void AdvanceShardPlanStep(
    TTestBasicRuntime& runtime, TActorId& sender, ui64& txId, int& writeId, const ui64 pathId, const TestTableDescription& testTable) {
    std::vector<ui64> writeIds;
    UNIT_ASSERT(WriteData(runtime, sender, writeId++, pathId, MakeTestBlob({ 0, 1 }, testTable.Schema), testTable.Schema, true, &writeIds));
    const auto planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
    PlanCommit(runtime, sender, planStep, txId);
}

bool HasPortionsRemovedAt(const TColumnShard& shard, const TInternalPathId pathId, const NOlap::TSnapshot& snapshot) {
    const auto& granule = shard.GetTablesManager().GetPrimaryIndexAsVerified<NOlap::TColumnEngineForLogs>().GetGranuleVerified(pathId);
    for (const auto& [_, portion] : granule.GetPortions()) {
        if (portion->IsRemovedFor(snapshot)) {
            return true;
        }
    }
    return false;
}

bool WaitForTruncatedPortionsCleanup(TDefaultTestsController& controller, TTestBasicRuntime& runtime, const TActorId& sender,
    const TInternalPathId pathId, const NOlap::TSnapshot& snapshot, const std::function<void()>& advancePlanStep) {
    const TInstant end = TInstant::Now() + TDuration::Seconds(60);
    while (TInstant::Now() < end) {
        Wakeup(runtime, sender, TTestTxConfig::TxTablet0);
        ForwardToTablet(runtime, TTestTxConfig::TxTablet0, sender, new NColumnShard::TEvPrivate::TEvPingSnapshotsUsage());
        advancePlanStep();
        runtime.SimulateSleep(TDuration::Seconds(1));
        Y_UNUSED(controller.WaitCleaning(TDuration::Seconds(1), &runtime));
        if (const auto* shard = controller.GetAnyShard()) {
            if (!HasPortionsRemovedAt(*shard, pathId, snapshot)) {
                return true;
            }
        }
    }
    return false;
}

bool CheckTableInfoV1RowExists(TTestBasicRuntime& runtime, ui64 tabletId, ui64 internalPathId, ui64 schemeShardLocalPathId) {
    TActorId sender = runtime.AllocateEdgeActor();
    const TString query = Sprintf(R"___(
        (
            (let key '('('PathId (Uint64 '%lu)) '('SchemeShardLocalPathId (Uint64 '%lu))))
            (let select '('PathId))
            (return (AsList (SetResult 'Result (SelectRow 'TableInfoV1 key select))))
        )
    )___", internalPathId, schemeShardLocalPathId);

    auto evTx = new TEvTablet::TEvLocalMKQL;
    evTx->Record.MutableProgram()->MutableProgram()->SetText(query);
    ForwardToTablet(runtime, tabletId, sender, evTx);

    auto event = runtime.GrabEdgeEvent<TEvTablet::TEvLocalMKQLResponse>(sender);
    UNIT_ASSERT(event);
    UNIT_ASSERT_VALUES_EQUAL(event->Get()->Record.GetStatus(), NKikimrProto::OK);
    const auto& result = event->Get()->Record.GetExecutionEngineEvaluatedResponse();
    return result.GetValue().GetStruct(0).GetOptional().HasOptional();
}

}   // namespace

Y_UNIT_TEST_SUITE(TruncateTable) {
    Y_UNIT_TEST(EmptyTable) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csDefaultControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 pathId = 1;
        TestTableDescription testTable{};
        auto planStep = PrepareTablet(runtime, pathId, testTable.Schema);

        ui64 txId = 10;
        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(pathId, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });

        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, NOlap::TSnapshot(planStep, txId));
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(!reader.IsError());
        }
    }

    // Write, truncate, then check both sides of the truncate boundary:
    // a read strictly before the truncate snapshot still sees the old data, a read exactly at
    // the truncate snapshot sees an empty table.
    Y_UNIT_TEST(WithData) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csDefaultControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 pathId = 1;
        TestTableDescription testTable{};
        auto planStep = PrepareTablet(runtime, pathId, testTable.Schema);

        ui64 txId = 10;
        int writeId = 10;
        {
            std::vector<ui64> writeIds;
            UNIT_ASSERT(
                WriteData(runtime, sender, writeId++, pathId, MakeTestBlob({ 0, 100 }, testTable.Schema), testTable.Schema, true, &writeIds));
            planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
            PlanCommit(runtime, sender, planStep, txId);
        }
        const auto snapshotBeforeTruncate = NOlap::TSnapshot(planStep, txId);
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, snapshotBeforeTruncate);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }

        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(pathId, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        const auto truncateSnapshot = NOlap::TSnapshot(planStep, txId);

        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, snapshotBeforeTruncate);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, truncateSnapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(!reader.IsError());
        }
    }

    Y_UNIT_TEST(TruncateAndInsert) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csDefaultControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 pathId = 1;
        TestTableDescription testTable{};
        auto planStep = PrepareTablet(runtime, pathId, testTable.Schema);

        ui64 txId = 10;
        int writeId = 10;
        {
            std::vector<ui64> writeIds;
            UNIT_ASSERT(
                WriteData(runtime, sender, writeId++, pathId, MakeTestBlob({ 0, 100 }, testTable.Schema), testTable.Schema, true, &writeIds));
            planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
            PlanCommit(runtime, sender, planStep, txId);
        }
        const auto snapshotBeforeTruncate = NOlap::TSnapshot(planStep, txId);

        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(pathId, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });

        {
            std::vector<ui64> writeIds;
            UNIT_ASSERT(
                WriteData(runtime, sender, writeId++, pathId, MakeTestBlob({ 200, 250 }, testTable.Schema), testTable.Schema, true, &writeIds));
            planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
            PlanCommit(runtime, sender, planStep, txId);
        }

        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, NOlap::TSnapshot(planStep, txId));
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 50);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, snapshotBeforeTruncate);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }
    }

    Y_UNIT_TEST(TruncateAbsentTable) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csDefaultControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 pathId = 1;
        TestTableDescription testTable{};
        Y_UNUSED(PrepareTablet(runtime, pathId, testTable.Schema));

        ui64 txId = 10;
        ProposeSchemaTxFail(runtime, sender, TTestSchema::TruncateTableTxBody(111, 1), ++txId);
    }

    // Each truncate interval has distinct data. Time-travel and the persistent truncate history
    // must survive reboot without changing table identity.
    Y_UNIT_TEST_DUO(MultipleTruncatesTimeTravel, Reboot) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csDefaultControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 pathId = 1;
        TestTableDescription testTable{};
        auto planStep = PrepareTablet(runtime, pathId, testTable.Schema);

        const auto initialPathId = WaitForShard(*csDefaultControllerGuard.operator->(), runtime)
                                       ->GetTablesManager()
                                       .ResolveInternalPathId(TSchemeShardLocalPathId::FromRawValue(pathId), false);
        UNIT_ASSERT(initialPathId);
        ui64 txId = 10;
        int writeId = 10;
        ui32 schemaRound = 0;

        auto writeAndCommit = [&](ui64 from, ui64 to) -> NOlap::TSnapshot {
            std::vector<ui64> writeIds;
            UNIT_ASSERT(
                WriteData(runtime, sender, writeId++, pathId, MakeTestBlob({ from, to }, testTable.Schema), testTable.Schema, true, &writeIds));
            planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
            PlanCommit(runtime, sender, planStep, txId);
            return NOlap::TSnapshot(planStep, txId);
        };
        auto truncate = [&]() -> NOlap::TSnapshot {
            planStep = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(pathId, ++schemaRound), ++txId);
            PlanSchemaTx(runtime, sender, { planStep, txId });
            return NOlap::TSnapshot(planStep, txId);
        };

        const auto g0Snapshot = writeAndCommit(0, 100);
        const auto t1 = truncate();
        const auto g1Snapshot = writeAndCommit(200, 230);
        const auto t2 = truncate();
        const auto g2Snapshot = writeAndCommit(300, 320);
        if (Reboot) {
            RebootTablet(runtime, TTestTxConfig::TxTablet0, sender);
        }
        const auto* shard = WaitForShard(*csDefaultControllerGuard.operator->(), runtime);
        UNIT_ASSERT_VALUES_EQUAL(shard->GetTablesManager().GetTables().size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(
            *shard->GetTablesManager().ResolveInternalPathId(TSchemeShardLocalPathId::FromRawValue(pathId), false), *initialPathId);
        const auto& granule =
            shard->GetTablesManager().GetPrimaryIndexAsVerified<NOlap::TColumnEngineForLogs>().GetGranuleVerified(*initialPathId);
        UNIT_ASSERT_VALUES_EQUAL(granule.GetTruncateSnapshots().size(), 2);

        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, g0Snapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, g1Snapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 30);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, g2Snapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 20);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, t1);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, t2);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(!reader.IsError());
        }
    }

    // TRUNCATE preserves TTL settings, which must continue to expire new rows.
    Y_UNIT_TEST(TruncatePreservesTtl) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<NOlap::TWaitCompactionController>();
        csControllerGuard->DisableBackground(NKikimr::NYDBTest::ICSController::EBackground::Compaction);
        csControllerGuard->SetOverrideTasksActualizationLag(TDuration::Zero());
        csControllerGuard->SetOverrideCompactionActualizationLag(TDuration::Zero());
        csControllerGuard->SetOverrideOptimizerFreshnessCheckDuration(TDuration::Zero());
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 pathId = 1;
        TestTableDescription testTable{};
        Y_UNUSED(PrepareTablet(runtime, pathId, testTable.Schema));

        const auto ttlDuration = TDuration::Seconds(3600);
        auto specials = TTestSchema::TTableSpecials().SetTtl(ttlDuration);
        specials.SetTtlColumn(TTestSchema::DefaultTtlColumn);
        const auto alterBody =
            TTestSchema::AlterTableTxBody(pathId, /*standalone=*/true, /*version=*/1, testTable.Schema, testTable.Pk, specials);
        ui64 txId = 10;
        auto planStep = ProposeSchemaTx(runtime, sender, alterBody, ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });

        auto& csController = *csControllerGuard.operator->();
        const auto* shard = csController.GetShard();

        {
            const auto internalPathId = shard->GetTablesManager().ResolveInternalPathId(TSchemeShardLocalPathId::FromRawValue(pathId), false);
            UNIT_ASSERT(internalPathId);
            const auto ttl = shard->GetTablesManager().GetTableTtl(*internalPathId);
            UNIT_ASSERT(ttl.has_value());
            UNIT_ASSERT_VALUES_EQUAL(ttl->GetEvictColumnName(), TTestSchema::DefaultTtlColumn);
            const auto& tiers = ttl->GetOrderedTiers();
            UNIT_ASSERT_VALUES_EQUAL(tiers.size(), 1);
            const auto& tier = *tiers.begin();
            UNIT_ASSERT_VALUES_EQUAL(tier.Get().GetEvictColumnName(), TTestSchema::DefaultTtlColumn);
            UNIT_ASSERT_VALUES_EQUAL(tier.Get().GetEvictDuration(), ttlDuration);
        }

        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(pathId, 2), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        shard = csController.GetShard();

        {
            const auto newInternalPathId = shard->GetTablesManager().ResolveInternalPathId(TSchemeShardLocalPathId::FromRawValue(pathId), false);
            UNIT_ASSERT(newInternalPathId);
            const auto ttl = shard->GetTablesManager().GetTableTtl(*newInternalPathId);
            UNIT_ASSERT(ttl.has_value());
            UNIT_ASSERT_VALUES_EQUAL(ttl->GetEvictColumnName(), TTestSchema::DefaultTtlColumn);
            const auto& tiers = ttl->GetOrderedTiers();
            UNIT_ASSERT_VALUES_EQUAL(tiers.size(), 1);
            const auto& tier = *tiers.begin();
            UNIT_ASSERT_VALUES_EQUAL(tier.Get().GetEvictColumnName(), TTestSchema::DefaultTtlColumn);
            UNIT_ASSERT_VALUES_EQUAL(tier.Get().GetEvictDuration(), ttlDuration);
        }

        const auto now = TAppData::TimeProvider->Now().Seconds();
        const auto staleTs = now - 7200;
        const auto freshTs = now - 1800;
        std::vector<ui64> writeIds;
        const auto arrowSchema = NArrow::MakeArrowSchema(testTable.Schema);
        auto writeWithTtlTs = [&](const ui64 writeId, const std::pair<ui64, ui64> range, const i64 ts) {
            const TString blob = MakeTestBlob(range, testTable.Schema);
            auto batch = NArrow::DeserializeBatch(blob, arrowSchema);
            UNIT_ASSERT(batch);
            batch = UpdateColumn(batch, TTestSchema::DefaultTtlColumn, ts);
            const TString data = NArrow::SerializeBatchNoCompression(batch);
            UNIT_ASSERT(WriteData(runtime, sender, writeId, pathId, data, testTable.Schema, true, &writeIds));
        };
        writeWithTtlTs(100, { 0, 1 }, staleTs);
        writeWithTtlTs(101, { 1, 2 }, freshTs);
        planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
        PlanCommit(runtime, sender, planStep, txId);
        const auto dataSnapshot = NOlap::TSnapshot(planStep, txId);
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, dataSnapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 2);
            UNIT_ASSERT(!reader.IsError());
        }

        // TTL eviction commits at a fresh plan step, so it is invisible at dataSnapshot (MVCC).
        // Advance the plan step with an empty commit and read the latest state at that step
        // (TxId = Max<ui64>()) to observe eviction of the stale row.
        auto readLatestRowCount = [&]() -> ui64 {
            planStep = planStep + 1;
            PlanCommit(runtime, sender, planStep, TSet<ui64>{});
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, NOlap::TSnapshot(planStep, Max<ui64>()));
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(reader.IsCorrectlyFinished());
            return rb ? rb->num_rows() : 0;
        };
        ui64 evictedRowCount = 0;
        csController.WaitCondition(TDuration::Seconds(30), [&] {
            runtime.SimulateSleep(TDuration::MilliSeconds(200));
            evictedRowCount = readLatestRowCount();
            return evictedRowCount == 1;
        });
        UNIT_ASSERT_VALUES_EQUAL(evictedRowCount, 1);
    }

    // When TTL is removed via ALTER and then TRUNCATE is performed, the table
    // must keep TTL disabled. With Reboot=true, exercises the
    // InitFromDB → AddVersionFromProto path with the nullopt case.
    Y_UNIT_TEST_DUO(TruncateAfterTtlRemoved, Reboot) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 pathId = 1;
        TestTableDescription testTable{};
        Y_UNUSED(PrepareTablet(runtime, pathId, testTable.Schema));

        ui64 txId = 10;

        // Step 1: Set TTL via ALTER.
        const auto ttlDuration = TDuration::Seconds(3600);
        auto specials = TTestSchema::TTableSpecials().SetTtl(ttlDuration);
        specials.SetTtlColumn(TTestSchema::DefaultTtlColumn);
        {
            const auto alterBody =
                TTestSchema::AlterTableTxBody(pathId, /*standalone=*/true, /*version=*/1, testTable.Schema, testTable.Pk, specials);
            auto planStep = ProposeSchemaTx(runtime, sender, alterBody, ++txId);
            PlanSchemaTx(runtime, sender, { planStep, txId });
        }

        auto& csController = *csControllerGuard.operator->();
        const auto* shard = csController.GetShard();

        // Verify TTL is set.
        {
            const auto internalPathId = shard->GetTablesManager().ResolveInternalPathId(TSchemeShardLocalPathId::FromRawValue(pathId), false);
            UNIT_ASSERT(internalPathId);
            const auto ttl = shard->GetTablesManager().GetTableTtl(*internalPathId);
            UNIT_ASSERT(ttl.has_value());
        }

        // Step 2: Remove TTL via ALTER (empty TTableSpecials → Disabled).
        {
            const auto alterBody = TTestSchema::AlterTableTxBody(
                pathId, /*standalone=*/true, /*version=*/2, testTable.Schema, testTable.Pk, TTestSchema::TTableSpecials{});
            auto planStep = ProposeSchemaTx(runtime, sender, alterBody, ++txId);
            PlanSchemaTx(runtime, sender, { planStep, txId });
        }

        // Verify TTL is removed.
        {
            const auto internalPathId = shard->GetTablesManager().ResolveInternalPathId(TSchemeShardLocalPathId::FromRawValue(pathId), false);
            UNIT_ASSERT(internalPathId);
            const auto ttl = shard->GetTablesManager().GetTableTtl(*internalPathId);
            UNIT_ASSERT(!ttl.has_value());
        }

        // Step 3: Optionally restart the tablet to force InitFromDB reload.
        if (Reboot) {
            RebootTablet(runtime, TTestTxConfig::TxTablet0, sender);
            shard = csController.GetShard();
            const auto internalPathId = shard->GetTablesManager().ResolveInternalPathId(TSchemeShardLocalPathId::FromRawValue(pathId), false);
            UNIT_ASSERT(internalPathId);
            const auto ttl = shard->GetTablesManager().GetTableTtl(*internalPathId);
            UNIT_ASSERT(!ttl.has_value());
        }

        // Step 4: TRUNCATE.
        {
            auto planStep = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(pathId, 3), ++txId);
            PlanSchemaTx(runtime, sender, { planStep, txId });
        }

        // Verify that TTL remains disabled after truncate.
        {
            const auto newInternalPathId = shard->GetTablesManager().ResolveInternalPathId(TSchemeShardLocalPathId::FromRawValue(pathId), false);
            UNIT_ASSERT(newInternalPathId);
            const auto ttl = shard->GetTablesManager().GetTableTtl(*newInternalPathId);
            UNIT_ASSERT(!ttl.has_value());
        }
    }

    // TTL must survive schema-only ALTER (e.g. ADD COLUMN) that does not carry TTL settings.
    // Before the fix, AddVersionFromProto added nullopt for versions without TTL settings,
    // so GetTableTtl(Max) resolved the latest version as no-TTL and tiering was lost after reboot.
    Y_UNIT_TEST_DUO(TtlSurvivesSchemaOnlyAlter, Reboot) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 pathId = 1;
        TestTableDescription testTable{};
        Y_UNUSED(PrepareTablet(runtime, pathId, testTable.Schema));

        ui64 txId = 10;

        // Step 1: Set TTL via ALTER.
        const auto ttlDuration = TDuration::Seconds(3600);
        auto specials = TTestSchema::TTableSpecials().SetTtl(ttlDuration);
        specials.SetTtlColumn(TTestSchema::DefaultTtlColumn);
        {
            const auto alterBody =
                TTestSchema::AlterTableTxBody(pathId, /*standalone=*/true, /*version=*/1, testTable.Schema, testTable.Pk, specials);
            auto planStep = ProposeSchemaTx(runtime, sender, alterBody, ++txId);
            PlanSchemaTx(runtime, sender, { planStep, txId });
        }

        auto& csController = *csControllerGuard.operator->();
        const auto* shard = csController.GetShard();

        // Verify TTL is set.
        {
            const auto internalPathId = shard->GetTablesManager().ResolveInternalPathId(TSchemeShardLocalPathId::FromRawValue(pathId), false);
            UNIT_ASSERT(internalPathId);
            const auto ttl = shard->GetTablesManager().GetTableTtl(*internalPathId);
            UNIT_ASSERT(ttl.has_value());
            UNIT_ASSERT_VALUES_EQUAL(ttl->GetEvictColumnName(), TTestSchema::DefaultTtlColumn);
        }

        // Step 2: Schema-only ALTER (ADD COLUMN) without TTL settings (carry-over).
        // setTtlSettings=false means the proto has no TtlSettings field → carry-over.
        {
            auto schemaWithNewColumn = testTable.Schema;
            schemaWithNewColumn.push_back(NArrow::NTest::TTestColumn("new_column", NScheme::TTypeInfo(NScheme::NTypeIds::Int32)));
            const auto alterBody = TTestSchema::AlterTableTxBody(pathId, /*standalone=*/true, /*version=*/2, schemaWithNewColumn, testTable.Pk,
                TTestSchema::TTableSpecials{}, /*setTtlSettings=*/false);
            auto planStep = ProposeSchemaTx(runtime, sender, alterBody, ++txId);
            PlanSchemaTx(runtime, sender, { planStep, txId });
        }

        // Verify TTL is still active (not lost due to schema-only ALTER).
        {
            const auto internalPathId = shard->GetTablesManager().ResolveInternalPathId(TSchemeShardLocalPathId::FromRawValue(pathId), false);
            UNIT_ASSERT(internalPathId);
            const auto ttl = shard->GetTablesManager().GetTableTtl(*internalPathId);
            UNIT_ASSERT(ttl.has_value());
            UNIT_ASSERT_VALUES_EQUAL(ttl->GetEvictColumnName(), TTestSchema::DefaultTtlColumn);
        }

        // Step 3: Optionally restart the tablet to force InitFromDB reload.
        // This is where the bug manifested: AddVersionFromProto added nullopt for version 2,
        // so GetTableTtl(Max) resolved to no-TTL after reboot.
        if (Reboot) {
            RebootTablet(runtime, TTestTxConfig::TxTablet0, sender);
        }
        shard = csController.GetShard();
        const auto internalPathId = shard->GetTablesManager().ResolveInternalPathId(TSchemeShardLocalPathId::FromRawValue(pathId), false);
        UNIT_ASSERT(internalPathId);
        const auto ttl = shard->GetTablesManager().GetTableTtl(*internalPathId);
        UNIT_ASSERT(ttl.has_value());
        UNIT_ASSERT_VALUES_EQUAL(ttl->GetEvictColumnName(), TTestSchema::DefaultTtlColumn);
    }

    // ALTER after TRUNCATE updates the same table while preserving pre-truncate time-travel.
    Y_UNIT_TEST(TruncateThenAlter) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 pathId = 1;
        TestTableDescription testTable{};
        auto planStep = PrepareTablet(runtime, pathId, testTable.Schema);

        ui64 txId = 10;
        int writeId = 10;
        {
            std::vector<ui64> writeIds;
            UNIT_ASSERT(
                WriteData(runtime, sender, writeId++, pathId, MakeTestBlob({ 0, 100 }, testTable.Schema), testTable.Schema, true, &writeIds));
            planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
            PlanCommit(runtime, sender, planStep, txId);
        }
        const auto snapshotBeforeTruncate = NOlap::TSnapshot(planStep, txId);

        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(pathId, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        const auto truncateSnapshot = NOlap::TSnapshot(planStep, txId);

        auto& csController = *csControllerGuard.operator->();
        const auto* shard = csController.GetShard();
        const auto newInternalPathId = shard->GetTablesManager().ResolveInternalPathId(TSchemeShardLocalPathId::FromRawValue(pathId), false);
        UNIT_ASSERT(newInternalPathId);
        UNIT_ASSERT(!shard->GetTablesManager().GetTableTtl(*newInternalPathId).has_value());

        auto specials = TTestSchema::TTableSpecials().SetTtl(TDuration::Seconds(3600));
        specials.SetTtlColumn(TTestSchema::DefaultTtlColumn);
        const auto alterBody =
            TTestSchema::AlterTableTxBody(pathId, /*standalone=*/true, /*version=*/2, testTable.Schema, testTable.Pk, specials);
        planStep = ProposeSchemaTx(runtime, sender, alterBody, ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });

        shard = csController.GetShard();
        {
            const auto resolved = shard->GetTablesManager().ResolveInternalPathId(TSchemeShardLocalPathId::FromRawValue(pathId), false);
            UNIT_ASSERT(resolved);
            UNIT_ASSERT_VALUES_EQUAL(*resolved, *newInternalPathId);
            UNIT_ASSERT(shard->GetTablesManager().GetTableTtl(*resolved).has_value());
        }

        {
            std::vector<ui64> writeIds;
            UNIT_ASSERT(
                WriteData(runtime, sender, writeId++, pathId, MakeTestBlob({ 200, 250 }, testTable.Schema), testTable.Schema, true, &writeIds));
            planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
            PlanCommit(runtime, sender, planStep, txId);
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, NOlap::TSnapshot(planStep, txId));
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 50);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, snapshotBeforeTruncate);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, truncateSnapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(!reader.IsError());
        }
    }

    // MOVE after TRUNCATE renames the SS path while preserving the table's truncate history.
    // Time-travel therefore works on dst, not on src. Reboot must preserve that mapping.
    Y_UNIT_TEST_DUO(TruncateThenMove, Reboot) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csDefaultControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 srcPathId = 1;
        const ui64 dstPathId = 2;
        TestTableDescription testTable{};
        auto planStep = PrepareTablet(runtime, srcPathId, testTable.Schema);

        ui64 txId = 10;
        int writeId = 10;
        {
            std::vector<ui64> writeIds;
            UNIT_ASSERT(
                WriteData(runtime, sender, writeId++, srcPathId, MakeTestBlob({ 0, 100 }, testTable.Schema), testTable.Schema, true, &writeIds));
            planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
            PlanCommit(runtime, sender, planStep, txId);
        }
        const auto snapshotBeforeTruncate = NOlap::TSnapshot(planStep, txId);

        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(srcPathId, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::MoveTableTxBody(srcPathId, dstPathId, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        const auto moveSnapshot = NOlap::TSnapshot(planStep, txId);

        if (Reboot) {
            RebootTablet(runtime, TTestTxConfig::TxTablet0, sender);
        }

        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, srcPathId, moveSnapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, dstPathId, moveSnapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(!reader.IsError());
        }

        {
            std::vector<ui64> writeIds;
            UNIT_ASSERT(WriteData(
                runtime, sender, writeId++, dstPathId, MakeTestBlob({ 200, 250 }, testTable.Schema), testTable.Schema, true, &writeIds));
            planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
            PlanCommit(runtime, sender, planStep, txId);
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, dstPathId, NOlap::TSnapshot(planStep, txId));
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 50);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, dstPathId, snapshotBeforeTruncate);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, srcPathId, snapshotBeforeTruncate);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(reader.IsError());
        }
    }

    // COPY after TRUNCATE captures the empty table. The copy is pinned at its CopyVersion,
    // so later writes to the source are NOT visible on the copy.
    Y_UNIT_TEST(TruncateThenCopy) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csDefaultControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 srcPathId = 1;
        const ui64 dstPathId = 2;
        TestTableDescription testTable{};
        auto planStep = PrepareTablet(runtime, srcPathId, testTable.Schema);

        ui64 txId = 10;
        int writeId = 10;
        {
            std::vector<ui64> writeIds;
            UNIT_ASSERT(
                WriteData(runtime, sender, writeId++, srcPathId, MakeTestBlob({ 0, 100 }, testTable.Schema), testTable.Schema, true, &writeIds));
            planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
            PlanCommit(runtime, sender, planStep, txId);
        }
        const auto snapshotBeforeTruncate = NOlap::TSnapshot(planStep, txId);

        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(srcPathId, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        const auto truncateSnapshot = NOlap::TSnapshot(planStep, txId);
        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::CopyTableTxBody(srcPathId, dstPathId, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        const auto copySnapshot = NOlap::TSnapshot(planStep, txId);

        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, srcPathId, copySnapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, dstPathId, copySnapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, srcPathId, snapshotBeforeTruncate);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, dstPathId, truncateSnapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(!reader.IsError());
        }

        {
            std::vector<ui64> writeIds;
            UNIT_ASSERT(WriteData(
                runtime, sender, writeId++, srcPathId, MakeTestBlob({ 200, 250 }, testTable.Schema), testTable.Schema, true, &writeIds));
            planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
            PlanCommit(runtime, sender, planStep, txId);
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, srcPathId, NOlap::TSnapshot(planStep, txId));
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 50);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            // The copy is pinned at CopyVersion, taken while the table was empty after truncate,
            // so the later source write is invisible on dst.
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, dstPathId, NOlap::TSnapshot(planStep, txId));
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(!reader.IsError());
        }

        ProposeSchemaTxFail(runtime, sender, TTestSchema::TruncateTableTxBody(dstPathId, 1), ++txId);
    }

    Y_UNIT_TEST(TruncateAndDrop) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csDefaultControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 pathId = 1;
        TestTableDescription testTable{};
        auto planStep = PrepareTablet(runtime, pathId, testTable.Schema);

        ui64 txId = 10;
        int writeId = 10;
        {
            std::vector<ui64> writeIds;
            UNIT_ASSERT(
                WriteData(runtime, sender, writeId++, pathId, MakeTestBlob({ 0, 100 }, testTable.Schema), testTable.Schema, true, &writeIds));
            planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
            PlanCommit(runtime, sender, planStep, txId);
        }
        const auto snapshotBeforeTruncate = NOlap::TSnapshot(planStep, txId);

        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(pathId, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        const auto truncateSnapshot = NOlap::TSnapshot(planStep, txId);
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, snapshotBeforeTruncate);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, truncateSnapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(!reader.IsError());
        }

        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::DropTableTxBody(pathId, 2), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, NOlap::TSnapshot(planStep, txId));
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, snapshotBeforeTruncate);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }
    }

    Y_UNIT_TEST(TruncateReadOnlyTableFails) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csDefaultControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 srcPathId = 1;
        TestTableDescription testTable{};
        auto planStep = PrepareTablet(runtime, srcPathId, testTable.Schema);

        ui64 txId = 10;
        int writeId = 10;
        {
            std::vector<ui64> writeIds;
            UNIT_ASSERT(
                WriteData(runtime, sender, writeId++, srcPathId, MakeTestBlob({ 0, 100 }, testTable.Schema), testTable.Schema, true, &writeIds));
            planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
            PlanCommit(runtime, sender, planStep, txId);
        }

        const ui64 dstPathId = 2;
        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::CopyTableTxBody(srcPathId, dstPathId, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        ProposeSchemaTxFail(runtime, sender, TTestSchema::TruncateTableTxBody(dstPathId, 1), ++txId);
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, dstPathId, NOlap::TSnapshot(planStep, txId));
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }
    }

    // Copies and time-travel retain truncated portions across reboot. Once both copies are
    // dropped and snapshots expire, GC removes those portions while retaining the live source.
    Y_UNIT_TEST(TruncateCopySourceRetention) {
        TTestBasicRuntime runtime;
        SetupTruncateTestRuntime(runtime);
        auto csControllerGuard = RegisterTruncateTestController();
        auto& csController = *csControllerGuard.operator->();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 srcPathId = 1;
        const ui64 copyPathIdA = 2;
        const ui64 copyPathIdB = 3;
        TestTableDescription testTable{};
        auto planStep = PrepareTablet(runtime, srcPathId, testTable.Schema);

        ui64 txId = 10;
        int writeId = 10;
        {
            std::vector<ui64> writeIds;
            UNIT_ASSERT(
                WriteData(runtime, sender, writeId++, srcPathId, MakeTestBlob({ 0, 100 }, testTable.Schema), testTable.Schema, true, &writeIds));
            planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
            PlanCommit(runtime, sender, planStep, txId);
        }
        const auto snapshotBeforeTruncate = NOlap::TSnapshot(planStep, txId);

        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::CopyTableTxBody(srcPathId, copyPathIdA, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::CopyTableTxBody(srcPathId, copyPathIdB, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(srcPathId, 2), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        const auto truncateSnapshot = NOlap::TSnapshot(planStep, txId);

        const auto* shard = WaitForShard(csController, runtime);
        UNIT_ASSERT(shard);
        const auto oldInternalPathId =
            shard->GetTablesManager().ResolveInternalPathId(TSchemeShardLocalPathId::FromRawValue(copyPathIdA), false);
        UNIT_ASSERT(oldInternalPathId);

        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, srcPathId, truncateSnapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, srcPathId, snapshotBeforeTruncate);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, copyPathIdA, truncateSnapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, copyPathIdB, truncateSnapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, copyPathIdA, snapshotBeforeTruncate);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }
        UNIT_ASSERT(CheckTableInfoV1RowExists(runtime, TTestTxConfig::TxTablet0, oldInternalPathId->GetRawValue(), srcPathId));

        RebootTablet(runtime, TTestTxConfig::TxTablet0, sender);

        const auto* restartedShard = WaitForShard(csController, runtime);
        UNIT_ASSERT(restartedShard);
        {
            const auto recoveredOld =
                restartedShard->GetTablesManager().ResolveInternalPathId(TSchemeShardLocalPathId::FromRawValue(copyPathIdA), false);
            UNIT_ASSERT(recoveredOld);
            UNIT_ASSERT_VALUES_EQUAL(*recoveredOld, *oldInternalPathId);
            const auto pathDropVersion = restartedShard->GetTablesManager()
                                             .GetTable(*oldInternalPathId)
                                             .GetPathDropVersionOptional(TSchemeShardLocalPathId::FromRawValue(srcPathId));
            UNIT_ASSERT(!pathDropVersion);
            UNIT_ASSERT(restartedShard->GetTablesManager()
                            .GetPrimaryIndexAsVerified<NOlap::TColumnEngineForLogs>()
                            .GetGranuleVerified(*oldInternalPathId)
                            .GetTruncateSnapshots()
                            .contains(truncateSnapshot));
        }

        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, srcPathId, truncateSnapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, srcPathId, snapshotBeforeTruncate);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, copyPathIdA, truncateSnapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, copyPathIdB, truncateSnapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }

        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::DropTableTxBody(copyPathIdA, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, copyPathIdB, truncateSnapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }
        UNIT_ASSERT(restartedShard->GetTablesManager().HasTable(*oldInternalPathId, true));

        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::DropTableTxBody(copyPathIdB, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        UNIT_ASSERT(!restartedShard->GetTablesManager().GetTable(*oldInternalPathId, true).IsDropped());

        // Expire read snapshots and allow GC to remove truncated portions.
        csControllerGuard->SetOverrideUsedSnapshotLivetime(TDuration::Zero());
        csControllerGuard->EnableBackground(NKikimr::NYDBTest::ICSController::EBackground::Cleanup);
        auto advancePlanStep = [&] {
            AdvanceShardPlanStep(runtime, sender, txId, writeId, srcPathId, testTable);
        };
        UNIT_ASSERT(WaitForTruncatedPortionsCleanup(csController, runtime, sender, *oldInternalPathId, truncateSnapshot, advancePlanStep));

        {
            const auto* finalizedShard = csController.GetAnyShard();
            UNIT_ASSERT(finalizedShard);
            UNIT_ASSERT(finalizedShard->GetTablesManager().HasTable(*oldInternalPathId));
        }
        {
            // After GC removes truncated portions, the read-staleness floor has advanced past
            // snapshotBeforeTruncate, so the time-travel read is rejected ("Snapshot too old").
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, srcPathId, snapshotBeforeTruncate);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(reader.IsError());
        }
    }

    // An active copy scan must retain its portions across source TRUNCATE and copy DROP.
    Y_UNIT_TEST(ActiveCopyScanSurvivesTruncateAndDrop) {
        TTestBasicRuntime runtime;
        SetupTruncateTestRuntime(runtime);
        auto controllerGuard = RegisterTruncateTestController();
        auto& controller = *controllerGuard.operator->();
        controllerGuard->SetOverridePeriodicWakeupActivationPeriod(TDuration::Seconds(1));
        TActorId sender = runtime.AllocateEdgeActor();

        constexpr ui64 srcPathId = 1;
        constexpr ui64 copyPathId = 2;
        TestTableDescription testTable{};
        Y_UNUSED(PrepareTablet(runtime, srcPathId, testTable.Schema));

        ui64 txId = 10;
        int writeId = 10;
        std::vector<ui64> writeIds;
        UNIT_ASSERT(
            WriteData(runtime, sender, writeId++, srcPathId, MakeTestBlob({ 0, 100 }, testTable.Schema), testTable.Schema, true, &writeIds));
        auto planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
        PlanCommit(runtime, sender, planStep, txId);

        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::CopyTableTxBody(srcPathId, copyPathId, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        const auto copySnapshot = NOlap::TSnapshot(planStep, txId);
        const auto* shard = WaitForShard(controller, runtime);
        const auto oldInternalPathId = shard->GetTablesManager().ResolveInternalPathId(TSchemeShardLocalPathId::FromRawValue(copyPathId), false);
        UNIT_ASSERT(oldInternalPathId);

        TShardReader activeScan(runtime, TTestTxConfig::TxTablet0, copyPathId, copySnapshot);
        activeScan.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
        UNIT_ASSERT_C(activeScan.InitializeScanner(), "copy scan must start before TRUNCATE");

        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(srcPathId, 2), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        const auto truncateSnapshot = NOlap::TSnapshot(planStep, txId);
        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::DropTableTxBody(copyPathId, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });

        shard = WaitForShard(controller, runtime);
        UNIT_ASSERT(!shard->GetTablesManager().GetTable(*oldInternalPathId, true).IsDropped());
        AssertPathsToDropState(*shard, *oldInternalPathId, false);

        controllerGuard->SetOverrideUsedSnapshotLivetime(TDuration::Zero());
        controllerGuard->EnableBackground(NKikimr::NYDBTest::ICSController::EBackground::Cleanup);
        for (ui32 i = 0; i < 5; ++i) {
            AdvanceShardPlanStep(runtime, sender, txId, writeId, srcPathId, testTable);
            Wakeup(runtime, sender, TTestTxConfig::TxTablet0);
            ForwardToTablet(runtime, TTestTxConfig::TxTablet0, sender, new NColumnShard::TEvPrivate::TEvPingSnapshotsUsage());
            runtime.SimulateSleep(TDuration::Seconds(1));
            Y_UNUSED(controller.WaitCleaning(TDuration::Seconds(1), &runtime));
            shard = WaitForShard(controller, runtime);
            UNIT_ASSERT_C(HasPortionsRemovedAt(*shard, *oldInternalPathId, truncateSnapshot),
                "GC must retain truncated portions while the copy scan is active");
        }

        activeScan.Ack();
        const auto rows = activeScan.ContinueReadAll();
        UNIT_ASSERT(activeScan.IsCorrectlyFinished());
        UNIT_ASSERT(rows);
        UNIT_ASSERT_VALUES_EQUAL(rows->num_rows(), 100);

        auto advancePlanStep = [&] {
            AdvanceShardPlanStep(runtime, sender, txId, writeId, srcPathId, testTable);
        };
        UNIT_ASSERT(WaitForTruncatedPortionsCleanup(controller, runtime, sender, *oldInternalPathId, truncateSnapshot, advancePlanStep));
        UNIT_ASSERT(controller.GetAnyShard()->GetTablesManager().HasTable(*oldInternalPathId, true));
    }

    // A live copy retains the shared table after source DROP. Expired source scans must
    // fail normally while the copy remains readable.
    Y_UNIT_TEST(DroppedSourceWithLiveCopyRejectsLateScanAfterGc) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        runtime.GetAppData(0).FeatureFlags.SetEnableSnapshotsLocking(true);
        auto& longTx = runtime.GetAppData(0).LongTxServiceConfig;
        longTx.SetLocalSnapshotPromotionTimeSeconds(1);
        longTx.SetMaxClockSkewMs(1000);
        longTx.SetSnapshotsExchangeIntervalSeconds(1);
        longTx.SetSnapshotsRegistryUpdateIntervalSeconds(1);

        auto controllerGuard = RegisterTruncateTestController();
        auto& controller = *controllerGuard.operator->();
        controllerGuard->SetOverridePeriodicWakeupActivationPeriod(TDuration::Seconds(1));
        controllerGuard->SetOverrideUsedSnapshotLivetime(TDuration::Zero());
        TActorId sender = runtime.AllocateEdgeActor();

        constexpr ui64 srcPathId = 1;
        constexpr ui64 copyPathId = 2;
        TestTableDescription testTable{};
        Y_UNUSED(PrepareTablet(runtime, srcPathId, testTable.Schema));

        ui64 txId = 10;
        int writeId = 10;
        std::vector<ui64> writeIds;
        UNIT_ASSERT(
            WriteData(runtime, sender, writeId++, srcPathId, MakeTestBlob({ 0, 100 }, testTable.Schema), testTable.Schema, true, &writeIds));
        auto planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
        PlanCommit(runtime, sender, planStep, txId);

        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::CopyTableTxBody(srcPathId, copyPathId, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        const auto copySnapshot = NOlap::TSnapshot(planStep, txId);
        const auto* shard = WaitForShard(controller, runtime);
        const auto oldInternalPathId = shard->GetTablesManager().ResolveInternalPathId(TSchemeShardLocalPathId::FromRawValue(copyPathId), false);
        UNIT_ASSERT(oldInternalPathId);

        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(srcPathId, 2), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        const auto truncateSnapshot = NOlap::TSnapshot(planStep, txId);
        shard = WaitForShard(controller, runtime);
        const auto newInternalPathId = shard->GetTablesManager().ResolveInternalPathId(TSchemeShardLocalPathId::FromRawValue(srcPathId), false);
        UNIT_ASSERT(newInternalPathId);
        UNIT_ASSERT_VALUES_EQUAL(*oldInternalPathId, *newInternalPathId);

        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::DropTableTxBody(srcPathId, 3), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        shard = WaitForShard(controller, runtime);
        AssertPathsToDropState(*shard, *newInternalPathId, false);
        AssertPathsToDropState(*shard, *oldInternalPathId, false);

        controllerGuard->EnableBackground(NKikimr::NYDBTest::ICSController::EBackground::Cleanup);
        ui64 nextPlanStep = planStep.Val() + 10000;
        for (ui32 i = 0; i < 5; ++i) {
            runtime.SimulateSleep(TDuration::Seconds(1));
            auto registryBuilder = CreateImmutableSnapshotRegistryBuilder();
            registryBuilder->SetOldestCollectionTime(runtime.GetCurrentTime());
            runtime.GetAppData(0).SnapshotRegistryHolder->Set(std::move(*registryBuilder).Build());
            PlanCommit(runtime, sender, TPlanStep{ nextPlanStep++ }, TSet<ui64>{});
            Wakeup(runtime, sender, TTestTxConfig::TxTablet0);
            Y_UNUSED(controller.WaitCleaning(TDuration::Seconds(1), &runtime));
        }

        shard = WaitForShard(controller, runtime);
        UNIT_ASSERT_C(shard->GetTablesManager().HasTable(*newInternalPathId, true), "the copy must retain the shared table");
        UNIT_ASSERT(shard->GetTablesManager().HasTable(*oldInternalPathId, true));
        const auto src = TSchemeShardLocalPathId::FromRawValue(srcPathId);
        UNIT_ASSERT(!shard->GetTablesManager().ResolveInternalPathId(src, false));

        TShardReader lateScan(runtime, TTestTxConfig::TxTablet0, srcPathId, truncateSnapshot);
        lateScan.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
        UNIT_ASSERT(!lateScan.ReadAll());
        UNIT_ASSERT_C(lateScan.IsError(), "late scan of dropped source must fail without crashing the tablet");

        // A path with neither history nor a live mapping must also fail without aborting the tablet.
        TShardReader missingScan(runtime, TTestTxConfig::TxTablet0, 3, copySnapshot);
        missingScan.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
        UNIT_ASSERT(!missingScan.ReadAll());
        UNIT_ASSERT(missingScan.IsError());

        TShardReader copyScan(runtime, TTestTxConfig::TxTablet0, copyPathId, copySnapshot);
        copyScan.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
        const auto rows = copyScan.ReadAll();
        UNIT_ASSERT(copyScan.IsCorrectlyFinished());
        UNIT_ASSERT(rows);
        UNIT_ASSERT_VALUES_EQUAL(rows->num_rows(), 100);
    }

    // Second TRUNCATE of the source while a copy of its initial contents is still alive.
    Y_UNIT_TEST(TruncateSourceTwiceWithLiveCopy) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csDefaultControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 srcPathId = 1;
        const ui64 dstPathId = 2;
        TestTableDescription testTable{};
        auto planStep = PrepareTablet(runtime, srcPathId, testTable.Schema);

        ui64 txId = 10;
        int writeId = 10;
        {
            std::vector<ui64> writeIds;
            UNIT_ASSERT(
                WriteData(runtime, sender, writeId++, srcPathId, MakeTestBlob({ 0, 100 }, testTable.Schema), testTable.Schema, true, &writeIds));
            planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
            PlanCommit(runtime, sender, planStep, txId);
        }
        const auto g0Snapshot = NOlap::TSnapshot(planStep, txId);

        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::CopyTableTxBody(srcPathId, dstPathId, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(srcPathId, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        const auto t1 = NOlap::TSnapshot(planStep, txId);

        {
            std::vector<ui64> writeIds;
            UNIT_ASSERT(WriteData(
                runtime, sender, writeId++, srcPathId, MakeTestBlob({ 200, 220 }, testTable.Schema), testTable.Schema, true, &writeIds));
            planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
            PlanCommit(runtime, sender, planStep, txId);
        }
        const auto g1Snapshot = NOlap::TSnapshot(planStep, txId);

        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(srcPathId, 2), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        const auto t2 = NOlap::TSnapshot(planStep, txId);

        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, srcPathId, t2);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, dstPathId, t2);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, srcPathId, g0Snapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, srcPathId, g1Snapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 20);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, srcPathId, t1);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(!reader.IsError());
        }
    }

    Y_UNIT_TEST(TruncateSourceAfterDropCopySucceeds) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csDefaultControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 srcPathId = 1;
        const ui64 dstPathId = 2;
        TestTableDescription testTable{};
        auto planStep = PrepareTablet(runtime, srcPathId, testTable.Schema);

        ui64 txId = 10;
        int writeId = 10;
        {
            std::vector<ui64> writeIds;
            UNIT_ASSERT(
                WriteData(runtime, sender, writeId++, srcPathId, MakeTestBlob({ 0, 100 }, testTable.Schema), testTable.Schema, true, &writeIds));
            planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
            PlanCommit(runtime, sender, planStep, txId);
        }
        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::CopyTableTxBody(srcPathId, dstPathId, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::DropTableTxBody(dstPathId, 2), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(srcPathId, 3), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });

        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, srcPathId, NOlap::TSnapshot(planStep, txId));
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, dstPathId, NOlap::TSnapshot(planStep, txId));
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(reader.IsError());
        }
    }

    Y_UNIT_TEST(TruncateSeqNoCheck) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csDefaultControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 pathId = 1;
        TestTableDescription testTable{};
        auto planStep = PrepareTablet(runtime, pathId, testTable.Schema);

        ui64 txId = 10;
        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(pathId, 5), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        ProposeSchemaTxFail(runtime, sender, TTestSchema::TruncateTableTxBody(pathId, 3), ++txId);
        ProposeSchemaTxFail(runtime, sender, TTestSchema::DropTableTxBody(pathId, 4), ++txId);
        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(pathId, 6), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
    }

    Y_UNIT_TEST_DUO(TruncateWithCommitInProgress, Reboot) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csDefaultControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 pathId = 1;
        TestTableDescription testTable{};
        Y_UNUSED(PrepareTablet(runtime, pathId, testTable.Schema));

        ui64 txId = 10;
        int writeId = 10;

        std::vector<ui64> writeIds;
        UNIT_ASSERT(
            WriteData(runtime, sender, writeId++, pathId, MakeTestBlob({ 0, 100 }, testTable.Schema), testTable.Schema, true, &writeIds));
        const auto commitTxId = ++txId;
        const auto commitPlanStep = ProposeCommit(runtime, sender, commitTxId, writeIds);

        const auto truncateTxId = ++txId;
        {
            auto event = std::make_unique<TEvColumnShard::TEvProposeTransaction>(
                NKikimrTxColumnShard::TX_KIND_SCHEMA, 0, sender, truncateTxId, TTestSchema::TruncateTableTxBody(pathId, 1), 0, 0);
            ForwardToTablet(runtime, TTestTxConfig::TxTablet0, sender, event.release());
        }

        runtime.SimulateSleep(TDuration::MilliSeconds(100));
        if (Reboot) {
            RebootTablet(runtime, TTestTxConfig::TxTablet0, sender);
        }

        PlanCommit(runtime, sender, commitPlanStep, commitTxId);

        runtime.SimulateSleep(TDuration::MilliSeconds(100));
        if (Reboot) {
            RebootTablet(runtime, TTestTxConfig::TxTablet0, sender);
        }

        auto ev = runtime.GrabEdgeEvent<TEvColumnShard::TEvProposeTransactionResult>(sender);
        UNIT_ASSERT(ev);
        const auto& res = ev->Get()->Record;
        UNIT_ASSERT_VALUES_EQUAL(res.GetTxId(), truncateTxId);
        UNIT_ASSERT_EQUAL(res.GetTxKind(), NKikimrTxColumnShard::TX_KIND_SCHEMA);
        UNIT_ASSERT_EQUAL(res.GetStatus(), NKikimrTxColumnShard::PREPARED);
        const auto truncatePlanStep = TPlanStep{ res.GetMinStep() };
        UNIT_ASSERT(commitPlanStep.Val() < truncatePlanStep.Val());

        runtime.SimulateSleep(TDuration::MilliSeconds(100));
        if (Reboot) {
            RebootTablet(runtime, TTestTxConfig::TxTablet0, sender);
        }

        PlanSchemaTx(runtime, sender, { truncatePlanStep, truncateTxId });
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, NOlap::TSnapshot(truncatePlanStep, truncateTxId));
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(!reader.IsError());
        }

        {
            std::vector<ui64> writeIds;
            UNIT_ASSERT(
                WriteData(runtime, sender, writeId++, pathId, MakeTestBlob({ 200, 250 }, testTable.Schema), testTable.Schema, true, &writeIds));
            auto planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
            PlanCommit(runtime, sender, planStep, txId);
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, NOlap::TSnapshot(planStep, txId));
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 50);
            UNIT_ASSERT(!reader.IsError());
        }
    }

    // Completing an unrelated schema tx on another path must not unblock TRUNCATE's TWaitTxs.
    Y_UNIT_TEST(TruncateWaitTxsIgnoresUnrelatedTxCompleted) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csDefaultControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 pathId = 1;
        TestTableDescription testTable{};
        Y_UNUSED(PrepareTablet(runtime, pathId, testTable.Schema, 1, testTable.Standalone));

        ui64 txId = 10;
        int writeId = 10;
        std::vector<ui64> writeIds;
        const auto lockId = 1;
        UNIT_ASSERT(WriteData(runtime, sender, writeId++, pathId, MakeTestBlob({ 0, 100 }, testTable.Schema), testTable.Schema, true, &writeIds,
            NEvWrite::EModificationType::Upsert, lockId));
        const auto commitTxId = ++txId;
        const auto commitPlanStep = ProposeCommit(runtime, sender, commitTxId, writeIds, lockId);

        const auto truncateTxId = ++txId;
        {
            auto event = std::make_unique<TEvColumnShard::TEvProposeTransaction>(
                NKikimrTxColumnShard::TX_KIND_SCHEMA, 0, sender, truncateTxId, TTestSchema::TruncateTableTxBody(pathId, 1), 0, 0);
            ForwardToTablet(runtime, TTestTxConfig::TxTablet0, sender, event.release());
        }
        runtime.SimulateSleep(TDuration::MilliSeconds(100));

        TPlanStep lastPlanStep = commitPlanStep;
        {
            constexpr ui64 auxPathId = 99;
            NKikimrTxColumnShard::TSchemaTxBody auxTx;
            Y_ABORT_UNLESS(
                auxTx.ParseFromString(TTestSchema::CreateTableTxBody(auxPathId, testTable.Standalone, testTable.Schema, testTable.Pk)));
            auxTx.MutableSeqNo()->SetRound(2);
            TString auxTxBody;
            Y_PROTOBUF_SUPPRESS_NODISCARD auxTx.SerializeToString(&auxTxBody);
            const auto auxPlan = ProposeSchemaTx(runtime, sender, auxTxBody, ++txId);
            PlanSchemaTx(runtime, sender, { auxPlan, txId });
            lastPlanStep = auxPlan;
        }

        PlanCommit(runtime, sender, TPlanStep{ lastPlanStep.Val() + 1 }, commitTxId);

        auto ev = runtime.GrabEdgeEvent<TEvColumnShard::TEvProposeTransactionResult>(sender);
        UNIT_ASSERT(ev);
        const auto& res = ev->Get()->Record;
        UNIT_ASSERT_VALUES_EQUAL(res.GetTxId(), truncateTxId);
        UNIT_ASSERT_EQUAL(res.GetStatus(), NKikimrTxColumnShard::PREPARED);
        // Plan at MaxStep, not MinStep: MinStep is frozen at propose-start time, but while TRUNCATE
        // waits in TWaitTxs the test advances the plan step (the aux CreateTable tx and the in-flight
        // commit are planned at auxPlan/auxPlan+1), pushing LastPlannedStep past MinStep. A plan at
        // MinStep would be silently dropped by TTxPlanStep ("Ignore old txIds") and the test would hang.
        const auto truncatePlanStep = TPlanStep{ res.GetMaxStep() };
        PlanSchemaTx(runtime, sender, { truncatePlanStep, truncateTxId });
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, NOlap::TSnapshot(truncatePlanStep, truncateTxId));
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(!reader.IsError());
        }
    }

    Y_UNIT_TEST(CommitOldLockAfterTruncatePlanIsRejected) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csDefaultControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 srcPathId = 1;
        const ui64 copyPathId = 2;
        const ui64 lockId = 3;
        TestTableDescription testTable{};
        Y_UNUSED(PrepareTablet(runtime, srcPathId, testTable.Schema));

        ui64 txId = 10;
        int writeId = 10;
        std::vector<ui64> committedWriteIds;
        UNIT_ASSERT(WriteData(
            runtime, sender, writeId++, srcPathId, MakeTestBlob({ 0, 100 }, testTable.Schema), testTable.Schema, true, &committedWriteIds));
        auto planStep = ProposeCommit(runtime, sender, ++txId, committedWriteIds);
        PlanCommit(runtime, sender, planStep, txId);

        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::CopyTableTxBody(srcPathId, copyPathId, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        const auto snapshotBeforeTruncate = NOlap::TSnapshot(planStep, txId);

        std::vector<ui64> lockedWriteIds;
        UNIT_ASSERT(WriteData(runtime, sender, writeId++, srcPathId, MakeTestBlob({ 100, 150 }, testTable.Schema), testTable.Schema, true,
            &lockedWriteIds, NEvWrite::EModificationType::Upsert, lockId));

        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(srcPathId, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        const auto truncateSnapshot = NOlap::TSnapshot(planStep, txId);

        // The table identity is unchanged, but truncate must break the lock of a write
        // accepted before its plan step.
        ProposeCommitFail(runtime, sender, TTestTxConfig::TxTablet0, ++txId, lockedWriteIds, lockId);

        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, srcPathId, snapshotBeforeTruncate);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, srcPathId, truncateSnapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            UNIT_ASSERT(!reader.ReadAll());
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, copyPathId, truncateSnapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }
    }

    Y_UNIT_TEST_DUO(TruncatePausesNewCompactions, Reboot) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto controller = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<NOlap::TWaitCompactionController>();
        controller->DisableBackground(NKikimr::NYDBTest::ICSController::EBackground::Compaction);
        controller->DisableBackground(NKikimr::NYDBTest::ICSController::EBackground::Cleanup);
        TActorId sender = runtime.AllocateEdgeActor();
        TestTableDescription table{};
        Y_UNUSED(PrepareTablet(runtime, 1, table.Schema));
        ui64 txId = 10;
        int writeId = 10;
        auto write = [&] {
            std::vector<ui64> ids;
            UNIT_ASSERT(WriteData(runtime, sender, writeId++, 1, MakeTestBlob({ 0, 100 }, table.Schema), table.Schema, true, &ids));
            const auto step = ProposeCommit(runtime, sender, ++txId, ids);
            PlanCommit(runtime, sender, step, txId);
        };
        for (ui32 i = 0; i < 4; ++i) {
            write();
        }
        const auto internalPathId =
            *WaitForShard(*controller.operator->(), runtime)->GetTablesManager().ResolveInternalPathId(TSchemeShardLocalPathId::FromRawValue(1),
                false);
        auto priority = [&] {
            const auto& granule = WaitForShard(*controller.operator->(), runtime)
                                      ->GetTablesManager()
                                      .GetPrimaryIndexAsVerified<NOlap::TColumnEngineForLogs>()
                                      .GetGranuleVerified(internalPathId);
            granule.ActualizeOptimizer(runtime.GetCurrentTime(), TDuration::Zero());
            return granule.GetCompactionPriority();
        };
        runtime.SimulateSleep(TDuration::Seconds(1));
        UNIT_ASSERT(!priority().IsZero());
        const auto truncateTxId = ++txId;
        const auto step = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(1, 1), truncateTxId);
        UNIT_ASSERT(priority().IsZero());
        if (Reboot) {
            RebootTablet(runtime, TTestTxConfig::TxTablet0, sender);
            UNIT_ASSERT(priority().IsZero());
        }
        controller->EnableBackground(NKikimr::NYDBTest::ICSController::EBackground::Compaction);
        const auto startedBefore = controller->GetCompactionStartedCounter().Val();
        for (ui32 i = 0; i < 3; ++i) {
            Wakeup(runtime, sender, TTestTxConfig::TxTablet0);
            runtime.SimulateSleep(TDuration::Seconds(1));
        }
        UNIT_ASSERT_VALUES_EQUAL(controller->GetCompactionStartedCounter().Val(), startedBefore);
        controller->DisableBackground(NKikimr::NYDBTest::ICSController::EBackground::Compaction);
        PlanSchemaTx(runtime, sender, { step, truncateTxId });
        for (ui32 i = 0; i < 4; ++i) {
            write();
        }
        runtime.SimulateSleep(TDuration::Seconds(1));
        UNIT_ASSERT(!priority().IsZero());
    }

    Y_UNIT_TEST_DUO(CompactionPublishedAfterTruncate, Reboot) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto controller = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<NOlap::TWaitCompactionController>();
        controller->DisableBackground(NKikimr::NYDBTest::ICSController::EBackground::Cleanup);
        controller->DisableBackground(NKikimr::NYDBTest::ICSController::EBackground::Compaction);
        TActorId sender = runtime.AllocateEdgeActor();
        TestTableDescription table{};
        Y_UNUSED(PrepareTablet(runtime, 1, table.Schema));
        ui64 txId = 10;
        int writeId = 10;
        auto write = [&](ui64 from, ui64 to) {
            std::vector<ui64> ids;
            UNIT_ASSERT(WriteData(runtime, sender, writeId++, 1, MakeTestBlob({ from, to }, table.Schema), table.Schema, true, &ids));
            auto step = ProposeCommit(runtime, sender, ++txId, ids);
            PlanCommit(runtime, sender, step, txId);
            return NOlap::TSnapshot(step, txId);
        };
        for (ui32 i = 0; i < 4; ++i) {
            write(0, 100);
        }
        auto step = ProposeSchemaTx(runtime, sender, TTestSchema::CopyTableTxBody(1, 2, 1), ++txId);
        PlanSchemaTx(runtime, sender, { step, txId });
        const auto beforeTruncate = NOlap::TSnapshot(step, txId);

        std::vector<TAutoPtr<IEventHandle>> delayedCompactions;
        runtime.SetEventFilter([&](TTestActorRuntimeBase&, TAutoPtr<IEventHandle>& ev) {
            if (auto* msg = TryGetPrivateEvent<NColumnShard::TEvPrivate::TEvWriteIndex>(ev)) {
                if (msg->GetPutStatus() == NKikimrProto::OK &&
                    std::dynamic_pointer_cast<NOlap::TCompactColumnEngineChanges>(msg->IndexChanges)) {
                    delayedCompactions.emplace_back(ev.Release());
                    return true;
                }
            }
            return false;
        });
        controller->EnableBackground(NKikimr::NYDBTest::ICSController::EBackground::Compaction);
        for (ui32 i = 0; delayedCompactions.empty() && i < 30; ++i) {
            Wakeup(runtime, sender, TTestTxConfig::TxTablet0);
            runtime.SimulateSleep(TDuration::Seconds(1));
        }
        UNIT_ASSERT_C(!delayedCompactions.empty(), "compaction must be ready to publish before truncate");
        controller->DisableBackground(NKikimr::NYDBTest::ICSController::EBackground::Compaction);
        step = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(1, 2), ++txId);
        PlanSchemaTx(runtime, sender, { step, txId });
        const auto afterWrite = write(1000, 1020);

        const auto finishedBefore = controller->GetCompactionFinishedCounter().Val();
        const intptr_t delayedCount = delayedCompactions.size();
        runtime.SetEventFilter([](TTestActorRuntimeBase&, TAutoPtr<IEventHandle>&) {
            return false;
        });
        for (auto& ev : delayedCompactions) {
            runtime.Send(ev.Release());
        }
        for (ui32 i = 0; controller->GetCompactionFinishedCounter().Val() < finishedBefore + delayedCount && i < 30; ++i) {
            runtime.SimulateSleep(TDuration::Seconds(1));
        }
        UNIT_ASSERT_VALUES_EQUAL(controller->GetCompactionFinishedCounter().Val(), finishedBefore + delayedCount);
        auto checkRows = [&](ui64 pathId, const NOlap::TSnapshot& snapshot, ui64 count) {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, snapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(table.Schema));
            auto rows = reader.ReadAll();
            UNIT_ASSERT(!reader.IsError());
            UNIT_ASSERT_VALUES_EQUAL(rows ? rows->num_rows() : 0, count);
        };
        checkRows(1, beforeTruncate, 100);
        checkRows(1, afterWrite, 20);
        checkRows(2, afterWrite, 100);

        step = ProposeSchemaTx(runtime, sender, TTestSchema::CopyTableTxBody(1, 3, 3), ++txId);
        PlanSchemaTx(runtime, sender, { step, txId });
        step = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(1, 4), ++txId);
        PlanSchemaTx(runtime, sender, { step, txId });
        const auto afterSecondTruncate = NOlap::TSnapshot(step, txId);
        if (Reboot) {
            RebootTablet(runtime, TTestTxConfig::TxTablet0, sender);
        }
        checkRows(1, beforeTruncate, 100);
        checkRows(1, afterWrite, 20);
        checkRows(1, afterSecondTruncate, 0);
        checkRows(2, afterSecondTruncate, 100);
        checkRows(3, afterSecondTruncate, 20);
    }

    Y_UNIT_TEST(TruncateBreaksSourceReadLocks) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto controller = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        controller->DisableBackground(NKikimr::NYDBTest::ICSController::EBackground::Cleanup);
        TActorId sender = runtime.AllocateEdgeActor();
        TestTableDescription table{};
        Y_UNUSED(PrepareTablet(runtime, 1, table.Schema));
        std::vector<ui64> ids;
        UNIT_ASSERT(WriteData(runtime, sender, 10, 1, MakeTestBlob({ 0, 100 }, table.Schema), table.Schema, true, &ids));
        auto step = ProposeCommit(runtime, sender, 11, ids);
        PlanCommit(runtime, sender, step, 11);
        step = ProposeSchemaTx(runtime, sender, TTestSchema::CopyTableTxBody(1, 2, 1), 12);
        PlanSchemaTx(runtime, sender, { step, 12 });
        const auto readSnapshot = NOlap::TSnapshot(step, 12);

        ui64 scanLockId = 0;
        runtime.SetEventFilter([&](TTestActorRuntimeBase&, TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() == TEvDataShard::TEvKqpScan::EventType && scanLockId) {
                auto& record = ev->Get<TEvDataShard::TEvKqpScan>()->Record;
                record.SetLockTxId(scanLockId);
                record.SetLockMode(NKikimrDataEvents::OPTIMISTIC);
            }
            return false;
        });
        auto readWithLock = [&](ui64 pathId, ui64 lockId) {
            scanLockId = lockId;
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, readSnapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(table.Schema));
            UNIT_ASSERT(reader.ReadAll());
            UNIT_ASSERT(!reader.IsError());
            scanLockId = 0;
        };
        auto isBroken = [&](ui64 lockId) {
            auto* lock = WaitForShard(*controller.operator->(), runtime)->GetOperationsManager().GetLockOptional(lockId);
            UNIT_ASSERT(lock);
            return lock->IsBroken();
        };
        readWithLock(1, 101);
        readWithLock(2, 102);
        UNIT_ASSERT(!isBroken(101));
        UNIT_ASSERT(!isBroken(102));
        step = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(1, 2), 13);
        PlanSchemaTx(runtime, sender, { step, 13 });
        UNIT_ASSERT(isBroken(101));
        UNIT_ASSERT(!isBroken(102));

        // A late read of the source's old snapshot must also conflict with truncate.
        readWithLock(1, 103);
        readWithLock(2, 104);
        UNIT_ASSERT(isBroken(103));
        UNIT_ASSERT(!isBroken(104));
    }

    Y_UNIT_TEST(CommitIsRejectedAfterAbortWasQueued) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 pathId = 1;
        const ui64 lockId = 3;
        const ui64 commitTxId = 13;
        TestTableDescription testTable{};
        Y_UNUSED(PrepareTablet(runtime, pathId, testTable.Schema));

        int writeId = 10;
        std::vector<ui64> lockedWriteIds;
        UNIT_ASSERT(WriteData(runtime, sender, writeId++, pathId, MakeTestBlob({ 0, 100 }, testTable.Schema), testTable.Schema, true,
            &lockedWriteIds, NEvWrite::EModificationType::Upsert, lockId));

        const auto createTableTxBody = [&](const ui64 auxPathId, const ui32 round) {
            NKikimrTxColumnShard::TSchemaTxBody auxTx;
            UNIT_ASSERT(auxTx.ParseFromString(TTestSchema::CreateTableTxBody(auxPathId, testTable.Standalone, testTable.Schema, testTable.Pk)));
            auxTx.MutableSeqNo()->SetRound(round);
            TString body;
            Y_PROTOBUF_SUPPRESS_NODISCARD auxTx.SerializeToString(&body);
            return body;
        };
        // The response to a schema proposal is sent after the earlier write finishes in the executor.
        const auto barrierPlanStep = ProposeSchemaTx(runtime, sender, createTableTxBody(98, 2), 11);
        PlanSchemaTx(runtime, sender, { barrierPlanStep, 11 });

        auto* shard = WaitForShard(*csControllerGuard.operator->(), runtime);
        auto* lockInfo = shard->GetOperationsManager().GetLockOptional(lockId);
        UNIT_ASSERT(lockInfo);
        // Reproduce the state after rollback has queued TAbortWriteTransaction, but before it
        // executes. The commit proposal must not assign a TxId to this lock.
        lockInfo->SetNeedsAborting();
        UNIT_ASSERT(lockInfo->ReadyForAborting());
        lockInfo->SetAborting();

        auto commit = std::make_unique<NEvents::TDataEvents::TEvWrite>(commitTxId, NKikimrDataEvents::TEvWrite::MODE_PREPARE);
        commit->Record.MutableLocks()->AddLocks()->SetLockId(lockId);
        commit->Record.MutableLocks()->SetOp(NKikimrDataEvents::TKqpLocks::Commit);
        ForwardToTablet(runtime, TTestTxConfig::TxTablet0, sender, commit.release());
        auto commitResult = runtime.GrabEdgeEvent<NEvents::TDataEvents::TEvWriteResult>(sender);
        UNIT_ASSERT(commitResult);
        UNIT_ASSERT_VALUES_EQUAL(commitResult->Get()->Record.GetTxId(), commitTxId);
        UNIT_ASSERT_VALUES_EQUAL(commitResult->Get()->Record.GetStatus(), NKikimrDataEvents::TEvWriteResult::STATUS_LOCKS_BROKEN);
        UNIT_ASSERT(!lockInfo->IsTxIdAssigned());

        std::vector<ui64> nextWriteIds;
        UNIT_ASSERT(
            WriteData(runtime, sender, writeId++, pathId, MakeTestBlob({ 100, 150 }, testTable.Schema), testTable.Schema, true, &nextWriteIds));
    }

    // Path fence on TRUNCATE propose: uncommitted writes, new writes, and CommitWriteLock for a
    // lock that wrote before the fence must fail; after plan the table is empty.
    Y_UNIT_TEST(TruncateFencesWritesOnPropose) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csDefaultControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 pathId = 1;
        TestTableDescription testTable{};
        Y_UNUSED(PrepareTablet(runtime, pathId, testTable.Schema));

        ui64 txId = 10;
        int writeId = 10;

        std::vector<ui64> uncommittedWriteIds;
        UNIT_ASSERT(WriteData(
            runtime, sender, writeId++, pathId, MakeTestBlob({ 0, 50 }, testTable.Schema), testTable.Schema, true, &uncommittedWriteIds));

        std::vector<ui64> writeIdsBefore;
        const auto lockBefore = 1;
        UNIT_ASSERT(WriteData(runtime, sender, writeId++, pathId, MakeTestBlob({ 50, 100 }, testTable.Schema), testTable.Schema, true,
            &writeIdsBefore, NEvWrite::EModificationType::Upsert, lockBefore));

        const auto truncateTxId = ++txId;
        {
            auto event = std::make_unique<TEvColumnShard::TEvProposeTransaction>(
                NKikimrTxColumnShard::TX_KIND_SCHEMA, 0, sender, truncateTxId, TTestSchema::TruncateTableTxBody(pathId, 1), 0, 0);
            ForwardToTablet(runtime, TTestTxConfig::TxTablet0, sender, event.release());
        }
        runtime.SimulateSleep(TDuration::MilliSeconds(50));

        {
            std::vector<ui64> writeIdsAfter;
            UNIT_ASSERT(!WriteData(
                runtime, sender, writeId++, pathId, MakeTestBlob({ 100, 150 }, testTable.Schema), testTable.Schema, true, &writeIdsAfter));
        }
        ProposeCommitFail(runtime, sender, TTestTxConfig::TxTablet0, ++txId, writeIdsBefore, lockBefore);

        auto ev = runtime.GrabEdgeEvent<TEvColumnShard::TEvProposeTransactionResult>(sender);
        UNIT_ASSERT(ev);
        const auto& res = ev->Get()->Record;
        UNIT_ASSERT_VALUES_EQUAL(res.GetTxId(), truncateTxId);
        UNIT_ASSERT_EQUAL(res.GetStatus(), NKikimrTxColumnShard::PREPARED);
        const auto planStep = TPlanStep{ res.GetMinStep() };
        PlanSchemaTx(runtime, sender, { planStep, truncateTxId });
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, NOlap::TSnapshot(planStep, truncateTxId));
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(!reader.IsError());
        }
    }

    Y_UNIT_TEST(TruncateInStoreTableFails) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csDefaultControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 pathId = 1;
        TestTableDescription testTable{};
        Y_UNUSED(PrepareTablet(runtime, pathId, testTable.Schema, 1, false));
        ui64 txId = 10;
        ProposeSchemaTxFail(runtime, sender, TTestSchema::TruncateTableTxBody(pathId, 1), ++txId);
    }

    Y_UNIT_TEST(TruncateSurvivesRestart) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csDefaultControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 pathId = 1;
        TestTableDescription testTable{};
        auto planStep = PrepareTablet(runtime, pathId, testTable.Schema);

        ui64 txId = 10;
        int writeId = 1;
        {
            std::vector<ui64> writeIds;
            UNIT_ASSERT(
                WriteData(runtime, sender, writeId++, pathId, MakeTestBlob({ 0, 50 }, testTable.Schema), testTable.Schema, true, &writeIds));
            planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
            PlanCommit(runtime, sender, planStep, txId);
        }

        const auto truncateTxId = ++txId;
        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(pathId, 1), truncateTxId);
        RebootTablet(runtime, TTestTxConfig::TxTablet0, sender);
        PlanSchemaTx(runtime, sender, { planStep, truncateTxId });
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, NOlap::TSnapshot(planStep, truncateTxId));
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(!reader.IsError());
        }
    }

    Y_UNIT_TEST(TruncateRestartAfterPlan) {
        TTestBasicRuntime runtime;
        TTester::Setup(runtime);
        auto csDefaultControllerGuard = NKikimr::NYDBTest::TControllers::RegisterCSControllerGuard<TDefaultTestsController>();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 pathId = 1;
        TestTableDescription testTable{};
        auto planStep = PrepareTablet(runtime, pathId, testTable.Schema);

        ui64 txId = 10;
        int writeId = 10;
        {
            std::vector<ui64> writeIds;
            UNIT_ASSERT(
                WriteData(runtime, sender, writeId++, pathId, MakeTestBlob({ 0, 100 }, testTable.Schema), testTable.Schema, true, &writeIds));
            planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
            PlanCommit(runtime, sender, planStep, txId);
        }
        const auto snapshotBeforeTruncate = NOlap::TSnapshot(planStep, txId);

        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(pathId, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        const auto truncateSnapshot = NOlap::TSnapshot(planStep, txId);
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, truncateSnapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(!reader.IsError());
        }

        RebootTablet(runtime, TTestTxConfig::TxTablet0, sender);

        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, truncateSnapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, snapshotBeforeTruncate);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }
        {
            std::vector<ui64> writeIds;
            UNIT_ASSERT(
                WriteData(runtime, sender, writeId++, pathId, MakeTestBlob({ 200, 250 }, testTable.Schema), testTable.Schema, true, &writeIds));
            planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
            PlanCommit(runtime, sender, planStep, txId);
        }
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, NOlap::TSnapshot(planStep, txId));
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 50);
            UNIT_ASSERT(!reader.IsError());
        }
    }

    // Cleanup removes truncated portions without deleting the live table. Time-travel
    // works until retention expires.
    Y_UNIT_TEST(TruncateCleanupPreservesTable) {
        TTestBasicRuntime runtime;
        SetupTruncateTestRuntime(runtime);
        auto csControllerGuard = RegisterTruncateTestController();
        auto& csController = *csControllerGuard.operator->();
        csControllerGuard->SetOverridePeriodicWakeupActivationPeriod(TDuration::Seconds(1));
        TActorId sender = runtime.AllocateEdgeActor();

        const ui64 pathId = 1;
        TestTableDescription testTable{};
        auto planStep = PrepareTablet(runtime, pathId, testTable.Schema);

        ui64 txId = 10;
        int writeId = 10;
        {
            std::vector<ui64> writeIds;
            UNIT_ASSERT(
                WriteData(runtime, sender, writeId++, pathId, MakeTestBlob({ 0, 100 }, testTable.Schema), testTable.Schema, true, &writeIds));
            planStep = ProposeCommit(runtime, sender, ++txId, writeIds);
            PlanCommit(runtime, sender, planStep, txId);
        }
        const auto snapshotBeforeTruncate = NOlap::TSnapshot(planStep, txId);

        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::TruncateTableTxBody(pathId, 1), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        const auto truncateSnapshot = NOlap::TSnapshot(planStep, txId);

        const auto* shard = WaitForShard(csController, runtime);
        UNIT_ASSERT(shard);

        const auto newInternalPathId = shard->GetTablesManager().ResolveInternalPathId(TSchemeShardLocalPathId::FromRawValue(pathId), false);
        UNIT_ASSERT(newInternalPathId);
        UNIT_ASSERT_VALUES_EQUAL(shard->GetTablesManager().GetTables().size(), 1);
        AssertPathsToDropState(*shard, *newInternalPathId, false);
        UNIT_ASSERT(HasPortionsRemovedAt(*shard, *newInternalPathId, truncateSnapshot));
        {
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, snapshotBeforeTruncate);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(rb);
            UNIT_ASSERT_VALUES_EQUAL(rb->num_rows(), 100);
            UNIT_ASSERT(!reader.IsError());
        }

        // Expire the time-travel snapshot and enable cleanup of truncated portions.
        csControllerGuard->SetOverrideUsedSnapshotLivetime(TDuration::Zero());
        csControllerGuard->EnableBackground(NKikimr::NYDBTest::ICSController::EBackground::Cleanup);
        auto advancePlanStep = [&] {
            AdvanceShardPlanStep(runtime, sender, txId, writeId, pathId, testTable);
        };
        UNIT_ASSERT(WaitForTruncatedPortionsCleanup(csController, runtime, sender, *newInternalPathId, truncateSnapshot, advancePlanStep));

        {
            const auto* finalizedShard = csController.GetAnyShard();
            UNIT_ASSERT(finalizedShard);
            const auto& tables = finalizedShard->GetTablesManager().GetTables();
            UNIT_ASSERT_VALUES_EQUAL(tables.size(), 1);
            UNIT_ASSERT(tables.contains(*newInternalPathId));
            UNIT_ASSERT(!HasPortionsRemovedAt(*finalizedShard, *newInternalPathId, truncateSnapshot));
        }
        {
            // GC advanced the read-staleness floor past the truncate snapshot, so this read is rejected.
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, truncateSnapshot);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(reader.IsError());
        }
        {
            // Likewise, snapshotBeforeTruncate is below the floor after GC removed truncated portions.
            TShardReader reader(runtime, TTestTxConfig::TxTablet0, pathId, snapshotBeforeTruncate);
            reader.SetReplyColumnIds(TTestSchema::ExtractIds(testTable.Schema));
            auto rb = reader.ReadAll();
            UNIT_ASSERT(!rb);
            UNIT_ASSERT(reader.IsError());
        }

        // Final DROP must remove the durable truncate history together with the table.
        planStep = ProposeSchemaTx(runtime, sender, TTestSchema::DropTableTxBody(pathId, 2), ++txId);
        PlanSchemaTx(runtime, sender, { planStep, txId });
        ui64 nextPlanStep = planStep.Val() + 10000;
        const auto deadline = TInstant::Now() + TDuration::Seconds(60);
        while (csController.GetShard()->GetTablesManager().HasTable(*newInternalPathId, true) && TInstant::Now() < deadline) {
            PlanCommit(runtime, sender, TPlanStep{ nextPlanStep }, TSet<ui64>{});
            nextPlanStep += 10000;
            Wakeup(runtime, sender, TTestTxConfig::TxTablet0);
            ForwardToTablet(runtime, TTestTxConfig::TxTablet0, sender, new NColumnShard::TEvPrivate::TEvPingSnapshotsUsage());
            runtime.SimulateSleep(TDuration::Seconds(1));
            Y_UNUSED(csController.WaitCleaning(TDuration::Seconds(1), &runtime));
        }
        UNIT_ASSERT(!csController.GetShard()->GetTablesManager().HasTable(*newInternalPathId, true));
        RebootTablet(runtime, TTestTxConfig::TxTablet0, sender);
        UNIT_ASSERT(WaitForShard(csController, runtime)->GetTablesManager().GetTables().empty());
    }
}
}   // namespace NKikimr
