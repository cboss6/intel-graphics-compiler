/*===================== begin_copyright_notice ==================================

Copyright (c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


======================= end_copyright_notice ==================================*/

#include "Compiler/Optimizer/OpenCLPasses/WIFuncs/WIFuncResolution.hpp"
#include "Compiler/Optimizer/OpenCLPasses/WIFuncs/WIFuncsAnalysis.hpp"
#include "Compiler/IGCPassSupport.h"
#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include "common/LLVMWarningsPop.hpp"
#include "Probe/Assertion.h"
#include <llvmWrapper/Support/Alignment.h>
#include <llvmWrapper/IR/DerivedTypes.h>

using namespace llvm;
using namespace IGC;

// Register pass to igc-opt
#define PASS_FLAG "igc-wi-func-resolution"
#define PASS_DESCRIPTION "Resolves work item functions"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(WIFuncResolution, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_DEPENDENCY(MetaDataUtilsWrapper)
IGC_INITIALIZE_PASS_DEPENDENCY(CodeGenContextWrapper)
IGC_INITIALIZE_PASS_END(WIFuncResolution, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

char WIFuncResolution::ID = 0;

WIFuncResolution::WIFuncResolution() : FunctionPass(ID), m_implicitArgs()
{
    initializeWIFuncResolutionPass(*PassRegistry::getPassRegistry());
}

Constant* WIFuncResolution::getKnownWorkGroupSize(
    IGCMD::MetaDataUtils* MDUtils, llvm::Function& F) const
{
    auto finfo = MDUtils->findFunctionsInfoItem(&F);
    if (finfo == MDUtils->end_FunctionsInfo())
        return nullptr;

    auto& FI = finfo->second;
    if (FI->getThreadGroupSize()->hasValue())
    {
        uint32_t Dims[] =
        {
            (uint32_t)FI->getThreadGroupSize()->getXDim(),
            (uint32_t)FI->getThreadGroupSize()->getYDim(),
            (uint32_t)FI->getThreadGroupSize()->getZDim(),
        };
        return ConstantDataVector::get(F.getContext(), Dims);
    }

    return nullptr;
}

bool WIFuncResolution::runOnFunction(Function& F)
{
    m_changed = false;
    auto* MDUtils = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();
    m_implicitArgs = ImplicitArgs(F, getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils());
    visit(F);

    /// If the work group size is known at compile time, emit it as a
    /// literal rather than reading from the payload.
    if (Constant * KnownWorkGroupSize = getKnownWorkGroupSize(MDUtils, F))
    {
        if (auto * Arg = m_implicitArgs.getImplicitArg(F, ImplicitArg::ENQUEUED_LOCAL_WORK_SIZE))
            Arg->replaceAllUsesWith(KnownWorkGroupSize);
    }

    return m_changed;
}

void WIFuncResolution::visitCallInst(CallInst& CI)
{
    if (!CI.getCalledFunction())
    {
        return;
    }

    Value* wiRes = nullptr;

    // Add appropriate sequence and handle out of range where needed
    StringRef funcName = CI.getCalledFunction()->getName();

    if (funcName.equals(WIFuncsAnalysis::GET_LOCAL_ID_X))
    {
        wiRes = getLocalId(CI, ImplicitArg::LOCAL_ID_X);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_LOCAL_ID_Y))
    {
        wiRes = getLocalId(CI, ImplicitArg::LOCAL_ID_Y);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_LOCAL_ID_Z))
    {
        wiRes = getLocalId(CI, ImplicitArg::LOCAL_ID_Z);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_GROUP_ID))
    {
        wiRes = getGroupId(CI);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_GLOBAL_SIZE))
    {
        wiRes = getGlobalSize(CI);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_LOCAL_SIZE))
    {
        wiRes = getLocalSize(CI);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_ENQUEUED_LOCAL_SIZE)) {
        wiRes = getEnqueuedLocalSize(CI);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_GLOBAL_OFFSET))
    {
        wiRes = getGlobalOffset(CI);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_WORK_DIM))
    {
        wiRes = getWorkDim(CI);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_NUM_GROUPS))
    {
        wiRes = getNumGroups(CI);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_STAGE_IN_GRID_ORIGIN))
    {
        wiRes = getStageInGridOrigin(CI);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_STAGE_IN_GRID_SIZE))
    {
        wiRes = getStageInGridSize(CI);
    }
    else if (funcName.equals(WIFuncsAnalysis::GET_SYNC_BUFFER))
    {
        wiRes = getSyncBufferPtr(CI);
    }
    else
    {
        // Non WI function, do nothing
        return;
    }

    // Handle size_t return type for 64 bits
    if (wiRes->getType()->getScalarSizeInBits() < CI.getType()->getScalarSizeInBits())
    {
        CastInst* pCast = CastInst::Create(Instruction::ZExt, wiRes, IntegerType::get(CI.getContext(), CI.getType()->getScalarSizeInBits()), wiRes->getName(), &CI);
        updateDebugLoc(&CI, pCast);
        wiRes = pCast;
    }

    // Replace original WI call instruction by the result of the appropriate sequence
    CI.replaceAllUsesWith(wiRes);
    CI.eraseFromParent();

    m_changed = true;
}

