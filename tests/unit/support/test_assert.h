#ifndef TEST_ASSERT_H
#define TEST_ASSERT_H
#include <stdio.h>
#include <stdlib.h>

#define ASSERT_EQ_INT(a,b) do { \
  if ((a) != (b)) { \
    fprintf(stderr, "[FAIL] %s:%d: %s != %s (%d != %d)\n", __FILE__, __LINE__, #a, #b, (int)(a), (int)(b)); \
    exit(1); \
  } \
} while (0)

#define ASSERT_TRUE(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "[FAIL] %s:%d: %s was false\n", __FILE__, __LINE__, #expr); \
    exit(1); \
  } \
} while (0)

#define TEST(name) static void name(void)
#define RUN(testfn) do { \
  fprintf(stderr, "[RUN ] %s\n", #testfn); \
  testfn(); \
  fprintf(stderr, "[PASS] %s\n", #testfn); \
} while (0)

#endif
