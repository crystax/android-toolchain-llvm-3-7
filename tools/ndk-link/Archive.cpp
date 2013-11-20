//===-- Archive.cpp - Generic LLVM archive functions ------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the implementation of the Archive and ArchiveMember
// classes that is common to both reading and writing archives..
//
//===----------------------------------------------------------------------===//

#include "Archive.h"
#include "ArchiveInternals.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Process.h"
#include <cstring>
#include <memory>
#include <system_error>
using namespace llvm;

// getMemberSize - compute the actual physical size of the file member as seen
// on disk. This isn't the size of member's payload. Use getSize() for that.
unsigned
ArchiveMember::getMemberSize() const {
  // Basically its the file size plus the header size
  unsigned result = Size + sizeof(ArchiveMemberHeader);

  // If it has a long filename, include the name length
  if (hasLongFilename())
    result += path.length() + 1;

  // If its now odd lengthed, include the padding byte
  if (result % 2 != 0 )
    result++;

  return result;
}

// This default constructor is only use by the ilist when it creates its
// sentry node. We give it specific static values to make it stand out a bit.
ArchiveMember::ArchiveMember()
  : parent(0), path("--invalid--"), flags(0), data(0)
{
  User = 65536;
  Group = 65536;
  Mode = 0777;
  Size = 0;
  ModTime = sys::TimeValue::now();
}

// This is the constructor that the Archive class uses when it is building or
// reading an archive. It just defaults a few things and ensures the parent is
// set for the iplist. The Archive class fills in the ArchiveMember's data.
// This is required because correctly setting the data may depend on other
// things in the Archive.
ArchiveMember::ArchiveMember(Archive* PAR)
  : parent(PAR), path(), flags(0), data(0)
{
}

// This method allows an ArchiveMember to be replaced with the data for a
// different file, presumably as an update to the member. It also makes sure
// the flags are reset correctly.
bool ArchiveMember::replaceWith(StringRef newFile, std::string* ErrMsg) {
  if (!sys::fs::exists(newFile)) {
    if (ErrMsg)
      *ErrMsg = "Can not replace an archive member with a non-existent file";
    return true;
  }

  data = 0;
  path = newFile.str();

  // SVR4 symbol tables have an empty name
  if (path == ARFILE_SVR4_SYMTAB_NAME)
    flags |= SVR4SymbolTableFlag;
  else
    flags &= ~SVR4SymbolTableFlag;

  // BSD4.4 symbol tables have a special name
  if (path == ARFILE_BSD4_SYMTAB_NAME)
    flags |= BSD4SymbolTableFlag;
  else
    flags &= ~BSD4SymbolTableFlag;

  // String table name
  if (path == ARFILE_STRTAB_NAME)
    flags |= StringTableFlag;
  else
    flags &= ~StringTableFlag;

  // If it has a slash or its over 15 chars then its a long filename format
  if (path.length() > 15)
    flags |= HasLongFilenameFlag;
  else
    flags &= ~HasLongFilenameFlag;

  // Get the file status info
  sys::fs::file_status Status;
  if (sys::fs::status(path, Status)) {
    return true;
  }

  User = Status.getUser();
  Group = Status.getGroup();
  Mode = Status.permissions();
  ModTime = Status.getLastModificationTime();
  Size = Status.getSize();

  // Determine what kind of file it is.
  sys::fs::file_magic magic = sys::fs::file_magic::unknown;
  if (sys::fs::identify_magic(path, magic)) {
    return true;
  }
  if (magic == sys::fs::file_magic::bitcode)
    flags |= BitcodeFlag;
  else
    flags &= ~BitcodeFlag;

  return false;
}

// Archive constructor - this is the only constructor that gets used for the
// Archive class. Everything else (default,copy) is deprecated. This just
// initializes and maps the file into memory, if requested.
Archive::Archive(StringRef filename, LLVMContext &C)
  : archPath(filename), members(), mapfile(0), base(0), symTab(), strtab(),
    symTabSize(0), firstFileOffset(0), modules(), Context(C) {}

bool
Archive::mapToMemory(std::string* ErrMsg) {
  ErrorOr<std::unique_ptr<MemoryBuffer> > FileOrErr =
      MemoryBuffer::getFile(archPath.str());
  if (!FileOrErr) {
    if (ErrMsg)
      *ErrMsg = FileOrErr.getError().message();
    return true;
  }
  mapfile = FileOrErr.get().release();
  base = mapfile->getBufferStart();
  return false;
}

void Archive::cleanUpMemory() {
  // Shutdown the file mapping
  delete mapfile;
  mapfile = 0;
  base = 0;

  // Forget the entire symbol table
  symTab.clear();
  symTabSize = 0;

  firstFileOffset = 0;

  // Delete any Modules and ArchiveMember's we've allocated as a result of
  // symbol table searches.
  for (ModuleMap::iterator I=modules.begin(), E=modules.end(); I != E; ++I ) {
    delete I->second.first;
    delete I->second.second;
  }
}

// Archive destructor - just clean up memory
Archive::~Archive() {
  cleanUpMemory();
}



static void getSymbols(Module*M, std::vector<std::string>& symbols) {
  // Loop over global variables
  for (Module::global_iterator GI = M->global_begin(), GE=M->global_end(); GI != GE; ++GI)
    if (!GI->isDeclaration() && !GI->hasLocalLinkage())
      if (!GI->getName().empty())
        symbols.push_back(GI->getName());

  // Loop over functions
  for (Module::iterator FI = M->begin(), FE = M->end(); FI != FE; ++FI)
    if (!FI->isDeclaration() && !FI->hasLocalLinkage())
      if (!FI->getName().empty())
        symbols.push_back(FI->getName());

  // Loop over aliases
  for (Module::alias_iterator AI = M->alias_begin(), AE = M->alias_end();
       AI != AE; ++AI) {
    if (AI->hasName())
      symbols.push_back(AI->getName());
  }
}

// Get just the externally visible defined symbols from the bitcode
bool llvm::GetBitcodeSymbols(StringRef FileName,
                             LLVMContext& Context,
                             std::vector<std::string>& symbols,
                             std::string* ErrMsg) {
  ErrorOr<std::unique_ptr<MemoryBuffer> > BufferOrErr =
      MemoryBuffer::getFileOrSTDIN(FileName);
  if (!BufferOrErr) {
    if (ErrMsg) {
      *ErrMsg = "Could not open file '" + FileName.str() + "'" + ": " +
                BufferOrErr.getError().message();
    }
    return true;
  }

  ErrorOr<Module*> M =
      parseBitcodeFile(BufferOrErr.get()->getMemBufferRef(), Context);
  if (!M) {
    if (ErrMsg) {
      *ErrMsg = M.getError().message();
    }
    return true;
  }

  // Get the symbols
  getSymbols(M.get(), symbols);

  // Done with the module.
  delete M.get();
  return true;
}

Module*
llvm::GetBitcodeSymbols(const char *BufPtr, unsigned Length,
                        const std::string& ModuleID,
                        LLVMContext& Context,
                        std::vector<std::string>& symbols,
                        std::string* ErrMsg) {
  // Get the module.
  MemoryBufferRef Buffer(StringRef(BufPtr, Length), ModuleID.c_str());

  llvm::ErrorOr<Module*> M = parseBitcodeFile(Buffer, Context);
  if (!M) {
    if (ErrMsg) {
      *ErrMsg = M.getError().message();
    }
    return 0;
  }

  // Get the symbols
  getSymbols(M.get(), symbols);

  // Done with the module. Note that it's the caller's responsibility to delete
  // the Module.
  return M.get();
}
