#include "kqp_rbo_physical_join_builder.h"
#include "kqp_rbo_physical_convertion_utils.h"
#include <ydb/core/kqp/opt/rbo/kqp_rbo_utils.h>

#include <yql/essentials/core/yql_expr_type_annotation.h>

using namespace NYql::NNodes;
using namespace NKikimr;
using namespace NKikimr::NKqp;


TExprNode::TPtr TPhysicalJoinBuilder::BuildCrossJoin(TExprNode::TPtr leftInput, TExprNode::TPtr rightInput) {
    TCoArgument leftArg{Ctx.NewArgument(Pos, "_kqp_left")};
    TCoArgument rightArg{Ctx.NewArgument(Pos, "_kqp_right")};
    const auto leftIUs = NPhysicalConvertionUtils::GetLiveInputIUs(*Join, 0);
    const auto rightIUs = NPhysicalConvertionUtils::GetLiveInputIUs(*Join, 1);

    leftInput = NPhysicalConvertionUtils::ExtractMembers(leftInput, Ctx, leftIUs);
    rightInput = NPhysicalConvertionUtils::ExtractMembers(rightInput, Ctx, rightIUs);

    TVector<TExprNode::TPtr> keys;
    for (const auto& iu : leftIUs) {
        YQL_CLOG(TRACE, CoreDq) << "Converting Cross Join, left key: " << iu.GetFullName();

        // clang-format off
        auto keyPtr = Build<TCoNameValueTuple>(Ctx, Pos)
            .Name().Build(iu.GetFullName())
            .Value<TCoMember>()
                .Struct(leftArg)
                .Name().Build(iu.GetFullName())
            .Build()
            .Done().Ptr();
        // clang-format on
        keys.push_back(keyPtr);
    }

    for (const auto& iu : rightIUs) {
        YQL_CLOG(TRACE, CoreDq) << "Converting Cross Join, right key: " << iu.GetFullName();

        // clang-format off
        auto keyPtr = Build<TCoNameValueTuple>(Ctx, Pos)
            .Name().Build(iu.GetFullName())
            .Value<TCoMember>()
                .Struct(rightArg)
                .Name().Build(iu.GetFullName())
            .Build()
            .Done().Ptr();
        // clang-format on
        keys.push_back(keyPtr);
    }

    // clang-format off
    // We have to `Condense` right input as single-element stream of lists (single list of all elements from the right),
    // because stream supports single iteration only
    //auto itemArg = Build<TCoArgument>(Ctx, Pos).Name("item").Done();
    auto rightAsStreamOfLists = Build<TCoCondense1>(Ctx, Pos)
        .Input<TCoToFlow>()
            .Input(rightInput)
            .Build()
        .InitHandler()
            .Args({"itemArg"})
            .Body<TCoAsList>()
                .Add("itemArg")
                .Build()
            .Build()
        .SwitchHandler()
            .Args({"item", "state"})
            .Body<TCoBool>()
                .Literal().Build("false")
                .Build()
            .Build()
        .UpdateHandler()
            .Args({"item", "state"})
            .Body<TCoAppend>()
                .List("state")
                .Item("item")
            .Build()
        .Build()
    .Done();

    auto flatMap = Build<TCoFlatMap>(Ctx, Pos)
        .Input(rightAsStreamOfLists)
        .Lambda()
            .Args({"rightAsList"})
            .Body<TCoFlatMap>()
                .Input(leftInput)
                .Lambda()
                    .Args({leftArg})
                    .Body<TCoMap>()
                        // here we have `List`, so we can iterate over it many times (for every `leftArg`)
                        .Input("rightAsList")
                        .Lambda()
                            .Args({rightArg})
                            .Body<TCoAsStruct>()
                                .Add(keys)
                            .Build()
                        .Build()
                    .Build()
                .Build()
            .Build()
        .Build()
    .Done().Ptr();

    return Build<TCoFromFlow>(Ctx, Pos)
        .Input(flatMap)
    .Done().Ptr();
    // clang-format on
}

