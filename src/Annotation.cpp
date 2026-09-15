#include "shroud/Annotation.h"
#include "shroud/Config.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

using namespace llvm;

namespace shroud {

static StringRef annotationString(Constant *C) {
  if (!C) return StringRef();
  auto *GV = dyn_cast<GlobalVariable>(C->stripPointerCasts());
  if (!GV || !GV->hasInitializer()) return StringRef();
  auto *Data = dyn_cast<ConstantDataArray>(GV->getInitializer());
  if (!Data || !Data->isString()) return StringRef();
  StringRef text = Data->getAsString();
  while (!text.empty() && text.back() == '\0') text = text.drop_back();
  return text;
}

FunctionAnnotations collectAnnotations(Module &M) {
  FunctionAnnotations result;
  GlobalVariable *GV = M.getNamedGlobal("llvm.global.annotations");
  if (!GV || !GV->hasInitializer()) return result;
  auto *Array = dyn_cast<ConstantArray>(GV->getInitializer());
  if (!Array) return result;
  for (Value *Operand : Array->operands()) {
    auto *Entry = dyn_cast<ConstantStruct>(Operand);
    if (!Entry || Entry->getNumOperands() < 4) continue;
    auto *F = dyn_cast<Function>(Entry->getOperand(0)->stripPointerCasts());
    if (!F) continue;
    StringRef text = annotationString(Entry->getOperand(1));
    if (text.empty()) continue;
    result[F].push_back(text.str());
  }
  return result;
}

bool hasShroudSpec(const std::vector<std::string> &annotations) {
  for (const std::string &spec : annotations)
    if (isShroudSpec(spec)) return true;
  return false;
}

std::string moduleDefaultsSpec(Module &M) {
  NamedMDNode *NMD = M.getNamedMetadata("shroud");
  if (!NMD || NMD->getNumOperands() == 0) return {};
  MDNode *Node = NMD->getOperand(0);
  if (!Node || Node->getNumOperands() == 0) return {};
  if (auto *Str = dyn_cast<MDString>(Node->getOperand(0)))
    return Str->getString().str();
  return {};
}

}
