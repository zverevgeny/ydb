#include "mkql_map_join.h"

#include <yql/essentials/minikql/computation/mkql_computation_node_codegen.h> // Y_IGNORE
#include <yql/essentials/minikql/computation/mkql_computation_node_holders.h>
#include <yql/essentials/minikql/computation/mkql_computation_node_holders_codegen.h>
#include <yql/essentials/minikql/mkql_node_cast.h>
#include <yql/essentials/minikql/mkql_program_builder.h>
#include <yql/essentials/minikql/invoke_builtins/mkql_builtins.h>

#include <util/string/cast.h>

namespace NKikimr {
namespace NMiniKQL {

namespace {

template <bool IsTuple>
class TWideMapJoinBase {
protected:
    TWideMapJoinBase(TComputationMutables& mutables, std::vector<TFunctionDescriptor>&& leftKeyConverters,
                     TDictType* dictType, std::vector<EValueRepresentation>&& outputRepresentations, std::vector<ui32>&& leftKeyColumns,
                     std::vector<ui32>&& leftRenames, std::vector<ui32>&& rightRenames,
                     IComputationWideFlowNode* flow, IComputationNode* dict, ui32 inputWidth)
        : LeftKeyConverters(std::move(leftKeyConverters))
        , DictType(dictType)
        , OutputRepresentations(std::move(outputRepresentations))
        , LeftKeyColumns(std::move(leftKeyColumns))
        , LeftRenames(std::move(leftRenames))
        , RightRenames(std::move(rightRenames))
        , UsedInputs(GetUsedInputs())
        , Flow(flow)
        , Dict(dict)
        , KeyTuple(mutables)
        , InputsIndex(mutables.CurValueIndex)
        , WideFieldsIndex(mutables.CurWideFieldsIndex)
    {
        mutables.DeferWideFieldsInit(inputWidth, UsedInputs);
    }

#if 0 // TODO(T5): Re-enable LLVM codegen for lambda-based MapJoinCore
    Value* GenMakeKeysTuple(Value* keysPtr, const ICodegeneratorInlineWideNode::TGettersList& getters, const TCodegenContext& ctx, BasicBlock*& block) const {
        auto& context = ctx.Codegen.GetContext();
        const auto zero = ConstantInt::get(Type::getInt128Ty(context), 0);
        const auto keys = getters[LeftKeyColumns.front()](ctx, block);
        new StoreInst(keys, keysPtr, block);
        const auto check = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_EQ, keys, zero, "check", block);
        return check;
    }

    Value* GenMakeKeysTuple(Value* keysPtr, const ICodegeneratorInlineWideNode::TGettersList& getters, Value* itemsPtr, Type* keysType, const TCodegenContext& ctx, BasicBlock*& block) const {
        auto& context = ctx.Codegen.GetContext();
        const auto idxType = Type::getInt32Ty(context);
        const auto zero = ConstantInt::get(Type::getInt128Ty(context), 0);

        const auto keys = KeyTuple.GenNewArray(LeftKeyColumns.size(), itemsPtr, ctx, block);
        const auto items = new LoadInst(PointerType::getUnqual(keysType), itemsPtr, "items", block);

        const auto done = BasicBlock::Create(context, "done", ctx.Func);
        const auto result = PHINode::Create(Type::getInt1Ty(context), (LeftKeyColumns.size() + 1U) << 1U, "result", done);

        const auto keyType = AS_TYPE(TTupleType, DictType->GetKeyType());
        for (ui32 i = 0; i < LeftKeyColumns.size(); ++i) {
            const auto index = ConstantInt::get(idxType, i);
            const auto ptr = GetElementPtrInst::CreateInBounds(keysType, items, {ConstantInt::get(idxType, 0), index}, (TString("ptr_") += ToString(i)).c_str(), block);
            const auto elem = getters[LeftKeyColumns[i]](ctx, block);
            const auto converter = reinterpret_cast<TGeneratorPtr>(LeftKeyConverters[i].Generator);
            const auto conv = converter ? converter(reinterpret_cast<Value* const*>(&elem), ctx, block) : elem;

            result->addIncoming(ConstantInt::getTrue(context), block);
            const auto next = BasicBlock::Create(context, (TString("next_") += ToString(i)).c_str(), ctx.Func);

            const auto check = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_EQ, conv, zero, "check", block);
            BranchInst::Create(done, next, check, block);

            block = next;

            new StoreInst(conv, ptr, block);
            ValueAddRef(GetValueRepresentation(keyType->GetElementType(i)), conv, ctx, block);
        }

        result->addIncoming(ConstantInt::getFalse(context), block);
        BranchInst::Create(done, block);

        block = done;
        new StoreInst(keys, keysPtr, block);
        return result;
    }

    void GenFillLeftStruct(const std::vector<Value*>& pointers, ICodegeneratorInlineWideNode::TGettersList& output) const {
        for (auto i = 0U; i < pointers.size(); ++i) {
            output[LeftRenames[(i << 1U) + 1U]] = [p = pointers[i]](const TCodegenContext& ctx, BasicBlock*& block) {
                auto& context = ctx.Codegen.GetContext();
                return new LoadInst(Type::getInt128Ty(context), p, "value", block);
            };
        }
    }

    void GenFillLeftStruct(const ICodegeneratorInlineWideNode::TGettersList& input, ICodegeneratorInlineWideNode::TGettersList& output) const {
        for (auto i = 0U; i < LeftRenames.size(); ++i) {
            const auto& src = input[LeftRenames[i]];
            output[LeftRenames[++i]] = src;
        }
    }

    bool IsUnusedInput(const ui32 index) const {
        for (auto i = 0U; i < LeftRenames.size(); ++ ++i) {
            if (LeftRenames[i] == index) {
                return false;
            }
        }
        return true;
    }

    template <class TLeftSideSource>
    std::array<Value*, 2U> GenFillOutput(ui32 idx, const TCodegenContext& ctx, const TLeftSideSource& input, ICodegeneratorInlineWideNode::TGettersList& output) const {
        GenFillLeftStruct(input, output);

        auto& context = ctx.Codegen.GetContext();

        const auto valueType = Type::getInt128Ty(context);
        const auto zero = ConstantInt::get(valueType, 0);

        auto width = 0U;
        for (auto i = 0U; i < RightRenames.size(); ++ ++i) {
            width = std::max(width, RightRenames[i]);
        }

        const auto arrayType = ArrayType::get(valueType, ++width);
        const auto atTop = &ctx.Func->getEntryBlock().back();

        const auto stub = new AllocaInst(arrayType, 0U, "stub", atTop);

        Value* zeros = UndefValue::get(arrayType);
        for (auto i = 0U; i < width; ++i) {
            zeros = InsertValueInst::Create(zeros, zero, {i}, (TString("zero_") += ToString(i)).c_str(), atTop);
        }

        new StoreInst(zeros, stub, atTop);

        const auto item = new AllocaInst(valueType, 0U, "item", atTop);
        const auto placeholder = new AllocaInst(stub->getType(), 0U, "placeholder", atTop);
        const auto pointer = GetElementPtrInst::CreateInBounds(valueType, ctx.GetMutables(), {ConstantInt::get(Type::getInt32Ty(context), idx)}, "pointer", atTop);

        for (auto i = 0U; i < RightRenames.size(); ++i) {
            const auto from = RightRenames[i];
            const auto to = RightRenames[++i];
            const auto kind = OutputRepresentations[to];
            output[to] = [from, kind, item, pointer, placeholder, arrayType, valueType](const TCodegenContext& ctx, BasicBlock*& block) {
                auto& context = ctx.Codegen.GetContext();

                const auto index = ConstantInt::get(Type::getInt32Ty(context), from);
                const auto pointerType = PointerType::getUnqual(arrayType);
                const auto elements = new LoadInst(pointerType, placeholder, "elements", block);
                const auto null = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_EQ, elements, ConstantPointerNull::get(pointerType), "null", block);

                const auto fast = BasicBlock::Create(context, "fast", ctx.Func);
                const auto slow = BasicBlock::Create(context, "slow", ctx.Func);
                const auto done = BasicBlock::Create(context, "done", ctx.Func);

                const auto out = PHINode::Create(pointer->getType(), 2U, "out", done);

                BranchInst::Create(slow, fast, null, block);

                block = fast;

                const auto ptr = GetElementPtrInst::CreateInBounds(arrayType, elements, {ConstantInt::get(Type::getInt32Ty(context), 0), index}, "ptr", block);
                out->addIncoming(ptr, block);

                BranchInst::Create(done, block);

                block = slow;

                const auto value = new LoadInst(valueType, pointer, "value", block);
                CallBoxedValueVirtualMethod<NUdf::TBoxedValueAccessor::EMethod::GetElement>(item, value, ctx.Codegen, block, index);
                ValueRelease(kind, item, ctx, block);
                out->addIncoming(item, block);

                BranchInst::Create(done, block);

                block = done;

                const auto load = new LoadInst(valueType, out, "load", block);
                return load;
            };
        }

        return {{placeholder, stub}};
    }
#endif
    NUdf::TUnboxedValue MakeKeysTuple(TComputationContext& ctx, NUdf::TUnboxedValue** fields) const {
        if constexpr (IsTuple) {
            NUdf::TUnboxedValue* items = nullptr;
            const auto keys = KeyTuple.NewArray(ctx, LeftKeyColumns.size(), items);
            if (!LeftKeyColumns.empty()) {
                Y_ABORT_UNLESS(items);
                for (auto i = 0U; i < LeftKeyColumns.size(); ++i) {
                    const auto value = fields[LeftKeyColumns[i]];
                    const auto converter = LeftKeyConverters[i].Function;
                    if (!(*items++ = converter ? converter(value) : *value)) {
                        return NUdf::TUnboxedValuePod();
                    }
                }
            }

            return keys;
        } else {
            const auto value = fields[LeftKeyColumns.front()];
            const auto converter = LeftKeyConverters.front().Function;
            return converter ? converter(value) : *value;
        }
    }

    void FillLeftStruct(NUdf::TUnboxedValue* const* output, NUdf::TUnboxedValue** fields) const {
        for (auto i = 0U; i < LeftRenames.size(); ++i) {
            const auto prevIndex = LeftRenames[i];
            const auto newIndex = LeftRenames[++i];
            if (const auto out = output[newIndex]) {
                *out = *fields[prevIndex];
            }
        }
    }

    void FillRightStruct(const NUdf::TUnboxedValue& structObj, NUdf::TUnboxedValue* const* output) const {
        if (const auto ptr = structObj.GetElements()) {
            for (auto i = 0U; i < RightRenames.size(); ++i) {
                const auto prevIndex = RightRenames[i];
                const auto newIndex = RightRenames[++i];
                if (const auto out = output[newIndex]) {
                    *out = ptr[prevIndex];
                }
            }
        } else {
            for (auto i = 0U; i < RightRenames.size(); ++i) {
                const auto prevIndex = RightRenames[i];
                const auto newIndex = RightRenames[++i];
                if (const auto out = output[newIndex]) {
                    *out = structObj.GetElement(prevIndex);
                }
            }
        }
    }

    void NullRightStruct(NUdf::TUnboxedValue* const* output) const {
        for (auto i = 0U; i < RightRenames.size(); ++i) {
            const auto newIndex = RightRenames[++i];
            if (const auto out = output[newIndex]) {
                *out = NUdf::TUnboxedValuePod();
            }
        }
    }

    std::set<ui32> GetUsedInputs() const {
        std::set<ui32> unique;
        for (auto i = 0U; i < LeftKeyColumns.size(); ++i) {
            unique.emplace(LeftKeyColumns[i]);
        }
        for (auto i = 0U; i < LeftRenames.size(); i += 2U) {
            unique.emplace(LeftRenames[i]);
        }
        return unique;
    }

