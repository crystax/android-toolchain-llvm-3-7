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

#include <list>
#include <cstring>
#include <utility>

#include "AndroidBitcodeLinker.h"
#include "Archive.h"

#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Wrap/BitcodeWrapper.h"

#include <memory>

using namespace llvm;

static cl::list<std::string>
InputFilenames(cl::Positional, cl::OneOrMore,
               cl::desc("<input bitcode files>"));

static cl::opt<bool>
Shared("shared", cl::ZeroOrMore, cl::desc("Generate shared bitcode library"));

static cl::opt<bool>
Static("static", cl::ZeroOrMore, cl::desc("Hint for generating static library"));

static cl::opt<std::string>
OutputFilename("o", cl::desc("Override output filename"),
                cl::value_desc("output bitcode file"));

static cl::opt<std::string> Sysroot("sysroot",
                                    cl::desc("Specify sysroot"));

static cl::list<std::string>
LibPaths("L", cl::Prefix,
         cl::desc("Specify a library search path"),
         cl::value_desc("directory"));

static cl::list<std::string>
Libraries("l", cl::Prefix,
          cl::desc("Specify libraries to link to"),
          cl::value_desc("library name"));

static cl::opt<bool>
Verbose("v", cl::desc("Print verbose information"));

static cl::opt<bool>
DisableOptimizations("disable-opt",
                     cl::desc("Do not run any optimization passes"));

static cl::opt<std::string>
SOName("soname", cl::desc("Set the DT_SONAME field to the specified name"));

static cl::list<bool> WholeArchive("whole-archive",
  cl::desc("include every bitcode in the archive after --whole-archive"));

static cl::list<bool> NoWholeArchive("no-whole-archive",
  cl::desc("Turn off of the --whole-archive option for for subsequent archive files"));

static cl::opt<bool>
LinkNativeBinary("link-native-binary",
  cl::ZeroOrMore,
  cl::Hidden,
  cl::desc("Allow to link native binaries, this is only for testing purpose"));

// Strip options
static cl::opt<bool>
Strip("strip-all", cl::desc("Strip all symbol info"));

static cl::opt<bool>
StripDebug("strip-debug", cl::desc("Strip debugger symbol info"));

static cl::alias A0("s", cl::desc("Alias for --strip-all"),
  cl::aliasopt(Strip));

static cl::alias A1("S", cl::desc("Alias for --strip-debug"),
  cl::aliasopt(StripDebug));

static cl::opt<bool>
NoUndefined("no-undefined", cl::desc("-z defs"));

static cl::list<std::string>
ZOptions("z", cl::desc("-z keyword"), cl::value_desc("keyword"));

static cl::opt<bool>
PIE("pie", cl::desc("position independent executable"));

static cl::list<std::string> CO1("Wl", cl::Prefix,
  cl::desc("Compatibility option: ignored"));

static cl::opt<std::string> CO3("exclude-libs",
  cl::desc("Compatibility option: ignored"));

static cl::opt<std::string> CO4("icf",
  cl::desc("Compatibility option: ignored"));

static cl::opt<std::string> CO5("dynamic-linker",
  cl::desc("Compatibility option: ignored"));

static cl::opt<bool> CO6("gc-sections",
  cl::ZeroOrMore,
  cl::desc("Compatibility option: ignored"));

static cl::list<std::string> CO7("B", cl::Prefix,
 cl::desc("Compatibility option: ignored"));

// TODO: Support --start-group and --end-group

static cl::list<bool> CO8("start-group",
  cl::desc("Compatibility option: ignored"));

static cl::list<bool> CO9("end-group",
  cl::desc("Compatibility option: ignored"));

static cl::opt<bool> CO10("eh-frame-hdr",
  cl::ZeroOrMore,
  cl::desc("Compatibility option: ignored"));

static cl::opt<bool> CO11("no-warn-mismatch",
  cl::ZeroOrMore,
  cl::desc("Compatibility option: ignored"));

static cl::list<std::string> CO12("rpath-link",
  cl::ZeroOrMore,
  cl::desc("Compatibility option: ignored"),
  cl::value_desc("dir"));

static cl::list<unsigned int> OptimizationLevel("O",
  cl::Prefix,
  cl::desc("Optimization level for bitcode compiler"));

static std::string progname;

// Implied library dependency for specific libraries
static std::map<std::string, std::string> ImpliedLibs;

// FileRemover objects to clean up output files on the event of an error.
static FileRemover OutputRemover;

static void InitImpliedLibs() {
  ImpliedLibs.clear();
  ImpliedLibs.insert(std::make_pair("stlport_shared","gabi++_static"));
  ImpliedLibs.insert(std::make_pair("stlport_static","gabi++_static"));
  ImpliedLibs.insert(std::make_pair("c++_shared", "gabi++_static"));
  ImpliedLibs.insert(std::make_pair("c++_static", "gabi++_static"));
}

