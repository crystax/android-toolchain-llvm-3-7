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

class Mips64ExpandVAArg : public NDK64ExpandVAArg {
public:
  virtual const char *getPassName() const {
    return "Mips64 LLVM va_arg Instruction Expansion Pass";
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

void Mips64ExpandVAArg::fillupVAArgFunc(llvm::Function &func) {
  llvm::BasicBlock *entry_bb =
    llvm::BasicBlock::Create(*mContext, "entry", &func);
  llvm::Value *va_list_addr = func.getArgumentList().begin();
  llvm::IRBuilder<> builder(entry_bb);

  // First of all, replace it to native va_list type
  va_list_addr = builder.CreateBitCast(va_list_addr,
                          llvm::PointerType::getUnqual(getNativeVAListType()));

  llvm::Value *va_list_addr_as_bpp =
    builder.CreateBitCast(va_list_addr,
              llvm::PointerType::getUnqual(llvm::Type::getInt8PtrTy(*mContext)),
                          "ap");
  llvm::Value *addr = builder.CreateLoad(va_list_addr_as_bpp, "ap.cur");

 // TODO: Alignment?
  llvm::Value *addr_typed =
    builder.CreateBitCast(addr, llvm::PointerType::getUnqual(mVAArgTy));
  llvm::Value *aligned_addr =
    builder.CreateBitCast(addr_typed, llvm::Type::getInt8PtrTy(*mContext));

  uint64_t offset = mVAArgTy->getPrimitiveSizeInBits() / 8;
  assert (offset > 0 && "Cannot get size of va_arg");
  llvm::Value *next_addr =
    builder.CreateGEP(aligned_addr,
                      llvm::ConstantInt::get(llvm::Type::getInt64Ty(*mContext),
                                             offset),
                      "ap.next");
  builder.CreateStore(next_addr, va_list_addr_as_bpp);

  builder.CreateRet(builder.CreateLoad(addr_typed));
}


llvm::Type *Mips64ExpandVAArg::getNativeVAListType() {
  return llvm::Type::getInt8Ty(*mContext);
}


ExpandVAArgPass* createMips64ExpandVAArgPass() {
  return new Mips64ExpandVAArg();
}
