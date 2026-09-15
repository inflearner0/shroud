#include "shroud/VMVirtualizer.h"

#include "shroud/RNG.h"
#include "shroud/RuntimeSupport.h"
#include "shroud/VMBytecode.h"
#include "shroud/VMRuntimeData.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace llvm;

namespace shroud {

static uint64_t encodeInsn(unsigned op, unsigned dst, unsigned a, unsigned b, int64_t imm) {
  uint64_t raw = (uint64_t)(uint32_t)imm;
  return (uint64_t)(op & 0xFFu) | ((uint64_t)(dst & 0xFFu) << 8) |
         ((uint64_t)(a & 0xFFu) << 16) | ((uint64_t)(b & 0xFFu) << 24) |
         (raw << 32);
}

static uint64_t vmMix(uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

static std::string typeString(Type *T) {
  std::string s;
  raw_string_ostream os(s);
  T->print(os);
  return s;
}

class Lifter {
public:
  Lifter(Function &F, const DataLayout &DL, RNG &rng)
      : F(F), M(*F.getParent()), DL(DL), rng(rng) {}

  VMLiftResult run() {
    if (F.isVarArg()) fail("vararg function");
    FunctionType *FT = F.getFunctionType();
    Type *RT = FT->getReturnType();
    if (!RT->isVoidTy() && !isLiftableType(RT)) fail("unsupported return type");
    for (Type *PT : FT->params())
      if (!isLiftableType(PT)) fail("unsupported parameter type");
    if (failed) return finish();

    for (Argument &A : F.args()) {
      unsigned R = allocReg();
      regs[&A] = R;
      regNames[R] = "arg" + std::to_string(A.getArgNo());
      result.argRegs.push_back(R);
    }

    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (failed) break;
        prepare(I);
      }
      if (failed) break;
    }
    if (failed) return finish();

    for (const Constant *C : constList) {
      if (failed) break;
      ensureMaterialized(C);
    }
    if (failed) return finish();

    for (BasicBlock &BB : F) {
      if (failed) break;
      currentBlock = (unsigned)blockOrder.size();
      blockIndex[&BB] = currentBlock;
      blockOrder.push_back(&BB);
      blockCode.emplace_back();
      out = &blockCode.back();
      emitBlock(BB);
    }
    if (failed) return finish();

    std::vector<uint64_t> cells = prologue;
    std::vector<uint64_t> starts(blockOrder.size(), 0);
    for (size_t i = 0; i < blockOrder.size(); ++i) {
      starts[i] = cells.size();
      for (uint64_t C : blockCode[i]) cells.push_back(C);
    }
    for (const Patch &P : patches) {
      auto it = blockIndex.find(P.target);
      if (it == blockIndex.end()) {
        fail("branch target missing");
        break;
      }
      size_t idx = (size_t)starts[P.block] + P.cell;
      uint64_t target = starts[it->second];
      cells[idx] = (cells[idx] & 0xFFFFFFFFULL) | (target << 32);
    }
    for (const StubPatch &P : stubPatches) {
      size_t idx = (size_t)starts[P.block] + P.cell;
      uint64_t target = starts[P.block] + P.stubCell;
      cells[idx] = (cells[idx] & 0xFFFFFFFFULL) | (target << 32);
    }
    if (failed) return finish();

    result.cells = std::move(cells);
    for (size_t i = 0; i < blockOrder.size(); ++i)
      result.blocks.push_back({(unsigned)starts[i], blockOrder[i]->getName().str()});
    result.ok = true;
    return finish();
  }

private:
  struct GEPPlan {
    int64_t constOffset = 0;
    std::vector<std::pair<unsigned, int64_t>> varTerms;
  };
  struct Patch {
    unsigned block;
    unsigned cell;
    const BasicBlock *target;
  };
  struct StubPatch {
    unsigned block;
    unsigned cell;
    unsigned stubCell;
  };

  Function &F;
  Module &M;
  const DataLayout &DL;
  RNG &rng;
  VMLiftResult result;
  bool failed = false;
  std::string failReason;

  DenseMap<const Value *, unsigned> regs;
  std::set<const Constant *> constSeen;
  std::vector<const Constant *> constList;
  DenseMap<const Constant *, unsigned> materialized;
  DenseMap<const Constant *, unsigned> globalIndex;
  std::map<const GetElementPtrInst *, GEPPlan> gepPlans;
  DenseMap<Function *, unsigned> callIndex;

  std::vector<uint64_t> prologue;
  std::vector<uint64_t> *out = &prologue;
  std::vector<std::vector<uint64_t>> blockCode;
  std::vector<const BasicBlock *> blockOrder;
  DenseMap<const BasicBlock *, unsigned> blockIndex;
  std::vector<Patch> patches;
  std::vector<StubPatch> stubPatches;
  unsigned currentBlock = 0;
  unsigned nextReg = 0;
  unsigned nextTemp = SHROUD_VM_TEMP_BASE;
  std::vector<std::string> regNames;

  void fail(const std::string &why) {
    if (!failed) {
      failed = true;
      failReason = why;
    }
  }

  VMLiftResult finish() {
    result.nregs = nextReg;
    result.regNames = regNames;
    if (failed) {
      result.ok = false;
      result.reason = failReason;
    }
    return std::move(result);
  }

  bool isLiftableType(Type *T) const { return T->isIntegerTy() || T->isPointerTy(); }

  unsigned allocReg() {
    if (nextReg >= SHROUD_VM_TEMP_BASE) {
      fail("register budget exceeded");
      return 0;
    }
    unsigned r = nextReg++;
    regNames.emplace_back();
    return r;
  }

  unsigned allocTemp() {
    if (nextTemp >= SHROUD_VM_SINK_REG) {
      fail("temporary register budget exceeded");
      return SHROUD_VM_TEMP_BASE;
    }
    return nextTemp++;
  }

  unsigned intWidth(Type *T) const {
    return T->isIntegerTy() ? T->getIntegerBitWidth() : 64u;
  }

  void emitCell(uint64_t C) { out->push_back(C); }

  void emit(unsigned op, unsigned dst, unsigned a, unsigned b, int64_t imm) {
    emitCell(encodeInsn(op, dst, a, b, imm));
  }

  void emitConst(unsigned dst, uint64_t value, unsigned width) {
    if (width > 32) {
      emitCell(encodeInsn(SHROUD_VM_CONST64, dst, 0, 0, (int64_t)(uint32_t)value));
      emitCell(encodeInsn(0, 0, 0, 0, (int64_t)(uint32_t)(value >> 32)));
    } else {
      emit(SHROUD_VM_CONST, dst, 0, 0, (int64_t)(uint32_t)value);
    }
  }

  void noteConstant(const Constant *C) {
    Type *T = C->getType();
    if (T->isTokenTy() || T->isMetadataTy()) return;
    if (constSeen.insert(C).second) constList.push_back(C);
  }

  unsigned regOf(const Value *V) {
    auto it = regs.find(V);
    if (it != regs.end()) return it->second;
    if (auto *C = dyn_cast<Constant>(V)) return ensureMaterialized(C);
    fail("value has no virtual register");
    return 0;
  }

