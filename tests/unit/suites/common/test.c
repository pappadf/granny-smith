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

// The byte-order accessors at an odd offset: each width both ways, the
// signed 16-bit read, the 24-bit read, and an argument evaluated once.
TEST(byte_order_accessors) {
    uint8_t b[17] = {0};
    for (int i = 0; i < 16; i++)
        b[i + 1] = (uint8_t)(0xF1 + i);
    const uint8_t *p = b + 1;
    ASSERT_TRUE(RD_BE16(p) == 0xF1F2u);
    ASSERT_TRUE(RD_LE16(p) == 0xF2F1u);
    ASSERT_TRUE(RD_BE16S(p) == (int16_t)-3598);
    ASSERT_TRUE(RD_BE24(p) == 0xF1F2F3u);
    ASSERT_TRUE(RD_BE32(p) == 0xF1F2F3F4u);
    ASSERT_TRUE(RD_LE32(p) == 0xF4F3F2F1u);
    ASSERT_TRUE(RD_BE64(p) == 0xF1F2F3F4F5F6F7F8ull);
    ASSERT_TRUE(RD_LE64(p) == 0xF8F7F6F5F4F3F2F1ull);
    uint8_t w[9] = {0};
    WR_LE64(w + 1, 0x0807060504030201ull);
    for (int i = 0; i < 8; i++)
        ASSERT_EQ_INT(i + 1, w[i + 1]);
    WR_BE64(w + 1, 0x0102030405060708ull);
    ASSERT_TRUE(RD_LE64(w + 1) == 0x0807060504030201ull);
    // A side-effecting argument is expanded once
    const uint8_t *q = p;
    ASSERT_TRUE(RD_BE24(q++) == 0xF1F2F3u);
    ASSERT_TRUE(q == p + 1);
}

// The Mac epoch: 1904-01-01 is Unix -2082844800, 1970-01-01 is Mac
// 2082844800, and 2000-01-01 (946684800) is the AppleDouble epoch.
TEST(mac_epoch_round_trips) {
    ASSERT_TRUE(mac_to_unix64(0) == -2082844800LL);
    ASSERT_TRUE(mac_to_unix64(2082844800u) == 0);
    ASSERT_TRUE(unix_to_mac64(946684800) == 3029529600LL);
    ASSERT_TRUE(mac_to_unix64(UINT32_MAX) == 2212122495LL); // the 2040 wrap
    ASSERT_TRUE(unix_to_mac64(mac_to_unix64(123456789u)) == 123456789LL);
}

int main(void) {
    RUN(mac_epoch_round_trips);
    RUN(array_len_counts_elements);
    RUN(fnv1a_matches_published_vectors);
    RUN(hex_digit_and_encode);
    RUN(byte_order_accessors);
    fprintf(stderr, "All common tests passed\n");
    return 0;
}
