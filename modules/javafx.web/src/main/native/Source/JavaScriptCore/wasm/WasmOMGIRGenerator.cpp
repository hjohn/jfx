/*
 * Copyright (C) 2016-2024 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WasmOMGIRGenerator.h"

#if ENABLE(WEBASSEMBLY_OMGJIT)

#include "AirCode.h"
#include "AllowMacroScratchRegisterUsageIf.h"
#include "B3BasicBlockInlines.h"
#include "B3CCallValue.h"
#include "B3Const128Value.h"
#include "B3ConstPtrValue.h"
#include "B3EstimateStaticExecutionCounts.h"
#include "B3FixSSA.h"
#include "B3Generate.h"
#include "B3InsertionSet.h"
#include "B3SIMDValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3SwitchValue.h"
#include "B3UpsilonValue.h"
#include "B3Validate.h"
#include "B3ValueInlines.h"
#include "B3ValueKey.h"
#include "B3Variable.h"
#include "B3VariableValue.h"
#include "B3WasmAddressValue.h"
#include "B3WasmBoundsCheckValue.h"
#include "CompilerTimingScope.h"
#include "FunctionAllowlist.h"
#include "JSCJSValueInlines.h"
#include "JSWebAssemblyArray.h"
#include "JSWebAssemblyInstance.h"
#include "JSWebAssemblyStruct.h"
#include "ProbeContext.h"
#include "ScratchRegisterAllocator.h"
#include "WasmBranchHints.h"
#include "WasmCallingConvention.h"
#include "WasmContext.h"
#include "WasmExceptionType.h"
#include "WasmFunctionParser.h"
#include "WasmIRGeneratorHelpers.h"
#include "WasmMemory.h"
#include "WasmOSREntryData.h"
#include "WasmOpcodeOrigin.h"
#include "WasmOperations.h"
#include "WasmSIMDOpcodes.h"
#include "WasmThunks.h"
#include "WasmTypeDefinitionInlines.h"
#include <limits>
#include <wtf/FastMalloc.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

#if !ENABLE(WEBASSEMBLY)
#error ENABLE(WEBASSEMBLY_OMGJIT) is enabled, but ENABLE(WEBASSEMBLY) is not.
#endif

void dumpProcedure(void* ptr)
{
    JSC::B3::Procedure* proc = static_cast<JSC::B3::Procedure*>(ptr);
    proc->dump(WTF::dataFile());
}

#if USE(JSVALUE64)

namespace JSC { namespace Wasm {

using namespace B3;

namespace WasmOMGIRGeneratorInternal {
static constexpr bool verbose = false;
static constexpr bool verboseInlining = false;
static constexpr bool traceExecution = false;
static constexpr bool traceStackValues = false;
static constexpr bool verboseTailCalls = false;
#if ASSERT_ENABLED
static constexpr bool traceExecutionIncludesConstructionSite = false;
#endif

template<typename ValueType>
struct HeapRecorder;
template<typename ValueType>
struct ValueAppender;
}

#define TRACE_VALUE(...) do { if constexpr (WasmOMGIRGeneratorInternal::traceExecution) { traceValue(__VA_ARGS__); } } while (0)

#define TRACE_CF(...) do { if constexpr (WasmOMGIRGeneratorInternal::traceExecution) { traceCF(__VA_ARGS__); } } while (0)

class OMGIRGenerator {
    WTF_MAKE_TZONE_ALLOCATED(OMGIRGenerator);
public:
    using ExpressionType = Variable*;
    using ResultList = Vector<ExpressionType, 8>;
    using CallType = CallLinkInfo::CallType;
    using CallPatchpointData = std::tuple<B3::PatchpointValue*, Box<PatchpointExceptionHandle>, RefPtr<B3::StackmapGenerator>>;

    static constexpr bool tierSupportsSIMD = true;

    struct ControlData {
        ControlData(Procedure& proc, Origin origin, BlockSignature signature, BlockType type, unsigned stackSize, BasicBlock* continuation, BasicBlock* special = nullptr)
            : controlBlockType(type)
            , m_signature(signature)
            , m_stackSize(stackSize)
            , continuation(continuation)
            , special(special)
        {
            ASSERT(type != BlockType::Try && type != BlockType::Catch);
            if (type != BlockType::TopLevel)
                m_stackSize -= signature->argumentCount();

            if (type == BlockType::Loop) {
                for (unsigned i = 0; i < signature->argumentCount(); ++i)
                    phis.append(proc.add<Value>(Phi, toB3Type(signature->argumentType(i)), origin));
            } else {
                for (unsigned i = 0; i < signature->returnCount(); ++i)
                    phis.append(proc.add<Value>(Phi, toB3Type(signature->returnType(i)), origin));
            }
        }

        ControlData(Procedure& proc, Origin origin, BlockSignature signature, BlockType type, unsigned stackSize, BasicBlock* continuation, unsigned tryStart, unsigned tryDepth)
            : controlBlockType(type)
            , m_signature(signature)
            , m_stackSize(stackSize)
            , continuation(continuation)
            , special(nullptr)
            , m_tryStart(tryStart)
            , m_tryCatchDepth(tryDepth)
        {
            ASSERT(type == BlockType::Try);
            m_stackSize -= signature->argumentCount();
            for (unsigned i = 0; i < signature->returnCount(); ++i)
                phis.append(proc.add<Value>(Phi, toB3Type(signature->returnType(i)), origin));
        }

        ControlData()
        {
        }

        static bool isIf(const ControlData& control) { return control.blockType() == BlockType::If; }
        static bool isTry(const ControlData& control) { return control.blockType() == BlockType::Try; }
        static bool isAnyCatch(const ControlData& control) { return control.blockType() == BlockType::Catch; }
        static bool isTopLevel(const ControlData& control) { return control.blockType() == BlockType::TopLevel; }
        static bool isLoop(const ControlData& control) { return control.blockType() == BlockType::Loop; }
        static bool isBlock(const ControlData& control) { return control.blockType() == BlockType::Block; }
        static bool isCatch(const ControlData& control)
        {
            if (control.blockType() != BlockType::Catch)
                return false;
            return control.catchKind() == CatchKind::Catch;
        }

        void dump(PrintStream& out) const
        {
            switch (blockType()) {
            case BlockType::If:
                out.print("If:       ");
                break;
            case BlockType::Block:
                out.print("Block:    ");
                break;
            case BlockType::Loop:
                out.print("Loop:     ");
                break;
            case BlockType::TopLevel:
                out.print("TopLevel: ");
                break;
            case BlockType::Try:
                out.print("Try: ");
                break;
            case BlockType::Catch:
                out.print("Catch: ");
                break;
            }
            out.print("Continuation: ", *continuation, ", Special: ");
            if (special)
                out.print(*special);
            else
                out.print("None");
        }

        BlockType blockType() const { return controlBlockType; }

        BlockSignature signature() const { return m_signature; }

        bool hasNonVoidresult() const { return m_signature->returnsVoid(); }

        BasicBlock* targetBlockForBranch()
        {
            if (blockType() == BlockType::Loop)
                return special;
            return continuation;
        }

        void convertIfToBlock()
        {
            ASSERT(blockType() == BlockType::If);
            controlBlockType = BlockType::Block;
            special = nullptr;
        }

        void convertTryToCatch(unsigned tryEndCallSiteIndex, Variable* exception)
        {
            ASSERT(blockType() == BlockType::Try);
            controlBlockType = BlockType::Catch;
            m_catchKind = CatchKind::Catch;
            m_tryEnd = tryEndCallSiteIndex;
            m_exception = exception;
        }

        void convertTryToCatchAll(unsigned tryEndCallSiteIndex, Variable* exception)
        {
            ASSERT(blockType() == BlockType::Try);
            controlBlockType = BlockType::Catch;
            m_catchKind = CatchKind::CatchAll;
            m_tryEnd = tryEndCallSiteIndex;
            m_exception = exception;
        }

        FunctionArgCount branchTargetArity() const
        {
            if (blockType() == BlockType::Loop)
                return m_signature->argumentCount();
            return m_signature->returnCount();
        }

        Type branchTargetType(unsigned i) const
        {
            ASSERT(i < branchTargetArity());
            if (blockType() == BlockType::Loop)
                return m_signature->argumentType(i);
            return m_signature->returnType(i);
        }

        unsigned tryStart() const
        {
            ASSERT(controlBlockType == BlockType::Try || controlBlockType == BlockType::Catch);
            return m_tryStart;
        }

        unsigned tryEnd() const
        {
            ASSERT(controlBlockType == BlockType::Catch);
            return m_tryEnd;
        }

        unsigned tryDepth() const
        {
            ASSERT(controlBlockType == BlockType::Try || controlBlockType == BlockType::Catch);
            return m_tryCatchDepth;
        }

        CatchKind catchKind() const
        {
            ASSERT(controlBlockType == BlockType::Catch);
            return m_catchKind;
        }

        Variable* exception() const
        {
            ASSERT(controlBlockType == BlockType::Catch);
            return m_exception;
        }

        unsigned stackSize() const { return m_stackSize; }

    private:
        // FIXME: Compress OMGIRGenerator::ControlData fields using an union
        // https://bugs.webkit.org/show_bug.cgi?id=231212
        friend class OMGIRGenerator;
        BlockType controlBlockType;
        BlockSignature m_signature;
        unsigned m_stackSize;
        BasicBlock* continuation;
        BasicBlock* special;
        Vector<Value*> phis;
        unsigned m_tryStart;
        unsigned m_tryEnd;
        unsigned m_tryCatchDepth;
        CatchKind m_catchKind;
        Variable* m_exception;
    };

    using ControlType = ControlData;
    using ExpressionList = Vector<ExpressionType, 1>;

    using ControlEntry = FunctionParser<OMGIRGenerator>::ControlEntry;
    using ControlStack = FunctionParser<OMGIRGenerator>::ControlStack;
    using Stack = FunctionParser<OMGIRGenerator>::Stack;
    using TypedExpression = FunctionParser<OMGIRGenerator>::TypedExpression;

    static_assert(std::is_same_v<ResultList, FunctionParser<OMGIRGenerator>::ResultList>);

    typedef String ErrorType;
    typedef Unexpected<ErrorType> UnexpectedResult;
    typedef Expected<std::unique_ptr<InternalFunction>, ErrorType> Result;
    typedef Expected<void, ErrorType> PartialResult;

    static ExpressionType emptyExpression() { return nullptr; };

    enum class CastKind { Cast, Test };

    struct AbstractHeap {
        WTF_MAKE_STRUCT_FAST_ALLOCATED_WITH_HEAP_IDENTIFIER(AbstractHeap);
        Vector<AbstractHeap*> m_children;
        bool hasAnyUnindexed = false;
        unsigned minBeginIndex = 0xffffffffu;
        unsigned maxEndIndex = 0;
        unsigned computedBegin;
        unsigned computedEnd;

        AbstractHeap() { }

        void addChild(AbstractHeap* child)
        {
            m_children.append(child);
        }
    };

    struct AbstractHeapRange {
        AbstractHeap* heap;
        unsigned isIndexed : 1;
        unsigned begin : 31;
        unsigned end;

        AbstractHeapRange(AbstractHeap* heap_in)
            : heap(heap_in)
            , isIndexed(false)
        { }

        AbstractHeapRange(AbstractHeap* heap, unsigned index)
            : AbstractHeapRange(heap, index, index + 1)
        { }

        AbstractHeapRange(AbstractHeap* heap_in, unsigned begin_in, unsigned end_in)
            : heap(heap_in)
            , isIndexed(true)
            , begin(begin_in)
            , end(end_in)
        {
            if (begin != begin_in) // We overflowed the bitfield...
                isIndexed = false; // ...so clobber the whole heap pessimistically.
        }
    };

#define FOR_EACH_OMG_ABSTRACT_HEAP(macro) \
    macro(Top, Top) \
    macro(Top, Globals) \
    macro(Top, WasmMemory) \
    macro(Top, Tables) \
    macro(Top, Imports) \
    macro(Top, InstanceFields) \
    macro(Top, TableFields) \
    macro(Top, FuncRefTableFields) \
    macro(Top, FunctionFields) \
    macro(Top, Frame) \
    macro(Frame, CallSiteIndex)

private:
#define DEFINE_VARIABLE(parent, name) \
    std::unique_ptr<AbstractHeap> m_heap ## name;
    FOR_EACH_OMG_ABSTRACT_HEAP(DEFINE_VARIABLE)
#undef DEFINE_VARIABLE

#define INITIALIZE_HEAP(parent, name) \
    m_heap ## name = makeUnique<AbstractHeap>(); \
    if (m_heap ## name != m_heap ## parent) \
        m_heap ## parent ->addChild(m_heap ## name.get());
    void initializeHeaps()
    {
        FOR_EACH_OMG_ABSTRACT_HEAP(INITIALIZE_HEAP)
    }
#undef INITIALIZE_HEAP

public:
#define DEFINE_GETTER(parent, name) \
    AbstractHeap* heap ## name () { return m_heap ## name.get() ; }
    FOR_EACH_OMG_ABSTRACT_HEAP(DEFINE_GETTER)
#undef DEFINE_GETTER

    template<typename ValueType>
    struct HeapAndValue {
        AbstractHeapRange range;
        ValueType* value;
    };

    template<typename ValueType>
    friend struct WasmOMGIRGeneratorInternal::HeapRecorder;

    template<typename ValueType>
    void recordHeap(ValueType* value, AbstractHeap* heap, std::optional<unsigned> index = std::nullopt)
    {
        WasmOMGIRGeneratorInternal::HeapRecorder<ValueType> { }.recordHeap(*this, value, heap, index);
    }

    // Add more specializations for each type of value we want to add effects to.

    template<typename ValueType>
    friend struct WasmOMGIRGeneratorInternal::ValueAppender;

    template<typename ValueType, typename... Args>
    ValueType* append(BasicBlock* block, Args&&... args)
    {
        return WasmOMGIRGeneratorInternal::ValueAppender<ValueType> { }.append(*this, block, std::forward<Args>(args)...);
    }

    template<typename ValueType, typename... Args>
    ValueType* append(Args&&... args)
    {
        return append<ValueType>(m_currentBlock, std::forward<Args>(args)...);
    }

    template <typename ...Args>
    NEVER_INLINE UnexpectedResult WARN_UNUSED_RETURN fail(Args... args) const
    {
        using namespace FailureHelper; // See ADL comment in WasmParser.h.
        return UnexpectedResult(makeString("WebAssembly.Module failed compiling: "_s, makeString(args)...));
    }
#define WASM_COMPILE_FAIL_IF(condition, ...) do { \
        if (UNLIKELY(condition))                  \
            return fail(__VA_ARGS__);             \
    } while (0)

    unsigned advanceCallSiteIndex()
    {
        if (m_inlineParent)
            return m_inlineRoot->advanceCallSiteIndex();
        return ++m_callSiteIndex;
    }

    unsigned callSiteIndex() const
    {
        if (m_inlineParent)
            return m_inlineRoot->callSiteIndex();
        return m_callSiteIndex;
    }

    OMGIRGenerator(CalleeGroup&, const ModuleInformation&, OptimizingJITCallee&, Procedure&, Vector<UnlinkedWasmToWasmCall>&, unsigned& osrEntryScratchBufferSize, MemoryMode, CompilationMode, unsigned functionIndex, std::optional<bool> hasExceptionHandlers, unsigned loopIndexForOSREntry, TierUpCount*);
    OMGIRGenerator(OMGIRGenerator& inlineCaller, OMGIRGenerator& inlineRoot, CalleeGroup&, unsigned functionIndex, std::optional<bool> hasExceptionHandlers, BasicBlock* returnContinuation, Vector<Value*> args);

    void computeStackCheckSize(bool& needsOverflowCheck, int32_t& checkSize);

    // SIMD
    bool usesSIMD() { return m_info.usesSIMD(m_functionIndex); }
    void notifyFunctionUsesSIMD() { ASSERT(m_info.usesSIMD(m_functionIndex)); }
    PartialResult WARN_UNUSED_RETURN addSIMDLoad(ExpressionType pointer, uint32_t offset, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addSIMDStore(ExpressionType value, ExpressionType pointer, uint32_t offset);
    PartialResult WARN_UNUSED_RETURN addSIMDSplat(SIMDLane, ExpressionType scalar, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addSIMDShuffle(v128_t imm, ExpressionType a, ExpressionType b, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addSIMDShift(SIMDLaneOperation, SIMDInfo, ExpressionType v, ExpressionType shift, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addSIMDExtmul(SIMDLaneOperation, SIMDInfo, ExpressionType lhs, ExpressionType rhs, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addSIMDLoadSplat(SIMDLaneOperation, ExpressionType pointer, uint32_t offset, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addSIMDLoadLane(SIMDLaneOperation, ExpressionType pointer, ExpressionType vector, uint32_t offset, uint8_t laneIndex, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addSIMDStoreLane(SIMDLaneOperation, ExpressionType pointer, ExpressionType vector, uint32_t offset, uint8_t laneIndex);
    PartialResult WARN_UNUSED_RETURN addSIMDLoadExtend(SIMDLaneOperation, ExpressionType pointer, uint32_t offset, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addSIMDLoadPad(SIMDLaneOperation, ExpressionType pointer, uint32_t offset, ExpressionType& result);

    ExpressionType WARN_UNUSED_RETURN addConstant(v128_t value)
    {
        return push<Const128Value>(m_proc, origin(), value);
    }

    // SIMD generated

    #define B3_OP_CASE(OP) \
    else if (op == SIMDLaneOperation::OP) b3Op = B3::Vector##OP;

    #define B3_OP_CASES() \
    B3::Opcode b3Op = B3::Oops; \
    if (false) { }

    auto addExtractLane(SIMDInfo info, uint8_t lane, ExpressionType v, ExpressionType& result) -> PartialResult
    {
        result = push(append<SIMDValue>(m_proc, origin(), B3::VectorExtractLane, toB3Type(simdScalarType(info.lane)), info,
            lane,
            get(v)));
        return { };
    }

    auto addReplaceLane(SIMDInfo info, uint8_t lane, ExpressionType v, ExpressionType s, ExpressionType& result) -> PartialResult
    {
        result = push(append<SIMDValue>(m_proc, origin(), B3::VectorReplaceLane, B3::V128, info,
            lane,
            get(v),
            get(s)));
        return { };
    }

    auto addSIMDI_V(SIMDLaneOperation op, SIMDInfo info, ExpressionType v, ExpressionType& result) -> PartialResult
    {
        B3_OP_CASES()
        B3_OP_CASE(Bitmask)
        B3_OP_CASE(AnyTrue)
        B3_OP_CASE(AllTrue)
        result = push(append<SIMDValue>(m_proc, origin(), b3Op, B3::Int32, info,
            get(v)));
        return { };
    }

    auto addSIMDV_V(SIMDLaneOperation op, SIMDInfo info, ExpressionType v, ExpressionType& result) -> PartialResult
    {
        B3_OP_CASES()
        B3_OP_CASE(Demote)
        B3_OP_CASE(Promote)
        B3_OP_CASE(Abs)
        B3_OP_CASE(Popcnt)
        B3_OP_CASE(Ceil)
        B3_OP_CASE(Floor)
        B3_OP_CASE(Trunc)
        B3_OP_CASE(Nearest)
        B3_OP_CASE(Sqrt)
        B3_OP_CASE(ExtaddPairwise)
        B3_OP_CASE(Convert)
        B3_OP_CASE(ConvertLow)
        B3_OP_CASE(ExtendHigh)
        B3_OP_CASE(ExtendLow)
        B3_OP_CASE(TruncSat)
        B3_OP_CASE(RelaxedTruncSat)
        B3_OP_CASE(Not)
        B3_OP_CASE(Neg)

        result = push(append<SIMDValue>(m_proc, origin(), b3Op, B3::V128, info,
            get(v)));
        return { };
    }

    auto addSIMDBitwiseSelect(ExpressionType v1, ExpressionType v2, ExpressionType c, ExpressionType& result) -> PartialResult
    {
        auto b3Op = B3::VectorBitwiseSelect;
        result = push(append<SIMDValue>(m_proc, origin(), b3Op, B3::V128, SIMDInfo { SIMDLane::v128, SIMDSignMode::None },
            get(v1), get(v2), get(c)));
        return { };
    }

    auto addSIMDRelOp(SIMDLaneOperation, SIMDInfo info, ExpressionType lhs, ExpressionType rhs, Air::Arg relOp, ExpressionType& result) -> PartialResult
    {
        B3::Opcode b3Op = Oops;
        if (scalarTypeIsIntegral(info.lane)) {
            switch (relOp.asRelationalCondition()) {
            case MacroAssembler::Equal:
                b3Op = VectorEqual;
                break;
            case MacroAssembler::NotEqual:
                b3Op = VectorNotEqual;
                break;
            case MacroAssembler::LessThan:
                b3Op = VectorLessThan;
                break;
            case MacroAssembler::LessThanOrEqual:
                b3Op = VectorLessThanOrEqual;
                break;
            case MacroAssembler::Below:
                b3Op = VectorBelow;
                break;
            case MacroAssembler::BelowOrEqual:
                b3Op = VectorBelowOrEqual;
                break;
            case MacroAssembler::GreaterThan:
                b3Op = VectorGreaterThan;
                break;
            case MacroAssembler::GreaterThanOrEqual:
                b3Op = VectorGreaterThanOrEqual;
                break;
            case MacroAssembler::Above:
                b3Op = VectorAbove;
                break;
            case MacroAssembler::AboveOrEqual:
                b3Op = VectorAboveOrEqual;
                break;
            }
        } else {
            switch (relOp.asDoubleCondition()) {
            case MacroAssembler::DoubleEqualAndOrdered:
                b3Op = VectorEqual;
                break;
            case MacroAssembler::DoubleNotEqualOrUnordered:
                b3Op = VectorNotEqual;
                break;
            case MacroAssembler::DoubleLessThanAndOrdered:
                b3Op = VectorLessThan;
                break;
            case MacroAssembler::DoubleLessThanOrEqualAndOrdered:
                b3Op = VectorLessThanOrEqual;
                break;
            case MacroAssembler::DoubleGreaterThanAndOrdered:
                b3Op = VectorGreaterThan;
                break;
            case MacroAssembler::DoubleGreaterThanOrEqualAndOrdered:
                b3Op = VectorGreaterThanOrEqual;
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
                break;
            }
        }

        result = push(append<SIMDValue>(m_proc, origin(), b3Op, B3::V128, info,
            get(lhs), get(rhs)));
        return { };
    }

    Value* fixupOutOfBoundsIndicesForSwizzle(Value* input, Value* indexes)
    {
        // The intel version of the swizzle instruction does not handle OOB indices properly,
        // so we need to fix them up.
        ASSERT(isX86());
        // Let each byte mask be 112 (0x70) then after VectorAddSat
        // each index > 15 would set the saturated index's bit 7 to 1,
        // whose corresponding byte will be zero cleared in VectorSwizzle.
        // https://github.com/WebAssembly/simd/issues/93
        v128_t mask;
        mask.u64x2[0] = 0x7070707070707070;
        mask.u64x2[1] = 0x7070707070707070;
        auto saturatingMask = append<Const128Value>(m_proc, origin(), mask);
        auto saturatedIndexes = append<SIMDValue>(m_proc, origin(), VectorAddSat, B3::V128, SIMDLane::i8x16, SIMDSignMode::Unsigned, saturatingMask, indexes);
        return append<SIMDValue>(m_proc, origin(), VectorSwizzle, B3::V128, SIMDLane::i8x16, SIMDSignMode::None, input, saturatedIndexes);
    }

    auto addSIMDV_VV(SIMDLaneOperation op, SIMDInfo info, ExpressionType a, ExpressionType b, ExpressionType& result) -> PartialResult
    {
        B3_OP_CASES()
        B3_OP_CASE(And)
        B3_OP_CASE(Andnot)
        B3_OP_CASE(AvgRound)
        B3_OP_CASE(DotProduct)
        B3_OP_CASE(Add)
        B3_OP_CASE(Mul)
        B3_OP_CASE(MulSat)
        B3_OP_CASE(Sub)
        B3_OP_CASE(Div)
        B3_OP_CASE(Pmax)
        B3_OP_CASE(Pmin)
        B3_OP_CASE(Or)
        B3_OP_CASE(Swizzle)
        B3_OP_CASE(RelaxedSwizzle)
        B3_OP_CASE(Xor)
        B3_OP_CASE(Narrow)
        B3_OP_CASE(AddSat)
        B3_OP_CASE(SubSat)
        B3_OP_CASE(Max)
        B3_OP_CASE(Min)

        if (isX86() && b3Op == B3::VectorSwizzle) {
            result = push(fixupOutOfBoundsIndicesForSwizzle(get(a), get(b)));
            return { };
        }

        result = push(append<SIMDValue>(m_proc, origin(), b3Op, B3::V128, info,
            get(a), get(b)));
        return { };
    }

    auto addSIMDRelaxedFMA(SIMDLaneOperation op, SIMDInfo info, ExpressionType m1, ExpressionType m2, ExpressionType add, ExpressionType& result) -> PartialResult
    {
        B3_OP_CASES()
        B3_OP_CASE(RelaxedMAdd)
        B3_OP_CASE(RelaxedNMAdd)

        result = push(append<SIMDValue>(m_proc, origin(), b3Op, B3::V128, info,
            get(m1), get(m2), get(add)));
        return { };
    }

    PartialResult WARN_UNUSED_RETURN addDrop(ExpressionType);
    PartialResult WARN_UNUSED_RETURN addInlinedArguments(const TypeDefinition&);
    PartialResult WARN_UNUSED_RETURN addArguments(const TypeDefinition&);
    PartialResult WARN_UNUSED_RETURN addLocal(Type, uint32_t);
    ExpressionType addConstant(Type, uint64_t);

    // References
    PartialResult WARN_UNUSED_RETURN addRefIsNull(ExpressionType value, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addRefFunc(uint32_t index, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addRefAsNonNull(ExpressionType, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addRefEq(ExpressionType, ExpressionType, ExpressionType&);

    // Tables
    PartialResult WARN_UNUSED_RETURN addTableGet(unsigned, ExpressionType index, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addTableSet(unsigned, ExpressionType index, ExpressionType value);
    PartialResult WARN_UNUSED_RETURN addTableInit(unsigned, unsigned, ExpressionType dstOffset, ExpressionType srcOffset, ExpressionType length);
    PartialResult WARN_UNUSED_RETURN addElemDrop(unsigned);
    PartialResult WARN_UNUSED_RETURN addTableSize(unsigned, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addTableGrow(unsigned, ExpressionType fill, ExpressionType delta, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addTableFill(unsigned, ExpressionType offset, ExpressionType fill, ExpressionType count);
    PartialResult WARN_UNUSED_RETURN addTableCopy(unsigned, unsigned, ExpressionType dstOffset, ExpressionType srcOffset, ExpressionType length);

    // Locals
    PartialResult WARN_UNUSED_RETURN getLocal(uint32_t index, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN setLocal(uint32_t index, ExpressionType value);
    PartialResult WARN_UNUSED_RETURN teeLocal(uint32_t, ExpressionType, ExpressionType& result);

    // Globals
    PartialResult WARN_UNUSED_RETURN getGlobal(uint32_t index, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN setGlobal(uint32_t index, ExpressionType value);

    // Memory
    PartialResult WARN_UNUSED_RETURN load(LoadOpType, ExpressionType pointer, ExpressionType& result, uint32_t offset);
    PartialResult WARN_UNUSED_RETURN store(StoreOpType, ExpressionType pointer, ExpressionType value, uint32_t offset);
    PartialResult WARN_UNUSED_RETURN addGrowMemory(ExpressionType delta, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addCurrentMemory(ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addMemoryFill(ExpressionType dstAddress, ExpressionType targetValue, ExpressionType count);
    PartialResult WARN_UNUSED_RETURN addMemoryCopy(ExpressionType dstAddress, ExpressionType srcAddress, ExpressionType count);
    PartialResult WARN_UNUSED_RETURN addMemoryInit(unsigned, ExpressionType dstAddress, ExpressionType srcAddress, ExpressionType length);
    PartialResult WARN_UNUSED_RETURN addDataDrop(unsigned);

    // Atomics
    PartialResult WARN_UNUSED_RETURN atomicLoad(ExtAtomicOpType, Type, ExpressionType pointer, ExpressionType& result, uint32_t offset);
    PartialResult WARN_UNUSED_RETURN atomicStore(ExtAtomicOpType, Type, ExpressionType pointer, ExpressionType value, uint32_t offset);
    PartialResult WARN_UNUSED_RETURN atomicBinaryRMW(ExtAtomicOpType, Type, ExpressionType pointer, ExpressionType value, ExpressionType& result, uint32_t offset);
    PartialResult WARN_UNUSED_RETURN atomicCompareExchange(ExtAtomicOpType, Type, ExpressionType pointer, ExpressionType expected, ExpressionType value, ExpressionType& result, uint32_t offset);

    PartialResult WARN_UNUSED_RETURN atomicWait(ExtAtomicOpType, ExpressionType pointer, ExpressionType value, ExpressionType timeout, ExpressionType& result, uint32_t offset);
    PartialResult WARN_UNUSED_RETURN atomicNotify(ExtAtomicOpType, ExpressionType pointer, ExpressionType value, ExpressionType& result, uint32_t offset);
    PartialResult WARN_UNUSED_RETURN atomicFence(ExtAtomicOpType, uint8_t flags);

    // Saturated truncation.
    PartialResult WARN_UNUSED_RETURN truncSaturated(Ext1OpType, ExpressionType operand, ExpressionType& result, Type returnType, Type operandType);

    // GC
    PartialResult WARN_UNUSED_RETURN addRefI31(ExpressionType value, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addI31GetS(ExpressionType ref, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addI31GetU(ExpressionType ref, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addArrayNew(uint32_t index, ExpressionType size, ExpressionType value, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addArrayNewDefault(uint32_t index, ExpressionType size, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addArrayNewFixed(uint32_t typeIndex, Vector<ExpressionType>& args, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addArrayGet(ExtGCOpType arrayGetKind, uint32_t typeIndex, ExpressionType arrayref, ExpressionType index, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addArrayNewData(uint32_t typeIndex, uint32_t dataIndex, ExpressionType size, ExpressionType offset, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addArrayNewElem(uint32_t typeIndex, uint32_t elemSegmentIndex, ExpressionType size, ExpressionType offset, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addArraySet(uint32_t typeIndex, ExpressionType arrayref, ExpressionType index, ExpressionType value);
    PartialResult WARN_UNUSED_RETURN addArrayLen(ExpressionType arrayref, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addArrayFill(uint32_t, ExpressionType, ExpressionType, ExpressionType, ExpressionType);
    PartialResult WARN_UNUSED_RETURN addArrayCopy(uint32_t, ExpressionType, ExpressionType, uint32_t, ExpressionType, ExpressionType, ExpressionType);
    PartialResult WARN_UNUSED_RETURN addArrayInitElem(uint32_t, ExpressionType, ExpressionType, uint32_t, ExpressionType, ExpressionType);
    PartialResult WARN_UNUSED_RETURN addArrayInitData(uint32_t, ExpressionType, ExpressionType, uint32_t, ExpressionType, ExpressionType);
    PartialResult WARN_UNUSED_RETURN addStructNew(uint32_t typeIndex, Vector<ExpressionType>& args, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addStructNewDefault(uint32_t index, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addStructGet(ExtGCOpType structGetKind, ExpressionType structReference, const StructType&, uint32_t fieldIndex, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addStructSet(ExpressionType structReference, const StructType&, uint32_t fieldIndex, ExpressionType value);
    PartialResult WARN_UNUSED_RETURN addRefTest(ExpressionType reference, bool allowNull, int32_t heapType, bool shouldNegate, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addRefCast(ExpressionType reference, bool allowNull, int32_t heapType, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addAnyConvertExtern(ExpressionType reference, ExpressionType& result);
    PartialResult WARN_UNUSED_RETURN addExternConvertAny(ExpressionType reference, ExpressionType& result);

    // Basic operators
#define X(name, opcode, short, idx, ...) \
    PartialResult WARN_UNUSED_RETURN add##name(ExpressionType arg, ExpressionType& result);
    FOR_EACH_WASM_UNARY_OP(X)
#undef X
#define X(name, opcode, short, idx, ...) \
    PartialResult WARN_UNUSED_RETURN add##name(ExpressionType left, ExpressionType right, ExpressionType& result);
    FOR_EACH_WASM_BINARY_OP(X)
#undef X

    PartialResult WARN_UNUSED_RETURN addSelect(ExpressionType condition, ExpressionType nonZero, ExpressionType zero, ExpressionType& result);

    // Control flow
    ControlData WARN_UNUSED_RETURN addTopLevel(BlockSignature);
    PartialResult WARN_UNUSED_RETURN addBlock(BlockSignature, Stack& enclosingStack, ControlType& newBlock, Stack& newStack);
    PartialResult WARN_UNUSED_RETURN addLoop(BlockSignature, Stack& enclosingStack, ControlType& block, Stack& newStack, uint32_t loopIndex);
    PartialResult WARN_UNUSED_RETURN addIf(ExpressionType condition, BlockSignature, Stack& enclosingStack, ControlType& result, Stack& newStack);
    PartialResult WARN_UNUSED_RETURN addElse(ControlData&, const Stack&);
    PartialResult WARN_UNUSED_RETURN addElseToUnreachable(ControlData&);

    PartialResult WARN_UNUSED_RETURN addTry(BlockSignature, Stack& enclosingStack, ControlType& result, Stack& newStack);
    PartialResult WARN_UNUSED_RETURN addCatch(unsigned exceptionIndex, const TypeDefinition&, Stack&, ControlType&, ResultList&);
    PartialResult WARN_UNUSED_RETURN addCatchToUnreachable(unsigned exceptionIndex, const TypeDefinition&, ControlType&, ResultList&);
    PartialResult WARN_UNUSED_RETURN addCatchAll(Stack&, ControlType&);
    PartialResult WARN_UNUSED_RETURN addCatchAllToUnreachable(ControlType&);
    PartialResult WARN_UNUSED_RETURN addDelegate(ControlType&, ControlType&);
    PartialResult WARN_UNUSED_RETURN addDelegateToUnreachable(ControlType&, ControlType&);
    PartialResult WARN_UNUSED_RETURN addThrow(unsigned exceptionIndex, Vector<ExpressionType>& args, Stack&);
    PartialResult WARN_UNUSED_RETURN addRethrow(unsigned, ControlType&);

    PartialResult WARN_UNUSED_RETURN addInlinedReturn(const Stack& returnValues);
    PartialResult WARN_UNUSED_RETURN addReturn(const ControlData&, const Stack& returnValues);
    PartialResult WARN_UNUSED_RETURN addBranch(ControlData&, ExpressionType condition, const Stack& returnValues);
    PartialResult WARN_UNUSED_RETURN addBranchNull(ControlType&, ExpressionType, const Stack&, bool, ExpressionType&);
    PartialResult WARN_UNUSED_RETURN addBranchCast(ControlType&, ExpressionType, const Stack&, bool, int32_t, bool);
    PartialResult WARN_UNUSED_RETURN addSwitch(ExpressionType condition, const Vector<ControlData*>& targets, ControlData& defaultTargets, const Stack& expressionStack);
    PartialResult WARN_UNUSED_RETURN endBlock(ControlEntry&, Stack& expressionStack);
    PartialResult WARN_UNUSED_RETURN addEndToUnreachable(ControlEntry&, const Stack& = { });

    PartialResult WARN_UNUSED_RETURN endTopLevel(BlockSignature, const Stack&) { return { }; }

    // Calls
    PartialResult WARN_UNUSED_RETURN addCall(uint32_t calleeIndex, const TypeDefinition&, Vector<ExpressionType>& args, ResultList& results, CallType = CallType::Call);
    PartialResult WARN_UNUSED_RETURN addCallIndirect(unsigned tableIndex, const TypeDefinition&, Vector<ExpressionType>& args, ResultList& results, CallType = CallType::Call);
    PartialResult WARN_UNUSED_RETURN addCallRef(const TypeDefinition&, Vector<ExpressionType>& args, ResultList& results);
    PartialResult WARN_UNUSED_RETURN addUnreachable();
    PartialResult WARN_UNUSED_RETURN addCrash();
    PartialResult WARN_UNUSED_RETURN emitIndirectCall(Value* calleeInstance, Value* calleeCode, Value* boxedCalleeCallee, const TypeDefinition&, const Vector<ExpressionType>& args, ResultList&, CallType = CallType::Call);
    auto createCallPatchpoint(BasicBlock*, B3::Type, const CallInformation&, const Vector<ExpressionType>& tmpArgs) -> CallPatchpointData;
    auto createTailCallPatchpoint(BasicBlock*, CallInformation wasmCallerInfoAsCallee, CallInformation wasmCalleeInfoAsCallee, const Vector<ExpressionType>& tmpArgSourceLocations, Vector<B3::ConstrainedValue> patchArgs) -> CallPatchpointData;

    PartialResult WARN_UNUSED_RETURN emitInlineDirectCall(uint32_t calleeIndex, const TypeDefinition&, Vector<ExpressionType>& args, ResultList& results);

    void dump(const ControlStack&, const Stack* expressionStack);
    void setParser(FunctionParser<OMGIRGenerator>* parser) { m_parser = parser; };
    ALWAYS_INLINE void willParseOpcode() { }
    ALWAYS_INLINE void willParseExtendedOpcode() { }
    ALWAYS_INLINE void didParseOpcode() { }
    void didFinishParsingLocals() { }
    void didPopValueFromStack(ExpressionType expr, ASCIILiteral message)
    {
        --m_stackSize;
        TRACE_VALUE(Wasm::Types::Void, get(expr), "pop at height: ", m_stackSize.value() + 1, " site: [", message, "], var ", *expr);
    }
    const Ref<TypeDefinition> getTypeDefinition(uint32_t typeIndex) { return m_info.typeSignatures[typeIndex]; }
    const ArrayType* getArrayTypeDefinition(uint32_t);
    void getArrayElementType(uint32_t, StorageType&);
    void getArrayRefType(uint32_t, Type&);

    bool canInline() const;

    Value* constant(B3::Type, uint64_t bits, std::optional<Origin> = std::nullopt);
    Value* constant(B3::Type, v128_t bits, std::optional<Origin> = std::nullopt);
    Value* framePointer();
    void insertEntrySwitch();
    void insertConstants();

    B3::Type toB3ResultType(const TypeDefinition*);

    void addStackMap(unsigned callSiteIndex, StackMap&& stackmap)
    {
        if (m_inlineParent) {
            m_inlineRoot->addStackMap(callSiteIndex, WTFMove(stackmap));
            return;
        }
        m_stackmaps.add(CallSiteIndex(callSiteIndex), WTFMove(stackmap));
    }

    StackMaps&& takeStackmaps()
    {
        RELEASE_ASSERT(m_inlineRoot == this);
        return WTFMove(m_stackmaps);
    }

    Vector<UnlinkedHandlerInfo>&& takeExceptionHandlers()
    {
        RELEASE_ASSERT(m_inlineRoot == this);
        return WTFMove(m_exceptionHandlers);
    }

    void finalizeHeapRanges(AbstractHeap* heap, unsigned start)
    {
        heap->computedBegin = start;
        unsigned numberOfIndices = heap->maxEndIndex - heap->minBeginIndex;
        heap->computedEnd = start + numberOfIndices;
        if (heap->hasAnyUnindexed && heap->computedEnd == heap->computedBegin)
            heap->computedEnd++;
        for (AbstractHeap* child : heap->m_children) {
            finalizeHeapRanges(child, heap->computedEnd);
            heap->computedEnd = child->computedEnd;
        }
    }

    void computeHeapRanges()
    {
        // FIXME: We should consider unifying this with the FTL heap range computation at some point.
        // https://bugs.webkit.org/show_bug.cgi?id=276505

        for (auto heapAndValue : m_heapMemoryValues) {
            auto range = heapAndValue.range;
            AbstractHeap* heap = range.heap;
            if (!range.isIndexed) {
                heap->hasAnyUnindexed = true;
                continue;
            }
            heap->minBeginIndex = std::min<unsigned>(heap->minBeginIndex, range.begin);
            heap->maxEndIndex = std::max<unsigned>(heap->maxEndIndex, range.end);
        }

        finalizeHeapRanges(heapTop(), 0);

        for (auto heapAndValue : m_heapMemoryValues) {
            MemoryValue* value = heapAndValue.value;
            auto range = heapAndValue.range;
            AbstractHeap* heap = range.heap;
            if (range.isIndexed)
                value->setRange({ heap->computedBegin + range.begin, heap->computedBegin + range.end });
            else
                value->setRange({ heap->computedBegin, heap->computedEnd });
        }
    }

private:
    void emitPrepareWasmOperation(BasicBlock* block)
    {
#if !USE(BUILTIN_FRAME_ADDRESS) || ASSERT_ENABLED
        // Prepare wasm operation calls.
        append<B3::MemoryValue>(block, heapInstanceFields(), m_proc, B3::Store, origin(), framePointer(), instanceValue(), JSWebAssemblyInstance::offsetOfTemporaryCallFrame());
#else
        UNUSED_PARAM(block);
#endif
    }

    template<typename OperationType, typename ...Args>
    Value* callWasmOperation(BasicBlock* block, B3::Type resultType, OperationType operation, Args&&... args)
    {
        emitPrepareWasmOperation(block);
        static_assert(FunctionTraits<OperationType>::cCallArity() == sizeof...(Args), "Sanity check");
        Value* operationValue = append<ConstPtrValue>(block, m_proc, origin(), tagCFunction<OperationPtrTag>(operation));
        return append<CCallValue>(block, m_proc, resultType, origin(), operationValue, std::forward<Args>(args)...);
    }

    void emitExceptionCheck(CCallHelpers&, ExceptionType);

    void emitEntryTierUpCheck();
    void emitLoopTierUpCheck(uint32_t loopIndex, const Stack& enclosingStack, const Stack& newStack);

    void emitWriteBarrierForJSWrapper();
    void emitWriteBarrier(Value* cell, Value* instanceCell);
    Value* emitCheckAndPreparePointer(Value* pointer, uint32_t offset, uint32_t sizeOfOp);
    B3::Kind memoryKind(B3::Opcode memoryOp);
    Value* emitLoadOp(LoadOpType, Value* pointer, uint32_t offset);
    void emitStoreOp(StoreOpType, Value* pointer, Value*, uint32_t offset);

    Value* sanitizeAtomicResult(ExtAtomicOpType, Type, Value* result);
    Value* emitAtomicLoadOp(ExtAtomicOpType, Type, Value* pointer, uint32_t offset);
    void emitAtomicStoreOp(ExtAtomicOpType, Type, Value* pointer, Value*, uint32_t offset);
    Value* emitAtomicBinaryRMWOp(ExtAtomicOpType, Type, Value* pointer, Value*, uint32_t offset);
    Value* emitAtomicCompareExchange(ExtAtomicOpType, Type, Value* pointer, Value* expected, Value*, uint32_t offset);

    void emitArrayNullCheck(Value*, ExceptionType);
    void emitArraySetUnchecked(uint32_t, Value*, Value*, Value*);
    void emitStructSet(Value*, uint32_t, const StructType&, Value*);
    ExpressionType WARN_UNUSED_RETURN pushArrayNew(uint32_t typeIndex, Value* initValue, ExpressionType size);
    using ArraySegmentOperation = EncodedJSValue SYSV_ABI (&)(JSC::JSWebAssemblyInstance*, uint32_t, uint32_t, uint32_t, uint32_t);
    ExpressionType WARN_UNUSED_RETURN pushArrayNewFromSegment(ArraySegmentOperation, uint32_t typeIndex, uint32_t segmentIndex, ExpressionType arraySize, ExpressionType offset, ExceptionType);
    void emitRefTestOrCast(CastKind, ExpressionType, bool, int32_t, bool, ExpressionType&);
    template <typename Generator>
    void emitCheckOrBranchForCast(CastKind, Value*, const Generator&, BasicBlock*);
    Value* emitLoadRTTFromFuncref(Value*);
    Value* emitLoadRTTFromObject(Value*);
    Value* emitNotRTTKind(Value*, RTTKind);

    void unify(Value* phi, const ExpressionType source);
    void unifyValuesWithBlock(const Stack& resultStack, const ControlData& block);

    void emitChecksForModOrDiv(B3::Opcode, Value* left, Value* right);

    int32_t WARN_UNUSED_RETURN fixupPointerPlusOffset(Value*&, uint32_t);
    Value* WARN_UNUSED_RETURN fixupPointerPlusOffsetForAtomicOps(ExtAtomicOpType, Value*, uint32_t);

    void restoreWasmContextInstance(BasicBlock*, Value*);
    void restoreWebAssemblyGlobalState(const MemoryInformation&, Value* instance, BasicBlock*);
    void reloadMemoryRegistersFromInstance(const MemoryInformation&, Value* instance, BasicBlock*);

    Value* loadFromScratchBuffer(unsigned& indexInBuffer, Value* pointer, B3::Type);
    void connectControlAtEntrypoint(unsigned& indexInBuffer, Value* pointer, ControlData&, Stack& expressionStack, ControlData& currentData, bool fillLoopPhis = false);
    Value* emitCatchImpl(CatchKind, ControlType&, unsigned exceptionIndex = 0);
    PatchpointExceptionHandle preparePatchpointForExceptions(BasicBlock*, PatchpointValue*);

    Origin origin();

    uint32_t outerLoopIndex() const
    {
        if (m_outerLoops.isEmpty())
            return UINT32_MAX;
        return m_outerLoops.last();
    }

    ExpressionType getPushVariable(B3::Type type)
    {
        ++m_stackSize;
        if (m_stackSize > m_maxStackSize) {
            m_maxStackSize = m_stackSize;
            Variable* var = m_proc.addVariable(type);
            if constexpr (WasmOMGIRGeneratorInternal::traceStackValues)
                set(var, constant(type, 0xBADBEEFEF));
            m_stack.append(var);
            return var;
        }

        if constexpr (WasmOMGIRGeneratorInternal::traceStackValues) {
            // When we push, everything else *should* be dead
            for (unsigned i = m_stackSize - 1; i < m_stack.size(); ++i)
                set(m_stack[i], constant(m_stack[i]->type(), 0xBADBEEFEF));
        }

        Variable* var = m_stack[m_stackSize - 1];
        if (var->type() == type)
            return var;

        var = m_proc.addVariable(type);
        m_stack[m_stackSize - 1] = var;
        return var;
    }

    ExpressionType push(Value* value)
    {
        Variable* var = getPushVariable(value->type());
        set(var, value);
        if constexpr (!WasmOMGIRGeneratorInternal::traceExecution)
            return var;
        String site;
#if ASSERT_ENABLED
        if constexpr (WasmOMGIRGeneratorInternal::traceExecutionIncludesConstructionSite)
            site = Value::generateCompilerConstructionSite();
#endif
        TRACE_VALUE(Wasm::Types::Void, get(var), "push to stack height ", m_stackSize.value(), " site: [", site, "] var ", *var);
        return var;
    }

    template<typename ValueType, typename... Args>
    ExpressionType push(BasicBlock* block, Args&&... args)
    {
        return this->push(append<ValueType>(block, std::forward<Args>(args)...));
    }

    template<typename ValueType, typename... Args>
    ExpressionType push(Args&&... args)
    {
        return this->push<ValueType>(m_currentBlock, std::forward<Args>(args)...);
    }

    Value* get(BasicBlock* block, Variable* variable)
    {
        return append<VariableValue>(block, m_proc, B3::Get, origin(), variable);
    }

    Value* get(Variable* variable)
    {
        return get(m_currentBlock, variable);
    }

    Value* set(BasicBlock* block, Variable* dst, Value* src)
    {
        return append<VariableValue>(block, m_proc, B3::Set, origin(), dst, src);
    }

    Value* set(Variable* dst, Value* src)
    {
        return set(m_currentBlock, dst, src);
    }

    Value* set(Variable* dst, Variable* src)
    {
        return set(dst, get(src));
    }

    bool useSignalingMemory() const
    {
        return m_mode == MemoryMode::Signaling;
    }

    template<typename... Args>
    void traceValue(Type, Value*, Args&&... info);
    template<typename... Args>
    void traceCF(Args&&... info);

    FunctionParser<OMGIRGenerator>* m_parser { nullptr };
    CalleeGroup& m_calleeGroup;
    const ModuleInformation& m_info;
    OptimizingJITCallee* m_callee;
    const MemoryMode m_mode { MemoryMode::BoundsChecking };
    const CompilationMode m_compilationMode;
    const unsigned m_functionIndex { UINT_MAX };
    const unsigned m_loopIndexForOSREntry { UINT_MAX };
    TierUpCount* m_tierUp { nullptr };

    Procedure& m_proc;
    Vector<BasicBlock*> m_rootBlocks;
    BasicBlock* m_topLevelBlock;
    BasicBlock* m_currentBlock { nullptr };

    // Only used when this is an inlined context
    BasicBlock* m_returnContinuation { nullptr };
    OMGIRGenerator* m_inlineRoot { nullptr };
    OMGIRGenerator* m_inlineParent { nullptr };
    Vector<Value*> m_inlinedArgs;
    Vector<Variable*> m_inlinedResults;
    unsigned m_inlineDepth { 0 };
    Checked<uint32_t> m_inlinedBytes { 0 };

    Vector<uint32_t> m_outerLoops;
    Vector<Variable*> m_locals;
    Vector<Variable*> m_stack;
    Vector<UnlinkedWasmToWasmCall>& m_unlinkedWasmToWasmCalls; // List each call site and the function index whose address it should be patched with.
    unsigned* m_osrEntryScratchBufferSize;
    HashMap<ValueKey, Value*> m_constantPool;
    HashMap<const TypeDefinition*, B3::Type> m_tupleMap;
    InsertionSet m_constantInsertionValues;
    Vector<HeapAndValue<MemoryValue>> m_heapMemoryValues;
    Value* m_framePointer { nullptr };
    bool m_makesCalls { false };
    bool m_makesTailCalls { false };

    // This tracks the maximum stack offset for a tail call, to be used in the stack overflow check.
    Checked<int32_t> m_tailCallStackOffsetFromFP { 0 };

    std::optional<bool> m_hasExceptionHandlers;

    Value* m_instanceValue { nullptr };
    Value* m_baseMemoryValue { nullptr };
    Value* m_boundsCheckingSizeValue { nullptr };

    Value* instanceValue()
    {
        return m_instanceValue;
    }

    Value* baseMemoryValue()
    {
        return m_baseMemoryValue;
    }

    Value* boundsCheckingSizeValue()
    {
        return m_boundsCheckingSizeValue;
    }

    uint32_t m_maxNumJSCallArguments { 0 };
    unsigned m_numImportFunctions;

    Checked<unsigned> m_tryCatchDepth { 0 };
    Checked<unsigned> m_callSiteIndex { 0 };
    Checked<unsigned> m_stackSize { 0 };
    Checked<unsigned> m_maxStackSize { 0 };
    StackMaps m_stackmaps;
    Vector<UnlinkedHandlerInfo> m_exceptionHandlers;

    RefPtr<B3::Air::PrologueGenerator> m_prologueGenerator;

    Vector<std::unique_ptr<OMGIRGenerator>> m_protectedInlineeGenerators;
    Vector<std::unique_ptr<FunctionParser<OMGIRGenerator>>> m_protectedInlineeParsers;
};

namespace WasmOMGIRGeneratorInternal {
template<>
struct HeapRecorder<MemoryValue> {
    void recordHeap(OMGIRGenerator& generator, MemoryValue* value, OMGIRGenerator::AbstractHeap* heap, std::optional<unsigned> index)
    {
        if (index)
            generator.m_heapMemoryValues.append({ OMGIRGenerator::AbstractHeapRange(heap, *index), value });
        else
            generator.m_heapMemoryValues.append({ OMGIRGenerator::AbstractHeapRange(heap), value });
    }
};

template<>
struct HeapRecorder<AtomicValue> {
    void recordHeap(OMGIRGenerator& generator, MemoryValue* value, OMGIRGenerator::AbstractHeap* heap, std::optional<unsigned> index)
    {
        if (index)
            generator.m_heapMemoryValues.append({ OMGIRGenerator::AbstractHeapRange(heap, *index), value });
        else
            generator.m_heapMemoryValues.append({ OMGIRGenerator::AbstractHeapRange(heap), value });
    }
};

template<typename ValueType>
struct ValueAppender {
    template<typename... Args>
    ValueType* append(OMGIRGenerator&, BasicBlock* block, Args&&... args)
    {
        return block->appendNew<ValueType>(std::forward<Args>(args)...);
    }
};

template<>
struct ValueAppender<MemoryValue> {
    template<typename... Args>
    MemoryValue* append(OMGIRGenerator& generator, BasicBlock* block, OMGIRGenerator::AbstractHeap* heap, Args&&... args)
    {
        auto result = block->appendNew<MemoryValue>(std::forward<Args>(args)...);
        generator.recordHeap(result, heap);
        return result;
    }

    template<typename... Args>
    MemoryValue* append(OMGIRGenerator& generator, BasicBlock* block, OMGIRGenerator::AbstractHeap* heap, unsigned index, Args&&... args)
    {
        auto result = block->appendNew<MemoryValue>(std::forward<Args>(args)...);
        generator.recordHeap(result, heap, index);
        return result;
    }
};

template<>
struct ValueAppender<AtomicValue> {
    template<typename... Args>
    AtomicValue* append(OMGIRGenerator& generator, BasicBlock* block, OMGIRGenerator::AbstractHeap* heap, Args&&... args)
    {
        auto result = block->appendNew<AtomicValue>(std::forward<Args>(args)...);
        generator.recordHeap(result, heap);
        return result;
    }

    template<typename... Args>
    AtomicValue* append(OMGIRGenerator& generator, BasicBlock* block, OMGIRGenerator::AbstractHeap* heap, unsigned index, Args&&... args)
    {
        auto result = block->appendNew<AtomicValue>(std::forward<Args>(args)...);
        generator.recordHeap(result, heap, index);
        return result;
    }
};
} // namespace JSC::Wasm::WasmOMGIRGeneratorInternal

WTF_MAKE_TZONE_ALLOCATED_IMPL(OMGIRGenerator);

using FunctionParserOMGIRGenerator = FunctionParser<OMGIRGenerator>;

WTF_MAKE_TZONE_ALLOCATED_IMPL_TEMPLATE(FunctionParserOMGIRGenerator);

// Memory accesses in WebAssembly have unsigned 32-bit offsets, whereas they have signed 32-bit offsets in B3.
int32_t OMGIRGenerator::fixupPointerPlusOffset(Value*& ptr, uint32_t offset)
{
    if (static_cast<uint64_t>(offset) > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
        ptr = append<Value>(m_proc, Add, origin(), ptr, append<Const64Value>(m_proc, origin(), offset));
        return 0;
    }
    return offset;
}

void OMGIRGenerator::restoreWasmContextInstance(BasicBlock* block, Value* arg)
{
    // FIXME: Because WasmToWasm call clobbers wasmContextInstance register and does not restore it, we need to restore it in the caller side.
    // This prevents us from using ArgumentReg to this (logically) immutable pinned register.
    PatchpointValue* patchpoint = append<PatchpointValue>(block, m_proc, B3::Void, Origin());
    Effects effects = Effects::none();
    effects.writesPinned = true;
    effects.reads = B3::HeapRange::top();
    patchpoint->effects = effects;
    patchpoint->clobberLate(RegisterSetBuilder(GPRInfo::wasmContextInstancePointer));
    patchpoint->append(arg, ValueRep::SomeRegister);
    patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& param) {
        jit.move(param[0].gpr(), GPRInfo::wasmContextInstancePointer);
    });
}

void OMGIRGenerator::computeStackCheckSize(bool& needsOverflowCheck, int32_t& checkSize)
{
    const Checked<int32_t> wasmFrameSize = m_proc.frameSize();
    const Checked<int32_t> wasmTailCallFrameSize = -m_tailCallStackOffsetFromFP;
    const unsigned minimumParentCheckSize = WTF::roundUpToMultipleOf<stackAlignmentBytes()>(1024);
    const unsigned extraFrameSize = WTF::roundUpToMultipleOf<stackAlignmentBytes()>(std::max<uint32_t>(
        // This allows us to elide stack checks for functions that are terminal nodes in the call
        // tree, (e.g they don't make any calls) and have a small enough frame size. This works by
        // having any such terminal node have its parent caller include some extra size in its
        // own check for it. The goal here is twofold:
        // 1. Emit less code.
        // 2. Try to speed things up by skipping stack checks.
        minimumParentCheckSize,
        // This allows us to elide stack checks in the Wasm -> JS call IC stub. Since these will
        // spill all arguments to the stack, we ensure that a stack check here covers the
        // stack that such a stub would use.
        Checked<uint32_t>(m_maxNumJSCallArguments) * sizeof(Register) + JSCallingConvention::headerSizeInBytes
    ));

    checkSize = wasmFrameSize.value();
    bool frameSizeNeedsOverflowCheck = checkSize >= static_cast<int32_t>(minimumParentCheckSize);
    needsOverflowCheck = frameSizeNeedsOverflowCheck;

    if (m_makesCalls) {
        needsOverflowCheck = true;
        checkSize = checkedSum<int32_t>(checkSize, extraFrameSize).value();
    } else if (m_makesTailCalls) {
        Checked<int32_t> tailCallCheckSize = std::max<Checked<int32_t>>(wasmTailCallFrameSize + extraFrameSize, 0);
        checkSize = frameSizeNeedsOverflowCheck ? std::max<Checked<int32_t>>(tailCallCheckSize, wasmFrameSize).value() : tailCallCheckSize.value();
        needsOverflowCheck = needsOverflowCheck || checkSize >= static_cast<int32_t>(minimumParentCheckSize);
    }

    bool needUnderflowCheck = static_cast<unsigned>(checkSize) > Options::reservedZoneSize();
    needsOverflowCheck = needsOverflowCheck || needUnderflowCheck;
}

OMGIRGenerator::OMGIRGenerator(OMGIRGenerator& parentCaller, OMGIRGenerator& rootCaller, CalleeGroup& calleeGroup, unsigned functionIndex, std::optional<bool> hasExceptionHandlers, BasicBlock* returnContinuation, Vector<Value*> args)
    : m_calleeGroup(calleeGroup)
    , m_info(rootCaller.m_info)
    , m_callee(parentCaller.m_callee)
    , m_mode(rootCaller.m_mode)
    , m_compilationMode(CompilationMode::OMGMode)
    , m_functionIndex(functionIndex)
    , m_loopIndexForOSREntry(-1)
    , m_tierUp(nullptr)
    , m_proc(rootCaller.m_proc)
    , m_returnContinuation(returnContinuation)
    , m_inlineRoot(&rootCaller)
    , m_inlineParent(&parentCaller)
    , m_inlinedArgs(WTFMove(args))
    , m_inlineDepth(parentCaller.m_inlineDepth + 1)
    , m_unlinkedWasmToWasmCalls(rootCaller.m_unlinkedWasmToWasmCalls)
    , m_osrEntryScratchBufferSize(nullptr)
    , m_constantInsertionValues(m_proc)
    , m_hasExceptionHandlers(hasExceptionHandlers)
    , m_numImportFunctions(m_info.importFunctionCount())
    , m_tryCatchDepth(parentCaller.m_tryCatchDepth)
    , m_callSiteIndex(0)
{
    initializeHeaps();
    m_topLevelBlock = m_proc.addBlock();
    m_rootBlocks.append(m_proc.addBlock());
    m_currentBlock = m_rootBlocks[0];
    m_instanceValue = rootCaller.m_instanceValue;
    m_baseMemoryValue = rootCaller.m_baseMemoryValue;
    m_boundsCheckingSizeValue = rootCaller.m_boundsCheckingSizeValue;
    if (parentCaller.m_hasExceptionHandlers && *parentCaller.m_hasExceptionHandlers)
        m_hasExceptionHandlers = { true };
}

OMGIRGenerator::OMGIRGenerator(CalleeGroup& calleeGroup, const ModuleInformation& info, OptimizingJITCallee& callee, Procedure& procedure, Vector<UnlinkedWasmToWasmCall>& unlinkedWasmToWasmCalls, unsigned& osrEntryScratchBufferSize, MemoryMode mode, CompilationMode compilationMode, unsigned functionIndex, std::optional<bool> hasExceptionHandlers, unsigned loopIndexForOSREntry, TierUpCount* tierUp)
    : m_calleeGroup(calleeGroup)
    , m_info(info)
    , m_callee(&callee)
    , m_mode(mode)
    , m_compilationMode(compilationMode)
    , m_functionIndex(functionIndex)
    , m_loopIndexForOSREntry(loopIndexForOSREntry)
    , m_tierUp(tierUp)
    , m_proc(procedure)
    , m_inlineRoot(this)
    , m_inlinedBytes(m_info.functionWasmSize(m_functionIndex))
    , m_unlinkedWasmToWasmCalls(unlinkedWasmToWasmCalls)
    , m_osrEntryScratchBufferSize(&osrEntryScratchBufferSize)
    , m_constantInsertionValues(m_proc)
    , m_hasExceptionHandlers(hasExceptionHandlers)
    , m_numImportFunctions(info.importFunctionCount())
{
    initializeHeaps();
    m_topLevelBlock = m_proc.addBlock();
    m_rootBlocks.append(m_proc.addBlock());
    m_currentBlock = m_rootBlocks[0];

    // FIXME we don't really need to pin registers here if there's no memory. It makes wasm -> wasm thunks simpler for now. https://bugs.webkit.org/show_bug.cgi?id=166623

    m_proc.pinRegister(GPRInfo::wasmBaseMemoryPointer);
    m_proc.pinRegister(GPRInfo::wasmContextInstancePointer);
    if (mode == MemoryMode::BoundsChecking)
        m_proc.pinRegister(GPRInfo::wasmBoundsCheckingSizeRegister);

    if (info.memory) {
        m_proc.setWasmBoundsCheckGenerator([=, this] (CCallHelpers& jit, GPRReg pinnedGPR) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            switch (m_mode) {
            case MemoryMode::BoundsChecking:
                ASSERT_UNUSED(pinnedGPR, GPRInfo::wasmBoundsCheckingSizeRegister == pinnedGPR);
                break;
            case MemoryMode::Signaling:
                ASSERT_UNUSED(pinnedGPR, InvalidGPRReg == pinnedGPR);
                break;
            }
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsMemoryAccess);
        });
    }

    {
        // FIXME: Because WasmToWasm call clobbers wasmContextInstance register and does not restore it, we need to restore it in the caller side.
        // This prevents us from using ArgumentReg to this (logically) immutable pinned register.

        B3::PatchpointValue* getInstance = append<B3::PatchpointValue>(m_topLevelBlock, m_proc, pointerType(), Origin());
        getInstance->effects.writesPinned = false;
        getInstance->effects.readsPinned = true;
        getInstance->resultConstraints = { ValueRep::reg(GPRInfo::wasmContextInstancePointer) };
        getInstance->setGenerator([=] (CCallHelpers&, const B3::StackmapGenerationParams&) { });
        m_instanceValue = getInstance;

        if (!!m_info.memory) {
            if (useSignalingMemory() || m_info.memory.isShared()) {
                // Capacity and basePointer will not be changed in this case.
                if (m_mode == MemoryMode::BoundsChecking) {
                    B3::PatchpointValue* getBoundsCheckingSize = append<B3::PatchpointValue>(m_topLevelBlock, m_proc, pointerType(), Origin());
                    getBoundsCheckingSize->effects.writesPinned = false;
                    getBoundsCheckingSize->effects.readsPinned = true;
                    getBoundsCheckingSize->resultConstraints = { ValueRep::reg(GPRInfo::wasmBoundsCheckingSizeRegister) };
                    getBoundsCheckingSize->setGenerator([=] (CCallHelpers&, const B3::StackmapGenerationParams&) { });
                    m_boundsCheckingSizeValue = getBoundsCheckingSize;
                }
                B3::PatchpointValue* getBaseMemory = append<B3::PatchpointValue>(m_topLevelBlock, m_proc, pointerType(), Origin());
                getBaseMemory->effects.writesPinned = false;
                getBaseMemory->effects.readsPinned = true;
                getBaseMemory->resultConstraints = { ValueRep::reg(GPRInfo::wasmBaseMemoryPointer) };
                getBaseMemory->setGenerator([=] (CCallHelpers&, const B3::StackmapGenerationParams&) { });
                m_baseMemoryValue = getBaseMemory;
            }
        }
    }

    m_prologueGenerator = createSharedTask<B3::Air::PrologueGeneratorFunction>([=, this] (CCallHelpers& jit, B3::Air::Code& code) {
        RELEASE_ASSERT(m_callee);
        AllowMacroScratchRegisterUsage allowScratch(jit);

        if (WasmOMGIRGeneratorInternal::verboseTailCalls) {
            int fi = this->m_functionIndex;
            jit.probeDebugSIMD([fi] (Probe::Context& context) {
                dataLogLn(" General Before Prologue, fucntion ", fi, " FP: ", RawHex(context.gpr<uint64_t>(GPRInfo::callFrameRegister)), " SP: ", RawHex(context.gpr<uint64_t>(MacroAssembler::stackPointerRegister)));
            });
        }

        code.emitDefaultPrologue(jit);
        GPRReg scratchGPR = wasmCallingConvention().prologueScratchGPRs[0];
        jit.move(CCallHelpers::TrustedImmPtr(CalleeBits::boxNativeCallee(m_callee)), scratchGPR);
        static_assert(CallFrameSlot::codeBlock + 1 == CallFrameSlot::callee);
        jit.storePairPtr(GPRInfo::wasmContextInstancePointer, scratchGPR, GPRInfo::callFrameRegister, CCallHelpers::TrustedImm32(CallFrameSlot::codeBlock * sizeof(Register)));
    });
    {
        B3::PatchpointValue* stackOverflowCheck = append<B3::PatchpointValue>(m_proc, Void, Origin());
        stackOverflowCheck->appendSomeRegister(instanceValue());
        stackOverflowCheck->appendSomeRegister(framePointer());
        stackOverflowCheck->clobber(RegisterSetBuilder::macroClobberedGPRs());
        stackOverflowCheck->numGPScratchRegisters = 0;
        stackOverflowCheck->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
            ASSERT(m_proc.frameSize() == params.proc().frameSize());
            int32_t checkSize = 0;
            bool needsOverflowCheck = false;
            computeStackCheckSize(needsOverflowCheck, checkSize);
            ASSERT(checkSize || !needsOverflowCheck);

            // This allows leaf functions to not do stack checks if their frame size is within
            // certain limits since their caller would have already done the check.
            if (needsOverflowCheck) {
                AllowMacroScratchRegisterUsage allowScratch(jit);
                GPRReg contextInstance = params[0].gpr();
                GPRReg fp = params[1].gpr();
                if (isOSREntry(m_compilationMode))
                    jit.checkWasmStackOverflow(contextInstance, CCallHelpers::TrustedImm32(checkSize), fp).linkThunk(CodeLocationLabel<JITThunkPtrTag>(Thunks::singleton().stub(crashDueToOMGStackOverflowGenerator).code()), &jit);
                else
                    jit.checkWasmStackOverflow(contextInstance, CCallHelpers::TrustedImm32(checkSize), fp).linkThunk(CodeLocationLabel<JITThunkPtrTag>(Thunks::singleton().stub(throwStackOverflowFromWasmThunkGenerator).code()), &jit);
            }
        });
    }

    emitEntryTierUpCheck();

    if (isOSREntry(m_compilationMode))
        m_currentBlock = m_proc.addBlock();
}

void OMGIRGenerator::restoreWebAssemblyGlobalState(const MemoryInformation& memory, Value* instance, BasicBlock* block)
{
    restoreWasmContextInstance(block, instance);

    if (!!memory) {
        if (useSignalingMemory() || memory.isShared()) {
            RegisterSet clobbers;
            clobbers.add(GPRInfo::wasmBaseMemoryPointer, IgnoreVectors);
            if (m_mode == MemoryMode::BoundsChecking)
                clobbers.add(GPRInfo::wasmBoundsCheckingSizeRegister, IgnoreVectors);

            B3::PatchpointValue* patchpoint = append<B3::PatchpointValue>(block, m_proc, B3::Void, origin());
            Effects effects = Effects::none();
            effects.writesPinned = true;
            effects.reads = B3::HeapRange::top();
            patchpoint->effects = effects;
            patchpoint->clobber(clobbers);

            patchpoint->append(baseMemoryValue(), ValueRep::SomeRegister);
            if (m_mode == MemoryMode::BoundsChecking)
                patchpoint->append(boundsCheckingSizeValue(), ValueRep::SomeRegister);
            patchpoint->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
                jit.move(params[0].gpr(), GPRInfo::wasmBaseMemoryPointer);
                if (params.size() == 2)
                    jit.move(params[1].gpr(), GPRInfo::wasmBoundsCheckingSizeRegister);
            });
            return;
        }

        reloadMemoryRegistersFromInstance(memory, instance, block);
    }
}

void OMGIRGenerator::reloadMemoryRegistersFromInstance(const MemoryInformation& memory, Value* instance, BasicBlock* block)
{
    if (!!memory) {
        RegisterSet clobbers;
        clobbers.add(GPRInfo::wasmBaseMemoryPointer, IgnoreVectors);
        clobbers.add(GPRInfo::wasmBoundsCheckingSizeRegister, IgnoreVectors);
        clobbers.merge(RegisterSetBuilder::macroClobberedGPRs());

        B3::PatchpointValue* patchpoint = append<B3::PatchpointValue>(block, m_proc, B3::Void, origin());
        Effects effects = Effects::none();
        effects.writesPinned = true;
        effects.reads = B3::HeapRange::top();
        patchpoint->effects = effects;
        patchpoint->clobber(clobbers);
        patchpoint->numGPScratchRegisters = 1;

        patchpoint->append(instance, ValueRep::SomeRegister);
        patchpoint->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            GPRReg scratch = params.gpScratch(0);
            jit.loadPairPtr(params[0].gpr(), CCallHelpers::TrustedImm32(JSWebAssemblyInstance::offsetOfCachedMemory()), GPRInfo::wasmBaseMemoryPointer, GPRInfo::wasmBoundsCheckingSizeRegister);
            jit.cageConditionally(Gigacage::Primitive, GPRInfo::wasmBaseMemoryPointer, GPRInfo::wasmBoundsCheckingSizeRegister, scratch);
        });
    }
}

void OMGIRGenerator::emitExceptionCheck(CCallHelpers& jit, ExceptionType type)
{
    jit.move(CCallHelpers::TrustedImm32(static_cast<uint32_t>(type)), GPRInfo::argumentGPR1);
    jit.jumpThunk(CodeLocationLabel<JITThunkPtrTag>(Thunks::singleton().stub(throwExceptionFromWasmThunkGenerator).code()));
}

Value* OMGIRGenerator::constant(B3::Type type, uint64_t bits, std::optional<Origin> maybeOrigin)
{
    auto result = m_constantPool.ensure(ValueKey(opcodeForConstant(type), type, static_cast<int64_t>(bits)), [&] {
        Value* result = nullptr;
        if (type.kind() == B3::V128) {
            v128_t vector { };
            vector.u64x2[0] = bits;
            vector.u64x2[1] = 0;
            result = m_proc.addConstant(maybeOrigin ? *maybeOrigin : origin(), type, vector);
        } else
            result = m_proc.addConstant(maybeOrigin ? *maybeOrigin : origin(), type, bits);
        m_constantInsertionValues.insertValue(0, result);
        return result;
    });
    return result.iterator->value;
}

Value* OMGIRGenerator::constant(B3::Type type, v128_t bits, std::optional<Origin> maybeOrigin)
{
    Value* result = m_proc.addConstant(maybeOrigin ? *maybeOrigin : origin(), type, bits);
    m_constantInsertionValues.insertValue(0, result);
    return result;
}

Value* OMGIRGenerator::framePointer()
{
    if (!m_framePointer) {
        m_framePointer = m_proc.add<B3::Value>(B3::FramePointer, Origin());
        ASSERT(m_framePointer);
        m_constantInsertionValues.insertValue(0, m_framePointer);
    }
    return m_framePointer;
}

void OMGIRGenerator::insertEntrySwitch()
{
    m_proc.setNumEntrypoints(m_rootBlocks.size());

    Ref<B3::Air::PrologueGenerator> catchPrologueGenerator = createSharedTask<B3::Air::PrologueGeneratorFunction>([] (CCallHelpers& jit, B3::Air::Code& code) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        jit.addPtr(CCallHelpers::TrustedImm32(-code.frameSize()), GPRInfo::callFrameRegister, CCallHelpers::stackPointerRegister);
        jit.probe(tagCFunction<JITProbePtrTag>(code.usesSIMD() ? buildEntryBufferForCatchSIMD : buildEntryBufferForCatchNoSIMD), nullptr, code.usesSIMD() ? SavedFPWidth::SaveVectors : SavedFPWidth::DontSaveVectors);
    });

    m_proc.code().setPrologueForEntrypoint(0, Ref<B3::Air::PrologueGenerator>(*m_prologueGenerator));
    for (unsigned i = 1; i < m_rootBlocks.size(); ++i)
        m_proc.code().setPrologueForEntrypoint(i, catchPrologueGenerator.copyRef());

    m_currentBlock = m_topLevelBlock;
    append<Value>(m_proc, EntrySwitch, Origin());
    for (BasicBlock* block : m_rootBlocks)
        m_currentBlock->appendSuccessor(FrequentedBlock(block));
}

void OMGIRGenerator::insertConstants()
{
    bool mayHaveExceptionHandlers = !m_hasExceptionHandlers || m_hasExceptionHandlers.value();

    Value* invalidCallSiteIndex = nullptr;
    if (mayHaveExceptionHandlers)
        invalidCallSiteIndex = constant(B3::Int32, PatchpointExceptionHandle::s_invalidCallSiteIndex, Origin());
    m_constantInsertionValues.execute(m_proc.at(0));

    if (!mayHaveExceptionHandlers)
        return;

    MemoryValue* storeCallSiteIndex = m_proc.add<B3::MemoryValue>(B3::Store, Origin(), invalidCallSiteIndex, framePointer(), safeCast<int32_t>(CallFrameSlot::argumentCountIncludingThis * sizeof(Register) + TagOffset));
    recordHeap(storeCallSiteIndex, heapCallSiteIndex());

    BasicBlock* block = m_rootBlocks[0];
    m_constantInsertionValues.insertValue(0, storeCallSiteIndex);
    m_constantInsertionValues.execute(block);
}

B3::Type OMGIRGenerator::toB3ResultType(const TypeDefinition* returnType)
{
    if (returnType->as<FunctionSignature>()->returnsVoid())
        return B3::Void;

    if (returnType->as<FunctionSignature>()->returnCount() == 1)
        return toB3Type(returnType->as<FunctionSignature>()->returnType(0));

    auto result = m_tupleMap.ensure(returnType, [&] {
        Vector<B3::Type> result;
        for (unsigned i = 0; i < returnType->as<FunctionSignature>()->returnCount(); ++i)
            result.append(toB3Type(returnType->as<FunctionSignature>()->returnType(i)));
        return m_proc.addTuple(WTFMove(result));
    });
    return result.iterator->value;
}

auto OMGIRGenerator::addLocal(Type type, uint32_t count) -> PartialResult
{
    size_t newSize = m_locals.size() + count;
    ASSERT(!(CheckedUint32(count) + m_locals.size()).hasOverflowed());
    ASSERT(newSize <= maxFunctionLocals);
    WASM_COMPILE_FAIL_IF(!m_locals.tryReserveCapacity(newSize), "can't allocate memory for "_s, newSize, " locals"_s);

    m_locals.appendUsingFunctor(count, [&](size_t) {
        Variable* local = m_proc.addVariable(toB3Type(type));
        if (type.isV128())
            append<VariableValue>(m_proc, Set, Origin(), local, constant(toB3Type(type), v128_t { }, Origin()));
        else {
            auto val = isRefType(type) ? JSValue::encode(jsNull()) : 0;
            append<VariableValue>(m_proc, Set, Origin(), local, constant(toB3Type(type), val, Origin()));
        }
        return local;
    });
    return { };
}

auto OMGIRGenerator::addDrop(ExpressionType) -> PartialResult
{
    return { };
}

auto OMGIRGenerator::addInlinedArguments(const TypeDefinition& signature) -> PartialResult
{
    RELEASE_ASSERT(signature.as<FunctionSignature>()->argumentCount() == m_inlinedArgs.size());
    CallInformation wasmCallInfo = wasmCallingConvention().callInformationFor(signature, CallRole::Callee);

    for (size_t i = 0; i < signature.as<FunctionSignature>()->argumentCount(); ++i) {
        B3::Type type = toB3Type(signature.as<FunctionSignature>()->argumentType(i));
        Value* value = m_inlinedArgs[i];
        RELEASE_ASSERT(value->type() == type);

        Variable* argumentVariable = m_proc.addVariable(type);
        m_locals[i] = argumentVariable;
        append<VariableValue>(m_proc, Set, Origin(), argumentVariable, value);
    }

    return { };
}

auto OMGIRGenerator::addArguments(const TypeDefinition& signature) -> PartialResult
{
    ASSERT(!m_locals.size());
    WASM_COMPILE_FAIL_IF(!m_locals.tryReserveCapacity(signature.as<FunctionSignature>()->argumentCount()), "can't allocate memory for "_s, signature.as<FunctionSignature>()->argumentCount(), " arguments"_s);

    m_locals.grow(signature.as<FunctionSignature>()->argumentCount());

    if (m_inlineParent)
        return addInlinedArguments(signature);

    CallInformation wasmCallInfo = wasmCallingConvention().callInformationFor(signature, CallRole::Callee);

    if (WasmOMGIRGeneratorInternal::verboseTailCalls) {
        PatchpointValue* patch = m_proc.add<PatchpointValue>(B3::Void, Origin());
        patch->effects = Effects::none();
        patch->effects.controlDependent = true;
        patch->effects.fence = true;
        patch->effects.reads = HeapRange::top();
        patch->effects.writes = HeapRange::top();
        m_currentBlock->append(patch);
        patch->setGenerator([functionIndex = m_functionIndex, signature = signature.as<FunctionSignature>(), wasmCallInfo] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            jit.probeDebugSIMD([functionIndex, signature, wasmCallInfo] (Probe::Context& context) {
                dataLogLn(" General Add arguments, fucntion ", functionIndex, " FP: ", RawHex(context.gpr<uint64_t>(GPRInfo::callFrameRegister)), " SP: ", RawHex(context.gpr<uint64_t>(MacroAssembler::stackPointerRegister)));

                auto fpl = context.gpr<uint64_t*>(GPRInfo::callFrameRegister);
                auto fpi = context.gpr<uint32_t*>(GPRInfo::callFrameRegister);

                for (size_t i = 0; i < signature->argumentCount(); ++i) {
                    auto rep = wasmCallInfo.params[i];
                    auto src = rep.location;
                    auto width = rep.width;
                    dataLog("     Arg source ", i, " located at ", src, " = ");

                    if (src.isGPR())
                        dataLog(context.gpr(src.jsr().payloadGPR()), " / ", (int) context.gpr(src.jsr().payloadGPR()));
                    else if (src.isFPR() && width <= Width::Width64)
                        dataLog(context.fpr(src.fpr(), SavedFPWidth::SaveVectors));
                    else if (src.isFPR())
                        dataLog(context.vector(src.fpr()));
                    else
                        dataLog(fpl[src.offsetFromFP() / sizeof(uint64_t)], " / ", fpi[src.offsetFromFP() / sizeof(uint32_t)],  " / ", RawHex(fpi[src.offsetFromFP() / sizeof(uint32_t)]), " / ", bitwise_cast<double>(fpl[src.offsetFromFP() / sizeof(uint64_t)]), " at ", RawPointer(&fpi[src.offsetFromFP() / sizeof(uint32_t)]));
                    dataLogLn();
                }
            });
        });
    }

    for (size_t i = 0; i < signature.as<FunctionSignature>()->argumentCount(); ++i) {
        B3::Type type = toB3Type(signature.as<FunctionSignature>()->argumentType(i));
        B3::Value* argument;
        auto rep = wasmCallInfo.params[i];
        if (rep.location.isGPR()) {
            argument = append<B3::ArgumentRegValue>(m_proc, Origin(), rep.location.jsr().payloadGPR());
            if (type == B3::Int32)
                argument = append<B3::Value>(m_proc, B3::Trunc, Origin(), argument);
        } else if (rep.location.isFPR()) {
            if (type.isVector()) {
                ASSERT(rep.width == Width128);
                argument = append<B3::ArgumentRegValue>(m_proc, Origin(), rep.location.fpr(), B3::ArgumentRegValue::UsesVectorArgs);
            } else {
                ASSERT(rep.width != Width128);
                argument = append<B3::ArgumentRegValue>(m_proc, Origin(), rep.location.fpr());
            }
            if (type == B3::Float)
                argument = append<B3::Value>(m_proc, B3::Trunc, Origin(), argument);
        } else {
            ASSERT(rep.location.isStack());
            B3::Value* address = append<B3::Value>(m_proc, B3::Add, Origin(), framePointer(),
                append<B3::Const64Value>(m_proc, Origin(), rep.location.offsetFromFP()));
            argument = append<B3::MemoryValue>(heapFrame(), m_proc, B3::Load, type, Origin(), address);
        }

        Variable* argumentVariable = m_proc.addVariable(argument->type());
        m_locals[i] = argumentVariable;
        append<VariableValue>(m_proc, Set, Origin(), argumentVariable, argument);
    }

    return { };
}

auto OMGIRGenerator::addRefIsNull(ExpressionType value, ExpressionType& result) -> PartialResult
{
    result = push<Value>(m_proc, B3::Equal, origin(), get(value), append<Const64Value>(m_proc, origin(), JSValue::encode(jsNull())));
    return { };
}

auto OMGIRGenerator::addTableGet(unsigned tableIndex, ExpressionType index, ExpressionType& result) -> PartialResult
{
    // FIXME: Emit this inline <https://bugs.webkit.org/show_bug.cgi?id=198506>.
    Value* resultValue = callWasmOperation(m_currentBlock, toB3Type(Types::Externref), operationGetWasmTableElement,
        instanceValue(), append<Const32Value>(m_proc, origin(), tableIndex), get(index));
    {
        result = push(resultValue);
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), resultValue, append<Const64Value>(m_proc, origin(), 0)));

        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsTableAccess);
        });
    }

    return { };
}

auto OMGIRGenerator::addTableSet(unsigned tableIndex, ExpressionType index, ExpressionType value) -> PartialResult
{
    // FIXME: Emit this inline <https://bugs.webkit.org/show_bug.cgi?id=198506>.
    auto shouldThrow = callWasmOperation(m_currentBlock, B3::Int32, operationSetWasmTableElement,
        instanceValue(), append<Const32Value>(m_proc, origin(), tableIndex), get(index), get(value));
    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), shouldThrow, append<Const32Value>(m_proc, origin(), 0)));

        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsTableAccess);
        });
    }

    return { };
}

auto OMGIRGenerator::addRefFunc(uint32_t index, ExpressionType& result) -> PartialResult
{
    // FIXME: Emit this inline <https://bugs.webkit.org/show_bug.cgi?id=198506>.
    result = push(callWasmOperation(m_currentBlock, B3::Int64, operationWasmRefFunc,
        instanceValue(), constant(toB3Type(Types::I32), index)));
    TRACE_VALUE(Wasm::Types::Funcref, get(result), "ref_func ", index);
    return { };
}

auto OMGIRGenerator::addRefAsNonNull(ExpressionType reference, ExpressionType& result) -> PartialResult
{
    result = push(get(reference));
    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), get(reference), append<Const64Value>(m_proc, origin(), JSValue::encode(jsNull()))));
        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::NullRefAsNonNull);
        });
    }
    return { };
}

auto OMGIRGenerator::addRefEq(ExpressionType ref0, ExpressionType ref1, ExpressionType& result) -> PartialResult
{
    return addI64Eq(ref0, ref1, result);
}

auto OMGIRGenerator::addTableInit(unsigned elementIndex, unsigned tableIndex, ExpressionType dstOffset, ExpressionType srcOffset, ExpressionType length) -> PartialResult
{
    Value* resultValue = callWasmOperation(m_currentBlock, toB3Type(Types::I32), operationWasmTableInit,
        instanceValue(),
        append<Const32Value>(m_proc, origin(), elementIndex),
        append<Const32Value>(m_proc, origin(), tableIndex),
        get(dstOffset), get(srcOffset), get(length));

    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), resultValue, append<Const32Value>(m_proc, origin(), 0)));

        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsTableAccess);
        });
    }

    return { };
}

auto OMGIRGenerator::addElemDrop(unsigned elementIndex) -> PartialResult
{
    callWasmOperation(m_currentBlock, B3::Void, operationWasmElemDrop,
        instanceValue(),
        append<Const32Value>(m_proc, origin(), elementIndex));

    return { };
}

auto OMGIRGenerator::addTableSize(unsigned tableIndex, ExpressionType& result) -> PartialResult
{
    // FIXME: Emit this inline <https://bugs.webkit.org/show_bug.cgi?id=198506>.
    result = push(callWasmOperation(m_currentBlock, toB3Type(Types::I32), operationGetWasmTableSize,
        instanceValue(), append<Const32Value>(m_proc, origin(), tableIndex)));

    return { };
}

auto OMGIRGenerator::addTableGrow(unsigned tableIndex, ExpressionType fill, ExpressionType delta, ExpressionType& result) -> PartialResult
{
    result = push(callWasmOperation(m_currentBlock, toB3Type(Types::I32), operationWasmTableGrow,
        instanceValue(), append<Const32Value>(m_proc, origin(), tableIndex), get(fill), get(delta)));

    return { };
}

auto OMGIRGenerator::addTableFill(unsigned tableIndex, ExpressionType offset, ExpressionType fill, ExpressionType count) -> PartialResult
{
    Value* resultValue = callWasmOperation(m_currentBlock, toB3Type(Types::I32), operationWasmTableFill,
        instanceValue(), append<Const32Value>(m_proc, origin(), tableIndex), get(offset), get(fill), get(count));

    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), resultValue, append<Const32Value>(m_proc, origin(), 0)));

        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsTableAccess);
        });
    }

    return { };
}

auto OMGIRGenerator::addTableCopy(unsigned dstTableIndex, unsigned srcTableIndex, ExpressionType dstOffset, ExpressionType srcOffset, ExpressionType length) -> PartialResult
{
    Value* resultValue = callWasmOperation(
        m_currentBlock, toB3Type(Types::I32), operationWasmTableCopy,
        instanceValue(),
        append<Const32Value>(m_proc, origin(), dstTableIndex),
        append<Const32Value>(m_proc, origin(), srcTableIndex),
        get(dstOffset), get(srcOffset), get(length));

    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), resultValue, append<Const32Value>(m_proc, origin(), 0)));

        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsTableAccess);
        });
    }

    return { };
}

auto OMGIRGenerator::getLocal(uint32_t index, ExpressionType& result) -> PartialResult
{
    ASSERT(m_locals[index]);
    result = push<VariableValue>(m_proc, B3::Get, origin(), m_locals[index]);
    TRACE_VALUE(m_parser->typeOfLocal(index), get(result), "get_local ", index);
    return { };
}

auto OMGIRGenerator::addUnreachable() -> PartialResult
{
    B3::PatchpointValue* unreachable = append<B3::PatchpointValue>(m_proc, B3::Void, origin());
    unreachable->setGenerator([this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
        this->emitExceptionCheck(jit, ExceptionType::Unreachable);
    });
    unreachable->effects.terminal = true;
    return { };
}

auto OMGIRGenerator::addCrash() -> PartialResult
{
    B3::PatchpointValue* unreachable = append<B3::PatchpointValue>(m_proc, B3::Void, origin());
    unreachable->setGenerator([] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
        jit.breakpoint();
    });
    unreachable->effects.terminal = true;
    return { };
}

auto OMGIRGenerator::emitIndirectCall(Value* calleeInstance, Value* calleeCode, Value* boxedCalleeCallee, const TypeDefinition& signature, const Vector<ExpressionType>& args, ResultList& results, CallType callType) -> PartialResult
{
    const bool isTailCallInlineCaller = callType == CallType::TailCall && m_inlineParent;
    const bool isTailCall = callType == CallType::TailCall && !isTailCallInlineCaller;
    ASSERT(callType == CallType::Call || isTailCall || isTailCallInlineCaller);

    m_makesCalls = true;
    if (isTailCall || isTailCallInlineCaller)
        m_makesTailCalls = true;

    // Do a context switch if needed.
    {
        BasicBlock* continuation = m_proc.addBlock();
        BasicBlock* doContextSwitch = m_proc.addBlock();

        Value* isSameContextInstance = append<Value>(m_proc, Equal, origin(),
            calleeInstance, instanceValue());
        m_currentBlock->appendNewControlValue(m_proc, B3::Branch, origin(),
            isSameContextInstance, FrequentedBlock(continuation), FrequentedBlock(doContextSwitch));

        PatchpointValue* patchpoint = append<PatchpointValue>(doContextSwitch, m_proc, B3::Void, origin());
        patchpoint->effects.writesPinned = true;
        // We pessimistically assume we're calling something with BoundsChecking memory.
        // FIXME: We shouldn't have to do this: https://bugs.webkit.org/show_bug.cgi?id=172181
        patchpoint->clobber(RegisterSetBuilder::wasmPinnedRegisters());
        patchpoint->clobber(RegisterSetBuilder::macroClobberedGPRs());
        patchpoint->append(calleeInstance, ValueRep::SomeRegister);
        patchpoint->numGPScratchRegisters = 1;

        patchpoint->setGenerator([=] (CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            GPRReg calleeInstance = params[0].gpr();
            ASSERT(calleeInstance != GPRInfo::wasmBaseMemoryPointer);
            jit.storeWasmContextInstance(calleeInstance);
            static_assert(GPRInfo::wasmBoundsCheckingSizeRegister != GPRInfo::wasmBaseMemoryPointer);
            // FIXME: We should support more than one memory size register
            //   see: https://bugs.webkit.org/show_bug.cgi?id=162952
            ASSERT(GPRInfo::wasmBoundsCheckingSizeRegister != calleeInstance);
            GPRReg scratch = params.gpScratch(0);
            jit.loadPairPtr(calleeInstance, CCallHelpers::TrustedImm32(JSWebAssemblyInstance::offsetOfCachedMemory()), GPRInfo::wasmBaseMemoryPointer, GPRInfo::wasmBoundsCheckingSizeRegister);
            jit.cageConditionally(Gigacage::Primitive, GPRInfo::wasmBaseMemoryPointer, GPRInfo::wasmBoundsCheckingSizeRegister, scratch);
        });
        doContextSwitch->appendNewControlValue(m_proc, Jump, origin(), continuation);

        m_currentBlock = continuation;
    }

    const auto& callingConvention = wasmCallingConvention();
    CallInformation wasmCalleeInfo = callingConvention.callInformationFor(signature, CallRole::Caller);
    CallInformation wasmCalleeInfoAsCallee = callingConvention.callInformationFor(signature, CallRole::Callee);
    Checked<int32_t> calleeStackSize = WTF::roundUpToMultipleOf<stackAlignmentBytes()>(wasmCalleeInfo.headerAndArgumentStackSizeInBytes);
    if (isTailCall)
        calleeStackSize = WTF::roundUpToMultipleOf<stackAlignmentBytes()>(wasmCalleeInfo.headerAndArgumentStackSizeInBytes * 2 + sizeof(Register));

    m_proc.requestCallArgAreaSizeInBytes(calleeStackSize);

    if (isTailCall) {
        const TypeIndex callerTypeIndex = m_info.internalFunctionTypeIndices[m_functionIndex];
        const TypeDefinition& callerTypeDefinition = TypeInformation::get(callerTypeIndex).expand();
        CallInformation wasmCallerInfoAsCallee = callingConvention.callInformationFor(callerTypeDefinition, CallRole::Callee);

        auto [patchpoint, _, prepareForCall] = createTailCallPatchpoint(m_currentBlock, wasmCallerInfoAsCallee, wasmCalleeInfoAsCallee, args, { { calleeCode, ValueRep(GPRInfo::wasmScratchGPR0) } });
        unsigned patchArgsIndex = patchpoint->reps().size();
        patchpoint->append(calleeCode, ValueRep(GPRInfo::nonPreservedNonArgumentGPR0));
        patchpoint->append(boxedCalleeCallee, ValueRep::SomeRegister);
        patchArgsIndex += m_proc.resultCount(patchpoint->type());
        patchpoint->setGenerator([prepareForCall = prepareForCall, patchArgsIndex](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            prepareForCall->run(jit, params);
            jit.storeWasmCalleeCallee(params[patchArgsIndex + 1].gpr(), sizeof(CallerFrameAndPC) - prologueStackPointerDelta());
            jit.farJump(params[patchArgsIndex].gpr(), WasmEntryPtrTag);
        });
        return { };
    }

    B3::Type returnType = toB3ResultType(&signature);
    auto [patchpoint, handle, prepareForCall] = createCallPatchpoint(m_currentBlock, returnType, wasmCalleeInfo, args);
    // We need to clobber all potential pinned registers since we might be leaving the instance.
    // We pessimistically assume we're always calling something that is bounds checking so
    // because the wasm->wasm thunk unconditionally overrides the size registers.
    // FIXME: We should not have to do this, but the wasm->wasm stub assumes it can
    // use all the pinned registers as scratch: https://bugs.webkit.org/show_bug.cgi?id=172181
    patchpoint->clobberLate(RegisterSetBuilder::wasmPinnedRegisters());

    unsigned patchArgsIndex = patchpoint->reps().size();
    patchpoint->append(calleeCode, ValueRep::SomeRegister);
    patchpoint->append(boxedCalleeCallee, ValueRep::SomeRegister);
    patchArgsIndex += m_proc.resultCount(patchpoint->type());
    patchpoint->setGenerator([this, handle = handle, prepareForCall = prepareForCall, patchArgsIndex](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        if (prepareForCall)
            prepareForCall->run(jit, params);
        if (handle)
            handle->generate(jit, params, this);

        jit.storeWasmCalleeCallee(params[patchArgsIndex + 1].gpr());
        jit.call(params[patchArgsIndex].gpr(), WasmEntryPtrTag);
    });
    auto callResult = patchpoint;

    switch (returnType.kind()) {
    case B3::Void: {
        break;
    }
    case B3::Tuple: {
        const Vector<B3::Type>& tuple = m_proc.tupleForType(returnType);
        for (unsigned i = 0; i < signature.as<FunctionSignature>()->returnCount(); ++i)
            results.append(push<ExtractValue>(m_proc, origin(), tuple[i], callResult, i));
        break;
    }
    default: {
        results.append(push(callResult));
        break;
    }
    }

    // The call could have been to another WebAssembly instance, and / or could have modified our Memory.
    restoreWebAssemblyGlobalState(m_info.memory, instanceValue(), m_currentBlock);

    if (isTailCallInlineCaller) {
        Stack typedResults;
        typedResults.reserveInitialCapacity(results.size());
        for (unsigned i = 0; i < results.size(); ++i)
            typedResults.append(TypedExpression { signature.as<FunctionSignature>()->returnType(i), results[i] });
        ASSERT(m_returnContinuation);
        return addInlinedReturn(WTFMove(typedResults));
    }

    return { };
}

auto OMGIRGenerator::addGrowMemory(ExpressionType delta, ExpressionType& result) -> PartialResult
{
    result = push(callWasmOperation(m_currentBlock, Int32, operationGrowMemory,
        instanceValue(), get(delta)));

    restoreWebAssemblyGlobalState(m_info.memory, instanceValue(), m_currentBlock);

    return { };
}

auto OMGIRGenerator::addCurrentMemory(ExpressionType& result) -> PartialResult
{
    static_assert(sizeof(std::declval<Memory*>()->size()) == sizeof(uint64_t), "codegen relies on this size");

    Value* jsMemory = append<MemoryValue>(heapInstanceFields(), m_proc, Load, Int64, origin(), instanceValue(), safeCast<int32_t>(JSWebAssemblyInstance::offsetOfJSMemory()));
    Value* memory = append<MemoryValue>(heapTop(), m_proc, Load, Int64, origin(), jsMemory, safeCast<int32_t>(JSWebAssemblyMemory::offsetOfMemory()));
    Value* handle = append<MemoryValue>(heapTop(), m_proc, Load, Int64, origin(), memory, safeCast<int32_t>(Memory::offsetOfHandle()));
    Value* size = append<MemoryValue>(heapTop(), m_proc, Load, Int64, origin(), handle, safeCast<int32_t>(BufferMemoryHandle::offsetOfSize()));

    constexpr uint32_t shiftValue = 16;
    static_assert(PageCount::pageSize == 1ull << shiftValue, "This must hold for the code below to be correct.");
    Value* numPages = append<Value>(m_proc, ZShr, origin(),
        size, append<Const32Value>(m_proc, origin(), shiftValue));

    result = push<Value>(m_proc, Trunc, origin(), numPages);

    return { };
}

auto OMGIRGenerator::addMemoryFill(ExpressionType dstAddress, ExpressionType targetValue, ExpressionType count) -> PartialResult
{
    Value* resultValue = callWasmOperation(m_currentBlock, toB3Type(Types::I32), operationWasmMemoryFill,
        instanceValue(),
        get(dstAddress), get(targetValue), get(count));

    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), resultValue, append<Const32Value>(m_proc, origin(), 0)));

        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsMemoryAccess);
        });
    }

    return { };
}

auto OMGIRGenerator::addMemoryInit(unsigned dataSegmentIndex, ExpressionType dstAddress, ExpressionType srcAddress, ExpressionType length) -> PartialResult
{
    Value* resultValue = callWasmOperation(m_currentBlock, toB3Type(Types::I32), operationWasmMemoryInit,
        instanceValue(),
        append<Const32Value>(m_proc, origin(), dataSegmentIndex),
        get(dstAddress), get(srcAddress), get(length));

    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), resultValue, append<Const32Value>(m_proc, origin(), 0)));

        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsMemoryAccess);
        });
    }

    return { };
}

auto OMGIRGenerator::addMemoryCopy(ExpressionType dstAddress, ExpressionType srcAddress, ExpressionType count) -> PartialResult
{
    Value* resultValue = callWasmOperation(m_currentBlock, toB3Type(Types::I32), operationWasmMemoryCopy,
        instanceValue(),
        get(dstAddress), get(srcAddress), get(count));

    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), resultValue, append<Const32Value>(m_proc, origin(), 0)));

        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsMemoryAccess);
        });
    }

    return { };
}

auto OMGIRGenerator::addDataDrop(unsigned dataSegmentIndex) -> PartialResult
{
    callWasmOperation(m_currentBlock, B3::Void, operationWasmDataDrop,
        instanceValue(),
        append<Const32Value>(m_proc, origin(), dataSegmentIndex));

    return { };
}

template<typename... Args>
void OMGIRGenerator::traceValue(Type type, Value* value, Args&&... info)
{
    if constexpr (!WasmOMGIRGeneratorInternal::traceExecution)
        return;
    if (!type.isFuncref() && !type.isVoid())
        return;
    auto* patch = m_proc.add<PatchpointValue>(B3::Void, origin());
    patch->effects = Effects::none();
    patch->effects.controlDependent = true;
    patch->effects.fence = true;
    patch->effects.reads = HeapRange::top();
    patch->effects.writes = HeapRange::top();
    StringPrintStream sb;
    if (m_parser->unreachableBlocks())
        sb.print("(unreachable) ");
    sb.print("TRACE OMG EXECUTION fn[", m_functionIndex, "] stack height ", m_stackSize.value(), " type ", type, " ");
    sb.print(info...);
    dataLogLn("static: ", sb.toString());
    patch->setGenerator([infoString = sb.toString(), type] (CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        JIT_COMMENT(jit, "PROBE FOR ", infoString);
        jit.probeDebug([params, type, infoString](Probe::Context& ctx) {
            auto rep = params[0];
            uint64_t rawVal = 0;
            ASSERT(rep.isGPR() || rep.isFPR());
            if (rep.isGPR())
                rawVal = ctx.gpr(rep.gpr());
            else if (rep.isFPR())
                rawVal = ctx.fpr(rep.fpr());
            else if (rep.isConstant())
                rawVal = rep.value();

            dataLogLn(infoString, " = ", rawVal);

            if (type.isVoid() || !rawVal)
                return;

            JSValue jsValue = JSValue::decode(rawVal);
            RELEASE_ASSERT(jsValue.isCallable() || jsValue.isUndefinedOrNull());
        });
    });
    patch->append(ConstrainedValue(value, ValueRep::SomeRegister));
    m_currentBlock->append(patch);
}

template<typename... Args>
void OMGIRGenerator::traceCF(Args&&... info)
{
    if constexpr (!WasmOMGIRGeneratorInternal::traceExecution)
        return;
    auto* patch = m_proc.add<PatchpointValue>(B3::Void, origin());
    patch->effects = Effects::none();
    patch->effects.controlDependent = true;
    patch->effects.fence = true;
    patch->effects.reads = HeapRange::top();
    patch->effects.writes = HeapRange::top();
    StringPrintStream sb;
    sb.print("TRACE OMG EXECUTION fn[", m_functionIndex, "] stack height ", m_stackSize.value(), " CF ");
    sb.print(info...);
    dataLogLn("static: ", sb.toString());
    patch->setGenerator([infoString = sb.toString()] (CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        JIT_COMMENT(jit, "PROBE FOR ", infoString);
        jit.probeDebug([params, infoString](Probe::Context&) {
            dataLogLn(infoString);
        });
    });
    m_currentBlock->append(patch);

    if (!WasmOMGIRGeneratorInternal::traceStackValues)
        return;
    int i = 0;
    for (auto* val : m_stack) {
        ++i;
        traceValue(Wasm::Types::Void, get(val), " wasm stack[", i, "] = ", *val);
    }

    i = 0;
    for (auto val : m_parser->expressionStack()) {
        ++i;
        traceValue(Wasm::Types::Void, get(val.value()), " parser stack[", i, "] = ", *val.value());
    }

    if (m_parser->unreachableBlocks())
        return;
    if (m_parser->expressionStack().isEmpty() && m_stackSize) {
        dataLogLn("%%%%%%%%%%%%%%%%%%%");
        return;
    }
    if (!m_parser->expressionStack().isEmpty() && !m_stackSize) {
        dataLogLn("$$$$$$$$$$$$$$$$$$$");
        return;
    }
    for (i = 0; i < (int) m_parser->expressionStack().size(); ++i) {
        if (m_parser->expressionStack()[m_parser->expressionStack().size() - i - 1] != m_stack[m_stackSize.value() - i - 1]) {
            dataLogLn("************************");
            return;
        }
    }
}

auto OMGIRGenerator::setLocal(uint32_t index, ExpressionType value) -> PartialResult
{
    ASSERT(m_locals[index]);
    append<VariableValue>(m_proc, B3::Set, origin(), m_locals[index], get(value));
    TRACE_VALUE(m_parser->typeOfLocal(index), get(value), "set_local ", index);
    return { };
}

auto OMGIRGenerator::teeLocal(uint32_t index, ExpressionType value, ExpressionType& result) -> PartialResult
{
    ASSERT(m_locals[index]);
    Value* input = get(value);
    append<VariableValue>(m_proc, B3::Set, origin(), m_locals[index], input);
    result = push(input);
    TRACE_VALUE(m_parser->typeOfLocal(index), input, "tee_local ", index);
    return { };
}

auto OMGIRGenerator::getGlobal(uint32_t index, ExpressionType& result) -> PartialResult
{
    const Wasm::GlobalInformation& global = m_info.globals[index];
    switch (global.bindingMode) {
    case Wasm::GlobalInformation::BindingMode::EmbeddedInInstance: {
        MemoryValue* load = append<MemoryValue>(heapGlobals(), index, m_proc, Load, toB3Type(global.type), origin(), instanceValue(), safeCast<int32_t>(JSWebAssemblyInstance::offsetOfGlobalPtr(m_numImportFunctions, m_info.tableCount(), index)));
        result = push(load);
        break;
    }
    case Wasm::GlobalInformation::BindingMode::Portable: {
        ASSERT(global.mutability == Wasm::Mutability::Mutable);
        MemoryValue* pointer = append<MemoryValue>(heapGlobals(), index, m_proc, Load, B3::Int64, origin(), instanceValue(), safeCast<int32_t>(JSWebAssemblyInstance::offsetOfGlobalPtr(m_numImportFunctions, m_info.tableCount(), index)));
        result = push<MemoryValue>(heapGlobals(), index, m_proc, Load, toB3Type(global.type), origin(), pointer);
        break;
    }
    }
    TRACE_VALUE(global.type, get(result), "get_global ", index);

    return { };
}

auto OMGIRGenerator::setGlobal(uint32_t index, ExpressionType value) -> PartialResult
{
    const Wasm::GlobalInformation& global = m_info.globals[index];
    ASSERT(toB3Type(global.type) == value->type());
    TRACE_VALUE(global.type, get(value), "set_global ", index);

    switch (global.bindingMode) {
    case Wasm::GlobalInformation::BindingMode::EmbeddedInInstance: {
        append<MemoryValue>(heapGlobals(), index, m_proc, Store, origin(), get(value), instanceValue(), safeCast<int32_t>(JSWebAssemblyInstance::offsetOfGlobalPtr(m_numImportFunctions, m_info.tableCount(), index)));
        if (isRefType(global.type))
            emitWriteBarrierForJSWrapper();
        break;
    }
    case Wasm::GlobalInformation::BindingMode::Portable: {
        ASSERT(global.mutability == Wasm::Mutability::Mutable);
        MemoryValue* pointer = append<MemoryValue>(heapGlobals(), index, m_proc, Load, B3::Int64, origin(), instanceValue(), safeCast<int32_t>(JSWebAssemblyInstance::offsetOfGlobalPtr(m_numImportFunctions, m_info.tableCount(), index)));
        append<MemoryValue>(heapTop(), m_proc, Store, origin(), get(value), pointer->as<Value>());
        // We emit a write-barrier onto JSWebAssemblyGlobal, not JSWebAssemblyInstance.
        if (isRefType(global.type)) {
            Value* cell = append<MemoryValue>(heapTop(), m_proc, Load, pointerType(), origin(), pointer, Wasm::Global::offsetOfOwner() - Wasm::Global::offsetOfValue());
            Value* cellState = append<MemoryValue>(heapTop(), m_proc, Load8Z, Int32, origin(), cell, safeCast<int32_t>(JSCell::cellStateOffset()));
            Value* vm = append<MemoryValue>(heapTop(), m_proc, Load, pointerType(), origin(), instanceValue(), safeCast<int32_t>(JSWebAssemblyInstance::offsetOfVM()));
            Value* threshold = append<MemoryValue>(heapTop(), m_proc, Load, Int32, origin(), vm, safeCast<int32_t>(VM::offsetOfHeapBarrierThreshold()));

            BasicBlock* fenceCheckPath = m_proc.addBlock();
            BasicBlock* fencePath = m_proc.addBlock();
            BasicBlock* doSlowPath = m_proc.addBlock();
            BasicBlock* continuation = m_proc.addBlock();

            m_currentBlock->appendNewControlValue(m_proc, B3::Branch, origin(),
                append<Value>(m_proc, Above, origin(), cellState, threshold),
                FrequentedBlock(continuation), FrequentedBlock(fenceCheckPath, FrequencyClass::Rare));
            fenceCheckPath->addPredecessor(m_currentBlock);
            continuation->addPredecessor(m_currentBlock);
            m_currentBlock = fenceCheckPath;

            Value* shouldFence = append<MemoryValue>(heapTop(), m_proc, Load8Z, Int32, origin(), vm, safeCast<int32_t>(VM::offsetOfHeapMutatorShouldBeFenced()));
            m_currentBlock->appendNewControlValue(m_proc, B3::Branch, origin(),
                shouldFence,
                FrequentedBlock(fencePath), FrequentedBlock(doSlowPath));
            fencePath->addPredecessor(m_currentBlock);
            doSlowPath->addPredecessor(m_currentBlock);
            m_currentBlock = fencePath;

            B3::PatchpointValue* doFence = append<B3::PatchpointValue>(m_proc, B3::Void, origin());
            doFence->setGenerator([] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
                jit.memoryFence();
            });

            Value* cellStateLoadAfterFence = append<MemoryValue>(heapTop(), m_proc, Load8Z, Int32, origin(), cell, safeCast<int32_t>(JSCell::cellStateOffset()));
            m_currentBlock->appendNewControlValue(m_proc, B3::Branch, origin(),
                append<Value>(m_proc, Above, origin(), cellStateLoadAfterFence, append<Const32Value>(m_proc, origin(), blackThreshold)),
                FrequentedBlock(continuation), FrequentedBlock(doSlowPath, FrequencyClass::Rare));
            doSlowPath->addPredecessor(m_currentBlock);
            continuation->addPredecessor(m_currentBlock);
            m_currentBlock = doSlowPath;

            callWasmOperation(m_currentBlock, B3::Void, operationWasmWriteBarrierSlowPath, cell, vm);
            m_currentBlock->appendNewControlValue(m_proc, Jump, origin(), continuation);

            continuation->addPredecessor(m_currentBlock);
            m_currentBlock = continuation;
        }
        break;
    }
    }
    return { };
}

inline void OMGIRGenerator::emitWriteBarrierForJSWrapper()
{
    emitWriteBarrier(instanceValue(), instanceValue());
}

inline void OMGIRGenerator::emitWriteBarrier(Value* cell, Value* instanceCell)
{
    Value* cellState = append<MemoryValue>(heapTop(), m_proc, Load8Z, Int32, origin(), cell, safeCast<int32_t>(JSCell::cellStateOffset()));
    Value* vm = append<MemoryValue>(heapTop(), m_proc, Load, pointerType(), origin(), instanceCell, safeCast<int32_t>(JSWebAssemblyInstance::offsetOfVM()));
    Value* threshold = append<MemoryValue>(heapTop(), m_proc, Load, Int32, origin(), vm, safeCast<int32_t>(VM::offsetOfHeapBarrierThreshold()));

    BasicBlock* fenceCheckPath = m_proc.addBlock();
    BasicBlock* fencePath = m_proc.addBlock();
    BasicBlock* doSlowPath = m_proc.addBlock();
    BasicBlock* continuation = m_proc.addBlock();

    m_currentBlock->appendNewControlValue(m_proc, B3::Branch, origin(),
        append<Value>(m_proc, Above, origin(), cellState, threshold),
        FrequentedBlock(continuation), FrequentedBlock(fenceCheckPath, FrequencyClass::Rare));
    fenceCheckPath->addPredecessor(m_currentBlock);
    continuation->addPredecessor(m_currentBlock);
    m_currentBlock = fenceCheckPath;

    Value* shouldFence = append<MemoryValue>(heapTop(), m_proc, Load8Z, Int32, origin(), vm, safeCast<int32_t>(VM::offsetOfHeapMutatorShouldBeFenced()));
    m_currentBlock->appendNewControlValue(m_proc, B3::Branch, origin(),
        shouldFence,
        FrequentedBlock(fencePath), FrequentedBlock(doSlowPath));
    fencePath->addPredecessor(m_currentBlock);
    doSlowPath->addPredecessor(m_currentBlock);
    m_currentBlock = fencePath;

    B3::PatchpointValue* doFence = append<B3::PatchpointValue>(m_proc, B3::Void, origin());
    doFence->setGenerator([] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
        jit.memoryFence();
    });

    Value* cellStateLoadAfterFence = append<MemoryValue>(heapTop(), m_proc, Load8Z, Int32, origin(), cell, safeCast<int32_t>(JSCell::cellStateOffset()));
    m_currentBlock->appendNewControlValue(m_proc, B3::Branch, origin(),
        append<Value>(m_proc, Above, origin(), cellStateLoadAfterFence, append<Const32Value>(m_proc, origin(), blackThreshold)),
        FrequentedBlock(continuation), FrequentedBlock(doSlowPath, FrequencyClass::Rare));
    doSlowPath->addPredecessor(m_currentBlock);
    continuation->addPredecessor(m_currentBlock);
    m_currentBlock = doSlowPath;

    callWasmOperation(m_currentBlock, B3::Void, operationWasmWriteBarrierSlowPath, cell, vm);
    m_currentBlock->appendNewControlValue(m_proc, Jump, origin(), continuation);

    continuation->addPredecessor(m_currentBlock);
    m_currentBlock = continuation;
}

inline Value* OMGIRGenerator::emitCheckAndPreparePointer(Value* pointer, uint32_t offset, uint32_t sizeOfOperation)
{
    static_assert(GPRInfo::wasmBaseMemoryPointer != InvalidGPRReg);

    switch (m_mode) {
    case MemoryMode::BoundsChecking: {
        // We're not using signal handling only when the memory is not shared.
        // Regardless of signaling, we must check that no memory access exceeds the current memory size.
        static_assert(GPRInfo::wasmBoundsCheckingSizeRegister != InvalidGPRReg);
        ASSERT(sizeOfOperation + offset > offset);
        append<WasmBoundsCheckValue>(m_proc, origin(), GPRInfo::wasmBoundsCheckingSizeRegister, pointer, sizeOfOperation + offset - 1);
        break;
    }

    case MemoryMode::Signaling: {
        // We've virtually mapped 4GiB+redzone for this memory. Only the user-allocated pages are addressable, contiguously in range [0, current],
        // and everything above is mapped PROT_NONE. We don't need to perform any explicit bounds check in the 4GiB range because WebAssembly register
        // memory accesses are 32-bit. However WebAssembly register + offset accesses perform the addition in 64-bit which can push an access above
        // the 32-bit limit (the offset is unsigned 32-bit). The redzone will catch most small offsets, and we'll explicitly bounds check any
        // register + large offset access. We don't think this will be generated frequently.
        //
        // We could check that register + large offset doesn't exceed 4GiB+redzone since that's technically the limit we need to avoid overflowing the
        // PROT_NONE region, but it's better if we use a smaller immediate because it can codegens better. We know that anything equal to or greater
        // than the declared 'maximum' will trap, so we can compare against that number. If there was no declared 'maximum' then we still know that
        // any access equal to or greater than 4GiB will trap, no need to add the redzone.
        if (offset >= Memory::fastMappedRedzoneBytes()) {
            size_t maximum = m_info.memory.maximum() ? m_info.memory.maximum().bytes() : std::numeric_limits<uint32_t>::max();
            append<WasmBoundsCheckValue>(m_proc, origin(), pointer, sizeOfOperation + offset - 1, maximum);
        }
        break;
    }
    }

    pointer = append<Value>(m_proc, ZExt32, origin(), pointer);
    return append<WasmAddressValue>(m_proc, origin(), pointer, GPRInfo::wasmBaseMemoryPointer);
}

inline uint32_t sizeOfLoadOp(LoadOpType op)
{
    switch (op) {
    case LoadOpType::I32Load8S:
    case LoadOpType::I32Load8U:
    case LoadOpType::I64Load8S:
    case LoadOpType::I64Load8U:
        return 1;
    case LoadOpType::I32Load16S:
    case LoadOpType::I64Load16S:
    case LoadOpType::I32Load16U:
    case LoadOpType::I64Load16U:
        return 2;
    case LoadOpType::I32Load:
    case LoadOpType::I64Load32S:
    case LoadOpType::I64Load32U:
    case LoadOpType::F32Load:
        return 4;
    case LoadOpType::I64Load:
    case LoadOpType::F64Load:
        return 8;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

inline B3::Kind OMGIRGenerator::memoryKind(B3::Opcode memoryOp)
{
    if (useSignalingMemory() || m_info.memory.isShared())
        return trapping(memoryOp);
    return memoryOp;
}

inline Value* OMGIRGenerator::emitLoadOp(LoadOpType op, Value* pointer, uint32_t uoffset)
{
    int32_t offset = fixupPointerPlusOffset(pointer, uoffset);

    switch (op) {
    case LoadOpType::I32Load8S: {
        return append<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(Load8S), origin(), pointer, offset);
    }

    case LoadOpType::I64Load8S: {
        Value* value = append<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(Load8S), origin(), pointer, offset);
        return append<Value>(m_proc, SExt32, origin(), value);
    }

    case LoadOpType::I32Load8U: {
        return append<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(Load8Z), origin(), pointer, offset);
    }

    case LoadOpType::I64Load8U: {
        Value* value = append<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(Load8Z), origin(), pointer, offset);
        return append<Value>(m_proc, ZExt32, origin(), value);
    }

    case LoadOpType::I32Load16S: {
        return append<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(Load16S), origin(), pointer, offset);
    }

    case LoadOpType::I64Load16S: {
        Value* value = append<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(Load16S), origin(), pointer, offset);
        return append<Value>(m_proc, SExt32, origin(), value);
    }

    case LoadOpType::I32Load16U: {
        return append<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(Load16Z), origin(), pointer, offset);
    }

    case LoadOpType::I64Load16U: {
        Value* value = append<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(Load16Z), origin(), pointer, offset);
        return append<Value>(m_proc, ZExt32, origin(), value);
    }

    case LoadOpType::I32Load: {
        return append<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(Load), Int32, origin(), pointer, offset);
    }

    case LoadOpType::I64Load32U: {
        Value* value = append<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(Load), Int32, origin(), pointer, offset);
        return append<Value>(m_proc, ZExt32, origin(), value);
    }

    case LoadOpType::I64Load32S: {
        Value* value = append<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(Load), Int32, origin(), pointer, offset);
        return append<Value>(m_proc, SExt32, origin(), value);
    }

    case LoadOpType::I64Load: {
        return append<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(Load), Int64, origin(), pointer, offset);
    }

    case LoadOpType::F32Load: {
        return append<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(Load), Float, origin(), pointer, offset);
    }

    case LoadOpType::F64Load: {
        return append<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(Load), Double, origin(), pointer, offset);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

auto OMGIRGenerator::load(LoadOpType op, ExpressionType pointerVar, ExpressionType& result, uint32_t offset) -> PartialResult
{
    Value* pointer = get(pointerVar);
    ASSERT(pointer->type() == Int32);

    if (UNLIKELY(sumOverflows<uint32_t>(offset, sizeOfLoadOp(op)))) {
        // FIXME: Even though this is provably out of bounds, it's not a validation error, so we have to handle it
        // as a runtime exception. However, this may change: https://bugs.webkit.org/show_bug.cgi?id=166435
        B3::PatchpointValue* throwException = append<B3::PatchpointValue>(m_proc, B3::Void, origin());
        throwException->setGenerator([this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsMemoryAccess);
        });

        switch (op) {
        case LoadOpType::I32Load8S:
        case LoadOpType::I32Load16S:
        case LoadOpType::I32Load:
        case LoadOpType::I32Load16U:
        case LoadOpType::I32Load8U:
            result = push(constant(Int32, 0));
            break;
        case LoadOpType::I64Load8S:
        case LoadOpType::I64Load8U:
        case LoadOpType::I64Load16S:
        case LoadOpType::I64Load32U:
        case LoadOpType::I64Load32S:
        case LoadOpType::I64Load:
        case LoadOpType::I64Load16U:
            result = push(constant(Int64, 0));
            break;
        case LoadOpType::F32Load:
            result = push(constant(Float, 0));
            break;
        case LoadOpType::F64Load:
            result = push(constant(Double, 0));
            break;
        }
    } else
        result = push(emitLoadOp(op, emitCheckAndPreparePointer(pointer, offset, sizeOfLoadOp(op)), offset));

    return { };
}

inline uint32_t sizeOfStoreOp(StoreOpType op)
{
    switch (op) {
    case StoreOpType::I32Store8:
    case StoreOpType::I64Store8:
        return 1;
    case StoreOpType::I32Store16:
    case StoreOpType::I64Store16:
        return 2;
    case StoreOpType::I32Store:
    case StoreOpType::I64Store32:
    case StoreOpType::F32Store:
        return 4;
    case StoreOpType::I64Store:
    case StoreOpType::F64Store:
        return 8;
    }
    RELEASE_ASSERT_NOT_REACHED();
}


inline void OMGIRGenerator::emitStoreOp(StoreOpType op, Value* pointer, Value* value, uint32_t uoffset)
{
    int32_t offset = fixupPointerPlusOffset(pointer, uoffset);

    switch (op) {
    case StoreOpType::I64Store8:
        value = append<Value>(m_proc, Trunc, origin(), value);
        FALLTHROUGH;

    case StoreOpType::I32Store8:
        append<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(Store8), origin(), value, pointer, offset);
        return;

    case StoreOpType::I64Store16:
        value = append<Value>(m_proc, Trunc, origin(), value);
        FALLTHROUGH;

    case StoreOpType::I32Store16:
        append<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(Store16), origin(), value, pointer, offset);
        return;

    case StoreOpType::I64Store32:
        value = append<Value>(m_proc, Trunc, origin(), value);
        FALLTHROUGH;

    case StoreOpType::I64Store:
    case StoreOpType::I32Store:
    case StoreOpType::F32Store:
    case StoreOpType::F64Store:
        append<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(Store), origin(), value, pointer, offset);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

auto OMGIRGenerator::store(StoreOpType op, ExpressionType pointerVar, ExpressionType valueVar, uint32_t offset) -> PartialResult
{
    Value* pointer = get(pointerVar);
    Value* value = get(valueVar);
    ASSERT(pointer->type() == Int32);

    if (UNLIKELY(sumOverflows<uint32_t>(offset, sizeOfStoreOp(op)))) {
        // FIXME: Even though this is provably out of bounds, it's not a validation error, so we have to handle it
        // as a runtime exception. However, this may change: https://bugs.webkit.org/show_bug.cgi?id=166435
        B3::PatchpointValue* throwException = append<B3::PatchpointValue>(m_proc, B3::Void, origin());
        throwException->setGenerator([this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsMemoryAccess);
        });
    } else
        emitStoreOp(op, emitCheckAndPreparePointer(pointer, offset, sizeOfStoreOp(op)), value, offset);

    return { };
}

inline Width accessWidth(ExtAtomicOpType op)
{
    return widthForBytes(1 << memoryLog2Alignment(op));
}

inline uint32_t sizeOfAtomicOpMemoryAccess(ExtAtomicOpType op)
{
    return bytesForWidth(accessWidth(op));
}

inline Value* OMGIRGenerator::sanitizeAtomicResult(ExtAtomicOpType op, Type valueType, Value* result)
{
    auto sanitize32 = [&](Value* result) {
        switch (accessWidth(op)) {
        case Width8:
            return append<Value>(m_proc, BitAnd, origin(), result, constant(Int32, 0xff));
        case Width16:
            return append<Value>(m_proc, BitAnd, origin(), result, constant(Int32, 0xffff));
        default:
            return result;
        }
    };

    switch (valueType.kind) {
    case TypeKind::I64: {
        if (accessWidth(op) == Width64)
            return result;
        return append<Value>(m_proc, ZExt32, origin(), sanitize32(result));
    }
    case TypeKind::I32:
        return sanitize32(result);
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return nullptr;
    }
}

Value* OMGIRGenerator::fixupPointerPlusOffsetForAtomicOps(ExtAtomicOpType op, Value* ptr, uint32_t offset)
{
    auto pointer = append<Value>(m_proc, Add, origin(), ptr, append<Const64Value>(m_proc, origin(), offset));
    if (accessWidth(op) != Width8) {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, BitAnd, origin(), pointer, constant(pointerType(), sizeOfAtomicOpMemoryAccess(op) - 1)));
        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsMemoryAccess);
        });
    }
    return pointer;
}

inline Value* OMGIRGenerator::emitAtomicLoadOp(ExtAtomicOpType op, Type valueType, Value* pointer, uint32_t uoffset)
{
    pointer = fixupPointerPlusOffsetForAtomicOps(op, pointer, uoffset);

    Value* value = nullptr;
    switch (accessWidth(op)) {
    case Width8:
    case Width16:
    case Width32:
        value = constant(Int32, 0);
        break;
    case Width64:
        value = constant(Int64, 0);
        break;
    case Width128:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }

    return sanitizeAtomicResult(op, valueType, append<AtomicValue>(heapWasmMemory(), m_proc, memoryKind(AtomicXchgAdd), origin(), accessWidth(op), value, pointer));
}

auto OMGIRGenerator::atomicLoad(ExtAtomicOpType op, Type valueType, ExpressionType pointer, ExpressionType& result, uint32_t offset) -> PartialResult
{
    ASSERT(pointer->type() == Int32);

    if (UNLIKELY(sumOverflows<uint32_t>(offset, sizeOfAtomicOpMemoryAccess(op)))) {
        // FIXME: Even though this is provably out of bounds, it's not a validation error, so we have to handle it
        // as a runtime exception. However, this may change: https://bugs.webkit.org/show_bug.cgi?id=166435
        B3::PatchpointValue* throwException = append<B3::PatchpointValue>(m_proc, B3::Void, origin());
        throwException->setGenerator([this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsMemoryAccess);
        });

        switch (valueType.kind) {
        case TypeKind::I32:
            result = push(constant(Int32, 0));
            break;
        case TypeKind::I64:
            result = push(constant(Int64, 0));
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }
    } else
        result = push(emitAtomicLoadOp(op, valueType, emitCheckAndPreparePointer(get(pointer), offset, sizeOfAtomicOpMemoryAccess(op)), offset));

    return { };
}

inline void OMGIRGenerator::emitAtomicStoreOp(ExtAtomicOpType op, Type valueType, Value* pointer, Value* value, uint32_t uoffset)
{
    pointer = fixupPointerPlusOffsetForAtomicOps(op, pointer, uoffset);

    if (valueType.isI64() && accessWidth(op) != Width64)
        value = append<B3::Value>(m_proc, B3::Trunc, Origin(), value);
    append<AtomicValue>(heapWasmMemory(), m_proc, memoryKind(AtomicXchg), origin(), accessWidth(op), value, pointer);
}

auto OMGIRGenerator::atomicStore(ExtAtomicOpType op, Type valueType, ExpressionType pointer, ExpressionType value, uint32_t offset) -> PartialResult
{
    ASSERT(pointer->type() == Int32);

    if (UNLIKELY(sumOverflows<uint32_t>(offset, sizeOfAtomicOpMemoryAccess(op)))) {
        // FIXME: Even though this is provably out of bounds, it's not a validation error, so we have to handle it
        // as a runtime exception. However, this may change: https://bugs.webkit.org/show_bug.cgi?id=166435
        B3::PatchpointValue* throwException = append<B3::PatchpointValue>(m_proc, B3::Void, origin());
        throwException->setGenerator([this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsMemoryAccess);
        });
    } else
        emitAtomicStoreOp(op, valueType, emitCheckAndPreparePointer(get(pointer), offset, sizeOfAtomicOpMemoryAccess(op)), get(value), offset);

    return { };
}

inline Value* OMGIRGenerator::emitAtomicBinaryRMWOp(ExtAtomicOpType op, Type valueType, Value* pointer, Value* value, uint32_t uoffset)
{
    pointer = fixupPointerPlusOffsetForAtomicOps(op, pointer, uoffset);

    B3::Opcode opcode = B3::Nop;
    switch (op) {
    case ExtAtomicOpType::I32AtomicRmw8AddU:
    case ExtAtomicOpType::I32AtomicRmw16AddU:
    case ExtAtomicOpType::I32AtomicRmwAdd:
    case ExtAtomicOpType::I64AtomicRmw8AddU:
    case ExtAtomicOpType::I64AtomicRmw16AddU:
    case ExtAtomicOpType::I64AtomicRmw32AddU:
    case ExtAtomicOpType::I64AtomicRmwAdd:
        opcode = AtomicXchgAdd;
        break;
    case ExtAtomicOpType::I32AtomicRmw8SubU:
    case ExtAtomicOpType::I32AtomicRmw16SubU:
    case ExtAtomicOpType::I32AtomicRmwSub:
    case ExtAtomicOpType::I64AtomicRmw8SubU:
    case ExtAtomicOpType::I64AtomicRmw16SubU:
    case ExtAtomicOpType::I64AtomicRmw32SubU:
    case ExtAtomicOpType::I64AtomicRmwSub:
        opcode = AtomicXchgSub;
        break;
    case ExtAtomicOpType::I32AtomicRmw8AndU:
    case ExtAtomicOpType::I32AtomicRmw16AndU:
    case ExtAtomicOpType::I32AtomicRmwAnd:
    case ExtAtomicOpType::I64AtomicRmw8AndU:
    case ExtAtomicOpType::I64AtomicRmw16AndU:
    case ExtAtomicOpType::I64AtomicRmw32AndU:
    case ExtAtomicOpType::I64AtomicRmwAnd:
        opcode = AtomicXchgAnd;
        break;
    case ExtAtomicOpType::I32AtomicRmw8OrU:
    case ExtAtomicOpType::I32AtomicRmw16OrU:
    case ExtAtomicOpType::I32AtomicRmwOr:
    case ExtAtomicOpType::I64AtomicRmw8OrU:
    case ExtAtomicOpType::I64AtomicRmw16OrU:
    case ExtAtomicOpType::I64AtomicRmw32OrU:
    case ExtAtomicOpType::I64AtomicRmwOr:
        opcode = AtomicXchgOr;
        break;
    case ExtAtomicOpType::I32AtomicRmw8XorU:
    case ExtAtomicOpType::I32AtomicRmw16XorU:
    case ExtAtomicOpType::I32AtomicRmwXor:
    case ExtAtomicOpType::I64AtomicRmw8XorU:
    case ExtAtomicOpType::I64AtomicRmw16XorU:
    case ExtAtomicOpType::I64AtomicRmw32XorU:
    case ExtAtomicOpType::I64AtomicRmwXor:
        opcode = AtomicXchgXor;
        break;
    case ExtAtomicOpType::I32AtomicRmw8XchgU:
    case ExtAtomicOpType::I32AtomicRmw16XchgU:
    case ExtAtomicOpType::I32AtomicRmwXchg:
    case ExtAtomicOpType::I64AtomicRmw8XchgU:
    case ExtAtomicOpType::I64AtomicRmw16XchgU:
    case ExtAtomicOpType::I64AtomicRmw32XchgU:
    case ExtAtomicOpType::I64AtomicRmwXchg:
        opcode = AtomicXchg;
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }

    if (valueType.isI64() && accessWidth(op) != Width64)
        value = append<B3::Value>(m_proc, B3::Trunc, Origin(), value);

    return sanitizeAtomicResult(op, valueType, append<AtomicValue>(heapWasmMemory(), m_proc, memoryKind(opcode), origin(), accessWidth(op), value, pointer));
}

auto OMGIRGenerator::atomicBinaryRMW(ExtAtomicOpType op, Type valueType, ExpressionType pointer, ExpressionType value, ExpressionType& result, uint32_t offset) -> PartialResult
{
    ASSERT(pointer->type() == Int32);

    if (UNLIKELY(sumOverflows<uint32_t>(offset, sizeOfAtomicOpMemoryAccess(op)))) {
        // FIXME: Even though this is provably out of bounds, it's not a validation error, so we have to handle it
        // as a runtime exception. However, this may change: https://bugs.webkit.org/show_bug.cgi?id=166435
        B3::PatchpointValue* throwException = append<B3::PatchpointValue>(m_proc, B3::Void, origin());
        throwException->setGenerator([this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsMemoryAccess);
        });

        switch (valueType.kind) {
        case TypeKind::I32:
            result = push(constant(Int32, 0));
            break;
        case TypeKind::I64:
            result = push(constant(Int64, 0));
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }
    } else
        result = push(emitAtomicBinaryRMWOp(op, valueType, emitCheckAndPreparePointer(get(pointer), offset, sizeOfAtomicOpMemoryAccess(op)), get(value), offset));

    return { };
}

Value* OMGIRGenerator::emitAtomicCompareExchange(ExtAtomicOpType op, Type valueType, Value* pointer, Value* expected, Value* value, uint32_t uoffset)
{
    pointer = fixupPointerPlusOffsetForAtomicOps(op, pointer, uoffset);

    Width accessWidth = Wasm::accessWidth(op);

    if (widthForType(toB3Type(valueType)) == accessWidth)
        return sanitizeAtomicResult(op, valueType, append<AtomicValue>(heapWasmMemory(), m_proc, memoryKind(AtomicStrongCAS), origin(), accessWidth, expected, value, pointer));

    Value* maximum = nullptr;
    switch (valueType.kind) {
    case TypeKind::I64: {
        switch (accessWidth) {
        case Width8:
            maximum = constant(Int64, UINT8_MAX);
            break;
        case Width16:
            maximum = constant(Int64, UINT16_MAX);
            break;
        case Width32:
            maximum = constant(Int64, UINT32_MAX);
            break;
        case Width64:
        case Width128:
            RELEASE_ASSERT_NOT_REACHED();
        }
        break;
    }
    case TypeKind::I32:
        switch (accessWidth) {
        case Width8:
            maximum = constant(Int32, UINT8_MAX);
            break;
        case Width16:
            maximum = constant(Int32, UINT16_MAX);
            break;
        case Width32:
        case Width64:
        case Width128:
            RELEASE_ASSERT_NOT_REACHED();
        }
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }

    auto truncatedExpected = expected;
    auto truncatedValue = value;

    truncatedExpected = append<B3::Value>(m_proc, B3::BitAnd, origin(), maximum, expected);

    if (valueType.isI64()) {
        truncatedExpected = append<B3::Value>(m_proc, B3::Trunc, Origin(), expected);
        truncatedValue = append<B3::Value>(m_proc, B3::Trunc, Origin(), value);
    }

    auto result = append<AtomicValue>(heapWasmMemory(), m_proc, memoryKind(AtomicStrongCAS), origin(), accessWidth, truncatedExpected, truncatedValue, pointer);
    return sanitizeAtomicResult(op, valueType, result);
}

void OMGIRGenerator::emitStructSet(Value* structValue, uint32_t fieldIndex, const StructType& structType, Value* argument)
{
    auto fieldType = structType.field(fieldIndex).type;
    Value* payloadBase = append<MemoryValue>(heapTop(), m_proc, memoryKind(Load), Int64, origin(), structValue, JSWebAssemblyStruct::offsetOfPayload());
    int32_t fieldOffset = fixupPointerPlusOffset(payloadBase, *structType.offsetOfField(fieldIndex));

    if (fieldType.is<PackedType>()) {
        switch (structType.field(fieldIndex).type.as<PackedType>()) {
        case PackedType::I8:
            append<MemoryValue>(heapTop(), m_proc, memoryKind(Store8), origin(), argument, payloadBase, fieldOffset);
            return;
        case PackedType::I16:
            append<MemoryValue>(heapTop(), m_proc, memoryKind(Store16), origin(), argument, payloadBase, fieldOffset);
            return;
        }
    }

    ASSERT(fieldType.is<Type>());
    append<MemoryValue>(heapTop(), m_proc, memoryKind(Store), origin(), argument, payloadBase, fieldOffset);
    if (isRefType(fieldType.unpacked()))
        emitWriteBarrier(structValue, instanceValue());
}

auto OMGIRGenerator::atomicCompareExchange(ExtAtomicOpType op, Type valueType, ExpressionType pointer, ExpressionType expected, ExpressionType value, ExpressionType& result, uint32_t offset) -> PartialResult
{
    ASSERT(pointer->type() == Int32);

    if (UNLIKELY(sumOverflows<uint32_t>(offset, sizeOfAtomicOpMemoryAccess(op)))) {
        // FIXME: Even though this is provably out of bounds, it's not a validation error, so we have to handle it
        // as a runtime exception. However, this may change: https://bugs.webkit.org/show_bug.cgi?id=166435
        B3::PatchpointValue* throwException = append<B3::PatchpointValue>(m_proc, B3::Void, origin());
        throwException->setGenerator([this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsMemoryAccess);
        });

        switch (valueType.kind) {
        case TypeKind::I32:
            result = push(constant(Int32, 0));
            break;
        case TypeKind::I64:
            result = push(constant(Int64, 0));
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }
    } else
        result = push(emitAtomicCompareExchange(op, valueType, emitCheckAndPreparePointer(get(pointer), offset, sizeOfAtomicOpMemoryAccess(op)), get(expected), get(value), offset));

    return { };
}

auto OMGIRGenerator::atomicWait(ExtAtomicOpType op, ExpressionType pointerVar, ExpressionType valueVar, ExpressionType timeoutVar, ExpressionType& result, uint32_t offset) -> PartialResult
{
    Value* pointer = get(pointerVar);
    Value* value = get(valueVar);
    Value* timeout = get(timeoutVar);
    Value* resultValue = nullptr;
    if (op == ExtAtomicOpType::MemoryAtomicWait32) {
        resultValue = callWasmOperation(m_currentBlock, Int32, operationMemoryAtomicWait32,
            instanceValue(), pointer, append<Const32Value>(m_proc, origin(), offset), value, timeout);
    } else {
        resultValue = callWasmOperation(m_currentBlock, Int32, operationMemoryAtomicWait64,
            instanceValue(), pointer, append<Const32Value>(m_proc, origin(), offset), value, timeout);
    }

    {
        result = push(resultValue);
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, LessThan, origin(), resultValue, append<Const32Value>(m_proc, origin(), 0)));

        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsMemoryAccess);
        });
    }

    return { };
}

auto OMGIRGenerator::atomicNotify(ExtAtomicOpType, ExpressionType pointer, ExpressionType count, ExpressionType& result, uint32_t offset) -> PartialResult
{
    Value* resultValue = callWasmOperation(m_currentBlock, Int32, operationMemoryAtomicNotify,
        instanceValue(), get(pointer), append<Const32Value>(m_proc, origin(), offset), get(count));
    {
        result = push(resultValue);
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, LessThan, origin(), resultValue, append<Const32Value>(m_proc, origin(), 0)));

        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsMemoryAccess);
        });
    }

    return { };
}

auto OMGIRGenerator::atomicFence(ExtAtomicOpType, uint8_t) -> PartialResult
{
    append<FenceValue>(m_proc, origin());
    return { };
}

auto OMGIRGenerator::truncSaturated(Ext1OpType op, ExpressionType argVar, ExpressionType& result, Type returnType, Type) -> PartialResult
{
    Value* arg = get(argVar);
    Value* maxFloat = nullptr;
    Value* minFloat = nullptr;
    Value* signBitConstant = nullptr;
    bool requiresMacroScratchRegisters = false;
    switch (op) {
    case Ext1OpType::I32TruncSatF32S:
        maxFloat = constant(Float, bitwise_cast<uint32_t>(-static_cast<float>(std::numeric_limits<int32_t>::min())));
        minFloat = constant(Float, bitwise_cast<uint32_t>(static_cast<float>(std::numeric_limits<int32_t>::min())));
        break;
    case Ext1OpType::I32TruncSatF32U:
        maxFloat = constant(Float, bitwise_cast<uint32_t>(static_cast<float>(std::numeric_limits<int32_t>::min()) * static_cast<float>(-2.0)));
        minFloat = constant(Float, bitwise_cast<uint32_t>(static_cast<float>(-1.0)));
        break;
    case Ext1OpType::I32TruncSatF64S:
        maxFloat = constant(Double, bitwise_cast<uint64_t>(-static_cast<double>(std::numeric_limits<int32_t>::min())));
        minFloat = constant(Double, bitwise_cast<uint64_t>(static_cast<double>(std::numeric_limits<int32_t>::min()) - 1.0));
        break;
    case Ext1OpType::I32TruncSatF64U:
        maxFloat = constant(Double, bitwise_cast<uint64_t>(static_cast<double>(std::numeric_limits<int32_t>::min()) * -2.0));
        minFloat = constant(Double, bitwise_cast<uint64_t>(-1.0));
        break;
    case Ext1OpType::I64TruncSatF32S:
        maxFloat = constant(Float, bitwise_cast<uint32_t>(-static_cast<float>(std::numeric_limits<int64_t>::min())));
        minFloat = constant(Float, bitwise_cast<uint32_t>(static_cast<float>(std::numeric_limits<int64_t>::min())));
        break;
    case Ext1OpType::I64TruncSatF32U:
        maxFloat = constant(Float, bitwise_cast<uint32_t>(static_cast<float>(std::numeric_limits<int64_t>::min()) * static_cast<float>(-2.0)));
        minFloat = constant(Float, bitwise_cast<uint32_t>(static_cast<float>(-1.0)));
        // Since x86 doesn't have an instruction to convert floating points to unsigned integers, we at least try to do the smart thing if
        // the numbers would be positive anyway as a signed integer. Since we cannot materialize constants into fprs we have b3 do it
        // so we can pool them if needed.
        if (isX86())
            signBitConstant = constant(Float, bitwise_cast<uint32_t>(static_cast<float>(std::numeric_limits<uint64_t>::max() - std::numeric_limits<int64_t>::max())));
        requiresMacroScratchRegisters = true;
        break;
    case Ext1OpType::I64TruncSatF64S:
        maxFloat = constant(Double, bitwise_cast<uint64_t>(-static_cast<double>(std::numeric_limits<int64_t>::min())));
        minFloat = constant(Double, bitwise_cast<uint64_t>(static_cast<double>(std::numeric_limits<int64_t>::min())));
        break;
    case Ext1OpType::I64TruncSatF64U:
        maxFloat = constant(Double, bitwise_cast<uint64_t>(static_cast<double>(std::numeric_limits<int64_t>::min()) * -2.0));
        minFloat = constant(Double, bitwise_cast<uint64_t>(-1.0));
        // Since x86 doesn't have an instruction to convert floating points to unsigned integers, we at least try to do the smart thing if
        // the numbers are would be positive anyway as a signed integer. Since we cannot materialize constants into fprs we have b3 do it
        // so we can pool them if needed.
        if (isX86())
            signBitConstant = constant(Double, bitwise_cast<uint64_t>(static_cast<double>(std::numeric_limits<uint64_t>::max() - std::numeric_limits<int64_t>::max())));
        requiresMacroScratchRegisters = true;
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }

    PatchpointValue* patchpoint = append<PatchpointValue>(m_proc, toB3Type(returnType), origin());
    patchpoint->append(arg, ValueRep::SomeRegister);
    if (requiresMacroScratchRegisters) {
        if (isX86()) {
            ASSERT(signBitConstant);
            patchpoint->append(signBitConstant, ValueRep::SomeRegister);
            patchpoint->numFPScratchRegisters = 1;
        }
        patchpoint->clobber(RegisterSetBuilder::macroClobberedGPRs());
    }
    patchpoint->setGenerator([=] (CCallHelpers& jit, const StackmapGenerationParams& params) {
        switch (op) {
        case Ext1OpType::I32TruncSatF32S:
            jit.truncateFloatToInt32(params[1].fpr(), params[0].gpr());
            break;
        case Ext1OpType::I32TruncSatF32U:
            jit.truncateFloatToUint32(params[1].fpr(), params[0].gpr());
            break;
        case Ext1OpType::I32TruncSatF64S:
            jit.truncateDoubleToInt32(params[1].fpr(), params[0].gpr());
            break;
        case Ext1OpType::I32TruncSatF64U:
            jit.truncateDoubleToUint32(params[1].fpr(), params[0].gpr());
            break;
        case Ext1OpType::I64TruncSatF32S:
            jit.truncateFloatToInt64(params[1].fpr(), params[0].gpr());
            break;
        case Ext1OpType::I64TruncSatF32U: {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            ASSERT(requiresMacroScratchRegisters);
            FPRReg scratch = InvalidFPRReg;
            FPRReg constant = InvalidFPRReg;
            if (isX86()) {
                scratch = params.fpScratch(0);
                constant = params[2].fpr();
            }
            jit.truncateFloatToUint64(params[1].fpr(), params[0].gpr(), scratch, constant);
            break;
        }
        case Ext1OpType::I64TruncSatF64S:
            jit.truncateDoubleToInt64(params[1].fpr(), params[0].gpr());
            break;
        case Ext1OpType::I64TruncSatF64U: {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            ASSERT(requiresMacroScratchRegisters);
            FPRReg scratch = InvalidFPRReg;
            FPRReg constant = InvalidFPRReg;
            if (isX86()) {
                scratch = params.fpScratch(0);
                constant = params[2].fpr();
            }
            jit.truncateDoubleToUint64(params[1].fpr(), params[0].gpr(), scratch, constant);
            break;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }
    });
    patchpoint->effects = Effects::none();

    Value* maxResult = nullptr;
    Value* minResult = nullptr;
    Value* zero = nullptr;
    bool requiresNaNCheck = false;
    switch (op) {
    case Ext1OpType::I32TruncSatF32S:
    case Ext1OpType::I32TruncSatF64S:
        maxResult = constant(Int32, bitwise_cast<uint32_t>(INT32_MAX));
        minResult = constant(Int32, bitwise_cast<uint32_t>(INT32_MIN));
        zero = constant(Int32, 0);
        requiresNaNCheck = true;
        break;
    case Ext1OpType::I32TruncSatF32U:
    case Ext1OpType::I32TruncSatF64U:
        maxResult = constant(Int32, bitwise_cast<uint32_t>(UINT32_MAX));
        minResult = constant(Int32, bitwise_cast<uint32_t>(0U));
        break;
    case Ext1OpType::I64TruncSatF32S:
    case Ext1OpType::I64TruncSatF64S:
        maxResult = constant(Int64, bitwise_cast<uint64_t>(INT64_MAX));
        minResult = constant(Int64, bitwise_cast<uint64_t>(INT64_MIN));
        zero = constant(Int64, 0);
        requiresNaNCheck = true;
        break;
    case Ext1OpType::I64TruncSatF32U:
    case Ext1OpType::I64TruncSatF64U:
        maxResult = constant(Int64, bitwise_cast<uint64_t>(UINT64_MAX));
        minResult = constant(Int64, bitwise_cast<uint64_t>(0ULL));
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }

    result = push(append<Value>(m_proc, B3::Select, origin(),
        append<Value>(m_proc, GreaterThan, origin(), arg, minFloat),
        append<Value>(m_proc, B3::Select, origin(),
            append<Value>(m_proc, LessThan, origin(), arg, maxFloat),
            patchpoint, maxResult),
        requiresNaNCheck ? append<Value>(m_proc, B3::Select, origin(), append<Value>(m_proc, Equal, origin(), arg, arg), minResult, zero) : minResult));

    return { };
}

auto OMGIRGenerator::addRefI31(ExpressionType value, ExpressionType& result) -> PartialResult
{
    Value* masked = append<Value>(m_proc, B3::BitAnd, origin(), get(value), constant(Int32, 0x7fffffff));
    Value* shiftLeft = append<Value>(m_proc, B3::Shl, origin(), masked, constant(Int32, 0x1));
    Value* shiftRight = append<Value>(m_proc, B3::SShr, origin(), shiftLeft, constant(Int32, 0x1));
    Value* extended = append<Value>(m_proc, B3::ZExt32, origin(), shiftRight);
    result = push<Value>(m_proc, B3::BitOr, origin(), extended, constant(Int64, JSValue::NumberTag));

    return { };
}

auto OMGIRGenerator::addI31GetS(ExpressionType ref, ExpressionType& result) -> PartialResult
{
    // Trap on null reference.
    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), get(ref), append<Const64Value>(m_proc, origin(), JSValue::encode(jsNull()))));
        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::NullI31Get);
        });
    }

    result = push<Value>(m_proc, B3::Trunc, origin(), get(ref));

    return { };
}

auto OMGIRGenerator::addI31GetU(ExpressionType ref, ExpressionType& result) -> PartialResult
{
    // Trap on null reference.
    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), get(ref), append<Const64Value>(m_proc, origin(), JSValue::encode(jsNull()))));
        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::NullI31Get);
        });
    }

    Value* masked = append<Value>(m_proc, B3::BitAnd, origin(), get(ref), constant(Int64, 0x7fffffff));
    result = push<Value>(m_proc, B3::Trunc, origin(), masked);
    return { };
}

Variable* OMGIRGenerator::pushArrayNew(uint32_t typeIndex, Value* initValue, ExpressionType size)
{
    StorageType elementType;
    getArrayElementType(typeIndex, elementType);

    // FIXME: Emit this inline.
    // https://bugs.webkit.org/show_bug.cgi?id=245405
    Value* resultValue;
    if (!elementType.unpacked().isV128()) {
        resultValue = callWasmOperation(m_currentBlock, toB3Type(Types::Arrayref), operationWasmArrayNew,
            instanceValue(), append<Const32Value>(m_proc, origin(), typeIndex),
            get(size), initValue);
    } else {
        Value* lane0 = append<SIMDValue>(m_proc, origin(), B3::VectorExtractLane, B3::Int64, SIMDLane::i64x2, SIMDSignMode::None, uint8_t { 0 }, initValue);
        Value* lane1 = append<SIMDValue>(m_proc, origin(), B3::VectorExtractLane, B3::Int64, SIMDLane::i64x2, SIMDSignMode::None, uint8_t { 1 }, initValue);
        resultValue = callWasmOperation(m_currentBlock, toB3Type(Types::Arrayref), operationWasmArrayNewVector,
            instanceValue(), append<Const32Value>(m_proc, origin(), typeIndex),
            get(size), lane0, lane1);
    }

    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), resultValue, append<Const64Value>(m_proc, origin(), JSValue::encode(jsNull()))));

        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::BadArrayNew);
        });
    }

    return push(resultValue);
}

// Given a type index, verify that it's an array type and return its expansion
const ArrayType* OMGIRGenerator::getArrayTypeDefinition(uint32_t typeIndex)
{
    Ref<Wasm::TypeDefinition> typeDef = getTypeDefinition(typeIndex);
    const Wasm::TypeDefinition& arraySignature = typeDef->expand();
    ASSERT(arraySignature.is<ArrayType>());
    return arraySignature.as<ArrayType>();
}

// Given a type index for an array signature, look it up, expand it and
// return the element type
void OMGIRGenerator::getArrayElementType(uint32_t typeIndex, StorageType& result)
{
    const ArrayType* arrayType = getArrayTypeDefinition(typeIndex);
    result = arrayType->elementType().type;
}

// Given a type index, verify that it's an array type and return the type (Ref a)
void OMGIRGenerator::getArrayRefType(uint32_t typeIndex, Type& result)
{
    Ref<Wasm::TypeDefinition> typeDef = getTypeDefinition(typeIndex);
    result = Type { TypeKind::Ref, typeDef->index() };
}

auto OMGIRGenerator::addArrayNew(uint32_t typeIndex, ExpressionType size, ExpressionType value, ExpressionType& result) -> PartialResult
{
#if ASSERT_ENABLED
    StorageType elementType;
    getArrayElementType(typeIndex, elementType);
    ASSERT(toB3Type(elementType.unpacked()) == value->type());
#endif

    Value* initValue = get(value);
    if (value->type() == B3::Float || value->type() == B3::Double) {
        initValue = append<Value>(m_proc, BitwiseCast, origin(), initValue);
        if (initValue->type() == B3::Int32)
            initValue = append<Value>(m_proc, ZExt32, origin(), initValue);
    }

    result = pushArrayNew(typeIndex, initValue, size);

    emitArrayNullCheck(get(result), ExceptionType::BadArrayNew);

    return { };
}

Variable* OMGIRGenerator::pushArrayNewFromSegment(ArraySegmentOperation operation, uint32_t typeIndex, uint32_t segmentIndex, ExpressionType arraySize, ExpressionType offset, ExceptionType exceptionType)
{
    Value* resultValue = callWasmOperation(m_currentBlock, toB3Type(Types::Arrayref), operation,
        instanceValue(), append<Const32Value>(m_proc, origin(), typeIndex),
        append<Const32Value>(m_proc, origin(), segmentIndex),
        get(arraySize), get(offset));

    // Indicates out of bounds for the segment or allocation failure.
    emitArrayNullCheck(resultValue, exceptionType);

    return push(resultValue);
}

auto OMGIRGenerator::addArrayNewDefault(uint32_t typeIndex, ExpressionType size, ExpressionType& result) -> PartialResult
{
    Type resultType;
    getArrayRefType(typeIndex, resultType);

    result = push(callWasmOperation(m_currentBlock, toB3Type(resultType), operationWasmArrayNewEmpty,
        instanceValue(), append<Const32Value>(m_proc, origin(), typeIndex), get(size)));

    emitArrayNullCheck(get(result), ExceptionType::BadArrayNew);

    return { };
}

auto OMGIRGenerator::addArrayNewData(uint32_t typeIndex, uint32_t dataIndex, ExpressionType arraySize, ExpressionType offset, ExpressionType& result) -> PartialResult
{
    result = pushArrayNewFromSegment(operationWasmArrayNewData, typeIndex, dataIndex, arraySize, offset, ExceptionType::BadArrayNewInitData);

    return { };
}

auto OMGIRGenerator::addArrayNewElem(uint32_t typeIndex, uint32_t elemSegmentIndex, ExpressionType arraySize, ExpressionType offset, ExpressionType& result) -> PartialResult
{
    result = pushArrayNewFromSegment(operationWasmArrayNewElem, typeIndex, elemSegmentIndex, arraySize, offset, ExceptionType::BadArrayNewInitElem);
    return { };
}

auto OMGIRGenerator::addArrayNewFixed(uint32_t typeIndex, Vector<ExpressionType>& args, ExpressionType& result) -> PartialResult
{
    // Get the result type for the array.new_fixed operation
    Type resultType;
    getArrayRefType(typeIndex, resultType);

    // Allocate an uninitialized array whose length matches the argument count

    // FIXME: inline the allocation.
    // https://bugs.webkit.org/show_bug.cgi?id=244388
    Value* arrayValue = callWasmOperation(m_currentBlock, toB3Type(resultType), operationWasmArrayNewEmpty,
        instanceValue(), append<Const32Value>(m_proc, origin(), typeIndex),
        append<Const32Value>(m_proc, origin(), args.size()));

    emitArrayNullCheck(arrayValue, ExceptionType::BadArrayNew);

    for (uint32_t i = 0; i < args.size(); ++i) {
        // Emit the array set code -- note that this omits the bounds check, since
        // if operationWasmArrayNewEmpty() returned a non-null value, it's an array of the right size
        emitArraySetUnchecked(typeIndex, arrayValue, append<Const32Value>(m_proc, origin(), i), get(args[i]));
    }
    result = push(arrayValue);

    return { };
}

auto OMGIRGenerator::addArrayGet(ExtGCOpType arrayGetKind, uint32_t typeIndex, ExpressionType arrayref, ExpressionType index, ExpressionType& result) -> PartialResult
{
    StorageType elementType;
    getArrayElementType(typeIndex, elementType);
    Wasm::Type resultType = elementType.unpacked();

    // Ensure arrayref is non-null.
    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), get(arrayref), append<Const64Value>(m_proc, origin(), JSValue::encode(jsNull()))));
        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::NullArrayGet);
        });
    }

    // Check array bounds.
    Value* arraySize = append<MemoryValue>(heapTop(), m_proc, Load, Int32, origin(),
        get(arrayref), safeCast<int32_t>(JSWebAssemblyArray::offsetOfSize()));
    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, AboveEqual, origin(), get(index), arraySize));
        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsArrayGet);
        });
    }

    Value* payloadBase = append<MemoryValue>(heapTop(), m_proc, memoryKind(Load), pointerType(), origin(), get(arrayref), JSWebAssemblyArray::offsetOfPayload());
    Value* indexValue = is32Bit() ? get(index) : append<Value>(m_proc, ZExt32, origin(), get(index));
    Value* indexedAddress = append<Value>(m_proc, Add, pointerType(), origin(), payloadBase,
        append<Value>(m_proc, Add, pointerType(), origin(), constant(pointerType(), JSWebAssemblyArray::offsetOfElements(elementType)),
            append<Value>(m_proc, Mul, pointerType(), origin(), indexValue, constant(pointerType(), elementType.elementSize()))));

    if (elementType.is<PackedType>()) {
        Value* load;
        switch (elementType.as<PackedType>()) {
        case PackedType::I8:
            load = append<MemoryValue>(heapTop(), m_proc, memoryKind(Load8Z), Int32, origin(), indexedAddress);
            break;
        case PackedType::I16:
            load = append<MemoryValue>(heapTop(), m_proc, memoryKind(Load16Z), Int32, origin(), indexedAddress);
            break;
        }
        Value* postProcess = load;
        switch (arrayGetKind) {
        case ExtGCOpType::ArrayGet:
        case ExtGCOpType::ArrayGetU:
            break;
        case ExtGCOpType::ArrayGetS: {
            size_t elementSize = elementType.as<PackedType>() == PackedType::I8 ? sizeof(uint8_t) : sizeof(uint16_t);
            uint8_t bitShift = (sizeof(uint32_t) - elementSize) * 8;
            Value* shiftLeft = append<Value>(m_proc, B3::Shl, origin(), postProcess, append<Const32Value>(m_proc, origin(), bitShift));
            postProcess = append<Value>(m_proc, B3::SShr, origin(), shiftLeft, append<Const32Value>(m_proc, origin(), bitShift));
            break;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return { };
        }
        result = push(postProcess);
        return { };
    }

    ASSERT(elementType.is<Type>());
    result = push<MemoryValue>(heapTop(), m_proc, memoryKind(Load), toB3Type(resultType), origin(), indexedAddress);

    return { };
}

void OMGIRGenerator::emitArrayNullCheck(Value* arrayref, ExceptionType exceptionType)
{
    CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
        append<Value>(m_proc, Equal, origin(), arrayref, append<Const64Value>(m_proc, origin(), JSValue::encode(jsNull()))));
    check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
        this->emitExceptionCheck(jit, exceptionType);
    });
}

// Does the array set without null check and bounds checks -- can be
// called directly by addArrayNewFixed()
void OMGIRGenerator::emitArraySetUnchecked(uint32_t typeIndex, Value* arrayref, Value* index, Value* setValue)
{
    StorageType elementType;
    getArrayElementType(typeIndex, elementType);

    auto payloadBase = append<MemoryValue>(heapTop(), m_proc, memoryKind(Load), pointerType(), origin(), arrayref, JSWebAssemblyArray::offsetOfPayload());
    auto indexValue = is32Bit() ? index : append<Value>(m_proc, ZExt32, origin(), index);
    auto indexedAddress = append<Value>(m_proc, Add, pointerType(), origin(), payloadBase,
        append<Value>(m_proc, Add, pointerType(), origin(), constant(pointerType(), JSWebAssemblyArray::offsetOfElements(elementType)),
            append<Value>(m_proc, Mul, pointerType(), origin(), indexValue, constant(pointerType(), elementType.elementSize()))));

    if (elementType.is<PackedType>()) {
        switch (elementType.as<PackedType>()) {
        case PackedType::I8:
            append<MemoryValue>(heapTop(), m_proc, memoryKind(Store8), origin(), setValue, indexedAddress);
            break;
        case PackedType::I16:
            append<MemoryValue>(heapTop(), m_proc, memoryKind(Store16), origin(), setValue, indexedAddress);
            break;
        }
        return;
    }

    ASSERT(elementType.is<Type>());
    append<MemoryValue>(heapTop(), m_proc, memoryKind(Store), origin(), setValue, indexedAddress);

    if (isRefType(elementType.unpacked()))
        emitWriteBarrier(arrayref, instanceValue());

    return;
}

auto OMGIRGenerator::addArraySet(uint32_t typeIndex, ExpressionType arrayref, ExpressionType index, ExpressionType value) -> PartialResult
{
#if ASSERT_ENABLED
    const ArrayType* arrayType = getArrayTypeDefinition(typeIndex);
    UNUSED_VARIABLE(arrayType);
#endif

    // Check for null array
    emitArrayNullCheck(get(arrayref), ExceptionType::NullArraySet);

    // Check array bounds.
    Value* arraySize = append<MemoryValue>(heapTop(), m_proc, Load, Int32, origin(),
        get(arrayref), safeCast<int32_t>(JSWebAssemblyArray::offsetOfSize()));
    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, AboveEqual, origin(), get(index), arraySize));
        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsArraySet);
        });
    }

    emitArraySetUnchecked(typeIndex, get(arrayref), get(index), get(value));

    return { };
}

auto OMGIRGenerator::addArrayLen(ExpressionType arrayref, ExpressionType& result) -> PartialResult
{
    // Ensure arrayref is non-null.
    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), get(arrayref), append<Const64Value>(m_proc, origin(), JSValue::encode(jsNull()))));
        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::NullArrayLen);
        });
    }

    result = push<MemoryValue>(heapTop(), m_proc, Load, Int32, origin(), get(arrayref), safeCast<int32_t>(JSWebAssemblyArray::offsetOfSize()));

    return { };
}

auto OMGIRGenerator::addArrayFill(uint32_t typeIndex, ExpressionType arrayref, ExpressionType offset, ExpressionType value, ExpressionType size) -> PartialResult
{
    StorageType elementType;
    getArrayElementType(typeIndex, elementType);

    emitArrayNullCheck(get(arrayref), ExceptionType::NullArrayFill);

    Value* resultValue;
    if (!elementType.unpacked().isV128()) {
        Value* valueGPR = get(value);
        if (value->type().isFloat())
            valueGPR = append<Value>(m_proc, BitwiseCast, origin(), valueGPR);
        resultValue = callWasmOperation(m_currentBlock, toB3Type(Types::I32), operationWasmArrayFill,
            instanceValue(), get(arrayref), get(offset), valueGPR, get(size));
    } else {
        Value* lane0 = append<SIMDValue>(m_proc, origin(), B3::VectorExtractLane, B3::Int64, SIMDLane::i64x2, SIMDSignMode::None, uint8_t { 0 }, get(value));
        Value* lane1 = append<SIMDValue>(m_proc, origin(), B3::VectorExtractLane, B3::Int64, SIMDLane::i64x2, SIMDSignMode::None, uint8_t { 1 }, get(value));
        resultValue = callWasmOperation(m_currentBlock, toB3Type(Types::I32), operationWasmArrayFillVector,
            instanceValue(), get(arrayref), get(offset), lane0, lane1, get(size));
    }

    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), resultValue, append<Const32Value>(m_proc, origin(), 0)));

        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsArrayFill);
        });
    }

    return { };
}

auto OMGIRGenerator::addArrayCopy(uint32_t, ExpressionType dst, ExpressionType dstOffset, uint32_t, ExpressionType src, ExpressionType srcOffset, ExpressionType size) -> PartialResult
{
    emitArrayNullCheck(get(dst), ExceptionType::NullArrayCopy);
    emitArrayNullCheck(get(src), ExceptionType::NullArrayCopy);

    Value* resultValue = callWasmOperation(m_currentBlock, toB3Type(Types::I32), operationWasmArrayCopy,
        instanceValue(),
        get(dst), get(dstOffset),
        get(src), get(srcOffset),
        get(size));

    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), resultValue, append<Const32Value>(m_proc, origin(), 0)));

        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsArrayCopy);
        });
    }

    return { };
}

auto OMGIRGenerator::addArrayInitElem(uint32_t, ExpressionType dst, ExpressionType dstOffset, uint32_t srcElementIndex, ExpressionType srcOffset, ExpressionType size) -> PartialResult
{
    emitArrayNullCheck(get(dst), ExceptionType::NullArrayInitElem);

    Value* resultValue = callWasmOperation(m_currentBlock, toB3Type(Types::I32), operationWasmArrayInitElem,
        instanceValue(),
        get(dst), get(dstOffset),
        append<Const32Value>(m_proc, origin(), srcElementIndex), get(srcOffset),
        get(size));

    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), resultValue, append<Const32Value>(m_proc, origin(), 0)));

        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsArrayInitElem);
        });
    }

    return { };
}

auto OMGIRGenerator::addArrayInitData(uint32_t, ExpressionType dst, ExpressionType dstOffset, uint32_t srcDataIndex, ExpressionType srcOffset, ExpressionType size) -> PartialResult
{
    emitArrayNullCheck(get(dst), ExceptionType::NullArrayInitData);

    Value* resultValue = callWasmOperation(m_currentBlock, toB3Type(Types::I32), operationWasmArrayInitData,
        instanceValue(),
        get(dst), get(dstOffset),
        append<Const32Value>(m_proc, origin(), srcDataIndex), get(srcOffset),
        get(size));

    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), resultValue, append<Const32Value>(m_proc, origin(), 0)));

        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsArrayInitData);
        });
    }

    return { };
}

auto OMGIRGenerator::addStructNew(uint32_t typeIndex, Vector<ExpressionType>& args, ExpressionType& result) -> PartialResult
{
    const auto type = Type { TypeKind::Ref, m_info.typeSignatures[typeIndex]->index() };

    // FIXME: inline the allocation.
    // https://bugs.webkit.org/show_bug.cgi?id=244388
    Value* structValue = callWasmOperation(m_currentBlock, toB3Type(type), operationWasmStructNewEmpty,
        instanceValue(),
        append<Const32Value>(m_proc, origin(), typeIndex));

    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), structValue, append<Const64Value>(m_proc, origin(), JSValue::encode(jsNull()))));
        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::BadStructNew);
        });
    }

    const auto& structType = *m_info.typeSignatures[typeIndex]->expand().template as<StructType>();
    for (uint32_t i = 0; i < args.size(); ++i)
        emitStructSet(structValue, i, structType, get(args[i]));

    result = push(structValue);

    return { };
}

auto OMGIRGenerator::addStructNewDefault(uint32_t typeIndex, ExpressionType& result) -> PartialResult
{
    const auto type = Type { TypeKind::Ref, m_info.typeSignatures[typeIndex]->index() };

    // FIXME: inline the allocation.
    // https://bugs.webkit.org/show_bug.cgi?id=244388
    Value* structValue = callWasmOperation(m_currentBlock, toB3Type(type), operationWasmStructNewEmpty,
        instanceValue(),
        append<Const32Value>(m_proc, origin(), typeIndex));

    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), structValue, append<Const64Value>(m_proc, origin(), JSValue::encode(jsNull()))));
        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::BadStructNew);
        });
    }

    const auto& structType = *m_info.typeSignatures[typeIndex]->expand().template as<StructType>();
    for (StructFieldCount i = 0; i < structType.fieldCount(); ++i) {
        Value* initValue;
        if (Wasm::isRefType(structType.field(i).type))
            initValue = append<Const64Value>(m_proc, origin(), JSValue::encode(jsNull()));
        else
            initValue = append<Const64Value>(m_proc, origin(), 0);
        emitStructSet(structValue, i, structType, initValue);
    }

    result = push(structValue);

    return { };
}

auto OMGIRGenerator::addStructGet(ExtGCOpType structGetKind, ExpressionType structReference, const StructType& structType, uint32_t fieldIndex, ExpressionType& result) -> PartialResult
{
    auto fieldType = structType.field(fieldIndex).type;
    auto resultType = fieldType.unpacked();

    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), get(structReference), append<Const64Value>(m_proc, origin(), JSValue::encode(jsNull()))));
        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::NullStructGet);
        });
    }

    Value* payloadBase = append<MemoryValue>(heapTop(), m_proc, memoryKind(Load), pointerType(), origin(), get(structReference), JSWebAssemblyStruct::offsetOfPayload());
    int32_t fieldOffset = fixupPointerPlusOffset(payloadBase, *structType.offsetOfField(fieldIndex));

    if (fieldType.is<PackedType>()) {
        Value* load;
        switch (fieldType.as<PackedType>()) {
        case PackedType::I8:
            load = append<MemoryValue>(heapTop(), m_proc, memoryKind(Load8Z), Int32, origin(), payloadBase, fieldOffset);
            break;
        case PackedType::I16:
            load = append<MemoryValue>(heapTop(), m_proc, memoryKind(Load16Z), Int32, origin(), payloadBase, fieldOffset);
            break;
        }
        Value* postProcess = load;
        switch (structGetKind) {
        case ExtGCOpType::StructGetU:
            break;
        case ExtGCOpType::StructGetS: {
            uint8_t bitShift = (sizeof(uint32_t) - fieldType.elementSize()) * 8;
            Value* shiftLeft = append<Value>(m_proc, B3::Shl, origin(), postProcess, append<Const32Value>(m_proc, origin(), bitShift));
            postProcess = append<Value>(m_proc, B3::SShr, origin(), shiftLeft, append<Const32Value>(m_proc, origin(), bitShift));
            break;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return { };
        }
        result = push(postProcess);
        return { };
    }

    ASSERT(fieldType.is<Type>());
    result = push<MemoryValue>(heapTop(), m_proc, memoryKind(Load), toB3Type(resultType), origin(), payloadBase, fieldOffset);

    return { };
}

auto OMGIRGenerator::addStructSet(ExpressionType structReference, const StructType& structType, uint32_t fieldIndex, ExpressionType value) -> PartialResult
{
    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), get(structReference), append<Const64Value>(m_proc, origin(), JSValue::encode(jsNull()))));
        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::NullStructSet);
        });
    }

    emitStructSet(get(structReference), fieldIndex, structType, get(value));
    return { };
}

auto OMGIRGenerator::addRefTest(ExpressionType reference, bool allowNull, int32_t heapType, bool shouldNegate, ExpressionType& result) -> PartialResult
{
    emitRefTestOrCast(CastKind::Test, reference, allowNull, heapType, shouldNegate, result);
    return { };
}

auto OMGIRGenerator::addRefCast(ExpressionType reference, bool allowNull, int32_t heapType, ExpressionType& result) -> PartialResult
{
    emitRefTestOrCast(CastKind::Cast, reference, allowNull, heapType, false, result);
    return { };
}

void OMGIRGenerator::emitRefTestOrCast(CastKind castKind, ExpressionType reference, bool allowNull, int32_t heapType, bool shouldNegate, ExpressionType& result)
{
    if (castKind == CastKind::Cast)
        result = push(get(reference));

    BasicBlock* continuation = m_proc.addBlock();
    BasicBlock* trueBlock = nullptr;
    BasicBlock* falseBlock = nullptr;
    if (castKind == CastKind::Test) {
        trueBlock = m_proc.addBlock();
        falseBlock = m_proc.addBlock();
    }

    auto castFailure = [this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
        this->emitExceptionCheck(jit, ExceptionType::CastFailure);
    };

    // Ensure reference nullness agrees with heap type.
    {
        BasicBlock* nullCase = m_proc.addBlock();
        BasicBlock* nonNullCase = m_proc.addBlock();

        Value* isNull = append<Value>(m_proc, Equal, origin(),
            get(reference), append<Const64Value>(m_proc, origin(), JSValue::encode(jsNull())));
        m_currentBlock->appendNewControlValue(m_proc, B3::Branch, origin(), isNull,
            FrequentedBlock(nullCase), FrequentedBlock(nonNullCase));
        nullCase->addPredecessor(m_currentBlock);
        nonNullCase->addPredecessor(m_currentBlock);

        m_currentBlock = nullCase;
        if (castKind == CastKind::Cast) {
            if (!allowNull) {
                B3::PatchpointValue* throwException = append<B3::PatchpointValue>(m_proc, B3::Void, origin());
                throwException->setGenerator(castFailure);
            }
            m_currentBlock->appendNewControlValue(m_proc, Jump, origin(), continuation);
            continuation->addPredecessor(m_currentBlock);
        } else {
            BasicBlock* nextBlock;
            if (!allowNull)
                nextBlock = falseBlock;
            else
                nextBlock = trueBlock;
            m_currentBlock->appendNewControlValue(m_proc, Jump, origin(), nextBlock);
            nextBlock->addPredecessor(m_currentBlock);
        }

        m_currentBlock = nonNullCase;
    }

    if (typeIndexIsType(static_cast<Wasm::TypeIndex>(heapType))) {
        switch (static_cast<TypeKind>(heapType)) {
        case Wasm::TypeKind::Funcref:
        case Wasm::TypeKind::Externref:
        case Wasm::TypeKind::Anyref:
            // Casts to these types cannot fail as they are the top types of their respective hierarchies, and static type-checking does not allow cross-hierarchy casts.
            break;
        case Wasm::TypeKind::Nullref:
        case Wasm::TypeKind::Nullfuncref:
        case Wasm::TypeKind::Nullexternref:
            // Casts to any bottom type should always fail.
            if (castKind == CastKind::Cast) {
                B3::PatchpointValue* throwException = append<B3::PatchpointValue>(m_proc, B3::Void, origin());
                throwException->setGenerator(castFailure);
            } else {
                m_currentBlock->appendNewControlValue(m_proc, Jump, origin(), falseBlock);
                falseBlock->addPredecessor(m_currentBlock);
                m_currentBlock = m_proc.addBlock();
            }
            break;
        case Wasm::TypeKind::Eqref: {
            auto nop = [] (CCallHelpers&, const B3::StackmapGenerationParams&) { };
            BasicBlock* endBlock = castKind == CastKind::Cast ? continuation : trueBlock;
            BasicBlock* checkObject = m_proc.addBlock();

            // The eqref case chains together checks for i31, array, and struct with disjunctions so the control flow is more complicated, and requires some extra basic blocks to be created.
            emitCheckOrBranchForCast(CastKind::Test, append<Value>(m_proc, Below, origin(), get(reference), constant(Int64, JSValue::NumberTag)), nop, checkObject);
            Value* untagged = append<Value>(m_proc, Trunc, origin(), get(reference));
            emitCheckOrBranchForCast(CastKind::Test, append<Value>(m_proc, GreaterThan, origin(), untagged, constant(Int32, Wasm::maxI31ref)), nop, checkObject);
            emitCheckOrBranchForCast(CastKind::Test, append<Value>(m_proc, LessThan, origin(), untagged, constant(Int32, Wasm::minI31ref)), nop, checkObject);
            m_currentBlock->appendNewControlValue(m_proc, Jump, origin(), endBlock);
            checkObject->addPredecessor(m_currentBlock);
            endBlock->addPredecessor(m_currentBlock);

            m_currentBlock = checkObject;
            emitCheckOrBranchForCast(castKind, append<Value>(m_proc, BitAnd, origin(), get(reference), constant(Int64, JSValue::NotCellMask)), castFailure, falseBlock);
            Value* jsType = append<MemoryValue>(heapTop(), m_proc, Load8Z, Int32, origin(), get(reference), safeCast<int32_t>(JSCell::typeInfoTypeOffset()));
            emitCheckOrBranchForCast(castKind, append<Value>(m_proc, NotEqual, origin(), jsType, constant(Int32, JSType::WebAssemblyGCObjectType)), castFailure, falseBlock);
            break;
        }
        case Wasm::TypeKind::I31ref: {
            emitCheckOrBranchForCast(castKind, append<Value>(m_proc, Below, origin(), get(reference), constant(Int64, JSValue::NumberTag)), castFailure, falseBlock);
            Value* untagged = append<Value>(m_proc, Trunc, origin(), get(reference));
            emitCheckOrBranchForCast(castKind, append<Value>(m_proc, GreaterThan, origin(), untagged, constant(Int32, Wasm::maxI31ref)), castFailure, falseBlock);
            emitCheckOrBranchForCast(castKind, append<Value>(m_proc, LessThan, origin(), untagged, constant(Int32, Wasm::minI31ref)), castFailure, falseBlock);
            break;
        }
        case Wasm::TypeKind::Arrayref:
        case Wasm::TypeKind::Structref: {
            emitCheckOrBranchForCast(castKind, append<Value>(m_proc, BitAnd, origin(), get(reference), constant(Int64, JSValue::NotCellMask)), castFailure, falseBlock);
            Value* jsType = append<MemoryValue>(heapTop(), m_proc, Load8Z, Int32, origin(), get(reference), safeCast<int32_t>(JSCell::typeInfoTypeOffset()));
            emitCheckOrBranchForCast(castKind, append<Value>(m_proc, NotEqual, origin(), jsType, constant(Int32, JSType::WebAssemblyGCObjectType)), castFailure, falseBlock);
            Value* rtt = emitLoadRTTFromObject(get(reference));
            emitCheckOrBranchForCast(castKind, emitNotRTTKind(rtt, static_cast<TypeKind>(heapType) == Wasm::TypeKind::Arrayref ? RTTKind::Array : RTTKind::Struct), castFailure, falseBlock);
            break;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    } else {
        Wasm::TypeDefinition& signature = m_info.typeSignatures[heapType];
        BasicBlock* slowPath = m_proc.addBlock();

        Value* rtt;
        if (signature.expand().is<Wasm::FunctionSignature>())
            rtt = emitLoadRTTFromFuncref(get(reference));
        else {
            // The cell check is only needed for non-functions, as the typechecker does not allow non-Cell values for funcref casts.
            emitCheckOrBranchForCast(castKind, append<Value>(m_proc, BitAnd, origin(), get(reference), constant(Int64, JSValue::NotCellMask)), castFailure, falseBlock);
            Value* jsType = append<MemoryValue>(heapTop(), m_proc, Load8Z, Int32, origin(), get(reference), safeCast<int32_t>(JSCell::typeInfoTypeOffset()));
            emitCheckOrBranchForCast(castKind, append<Value>(m_proc, NotEqual, origin(), jsType, constant(Int32, JSType::WebAssemblyGCObjectType)), castFailure, falseBlock);
            rtt = emitLoadRTTFromObject(get(reference));
            emitCheckOrBranchForCast(castKind, emitNotRTTKind(rtt, signature.expand().is<Wasm::ArrayType>() ? RTTKind::Array : RTTKind::Struct), castFailure, falseBlock);
        }

        Value* targetRTT = append<ConstPtrValue>(m_proc, origin(), m_info.rtts[heapType].get());
        Value* rttsAreEqual = append<Value>(m_proc, Equal, origin(),
            rtt, targetRTT);
        BasicBlock* equalBlock;
        if (castKind == CastKind::Cast)
            equalBlock = continuation;
        else
            equalBlock = trueBlock;
        m_currentBlock->appendNewControlValue(m_proc, B3::Branch, origin(), rttsAreEqual,
            FrequentedBlock(equalBlock), FrequentedBlock(slowPath));
        equalBlock->addPredecessor(m_currentBlock);
        slowPath->addPredecessor(m_currentBlock);

        m_currentBlock = slowPath;
        // FIXME: It may be worthwhile to JIT inline this in the future.
        Value* isSubRTT = append<CCallValue>(m_proc, B3::Int32, origin(),
            append<ConstPtrValue>(m_proc, origin(), tagCFunction<OperationPtrTag>(operationWasmIsSubRTT)),
            rtt, targetRTT);
        emitCheckOrBranchForCast(castKind, append<Value>(m_proc, Equal, origin(), isSubRTT, constant(Int32, 0)), castFailure, falseBlock);
    }

    if (castKind == CastKind::Cast) {
        m_currentBlock->appendNewControlValue(m_proc, Jump, origin(), continuation);
        continuation->addPredecessor(m_currentBlock);
        m_currentBlock = continuation;
    } else {
        m_currentBlock->appendNewControlValue(m_proc, Jump, origin(), trueBlock);
        trueBlock->addPredecessor(m_currentBlock);
        m_currentBlock = trueBlock;
        UpsilonValue* trueUpsilon = append<UpsilonValue>(m_proc, origin(), constant(B3::Int32, shouldNegate ? 0 : 1));
        m_currentBlock->appendNewControlValue(m_proc, Jump, origin(), continuation);
        continuation->addPredecessor(m_currentBlock);

        m_currentBlock = falseBlock;
        UpsilonValue* falseUpsilon = append<UpsilonValue>(m_proc, origin(), constant(B3::Int32, shouldNegate ? 1 : 0));
        m_currentBlock->appendNewControlValue(m_proc, Jump, origin(), continuation);
        continuation->addPredecessor(m_currentBlock);

        m_currentBlock = continuation;
        Value* phi = append<Value>(m_proc, Phi, B3::Int32, origin());
        trueUpsilon->setPhi(phi);
        falseUpsilon->setPhi(phi);
        result = push(phi);
    }
}

template <typename Generator>
void OMGIRGenerator::emitCheckOrBranchForCast(CastKind kind, Value* condition, const Generator& generator, BasicBlock* falseBlock)
{
    if (kind == CastKind::Cast) {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(), condition);
        check->setGenerator(generator);
    } else {
        ASSERT(falseBlock);
        BasicBlock* success = m_proc.addBlock();
        m_currentBlock->appendNewControlValue(m_proc, B3::Branch, origin(), condition,
            FrequentedBlock(falseBlock), FrequentedBlock(success));
        falseBlock->addPredecessor(m_currentBlock);
        success->addPredecessor(m_currentBlock);
        m_currentBlock = success;
    }
}

Value* OMGIRGenerator::emitLoadRTTFromFuncref(Value* funcref)
{
    PatchpointValue* patch = append<PatchpointValue>(m_proc, B3::Int64, Origin());
    patch->append(funcref, ValueRep::SomeRegister);
    patch->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        jit.loadCompactPtr(CCallHelpers::Address(params[1].gpr(), WebAssemblyFunctionBase::offsetOfRTT()), params[0].gpr());
    });
    return patch;
}

Value* OMGIRGenerator::emitLoadRTTFromObject(Value* reference)
{
    return append<MemoryValue>(heapTop(), m_proc, B3::Load, toB3Type(Types::Ref), origin(), reference, safeCast<int32_t>(WebAssemblyGCObjectBase::offsetOfRTT()));
}

Value* OMGIRGenerator::emitNotRTTKind(Value* rtt, RTTKind targetKind)
{
    Value* kind = append<MemoryValue>(heapTop(), m_proc, memoryKind(Load8Z), origin(), rtt, safeCast<int32_t>(RTT::offsetOfKind()));
    return append<Value>(m_proc, NotEqual, origin(), kind, constant(Int32, static_cast<uint8_t>(targetKind)));
}

auto OMGIRGenerator::addAnyConvertExtern(ExpressionType reference, ExpressionType& result) -> PartialResult
{
    result = push(callWasmOperation(m_currentBlock, toB3Type(anyrefType()), operationWasmAnyConvertExtern, get(reference)));
    return { };
}

auto OMGIRGenerator::addExternConvertAny(ExpressionType reference, ExpressionType& result) -> PartialResult
{
    result = push(get(reference));
    return { };
}

auto OMGIRGenerator::addSelect(ExpressionType condition, ExpressionType nonZero, ExpressionType zero, ExpressionType& result) -> PartialResult
{
    result = push<Value>(m_proc, B3::Select, origin(), get(condition), get(nonZero), get(zero));
    return { };
}

OMGIRGenerator::ExpressionType OMGIRGenerator::addConstant(Type type, uint64_t value)
{
    return push(constant(toB3Type(type), value));
}

auto OMGIRGenerator::addSIMDSplat(SIMDLane lane, ExpressionType scalar, ExpressionType& result) -> PartialResult
{
    Value* toSplat = get(scalar);
    result = push<SIMDValue>(m_proc, origin(), B3::VectorSplat, B3::V128, lane, SIMDSignMode::None, toSplat);
    return { };
}

auto OMGIRGenerator::addSIMDShift(SIMDLaneOperation op, SIMDInfo info, ExpressionType v, ExpressionType shift, ExpressionType& result) -> PartialResult
{
    result = push(append<SIMDValue>(m_proc, origin(),
        op == SIMDLaneOperation::Shr ? B3::VectorShr : B3::VectorShl, B3::V128, info, get(v), get(shift)));
    return { };
}

auto OMGIRGenerator::addSIMDExtmul(SIMDLaneOperation op, SIMDInfo info, ExpressionType lhs, ExpressionType rhs, ExpressionType& result) -> PartialResult
{
    ASSERT(info.signMode != SIMDSignMode::None);

    auto extOp = op == SIMDLaneOperation::ExtmulLow ? VectorExtendLow : VectorExtendHigh;
    Value* extLhs = append<SIMDValue>(m_proc, origin(), extOp, B3::V128, info, get(lhs));
    Value* extRhs = append<SIMDValue>(m_proc, origin(), extOp, B3::V128, info, get(rhs));
    result = push<SIMDValue>(m_proc, origin(), VectorMul, B3::V128, info, extLhs, extRhs);

    return { };
}

auto OMGIRGenerator::addSIMDShuffle(v128_t imm, ExpressionType a, ExpressionType b, ExpressionType& result) -> PartialResult
{
    if constexpr (isX86()) {
        v128_t leftImm = imm;
        v128_t rightImm = imm;
        for (unsigned i = 0; i < 16; ++i) {
            if (leftImm.u8x16[i] > 15)
                leftImm.u8x16[i] = 0xFF; // Force OOB
            if (rightImm.u8x16[i] < 16 || rightImm.u8x16[i] > 31)
                rightImm.u8x16[i] = 0xFF; // Force OOB
        }
        // Store each byte (w/ index < 16) of `a` to result
        // and zero clear each byte (w/ index > 15) in result.
        Value* leftImmConst = append<Const128Value>(m_proc, origin(), leftImm);
        Value* leftResult = append<SIMDValue>(m_proc, origin(),
            VectorSwizzle, B3::V128, SIMDLane::i8x16, SIMDSignMode::None, get(a), leftImmConst);

        // Store each byte (w/ index - 16 >= 0) of `b` to result2
        // and zero clear each byte (w/ index - 16 < 0) in result2.
        Value* rightImmConst = append<Const128Value>(m_proc, origin(), rightImm);
        Value* rightResult = append<SIMDValue>(m_proc, origin(),
            VectorSwizzle, B3::V128, SIMDLane::i8x16, SIMDSignMode::None, get(b), rightImmConst);

        result = push(append<SIMDValue>(m_proc, origin(),
            VectorOr, B3::V128, SIMDLane::v128, SIMDSignMode::None, leftResult, rightResult));

        return { };
    }

    if constexpr (!isARM64())
        UNREACHABLE_FOR_PLATFORM();

    Value* indexes = append<Const128Value>(m_proc, origin(), imm);
    result = push<SIMDValue>(m_proc, origin(),
        VectorSwizzle, B3::V128, SIMDLane::i8x16, SIMDSignMode::None, get(a), get(b), indexes);

    return { };
}

auto OMGIRGenerator::addSIMDLoad(ExpressionType pointerVariable, uint32_t uoffset, ExpressionType& result) -> PartialResult
{
    Value* ptr = emitCheckAndPreparePointer(get(pointerVariable), uoffset, 16);
    int32_t offset = fixupPointerPlusOffset(ptr, uoffset);
    result = push<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(Load), B3::V128, origin(), ptr, offset);

    return { };
}

auto OMGIRGenerator::addSIMDStore(ExpressionType value, ExpressionType pointerVariable, uint32_t uoffset) -> PartialResult
{
    Value* ptr = emitCheckAndPreparePointer(get(pointerVariable), uoffset, 16);
    int32_t offset = fixupPointerPlusOffset(ptr, uoffset);
    append<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(Store), origin(), get(value), ptr, offset);

    return { };
}

auto OMGIRGenerator::addSIMDLoadSplat(SIMDLaneOperation op, ExpressionType pointerVariable, uint32_t uoffset, ExpressionType& result) -> PartialResult
{
    size_t byteSize;

    B3::Opcode loadOp;
    B3::Type type;
    SIMDLane lane;
    switch (op) {
    case SIMDLaneOperation::LoadSplat8:
        loadOp = Load8Z;
        type = B3::Int32;
        lane = SIMDLane::i8x16;
        byteSize = 1;
        break;
    case SIMDLaneOperation::LoadSplat16:
        loadOp = Load16Z;
        type = B3::Int32;
        lane = SIMDLane::i16x8;
        byteSize = 2;
        break;
    case SIMDLaneOperation::LoadSplat32:
        loadOp = Load;
        type = B3::Int32;
        lane = SIMDLane::i32x4;
        byteSize = 4;
        break;
    case SIMDLaneOperation::LoadSplat64:
        loadOp = Load;
        type = B3::Int64;
        lane = SIMDLane::i64x2;
        byteSize = 8;
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }

    Value* ptr = emitCheckAndPreparePointer(get(pointerVariable), uoffset, byteSize);
    int32_t offset = fixupPointerPlusOffset(ptr, uoffset);
    MemoryValue* memLoad = append<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(loadOp), type, origin(), ptr, offset);
    result = push<SIMDValue>(m_proc, origin(), B3::VectorSplat, B3::V128, lane, SIMDSignMode::None, memLoad);

    return { };
}

auto OMGIRGenerator::addSIMDLoadLane(SIMDLaneOperation op, ExpressionType pointerVariable, ExpressionType vectorVariable, uint32_t uoffset, uint8_t laneIndex, ExpressionType& result) -> PartialResult
{
    size_t byteSize;
    B3::Opcode loadOp;
    B3::Type type;
    SIMDLane lane;
    switch (op) {
    case SIMDLaneOperation::LoadLane8:
        loadOp = Load8Z;
        type = B3::Int32;
        lane = SIMDLane::i8x16;
        byteSize = 1;
        break;
    case SIMDLaneOperation::LoadLane16:
        loadOp = Load16Z;
        type = B3::Int32;
        lane = SIMDLane::i16x8;
        byteSize = 2;
        break;
    case SIMDLaneOperation::LoadLane32:
        loadOp = Load;
        type = B3::Int32;
        lane = SIMDLane::i32x4;
        byteSize = 4;
        break;
    case SIMDLaneOperation::LoadLane64:
        loadOp = Load;
        type = B3::Int64;
        lane = SIMDLane::i64x2;
        byteSize = 8;
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }

    Value* ptr = emitCheckAndPreparePointer(get(pointerVariable), uoffset, byteSize);
    int32_t offset = fixupPointerPlusOffset(ptr, uoffset);
    MemoryValue* memLoad = append<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(loadOp), type, origin(), ptr, offset);
    result = push<SIMDValue>(m_proc, origin(), B3::VectorReplaceLane, B3::V128, lane, SIMDSignMode::None, laneIndex, get(vectorVariable), memLoad);

    return { };
}

auto OMGIRGenerator::addSIMDStoreLane(SIMDLaneOperation op, ExpressionType pointerVariable, ExpressionType vectorVariable, uint32_t uoffset, uint8_t laneIndex) -> PartialResult
{
    size_t byteSize;
    B3::Opcode storeOp;
    B3::Type type;
    SIMDLane lane;
    switch (op) {
    case SIMDLaneOperation::StoreLane8:
        storeOp = Store8;
        type = B3::Int32;
        lane = SIMDLane::i8x16;
        byteSize = 1;
        break;
    case SIMDLaneOperation::StoreLane16:
        storeOp = Store16;
        type = B3::Int32;
        lane = SIMDLane::i16x8;
        byteSize = 2;
        break;
    case SIMDLaneOperation::StoreLane32:
        storeOp = Store;
        type = B3::Int32;
        lane = SIMDLane::i32x4;
        byteSize = 4;
        break;
    case SIMDLaneOperation::StoreLane64:
        storeOp = Store;
        type = B3::Int64;
        lane = SIMDLane::i64x2;
        byteSize = 8;
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }

    Value* ptr = emitCheckAndPreparePointer(get(pointerVariable), uoffset, byteSize);
    int32_t offset = fixupPointerPlusOffset(ptr, uoffset);
    Value* laneValue = append<SIMDValue>(m_proc, origin(), B3::VectorExtractLane, type, lane, byteSize < 4 ? SIMDSignMode::Unsigned : SIMDSignMode::None, laneIndex, get(vectorVariable));
    append<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(storeOp), origin(), laneValue, ptr, offset);

    return { };
}

auto OMGIRGenerator::addSIMDLoadExtend(SIMDLaneOperation op, ExpressionType pointerVariable, uint32_t uoffset, ExpressionType& result) -> PartialResult
{
    B3::Opcode loadOp = Load;
    size_t byteSize = 8;
    SIMDLane lane;
    SIMDSignMode signMode;
    switch (op) {
    case SIMDLaneOperation::LoadExtend8U:
        lane = SIMDLane::i16x8;
        signMode = SIMDSignMode::Unsigned;
        break;
    case SIMDLaneOperation::LoadExtend8S:
        lane = SIMDLane::i16x8;
        signMode = SIMDSignMode::Signed;
        break;
    case SIMDLaneOperation::LoadExtend16U:
        lane = SIMDLane::i32x4;
        signMode = SIMDSignMode::Unsigned;
        break;
    case SIMDLaneOperation::LoadExtend16S:
        lane = SIMDLane::i32x4;
        signMode = SIMDSignMode::Signed;
        break;
    case SIMDLaneOperation::LoadExtend32U:
        lane = SIMDLane::i64x2;
        signMode = SIMDSignMode::Unsigned;
        break;
    case SIMDLaneOperation::LoadExtend32S:
        lane = SIMDLane::i64x2;
        signMode = SIMDSignMode::Signed;
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }

    Value* ptr = emitCheckAndPreparePointer(get(pointerVariable), uoffset, byteSize);
    int32_t offset = fixupPointerPlusOffset(ptr, uoffset);
    MemoryValue* memLoad = append<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(loadOp), B3::Double, origin(), ptr, offset);
    result = push<SIMDValue>(m_proc, origin(), VectorExtendLow, B3::V128, SIMDInfo { lane, signMode }, memLoad);

    return { };
}

auto OMGIRGenerator::addSIMDLoadPad(SIMDLaneOperation op, ExpressionType pointerVariable, uint32_t uoffset, ExpressionType& result) -> PartialResult
{
    B3::Type loadType;
    unsigned byteSize;
    SIMDLane lane;
    uint8_t idx = 0;
    switch (op) {
    case SIMDLaneOperation::LoadPad32:
        loadType = B3::Float;
        byteSize = 4;
        lane = SIMDLane::f32x4;
        break;
    case SIMDLaneOperation::LoadPad64:
        loadType = B3::Double;
        byteSize = 8;
        lane = SIMDLane::f64x2;
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }

    Value* ptr = emitCheckAndPreparePointer(get(pointerVariable), uoffset, byteSize);
    int32_t offset = fixupPointerPlusOffset(ptr, uoffset);
    MemoryValue* memLoad = append<MemoryValue>(heapWasmMemory(), m_proc, memoryKind(Load), loadType, origin(), ptr, offset);
    result = push(append<SIMDValue>(m_proc, origin(), B3::VectorReplaceLane, B3::V128, lane, SIMDSignMode::None, idx,
        append<Const128Value>(m_proc, origin(), v128_t { }),
        memLoad));

    return { };
}

void OMGIRGenerator::emitEntryTierUpCheck()
{
    if (!m_tierUp)
        return;

    ASSERT(m_tierUp);
    Value* countDownLocation = constant(pointerType(), bitwise_cast<uintptr_t>(&m_tierUp->m_counter), Origin());

    PatchpointValue* patch = append<PatchpointValue>(m_proc, B3::Void, Origin());
    Effects effects = Effects::none();
    // FIXME: we should have a more precise heap range for the tier up count.
    effects.reads = B3::HeapRange::top();
    effects.writes = B3::HeapRange::top();
    patch->effects = effects;
    patch->clobber(RegisterSetBuilder::macroClobberedGPRs());

    patch->append(countDownLocation, ValueRep::SomeRegister);
    patch->setGenerator([=, this] (CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        CCallHelpers::Jump tierUp = jit.branchAdd32(CCallHelpers::PositiveOrZero, CCallHelpers::TrustedImm32(TierUpCount::functionEntryIncrement()), CCallHelpers::Address(params[0].gpr()));
        CCallHelpers::Label tierUpResume = jit.label();

        params.addLatePath([=, this] (CCallHelpers& jit) {
            tierUp.link(&jit);

            const unsigned extraPaddingBytes = 0;
            RegisterSet registersToSpill = { };
            registersToSpill.add(GPRInfo::nonPreservedNonArgumentGPR0, IgnoreVectors);
            unsigned numberOfStackBytesUsedForRegisterPreservation = ScratchRegisterAllocator::preserveRegistersToStackForCall(jit, registersToSpill, extraPaddingBytes);

            jit.move(MacroAssembler::TrustedImm32(m_functionIndex), GPRInfo::nonPreservedNonArgumentGPR0);
            jit.nearCallThunk(CodeLocationLabel<JITThunkPtrTag>(Thunks::singleton().stub(triggerOMGEntryTierUpThunkGenerator(m_proc.usesSIMD())).code()));

            ScratchRegisterAllocator::restoreRegistersFromStackForCall(jit, registersToSpill, { }, numberOfStackBytesUsedForRegisterPreservation, extraPaddingBytes);
            jit.jump(tierUpResume);
        });
    });
}

void OMGIRGenerator::emitLoopTierUpCheck(uint32_t loopIndex, const Stack& enclosingStack, const Stack& newStack)
{
    uint32_t outerLoopIndex = this->outerLoopIndex();
    m_outerLoops.append(loopIndex);

    if (!m_tierUp)
        return;
    ASSERT(!m_proc.usesSIMD() || isAnyBBQ(m_compilationMode));

    Origin origin = this->origin();
    ASSERT(m_tierUp->osrEntryTriggers().size() == loopIndex);
    m_tierUp->osrEntryTriggers().append(TierUpCount::TriggerReason::DontTrigger);
    m_tierUp->outerLoops().append(outerLoopIndex);

    Value* countDownLocation = constant(pointerType(), bitwise_cast<uintptr_t>(&m_tierUp->m_counter), origin);

    Vector<Value*> stackmap;
    for (auto& local : m_locals)
        stackmap.append(get(local));

    if (Options::useWasmIPInt()) {
        // Do rethrow slots first because IPInt has them in a shadow stack.
        for (unsigned controlIndex = 0; controlIndex < m_parser->controlStack().size(); ++controlIndex) {
            auto& data = m_parser->controlStack()[controlIndex].controlData;
            if (ControlType::isAnyCatch(data))
                stackmap.append(get(data.exception()));
        }

        for (unsigned controlIndex = 0; controlIndex < m_parser->controlStack().size(); ++controlIndex) {
            auto& expressionStack = m_parser->controlStack()[controlIndex].enclosedExpressionStack;
            for (TypedExpression value : expressionStack)
                stackmap.append(get(value));
        }
        for (TypedExpression value : enclosingStack)
            stackmap.append(get(value));
        for (TypedExpression value : newStack)
            stackmap.append(get(value));
    } else {
        for (unsigned controlIndex = 0; controlIndex < m_parser->controlStack().size(); ++controlIndex) {
            auto& data = m_parser->controlStack()[controlIndex].controlData;
            auto& expressionStack = m_parser->controlStack()[controlIndex].enclosedExpressionStack;
            for (TypedExpression value : expressionStack)
                stackmap.append(get(value));
            if (ControlType::isAnyCatch(data))
                stackmap.append(get(data.exception()));
        }
        for (TypedExpression value : enclosingStack)
            stackmap.append(get(value));
        for (TypedExpression value : newStack)
            stackmap.append(get(value));
    }


    PatchpointValue* patch = append<PatchpointValue>(m_proc, B3::Void, origin);
    Effects effects = Effects::none();
    // FIXME: we should have a more precise heap range for the tier up count.
    effects.reads = B3::HeapRange::top();
    effects.writes = B3::HeapRange::top();
    effects.exitsSideways = true;
    patch->effects = effects;

    patch->clobber(RegisterSetBuilder::macroClobberedGPRs());
    RegisterSet clobberLate;
    clobberLate.add(GPRInfo::nonPreservedNonArgumentGPR0, IgnoreVectors);
    patch->clobberLate(clobberLate);

    patch->append(countDownLocation, ValueRep::SomeRegister);
    patch->appendVectorWithRep(stackmap, ValueRep::ColdAny);

    TierUpCount::TriggerReason* forceEntryTrigger = &(m_tierUp->osrEntryTriggers().last());
    static_assert(!static_cast<uint8_t>(TierUpCount::TriggerReason::DontTrigger), "the JIT code assumes non-zero means 'enter'");
    static_assert(sizeof(TierUpCount::TriggerReason) == 1, "branchTest8 assumes this size");
    SavedFPWidth savedFPWidth = m_proc.usesSIMD() ? SavedFPWidth::SaveVectors : SavedFPWidth::DontSaveVectors;
    patch->setGenerator([=, this] (CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        CCallHelpers::Jump forceOSREntry = jit.branchTest8(CCallHelpers::NonZero, CCallHelpers::AbsoluteAddress(forceEntryTrigger));
        CCallHelpers::Jump tierUp = jit.branchAdd32(CCallHelpers::PositiveOrZero, CCallHelpers::TrustedImm32(TierUpCount::loopIncrement()), CCallHelpers::Address(params[0].gpr()));
        MacroAssembler::Label tierUpResume = jit.label();

        // First argument is the countdown location.
        ASSERT(params.value()->numChildren() >= 1);
        StackMap values(params.value()->numChildren() - 1);
        for (unsigned i = 1; i < params.value()->numChildren(); ++i)
            values[i - 1] = OSREntryValue(params[i], params.value()->child(i)->type());

        OSREntryData& osrEntryData = m_tierUp->addOSREntryData(m_functionIndex, loopIndex, WTFMove(values));
        OSREntryData* osrEntryDataPtr = &osrEntryData;

        params.addLatePath([=] (CCallHelpers& jit) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            forceOSREntry.link(&jit);
            tierUp.link(&jit);

            jit.probe(tagCFunction<JITProbePtrTag>(operationWasmTriggerOSREntryNow), osrEntryDataPtr, savedFPWidth);
            jit.branchTestPtr(CCallHelpers::Zero, GPRInfo::nonPreservedNonArgumentGPR0).linkTo(tierUpResume, &jit);
            jit.farJump(GPRInfo::nonPreservedNonArgumentGPR0, WasmEntryPtrTag);
        });
    });
}

Value* OMGIRGenerator::loadFromScratchBuffer(unsigned& indexInBuffer, Value* pointer, B3::Type type)
{
    unsigned valueSize = m_proc.usesSIMD() ? 2 : 1;
    size_t offset = valueSize * sizeof(uint64_t) * (indexInBuffer++);
    RELEASE_ASSERT(type.isNumeric());
    return append<MemoryValue>(heapTop(), m_proc, Load, type, origin(), pointer, offset);
}

void OMGIRGenerator::connectControlAtEntrypoint(unsigned& indexInBuffer, Value* pointer, ControlData& data, Stack& expressionStack, ControlData& currentData, bool fillLoopPhis)
{
    TRACE_CF("Connect control at entrypoint");
    for (unsigned i = 0; i < expressionStack.size(); i++) {
        TypedExpression value = expressionStack[i];
        auto* load = loadFromScratchBuffer(indexInBuffer, pointer, value->type());
        if (fillLoopPhis)
            append<UpsilonValue>(m_proc, origin(), load, data.phis[i]);
        else
            append<VariableValue>(m_proc, Set, origin(), value.value(), load);
    }
    if (ControlType::isAnyCatch(data) && &data != &currentData) {
        auto* load = loadFromScratchBuffer(indexInBuffer, pointer, pointerType());
        append<VariableValue>(m_proc, Set, origin(), data.exception(), load);
    }
};

auto OMGIRGenerator::addLoop(BlockSignature signature, Stack& enclosingStack, ControlType& block, Stack& newStack, uint32_t loopIndex) -> PartialResult
{
    TRACE_CF("LOOP: entering loop index: ", loopIndex, " signature: ", *signature);
    BasicBlock* body = m_proc.addBlock();
    BasicBlock* continuation = m_proc.addBlock();

    block = ControlData(m_proc, origin(), signature, BlockType::Loop, m_stackSize, continuation, body);

    unsigned offset = enclosingStack.size() - signature->argumentCount();
    for (unsigned i = 0; i < signature->argumentCount(); ++i) {
        TypedExpression value = enclosingStack.at(offset + i);
        Value* phi = block.phis[i];
        append<UpsilonValue>(m_proc, origin(), get(value), phi);
        body->append(phi);
        set(body, value, phi);
        newStack.append(value);
    }
    enclosingStack.shrink(offset);

    m_currentBlock->appendNewControlValue(m_proc, Jump, origin(), body);
    if (loopIndex == m_loopIndexForOSREntry) {
        dataLogLnIf(WasmOMGIRGeneratorInternal::verbose, "Setting up for OSR entry");

        m_currentBlock = m_rootBlocks[0];
        Value* pointer = append<ArgumentRegValue>(m_rootBlocks[0], m_proc, Origin(), GPRInfo::argumentGPR0);

        unsigned indexInBuffer = 0;

        for (auto& local : m_locals)
            append<VariableValue>(m_proc, Set, Origin(), local, loadFromScratchBuffer(indexInBuffer, pointer, local->type()));

        for (unsigned controlIndex = 0; controlIndex < m_parser->controlStack().size(); ++controlIndex) {
            auto& data = m_parser->controlStack()[controlIndex].controlData;
            auto& expressionStack = m_parser->controlStack()[controlIndex].enclosedExpressionStack;
            connectControlAtEntrypoint(indexInBuffer, pointer, data, expressionStack, block);
        }
        connectControlAtEntrypoint(indexInBuffer, pointer, block, enclosingStack, block);
        connectControlAtEntrypoint(indexInBuffer, pointer, block, newStack, block, true);

        ASSERT(!m_proc.usesSIMD() || m_compilationMode == CompilationMode::OMGForOSREntryMode);
        unsigned valueSize = m_proc.usesSIMD() ? 2 : 1;
        *m_osrEntryScratchBufferSize = valueSize * indexInBuffer;
        m_currentBlock->appendNewControlValue(m_proc, Jump, origin(), body);
        body->addPredecessor(m_currentBlock);
    }

    m_currentBlock = body;
    emitLoopTierUpCheck(loopIndex, enclosingStack, newStack);
    return { };
}

OMGIRGenerator::ControlData OMGIRGenerator::addTopLevel(BlockSignature signature)
{
    TRACE_CF("TopLevel: ", *signature);
    return ControlData(m_proc, Origin(), signature, BlockType::TopLevel, m_stackSize, m_proc.addBlock());
}

auto OMGIRGenerator::addBlock(BlockSignature signature, Stack& enclosingStack, ControlType& newBlock, Stack& newStack) -> PartialResult
{
    TRACE_CF("Block: ", *signature);
    BasicBlock* continuation = m_proc.addBlock();

    splitStack(signature, enclosingStack, newStack);
    newBlock = ControlData(m_proc, origin(), signature, BlockType::Block, m_stackSize, continuation);
    return { };
}

auto OMGIRGenerator::addIf(ExpressionType condition, BlockSignature signature, Stack& enclosingStack, ControlType& result, Stack& newStack) -> PartialResult
{
    // FIXME: This needs to do some kind of stack passing.

    BasicBlock* taken = m_proc.addBlock();
    BasicBlock* notTaken = m_proc.addBlock();
    BasicBlock* continuation = m_proc.addBlock();
    FrequencyClass takenFrequency = FrequencyClass::Normal;
    FrequencyClass notTakenFrequency = FrequencyClass::Normal;

    BranchHint hint = m_info.getBranchHint(m_functionIndex, m_parser->currentOpcodeStartingOffset());
    switch (hint) {
    case BranchHint::Unlikely:
        takenFrequency = FrequencyClass::Rare;
        break;
    case BranchHint::Likely:
        notTakenFrequency = FrequencyClass::Rare;
        break;
    case BranchHint::Invalid:
        break;
    }

    append<Value>(m_proc, B3::Branch, origin(), get(condition));
    m_currentBlock->setSuccessors(FrequentedBlock(taken, takenFrequency), FrequentedBlock(notTaken, notTakenFrequency));
    taken->addPredecessor(m_currentBlock);
    notTaken->addPredecessor(m_currentBlock);

    m_currentBlock = taken;
    TRACE_CF("IF");
    splitStack(signature, enclosingStack, newStack);
    result = ControlData(m_proc, origin(), signature, BlockType::If, m_stackSize, continuation, notTaken);
    return { };
}

auto OMGIRGenerator::addElse(ControlData& data, const Stack& currentStack) -> PartialResult
{
    unifyValuesWithBlock(currentStack, data);
    m_currentBlock->appendNewControlValue(m_proc, Jump, origin(), data.continuation);
    return addElseToUnreachable(data);
}

auto OMGIRGenerator::addElseToUnreachable(ControlData& data) -> PartialResult
{
    ASSERT(data.blockType() == BlockType::If);
    m_stackSize = data.stackSize() + data.m_signature->argumentCount();
    m_currentBlock = data.special;
    data.convertIfToBlock();
    TRACE_CF("ELSE");
    return { };
}

auto OMGIRGenerator::addTry(BlockSignature signature, Stack& enclosingStack, ControlType& result, Stack& newStack) -> PartialResult
{
    ++m_tryCatchDepth;
    TRACE_CF("TRY");

    BasicBlock* continuation = m_proc.addBlock();
    splitStack(signature, enclosingStack, newStack);
    result = ControlData(m_proc, origin(), signature, BlockType::Try, m_stackSize, continuation, advanceCallSiteIndex(), m_tryCatchDepth);
    return { };
}

auto OMGIRGenerator::addCatch(unsigned exceptionIndex, const TypeDefinition& signature, Stack& currentStack, ControlType& data, ResultList& results) -> PartialResult
{
    TRACE_CF("CATCH: ", signature);
    unifyValuesWithBlock(currentStack, data);
    m_currentBlock->appendNewControlValue(m_proc, Jump, origin(), data.continuation);
    return addCatchToUnreachable(exceptionIndex, signature, data, results);
}

PatchpointExceptionHandle OMGIRGenerator::preparePatchpointForExceptions(BasicBlock* block, PatchpointValue* patch)
{
    advanceCallSiteIndex();
    bool mustSaveState = m_tryCatchDepth;

    if (!mustSaveState)
        return { m_hasExceptionHandlers, callSiteIndex() };

    unsigned firstStackmapChildOffset = patch->numChildren();
    unsigned firstStackmapParamOffset = firstStackmapChildOffset + m_proc.resultCount(patch->type());
    Vector<Value*> liveValues;
    Origin origin = this->origin();

    Vector<OMGIRGenerator*> frames;
    for (auto* currentFrame = this; currentFrame; currentFrame = currentFrame->m_inlineParent)
        frames.append(currentFrame);
    frames.reverse();

    for (auto* currentFrame : frames) {
        for (Variable* local : currentFrame->m_locals) {
            Value* result = append<VariableValue>(block, m_proc, B3::Get, origin, local);
            liveValues.append(result);
        }
        for (unsigned controlIndex = 0; controlIndex < currentFrame->m_parser->controlStack().size(); ++controlIndex) {
            ControlData& data = currentFrame->m_parser->controlStack()[controlIndex].controlData;
            Stack& expressionStack = currentFrame->m_parser->controlStack()[controlIndex].enclosedExpressionStack;
            for (Variable* value : expressionStack)
                liveValues.append(get(block, value));
            if (ControlType::isAnyCatch(data))
                liveValues.append(get(block, data.exception()));
        }
        for (Variable* value : currentFrame->m_parser->expressionStack())
            liveValues.append(get(block, value));
    }

    patch->effects.exitsSideways = true;
    patch->appendVectorWithRep(liveValues, ValueRep::LateColdAny);

    return { m_hasExceptionHandlers, callSiteIndex(), static_cast<unsigned>(liveValues.size()), firstStackmapParamOffset, firstStackmapChildOffset };
}

auto OMGIRGenerator::addCatchToUnreachable(unsigned exceptionIndex, const TypeDefinition& signature, ControlType& data, ResultList& results) -> PartialResult
{
    Value* payload = emitCatchImpl(CatchKind::Catch, data, exceptionIndex);
    unsigned offset = 0;
    for (unsigned i = 0; i < signature.as<FunctionSignature>()->argumentCount(); ++i) {
        Type type = signature.as<FunctionSignature>()->argumentType(i);
        Value* value = append<MemoryValue>(heapTop(), m_proc, Load, toB3Type(type), origin(), payload, offset * sizeof(uint64_t));
        results.append(push(value));
        offset += type.kind == TypeKind::V128 ? 2 : 1;
    }
    TRACE_CF("CATCH");
    return { };
}

auto OMGIRGenerator::addCatchAll(Stack& currentStack, ControlType& data) -> PartialResult
{
    unifyValuesWithBlock(currentStack, data);
    TRACE_CF("CATCH_ALL");
    m_currentBlock->appendNewControlValue(m_proc, Jump, origin(), data.continuation);
    return addCatchAllToUnreachable(data);
}

auto OMGIRGenerator::addCatchAllToUnreachable(ControlType& data) -> PartialResult
{
    emitCatchImpl(CatchKind::CatchAll, data);
    return { };
}

Value* OMGIRGenerator::emitCatchImpl(CatchKind kind, ControlType& data, unsigned exceptionIndex)
{
    m_currentBlock = m_proc.addBlock();
    m_rootBlocks.append(m_currentBlock);
    m_stackSize = data.stackSize();

    if (ControlType::isTry(data)) {
        if (kind == CatchKind::Catch)
            data.convertTryToCatch(advanceCallSiteIndex(), m_proc.addVariable(pointerType()));
        else
            data.convertTryToCatchAll(advanceCallSiteIndex(), m_proc.addVariable(pointerType()));
    }
    // We convert from "try" to "catch" ControlType above. This doesn't
    // happen if ControlType is already a "catch". This can happen when
    // we have multiple catches like "try {} catch(A){} catch(B){}...CatchAll(E){}"
    ASSERT(ControlType::isAnyCatch(data));

    HandlerType handlerType = kind == CatchKind::Catch ? HandlerType::Catch : HandlerType::CatchAll;
    m_exceptionHandlers.append({ handlerType, data.tryStart(), data.tryEnd(), 0, m_tryCatchDepth, exceptionIndex });

    reloadMemoryRegistersFromInstance(m_info.memory, instanceValue(), m_currentBlock);

    Value* pointer = append<ArgumentRegValue>(m_proc, Origin(), GPRInfo::argumentGPR0);
    Value* exception = append<ArgumentRegValue>(m_proc, Origin(), GPRInfo::argumentGPR1);
    Value* buffer = append<ArgumentRegValue>(m_proc, Origin(), GPRInfo::argumentGPR2);

    unsigned indexInBuffer = 0;

    Vector<OMGIRGenerator*> frames;
    for (auto* currentFrame = this; currentFrame; currentFrame = currentFrame->m_inlineParent)
        frames.append(currentFrame);
    frames.reverse();

    for (auto* currentFrame : frames) {
        for (auto& local : currentFrame->m_locals)
            append<VariableValue>(m_proc, Set, Origin(), local, loadFromScratchBuffer(indexInBuffer, pointer, local->type()));

        for (unsigned controlIndex = 0; controlIndex < currentFrame->m_parser->controlStack().size(); ++controlIndex) {
            auto& controlData = currentFrame->m_parser->controlStack()[controlIndex].controlData;
            auto& expressionStack = currentFrame->m_parser->controlStack()[controlIndex].enclosedExpressionStack;
            connectControlAtEntrypoint(indexInBuffer, pointer, controlData, expressionStack, data);
        }

        auto& topControlData = currentFrame->m_parser->controlStack().last().controlData;
        auto& topExpressionStack = currentFrame->m_parser->expressionStack();
        connectControlAtEntrypoint(indexInBuffer, pointer, topControlData, topExpressionStack, data);
    }

    set(data.exception(), exception);
    TRACE_CF("CATCH");

    return buffer;
}

auto OMGIRGenerator::addDelegate(ControlType& target, ControlType& data) -> PartialResult
{
    return addDelegateToUnreachable(target, data);
}

auto OMGIRGenerator::addDelegateToUnreachable(ControlType& target, ControlType& data) -> PartialResult
{
    TRACE_CF("DELEGATE");
    unsigned targetDepth = 0;
    if (m_inlineParent)
        targetDepth += m_inlineParent->m_tryCatchDepth;

    if (ControlType::isTry(target))
        targetDepth = target.tryDepth();

    m_exceptionHandlers.append({ HandlerType::Delegate, data.tryStart(), advanceCallSiteIndex(), 0, m_tryCatchDepth, targetDepth });
    return { };
}

auto OMGIRGenerator::addThrow(unsigned exceptionIndex, Vector<ExpressionType>& args, Stack&) -> PartialResult
{
    TRACE_CF("THROW");

    PatchpointValue* patch = m_proc.add<PatchpointValue>(B3::Void, origin(), cloningForbidden(Patchpoint));
    patch->effects.terminal = true;
    patch->append(instanceValue(), ValueRep::reg(GPRInfo::argumentGPR0));
    unsigned offset = 0;
    for (auto arg : args) {
        patch->append(get(arg), ValueRep::stackArgument(offset * sizeof(EncodedJSValue)));
        offset += arg->type().isVector() ? 2 : 1;
    }
    m_maxNumJSCallArguments = std::max(m_maxNumJSCallArguments, offset);
    patch->clobber(RegisterSetBuilder::registersToSaveForJSCall(m_proc.usesSIMD() ? RegisterSetBuilder::allRegisters() : RegisterSetBuilder::allScalarRegisters()));
    PatchpointExceptionHandle handle = preparePatchpointForExceptions(m_currentBlock, patch);
    patch->setGenerator([this, exceptionIndex, handle] (CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        handle.generate(jit, params, this);
        emitThrowImpl(jit, exceptionIndex);
    });
    m_currentBlock->append(patch);

    return { };
}

auto OMGIRGenerator::addRethrow(unsigned, ControlType& data) -> PartialResult
{
    TRACE_CF("RETHROW");

    PatchpointValue* patch = m_proc.add<PatchpointValue>(B3::Void, origin(), cloningForbidden(Patchpoint));
    patch->clobber(RegisterSetBuilder::registersToSaveForJSCall(m_proc.usesSIMD() ? RegisterSetBuilder::allRegisters() : RegisterSetBuilder::allScalarRegisters()));
    patch->effects.terminal = true;
    patch->append(instanceValue(), ValueRep::reg(GPRInfo::argumentGPR0));
    patch->append(get(data.exception()), ValueRep::reg(GPRInfo::argumentGPR1));
    PatchpointExceptionHandle handle = preparePatchpointForExceptions(m_currentBlock, patch);
    patch->setGenerator([this, handle] (CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        handle.generate(jit, params, this);
        emitRethrowImpl(jit);
    });
    m_currentBlock->append(patch);

    return { };
}

auto OMGIRGenerator::addInlinedReturn(const Stack& returnValues) -> PartialResult
{
    dataLogLnIf(WasmOMGIRGeneratorInternal::verboseInlining, "Returning inline to BB ", *m_returnContinuation);

    auto* signature = m_parser->signature().as<FunctionSignature>();
    CallInformation wasmCallInfo = wasmCallingConvention().callInformationFor(m_parser->signature(), CallRole::Callee);
    RELEASE_ASSERT(returnValues.size() >= wasmCallInfo.results.size());
    RELEASE_ASSERT(signature->returnCount() == wasmCallInfo.results.size());

    if (!m_inlinedResults.size() && wasmCallInfo.results.size()) {
        for (unsigned i = 0; i < wasmCallInfo.results.size(); ++i)
            m_inlinedResults.append(m_proc.addVariable(toB3Type(signature->returnType(i))));
    }
    RELEASE_ASSERT(m_inlinedResults.size() == wasmCallInfo.results.size());

    unsigned offset = returnValues.size() - wasmCallInfo.results.size();
    for (unsigned i = 0; i < wasmCallInfo.results.size(); ++i)
        append<B3::VariableValue>(m_proc, B3::Set, origin(), m_inlinedResults[i], get(returnValues[offset + i]));

    m_currentBlock->appendNewControlValue(m_proc, B3::Jump, origin(), FrequentedBlock(m_returnContinuation));
    return { };
}

auto OMGIRGenerator::addReturn(const ControlData&, const Stack& returnValues) -> PartialResult
{
    TRACE_CF("RETURN");
    if (m_returnContinuation)
        return addInlinedReturn(returnValues);

    CallInformation wasmCallInfo = wasmCallingConvention().callInformationFor(m_parser->signature(), CallRole::Callee);
    PatchpointValue* patch = m_proc.add<PatchpointValue>(B3::Void, origin());
    patch->setGenerator([] (CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        params.code().emitEpilogue(jit);
    });
    patch->effects.terminal = true;

    RELEASE_ASSERT(returnValues.size() >= wasmCallInfo.results.size());
    unsigned offset = returnValues.size() - wasmCallInfo.results.size();
    for (unsigned i = 0; i < wasmCallInfo.results.size(); ++i) {
        B3::ValueRep rep = wasmCallInfo.results[i].location;
        if (rep.isStack()) {
            B3::Value* address = append<B3::Value>(m_proc, B3::Add, Origin(), framePointer(), constant(pointerType(), rep.offsetFromFP()));
            append<B3::MemoryValue>(heapFrame(), m_proc, B3::Store, Origin(), get(returnValues[offset + i]), address);
        } else {
            ASSERT(rep.isReg());
            patch->append(get(returnValues[offset + i]), rep);
        }

        TRACE_VALUE(m_parser->signature().as<FunctionSignature>()->returnType(i), get(returnValues[offset + i]), "put to return value ", i);
    }

    m_currentBlock->append(patch);
    return { };
}

auto OMGIRGenerator::addBranch(ControlData& data, ExpressionType condition, const Stack& returnValues) -> PartialResult
{
    unifyValuesWithBlock(returnValues, data);

    BasicBlock* target = data.targetBlockForBranch();
    FrequencyClass targetFrequency = FrequencyClass::Normal;
    FrequencyClass continuationFrequency = FrequencyClass::Normal;

    BranchHint hint = m_info.getBranchHint(m_functionIndex, m_parser->currentOpcodeStartingOffset());
    switch (hint) {
    case BranchHint::Unlikely:
        targetFrequency = FrequencyClass::Rare;
        break;
    case BranchHint::Likely:
        continuationFrequency = FrequencyClass::Rare;
        break;
    case BranchHint::Invalid:
        break;
    }

    TRACE_CF("BRANCH to ", *target);

    if (condition) {
        BasicBlock* continuation = m_proc.addBlock();
        append<Value>(m_proc, B3::Branch, origin(), get(condition));
        m_currentBlock->setSuccessors(FrequentedBlock(target, targetFrequency), FrequentedBlock(continuation, continuationFrequency));
        target->addPredecessor(m_currentBlock);
        continuation->addPredecessor(m_currentBlock);
        m_currentBlock = continuation;
    } else {
        m_currentBlock->appendNewControlValue(m_proc, Jump, origin(), FrequentedBlock(target, targetFrequency));
        target->addPredecessor(m_currentBlock);
    }

    return { };
}

auto OMGIRGenerator::addBranchNull(ControlData& data, ExpressionType reference, const Stack& returnValues, bool shouldNegate, ExpressionType& result) -> PartialResult
{
    auto condition = push<Value>(m_proc, shouldNegate ? B3::NotEqual : B3::Equal, origin(), get(reference), append<Const64Value>(m_proc, origin(), JSValue::encode(jsNull())));
    // We should pop the condition here to keep stack size consistent.
    --m_stackSize;

    WASM_FAIL_IF_HELPER_FAILS(addBranch(data, condition, returnValues));

    if (!shouldNegate)
        result = push(get(reference));

    return { };
}

auto OMGIRGenerator::addBranchCast(ControlData& data, ExpressionType reference, const Stack& returnValues, bool allowNull, int32_t heapType, bool shouldNegate) -> PartialResult
{
    ExpressionType condition;
    emitRefTestOrCast(CastKind::Test, reference, allowNull, heapType, shouldNegate, condition);
    // We should pop the condition here to keep stack size consistent.
    --m_stackSize;

    WASM_FAIL_IF_HELPER_FAILS(addBranch(data, condition, returnValues));

    return { };
}

auto OMGIRGenerator::addSwitch(ExpressionType condition, const Vector<ControlData*>& targets, ControlData& defaultTarget, const Stack& expressionStack) -> PartialResult
{
    TRACE_CF("SWITCH");
    UNUSED_PARAM(expressionStack);
    for (size_t i = 0; i < targets.size(); ++i)
        unifyValuesWithBlock(expressionStack, *targets[i]);
    unifyValuesWithBlock(expressionStack, defaultTarget);

    SwitchValue* switchValue = append<SwitchValue>(m_proc, origin(), get(condition));
    switchValue->setFallThrough(FrequentedBlock(defaultTarget.targetBlockForBranch()));
    for (size_t i = 0; i < targets.size(); ++i)
        switchValue->appendCase(SwitchCase(i, FrequentedBlock(targets[i]->targetBlockForBranch())));

    return { };
}

auto OMGIRGenerator::endBlock(ControlEntry& entry, Stack& expressionStack) -> PartialResult
{
    ControlData& data = entry.controlData;

    ASSERT(expressionStack.size() == data.signature()->returnCount());
    if (data.blockType() != BlockType::Loop)
        unifyValuesWithBlock(expressionStack, data);

    m_currentBlock->appendNewControlValue(m_proc, Jump, origin(), data.continuation);
    data.continuation->addPredecessor(m_currentBlock);

    return addEndToUnreachable(entry, expressionStack);
}

auto OMGIRGenerator::addEndToUnreachable(ControlEntry& entry, const Stack& expressionStack) -> PartialResult
{
    ControlData& data = entry.controlData;
    m_currentBlock = data.continuation;
    m_stackSize = data.stackSize();

    if (data.blockType() == BlockType::If) {
        data.special->appendNewControlValue(m_proc, Jump, origin(), m_currentBlock);
        m_currentBlock->addPredecessor(data.special);
    } else if (data.blockType() == BlockType::Try || data.blockType() == BlockType::Catch)
        --m_tryCatchDepth;

    if (data.blockType() != BlockType::Loop) {
        for (unsigned i = 0; i < data.signature()->returnCount(); ++i) {
            Value* result = data.phis[i];
            m_currentBlock->append(result);
            entry.enclosedExpressionStack.constructAndAppend(data.signature()->returnType(i), push(result));
        }
    } else {
        m_outerLoops.removeLast();
        for (unsigned i = 0; i < data.signature()->returnCount(); ++i) {
            if (i < expressionStack.size()) {
                ++m_stackSize;
                entry.enclosedExpressionStack.append(expressionStack[i]);
            } else {
                Type returnType = data.signature()->returnType(i);
                entry.enclosedExpressionStack.constructAndAppend(returnType, push(constant(toB3Type(returnType), 0xbbadbeef)));
            }
        }
    }

    if constexpr (WasmOMGIRGeneratorInternal::traceStackValues) {
        m_parser->expressionStack().swap(entry.enclosedExpressionStack);
        TRACE_CF("END: ", *data.signature(), " block type ", (int) data.blockType());
        m_parser->expressionStack().swap(entry.enclosedExpressionStack);
    }

    // TopLevel does not have any code after this so we need to make sure we emit a return here.
    if (data.blockType() == BlockType::TopLevel)
        return addReturn(entry.controlData, entry.enclosedExpressionStack);

    return { };
}


auto OMGIRGenerator::createCallPatchpoint(BasicBlock* block, B3::Type returnType, const CallInformation& wasmCalleeInfo, const Vector<ExpressionType>& tmpArgs) -> CallPatchpointData
{
    Vector<B3::ConstrainedValue> constrainedPatchArgs;
    for (unsigned i = 0; i < tmpArgs.size(); ++i)
        constrainedPatchArgs.append(B3::ConstrainedValue(get(block, tmpArgs[i]), wasmCalleeInfo.params[i]));

    Box<PatchpointExceptionHandle> exceptionHandle = Box<PatchpointExceptionHandle>::create(m_hasExceptionHandlers, callSiteIndex());

    PatchpointValue* patchpoint = m_proc.add<PatchpointValue>(returnType, origin());
    patchpoint->effects.writesPinned = true;
    patchpoint->effects.readsPinned = true;
    patchpoint->clobberEarly(RegisterSetBuilder::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSetBuilder::registersToSaveForJSCall(m_proc.usesSIMD() ? RegisterSetBuilder::allRegisters() : RegisterSetBuilder::allScalarRegisters()));
    patchpoint->appendVector(constrainedPatchArgs);

    *exceptionHandle = preparePatchpointForExceptions(block, patchpoint);

    const Vector<ArgumentLocation, 1>& constrainedResultLocations = wasmCalleeInfo.results;
    if (returnType != B3::Void) {
        Vector<B3::ValueRep, 1> resultConstraints;
        for (auto valueLocation : constrainedResultLocations)
            resultConstraints.append(B3::ValueRep(valueLocation.location));
        patchpoint->resultConstraints = WTFMove(resultConstraints);
    }
    block->append(patchpoint);
    return { patchpoint, exceptionHandle, nullptr };
}

// See emitTailCallPatchpoint for the setup before this.
static inline void prepareForTailCallImpl(unsigned functionIndex, CCallHelpers& jit, const B3::StackmapGenerationParams& params, CallInformation wasmCallerInfoAsCallee, CallInformation wasmCalleeInfoAsCallee, unsigned firstPatchArg, unsigned lastPatchArg, int32_t newFPOffsetFromFP)
{
    const Checked<int32_t> offsetOfFirstSlotFromFP = WTF::roundUpToMultipleOf<stackAlignmentBytes()>(wasmCallerInfoAsCallee.headerAndArgumentStackSizeInBytes);
    JIT_COMMENT(jit, "Set up tail call, new FP offset from FP: ", newFPOffsetFromFP);
    AllowMacroScratchRegisterUsage allowScratch(jit);

    // Be careful not to clobber this below.
    // We also need to make sure that we preserve this if it is used by the patchpoint body.
    bool clobbersTmp = false;
    auto tmp = jit.scratchRegister();
    int tmpSpill = 0;

    // Set up a valid frame so that we can clobber this one.
    RegisterAtOffsetList calleeSaves = params.code().calleeSaveRegisterAtOffsetList();
    jit.emitRestore(calleeSaves);

    for (unsigned i = 0; i < params.size(); ++i) {
        auto arg = params[i];
        if (arg.isGPR()) {
            ASSERT(!calleeSaves.find(arg.gpr()));
            if (arg.gpr() == tmp)
                clobbersTmp = true;
            continue;
        }
        if (arg.isFPR()) {
            ASSERT(!calleeSaves.find(arg.fpr()));
            continue;
        }
    }

    const unsigned frameSize = params.code().frameSize();
    ASSERT(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(frameSize) == frameSize);
    ASSERT(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(std::abs(newFPOffsetFromFP)) == static_cast<size_t>(std::abs(newFPOffsetFromFP)));

    auto fpOffsetToSPOffset = [frameSize](int32_t offset) {
        return checkedSum<int>(safeCast<int>(frameSize), offset).value();
    };

    JIT_COMMENT(jit, "Let's use the caller's frame, so that we always have a valid frame.");
    if (WasmOMGIRGeneratorInternal::verboseTailCalls) {
        jit.probeDebugSIMD([frameSize, fpOffsetToSPOffset, newFPOffsetFromFP, wasmCalleeInfoAsCallee, firstPatchArg, params, functionIndex] (Probe::Context& context) {
            uint64_t sp = context.gpr<uint64_t>(MacroAssembler::stackPointerRegister);
            uint64_t fp = context.gpr<uint64_t>(GPRInfo::callFrameRegister);
            dataLogLn("Before tail call in function ", functionIndex, " before changing anything: FP: ", RawHex(fp), " SP: ", RawHex(sp));
            dataLogLn("New FP will be at ", RawHex(sp + fpOffsetToSPOffset(newFPOffsetFromFP)));
            CallFrame* fpp = context.gpr<CallFrame*>(GPRInfo::callFrameRegister);
            dataLogLn("callee original: ", RawPointer(fpp->callee().rawPtr()));
            auto& wasmCallee = context.gpr<uint64_t*>(GPRInfo::callFrameRegister)[CallFrameSlot::callee * 1];
            dataLogLn("callee original: ", RawHex(wasmCallee), " at ", RawPointer(&wasmCallee));
            dataLogLn("retPC original: ", RawPointer(fpp->rawReturnPCForInspection()));
            auto& retPC = context.gpr<uint64_t*>(GPRInfo::callFrameRegister)[CallFrame::returnPCOffset() / sizeof(uint64_t)];
            dataLogLn("retPC original: ", RawHex(retPC), " at ", RawPointer(&retPC));
            dataLogLn("callerFrame original: ", RawPointer(fpp->callerFrame()));
            ASSERT_UNUSED(frameSize, sp + frameSize == fp);

            auto fpl = context.gpr<uint64_t*>(GPRInfo::callFrameRegister);
            auto fpi = context.gpr<uint32_t*>(GPRInfo::callFrameRegister);

            for (unsigned i = 0; i < wasmCalleeInfoAsCallee.params.size(); ++i) {
                auto src = params[firstPatchArg + i];
                auto dst = wasmCalleeInfoAsCallee.params[i].location;
                auto width = wasmCalleeInfoAsCallee.params[i].width;
                dataLog("Arg source ", i, " located at ", src, " = ");
                if (src.isGPR())
                    dataLog(context.gpr(src.gpr()), " / ", (int) context.gpr(src.gpr()));
                else if (src.isFPR() && width <= Width::Width64)
                    dataLog(context.fpr(src.fpr(), SavedFPWidth::SaveVectors));
                else if (src.isFPR())
                    dataLog(context.vector(src.fpr()));
                else if (src.isConstant())
                    dataLog(src.value(), " / ", src.doubleValue());
                else
                    dataLog(fpl[src.offsetFromFP() / sizeof(uint64_t)], " / ", fpi[src.offsetFromFP() / sizeof(uint32_t)], " / ", bitwise_cast<double>(fpl[src.offsetFromFP() / sizeof(uint64_t)]), " at ", RawPointer(&fpp[src.offsetFromFP() / sizeof(uint64_t)]));
                dataLogLn(" -> ", dst);
            }
        });
    }
    jit.loadPtr(CCallHelpers::Address(MacroAssembler::framePointerRegister, CallFrame::callerFrameOffset()), MacroAssembler::framePointerRegister);
    if (WasmOMGIRGeneratorInternal::verboseTailCalls) {
        jit.probeDebugSIMD([] (Probe::Context& context) {
            uint64_t sp = context.gpr<uint64_t>(MacroAssembler::stackPointerRegister);
            uint64_t fp = context.gpr<uint64_t>(GPRInfo::callFrameRegister);
            dataLogLn("In the new expanded frame, including F's caller: FP: ", RawHex(fp), " SP: ", RawHex(sp));
        });
    }

    auto newReturnPCOffset = fpOffsetToSPOffset(checkedSum<intptr_t>(CallFrame::returnPCOffset(), newFPOffsetFromFP).value());

    JIT_COMMENT(jit, "Copy over args if needed into their final position, clobbering everything.");
    // This code has a bunch of overlap with CallFrameShuffler and Shuffle in Air/BBQ

    // We requested some extra stack space below via requestCallArgAreaSize
    // ... FP [initial safe area][caller stack space ] [callArgSpace                    ] SP ...
    // becomes
    // ... FP [safe area growing ->    ] [danger           ] [ scratch                  ] SP ...
    // This scratch space sits at the very bottom of the stack, near sp.
    // AirLowerStackArgs takes care of adding callArgSpace to our total caller frame size.
    // BUT, even though we have this extra space, the new frame might be bigger, so we can't
    // use the new frame as scratch. The new return pc represents the lowest offset from SP we can use.
    int spillPointer = 0;
    const int scratchAreaUpperBound = std::min(
        safeCast<int>(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(static_cast<int>(wasmCalleeInfoAsCallee.headerAndArgumentStackSizeInBytes))),
        newReturnPCOffset);
    auto allocateSpill = [&] (Width width) -> int {
        int offset = spillPointer;
        spillPointer += bytesForWidth(width);
        ASSERT(spillPointer <= scratchAreaUpperBound);
        ASSERT(offset < scratchAreaUpperBound);
        return offset;
    };

    auto doMove = [&] (int srcOffset, int dstOffset, Width width) {
        JIT_COMMENT(jit, "Do move ", srcOffset, " -> ", dstOffset);
        auto src = CCallHelpers::Address(MacroAssembler::stackPointerRegister, srcOffset);
        auto dst = CCallHelpers::Address(MacroAssembler::stackPointerRegister, dstOffset);
        if (width <= Width32)
            jit.transfer32(src, dst);
        else if (width <= Width64)
            jit.transfer64(src, dst);
        else {
            jit.transfer64(src, dst);
            jit.transfer64(src.withOffset(bytesForWidth(Width::Width64)), dst.withOffset(bytesForWidth(Width::Width64)));
        }
        if (WasmOMGIRGeneratorInternal::verboseTailCalls) {
            jit.probeDebugSIMD([tmp, srcOffset, dstOffset, width] (Probe::Context& context) {
                uint64_t val = context.gpr<uint64_t>(tmp);
                uint64_t sp = context.gpr<uint64_t>(MacroAssembler::stackPointerRegister);
                dataLogLn("Move value ", val, " / ", RawHex(val), " at ", RawHex(sp + srcOffset), " -> ", RawHex(sp + dstOffset), " width ", width);
            });
        }
    };

    // This should grow down towards SP (towards 0) as we move stuff out of the way.
    int safeAreaLowerBound = fpOffsetToSPOffset(CallFrameSlot::codeBlock * sizeof(Register));
    const int stackUpperBound = fpOffsetToSPOffset(offsetOfFirstSlotFromFP); // ArgN in the stack diagram
    ASSERT(safeAreaLowerBound > 0);
    ASSERT(safeAreaLowerBound < stackUpperBound);

    JIT_COMMENT(jit, "SP[", safeAreaLowerBound, "] to SP[", stackUpperBound, "] form the safe portion of the stack to clobber; Scratches go from SP[0] to SP[", scratchAreaUpperBound, "].");

#if ASSERT_ENABLED
    // Clobber all safe values to make debugging easier.
    for (int i = safeAreaLowerBound; i < stackUpperBound; i += sizeof(Register)) {
        jit.storePtr(MacroAssembler::TrustedImmPtr(0xBEEF),
            CCallHelpers::Address(MacroAssembler::stackPointerRegister, i));
    }
#endif

    // srcOffset, dstOffset
    Vector<std::tuple<int, int, Width>> argsToMove;
    Vector<std::tuple<int, int, Width>> spillsToMove;
    argsToMove.reserveInitialCapacity(wasmCalleeInfoAsCallee.params.size() + 1);

    if (clobbersTmp) {
        tmpSpill = allocateSpill(Width::Width64);
        jit.storePtr(tmp, CCallHelpers::Address(MacroAssembler::stackPointerRegister, tmpSpill));
    }

    // We will complete those moves who's source is closest to the danger frontier first.
    // That will move the danger frontier.

    for (unsigned i = 0; i < wasmCalleeInfoAsCallee.params.size(); ++i) {
        auto dst = wasmCalleeInfoAsCallee.params[i];
        if (dst.location.isGPR()) {
            ASSERT(!calleeSaves.find(dst.location.jsr().payloadGPR()));
            continue;
        }
        if (dst.location.isFPR()) {
            ASSERT(!calleeSaves.find(dst.location.fpr()));
            continue;
        }
        auto src = params[firstPatchArg + i];
        ASSERT_UNUSED(lastPatchArg, firstPatchArg + i < lastPatchArg);

        intptr_t srcOffset = -1;

        if (src.isGPR()) {
            ASSERT(dst.width <= Width::Width64);
            srcOffset = allocateSpill(dst.width);
            jit.storePtr(src.gpr(), CCallHelpers::Address(MacroAssembler::stackPointerRegister, srcOffset));
        } else if (src.isFPR()) {
            srcOffset = allocateSpill(dst.width);
            if (dst.width <= Width::Width64)
                jit.storeDouble(src.fpr(), CCallHelpers::Address(MacroAssembler::stackPointerRegister, srcOffset));
            else
                jit.storeVector(src.fpr(), CCallHelpers::Address(MacroAssembler::stackPointerRegister, srcOffset));
        } else if (src.isConstant()) {
            srcOffset = allocateSpill(dst.width);
            ASSERT(dst.width <= Width::Width64);
            jit.move(MacroAssembler::TrustedImm64(src.value()), tmp);
            jit.store64(tmp, CCallHelpers::Address(MacroAssembler::stackPointerRegister, srcOffset));
        } else {
            ASSERT(src.isStack());
            srcOffset = fpOffsetToSPOffset(src.offsetFromFP());
        }
        intptr_t dstOffset = fpOffsetToSPOffset(checkedSum<int32_t>(dst.location.offsetFromFP(), newFPOffsetFromFP).value());
        ASSERT(srcOffset >= 0);
        ASSERT(dstOffset >= 0);
        JIT_COMMENT(jit, "Arg ", i, " has srcOffset ", srcOffset, " dstOffset ", dstOffset);
        argsToMove.append({ srcOffset, dstOffset, dst.width });
    }

    argsToMove.append({
        fpOffsetToSPOffset(CallFrame::returnPCOffset()),
        newReturnPCOffset,
        Width::Width64
    });
    JIT_COMMENT(jit, "ReturnPC has srcOffset ", fpOffsetToSPOffset(CallFrame::returnPCOffset()), " dstOffset ", newReturnPCOffset);

    std::sort(
        argsToMove.begin(), argsToMove.end(),
        [] (const auto& left, const auto& right) {
            return std::get<0>(left) > std::get<0>(right);
        });

    for (unsigned i = 0; i < argsToMove.size(); ++i) {
        auto [srcOffset, dstOffset, width] = argsToMove[i];
        // The first arg is the highest-offset arg, and we expect that moving it should
        // make progress on moving the safe area down.
        ASSERT_UNUSED(safeAreaLowerBound, srcOffset <= safeAreaLowerBound);

        safeAreaLowerBound = srcOffset;
        ASSERT(srcOffset < stackUpperBound);
        ASSERT(dstOffset < stackUpperBound);
        ASSERT(dstOffset >= scratchAreaUpperBound);
        ASSERT(srcOffset >= 0);
        ASSERT(dstOffset >= 0);

        JIT_COMMENT(jit, "SP[", safeAreaLowerBound, "] to SP[", stackUpperBound, "] form the safe portion of the stack to clobber.");

        if (dstOffset >= safeAreaLowerBound)
            doMove(srcOffset, dstOffset, width);
        else {
            JIT_COMMENT(jit, "Must spill.");
            auto scratch = allocateSpill(width);
            doMove(srcOffset, scratch, width);
            spillsToMove.append({ scratch, dstOffset, width });
        }
    }

    JIT_COMMENT(jit, "Move spills");

    for (unsigned i = 0; i < spillsToMove.size(); ++i) {
        auto [srcOffset, dstOffset, width] = spillsToMove[i];
        ASSERT(srcOffset < stackUpperBound);
        ASSERT(dstOffset < stackUpperBound);
        ASSERT(dstOffset >= scratchAreaUpperBound);
        ASSERT(srcOffset >= 0);
        ASSERT(dstOffset >= 0);

        doMove(srcOffset, dstOffset, width);
    }

    JIT_COMMENT(jit, "Now we can restore / resign lr.");

    // Pop our locals, leaving only the new frame behind as though our original caller had called the callee.
    // Also pop callee.
    auto newFPOffsetFromSP = fpOffsetToSPOffset(newFPOffsetFromFP);
    ASSERT(newFPOffsetFromSP > 0);
    ASSERT(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(std::abs(newFPOffsetFromSP)) == static_cast<size_t>(std::abs(newFPOffsetFromSP)));

    auto newSPAtPrologueOffsetFromSP = newFPOffsetFromSP + prologueStackPointerDelta();

    // The return PC should be at the top of the new stack.
    // On ARM64E, we load it before changing SP to avoid needing an extra temp register.

#if CPU(ARM) || CPU(ARM64) || CPU(RISCV64)
    JIT_COMMENT(jit, "Load the return pointer from its saved location.");
    jit.load64(CCallHelpers::Address(MacroAssembler::stackPointerRegister, newFPOffsetFromSP + OBJECT_OFFSETOF(CallerFrameAndPC, returnPC)), tmp);
    jit.move(tmp, MacroAssembler::linkRegister);
    if (WasmOMGIRGeneratorInternal::verboseTailCalls) {
        jit.probeDebugSIMD([] (Probe::Context& context) {
            dataLogLn("tagged return pc: ", RawHex(context.gpr<uint64_t>(MacroAssembler::linkRegister)));
        });
    }
#if CPU(ARM64E)
    JIT_COMMENT(jit, "The return pointer was signed with the stack height before we pushed lr, fp, see emitFunctionPrologue. newFPOffsetFromSP: ", newFPOffsetFromSP, " newFPOffsetFromFP ", newFPOffsetFromFP);
    jit.addPtr(MacroAssembler::TrustedImm32(params.code().frameSize() + sizeof(CallerFrameAndPC)), MacroAssembler::stackPointerRegister, tmp);
    jit.untagPtr(tmp, MacroAssembler::linkRegister);
    if (WasmOMGIRGeneratorInternal::verboseTailCalls) {
        jit.probeDebugSIMD([] (Probe::Context& context) {
            dataLogLn("untagged return pc: ", RawHex(context.gpr<uint64_t>(MacroAssembler::linkRegister)));
        });
    }
    jit.validateUntaggedPtr(MacroAssembler::linkRegister);
#endif
#endif

    jit.addPtr(MacroAssembler::TrustedImm32(newSPAtPrologueOffsetFromSP), MacroAssembler::stackPointerRegister);

#if CPU(X86_64)
    if (WasmOMGIRGeneratorInternal::verboseTailCalls) {
        jit.probeDebugSIMD([] (Probe::Context& context) {
            dataLogLn("return pc on the top of the stack: ", RawHex(*context.gpr<uint64_t*>(MacroAssembler::stackPointerRegister)), " at ", RawHex(context.gpr<uint64_t>(MacroAssembler::stackPointerRegister)));
        });
    }
#endif

#if ASSERT_ENABLED
    for (unsigned i = 2; i < 50; ++i) {
        // Everthing after sp might be overwritten anyway.
        jit.store64(MacroAssembler::TrustedImm32(0xBFFF), CCallHelpers::Address(MacroAssembler::stackPointerRegister, -i * sizeof(uint64_t)));
    }
#endif

    JIT_COMMENT(jit, "OK, now we can jump.");
    if (WasmOMGIRGeneratorInternal::verboseTailCalls) {
        jit.probeDebugSIMD([wasmCalleeInfoAsCallee] (Probe::Context& context) {
            dataLogLn("Can now jump: FP: ", RawHex(context.gpr<uint64_t>(GPRInfo::callFrameRegister)), " SP: ", RawHex(context.gpr<uint64_t>(MacroAssembler::stackPointerRegister)));
            auto* newFP = context.gpr<uint64_t*>(MacroAssembler::stackPointerRegister) - prologueStackPointerDelta() / sizeof(uint64_t);
            dataLogLn("New (callee) FP at prologue will be at ", RawPointer(newFP));
            auto fpl = static_cast<uint64_t*>(newFP);
            auto fpi = reinterpret_cast<uint32_t*>(newFP);

            for (unsigned i = 0; i < wasmCalleeInfoAsCallee.params.size(); ++i) {
                auto arg = wasmCalleeInfoAsCallee.params[i];
                auto src = arg.location;
                dataLog("Arg ", i, " located at ", arg.location, " = ");
                if (arg.location.isGPR())
                    dataLog(context.gpr(arg.location.jsr().gpr()), " / ", (int) context.gpr(arg.location.jsr().gpr()));
                else if (arg.location.isFPR() && arg.width <= Width::Width64)
                    dataLog(context.fpr(arg.location.fpr(), SavedFPWidth::SaveVectors));
                else if (arg.location.isFPR())
                    dataLog(context.vector(arg.location.fpr()));
                else
                    dataLog(fpl[src.offsetFromFP() / sizeof(uint64_t)], " / ", fpi[src.offsetFromFP() / sizeof(uint32_t)],  " / ", RawHex(fpi[src.offsetFromFP() / sizeof(uint32_t)]), " / ", bitwise_cast<double>(fpl[src.offsetFromFP() / sizeof(uint64_t)]), " at ", RawPointer(&fpi[src.offsetFromFP() / sizeof(uint32_t)]));
                dataLogLn();
            }
        });
    }

    if (clobbersTmp)
        jit.loadPtr(CCallHelpers::Address(MacroAssembler::stackPointerRegister, tmpSpill), tmp);
}

// See also: https://leaningtech.com/fantastic-tail-calls-and-how-to-implement-them/, a blog post about contributing this feature.
auto OMGIRGenerator::createTailCallPatchpoint(BasicBlock* block, CallInformation wasmCallerInfoAsCallee, CallInformation wasmCalleeInfoAsCallee, const Vector<ExpressionType>& tmpArgSourceLocations, Vector<B3::ConstrainedValue> patchArgs) -> CallPatchpointData
{
    m_makesTailCalls = true;
    // Our args are placed in argument registers or locals.
    // We must:
    // - Restore callee saves
    // - Restore and re-sign lr
    // - Restore our caller's FP so that the stack area we write to is always valid
    // - Move stack args from our stack to their final resting spots. Note that they might overlap.
    // - Move argumentCountIncludingThis (a.k.a. callSiteIndex) to its final spot, since WASM uses it for exceptions.
    // Layout of stack right now, and after this patchpoint.
    //
    //
    //    |          Original Caller   |                                                                      |          ......            |
    //    +----------------------------+ <--                                                                  +----------------------------+ <--
    //    |           F.argN           |    |                                    +-------------------->       |           G.argM           |    |                            Safe to clobber
    //    +----------------------------+    | lower address                      |                            +----------------------------+    | lower address
    //    |           F.arg1           |    v                                    |                            |           arg1             |    v
    //    +----------------------------+                                         |                            +----------------------------+
    //    |           F.arg0           |                                         |                            |           arg0             |                                   .......... < Danger froniter, grows down as args get moved out of the way
    //    +----------------------------+                                         |                            +----------------------------+
    //    |           F.this           |                                         |                            |           this'            |                                Dangerous to clobber
    //    +----------------------------+                                         |                            +----------------------------+
    //    | argumentCountIncludingThis |                                         |                            |          A.C.I.T.'         |
    //    +----------------------------+                                         |                            +----------------------------+
    //    |          F.callee          |                                         |                            |        G.callee            |
    //    +----------------------------+                                         |                            +----------------------------+
    //    |        F.codeBlock         |                               (shuffleStackArgs...)                  |        G.codeBlock         |
    //    +----------------------------+                                         |                     (arm) >+----------------------------+
    //    | return-address after F     |                                         |                            |   return-address after F   |
    //    +----------------------------+                                         | SP at G prologue (intel) ->+----------------------------+
    //    |          F.caller.FP       |                                         |                            |          F.caller.FP       |
    //    +----------------------------+  <- F.FP                                |    G.FP after G prologue-> +----------------------------+
    //    |          callee saves      |                                         |                            |          callee saves      |
    //    +----------------------------+   <----+   argM to G  ------------------+                            +----------------------------+
    //    |          F.local0          |        |   ....                                                      |          G.local0          |
    //    +----------------------------+        |   arg0 to G                                                 +----------------------------+
    //    |          F.local1          |        |                                                             |          G.local1          |
    //    +----------------------------+        |                                                             +----------------------------+
    //    |          F.localN          |        |                                                             |          G.localM          |
    //    +----------------------------|        |                                                             +----------------------------+
    //    |          ......            |        |                                                             |          ......            |
    //    +----------------------------|  <- SP |                                       SP after G prologue-> +----------------------------+
    //                                          |
    //    Note that F.FP is not the same as G.FP because the number of args may differ.
    // We must not clobber any local because source args may be located anywhere.
    // The final resting place of G.argM (F.argN) up to the return address after F is fair game to clobber; we do not permit StackArgument value reps.

    // First slot here is the last argument to F, a.k.a the first stack slot that belongs to F.
    const Checked<int32_t> offsetOfFirstSlotFromFP = WTF::roundUpToMultipleOf<stackAlignmentBytes()>(wasmCallerInfoAsCallee.headerAndArgumentStackSizeInBytes);
    ASSERT(offsetOfFirstSlotFromFP > 0);
    const Checked<int32_t> offsetOfNewFPFromFirstSlot = checkedProduct<int32_t>(-1, WTF::roundUpToMultipleOf<stackAlignmentBytes()>(wasmCalleeInfoAsCallee.headerAndArgumentStackSizeInBytes));
    const Checked<int32_t> newFPOffsetFromFP = offsetOfFirstSlotFromFP + offsetOfNewFPFromFirstSlot;
    m_tailCallStackOffsetFromFP = std::min(m_tailCallStackOffsetFromFP, newFPOffsetFromFP);

    RegisterSet scratchRegisters = RegisterSetBuilder::macroClobberedGPRs();
    RegisterSet forbiddenArgumentRegisters = RegisterSetBuilder::calleeSaveRegisters().merge(scratchRegisters);

    ASSERT(wasmCalleeInfoAsCallee.params.size() == tmpArgSourceLocations.size());
#if ASSERT_ENABLED
    for (unsigned i = 0; i < patchArgs.size(); ++i) {
        // We will clobber our stack, so we shouldn't be reading any special extra patch args from it after this point.
        ASSERT(patchArgs[i].rep().isReg() || patchArgs[i].rep().isConstant());
        ASSERT(!scratchRegisters.contains(patchArgs[i].rep().reg(), IgnoreVectors));
    }
#endif

    ASSERT(wasmCalleeInfoAsCallee.params.size() == tmpArgSourceLocations.size());
    unsigned firstPatchArg = patchArgs.size();

    for (unsigned i = 0; i < tmpArgSourceLocations.size(); ++i) {
        auto src = get(block, tmpArgSourceLocations[i]);
        auto dst = wasmCalleeInfoAsCallee.params[i];
        ASSERT(dst.location.isStack() || dst.location.isFPR() || dst.location.isGPR());
        ASSERT(dst.width >= src->resultWidth());
        if (!dst.location.isStack()) {
            // We will restore callee saves before jumping to the callee.
            // The calling convention should guarantee this anyway, but let's document it just in case.
            ASSERT_UNUSED(forbiddenArgumentRegisters, !forbiddenArgumentRegisters.contains(dst.location.isGPR() ? Reg(dst.location.jsr().gpr()) : Reg(dst.location.fpr()), IgnoreVectors));
            patchArgs.append(ConstrainedValue(src, dst));
            continue;
        }
        ASSERT(dst.width >= Width64);
        patchArgs.append(src);
    }
    unsigned lastPatchArg = patchArgs.size();

    PatchpointValue* patchpoint = m_proc.add<PatchpointValue>(B3::Void, origin());
    patchpoint->effects.terminal = true;
    patchpoint->effects.readsPinned = true;
    patchpoint->effects.writesPinned = true;

    RegisterSetBuilder clobbers;
    clobbers.merge(RegisterSetBuilder::calleeSaveRegisters());
    clobbers.exclude(RegisterSetBuilder::stackRegisters());
    patchpoint->clobberEarly(WTFMove(clobbers));
    patchpoint->clobberLate(RegisterSetBuilder::macroClobberedGPRs());
    patchpoint->appendVector(WTFMove(patchArgs));
    // See prepareForTailCallImpl for the heart of this patchpoint.
    block->append(patchpoint);

    firstPatchArg += m_proc.resultCount(patchpoint->type());
    lastPatchArg += m_proc.resultCount(patchpoint->type());

    auto prepareForCall = createSharedTask<B3::StackmapGeneratorFunction>([wasmCalleeInfoAsCallee, wasmCallerInfoAsCallee, newFPOffsetFromFP, firstPatchArg, lastPatchArg, functionIndex = m_functionIndex](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        ASSERT(newFPOffsetFromFP >= 0 || params.code().frameSize() >= static_cast<uint32_t>(-newFPOffsetFromFP));
        prepareForTailCallImpl(functionIndex, jit, params, wasmCallerInfoAsCallee, wasmCalleeInfoAsCallee, firstPatchArg, lastPatchArg, newFPOffsetFromFP);
    });

    return { patchpoint, nullptr, WTFMove(prepareForCall) };
}

bool OMGIRGenerator::canInline() const
{
    ASSERT(!m_inlinedBytes || !m_inlineParent);
    return m_inlineDepth < Options::maximumWasmDepthForInlining()
        && m_inlineRoot->m_inlinedBytes.value() < Options::maximumWasmCallerSizeForInlining()
        && (m_inlineDepth <= 1 || StackCheck().isSafeToRecurse());
}

auto OMGIRGenerator::emitInlineDirectCall(uint32_t calleeFunctionIndex, const TypeDefinition& calleeSignature, Vector<ExpressionType>& args, ResultList& resultList) -> PartialResult
{
    Vector<Value*> getArgs;

    for (auto* arg : args)
        getArgs.append(append<VariableValue>(m_proc, B3::Get, origin(), arg));

    BasicBlock* continuation = m_proc.addBlock();
    // Not all inine frames need to save state, but we still need to make sure that there is at least
    // one unique csi per inline frame for stack traces to work.
    advanceCallSiteIndex();
    auto firstInlineCSI = advanceCallSiteIndex();

    const FunctionData& function = m_info.functions[calleeFunctionIndex];
    std::optional<bool> inlineeHasExceptionHandlers;
    {
        Locker locker { m_calleeGroup.m_lock };
        unsigned calleeFunctionIndexSpace = calleeFunctionIndex + m_numImportFunctions;
        auto& inlineCallee = m_calleeGroup.wasmEntrypointCalleeFromFunctionIndexSpace(locker, calleeFunctionIndexSpace);
        inlineeHasExceptionHandlers = inlineCallee.hasExceptionHandlers();
    }
    m_protectedInlineeGenerators.append(makeUnique<OMGIRGenerator>(*this, *m_inlineRoot, m_calleeGroup, calleeFunctionIndex, inlineeHasExceptionHandlers, continuation, WTFMove(getArgs)));
    auto& irGenerator = *m_protectedInlineeGenerators.last();
    m_protectedInlineeParsers.append(makeUnique<FunctionParser<OMGIRGenerator>>(irGenerator, function.data, calleeSignature, m_info));
    auto& parser = *m_protectedInlineeParsers.last();
    WASM_FAIL_IF_HELPER_FAILS(parser.parse());

    irGenerator.insertConstants();
    for (unsigned i = 1; i < irGenerator.m_rootBlocks.size(); ++i) {
        auto* block = irGenerator.m_rootBlocks[i];
        dataLogLnIf(WasmOMGIRGeneratorInternal::verboseInlining, "Block (", i, ")", *block, " is an inline catch handler");
        m_rootBlocks.append(block);
    }
    m_exceptionHandlers.appendVector(WTFMove(irGenerator.m_exceptionHandlers));
    if (irGenerator.m_exceptionHandlers.size())
        m_hasExceptionHandlers = { true };
    RELEASE_ASSERT(!irGenerator.m_callSiteIndex);

    irGenerator.m_topLevelBlock->appendNewControlValue(m_proc, B3::Jump, origin(), FrequentedBlock(irGenerator.m_rootBlocks[0]));
    m_makesCalls |= irGenerator.m_makesCalls;
    ASSERT(&irGenerator.m_proc == &m_proc);

    dataLogLnIf(WasmOMGIRGeneratorInternal::verboseInlining, "Block ", *m_currentBlock, " is going to do an inline call to block ", *irGenerator.m_topLevelBlock, " then continue at ", *continuation);

    append<B3::MemoryValue>(heapCallSiteIndex(), m_proc, B3::Store, origin(),
        m_currentBlock->appendIntConstant(m_proc, origin(), Int32, firstInlineCSI),
        framePointer(), safeCast<int32_t>(CallFrameSlot::argumentCountIncludingThis * sizeof(Register) + TagOffset));

    m_currentBlock->appendNewControlValue(m_proc, B3::Jump, origin(), FrequentedBlock(irGenerator.m_topLevelBlock));
    m_currentBlock = continuation;

    for (unsigned i = 0; i < calleeSignature.as<FunctionSignature>()->returnCount(); ++i)
        resultList.append(push<VariableValue>(m_proc, B3::Get, origin(), irGenerator.m_inlinedResults[i]));

    auto lastInlineCSI = advanceCallSiteIndex();

    append<B3::MemoryValue>(heapCallSiteIndex(), m_proc, B3::Store, origin(),
        m_currentBlock->appendIntConstant(m_proc, origin(), Int32, advanceCallSiteIndex()),
        framePointer(), safeCast<int32_t>(CallFrameSlot::argumentCountIncludingThis * sizeof(Register) + TagOffset));

    m_callee->addCodeOrigin(firstInlineCSI, lastInlineCSI, m_info, calleeFunctionIndex + m_numImportFunctions);

    return { };
}

auto OMGIRGenerator::addCall(uint32_t functionIndex, const TypeDefinition& signature, Vector<ExpressionType>& args, ResultList& results, CallType callType) -> PartialResult
{
    const bool isTailCallInlineCaller = callType == CallType::TailCall && m_inlineParent;
    const bool isTailCall = callType == CallType::TailCall && !isTailCallInlineCaller;

    ASSERT(callType == CallType::Call || isTailCall || isTailCallInlineCaller);
    ASSERT(signature.as<FunctionSignature>()->argumentCount() == args.size());

    TRACE_CF("Call: entered with ", signature);

    const auto& callingConvention = wasmCallingConvention();
    Checked<int32_t> tailCallStackOffsetFromFP;
    CallInformation wasmCalleeInfo = callingConvention.callInformationFor(signature, CallRole::Caller);
    CallInformation wasmCalleeInfoAsCallee = callingConvention.callInformationFor(signature, CallRole::Callee);
    Checked<int32_t> calleeStackSize = WTF::roundUpToMultipleOf<stackAlignmentBytes()>(wasmCalleeInfo.headerAndArgumentStackSizeInBytes);
    if (isTailCall)
        calleeStackSize = WTF::roundUpToMultipleOf<stackAlignmentBytes()>(wasmCalleeInfo.headerAndArgumentStackSizeInBytes * 2 + sizeof(Register));
    const TypeIndex callerTypeIndex = m_info.internalFunctionTypeIndices[m_functionIndex];
    const TypeDefinition& callerTypeDefinition = TypeInformation::get(callerTypeIndex).expand();
    CallInformation wasmCallerInfoAsCallee = callingConvention.callInformationFor(callerTypeDefinition, CallRole::Callee);

    B3::Type returnType = toB3ResultType(&signature);
    Value* jumpDestination = nullptr;

    m_makesCalls = true;
    if (isTailCall || isTailCallInlineCaller)
        m_makesTailCalls = true;

    auto fillResults = [&] (Value* callResult) {
        ASSERT(returnType == callResult->type());

        switch (returnType.kind()) {
        case B3::Void: {
            break;
        }
        case B3::Tuple: {
            const Vector<B3::Type>& tuple = m_proc.tupleForType(returnType);
            ASSERT(signature.as<FunctionSignature>()->returnCount() == tuple.size());
            for (unsigned i = 0; i < signature.as<FunctionSignature>()->returnCount(); ++i)
                results.append(push<ExtractValue>(m_proc, origin(), tuple[i], callResult, i));
            break;
        }
        default: {
            results.append(push(callResult));
            break;
        }
        }
    };

    m_proc.requestCallArgAreaSizeInBytes(calleeStackSize);

    if (m_info.isImportedFunctionFromFunctionIndexSpace(functionIndex)) {
        auto emitCallToImport = [&, this](PatchpointValue* patchpoint, Box<PatchpointExceptionHandle> handle, RefPtr<B3::StackmapGenerator> prepareForCall) -> void {
            unsigned patchArgsIndex = patchpoint->reps().size();
            patchpoint->append(jumpDestination, ValueRep(GPRInfo::nonPreservedNonArgumentGPR0));
            // We need to clobber all potential pinned registers since we might be leaving the instance.
            // We pessimistically assume we could be calling to something that is bounds checking.
            // FIXME: We shouldn't have to do this: https://bugs.webkit.org/show_bug.cgi?id=172181
            patchpoint->clobberLate(RegisterSetBuilder::wasmPinnedRegisters());
            patchArgsIndex += m_proc.resultCount(patchpoint->type());
            patchpoint->setGenerator([this, patchArgsIndex, handle, isTailCall, tailCallStackOffsetFromFP, prepareForCall](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
                AllowMacroScratchRegisterUsage allowScratch(jit);
                if (prepareForCall)
                    prepareForCall->run(jit, params);
                if (handle)
                    handle->generate(jit, params, this);
                if (isTailCall)
                    jit.farJump(params[patchArgsIndex].gpr(), WasmEntryPtrTag);
                else
                    jit.call(params[patchArgsIndex].gpr(), WasmEntryPtrTag);
            });
        };

        m_maxNumJSCallArguments = std::max(m_maxNumJSCallArguments, static_cast<uint32_t>(args.size()));

        // FIXME: Let's remove this indirection by creating a PIC friendly IC
        // for calls out to the js. This shouldn't be that hard to do. We could probably
        // implement the IC to be over Context*.
        // https://bugs.webkit.org/show_bug.cgi?id=170375
        jumpDestination = append<MemoryValue>(heapImports(), m_proc, Load, pointerType(), origin(), instanceValue(), safeCast<int32_t>(JSWebAssemblyInstance::offsetOfImportFunctionStub(functionIndex)));

        if (isTailCall) {
            auto [patchpoint, handle, prepareForCall] = createTailCallPatchpoint(m_currentBlock, wasmCallerInfoAsCallee, wasmCalleeInfoAsCallee, args, { });
            emitCallToImport(patchpoint, handle, prepareForCall);
            return { };
        }

        auto [patchpoint, handle, prepareForCall] = createCallPatchpoint(m_currentBlock, returnType, wasmCalleeInfo, args);
        emitCallToImport(patchpoint, handle, prepareForCall);

        if (returnType != B3::Void)
            fillResults(patchpoint);

        // The call could have been to another WebAssembly instance, and / or could have modified our Memory.
        restoreWebAssemblyGlobalState(m_info.memory, instanceValue(), m_currentBlock);

        if (isTailCallInlineCaller) {
            Stack typedResults;
            typedResults.reserveInitialCapacity(results.size());
            for (unsigned i = 0; i < results.size(); ++i)
                typedResults.append(TypedExpression { signature.as<FunctionSignature>()->returnType(i), results[i] });
            ASSERT(m_returnContinuation);
            return addInlinedReturn(WTFMove(typedResults));
        }

        return { };

    } // isImportedFunctionFromFunctionIndexSpace

    Vector<UnlinkedWasmToWasmCall>* unlinkedWasmToWasmCalls = &m_unlinkedWasmToWasmCalls;

    auto emitUnlinkedWasmToWasmCall = [&, this](PatchpointValue* patchpoint, Box<PatchpointExceptionHandle> handle, RefPtr<B3::StackmapGenerator> prepareForCall) -> void {
        patchpoint->setGenerator([this, handle, unlinkedWasmToWasmCalls, functionIndex, isTailCall, tailCallStackOffsetFromFP, prepareForCall](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            if (prepareForCall)
                prepareForCall->run(jit, params);
            if (handle)
                handle->generate(jit, params, this);

            auto calleeMove = jit.storeWasmCalleeCalleePatchable(isTailCall ? sizeof(CallerFrameAndPC) - prologueStackPointerDelta() : 0);
            CCallHelpers::Call call = isTailCall ? jit.threadSafePatchableNearTailCall() : jit.threadSafePatchableNearCall();
            jit.addLinkTask([unlinkedWasmToWasmCalls, call, functionIndex, calleeMove](LinkBuffer& linkBuffer) {
                unlinkedWasmToWasmCalls->append({ linkBuffer.locationOfNearCall<WasmEntryPtrTag>(call), functionIndex, linkBuffer.locationOf<WasmEntryPtrTag>(calleeMove) });
            });
        });
    };

    if (isTailCall) {
        auto [patchpoint, handle, prepareForCall] = createTailCallPatchpoint(m_currentBlock, wasmCallerInfoAsCallee, wasmCalleeInfoAsCallee, args, { });
        emitUnlinkedWasmToWasmCall(patchpoint, handle, prepareForCall);
        return { };
    }

    if (callType == CallType::Call
        && functionIndex - m_numImportFunctions != m_functionIndex
        && m_info.functionWasmSizeImportSpace(functionIndex) < Options::maximumWasmCalleeSizeForInlining()
        && isAnyOMG(m_compilationMode)
        && canInline()
        && !m_info.callCanClobberInstance(functionIndex)) {
        dataLogLnIf(WasmOMGIRGeneratorInternal::verboseInlining, " inlining call to ", functionIndex - m_numImportFunctions, " from ", m_functionIndex, " depth ", m_inlineDepth);
        m_inlineRoot->m_inlinedBytes += m_info.functionWasmSizeImportSpace(functionIndex);

        return emitInlineDirectCall(functionIndex - m_numImportFunctions, signature, args, results);
    }

    // We do not need to store |this| with JS instance since,
    // 1. It is not tail-call. So this does not clobber the arguments of this function.
    // 2. We are not changing instance. Thus, |this| of this function's arguments are the same and OK.

    auto [patchpoint, handle, prepareForCall] = createCallPatchpoint(m_currentBlock, returnType, wasmCalleeInfo, args);
    emitUnlinkedWasmToWasmCall(patchpoint, handle, prepareForCall);
    // We need to clobber the size register since the LLInt always bounds checks
    if (useSignalingMemory() || m_info.memory.isShared())
        patchpoint->clobberLate(RegisterSetBuilder { GPRInfo::wasmBoundsCheckingSizeRegister });


    fillResults(patchpoint);

    if (m_info.callCanClobberInstance(functionIndex))
        restoreWebAssemblyGlobalState(m_info.memory, instanceValue(), m_currentBlock);

    if (isTailCallInlineCaller) {
        Stack typedResults;
        typedResults.reserveInitialCapacity(results.size());
        for (unsigned i = 0; i < results.size(); ++i)
            typedResults.append(TypedExpression { signature.as<FunctionSignature>()->returnType(i), results[i] });
        ASSERT(m_returnContinuation);
        return addInlinedReturn(WTFMove(typedResults));
    }

    return { };
}

auto OMGIRGenerator::addCallIndirect(unsigned tableIndex, const TypeDefinition& originalSignature, Vector<ExpressionType>& args, ResultList& results, CallType callType) -> PartialResult
{
    Value* calleeIndex = get(args.takeLast());
    const TypeDefinition& signature = originalSignature.expand();
    ASSERT(signature.as<FunctionSignature>()->argumentCount() == args.size());

    TRACE_CF("Call_indirect: entered with table index: ", tableIndex, " ", originalSignature);

    // Note: call indirect can call either WebAssemblyFunction or WebAssemblyWrapperFunction. Because
    // WebAssemblyWrapperFunction is like calling into the js, we conservatively assume all call indirects
    // can be to the js for our stack check calculation.
    m_maxNumJSCallArguments = std::max(m_maxNumJSCallArguments, static_cast<uint32_t>(args.size()));

    Value* callableFunctionBuffer = nullptr;
    Value* callableFunctionBufferLength;
    {
        MemoryValue* table = append<MemoryValue>(heapTables(), tableIndex, m_proc, Load, pointerType(), origin(), instanceValue(), safeCast<int32_t>(JSWebAssemblyInstance::offsetOfTablePtr(m_numImportFunctions, tableIndex)));
        ASSERT(tableIndex < m_info.tableCount());
        auto& tableInformation = m_info.table(tableIndex);

        if (tableInformation.maximum() && tableInformation.maximum().value() == tableInformation.initial()) {
            callableFunctionBufferLength = constant(B3::Int32, tableInformation.initial(), origin());
            if (!tableInformation.isImport()) {
                // Table is fixed-sized and it is not imported one. Thus this is definitely fixed-sized FuncRefTable.
                callableFunctionBuffer = append<Value>(m_proc, Add, origin(), table, constant(pointerType(), safeCast<int32_t>(FuncRefTable::offsetOfFunctionsForFixedSizedTable())));
            }
        } else
            callableFunctionBufferLength = append<MemoryValue>(heapTableFields(), m_proc, Load, Int32, origin(), table, safeCast<int32_t>(Table::offsetOfLength()));

        if (!callableFunctionBuffer)
            callableFunctionBuffer = append<MemoryValue>(heapFuncRefTableFields(), m_proc, Load, pointerType(), origin(), table, safeCast<int32_t>(FuncRefTable::offsetOfFunctions()));
    }

    // Check the index we are looking for is valid.
    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, AboveEqual, origin(), calleeIndex, callableFunctionBufferLength));

        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsCallIndirect);
        });
    }

    calleeIndex = append<Value>(m_proc, ZExt32, origin(), calleeIndex);

    Value* callableFunction = append<Value>(m_proc, Add, origin(), callableFunctionBuffer, append<Value>(m_proc, Mul, origin(), calleeIndex, constant(pointerType(), sizeof(FuncRefTable::Function))));

    // Check that the WasmToWasmImportableFunction is initialized. We trap if it isn't. An "invalid" SignatureIndex indicates it's not initialized.
    // FIXME: when we have trap handlers, we can just let the call fail because Signature::invalidIndex is 0. https://bugs.webkit.org/show_bug.cgi?id=177210
    static_assert(sizeof(WasmToWasmImportableFunction::typeIndex) == sizeof(uint64_t), "Load codegen assumes i64");
    Value* calleeSignatureIndex = append<MemoryValue>(heapFunctionFields(), m_proc, Load, Int64, origin(), callableFunction, safeCast<int32_t>(FuncRefTable::Function::offsetOfFunction() + WasmToWasmImportableFunction::offsetOfSignatureIndex()));
    Value* calleeCodeLocation = append<MemoryValue>(heapFunctionFields(), m_proc, Load, pointerType(), origin(), callableFunction, safeCast<int32_t>(FuncRefTable::Function::offsetOfFunction() + WasmToWasmImportableFunction::offsetOfEntrypointLoadLocation()));
    Value* calleeCallee = append<MemoryValue>(heapTop(), m_proc, Load, pointerType(), origin(),
        append<MemoryValue>(heapFunctionFields(), m_proc, Load, pointerType(), origin(), callableFunction, safeCast<int32_t>(FuncRefTable::Function::offsetOfFunction() + WasmToWasmImportableFunction::offsetOfBoxedWasmCalleeLoadLocation())));
    Value* calleeRTT = append<MemoryValue>(heapFunctionFields(), m_proc, Load, pointerType(), origin(), callableFunction, safeCast<int32_t>(FuncRefTable::Function::offsetOfFunction() + WasmToWasmImportableFunction::offsetOfRTT()));
    Value* calleeInstance = append<MemoryValue>(heapFunctionFields(), m_proc, Load, pointerType(), origin(), callableFunction, safeCast<int32_t>(FuncRefTable::Function::offsetOfInstance()));

    BasicBlock* continuation = m_proc.addBlock();
    BasicBlock* moreChecks = m_proc.addBlock();
    Value* expectedSignatureIndex = append<Const64Value>(m_proc, origin(), TypeInformation::get(originalSignature));
    Value* hasEqualSignatures = append<Value>(m_proc, Equal, origin(), calleeSignatureIndex, expectedSignatureIndex);
    m_currentBlock->appendNewControlValue(m_proc, B3::Branch, origin(), hasEqualSignatures,
        FrequentedBlock(continuation), FrequentedBlock(moreChecks, FrequencyClass::Rare));

    m_currentBlock = moreChecks;
    // If the table entry is null we can't do any further checks.
    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), calleeSignatureIndex, constant(pointerType(), 0)));

        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::NullTableEntry);
        });
    }

    BasicBlock* throwBlock = m_proc.addBlock();
    // The subtype check can be omitted as an optimization for final types, but is needed otherwise if GC is on.
    if (Options::useWasmGC() && !originalSignature.isFinalType()) {
        // We don't need to check the RTT kind because by validation both RTTs must be for functions.
        Value* rttSize = append<MemoryValue>(heapTop(), m_proc, Load, Int32, origin(), calleeRTT, safeCast<uint32_t>(RTT::offsetOfDisplaySize()));
        Value* rttSizeAsPointerType = append<Value>(m_proc, ZExt32, origin(), rttSize);
        Value* rttPayloadPointer = append<Value>(m_proc, Add, pointerType(), origin(), calleeRTT, constant(pointerType(), RTT::offsetOfPayload()));
        auto signatureRTT = TypeInformation::getCanonicalRTT(originalSignature.index());

        // If the RTT display size is <= 0 then throw.
        BasicBlock* greaterThanZero = m_proc.addBlock();
        m_currentBlock->appendNewControlValue(m_proc, B3::Branch, origin(),
            append<Value>(m_proc, Above, origin(), rttSize, constant(Int32, 0)),
            FrequentedBlock(greaterThanZero), FrequentedBlock(throwBlock, FrequencyClass::Rare));
        m_currentBlock = greaterThanZero;

        BasicBlock* checkIfSupertypeIsInDisplay = m_proc.addBlock();
        bool parentRTTHasEntries = signatureRTT->displaySize() > 0;
        if (parentRTTHasEntries) {
            // If the RTT display is not larger than the signature display, throw.
            m_currentBlock->appendNewControlValue(m_proc, B3::Branch, origin(),
                append<Value>(m_proc, Above, origin(), rttSize, constant(Int32, signatureRTT->displaySize())),
                FrequentedBlock(checkIfSupertypeIsInDisplay), FrequentedBlock(throwBlock, FrequencyClass::Rare));
        } else
            m_currentBlock->appendNewControlValue(m_proc, B3::Jump, origin(), FrequentedBlock(checkIfSupertypeIsInDisplay));

        // Check if the display contains the supertype signature.
        m_currentBlock = checkIfSupertypeIsInDisplay;
        Value* payloadIndexed = append<Value>(m_proc, Add, pointerType(), origin(), rttPayloadPointer,
            append<Value>(m_proc, Mul, pointerType(), origin(), constant(pointerType(), sizeof(uintptr_t)),
                append<Value>(m_proc, Sub, pointerType(), origin(), rttSizeAsPointerType, constant(pointerType(), 1 + (parentRTTHasEntries ? signatureRTT->displaySize() : 0)))));
        Value* displayEntry = append<MemoryValue>(heapTop(), m_proc, Load, pointerType(), origin(), payloadIndexed);
        m_currentBlock->appendNewControlValue(m_proc, B3::Branch, origin(),
            append<Value>(m_proc, Equal, origin(), displayEntry, constant(pointerType(), bitwise_cast<uintptr_t>(signatureRTT.get()))),
            FrequentedBlock(continuation), FrequentedBlock(throwBlock, FrequencyClass::Rare));
    } else
        m_currentBlock->appendNewControlValue(m_proc, B3::Jump, origin(), throwBlock);

    m_currentBlock = throwBlock;
    B3::PatchpointValue* throwException = append<B3::PatchpointValue>(m_proc, B3::Void, origin());
    throwException->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
        this->emitExceptionCheck(jit, ExceptionType::BadSignature);
    });
    throwException->effects.terminal = true;

    m_currentBlock = continuation;
    Value* calleeCode = append<MemoryValue>(heapTop(), m_proc, Load, pointerType(), origin(), calleeCodeLocation);
    return emitIndirectCall(calleeInstance, calleeCode, calleeCallee, signature, args, results, callType);
}

auto OMGIRGenerator::addCallRef(const TypeDefinition& originalSignature, Vector<ExpressionType>& args, ResultList& results) -> PartialResult
{
    Value* callee = get(args.takeLast());
    TRACE_VALUE(Wasm::Types::Void, callee, "call_ref: ", originalSignature);
    const TypeDefinition& signature = originalSignature.expand();
    ASSERT(signature.as<FunctionSignature>()->argumentCount() == args.size());
    m_makesCalls = true;

    // Note: call ref can call either WebAssemblyFunction or WebAssemblyWrapperFunction. Because
    // WebAssemblyWrapperFunction is like calling into the js, we conservatively assume all call indirects
    // can be to the js for our stack check calculation.
    m_maxNumJSCallArguments = std::max(m_maxNumJSCallArguments, static_cast<uint32_t>(args.size()));

    // Check the target reference for null.
    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), callee, append<Const64Value>(m_proc, origin(), JSValue::encode(jsNull()))));
        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::NullReference);
        });
    }

    Value* instanceOffset = constant(pointerType(), safeCast<int32_t>(WebAssemblyFunctionBase::offsetOfInstance()));
    Value* calleeInstance = append<MemoryValue>(heapTop(), m_proc, Load, pointerType(), origin(),
        append<Value>(m_proc, Add, origin(), callee, instanceOffset));

    Value* calleeCode = append<MemoryValue>(heapTop(), m_proc, Load, pointerType(), origin(),
        append<MemoryValue>(heapTop(), m_proc, Load, pointerType(), origin(), callee,
        safeCast<int32_t>(WebAssemblyFunctionBase::offsetOfEntrypointLoadLocation())));

    Value* calleeCallee = append<MemoryValue>(heapTop(), m_proc, Load, pointerType(), origin(),
        append<MemoryValue>(heapTop(), m_proc, Load, pointerType(), origin(), callee,
        safeCast<int32_t>(WebAssemblyFunctionBase::offsetOfBoxedWasmCalleeLoadLocation())));

    return emitIndirectCall(calleeInstance, calleeCode, calleeCallee, signature, args, results);
}

void OMGIRGenerator::unify(Value* phi, const ExpressionType source)
{
    append<UpsilonValue>(m_proc, origin(), get(source), phi);
}

void OMGIRGenerator::unifyValuesWithBlock(const Stack& resultStack, const ControlData& block)
{
    const Vector<Value*>& phis = block.phis;
    size_t resultSize = phis.size();

    ASSERT(resultSize <= resultStack.size());

    for (size_t i = 0; i < resultSize; ++i)
        unify(phis[resultSize - 1 - i], resultStack.at(resultStack.size() - 1 - i));
}

static void dumpExpressionStack(const CommaPrinter& comma, const OMGIRGenerator::Stack& expressionStack)
{
    dataLog(comma, "ExpressionStack:");
    for (const auto& expression : expressionStack)
        dataLog(comma, *expression);
}

void OMGIRGenerator::dump(const ControlStack& controlStack, const Stack* expressionStack)
{
    dataLogLn("Constants:");
    for (const auto& constant : m_constantPool)
        dataLogLn(deepDump(m_proc, constant.value));

    dataLogLn("Processing Graph:");
    dataLog(m_proc);
    dataLogLn("With current block:", *m_currentBlock);
    dataLogLn("Control stack:");
    ASSERT(controlStack.size());
    for (size_t i = controlStack.size(); i--;) {
        dataLog("  ", controlStack[i].controlData, ": ");
        CommaPrinter comma(", "_s, ""_s);
        dumpExpressionStack(comma, *expressionStack);
        expressionStack = &controlStack[i].enclosedExpressionStack;
        dataLogLn();
    }
    dataLogLn();
}

auto OMGIRGenerator::origin() -> Origin
{
    if (!m_parser)
        return Origin();
    OpcodeOrigin origin = OpcodeOrigin(m_parser->currentOpcode(), m_parser->currentOpcodeStartingOffset());
    switch (m_parser->currentOpcode()) {
    case OpType::Ext1:
    case OpType::ExtGC:
    case OpType::ExtAtomic:
    case OpType::ExtSIMD:
        origin = OpcodeOrigin(m_parser->currentOpcode(), m_parser->currentExtendedOpcode(), m_parser->currentOpcodeStartingOffset());
        break;
    default:
        break;
    }
    ASSERT(isValidOpType(static_cast<uint8_t>(origin.opcode())));
    return bitwise_cast<Origin>(origin);
}

static bool shouldDumpIRFor(uint32_t functionIndex)
{
    static LazyNeverDestroyed<FunctionAllowlist> dumpAllowlist;
    static std::once_flag initializeAllowlistFlag;
    std::call_once(initializeAllowlistFlag, [] {
        const char* functionAllowlistFile = Options::wasmOMGFunctionsToDump();
        dumpAllowlist.construct(functionAllowlistFile);
    });
    return dumpAllowlist->shouldDumpWasmFunction(functionIndex);
}

Expected<std::unique_ptr<InternalFunction>, String> parseAndCompileOMG(CompilationContext& compilationContext, OptimizingJITCallee& callee, const FunctionData& function, const TypeDefinition& signature, Vector<UnlinkedWasmToWasmCall>& unlinkedWasmToWasmCalls, CalleeGroup& calleeGroup, const ModuleInformation& info, MemoryMode mode, CompilationMode compilationMode, uint32_t functionIndex, std::optional<bool> hasExceptionHandlers, uint32_t loopIndexForOSREntry, TierUpCount* tierUp)
{
    CompilerTimingScope totalScope("B3"_s, "Total OMG compilation"_s);

    Wasm::Thunks::singleton().stub(Wasm::catchInWasmThunkGenerator);

    auto result = makeUnique<InternalFunction>();

    compilationContext.wasmEntrypointJIT = makeUnique<CCallHelpers>();
    compilationContext.procedure = makeUnique<Procedure>(info.usesSIMD(functionIndex));

    Procedure& procedure = *compilationContext.procedure;
    if (shouldDumpIRFor(functionIndex + info.importFunctionCount()))
        procedure.setShouldDumpIR();


    if (Options::useSamplingProfiler()) {
        // FIXME: We should do this based on VM relevant info.
        // But this is good enough for our own profiling for now.
        // When we start to show this data in web inspector, we'll
        // need other hooks into this besides the JSC option.
        procedure.setNeedsPCToOriginMap();
    }

    procedure.setOriginPrinter([] (PrintStream& out, Origin origin) {
        if (origin.data())
            out.print("Wasm: ", OpcodeOrigin(origin));
    });

    // This means we cannot use either StackmapGenerationParams::usedRegisters() or
    // StackmapGenerationParams::unavailableRegisters(). In exchange for this concession, we
    // don't strictly need to run Air::reportUsedRegisters(), which saves a bit of CPU time at
    // optLevel=1.
    procedure.setNeedsUsedRegisters(false);

    procedure.setOptLevel(Options::wasmOMGOptimizationLevel());

    procedure.code().setForceIRCRegisterAllocation();

    OMGIRGenerator irGenerator(calleeGroup, info, callee, procedure, unlinkedWasmToWasmCalls, result->osrEntryScratchBufferSize, mode, compilationMode, functionIndex, hasExceptionHandlers, loopIndexForOSREntry, tierUp);
    FunctionParser<OMGIRGenerator> parser(irGenerator, function.data, signature, info);
    WASM_FAIL_IF_HELPER_FAILS(parser.parse());

    irGenerator.computeHeapRanges();
    irGenerator.insertEntrySwitch();
    irGenerator.insertConstants();

    procedure.resetReachability();
    if (ASSERT_ENABLED)
        validate(procedure, "After parsing:\n");

    estimateStaticExecutionCounts(procedure);

    dataLogIf(WasmOMGIRGeneratorInternal::verbose, "Pre SSA: ", procedure);
    fixSSA(procedure);
    dataLogIf(WasmOMGIRGeneratorInternal::verbose, "Post SSA: ", procedure);

    {
        if (shouldDumpDisassemblyFor(compilationMode))
            procedure.code().setDisassembler(makeUnique<B3::Air::Disassembler>());
        B3::prepareForGeneration(procedure);
        B3::generate(procedure, *compilationContext.wasmEntrypointJIT);
        compilationContext.wasmEntrypointByproducts = procedure.releaseByproducts();
        result->entrypoint.calleeSaveRegisters = procedure.calleeSaveRegisterAtOffsetList();
    }

    result->stackmaps = irGenerator.takeStackmaps();
    result->exceptionHandlers = irGenerator.takeExceptionHandlers();

    if (isOSREntry(compilationMode)) {
        int32_t checkSize = 0;
        bool needsOverflowCheck = false;
        irGenerator.computeStackCheckSize(needsOverflowCheck, checkSize);
        ASSERT(checkSize || !needsOverflowCheck);
        if (!needsOverflowCheck)
            checkSize = stackCheckNotNeeded;
        static_cast<OSREntryCallee*>(&callee)->setStackCheckSize(checkSize);
    }

    return result;
}

// Custom wasm ops. These are the ones too messy to do in wasm.json.

void OMGIRGenerator::emitChecksForModOrDiv(B3::Opcode operation, Value* left, Value* right)
{
    ASSERT(operation == Div || operation == Mod || operation == UDiv || operation == UMod);
    const B3::Type type = left->type();

    {
        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, Equal, origin(), right, constant(type, 0)));

        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::DivisionByZero);
        });
    }

    if (operation == Div) {
        int64_t min = type == Int32 ? std::numeric_limits<int32_t>::min() : std::numeric_limits<int64_t>::min();

        CheckValue* check = append<CheckValue>(m_proc, Check, origin(),
            append<Value>(m_proc, BitAnd, origin(),
                append<Value>(m_proc, Equal, origin(), left, constant(type, min)),
                append<Value>(m_proc, Equal, origin(), right, constant(type, -1))));

        check->setGenerator([=, this] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            this->emitExceptionCheck(jit, ExceptionType::IntegerOverflow);
        });
    }
}

auto OMGIRGenerator::addI32DivS(ExpressionType leftVar, ExpressionType rightVar, ExpressionType& result) -> PartialResult
{
    const B3::Opcode op = Div;
    Value* left = get(leftVar);
    Value* right = get(rightVar);
    emitChecksForModOrDiv(op, left, right);
    result = push<Value>(m_proc, op, origin(), left, right);
    return { };
}

auto OMGIRGenerator::addI32RemS(ExpressionType leftVar, ExpressionType rightVar, ExpressionType& result) -> PartialResult
{
    const B3::Opcode op = Mod;
    Value* left = get(leftVar);
    Value* right = get(rightVar);
    emitChecksForModOrDiv(op, left, right);
    result = push<Value>(m_proc, chill(op), origin(), left, right);
    return { };
}

auto OMGIRGenerator::addI32DivU(ExpressionType leftVar, ExpressionType rightVar, ExpressionType& result) -> PartialResult
{
    const B3::Opcode op = UDiv;
    Value* left = get(leftVar);
    Value* right = get(rightVar);
    emitChecksForModOrDiv(op, left, right);
    result = push<Value>(m_proc, op, origin(), left, right);
    return { };
}

auto OMGIRGenerator::addI32RemU(ExpressionType leftVar, ExpressionType rightVar, ExpressionType& result) -> PartialResult
{
    const B3::Opcode op = UMod;
    Value* left = get(leftVar);
    Value* right = get(rightVar);
    emitChecksForModOrDiv(op, left, right);
    result = push<Value>(m_proc, op, origin(), left, right);
    return { };
}

auto OMGIRGenerator::addI64DivS(ExpressionType leftVar, ExpressionType rightVar, ExpressionType& result) -> PartialResult
{
    const B3::Opcode op = Div;
    Value* left = get(leftVar);
    Value* right = get(rightVar);
    emitChecksForModOrDiv(op, left, right);
    result = push<Value>(m_proc, op, origin(), left, right);
    return { };
}

auto OMGIRGenerator::addI64RemS(ExpressionType leftVar, ExpressionType rightVar, ExpressionType& result) -> PartialResult
{
    const B3::Opcode op = Mod;
    Value* left = get(leftVar);
    Value* right = get(rightVar);
    emitChecksForModOrDiv(op, left, right);
    result = push<Value>(m_proc, chill(op), origin(), left, right);
    return { };
}

auto OMGIRGenerator::addI64DivU(ExpressionType leftVar, ExpressionType rightVar, ExpressionType& result) -> PartialResult
{
    const B3::Opcode op = UDiv;
    Value* left = get(leftVar);
    Value* right = get(rightVar);
    emitChecksForModOrDiv(op, left, right);
    result = push<Value>(m_proc, op, origin(), left, right);
    return { };
}

auto OMGIRGenerator::addI64RemU(ExpressionType leftVar, ExpressionType rightVar, ExpressionType& result) -> PartialResult
{
    const B3::Opcode op = UMod;
    Value* left = get(leftVar);
    Value* right = get(rightVar);
    emitChecksForModOrDiv(op, left, right);
    result = push<Value>(m_proc, op, origin(), left, right);
    return { };
}

auto OMGIRGenerator::addI32Ctz(ExpressionType argVar, ExpressionType& result) -> PartialResult
{
    Value* arg = get(argVar);
    PatchpointValue* patchpoint = append<PatchpointValue>(m_proc, Int32, origin());
    patchpoint->append(arg, ValueRep::SomeRegister);
    patchpoint->setGenerator([=] (CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.countTrailingZeros32(params[1].gpr(), params[0].gpr());
    });
    patchpoint->effects = Effects::none();
    result = push(patchpoint);
    return { };
}

auto OMGIRGenerator::addI64Ctz(ExpressionType argVar, ExpressionType& result) -> PartialResult
{
    Value* arg = get(argVar);
    PatchpointValue* patchpoint = append<PatchpointValue>(m_proc, Int64, origin());
    patchpoint->append(arg, ValueRep::SomeRegister);
    patchpoint->setGenerator([=] (CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.countTrailingZeros64(params[1].gpr(), params[0].gpr());
    });
    patchpoint->effects = Effects::none();
    result = push(patchpoint);
    return { };
}

auto OMGIRGenerator::addI32Popcnt(ExpressionType argVar, ExpressionType& result) -> PartialResult
{
    Value* arg = get(argVar);
    if (MacroAssembler::supportsCountPopulation()) {
        PatchpointValue* patchpoint = append<PatchpointValue>(m_proc, Int32, origin());
        patchpoint->append(arg, ValueRep::SomeRegister);
#if CPU(X86_64)
        patchpoint->setGenerator([=] (CCallHelpers& jit, const StackmapGenerationParams& params) {
            jit.countPopulation32(params[1].gpr(), params[0].gpr());
        });
#else
        patchpoint->numFPScratchRegisters = 1;
        patchpoint->setGenerator([=] (CCallHelpers& jit, const StackmapGenerationParams& params) {
            jit.countPopulation32(params[1].gpr(), params[0].gpr(), params.fpScratch(0));
        });
#endif
        patchpoint->effects = Effects::none();
        result = push(patchpoint);
        return { };
    }

    // Pure math function does not need to call emitPrepareWasmOperation.
    Value* funcAddress = append<ConstPtrValue>(m_proc, origin(), tagCFunction<OperationPtrTag>(operationPopcount32));
    result = push<CCallValue>(m_proc, Int32, origin(), Effects::none(), funcAddress, arg);
    return { };
}

auto OMGIRGenerator::addI64Popcnt(ExpressionType argVar, ExpressionType& result) -> PartialResult
{
    Value* arg = get(argVar);
    if (MacroAssembler::supportsCountPopulation()) {
        PatchpointValue* patchpoint = append<PatchpointValue>(m_proc, Int64, origin());
        patchpoint->append(arg, ValueRep::SomeRegister);
#if CPU(X86_64)
        patchpoint->setGenerator([=] (CCallHelpers& jit, const StackmapGenerationParams& params) {
            jit.countPopulation64(params[1].gpr(), params[0].gpr());
        });
#else
        patchpoint->numFPScratchRegisters = 1;
        patchpoint->setGenerator([=] (CCallHelpers& jit, const StackmapGenerationParams& params) {
            jit.countPopulation64(params[1].gpr(), params[0].gpr(), params.fpScratch(0));
        });
#endif
        patchpoint->effects = Effects::none();
        result = push(patchpoint);
        return { };
    }

    // Pure math function does not need to call emitPrepareWasmOperation.
    Value* funcAddress = append<ConstPtrValue>(m_proc, origin(), tagCFunction<OperationPtrTag>(operationPopcount64));
    result = push<CCallValue>(m_proc, Int64, origin(), Effects::none(), funcAddress, arg);
    return { };
}

auto OMGIRGenerator::addF64ConvertUI64(ExpressionType argVar, ExpressionType& result) -> PartialResult
{
    Value* arg = get(argVar);
    PatchpointValue* patchpoint = append<PatchpointValue>(m_proc, Double, origin());
    if (isX86())
        patchpoint->numGPScratchRegisters = 1;
    patchpoint->clobber(RegisterSetBuilder::macroClobberedGPRs());
    patchpoint->append(ConstrainedValue(arg, ValueRep::SomeRegister));
    patchpoint->setGenerator([=] (CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
#if CPU(X86_64)
        jit.convertUInt64ToDouble(params[1].gpr(), params[0].fpr(), params.gpScratch(0));
#else
        jit.convertUInt64ToDouble(params[1].gpr(), params[0].fpr());
#endif
    });
    patchpoint->effects = Effects::none();
    result = push(patchpoint);
    return { };
}

auto OMGIRGenerator::addF32ConvertUI64(ExpressionType argVar, ExpressionType& result) -> PartialResult
{
    Value* arg = get(argVar);
    PatchpointValue* patchpoint = append<PatchpointValue>(m_proc, Float, origin());
    if (isX86())
        patchpoint->numGPScratchRegisters = 1;
    patchpoint->clobber(RegisterSetBuilder::macroClobberedGPRs());
    patchpoint->append(ConstrainedValue(arg, ValueRep::SomeRegister));
    patchpoint->setGenerator([=] (CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
#if CPU(X86_64)
        jit.convertUInt64ToFloat(params[1].gpr(), params[0].fpr(), params.gpScratch(0));
#else
        jit.convertUInt64ToFloat(params[1].gpr(), params[0].fpr());
#endif
    });
    patchpoint->effects = Effects::none();
    result = push(patchpoint);
    return { };
}

auto OMGIRGenerator::addF64Nearest(ExpressionType argVar, ExpressionType& result) -> PartialResult
{
    Value* arg = get(argVar);
    PatchpointValue* patchpoint = append<PatchpointValue>(m_proc, Double, origin());
    patchpoint->append(arg, ValueRep::SomeRegister);
    patchpoint->setGenerator([=] (CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.roundTowardNearestIntDouble(params[1].fpr(), params[0].fpr());
    });
    patchpoint->effects = Effects::none();
    result = push(patchpoint);
    return { };
}

auto OMGIRGenerator::addF32Nearest(ExpressionType argVar, ExpressionType& result) -> PartialResult
{
    Value* arg = get(argVar);
    PatchpointValue* patchpoint = append<PatchpointValue>(m_proc, Float, origin());
    patchpoint->append(arg, ValueRep::SomeRegister);
    patchpoint->setGenerator([=] (CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.roundTowardNearestIntFloat(params[1].fpr(), params[0].fpr());
    });
    patchpoint->effects = Effects::none();
    result = push(patchpoint);
    return { };
}

auto OMGIRGenerator::addF64Trunc(ExpressionType argVar, ExpressionType& result) -> PartialResult
{
    Value* arg = get(argVar);
    PatchpointValue* patchpoint = append<PatchpointValue>(m_proc, Double, origin());
    patchpoint->append(arg, ValueRep::SomeRegister);
    patchpoint->setGenerator([=] (CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.roundTowardZeroDouble(params[1].fpr(), params[0].fpr());
    });
    patchpoint->effects = Effects::none();
    result = push(patchpoint);
    return { };
}

auto OMGIRGenerator::addF32Trunc(ExpressionType argVar, ExpressionType& result) -> PartialResult
{
    Value* arg = get(argVar);
    PatchpointValue* patchpoint = append<PatchpointValue>(m_proc, Float, origin());
    patchpoint->append(arg, ValueRep::SomeRegister);
    patchpoint->setGenerator([=] (CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.roundTowardZeroFloat(params[1].fpr(), params[0].fpr());
    });
    patchpoint->effects = Effects::none();
    result = push(patchpoint);
    return { };
}

auto OMGIRGenerator::addI32TruncSF64(ExpressionType argVar, ExpressionType& result) -> PartialResult
{
    Value* arg = get(argVar);
    Value* max = constant(Double, bitwise_cast<uint64_t>(-static_cast<double>(std::numeric_limits<int32_t>::min())));
    Value* min = constant(Double, bitwise_cast<uint64_t>(static_cast<double>(std::numeric_limits<int32_t>::min()) - 1.0));
    Value* outOfBounds = append<Value>(m_proc, BitAnd, origin(),
        append<Value>(m_proc, LessThan, origin(), arg, max),
        append<Value>(m_proc, GreaterThan, origin(), arg, min));
    outOfBounds = append<Value>(m_proc, Equal, origin(), outOfBounds, constant(Int32, 0));
    CheckValue* trap = append<CheckValue>(m_proc, Check, origin(), outOfBounds);
    trap->setGenerator([=, this] (CCallHelpers& jit, const StackmapGenerationParams&) {
        this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsTrunc);
    });
    PatchpointValue* patchpoint = append<PatchpointValue>(m_proc, Int32, origin());
    patchpoint->append(arg, ValueRep::SomeRegister);
    patchpoint->setGenerator([=] (CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.truncateDoubleToInt32(params[1].fpr(), params[0].gpr());
    });
    patchpoint->effects = Effects::none();
    result = push(patchpoint);
    return { };
}

auto OMGIRGenerator::addI32TruncSF32(ExpressionType argVar, ExpressionType& result) -> PartialResult
{
    Value* arg = get(argVar);
    Value* max = constant(Float, bitwise_cast<uint32_t>(-static_cast<float>(std::numeric_limits<int32_t>::min())));
    Value* min = constant(Float, bitwise_cast<uint32_t>(static_cast<float>(std::numeric_limits<int32_t>::min())));
    Value* outOfBounds = append<Value>(m_proc, BitAnd, origin(),
        append<Value>(m_proc, LessThan, origin(), arg, max),
        append<Value>(m_proc, GreaterEqual, origin(), arg, min));
    outOfBounds = append<Value>(m_proc, Equal, origin(), outOfBounds, constant(Int32, 0));
    CheckValue* trap = append<CheckValue>(m_proc, Check, origin(), outOfBounds);
    trap->setGenerator([=, this] (CCallHelpers& jit, const StackmapGenerationParams&) {
        this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsTrunc);
    });
    PatchpointValue* patchpoint = append<PatchpointValue>(m_proc, Int32, origin());
    patchpoint->append(arg, ValueRep::SomeRegister);
    patchpoint->setGenerator([=] (CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.truncateFloatToInt32(params[1].fpr(), params[0].gpr());
    });
    patchpoint->effects = Effects::none();
    result = push(patchpoint);
    return { };
}


auto OMGIRGenerator::addI32TruncUF64(ExpressionType argVar, ExpressionType& result) -> PartialResult
{
    Value* arg = get(argVar);
    Value* max = constant(Double, bitwise_cast<uint64_t>(static_cast<double>(std::numeric_limits<int32_t>::min()) * -2.0));
    Value* min = constant(Double, bitwise_cast<uint64_t>(-1.0));
    Value* outOfBounds = append<Value>(m_proc, BitAnd, origin(),
        append<Value>(m_proc, LessThan, origin(), arg, max),
        append<Value>(m_proc, GreaterThan, origin(), arg, min));
    outOfBounds = append<Value>(m_proc, Equal, origin(), outOfBounds, constant(Int32, 0));
    CheckValue* trap = append<CheckValue>(m_proc, Check, origin(), outOfBounds);
    trap->setGenerator([=, this] (CCallHelpers& jit, const StackmapGenerationParams&) {
        this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsTrunc);
    });
    PatchpointValue* patchpoint = append<PatchpointValue>(m_proc, Int32, origin());
    patchpoint->append(arg, ValueRep::SomeRegister);
    patchpoint->setGenerator([=] (CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.truncateDoubleToUint32(params[1].fpr(), params[0].gpr());
    });
    patchpoint->effects = Effects::none();
    result = push(patchpoint);
    return { };
}

auto OMGIRGenerator::addI32TruncUF32(ExpressionType argVar, ExpressionType& result) -> PartialResult
{
    Value* arg = get(argVar);
    Value* max = constant(Float, bitwise_cast<uint32_t>(static_cast<float>(std::numeric_limits<int32_t>::min()) * static_cast<float>(-2.0)));
    Value* min = constant(Float, bitwise_cast<uint32_t>(static_cast<float>(-1.0)));
    Value* outOfBounds = append<Value>(m_proc, BitAnd, origin(),
        append<Value>(m_proc, LessThan, origin(), arg, max),
        append<Value>(m_proc, GreaterThan, origin(), arg, min));
    outOfBounds = append<Value>(m_proc, Equal, origin(), outOfBounds, constant(Int32, 0));
    CheckValue* trap = append<CheckValue>(m_proc, Check, origin(), outOfBounds);
    trap->setGenerator([=, this] (CCallHelpers& jit, const StackmapGenerationParams&) {
        this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsTrunc);
    });
    PatchpointValue* patchpoint = append<PatchpointValue>(m_proc, Int32, origin());
    patchpoint->append(arg, ValueRep::SomeRegister);
    patchpoint->setGenerator([=] (CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.truncateFloatToUint32(params[1].fpr(), params[0].gpr());
    });
    patchpoint->effects = Effects::none();
    result = push(patchpoint);
    return { };
}

auto OMGIRGenerator::addI64TruncSF64(ExpressionType argVar, ExpressionType& result) -> PartialResult
{
    Value* arg = get(argVar);
    Value* max = constant(Double, bitwise_cast<uint64_t>(-static_cast<double>(std::numeric_limits<int64_t>::min())));
    Value* min = constant(Double, bitwise_cast<uint64_t>(static_cast<double>(std::numeric_limits<int64_t>::min())));
    Value* outOfBounds = append<Value>(m_proc, BitAnd, origin(),
        append<Value>(m_proc, LessThan, origin(), arg, max),
        append<Value>(m_proc, GreaterEqual, origin(), arg, min));
    outOfBounds = append<Value>(m_proc, Equal, origin(), outOfBounds, constant(Int32, 0));
    CheckValue* trap = append<CheckValue>(m_proc, Check, origin(), outOfBounds);
    trap->setGenerator([=, this] (CCallHelpers& jit, const StackmapGenerationParams&) {
        this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsTrunc);
    });
    PatchpointValue* patchpoint = append<PatchpointValue>(m_proc, Int64, origin());
    patchpoint->append(arg, ValueRep::SomeRegister);
    patchpoint->setGenerator([=] (CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.truncateDoubleToInt64(params[1].fpr(), params[0].gpr());
    });
    patchpoint->effects = Effects::none();
    result = push(patchpoint);
    return { };
}

auto OMGIRGenerator::addI64TruncUF64(ExpressionType argVar, ExpressionType& result) -> PartialResult
{
    Value* arg = get(argVar);
    Value* max = constant(Double, bitwise_cast<uint64_t>(static_cast<double>(std::numeric_limits<int64_t>::min()) * -2.0));
    Value* min = constant(Double, bitwise_cast<uint64_t>(-1.0));
    Value* outOfBounds = append<Value>(m_proc, BitAnd, origin(),
        append<Value>(m_proc, LessThan, origin(), arg, max),
        append<Value>(m_proc, GreaterThan, origin(), arg, min));
    outOfBounds = append<Value>(m_proc, Equal, origin(), outOfBounds, constant(Int32, 0));
    CheckValue* trap = append<CheckValue>(m_proc, Check, origin(), outOfBounds);
    trap->setGenerator([=, this] (CCallHelpers& jit, const StackmapGenerationParams&) {
        this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsTrunc);
    });

    Value* signBitConstant;
    if (isX86()) {
        // Since x86 doesn't have an instruction to convert floating points to unsigned integers, we at least try to do the smart thing if
        // the numbers are would be positive anyway as a signed integer. Since we cannot materialize constants into fprs we have b3 do it
        // so we can pool them if needed.
        signBitConstant = constant(Double, bitwise_cast<uint64_t>(static_cast<double>(std::numeric_limits<uint64_t>::max() - std::numeric_limits<int64_t>::max())));
    }
    PatchpointValue* patchpoint = append<PatchpointValue>(m_proc, Int64, origin());
    patchpoint->append(arg, ValueRep::SomeRegister);
    if (isX86()) {
        patchpoint->append(signBitConstant, ValueRep::SomeRegister);
        patchpoint->numFPScratchRegisters = 1;
    }
    patchpoint->clobber(RegisterSetBuilder::macroClobberedGPRs());
    patchpoint->setGenerator([=] (CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        FPRReg scratch = InvalidFPRReg;
        FPRReg constant = InvalidFPRReg;
        if (isX86()) {
            scratch = params.fpScratch(0);
            constant = params[2].fpr();
        }
        jit.truncateDoubleToUint64(params[1].fpr(), params[0].gpr(), scratch, constant);
    });
    patchpoint->effects = Effects::none();
    result = push(patchpoint);
    return { };
}

auto OMGIRGenerator::addI64TruncSF32(ExpressionType argVar, ExpressionType& result) -> PartialResult
{
    Value* arg = get(argVar);
    Value* max = constant(Float, bitwise_cast<uint32_t>(-static_cast<float>(std::numeric_limits<int64_t>::min())));
    Value* min = constant(Float, bitwise_cast<uint32_t>(static_cast<float>(std::numeric_limits<int64_t>::min())));
    Value* outOfBounds = append<Value>(m_proc, BitAnd, origin(),
        append<Value>(m_proc, LessThan, origin(), arg, max),
        append<Value>(m_proc, GreaterEqual, origin(), arg, min));
    outOfBounds = append<Value>(m_proc, Equal, origin(), outOfBounds, constant(Int32, 0));
    CheckValue* trap = append<CheckValue>(m_proc, Check, origin(), outOfBounds);
    trap->setGenerator([=, this] (CCallHelpers& jit, const StackmapGenerationParams&) {
        this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsTrunc);
    });
    PatchpointValue* patchpoint = append<PatchpointValue>(m_proc, Int64, origin());
    patchpoint->append(arg, ValueRep::SomeRegister);
    patchpoint->setGenerator([=] (CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.truncateFloatToInt64(params[1].fpr(), params[0].gpr());
    });
    patchpoint->effects = Effects::none();
    result = push(patchpoint);
    return { };
}

auto OMGIRGenerator::addI64TruncUF32(ExpressionType argVar, ExpressionType& result) -> PartialResult
{
    Value* arg = get(argVar);
    Value* max = constant(Float, bitwise_cast<uint32_t>(static_cast<float>(std::numeric_limits<int64_t>::min()) * static_cast<float>(-2.0)));
    Value* min = constant(Float, bitwise_cast<uint32_t>(static_cast<float>(-1.0)));
    Value* outOfBounds = append<Value>(m_proc, BitAnd, origin(),
        append<Value>(m_proc, LessThan, origin(), arg, max),
        append<Value>(m_proc, GreaterThan, origin(), arg, min));
    outOfBounds = append<Value>(m_proc, Equal, origin(), outOfBounds, constant(Int32, 0));
    CheckValue* trap = append<CheckValue>(m_proc, Check, origin(), outOfBounds);
    trap->setGenerator([=, this] (CCallHelpers& jit, const StackmapGenerationParams&) {
        this->emitExceptionCheck(jit, ExceptionType::OutOfBoundsTrunc);
    });

    Value* signBitConstant;
    if (isX86()) {
        // Since x86 doesn't have an instruction to convert floating points to unsigned integers, we at least try to do the smart thing if
        // the numbers would be positive anyway as a signed integer. Since we cannot materialize constants into fprs we have b3 do it
        // so we can pool them if needed.
        signBitConstant = constant(Float, bitwise_cast<uint32_t>(static_cast<float>(std::numeric_limits<uint64_t>::max() - std::numeric_limits<int64_t>::max())));
    }
    PatchpointValue* patchpoint = append<PatchpointValue>(m_proc, Int64, origin());
    patchpoint->append(arg, ValueRep::SomeRegister);
    if (isX86()) {
        patchpoint->append(signBitConstant, ValueRep::SomeRegister);
        patchpoint->numFPScratchRegisters = 1;
    }
    patchpoint->clobber(RegisterSetBuilder::macroClobberedGPRs());
    patchpoint->setGenerator([=] (CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        FPRReg scratch = InvalidFPRReg;
        FPRReg constant = InvalidFPRReg;
        if (isX86()) {
            scratch = params.fpScratch(0);
            constant = params[2].fpr();
        }
        jit.truncateFloatToUint64(params[1].fpr(), params[0].gpr(), scratch, constant);
    });
    patchpoint->effects = Effects::none();
    result = push(patchpoint);
    return { };
}

} } // namespace JSC::Wasm

#include "WasmOMGIRGeneratorInlines.h"

#endif // USE(JSVALUE64)
#endif // ENABLE(WEBASSEMBLY_OMGJIT)
