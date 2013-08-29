//===--- SerializedModuleLoader.cpp - Import Swift modules ------*- c++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Serialization/SerializedModuleLoader.h"
#include "ModuleFile.h"
#include "swift/Subsystems.h"
#include "swift/AST/AST.h"
#include "swift/AST/Component.h"
#include "swift/AST/Diagnostics.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/SourceManager.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/system_error.h"

using namespace swift;

namespace {
typedef std::pair<Identifier, SourceLoc> AccessPathElem;
}

// Defined out-of-line so that we can see ~ModuleFile.
SerializedModuleLoader::SerializedModuleLoader(ASTContext &ctx) : Ctx(ctx) {}
SerializedModuleLoader::~SerializedModuleLoader() = default;

// FIXME: Copied from SourceLoader. Not bothering to fix until we decide that
// the source loader search path should be the same as the module loader search
// path.
static llvm::error_code findModule(ASTContext &ctx, AccessPathElem moduleID,
           llvm::OwningPtr<llvm::MemoryBuffer> &buffer){
  llvm::SmallString<64> moduleFilename(moduleID.first.str());
  moduleFilename += '.';
  moduleFilename += SERIALIZED_MODULE_EXTENSION;

  llvm::SmallString<128> inputFilename;

  // First, search in the directory corresponding to the import location.
  // FIXME: This screams for a proper FileManager abstraction.
  if (moduleID.second.isValid()) {
    unsigned currentBufferID =
        ctx.SourceMgr.findBufferContainingLoc(moduleID.second);
    const llvm::MemoryBuffer *importingBuffer
      = ctx.SourceMgr->getMemoryBuffer(currentBufferID);
    StringRef currentDirectory
      = llvm::sys::path::parent_path(importingBuffer->getBufferIdentifier());
    if (!currentDirectory.empty()) {
      inputFilename = currentDirectory;
      llvm::sys::path::append(inputFilename, moduleFilename.str());
      llvm::error_code err = llvm::MemoryBuffer::getFile(inputFilename, buffer);
      if (!err)
        return err;
    }
  }

  // Second, search in the current directory.
  llvm::error_code err = llvm::MemoryBuffer::getFile(moduleFilename, buffer);
  if (!err)
    return err;

  // If we fail, search each import search path.
  for (auto Path : ctx.ImportSearchPaths) {
    inputFilename = Path;
    llvm::sys::path::append(inputFilename, moduleFilename.str());
    err = llvm::MemoryBuffer::getFile(inputFilename, buffer);
    if (!err)
      return err;
  }

  return err;
}


Module *SerializedModuleLoader::loadModule(SourceLoc importLoc,
                                           Module::AccessPathTy path) {
  // FIXME: Swift submodules?
  if (path.size() > 1)
    return nullptr;

  auto moduleID = path[0];

  llvm::OwningPtr<llvm::MemoryBuffer> inputFile;
  // First see if we find it in the registered bitstreams.
  if (!Bitstreams.empty()) {
    // FIXME: Right now this works only with fully-qualified absolute
    // pathnames, which is incidentally what LLDB uses. Fix this to
    // support suffix matching and a search path.
    llvm::SmallString<256> spath;
    for (auto el : path)
      llvm::sys::path::append(spath, el.first.str());

    auto bs = Bitstreams.find(spath.str());
    if (bs != Bitstreams.end())
      inputFile.reset(bs->second.take());
  }

  // Otherwise look on disk.
  if (!inputFile)
    if (llvm::error_code err = findModule(Ctx, moduleID, inputFile)) {
      if (err.value() != llvm::errc::no_such_file_or_directory) {
        Ctx.Diags.diagnose(moduleID.second, diag::sema_opening_import,
                           moduleID.first.str(), err.message());
      }

      return nullptr;
    }

  assert(inputFile);
  std::string DebugModuleName = inputFile->getBufferIdentifier();

  std::unique_ptr<ModuleFile> loadedModuleFile;
  ModuleStatus err = ModuleFile::load(std::move(inputFile), loadedModuleFile);
  switch (err) {
  case ModuleStatus::Valid:
    Ctx.bumpGeneration();
    break;
  case ModuleStatus::FormatTooNew:
    Ctx.Diags.diagnose(moduleID.second, diag::serialization_module_too_new);
    loadedModuleFile.reset();
    break;
  case ModuleStatus::Malformed:
    Ctx.Diags.diagnose(moduleID.second, diag::serialization_malformed_module);
    loadedModuleFile.reset();
    break;
  case ModuleStatus::MissingDependency:
    llvm_unreachable("dependencies haven't been loaded yet");
  }

  auto comp = new (Ctx.Allocate<Component>(1)) Component();
  auto module = new (Ctx) SerializedModule(Ctx, *this, moduleID.first,
                                           DebugModuleName,
                                           comp, loadedModuleFile.get());

  // Whether we succeed or fail, don't try to load this module again.
  Ctx.LoadedModules[moduleID.first.str()] = module;

  if (loadedModuleFile) {
    bool success = loadedModuleFile->associateWithModule(module);
    if (success) {
      LoadedModuleFiles.emplace_back(std::move(loadedModuleFile),
                                     Ctx.getCurrentGeneration());
    } else {
      assert(loadedModuleFile->getStatus() == ModuleStatus::MissingDependency);

      SmallVector<ModuleFile::Dependency, 4> missing;
      std::copy_if(loadedModuleFile->getDependencies().begin(),
                   loadedModuleFile->getDependencies().end(),
                   std::back_inserter(missing),
                   [](const ModuleFile::Dependency &dependency) {
        return !dependency.isLoaded();
      });

      // FIXME: only show module part of RawAccessPath
      assert(!missing.empty() && "unknown missing dependency?");
      if (missing.size() == 1) {
        Ctx.Diags.diagnose(moduleID.second,
                           diag::serialization_missing_single_dependency,
                           missing.front().RawAccessPath);
      } else {
        llvm::SmallString<64> missingNames;
        missingNames += '\'';
        interleave(missing,
                   [&](const ModuleFile::Dependency &next) {
                     missingNames += next.RawAccessPath;
                   },
                   [&] { missingNames += "', '"; });
        missingNames += '\'';

        Ctx.Diags.diagnose(moduleID.second,
                           diag::serialization_missing_dependencies,
                           missingNames);
      }

      module->File = nullptr;
    }
  }

  return module;
}

