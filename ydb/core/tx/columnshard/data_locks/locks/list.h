#pragma once
#include "abstract.h"
#include <ydb/core/tx/columnshard/engines/portions/portion_info.h>
#include <ydb/core/tx/columnshard/engines/storage/granule/granule.h>

namespace NKikimr::NOlap::NDataLocks {

class TListPortionsLock: public ILock {
private:
    using TBase = ILock;
    THashSet<TPortionAddress> Portions;
    THashSet<ui64> Granules;
protected:
    virtual std::optional<TString> IsLocked(const TPortionInfo& portion, const EAction action) const override {
        Y_UNUSED(action);
        if (Portions.contains(portion.GetAddress())) {
            return GetLockName();
        }
        return {};
    }
    virtual std::optional<TString> IsLocked(const TGranuleMeta& granule, const EAction action) const override {
        Y_UNUSED(action);
        if (Granules.contains(granule.GetPathId())) {
            return GetLockName();
        }
        return {};
    }
    virtual std::optional<TString> IsLockedTableSchema(const ui64 pathId, const EAction action) const override {
        Y_UNUSED(pathId);
        Y_UNUSED(action);
        return {};
    }
    bool IsEmpty() const override {
        return Portions.empty();
    }
public:
    TListPortionsLock(const TString& lockName, const std::vector<std::shared_ptr<TPortionInfo>>& portions)
        : TBase(lockName)
    {
        for (auto&& p : portions) {
            Portions.emplace(p->GetAddress());
            Granules.emplace(p->GetPathId());
        }
    }

    TListPortionsLock(const TString& lockName, const std::vector<TPortionInfo>& portions)
        : TBase(lockName) {
        for (auto&& p : portions) {
            Portions.emplace(p.GetAddress());
            Granules.emplace(p.GetPathId());
        }
    }

    template <class T, class TGetter>
    TListPortionsLock(const TString& lockName, const std::vector<T>& portions, const TGetter& g)
        : TBase(lockName) {
        for (auto&& p : portions) {
            const auto address = g(p);
            Portions.emplace(address);
            Granules.emplace(address.GetPathId());
        }
    }

    template <class T>
    TListPortionsLock(const TString& lockName, const THashMap<TPortionAddress, T>& portions)
        : TBase(lockName) {
        for (auto&& p : portions) {
            const auto address = p.first;
            Portions.emplace(address);
            Granules.emplace(address.GetPathId());
        }
    }

    TListPortionsLock(const TString& lockName, const THashSet<TPortionAddress>& portions)
        : TBase(lockName) {
        for (auto&& address : portions) {
            Portions.emplace(address);
            Granules.emplace(address.GetPathId());
        }
    }
};

class TListTablesLock: public ILock {
private:
    using TBase = ILock;
    const THashSet<ui64> Tables;
    const NDataLocks::EAction Action;
private:
protected:
    // virtual bool DoIsEqualTo(ILock& other) const override {
    //     if (auto& otherLock = dynamic_cast<TListTablesLock&>(other)) {
    //         return Tables == other.Tables && Action == other.Action;
    //     };
    //     return false;
    // }
    // virtual bool DoIsCompatibleWith(ILock& other) const override {
    //     // if (auto& other.first = dynamic_cast<TListTablesLock&>(other)) {
            
    //     // }
    // }
    virtual std::optional<TString> IsLocked(const TPortionInfo& portion, const EAction) const override {
        if (Tables.contains(portion.GetPathId())) {
            return GetLockName();
        }
        return {};
    }
    virtual std::optional<TString> IsLocked(const TGranuleMeta& granule, const EAction) const override {
        if (Tables.contains(granule.GetPathId())) {
            return GetLockName();
        }
        return {};
    }
    virtual std::optional<TString> IsLockedTableSchema(const ui64 pathId, const EAction) const override {
        Y_UNUSED(pathId);
        return GetLockName();
    }
    bool IsEmpty() const override {
        return Tables.empty();
    }
public:
    TListTablesLock(const TString& lockName, const THashSet<ui64>& tables, const EAction action)
        : TBase(lockName)
        , Tables(tables)
        , Action(action)
    {
        Y_UNUSED(Action);
    }
};

}