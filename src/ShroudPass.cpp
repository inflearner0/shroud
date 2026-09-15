#include "shroud/Annotation.h"
#include "shroud/Config.h"
#include "shroud/RNG.h"
#include "shroud/RuntimeSupport.h"
#include "shroud/Transforms.h"
#include "shroud/VMVirtualizer.h"

#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace {

uint64_t envSeed(uint64_t fallback) {
  const char *s = std::getenv("SHROUD_SEED");
  if (!s || !*s) return fallback;
  char *end = nullptr;
  unsigned long long v = std::strtoull(s, &end, 0);
  if (end == s || *end != '\0') return fallback;
  return static_cast<uint64_t>(v);
}

bool envFlag(const char *name) {
  const char *s = std::getenv(name);
  return s && *s && s[0] != '0';
}

GlobalVariable *getTrap(Module &M) {
  if (GlobalVariable *GV = M.getNamedGlobal("shroud.trap")) return GV;
  LLVMContext &ctx = M.getContext();
  auto *i64 = Type::getInt64Ty(ctx);
  auto *GV = new GlobalVariable(M, i64, false, GlobalValue::InternalLinkage,
                                ConstantInt::get(i64, 0), "shroud.trap");
  GV->setAlignment(Align(8));
  return GV;
}

Value *opaqueTrue(IRBuilder<> &builder, Module &M, uint64_t seed, shroud::RNG &rng) {
  LLVMContext &ctx = M.getContext();
  auto *i64 = Type::getInt64Ty(ctx);
  GlobalVariable *anchor = shroud::getOrCreateAnchor(M, seed);
  GlobalVariable *expected = shroud::getOrCreateExpected(M);

  Value *a = builder.CreateLoad(i64, anchor, "shroud.anchor.load");
  Value *b = builder.CreateLoad(i64, expected, "shroud.expected.load");
  switch (rng.next32() % 4) {
  case 0:
    return builder.CreateICmpEQ(a, b);
  case 1:
    return builder.CreateICmpEQ(builder.CreateXor(a, b), ConstantInt::get(i64, 0));
  case 2:
    return builder.CreateICmpEQ(builder.CreateOr(a, b), builder.CreateAnd(a, b));
  default:
    return builder.CreateICmpEQ(builder.CreateSub(a, b), ConstantInt::get(i64, 0));
  }
}

Value *emitJunk(IRBuilder<> &builder, shroud::RNG &rng, unsigned count) {
  LLVMContext &ctx = builder.getContext();
  auto *i64 = Type::getInt64Ty(ctx);
  Value *x = ConstantInt::get(i64, rng.next64());
  for (unsigned i = 0; i < count; ++i) {
    Value *c = ConstantInt::get(i64, rng.next64());
    switch (rng.next32() % 4) {
    case 0:
      x = builder.CreateAdd(x, c);
      break;
    case 1:
      x = builder.CreateXor(x, c);
      break;
    case 2:
      x = builder.CreateMul(x, builder.CreateOr(c, ConstantInt::get(i64, 1)));
      break;
    default:
      x = builder.CreateShl(x, ConstantInt::get(i64, rng.next32() % 62 + 1));
      break;
    }
  }
  return x;
}

bool cloakEntry(Function &F, bool bogus, uint64_t seed, shroud::RNG &rng) {
  if (F.isDeclaration() || F.empty()) return false;
  shroud::noteCloakUsed(*F.getParent());
  BasicBlock &entry = F.getEntryBlock();
  if (!entry.getTerminator()) return false;
  LLVMContext &ctx = F.getContext();
  auto *cloak = BasicBlock::Create(ctx, "shroud.entry", &F, &entry);
  IRBuilder<> builder(cloak);
  Value *cond = opaqueTrue(builder, *F.getParent(), seed, rng);
  if (bogus) {
    auto *junk = BasicBlock::Create(ctx, "shroud.junk", &F, &entry);
    builder.CreateCondBr(cond, &entry, junk);
    IRBuilder<> junkBuilder(junk);
    Value *junkValue = emitJunk(junkBuilder, rng, 3 + (rng.next32() % 5));
    junkBuilder.CreateStore(junkValue, getTrap(*F.getParent()), true);
    junkBuilder.CreateBr(&entry);
  } else {
    builder.CreateBr(&entry);
  }
  return true;
}

