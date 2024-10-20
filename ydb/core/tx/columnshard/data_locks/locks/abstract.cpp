#include "abstract.h"

namespace NKikimr::NOlap::NDataLocks {

namespace {

bool HasCommonElementImpl(const THashSet<ui64>& smaller, const THashSet<ui64>& bigger) {
    for (const auto& e: smaller) {
        if (bigger.contains(e)) {
            return true;
        }
    }
    return false;
}

bool HasCommonElement(const THashSet<ui64>& a, const THashSet<ui64>& b) {
    if (a.size() < b.size()) {
        return HasCommonElementImpl(a, b);
    } else {
        return HasCommonElementImpl(b, a);
    }
}

} //namespace

bool ILock::IsEqualTo(ILock& other) const {
    return DoIsEqualTo(other);
}

bool ILock::IsCompatibleWith(ILock& other) const {
    if ((Object == EObject::Schema) != (other.Object == EObject::Schema)) {
        //Schema locks are compatible with non-schema locks
        return true;
    }
    if (!HasCommonElement(Paths, other.Paths)) {
        return true;
    }
    return DoIsCompatibleWith(other);
}

} //namespace NKikimr::NOlap::NDataLocks

