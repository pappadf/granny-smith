// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for the Disk Copy 6.x / NDIF decoder: the ADC decompressor
// (src/core/storage/adc.c) and the 'bcem' block-map parser + chunk decoder
// (src/core/storage/image_ndif.c).  The 'bcem' layout is built by hand here
// so the tests are self-contained (no external image fixtures); the byte
// layout matches real Disk Copy 6.3.3 images and Aaru's NDIF Structs.cs.

#include "adc.h"
#include "image_ndif.h"
#include "resource_fork.h"
#include "test_assert.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void w_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void w_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// ---- ADC decompressor ------------------------------------------------------

TEST(adc_literal_run) {
    // 0x82 = 0x80 | 2 -> literal run of length 3, then 3 literal bytes.
    uint8_t in[] = {0x82, 'A', 'B', 'C'};
    uint8_t out[8] = {0};
    long n = adc_decompress(in, sizeof(in), out, sizeof(out));
    ASSERT_EQ_INT(3, (int)n);
    ASSERT_EQ_INT(0, memcmp(out, "ABC", 3));
}

TEST(adc_short_match_rle) {
    // literal 'A', then a short match len=3 off=0 -> repeats the last byte
    // (overlapping copy = RLE) producing "AAAA".
    uint8_t in[] = {0x80, 'A', 0x00, 0x00};
    uint8_t out[8] = {0};
    long n = adc_decompress(in, sizeof(in), out, sizeof(out));
    ASSERT_EQ_INT(4, (int)n);
    ASSERT_EQ_INT(0, memcmp(out, "AAAA", 4));
}

TEST(adc_long_match) {
    // literal "ABCD", then a long match len=4 off=3 copies from the start ->
    // "ABCDABCD".
    uint8_t in[] = {0x83, 'A', 'B', 'C', 'D', 0x40, 0x00, 0x03};
    uint8_t out[16] = {0};
    long n = adc_decompress(in, sizeof(in), out, sizeof(out));
    ASSERT_EQ_INT(8, (int)n);
    ASSERT_EQ_INT(0, memcmp(out, "ABCDABCD", 8));
}

TEST(adc_backref_before_start_is_error) {
    // A short match as the very first token references before the output
    // start -> must fail rather than read out of bounds.
    uint8_t in[] = {0x00, 0x00};
    uint8_t out[8] = {0};
    long n = adc_decompress(in, sizeof(in), out, sizeof(out));
    ASSERT_EQ_INT(-1, (int)n);
}

TEST(adc_respects_out_cap) {
    // A long RLE run clamped by a small output buffer stops at out_cap.
    uint8_t in[] = {0x80, 'Z', 0x7F, 0x00, 0x00}; // literal Z + long match len 67
    uint8_t out[5] = {0};
    long n = adc_decompress(in, sizeof(in), out, sizeof(out));
    ASSERT_EQ_INT(5, (int)n);
    ASSERT_EQ_INT(0, memcmp(out, "ZZZZZ", 5));
}

// Build an ADC stream that decodes to `n` copies of byte `v` into `out`
// (assumed large enough).  Returns the encoded length.
static size_t adc_fill(uint8_t *out, uint8_t v, size_t n) {
    size_t o = 0, produced = 0;
    out[o++] = 0x80; // literal run length 1
    out[o++] = v;
    produced = 1;
    while (produced < n) {
        size_t remain = n - produced;
        if (remain >= 4) {
            size_t len = remain > 67 ? 67 : remain;
            out[o++] = (uint8_t)(0x40 | (len - 4)); // long match, off=0
            out[o++] = 0;
            out[o++] = 0;
            produced += len;
        } else {
            size_t len = remain; // 1..3 -> short match (min len 3) or literal
            if (len >= 3) {
                out[o++] = (uint8_t)(((len - 3) << 2) | 0);
                out[o++] = 0;
            } else {
                out[o++] = (uint8_t)(0x80 | (len - 1));
                for (size_t k = 0; k < len; k++)
                    out[o++] = v;
            }
            produced += len;
        }
    }
    return o;
}

// ---- 'bcem' block-map builders --------------------------------------------

