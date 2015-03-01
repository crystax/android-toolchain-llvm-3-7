/*
 * Copyright 2013, The Android Open Source Project
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

#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <iterator>

char ExpandVAArgPass::ID = 0;

bool ExpandVAArgPass::runOnModule(llvm::Module &pM) {
  bool changed = false;

  mContext = &pM.getContext();
  llvm::SmallVector<llvm::Instruction*, 8> Insts;

  // process va_arg inst
  for (llvm::Module::iterator i = pM.begin(), e = pM.end(); i != e; ++i) {
    for (llvm::Function::iterator fi = i->begin(), fe = i->end(); fi != fe; ++fi) {
      for (llvm::BasicBlock::iterator bi = fi->begin(), be = fi->end();
           bi != be; ++bi) {
        llvm::Instruction *inst = &*bi;
        if (inst->getOpcode() == llvm::Instruction::VAArg) {
          llvm::VAArgInst* va_inst = llvm::cast<llvm::VAArgInst>(&*inst);
          Insts.push_back(va_inst);
        }
      }
    }
  }

  for (llvm::SmallVector<llvm::Instruction*, 8>::iterator i = Insts.begin(),
       e = Insts.end(); i != e; ++i) {
    llvm::Value *v = expandVAArg(*i);
    (*i)->replaceAllUsesWith(v);
    (*i)->eraseFromParent();
    changed = true;
  }

  return changed;
}

llvm::Value *NDK64ExpandVAArg::expandVAArg(llvm::Instruction *pInst) {
  mVAArgInst = pInst;
  mVAArgTy = pInst->getType();
  llvm::Value *va_list_addr_ptr = pInst->getOperand(0);
  llvm::IRBuilder<> builder(pInst);
  mVAList = builder.CreateConstGEP1_32(va_list_addr_ptr, 0, "va_list");

  llvm::Function *vaarg_func = getOrCreateFunc();
  assert (vaarg_func->arg_begin()->getType()->isPointerTy() &&
          "Parameter should be pointer type to va_list struct");
  return builder.CreateCall(vaarg_func,
                            builder.CreateBitCast(mVAList,
                                           vaarg_func->arg_begin()->getType()));
}

llvm::Function *NDK64ExpandVAArg::getOrCreateFunc() {
  llvm::Function *func =
    llvm::Function::Create(llvm::FunctionType::get(mVAArgTy, mVAList->getType(),
                                                   /*VarArg*/false),
                           llvm::GlobalValue::InternalLinkage,
                           getVAArgFuncName(),
                           mVAArgInst->getParent()->getParent()->getParent());

  std::pair<VAArgFuncMapTy::iterator, bool> ret =
    mVAArgFuncs.insert(std::make_pair(mVAArgTy, func));
  if (!ret.second) {
    assert (!ret.first->second->isDeclaration() && "Function should be defined");
    func->eraseFromParent();
    return ret.first->second;
  }

  fillupVAArgFunc(*func);
  return func;
}

std::string NDK64ExpandVAArg::getVAArgFuncName() const {
  std::string func_name = "va_arg";
  if (mVAArgTy->isHalfTy())
    func_name += ".f16";
  else if (mVAArgTy->isFloatTy())
    func_name += ".f32";
  else if (mVAArgTy->isDoubleTy())
    func_name += ".f64";
  else if (mVAArgTy->isFP128Ty())
    func_name += ".f128";
  else if (mVAArgTy->isIntegerTy(8))
    func_name += ".i8";
  else if (mVAArgTy->isIntegerTy(16))
    func_name += ".i16";
  else if (mVAArgTy->isIntegerTy(32))
    func_name += ".i32";
  else if (mVAArgTy->isIntegerTy(64))
    func_name += ".i64";
  else if (mVAArgTy->isPointerTy())
    func_name += ".p";
  else {
    mVAArgTy->dump();
    assert (false && "va_arg for un-support type.");
  }
  return func_name;
}