// Automaton-based LIKE join.
//
// Semantics mirror BuildCrossJoin (materialize the whole right side, then join
// every left row against it), but instead of emitting the full cross product and
// filtering each pair with a per-pair Re2.Match, we build ONE combined Hyperscan
// automaton from all right-side LIKE masks and probe each left row a single time.
//
// The right side is condensed into a single materialized list `rightAsList`.
// From it we derive:
//   * patternList = OrderedMap(rightAsList, r -> Re2.PatternFromLike(r.<mask>))
//     which becomes the runtime RunConfig (List<String>) of the
//     Hyperscan.MultiMatchIndices UDF (anchored ^...$ matching == full-string
//     LIKE semantics).
//   * indexDict = ToIndexDict(rightAsList) : Dict<ui64, rightRow>, so a matched
//     index can be mapped back to its originating right row.
//
// For each left row we compute the list of matched pattern indices with a single
// automaton pass, then emit one output struct per matched index (left columns +
// the corresponding right columns). Left rows that match nothing produce no rows
// here; the outer LEFT re-join installed by TInlineJoinFiltersRule restores their
// NULL-filled output.
TExprNode::TPtr TPhysicalJoinBuilder::BuildLikeJoin(TExprNode::TPtr leftInput, TExprNode::TPtr rightInput) {
    const auto leftIUs = NPhysicalConvertionUtils::GetLiveInputIUs(*Join, 0);
    const auto rightIUs = NPhysicalConvertionUtils::GetLiveInputIUs(*Join, 1);

    leftInput = NPhysicalConvertionUtils::ExtractMembers(leftInput, Ctx, leftIUs);
    rightInput = NPhysicalConvertionUtils::ExtractMembers(rightInput, Ctx, rightIUs);

    const auto probeCol = Join->LikeProbeColumn.GetFullName();
    const auto patternCol = Join->LikePatternColumn.GetFullName();

    YQL_CLOG(TRACE, CoreDq) << "Converting LIKE Join, probe: " << probeCol << ", pattern: " << patternCol;

    // clang-format off
    auto flatMap = Ctx.Builder(Pos)
        .Callable("FlatMap")
            // Condense the right input into a single-element stream carrying the
            // whole right side as one materializable List (a stream can only be
            // iterated once; a List can be reused for every left row).
            .Callable(0, "Condense1")
                .Callable(0, "ToFlow")
                    .Add(0, rightInput)
                .Seal()
                .Lambda(1)
                    .Param("itemArg")
                    .Callable("AsList")
                        .Arg(0, "itemArg")
                    .Seal()
                .Seal()
                .Lambda(2)
                    .Param("item")
                    .Param("state")
                    .Callable("Bool")
                        .Atom(0, "false")
                    .Seal()
                .Seal()
                .Lambda(3)
                    .Param("item")
                    .Param("state")
                    .Callable("Append")
                        .Arg(0, "state")
                        .Arg(1, "item")
                    .Seal()
                .Seal()
            .Seal()
            .Lambda(1)
                .Param("rightAsList")
                .Callable("FlatMap")
                    .Add(0, leftInput)
                    .Lambda(1)
                        .Param("leftRow")
                        .Callable("OrderedMap")
                            // Single automaton pass: matched pattern indices for this left row.
                            .Callable(0, "Apply")
                                .Callable(0, "Udf")
                                    .Atom(0, "Hyperscan.MultiMatchIndices")
                                    // RunConfig: the List<String> of Re2 patterns compiled from
                                    // every right-side LIKE mask, in right-list order.
                                    .Callable(1, "OrderedMap")
                                        .Arg(0, "rightAsList")
                                        .Lambda(1)
                                            .Param("patRow")
                                            .Callable("Apply")
                                                .Callable(0, "Udf")
                                                    .Atom(0, "Re2.PatternFromLike")
                                                .Seal()
                                                .Callable(1, "Member")
                                                    .Arg(0, "patRow")
                                                    .Atom(1, patternCol)
                                                .Seal()
                                            .Seal()
                                        .Seal()
                                    .Seal()
                                .Seal()
                                .Callable(1, "Member")
                                    .Arg(0, "leftRow")
                                    .Atom(1, probeCol)
                                .Seal()
                            .Seal()
                            .Lambda(1)
                                .Param("idx")
                                .Callable("AsStruct")
                                    .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                                        ui32 i = 0;
                                        for (const auto& iu : leftIUs) {
                                            const auto name = iu.GetFullName();
                                            parent.List(i++)
                                                .Atom(0, name)
                                                .Callable(1, "Member")
                                                    .Arg(0, "leftRow")
                                                    .Atom(1, name)
                                                .Seal()
                                            .Seal();
                                        }
                                        for (const auto& iu : rightIUs) {
                                            const auto name = iu.GetFullName();
                                            // Map the matched index back to the originating right row.
                                            parent.List(i++)
                                                .Atom(0, name)
                                                .Callable(1, "Member")
                                                    .Callable(0, "Unwrap")
                                                        .Callable(0, "Lookup")
                                                            .Callable(0, "ToIndexDict")
                                                                .Arg(0, "rightAsList")
                                                            .Seal()
                                                            .Arg(1, "idx")
                                                        .Seal()
                                                    .Seal()
                                                    .Atom(1, name)
                                                .Seal()
                                            .Seal();
                                        }
                                        return parent;
                                    })
                                .Seal()
                            .Seal()
                        .Seal()
                    .Seal()
                .Seal()
            .Seal()
        .Seal()
    .Build();
    // clang-format on

    return Build<TCoFromFlow>(Ctx, Pos)
        .Input(flatMap)
    .Done().Ptr();
}