// Build a 'bcem' body: 128-byte header + `n` 12-byte descriptors.
static uint8_t *build_bcem(uint32_t sectors, uint32_t crc, const char *name, const uint32_t *words,
                           const uint32_t *offs, const uint32_t *lens, uint32_t n, size_t *out_len) {
    size_t len = 128 + (size_t)n * 12;
    uint8_t *b = calloc(1, len);
    w_u16(b + 0x00, 11); // version
    w_u16(b + 0x02, 0); // driver (HFS)
    size_t nl = strlen(name);
    if (nl > 63)
        nl = 63;
    b[0x04] = (uint8_t)nl;
    memcpy(b + 0x05, name, nl);
    w_u32(b + 0x44, sectors);
    w_u32(b + 0x48, 128); // maxSectorsPerChunk
    w_u32(b + 0x4C, 0); // dataOffset
    w_u32(b + 0x50, crc);
    w_u32(b + 0x7C, n); // chunk count
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *e = b + 128 + (size_t)i * 12;
        w_u32(e + 0, words[i]);
        w_u32(e + 4, offs[i]);
        w_u32(e + 8, lens[i]);
    }
    *out_len = len;
    return b;
}

// Wrap a 'bcem' body in a minimal resource fork (single resource, id 128).
static uint8_t *build_bcem_fork(const uint8_t *bcem, size_t bcem_len, size_t *out_len) {
    size_t data_area = 4 + bcem_len;
    size_t map_area = 50; // 28-byte preamble + 2 + 8 (one type) + 12 (one ref)
    size_t fork_len = 16 + data_area + map_area;
    uint8_t *buf = calloc(1, fork_len);
    uint32_t data_off = 16;
    uint32_t map_off = (uint32_t)(16 + data_area);
    w_u32(buf + 0, data_off);
    w_u32(buf + 4, map_off);
    w_u32(buf + 8, (uint32_t)data_area);
    w_u32(buf + 12, (uint32_t)map_area);
    // Data record: 4-byte length prefix + body.
    w_u32(buf + data_off, (uint32_t)bcem_len);
    memcpy(buf + data_off + 4, bcem, bcem_len);
    // Map area.
    uint8_t *map = buf + map_off;
    uint16_t tlo = 28; // type-list offset
    uint16_t ref_start = (uint16_t)(tlo + 2 + 8);
    uint16_t nlo = (uint16_t)(ref_start + 12);
    w_u16(map + 24, tlo);
    w_u16(map + 26, nlo);
    w_u16(map + tlo, 0); // num types - 1 = 0
    uint8_t *te = map + tlo + 2;
    te[0] = 'b';
    te[1] = 'c';
    te[2] = 'e';
    te[3] = 'm';
    w_u16(te + 4, 0); // count - 1 = 0
    w_u16(te + 6, (uint16_t)(ref_start - tlo));
    uint8_t *re = map + ref_start;
    w_u16(re, 128); // resource id
    re[2] = 0xFF; // name offset -1 high byte
    re[3] = 0xFF;
    re[4] = 0; // attrs
    re[5] = re[6] = re[7] = 0; // data offset 0
    *out_len = fork_len;
    return buf;
}

// ---- 'bcem' parse + chunk decode ------------------------------------------

TEST(ndif_detect_and_parse) {
    // Two chunks: sector 0 = COPY (1 sector), sector 1 = ZERO (1 sector),
    // plus the END terminator at sector 2.  sectors = 2.
    uint32_t words[] = {(0u << 8) | NDIF_CHUNK_COPY, (1u << 8) | NDIF_CHUNK_ZERO, (2u << 8) | NDIF_CHUNK_END};
    uint32_t offs[] = {0, 512, 0};
    uint32_t lens[] = {512, 0, 0};
    size_t blen = 0;
    uint8_t *bcem = build_bcem(2, 0x1234abcd, "Test Vol", words, offs, lens, 3, &blen);
    size_t flen = 0;
    uint8_t *fork = build_bcem_fork(bcem, blen, &flen);

    ASSERT_TRUE(ndif_detect(fork, flen));

    ndif_map_t *m = NULL;
    ASSERT_EQ_INT(0, ndif_parse(fork, flen, &m));
    ASSERT_TRUE(m != NULL);
    ASSERT_EQ_INT(2, (int)m->sectors);
    ASSERT_EQ_INT((int)0x1234abcd, (int)m->crc);
    ASSERT_EQ_INT(0, strcmp(m->volume_name, "Test Vol"));
    ASSERT_EQ_INT(2, (int)m->n_chunks); // END dropped
    ASSERT_EQ_INT(0, (int)m->chunks[0].sector);
    ASSERT_EQ_INT(1, (int)m->chunks[0].count);
    ASSERT_EQ_INT(NDIF_CHUNK_COPY, m->chunks[0].type);
    ASSERT_EQ_INT(1, (int)m->chunks[1].sector);
    ASSERT_EQ_INT(1, (int)m->chunks[1].count);
    ASSERT_EQ_INT(NDIF_CHUNK_ZERO, m->chunks[1].type);

    ndif_map_free(m);
    free(bcem);
    free(fork);
}