/************************************************************************************************

R0:

 -----------------------------------------------------------------------------------------------
| Local mem | Group     | Barrier ID| Sampler   | Binding   | Scratch   | Group     | Group     |
| mem index/| number    | /Interface| state     | table     | space     | number    | number    |
| URB handle| X         | descriptor| pointer   | pointer   | pointer   | Y         | Z         |
|           | 32bit     | offset    |           |           |           | 32bit     | 32bit     |
 -----------------------------------------------------------------------------------------------
 <low>                                                                                     <high>


 PayloadHeader:

-----------------------------------------------------------------------------------------------
| Global    | Global    | Global    | Local     | Local     | Local     | Reserved  | Num       |
| offset    | offset    | offset    | size      | size      | size      |           | HW        |
| X         | Y         | Z         | X         | Y         | Z         |           | Threads   |
| 32bit     | 32bit     | 32bit     | 32bit     | 32bit     | 32bit     |           | 32bit     |
 -----------------------------------------------------------------------------------------------
 <low>                                                                                     <high>

*************************************************************************************************/

// Structure of side buffer generated by NEO:
// struct arg_s {
//    uint64_t global_size_x;
//    uint64_t global_size_y;
//    uint64_t global_size_z;
//    uint64_t printf_buffer_ptr; / uint32_t bindless offset
//    uint64_t global_offset_x;
//    uint64_t global_offset_y;
//    uint64_t global_offset_z;
//    uint32_t local_size_x;
//    uint32_t local_size_y;
//    uint32_t local_size_z;
//    uint32_t group_count_x;
//    uint32_t group_count_ y;
//    uint32_t group_count_z;
//    local_id_s* local_id_buffer; // A64 ptr
//    uint8_t structSize;
//    uint8_t num_work_dim;
//};

//struct local_id_s {
//    uint16_t lx[simd - width];
//    uint16_t ly[simd - width];
//    uint16_t lz[simd - width];
//};


class GLOBAL_STATE_FIELD_OFFSETS
{
public:
    // This class holds offsets of various fields in side buffer
    static const uint32_t GLOBAL_SIZES = 0;
    static const uint32_t GLOBAL_SIZE_X = GLOBAL_SIZES;
    static const uint32_t GLOBAL_SIZE_Y = GLOBAL_SIZE_X + sizeof(uint64_t);
    static const uint32_t GLOBAL_SIZE_Z = GLOBAL_SIZE_Y + sizeof(uint64_t);

    static const uint32_t PRINTF_BUFFER = GLOBAL_SIZE_Z + sizeof(uint64_t);

    static const uint32_t GLOBAL_OFFSETS = PRINTF_BUFFER + sizeof(uint64_t);
    static const uint32_t GLOBAL_OFFSET_X = GLOBAL_OFFSETS;
    static const uint32_t GLOBAL_OFFSET_Y = GLOBAL_OFFSET_X + sizeof(uint64_t);
    static const uint32_t GLOBAL_OFFSET_Z = GLOBAL_OFFSET_Y + sizeof(uint64_t);

    static const uint32_t LOCAL_SIZES = GLOBAL_OFFSET_Z + sizeof(uint64_t);
    static const uint32_t LOCAL_SIZE_X = LOCAL_SIZES;
    static const uint32_t LOCAL_SIZE_Y = LOCAL_SIZE_X + sizeof(uint32_t);
    static const uint32_t LOCAL_SIZE_Z = LOCAL_SIZE_Y + sizeof(uint32_t);

    static const uint32_t GROUP_COUNTS = LOCAL_SIZE_Z + sizeof(uint32_t);
    static const uint32_t GROUP_COUNT_X = GROUP_COUNTS;
    static const uint32_t GROUP_COUNT_Y = GROUP_COUNT_X + sizeof(uint32_t);
    static const uint32_t GROUP_COUNT_Z = GROUP_COUNT_Y + sizeof(uint32_t);

