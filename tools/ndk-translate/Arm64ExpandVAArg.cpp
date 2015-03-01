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

class Arm64ExpandVAArg : public ExpandVAArgPass {
public:
  virtual const char *getPassName() const {
    return "Arm64 LLVM va_arg Instruction Expansion Pass";
  }

private:
  // Derivative work from clang/lib/CodeGen/TargetInfo.cpp.
  virtual llvm::Value *expandVAArg(llvm::Instruction *pInst);
};

llvm::Value *Arm64ExpandVAArg::expandVAArg(llvm::Instruction *pInst) {
  // TODO: Support argument number > 8
  // TODO: Support other types
  // struct {
  //   void *__stack;
  //   void *__gr_top;
  //   void *__vr_top;
  //   int __gr_offs;
  //   int __vr_offs;
  // };
  llvm::Type *va_arg_type = pInst->getType();
  llvm::Value *va_list_addr = pInst->getOperand(0);
  llvm::IRBuilder<> builder(pInst);
  unsigned reg_top_field = 1;
  unsigned reg_offset_field = 3;
  unsigned reg_used_size = 8 * 1;  // 8 byte, we use 1 register
  if (va_arg_type->isHalfTy() || va_arg_type->isFloatTy() ||
      va_arg_type->isDoubleTy() || va_arg_type->isVectorTy()) {
    reg_top_field = 2;
    reg_offset_field = 4;
    reg_used_size = 16 * 1;
  }

  llvm::Value *reg_offs_p =
      builder.CreateStructGEP(va_list_addr, reg_offset_field, "gr_offs_p");
  llvm::Value *reg_offs = builder.CreateLoad(reg_offs_p, "gr_offs");

  // Update the gr/vr_offs pointer for next call to va_arg on this va_list.
  llvm::Value *new_offset =
      builder.CreateAdd(reg_offs,
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(*mContext),
                                               reg_used_size),
                         "new_reg_offs");
  builder.CreateStore(new_offset, reg_offs_p);

  // Load the integer value
  llvm::Value *reg_top_p = builder.CreateStructGEP(va_list_addr,
                                                   reg_top_field, "reg_top_p");
  llvm::Value *reg_top = builder.CreateLoad(reg_top_p, "reg_top");
  llvm::Value *base_addr = builder.CreateGEP(reg_top, reg_offs);
  llvm::Value *valueTy_addr =
      builder.CreateBitCast(base_addr, llvm::PointerType::getUnqual(va_arg_type));
  llvm::Value *result = builder.CreateLoad(valueTy_addr);
  return result;
}

ExpandVAArgPass* createArm64ExpandVAArgPass() {
  return new Arm64ExpandVAArg();
}