    const std::vector<TFunctionDescriptor> LeftKeyConverters;
    TDictType* const DictType;
    const std::vector<EValueRepresentation> OutputRepresentations;
    const std::vector<ui32> LeftKeyColumns;
    const std::vector<ui32> LeftRenames;
    const std::vector<ui32> RightRenames;
    const std::set<ui32> UsedInputs;
    IComputationWideFlowNode* const Flow;
    IComputationNode* const Dict;

    const TContainerCacheOnContext KeyTuple;

    ui32 InputsIndex;
    ui32 WideFieldsIndex;
};

template <bool WithoutRight, bool RightRequired, bool IsTuple>
class TWideMapJoinWrapper: public TWideMapJoinBase<IsTuple>, public TStatefulWideFlowCodegeneratorNode<TWideMapJoinWrapper<WithoutRight, RightRequired, IsTuple>> {
    using TBaseComputation = TStatefulWideFlowCodegeneratorNode<TWideMapJoinWrapper<WithoutRight, RightRequired, IsTuple>>;

public:
    TWideMapJoinWrapper(TComputationMutables& mutables, std::vector<TFunctionDescriptor>&& leftKeyConverters,
                        TDictType* dictType, std::vector<EValueRepresentation>&& outputRepresentations, std::vector<ui32>&& leftKeyColumns,
                        std::vector<ui32>&& leftRenames, std::vector<ui32>&& rightRenames,
                        IComputationWideFlowNode* flow, IComputationNode* dict, ui32 inputWidth)
        : TWideMapJoinBase<IsTuple>(mutables, std::move(leftKeyConverters), dictType, std::move(outputRepresentations), std::move(leftKeyColumns), std::move(leftRenames), std::move(rightRenames), flow, dict, inputWidth)
        , TBaseComputation(mutables, flow, EValueRepresentation::Boxed)
    {
    }

    EFetchResult DoCalculate(NUdf::TUnboxedValue& lookup, TComputationContext& ctx, NUdf::TUnboxedValue* const* output) const {
        auto** fields = ctx.WideFields.data() + this->WideFieldsIndex;

        const auto dict = this->Dict->GetValue(ctx);
        do {
            if (const auto res = this->Flow->FetchValues(ctx, fields); EFetchResult::One != res) {
                return res;
            }

            const auto keys = this->MakeKeysTuple(ctx, fields);

            if constexpr (WithoutRight) {
                if ((keys && dict.Contains(keys)) == RightRequired) {
                    break;
                } else {
                    continue;
                }
            } else if (keys) {
                if (lookup = dict.Lookup(keys)) {
                    this->FillLeftStruct(output, fields);
                    this->FillRightStruct(lookup, output);
                    return EFetchResult::One;
                }
            }
        } while (RightRequired || WithoutRight);

        this->FillLeftStruct(output, fields);
        this->NullRightStruct(output);
        return EFetchResult::One;
    }
#if 0 // TODO(T5): Re-enable LLVM codegen for lambda-based MapJoinCore
    ICodegeneratorInlineWideNode::TGenerateResult DoGenGetValues(const TCodegenContext& ctx, Value* lookupPtr, BasicBlock*& block) const {
        MKQL_ENSURE(!this->Dict->IsTemporaryValue(), "Dict can't be temporary");

        auto& context = ctx.Codegen.GetContext();

        const auto valueType = Type::getInt128Ty(context);
        const auto resultType = Type::getInt32Ty(context);
        const auto zero = ConstantInt::get(valueType, 0);

        const auto keysType = IsTuple ? ArrayType::get(valueType, this->LeftKeyColumns.size()) : nullptr;
        const auto kitmsPtr = IsTuple ? new AllocaInst(PointerType::getUnqual(keysType), 0U, "kitms_ptr", &ctx.Func->getEntryBlock().back()) : nullptr;

        const auto keysPtr = new AllocaInst(valueType, 0U, "keys_ptr", &ctx.Func->getEntryBlock().back());
        new StoreInst(zero, keysPtr, block);

        const auto dict = GetNodeValue(this->Dict, ctx, block);

        const auto loop = BasicBlock::Create(context, "loop", ctx.Func);
        const auto next = BasicBlock::Create(context, "next", ctx.Func);
        const auto step = BasicBlock::Create(context, "step", ctx.Func);
        const auto stop = BasicBlock::Create(context, "stop", ctx.Func);

        const auto result = PHINode::Create(resultType, RightRequired ? 2U : 3U, "result", stop);

        BranchInst::Create(loop, block);

        block = loop;

        const auto current = GetNodeValues(this->Flow, ctx, block);
        const auto special = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_SLE, current.first, ConstantInt::get(resultType, 0), "special", block);

        result->addIncoming(current.first, block);

        BranchInst::Create(stop, next, special, block);

        block = next;

        const auto none = IsTuple ? this->GenMakeKeysTuple(keysPtr, current.second, kitmsPtr, keysType, ctx, block) : this->GenMakeKeysTuple(keysPtr, current.second, ctx, block);

        ICodegeneratorInlineWideNode::TGettersList getters(this->OutputRepresentations.size());
        if constexpr (WithoutRight) {
            this->GenFillLeftStruct(current.second, getters);

            if constexpr (RightRequired) {
                BranchInst::Create(loop, step, none, block);
            } else {
                result->addIncoming(ConstantInt::get(resultType, i32(EFetchResult::One)), block);
                BranchInst::Create(stop, step, none, block);
            }

            block = step;

            const auto cont = CallBoxedValueVirtualMethod<NUdf::TBoxedValueAccessor::EMethod::Contains>(Type::getInt1Ty(context), dict, ctx.Codegen, block, keysPtr);

            if constexpr (!IsTuple) {
                ValueCleanup(GetValueRepresentation(this->DictType->GetKeyType()), keysPtr, ctx, block);
            }

            result->addIncoming(ConstantInt::get(resultType, i32(EFetchResult::One)), block);

            if constexpr (RightRequired) {
                BranchInst::Create(stop, loop, cont, block);
            } else {
                BranchInst::Create(loop, stop, cont, block);
            }
        } else {
            const auto output = this->GenFillOutput(static_cast<const IComputationNode*>(this)->GetIndex(), ctx, current.second, getters);

            const auto left = RightRequired ? nullptr : BasicBlock::Create(context, "left", ctx.Func);

            if constexpr (RightRequired) {
                BranchInst::Create(loop, step, none, block);
            } else {
                BranchInst::Create(left, step, none, block);
            }

            block = step;

            ValueUnRef(EValueRepresentation::Boxed, lookupPtr, ctx, block);
            CallBoxedValueVirtualMethod<NUdf::TBoxedValueAccessor::EMethod::Lookup>(lookupPtr, dict, ctx.Codegen, block, keysPtr);

            const auto lookup = new LoadInst(valueType, lookupPtr, "lookup", block);

            if constexpr (!IsTuple) {
                ValueCleanup(GetValueRepresentation(this->DictType->GetKeyType()), keysPtr, ctx, block);
            }

            const auto ok = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_NE, lookup, zero, "ok", block);
            const auto full = BasicBlock::Create(context, "full", ctx.Func);

            if constexpr (RightRequired) {
                BranchInst::Create(full, loop, ok, block);
            } else {
                BranchInst::Create(full, left, ok, block);

                block = left;

                new StoreInst(std::get<1U>(output), std::get<0U>(output), block);
                result->addIncoming(ConstantInt::get(resultType, i32(EFetchResult::One)), block);

                BranchInst::Create(stop, block);
            }

            {
                block = full;

                const auto elements = CallBoxedValueVirtualMethod<NUdf::TBoxedValueAccessor::EMethod::GetElements>(std::get<1U>(output)->getType(), lookup, ctx.Codegen, block);
                new StoreInst(elements, std::get<0U>(output), block);

                result->addIncoming(ConstantInt::get(resultType, i32(EFetchResult::One)), block);

                BranchInst::Create(stop, block);
            }
        }

        block = stop;
        return {result, std::move(getters)};
    }
#endif
private:
    void RegisterDependencies() const final {
        if (const auto flow = this->FlowDependsOn(this->Flow)) {
            this->DependsOn(flow, this->Dict);
        }
    }
};

template <bool RightRequired, bool IsTuple>
class TWideMultiMapJoinWrapper: public TWideMapJoinBase<IsTuple>, public TPairStateWideFlowCodegeneratorNode<TWideMultiMapJoinWrapper<RightRequired, IsTuple>> {
    using TBaseComputation = TPairStateWideFlowCodegeneratorNode<TWideMultiMapJoinWrapper<RightRequired, IsTuple>>;
    using TBase = TWideMapJoinBase<IsTuple>;

public:
    TWideMultiMapJoinWrapper(TComputationMutables& mutables, std::vector<TFunctionDescriptor>&& leftKeyConverters,
                             TDictType* dictType, std::vector<EValueRepresentation>&& outputRepresentations, std::vector<ui32>&& leftKeyColumns,
                             std::vector<ui32>&& leftRenames, std::vector<ui32>&& rightRenames,
                             IComputationWideFlowNode* flow, IComputationNode* dict, ui32 inputWidth)
        : TWideMapJoinBase<IsTuple>(mutables, std::move(leftKeyConverters), dictType, std::move(outputRepresentations), std::move(leftKeyColumns), std::move(leftRenames), std::move(rightRenames), flow, dict, inputWidth)
        , TBaseComputation(mutables, flow, EValueRepresentation::Boxed, EValueRepresentation::Boxed)
    {
        if (!TBase::LeftRenames.empty()) {
            LeftRenamesStorageIndex = mutables.CurValueIndex;
            mutables.CurValueIndex += TBase::LeftRenames.size() >> 1U;
        }
    }

