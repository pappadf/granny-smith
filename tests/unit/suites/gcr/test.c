// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for the shared 6-and-2 GCR codec (src/core/storage/gcr.c).
// See Makefile.

#include "gcr.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Deterministic sector contents.
static void fill(uint8_t tag[GCR_TAG_BYTES], uint8_t data[GCR_DATA_BYTES], uint32_t seed) {
    uint32_t x = seed;
    for (int i = 0; i < GCR_DATA_BYTES; i++) {
        x = x * 1103515245u + 12345u;
        data[i] = (uint8_t)(x >> 16);
    }
    for (int i = 0; i < GCR_TAG_BYTES; i++)
        tag[i] = (uint8_t)(seed * 13 + i);
}

// 64 six-bit values map to 64 legal disk bytes: every codeword has bit 7
// set, and the table is strictly increasing (so it is a bijection).
TEST(test_codeword_table_shape) {
    for (int i = 0; i < 64; i++) {
        ASSERT_TRUE(gcr_encode6[i] >= 0x96);
        if (i > 0)
            ASSERT_TRUE(gcr_encode6[i] > gcr_encode6[i - 1]);
    }
}

// The decode table is the encode table's exact inverse over all 256 bytes:
// a codeword decodes to its value, anything else to GCR_BAD_CODEWORD (the
// old `& 0x7F`-keyed table folded $16 onto $96 and so on).
TEST(test_decode_table_inverts_encode) {
    int legal = 0;
    for (int b = 0; b < 256; b++) {
        int want = GCR_BAD_CODEWORD;
        for (int v = 0; v < 64; v++)
            if (gcr_encode6[v] == b)
                want = v;
        ASSERT_EQ_INT(want, gcr_decode6[b]);
        legal += want != GCR_BAD_CODEWORD;
    }
    ASSERT_EQ_INT(64, legal);
}

// A sector survives nibblize -> denibblize with its checksum, over random
// and edge payloads (the chain's carry feed and its final pair are where a
// transcription goes wrong); a bad checksum is laid down on request and
// detected.  Every value written is six bits.
TEST(test_sector_round_trip) {
    uint8_t tag[GCR_TAG_BYTES], data[GCR_DATA_BYTES], v[GCR_SECTOR_VALUES];
    uint8_t out_tag[GCR_TAG_BYTES], out[GCR_DATA_BYTES];
    for (uint32_t seed = 0; seed < 20; seed++) {
        fill(tag, data, seed);
        if (seed < 4) {
            static const uint8_t fills[] = {0x00, 0xFF, 0x80, 0x7F};
            memset(tag, fills[seed], sizeof tag);
            memset(data, fills[seed], sizeof data);
        }
        for (int bad = 0; bad < 2; bad++) {
            gcr_sector_nibblize(tag, data, bad, v);
            for (int i = 0; i < GCR_SECTOR_VALUES; i++)
                ASSERT_TRUE(v[i] < 64);
            memset(out, 0, sizeof out);
            memset(out_tag, 0, sizeof out_tag);
            ASSERT_EQ_INT(!bad, gcr_sector_denibblize(v, out_tag, out));
            ASSERT_TRUE(memcmp(out, data, sizeof data) == 0);
            ASSERT_TRUE(memcmp(out_tag, tag, sizeof tag) == 0);
        }
    }
}

// The disk-byte form: the codewords decode and the checksum verifies; one
// byte that is no codeword is refused (-1) without writing the outputs; a
// flipped data value fails the checksum (0).
TEST(test_sector_from_disk) {
    uint8_t tag[GCR_TAG_BYTES], data[GCR_DATA_BYTES], v[GCR_SECTOR_VALUES], disk[GCR_SECTOR_VALUES];
    uint8_t out_tag[GCR_TAG_BYTES], out[GCR_DATA_BYTES];
    fill(tag, data, 99);
    gcr_sector_nibblize(tag, data, false, v);
    for (int i = 0; i < GCR_SECTOR_VALUES; i++)
        disk[i] = gcr_encode6[v[i]];
    ASSERT_EQ_INT(1, gcr_sector_from_disk(disk, out_tag, out));
    ASSERT_TRUE(memcmp(out, data, sizeof data) == 0);
    ASSERT_TRUE(memcmp(out_tag, tag, sizeof tag) == 0);

    disk[300] = gcr_encode6[(v[300] + 1) & 0x3F];
    ASSERT_EQ_INT(0, gcr_sector_from_disk(disk, out_tag, out));

    memset(out, 0xEE, sizeof out);
    disk[300] = 0x16; // no codeword
    ASSERT_EQ_INT(-1, gcr_sector_from_disk(disk, out_tag, out));
    for (int i = 0; i < GCR_DATA_BYTES; i++)
        ASSERT_EQ_INT(0xEE, out[i]);
}

// The address field over every track, sector and side: encoded, laid down
// as codewords and read back; a wrong checksum or a non-codeword refused.
TEST(test_address_field) {
    uint8_t a[GCR_ADDR_VALUES], disk[GCR_ADDR_VALUES];
    for (int track = 0; track < 80; track++)
        for (int sector = 0; sector < 12; sector++)
            for (int side = 0; side < 2; side++) {
                gcr_addr_encode(track, sector, side, 0x22, a);
                for (int i = 0; i < GCR_ADDR_VALUES; i++) {
                    ASSERT_TRUE(a[i] < 64);
                    disk[i] = gcr_encode6[a[i]];
                }
                int t = -1, s = -1, sd = -1;
                uint8_t fmt = 0;
                ASSERT_TRUE(gcr_addr_from_disk(disk, &t, &s, &sd, &fmt));
                ASSERT_EQ_INT(track, t);
                ASSERT_EQ_INT(sector, s);
                ASSERT_EQ_INT(side, sd);
                ASSERT_EQ_INT(0x22, fmt);
            }
    // Track 70, side 1: side in bit 5, track bit 6 below it.
    gcr_addr_encode(70, 3, 1, 0x22, a);
    ASSERT_EQ_INT(70 & 0x3F, a[0]);
    ASSERT_EQ_INT(0x21, a[2]);
    ASSERT_EQ_INT((70 ^ 3 ^ 0x21 ^ 0x22) & 0x3F, a[4]);
    for (int i = 0; i < GCR_ADDR_VALUES; i++)
        disk[i] = gcr_encode6[a[i]];
    disk[4] = gcr_encode6[a[4] ^ 1];
    ASSERT_TRUE(!gcr_addr_from_disk(disk, NULL, NULL, NULL, NULL));
    disk[4] = gcr_encode6[a[4]];
    disk[1] = 0x00;
    ASSERT_TRUE(!gcr_addr_from_disk(disk, NULL, NULL, NULL, NULL));
}

int main(void) {
    RUN(test_codeword_table_shape);
    RUN(test_decode_table_inverts_encode);
    RUN(test_sector_round_trip);
    RUN(test_sector_from_disk);
    RUN(test_address_field);
    printf("All gcr tests passed\n");
    return 0;
}
