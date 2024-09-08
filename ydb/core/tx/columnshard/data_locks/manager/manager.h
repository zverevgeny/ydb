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

//TODO consider to use generic TGuard
class TGuard {
    private:
        TManager& Manager;
        const size_t LockId;
    public:
        TGuard(TManager& manager, const size_t lockId)
            : Manager(manager)
            , LockId(lockId)
        {
        }
        ~TGuard();
        void Release();
};

struct ILockAccuired {
    using TPtr = std::unique_ptr<ILockAccuired>;
    virtual void OnLockAccuired(TGuard&& guard) = 0;
    virtual ~ILockAccuired() = default;
};

class TManager {
private:
    struct TLockInfo {
        std::unique_ptr<ILock> Lock;
        ELockType LockType;
        size_t LockCount;
    };
    THashMap<size_t, TLockInfo> Locks;
    std::deque<TLockInfo> Awaiting;
    size_t LastLockId = 0;
    void ReleaseLock(const size_t lockId);
public:
    TManager() = default;
    //void Stop();

    std::optional<TGuard> Lock(ILock::TPtr&& lock,  const ELockType type, ILockAccuired::TPtr&& onAccuired);
    std::optional<TGuard> TryLock(ILock::TPtr&& lock,  const ELockType type) {
        return Lock(std::move(lock), type, ILockAccuired::TPtr{});
    }
    
    std::optional<TString> IsLocked(const TPortionInfo& portion, const TLockScope& scope = TLockScope{.Action = EAction::Modify, .Originator = EOriginator::Bg}) const;
    std::optional<TString> IsLocked(const TGranuleMeta& granule, const TLockScope& scope = TLockScope{.Action = EAction::Modify, .Originator = EOriginator::Bg}) const;
    //std::optional<TString> IsLockedTableDataCommitted(const ui64 pathId, const TLockScope& scope = TLockScope{.Action = EAction::Modify, .Originator = EOriginator::Bg});
    std::optional<TString> IsLockedTableSchema(const ui64 pathId, const TLockScope& scope = TLockScope{.Action = EAction::Modify, .Originator = EOriginator::Bg});
    std::optional<TString> IsLocked(const ui64 pathId);
};

}