  unsigned globalIndexFor(const Constant *C) {
    auto it = globalIndex.find(C);
    if (it != globalIndex.end()) return it->second;
    unsigned idx = (unsigned)result.globalTargets.size();
    result.globalTargets.push_back(const_cast<Constant *>(C));
    globalIndex[C] = idx;
    return idx;
  }

  unsigned ensureMaterialized(const Constant *C) {
    auto it = materialized.find(C);
    if (it != materialized.end()) return it->second;
    if (auto rit = regs.find(C); rit != regs.end()) {
      materialized[C] = rit->second;
      return rit->second;
    }
    unsigned r = allocReg();
    regNames[r] = "const";
    Type *T = C->getType();
    if (auto *CI = dyn_cast<ConstantInt>(C)) {
      materialized[C] = r;
      regs[C] = r;
      emitConst(r, CI->getValue().getZExtValue(), T->getIntegerBitWidth());
      return r;
    }
    if (isa<ConstantPointerNull>(C) || isa<ConstantAggregateZero>(C) ||
        isa<UndefValue>(C) || isa<PoisonValue>(C)) {
      materialized[C] = r;
      regs[C] = r;
      emitConst(r, 0, 64);
      return r;
    }
    if (isa<GlobalValue>(C)) {
      auto *GV = cast<GlobalValue>(C);
      if (auto *Fn = dyn_cast<Function>(GV)) {
        if (Fn->isIntrinsic()) {
          fail("intrinsic used as value: " + Fn->getName().str());
          return r;
        }
      }
      materialized[C] = r;
      regs[C] = r;
      emit(SHROUD_VM_LOADADDR, r, 0, 0, (int64_t)globalIndexFor(C));
      return r;
    }
    if (auto *CE = dyn_cast<ConstantExpr>(C)) {
      switch (CE->getOpcode()) {
      case Instruction::GetElementPtr: {
        auto *GEP = cast<GEPOperator>(CE);
        APInt off(DL.getIndexTypeSizeInBits(CE->getType()), 0);
        if (!GEP->accumulateConstantOffset(DL, off)) {
          fail("unsupported constant GEP");
          return r;
        }
        unsigned base = ensureMaterialized(CE->getOperand(0));
        if (failed) return r;
        materialized[C] = r;
        regs[C] = r;
        emit(SHROUD_VM_GEP_IMM, r, base, 0, off.getSExtValue());
        return r;
      }
      case Instruction::BitCast:
      case Instruction::PtrToInt:
      case Instruction::IntToPtr:
      case Instruction::AddrSpaceCast: {
        unsigned inner = ensureMaterialized(CE->getOperand(0));
        if (failed) return r;
        materialized[C] = inner;
        regs[C] = inner;
        return inner;
      }
      default:
        fail("unsupported constant expression");
        return r;
      }
    }
    fail("unsupported constant kind");
    return r;
  }

  bool analyzeGEP(const GetElementPtrInst *G, GEPPlan &plan) {
    Type *cur = G->getSourceElementType();
    unsigned n = G->getNumIndices();
    const DataLayout &L = DL;
    for (unsigned k = 0; k < n; ++k) {
      const Value *idx = G->getOperand(1 + k);
      if (cur->isStructTy()) {
        auto *ST = cast<StructType>(cur);
        auto *CI = dyn_cast<ConstantInt>(idx);
        if (k == 0 && !CI) {
          plan.varTerms.push_back(
              {regOf(idx), (int64_t)L.getTypeAllocSize(cur).getFixedValue()});
          continue;
        }
        if (!CI) {
          fail("non-constant struct index");
          return false;
        }
        unsigned fi = (unsigned)CI->getZExtValue();
        if (k == 0) {
          if (fi != 0) {
            fail("first struct index not zero");
            return false;
          }
          continue;
        }
        plan.constOffset += (int64_t)L.getStructLayout(ST)->getElementOffset(fi);
        cur = ST->getElementType(fi);
        continue;
      }
      if (cur->isVectorTy()) {
        fail("vector GEP");
        return false;
      }
      if (cur->isArrayTy()) {
        if (k == 0) {
          uint64_t scale = L.getTypeAllocSize(cur).getFixedValue();
          if (auto *CI = dyn_cast<ConstantInt>(idx)) {
            plan.constOffset += CI->getSExtValue() * (int64_t)scale;
          } else {
            plan.varTerms.push_back({regOf(idx), (int64_t)scale});
          }
          continue;
        }
        Type *elem = cur->getArrayElementType();
        uint64_t stride = L.getTypeAllocSize(elem).getFixedValue();
        if (auto *CI = dyn_cast<ConstantInt>(idx)) {
          plan.constOffset += CI->getSExtValue() * (int64_t)stride;
        } else {
          plan.varTerms.push_back({regOf(idx), (int64_t)stride});
        }
        cur = elem;
        continue;
      }
      uint64_t scale = L.getTypeAllocSize(cur).getFixedValue();
      if (auto *CI = dyn_cast<ConstantInt>(idx)) {
        plan.constOffset += CI->getSExtValue() * (int64_t)scale;
      } else {
        plan.varTerms.push_back({regOf(idx), (int64_t)scale});
      }
      if (k + 1 < n) {
        fail("GEP indexes past scalar");
        return false;
      }
    }
    return true;
  }