TEST(ndif_detect_false_on_plain_fork) {
    // A fork with no 'bcem' resource is not NDIF.
    uint8_t junk[64];
    memset(junk, 0, sizeof(junk));
    ASSERT_TRUE(!ndif_detect(junk, sizeof(junk)));
}

TEST(ndif_decode_copy_zero_adc) {
    // Chunk 0: ADC of 512 bytes of 0xCD.  Chunk 1: COPY of 512 bytes of 0x11.
    // Chunk 2: ZERO.  Chunk 3: END.  sectors = 3.
    uint8_t adc_stream[1024];
    size_t adc_len = adc_fill(adc_stream, 0xCD, 512);

    uint32_t words[] = {(0u << 8) | NDIF_CHUNK_ADC, (1u << 8) | NDIF_CHUNK_COPY, (2u << 8) | NDIF_CHUNK_ZERO,
                        (3u << 8) | NDIF_CHUNK_END};
    uint32_t offs[] = {0, (uint32_t)adc_len, (uint32_t)adc_len + 512, 0};
    uint32_t lens[] = {(uint32_t)adc_len, 512, 0, 0};
    size_t blen = 0;
    uint8_t *bcem = build_bcem(3, 0, "X", words, offs, lens, 4, &blen);
    size_t flen = 0;
    uint8_t *fork = build_bcem_fork(bcem, blen, &flen);

    ndif_map_t *m = NULL;
    ASSERT_EQ_INT(0, ndif_parse(fork, flen, &m));
    ASSERT_EQ_INT(3, (int)m->n_chunks);

    uint8_t sect[512];

    // ADC chunk -> 512 x 0xCD.
    ASSERT_EQ_INT(0, ndif_decode_chunk(&m->chunks[0], adc_stream, adc_len, sect, sizeof(sect)));
    for (int i = 0; i < 512; i++)
        ASSERT_EQ_INT(0xCD, sect[i]);

    // COPY chunk -> verbatim.
    uint8_t raw[512];
    memset(raw, 0x11, sizeof(raw));
    ASSERT_EQ_INT(0, ndif_decode_chunk(&m->chunks[1], raw, sizeof(raw), sect, sizeof(sect)));
    for (int i = 0; i < 512; i++)
        ASSERT_EQ_INT(0x11, sect[i]);

    // ZERO chunk -> zeros (no source bytes).
    memset(sect, 0xAA, sizeof(sect));
    ASSERT_EQ_INT(0, ndif_decode_chunk(&m->chunks[2], NULL, 0, sect, sizeof(sect)));
    for (int i = 0; i < 512; i++)
        ASSERT_EQ_INT(0, sect[i]);

    ndif_map_free(m);
    free(bcem);
    free(fork);
}

TEST(ndif_decode_rejects_unsupported_type) {
    ndif_chunk_t c = {.sector = 0, .count = 1, .type = NDIF_CHUNK_KENCODE, .offset = 0, .length = 4};
    uint8_t src[4] = {0};
    uint8_t dst[512];
    ASSERT_EQ_INT(-EINVAL, ndif_decode_chunk(&c, src, sizeof(src), dst, sizeof(dst)));
}

int main(void) {
    RUN(adc_literal_run);
    RUN(adc_short_match_rle);
    RUN(adc_long_match);
    RUN(adc_backref_before_start_is_error);
    RUN(adc_respects_out_cap);
    RUN(ndif_detect_and_parse);
    RUN(ndif_detect_false_on_plain_fork);
    RUN(ndif_decode_copy_zero_adc);
    RUN(ndif_decode_rejects_unsupported_type);
    fprintf(stderr, "All ndif tests passed.\n");
    return 0;
}
