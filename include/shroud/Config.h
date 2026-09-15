#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace shroud {

struct Plan {
  bool enabled = false;
  bool opaque = true;
  bool bogus = true;
  bool bpred = false;
  uint32_t mbaDepth = 0;
  bool flatten = false;
  bool virtualize = false;
  bool strings = false;
  bool consts = false;
  std::optional<uint64_t> seed;
  std::vector<std::string> unknown;
};

struct Config {
  Plan defaults;
  uint64_t moduleSeed = 0x243F6A8885A308D3ULL;
  bool verbose = false;
};

std::vector<std::string> splitSpec(const std::string &spec);
bool isShroudToken(const std::string &token);
bool isShroudSpec(const std::string &spec);
Plan parsePlan(const std::string &spec, const Plan &base = Plan{});
bool globMatch(const std::string &pattern, const std::string &name);

}
