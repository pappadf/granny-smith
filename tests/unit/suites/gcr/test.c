// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for the shared 6-and-2 GCR codec (src/core/storage/gcr.c).
// See Makefile.

#include "gcr.h"
#include "test_assert.h"

#include <stdbool.h>
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

// === Steady-state framing ===================================================

// A bitstream built from disk bytes: each byte eight cells, or a self-sync
// byte ($FF and two zero cells) when `sync` is set for it.
typedef struct {
    uint8_t bits[2048];
    uint32_t n;
} cells_t;

static void cells_byte(cells_t *c, uint8_t v, bool sync) {
    int width = sync ? 10 : 8;
    uint32_t w = sync ? (uint32_t)v << 2 : v;
    for (int k = width - 1; k >= 0; k--, c->n++)
        if ((w >> k) & 1)
            c->bits[c->n >> 3] |= (uint8_t)(0x80 >> (c->n & 7));
}

// `c` rotated left by `by` cells: the capture starting mid-stream.
static void cells_rotate(const cells_t *c, uint32_t by, cells_t *out) {
    memset(out, 0, sizeof *out);
    out->n = c->n;
    for (uint32_t i = 0; i < c->n; i++) {
        uint32_t from = (i + by) % c->n;
        if ((c->bits[from >> 3] >> (7 - (from & 7))) & 1)
            out->bits[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
    }
}

// The reference: frame `revs` revolutions from a cold register, keeping each
// revolution's bytes apart (as projects that scan captures do it).
static size_t frame_rev(const cells_t *c, int rev, uint8_t *out) {
    uint32_t reg = 0;
    size_t n = 0;
    for (int r = 0; r <= rev; r++)
        for (uint32_t k = 0; k < c->n; k++) {
            reg = ((reg << 1) | ((c->bits[k >> 3] >> (7 - (k & 7))) & 1)) & 0xFF;
            if (reg & 0x80) {
                if (r == rev)
                    out[n++] = (uint8_t)reg;
                reg = 0;
            }
        }
    return n;
}

// The second revolution is what every later one is: framing it again gives
// the same bytes, so a buffer that loops it is the steady state.
TEST(test_frame_revolution_is_steady) {
    // A sector-like stream: sync, marks, values, more sync.
    cells_t c = {0};
    for (int i = 0; i < 9; i++)
        cells_byte(&c, 0xFF, true);
    static const uint8_t marks[] = {0xD5, 0xAA, 0x96, 0x96, 0x97, 0x9A, 0xDE, 0xAA, 0xFF};
    for (size_t i = 0; i < sizeof marks; i++)
        cells_byte(&c, marks[i], false);
    for (int i = 0; i < 100; i++)
        cells_byte(&c, gcr_encode6[(i * 7) & 63], false);
    for (int i = 0; i < 7; i++)
        cells_byte(&c, 0xFF, true);
    for (uint32_t by = 0; by < 24; by++) {
        cells_t r;
        cells_rotate(&c, by * 37 % c.n, &r);
        uint8_t rev2[400], rev3[400], got[400];
        size_t n2 = frame_rev(&r, 1, rev2), n3 = frame_rev(&r, 2, rev3);
        ASSERT_EQ_INT((int)n2, (int)n3);
        ASSERT_EQ_INT(0, memcmp(rev2, rev3, n2));
        size_t n = gcr_frame_revolution(r.bits, r.n, got, sizeof got);
        ASSERT_EQ_INT((int)n2, (int)n);
        ASSERT_EQ_INT(0, memcmp(got, rev2, n));
    }
}

// A byte that straddles the index is framed once, whole: wherever the
// capture starts, the revolution is a rotation of the bytes the stream was
// built from (self-sync first, so the framer locks on), each exactly once.
TEST(test_frame_revolution_straddling_byte) {
    static const uint8_t bytes[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xD5, 0xAA, 0x96,
                                    0xB5, 0x96, 0xFA, 0xB5, 0xDE, 0xAA, 0xEB, 0xCD, 0x9F};
    const size_t nb = sizeof bytes;
    cells_t c = {0};
    for (size_t i = 0; i < nb; i++)
        cells_byte(&c, bytes[i], i < 6); // six ten-cell sync bytes
    for (uint32_t by = 1; by < c.n; by += 3) {
        cells_t r;
        cells_rotate(&c, by, &r);
        uint8_t got[64];
        size_t n = gcr_frame_revolution(r.bits, r.n, got, sizeof got);
        ASSERT_EQ_INT((int)nb, (int)n);
        size_t at = 0;
        while (at < n && got[at] != 0xD5)
            at++;
        ASSERT_TRUE(at < n);
        for (size_t i = 0; i < nb; i++)
            ASSERT_EQ_HEX(got[(at + i) % n], bytes[(6 + i) % nb]);
    }
    // A cap too small for the revolution frames nothing.
    uint8_t small[4];
    ASSERT_EQ_INT(0, (int)gcr_frame_revolution(c.bits, c.n, small, sizeof small));
}

int main(void) {
    RUN(test_codeword_table_shape);
    RUN(test_decode_table_inverts_encode);
    RUN(test_sector_round_trip);
    RUN(test_sector_from_disk);
    RUN(test_address_field);
    RUN(test_frame_revolution_is_steady);
    RUN(test_frame_revolution_straddling_byte);
    printf("All gcr tests passed\n");
    return 0;
}