    EFetchResult DoCalculate(NUdf::TUnboxedValue& iter, NUdf::TUnboxedValue& item, TComputationContext& ctx, NUdf::TUnboxedValue* const* output) const {
        auto** fields = ctx.WideFields.data() + this->WideFieldsIndex;

        for (auto iterator = std::move(iter);;) {
            if (iterator.HasValue()) {
                if (iterator.Next(item)) {
                    this->FillLeftStruct(output, fields);
                    this->FillRightStruct(item, output);
                    iter = std::move(iterator);
                    return EFetchResult::One;
                }
            }

            for (const auto& dict = this->Dict->GetValue(ctx);;) {
                if (const auto res = this->Flow->FetchValues(ctx, fields); EFetchResult::One != res) {
                    return res;
                }

                if (const auto keys = this->MakeKeysTuple(ctx, fields)) {
                    if (const auto lookup = dict.Lookup(keys)) {
                        iterator = lookup.GetListIterator();
                        break;
                    }
                }

                if constexpr (!RightRequired) {
                    this->FillLeftStruct(output, fields);
                    this->NullRightStruct(output);
                    return EFetchResult::One;
                }
            }
        }
    }
#if 0 // TODO(T5): Re-enable LLVM codegen for lambda-based MapJoinCore
    ICodegeneratorInlineWideNode::TGenerateResult DoGenGetValues(const TCodegenContext& ctx, Value* iteraratorPtr, Value* itemPtr, BasicBlock*& block) const {
        MKQL_ENSURE(!this->Dict->IsTemporaryValue(), "Dict can't be temporary");
        auto& context = ctx.Codegen.GetContext();

        const auto resultType = Type::getInt32Ty(context);
        const auto valueType = Type::getInt128Ty(context);
        const auto zero = ConstantInt::get(valueType, 0);

        const auto keysType = IsTuple ? ArrayType::get(valueType, this->LeftKeyColumns.size()) : nullptr;
        const auto kitmsPtr = IsTuple ? new AllocaInst(PointerType::getUnqual(keysType), 0U, "kitms_ptr", &ctx.Func->getEntryBlock().back()) : nullptr;

        const auto keysPtr = new AllocaInst(valueType, 0U, "keys_ptr", &ctx.Func->getEntryBlock().back());

        std::vector<Value*> leftStoragePointers;
        leftStoragePointers.reserve(TBase::LeftRenames.size() >> 1U);
        auto i = 0U;
        const auto values = ctx.GetMutables();
        std::generate_n(std::back_inserter(leftStoragePointers), TBase::LeftRenames.size() >> 1U,
                        [&]() { return GetElementPtrInst::CreateInBounds(valueType, values, {ConstantInt::get(resultType, LeftRenamesStorageIndex + i++)}, (TString("left_out_") += ToString(i)).c_str(), &ctx.Func->getEntryBlock().back()); });

        const auto work = BasicBlock::Create(context, "work", ctx.Func);

        BranchInst::Create(work, block);

        block = work;

        const auto subiter = new LoadInst(valueType, iteraratorPtr, "subiter", block);

        const auto hasi = BasicBlock::Create(context, "hasi", ctx.Func);
        const auto loop = BasicBlock::Create(context, "loop", ctx.Func);
        const auto full = BasicBlock::Create(context, "full", ctx.Func);
        const auto skip = BasicBlock::Create(context, "loop", ctx.Func);
        const auto part = BasicBlock::Create(context, "part", ctx.Func);
        const auto next = BasicBlock::Create(context, "next", ctx.Func);
        const auto step = BasicBlock::Create(context, "step", ctx.Func);
        const auto fill = BasicBlock::Create(context, "fill", ctx.Func);
        const auto left = RightRequired ? nullptr : BasicBlock::Create(context, "left", ctx.Func);
        const auto exit = BasicBlock::Create(context, "exit", ctx.Func);

        const auto result = PHINode::Create(resultType, RightRequired ? 2U : 3U, "result", exit);

        ICodegeneratorInlineWideNode::TGettersList getters(this->OutputRepresentations.size());

        BranchInst::Create(hasi, part, HasValue(subiter, block, context), block);

        block = part;

        for (const auto ptr : leftStoragePointers) {
            new StoreInst(GetInvalid(context), ptr, block);
        }

        const auto dict = GetNodeValue(this->Dict, ctx, block);
        BranchInst::Create(loop, block);

        block = loop;

        const auto current = GetNodeValues(this->Flow, ctx, block);

        const auto output = this->GenFillOutput(static_cast<const IComputationNode*>(this)->GetIndex() + 1U, ctx, leftStoragePointers, getters);

        const auto special = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_SLE, current.first, ConstantInt::get(resultType, 0), "special", block);

        result->addIncoming(current.first, block);

        BranchInst::Create(exit, next, special, block);

        block = hasi;

        const auto status = CallBoxedValueNext(subiter, ctx, block, itemPtr);
        BranchInst::Create(full, skip, status, block);

        {
            block = full;

            const auto item = new LoadInst(valueType, itemPtr, "item", block);
            const auto elements = CallBoxedValueVirtualMethod<NUdf::TBoxedValueAccessor::EMethod::GetElements>(std::get<1U>(output)->getType(), item, ctx.Codegen, block);
            new StoreInst(elements, std::get<0U>(output), block);

            result->addIncoming(ConstantInt::get(resultType, i32(EFetchResult::One)), block);

            BranchInst::Create(exit, block);
        }

        {
            block = skip;
            UnRefBoxed(subiter, ctx, block);
            new StoreInst(zero, iteraratorPtr, block);

            for (auto i = 0U; i < leftStoragePointers.size(); ++i) {
                const auto ptr = leftStoragePointers[i];
                ValueUnRef(TBase::OutputRepresentations[TBase::LeftRenames[(i << 1U) + 1U]], ptr, ctx, block);
                new StoreInst(GetInvalid(context), ptr, block);
            }

            BranchInst::Create(part, block);
        }

        block = next;

        const auto none = IsTuple ? this->GenMakeKeysTuple(keysPtr, current.second, kitmsPtr, keysType, ctx, block) : this->GenMakeKeysTuple(keysPtr, current.second, ctx, block);

        if constexpr (RightRequired) {
            BranchInst::Create(loop, step, none, block);
        } else {
            BranchInst::Create(left, step, none, block);
        }

        block = step;

        ValueUnRef(EValueRepresentation::Boxed, itemPtr, ctx, block);
        CallBoxedValueVirtualMethod<NUdf::TBoxedValueAccessor::EMethod::Lookup>(itemPtr, dict, ctx.Codegen, block, keysPtr);

        if constexpr (!IsTuple) {
            if (this->IsUnusedInput(this->LeftKeyColumns.front())) {
                ValueCleanup(GetValueRepresentation(this->DictType->GetKeyType()), keysPtr, ctx, block);
            }
        }

        const auto lookup = new LoadInst(valueType, itemPtr, "lookup", block);
        const auto ok = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_NE, lookup, zero, "ok", block);

        if constexpr (RightRequired) {
            BranchInst::Create(fill, loop, ok, block);
        } else {
            BranchInst::Create(fill, left, ok, block);

            block = left;

            for (auto i = 0U; i < leftStoragePointers.size(); ++i) {
                const auto item = current.second[TBase::LeftRenames[i << 1U]](ctx, block);
                new StoreInst(item, leftStoragePointers[i], block);
            }

            new StoreInst(std::get<1U>(output), std::get<0U>(output), block);
            result->addIncoming(ConstantInt::get(resultType, i32(EFetchResult::One)), block);

            BranchInst::Create(exit, block);
        }

        {
            block = fill;
            CallBoxedValueVirtualMethod<NUdf::TBoxedValueAccessor::EMethod::GetListIterator>(iteraratorPtr, lookup, ctx.Codegen, block);

            for (auto i = 0U; i < leftStoragePointers.size(); ++i) {
                const auto item = current.second[TBase::LeftRenames[i << 1U]](ctx, block);
                ValueAddRef(TBase::OutputRepresentations[TBase::LeftRenames[(i << 1U) + 1U]], item, ctx, block);
                new StoreInst(item, leftStoragePointers[i], block);
            }

            BranchInst::Create(work, block);
        }

        block = exit;
        return {result, std::move(getters)};
    }
#endif
private:
    void RegisterDependencies() const final {
        if (const auto flow = this->FlowDependsOn(this->Flow)) {
            this->DependsOn(flow, this->Dict);
        }
    }

    ui32 LeftRenamesStorageIndex = 0U;
};

template <bool IsTuple>
class TMapJoinCoreWrapperBase {
protected:
    TMapJoinCoreWrapperBase(TComputationMutables& mutables, std::vector<TFunctionDescriptor>&& leftKeyConverters,
                            TType* keyType, std::vector<EValueRepresentation>&& outputRepresentations, std::vector<ui32>&& leftKeyColumns,
                            std::vector<ui32>&& leftRenames, std::vector<ui32>&& rightRenames, IComputationNode* stream,
                            IComputationExternalNode* keyArg, IComputationNode* lookupBody)
        : LeftKeyConverters(std::move(leftKeyConverters))
        , KeyType(keyType)
        , OutputRepresentations(std::move(outputRepresentations))
        , LeftKeyColumns(std::move(leftKeyColumns))
        , LeftRenames(std::move(leftRenames))
        , RightRenames(std::move(rightRenames))
        , Stream(stream)
        , KeyArg(keyArg)
        , LookupBody(lookupBody)
        , ResStruct(mutables)
        , KeyTuple(mutables)
    {
    }

    // Invoke the right-side lambda with `key` and return the result.
    NUdf::TUnboxedValue LookupKey(TComputationContext& ctx, NUdf::TUnboxedValue&& key) const {
        KeyArg->SetValue(ctx, std::move(key));
        return LookupBody->GetValue(ctx);
    }

    static void FillStruct(const NUdf::TUnboxedValue& structObj, NUdf::TUnboxedValue* items, const std::vector<ui32>& renames) {
        if (renames.empty()) {
            return;
        }
        Y_ABORT_UNLESS(items);
        if (const auto ptr = structObj.GetElements()) {
            for (auto i = 0U; i < renames.size();) {
                const auto prevIndex = renames[i++];
                const auto newIndex = renames[i++];
                items[newIndex] = ptr[prevIndex];
            }
        } else {
            for (auto i = 0U; i < renames.size();) {
                const auto prevIndex = renames[i++];
                const auto newIndex = renames[i++];
                items[newIndex] = structObj.GetElement(prevIndex);
            }
        }
    }

    void FillLeftStruct(const NUdf::TUnboxedValue& structObj, NUdf::TUnboxedValue* items) const {
        FillStruct(structObj, items, LeftRenames);
    }

    void FillRightStruct(const NUdf::TUnboxedValue& structObj, NUdf::TUnboxedValue* items) const {
        FillStruct(structObj, items, RightRenames);
    }

    NUdf::TUnboxedValue MakeKeysTuple(TComputationContext& ctx, const NUdf::TUnboxedValuePod& structObj) const {
        if (IsTuple) {
            NUdf::TUnboxedValue* items = nullptr;
            const auto keys = KeyTuple.NewArray(ctx, LeftKeyColumns.size(), items);
            if (!LeftKeyColumns.empty()) {
                Y_ABORT_UNLESS(items);
                const auto ptr = structObj.GetElements();
                for (auto i = 0U; i < LeftKeyColumns.size(); ++i) {
                    auto value = ptr ? ptr[LeftKeyColumns[i]] : structObj.GetElement(LeftKeyColumns[i]);
                    const auto converter = LeftKeyConverters[i].Function;
                    if (!(*items++ = converter ? NUdf::TUnboxedValue(converter(&value)) : std::move(value))) {
                        return NUdf::TUnboxedValuePod();
                    }
                }
            }

            return keys;
        } else {
            const auto value = structObj.GetElement(LeftKeyColumns.front());
            const auto converter = LeftKeyConverters.front().Function;
            return converter ? NUdf::TUnboxedValue(converter(&value)) : value;
        }
    }
#if 0 // TODO(T5 stage 2): Re-enable LLVM codegen for lambda-based MapJoinCore
    void GenFillLeftStruct(Value* left, Value* items, Type* arrayType, const TCodegenContext& ctx, BasicBlock*& block) const {
        auto& context = ctx.Codegen.GetContext();
        const auto idxType = Type::getInt32Ty(context);
        const auto valType = Type::getInt128Ty(context);
        const auto ptrType = PointerType::getUnqual(valType);
        const auto elements = CallBoxedValueVirtualMethod<NUdf::TBoxedValueAccessor::EMethod::GetElements>(ptrType, left, ctx.Codegen, block);

        const auto null = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_EQ, elements, ConstantPointerNull::get(ptrType), "null", block);

        const auto fast = BasicBlock::Create(context, "fast", ctx.Func);
        const auto slow = BasicBlock::Create(context, "slow", ctx.Func);
        const auto done = BasicBlock::Create(context, "done", ctx.Func);

