#pragma once
#include <ydb/core/tx/columnshard/data_locks/locks/abstract.h>
#include <util/generic/hash.h>
#include <util/generic/string.h>
#include <optional>
#include <deque>

namespace NKikimr::NOlap::NDataLocks {


enum class ELockType {
    Shared,
    Exclusive
};

class TManager;

class TGuard: TNonCopyable {
private:
    const size_t LockId;
    std::shared_ptr<TAtomicCounter> StopFlag;
    bool Released = false;
public:
    using TPtr = std::unique_ptr<TGuard>;
    TGuard(const size_t lockId, const std::shared_ptr<TAtomicCounter>& stopFlag)
        : LockId(lockId)
        , StopFlag(stopFlag)
    {
    }

    size_t GetLockId() const {
        return LockId;
    }

    void AbortLock();

    ~TGuard();

    void Release(TManager& manager);
};

struct ILockAcquired {
    using TPtr = std::unique_ptr<ILockAcquired>;
    virtual void OnLockAcquired(TGuard::TPtr&& guard) = 0;
    virtual ~ILockAcquired() = default;
};


class TManager {
private:
    using TLockAndType = std::pair<std::unique_ptr<ILock>, ELockType>;
    friend class TGuard;
    THashMap<size_t, std::pair<
        TLockAndType,
        size_t //lock count
    >> Locked;
    std::deque<std::pair<TLockAndType, ILockAcquired::TPtr>> Awaiting;
    size_t LastLockId = 0;
    std::shared_ptr<TAtomicCounter> StopFlag = std::make_shared<TAtomicCounter>(0);
private:
    bool IsCompatibleWithExistingLocks(const ILock& lock, const ELockType lockType) const;
    TGuard::TPtr PutInLocked(ILock::TPtr&& lock, ELockType type);
    void PutInAwaiting(TLockAndType&& newLock, ILockAcquired::TPtr&& onAcquired);
    void ReleaseLock(const size_t lockId);
public:
    TManager() = default;

    void Stop();



    std::unique_ptr<TGuard> Lock(ILock::TPtr&& lock,  const ELockType type, ILockAcquired::TPtr&& onAcquired);
    std::unique_ptr<TGuard> TryLock(ILock::TPtr&& lock,  const ELockType type) {
        return Lock(std::move(lock), type, ILockAcquired::TPtr{});
    }
    
    std::optional<TString> IsLocked(const TPortionInfo& portion, const EAction action, const std::unique_ptr<TGuard>& ignored = nullptr) const;
    std::optional<TString> IsLocked(const TGranuleMeta& granule, const EAction action, const std::unique_ptr<TGuard>& ignored = nullptr) const;
    std::optional<TString> IsLockedTableSchema(const ui64 pathId, const EAction action, const std::unique_ptr<TGuard>& ignored = nullptr);
    //std::optional<TString> IsLockedTableDataCommitted(const ui64 pathId, const EAction action = TLockScope{.Action = EAction::Modify, .Originator = EOriginator::Bg});
};

}