void SerializedModuleLoader::lookupValue(Module *module,
                                         Module::AccessPathTy accessPath,
                                         Identifier name, NLKind lookupKind,
                                         SmallVectorImpl<ValueDecl*> &results) {
  assert(accessPath.size() <= 1 && "can only refer to top-level decls");

  // If this import is specific to some named type or decl ("import swift.int")
  // then filter out any lookups that don't match.
  if (accessPath.size() == 1 && accessPath.front().first != name)
    return;

  ModuleFile *moduleFile = cast<SerializedModule>(module)->File;
  if (!moduleFile)
    return;

  moduleFile->lookupValue(name, results);
}

OperatorDecl *SerializedModuleLoader::lookupOperator(Module *module,
                                                     Identifier name,
                                                     DeclKind fixity) {
  ModuleFile *moduleFile = cast<SerializedModule>(module)->File;
  if (!moduleFile)
    return nullptr;

  return moduleFile->lookupOperator(name, fixity);
}

void SerializedModuleLoader::getImportedModules(
    const Module *module,
    SmallVectorImpl<Module::ImportedModule> &imports,
    bool includePrivate) {

  ModuleFile *moduleFile = cast<SerializedModule>(module)->File;
  if (!moduleFile)
    return;

  moduleFile->getImportedModules(imports, includePrivate);
}

void
SerializedModuleLoader::lookupVisibleDecls(const Module *module,
                                           Module::AccessPathTy accessPath,
                                           VisibleDeclConsumer &consumer,
                                           NLKind lookupKind) {

  ModuleFile *moduleFile = cast<SerializedModule>(module)->File;
  if (!moduleFile)
    return;
  
  moduleFile->lookupVisibleDecls(accessPath, consumer, lookupKind);
}

void SerializedModuleLoader::loadExtensions(NominalTypeDecl *nominal,
                                            unsigned previousGeneration) {
  for (auto &modulePair : LoadedModuleFiles) {
    if (modulePair.second <= previousGeneration)
      continue;
    modulePair.first->loadExtensions(nominal);
  }
}

void
SerializedModuleLoader::loadDeclsConformingTo(KnownProtocolKind kind,
                                              unsigned previousGeneration) {
  for (auto &modulePair : LoadedModuleFiles) {
    if (modulePair.second <= previousGeneration)
      continue;
    modulePair.first->loadDeclsConformingTo(kind);
  }
}

void SerializedModuleLoader::lookupClassMembers(const Module *module,
                                                Module::AccessPathTy accessPath,
                                                VisibleDeclConsumer &consumer) {
  ModuleFile *moduleFile = cast<SerializedModule>(module)->File;
  if (!moduleFile)
    return;
  moduleFile->lookupClassMembers(accessPath, consumer);
}

void
SerializedModuleLoader::lookupClassMember(const Module *module,
                                          Module::AccessPathTy accessPath,
                                          Identifier name,
                                          SmallVectorImpl<ValueDecl*> &decls) {
  ModuleFile *moduleFile = cast<SerializedModule>(module)->File;
  if (!moduleFile)
    return;
  moduleFile->lookupClassMember(accessPath, name, decls);
}

void
SerializedModuleLoader::getLinkLibraries(const Module *module,
                                         Module::LinkLibraryCallback callback) {
  ModuleFile *moduleFile = cast<SerializedModule>(module)->File;
  if (!moduleFile)
    return;
  moduleFile->getLinkLibraries(callback);
}

void SerializedModuleLoader::getDisplayDecls(const Module *module,
                                             SmallVectorImpl<Decl*> &results) {
  ModuleFile *moduleFile = cast<SerializedModule>(module)->File;
  if (!moduleFile)
    return;
  moduleFile->getDisplayDecls(results);
}
