// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for common.h's header-only helpers.

#include "common.h"
#include "test_assert.h"

#include <stdio.h>
#include <string.h>

// The array-length macro counts elements, not bytes.
TEST(array_len_counts_elements) {
    static const uint32_t words[7] = {0};
    static const char text[] = "abc";
    ASSERT_EQ_INT(7, (int)GS_ARRAY_LEN(words));
    ASSERT_EQ_INT(4, (int)GS_ARRAY_LEN(text));
}

// FNV-1a: the published test vectors, the string and byte forms agreeing,
// and chaining over a split.
TEST(fnv1a_matches_published_vectors) {
    ASSERT_TRUE(fnv1a32_str(FNV1A32_INIT, "") == 0x811c9dc5u);
    ASSERT_TRUE(fnv1a32_str(FNV1A32_INIT, "a") == 0xe40c292cu);
    ASSERT_TRUE(fnv1a32_str(FNV1A32_INIT, "foobar") == 0xbf9cf968u);
    ASSERT_TRUE(fnv1a64_str(FNV1A64_INIT, "") == 0xcbf29ce484222325ull);
    ASSERT_TRUE(fnv1a64_str(FNV1A64_INIT, "a") == 0xaf63dc4c8601ec8cull);
    ASSERT_TRUE(fnv1a64_str(FNV1A64_INIT, "foobar") == 0x85944171f73967e8ull);
    ASSERT_TRUE(fnv1a32(FNV1A32_INIT, "foobar", 6) == 0xbf9cf968u);
    ASSERT_TRUE(fnv1a64(fnv1a64(FNV1A64_INIT, "foo", 3), "bar", 3) == 0x85944171f73967e8ull);
    // A byte above 0x7F is folded in unsigned
    ASSERT_TRUE(fnv1a32_str(FNV1A32_INIT, "\xe9") == fnv1a32(FNV1A32_INIT, "\xe9", 1));
}

// hex_digit reads either case and refuses everything else; hex_encode writes
// two digits a byte in the case asked for, terminated.
TEST(hex_digit_and_encode) {
    ASSERT_EQ_INT(0, hex_digit('0'));
    ASSERT_EQ_INT(9, hex_digit('9'));
    ASSERT_EQ_INT(10, hex_digit('a'));
    ASSERT_EQ_INT(15, hex_digit('F'));
    ASSERT_EQ_INT(-1, hex_digit('g'));
    ASSERT_EQ_INT(-1, hex_digit('\0'));
    ASSERT_EQ_INT(-1, hex_digit(0xE9));
    static const uint8_t bytes[] = {0x00, 0x9F, 0xA5, 0xFF};
    char out[2 * sizeof bytes + 1];
    hex_encode(out, bytes, sizeof bytes, false);
    ASSERT_TRUE(strcmp(out, "009fa5ff") == 0);
    hex_encode(out, bytes, sizeof bytes, true);
    ASSERT_TRUE(strcmp(out, "009FA5FF") == 0);
    hex_encode(out, bytes, 0, true);
    ASSERT_TRUE(out[0] == '\0');
}

int main(void) {
    RUN(array_len_counts_elements);
    RUN(fnv1a_matches_published_vectors);
    RUN(hex_digit_and_encode);
    fprintf(stderr, "All common tests passed\n");
    return 0;
}
