#include "kqp_rules_include.h"

#include <ydb/core/kqp/opt/rbo/map_renames.h>

namespace {

using namespace NKikimr::NKqp;
using namespace NKikimr::NKqp::NMapRenames;

// Recursively locate the argument fed to `Re2.PatternFromLike` inside the
// Re2.Match RunConfig subtree. Returns the Member node (the LIKE mask column).
TExprNode::TPtr FindPatternFromLikeArg(const TExprNode::TPtr& node) {
    if (!node) {
        return nullptr;
    }
    if (node->IsCallable("Apply") && node->ChildrenSize() >= 2) {
        auto callee = node->ChildPtr(0);
        while (callee && (callee->IsCallable("AssumeStrict") || callee->IsCallable("Just"))) {
            callee = callee->ChildPtr(0);
        }
        if (callee && callee->IsCallable("Udf") && callee->ChildrenSize() >= 1 && callee->Child(0)->IsAtom() &&
            callee->Child(0)->Content() == "Re2.PatternFromLike") {
            return node->ChildPtr(1);
        }
    }
    for (const auto& child : node->Children()) {
        if (auto found = FindPatternFromLikeArg(child)) {
            return found;
        }
    }
    return nullptr;
}

// Detect a join filter of the form `probe LIKE mask`, lowered by the frontend to
//   (Apply (AssumeStrict (Udf "Re2.Match" '((Apply <PatternFromLike> (Member row MASK)) <opts>)))
//          (Member row PROBE))
// On success, `probe` and `pattern` are set to the probed (left) column and the
// LIKE mask (right) column respectively. Requires pattern to originate from the
// right input and probe from the left input, matching the automaton-from-right
// physical layout.
bool DetectLikeJoinFilter(const TExpression& filter, const TIntrusivePtr<TOpJoin>& join, TInfoUnit& probe, TInfoUnit& pattern) {
    auto body = filter.GetExpressionBody();
    if (!body || !body->IsCallable("Apply") || body->ChildrenSize() < 2) {
        return false;
    }

    auto matcher = body->ChildPtr(0);
    while (matcher && (matcher->IsCallable("AssumeStrict") || matcher->IsCallable("Just"))) {
        matcher = matcher->ChildPtr(0);
    }
    if (!matcher || !matcher->IsCallable("Udf") || matcher->ChildrenSize() < 2 || !matcher->Child(0)->IsAtom() ||
        matcher->Child(0)->Content() != "Re2.Match") {
        return false;
    }

    auto patternMember = FindPatternFromLikeArg(matcher->ChildPtr(1));
    auto probeMember = body->ChildPtr(1);
    if (!patternMember || !patternMember->IsCallable("Member") || patternMember->ChildrenSize() < 2 ||
        !probeMember->IsCallable("Member") || probeMember->ChildrenSize() < 2) {
        return false;
    }

    TInfoUnit patternIU(TString(patternMember->Child(1)->Content()));
    TInfoUnit probeIU(TString(probeMember->Child(1)->Content()));

    TInfoUnitSet leftSet;
    for (const auto& iu : join->GetLeftInput()->GetOutputIUs()) {
        leftSet.insert(iu);
    }
    TInfoUnitSet rightSet;
    for (const auto& iu : join->GetRightInput()->GetOutputIUs()) {
        rightSet.insert(iu);
    }

    // Pattern (LIKE mask) must be on the right (broadcast) side; probe on the left.
    if (leftSet.contains(probeIU) && rightSet.contains(patternIU)) {
        probe = probeIU;
        pattern = patternIU;
        return true;
    }
    return false;
}

bool CheckNonNullKeys(const TIntrusivePtr<IOperator> &input, const TVector<TInfoUnit>& columns) {
    auto itemType = input->Type->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
    for (const auto & column : columns) {
        const auto* columnType = itemType->FindItemType(column.GetFullName());
        // A key column may be absent from the row type when downstream alias rewrites have renamed
        // it but the propagated KeyColumns metadata still references the old name. In that case we
        // cannot prove the key is non-null (nor build a valid join on it), so bail out of the rewrite.
        if (!columnType || columnType->IsOptionalOrNull()) {
            return false;
        }
    }
    return true;
}

}