TExprNode::TPtr TPhysicalJoinBuilder::PrepareJoinSide(TExprNode::TPtr input, const TVector<TInfoUnit>& colNames, TVector<TString>& joinKeys,
                                                      const TModifyKeysList& remap, const bool filterNulls) {
    // clang-format off
    auto castMap = Ctx.Builder(Pos)
        .Callable("Map")
            .Add(0, input)
            .Lambda(1)
                .Param("row")
                .Callable("AsStruct")
                    .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                        ui32 i = 0U;
                        for (const auto& colName : colNames) {
                            const auto colNameStr = colName.GetFullName();
                            parent.List(i++)
                                .Atom(0, colNameStr)
                                .Callable(1, "Member")
                                    .Arg(0, "row")
                                    .Atom(1, colNameStr)
                                .Seal()
                            .Seal();
                        }
                        for (const auto& key : remap) {
                            parent.List(i++)
                                .Add(0, std::get<1>(key).Ptr())
                                .Callable(1, "StrictCast")
                                    .Callable(0, "Member")
                                        .Arg(0, "row")
                                        .Add(1, std::get<0>(key).Ptr())
                                    .Seal()
                                    .Add(1, ExpandType(Pos, *std::get<const TTypeAnnotationNode*>(key), Ctx))
                                .Seal()
                            .Seal();
                        }
                        return parent;
                    })
                .Seal()
            .Seal()
        .Seal()
    .Build();
    // clang-format on

    if (filterNulls) {
        TExprNode::TListType skipNullMembers, filterNullMembers;
        for (const auto& joinKey : joinKeys) {
            skipNullMembers.emplace_back(Ctx.NewAtom(Pos, joinKey));
        }

        for (const auto& remapTuple : remap) {
            auto key = std::get<1>(remapTuple).Ptr();
            if (std::get<const TTypeAnnotationNode*>(remapTuple)->IsOptionalOrNull()) {
                skipNullMembers.emplace_back(key);
            } else {
                filterNullMembers.emplace_back(key);
            }
        }

        // clang-format off
        castMap = Build<TCoSkipNullMembers>(Ctx, Pos)
            .Input(castMap)
            .Members().Add(std::move(skipNullMembers)).Build()
        .Done().Ptr();
        // clang-format on

        if (!filterNullMembers.empty()) {
            // clang-format off
            castMap = Build<TCoFilterNullMembers>(Ctx, Pos)
                .Input(castMap)
                .Members().Add(std::move(filterNullMembers)).Build()
            .Done().Ptr();
            // clang-format on
        }
    }

    for (const auto& remapTuple: remap) {
        const auto oldKey = std::get<0>(remapTuple).StringValue();
        const auto newKey = std::get<1>(remapTuple).StringValue();
        const ui32 joinKeyIndex = std::get<2>(remapTuple);
        Y_ENSURE(joinKeyIndex < joinKeys.size());
        Y_ENSURE(joinKeys[joinKeyIndex] == oldKey);
        joinKeys[joinKeyIndex] = newKey;
    }

    return castMap;
}

