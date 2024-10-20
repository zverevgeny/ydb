#include "manager.h"
#include <ydb/library/actors/core/log.h>

namespace NKikimr::NOlap::NDataLocks {


bool TManager::IsCompatibleWithExistingLocks(const ILock& lock, const ELockType lockType) const {
    for (const auto& [_, lockAndCount]: Locked) {
        const auto& [existingLock, count] = lockAndCount;
        if ((lockType == ELockType::Exclusive) || (existingLock.second == ELockType::Exclusive)) {
            AFL_INFO(NKikimrServices::TX_COLUMNSHARD)("event", "lock")("name", lock.GetLockName())("incompatible", existingLock.first->GetLockName());
            if (!lock.IsCompatibleWith(*existingLock.first)) {
                return false;
            }
        } else {
            AFL_INFO(NKikimrServices::TX_COLUMNSHARD)("event", "lock")("name", lock.GetLockName())("incompatible", existingLock.first->GetLockName());
        }
    }
    return true;
}

std::unique_ptr<TGuard> TManager::PutInLocked(ILock::TPtr&& lock, ELockType type) {
    ++LastLockId;
    AFL_INFO(NKikimrServices::TX_COLUMNSHARD)("event", "lock")("name", lock->GetLockName())("registered", LastLockId);
    AFL_VERIFY(Locked.emplace(
        LastLockId, 
        std::pair{
            std::pair{std::move(lock), type},
            1
        }
    ).second); 
    return std::make_unique<TGuard>(LastLockId, StopFlag);
}

void TManager::PutInAwaiting(TManager::TLockAndType&& newLock, ILockAcquired::TPtr&& onAcquired) {
    AFL_VERIFY(onAcquired);
    Awaiting.emplace_back(
            std::move(newLock),
            std::move(onAcquired)
    );    
}

std::unique_ptr<TGuard> TManager::Lock(ILock::TPtr&& lock, const ELockType lockType, ILockAcquired::TPtr&& onAcquired) {
    AFL_VERIFY(lock);
    AFL_INFO(NKikimrServices::TX_COLUMNSHARD)("event", "lock")("name", lock->GetLockName())("try", onAcquired ? "yes" : "no");
    for (const auto& [awaiting, _]: Awaiting) {
        if (!lock->IsCompatibleWith(*awaiting.first)) {
            AFL_INFO(NKikimrServices::TX_COLUMNSHARD)("event", "lock")("name", lock->GetLockName())("incompatible", awaiting.first->GetLockName());
            if (onAcquired) {
                PutInAwaiting({std::move(lock), lockType}, std::move(onAcquired));
            } else {
                return nullptr;
            }
        }
    }
    for (auto& [id, lockAndCount]: Locked) {
        auto& [existingLock, count] = lockAndCount;
        if (lockType == ELockType::Shared && existingLock.second == ELockType::Shared && lock->IsEqualTo(*existingLock.first)) {
            ++count;
            AFL_INFO(NKikimrServices::TX_COLUMNSHARD)("event", "lock")("name", lock->GetLockName())("reuse", existingLock.first->GetLockName())("count", count);
            return std::make_unique<TGuard>(id, StopFlag);
        }
    }
    if (IsCompatibleWithExistingLocks(*lock, lockType)) {
        PutInLocked(std::move(lock), lockType);
    } else {
        if (onAcquired) {
            PutInAwaiting({std::move(lock), lockType}, std::move(onAcquired));
        } else {
            return nullptr;
        }
    }
    return PutInLocked(std::move(lock), lockType);
}

void TManager::ReleaseLock(const size_t lockId) {
    AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD)("event", "unlock")("lock_id", lockId);
    auto lockInfo = Locked.FindPtr(lockId);
    AFL_VERIFY(lockInfo);
    AFL_VERIFY(lockInfo->second != 0);
    if (--lockInfo->second > 0) {
        return;
    }
    AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD)("event", "erase_lock")("lock_id", lockId)("locked", Locked.size())("awaiting", Awaiting.size());
    AFL_VERIFY(Locked.erase(lockId))("lock_id", lockId);
    while(!Awaiting.empty()) {
        auto& [nextToLock, onAcquired] = Awaiting.front();
        if (!IsCompatibleWithExistingLocks(*nextToLock.first, nextToLock.second)) {
            return;
        }
        onAcquired->OnLockAcquired(PutInLocked(std::move(nextToLock.first), nextToLock.second));
        Awaiting.pop_front();
    }
}

std::optional<TString> TManager::IsLocked(const TPortionInfo& portion, const EAction action, const std::unique_ptr<TGuard>& ignored) const {
    Y_UNUSED(action);
    for (const auto& [id, lockAndType] : Locked) {
        if (ignored && ignored->GetLockId() == id) {
            continue;
        }
        if (auto lockName = lockAndType.first.first->IsLocked(portion, action)) {
            return lockName;
        }
    }
    return {};
}

std::optional<TString> TManager::IsLocked(const TGranuleMeta& granule, const EAction action, const std::unique_ptr<TGuard>& ignored) const {
    for (const auto& [id, lockInfo] : Locked) {
        if (ignored && ignored->GetLockId() == id) {
            continue;
        }
        if (auto lockName = lockInfo.first.first->IsLocked(granule, action)) {
            return lockName;
        }
    }
    return {};
}

void TManager::Stop() {
    AFL_VERIFY(StopFlag->Inc() == 1);
}

TGuard::~TGuard() {
    AFL_VERIFY(Released || !NActors::TlsActivationContext || StopFlag->Val() == 1);
}

void TGuard::Release(TManager& manager) {
    AFL_VERIFY(!Released);
    manager.ReleaseLock(LockId);
    Released = true;
}

void TGuard::AbortLock() {
    if (!Released) {
        AFL_WARN(NKikimrServices::TX_COLUMNSHARD)("message", "aborted data locks manager");
    }
    Released = true;
}

} //namespace NKikimr::NOlap::NDataLocks