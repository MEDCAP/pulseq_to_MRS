// Minimal test helpers: CHECK prints failures and counts them; main returns the
// failure count so ctest reports the test as failed.
#pragma once

#include <cmath>
#include <cstdio>
#include <exception>

namespace pp::test {
inline int& Failures() {
  static int n = 0;
  return n;
}
}  // namespace pp::test

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++pp::test::Failures();                                              \
    }                                                                      \
  } while (0)

#define CHECK_EQ(a, b)                                                       \
  do {                                                                       \
    const auto va_ = (a);                                                    \
    const auto vb_ = (b);                                                    \
    if (!(va_ == vb_)) {                                                     \
      std::printf("%s:%d: CHECK_EQ failed: %s == %s (%lld vs %lld)\n",       \
                  __FILE__, __LINE__, #a, #b, static_cast<long long>(va_),   \
                  static_cast<long long>(vb_));                              \
      ++pp::test::Failures();                                                \
    }                                                                        \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                \
  do {                                                                       \
    const double va_ = (a);                                                  \
    const double vb_ = (b);                                                  \
    if (!(std::abs(va_ - vb_) <= (tol))) {                                   \
      std::printf("%s:%d: CHECK_NEAR failed: %s ~ %s (%.9g vs %.9g)\n",      \
                  __FILE__, __LINE__, #a, #b, va_, vb_);                     \
      ++pp::test::Failures();                                                \
    }                                                                        \
  } while (0)

#define CHECK_THROWS(expr, Type)                                             \
  do {                                                                       \
    bool thrown_ = false;                                                    \
    try {                                                                    \
      (void)(expr);                                                          \
    } catch (const Type&) {                                                  \
      thrown_ = true;                                                        \
    }                                                                        \
    if (!thrown_) {                                                          \
      std::printf("%s:%d: expected %s from %s\n", __FILE__, __LINE__, #Type, \
                  #expr);                                                    \
      ++pp::test::Failures();                                                \
    }                                                                        \
  } while (0)

#define TEST_MAIN(body)                                          \
  int main() {                                                   \
    try {                                                        \
      body();                                                    \
    } catch (const std::exception& e) {                          \
      std::printf("uncaught exception: %s\n", e.what());         \
      return 100;                                                \
    }                                                            \
    std::printf("%d failure(s)\n", pp::test::Failures());        \
    return pp::test::Failures();                                 \
  }