        BranchInst::Create(slow, fast, null, block);
        {
            block = fast;
            for (auto i = 0U; i < LeftRenames.size();) {
                const auto oldI = LeftRenames[i++];
                const auto newI = LeftRenames[i++];
                const auto oldIndex = ConstantInt::get(idxType, oldI);
                const auto newIndex = ConstantInt::get(idxType, newI);
                const auto oldPtr = GetElementPtrInst::CreateInBounds(valType, elements, {oldIndex}, "old", block);
                const auto newPtr = GetElementPtrInst::CreateInBounds(arrayType, items, {ConstantInt::get(idxType, 0), newIndex}, "new", block);
                const auto item = new LoadInst(valType, oldPtr, "item", block);
                new StoreInst(item, newPtr, block);
                ValueAddRef(OutputRepresentations[newI], newPtr, ctx, block);
            }
            BranchInst::Create(done, block);
        }
        {
            block = slow;
            for (auto i = 0U; i < LeftRenames.size();) {
                const auto oldI = LeftRenames[i++];
                const auto newI = LeftRenames[i++];
                const auto oldIndex = ConstantInt::get(idxType, oldI);
                const auto newIndex = ConstantInt::get(idxType, newI);
                const auto item = GetElementPtrInst::CreateInBounds(arrayType, items, {ConstantInt::get(idxType, 0), newIndex}, "item", block);
                CallBoxedValueVirtualMethod<NUdf::TBoxedValueAccessor::EMethod::GetElement>(item, left, ctx.Codegen, block, oldIndex);
            }
            BranchInst::Create(done, block);
        }
        block = done;
    }

    void GenFillRightStruct(Value* right, Value* items, Type* arrayType, const TCodegenContext& ctx, BasicBlock*& block) const {
        auto& context = ctx.Codegen.GetContext();
        const auto idxType = Type::getInt32Ty(context);
        const auto valType = Type::getInt128Ty(context);
        const auto ptrType = PointerType::getUnqual(valType);
        const auto elements = CallBoxedValueVirtualMethod<NUdf::TBoxedValueAccessor::EMethod::GetElements>(ptrType, right, ctx.Codegen, block);

        const auto null = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_EQ, elements, ConstantPointerNull::get(ptrType), "null", block);

        const auto fast = BasicBlock::Create(context, "fast", ctx.Func);
        const auto slow = BasicBlock::Create(context, "slow", ctx.Func);
        const auto done = BasicBlock::Create(context, "done", ctx.Func);

        BranchInst::Create(slow, fast, null, block);
        {
            block = fast;
            for (auto i = 0U; i < RightRenames.size();) {
                const auto oldI = RightRenames[i++];
                const auto newI = RightRenames[i++];
                const auto oldIndex = ConstantInt::get(idxType, oldI);
                const auto newIndex = ConstantInt::get(idxType, newI);
                const auto oldPtr = GetElementPtrInst::CreateInBounds(valType, elements, {oldIndex}, "old", block);
                const auto newPtr = GetElementPtrInst::CreateInBounds(arrayType, items, {ConstantInt::get(idxType, 0), newIndex}, "new", block);
                const auto elem = new LoadInst(valType, oldPtr, "elem", block);
                new StoreInst(elem, newPtr, block);
                ValueAddRef(OutputRepresentations[newI], newPtr, ctx, block);
            }
            BranchInst::Create(done, block);
        }
        {
            block = slow;
            for (auto i = 0U; i < RightRenames.size();) {
                const auto oldI = RightRenames[i++];
                const auto newI = RightRenames[i++];
                const auto oldIndex = ConstantInt::get(idxType, oldI);
                const auto newIndex = ConstantInt::get(idxType, newI);
                const auto item = GetElementPtrInst::CreateInBounds(arrayType, items, {ConstantInt::get(idxType, 0), newIndex}, "item", block);
                CallBoxedValueVirtualMethod<NUdf::TBoxedValueAccessor::EMethod::GetElement>(item, right, ctx.Codegen, block, oldIndex);
            }
            BranchInst::Create(done, block);
        }
        block = done;
    }

    Value* GenMakeKeysTuple(Value* keysPtr, Value* current, const TCodegenContext& ctx, BasicBlock*& block) const {
        auto& context = ctx.Codegen.GetContext();
        const auto idxType = Type::getInt32Ty(context);
        const auto zero = ConstantInt::get(Type::getInt128Ty(context), 0);

        const auto index = ConstantInt::get(idxType, LeftKeyColumns.front());
        CallBoxedValueVirtualMethod<NUdf::TBoxedValueAccessor::EMethod::GetElement>(keysPtr, current, ctx.Codegen, block, index);
        if (const auto converter = reinterpret_cast<TGeneratorPtr>(LeftKeyConverters.front().Generator)) {
            Value* const elem = new LoadInst(Type::getInt128Ty(context), keysPtr, "elem", block);
            const auto conv = converter(&elem, ctx, block);
            new StoreInst(conv, keysPtr, block);
            const auto check = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_EQ, conv, zero, "check", block);
            return check;
        } else {
            const auto keys = new LoadInst(Type::getInt128Ty(context), keysPtr, "keys", block);
            const auto check = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_EQ, keys, zero, "check", block);
            return check;
        }
    }

    Value* GenMakeKeysTuple(Value* keysPtr, Value* current, Value* itemsPtr, Type* keysType, const TCodegenContext& ctx, BasicBlock*& block) const {
        auto& context = ctx.Codegen.GetContext();
        const auto idxType = Type::getInt32Ty(context);
        const auto valueType = Type::getInt128Ty(context);
        const auto zero = ConstantInt::get(valueType, 0);

        const auto keys = KeyTuple.GenNewArray(LeftKeyColumns.size(), itemsPtr, ctx, block);
        const auto items = new LoadInst(PointerType::getUnqual(keysType), itemsPtr, "items", block);

        const auto ptrType = PointerType::getUnqual(valueType);
        const auto elements = CallBoxedValueVirtualMethod<NUdf::TBoxedValueAccessor::EMethod::GetElements>(ptrType, current, ctx.Codegen, block);

        const auto null = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_EQ, elements, ConstantPointerNull::get(ptrType), "null", block);

        const auto fast = BasicBlock::Create(context, "fast", ctx.Func);
        const auto slow = BasicBlock::Create(context, "slow", ctx.Func);
        const auto done = BasicBlock::Create(context, "done", ctx.Func);
        const auto result = PHINode::Create(Type::getInt1Ty(context), (LeftKeyColumns.size() + 1U) << 1U, "result", done);

        BranchInst::Create(slow, fast, null, block);
        {
            block = fast;

            const auto keyType = AS_TYPE(TTupleType, DictType->GetKeyType());
            for (ui32 i = 0; i < LeftKeyColumns.size(); ++i) {
                const auto oldIndex = ConstantInt::get(idxType, LeftKeyColumns[i]);
                const auto newIndex = ConstantInt::get(idxType, i);
                const auto oldPtr = GetElementPtrInst::CreateInBounds(valueType, elements, {oldIndex}, "old", block);
                const auto newPtr = GetElementPtrInst::CreateInBounds(keysType, items, {ConstantInt::get(idxType, 0), newIndex}, "new", block);
                const auto elem = new LoadInst(valueType, oldPtr, "elem", block);
                const auto converter = reinterpret_cast<TGeneratorPtr>(LeftKeyConverters[i].Generator);
                const auto conv = converter ? converter(reinterpret_cast<Value* const*>(&elem), ctx, block) : elem;

                result->addIncoming(ConstantInt::getTrue(context), block);
                const auto next = BasicBlock::Create(context, (TString("next_") += ToString(i)).c_str(), ctx.Func);

                const auto check = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_EQ, conv, zero, "check", block);
                BranchInst::Create(done, next, check, block);

                block = next;

                new StoreInst(conv, newPtr, block);
                ValueAddRef(GetValueRepresentation(keyType->GetElementType(i)), newPtr, ctx, block);
            }

            result->addIncoming(ConstantInt::getFalse(context), block);
            BranchInst::Create(done, block);
        }
        {
            block = slow;

            for (ui32 i = 0; i < LeftKeyColumns.size(); ++i) {
                const auto item = GetElementPtrInst::CreateInBounds(keysType, items, {ConstantInt::get(idxType, 0), ConstantInt::get(idxType, i)}, "item", block);
                const auto index = ConstantInt::get(idxType, LeftKeyColumns[i]);
                CallBoxedValueVirtualMethod<NUdf::TBoxedValueAccessor::EMethod::GetElement>(item, current, ctx.Codegen, block, index);

                const auto next = BasicBlock::Create(context, (TString("next_") += ToString(i)).c_str(), ctx.Func);
                const auto elem = new LoadInst(valueType, item, "elem", block);

                if (const auto converter = reinterpret_cast<TGeneratorPtr>(LeftKeyConverters[i].Generator)) {
                    const auto conv = converter(reinterpret_cast<Value* const*>(&elem), ctx, block);
                    new StoreInst(conv, item, block);
                    const auto check = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_EQ, conv, zero, "check", block);
                    result->addIncoming(ConstantInt::getTrue(context), block);
                    BranchInst::Create(done, next, check, block);
                } else {
                    const auto check = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_EQ, elem, zero, "check", block);
                    result->addIncoming(ConstantInt::getTrue(context), block);
                    BranchInst::Create(done, next, check, block);
                }

                block = next;
            }

            result->addIncoming(ConstantInt::getFalse(context), block);
            BranchInst::Create(done, block);
        }

        block = done;
        new StoreInst(keys, keysPtr, block);
        return result;
    }
#endif

    const std::vector<TFunctionDescriptor> LeftKeyConverters;
    TType* const KeyType;
    const std::vector<EValueRepresentation> OutputRepresentations;
    const std::vector<ui32> LeftKeyColumns;
    const std::vector<ui32> LeftRenames;
    const std::vector<ui32> RightRenames;
    IComputationNode* const Stream;
    IComputationExternalNode* const KeyArg;
    IComputationNode* const LookupBody;

    const TContainerCacheOnContext ResStruct;
    const TContainerCacheOnContext KeyTuple;
};

enum class ERightKind {
    None = 0,
    Once,
    Many
};

