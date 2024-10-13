#pragma once
#include <ydb/library/accessor/accessor.h>

#include <util/generic/string.h>
#include <util/generic/hash_set.h>

#include <optional>
#include <memory>
#include <vector>

namespace NKikimr::NOlap {
class TPortionInfo;
class TGranuleMeta;
}

namespace NKikimr::NOlap::NDataLocks {

enum class EObject {
    //Schema objects:
    Schema,
    //Data objects:
    Portions,
    Granules,
};

enum class EAction {
    Create,
    Read,
    Modify,
    Delete,
};


class ILock {
private:
    YDB_READONLY_DEF(TString, LockName);
    YDB_READONLY_DEF(EObject, Object);
    YDB_READONLY_DEF(THashSet<ui64>, Paths);

protected:
    virtual bool DoIsEqualTo(ILock& other) const {
        Y_UNUSED(other);
        return false;
    };
    virtual bool DoIsCompatibleWith(ILock& other) const {
        Y_UNUSED(other);
        return false;
    };
public:
    using TPtr = std::unique_ptr<ILock>;
    bool IsEqualTo(ILock& other) const;
    bool IsCompatibleWith(ILock& lock) const;
    virtual std::optional<TString> IsLocked(const TPortionInfo& portion, const EAction action) const = 0;
    virtual std::optional<TString> IsLocked(const TGranuleMeta& granule, const EAction action) const = 0;
    virtual std::optional<TString> IsLockedTableSchema(const ui64 pathId, const EAction action) const = 0;
    virtual bool IsEmpty() const = 0;
public:
    ILock(const TString& lockName)
        : LockName(lockName)
    {}
    virtual ~ILock() = default;
};

} //NKikimr::NOlap::NDataLocks