  void prepare(const Instruction &I) {
    const CallInst *CB = dyn_cast<CallInst>(&I);
    for (unsigned i = 0; i < I.getNumOperands(); ++i) {
      if (CB && CB->getCalledOperand() == I.getOperand(i)) continue;
      if (auto *C = dyn_cast<Constant>(I.getOperand(i))) noteConstant(C);
    }
    if (auto *SW = dyn_cast<SwitchInst>(&I))
      for (auto &Case : SW->cases()) noteConstant(Case.getCaseValue());

    if (auto *PHI = dyn_cast<PHINode>(&I)) {
      if (!isLiftableType(PHI->getType())) {
        fail("phi type");
        return;
      }
      assignResult(&I);
      return;
    }
    if (auto *AI = dyn_cast<AllocaInst>(&I)) {
      if (!isa<ConstantInt>(AI->getArraySize())) {
        fail("dynamic alloca");
        return;
      }
      if (!AI->getAllocatedType()->isSized()) {
        fail("unsized alloca");
        return;
      }
      assignResult(&I);
      regNames[regs[AI]] = "alloca";
      VMAlloca info;
      info.allocatedType = AI->getAllocatedType();
      info.arraySize = const_cast<Value *>(AI->getArraySize());
      info.align = AI->getAlign().value();
      info.reg = regs[AI];
      result.allocas.push_back(info);
      return;
    }
    if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
      if (!isLiftableType(BO->getType()) || !BO->getType()->isIntegerTy() ||
          !isLiftableType(BO->getOperand(0)->getType()) ||
          !isLiftableType(BO->getOperand(1)->getType())) {
        fail("binary operator type");
        return;
      }
      switch (BO->getOpcode()) {
      case Instruction::Add:
      case Instruction::Sub:
      case Instruction::Mul:
      case Instruction::UDiv:
      case Instruction::SDiv:
      case Instruction::URem:
      case Instruction::SRem:
      case Instruction::And:
      case Instruction::Or:
      case Instruction::Xor:
      case Instruction::Shl:
      case Instruction::LShr:
      case Instruction::AShr:
        break;
      default:
        fail("binary operator");
        return;
      }
      assignResult(&I);
      return;
    }
    if (auto *CI = dyn_cast<ICmpInst>(&I)) {
      if (!isLiftableType(CI->getOperand(0)->getType()) ||
          !isLiftableType(CI->getOperand(1)->getType())) {
        fail("icmp operand type");
        return;
      }
      assignResult(&I);
      return;
    }
    if (auto *SI = dyn_cast<SelectInst>(&I)) {
      if (!isLiftableType(SI->getType()) || !isLiftableType(SI->getTrueValue()->getType()) ||
          !SI->getCondition()->getType()->isIntegerTy(1)) {
        fail("select type");
        return;
      }
      assignResult(&I);
      return;
    }
    if (auto *CI = dyn_cast<CastInst>(&I)) {
      switch (CI->getOpcode()) {
      case Instruction::Trunc:
      case Instruction::ZExt:
      case Instruction::SExt:
      case Instruction::BitCast:
      case Instruction::PtrToInt:
      case Instruction::IntToPtr:
      case Instruction::AddrSpaceCast:
        break;
      default:
        fail("cast instruction");
        return;
      }
      if (!isLiftableType(CI->getType()) || !isLiftableType(CI->getOperand(0)->getType())) {
        fail("cast type");
        return;
      }
      assignResult(&I);
      return;
    }
    if (auto *G = dyn_cast<GetElementPtrInst>(&I)) {
      GEPPlan plan;
      if (!analyzeGEP(G, plan)) return;
      gepPlans[G] = plan;
      assignResult(&I);
      return;
    }
    if (auto *L = dyn_cast<LoadInst>(&I)) {
      if (!isLiftableType(L->getType())) {
        fail("load type: " + typeString(L->getType()));
        return;
      }
      assignResult(&I);
      return;
    }
    if (auto *S = dyn_cast<StoreInst>(&I)) {
      if (!isLiftableType(S->getValueOperand()->getType()) ||
          !S->getPointerOperand()->getType()->isPointerTy()) {
        fail("store type: " + typeString(S->getValueOperand()->getType()));
        return;
      }
      return;
    }
    if (auto *R = dyn_cast<ReturnInst>(&I)) {
      if (R->getReturnValue() && !isLiftableType(R->getReturnValue()->getType())) {
        fail("return type");
        return;
      }
      return;
    }
    if (auto *B = dyn_cast<BranchInst>(&I)) {
      if (B->isConditional() && !B->getCondition()->getType()->isIntegerTy(1)) {
        fail("branch condition type");
        return;
      }
      return;
    }
    if (auto *SW = dyn_cast<SwitchInst>(&I)) {
      if (!SW->getCondition()->getType()->isIntegerTy()) {
        fail("switch condition type");
        return;
      }
      return;
    }
    if (isa<UnreachableInst>(&I)) return;
    if (auto *CB = dyn_cast<CallInst>(&I)) {
      prepareCall(CB);
      return;
    }
    fail(std::string("unsupported instruction: ") + I.getOpcodeName());
  }

  void assignResult(const Instruction *I) {
    if (!I->getType()->isVoidTy()) {
      unsigned r = allocReg();
      regs[I] = r;
      regNames[r] = I->hasName() ? I->getName().str()
                                 : (std::string(I->getOpcodeName()) + " @bb");
    }
  }

  void prepareCall(const CallInst *CB) {
    if (CB->isInlineAsm()) {
      fail("inline asm");
      return;
    }
    if (CB->getFunctionType()->isVarArg()) {
      fail("vararg call");
      return;
    }
    for (unsigned i = 0; i < CB->arg_size(); ++i) {
      if (CB->paramHasAttr(i, Attribute::ByVal) ||
          CB->paramHasAttr(i, Attribute::StructRet) ||
          CB->paramHasAttr(i, Attribute::InAlloca)) {
        fail("aggregate call argument");
        return;
      }
      if (!isLiftableType(CB->getArgOperand(i)->getType())) {
        fail("call argument type");
        return;
      }
    }
    if (!CB->getType()->isVoidTy() && !isLiftableType(CB->getType())) {
      fail("call result type");
      return;
    }
    if (CB->arg_size() > 8) {
      fail("too many call arguments");
      return;
    }
    if (Function *callee = CB->getCalledFunction()) {
      if (callee->isIntrinsic()) {
        StringRef name = callee->getName();
        if (name.starts_with("llvm.memcpy") || name.starts_with("llvm.memmove")) {
          if (CB->arg_size() != 4 || !cast<ConstantInt>(CB->getArgOperand(3))->isZero()) {
            fail("unsupported memcpy form");
            return;
          }
        } else if (name.starts_with("llvm.memset")) {
          if (CB->arg_size() != 4 || !cast<ConstantInt>(CB->getArgOperand(3))->isZero()) {
            fail("unsupported memset form");
            return;
          }
        } else if (name.starts_with("llvm.fshl") || name.starts_with("llvm.fshr")) {
          if (CB->arg_size() != 3 || !CB->getArgOperand(0)->getType()->isIntegerTy(64) ||
              !isa<ConstantInt>(CB->getArgOperand(2))) {
            fail("unsupported funnel shift");
            return;
          }
        } else if (name.starts_with("llvm.bswap")) {
          unsigned w = CB->getType()->isIntegerTy() ? CB->getType()->getIntegerBitWidth() : 0;
          if (w != 16 && w != 32 && w != 64) {
            fail("unsupported bswap width");
            return;
          }
        } else if (name.starts_with("llvm.lifetime") || name.starts_with("llvm.dbg") ||
                   name.starts_with("llvm.assume") ||
                   name.starts_with("llvm.experimental.noalias.scope.decl")) {
          return;
        } else {
          fail("unsupported intrinsic");
          return;
        }
        assignResult(CB);
        return;
      }
      FunctionType *FT = callee->getFunctionType();
      if (FT->isVarArg()) {
        fail("vararg callee");
        return;
      }
      if (!FT->getReturnType()->isVoidTy() && !isLiftableType(FT->getReturnType())) {
        fail("callee return type");
        return;
      }
      for (Type *PT : FT->params()) {
        if (!isLiftableType(PT)) {
          fail("callee parameter type");
          return;
        }
      }
      if (!callIndex.count(callee)) {
        callIndex[callee] = (unsigned)result.callTargets.size();
        result.callTargets.push_back(callee);
      }
    } else {
      if (!isLiftableType(CB->getCalledOperand()->getType())) {
        fail("indirect callee type");
        return;
      }
    }
    assignResult(CB);
  }

  void emitBlock(BasicBlock &BB) {
    std::vector<Instruction *> insts;
    for (Instruction &I : BB) insts.push_back(&I);
    for (size_t idx = 0; idx < insts.size(); ++idx) {
      Instruction &I = *insts[idx];
      nextTemp = SHROUD_VM_TEMP_BASE;
      if (failed) return;
      if (isa<PHINode>(I) || isa<AllocaInst>(I)) continue;
      if (I.isTerminator()) {
        emitTerminator(I);
        return;
      }
      if (emitFusedPair(I, idx + 1 < insts.size() ? insts[idx + 1] : nullptr)) {
        ++idx;
        continue;
      }
      emitInstruction(I);
    }
  }

  bool emitFusedPair(Instruction &I, Instruction *next) {
    if (!next) return false;
    auto *G = dyn_cast<GetElementPtrInst>(&I);
    if (!G || !G->hasOneUse()) return false;
    auto planIt = gepPlans.find(G);
    if (planIt == gepPlans.end()) return false;
    const GEPPlan &plan = planIt->second;
    if (plan.varTerms.size() != 1 || plan.constOffset != 0) return false;
    unsigned base = regOf(G->getPointerOperand());
    unsigned index = plan.varTerms[0].first;
    int64_t stride = plan.varTerms[0].second;
    if (auto *L = dyn_cast<LoadInst>(next)) {
      if (L->getPointerOperand() != G || L->isVolatile()) return false;
      Type *ty = L->getType();
      unsigned op = 0;
      if (ty->isIntegerTy(32))
        op = SHROUD_VM_GEP_LOAD32;
      else if (ty->isIntegerTy(64) || ty->isPointerTy())
        op = SHROUD_VM_GEP_LOAD64;
      else
        return false;
      emit(op, regs[L], base, index, stride);
      return true;
    }
    if (auto *S = dyn_cast<StoreInst>(next)) {
      if (S->getPointerOperand() != G || S->isVolatile()) return false;
      Type *ty = S->getValueOperand()->getType();
      unsigned op = 0;
      if (ty->isIntegerTy(32))
        op = SHROUD_VM_GEP_STORE32;
      else if (ty->isIntegerTy(64) || ty->isPointerTy())
        op = SHROUD_VM_GEP_STORE64;
      else
        return false;
      emit(op, regOf(S->getValueOperand()), base, index, stride);
      return true;
    }
    return false;
  }

  void emitInstruction(Instruction &I) {
    if (auto *BO = dyn_cast<BinaryOperator>(&I)) return emitBinary(BO);
    if (auto *CI = dyn_cast<ICmpInst>(&I)) return emitICmp(CI);
    if (auto *SI = dyn_cast<SelectInst>(&I)) return emitSelect(SI);
    if (auto *CI = dyn_cast<CastInst>(&I)) return emitCast(CI);
    if (auto *G = dyn_cast<GetElementPtrInst>(&I)) return emitGEP(G);
    if (auto *L = dyn_cast<LoadInst>(&I)) return emitLoad(L);
    if (auto *S = dyn_cast<StoreInst>(&I)) return emitStore(S);
    if (auto *CB = dyn_cast<CallInst>(&I)) return emitCall(CB);
    fail("unemittable instruction");
  }

  unsigned mapBinaryOpcode(unsigned opcode) {
    switch (opcode) {
    case Instruction::Add: return SHROUD_VM_ADD;
    case Instruction::Sub: return SHROUD_VM_SUB;
    case Instruction::Mul: return SHROUD_VM_MUL;
    case Instruction::UDiv: return SHROUD_VM_UDIV;
    case Instruction::SDiv: return SHROUD_VM_SDIV;
    case Instruction::URem: return SHROUD_VM_UREM;
    case Instruction::SRem: return SHROUD_VM_SREM;
    case Instruction::And: return SHROUD_VM_AND;
    case Instruction::Or: return SHROUD_VM_OR;
    case Instruction::Xor: return SHROUD_VM_XOR;
    case Instruction::Shl: return SHROUD_VM_SHL;
    case Instruction::LShr: return SHROUD_VM_LSHR;
    case Instruction::AShr: return SHROUD_VM_ASHR;
    default: return SHROUD_VM_HALT;
    }
  }

  void emitBinary(BinaryOperator *BO) {
    unsigned width = BO->getType()->getIntegerBitWidth();
    unsigned dst = regs[BO];
    unsigned a = regOf(BO->getOperand(0));
    unsigned b = regOf(BO->getOperand(1));
    bool signedOp = BO->getOpcode() == Instruction::SDiv ||
                    BO->getOpcode() == Instruction::SRem ||
                    BO->getOpcode() == Instruction::AShr;
    if (signedOp && width < 64) {
      unsigned ta = allocTemp();
      emit(SHROUD_VM_SEXT, ta, a, 0, (int64_t)width);
      unsigned tb = allocTemp();
      emit(SHROUD_VM_SEXT, tb, b, 0, (int64_t)width);
      a = ta;
      b = tb;
    }
    unsigned op = mapBinaryOpcode(BO->getOpcode());
    if (op == SHROUD_VM_ADD && rng.chance(40))
      op = SHROUD_VM_ADD_ALT;
    else if (op == SHROUD_VM_XOR && rng.chance(40))
      op = SHROUD_VM_XOR_ALT;
    emit(op, dst, a, b, 0);
    if (width < 64) emit(SHROUD_VM_TRUNC, dst, dst, 0, (int64_t)width);
  }
  unsigned mapICmpPredicate(CmpInst::Predicate pred) {
    switch (pred) {
    case CmpInst::ICMP_EQ: return SHROUD_VM_ICMP_EQ;
    case CmpInst::ICMP_NE: return SHROUD_VM_ICMP_NE;
    case CmpInst::ICMP_ULT: return SHROUD_VM_ICMP_ULT;
    case CmpInst::ICMP_ULE: return SHROUD_VM_ICMP_ULE;
    case CmpInst::ICMP_UGT: return SHROUD_VM_ICMP_UGT;
    case CmpInst::ICMP_UGE: return SHROUD_VM_ICMP_UGE;
    case CmpInst::ICMP_SLT: return SHROUD_VM_ICMP_SLT;
    case CmpInst::ICMP_SLE: return SHROUD_VM_ICMP_SLE;
    case CmpInst::ICMP_SGT: return SHROUD_VM_ICMP_SGT;
    case CmpInst::ICMP_SGE: return SHROUD_VM_ICMP_SGE;
    default: return SHROUD_VM_ICMP_EQ;
    }
  }

  void emitICmp(ICmpInst *CI) {
    unsigned width = intWidth(CI->getOperand(0)->getType());
    unsigned dst = regs[CI];
    unsigned a = regOf(CI->getOperand(0));
    unsigned b = regOf(CI->getOperand(1));
    if (ICmpInst::isSigned(CI->getPredicate()) && width < 64) {
      unsigned ta = allocTemp();
      emit(SHROUD_VM_SEXT, ta, a, 0, (int64_t)width);
      unsigned tb = allocTemp();
      emit(SHROUD_VM_SEXT, tb, b, 0, (int64_t)width);
      a = ta;
      b = tb;
    }
    unsigned op = mapICmpPredicate(CI->getPredicate());
    if (op == SHROUD_VM_ICMP_EQ && rng.chance(40)) op = SHROUD_VM_ICMP_EQ_ALT;
    emit(op, dst, a, b, 0);
  }

  void emitSelect(SelectInst *SI) {
    emit(SHROUD_VM_SELECT, regs[SI], regOf(SI->getTrueValue()),
         regOf(SI->getFalseValue()), (int64_t)regOf(SI->getCondition()));
  }

  void emitCast(CastInst *CI) {
    unsigned dst = regs[CI];
    unsigned src = regOf(CI->getOperand(0));
    switch (CI->getOpcode()) {
    case Instruction::Trunc:
      emit(SHROUD_VM_TRUNC, dst, src, 0, (int64_t)intWidth(CI->getType()));
      break;
    case Instruction::ZExt:
    case Instruction::BitCast:
    case Instruction::PtrToInt:
    case Instruction::IntToPtr:
    case Instruction::AddrSpaceCast:
      emit(rng.chance(30) ? SHROUD_VM_MOV_ALT : SHROUD_VM_MOV, dst, src, 0, 0);
      break;
    case Instruction::SExt:
      emit(SHROUD_VM_SEXT, dst, src, 0, (int64_t)intWidth(CI->getOperand(0)->getType()));
      break;
    default:
      fail("cast emission");
      break;
    }
  }

  void emitGEP(GetElementPtrInst *G) {
    const GEPPlan &plan = gepPlans[G];
    unsigned dst = regs[G];
    unsigned base = regOf(G->getPointerOperand());
    if (plan.varTerms.empty()) {
      emit(SHROUD_VM_GEP_IMM, dst, base, 0, plan.constOffset);
      return;
    }
    unsigned cur = base;
    if (plan.constOffset != 0) {
      cur = allocTemp();
      emit(SHROUD_VM_GEP_IMM, cur, base, 0, plan.constOffset);
    }
    for (size_t i = 0; i < plan.varTerms.size(); ++i) {
      bool last = (i + 1 == plan.varTerms.size());
      unsigned target = last ? dst : allocTemp();
      emit(SHROUD_VM_GEP_REG, target, cur, plan.varTerms[i].first,
           plan.varTerms[i].second);
      cur = target;
    }
  }

  unsigned loadOpcode(Type *T) {
    if (T->isPointerTy()) return SHROUD_VM_LOAD64;
    switch (T->getIntegerBitWidth()) {
    case 1:
    case 8: return SHROUD_VM_LOAD8;
    case 16: return SHROUD_VM_LOAD16;
    case 32: return SHROUD_VM_LOAD32;
    default: return SHROUD_VM_LOAD64;
    }
  }

  unsigned storeOpcode(Type *T) {
    if (T->isPointerTy()) return SHROUD_VM_STORE64;
    switch (T->getIntegerBitWidth()) {
    case 1:
    case 8: return SHROUD_VM_STORE8;
    case 16: return SHROUD_VM_STORE16;
    case 32: return SHROUD_VM_STORE32;
    default: return SHROUD_VM_STORE64;
    }
  }

  void emitLoad(LoadInst *L) {
    unsigned dst = regs[L];
    unsigned op = loadOpcode(L->getType());
    if (op == SHROUD_VM_LOAD32 && rng.chance(40)) op = SHROUD_VM_LOAD32_ALT;
    emit(op, dst, regOf(L->getPointerOperand()), 0, 0);
    if (L->getType()->isIntegerTy(1)) emit(SHROUD_VM_TRUNC, dst, dst, 0, 1);
  }

  void emitStore(StoreInst *S) {
    emit(storeOpcode(S->getValueOperand()->getType()), 0,
         regOf(S->getPointerOperand()), regOf(S->getValueOperand()), 0);
  }

  void emitIntrinsic(CallInst *CB, Function *callee) {
    StringRef name = callee->getName();
    if (name.starts_with("llvm.memcpy") || name.starts_with("llvm.memmove")) {
      unsigned opcode = name.starts_with("llvm.memmove") ? SHROUD_VM_MEMMOVE
                                                         : SHROUD_VM_MEMCPY;
      emit(opcode, 0, regOf(CB->getArgOperand(0)), regOf(CB->getArgOperand(1)),
           (int64_t)regOf(CB->getArgOperand(2)));
      return;
    }
    if (name.starts_with("llvm.memset")) {
      emit(SHROUD_VM_MEMSET, 0, regOf(CB->getArgOperand(0)),
           regOf(CB->getArgOperand(1)), (int64_t)regOf(CB->getArgOperand(2)));
      return;
    }
    if (name.starts_with("llvm.bswap")) {
      unsigned width = CB->getType()->getIntegerBitWidth();
      unsigned op = width == 16 ? SHROUD_VM_BSWAP16
                                : (width == 32 ? SHROUD_VM_BSWAP32 : SHROUD_VM_BSWAP64);
      emit(op, regs[CB], regOf(CB->getArgOperand(0)), 0, 0);
      if (width < 64)
        emit(SHROUD_VM_TRUNC, regs[CB], regs[CB], 0, (int64_t)width);
      return;
    }
    if (name.starts_with("llvm.fshl") || name.starts_with("llvm.fshr")) {
      bool left = name.starts_with("llvm.fshl");
      unsigned dst = regs[CB];
      unsigned a = regOf(CB->getArgOperand(0));
      unsigned b = regOf(CB->getArgOperand(1));
      uint64_t s = cast<ConstantInt>(CB->getArgOperand(2))->getZExtValue() & 63;
      if (s == 0) {
        emit(SHROUD_VM_MOV, dst, a, 0, 0);
        return;
      }
      unsigned cs = allocTemp();
      emitConst(cs, s, 32);
      unsigned co = allocTemp();
      emitConst(co, (64 - s) & 63, 32);
      unsigned t1 = allocTemp();
      emit(left ? SHROUD_VM_SHL : SHROUD_VM_LSHR, t1, a, cs, 0);
      unsigned t2 = allocTemp();
      emit(left ? SHROUD_VM_LSHR : SHROUD_VM_SHL, t2, b, co, 0);
      emit(SHROUD_VM_OR, dst, t1, t2, 0);
      return;
    }
  }

  void emitCall(CallInst *CB) {
    Function *callee = CB->getCalledFunction();
    if (callee && callee->isIntrinsic()) {
      emitIntrinsic(CB, callee);
      return;
    }
    unsigned argc = CB->arg_size();
    for (unsigned i = 0; i < argc; ++i)
      emit(SHROUD_VM_MOV, SHROUD_VM_ARG_BASE + i, regOf(CB->getArgOperand(i)), 0, 0);
    unsigned dst = CB->getType()->isVoidTy() ? SHROUD_VM_SINK_REG : regs[CB];
    if (callee) {
      emit(SHROUD_VM_CALL, dst, argc, 0, (int64_t)callIndex[callee]);
    } else {
      unsigned cr = regOf(CB->getCalledOperand());
      emit(SHROUD_VM_CALL_INDIRECT, dst, cr, argc, 0);
    }
    if (!CB->getType()->isVoidTy()) {
      unsigned width = intWidth(CB->getType());
      if (width < 64) emit(SHROUD_VM_TRUNC, dst, dst, 0, (int64_t)width);
    }
  }

  unsigned phiSourceReg(const BasicBlock *from, const PHINode &phi) {
    return regOf(phi.getIncomingValueForBlock(from));
  }

  void emitPhiCopies(const BasicBlock *from, const BasicBlock *to) {
    SmallVector<std::pair<unsigned, unsigned>, 8> copies;
    for (const PHINode &phi : to->phis())
      copies.emplace_back(regs[&phi], phiSourceReg(from, phi));
    if (copies.empty()) return;

    bool conflict = false;
    SmallPtrSet<const Instruction *, 16> dests;
    for (const PHINode &phi : to->phis()) dests.insert(&phi);
    for (const PHINode &phi : to->phis()) {
      const Value *in = phi.getIncomingValueForBlock(from);
      if (auto *inst = dyn_cast<Instruction>(in))
        if (dests.count(inst)) conflict = true;
    }

    if (!conflict) {
      for (auto &C : copies)
        if (C.first != C.second) emit(SHROUD_VM_MOV, C.first, C.second, 0, 0);
      return;
    }
    SmallVector<unsigned, 8> temps;
    for (auto &C : copies) {
      unsigned t = allocTemp();
      emit(SHROUD_VM_MOV, t, C.second, 0, 0);
      temps.push_back(t);
    }
    for (size_t i = 0; i < copies.size(); ++i)
      emit(SHROUD_VM_MOV, copies[i].first, temps[i], 0, 0);
  }

  void emitJmp(const BasicBlock *target) {
    unsigned cell = (unsigned)out->size();
    emitCell(encodeInsn(SHROUD_VM_JMP, 0, 0, 0, 0));
    patches.push_back({currentBlock, cell, target});
  }

  void emitTerminator(Instruction &I) {
    if (auto *R = dyn_cast<ReturnInst>(&I)) {
      unsigned r = R->getReturnValue() ? regOf(R->getReturnValue())
                                       : SHROUD_VM_REG_SENTINEL;
      emit(SHROUD_VM_RET, 0, r, 0, 0);
      return;
    }
    if (auto *B = dyn_cast<BranchInst>(&I)) {
      if (B->isUnconditional()) {
        emitPhiCopies(B->getParent(), B->getSuccessor(0));
        emitJmp(B->getSuccessor(0));
        return;
      }
      unsigned cond = regOf(B->getCondition());
      const BasicBlock *trueBB = B->getSuccessor(0);
      const BasicBlock *falseBB = B->getSuccessor(1);
      unsigned cell = (unsigned)out->size();
      emit(SHROUD_VM_CONDBR, 0, cond, 0, 0);
      emitPhiCopies(B->getParent(), falseBB);
      emitJmp(falseBB);
      uint32_t stub = (uint32_t)out->size();
      stubPatches.push_back({currentBlock, cell, stub});
      emitPhiCopies(B->getParent(), trueBB);
      emitJmp(trueBB);
      return;
    }
    if (auto *SW = dyn_cast<SwitchInst>(&I)) {
      unsigned cond = regOf(SW->getCondition());
      SmallVector<std::pair<unsigned, const BasicBlock *>, 8> checks;
      for (auto &Case : SW->cases()) {
        nextTemp = SHROUD_VM_TEMP_BASE;
        unsigned t = allocTemp();
        emit(SHROUD_VM_ICMP_EQ, t, cond, regOf(Case.getCaseValue()), 0);
        unsigned cell = (unsigned)out->size();
        emit(SHROUD_VM_CONDBR, 0, t, 0, 0);
        checks.push_back({cell, Case.getCaseSuccessor()});
      }
      emitPhiCopies(SW->getParent(), SW->getDefaultDest());
      emitJmp(SW->getDefaultDest());
      for (const auto &item : checks) {
        uint32_t stub = (uint32_t)out->size();
        stubPatches.push_back({currentBlock, item.first, stub});
        emitPhiCopies(SW->getParent(), item.second);
        emitJmp(item.second);
      }
      return;
    }
    if (isa<UnreachableInst>(&I)) {
      emit(SHROUD_VM_HALT, 0, 0, 0, 0);
      return;
    }
    fail("unsupported terminator");
  }
};

