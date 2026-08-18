/*
 * mayhem-b200 — minimal test harness.
 *
 * Deliberately dependency-free: the project already needs UHD, MSVC and Boost
 * headers to build, and adding a test framework on top of that is friction for
 * anyone trying to compile this on a fresh machine.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_TEST_MAIN_H__
#define __MB200_TEST_MAIN_H__

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>  /* _getpid */
#else
#include <unistd.h>   /* getpid */
#endif

namespace mb200test {

/* The current process id, spelled portably.
 *
 * Fixture files and directories are created under a shared temp directory, so
 * their names have to be unique per process or two test binaries running at
 * once tread on each other. MSVC spells this _getpid() and declares it in
 * <process.h>; POSIX spells it getpid() in <unistd.h> and has no <process.h>
 * at all -- which is exactly what broke the Linux build for the first outside
 * user who tried it (reported 2026-08-14: "Missing process.h"). Kept in one
 * place so no test has to care again. */
inline int test_pid() {
#if defined(_WIN32)
    return _getpid();
#else
    return static_cast<int>(getpid());
#endif
}

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

inline int& failure_count() {
    static int n = 0;
    return n;
}

inline std::string& current_test() {
    static std::string s;
    return s;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline void report_failure(const char* file, int line, const std::string& what) {
    failure_count()++;
    std::printf("  FAIL %s:%d\n    %s\n", file, line, what.c_str());
}

inline int run_all() {
    int failed_tests = 0;
    for (auto& tc : registry()) {
        current_test() = tc.name;
        const int before = failure_count();
        std::printf("[ RUN  ] %s\n", tc.name.c_str());
        try {
            tc.fn();
        } catch (const std::exception& e) {
            report_failure(__FILE__, __LINE__,
                           std::string{"unexpected exception: "} + e.what());
        } catch (...) {
            report_failure(__FILE__, __LINE__, "unexpected non-standard exception");
        }
        const bool ok = (failure_count() == before);
        if (!ok) failed_tests++;
        std::printf("[ %s ] %s\n", ok ? " OK " : "FAIL", tc.name.c_str());
    }

    std::printf("\n%d test(s), %d failed, %d assertion failure(s)\n",
                static_cast<int>(registry().size()), failed_tests, failure_count());
    return failed_tests == 0 ? 0 : 1;
}

}  // namespace mb200test

#define TEST(name)                                                            \
    static void name();                                                       \
    static ::mb200test::Registrar reg_##name{#name, name};                    \
    static void name()

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond))                                                          \
            ::mb200test::report_failure(__FILE__, __LINE__,                   \
                                        "CHECK failed: " #cond);              \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        const auto _a = (a);                                                  \
        const auto _b = (b);                                                  \
        if (!(_a == _b)) {                                                    \
            std::string m = "CHECK_EQ failed: " #a " == " #b;                 \
            ::mb200test::report_failure(__FILE__, __LINE__, m);               \
        }                                                                     \
    } while (0)

/* String comparison that prints both sides — the formatting tests need this to
 * be readable when they fail. */
#define CHECK_STR_EQ(a, b)                                                    \
    do {                                                                      \
        const std::string _a = (a);                                           \
        const std::string _b = (b);                                           \
        if (_a != _b) {                                                       \
            ::mb200test::report_failure(                                      \
                __FILE__, __LINE__,                                           \
                "CHECK_STR_EQ failed: " #a " == " #b "\n      got:      [" +  \
                    _a + "]\n      expected: [" + _b + "]");                  \
        }                                                                     \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                 \
    do {                                                                      \
        const double _a = static_cast<double>(a);                             \
        const double _b = static_cast<double>(b);                             \
        const double _t = static_cast<double>(tol);                           \
        if (!(std::fabs(_a - _b) <= _t)) {                                    \
            ::mb200test::report_failure(                                      \
                __FILE__, __LINE__,                                           \
                "CHECK_NEAR failed: " #a " ~= " #b "\n      got:      " +     \
                    std::to_string(_a) + "\n      expected: " +               \
                    std::to_string(_b) + " +/- " + std::to_string(_t));       \
        }                                                                     \
    } while (0)

#endif /*__MB200_TEST_MAIN_H__*/
