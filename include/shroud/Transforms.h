#pragma once

namespace llvm {
class Function;
class Module;
}

namespace shroud {

class RNG;

void applyMba(llvm::Function &F, unsigned depth, RNG &rng);
void splitConstants(llvm::Function &F, llvm::Module &M, RNG &rng);
bool flattenFunction(llvm::Function &F, llvm::Module &M, RNG &rng);
bool injectBranchPredicates(llvm::Function &F, llvm::Module &M, RNG &rng, bool diamonds);

}
