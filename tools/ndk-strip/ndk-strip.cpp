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

#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

#include "llvm/Bitcode/BitcodeWriterPass.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SystemUtils.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Wrap/BitcodeWrapper.h"

using namespace llvm;

static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("<input bitcode file>"),
              cl::value_desc("filename"));

static cl::opt<std::string>
OutputFilename("o", cl::desc("Override output filename"),
               cl::value_desc("filename"));

static cl::opt<bool>
Strip("strip-all", cl::desc("Strip all symbol info"));

static cl::opt<bool>
StripDebug("strip-debug", cl::desc("Strip debugger symbol info"));

static cl::alias A0("s", cl::desc("Alias for --strip-all"),
  cl::aliasopt(Strip));

static cl::alias A1("S", cl::desc("Alias for --strip-debug"),
  cl::aliasopt(StripDebug));

static cl::alias A2("strip-unneeded", cl::desc("Alias for -strip-all"),
  cl::aliasopt(Strip));


static uint32_t ReadInt32(unsigned char *wrapper, size_t offset) {
  uint32_t value = wrapper[offset]         |
                   wrapper[offset+1] << 8  |
                   wrapper[offset+2] << 16 |
                   wrapper[offset+3] << 24;
  return value;
}

static void WriteInt32(unsigned char *wrapper, unsigned offset, uint32_t value)
{
  wrapper[offset  ] = value & 0x000000ff;
  wrapper[offset+1] = (value & 0x0000ff00) >> 8;
  wrapper[offset+2] = (value & 0x00ff0000) >> 16;
  wrapper[offset+3] = (value & 0xff000000) >> 24;
}

static size_t ReadBitcodeWrapper(int input_fd, unsigned char **wrapper, size_t& bitcode_size) {
  size_t buffer_size = 1024;
  size_t fixed_field_size = 7*4;

  *wrapper = (unsigned char*) calloc(1, buffer_size);
  size_t nread = read(input_fd, (void*) *wrapper, fixed_field_size);

  if (nread != fixed_field_size) {
    errs() << "Could not read bitcode header\n";
    exit(1);
  }

  if (!isBitcodeWrapper((const unsigned char *) *wrapper,
                        (const unsigned char *) *wrapper+fixed_field_size)) {
    errs() << "Input file is not bitcode wrapper\n";
    exit(0);
  }

  size_t offset_field = 2*4;
  size_t size_field = 3*4;
  size_t header_size = ReadInt32(*wrapper, offset_field);
  bitcode_size = ReadInt32(*wrapper, size_field);

  if (header_size > buffer_size) {
    *wrapper = (unsigned char*) realloc((void *) *wrapper, header_size);
  }

  size_t variable_field_size = header_size-fixed_field_size;
  if (variable_field_size > 0) {
    nread = read(input_fd, (void*) ((*wrapper)+fixed_field_size), variable_field_size);
    if (nread != (variable_field_size)) {
      errs() << "Could not read bitcode header\n";
      exit(1);
    }
  }

  return header_size;
}


static void StripBitcode(const char *Bitcode, size_t BitcodeSize, std::string &BCString, LLVMContext &Context) {
  StringRef input_data(Bitcode, BitcodeSize);
  MemoryBufferRef buffer(input_data, "");

  ErrorOr<Module*> Result = parseBitcodeFile(buffer, Context);
  if (!Result) {
    errs() << Result.getError().message() << '\n';
  }

  std::unique_ptr<Module> M(Result.get());
  raw_string_ostream BCStream(BCString);

  PassManager PM;

  PM.add(createVerifierPass());
  PM.add(new DataLayoutPass());

  // Strip debug info and symbols.
  if (Strip || StripDebug)
    PM.add(createStripSymbolsPass(StripDebug && !Strip));

  PM.add(createBitcodeWriterPass(BCStream));
  PM.run(*M.get());
  BCStream.flush();
}

int main(int argc, char **argv) {

  sys::PrintStackTraceOnErrorSignal();
  llvm::PrettyStackTraceProgram X(argc, argv);

  llvm_shutdown_obj Y;  // Call llvm_shutdown() on exit.
  LLVMContext &Context = getGlobalContext();

  cl::ParseCommandLineOptions(argc, argv, "Bitcode strip tool\n");

  int input_fd = open(InputFilename.c_str(), O_RDONLY);

  unsigned char *wrapper = NULL;
  const char *bitcode = NULL;

  // Read bitcode wrapper
  size_t bitcode_size = 0;
  size_t wrapper_size = ReadBitcodeWrapper(input_fd, &wrapper, bitcode_size);

  // Read bitcode
  bitcode = (const char*) calloc(1, bitcode_size);
  size_t nread = read(input_fd, (void*) bitcode, bitcode_size);
  if (nread != bitcode_size) {
    errs() << "Could not read bitcode\n";
    return 1;
  }

  // Strip bitcode
  std::string BCString;
  StripBitcode(bitcode, bitcode_size, BCString, Context);

  // Update bitcode size
  WriteInt32(wrapper, 12, BCString.length());

  // Default to input filename
  if (OutputFilename.empty())
    OutputFilename = InputFilename;

  // Output stripped bitcode
  std::error_code EC;
  tool_output_file Out(OutputFilename.c_str(), EC, sys::fs::F_None);
  if (EC) {
    errs() << EC.message() << '\n';
    return 1;
  }

  Out.os().write((const char *) wrapper, wrapper_size);
  Out.os().write(BCString.c_str(), BCString.length());
  Out.keep();

  // Clean up
  free((void *) wrapper);
  free((void *) bitcode);
  close(input_fd);

  return 0;
}