    static const uint32_t LOCAL_IDS = GROUP_COUNT_Z + sizeof(uint32_t);

    static const uint32_t NUM_WORK_DIM = LOCAL_IDS + sizeof(uint64_t);

    static const uint32_t STRUCT_SIZE = NUM_WORK_DIM + sizeof(uint8_t);
};

static bool hasStackCallAttr(const llvm::Function& F)
{
    return F.hasFnAttribute("visaStackCall");
}

static Value* BuildLoadInst(CallInst& CI, unsigned int Offset, unsigned int Size, Type* DataType)
{
    // This function computes 4-byte aligned address that includes Offset.
    // Then it loads at least Size number of bytes of type <i8 x Size>.
    // Next, it extracts Size elements to a new vector.
    // It bitcasts extracted <i8 x Size> to DataType.
    // It returns a value of type DataType.
    unsigned int AlignedOffset = (Offset / 4) * 4;
    unsigned int LoadSize = Size + (Offset - AlignedOffset);

    llvm::IRBuilder<> Builder(&CI);
    auto F = CI.getFunction();
    auto Int32Ptr = PointerType::get(Type::getInt32Ty(F->getParent()->getContext()), ADDRESS_SPACE_A32);
    auto ElemType = Type::getInt8Ty(F->getParent()->getContext());
    auto LoadType = IGCLLVM::FixedVectorType::get(ElemType, LoadSize);
    auto PtrType = PointerType::get(LoadType, ADDRESS_SPACE_A32);
    auto IntToPtr = Builder.CreateIntToPtr(Builder.getIntN(F->getParent()->getDataLayout().getPointerSizeInBits(ADDRESS_SPACE_A32), AlignedOffset), Int32Ptr);
    auto BitCast = Builder.CreateBitCast(IntToPtr, PtrType);
    auto LoadInst = Builder.CreateLoad(BitCast);
    LoadInst->setAlignment(IGCLLVM::getCorrectAlign(4));

    if (Offset != AlignedOffset)
    {
        Value* NewVector = UndefValue::get(IGCLLVM::FixedVectorType::get(ElemType, Size));
        for (unsigned int I = Offset; I != (Offset + Size); ++I)
        {
            auto Elem = Builder.CreateExtractElement(LoadInst, I - AlignedOffset);
            NewVector = Builder.CreateInsertElement(NewVector, Elem, (uint64_t)I - (uint64_t)Offset);
        }
        auto Result = Builder.CreateBitCast(NewVector, DataType);
        return Result;
    }
    auto Result = Builder.CreateBitCast(LoadInst, DataType);
    return Result;
}

Value* WIFuncResolution::getLocalId(CallInst& CI, ImplicitArg::ArgType argType)
{
    // Receives:
    // call i32 @__builtin_IB_get_local_id_x()

    // Creates:
    // %localIdX

    Value* V = nullptr;
    auto F = CI.getFunction();
    if (hasStackCallAttr(*F))
    {
    }
    else
    {
        Argument* localId = getImplicitArg(CI, argType);
        V = localId;
    }

    return V;
}

Value* WIFuncResolution::getGroupId(CallInst& CI)
{
    // Receives:
    // call i32 @__builtin_IB_get_group_id(i32 %dim)

    // Creates:
    // %cmpDim = icmp eq i32 %dim, 0
    // %tmpOffsetR0 = select i1 %cmpDim, i32 1, i32 5
    // %offsetR0 = add i32 %dim, %tmpOffsetR0
    // %groupId = extractelement <8 x i32> %r0, i32 %offsetR0

    // The cmp select insts are present because:
    // if dim = 0 then we need to access R0.1
    // if dim = 1 then we need to access R0.6
    // if dim = 2 then we need to access R0.7

    Value* V = nullptr;
    auto F = CI.getFunction();
    if (hasStackCallAttr(*F))
    {
        auto Ctx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
        llvm::IRBuilder<> Builder(&CI);
        Type* Int32Ty = Type::getInt32Ty(F->getParent()->getContext());
        VectorType* Tys = IGCLLVM::FixedVectorType::get(Int32Ty, Ctx->platform.getGRFSize() / SIZE_DWORD);
        Function* R0Dcl = GenISAIntrinsic::getDeclaration(F->getParent(), GenISAIntrinsic::ID::GenISA_getR0, Tys);
        auto IntCall = Builder.CreateCall(R0Dcl);
        V = IntCall;
    }
    else
    {
        Argument* arg = getImplicitArg(CI, ImplicitArg::R0);
        V = arg;
    }

    Value* dim = CI.getArgOperand(0);
    Instruction* cmpDim = CmpInst::Create(Instruction::ICmp, CmpInst::ICMP_EQ, dim, ConstantInt::get(Type::getInt32Ty(CI.getContext()), 0), "cmpDim", &CI);
    Instruction* offsetR0 = SelectInst::Create(cmpDim, ConstantInt::get(Type::getInt32Ty(CI.getContext()), 1), ConstantInt::get(Type::getInt32Ty(CI.getContext()), 5), "tmpOffsetR0", &CI);
    Instruction* index = BinaryOperator::CreateAdd(dim, offsetR0, "offsetR0", &CI);
    Instruction* groupId = ExtractElementInst::Create(V, index, "groupId", &CI);
    updateDebugLoc(&CI, cmpDim);
    updateDebugLoc(&CI, offsetR0);
    updateDebugLoc(&CI, index);
    updateDebugLoc(&CI, groupId);

    return groupId;
}

