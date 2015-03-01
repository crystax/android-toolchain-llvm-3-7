/*
 * Copyright 2014, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ExpandVAArgPass.h"

#include <sstream>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/Triple.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Pass.h>

class X86_64ExpandVAArg : public NDK64ExpandVAArg {
public:
  virtual const char *getPassName() const {
    return "X86_64 LLVM va_arg Instruction Expansion Pass";
  }

private:
  virtual void fillupVAArgFunc(llvm::Function &);

private:
  // Lookup the map to take an existing function, or it will
  // create a new one and insert into the map.
  llvm::Type *getNativeVAListType();
  llvm::Value *emitVAArgFromMemory(llvm::IRBuilder<> &);
};

// ------------ Implementation -------------- //

void X86_64ExpandVAArg::fillupVAArgFunc(llvm::Function &func) {
  // TODO: Support many argument numbers
  // TODO: Support other types
  llvm::BasicBlock *entry_bb =
    llvm::BasicBlock::Create(*mContext, "entry", &func);
  llvm::BasicBlock *next_bb = entry_bb->getNextNode();
  llvm::Value *va_list_addr = func.getArgumentList().begin();
  llvm::IRBuilder<> builder(entry_bb);

  // First of all, replace it to native va_list type
  va_list_addr = builder.CreateBitCast(va_list_addr,
                          llvm::PointerType::getUnqual(getNativeVAListType()));

  unsigned neededInt = 1u, neededSSE = 0u;
  if (mVAArgTy->isHalfTy() || mVAArgTy->isFloatTy() ||
      mVAArgTy->isDoubleTy() || mVAArgTy->isVectorTy()) {
    neededInt = 0u;
    neededSSE = 1u;
  }

  // AMD64-ABI 3.5.7p5: Step 1. Determine whether type may be passed
  // in the registers. If not go to step 7.
  if (!neededInt && !neededSSE) {
    emitVAArgFromMemory(builder);
    return;
  }

  // AMD64-ABI 3.5.7p5: Step 2. Compute num_gp to hold the number of
  // general purpose registers needed to pass type and num_fp to hold
  // the number of floating point registers needed.

  // AMD64-ABI 3.5.7p5: Step 3. Verify whether arguments fit into
  // registers. In the case: l->gp_offset > 48 - num_gp * 8 or
  // l->fp_offset > 304 - num_fp * 16 go to step 7.
  //
  // NOTE: 304 is a typo, there are (6 * 8 + 8 * 16) = 176 bytes of
  // register save space).
  llvm::Value *InRegs = 0;
  llvm::Value *gp_offset_p = 0, *gp_offset = 0;
  llvm::Value *fp_offset_p = 0, *fp_offset = 0;
  if (neededInt) {
    gp_offset_p = builder.CreateStructGEP(va_list_addr, 0, "gp_offset_p");
    gp_offset = builder.CreateLoad(gp_offset_p, "gp_offset");
    InRegs = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*mContext), 48 - neededInt * 8);
    InRegs = builder.CreateICmpULE(gp_offset, InRegs, "fits_in_gp");
  }
  if (neededSSE) {
    fp_offset_p = builder.CreateStructGEP(va_list_addr, 1, "fp_offset_p");
    fp_offset = builder.CreateLoad(fp_offset_p, "fp_offset");
    llvm::Value *FitsInFP =
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(*mContext), 176 - neededSSE * 16);
    FitsInFP = builder.CreateICmpULE(fp_offset, FitsInFP, "fits_in_fp");
    InRegs = InRegs ? builder.CreateAnd(InRegs, FitsInFP) : FitsInFP;
  }

  llvm::BasicBlock *InRegBlock = llvm::BasicBlock::Create(*mContext,
                                                          "vaarg.in_reg",
                                                          &func,
                                                          next_bb);
  llvm::BasicBlock *InMemBlock = llvm::BasicBlock::Create(*mContext,
                                                          "vaarg.in_mem",
                                                          &func,
                                                          next_bb);
  llvm::BasicBlock *ContBlock = llvm::BasicBlock::Create(*mContext,
                                                         "vaarg.end",
                                                         &func,
                                                         next_bb);
  builder.CreateCondBr(InRegs, InRegBlock, InMemBlock);

  llvm::Value *RegAddr = NULL, *MemAddr = NULL, *result = NULL;
  llvm::PHINode *ResAddr = NULL;
  // Emit code to load the value if it was passed in registers.
  {
    llvm::IRBuilder<> builder(InRegBlock);
    // AMD64-ABI 3.5.7p5: Step 4. Fetch type from l->reg_save_area with
    // an offset of l->gp_offset and/or l->fp_offset. This may require
    // copying to a temporary location in case the parameter is passed
    // in different register classes or requires an alignment greater
    // than 8 for general purpose registers and 16 for XMM registers.
    RegAddr =
      builder.CreateLoad(builder.CreateStructGEP(va_list_addr, 3),
                         "reg_save_area");
    if (neededInt && neededSSE) {
      builder.CreateUnreachable();
    } else if (neededInt) {
      RegAddr = builder.CreateGEP(RegAddr, gp_offset);
      RegAddr = builder.CreateBitCast(RegAddr,
                                      llvm::PointerType::getUnqual(mVAArgTy));
    } else if (neededSSE == 1) {
      RegAddr = builder.CreateGEP(RegAddr, fp_offset);
      RegAddr = builder.CreateBitCast(RegAddr,
                                      llvm::PointerType::getUnqual(mVAArgTy));
    } else {
      builder.CreateUnreachable();
    }

    // AMD64-ABI 3.5.7p5: Step 5. Set:
    // l->gp_offset = l->gp_offset + num_gp * 8
    // l->fp_offset = l->fp_offset + num_fp * 16.
    if (neededInt) {
      llvm::Value *Offset = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*mContext),
                                                   neededInt * 8);
      builder.CreateStore(builder.CreateAdd(gp_offset, Offset),
                          gp_offset_p);
    }
    if (neededSSE) {
      llvm::Value *Offset = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*mContext),
                                                   neededSSE * 16);
      builder.CreateStore(builder.CreateAdd(fp_offset, Offset),
                          fp_offset_p);
    }
    builder.CreateBr(ContBlock);
  }

  // Emit code to load the value if it was passed in memory.
  {
    llvm::IRBuilder<> builder(InMemBlock);
    MemAddr = emitVAArgFromMemory(builder);
  }

  // Return the appropriate result.
  {
    llvm::IRBuilder<> builder(ContBlock);
    //ResAddr = builder.CreatePHI(RegAddr->getType(), 2, "vaarg.addr");
    //ResAddr->addIncoming(MemAddr, InMemBlock);
    ResAddr = builder.CreatePHI(RegAddr->getType(), 1, "vaarg.addr");
    ResAddr->addIncoming(RegAddr, InRegBlock);
    result = builder.CreateLoad(ResAddr);
    builder.CreateRet(result);
  }
}


llvm::Type *X86_64ExpandVAArg::getNativeVAListType() {
  // struct {
  //   unsigned gp_offset;
  //   unsigned fp_offset;
  //   void *overflow_arg_area;
  //   void *reg_save_area;
  // };
  static llvm::SmallVector<llvm::Type*, 4> va_list_ty_elmnts;
  va_list_ty_elmnts.push_back(llvm::IntegerType::get(*mContext, /*bits*/32));
  va_list_ty_elmnts.push_back(llvm::IntegerType::get(*mContext, /*bits*/32));
  va_list_ty_elmnts.push_back(llvm::PointerType::getUnqual(
                              llvm::IntegerType::get(*mContext, /*bits*/8)));
  va_list_ty_elmnts.push_back(llvm::PointerType::getUnqual(
                              llvm::IntegerType::get(*mContext, /*bits*/8)));
  static llvm::StructType *va_list_ty =
    llvm::StructType::get(*mContext, va_list_ty_elmnts);
  return va_list_ty;
}

llvm::Value *X86_64ExpandVAArg::emitVAArgFromMemory(llvm::IRBuilder<> &pBuilder) {
  // TODO: There's lots of code in clang/lib/CodeGen/TargetInfo.cpp, this is a
  // big effort so that we only implement the simple part to make sure NDK tests
  // work for now.
  return pBuilder.CreateUnreachable();
}


ExpandVAArgPass* createX86_64ExpandVAArgPass() {
  return new X86_64ExpandVAArg();
}
