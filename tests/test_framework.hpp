// Minimal test harness — no external dependency. Tests must not sleep; use
// an injectable clock instead (see LockManager tests).
#pragma once

#include <iostream>
#include <string>

namespace testfw {

struct Result {
    int passed = 0;
    int failed = 0;
};

inline Result &global_result() {
    static Result r;
    return r;
}

inline void check(bool cond, const std::string &expr, const char *file, int line) {
    if (cond) {
        global_result().passed++;
    } else {
        global_result().failed++;
        std::cerr << "FAIL: " << expr << " at " << file << ":" << line << std::endl;
    }
}

inline int summary() {
    auto &r = global_result();
    std::cout << r.passed << " passed, " << r.failed << " failed ("
              << (r.passed + r.failed) << " assertions)" << std::endl;
    return r.failed == 0 ? 0 : 1;
}

} // namespace testfw

#define ASSERT_TRUE(cond) testfw::check((cond), #cond, __FILE__, __LINE__)
#define ASSERT_FALSE(cond) testfw::check(!(cond), "!(" #cond ")", __FILE__, __LINE__)
#define ASSERT_EQ(a, b) testfw::check((a) == (b), #a " == " #b, __FILE__, __LINE__)

#define RUN_TEST(fn)                              \
    do {                                           \
        std::cout << "-- " #fn << std::endl;       \
        fn();                                      \
    } while (0)
