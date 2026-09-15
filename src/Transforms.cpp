#include "shroud/Transforms.h"

#include "shroud/RNG.h"
#include "shroud/RuntimeSupport.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"

#include <set>
#include <vector>

using namespace llvm;

namespace shroud {

namespace {

bool mbaOpcode(unsigned opcode) {
  switch (opcode) {
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Xor:
  case Instruction::And:
  case Instruction::Or:
    return true;
  default:
    return false;
  }
}

static Value *emitMba(IRBuilder<> &B, Value *a, Value *b, unsigned opcode,
                      unsigned depth, RNG &rng, unsigned &budget) {
  Type *ty = a->getType();
  if (depth == 0 || budget == 0)
    return B.CreateBinOp(static_cast<Instruction::BinaryOps>(opcode), a, b);
  --budget;
  switch (opcode) {
  case Instruction::Add: {
    Value *x = emitMba(B, a, b, Instruction::Xor, depth - 1, rng, budget);
    Value *both = emitMba(B, a, b, Instruction::And, depth - 1, rng, budget);
    Value *twice = B.CreateShl(both, ConstantInt::get(ty, 1));
    return emitMba(B, x, twice, Instruction::Add, depth - 1, rng, budget);
  }
  case Instruction::Sub: {
    Value *notB = B.CreateNot(b);
    Value *sum = emitMba(B, a, notB, Instruction::Add, depth - 1, rng, budget);
    return emitMba(B, sum, ConstantInt::get(ty, 1), Instruction::Add, depth - 1, rng,
                   budget);
  }
  case Instruction::Xor: {
    Value *either = emitMba(B, a, b, Instruction::Or, depth - 1, rng, budget);
    Value *both = emitMba(B, a, b, Instruction::And, depth - 1, rng, budget);
    return emitMba(B, either, both, Instruction::Sub, depth - 1, rng, budget);
  }
  case Instruction::And: {
    Value *notA = B.CreateNot(a);
    Value *notB = B.CreateNot(b);
    Value *eitherNot = emitMba(B, notA, notB, Instruction::Or, depth - 1, rng, budget);
    return B.CreateNot(eitherNot);
  }
  case Instruction::Or: {
    Value *notB = B.CreateNot(b);
    Value *onlyA = emitMba(B, a, notB, Instruction::And, depth - 1, rng, budget);
    return emitMba(B, onlyA, b, Instruction::Add, depth - 1, rng, budget);
  }
  default:
    return B.CreateBinOp(static_cast<Instruction::BinaryOps>(opcode), a, b);
  }
}

}

void applyMba(Function &F, unsigned depth, RNG &rng) {
  if (depth == 0 || F.isDeclaration()) return;
  if (depth > 3) depth = 3;

  std::vector<BinaryOperator *> work;
  std::vector<ICmpInst *> compares;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
        Type *ty = BO->getType();
        if (!ty->isIntegerTy() || ty->getIntegerBitWidth() < 2) continue;
        if (!mbaOpcode(BO->getOpcode())) continue;
        work.push_back(BO);
        continue;
      }
      if (auto *CI = dyn_cast<ICmpInst>(&I)) {
        Type *ty = CI->getOperand(0)->getType();
        if (!ty->isIntegerTy() || ty->getIntegerBitWidth() < 2) continue;
        compares.push_back(CI);
      }
    }
  }

  unsigned budget = 1500;
  for (BinaryOperator *BO : work) {
    IRBuilder<> B(BO);
    unsigned nodeDepth = 1 + (rng.next32() % depth);
    Value *expanded = emitMba(B, BO->getOperand(0), BO->getOperand(1),
                              BO->getOpcode(), nodeDepth, rng, budget);
    BO->replaceAllUsesWith(expanded);
    BO->eraseFromParent();
  }

  for (ICmpInst *CI : compares) {
    if (!rng.chance(70)) continue;
    Type *ty = CI->getOperand(0)->getType();
    Value *a = CI->getOperand(0);
    Value *b = CI->getOperand(1);
    IRBuilder<> B(CI);
    Value *rewritten = nullptr;
    switch (CI->getPredicate()) {
    case CmpInst::ICMP_EQ:
      rewritten = B.CreateICmpEQ(B.CreateXor(a, b), ConstantInt::get(ty, 0));
      break;
    case CmpInst::ICMP_NE:
      rewritten = B.CreateICmpNE(B.CreateXor(a, b), ConstantInt::get(ty, 0));
      break;
    case CmpInst::ICMP_SLT:
      rewritten = B.CreateICmpSLT(B.CreateSub(a, b), ConstantInt::get(ty, 0));
      break;
    case CmpInst::ICMP_SLE:
      rewritten = B.CreateICmpSLE(B.CreateSub(a, b), ConstantInt::get(ty, 0));
      break;
    case CmpInst::ICMP_SGT:
      rewritten = B.CreateICmpSGT(B.CreateSub(a, b), ConstantInt::get(ty, 0));
      break;
    case CmpInst::ICMP_SGE:
      rewritten = B.CreateICmpSGE(B.CreateSub(a, b), ConstantInt::get(ty, 0));
      break;
    case CmpInst::ICMP_ULT:
      rewritten = B.CreateICmpUGT(b, a);
      break;
    case CmpInst::ICMP_ULE:
      rewritten = B.CreateICmpUGE(b, a);
      break;
    case CmpInst::ICMP_UGT:
      rewritten = B.CreateICmpULT(b, a);
      break;
    case CmpInst::ICMP_UGE:
      rewritten = B.CreateICmpULE(b, a);
      break;
    default:
      break;
    }
    if (!rewritten) continue;
    CI->replaceAllUsesWith(rewritten);
    CI->eraseFromParent();
  }
}