Value* WIFuncResolution::getGlobalSize(CallInst& CI)
{
    // Receives:
    // call i32 @__builtin_IB_get_global_size(i32 %dim)

    // Creates:
    // %globalSize1 = extractelement <3 x i32> %globalSize, i32 %dim

    Value* V = nullptr;
    auto F = CI.getFunction();
    if (hasStackCallAttr(*F))
    {
        llvm::IRBuilder<> Builder(&CI);
        auto ElemTypeQ = Type::getInt64Ty(F->getParent()->getContext());
        auto VecTyQ = IGCLLVM::FixedVectorType::get(ElemTypeQ, 3);
        unsigned int Offset = GLOBAL_STATE_FIELD_OFFSETS::GLOBAL_SIZE_X;
        unsigned int Size = sizeof(uint64_t) * 3;
        auto LoadInst = BuildLoadInst(CI, Offset, Size, VecTyQ);
        auto ElemType = CI.getType();
        Value* Undef = UndefValue::get(IGCLLVM::FixedVectorType::get(ElemType, 3));
        for (unsigned int I = 0; I != 3; ++I)
        {
            // Extract each dimension, truncate to i32, then insert in new vector
            auto Elem = Builder.CreateExtractElement(LoadInst, (uint64_t)I);
            auto TruncElem = Builder.CreateTrunc(Elem, ElemType);
            Undef = Builder.CreateInsertElement(Undef, TruncElem, (uint64_t)I);
        }
        V = Undef;
    }
    else
    {
        Argument* arg = getImplicitArg(CI, ImplicitArg::GLOBAL_SIZE);
        V = arg;
    }

    Value* dim = CI.getArgOperand(0);
    Instruction* globalSize = ExtractElementInst::Create(V, dim, "globalSize", &CI);
    updateDebugLoc(&CI, globalSize);

    return globalSize;
}

Value* WIFuncResolution::getLocalSize(CallInst& CI)
{
    // Receives:
    // call i32 @__builtin_IB_get_local_size(i32 %dim)

    // Creates:
    // %localSize = extractelement <3 x i32> %localSize, i32 %dim

    Value* V = nullptr;
    auto F = CI.getFunction();
    if (hasStackCallAttr(*F))
    {
        llvm::IRBuilder<> Builder(&CI);
        auto ElemTypeD = Type::getInt32Ty(F->getParent()->getContext());
        auto VecTyD = IGCLLVM::FixedVectorType::get(ElemTypeD, 3);
        unsigned int Offset = GLOBAL_STATE_FIELD_OFFSETS::LOCAL_SIZE_X;
        unsigned int Size = sizeof(uint32_t) * 3;
        auto LoadInst = BuildLoadInst(CI, Offset, Size, VecTyD);
        V = LoadInst;
    }
    else
    {
        Argument* arg = getImplicitArg(CI, ImplicitArg::LOCAL_SIZE);
        V = arg;
    }

    Value* dim = CI.getArgOperand(0);
    Instruction* localSize = ExtractElementInst::Create(V, dim, "localSize", &CI);
    updateDebugLoc(&CI, localSize);

    return localSize;
}

