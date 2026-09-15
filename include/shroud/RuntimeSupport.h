#pragma once

#include <cstdint>

#include "llvm/ADT/ArrayRef.h"

namespace llvm {
class Function;
class GlobalVariable;
class IRBuilderBase;
class Module;
class Value;
}

namespace shroud {

class RNG;

constexpr uint64_t kReadyMagic = 0x5A17C0DE5A17C0DEULL;

uint64_t bytecodeChecksum(llvm::ArrayRef<uint64_t> cells);

void setModuleSeed(llvm::Module &M, uint64_t seed);

llvm::Value *buildOpaqueTrue(llvm::IRBuilderBase &B, llvm::Module &M, RNG &rng);

llvm::GlobalVariable *getOrCreateAnchor(llvm::Module &M, uint64_t seed);
llvm::GlobalVariable *getOrCreateExpected(llvm::Module &M);
llvm::GlobalVariable *getOrCreateTamper(llvm::Module &M);
llvm::GlobalVariable *getOrCreateDecoy(llvm::Module &M);
llvm::GlobalVariable *getOrCreateReady(llvm::Module &M);

void noteCloakUsed(llvm::Module &M);
void noteVmUsed(llvm::Module &M);

void registerBytecodeCheck(llvm::Module &M, llvm::GlobalVariable *code, uint64_t expected);

unsigned encryptStringsInFunction(llvm::Function &F, llvm::Module &M, uint64_t seed);
unsigned encryptConstantsInFunction(llvm::Function &F, llvm::Module &M, uint64_t seed);

void finalizeShroudRuntime(llvm::Module &M);

}
