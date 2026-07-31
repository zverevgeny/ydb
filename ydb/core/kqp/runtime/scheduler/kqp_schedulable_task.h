#pragma once

#include "fwd.h"

#include <ydb/library/actors/core/actorid.h>
#include <ydb/library/actors/core/events.h>

#include <library/cpp/time_provider/monotonic.h>

namespace NKikimr::NKqp::NScheduler {

// The proxy-object between any schedulable actor and the scheduler itself

struct TSchedulableTask : public std::enable_shared_from_this<TSchedulableTask> {
    explicit TSchedulableTask(const NHdrf::NDynamic::TQueryPtr& query);
    ~TSchedulableTask();

    void RegisterForResume(const NActors::TActorId& actorId);
    void Resume();

    using TResumeEventType = NActors::TEvents::TEvWakeup;
    static bool IsResumeEvent(const TResumeEventType::TPtr& ev) {
        return ev->Get()->Tag == TAG_WAKEUP_RESUME;
    }
    static auto GetResumeEvent() {
        return std::make_unique<TResumeEventType>(TAG_WAKEUP_RESUME);
    }

    enum EUsageType {
        CPU_DEFAULT,
        CPU_RESUMED,
        READ_DEFAULT,
    };

    bool TryIncreaseUsage();
    void IncreaseUsage();
    void DecreaseUsage(const TDuration& burstUsage, EUsageType usageType);

    // Account external CPU (e.g. Topic SDK decompression) without holding a usage slot.
    void AccountBurstUsage(const TDuration& burstUsage, EUsageType usageType);

    // Streaming queries may wait for source data without needing CPU.
    // EnterIdle temporarily drops Demand so FairShare is not reserved;
    // ExitIdle restores it when work arrives.
    void EnterIdle();
    void ExitIdle();
    bool IsIdle() const {
        return Idle;
    }

    // Returns parent pool's 'fair-share' minus 'usage'
    size_t GetSpareUsage() const;

    void IncreaseBurstThrottle(const TDuration& burstThrottle);
    void IncreaseThrottle();
    void DecreaseThrottle();

    const NHdrf::NDynamic::TQueryPtr Query; // TODO: should be private

private:
    static constexpr ui64 TAG_WAKEUP_RESUME = 201; // TODO: why this value for magic number?

    std::optional<TSchedulableTaskList::iterator> Iterator; // TODO: improve iterator to not allow access to adjacent values in list.
    NActors::TActorId ActorId;
    bool Idle = false;
    TMonotonic IdleSince;
};

} // namespace NKikimr::NKqp::NScheduler