void TPhysicalJoinBuilder::PrepareJoinKeys(TVector<TString>& leftJoinKeys, TVector<TString>& rightJoinKeys, TModifyKeysList& remapLeft,
                                           TModifyKeysList& remapRight, THashMap<TString, TString>& leftColumnRemap,
                                           THashMap<TString, TString>& rightColumnRemap, TVector<TString>& leftJoinKeyRenames,
                                           TVector<TString>& rightJoinKeyRenames, const TStructExprType* leftInputType, const TStructExprType* rightInputType,
                                           const bool outer, const EJoinSide joinSide, const TTypeAnnotationContext& typesCtx) {
    THashSet<TString> seenLeftKeys;
    THashSet<TString> seenRightKeys;

    for (ui32 i = 0; i < Join->JoinKeys.size(); ++i) {
        const auto joinKeyPair = Join->JoinKeys[i];
        const auto leftKey = joinKeyPair.first.GetFullName();
        leftJoinKeys.emplace_back(leftKey);
        const auto rightKey = joinKeyPair.second.GetFullName();
        rightJoinKeys.emplace_back(rightKey);
        const bool duplicateLeftKey = !seenLeftKeys.insert(leftKey).second;
        const bool duplicateRightKey = !seenRightKeys.insert(rightKey).second;

        const auto leftKeyType = leftInputType->FindItemType(leftKey);
        const auto rightKeyType = rightInputType->FindItemType(rightKey);
        Y_ENSURE(leftKeyType && rightKeyType, "No types for join keys");

        const TTypeAnnotationNode* commonType = nullptr;
        if (joinSide == EJoinSide::Left) {
            commonType = JoinDryKeyType(outer, leftKeyType, rightKeyType, Ctx);
        } else if (joinSide == EJoinSide::Right) {
            commonType = JoinDryKeyType(outer, rightKeyType, leftKeyType, Ctx);
        } else {
            commonType = JoinCommonDryKeyType(Pos, outer, leftKeyType, rightKeyType, Ctx, typesCtx);
        }

        if (commonType) {
            if (!IsSameAnnotation(*leftKeyType, *commonType) || duplicateLeftKey) {
                const TString rename = TString("_rbo_join_key_left_") + ToString(i);
                leftColumnRemap[leftKey] = rename;
                const auto joinKey = Ctx.NewAtom(Pos, leftKey);
                const auto renameKey = Ctx.NewAtom(Pos, rename);
                remapLeft.emplace_back(joinKey, renameKey, i, commonType);
                leftJoinKeyRenames.emplace_back(rename);
            }
            if (!IsSameAnnotation(*rightKeyType, *commonType) || duplicateRightKey) {
                const TString rename = TString("_rbo_join_key_right_") + ToString(i);
                rightColumnRemap[rightKey] = rename;
                const auto joinKey = Ctx.NewAtom(Pos, rightKey);
                const auto renameKey = Ctx.NewAtom(Pos, rename);
                remapRight.emplace_back(joinKey, renameKey, i, commonType);
                rightJoinKeyRenames.emplace_back(rename);
            }
        } else {
            // FIXME: Add support for keys with diff types.
            Y_ENSURE(false, "No common types for join keys.");
        }
    }
}

