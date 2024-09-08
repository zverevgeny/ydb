#pragma once
#include "abstract.h"

namespace NKikimr::NOlap::NDataLocks {

class TCompositeLock: public ILock {
private:
    using TBase = ILock;
    std::vector<std::shared_ptr<ILock>> Locks;
protected:
    virtual std::optional<TString> IsLocked(const TPortionInfo& portion, const TLockScope& scope) const override {
        for (auto&& i : Locks) {
            if (auto lockName = i->IsLocked(portion, scope)) {
                return lockName;
            }
        }
        return {};
    }
    virtual std::optional<TString> IsLocked(const TGranuleMeta& granule, const TLockScope& scope) const override {
        for (auto&& i : Locks) {
            if (auto lockName = i->IsLocked(granule, scope)) {
                return lockName;
            }
        }
        return {};
    }
    virtual std::optional<TString> IsLockedTableSchema(const ui64 pathId, const TLockScope& scope) const override {
        Y_UNUSED(pathId);
        Y_UNUSED(scope);
        return {};
    }
    bool IsEmpty() const override {
        return Locks.empty();
    }
public:
    TCompositeLock(const TString& lockName, const std::vector<std::shared_ptr<ILock>>& locks)
        : TBase(lockName)
    {
        for (auto&& l : locks) {
            if (!l || l->IsEmpty()) {
                continue;
            }
            Locks.emplace_back(l);
        }
    }

    TCompositeLock(const TString& lockName, std::initializer_list<std::shared_ptr<ILock>> locks)
        : TBase(lockName)
    {
        for (auto&& l : locks) {
            if (!l || l->IsEmpty()) {
                continue;
            }
            Locks.emplace_back(l);
        }
    }
};

}