/**
 * @file test_common.hpp
 * @brief Minimal assert-style test helpers (always active, independent of NDEBUG).
 */
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace etd_test {
inline int& failures() {
    static int n = 0;
    return n;
}
inline int finish(const char* suite) {
    if (failures() == 0) {
        std::printf("[PASS] %s\n", suite);
        return EXIT_SUCCESS;
    }
    std::printf("[FAIL] %s: %d check(s) failed\n", suite, failures());
    return EXIT_FAILURE;
}
}  // namespace etd_test

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++etd_test::failures();                                                       \
        }                                                                                 \
    } while (false)

#define CHECK_NEAR(a, b, eps) CHECK(std::fabs(static_cast<double>(a) - static_cast<double>(b)) <= (eps))
