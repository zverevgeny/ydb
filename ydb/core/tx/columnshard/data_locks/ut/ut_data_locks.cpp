#include <ydb/core/tx/columnshard/data_locks/manager/manager.h>

#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr {

using namespace NOlap::NDataLocks;

Y_UNIT_TEST_SUITE(DataLocks) {
     struct TLockReceiver: public ILockAcquired {
        std::unique_ptr<TGuard> Acquired;
            void OnAcquired(std::unique_ptr<TGuard>&& guard) {
                Acquired = std::move(guard);
            }
    };
    
    Y_UNIT_TEST(Shared) {
        auto dataLockManager = std::make_shared<TManager>();
    }
} //Y_UNIT_TEST_SUITE(DataLocks)

} // namespace NKikimr