TExprNode::TPtr TPhysicalJoinBuilder::SqueezeJoinInputToDict(TExprNode::TPtr input, const ui32 width, const TVector<ui32>& joinKeys, const bool withPayloads) {
    // clang-format off
    return Ctx.Builder(Pos)
        .Callable("NarrowSqueezeToDict")
            .Add(0, input)
            .Lambda(1)
                .Params("items", width)
                .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                    if (joinKeys.size() > 1U) {
                        auto list = parent.List();
                        for (ui32 i = 0U; i < joinKeys.size(); ++i)
                            list.Arg(i, "items", joinKeys[i]);
                        list.Seal();
                    } else {
                        parent.Arg("items", joinKeys.front());
                    }
                    return parent;
                })
            .Seal()
            .Lambda(2)
                .Params("items", width)
                .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                    if (withPayloads) {
                        parent
                            .List()
                                .Args("items", width)
                            .Seal();
                    } else {
                        parent
                            .Callable("Void")
                        .Seal();
                    }
                    return parent;
                })
            .Seal()
            .List(3)
                .Atom(0, "Hashed")
                .Atom(1, withPayloads ? "Many" : "One")
                .Atom(2, "Compact")
            .Seal()
        .Seal().Build();
    // clang-format on
}

TExprNode::TPtr TPhysicalJoinBuilder::BuildMapJoin(const TString& joinType, TExprNode::TPtr leftInput, TExprNode::TPtr rightInput, TVector<TCoAtom>& leftColumnIdxs,
                                                   TVector<TCoAtom>& rightColumnIdxs, TVector<TCoAtom>& leftRenames, TVector<TCoAtom>& rightRenames,
                                                   TVector<TCoAtom>& leftKeyColumnNames, TVector<TCoAtom>& rightKeyColumnNames) {
    // clang-format off
    auto rightInputArg = Build<TCoArgument>(Ctx, Pos).Name("right_input").Done();
    return Build<TCoFlatMap>(Ctx, Pos)
        .Input(rightInput)
        .Lambda<TCoLambda>()
            .Args(rightInputArg)
            .Body<TCoMapJoinCore>()
                .LeftInput(leftInput)
                .RightDict(rightInputArg.Ptr())
                .JoinKind<TCoAtom>()
                    .Value(joinType)
                .Build()
                .LeftKeysColumns<TCoAtomList>()
                    .Add(leftColumnIdxs)
                .Build()
                .RightKeysColumns<TCoAtomList>()
                    .Add(rightColumnIdxs)
                .Build()
                .LeftRenames()
                    .Add(leftRenames)
                .Build()
                .RightRenames()
                    .Add(rightRenames)
                .Build()
                .LeftKeysColumnNames<TCoAtomList>()
                    .Add(leftKeyColumnNames)
                .Build()
                .RightKeysColumnNames<TCoAtomList>()
                    .Add(rightKeyColumnNames)
                .Build()
            .Build()
        .Build()
    .Done().Ptr();
    // clang-format on
}

TExprNode::TPtr TPhysicalJoinBuilder::BuildBlockHashJoin(const TString& joinType, TExprNode::TPtr leftInput, TExprNode::TPtr rightInput,
                                                         const TVector<TCoAtom>& leftKeyColumnIdxs, const TVector<TCoAtom>& rightKeyColumnIdsx,
                                                         const TVector<TCoAtom>& leftKeyColumnNames, const TVector<TCoAtom>& rightKeyColumnNames,
                                                         bool isReverseBlockJoin) {
    TVector<TCoNameValueTuple> joinSettings;
    if (isReverseBlockJoin) {
        // clang-format off
        joinSettings.push_back(
            Build<TCoNameValueTuple>(Ctx, Pos)
                .Name().Build("BuildSide")
                .Value<TCoAtom>().Build("Left")
                .Done());
        // clang-format on
    }

    // clang-format off
    return Build<TDqBlockHashJoinCore>(Ctx, Pos)
        .LeftInput(leftInput)
        .RightInput(rightInput)
        .JoinKind<TCoAtom>()
            .Value(joinType)
        .Build()
        .LeftKeyColumns<TCoAtomList>()
            .Add(leftKeyColumnIdxs)
        .Build()
        .RightKeyColumns<TCoAtomList>()
            .Add(rightKeyColumnIdsx)
        .Build()
        .LeftKeysColumnNames<TCoAtomList>()
            .Add(leftKeyColumnNames)
        .Build()
        .RightKeysColumnNames<TCoAtomList>()
            .Add(rightKeyColumnNames)
        .Build()
        .Settings()
            .Add(joinSettings)
        .Build()
    .Done().Ptr();
    // clang-format on
}