static void PrintAndExit(const std::string &Message, int errcode = 1) {
  errs() << progname << ": " << Message << "\n";
  llvm_shutdown();
  exit(errcode);
}

static void WriteInt32(uint8_t *mem, unsigned offset, uint32_t value) {
  mem[offset  ] = value & 0x000000ff;
  mem[offset+1] = (value & 0x0000ff00) >> 8;
  mem[offset+2] = (value & 0x00ff0000) >> 16;
  mem[offset+3] = (value & 0xff000000) >> 24;
}

static std::string getLibName(std::string LibPath) {
  std::string libname = sys::path::stem(LibPath);
  if (!libname.empty() && libname.substr(0,3) == "lib")
    return libname.substr(3,libname.length()-3);
  return "";
}

static std::string getImpliedLibName(std::string LibPath) {
  std::string libname = getLibName(LibPath);
  if (ImpliedLibs.count(libname) != 0)
    return ImpliedLibs.find(libname)->second;
  return "";
}

// Helper functions to determine file type

static bool isBitcode(StringRef FilePath) {
sys::fs::file_magic Magic;

  if (sys::fs::identify_magic(FilePath, Magic))
    return false;

  return Magic == sys::fs::file_magic::bitcode;
}

static bool isArchive(StringRef FilePath) {
sys::fs::file_magic Magic;

  if (sys::fs::identify_magic(FilePath, Magic))
    return false;

  return Magic == sys::fs::file_magic::archive;
}

static bool isDynamicLibrary(StringRef FilePath) {
  sys::fs::file_magic Magic;

  if (sys::fs::identify_magic(FilePath, Magic))
    return false;

  return Magic == sys::fs::file_magic::elf_shared_object;
}

static bool isBitcodeArchive(StringRef FilePath) {
  if (!isArchive(FilePath))
    return false;

  std::string ErrMsg;
  std::unique_ptr<Archive> AutoArch(
    Archive::OpenAndLoad(FilePath,
                         llvm::getGlobalContext(),
                         &ErrMsg));
  Archive* arch = AutoArch.get();

  if (!arch) {
    return false;
  }

  return arch->isBitcodeArchive();
}

static StringRef IsLibrary(StringRef Name, StringRef Directory) {
  SmallString<256> FullPath = Directory;
  sys::path::append(FullPath, "lib"+Name);

  // 1. Try bitcode archives
  sys::path::replace_extension(FullPath, "a");
  if (isBitcodeArchive(FullPath))
    return FullPath;

  // 2. Try libX.so
  sys::path::replace_extension(FullPath, "so");

  if (LinkNativeBinary && isDynamicLibrary(FullPath))
    return FullPath;
  if (isBitcode(FullPath))
    return FullPath;

  // 3. Try libX.bc
  sys::path::replace_extension(FullPath, "bc");
  if (isBitcode(FullPath))
    return FullPath;

  // 4. Try native archives
  sys::path::replace_extension(FullPath, "a");
  if (LinkNativeBinary && isArchive(FullPath))
    return FullPath;

  // Not found
  FullPath.clear();
  return FullPath;
}

static StringRef FindLib(StringRef Filename) {
  if (isArchive(Filename) || isDynamicLibrary(Filename))
    return Filename;

  for (unsigned Index = 0; Index != LibPaths.size(); ++Index) {
    StringRef Directory(LibPaths[Index]);
    StringRef FullPath = IsLibrary(Filename, Directory);
    if (sys::fs::exists(FullPath))
      return FullPath;
  }
  return "";
}

static std::string getSOName(const std::string& Filename,
                             AndroidBitcodeLinker::ABCItemList& Items) {
  for (unsigned i = 0; i < Items.size(); ++i) {
    if (Items[i].getFile().str() == Filename &&
        Items[i].getBitcodeType() == BCHeaderField::BC_SharedObject) {
      return Items[i].getSOName();
    }
  }
  return "";
}

