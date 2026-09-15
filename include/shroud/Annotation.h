#pragma once

#include <map>
#include <string>
#include <vector>

namespace llvm {
class Function;
class Module;
}

namespace shroud {

using FunctionAnnotations = std::map<llvm::Function *, std::vector<std::string>>;

FunctionAnnotations collectAnnotations(llvm::Module &M);
bool hasShroudSpec(const std::vector<std::string> &annotations);
std::string moduleDefaultsSpec(llvm::Module &M);

}
