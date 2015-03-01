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

#include "ReplaceUnwindHeaderSizePass.h"

#include "llvm/ADT/APInt.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Module.h"

char ReplaceUnwindHeaderSizePass::ID = 0;

bool ReplaceUnwindHeaderSizePass::runOnModule(llvm::Module &M) {
  bool changed = false;
  llvm::LLVMContext &ctx = M.getContext();
  const llvm::DataLayout *dl = M.getDataLayout();
  llvm::APInt unwind_hdr_size(/*numBits=*/dl->getPointerSizeInBits(),
                              /*val=*/getTargetUnwindHeaderSize());
  llvm::ConstantInt *size_value = llvm::ConstantInt::get(ctx, unwind_hdr_size);
  const char *k_func_name = "__ndk_unknown_getUnwindHeaderSize";

  llvm::SmallVector<llvm::Instruction*, 8> Insts;
  llvm::Function *Func = 0;

  for (llvm::Module::iterator i = M.begin(), e = M.end(); i != e; ++i) {
    if (i->getName() == k_func_name)
      Func = &*i;

    for (llvm::Function::iterator fi = i->begin(), fe = i->end(); fi != fe; ++fi) {
      for (llvm::BasicBlock::iterator bi = fi->begin(), be = fi->end(); bi != be; ++bi) {
        if (llvm::CallInst *call = llvm::dyn_cast<llvm::CallInst>(&*bi)) {
          if (!call->getCalledFunction())
            continue;
          if (call->getCalledFunction()->getName() != k_func_name)
            continue;

          call->replaceAllUsesWith(size_value);
          changed = true;
          Insts.push_back(call);
        }
      }
    }
  }

  for (llvm::SmallVector<llvm::Instruction*, 8>::iterator i = Insts.begin(), e = Insts.end();
       i != e; ++i)
    (*i)->eraseFromParent();

  if (Func)
    Func->eraseFromParent();

  return changed;
}

