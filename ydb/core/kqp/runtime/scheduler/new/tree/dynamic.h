#pragma once

#include "snapshot.h"

#include <ydb/core/kqp/counters/kqp_counters.h>

#include <util/datetime/base.h>

#include <atomic>
#include <vector>

namespace NKikimr::NKqp::NScheduler::NHdrf::NDynamic {

    template <class TSnapshotPtr>
    class TSnapshotSwitch {
        public:
            // returns previous snapshot
            TSnapshotPtr SetSnapshot(const TSnapshotPtr& snapshot) {
                ui8 oldSnapshotIdx = SnapshotIdx;
                ui8 newSnapshotIdx = 1 - SnapshotIdx;
                Snapshots.at(newSnapshotIdx) = snapshot;
                SnapshotIdx = newSnapshotIdx;
                return Snapshots.at(oldSnapshotIdx);
            }

            TSnapshotPtr GetSnapshot() const {
                return Snapshots.at(SnapshotIdx);
            }

        private:
            std::array<TSnapshotPtr, 2> Snapshots;
            std::atomic<ui8> SnapshotIdx = 0;
    };

    struct TTreeElementBase : public TStaticAttributes {
        std::atomic<ui64> Usage = 0;
        std::atomic<ui64> UsageExtra = 0;
        std::atomic<ui64> Demand = 0;
        std::atomic<ui64> Throttle = 0;

        std::atomic<ui64> BurstUsage = 0;
        std::atomic<ui64> BurstUsageExtra = 0;
        std::atomic<ui64> BurstThrottle = 0;

        TTreeElementBase* Parent = nullptr;
        std::vector<TTreeElementPtr> Children;

        virtual ~TTreeElementBase() = default;
        virtual NSnapshot::TTreeElementBase* TakeSnapshot() const = 0;
        virtual const TId& GetId() const = 0;

        void AddChild(const TTreeElementPtr& element);
        void RemoveChild(const TTreeElementPtr& element);

        bool IsRoot() const {
            return !Parent;
        }

        bool IsLeaf() const {
            return Children.empty();
        }
    };

    class TQuery : public TTreeElementBase, public TSnapshotSwitch<NSnapshot::TQueryPtr> {
    public:
        explicit TQuery(const TQueryId& id, const TStaticAttributes& attrs = {});

        const TId& GetId() const override {
            return Id;
        }

        NSnapshot::TQuery* TakeSnapshot() const override;

        std::atomic<ui64> CurrentTasksTime = 0; // sum of average execution time for all active tasks
        std::atomic<ui64> WaitingTasksTime = 0; // sum of average execution time for all throttled tasks

        NMonitoring::THistogramPtr Delay; // TODO: hacky counter for delays from queries - initialize from pool

    private:
        const TId Id;
    };

    class TPool : public TTreeElementBase {
    public:
        TPool(const TPoolId& id, const TIntrusivePtr<TKqpCounters>& counters, const TStaticAttributes& attrs = {});

        const TId& GetId() const override {
            return Id;
        }

        void AddQuery(const TQueryPtr& query);
        void RemoveQuery(const TQueryId& queryId);
        TQueryPtr GetQuery(const TQueryId& queryId) const;

        NSnapshot::TPool* TakeSnapshot() const override;

    private:
        const TId Id;
        THashMap<TQueryId, TQueryPtr> Queries;
        TPoolCounters Counters;
    };

    class TDatabase : public TTreeElementBase {
    public:
        explicit TDatabase(const TDatabaseId& id, const TStaticAttributes& attrs = {});

        const TId& GetId() const override {
            return Id;
        }

        void AddPool(const TPoolPtr& pool);
        TPoolPtr GetPool(const TPoolId& id) const;

        NSnapshot::TDatabase* TakeSnapshot() const override;

    private:
        const TId Id;
        THashMap<TPoolId, TPoolPtr> Pools;
    };

    class TRoot : public TTreeElementBase, public TSnapshotSwitch<NSnapshot::TRootPtr> {
    public:
        explicit TRoot(TIntrusivePtr<TKqpCounters> counters);

        const TId& GetId() const override {
            static const TId id("(ROOT)");
            return id;
        }

        void AddDatabase(const TDatabasePtr& database);
        TDatabasePtr GetDatabase(const TDatabaseId& id) const;

        NSnapshot::TRoot* TakeSnapshot() const override;

    public:
        ui64 TotalLimit = Infinity();

    private:
        THashMap<TDatabaseId, TDatabasePtr> Databases;

        struct {
            NMonitoring::TDynamicCounters::TCounterPtr TotalLimit;
        } Counters;
    };

} // namespace NKikimr::NKqp::NScheduler::NHdrf::NDynamic