template <ERightKind RightKind, bool RightRequired, bool IsTuple>
class TMapJoinCoreFlowWrapper: public TMapJoinCoreWrapperBase<IsTuple>, public std::conditional_t<ERightKind::Many != RightKind,
                                                                                                  TStatelessFlowCodegeneratorNode<TMapJoinCoreFlowWrapper<RightKind, RightRequired, IsTuple>>,
                                                                                                  TPairStateFlowCodegeneratorNode<TMapJoinCoreFlowWrapper<RightKind, RightRequired, IsTuple>>> {
    typedef std::conditional_t<ERightKind::Many != RightKind,
                               TStatelessFlowCodegeneratorNode<TMapJoinCoreFlowWrapper<RightKind, RightRequired, IsTuple>>,
                               TPairStateFlowCodegeneratorNode<TMapJoinCoreFlowWrapper<RightKind, RightRequired, IsTuple>>>
        TBaseComputation;

public:
    TMapJoinCoreFlowWrapper(TComputationMutables& mutables, EValueRepresentation kind, std::vector<TFunctionDescriptor>&& leftKeyConverters,
                            TType* keyType, std::vector<EValueRepresentation>&& outputRepresentations, std::vector<ui32>&& leftKeyColumns,
                            std::vector<ui32>&& leftRenames, std::vector<ui32>&& rightRenames,
                            IComputationNode* flow, IComputationExternalNode* keyArg, IComputationNode* lookupBody)
        : TMapJoinCoreWrapperBase<IsTuple>(mutables, std::move(leftKeyConverters), keyType, std::move(outputRepresentations),
                                           std::move(leftKeyColumns), std::move(leftRenames), std::move(rightRenames), flow, keyArg, lookupBody)
        , TBaseComputation(mutables, flow, kind)
    {
    }

    NUdf::TUnboxedValuePod DoCalculate(TComputationContext& ctx) const {
        for (;;) {
            auto item = this->Stream->GetValue(ctx);
            if (item.IsSpecial()) {
                return item.Release();
            }

            auto keys = this->MakeKeysTuple(ctx, item);

            switch (RightKind) {
                case ERightKind::Once:
                    if (keys) {
                        if (const auto lookup = this->LookupKey(ctx, std::move(keys))) {
                            NUdf::TUnboxedValue* items = nullptr;
                            const auto result = this->ResStruct.NewArray(ctx, this->OutputRepresentations.size(), items);
                            this->FillLeftStruct(item, items);
                            this->FillRightStruct(lookup, items);
                            return result;
                        }
                    }

                    if constexpr (RightRequired) {
                        continue;
                    } else {
                        break;
                    }

                case ERightKind::None:
                    if constexpr (RightRequired) {
                        if (keys && this->LookupKey(ctx, std::move(keys)).HasValue()) {
                            break;
                        } else {
                            continue;
                        }
                    } else {
                        if (keys && this->LookupKey(ctx, std::move(keys)).HasValue()) {
                            continue;
                        } else {
                            break;
                        }
                    }
                default:
                    Y_ABORT("Unreachable");
            }

            NUdf::TUnboxedValue* items = nullptr;
            const auto result = this->ResStruct.NewArray(ctx, this->OutputRepresentations.size(), items);
            this->FillLeftStruct(item, items);
            return result;
        }
    }

    NUdf::TUnboxedValuePod DoCalculate(NUdf::TUnboxedValue& curr, NUdf::TUnboxedValue& iter, TComputationContext& ctx) const {
        for (auto iterator = std::move(iter);;) {
            if (iterator.HasValue() && curr.HasValue()) {
                if (NUdf::TUnboxedValue item; iterator.Next(item)) {
                    NUdf::TUnboxedValue* items = nullptr;
                    const auto result = this->ResStruct.NewArray(ctx, this->OutputRepresentations.size(), items);
                    this->FillLeftStruct(curr, items);
                    this->FillRightStruct(item, items);
                    iter = std::move(iterator);
                    return result;
                }
            }

            for (auto current = std::move(curr);;) {
                current = this->Stream->GetValue(ctx);
                if (current.IsSpecial()) {
                    return current.Release();
                }

                if (auto keys = this->MakeKeysTuple(ctx, current)) {
                    if (const auto lookup = this->LookupKey(ctx, std::move(keys))) {
                        iterator = lookup.GetListIterator();
                        curr = std::move(current);
                        break;
                    }
                }

                if constexpr (!RightRequired) {
                    NUdf::TUnboxedValue* items = nullptr;
                    const auto result = this->ResStruct.NewArray(ctx, this->OutputRepresentations.size(), items);
                    this->FillLeftStruct(current, items);
                    return result;
                }
            }
        }
    }
#if 0 // TODO(T5 stage 2): Re-enable LLVM codegen for lambda-based MapJoinCore
    Value* DoGenerateGetValue(const TCodegenContext& ctx, BasicBlock*& block) const {
        auto& context = ctx.Codegen.GetContext();

        const auto valueType = Type::getInt128Ty(context);
        const auto zero = ConstantInt::get(valueType, 0);

        const auto arrayType = ArrayType::get(valueType, this->OutputRepresentations.size());
        const auto keysType = IsTuple ? ArrayType::get(valueType, this->LeftKeyColumns.size()) : nullptr;

        const auto itemsType = PointerType::getUnqual(arrayType);
        const auto itemsPtr = new AllocaInst(itemsType, 0U, "items_ptr", &ctx.Func->getEntryBlock().back());
        const auto kitmsPtr = IsTuple ? new AllocaInst(PointerType::getUnqual(keysType), 0U, "kitms_ptr", &ctx.Func->getEntryBlock().back()) : nullptr;

        const auto keysPtr = new AllocaInst(valueType, 0U, "keys_ptr", &ctx.Func->getEntryBlock().back());
        new StoreInst(zero, keysPtr, block);
        const auto itemPtr = new AllocaInst(valueType, 0U, "item_ptr", &ctx.Func->getEntryBlock().back());
        new StoreInst(zero, itemPtr, block);

        const auto dict = GetNodeValue(this->Dict, ctx, block);

        const auto loop = BasicBlock::Create(context, "loop", ctx.Func);
        const auto stop = BasicBlock::Create(context, "stop", ctx.Func);
        const auto result = PHINode::Create(valueType, 3U, "result", stop);
        BranchInst::Create(loop, block);

        block = loop;

        const auto current = GetNodeValue(this->Stream, ctx, block);
        result->addIncoming(current, block);

        const auto next = BasicBlock::Create(context, "next", ctx.Func);
        BranchInst::Create(stop, next, IsSpecial(current, block, context), block);
        block = next;

        const auto none = IsTuple ? this->GenMakeKeysTuple(keysPtr, current, kitmsPtr, keysType, ctx, block) : this->GenMakeKeysTuple(keysPtr, current, ctx, block);

        const auto skip = BasicBlock::Create(context, "skip", ctx.Func);
        const auto step = BasicBlock::Create(context, "step", ctx.Func);
        const auto half = BasicBlock::Create(context, "half", ctx.Func);

        BranchInst::Create(RightRequired ? skip : half, step, none, block);

        block = step;

        switch (RightKind) {
            case ERightKind::None: {
                const auto cont = CallBoxedValueVirtualMethod<NUdf::TBoxedValueAccessor::EMethod::Contains>(Type::getInt1Ty(context), dict, ctx.Codegen, block, keysPtr);

                if constexpr (!IsTuple) {
                    ValueUnRef(GetValueRepresentation(this->DictType->GetKeyType()), keysPtr, ctx, block);
                }

                if constexpr (RightRequired) {
                    BranchInst::Create(half, skip, cont, block);
                } else {
                    BranchInst::Create(skip, half, cont, block);
                }
                break;
            }
            case ERightKind::Once: {
                new StoreInst(zero, itemPtr, block);
                CallBoxedValueVirtualMethod<NUdf::TBoxedValueAccessor::EMethod::Lookup>(itemPtr, dict, ctx.Codegen, block, keysPtr);

                if constexpr (!IsTuple) {
                    ValueUnRef(GetValueRepresentation(this->DictType->GetKeyType()), keysPtr, ctx, block);
                }

                const auto lookup = new LoadInst(valueType, itemPtr, "lookup", block);
                const auto ok = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_NE, lookup, zero, "ok", block);

                const auto full = BasicBlock::Create(context, "full", ctx.Func);

                BranchInst::Create(full, RightRequired ? skip : half, ok, block);

                {
                    block = full;

                    const auto out = this->ResStruct.GenNewArray(this->OutputRepresentations.size(), itemsPtr, ctx, block);
                    const auto items = new LoadInst(itemsType, itemsPtr, "items", block);

                    this->GenFillLeftStruct(current, items, arrayType, ctx, block);
                    this->GenFillRightStruct(lookup, items, arrayType, ctx, block);

                    UnRefBoxed(lookup, ctx, block);
                    ValueCleanup(static_cast<const IComputationNode*>(this)->GetRepresentation(), current, ctx, block);

                    result->addIncoming(out, block);
                    BranchInst::Create(stop, block);
                }
                break;
            }
            case ERightKind::Many:
                Y_ABORT("Wrong case");
        }

        {
            block = half;

            const auto out = this->ResStruct.GenNewArray(this->OutputRepresentations.size(), itemsPtr, ctx, block);
            const auto items = new LoadInst(itemsType, itemsPtr, "items", block);

            this->GenFillLeftStruct(current, items, arrayType, ctx, block);

            ValueCleanup(static_cast<const IComputationNode*>(this)->GetRepresentation(), current, ctx, block);

            result->addIncoming(out, block);
            BranchInst::Create(stop, block);
        }

        {
            block = skip;
            ValueCleanup(static_cast<const IComputationNode*>(this)->GetRepresentation(), current, ctx, block);
            BranchInst::Create(loop, block);
        }

        block = stop;
        if (this->Dict->IsTemporaryValue()) {
            CleanupBoxed(dict, ctx, block);
        }
        return result;
    }

    Value* DoGenerateGetValue(const TCodegenContext& ctx, Value* currentPtr, Value* iteraratorPtr, BasicBlock*& block) const {
        Y_ABORT("MapJoinCore codegen not yet updated for lambda right side");
        auto& context = ctx.Codegen.GetContext();

        const auto valueType = Type::getInt128Ty(context);
        const auto zero = ConstantInt::get(valueType, 0);

        const auto arrayType = ArrayType::get(valueType, this->OutputRepresentations.size());
        const auto keysType = IsTuple ? ArrayType::get(valueType, this->LeftKeyColumns.size()) : nullptr;

        const auto itemsType = PointerType::getUnqual(arrayType);
        const auto itemsPtr = new AllocaInst(itemsType, 0U, "items_ptr", &ctx.Func->getEntryBlock().back());
        const auto kitmsPtr = IsTuple ? new AllocaInst(PointerType::getUnqual(keysType), 0U, "kitms_ptr", &ctx.Func->getEntryBlock().back()) : nullptr;

        const auto keysPtr = new AllocaInst(valueType, 0U, "keys_ptr", &ctx.Func->getEntryBlock().back());
        const auto itemPtr = new AllocaInst(valueType, 0U, "item_ptr", &ctx.Func->getEntryBlock().back());
        new StoreInst(zero, itemPtr, block);

        const auto work = BasicBlock::Create(context, "work", ctx.Func);
        BranchInst::Create(work, block);

        block = work;

        const auto subiter = new LoadInst(valueType, iteraratorPtr, "subiter", block);

        const auto hasi = BasicBlock::Create(context, "hasi", ctx.Func);
        const auto loop = BasicBlock::Create(context, "loop", ctx.Func);
        const auto full = BasicBlock::Create(context, "full", ctx.Func);
        const auto skip = BasicBlock::Create(context, "loop", ctx.Func);
        const auto part = BasicBlock::Create(context, "part", ctx.Func);
        const auto next = BasicBlock::Create(context, "next", ctx.Func);
        const auto stop = BasicBlock::Create(context, "stop", ctx.Func);
        const auto exit = BasicBlock::Create(context, "exit", ctx.Func);
        const auto result = PHINode::Create(valueType, 3U, "result", exit);

        BranchInst::Create(hasi, part, HasValue(subiter, block, context), block);

        block = hasi;
        const auto curr = new LoadInst(valueType, currentPtr, "curr", block);
        const auto status = CallBoxedValueNext(subiter, ctx, block, itemPtr);
        BranchInst::Create(full, skip, status, block);

        {
            block = full;

            const auto out = this->ResStruct.GenNewArray(this->OutputRepresentations.size(), itemsPtr, ctx, block);
            const auto items = new LoadInst(itemsType, itemsPtr, "items", block);
            const auto item = new LoadInst(valueType, itemPtr, "item", block);

            this->GenFillLeftStruct(curr, items, arrayType, ctx, block);
            this->GenFillRightStruct(item, items, arrayType, ctx, block);

            UnRefBoxed(item, ctx, block);

            result->addIncoming(out, block);
            BranchInst::Create(exit, block);
        }

        {
            block = skip;
            UnRefBoxed(curr, ctx, block);
            UnRefBoxed(subiter, ctx, block);
            new StoreInst(zero, currentPtr, block);
            new StoreInst(zero, iteraratorPtr, block);
            BranchInst::Create(part, block);
        }

        block = part;
        const auto dict = GetNodeValue(this->Dict, ctx, block);
        BranchInst::Create(loop, block);

        block = loop;
        GetNodeValue(currentPtr, this->Stream, ctx, block);
        const auto current = new LoadInst(valueType, currentPtr, "current", block);
        BranchInst::Create(stop, next, IsSpecial(current, block, context), block);

        block = stop;

        if (this->Dict->IsTemporaryValue()) {
            CleanupBoxed(dict, ctx, block);
        }
        result->addIncoming(current, block);

        BranchInst::Create(exit, block);

        block = next;
        const auto none = IsTuple ? this->GenMakeKeysTuple(keysPtr, current, kitmsPtr, keysType, ctx, block) : this->GenMakeKeysTuple(keysPtr, current, ctx, block);

        const auto step = BasicBlock::Create(context, "step", ctx.Func);
        const auto hsnt = BasicBlock::Create(context, "half", ctx.Func);

        BranchInst::Create(hsnt, step, none, block);

        block = step;

        new StoreInst(zero, itemPtr, block);
        CallBoxedValueVirtualMethod<NUdf::TBoxedValueAccessor::EMethod::Lookup>(itemPtr, dict, ctx.Codegen, block, keysPtr);

        if constexpr (!IsTuple) {
            ValueUnRef(GetValueRepresentation(this->DictType->GetKeyType()), keysPtr, ctx, block);
        }

        const auto lookup = new LoadInst(valueType, itemPtr, "lookup", block);
        const auto ok = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_NE, lookup, zero, "ok", block);

        const auto fill = BasicBlock::Create(context, "fill", ctx.Func);

        BranchInst::Create(fill, hsnt, ok, block);

        block = hsnt;

        if constexpr (RightRequired) {
            UnRefBoxed(current, ctx, block);
            new StoreInst(zero, currentPtr, block);
            BranchInst::Create(loop, block);
        } else {
            const auto out = this->ResStruct.GenNewArray(this->OutputRepresentations.size(), itemsPtr, ctx, block);
            const auto items = new LoadInst(itemsType, itemsPtr, "items", block);

            this->GenFillLeftStruct(current, items, arrayType, ctx, block);
            UnRefBoxed(current, ctx, block);
            new StoreInst(zero, currentPtr, block);

            if (this->Dict->IsTemporaryValue()) {
                CleanupBoxed(dict, ctx, block);
            }

            result->addIncoming(out, block);
            BranchInst::Create(exit, block);
        }

        {
            block = fill;
            CallBoxedValueVirtualMethod<NUdf::TBoxedValueAccessor::EMethod::GetListIterator>(iteraratorPtr, lookup, ctx.Codegen, block);
            CleanupBoxed(lookup, ctx, block);
            BranchInst::Create(work, block);
        }

        block = exit;
        return result;
    }
