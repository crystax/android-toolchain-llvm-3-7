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

#ifndef REPLACE_UNWIND_HEADER_SIZE_PASS_H
#define REPLACE_UNWIND_HEADER_SIZE_PASS_H

#include <llvm/Pass.h>

/*  This pass expands intrinsic __ndk_unknown_getUnwindHeaderSize.
 *
 *  _Unwind_Exception has different size for each target.
 *
 *  ARM: 88
 *  Mips: 24
 *  Arm64, x86, x86_64, Mips64: 32
 */
class ReplaceUnwindHeaderSizePass : public llvm::ModulePass {
private:
  static char ID;

public:
  ReplaceUnwindHeaderSizePass()
    : llvm::ModulePass(ID) {}
  virtual bool runOnModule(llvm::Module &M);
  virtual size_t getTargetUnwindHeaderSize() const = 0;
};

ReplaceUnwindHeaderSizePass* createARMReplaceUnwindHeaderSizePass();
ReplaceUnwindHeaderSizePass* createX86ReplaceUnwindHeaderSizePass();
ReplaceUnwindHeaderSizePass* createMipsReplaceUnwindHeaderSizePass();

#endif // REPLACE_UNWIND_HEADER_SIZE_PASS_H
