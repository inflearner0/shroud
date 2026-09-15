#include "shroud/RuntimeSupport.h"

#include "shroud/RNG.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include "shroud/RNG.h"

#include <cstdlib>
#include <set>
#include <string>
#include <vector>

using namespace llvm;

namespace shroud {

namespace {

struct ModuleState {
  bool cloak = false;
  bool vm = false;
  bool finalized = false;
  uint64_t moduleSeed = 0;
  std::vector<std::pair<GlobalVariable *, uint64_t>> checks;
  std::vector<std::pair<GlobalVariable *, uint64_t>> strings;
  std::set<GlobalVariable *> encrypted;
};

DenseMap<Module *, ModuleState> g_states;

constexpr uint64_t kFnvOffset = 0xCBF29CE484222325ULL;
constexpr uint64_t kFnvPrime = 0x100000001B3ULL;

uint64_t fnvCells(ArrayRef<uint64_t> cells) {
  uint64_t h = kFnvOffset;
  for (uint64_t c : cells) {
    h ^= c;
    h *= kFnvPrime;
  }
  return h;
}

uint64_t xorshiftNext(uint64_t state) {
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return state;
}

GlobalVariable *makeI64(Module &M, const char *name, uint64_t value) {
  if (GlobalVariable *existing = M.getNamedGlobal(name)) return existing;
  LLVMContext &ctx = M.getContext();
  auto *i64 = Type::getInt64Ty(ctx);
  auto *GV = new GlobalVariable(M, i64, false, GlobalValue::InternalLinkage,
                                ConstantInt::get(i64, value), name);
  GV->setAlignment(Align(8));
  return GV;
}

GlobalVariable *stringGlobalFor(const Value *v) {
  const Value *cur = v;
  while (cur) {
    if (auto *GV = dyn_cast<GlobalVariable>(cur))
      return const_cast<GlobalVariable *>(GV);
    if (auto *CE = dyn_cast<ConstantExpr>(cur)) {
      if (CE->getOpcode() == Instruction::GetElementPtr ||
          CE->getOpcode() == Instruction::BitCast) {
        cur = CE->getOperand(0);
        continue;
      }
    }
    return nullptr;
  }
  return nullptr;
}

bool encryptOne(Module &M, GlobalVariable *GV, uint64_t moduleSeed, uint64_t &keyOut,
                bool stringsOnly) {
  if (!GV->hasInitializer()) return false;
  auto *data = dyn_cast<ConstantDataArray>(GV->getInitializer());
  if (!data) return false;
  if (stringsOnly && !data->isString()) return false;
  if (!stringsOnly && !data->getElementType()->isIntegerTy()) return false;
  if (GV->getSection() == "llvm.metadata") return false;
  StringRef bytes = data->getRawDataValues();
  if (bytes.empty()) return false;

  uint64_t key = mixSeed(moduleSeed, "str:" + GV->getName().str());
  keyOut = key;
  std::vector<uint8_t> enc(bytes.size());
  uint64_t state = key;
  for (size_t i = 0; i < bytes.size(); ++i) {
    state = xorshiftNext(state);
    enc[i] = (uint8_t)((uint8_t)bytes[i] ^ (uint8_t)(state & 0xFF));
  }
  GV->setInitializer(ConstantDataArray::get(M.getContext(), enc));
  GV->setConstant(false);
  GV->setSection("");
  GV->setUnnamedAddr(GlobalValue::UnnamedAddr::None);
  return true;
}

void appendCtor(Module &M, Function *fn) {
  LLVMContext &ctx = M.getContext();
  auto *ptrTy = PointerType::get(ctx, 0);
  auto *ctorTy = StructType::get(ctx, {Type::getInt32Ty(ctx), ptrTy, ptrTy});
  auto *arrTy = ArrayType::get(ctorTy, 0);
  GlobalVariable *ctors = M.getGlobalVariable("llvm.global_ctors");
  if (!ctors)
    ctors = new GlobalVariable(M, arrTy, false, GlobalValue::AppendingLinkage,
                               ConstantAggregateZero::get(arrTy), "llvm.global_ctors");

  std::vector<Constant *> entries;
  if (Constant *init = ctors->getInitializer())
    if (auto *array = dyn_cast<ConstantArray>(init))
      for (Value *operand : array->operands())
        entries.push_back(cast<Constant>(operand));
  entries.push_back(ConstantStruct::get(ctorTy, ConstantInt::get(Type::getInt32Ty(ctx), 65535),
                                        fn, ConstantPointerNull::get(ptrTy)));
  ctors->setInitializer(ConstantArray::get(ArrayType::get(ctorTy, entries.size()), entries));
}

Value *emitDebuggerDetect(IRBuilder<> &B, Module &M) {
  LLVMContext &ctx = M.getContext();
  auto *i64 = Type::getInt64Ty(ctx);
  std::string triple = M.getTargetTriple();
  if (triple.find("windows") != std::string::npos ||
      triple.find("msvc") != std::string::npos) {
    FunctionCallee fn = M.getOrInsertFunction(
        "IsDebuggerPresent", FunctionType::get(Type::getInt32Ty(ctx), false));
    Value *call = B.CreateCall(fn);
    return B.CreateZExt(B.CreateICmpNE(call, ConstantInt::get(Type::getInt32Ty(ctx), 0)),
                        i64);
  }
  if (triple.find("linux") != std::string::npos) {
    FunctionCallee fn = M.getOrInsertFunction(
        "ptrace", FunctionType::get(i64, {Type::getInt32Ty(ctx), i64, i64, i64}, false));
    Value *call = B.CreateCall(fn, {ConstantInt::get(Type::getInt32Ty(ctx), 0),
                                    ConstantInt::get(i64, 0), ConstantInt::get(i64, 0),
                                    ConstantInt::get(i64, 0)});
    return B.CreateZExt(B.CreateICmpEQ(call, ConstantInt::get(i64, -1)), i64);
  }
  return ConstantInt::get(i64, 0);
}

Value *emitIntegrityCheck(IRBuilder<> &B, Module &M, GlobalVariable *code,
                          uint64_t expected, Value *tamper) {
  LLVMContext &ctx = M.getContext();
  auto *i64 = Type::getInt64Ty(ctx);
  auto *codeTy = cast<ArrayType>(code->getValueType());
  uint64_t count = codeTy->getNumElements();
  if (count == 0) return tamper;

  BasicBlock *pre = B.GetInsertBlock();
  BasicBlock *loop = BasicBlock::Create(ctx, "shroud.check.loop", pre->getParent());
  BasicBlock *body = BasicBlock::Create(ctx, "shroud.check.body", pre->getParent());
  BasicBlock *done = BasicBlock::Create(ctx, "shroud.check.done", pre->getParent());

  B.CreateBr(loop);
  B.SetInsertPoint(loop);
  PHINode *i = B.CreatePHI(i64, 2);
  PHINode *sum = B.CreatePHI(i64, 2);
  i->addIncoming(ConstantInt::get(i64, 0), pre);
  sum->addIncoming(ConstantInt::get(i64, kFnvOffset), pre);
  B.CreateCondBr(B.CreateICmpULT(i, ConstantInt::get(i64, count)), body, done);

  B.SetInsertPoint(body);
  Value *slot = B.CreateGEP(codeTy, code, {ConstantInt::get(i64, 0), i});
  Value *cell = B.CreateLoad(i64, slot);
  Value *mixed = B.CreateMul(B.CreateXor(sum, cell), ConstantInt::get(i64, kFnvPrime));
  Value *next = B.CreateAdd(i, ConstantInt::get(i64, 1));
  i->addIncoming(next, body);
  sum->addIncoming(mixed, body);
  B.CreateBr(loop);

  B.SetInsertPoint(done);
  Value *bad = B.CreateICmpNE(sum, ConstantInt::get(i64, expected));
  Value *flagged = B.CreateSelect(bad, ConstantInt::get(i64, 1), tamper);
  BasicBlock *after = BasicBlock::Create(ctx, "shroud.check.after", pre->getParent());
  B.CreateBr(after);
  B.SetInsertPoint(after);
  return flagged;
}

void emitStringDecrypt(IRBuilder<> &B, Module &M, GlobalVariable *gv, uint64_t key) {
  LLVMContext &ctx = M.getContext();
  auto *i64 = Type::getInt64Ty(ctx);
  auto *i8 = Type::getInt8Ty(ctx);
  auto *i8Ptr = PointerType::get(ctx, 0);
  auto *arrTy = dyn_cast<ArrayType>(gv->getValueType());
  uint64_t count = arrTy
                       ? M.getDataLayout().getTypeAllocSize(arrTy)
                       : gv->getValueType()->getPrimitiveSizeInBits() / 8;
  if (count == 0) return;

  BasicBlock *pre = B.GetInsertBlock();
  BasicBlock *loop = BasicBlock::Create(ctx, "shroud.str.loop", pre->getParent());
  BasicBlock *body = BasicBlock::Create(ctx, "shroud.str.body", pre->getParent());
  BasicBlock *done = BasicBlock::Create(ctx, "shroud.str.done", pre->getParent());
  Value *base = B.CreatePointerCast(gv, i8Ptr, "shroud.str.base");

  B.CreateBr(loop);
  B.SetInsertPoint(loop);
  PHINode *i = B.CreatePHI(i64, 2);
  PHINode *state = B.CreatePHI(i64, 2);
  i->addIncoming(ConstantInt::get(i64, 0), pre);
  state->addIncoming(ConstantInt::get(i64, key), pre);
  B.CreateCondBr(B.CreateICmpULT(i, ConstantInt::get(i64, count)), body, done);

  B.SetInsertPoint(body);
  Value *s1 = B.CreateXor(B.CreateShl(state, ConstantInt::get(i64, 13)), state);
  Value *s2 = B.CreateXor(B.CreateLShr(s1, ConstantInt::get(i64, 7)), s1);
  Value *s3 = B.CreateXor(B.CreateShl(s2, ConstantInt::get(i64, 17)), s2);
  Value *needle = B.CreateTrunc(s3, i8);
  Value *slot = B.CreateGEP(i8, base, i);
  Value *old = B.CreateLoad(i8, slot);
  B.CreateStore(B.CreateXor(old, needle), slot);
  Value *next = B.CreateAdd(i, ConstantInt::get(i64, 1));
  i->addIncoming(next, body);
  state->addIncoming(s3, body);
  B.CreateBr(loop);

  B.SetInsertPoint(done);
}

}

void setModuleSeed(Module &M, uint64_t seed) { g_states[&M].moduleSeed = seed; }

GlobalVariable *getOrCreateAnchor(Module &M, uint64_t seed) {
  uint64_t effective = seed ? seed : g_states[&M].moduleSeed;
  return makeI64(M, "shroud.anchor", mixSeed(effective, "anchor"));
}

GlobalVariable *getOrCreateExpected(Module &M) { return makeI64(M, "shroud.expected", 0); }

GlobalVariable *getOrCreateTamper(Module &M) { return makeI64(M, "shroud.tamper", 0); }

GlobalVariable *getOrCreateDecoy(Module &M) { return makeI64(M, "shroud.decoy", 0); }

GlobalVariable *getOrCreateReady(Module &M) { return makeI64(M, "shroud.ready", 0); }

Value *buildOpaqueTrue(IRBuilderBase &B, Module &M, RNG &rng) {
  LLVMContext &ctx = M.getContext();
  auto *i64 = Type::getInt64Ty(ctx);
  GlobalVariable *anchor = getOrCreateAnchor(M, 0);
  GlobalVariable *expected = getOrCreateExpected(M);
  Value *a = B.CreateLoad(i64, anchor, "shroud.anchor.load");
  Value *b = B.CreateLoad(i64, expected, "shroud.expected.load");
  switch (rng.next32() % 4) {
  case 0:
    return B.CreateICmpEQ(a, b);
  case 1:
    return B.CreateICmpEQ(B.CreateXor(a, b), ConstantInt::get(i64, 0));
  case 2:
    return B.CreateICmpEQ(B.CreateOr(a, b), B.CreateAnd(a, b));
  default:
    return B.CreateICmpEQ(B.CreateSub(a, b), ConstantInt::get(i64, 0));
  }
}

void noteCloakUsed(Module &M) { g_states[&M].cloak = true; }

void noteVmUsed(Module &M) { g_states[&M].vm = true; }

void registerBytecodeCheck(Module &M, GlobalVariable *code, uint64_t expected) {
  g_states[&M].checks.push_back({code, expected});
}

uint64_t bytecodeChecksum(ArrayRef<uint64_t> cells) { return fnvCells(cells); }

static unsigned encryptForFunction(Function &F, Module &M, uint64_t seed,
                                   bool stringsOnly) {
  ModuleState &st = g_states[&M];
  unsigned count = 0;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      for (Use &U : I.operands()) {
        auto *C = dyn_cast<Constant>(U.get());
        if (!C) continue;
        GlobalVariable *GV = stringGlobalFor(C);
        if (!GV || st.encrypted.count(GV)) continue;
        uint64_t key = 0;
        if (!encryptOne(M, GV, seed, key, stringsOnly)) continue;
        st.encrypted.insert(GV);
        st.strings.push_back({GV, key});
        ++count;
      }
    }
  }
  return count;
}