static bool linkRuntime(Module &M, std::string &reason) {
  if (M.getFunction("shroud_vm_run")) return true;
  MemoryBufferRef buf(StringRef(reinterpret_cast<const char *>(vm_runtime_bc),
                                vm_runtime_bc_size),
                      "shroud.vm.runtime");
  Expected<std::unique_ptr<Module>> parsed = parseBitcodeFile(buf, M.getContext());
  if (!parsed) {
    reason = "cannot parse VM runtime bitcode";
    return false;
  }
  (*parsed)->setTargetTriple(M.getTargetTriple());
  (*parsed)->setDataLayout(M.getDataLayout());
  if (auto *Flags = (*parsed)->getNamedMetadata("llvm.module.flags"))
    (*parsed)->eraseNamedMetadata(Flags);
  if (auto *Options = (*parsed)->getNamedMetadata("llvm.linker.options"))
    (*parsed)->eraseNamedMetadata(Options);
  if (auto *Ident = (*parsed)->getNamedMetadata("llvm.ident"))
    (*parsed)->eraseNamedMetadata(Ident);
  if (Linker::linkModules(M, std::move(*parsed))) {
    reason = "cannot link VM runtime";
    return false;
  }
  for (Function &F : M)
    if (F.getName().starts_with("shroud_vm_"))
      F.setLinkage(GlobalValue::InternalLinkage);
  return true;
}

