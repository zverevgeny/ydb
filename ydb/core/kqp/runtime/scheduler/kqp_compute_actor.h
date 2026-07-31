#pragma once

#include "kqp_schedulable_actor.h"
#include "kqp_schedulable_task.h"

#include <ydb/library/yql/dq/actors/compute/dq_sync_compute_actor_base.h>

namespace NKikimr::NKqp::NScheduler {

    template <class TDerived>
    class TSchedulableComputeActorBase : public NYql::NDq::TDqSyncComputeActorBase<TDerived>, TSchedulableActorBase {
        using TBase = NYql::NDq::TDqSyncComputeActorBase<TDerived>;

    public:
        template<typename ... TArgs>
        TSchedulableComputeActorBase(const TSchedulableActorOptions& options, TArgs&& ... args)
            : TBase(std::forward<TArgs>(args) ...)
            , TSchedulableActorBase(options)
        {
        }

    protected:
        void DoBootstrap() {
            if (IsAccountable()) {
                RegisterForResume(this->SelfId());
            }
        }

        // Magic state-function name to overload
        STATEFN(BaseStateFuncBody) {
            // TODO: account mailbox usage?

            // we assume that exceptions are handled in parents/descendants
            switch (ev->GetTypeRewrite()) {
                hFunc(TSchedulableTask::TResumeEventType, TSchedulableComputeActorBase<TDerived>::Handle);
                default:
                    TBase::BaseStateFuncBody(ev);
            }
        }

        void PassAway() override {
            if (!PassedAway && IsAccountable()) {
                PassedAway = true;
                // Restore Demand before destroying SchedulableTask.
                if (IsIdle() && IdleEnteredAt) {
                    this->OnSchedulerIdleTime(Now() - *IdleEnteredAt);
                    IdleEnteredAt.Clear();
                }
                ExitIdle();
                if (SourcesToldThrottled) {
                    NotifySourcesSchedulerThrottled(false);
                    SourcesToldThrottled = false;
                }
                StopExecution(ForcedResume);
            }

            TBase::PassAway();
        }

    private:
        void Handle(TSchedulableTask::TResumeEventType::TPtr& ev) {
            if (TSchedulableTask::IsResumeEvent(ev)) {
                if (IsThrottled()) {
                    ForcedResume = ev->Sender != this->SelfId();
                    TBase::DoExecute();
                }
            } else {
                TBase::HandleExecuteBase(ev);
            }
        }

        void OnSourceCpuTimeAccounted(TDuration delta) override {
            if (IsAccountable() && delta) {
                AccountBurstUsage(delta);
            }
        }

        void OnSinkCpuTimeAccounted(TDuration delta) override {
            if (IsAccountable() && delta) {
                AccountBurstUsage(delta);
            }
        }

        void OnBeforeContinueExecuteFromNewAsyncInput() override {
            if (!IsAccountable()) {
                return;
            }
            // ExitIdle before ContinueExecute as required by cpu-accounting plan.
            if (IsIdle() && IdleEnteredAt) {
                this->OnSchedulerIdleTime(Now() - *IdleEnteredAt);
                IdleEnteredAt.Clear();
            }
            ExitIdle();
        }

        void NotifySourcesSchedulerThrottled(bool throttled) {
            for (auto& [_, source] : this->SourcesMap) {
                if (source.AsyncInput) {
                    source.AsyncInput->SetSchedulerThrottled(throttled);
                }
            }
        }

        void DoExecuteImpl() override {
            if (!IsAccountable()) {
                return TBase::DoExecuteImpl();
            }

            // Work arrived (or throttle wakeup) — restore Demand before competing for FairShare.
            if (IsIdle() && IdleEnteredAt) {
                this->OnSchedulerIdleTime(Now() - *IdleEnteredAt);
                IdleEnteredAt.Clear();
            }
            ExitIdle();

            // TODO: account waiting on mailbox?

            const auto now = Now();

            if (StartExecution(now)) {
                if (SourcesToldThrottled) {
                    if (ThrottleEnteredAt) {
                        this->OnSchedulerThrottledTime(now - *ThrottleEnteredAt);
                        ThrottleEnteredAt.Clear();
                    }
                    NotifySourcesSchedulerThrottled(false);
                    SourcesToldThrottled = false;
                }
                TBase::DoExecuteImpl();
                if (!PassedAway) {
                    StopExecution(ForcedResume);
                    // Waiting for source data: drop Demand so long-lived streaming
                    // queries do not permanently reserve FairShare while IDLE.
                    if (!this->ResumeEventScheduled
                        && !this->SourcesMap.empty()
                        && this->ProcessOutputsState.Inflight == 0
                        && this->ProcessOutputsState.LastRunStatus == NYql::NDq::ERunStatus::PendingInput)
                    {
                        IdleEnteredAt = Now();
                        EnterIdle();
                    }
                }
                return;
            }

            if (!SourcesToldThrottled) {
                NotifySourcesSchedulerThrottled(true);
                SourcesToldThrottled = true;
                ThrottleEnteredAt = now;
            }
            this->Schedule(CalculateDelay(now), TSchedulableTask::GetResumeEvent().release());
        }

    private:
        bool PassedAway = false;
        bool ForcedResume = false;
        bool SourcesToldThrottled = false;
        TMaybe<TMonotonic> IdleEnteredAt;
        TMaybe<TMonotonic> ThrottleEnteredAt;
    };

} // namespace NKikimr::NKqp::NScheduler