void splitConstants(Function &F, Module &M, RNG &rng) {
  if (F.isDeclaration()) return;
  noteCloakUsed(M);
  LLVMContext &ctx = F.getContext();
  auto *i64 = Type::getInt64Ty(ctx);
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (isa<PHINode>(&I) || isa<AllocaInst>(&I) || isa<GetElementPtrInst>(&I) ||
          isa<SwitchInst>(&I) || I.isTerminator())
        continue;
      for (Use &U : I.operands()) {
        auto *CI = dyn_cast<ConstantInt>(U.get());
        if (!CI || CI->getType()->getIntegerBitWidth() < 2) continue;
        if (!rng.chance(40)) continue;
        IRBuilder<> B(&I);
        Type *ty = CI->getType();
        Value *zv = B.CreateXor(B.CreateLoad(i64, getOrCreateAnchor(M, 0)),
                                B.CreateLoad(i64, getOrCreateExpected(M)));
        if (!ty->isIntegerTy(64)) zv = B.CreateTrunc(zv, ty);
        ConstantInt *key =
            ConstantInt::get(cast<IntegerType>(ty), rng.next64());
        APInt masked = CI->getValue() ^ key->getValue();
        Value *split = B.CreateXor(
            B.CreateXor(ConstantInt::get(ty, masked), key), zv);
        U.set(split);
      }
    }
  }
}