#endif
private:
    void RegisterDependencies() const final {
        if (const auto flow = this->FlowDependsOn(this->Stream)) {
            this->DependsOn(flow, this->LookupBody);
        }
    }

    // Empty stubs: LLVM codegen disabled for lambda-based MapJoinCore (T5 stage 2)
    void GenerateFunctions(NYql::NCodegen::ICodegen& /*codegen*/) {
    }

    void FinalizeFunctions(NYql::NCodegen::ICodegen& /*codegen*/) {
    }

#ifndef MKQL_DISABLE_CODEGEN
    // Stub for DoGenerateGetValue (non-Many case)
    Value* DoGenerateGetValue(const TCodegenContext& /*ctx*/, BasicBlock*& /*block*/) const {
        Y_UNREACHABLE();
    }

    // Stub for DoGenerateGetValue (Many case)
    Value* DoGenerateGetValue(const TCodegenContext& /*ctx*/, Value* /*firstPtr*/, Value* /*secondPtr*/, BasicBlock*& /*block*/) const {
        Y_UNREACHABLE();
    }
#endif
};

template <ERightKind RightKind, bool RightRequired, bool IsTuple>
class TMapJoinCoreWrapper: public TMapJoinCoreWrapperBase<IsTuple>, public TCustomValueCodegeneratorNode<TMapJoinCoreWrapper<RightKind, RightRequired, IsTuple>> {
private:
    typedef TCustomValueCodegeneratorNode<TMapJoinCoreWrapper<RightKind, RightRequired, IsTuple>> TBaseComputation;

    // NOTE: These codegen value classes are temporarily disabled for stage 1 (interpreter-only).
    // They will be properly updated in stage 2 to use lambda invocation instead of Dict lookup.
    class TCodegenValue: public TComputationValue<TCodegenValue> {
    public:
        using TBase = TComputationValue<TCodegenValue>;

        using TFetchPtr = NUdf::EFetchStatus (*)(TComputationContext*, NUdf::TUnboxedValuePod, IComputationExternalNode*, IComputationNode*, NUdf::TUnboxedValuePod&);

        TCodegenValue(TMemoryUsageInfo* memInfo, TFetchPtr fetch, TComputationContext* ctx, NUdf::TUnboxedValue&& stream, IComputationExternalNode* keyArg, IComputationNode* lookupBody)
            : TBase(memInfo)
            , FetchFunc(fetch)
            , Ctx(ctx)
            , Stream(std::move(stream))
            , KeyArg(keyArg)
            , LookupBody(lookupBody)
        {
        }

    private:
        NUdf::EFetchStatus Fetch(NUdf::TUnboxedValue& result) final {
            return FetchFunc(Ctx, static_cast<const NUdf::TUnboxedValuePod&>(Stream), KeyArg, LookupBody, result);
        }

        const TFetchPtr FetchFunc;
        TComputationContext* const Ctx;
        const NUdf::TUnboxedValue Stream;
        IComputationExternalNode* const KeyArg;
        IComputationNode* const LookupBody;
    };

    class TCodegenStatefulValue: public TComputationValue<TCodegenStatefulValue> {
    public:
        using TBase = TComputationValue<TCodegenStatefulValue>;

        using TFetchPtr = NUdf::EFetchStatus (*)(TComputationContext*, NUdf::TUnboxedValuePod, IComputationExternalNode*, IComputationNode*, NUdf::TUnboxedValuePod&, NUdf::TUnboxedValuePod&, NUdf::TUnboxedValuePod&);

        TCodegenStatefulValue(TMemoryUsageInfo* memInfo, TFetchPtr fetch, TComputationContext* ctx, NUdf::TUnboxedValue&& stream, IComputationExternalNode* keyArg, IComputationNode* lookupBody)
            : TBase(memInfo)
            , FetchFunc(fetch)
            , Ctx(ctx)
            , Stream(std::move(stream))
            , KeyArg(keyArg)
            , LookupBody(lookupBody)
        {
        }

    private:
        NUdf::EFetchStatus Fetch(NUdf::TUnboxedValue& result) final {
            return FetchFunc(Ctx, static_cast<const NUdf::TUnboxedValuePod&>(Stream), KeyArg, LookupBody, Current, Iterator, result);
        }

        const TFetchPtr FetchFunc;
        TComputationContext* const Ctx;
        const NUdf::TUnboxedValue Stream;
        IComputationExternalNode* const KeyArg;
        IComputationNode* const LookupBody;

        NUdf::TUnboxedValue Current;
        NUdf::TUnboxedValue Iterator;
    };

    using TSelf = TMapJoinCoreWrapper<RightKind, RightRequired, IsTuple>;
    using TBase = TCustomValueCodegeneratorNode<TSelf>;

    class TValue: public TComputationValue<TValue> {
    public:
        using TBase = TComputationValue<TValue>;

        TValue(TMemoryUsageInfo* memInfo, NUdf::TUnboxedValue&& stream,
               IComputationExternalNode* keyArg, IComputationNode* lookupBody, TComputationContext& ctx, const TSelf* self)
            : TBase(memInfo)
            , Stream(std::move(stream))
            , KeyArg(keyArg)
            , LookupBody(lookupBody)
            , Ctx(ctx)
            , Self(self)
        {
        }

    private:
        NUdf::EFetchStatus Fetch(NUdf::TUnboxedValue& result) override {
            for (NUdf::TUnboxedValue current;;) {
                if (const auto status = Stream.Fetch(current); status != NUdf::EFetchStatus::Ok) {
                    return status;
                }

                auto keys = Self->MakeKeysTuple(Ctx, current);

                switch (RightKind) {
                    case ERightKind::Once:
                        if (keys) {
                            KeyArg->SetValue(Ctx, std::move(keys));
                            if (const auto lookup = LookupBody->GetValue(Ctx)) {
                                NUdf::TUnboxedValue* items = nullptr;
                                result = Self->ResStruct.NewArray(Ctx, Self->OutputRepresentations.size(), items);
                                Self->FillLeftStruct(current, items);
                                Self->FillRightStruct(lookup, items);
                                return NUdf::EFetchStatus::Ok;
                            }
                        }

                        if constexpr (RightRequired) {
                            continue;
                        } else {
                            break;
                        }

                    case ERightKind::None:
                        if constexpr (RightRequired) {
                            if (keys) {
                                KeyArg->SetValue(Ctx, std::move(keys));
                                if (LookupBody->GetValue(Ctx).HasValue()) {
                                    break;
                                } else {
                                    continue;
                                }
                            } else {
                                continue;
                            }
                        } else {
                            if (keys) {
                                KeyArg->SetValue(Ctx, std::move(keys));
                                if (LookupBody->GetValue(Ctx).HasValue()) {
                                    continue;
                                } else {
                                    break;
                                }
                            } else {
                                break;
                            }
                        }
                    default:
                        Y_ABORT("Unreachable");
                }

                NUdf::TUnboxedValue* items = nullptr;
                result = Self->ResStruct.NewArray(Ctx, Self->OutputRepresentations.size(), items);
                Self->FillLeftStruct(current, items);
                return NUdf::EFetchStatus::Ok;
            }
        }

    private:
        NUdf::TUnboxedValue Stream;
        IComputationExternalNode* const KeyArg;
        IComputationNode* const LookupBody;
        TComputationContext& Ctx;
        const TSelf* const Self;
    };

    class TMultiRowValue: public TComputationValue<TMultiRowValue> {
    public:
        using TBase = TComputationValue<TMultiRowValue>;

        TMultiRowValue(TMemoryUsageInfo* memInfo, NUdf::TUnboxedValue&& stream,
                       IComputationExternalNode* keyArg, IComputationNode* lookupBody, TComputationContext& ctx, const TSelf* self)
            : TBase(memInfo)
            , Stream(std::move(stream))
            , KeyArg(keyArg)
            , LookupBody(lookupBody)
            , Ctx(ctx)
            , Self(self)
        {
        }

    private:
        NUdf::EFetchStatus Fetch(NUdf::TUnboxedValue& result) override {
            for (auto iterator = std::move(Iterator);;) {
                if (iterator && Current) {
                    if (NUdf::TUnboxedValue item; iterator.Next(item)) {
                        NUdf::TUnboxedValue* items = nullptr;
                        result = Self->ResStruct.NewArray(Ctx, Self->OutputRepresentations.size(), items);
                        Self->FillLeftStruct(Current, items);
                        Self->FillRightStruct(item, items);
                        Iterator = std::move(iterator);
                        return NUdf::EFetchStatus::Ok;
                    }
                }

                for (auto current = std::move(Current);;) {
                    if (const auto status = Stream.Fetch(current); NUdf::EFetchStatus::Ok != status) {
                        return status;
                    }

                    if (auto keys = Self->MakeKeysTuple(Ctx, current)) {
                        KeyArg->SetValue(Ctx, std::move(keys));
                        if (const auto lookup = LookupBody->GetValue(Ctx)) {
                            iterator = lookup.GetListIterator();
                            Current = std::move(current);
                            break;
                        }
                    }

                    if (!RightRequired) {
                        NUdf::TUnboxedValue* items = nullptr;
                        result = Self->ResStruct.NewArray(Ctx, Self->OutputRepresentations.size(), items);
                        Self->FillLeftStruct(current, items);
                        return NUdf::EFetchStatus::Ok;
                    }
                }
            }
        }

    private:
        NUdf::TUnboxedValue Stream;
        IComputationExternalNode* const KeyArg;
        IComputationNode* const LookupBody;
        TComputationContext& Ctx;
        const TSelf* const Self;

