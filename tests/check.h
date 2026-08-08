#pragma once

// A three-macro test harness. No framework, matching the rest of the fleet:
// the tests that matter here are about decisions, and a decision test is a
// comparison and a message.

#include <cstdio>
#include <cstdlib>
#include <string>

namespace ferret_test {
inline int g_failures = 0;
inline int g_checks = 0;
}  // namespace ferret_test

#define CHECK(cond)                                                       \
  do {                                                                    \
    ++ferret_test::g_checks;                                              \
    if (!(cond)) {                                                        \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
      ++ferret_test::g_failures;                                          \
    }                                                                     \
  } while (0)

#define CHECK_EQ(a, b)                                                    \
  do {                                                                    \
    ++ferret_test::g_checks;                                              \
    auto va_ = (a);                                                       \
    auto vb_ = (b);                                                       \
    if (!(va_ == vb_)) {                                                  \
      std::printf("  FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b);\
      ++ferret_test::g_failures;                                          \
    }                                                                     \
  } while (0)

#define TEST_MAIN(name)                                                   \
  int main() {                                                            \
    std::printf("%s\n", name);                                            \
    run();                                                                \
    std::printf("%s: %d checks, %d failed\n", name, ferret_test::g_checks,\
                ferret_test::g_failures);                                 \
    return ferret_test::g_failures == 0 ? 0 : 1;                          \
  }
