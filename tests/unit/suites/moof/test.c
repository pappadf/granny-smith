// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for the Applesauce MOOF reader and writer
// (src/core/storage/image_moof.c).  Images are made by the writer from
// known sectors and then read back, so the suite needs no fixtures; the
// tests then damage them the ways real captures differ from a clean write
// -- a track starting mid-byte, a flux track, a bad sector, a blank track,
// a stale CRC -- and check what the reader makes of each.

#include "crc32.h"
#include "format_registry.h"
#include "image_moof.h"
#include "test_assert.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTORS_800K 1600
#define SECTORS_400K 800

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void wr32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

// Deterministic sector and tag contents.
static void fill(uint8_t *data, uint8_t *tags, uint32_t sectors, uint32_t seed) {
    uint32_t x = seed;
    for (size_t i = 0; i < (size_t)sectors * 512; i++) {
        x = x * 1103515245u + 12345u;
        data[i] = (uint8_t)(x >> 16);
    }
    for (size_t i = 0; tags && i < (size_t)sectors * MOOF_TAG_BYTES; i++)
        tags[i] = (uint8_t)(i * 7 + seed);
}

// Restamp the header CRC after an edit.
static void restamp(uint8_t *f, size_t len) {
    wr32(f + 8, gs_crc32(0, f + MOOF_HEADER_SIZE, len - MOOF_HEADER_SIZE));
}

// The TRK record (start block, block count, bit count) of track `t`, side `s`.
static uint8_t *trk_of(uint8_t *f, int t, int s) {
    uint8_t idx = f[88 + t * 2 + s];
    return idx == 0xFF ? NULL : f + 256 + 8 * idx;
}

TEST(moof_round_trip_800k) {
    uint8_t *data = malloc(SECTORS_800K * 512), *tags = malloc(SECTORS_800K * MOOF_TAG_BYTES);
    fill(data, tags, SECTORS_800K, 1);
    uint8_t *f = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(0, moof_encode(data, SECTORS_800K, tags, 0, &f, &len));
    ASSERT_TRUE(moof_detect(f, len));
    ASSERT_EQ_INT(0, (int)(len % 512));
    ASSERT_EQ_INT(2, f[20 + 1]); // disk type: 800K
    ASSERT_EQ_INT(16, f[20 + 4]); // 2 µs cells
    moof_disk_t d;
    ASSERT_EQ_INT(0, moof_decode(f, len, &d));
    ASSERT_EQ_INT(SECTORS_800K, (int)d.sectors);
    ASSERT_EQ_INT(0, (int)d.missing);
    ASSERT_EQ_INT(0, (int)d.bad_checksum);
    ASSERT_TRUE(!d.crc_mismatch);
    ASSERT_EQ_INT(0x22, d.format_byte);
    ASSERT_EQ_INT(0, memcmp(d.data, data, SECTORS_800K * 512));
    ASSERT_EQ_INT(0, memcmp(d.tags, tags, SECTORS_800K * MOOF_TAG_BYTES));
    moof_disk_free(&d);
    free(f);
    free(data);
    free(tags);
}

TEST(moof_round_trip_400k_without_tags) {
    uint8_t *data = malloc(SECTORS_400K * 512);
    fill(data, NULL, SECTORS_400K, 2);
    uint8_t *f = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(0, moof_encode(data, SECTORS_400K, NULL, 0, &f, &len));
    ASSERT_EQ_INT(1, f[20 + 1]); // disk type: 400K
    ASSERT_EQ_INT(0xFF, f[88 + 1]); // no side 1
    moof_disk_t d;
    ASSERT_EQ_INT(0, moof_decode(f, len, &d));
    ASSERT_EQ_INT(SECTORS_400K, (int)d.sectors);
    ASSERT_EQ_INT(0, (int)d.missing);
    ASSERT_EQ_INT(0x02, d.format_byte);
    ASSERT_EQ_INT(0, memcmp(d.data, data, SECTORS_400K * 512));
    for (size_t i = 0; i < SECTORS_400K * MOOF_TAG_BYTES; i++)
        ASSERT_EQ_INT(0, d.tags[i]);
    moof_disk_free(&d);
    free(f);
    free(data);
}

// A capture does not start on a byte boundary: rotate a track's bits by a
// few cells, and the framer must still find every sector (one straddles the
// index now).
TEST(moof_track_rotated_mid_byte) {
    uint8_t *data = malloc(SECTORS_800K * 512);
    fill(data, NULL, SECTORS_800K, 3);
    uint8_t *f = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(0, moof_encode(data, SECTORS_800K, NULL, 0, &f, &len));
    uint8_t *rec = trk_of(f, 0, 0);
    uint8_t *bits = f + (size_t)rd16(rec) * 512;
    uint32_t n = rd32(rec + 4);
    uint8_t *copy = calloc(1, (n + 7) / 8);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t from = (i + 3000 * 8 + 5) % n; // ~3000 bytes in, plus five cells
        if ((bits[from >> 3] >> (7 - (from & 7))) & 1)
            copy[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
    }
    memcpy(bits, copy, (n + 7) / 8);
    free(copy);
    restamp(f, len);
    moof_disk_t d;
    ASSERT_EQ_INT(0, moof_decode(f, len, &d));
    ASSERT_EQ_INT(0, (int)d.missing);
    ASSERT_EQ_INT(0, (int)d.bad_checksum);
    ASSERT_EQ_INT(0, memcmp(d.data, data, SECTORS_800K * 512));
    moof_disk_free(&d);
    free(f);
    free(data);
}

