#include <ydb/library/yql/providers/pq/async_io/dq_pq_cpu_accounting_executor.h>

#include <library/cpp/testing/unittest/registar.h>

namespace NYql::NDq {

Y_UNIT_TEST_SUITE(TCpuAccountingExecutor) {
    class TSyncExecutor final : public NYdb::IExecutor {
    public:
        void Stop() override {
        }

        bool IsAsync() const override {
            return false;
        }

        void Post(TFunction&& f) override {
            f();
        }

    protected:
        void DoStart() override {
        }
    };

    Y_UNIT_TEST(AccountsPostedWork) {
        auto micros = std::make_shared<std::atomic<ui64>>(0);
        auto executor = std::make_shared<TCpuAccountingExecutor>(
            std::make_shared<TSyncExecutor>(),
            micros);

        executor->Start();
        executor->Post([] {
            // Busy loop to accumulate measurable wall time.
            volatile ui64 x = 0;
            for (ui64 i = 0; i < 1'000'000; ++i) {
                x += i;
            }
            Y_UNUSED(x);
        });

        UNIT_ASSERT_GT(micros->load(), 0u);
        executor->Stop();
    }

    Y_UNIT_TEST(ZeroWorkIsFine) {
        auto micros = std::make_shared<std::atomic<ui64>>(0);
        auto executor = std::make_shared<TCpuAccountingExecutor>(
            std::make_shared<TSyncExecutor>(),
            micros);
        executor->Start();
        executor->Post([] {});
        // Very fast work may still round to 0 micros; just ensure no crash.
        UNIT_ASSERT_GE(micros->load(), 0u);
        executor->Stop();
    }
}

} // namespace NYql::NDq
