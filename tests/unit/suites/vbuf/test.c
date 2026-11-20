// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for vbuf.h, the growable text buffer.

#include "test_assert.h"
#include "vbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// putc grows through the 64-byte seed; appendf takes the long path past its
// 128-byte scratch; take hands the text over and resets the buffer.
TEST(putc_appendf_and_take) {
    vbuf_t b = {0};
    for (int i = 0; i < 100; i++)
        vbuf_putc(&b, (char)('a' + i % 26));
    ASSERT_EQ_INT(100, (int)b.len);
    ASSERT_TRUE(b.p[99] == 'v' && b.p[100] == '\0');
    vbuf_appendf(&b, "%0200d", 7);
    ASSERT_EQ_INT(300, (int)b.len);
    ASSERT_TRUE(b.p[299] == '7' && b.p[300] == '\0');
    char *t = vbuf_take(&b);
    ASSERT_TRUE(t && strlen(t) == 300);
    ASSERT_TRUE(b.p == NULL && b.len == 0 && b.cap == 0);
    free(t);
}

// An empty buffer takes as "", never NULL.
TEST(empty_takes_as_empty_string) {
    vbuf_t b = {0};
    char *t = vbuf_take(&b);
    ASSERT_TRUE(t && t[0] == '\0');
    free(t);
}

// Once `oom` is set (as a failed grow sets it) no append changes the text:
// the output stops at the failure rather than continuing with a hole.
// vbuf_free starts over.
TEST(oom_is_sticky) {
    vbuf_t b = {0};
    vbuf_append_str(&b, "ab");
    b.oom = true;
    vbuf_append_str(&b, "cd");
    vbuf_putc(&b, 'e');
    vbuf_appendf(&b, "%d", 7);
    ASSERT_TRUE(b.len == 2 && strcmp(b.p, "ab") == 0);
    vbuf_free(&b);
    ASSERT_TRUE(!b.oom && b.p == NULL);
}

// gs_grow seeds an empty array, doubles to fit, and leaves room alone.
TEST(gs_grow_seeds_and_doubles) {
    int *v = NULL;
    size_t cap = 0;
    v = gs_grow(v, &cap, 1, sizeof(*v), 4);
    ASSERT_TRUE(v != NULL);
    ASSERT_EQ_INT(4, (int)cap);
    int *same = gs_grow(v, &cap, 4, sizeof(*v), 4);
    ASSERT_TRUE(same == v);
    ASSERT_EQ_INT(4, (int)cap);
    v = gs_grow(v, &cap, 9, sizeof(*v), 4);
    ASSERT_TRUE(v != NULL);
    ASSERT_EQ_INT(16, (int)cap);
    v[15] = 7; // the whole capacity is usable
    free(v);
}

// An element count whose byte size overflows fails, leaving the array.
TEST(gs_grow_refuses_an_overflowing_size) {
    char *v = malloc(8);
    size_t cap = 8;
    ASSERT_TRUE(gs_grow(v, &cap, SIZE_MAX / 2, 16, 8) == NULL);
    ASSERT_EQ_INT(8, (int)cap);
    free(v);
}

int main(void) {
    RUN(putc_appendf_and_take);
    RUN(empty_takes_as_empty_string);
    RUN(oom_is_sticky);
    RUN(gs_grow_seeds_and_doubles);
    RUN(gs_grow_refuses_an_overflowing_size);
    fprintf(stderr, "All vbuf tests passed\n");
    return 0;
}