// A blank track (TMAP $FF) reads as missing sectors of zeros; a damaged
// data field keeps its bytes but counts as a bad checksum; a stale CRC is a
// warning, not a refusal.
TEST(moof_damage_is_reported) {
    uint8_t *data = malloc(SECTORS_800K * 512);
    fill(data, NULL, SECTORS_800K, 4);
    uint8_t *f = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(0, moof_encode(data, SECTORS_800K, NULL, 0, &f, &len));
    f[88 + 79 * 2 + 1] = 0xFF; // track 79, side 1: blank (8 sectors)
    // Flip one data nibble deep inside track 40, side 0's first sector:
    // swap a codeword for another so it still decodes.
    uint8_t *rec = trk_of(f, 40, 0);
    uint8_t *bits = f + (size_t)rd16(rec) * 512;
    // Find the first data prologue D5 AA AD byte-aligned in the writer's output
    // (the writer's sync bytes are 10 cells, so search bit by bit).
    uint32_t n = rd32(rec + 4), at = 0;
    for (uint32_t i = 0; i + 24 < n; i++) {
        uint32_t v = 0;
        for (int k = 0; k < 24; k++)
            v = v << 1 | ((bits[(i + k) >> 3] >> (7 - ((i + k) & 7))) & 1);
        if (v == 0xD5AAAD) {
            at = i + 24 + 8 + 100 * 8; // past the sector nibble, 100 nibbles in
            break;
        }
    }
    ASSERT_TRUE(at != 0);
    // 0x96 <-> 0x97 differ in the last bit: toggle it.
    uint32_t bit = at + 7;
    bits[bit >> 3] ^= (uint8_t)(0x80 >> (bit & 7));
    // The CRC is now stale.
    moof_disk_t d;
    ASSERT_EQ_INT(0, moof_decode(f, len, &d));
    ASSERT_TRUE(d.crc_mismatch);
    ASSERT_EQ_INT(8, (int)d.missing);
    ASSERT_EQ_INT(1, (int)d.bad_checksum);
    for (size_t i = (SECTORS_800K - 8) * 512; i < SECTORS_800K * 512; i++)
        ASSERT_EQ_INT(0, d.data[i]);
    moof_disk_free(&d);
    // A CRC of zero means "not computed".
    wr32(f + 8, 0);
    ASSERT_EQ_INT(0, moof_decode(f, len, &d));
    ASSERT_TRUE(!d.crc_mismatch);
    moof_disk_free(&d);
    free(f);
    free(data);
}

// A flux track (FLUX chunk) wins over its TMAP entry and decodes the same.
TEST(moof_flux_track) {
    uint8_t *data = malloc(SECTORS_400K * 512);
    fill(data, NULL, SECTORS_400K, 5);
    uint8_t *f = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(0, moof_encode(data, SECTORS_400K, NULL, 0, &f, &len));
    uint8_t *rec = trk_of(f, 0, 0);
    const uint8_t *bits = f + (size_t)rd16(rec) * 512;
    uint32_t n = rd32(rec + 4);
    // Ticks between transitions (16 per cell, a little jitter), 255 = more.
    uint8_t *flux = malloc(n * 2);
    size_t nf = 0;
    uint32_t cells = 0, first = 0;
    bool seen = false;
    for (uint32_t i = 0; i < n + first + 1; i++) {
        uint32_t k = i % n;
        cells++;
        if ((bits[k >> 3] >> (7 - (k & 7))) & 1) {
            if (!seen) { // start at the first transition, so the loop closes
                seen = true;
                first = i;
                cells = 0;
                continue;
            }
            uint32_t ticks = cells * 16 + (nf % 3) - 1; // -1, 0 or +1 tick
            while (ticks >= 255) {
                flux[nf++] = 255;
                ticks -= 255;
            }
            flux[nf++] = (uint8_t)ticks;
            cells = 0;
        }
    }
    // Lay out: flux data blocks after the file, then the FLUX chunk in a
    // block of its own.
    size_t fblocks = (nf + 511) / 512;
    size_t flux_at = len, chunk_at = len + fblocks * 512, nlen = chunk_at + 512;
    f = realloc(f, nlen);
    memset(f + len, 0, nlen - len);
    memcpy(f + flux_at, flux, nf);
    free(flux);
    uint8_t *frec = f + 256 + 8 * 80; // the first unused TRK record
    wr16(frec, (uint16_t)(flux_at / 512));
    wr16(frec + 2, (uint16_t)fblocks);
    wr32(frec + 4, (uint32_t)nf); // flux: a byte count
    memcpy(f + chunk_at, "FLUX", 4);
    wr32(f + chunk_at + 4, 160);
    memset(f + chunk_at + 8, 0xFF, 160);
    f[chunk_at + 8] = 80; // track 0, side 0
    f[88] = 0xFF; // and nothing in TMAP
    wr16(f + 20 + 40, (uint16_t)(chunk_at / 512)); // FLUX block
    wr16(f + 20 + 42, (uint16_t)fblocks); // largest flux track
    wr32(f + 252, (uint32_t)(chunk_at - 256)); // TRKS now ends at the FLUX chunk
    restamp(f, nlen);
    moof_disk_t d;
    ASSERT_EQ_INT(0, moof_decode(f, nlen, &d));
    ASSERT_EQ_INT(1, (int)d.flux_tracks);
    ASSERT_EQ_INT(0, (int)d.missing);
    ASSERT_EQ_INT(0, (int)d.bad_checksum);
    ASSERT_EQ_INT(0, memcmp(d.data, data, SECTORS_400K * 512));
    moof_disk_free(&d);
    free(f);
    free(data);
}