struct ShroudPass : PassInfoMixin<ShroudPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
    shroud::Config config;
    config.moduleSeed = envSeed(config.moduleSeed);
    config.verbose = envFlag("SHROUD_VERBOSE");
    config.defaults = shroud::parsePlan(shroud::moduleDefaultsSpec(M), config.defaults);

    if (config.verbose) {
      errs() << "[shroud] module seed 0x";
      errs().write_hex(config.moduleSeed);
      errs() << "\n";
    }

    shroud::FunctionAnnotations annotations = shroud::collectAnnotations(M);

    std::vector<std::pair<std::string, std::string>> profile;
    if (const char *path = std::getenv("SHROUD_PROFILE")) {
      std::ifstream in(path);
      std::string line;
      while (std::getline(in, line)) {
        size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos || line[b] == '#') continue;
        size_t eq = line.find('=', b);
        if (eq == std::string::npos) continue;
        size_t e = line.find_last_not_of(" \t\r\n", eq - 1);
        std::string pattern = line.substr(b, e - b + 1);
        size_t v = line.find_first_not_of(" \t\r\n", eq + 1);
        if (v == std::string::npos) continue;
        std::string spec = line.substr(v);
        profile.push_back({pattern, spec});
      }
    }

    std::vector<std::pair<Function *, std::vector<std::string>>> ordered;
    if (profile.empty()) {
      for (auto &item : annotations) ordered.push_back(item);
    } else {
      for (Function &F : M) {
        if (F.isDeclaration()) continue;
        auto it = annotations.find(&F);
        ordered.push_back({&F, it == annotations.end() ? std::vector<std::string>()
                                                       : it->second});
      }
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const std::pair<Function *, std::vector<std::string>> &a,
                 const std::pair<Function *, std::vector<std::string>> &b) {
                return a.first->getName() < b.first->getName();
              });

    shroud::setModuleSeed(M, config.moduleSeed);

    struct Stats {
      unsigned vm = 0;
      unsigned cloak = 0;
      unsigned mba = 0;
      unsigned split = 0;
      unsigned flatten = 0;
      unsigned bpred = 0;
      unsigned data = 0;
    } stats;

    unsigned transformed = 0;
    for (auto &item : ordered) {
      Function *F = item.first;
      if (!F || F->isDeclaration()) continue;
      bool annotated = shroud::hasShroudSpec(item.second);
      shroud::Plan plan = config.defaults;
      if (annotated) {
        for (const std::string &spec : item.second) {
          if (!shroud::isShroudSpec(spec)) continue;
          plan = shroud::parsePlan(spec, plan);
        }
      }
      bool matchedProfile = false;
      for (const auto &rule : profile) {
        if (!shroud::globMatch(rule.first, F->getName().str())) continue;
        plan = shroud::parsePlan(rule.second, plan);
        matchedProfile = true;
      }
      if (!annotated && !matchedProfile) continue;
      if (!plan.enabled) continue;
      shroud::RNG rng(shroud::mixSeed(plan.seed.value_or(config.moduleSeed),
                                      F->getName().str()));
      bool did = false;
      bool virtualized = false;
      unsigned stringCount = 0;
      std::string actions;
      if (plan.strings) {
        stringCount += shroud::encryptStringsInFunction(*F, M, config.moduleSeed);
        if (stringCount) {
          did = true;
          stats.data += stringCount;
        }
      }
      if (plan.consts) {
        unsigned n = shroud::encryptConstantsInFunction(*F, M, config.moduleSeed);
        stringCount += n;
        if (n) {
          did = true;
          stats.data += n;
        }
      }
      if (plan.mbaDepth) {
        shroud::applyMba(*F, plan.mbaDepth, rng);
        ++stats.mba;
        did = true;
      }
      if (plan.bpred) {
        if (shroud::injectBranchPredicates(*F, M, rng, true)) {
          did = true;
          ++stats.bpred;
          actions += actions.empty() ? "bpred" : " bpred";
        }
      }
      if (plan.flatten) {
        if (shroud::flattenFunction(*F, M, rng)) {
          did = true;
          ++stats.flatten;
          actions += actions.empty() ? "flatten" : " flatten";
        } else {
          actions += actions.empty() ? "flatten-skipped" : " flatten-skipped";
        }
      }
      if (plan.virtualize) {
        std::string vmNote;
        if (shroud::virtualizeFunction(*F, config.moduleSeed, vmNote)) {
          did = true;
          virtualized = true;
          ++stats.vm;
          actions += actions.empty() ? "" : " ";
          actions += "vm(" + vmNote + ")";
        } else {
          actions += actions.empty() ? "" : " ";
          actions += "vm-skipped(" + vmNote + ")";
        }
      }
      if (!virtualized && plan.opaque) {
        if (cloakEntry(*F, plan.bogus, config.moduleSeed, rng)) {
          did = true;
          ++stats.cloak;
          actions += actions.empty() ? "entry-cloak" : " entry-cloak";
        } else if (actions.empty()) {
          actions = "entry-cloak-skipped";
        }
      }
      if (!virtualized && plan.mbaDepth) {
        shroud::splitConstants(*F, M, rng);
        ++stats.split;
      }
      if (config.verbose) {
        errs() << "[shroud] " << F->getName() << ": " << actions;
        if (plan.mbaDepth) errs() << " mba=" << plan.mbaDepth;
        if (stringCount) errs() << " strings=" << stringCount;
        for (const std::string &unknown : plan.unknown)
          errs() << " unknown:" << unknown;
        errs() << "\n";
      }
      if (did) ++transformed;
    }
    shroud::finalizeShroudRuntime(M);
    if (config.verbose) {
      errs() << "[shroud] stats: vm=" << stats.vm << " cloak=" << stats.cloak
             << " mba=" << stats.mba << " split=" << stats.split
             << " flatten=" << stats.flatten << " bpred=" << stats.bpred
             << " data=" << stats.data << "\n";
      errs() << "[shroud] transformed " << transformed << " function(s)\n";
    }
    return PreservedAnalyses::none();
  }
};

}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "shroud", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (name == "shroud") {
                    MPM.addPass(ShroudPass());
                    return true;
                  }
                  return false;
                });
            PB.registerOptimizerLastEPCallback(
                [](ModulePassManager &MPM, OptimizationLevel) {
                  MPM.addPass(ShroudPass());
                });
          }};
}