static const char *opcodeName(unsigned op) {
  switch (op) {
  case SHROUD_VM_HALT: return "HALT";
  case SHROUD_VM_CONST: return "CONST";
  case SHROUD_VM_CONST64: return "CONST64";
  case SHROUD_VM_MOV: return "MOV";
  case SHROUD_VM_ADD: return "ADD";
  case SHROUD_VM_SUB: return "SUB";
  case SHROUD_VM_MUL: return "MUL";
  case SHROUD_VM_UDIV: return "UDIV";
  case SHROUD_VM_SDIV: return "SDIV";
  case SHROUD_VM_UREM: return "UREM";
  case SHROUD_VM_SREM: return "SREM";
  case SHROUD_VM_AND: return "AND";
  case SHROUD_VM_OR: return "OR";
  case SHROUD_VM_XOR: return "XOR";
  case SHROUD_VM_SHL: return "SHL";
  case SHROUD_VM_LSHR: return "LSHR";
  case SHROUD_VM_ASHR: return "ASHR";
  case SHROUD_VM_NEG: return "NEG";
  case SHROUD_VM_NOT: return "NOT";
  case SHROUD_VM_ICMP_EQ: return "ICMP_EQ";
  case SHROUD_VM_ICMP_NE: return "ICMP_NE";
  case SHROUD_VM_ICMP_ULT: return "ICMP_ULT";
  case SHROUD_VM_ICMP_ULE: return "ICMP_ULE";
  case SHROUD_VM_ICMP_UGT: return "ICMP_UGT";
  case SHROUD_VM_ICMP_UGE: return "ICMP_UGE";
  case SHROUD_VM_ICMP_SLT: return "ICMP_SLT";
  case SHROUD_VM_ICMP_SLE: return "ICMP_SLE";
  case SHROUD_VM_ICMP_SGT: return "ICMP_SGT";
  case SHROUD_VM_ICMP_SGE: return "ICMP_SGE";
  case SHROUD_VM_SELECT: return "SELECT";
  case SHROUD_VM_JMP: return "JMP";
  case SHROUD_VM_CONDBR: return "CONDBR";
  case SHROUD_VM_SEXT: return "SEXT";
  case SHROUD_VM_TRUNC: return "TRUNC";
  case SHROUD_VM_LOAD8: return "LOAD8";
  case SHROUD_VM_LOAD16: return "LOAD16";
  case SHROUD_VM_LOAD32: return "LOAD32";
  case SHROUD_VM_LOAD64: return "LOAD64";
  case SHROUD_VM_STORE8: return "STORE8";
  case SHROUD_VM_STORE16: return "STORE16";
  case SHROUD_VM_STORE32: return "STORE32";
  case SHROUD_VM_STORE64: return "STORE64";
  case SHROUD_VM_GEP_IMM: return "GEP_IMM";
  case SHROUD_VM_GEP_REG: return "GEP_REG";
  case SHROUD_VM_CALL: return "CALL";
  case SHROUD_VM_CALL_INDIRECT: return "CALL_INDIRECT";
  case SHROUD_VM_RET: return "RET";
  case SHROUD_VM_LOADADDR: return "LOADADDR";
  case SHROUD_VM_MEMCPY: return "MEMCPY";
  case SHROUD_VM_MEMMOVE: return "MEMMOVE";
  case SHROUD_VM_MEMSET: return "MEMSET";
  default: return "?";
  }
}