Value* WIFuncResolution::getEnqueuedLocalSize(CallInst& CI) {
    // Receives:
    // call i32 @__builtin_IB_get_enqueued_local_size(i32 %dim)

    // Creates:
    // %enqueuedLocalSize1 = extractelement <3 x i32> %enqueuedLocalSize, %dim

    Value* V = nullptr;
    auto F = CI.getFunction();
    if (hasStackCallAttr(*F))
    {
        // Assume that enqueued local size is same as local size
        llvm::IRBuilder<> Builder(&CI);
        auto ElemTypeD = Type::getInt32Ty(F->getParent()->getContext());
        auto VecTyD = IGCLLVM::FixedVectorType::get(ElemTypeD, 3);
        unsigned int Offset = GLOBAL_STATE_FIELD_OFFSETS::LOCAL_SIZE_X;
        unsigned int Size = sizeof(uint32_t) * 3;
        auto LoadInst = BuildLoadInst(CI, Offset, Size, VecTyD);
        V = LoadInst;
    }
    else
    {
        Argument* arg = getImplicitArg(CI, ImplicitArg::ENQUEUED_LOCAL_WORK_SIZE);
        V = arg;
    }

    Value* dim = CI.getArgOperand(0);
    Instruction* enqueuedLocalSize = ExtractElementInst::Create(V, dim, "enqueuedLocalSize", &CI);
    updateDebugLoc(&CI, enqueuedLocalSize);

    return enqueuedLocalSize;
}

Value* WIFuncResolution::getGlobalOffset(CallInst& CI)
{
    // Receives:
    // call i32 @__builtin_IB_get_global_offset(i32 %dim)

    // Creates:
    // %globalOffset = extractelement <8 x i32> %payloadHeader, i32 %dim

    Value* V = nullptr;
    auto F = CI.getFunction();
    if (hasStackCallAttr(*F))
    {
        llvm::IRBuilder<> Builder(&CI);
        auto ElemTypeQ = Type::getInt64Ty(F->getParent()->getContext());
        auto VecTyQ = IGCLLVM::FixedVectorType::get(ElemTypeQ, 3);
        unsigned int Offset = GLOBAL_STATE_FIELD_OFFSETS::GLOBAL_OFFSET_X;
        unsigned int Size = sizeof(uint64_t) * 3;
        auto LoadInst = BuildLoadInst(CI, Offset, Size, VecTyQ);
        auto ElemType = CI.getType();
        Value* Undef = UndefValue::get(IGCLLVM::FixedVectorType::get(ElemType, 3));
        for (unsigned int I = 0; I != 3; ++I)
        {
            // Extract each dimension, truncate to i32, then insert in new vector
            auto Elem = Builder.CreateExtractElement(LoadInst, (uint64_t)I);
            auto TruncElem = Builder.CreateTrunc(Elem, ElemType);
            Undef = Builder.CreateInsertElement(Undef, TruncElem, (uint64_t)I);
        }
        V = Undef;
    }
    else
    {
        Argument* arg = getImplicitArg(CI, ImplicitArg::PAYLOAD_HEADER);
        V = arg;
    }

    Value* dim = CI.getArgOperand(0);
    auto globalOffset = ExtractElementInst::Create(V, dim, "globalOffset", &CI);
    updateDebugLoc(&CI, cast<Instruction>(globalOffset));

    return globalOffset;
}

Value* WIFuncResolution::getWorkDim(CallInst& CI)
{
    // Receives:
    // call i32 @__builtin_IB_get_work_dim()

    // Creates:
    // %workDim

    Value* V = nullptr;
    auto F = CI.getFunction();
    if (hasStackCallAttr(*F))
    {
        llvm::IRBuilder<> Builder(&CI);
        unsigned int Offset = GLOBAL_STATE_FIELD_OFFSETS::NUM_WORK_DIM;
        unsigned int Size = 4;
        auto TypeUD = Type::getInt32Ty(F->getParent()->getContext());
        auto LoadInst = BuildLoadInst(CI, Offset, Size, TypeUD);
        V = LoadInst;
    }
    else
    {
        Argument* workDim = getImplicitArg(CI, ImplicitArg::WORK_DIM);
        V = workDim;
    }

    return V;
}

