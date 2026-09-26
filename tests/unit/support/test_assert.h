// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
#ifndef TEST_ASSERT_H
#define TEST_ASSERT_H
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