static void dumpBytecode(const VMLiftResult &lift, raw_ostream &os) {
  for (unsigned r = 0; r < lift.regNames.size(); ++r)
    if (!lift.regNames[r].empty()) os << "  r" << r << " = " << lift.regNames[r] << "\n";
  size_t nextBlock = 0;
  for (size_t pc = 0; pc < lift.cells.size();) {
    while (nextBlock < lift.blocks.size() && lift.blocks[nextBlock].first == pc) {
      os << "block " << nextBlock << " " << lift.blocks[nextBlock].second << ":\n";
      ++nextBlock;
    }
    uint64_t c = lift.cells[pc];
    unsigned op = (unsigned)(c & 0xFF);
    unsigned dst = (unsigned)((c >> 8) & 0xFF);
    unsigned a = (unsigned)((c >> 16) & 0xFF);
    unsigned b = (unsigned)((c >> 24) & 0xFF);
    int64_t imm = (int64_t)(int32_t)(uint32_t)(c >> 32);
    os << "  " << pc << " " << opcodeName(op) << " dst=" << dst << " a=" << a
       << " b=" << b << " imm=" << imm;
    if (op == SHROUD_VM_CONST64 && pc + 1 < lift.cells.size())
      os << " hi=" << (uint32_t)(lift.cells[pc + 1] >> 32);
    if (op == SHROUD_VM_JMP || op == SHROUD_VM_CONDBR)
      os << " -> " << (uint32_t)imm;
    os << "\n";
    pc += (op == SHROUD_VM_CONST64) ? 2 : 1;
  }
}