TEST(moof_refuses_what_it_cannot_read) {
    uint8_t *data = calloc(SECTORS_800K, 512);
    uint8_t *f = NULL;
    size_t len = 0;
    moof_disk_t d;
    ASSERT_EQ_INT(-EINVAL, moof_encode(data, 1234, NULL, 0, &f, &len));
    ASSERT_EQ_INT(0, moof_encode(data, SECTORS_800K, NULL, 0, &f, &len));
    f[20 + 1] = 3; // 1.44 MB MFM
    ASSERT_EQ_INT(-ENOTSUP, moof_decode(f, len, &d));
    f[20 + 1] = 2;
    memset(f + 88, 0xFF, 160); // every track blank: no sector at all
    ASSERT_EQ_INT(-EINVAL, moof_decode(f, len, &d));
    f[0] = 'W'; // not a MOOF
    ASSERT_TRUE(!moof_detect(f, len));
    ASSERT_EQ_INT(-EINVAL, moof_decode(f, len, &d));
    ASSERT_EQ_INT(-EINVAL, moof_decode(f, 10, &d));
    free(f);
    free(data);
}

// Through the registry: a MOOF unwraps to a DiskCopy 4.2 image, which
// unwraps to the disk ("moof+dc42"), and the tags are in its tag section.
TEST(moof_unwraps_through_the_registry) {
    uint8_t *data = malloc(SECTORS_800K * 512), *tags = malloc(SECTORS_800K * MOOF_TAG_BYTES);
    fill(data, tags, SECTORS_800K, 6);
    uint8_t *f = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(0, moof_encode(data, SECTORS_800K, tags, 0, &f, &len));
    source_t *file = source_memory(f, len, true, "mem:disk.moof");
    ASSERT_TRUE(file);
    format_unwrapped_t u;
    ASSERT_EQ_INT(0, format_unwrap(file, NULL, &u));
    ASSERT_TRUE(strcmp(u.chain, "moof+dc42") == 0);
    ASSERT_TRUE(u.dc42 != NULL);
    ASSERT_EQ_INT(SECTORS_800K * 512, (int)source_size(u.data));
    uint8_t *got = malloc(SECTORS_800K * 512);
    ASSERT_EQ_INT(0, source_read_exact(u.data, 0, got, SECTORS_800K * 512));
    ASSERT_EQ_INT(0, memcmp(got, data, SECTORS_800K * 512));
    uint8_t hdr[DISKCOPY_HEADER_SIZE];
    ASSERT_EQ_INT(0, source_read_exact(u.dc42, 0, hdr, sizeof hdr));
    uint32_t ds = 0, ts = 0;
    ASSERT_TRUE(dc42_parse_header(hdr, sizeof hdr, source_size(u.dc42), &ds, &ts));
    ASSERT_EQ_INT(SECTORS_800K * MOOF_TAG_BYTES, (int)ts);
    ASSERT_EQ_INT(1, hdr[DC42_OFF_DISK_FORMAT]); // 800K GCR
    ASSERT_EQ_INT(0x22, hdr[DC42_OFF_FORMAT_BYTE]);
    uint8_t t[MOOF_TAG_BYTES];
    ASSERT_EQ_INT(0, source_read_exact(u.dc42, DISKCOPY_HEADER_SIZE + ds + 5 * MOOF_TAG_BYTES, t, sizeof t));
    ASSERT_EQ_INT(0, memcmp(t, tags + 5 * MOOF_TAG_BYTES, MOOF_TAG_BYTES));
    format_unwrapped_free(&u);
    source_release(file);
    free(got);
    free(data);
    free(tags);
}

int main(void) {
    RUN(moof_round_trip_800k);
    RUN(moof_round_trip_400k_without_tags);
    RUN(moof_track_rotated_mid_byte);
    RUN(moof_damage_is_reported);
    RUN(moof_flux_track);
    RUN(moof_refuses_what_it_cannot_read);
    RUN(moof_unwraps_through_the_registry);
    fprintf(stderr, "All moof tests passed.\n");
    return 0;
}
