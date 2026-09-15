#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace llvm {
class AllocaInst;
class Constant;
class DataLayout;
class Function;
class Type;
class Value;
}

namespace shroud {

struct VMAlloca {
  llvm::Type *allocatedType = nullptr;
  llvm::Value *arraySize = nullptr;
  uint64_t align = 0;
  unsigned reg = 0;
};

struct VMLiftResult {
  bool ok = false;
  std::string reason;
  std::vector<uint64_t> cells;
  std::vector<unsigned> argRegs;
  std::vector<VMAlloca> allocas;
  std::vector<llvm::Function *> callTargets;
  std::vector<llvm::Constant *> globalTargets;
  std::vector<std::pair<unsigned, std::string>> blocks;
  std::vector<std::string> regNames;
  unsigned nregs = 0;
};

VMLiftResult liftFunctionToVM(llvm::Function &F, const llvm::DataLayout &DL);

bool virtualizeFunction(llvm::Function &F, uint64_t moduleSeed, std::string &note);

}