static Constant *getOrCreateThunk(Module &M, Function *callee) {
  std::string name = "shroud.vm.call." + callee->getName().str();
  if (Function *existing = M.getFunction(name)) return existing;

  LLVMContext &ctx = M.getContext();
  auto *i64 = Type::getInt64Ty(ctx);
  auto *ft = FunctionType::get(i64, std::vector<Type *>(8, i64), false);
  Function *thunk = Function::Create(ft, GlobalValue::InternalLinkage, name, M);
  BasicBlock *bb = BasicBlock::Create(ctx, "entry", thunk);
  IRBuilder<> B(bb);

  FunctionType *cft = callee->getFunctionType();
  SmallVector<Value *, 8> args;
  for (unsigned i = 0; i < cft->getNumParams(); ++i) {
    Type *pt = cft->getParamType(i);
    Value *raw = thunk->getArg(i);
    Value *conv;
    if (pt->isPointerTy())
      conv = B.CreateIntToPtr(raw, pt);
    else
      conv = B.CreateTrunc(raw, pt);
    args.push_back(conv);
  }
  Value *call = B.CreateCall(cft, callee, args);
  Type *rt = cft->getReturnType();
  Value *res;
  if (rt->isVoidTy())
    res = ConstantInt::get(i64, 0);
  else if (rt->isPointerTy())
    res = B.CreatePtrToInt(call, i64);
  else
    res = B.CreateZExtOrTrunc(call, i64);
  B.CreateRet(res);
  return thunk;
}

VMLiftResult liftFunctionToVM(Function &F, const DataLayout &DL) {
  RNG rng(0x123456789ABCDEFULL);
  Lifter lifter(F, DL, rng);
  return lifter.run();
}

