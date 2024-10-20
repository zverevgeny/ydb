#pragma once
#include "abstract.h"

namespace NKikimr::NOlap::NDataLocks {

class TCompositeLock: public ILock {
private:
    using TBase = ILock;
    std::vector<std::unique_ptr<ILock>> Locks;
protected:
    virtual std::optional<TString> IsLocked(const TPortionInfo& portion, const EAction action) const override {
        Y_UNUSED(action);
        for (auto&& i : Locks) {
            if (auto lockName = i->IsLocked(portion, action)) {
                return lockName;
            }
        }
        return {};
    }
    virtual std::optional<TString> IsLocked(const TGranuleMeta& granule, const EAction action) const override {
        Y_UNUSED(action);
        for (auto&& i : Locks) {
            if (auto lockName = i->IsLocked(granule, action)) {
                return lockName;
            }
        }
        return {};
    }
    virtual std::optional<TString> IsLockedTableSchema(const ui64 pathId, const EAction action) const override {
        Y_UNUSED(pathId);
        Y_UNUSED(action);
        return {};
    }
    bool IsEmpty() const override {
        return Locks.empty();
    }
public:
    TCompositeLock(const TString& lockName, std::vector<ILock::TPtr>&& locks)
        : TBase(lockName)
    {
        for (auto&& l : locks) {
            if (!l || l->IsEmpty()) {
                continue;
            }
            Locks.emplace_back(std::move(l));
        }
    }
};

}