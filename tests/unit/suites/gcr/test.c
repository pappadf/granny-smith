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

// === Weak bits ==============================================================

// Append cells from a string of '0' and '1'.
static void cells_str(cells_t *c, const char *s) {
    for (; *s; s++, c->n++)
        if (*s == '1')
            c->bits[c->n >> 3] |= (uint8_t)(0x80 >> (c->n & 7));
}

// Append `n` zero cells.
static void cells_zeros(cells_t *c, uint32_t n) {
    c->n += n;
}

static uint32_t cell(const uint8_t *bits, uint32_t k) {
    return (bits[k >> 3] >> (7 - (k & 7))) & 1u;
}

// Runs of 3, 10, 12 and 17 cells: only the 12 and the 17 are as long as 12.
TEST(test_zero_runs_threshold) {
    cells_t c = {0};
    cells_str(&c, "1");
    cells_zeros(&c, 3);
    cells_str(&c, "1");
    cells_zeros(&c, 10);
    cells_str(&c, "1");
    cells_zeros(&c, 12);
    cells_str(&c, "11");
    cells_zeros(&c, 17);
    cells_str(&c, "1101");
    gcr_weak_run_t r[8];
    ASSERT_EQ_INT(2, (int)gcr_zero_runs(c.bits, c.n, 12, r, 8));
    ASSERT_EQ_INT(16, (int)r[0].start);
    ASSERT_EQ_INT(12, (int)r[0].len);
    ASSERT_EQ_INT(30, (int)r[1].start);
    ASSERT_EQ_INT(17, (int)r[1].len);
    // Counting without room, and a cap smaller than the count.
    ASSERT_EQ_INT(2, (int)gcr_zero_runs(c.bits, c.n, 12, NULL, 0));
    ASSERT_EQ_INT(4, (int)gcr_zero_runs(c.bits, c.n, 3, r, 1));
    ASSERT_EQ_INT(3, (int)r[0].len); // the first is written
    // Every rotation: the 17-cell run is found exactly once, whole, even
    // where the index cuts it; the 12-cell one likewise.
    for (uint32_t by = 0; by < c.n; by++) {
        cells_t t;
        cells_rotate(&c, by, &t);
        size_t n = gcr_zero_runs(t.bits, t.n, 12, r, 8);
        ASSERT_EQ_INT(2, (int)n);
        int seen12 = 0, seen17 = 0;
        for (size_t i = 0; i < n; i++) {
            ASSERT_TRUE(r[i].start < t.n);
            if (i)
                ASSERT_TRUE(r[i].start > r[i - 1].start); // in order
            for (uint32_t k = 0; k < r[i].len; k++)
                ASSERT_EQ_INT(0, (int)cell(t.bits, (r[i].start + k) % t.n));
            ASSERT_EQ_INT(1, (int)cell(t.bits, (r[i].start + r[i].len) % t.n));
            ASSERT_EQ_INT(1, (int)cell(t.bits, (r[i].start + t.n - 1) % t.n));
            seen12 += r[i].len == 12;
            seen17 += r[i].len == 17;
        }
        ASSERT_EQ_INT(1, seen12);
        ASSERT_EQ_INT(1, seen17);
    }
    // A track with no transition at all is one run, all round.
    cells_t z = {0};
    cells_zeros(&z, 100);
    ASSERT_EQ_INT(1, (int)gcr_zero_runs(z.bits, z.n, 12, r, 8));
    ASSERT_EQ_INT(0, (int)r[0].start);
    ASSERT_EQ_INT(100, (int)r[0].len);
}

// A record of a weak area as protection lays it down: F6 AF xx yy 96 F6,
// then 16 cells with no transition (17 with the last byte's own zero).
static void cells_records(cells_t *c, int count) {
    for (int i = 0; i < 12; i++)
        cells_byte(c, 0xFF, true);
    for (int i = 0; i < count; i++) {
        const uint8_t rec[6] = {0xF6, 0xAF, gcr_encode6[i & 63], gcr_encode6[(i * 5 + 1) & 63], 0x96, 0xF6};
        for (int k = 0; k < 6; k++)
            cells_byte(c, rec[k], false);
        cells_zeros(c, 16);
    }
    for (int i = 0; i < 12; i++)
        cells_byte(c, 0xFF, true);
}