TExprNode::TPtr TPhysicalJoinBuilder::BuildGraceJoin(const TString& joinType, TExprNode::TPtr leftInput, TExprNode::TPtr rightInput,
                                                     TVector<TCoAtom>& leftColumnIdxs, TVector<TCoAtom>& rightColumnIdxs, TVector<TCoAtom>& leftRenames,
                                                     TVector<TCoAtom>& rightRenames, TVector<TCoAtom>& leftKeyColumnNames,
                                                     TVector<TCoAtom>& rightKeyColumnNames) {
    // clang-format off
    return Build<TCoGraceJoinCore>(Ctx, Pos)
        .LeftInput(leftInput)
        .RightInput(rightInput)
        .JoinKind<TCoAtom>()
            .Value(joinType)
        .Build()
        .LeftKeysColumns<TCoAtomList>()
            .Add(leftColumnIdxs)
        .Build()
        .RightKeysColumns<TCoAtomList>()
            .Add(rightColumnIdxs)
        .Build()
        .LeftRenames()
            .Add(leftRenames)
        .Build()
        .RightRenames()
            .Add(rightRenames)
        .Build()
        .LeftKeysColumnNames<TCoAtomList>()
            .Add(leftKeyColumnNames)
        .Build()
        .RightKeysColumnNames<TCoAtomList>()
            .Add(rightKeyColumnNames)
        .Build()
        .Flags().Build()
    .Done().Ptr();
    // clang-format on
}

