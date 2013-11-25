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

#ifndef EXPAND_VAARG_PASS_H
#define EXPAND_VAARG_PASS_H

#include <llvm/Pass.h>

namespace llvm {
  class Function;
  class Instruction;
  class LLVMContext;
  class Value;
} // end llvm namespace

/*
 * This pass expands va_arg LLVM instruction
 *
 * LLVM backend does not yet fully support va_arg on many targets. Also,
 * it does not currently support va_arg with aggregate types on any target.
 * Therefore, each target should implement its own verion of
 * ExpandVAArg::expandVAArg to expand va_arg.
 */

class ExpandVAArgPass : public llvm::FunctionPass {
private:
  static char ID;

protected:
  llvm::LLVMContext *mContext;

private:
  virtual llvm::Value *expandVAArg(llvm::Instruction *pInst) = 0;

public:
  ExpandVAArgPass() : llvm::FunctionPass(ID), mContext(NULL) { }

  virtual bool runOnFunction(llvm::Function &pFunc);
};


ExpandVAArgPass* createARMExpandVAArgPass();
ExpandVAArgPass* createX86ExpandVAArgPass();
ExpandVAArgPass* createMipsExpandVAArgPass();

#endif // EXPAND_VAARG_PASS_H
