#include "shroud/RNG.h"

namespace shroud {

static uint64_t splitmix64(uint64_t &state) {
  uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

RNG::RNG(uint64_t seed) {
  uint64_t s = seed;
  for (int i = 0; i < 4; ++i)
    state_[i] = splitmix64(s);
}

uint64_t RNG::next64() {
  uint64_t *s = state_;
  uint64_t result = ((s[1] * 5) << 7) | ((s[1] * 5) >> 57);
  uint64_t t = s[1] << 17;
  s[2] ^= s[0];
  s[3] ^= s[1];
  s[1] ^= s[2];
  s[0] ^= s[3];
  s[2] ^= t;
  s[3] = (s[3] << 45) | (s[3] >> 19);
  return result;
}

uint32_t RNG::next32() { return static_cast<uint32_t>(next64() >> 32); }

bool RNG::chance(uint32_t percent) {
  return percent >= 100 || next32() % 100 < percent;
}

uint64_t RNG::range(uint64_t lo, uint64_t hi) {
  if (hi <= lo) return lo;
  return lo + next64() % (hi - lo);
}

uint64_t mixSeed(uint64_t seed, const std::string &tag) {
  uint64_t h = 14695981039346656037ULL ^ seed;
  for (unsigned char c : tag) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  h ^= h >> 33;
  h *= 0xFF51AFD7ED558CCDULL;
  h ^= h >> 33;
  return h;
}

}
