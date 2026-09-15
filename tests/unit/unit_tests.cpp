#include "shroud/Config.h"
#include "shroud/RNG.h"

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>

static int g_failures = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      ++g_failures;                                                              \
    }                                                                            \
  } while (0)

int main() {
  shroud::RNG a(12345);
  shroud::RNG b(12345);
  for (int i = 0; i < 1000; ++i) CHECK(a.next64() == b.next64());

  shroud::RNG c(12345);
  shroud::RNG d(12346);
  CHECK(c.next64() != d.next64());

  shroud::RNG e(1);
  std::set<uint64_t> seen;
  for (int i = 0; i < 2000; ++i) seen.insert(e.next64() >> 32);
  CHECK(seen.size() > 1900);

  CHECK(shroud::RNG(7).chance(100));
  CHECK(!shroud::RNG(7).chance(0));

  shroud::RNG r(42);
  for (int i = 0; i < 100; ++i) {
    uint64_t v = r.range(10, 20);
    CHECK(v >= 10 && v < 20);
  }

  shroud::Plan plan = shroud::parsePlan("shroud;opaque=0;mba=3;seed=0xDEAD");
  CHECK(plan.enabled);
  CHECK(!plan.opaque);
  CHECK(plan.mbaDepth == 3);
  CHECK(plan.seed.has_value() && *plan.seed == 0xDEAD);
  CHECK(plan.unknown.empty());

  shroud::Plan colon = shroud::parsePlan("shroud:virtualize", shroud::Plan{});
  CHECK(colon.enabled);
  CHECK(colon.virtualize);

  shroud::Plan bad = shroud::parsePlan("shroud;frobnicate=1", shroud::Plan{});
  CHECK(bad.unknown.size() == 1 && bad.unknown[0] == "frobnicate");

  shroud::Plan base;
  base.opaque = false;
  shroud::Plan inherited = shroud::parsePlan("shroud", base);
  CHECK(inherited.enabled);
  CHECK(!inherited.opaque);

  shroud::Plan off = shroud::parsePlan("shroud;bogus=no", shroud::Plan{});
  CHECK(off.enabled && !off.bogus);

  CHECK(shroud::isShroudSpec("shroud;mba=1"));
  CHECK(shroud::isShroudSpec("noise;shroud:flatten"));
  CHECK(!shroud::isShroudSpec("shroudy"));
  CHECK(!shroud::isShroudSpec("not-here"));

  CHECK(shroud::splitSpec("a; b ;c").size() == 3);

  CHECK(shroud::globMatch("k_*", "k_bitmix_0"));
  CHECK(shroud::globMatch("*sort*", "k_sort_9"));
  CHECK(shroud::globMatch("exact", "exact"));
  CHECK(shroud::globMatch("a?c", "abc"));
  CHECK(!shroud::globMatch("k_*", "sort_0"));
  CHECK(!shroud::globMatch("abc", "abcd"));

  uint64_t s1 = shroud::mixSeed(42, "func");
  uint64_t s2 = shroud::mixSeed(42, "func");
  uint64_t s3 = shroud::mixSeed(43, "func");
  CHECK(s1 == s2);
  CHECK(s1 != s3);

  if (g_failures) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::puts("unit tests passed");
  return 0;
}