TExprNode::TPtr TPhysicalJoinBuilder::BuildPhysicalJoin(TExprNode::TPtr leftInput, TExprNode::TPtr rightInput, bool useBlockHashJoin, const TTypeAnnotationContext& typesCtx) {
    const TPhysicalOpProps& props = Join->Props;
    const auto leftIUs = NPhysicalConvertionUtils::GetLiveInputIUs(*Join, 0);
    const auto rightIUs = NPhysicalConvertionUtils::GetLiveInputIUs(*Join, 1);
    const auto joinType = GetValidJoinKind(Join->JoinKind);
    const bool rightSideEmpty = (joinType == "LeftSemi"sv || joinType == "LeftOnly"sv);

    const bool outer = !(joinType == "Inner"sv || joinType.EndsWith("Semi"));
    EJoinSide joinSide = EJoinSide::Both;
    if (joinType.StartsWith("Left"sv)) {
        joinSide = EJoinSide::Left;
    } else if (joinType.StartsWith("Right"sv)) {
        joinSide = EJoinSide::Right;
    }
    Y_ENSURE(props.JoinAlgo.has_value());
    const auto joinAlgo = *(props.JoinAlgo);

    const auto leftInputType = Join->GetLeftInput()->GetTypeAnn()->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
    const auto rightInputType = Join->GetRightInput()->GetTypeAnn()->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
    TModifyKeysList remapLeft;
    TModifyKeysList remapRight;
    THashMap<TString, TString> leftColumnRemap;
    THashMap<TString, TString> rightColumnRemap;
    TVector<TString> leftJoinKeys;
    TVector<TString> rightJoinKeys;
    TVector<TString> leftJoinKeyRenames;
    TVector<TString> rightJoinKeyRenames;
    TVector<TCoAtom> leftKeyColumnNames;
    TVector<TCoAtom> rightKeyColumnNames;

    PrepareJoinKeys(leftJoinKeys, rightJoinKeys, remapLeft, remapRight, leftColumnRemap, rightColumnRemap, leftJoinKeyRenames, rightJoinKeyRenames,
                    leftInputType, rightInputType, outer, joinSide, typesCtx);
    if (!remapLeft.empty()) {
        leftInput = PrepareJoinSide(leftInput, leftIUs, leftJoinKeys, remapLeft, !useBlockHashJoin && (!outer || joinSide == EJoinSide::Right));
    }
    if (!remapRight.empty()) {
        rightInput = PrepareJoinSide(rightInput, rightIUs, rightJoinKeys, remapRight, !useBlockHashJoin && (!outer || joinSide == EJoinSide::Left));
    }

    // Prepare inputs.
    const auto joinOutputs = NPhysicalConvertionUtils::BuildNameSet(NPhysicalConvertionUtils::GetLiveOutputIUs(*Join));

    TVector<TString> leftInputColumns;
    THashSet<TString> leftOutputColumns;
    for (const auto& leftCol : leftIUs) {
        const auto column = leftCol.GetFullName();
        leftInputColumns.push_back(column);
        if (joinOutputs.contains(column)) {
            leftOutputColumns.insert(column);
        }
    }
    leftInputColumns.insert(leftInputColumns.end(), leftJoinKeyRenames.begin(), leftJoinKeyRenames.end());

    TVector<TString> rightInputColumns;
    THashSet<TString> rightOutputColumns;
    for (const auto& rightCol : rightIUs) {
        const auto column = rightCol.GetFullName();
        rightInputColumns.push_back(column);
        if (joinOutputs.contains(column)) {
            rightOutputColumns.insert(column);
        }
    }
    rightInputColumns.insert(rightInputColumns.end(), rightJoinKeyRenames.begin(), rightJoinKeyRenames.end());

    // Prepare join keys.
    TVector<TCoAtom> leftColumnIdxs;
    for (const auto& leftKey : leftJoinKeys) {
        const auto leftIdx = std::distance(leftInputColumns.begin(), std::find(leftInputColumns.begin(), leftInputColumns.end(), leftKey));
        leftColumnIdxs.push_back(Build<TCoAtom>(Ctx, Pos).Value(leftIdx).Done());
        leftKeyColumnNames.push_back(Build<TCoAtom>(Ctx, Pos).Value(leftKey).Done());
    }

    TVector<TCoAtom> rightColumnIdxs;
    TVector<ui32> rightJoinKeyIdxs;
    for (const auto& rightKey : rightJoinKeys) {
        const auto rightIdx = std::distance(rightInputColumns.begin(), std::find(rightInputColumns.begin(), rightInputColumns.end(), rightKey));
        rightColumnIdxs.push_back(Build<TCoAtom>(Ctx, Pos).Value(rightIdx).Done());
        rightKeyColumnNames.push_back(Build<TCoAtom>(Ctx, Pos).Value(rightKey).Done());
        rightJoinKeyIdxs.push_back(rightIdx);
    }

    // LeftSemi/LeftOnly emit no right columns, even in same-name self-joins.
    ui32 outputIdx = 0;
    TVector<TString> joinOutputColumns;
    TVector<TCoAtom> leftRenames;
    for (ui32 i = 0; i < leftInputColumns.size(); ++i) {
        if (leftOutputColumns.contains(leftInputColumns[i])) {
            leftRenames.push_back(Build<TCoAtom>(Ctx, Pos).Value(i).Done());
            leftRenames.push_back(Build<TCoAtom>(Ctx, Pos).Value(outputIdx++).Done());
            joinOutputColumns.push_back(leftInputColumns[i]);
        }
    }

    TVector<TCoAtom> rightRenames;
    if (!rightSideEmpty) {
        for (ui32 i = 0; i < rightInputColumns.size(); ++i) {
            if (rightOutputColumns.contains(rightInputColumns[i])) {
                rightRenames.push_back(Build<TCoAtom>(Ctx, Pos).Value(i).Done());
                rightRenames.push_back(Build<TCoAtom>(Ctx, Pos).Value(outputIdx++).Done());
                joinOutputColumns.push_back(rightInputColumns[i]);
            }
        }
    }

    // clang-format off
    leftInput = Build<TCoToFlow>(Ctx, Pos)
        .Input(leftInput)
    .Done().Ptr();

    rightInput = Build<TCoToFlow>(Ctx, Pos)
        .Input(rightInput)
    .Done().Ptr();
    // clang-format on


    leftInput = NPhysicalConvertionUtils::BuildExpandMapForNarrowInput(leftInput, leftInputColumns, Ctx);
    rightInput = NPhysicalConvertionUtils::BuildExpandMapForNarrowInput(rightInput, rightInputColumns, Ctx);

    if (useBlockHashJoin) {
        // clang-format off
        leftInput = Build<TCoWideToBlocks>(Ctx, Pos)
            .Input<TCoFromFlow>()
                .Input(leftInput)
            .Build()
        .Done().Ptr();

        rightInput = Build<TCoWideToBlocks>(Ctx, Pos)
            .Input<TCoFromFlow>()
                .Input(rightInput)
            .Build()
        .Done().Ptr();
        // clang-format on
    }

    TExprNode::TPtr phyJoin;
    switch (joinAlgo) {
        case NKikimr::NKqp::EJoinAlgoType::MapJoin: {
            phyJoin = BuildMapJoin(joinType, leftInput, SqueezeJoinInputToDict(rightInput, rightInputColumns.size(), rightJoinKeyIdxs, !rightSideEmpty),
                                   leftColumnIdxs, rightColumnIdxs, leftRenames, rightRenames, leftKeyColumnNames, rightKeyColumnNames);
            break;
        }
        case NKikimr::NKqp::EJoinAlgoType::GraceJoin:
        case NKikimr::NKqp::EJoinAlgoType::ReverseBlockJoin: {
            phyJoin = useBlockHashJoin ? BuildBlockHashJoin(joinType, leftInput, rightInput, leftColumnIdxs, rightColumnIdxs, leftKeyColumnNames,
                                                            rightKeyColumnNames, joinAlgo == NKikimr::NKqp::EJoinAlgoType::ReverseBlockJoin)
                                       : BuildGraceJoin(joinType, leftInput, rightInput, leftColumnIdxs, rightColumnIdxs, leftRenames, rightRenames,
                                                        leftKeyColumnNames, rightKeyColumnNames);
            break;
        }
        default: {
            Y_ENSURE(false, "Unsupported join algo.");
            break;
        }
    }

    if (useBlockHashJoin) {
        auto inputs = leftInputColumns;
        if (!rightSideEmpty) {
            inputs.insert(inputs.end(), rightInputColumns.begin(), rightInputColumns.end());
        }

        // clang-format off
        phyJoin = Build<TCoToFlow>(Ctx, Pos)
            .Input<TCoWideFromBlocks>()
                .Input(phyJoin)
            .Build()
        .Done().Ptr();

        return Build<TCoFromFlow>(Ctx, Pos)
            .Input(NPhysicalConvertionUtils::BuildNarrowMapForWideInput(phyJoin, inputs, joinOutputs, Ctx))
        .Done().Ptr();
        // clang-format on
    }

    // clang-format off
    return Build<TCoFromFlow>(Ctx, Pos)
        .Input(NPhysicalConvertionUtils::BuildNarrowMapForWideInput(phyJoin, joinOutputColumns, joinOutputs, Ctx))
    .Done().Ptr();
    // clang-format on
}

TExprNode::TPtr TPhysicalJoinBuilder::BuildPhysicalOp(TExprNode::TPtr leftInput, TExprNode::TPtr rightInput, bool useBlockHashJoin, const TTypeAnnotationContext& typesCtx) {
    const auto joinKind = to_lower(Join->JoinKind);
    if (Join->IsLikeJoin) {
        return BuildLikeJoin(leftInput, rightInput);
    }
    if (joinKind == "cross") {
        return BuildCrossJoin(leftInput, rightInput);
    }
    if (Join->JoinKeys.empty()) {
        Y_ENSURE(joinKind == "cross" || joinKind == "inner",
                 TStringBuilder() << "Join without equi-keys is only supported for Cross/Inner, got: " << Join->JoinKind);
        return BuildCrossJoin(leftInput, rightInput);
    }

    Y_ENSURE(joinKind == "inner" || joinKind == "left" || joinKind == "leftonly" || joinKind == "leftsemi" || joinKind == "full");
    return BuildPhysicalJoin(leftInput, rightInput, useBlockHashJoin, typesCtx);
}
