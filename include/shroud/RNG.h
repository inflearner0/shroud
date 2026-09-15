#pragma once

#include <cstdint>
#include <string>

namespace shroud {

class RNG {
public:
  explicit RNG(uint64_t seed);

  uint64_t next64();
  uint32_t next32();
  bool chance(uint32_t percent);
  uint64_t range(uint64_t lo, uint64_t hi);

private:
  uint64_t state_[4];
};

uint64_t mixSeed(uint64_t seed, const std::string &tag);

}