        NUdf::TUnboxedValue Current;
        NUdf::TUnboxedValue Iterator;
    };

    using TMyCodegenValue = std::conditional_t<ERightKind::Many == RightKind, TCodegenStatefulValue, TCodegenValue>;

public:
    TMapJoinCoreWrapper(TComputationMutables& mutables, std::vector<TFunctionDescriptor>&& leftKeyConverters,
                        TType* keyType, std::vector<EValueRepresentation>&& outputRepresentations, std::vector<ui32>&& leftKeyColumns,
                        std::vector<ui32>&& leftRenames, std::vector<ui32>&& rightRenames,
                        IComputationNode* stream, IComputationExternalNode* keyArg, IComputationNode* lookupBody)
        : TMapJoinCoreWrapperBase<IsTuple>(mutables, std::move(leftKeyConverters), keyType, std::move(outputRepresentations),
                                           std::move(leftKeyColumns), std::move(leftRenames), std::move(rightRenames), stream, keyArg, lookupBody)
        , TBaseComputation(mutables)
    {
    }

    NUdf::TUnboxedValuePod DoCalculate(TComputationContext& ctx) const {
        return ctx.HolderFactory.Create<std::conditional_t<ERightKind::Many == RightKind, TMultiRowValue, TValue>>(this->Stream->GetValue(ctx), this->KeyArg, this->LookupBody, ctx, this);
    }

private:
    void RegisterDependencies() const final {
        this->DependsOn(this->Stream);
        this->DependsOn(this->LookupBody);
    }

    // Empty stubs: LLVM codegen disabled for lambda-based MapJoinCore (T5 stage 2)
    void GenerateFunctions(NYql::NCodegen::ICodegen& /*codegen*/) {
    }

    void FinalizeFunctions(NYql::NCodegen::ICodegen& /*codegen*/) {
    }

#if 0  // TODO(T5 stage 2): Re-enable LLVM codegen for lambda-based MapJoinCore
    Function* GenerateMapper(NYql::NCodegen::ICodegen& codegen) const {
        auto& module = codegen.GetModule();
        auto& context = codegen.GetContext();

        const auto& name = TBaseComputation::MakeName("Fetch");
        if (const auto f = module.getFunction(name.c_str())) {
            return f;
        }

        const auto valueType = Type::getInt128Ty(context);
        const auto arrayType = ArrayType::get(valueType, this->OutputRepresentations.size());
        const auto keysType = IsTuple ? ArrayType::get(valueType, this->LeftKeyColumns.size()) : nullptr;
        const auto containerType = static_cast<Type*>(valueType);
        const auto contextType = GetCompContextType(context);
        const auto statusType = Type::getInt32Ty(context);
        const auto funcType = FunctionType::get(statusType, {PointerType::getUnqual(contextType), containerType, containerType, PointerType::getUnqual(valueType)}, false);

        TCodegenContext ctx(codegen);
        ctx.Func = cast<Function>(module.getOrInsertFunction(name.c_str(), funcType).getCallee());

        DISubprogramAnnotator annotator(ctx, ctx.Func);

        auto args = ctx.Func->arg_begin();

        ctx.Ctx = &*args;
        const auto streamArg = &*++args;
        const auto dictArg = &*++args;
        const auto valuePtr = &*++args;

        const auto main = BasicBlock::Create(context, "main", ctx.Func);
        auto block = main;

        const auto stream = static_cast<Value*>(streamArg);

        const auto dict = static_cast<Value*>(dictArg);

        const auto zero = ConstantInt::get(valueType, 0);
        const auto fsok = ConstantInt::get(statusType, static_cast<ui32>(NUdf::EFetchStatus::Ok));

        const auto itemsType = PointerType::getUnqual(arrayType);
        const auto itemsPtr = new AllocaInst(itemsType, 0U, "items_ptr", block);
        const auto kitmsPtr = IsTuple ? new AllocaInst(PointerType::getUnqual(keysType), 0U, "kitms_ptr", block) : nullptr;

        const auto keysPtr = new AllocaInst(valueType, 0U, "keys_ptr", block);
        new StoreInst(zero, keysPtr, block);
        const auto itemPtr = new AllocaInst(valueType, 0U, "item_ptr", block);
        new StoreInst(zero, itemPtr, block);

        const auto loop = BasicBlock::Create(context, "loop", ctx.Func);
        const auto stop = BasicBlock::Create(context, "stop", ctx.Func);
        BranchInst::Create(loop, block);

        block = loop;
        const auto status = CallBoxedValueFetch(stream, ctx, block, itemPtr);
        ReturnInst::Create(context, status, stop);

        const auto stat = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_NE, status, fsok, "stat", block);

        const auto next = BasicBlock::Create(context, "next", ctx.Func);
        BranchInst::Create(stop, next, stat, block);
        block = next;

        const auto current = new LoadInst(valueType, itemPtr, "current", block);
        const auto none = IsTuple ? this->GenMakeKeysTuple(keysPtr, current, kitmsPtr, keysType, ctx, block) : this->GenMakeKeysTuple(keysPtr, current, ctx, block);

        const auto skip = BasicBlock::Create(context, "skip", ctx.Func);
        const auto step = BasicBlock::Create(context, "step", ctx.Func);
        const auto half = BasicBlock::Create(context, "half", ctx.Func);

        BranchInst::Create(RightRequired ? skip : half, step, none, block);

        block = step;
        switch (RightKind) {
            case ERightKind::None: {
                const auto cont = CallBoxedValueVirtualMethod<NUdf::TBoxedValueAccessor::EMethod::Contains>(Type::getInt1Ty(context), dict, codegen, block, keysPtr);

                if constexpr (!IsTuple) {
                    ValueUnRef(GetValueRepresentation(this->DictType->GetKeyType()), keysPtr, ctx, block);
                }

                if constexpr (RightRequired) {
                    BranchInst::Create(half, skip, cont, block);
                } else {
                    BranchInst::Create(skip, half, cont, block);
                }
                break;
            }

            case ERightKind::Once: {
                new StoreInst(zero, itemPtr, block);
                CallBoxedValueVirtualMethod<NUdf::TBoxedValueAccessor::EMethod::Lookup>(itemPtr, dict, codegen, block, keysPtr);

                if constexpr (!IsTuple) {
                    ValueUnRef(GetValueRepresentation(this->DictType->GetKeyType()), keysPtr, ctx, block);
                }

                const auto lookup = new LoadInst(valueType, itemPtr, "lookup", block);
                const auto ok = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_NE, lookup, zero, "ok", block);

                const auto full = BasicBlock::Create(context, "full", ctx.Func);

                BranchInst::Create(full, RightRequired ? skip : half, ok, block);

                {
                    block = full;

                    const auto result = this->ResStruct.GenNewArray(this->OutputRepresentations.size(), itemsPtr, ctx, block);
                    const auto items = new LoadInst(itemsType, itemsPtr, "items", block);

                    this->GenFillLeftStruct(current, items, arrayType, ctx, block);
                    this->GenFillRightStruct(lookup, items, arrayType, ctx, block);

                    SafeUnRefUnboxedOne(valuePtr, ctx, block);
                    AddRefBoxed(result, ctx, block);
                    new StoreInst(result, valuePtr, block);

                    UnRefBoxed(current, ctx, block);
                    UnRefBoxed(lookup, ctx, block);

                    ReturnInst::Create(context, fsok, block);
                }
                break;
            }
            case ERightKind::Many:
                Y_ABORT("Wrong case");
        }

        {
            block = half;

            const auto result = this->ResStruct.GenNewArray(this->OutputRepresentations.size(), itemsPtr, ctx, block);
            const auto items = new LoadInst(itemsType, itemsPtr, "items", block);

            this->GenFillLeftStruct(current, items, arrayType, ctx, block);

            SafeUnRefUnboxedOne(valuePtr, ctx, block);
            AddRefBoxed(result, ctx, block);
            new StoreInst(result, valuePtr, block);

            UnRefBoxed(current, ctx, block);

            ReturnInst::Create(context, fsok, block);
        }

        {
            block = skip;
            UnRefBoxed(current, ctx, block);
            new StoreInst(zero, itemPtr, block);
            BranchInst::Create(loop, block);
        }

        return ctx.Func;
    }

    Function* GenerateStatefulMapper(NYql::NCodegen::ICodegen& codegen) const {
        auto& module = codegen.GetModule();
        auto& context = codegen.GetContext();

        const auto& name = TBaseComputation::MakeName("Fetch");
        if (const auto f = module.getFunction(name.c_str())) {
            return f;
        }

        const auto valueType = Type::getInt128Ty(context);
        const auto arrayType = ArrayType::get(valueType, this->OutputRepresentations.size());
        const auto keysType = IsTuple ? ArrayType::get(valueType, this->LeftKeyColumns.size()) : nullptr;
        const auto containerType = static_cast<Type*>(valueType);
        const auto contextType = GetCompContextType(context);
        const auto statusType = Type::getInt32Ty(context);
        const auto funcType = FunctionType::get(statusType, {PointerType::getUnqual(contextType), containerType, containerType, PointerType::getUnqual(valueType), PointerType::getUnqual(valueType), PointerType::getUnqual(valueType)}, false);

        TCodegenContext ctx(codegen);
        ctx.Func = cast<Function>(module.getOrInsertFunction(name.c_str(), funcType).getCallee());

        DISubprogramAnnotator annotator(ctx, ctx.Func);

        auto args = ctx.Func->arg_begin();

        ctx.Ctx = &*args;
        const auto streamArg = &*++args;
        const auto dictArg = &*++args;
        const auto currentArg = &*++args;
        const auto iteratorArg = &*++args;
        const auto valuePtr = &*++args;

        const auto main = BasicBlock::Create(context, "main", ctx.Func);
        auto block = main;

        const auto stream = static_cast<Value*>(streamArg);

        const auto dict = static_cast<Value*>(dictArg);

        const auto zero = ConstantInt::get(valueType, 0);
        const auto fsok = ConstantInt::get(statusType, static_cast<ui32>(NUdf::EFetchStatus::Ok));

        const auto itemsType = PointerType::getUnqual(arrayType);
        const auto itemsPtr = new AllocaInst(itemsType, 0U, "items_ptr", block);
        const auto kitmsPtr = IsTuple ? new AllocaInst(PointerType::getUnqual(keysType), 0U, "kitms_ptr", block) : nullptr;

        const auto keysPtr = new AllocaInst(valueType, 0U, "keys_ptr", block);
        const auto itemPtr = new AllocaInst(valueType, 0U, "item_ptr", block);
        new StoreInst(zero, itemPtr, block);

        const auto work = BasicBlock::Create(context, "work", ctx.Func);
        BranchInst::Create(work, block);

        block = work;

        const auto subiter = new LoadInst(valueType, iteratorArg, "subiter", block);

        const auto hasi = BasicBlock::Create(context, "hasi", ctx.Func);
        const auto loop = BasicBlock::Create(context, "loop", ctx.Func);
        const auto full = BasicBlock::Create(context, "full", ctx.Func);
        const auto skip = BasicBlock::Create(context, "loop", ctx.Func);

        const auto ichk = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_NE, subiter, zero, "ichk", block);
        BranchInst::Create(hasi, loop, ichk, block);

        {
            block = hasi;
            const auto status = CallBoxedValueNext(subiter, ctx, block, itemPtr);
            BranchInst::Create(full, skip, status, block);
        }

        {
            block = skip;
            UnRefBoxed(subiter, ctx, block);
            new StoreInst(zero, iteratorArg, block);
            BranchInst::Create(loop, block);
        }

        {
            block = full;

            const auto result = this->ResStruct.GenNewArray(this->OutputRepresentations.size(), itemsPtr, ctx, block);
            const auto items = new LoadInst(itemsType, itemsPtr, "items", block);
            const auto curr = new LoadInst(valueType, currentArg, "curr", block);
            const auto item = new LoadInst(valueType, itemPtr, "item", block);

            this->GenFillLeftStruct(curr, items, arrayType, ctx, block);
            this->GenFillRightStruct(item, items, arrayType, ctx, block);

            SafeUnRefUnboxedOne(valuePtr, ctx, block);
            AddRefBoxed(result, ctx, block);
            new StoreInst(result, valuePtr, block);

            UnRefBoxed(item, ctx, block);

            ReturnInst::Create(context, fsok, block);
        }

        const auto next = BasicBlock::Create(context, "next", ctx.Func);
        const auto stop = BasicBlock::Create(context, "stop", ctx.Func);

        {
            block = loop;
            const auto status = CallBoxedValueFetch(stream, ctx, block, currentArg);
            ReturnInst::Create(context, status, stop);

            const auto stat = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_NE, status, fsok, "stat", block);
            BranchInst::Create(stop, next, stat, block);
        }

        {
            block = next;
            const auto current = new LoadInst(valueType, currentArg, "current", block);

            const auto none = IsTuple ? this->GenMakeKeysTuple(keysPtr, current, kitmsPtr, keysType, ctx, block) : this->GenMakeKeysTuple(keysPtr, current, ctx, block);

            const auto step = BasicBlock::Create(context, "step", ctx.Func);
            const auto hsnt = RightRequired ? loop : BasicBlock::Create(context, "half", ctx.Func);

            BranchInst::Create(hsnt, step, none, block);

            block = step;

            new StoreInst(zero, itemPtr, block);
            CallBoxedValueVirtualMethod<NUdf::TBoxedValueAccessor::EMethod::Lookup>(itemPtr, dict, ctx.Codegen, block, keysPtr);

            if constexpr (!IsTuple) {
                ValueUnRef(GetValueRepresentation(this->DictType->GetKeyType()), keysPtr, ctx, block);
            }

            const auto lookup = new LoadInst(valueType, itemPtr, "lookup", block);
            const auto ok = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_NE, lookup, zero, "ok", block);

            const auto fill = BasicBlock::Create(context, "fill", ctx.Func);

            BranchInst::Create(fill, hsnt, ok, block);

            if constexpr (!RightRequired) {
                block = hsnt;

                const auto result = this->ResStruct.GenNewArray(this->OutputRepresentations.size(), itemsPtr, ctx, block);
                const auto items = new LoadInst(itemsType, itemsPtr, "items", block);

                this->GenFillLeftStruct(current, items, arrayType, ctx, block);

                SafeUnRefUnboxedOne(valuePtr, ctx, block);
                AddRefBoxed(result, ctx, block);
                new StoreInst(result, valuePtr, block);

                ReturnInst::Create(context, fsok, block);
            }

            {
                block = fill;
                CallBoxedValueVirtualMethod<NUdf::TBoxedValueAccessor::EMethod::GetListIterator>(iteratorArg, lookup, codegen, block);
                CleanupBoxed(lookup, ctx, block);
                BranchInst::Create(work, block);
            }
        }

        return ctx.Func;
    }

    using TMapJoinPtr = typename TMyCodegenValue::TFetchPtr;

    Function* MapJoinFunc = nullptr;

    TMapJoinPtr MapJoin = nullptr;