static std::string* ProcessArgv(int argc, char **argv,
				AndroidBitcodeLinker::ABCItemList& Items) {
  std::string *ArgvString = new std::string;
  raw_string_ostream Output(*ArgvString);

  for (int i = 1 ; i < argc ; ++i) {
    // option
    if (argv[i][0] == '-') {
      // ignore "-" or "--"
      char *c = argv[i];
      while (*c == '-')
        ++c;

      // skip -o and -soname, we will add it back later
      if (!strcmp (c,"o") || !strcmp(c,"soname")) {
        i++;
        continue;
      }

      // ignore these option that doesn't need
      if (!strncmp (c,"sysroot",7) ||
          !strncmp(c,"L",1) ||
          !strcmp(c,"disable-opt") ||
          !strcmp(c,"link-native-binary") ||
          (c[0] == 'O'))
        continue;

      Output << argv[i] << " ";
    }
    else { // file or directory
      StringRef file(argv[i]);

      if (!sys::fs::is_regular_file(file)) {
        Output << argv[i] << " ";
        continue;
      }

      if (!isBitcodeArchive(file)) {
        if (!isBitcode(file)) {
          if (LinkNativeBinary) {
            Output << argv[i] << " ";
          }
          else {
            std::string libname = getLibName(argv[i]);
            if (!libname.empty())
              Output << "-l" << libname << " ";
          }
        }
        else { // bitcode or bitcode wrapper
          std::string soname = getLibName(getSOName(file.str(), Items));

          if (!soname.empty()) {
            Output << "-l" << soname << " ";
          }
        }
      }

      // Check implied libs
      std::string implied_lib = getImpliedLibName(file.str());
      if (!implied_lib.empty())
        Output << "-l" << implied_lib << " ";
    }
  }

  // Add the implied lib
  for (unsigned i = 0 ; i < Items.size(); i++) {
    std::string implied_lib = getImpliedLibName(Items[i].getSOName());
    if (!implied_lib.empty())
      Output << "-l" << implied_lib << " ";
  }

  // Convert .bc into .so
  std::string NativeFileName;
  if (Shared) {
    if (SOName.empty()) {
       NativeFileName = sys::path::stem(OutputFilename);
    }
    else {
       NativeFileName = sys::path::stem(SOName);
    }
    NativeFileName += ".so";
    Output << "-soname " << NativeFileName << " ";
  }
  else {
    NativeFileName = sys::path::stem(OutputFilename);
  }

  if (Static) {
    if (PIE) {
      errs() << "Cannot use PIE with static build\n";
      exit (1);
    }
    Output << "-static ";
  }

  std::string implied_lib = getImpliedLibName(NativeFileName);
  if (!implied_lib.empty())
    Output << "-l" << implied_lib << " ";

  Output << "-o " << NativeFileName;
  Output.flush();
  return ArgvString;
}

static void WrapAndroidBitcode(std::vector<std::string*> &BCStrings,
                               std::string& LDFlags, raw_ostream &Output) {
  std::vector<BCHeaderField> header_fields;
  std::vector<uint8_t *> field_data;
  size_t variable_header_size = 0;

  // shared object or executable
  uint32_t BitcodeType = (Shared) ? BCHeaderField::BC_SharedObject
                                  : BCHeaderField::BC_Executable;
  field_data.push_back(new uint8_t[sizeof(uint32_t)]);
  WriteInt32(field_data.back(), 0, BitcodeType);
  BCHeaderField BitcodeTypeField(BCHeaderField::kAndroidBitcodeType,
                                 sizeof(uint32_t), field_data.back());
  header_fields.push_back(BitcodeTypeField);
  variable_header_size += BitcodeTypeField.GetTotalSize();

  // ldflags
  field_data.push_back(new uint8_t[LDFlags.size()+1]);
  strcpy((char *) field_data.back(), LDFlags.c_str());
  BCHeaderField LDFlagsField(BCHeaderField::kAndroidLDFlags,
                            LDFlags.size()+1, field_data.back());
  header_fields.push_back(LDFlagsField);
  variable_header_size += LDFlagsField.GetTotalSize();

  // Compute bitcode size
  uint32_t totalBCSize = 0;
  for (unsigned i = 0; i < BCStrings.size(); ++i) {
    uint32_t BCSize = BCStrings[i]->size();
    totalBCSize += BCSize;
  }

  AndroidBitcodeWrapper wrapper;
  uint32_t opt_lv = 0;
  if (OptimizationLevel.size() > 0)
    opt_lv = OptimizationLevel[OptimizationLevel.size()-1];
  size_t actualWrapperLen = writeAndroidBitcodeWrapper(&wrapper,
                                                       totalBCSize,
                                                       14,      /* FIXME: TargetAPI     */
                                                       3400,    /* llvm-3.4             */
                                                       opt_lv); /* OptimizationLevel    */
  wrapper.BitcodeOffset += variable_header_size;

  // Write fixed fields
  Output.write(reinterpret_cast<char*>(&wrapper), actualWrapperLen);

  // Write variable fields
  for (unsigned i = 0 ; i < header_fields.size(); ++i) {
    const uint32_t buffer_size = 1024;
    uint8_t buffer[buffer_size];
    header_fields[i].Write(buffer, buffer_size);
    Output.write(reinterpret_cast<char*>(buffer), header_fields[i].GetTotalSize());
  }

  // Delete field data
  for (unsigned i = 0 ; i < field_data.size(); ++i) {
    delete [] field_data[i];
  }

  for (unsigned i = 0 ; i < BCStrings.size(); ++i) {
    Output.write(BCStrings[i]->c_str(), BCStrings[i]->size());
    delete BCStrings[i];
  }
}

