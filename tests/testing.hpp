// Minimal assertion harness. The project deliberately has no test-framework
// dependency: the build has to work on a bare CI runner and in a distro
// container without fetching anything.
#pragma once

#include <cmath>
#include <cstdio>
#include <string>

namespace testing {

inline int failures = 0;
inline int checks = 0;

inline void record(bool ok, const char* file, int line, const char* expression) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expression);
}

inline void record_near(double actual, double expected, double epsilon, const char* file, int line,
                        const char* expression) {
  ++checks;
  if (std::fabs(actual - expected) <= epsilon) return;
  ++failures;
  std::fprintf(stderr, "FAIL %s:%d  %s  (got %.6f, want %.6f)\n", file, line, expression, actual,
               expected);
}

inline int summarize(const char* suite) {
  if (failures > 0) {
    std::fprintf(stderr, "%s: %d of %d checks failed\n", suite, failures, checks);
    return 1;
  }
  std::printf("%s: %d checks passed\n", suite, checks);
  return 0;
}

}  // namespace testing

#define CHECK(expr) ::testing::record((expr), __FILE__, __LINE__, #expr)
#define CHECK_NEAR(actual, expected, epsilon) \
  ::testing::record_near((actual), (expected), (epsilon), __FILE__, __LINE__, #actual)