namespace NKikimr {
namespace NKqp {

bool TInlineJoinFiltersRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::Join;
}

// Inline join filters. In case of inner join, replace the join with a filter on top of inner or cross join
// More complex logic for other types of joins

TIntrusivePtr<IOperator> TInlineJoinFiltersRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator> &input, TRBOContext &ctx, TPlanProps &props) {
    Y_UNUSED(props);

    if (input->Kind != EOperator::Join) {
        return input;
    }

    auto join = CastOperator<TOpJoin>(input);
    if (join->JoinFilters.empty()) {
        return input;
    }

    // In case of inner or cross join, we push the join filters above the join
    if (join->JoinKind == "Inner" || join->JoinKind == "Cross") {
        auto filterExpr = MakeConjunction(join->JoinFilters);
        auto newFilter = MakeIntrusive<TOpFilter>(join, input->Pos, filterExpr);

        join->JoinFilters = {};

        // Now that we pushed the filters out of the join, the join might turn into a cross-join
        if (join->JoinKeys.empty()) {
            join->JoinKind = "Cross";
        }

        return newFilter;
    }

    // We only support various left joins now
    if (join->JoinKind != "Left" && join->JoinKind != "LeftSemi" && join->JoinKind != "LeftOnly") {
        Y_ENSURE(false, TStringBuilder() << "Join filter in unsupported join type: " << join->JoinKind);
        return input;
    }

    THashSet<TInfoUnit, TInfoUnit::THashFunction> usedIUs;
    AddUsedIUs(usedIUs, join->GetLeftInput()->GetOutputIUs());
    AddUsedIUs(usedIUs, join->GetRightInput()->GetOutputIUs());
    for (const auto& [leftKey, rightKey] : join->JoinKeys) {
        usedIUs.insert(leftKey);
        usedIUs.insert(rightKey);
    }
    for (const auto& joinFilter : join->JoinFilters) {
        AddUsedIUs(usedIUs, joinFilter.GetInputIUs(false, true));
    }

    // Build an inner (or cross, when there are no equi-keys) join, but in case of LeftSemi and LeftOnly,
    // the right side may contain duplicate IUs which will break the plan. So we rename them.
    auto commonIUs = IUSetIntersect(join->GetLeftInput()->GetOutputIUs(), join->GetRightInput()->GetOutputIUs());
    auto rightRenameMap = MakeRenameMap(commonIUs, props.InternalVarIdx, usedIUs);
    const TString equiJoinKind = join->JoinKeys.empty() ? "Cross" : "Inner";
    auto innerJoin = MakeJoinWithRightRenames(
        join->GetLeftInput(), join->GetRightInput(), join->Pos, equiJoinKind, join->JoinKeys, {}, rightRenameMap, ctx.ExprCtx, props);

    // The inner (cross) join feeds the outer LEFT re-join below. For a plain
    // left join we materialize the join filters as a TOpFilter on top of the
    // inner join. For a `probe LIKE mask` predicate we instead tag the inner
    // cross join as an automaton-based LIKE join and drop the filter: physical
    // conversion emits a single combined Hyperscan pass over all right masks,
    // and the outer LEFT re-join restores NULL-fill for probe rows that matched
    // no pattern.
    // The Hyperscan automaton LIKE-join is opt-in: only when the
    // `ydb.EnableLikeJoinHyperscan` pragma is set do we tag the inner cross
    // join as an automaton-based LIKE join. Otherwise we fall through to the
    // standard TOpFilter path (a plain double loop over the cross join).
    const bool useHyperscanLikeJoin =
        ctx.KqpCtx.Config->EnableLikeJoinHyperscan.Get().GetOrElse(false);

    TIntrusivePtr<IOperator> innerResult;
    TInfoUnit likeProbe;
    TInfoUnit likePattern;
    if (useHyperscanLikeJoin && join->JoinFilters.size() == 1 && DetectLikeJoinFilter(join->JoinFilters[0], join, likeProbe, likePattern)) {
        // The pattern column lives on the right input, which may have been
        // renamed to avoid collisions with the left side; follow the rename.
        if (const auto it = rightRenameMap.find(likePattern); it != rightRenameMap.end()) {
            likePattern = it->second;
        }
        innerJoin->IsLikeJoin = true;
        innerJoin->LikeProbeColumn = likeProbe;
        innerJoin->LikePatternColumn = likePattern;
        innerResult = innerJoin;
    } else {
        auto filterExpr = MakeConjunction(join->JoinFilters);
        innerResult = MakeIntrusive<TOpFilter>(innerJoin, input->Pos, filterExpr);
    }

    // We need to remap the appropriate side of the output columns, so we can join on the same columns again
    // without confilcts

    auto topCommonIUs = IUSetIntersect(join->GetLeftInput()->GetOutputIUs(), innerJoin->GetOutputIUs());

    auto renameMap = MakeRenameMap(topCommonIUs, props.InternalVarIdx, usedIUs);

    // The join will be on the keys of lhs, we just need to check that all the keys are non-null
    // We don't support nullable keys at this stage
    auto keyColumns = join->GetLeftInput()->Props.Metadata->KeyColumns;
    if (keyColumns.empty()) {
        Y_ENSURE(false, "No key columns when inlining join filter");
    }

    if (!CheckNonNullKeys(join->GetLeftInput(), keyColumns)) {
        Y_ENSURE(false, "During join filter inlining the keys on the left side cannot be null");
    }

    TVector<std::pair<TInfoUnit, TInfoUnit>> newJoinKeys;
    for (const auto & column : keyColumns) {
        newJoinKeys.push_back(std::make_pair(column, column));
    }

    auto result = MakeJoinWithRightRenames(join->GetLeftInput(), innerResult, join->Pos, join->JoinKind, newJoinKeys, {}, renameMap, ctx.ExprCtx, props);
    
    return result;
}
}
}
