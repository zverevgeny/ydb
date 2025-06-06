#pragma once

#include "hive.h"
#include "tablet_info.h"

namespace NKikimr {
namespace NHive {

struct TBootQueue {
    struct TBootQueueRecord {
        TTabletId TabletId;
        double Priority;
        TFollowerId FollowerId;
        TNodeId SuggestedNodeId;

        static double GetBootPriority(const TTabletInfo& tablet) {
            double priority = 0;
            switch (tablet.GetTabletType()) {
            case TTabletTypes::Hive:
                priority = 4;
                break;
            case TTabletTypes::SchemeShard:
                priority = 3;
                break;
            case TTabletTypes::Mediator:
            case TTabletTypes::Coordinator:
            case TTabletTypes::BlobDepot:
                priority = 2;
                break;
            case TTabletTypes::ColumnShard:
                priority = 0;
                break;
            default:
                if (tablet.IsLeader()) {
                    priority = 1;
                }
                break;
            }
            priority += tablet.Weight;
            if (tablet.RestartsOften()) {
               priority -= 5;
            }
            return priority;
        }

        bool operator <(const TBootQueueRecord& o) const {
            return Priority < o.Priority;
        }

        TBootQueueRecord(const TTabletInfo& tablet, TNodeId suggestedNodeId = 0);
    };

    static_assert(sizeof(TBootQueueRecord) <= 24);

    using TQueue = TPriorityQueue<TBootQueueRecord>;

    TQueue BootQueue;
    TQueue WaitQueue; // tablets from BootQueue waiting for new nodes
private:
    bool ProcessWaitQueue = false;
    bool NextFromWaitQueue = false;

public:
    void AddToBootQueue(TBootQueueRecord record);
    TBootQueueRecord PopFromBootQueue();
    void AddToWaitQueue(TBootQueueRecord record);
    void IncludeWaitQueue();
    void ExcludeWaitQueue();
    bool Empty() const;
    size_t Size() const;

    template<typename... Args>
    void EmplaceToBootQueue(Args&&... args) {
        BootQueue.emplace(args...);
    }

private:
    TQueue& GetCurrentQueue();
};

}
}