bool virtualizeFunction(Function &F, uint64_t moduleSeed, std::string &note) {
  Module &M = *F.getParent();
  DataLayout DL = M.getDataLayout();

  RNG opRng(mixSeed(moduleSeed, "ops:" + F.getName().str()));
  Lifter lifter(F, DL, opRng);
  VMLiftResult lift = lifter.run();
  if (!lift.ok) {
    note = lift.reason;
    return false;
  }

  std::string reason;
  if (!linkRuntime(M, reason)) {
    note = reason;
    return false;
  }
  shroud::noteVmUsed(M);

  if (std::getenv("SHROUD_DUMP")) {
    errs() << "[shroud] vm bytecode for " << F.getName() << " ("
           << lift.cells.size() << " cells, " << lift.nregs << " regs)\n";
    for (unsigned i = 0; i < lift.callTargets.size(); ++i)
      errs() << "  call[" << i << "] = " << lift.callTargets[i]->getName() << "\n";
    for (unsigned i = 0; i < lift.globalTargets.size(); ++i)
      errs() << "  global[" << i << "] = " << *lift.globalTargets[i] << "\n";
    dumpBytecode(lift, errs());
  }

  LLVMContext &ctx = M.getContext();
  auto *i64 = Type::getInt64Ty(ctx);
  auto *ptrTy = PointerType::get(ctx, 0);
  auto *voidTy = Type::getVoidTy(ctx);

  SmallVector<Constant *, 8> callPtrs;
  for (Function *callee : lift.callTargets)
    callPtrs.push_back(getOrCreateThunk(M, callee));
  auto *callTy = ArrayType::get(ptrTy, callPtrs.size());
  Constant *callInit = callPtrs.empty()
                           ? ConstantAggregateZero::get(callTy)
                           : ConstantArray::get(callTy, callPtrs);
  auto *callTable = new GlobalVariable(M, callTy, true, GlobalValue::PrivateLinkage,
                                       callInit, "shroud.vm.calls");
  callTable->setAlignment(Align(8));

  auto *globalTy = ArrayType::get(ptrTy, lift.globalTargets.size());
  Constant *globalInit = lift.globalTargets.empty()
                             ? ConstantAggregateZero::get(globalTy)
                             : ConstantArray::get(globalTy, lift.globalTargets);
  auto *globalTable = new GlobalVariable(M, globalTy, true, GlobalValue::PrivateLinkage,
                                         globalInit, "shroud.vm.globals");
  globalTable->setAlignment(Align(8));

  uint64_t key = mixSeed(moduleSeed, "vm:" + F.getName().str());
  std::vector<uint64_t> encrypted = lift.cells;
  std::vector<uint64_t> starts;
  for (const auto &block : lift.blocks) starts.push_back(block.first);
  {
    uint64_t state = key;
    size_t next = 0;
    for (size_t pc = 0; pc < encrypted.size(); ++pc) {
      if (next < starts.size() && pc == starts[next]) {
        if (pc != 0) state = vmMix(state ^ 0x5DEECE66DULL);
        ++next;
      }
      state = vmMix(state);
      encrypted[pc] ^= state;
    }
  }

  SmallVector<Constant *, 64> codeCells;
  for (uint64_t cell : encrypted) codeCells.push_back(ConstantInt::get(i64, cell));
  auto *codeTy = ArrayType::get(i64, codeCells.size());
  auto *codeGlobal = new GlobalVariable(M, codeTy, true, GlobalValue::PrivateLinkage,
                                        ConstantArray::get(codeTy, codeCells),
                                        "shroud.vm.code");
  codeGlobal->setAlignment(Align(8));
  uint64_t checksum = shroud::bytecodeChecksum(encrypted);
  shroud::registerBytecodeCheck(M, codeGlobal, checksum);

  SmallVector<Constant *, 16> startCells;
  for (uint64_t value : starts) startCells.push_back(ConstantInt::get(i64, value));
  auto *startsTy = ArrayType::get(i64, startCells.size());
  auto *startsGlobal =
      new GlobalVariable(M, startsTy, true, GlobalValue::PrivateLinkage,
                         ConstantArray::get(startsTy, startCells), "shroud.vm.starts");
  startsGlobal->setAlignment(Align(8));

  F.deleteBody();
  BasicBlock *entry = BasicBlock::Create(ctx, "entry", &F);
  IRBuilder<> B(entry);
  auto *regsTy = ArrayType::get(i64, SHROUD_VM_MAX_REGS);
  AllocaInst *regs = B.CreateAlloca(regsTy, nullptr, "shroud.vm.regs");

  auto storeReg = [&](unsigned reg, Value *wide) {
    Value *slot = B.CreateGEP(regsTy, regs,
                              {ConstantInt::get(i64, 0), ConstantInt::get(i64, reg)});
    B.CreateStore(wide, slot);
  };

  for (unsigned i = 0; i < (unsigned)lift.argRegs.size(); ++i) {
    Value *arg = F.getArg(i);
    Value *wide;
    if (arg->getType()->isPointerTy())
      wide = B.CreatePtrToInt(arg, i64);
    else if (arg->getType()->isIntegerTy(64))
      wide = arg;
    else
      wide = B.CreateZExt(arg, i64);
    storeReg(lift.argRegs[i], wide);
  }

  for (const VMAlloca &am : lift.allocas) {
    AllocaInst *slot = B.CreateAlloca(am.allocatedType, am.arraySize, "shroud.vm.slot");
    slot->setAlignment(Align(am.align));
    storeReg(am.reg, B.CreatePtrToInt(slot, i64));
  }

  auto bufferTy = ArrayType::get(i64, lift.cells.size());
  AllocaInst *codeBuf = B.CreateAlloca(bufferTy, nullptr, "shroud.vm.decoded");
  GlobalVariable *tamper = shroud::getOrCreateTamper(M);
  GlobalVariable *ready = shroud::getOrCreateReady(M);
  Value *ok = B.CreateAnd(
      B.CreateICmpEQ(B.CreateLoad(i64, tamper), ConstantInt::get(i64, 0)),
      B.CreateICmpEQ(B.CreateLoad(i64, ready),
                     ConstantInt::get(i64, shroud::kReadyMagic)));
  Value *bad = B.CreateNot(ok);
  BasicBlock *trapBB = BasicBlock::Create(ctx, "shroud.vm.trap", &F);
  BasicBlock *runBB = BasicBlock::Create(ctx, "shroud.vm.run", &F);
  B.CreateCondBr(bad, trapBB, runBB);
  B.SetInsertPoint(trapBB);
  FunctionCallee trapFn =
      M.getOrInsertFunction("shroud_vm_trap", FunctionType::get(voidTy, false));
  B.CreateCall(trapFn);
  B.CreateUnreachable();
  B.SetInsertPoint(runBB);
  FunctionCallee decryptFn = M.getOrInsertFunction(
      "shroud_vm_decrypt_blocks",
      FunctionType::get(voidTy, {ptrTy, ptrTy, i64, i64, ptrTy, i64}, false));
  B.CreateCall(decryptFn, {codeGlobal, codeBuf, ConstantInt::get(i64, lift.cells.size()),
                           ConstantInt::get(i64, key), startsGlobal,
                           ConstantInt::get(i64, starts.size())});

  FunctionCallee runFn = M.getOrInsertFunction(
      "shroud_vm_run",
      FunctionType::get(i64, {ptrTy, i64, ptrTy, ptrTy, i64, ptrTy, i64, ptrTy, i64},
                        false));
  Value *res = B.CreateCall(runFn,
                            {codeBuf, ConstantInt::get(i64, lift.cells.size()), regs,
                             callTable, ConstantInt::get(i64, callPtrs.size()), globalTable,
                             ConstantInt::get(i64, lift.globalTargets.size()), codeGlobal,
                             ConstantInt::get(i64, checksum)});

  Type *RT = F.getReturnType();
  if (RT->isVoidTy()) {
    B.CreateRetVoid();
  } else if (RT->isPointerTy()) {
    B.CreateRet(B.CreateIntToPtr(res, RT));
  } else if (RT->isIntegerTy(64)) {
    B.CreateRet(res);
  } else {
    B.CreateRet(B.CreateTrunc(res, RT));
  }

  F.removeFnAttr(Attribute::Memory);
  F.removeFnAttr(Attribute::NoRecurse);
  F.removeFnAttr(Attribute::WillReturn);
  F.removeFnAttr(Attribute::NoSync);
  F.removeFnAttr(Attribute::NoFree);
  F.removeFnAttr(Attribute::NoReturn);

  note = "cells=" + std::to_string(lift.cells.size()) +
         " regs=" + std::to_string(lift.nregs) +
         " calls=" + std::to_string(callPtrs.size());
  return true;
}

}