#endif // LLVM codegen disabled
};

} // namespace

IComputationNode* WrapMapJoinCore(TCallable& callable, const TComputationNodeFactoryContext& ctx) {
    MKQL_ENSURE(callable.GetInputsCount() == 7, "Expected 7 args (flow, lambda, lambdaBody, joinKind, leftKeyColumns, leftRenames, rightRenames)");

    const auto type = callable.GetType()->GetReturnType();
    const auto leftStreamNode = callable.GetInput(0);
    const auto leftItemType = leftStreamNode.GetStaticType()->IsFlow() ? AS_TYPE(TFlowType, leftStreamNode)->GetItemType() : AS_TYPE(TStreamType, leftStreamNode)->GetItemType();
    const auto lambdaBodyNode = callable.GetInput(2);
    const auto lambdaReturnType = lambdaBodyNode.GetStaticType();
    const auto joinKindNode = callable.GetInput(3);
    const auto rawKind = AS_VALUE(TDataLiteral, joinKindNode)->AsValue().Get<ui32>();
    const auto kind = GetJoinKind(rawKind);
    const bool isMany = lambdaReturnType->IsList();
    Y_UNUSED(EJoinKind::LeftOnly);
    Y_UNUSED(EJoinKind::LeftSemi);
    const auto returnItemType = type->IsFlow() ? AS_TYPE(TFlowType, callable.GetType()->GetReturnType())->GetItemType() : AS_TYPE(TStreamType, callable.GetType()->GetReturnType())->GetItemType();
    const auto leftKeyColumnsNode = AS_VALUE(TTupleLiteral, callable.GetInput(4));
    const bool isTupleKey = leftKeyColumnsNode->GetValuesCount() > 1;
    const auto leftRenamesNode = AS_VALUE(TTupleLiteral, callable.GetInput(5));
    const auto rightRenamesNode = AS_VALUE(TTupleLiteral, callable.GetInput(6));

    std::vector<ui32> leftKeyColumns, leftRenames, rightRenames;

    leftKeyColumns.reserve(leftKeyColumnsNode->GetValuesCount());
    for (ui32 i = 0; i < leftKeyColumnsNode->GetValuesCount(); ++i) {
        leftKeyColumns.emplace_back(AS_VALUE(TDataLiteral, leftKeyColumnsNode->GetValue(i))->AsValue().Get<ui32>());
    }

    leftRenames.reserve(leftRenamesNode->GetValuesCount());
    for (ui32 i = 0; i < leftRenamesNode->GetValuesCount(); ++i) {
        leftRenames.emplace_back(AS_VALUE(TDataLiteral, leftRenamesNode->GetValue(i))->AsValue().Get<ui32>());
    }

    rightRenames.reserve(rightRenamesNode->GetValuesCount());
    for (ui32 i = 0; i < rightRenamesNode->GetValuesCount(); ++i) {
        rightRenames.emplace_back(AS_VALUE(TDataLiteral, rightRenamesNode->GetValue(i))->AsValue().Get<ui32>());
    }

    // Key type is derived from the left item type (the lambda key matches the left key).
    TType* keyType = nullptr;
    if (isTupleKey) {
        std::vector<TType*> keyTypes;
        keyTypes.reserve(leftKeyColumns.size());
        for (const auto idx : leftKeyColumns) {
            keyTypes.push_back(leftItemType->IsTuple() ? AS_TYPE(TTupleType, leftItemType)->GetElementType(idx) : (leftItemType->IsMulti() ? AS_TYPE(TMultiType, leftItemType)->GetElementType(idx) : AS_TYPE(TStructType, leftItemType)->GetMemberType(idx)));
        }
        keyType = TTupleType::Create(keyTypes.size(), keyTypes.data(), ctx.Env);
    } else {
        keyType = leftItemType->IsTuple() ? AS_TYPE(TTupleType, leftItemType)->GetElementType(leftKeyColumns[0]) : (leftItemType->IsMulti() ? AS_TYPE(TMultiType, leftItemType)->GetElementType(leftKeyColumns[0]) : AS_TYPE(TStructType, leftItemType)->GetMemberType(leftKeyColumns[0]));
    }

    std::vector<TFunctionDescriptor> leftKeyConverters;
    leftKeyConverters.resize(leftKeyColumns.size());
    // Key converters are no-ops because the lambda key type matches the left key type.

    std::vector<EValueRepresentation> outputRepresentations;
    if (returnItemType->IsTuple()) {
        const auto tupleType = AS_TYPE(TTupleType, returnItemType);
        outputRepresentations.reserve(tupleType->GetElementsCount());
        for (ui32 i = 0U; i < tupleType->GetElementsCount(); ++i) {
            outputRepresentations.emplace_back(GetValueRepresentation(tupleType->GetElementType(i)));
        }
    } else if (returnItemType->IsMulti()) {
        const auto multiType = AS_TYPE(TMultiType, returnItemType);
        outputRepresentations.reserve(multiType->GetElementsCount());
        for (ui32 i = 0U; i < multiType->GetElementsCount(); ++i) {
            outputRepresentations.emplace_back(GetValueRepresentation(multiType->GetElementType(i)));
        }
    } else if (returnItemType->IsStruct()) {
        const auto structType = AS_TYPE(TStructType, returnItemType);
        outputRepresentations.reserve(structType->GetMembersCount());
        for (ui32 i = 0U; i < structType->GetMembersCount(); ++i) {
            outputRepresentations.emplace_back(GetValueRepresentation(structType->GetMemberType(i)));
        }
    }

    const auto flow = LocateNode(ctx.NodeLocator, callable, 0);
    const auto keyArg = LocateExternalNode(ctx.NodeLocator, callable, 1);
    const auto lookupBody = LocateNode(ctx.NodeLocator, callable, 2);

#define NEW_WRAPPER(KIND, RIGHT_REQ, IS_TUPLE)                                                                                                                                   \
    if (type->IsFlow()) {                                                                                                                                                        \
        if (const auto wide = dynamic_cast<IComputationWideFlowNode*>(flow)) {                                                                                                   \
            Y_UNUSED(wide);                                                                                                                                                      \
            /* Wide path not yet updated for lambda right side */                                                                                                                \
            Y_ABORT("Wide MapJoin not yet supported with lambda right side");                                                                                                    \
        } else                                                                                                                                                                   \
            return new TMapJoinCoreFlowWrapper<KIND, RIGHT_REQ, IS_TUPLE>(ctx.Mutables, GetValueRepresentation(type),                                                            \
                                                                          std::move(leftKeyConverters), keyType, std::move(outputRepresentations),                               \
                                                                          std::move(leftKeyColumns), std::move(leftRenames), std::move(rightRenames), flow, keyArg, lookupBody); \
    } else {                                                                                                                                                                     \
        return new TMapJoinCoreWrapper<KIND, RIGHT_REQ, IS_TUPLE>(ctx.Mutables,                                                                                                  \
                                                                  std::move(leftKeyConverters), keyType, std::move(outputRepresentations),                                       \
                                                                  std::move(leftKeyColumns), std::move(leftRenames), std::move(rightRenames), flow, keyArg, lookupBody);         \
    }

#define MAKE_WRAPPER(IS_TUPLE)                                  \
    switch (kind) {                                             \
        case EJoinKind::Inner:                                  \
            if (isMany) {                                       \
                NEW_WRAPPER(ERightKind::Many, true, IS_TUPLE);  \
            } else {                                            \
                NEW_WRAPPER(ERightKind::Once, true, IS_TUPLE);  \
            }                                                   \
        case EJoinKind::Left:                                   \
            if (isMany) {                                       \
                NEW_WRAPPER(ERightKind::Many, false, IS_TUPLE); \
            } else {                                            \
                NEW_WRAPPER(ERightKind::Once, false, IS_TUPLE); \
            }                                                   \
        case EJoinKind::LeftOnly:                               \
            NEW_WRAPPER(ERightKind::None, false, IS_TUPLE);     \
        case EJoinKind::LeftSemi:                               \
            NEW_WRAPPER(ERightKind::None, true, IS_TUPLE);      \
        default:                                                \
            ythrow yexception() << "Unsupported join kind";     \
    }

    if (isTupleKey) {
        MAKE_WRAPPER(true)
    } else {
        MAKE_WRAPPER(false)
    }

#undef MAKE_WRAPPER
#undef NEW_WRAPPER
}

} // namespace NMiniKQL
} // namespace NKikimr