Value* WIFuncResolution::getNumGroups(CallInst& CI)
{
    // Receives:
    // call i32 @__builtin_IB_get_num_groups(i32 %dim)

    // Creates:
    // %numGroups1 = extractelement <3 x i32> %numGroups, i32 %dim

    Value* V = nullptr;
    auto F = CI.getFunction();
    if (hasStackCallAttr(*F))
    {
        llvm::IRBuilder<> Builder(&CI);
        auto ElemTypeUD = Type::getInt32Ty(F->getParent()->getContext());
        auto VecTyUD = IGCLLVM::FixedVectorType::get(ElemTypeUD, 3);
        unsigned int Offset = GLOBAL_STATE_FIELD_OFFSETS::GROUP_COUNT_X;
        unsigned int Size = sizeof(uint32_t) * 3;
        auto LoadInst = BuildLoadInst(CI, Offset, Size, VecTyUD);
        V = LoadInst;
    }
    else
    {
        Argument* arg = getImplicitArg(CI, ImplicitArg::NUM_GROUPS);
        V = arg;
    }

    Value* dim = CI.getArgOperand(0);
    Instruction* numGroups = ExtractElementInst::Create(V, dim, "numGroups", &CI);
    updateDebugLoc(&CI, numGroups);

    return numGroups;
}

Value* WIFuncResolution::getStageInGridOrigin(CallInst& CI)
{
    // Receives:
    // call i32 @__builtin_IB_get_grid_origin(i32 %dim)

    // Creates:
    // %grid_origin1 = extractelement <3 x i32> %globalSize, i32 %dim

    Argument* arg = getImplicitArg(CI, ImplicitArg::STAGE_IN_GRID_ORIGIN);

    Value* dim = CI.getArgOperand(0);
    Instruction* globalSize = ExtractElementInst::Create(arg, dim, "grid_origin", &CI);
    updateDebugLoc(&CI, globalSize);

    return globalSize;
}

Value* WIFuncResolution::getStageInGridSize(CallInst& CI)
{
    // Receives:
    // call i32 @__builtin_IB_get_grid_size(i32 %dim)

    // Creates:
    // %grid_size1 = extractelement <3 x i32> %globalSize, i32 %dim

    Value* V = nullptr;
    auto F = CI.getFunction();
    if (hasStackCallAttr(*F))
    {
        llvm::IRBuilder<> Builder(&CI);
        auto ElemTypeQ = Type::getInt64Ty(F->getParent()->getContext());
        auto VecTyQ = IGCLLVM::FixedVectorType::get(ElemTypeQ, 3);
        unsigned int Offset = GLOBAL_STATE_FIELD_OFFSETS::GLOBAL_SIZE_X;
        unsigned int Size = sizeof(uint64_t) * 3;
        auto LoadInst = BuildLoadInst(CI, Offset, Size, VecTyQ);
        auto ElemType = Type::getInt32Ty(F->getParent()->getContext());
        Value* Undef = UndefValue::get(IGCLLVM::FixedVectorType::get(ElemType, 3));
        for (unsigned int I = 0; I != 3; ++I)
        {
            // Extract each dimension, truncate to i32, then insert in new vector
            auto Elem = Builder.CreateExtractElement(LoadInst, (uint64_t)I);
            auto TruncElem = Builder.CreateTrunc(Elem, ElemType);
            Undef = Builder.CreateInsertElement(Undef, TruncElem, (uint64_t)I);
        }
        V = Undef;
    }
    else
    {
        Argument* arg = getImplicitArg(CI, ImplicitArg::STAGE_IN_GRID_SIZE);
        V = arg;
    }

    Value* dim = CI.getArgOperand(0);
    Instruction* globalSize = ExtractElementInst::Create(V, dim, "grid_size", &CI);
    updateDebugLoc(&CI, globalSize);

    return globalSize;
}

Value* WIFuncResolution::getSyncBufferPtr(CallInst& CI)
{
    // Receives:
    // call i8 addrspace(1)* @__builtin_IB_get_sync_buffer()

    // Creates:
    // i8 addrspace(1)* %syncBuffer

    Argument* syncBuffer = getImplicitArg(CI, ImplicitArg::SYNC_BUFFER);

    return syncBuffer;
}

Argument* WIFuncResolution::getImplicitArg(CallInst& CI, ImplicitArg::ArgType argType)
{
    unsigned int numImplicitArgs = m_implicitArgs.size();
    unsigned int implicitArgIndex = m_implicitArgs.getArgIndex(argType);

    Function* pFunc = CI.getParent()->getParent();
    unsigned int implicitArgIndexInFunc = pFunc->arg_size() - numImplicitArgs + implicitArgIndex;

    Function::arg_iterator arg = pFunc->arg_begin();
    for (unsigned int i = 0; i < implicitArgIndexInFunc; ++i, ++arg);

    return &(*arg);
}