bool injectBranchPredicates(Function &F, Module &M, RNG &rng, bool diamonds) {
  if (F.isDeclaration() || F.empty()) return false;
  std::vector<BranchInst *> branches;
  for (BasicBlock &BB : F) {
    auto *BI = dyn_cast<BranchInst>(BB.getTerminator());
    if (BI && BI->isConditional()) branches.push_back(BI);
  }
  if (branches.empty()) return false;

  noteCloakUsed(M);
  LLVMContext &ctx = F.getContext();
  auto *i64 = Type::getInt64Ty(ctx);
  GlobalVariable *decoy = diamonds ? getOrCreateDecoy(M) : nullptr;

  for (BranchInst *BI : branches) {
    IRBuilder<> B(BI);
    Value *opTrue = buildOpaqueTrue(B, M, rng);
    Value *cond = BI->getCondition();
    switch (rng.next32() % 3) {
    case 0:
      BI->setCondition(B.CreateAnd(cond, opTrue));
      break;
    case 1:
      BI->setCondition(B.CreateOr(cond, B.CreateNot(opTrue)));
      break;
    default:
      BI->setCondition(B.CreateSelect(opTrue, cond, cond));
      break;
    }

    if (!decoy) continue;
    if (BI->getSuccessor(0) == BI->getSuccessor(1)) continue;
    if (!rng.chance(50)) continue;

    unsigned idx = rng.next32() & 1;
    BasicBlock *succ = BI->getSuccessor(idx);
    BasicBlock *parent = BI->getParent();
    BasicBlock *diamond = BasicBlock::Create(ctx, "shroud.decoy", &F, succ);
    IRBuilder<> DB(diamond);
    Value *junk = ConstantInt::get(i64, rng.next64());
    unsigned count = 2 + (rng.next32() % 3);
    for (unsigned i = 0; i < count; ++i) {
      Value *c = ConstantInt::get(i64, rng.next64());
      switch (rng.next32() % 3) {
      case 0:
        junk = DB.CreateAdd(junk, c);
        break;
      case 1:
        junk = DB.CreateXor(junk, c);
        break;
      default:
        junk = DB.CreateMul(junk, DB.CreateOr(c, ConstantInt::get(i64, 1)));
        break;
      }
    }
    DB.CreateStore(junk, decoy, true);
    DB.CreateBr(succ);
    for (PHINode &phi : succ->phis()) {
      Value *incoming = phi.getIncomingValueForBlock(parent);
      phi.removeIncomingValue(parent);
      phi.addIncoming(incoming, diamond);
    }
    BI->setSuccessor(idx, diamond);
  }
  return true;
}

