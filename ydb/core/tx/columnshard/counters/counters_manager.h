#pragma once

#include "background_controller.h"
#include "column_tables.h"
#include "columnshard.h"
#include "indexation.h"
#include "portion_index.h"
#include "req_tracer.h"
#include "scan.h"
#include "tablet_counters.h"
#include "writes_monitor.h"

#include <ydb/core/base/appdata_fwd.h>
#include <ydb/core/protos/counters_columnshard.pb.h>
#include <ydb/core/protos/counters_datashard.pb.h>
#include <ydb/core/protos/table_stats.pb.h>
#include <ydb/core/tablet/tablet_counters.h>
#include <ydb/core/tablet_flat/tablet_flat_executor.h>
#include <ydb/core/tx/columnshard/common/path_id.h>
#include <ydb/core/tx/columnshard/counters/duplicate_filtering.h>
#include <ydb/core/tx/columnshard/engines/column_engine.h>

#include <library/cpp/time_provider/time_provider.h>

namespace NKikimr::NColumnShard {

class TCountersManager {
private:
    YDB_READONLY_DEF(std::shared_ptr<const TTabletCountersHandle>, TabletCounters);
    YDB_READONLY_DEF(std::shared_ptr<TWritesMonitor>, WritesMonitor);

    YDB_READONLY_DEF(std::shared_ptr<TBackgroundControllerCounters>, BackgroundControllerCounters);
    YDB_READONLY_DEF(std::shared_ptr<TColumnTablesCounters>, ColumnTablesCounters);
    YDB_READONLY_DEF(std::shared_ptr<TPortionIndexStats>, PortionIndexCounters);

    YDB_READONLY(TCSCounters, CSCounters, TCSCounters());
    YDB_READONLY(TIndexationCounters, EvictionCounters, TIndexationCounters("Eviction"));
    YDB_READONLY(TIndexationCounters, IndexationCounters, TIndexationCounters("Indexation"));
    YDB_READONLY(TIndexationCounters, CompactionCounters, TIndexationCounters("GeneralCompaction"));
    YDB_READONLY(TScanCounters, ScanCounters, TScanCounters("Scan"));
    YDB_READONLY_DEF(TDuplicateFilteringCounters, DuplicateFilteringCounters);
    YDB_READONLY_DEF(std::shared_ptr<TRequestsTracerCounters>, RequestsTracingCounters);
    YDB_READONLY_DEF(std::shared_ptr<NOlap::NResourceBroker::NSubscribe::TSubscriberCounters>, SubscribeCounters);

public:
    TCountersManager(TTabletCountersBase& tabletCounters)
        : TabletCounters(std::make_shared<const TTabletCountersHandle>(tabletCounters))
        , WritesMonitor(std::make_shared<TWritesMonitor>(tabletCounters))
        , BackgroundControllerCounters(std::make_shared<TBackgroundControllerCounters>())
        , ColumnTablesCounters(std::make_shared<TColumnTablesCounters>())
        , PortionIndexCounters(std::make_shared<TPortionIndexStats>())
        , RequestsTracingCounters(std::make_shared<TRequestsTracerCounters>())
        , SubscribeCounters(std::make_shared<NOlap::NResourceBroker::NSubscribe::TSubscriberCounters>()) {
    }

    void OnWriteOverloadDisk() const {
        TabletCounters->IncCounter(COUNTER_OUT_OF_SPACE);
    }

    void OnWriteOverloadMetadata(const ui64 size) const {
        TabletCounters->IncCounter(COUNTER_WRITE_OVERLOAD);
        CSCounters.OnWriteOverloadMetadata(size);
    }

    void OnWriteOverloadCompaction(const ui64 size) const {
        CSCounters.OnWriteOverloadCompaction(size);
    }

    void OnWriteOverloadShardTx(const ui64 size) const {
        TabletCounters->IncCounter(COUNTER_WRITE_OVERLOAD);
        CSCounters.OnWriteOverloadShardTx(size);
    }

    void OnWriteOverloadShardWrites(const ui64 size) const {
        TabletCounters->IncCounter(COUNTER_WRITE_OVERLOAD);
        CSCounters.OnWriteOverloadShardWrites(size);
    }

    void OnWriteOverloadShardWritesSize(const ui64 size) const {
        TabletCounters->IncCounter(COUNTER_WRITE_OVERLOAD);
        CSCounters.OnWriteOverloadShardWritesSize(size);
    }

    void FillTableStats(TInternalPathId pathId, ::NKikimrTableStats::TTableStats& tableStats) {
        ColumnTablesCounters->GetPathIdCounter(pathId)->FillStats(tableStats);
        BackgroundControllerCounters->FillStats(pathId, tableStats);
    }

    void FillTotalTableStats(::NKikimrTableStats::TTableStats& tableStats) {
        ColumnTablesCounters->FillStats(tableStats);
        TabletCounters->FillStats(tableStats);
        BackgroundControllerCounters->FillTotalStats(tableStats);
        ScanCounters.FillStats(tableStats);
    }

    void OnWritePutBlobsSuccess(const TDuration d, const ui64 rowsWritten) const {
        TabletCounters->OnWritePutBlobsSuccess(rowsWritten);
        CSCounters.OnWritePutBlobsSuccess(d);
    }

    void OnWritePutBulkBlobsSuccess(const TDuration d, const ui64 rowsWritten) const {
        TabletCounters->OnWritePutBulkBlobsSuccess(rowsWritten);
        CSCounters.OnWritePutBlobsSuccess(d);
    }

    void OnWritePutBlobsFailed(const TDuration d, const ui64 /*rowsWritten*/) const {
        TabletCounters->OnWriteFailure();
        CSCounters.OnWritePutBlobsFail(d);
    }

    void OnWritePutBulkBlobsFailed(const TDuration d, const ui64 /*rowsWritten*/) const {
        TabletCounters->OnWriteFailure();
        CSCounters.OnWritePutBlobsFail(d);
    }
};

}   // namespace NKikimr::NColumnShard
