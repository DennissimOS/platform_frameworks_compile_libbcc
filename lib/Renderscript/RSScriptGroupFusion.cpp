/*
 * Copyright 2015, The Android Open Source Project
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

#include "bcc/Renderscript/RSScriptGroupFusion.h"

#include "bcc/Assert.h"
#include "bcc/BCCContext.h"
#include "bcc/Source.h"
#include "bcc/Support/Log.h"
#include "bcinfo/MetadataExtractor.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

using llvm::Function;
using llvm::Module;

using std::string;

namespace bcc {

namespace {

const Function* getInvokeFunction(const Source& source, const int slot,
                                  Module* newModule) {
  Module* module = const_cast<Module*>(&source.getModule());
  bcinfo::MetadataExtractor metadata(module);
  if (!metadata.extract()) {
    return nullptr;
  }
  const char* functionName = metadata.getExportFuncNameList()[slot];
  Function* func = newModule->getFunction(functionName);
  // Materialize the function so that later the caller can inspect its argument
  // and return types.
  newModule->materialize(func);
  return func;
}

const Function*
getFunction(Module* mergedModule, const Source* source, const int slot,
            uint32_t* signature) {
  bcinfo::MetadataExtractor metadata(&source->getModule());
  metadata.extract();

  const char* functionName = metadata.getExportForEachNameList()[slot];
  if (functionName == nullptr || !functionName[0]) {
    return nullptr;
  }

  if (metadata.getExportForEachInputCountList()[slot] > 1) {
    // TODO: Handle multiple inputs.
    ALOGW("Kernel %s has multiple inputs", functionName);
    return nullptr;
  }

  if (signature != nullptr) {
    *signature = metadata.getExportForEachSignatureList()[slot];
  }

  const Function* function = mergedModule->getFunction(functionName);

  return function;
}

// TODO: Handle the context argument
constexpr uint32_t ExpectedSignatureBits =
        bcinfo::MD_SIG_In |
        bcinfo::MD_SIG_Out |
        bcinfo::MD_SIG_X |
        bcinfo::MD_SIG_Y |
        bcinfo::MD_SIG_Z |
        bcinfo::MD_SIG_Kernel;

int getFusedFuncSig(const std::vector<Source*>& sources,
                    const std::vector<int>& slots,
                    uint32_t* retSig) {
  *retSig = 0;
  uint32_t firstSignature = 0;
  uint32_t signature = 0;
  auto slotIter = slots.begin();
  for (const Source* source : sources) {
    const int slot = *slotIter++;
    bcinfo::MetadataExtractor metadata(&source->getModule());
    metadata.extract();

    if (metadata.getExportForEachInputCountList()[slot] > 1) {
      // TODO: Handle multiple inputs in kernel fusion.
      ALOGW("Kernel %d in source %p has multiple inputs", slot, source);
      return -1;
    }

    signature = metadata.getExportForEachSignatureList()[slot];
    if (signature & ~ExpectedSignatureBits) {
      ALOGW("Unexpected signature %x seen while fusing kernels", signature);
      return -1;
    }

    if (firstSignature == 0) {
      firstSignature = signature;
    }

    *retSig |= signature;
  }

  if (!bcinfo::MetadataExtractor::hasForEachSignatureIn(firstSignature)) {
    *retSig &= ~bcinfo::MD_SIG_In;
  }

  if (!bcinfo::MetadataExtractor::hasForEachSignatureOut(signature)) {
    *retSig &= ~bcinfo::MD_SIG_Out;
  }

  return 0;
}

llvm::FunctionType* getFusedFuncType(bcc::BCCContext& Context,
                                     const std::vector<Source*>& sources,
                                     const std::vector<int>& slots,
                                     Module* M,
                                     uint32_t* signature) {
  int error = getFusedFuncSig(sources, slots, signature);

  if (error < 0) {
    return nullptr;
  }

  const Function* firstF = getFunction(M, sources.front(), slots.front(), nullptr);

  bccAssert (firstF != nullptr);

  llvm::SmallVector<llvm::Type*, 8> ArgTys;

  if (bcinfo::MetadataExtractor::hasForEachSignatureIn(*signature)) {
    ArgTys.push_back(firstF->arg_begin()->getType());
  }

  llvm::Type* I32Ty = llvm::IntegerType::get(Context.getLLVMContext(), 32);
  if (bcinfo::MetadataExtractor::hasForEachSignatureX(*signature)) {
    ArgTys.push_back(I32Ty);
  }
  if (bcinfo::MetadataExtractor::hasForEachSignatureY(*signature)) {
    ArgTys.push_back(I32Ty);
  }
  if (bcinfo::MetadataExtractor::hasForEachSignatureZ(*signature)) {
    ArgTys.push_back(I32Ty);
  }

  const Function* lastF = getFunction(M, sources.back(), slots.back(), nullptr);

  bccAssert (lastF != nullptr);

  llvm::Type* retTy = lastF->getReturnType();

  return llvm::FunctionType::get(retTy, ArgTys, false);
}

}  // anonymous namespace

bool fuseKernels(bcc::BCCContext& Context,
                 const std::vector<Source *>& sources,
                 const std::vector<int>& slots,
                 const std::string& fusedName,
                 Module* mergedModule) {
  bccAssert(sources.size() == slots.size() && "sources and slots differ in size");

  uint32_t signature;

  llvm::FunctionType* fusedType =
          getFusedFuncType(Context, sources, slots, mergedModule, &signature);

  if (fusedType == nullptr) {
    return false;
  }

  Function* fusedKernel =
          (Function*)(mergedModule->getOrInsertFunction(fusedName, fusedType));

  llvm::LLVMContext& ctxt = Context.getLLVMContext();

  llvm::BasicBlock* block = llvm::BasicBlock::Create(ctxt, "entry", fusedKernel);
  llvm::IRBuilder<> builder(block);

  Function::arg_iterator argIter = fusedKernel->arg_begin();

  llvm::Value* dataElement = nullptr;
  if (bcinfo::MetadataExtractor::hasForEachSignatureIn(signature)) {
    dataElement = argIter++;
    dataElement->setName("DataIn");
  }

  llvm::Value* X = nullptr;
  if (bcinfo::MetadataExtractor::hasForEachSignatureX(signature)) {
      X = argIter++;
      X->setName("x");
  }

  llvm::Value* Y = nullptr;
  if (bcinfo::MetadataExtractor::hasForEachSignatureY(signature)) {
      Y = argIter++;
      Y->setName("y");
  }

  llvm::Value* Z = nullptr;
  if (bcinfo::MetadataExtractor::hasForEachSignatureZ(signature)) {
      Z = argIter++;
      Z->setName("z");
  }

  auto slotIter = slots.begin();
  for (const Source* source : sources) {
    int slot = *slotIter++;

    uint32_t signature;
    const Function* function = getFunction(mergedModule, source, slot, &signature);

    if (function == nullptr) {
      return false;
    }

    std::vector<llvm::Value*> args;
    if (dataElement != nullptr) {
      args.push_back(dataElement);
    }

    // TODO: Handle the context argument

    if (bcinfo::MetadataExtractor::hasForEachSignatureX(signature)) {
      args.push_back(X);
    }

    if (bcinfo::MetadataExtractor::hasForEachSignatureY(signature)) {
      args.push_back(Y);
    }

    if (bcinfo::MetadataExtractor::hasForEachSignatureZ(signature)) {
      args.push_back(Z);
    }

    dataElement = builder.CreateCall((llvm::Value*)function, args);
  }

  if (fusedKernel->getReturnType()->isVoidTy()) {
    builder.CreateRetVoid();
  } else {
    builder.CreateRet(dataElement);
  }

  llvm::NamedMDNode* ExportForEachNameMD =
    mergedModule->getOrInsertNamedMetadata("#rs_export_foreach_name");

  llvm::MDString* nameMDStr = llvm::MDString::get(ctxt, fusedName);
  llvm::MDNode* nameMDNode = llvm::MDNode::get(ctxt, nameMDStr);
  ExportForEachNameMD->addOperand(nameMDNode);

  llvm::NamedMDNode* ExportForEachMD =
    mergedModule->getOrInsertNamedMetadata("#rs_export_foreach");
  llvm::MDString* sigMDStr = llvm::MDString::get(ctxt,
                                                 llvm::utostr_32(signature));
  llvm::MDNode* sigMDNode = llvm::MDNode::get(ctxt, sigMDStr);
  ExportForEachMD->addOperand(sigMDNode);

  return true;
}

bool renameInvoke(BCCContext& Context, const Source* source, const int slot,
                  const std::string& newName, Module* module) {
  const llvm::Function* F = getInvokeFunction(*source, slot, module);
  std::vector<llvm::Type*> params;
  for (auto I = F->arg_begin(), E = F->arg_end(); I != E; ++I) {
    params.push_back(I->getType());
  }
  llvm::Type* returnTy = F->getReturnType();

  llvm::FunctionType* batchFuncTy =
          llvm::FunctionType::get(returnTy, params, false);

  llvm::Function* newF =
          llvm::Function::Create(batchFuncTy,
                                 llvm::GlobalValue::ExternalLinkage, newName,
                                 module);

  llvm::BasicBlock* block = llvm::BasicBlock::Create(Context.getLLVMContext(),
                                                     "entry", newF);
  llvm::IRBuilder<> builder(block);

  llvm::Function::arg_iterator argIter = newF->arg_begin();
  llvm::Value* arg1 = argIter++;
  builder.CreateCall((llvm::Value*)F, arg1);

  builder.CreateRetVoid();

  llvm::NamedMDNode* ExportFuncNameMD =
          module->getOrInsertNamedMetadata("#rs_export_func");
  llvm::MDString* strMD = llvm::MDString::get(module->getContext(), newName);
  llvm::MDNode* nodeMD = llvm::MDNode::get(module->getContext(), strMD);
  ExportFuncNameMD->addOperand(nodeMD);

  return true;
}

}  // namespace bcc
