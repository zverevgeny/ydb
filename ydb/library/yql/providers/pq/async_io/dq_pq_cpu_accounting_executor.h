#pragma once

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/executor/executor.h>

#include <util/system/hp_timer.h>
#include <util/system/yassert.h>
#include <util/generic/yexception.h>

#include <atomic>
#include <memory>

namespace NYql::NDq {

// Wraps a Topic SDK executor and accumulates wall time of posted tasks.
// Used to expose SDK compression/decompression CPU via GetCpuTime() for HDRF.
class TCpuAccountingExecutor final : public NYdb::IExecutor {
public:
    TCpuAccountingExecutor(TPtr base, std::shared_ptr<std::atomic<ui64>> cpuMicros)
        : Base(std::move(base))
        , CpuMicros(std::move(cpuMicros))
    {
        Y_ENSURE(Base);
        Y_ENSURE(CpuMicros);
    }

    void Stop() override {
        Base->Stop();
    }

    bool IsAsync() const override {
        return Base->IsAsync();
    }

    void Post(TFunction&& f) override {
        Base->Post([cpu = CpuMicros, fn = std::move(f)]() mutable {
            THPTimer timer;
            fn();
            const auto micros = static_cast<ui64>(timer.Passed() * 1'000'000.0);
            if (micros) {
                cpu->fetch_add(micros, std::memory_order_relaxed);
            }
        });
    }

protected:
    void DoStart() override {
        Base->Start();
    }

private:
    TPtr Base;
    std::shared_ptr<std::atomic<ui64>> CpuMicros;
};

} // namespace NYql::NDq