void GenerateBitcode(std::vector<std::string*> &BCStrings,
                     std::string& LDFlags, const std::string& FileName) {
  if (Verbose)
    errs() << "Generating Bitcode To " << FileName << '\n';

  // Create the output file.
  std::error_code EC;
  tool_output_file Out(FileName.c_str(), EC, sys::fs::F_None);
  if (EC) {
    PrintAndExit(EC.message());
    return;
  }

  WrapAndroidBitcode(BCStrings, LDFlags, Out.os());
  Out.keep();
}

static void BuildLinkItems(AndroidBitcodeLinker::ABCItemList& Items,
                           const cl::list<std::string>& Files) {
  cl::list<bool>::const_iterator wholeIt = WholeArchive.begin();
  cl::list<bool>::const_iterator noWholeIt = NoWholeArchive.begin();
  int wholePos = -1, noWholePos = -1;
  std::vector<std::pair<int,int> > wholeRange;

  while (wholeIt != WholeArchive.end()) {
    wholePos =  WholeArchive.getPosition(wholeIt - WholeArchive.begin());
    if (noWholeIt != NoWholeArchive.end())
      noWholePos = NoWholeArchive.getPosition(noWholeIt - NoWholeArchive.begin());
    else
      noWholePos = -1;

    if (wholePos < noWholePos) {
      wholeRange.push_back(std::make_pair(wholePos, noWholePos));
      ++wholeIt;
      ++noWholeIt;
    }
    else if (noWholePos <= 0) {
      wholeRange.push_back(std::make_pair(wholePos, -1));
      break;
    }
    else {
      noWholeIt++;
    }
  }

  cl::list<std::string>::const_iterator fileIt = Files.begin();
  while ( fileIt != Files.end() ) {
      bool isWhole = false;
      int filePos = Files.getPosition(fileIt - Files.begin());
      for(unsigned i = 0 ; i < wholeRange.size() ; ++i) {
        if (filePos > wholeRange[i].first &&
           (filePos < wholeRange[i].second || wholeRange[i].second == -1)) {
          isWhole = true;
          break;
        }
      }
      if (Verbose)
        errs() << *fileIt << ":" << isWhole << '\n';
      Items.push_back(AndroidBitcodeItem(*fileIt++, isWhole));
  }

  // Find libaries in search path
  for (cl::list<std::string>::const_iterator lib_iter = Libraries.begin(),
       lib_end = Libraries.end(); lib_iter != lib_end; ++lib_iter) {
    std::string p = FindLib(*lib_iter);

    if (!p.empty()) {
      bool isWhole = false;
      int filePos = Libraries.getPosition(lib_iter - Libraries.begin());
      for (unsigned i = 0 ; i < wholeRange.size(); ++i) {
        if (filePos > wholeRange[i].first &&
           (filePos < wholeRange[i].second || wholeRange[i].second == -1)) {
          isWhole = true;
          break;
        }
      }
      Items.push_back(AndroidBitcodeItem(p, isWhole));
    }
    else {
      PrintAndExit("cannot find -l" + *lib_iter);
    }
  }
}

int main(int argc, char** argv) {
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram X(argc, argv);

  llvm_shutdown_obj _ShutdownObj;
  LLVMContext& Ctx = llvm::getGlobalContext();

  progname = sys::path::stem(argv[0]);

  cl::ParseCommandLineOptions(argc, argv, "Bitcode link tool\n");

  // Arrange for the output file to be delete on any errors.
  OutputRemover.setFile(OutputFilename);
  sys::RemoveFileOnSignal(OutputFilename);

  // Add default search path
  if (!Sysroot.empty())
    LibPaths.insert(LibPaths.begin(), Sysroot + "/usr/lib");

  InitImpliedLibs();

  // Build a list of the items from our command line
  AndroidBitcodeLinker::ABCItemList Items;
  BuildLinkItems(Items, InputFilenames);

  // Save each bitcode in strings
  std::vector<std::string*> BCStrings;

  LinkerConfig Config(Ctx, progname, OutputFilename,
                      Verbose, DisableOptimizations,
                      Strip, StripDebug, LinkNativeBinary);

  AndroidBitcodeLinker linker(Config);

  if (linker.LinkInAndroidBitcodes(Items, BCStrings))
    return 1;

  // Output processed argv
  std::string *LDFlags = ProcessArgv(argc, argv, Items);
  // Write linked bitcode
  GenerateBitcode(BCStrings, *LDFlags, OutputFilename);

  // Operation complete
  delete LDFlags;
  OutputRemover.releaseFile();
  return 0;
}