unsigned encryptStringsInFunction(Function &F, Module &M, uint64_t seed) {
  return encryptForFunction(F, M, seed, true);
}

unsigned encryptConstantsInFunction(Function &F, Module &M, uint64_t seed) {
  return encryptForFunction(F, M, seed, false);
}

void finalizeShroudRuntime(Module &M) {
  ModuleState &st = g_states[&M];
  if (st.finalized) return;
  st.finalized = true;
  if (!st.cloak && !st.vm && st.checks.empty() && st.strings.empty()) return;
  if (M.getFunction("shroud.init")) return;

  bool tamperEnabled = true;
  if (const char *off = std::getenv("SHROUD_NO_TAMPER"))
    if (off[0] != '0' && off[0] != '\0') tamperEnabled = false;

  LLVMContext &ctx = M.getContext();
  auto *i64 = Type::getInt64Ty(ctx);
  auto *voidTy = Type::getVoidTy(ctx);
  Function *fn = Function::Create(FunctionType::get(voidTy, false),
                                  GlobalValue::InternalLinkage, "shroud.init", &M);
  IRBuilder<> B(BasicBlock::Create(ctx, "entry", fn));

  for (const auto &item : st.strings)
    emitStringDecrypt(B, M, item.first, item.second);

  Value *tamper =
      tamperEnabled ? emitDebuggerDetect(B, M) : ConstantInt::get(i64, 0);
  if (tamperEnabled)
    for (const auto &check : st.checks)
      tamper = emitIntegrityCheck(B, M, check.first, check.second, tamper);

  B.CreateStore(tamper, getOrCreateTamper(M));

  if (st.cloak) {
    GlobalVariable *anchor = getOrCreateAnchor(M, 0);
    GlobalVariable *expected = getOrCreateExpected(M);
    Value *value = B.CreateLoad(i64, anchor);
    B.CreateStore(B.CreateXor(value, tamper), expected);
  }

  B.CreateStore(ConstantInt::get(i64, kReadyMagic), getOrCreateReady(M));
  B.CreateRetVoid();
  appendCtor(M, fn);
}

}