bool flattenFunction(Function &F, Module &M, RNG &rng) {
  if (F.isDeclaration() || F.empty() || F.hasPersonalityFn()) return false;

  std::vector<BasicBlock *> blocks;
  for (BasicBlock &BB : F) {
    Instruction *T = BB.getTerminator();
    if (!T) return false;
    if (!isa<BranchInst>(T) && !isa<SwitchInst>(T) && !isa<ReturnInst>(T) &&
        !isa<UnreachableInst>(T))
      return false;
    for (Instruction &I : BB)
      if (auto *CB = dyn_cast<CallInst>(&I))
        if (CB->isMustTailCall()) return false;
    blocks.push_back(&BB);
  }
  if (blocks.size() < 2) return false;

  noteCloakUsed(M);
  LLVMContext &ctx = F.getContext();
  auto *i32 = Type::getInt32Ty(ctx);
  auto *i64 = Type::getInt64Ty(ctx);
  auto *ptrTy = PointerType::get(ctx, 0);
  bool pointerMode = rng.chance(50);

  BasicBlock *origEntry = &F.getEntryBlock();
  BasicBlock *newEntry = BasicBlock::Create(ctx, "shroud.flatten.entry", &F, origEntry);
  BasicBlock *dispatch = BasicBlock::Create(ctx, "shroud.flatten.dispatch", &F, origEntry);
  BasicBlock *trap = BasicBlock::Create(ctx, "shroud.flatten.trap", &F);

  IRBuilder<> entryB(newEntry);
  Type *stateTy = pointerMode ? static_cast<Type *>(ptrTy) : static_cast<Type *>(i32);
  AllocaInst *state = entryB.CreateAlloca(stateTy, nullptr, "shroud.state");

  GlobalVariable *anchor = getOrCreateAnchor(M, 0);
  GlobalVariable *expected = getOrCreateExpected(M);
  Value *zv = entryB.CreateXor(entryB.CreateLoad(i64, anchor),
                               entryB.CreateLoad(i64, expected));
  if (!pointerMode) zv = entryB.CreateTrunc(zv, i32);

  std::vector<Constant *> ids(blocks.size());
  if (pointerMode) {
    for (size_t i = 0; i < blocks.size(); ++i) {
      ids[i] = new GlobalVariable(M, Type::getInt8Ty(ctx), false,
                                  GlobalValue::InternalLinkage,
                                  ConstantInt::get(Type::getInt8Ty(ctx), 0),
                                  "shroud.state.id");
    }
  } else {
    std::set<uint32_t> used;
    for (size_t i = 0; i < blocks.size(); ++i) {
      uint32_t id = 0;
      do {
        id = rng.next32() & 0x7FFFFFFFu;
      } while (id == 0 || !used.insert(id).second);
      ids[i] = ConstantInt::get(i32, id);
    }
  }

  DenseMap<BasicBlock *, unsigned> blockIdx;
  for (unsigned i = 0; i < blocks.size(); ++i) blockIdx[blocks[i]] = i;

  DenseMap<PHINode *, AllocaInst *> slots;
  for (BasicBlock *BB : blocks)
    for (PHINode &phi : BB->phis())
      slots[&phi] = entryB.CreateAlloca(phi.getType(), nullptr, "shroud.phi");

  Value *initId = ids[blockIdx[origEntry]];
  if (pointerMode)
    entryB.CreateStore(entryB.CreateGEP(Type::getInt8Ty(ctx), initId, zv), state);
  else
    entryB.CreateStore(entryB.CreateAdd(initId, zv), state);
  entryB.CreateBr(dispatch);

  for (BasicBlock *BB : blocks) {
    Instruction *T = BB->getTerminator();
    for (unsigned s = 0; s < T->getNumSuccessors(); ++s) {
      BasicBlock *S = T->getSuccessor(s);
      for (PHINode &phi : S->phis()) {
        IRBuilder<> sb(T);
        sb.CreateStore(phi.getIncomingValueForBlock(BB), slots[&phi]);
      }
    }
  }

  for (BasicBlock *BB : blocks) {
    SmallVector<PHINode *, 8> phis;
    for (PHINode &phi : BB->phis()) phis.push_back(&phi);
    for (PHINode *phi : phis) {
      IRBuilder<> lb(BB, BB->getFirstInsertionPt());
      LoadInst *load = lb.CreateLoad(phi->getType(), slots[phi]);
      phi->replaceAllUsesWith(load);
      phi->eraseFromParent();
    }
  }

  for (BasicBlock *BB : blocks) {
    Instruction *T = BB->getTerminator();
    if (isa<ReturnInst>(T) || isa<UnreachableInst>(T)) continue;
    IRBuilder<> b(T);
    Value *next = nullptr;
    if (auto *BI = dyn_cast<BranchInst>(T)) {
      if (BI->isUnconditional()) {
        next = ids[blockIdx[BI->getSuccessor(0)]];
      } else {
        next = b.CreateSelect(BI->getCondition(), ids[blockIdx[BI->getSuccessor(0)]],
                              ids[blockIdx[BI->getSuccessor(1)]]);
      }
    } else {
      auto *SW = cast<SwitchInst>(T);
      next = ids[blockIdx[SW->getDefaultDest()]];
      for (auto &Case : SW->cases()) {
        Value *match = b.CreateICmpEQ(SW->getCondition(), Case.getCaseValue());
        next = b.CreateSelect(match, ids[blockIdx[Case.getCaseSuccessor()]], next);
      }
    }
    if (pointerMode)
      b.CreateStore(b.CreateGEP(Type::getInt8Ty(ctx), next, zv), state);
    else
      b.CreateStore(b.CreateAdd(next, zv), state);
    b.CreateBr(dispatch);
    T->eraseFromParent();
  }

  if (pointerMode) {
    IRBuilder<> db(dispatch);
    Value *loaded = db.CreateLoad(ptrTy, state);
    std::vector<BasicBlock *> checks;
    for (size_t i = 0; i < blocks.size(); ++i)
      checks.push_back(BasicBlock::Create(ctx, "shroud.state.check", &F));
    db.CreateBr(checks[0]);
    for (size_t i = 0; i < blocks.size(); ++i) {
      IRBuilder<> cb(checks[i]);
      Value *eq = cb.CreateICmpEQ(loaded, ids[i]);
      BasicBlock *nextBB = (i + 1 < blocks.size()) ? checks[i + 1] : trap;
      cb.CreateCondBr(eq, blocks[i], nextBB);
    }
  } else {
    IRBuilder<> db(dispatch);
    Value *loaded = db.CreateLoad(i32, state);
    Value *keyValue = db.CreateSub(loaded, zv);
    SwitchInst *sw = db.CreateSwitch(keyValue, trap, blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i)
      sw->addCase(cast<ConstantInt>(ids[i]), blocks[i]);
  }

  IRBuilder<> tb(trap);
  tb.CreateUnreachable();
  return true;
}

}
