// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
#ifndef TEST_ASSERT_H
#define TEST_ASSERT_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT_EQ_INT(a, b)                                                                                            \
    do {                                                                                                               \
        if ((a) != (b)) {                                                                                              \
            fprintf(stderr, "[FAIL] %s:%d: %s != %s (%d != %d)\n", __FILE__, __LINE__, #a, #b, (int)(a), (int)(b));    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

#define ASSERT_TRUE(expr)                                                                                              \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            fprintf(stderr, "[FAIL] %s:%d: %s was false\n", __FILE__, __LINE__, #expr);                                \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

// A register or pixel compared in hexadecimal; fail-fast like ASSERT_EQ_INT.
#define ASSERT_EQ_HEX(a, b)                                                                                            \
    do {                                                                                                               \
        unsigned a_ = (unsigned)(a), b_ = (unsigned)(b);                                                               \
        if (a_ != b_) {                                                                                                \
            fprintf(stderr, "[FAIL] %s:%d: %s != %s ($%02X != $%02X)\n", __FILE__, __LINE__, #a, #b, a_, b_);          \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

// Counting checks, for a suite defining TEST_ASSERT_COUNTING before the
// include: a failed check is printed and counted and the run goes on, so
// one run reports every wrong result of a table-driven suite.  The
// counters are the suite's (one translation unit); CHECK_SUMMARY prints
// them and is the exit status.
#ifdef TEST_ASSERT_COUNTING
static int failures, checks;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define CHECK_EQ32(got, want)                                                                                          \
    do {                                                                                                               \
        checks++;                                                                                                      \
        uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want);                                                          \
        if (g_ != w_) {                                                                                                \
            printf("FAIL %s:%d: %s = $%08X, want $%08X\n", __FILE__, __LINE__, #got, g_, w_);                          \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)
#define CHECK_EQ(got, want) CHECK_EQ32(got, want)

#define CHECK_EQ64(got, want)                                                                                          \
    do {                                                                                                               \
        checks++;                                                                                                      \
        uint64_t g_ = (got), w_ = (want);                                                                              \
        if (g_ != w_) {                                                                                                \
            printf("FAIL %s:%d: %s = $%016llX, want $%016llX\n", __FILE__, __LINE__, #got, (unsigned long long)g_,     \
                   (unsigned long long)w_);                                                                            \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// "<name>: N checks, M failures", then 0 when every check held, else 1.
#define CHECK_SUMMARY(name) (printf("%s: %d checks, %d failures\n", (name), checks, failures), failures ? 1 : 0)
#endif

#define TEST(name) static void name(void)
// TEST_ONLY=<test name> runs that test alone -- how a fixture is checked
// against unfixed code on its own, when an earlier test's failure (a crash,
// say) would otherwise end the run before it.
#define RUN(testfn)                                                                                                    \
    do {                                                                                                               \
        const char *test_only_ = getenv("TEST_ONLY");                                                                  \
        if (test_only_ && strcmp(test_only_, #testfn) != 0)                                                            \
            break;                                                                                                     \
        fprintf(stderr, "[RUN ] %s\n", #testfn);                                                                       \
        testfn();                                                                                                      \
        fprintf(stderr, "[PASS] %s\n", #testfn);                                                                       \
    } while (0)

#endif