// Records F6 AF xx yy 96 F6 a framed revolution holds.
static int count_records(const uint8_t *b, size_t n) {
    int found = 0;
    for (size_t i = 0; i + 6 <= n; i++)
        found += b[i] == 0xF6 && b[i + 1] == 0xAF && b[i + 4] == 0x96 && b[i + 5] == 0xF6;
    return found;
}

// With no runs the noisy framer is the plain one; with runs, a seed gives
// the same bytes every time and another seed other bytes, and the noise
// slips the framing so a revolution loses records the clean one has.
TEST(test_frame_revolution_noisy) {
    cells_t c = {0};
    cells_records(&c, 40);
    uint8_t plain[1024], a[1024], b[1024];
    size_t np = gcr_frame_revolution(c.bits, c.n, plain, sizeof plain);
    ASSERT_EQ_INT(40, count_records(plain, np));
    ASSERT_EQ_INT((int)np, (int)gcr_frame_revolution_noisy(c.bits, c.n, NULL, 0, 1, 2, a, sizeof a));
    ASSERT_EQ_INT(0, memcmp(plain, a, np));
    gcr_weak_run_t runs[64];
    size_t nr = gcr_zero_runs(c.bits, c.n, 12, runs, 64);
    ASSERT_EQ_INT(40, (int)nr);
    size_t na = gcr_frame_revolution_noisy(c.bits, c.n, runs, nr, 6, 7, a, sizeof a);
    size_t nb = gcr_frame_revolution_noisy(c.bits, c.n, runs, nr, 6, 7, b, sizeof b);
    ASSERT_TRUE(na > 0);
    ASSERT_TRUE(na == nb && memcmp(a, b, na) == 0); // the same draw
    int fewer = 0;
    for (uint64_t seed = 8; seed < 24; seed++) {
        nb = gcr_frame_revolution_noisy(c.bits, c.n, runs, nr, seed - 1, seed, b, sizeof b);
        ASSERT_TRUE(nb > 0);
        ASSERT_TRUE(nb != na || memcmp(a, b, na) != 0); // another draw
        fewer += count_records(b, nb) < 40;
    }
    ASSERT_TRUE(fewer > 0);
    // No room: nothing.
    ASSERT_EQ_INT(0, (int)gcr_frame_revolution_noisy(c.bits, c.n, runs, nr, 6, 7, a, 8));
}

// The noise is only ever inside a run, never within GCR_WEAK_GRACE cells
// after a transition (captured or invented), and is there at all.
TEST(test_weak_fill_grace) {
    cells_t c = {0};
    cells_records(&c, 20);
    gcr_weak_run_t runs[32];
    size_t nr = gcr_zero_runs(c.bits, c.n, 12, runs, 32);
    ASSERT_EQ_INT(20, (int)nr);
    int injected = 0;
    for (uint64_t seed = 0; seed < 64; seed++) {
        uint8_t out[sizeof c.bits];
        gcr_weak_fill(c.bits, c.n, runs, nr, seed, out);
        for (uint32_t k = 0; k < c.n; k++) {
            uint32_t was = cell(c.bits, k), now = cell(out, k);
            if (was) {
                ASSERT_EQ_INT(1, (int)now); // a transition is never lost
                continue;
            }
            if (!now)
                continue;
            injected++;
            bool in_run = false;
            for (size_t r = 0; r < nr; r++)
                in_run |= (k + c.n - runs[r].start) % c.n < runs[r].len;
            ASSERT_TRUE(in_run);
            for (uint32_t g = 1; g <= GCR_WEAK_GRACE; g++)
                ASSERT_EQ_INT(0, (int)cell(out, (k + c.n - g) % c.n));
        }
    }
    // About 30 % of the 13 cells past the grace in each of 20 runs, at
    // 64 seeds, less what the grace after each invented bit takes back.
    ASSERT_TRUE(injected > 64 * 20);
}

int main(void) {
    RUN(test_codeword_table_shape);
    RUN(test_decode_table_inverts_encode);
    RUN(test_sector_round_trip);
    RUN(test_sector_from_disk);
    RUN(test_address_field);
    RUN(test_frame_revolution_is_steady);
    RUN(test_frame_revolution_straddling_byte);
    RUN(test_zero_runs_threshold);
    RUN(test_frame_revolution_noisy);
    RUN(test_weak_fill_grace);
    printf("All gcr tests passed\n");
    return 0;
}
