//===- Util.cpp - Help build/query LLVM for LunarGLASS --------------------===//
//
// LunarGLASS: An Open Modular Shader Compiler Architecture
// Copyright (C) 2010-2014 LunarG, Inc.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 
//     Redistributions of source code must retain the above copyright
//     notice, this list of conditions and the following disclaimer.
// 
//     Redistributions in binary form must reproduce the above
//     copyright notice, this list of conditions and the following
//     disclaimer in the documentation and/or other materials provided
//     with the distribution.
// 
//     Neither the name of LunarG Inc. nor the names of its
//     contributors may be used to endorse or promote products derived
//     from this software without specific prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
//===----------------------------------------------------------------------===//
//
// Author: John Kessenich, LunarG
// Author: Cody Northrop, LunarG
// Author: Michael Ilseman, LunarG
//
//===----------------------------------------------------------------------===//

#include "Exceptions.h"
#include "Util.h"
#include "LunarGLASSTopIR.h"

// LLVM includes
#pragma warning(push, 1)
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CFG.h"
#pragma warning(pop)

namespace gla {

//
// Util definitions
//

int GetConstantInt(const llvm::Value* value)
{
    const llvm::ConstantInt *constantInt = llvm::dyn_cast<llvm::ConstantInt>(value);
    
    // this might still be a constant expression, rather than a numeric constant,
    // e.g., expression with undef's in it, so it was not folded
    if (! constantInt) {
        gla::UnsupportedFunctionality("non-simple constant", EATContinue);

        return 0;
    }

    return (int)constantInt->getValue().getSExtValue();
}

float GetConstantFloat(const llvm::Value* value)
{
    const llvm::ConstantFP *constantFP = llvm::dyn_cast<llvm::ConstantFP>(value);
    
    // this might still be a constant expression, rather than a numeric constant,
    // e.g., expression with undef's in it, so it was not folded
    if (! constantFP) {
        gla::UnsupportedFunctionality("non-simple constant", EATContinue);

        return 0.0f;
    }

    return constantFP->getValueAPF().convertToFloat();
}

double GetConstantDouble(const llvm::Value* value)
{
    const llvm::ConstantFP *constantFP = llvm::dyn_cast<llvm::ConstantFP>(value);
        
    // this might still be a constant expression, rather than a numeric constant,
    // e.g., expression with undef's in it, so it was not folded
    if (! constantFP) {
        gla::UnsupportedFunctionality("non-simple constant", EATContinue);

        return 0.0;
    }

    return constantFP->getValueAPF().convertToDouble();
}

// Returns false if base value is undef, or if any member is undef
bool AreAllDefined(const llvm::Value* value)
{
    if (IsUndef(value))
        return false;

    if (const llvm::User* user = llvm::dyn_cast<llvm::User>(value)) {
        switch (user->getType()->getTypeID()) {
        case llvm::Type::VectorTyID:
        case llvm::Type::ArrayTyID:
        case llvm::Type::StructTyID:
            if (user->getNumOperands() > 0) {
                for (llvm::User::const_op_iterator i = user->op_begin(), e = user->op_end(); i != e; ++i) {
                    if (! AreAllDefined(*i))
                        return false;
                }
            }
        default:
            break;
        }
    }

    return true;
}

// Returns true if base value is undef, or if all members are undef
bool AreAllUndefined(const llvm::Value* value)
{
    if (IsUndef(value))
        return true;

    // Assume a fully undef aggregate satisfies "IsUndef" at the highest level
    return false;

    // If a fully undef aggregate ever needs to be walked to to verify that,
    // use the following code.
    //if (const llvm::User* user = llvm::dyn_cast<llvm::User>(value)) {
    //    switch (user->getType()->getTypeID()) {
    //    case llvm::Type::VectorTyID:
    //    case llvm::Type::ArrayTyID:
    //    case llvm::Type::StructTyID:
    //        if (user->getNumOperands() > 0) {
    //            for (llvm::User::const_op_iterator i = user->op_begin(), e = user->op_end(); i != e; ++i) {
    //                if (! AreAllUndefined(*i))
    //                    return false;
    //            }
    //        }
    //    default:
    //        break;
    //    }
    //}

    //return true;
}

bool HasAllSet(const llvm::Value* value)
{
    const llvm::Constant* c = llvm::dyn_cast<llvm::Constant>(value);
    if (! c)
        return false;

    if (IsScalar(c->getType())) {
        return GetConstantInt(c) == -1;
    } else {
        assert(llvm::isa<llvm::ConstantDataVector>(c) || llvm::isa<llvm::ConstantVector>(c));

        for (int op = 0; op < GetComponentCount(c); ++op) {
            if (GetConstantInt(c->getAggregateElement(op)) != -1)
                return false;
        }

        return true;
    }
}

void RemoveInlineNotation(std::string& name)
{
    int end = name.size();
    if (name[end-1] == 'i' && name[end-2] == '.')
        name.resize(end-2);
}

// Some interfaces to LLVM builder require unsigned indices instead of a vector.
// i.e. llvm::IRBuilder::CreateExtractValue()
// This method will do the conversion and inform the caller if not every element was
// a constant integer.
bool ConvertValuesToUnsigned(unsigned* indices, int &count, llvm::ArrayRef<llvm::Value*> chain)
{
    llvm::ArrayRef<llvm::Value*>::iterator start = chain.begin();

    for (count = 0; start != chain.end(); ++start, ++count) {
        if (llvm::Constant* constant = llvm::dyn_cast<llvm::Constant>(*start)) {
            if (llvm::ConstantInt *constantInt = llvm::dyn_cast<llvm::ConstantInt>(constant))
                indices[count] = (unsigned)constantInt->getValue().getSExtValue();
            else
                return false;
        } else {
            return false;
        }
    }

    return true;
}

bool IsPerComponentOp(const llvm::IntrinsicInst* intr)
{
    switch (intr->getIntrinsicID()) {
    // Pipeline ops
    case llvm::Intrinsic::gla_readData :
    case llvm::Intrinsic::gla_fReadData :
    case llvm::Intrinsic::gla_fWriteInterpolant :
    case llvm::Intrinsic::gla_fReadInterpolant :
    case llvm::Intrinsic::gla_fReadInterpolantOffset :
    case llvm::Intrinsic::gla_getInterpolant :
    case llvm::Intrinsic::gla_writeData :
    case llvm::Intrinsic::gla_fWriteData :

    // Packing
    case llvm::Intrinsic::gla_fPackUnorm2x16:
    case llvm::Intrinsic::gla_fPackUnorm4x8:
    case llvm::Intrinsic::gla_fPackSnorm4x8:
    case llvm::Intrinsic::gla_fUnpackUnorm2x16:
    case llvm::Intrinsic::gla_fUnpackUnorm4x8:
    case llvm::Intrinsic::gla_fUnpackSnorm4x8:
    case llvm::Intrinsic::gla_fPackDouble2x32:
    case llvm::Intrinsic::gla_fUnpackDouble2x32:

    // Texture Sampling
    case llvm::Intrinsic::gla_textureSample:
    case llvm::Intrinsic::gla_fTextureSample:
    case llvm::Intrinsic::gla_rTextureSample1:
    case llvm::Intrinsic::gla_fRTextureSample1:
    case llvm::Intrinsic::gla_rTextureSample2:
    case llvm::Intrinsic::gla_fRTextureSample2:
    case llvm::Intrinsic::gla_rTextureSample3:
    case llvm::Intrinsic::gla_fRTextureSample3:
    case llvm::Intrinsic::gla_rTextureSample4:
    case llvm::Intrinsic::gla_fRTextureSample4:
    case llvm::Intrinsic::gla_textureSampleLodRefZ:
    case llvm::Intrinsic::gla_fTextureSampleLodRefZ:
    case llvm::Intrinsic::gla_rTextureSampleLodRefZ1:
    case llvm::Intrinsic::gla_fRTextureSampleLodRefZ1:
    case llvm::Intrinsic::gla_rTextureSampleLodRefZ2:
    case llvm::Intrinsic::gla_fRTextureSampleLodRefZ2:
    case llvm::Intrinsic::gla_rTextureSampleLodRefZ3:
    case llvm::Intrinsic::gla_fRTextureSampleLodRefZ3:
    case llvm::Intrinsic::gla_rTextureSampleLodRefZ4:
    case llvm::Intrinsic::gla_fRTextureSampleLodRefZ4:
    case llvm::Intrinsic::gla_textureSampleLodRefZOffset:
    case llvm::Intrinsic::gla_fTextureSampleLodRefZOffset:
    case llvm::Intrinsic::gla_rTextureSampleLodRefZOffset1:
    case llvm::Intrinsic::gla_fRTextureSampleLodRefZOffset1:
    case llvm::Intrinsic::gla_rTextureSampleLodRefZOffset2:
    case llvm::Intrinsic::gla_fRTextureSampleLodRefZOffset2:
    case llvm::Intrinsic::gla_rTextureSampleLodRefZOffset3:
    case llvm::Intrinsic::gla_fRTextureSampleLodRefZOffset3:
    case llvm::Intrinsic::gla_rTextureSampleLodRefZOffset4:
    case llvm::Intrinsic::gla_fRTextureSampleLodRefZOffset4:
    case llvm::Intrinsic::gla_textureSampleLodRefZOffsetGrad:
    case llvm::Intrinsic::gla_fTextureSampleLodRefZOffsetGrad:
    case llvm::Intrinsic::gla_rTextureSampleLodRefZOffsetGrad1:
    case llvm::Intrinsic::gla_fRTextureSampleLodRefZOffsetGrad1:
    case llvm::Intrinsic::gla_rTextureSampleLodRefZOffsetGrad2:
    case llvm::Intrinsic::gla_fRTextureSampleLodRefZOffsetGrad2:
    case llvm::Intrinsic::gla_rTextureSampleLodRefZOffsetGrad3:
    case llvm::Intrinsic::gla_fRTextureSampleLodRefZOffsetGrad3:
    case llvm::Intrinsic::gla_rTextureSampleLodRefZOffsetGrad4:
    case llvm::Intrinsic::gla_fRTextureSampleLodRefZOffsetGrad4:
    case llvm::Intrinsic::gla_texelFetchOffset:
    case llvm::Intrinsic::gla_fTexelFetchOffset:
    case llvm::Intrinsic::gla_texelGather:
    case llvm::Intrinsic::gla_fTexelGather:
    case llvm::Intrinsic::gla_texelGatherOffset:
    case llvm::Intrinsic::gla_fTexelGatherOffset:
    case llvm::Intrinsic::gla_texelGatherOffsets:
    case llvm::Intrinsic::gla_fTexelGatherOffsets:

    // Texture Query
    case llvm::Intrinsic::gla_queryTextureSize:
    case llvm::Intrinsic::gla_fQueryTextureLod:

    // Geometry
    case llvm::Intrinsic::gla_fLength:
    case llvm::Intrinsic::gla_fDistance:
    case llvm::Intrinsic::gla_fDot2:
    case llvm::Intrinsic::gla_fDot3:
    case llvm::Intrinsic::gla_fDot4:
    case llvm::Intrinsic::gla_fCross:
    case llvm::Intrinsic::gla_fNormalize:
    case llvm::Intrinsic::gla_fNormalize3D:
    case llvm::Intrinsic::gla_fLit:
    case llvm::Intrinsic::gla_fFaceForward:
    case llvm::Intrinsic::gla_fReflect:
    case llvm::Intrinsic::gla_fRefract:

    // Derivatives/transform
    case llvm::Intrinsic::gla_fDFdx:
    case llvm::Intrinsic::gla_fDFdy:
    case llvm::Intrinsic::gla_fFilterWidth:
    case llvm::Intrinsic::gla_fFixedTransform:

    // Vector ops
    case llvm::Intrinsic::gla_any:
    case llvm::Intrinsic::gla_all:

        return false;
    default:
        break;
    } // end of switch (intr->getIntrinsicID())

    return true;
}


bool IsPerComponentOp(const llvm::Instruction* inst)
{
    if (const llvm::IntrinsicInst* intr = llvm::dyn_cast<const llvm::IntrinsicInst>(inst))
        return IsPerComponentOp(intr);

    if (inst->isTerminator())
        return false;

    switch (inst->getOpcode()) {

    // Cast ops are only per-component if they cast back to the same vector
    // width
    case llvm::Instruction::Trunc:
    case llvm::Instruction::ZExt:
    case llvm::Instruction::SExt:
    case llvm::Instruction::FPToUI:
    case llvm::Instruction::FPToSI:
    case llvm::Instruction::UIToFP:
    case llvm::Instruction::SIToFP:
    case llvm::Instruction::FPTrunc:
    case llvm::Instruction::FPExt:
    case llvm::Instruction::PtrToInt:
    case llvm::Instruction::IntToPtr:
    case llvm::Instruction::BitCast:
        return GetComponentCount(inst->getOperand(0)) == GetComponentCount(inst);

    // Vector ops
    case llvm::Instruction::InsertElement:
    case llvm::Instruction::ExtractElement:
    case llvm::Instruction::ShuffleVector:

    // Ways of accessing/loading/storing vectors
    case llvm::Instruction::ExtractValue:
    case llvm::Instruction::InsertValue:

    // Memory ops
    case llvm::Instruction::Alloca:
    case llvm::Instruction::Load:
    case llvm::Instruction::Store:
    case llvm::Instruction::GetElementPtr:

    // Phis are a little special. We consider them not to be per-component
    // because the mechanism of choice is a single value (what path we took to
    // get here), and doesn't choose per-component (as select would). The caller
    // should know to handle phis specially
    case llvm::Instruction::PHI:

    // Call insts, conservatively are no per-component
    case llvm::Instruction::Call:

    // Misc
    // case llvm::Instruction::LandingPad:  --- 3.0
    case llvm::Instruction::VAArg:
        return false;

    default:
        break;
    } // end of switch (inst->getOpcode())

    return true;
}


bool IsPerComponentOp(const llvm::Value* value)
{
    const llvm::Instruction* inst = llvm::dyn_cast<const llvm::Instruction>(value);
    return inst && IsPerComponentOp(inst);
}



}; // end gla namespace
