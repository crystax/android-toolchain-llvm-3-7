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
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>

char ExpandVAArgPass::ID = 0;

bool ExpandVAArgPass::runOnFunction(llvm::Function &pFunc) {
  bool changed = false;

  mContext = &pFunc.getContext();

  // process va_arg inst
  for (llvm::inst_iterator inst = llvm::inst_begin(pFunc),
          inst_end = llvm::inst_end(pFunc), next_inst = std::next(inst);
       inst != inst_end; inst = next_inst++) {
    if (inst->getOpcode() == llvm::Instruction::VAArg) {
      llvm::Value *v = expandVAArg(&*inst);
      inst->replaceAllUsesWith(v);
      inst->eraseFromParent();
      changed = true;
      continue;
    }
  }
  return changed;
}

