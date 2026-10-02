// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for the compressing half of the UDIF support: the deflate
// encoder (deflate.c), the CRC of a zero run (crc32.c), the streaming UDIF
// writer and verifier (udif_writer.c), and reading what it writes in place
// through the chunk-mapped source (image_chunkmap.c).

#include "crc32.h"
#include "deflate.h"
#include "image_chunkmap.h"
#include "image_udif.h"
#include "inflate.h"
#include "source.h"
#include "test_assert.h"
#include "udif_writer.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SANDBOX "_test_sandbox_udifw"

static char g_path[256];

static const char *sb_path(const char *name) {
    snprintf(g_path, sizeof(g_path), SANDBOX "/%s", name);
    return g_path;
}

static void sandbox(void) {
    mkdir(SANDBOX, 0777);
}

// Remove the sandbox and everything the tests left in it.
static void sandbox_remove(void) {
    const char *names[] = {"rt.dmg",    "l0.dmg",  "g0.dmg",   "g1.dmg",  "g2.dmg",     "g3.dmg",
                           "empty.dmg", "bad.dmg", "bad2.dmg", "big.dmg", "foreign.dmg"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        unlink(sb_path(names[i]));
    rmdir(SANDBOX);
}

// Deterministic content: a mix of zero runs, text-like (compressible) runs
// and random (incompressible) runs, in 4 KB stretches.
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)g_rng;
}

static uint8_t *make_content(size_t len, uint64_t seed) {
    g_rng = seed | 1;
    uint8_t *b = malloc(len ? len : 1);
    ASSERT_TRUE(b != NULL);
    static const char words[] = "the quick brown fox jumps over the lazy dog; System Folder Finder ";
    for (size_t at = 0; at < len; at += 4096) {
        size_t n = len - at < 4096 ? len - at : 4096;
        uint32_t kind = rnd() % 4;
        for (size_t i = 0; i < n; i++)
            b[at + i] = kind <= 1 ? 0 : kind == 2 ? (uint8_t)words[(at + i) % (sizeof(words) - 1)] : (uint8_t)rnd();
    }
    return b;
}

static long file_size(const char *path) {
    struct stat st;
    ASSERT_TRUE(stat(path, &st) == 0);
    return (long)st.st_size;
}

// ---- deflate ---------------------------------------------------------------

TEST(deflate_round_trips_at_every_level) {
    const size_t sizes[] = {0, 1, 100, 65536, 200000};
    const int levels[] = {0, 1, 6, 9};
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        uint8_t *in = make_content(sizes[s], 7 + s);
        size_t cap = deflate_bound(sizes[s]);
        uint8_t *z = malloc(cap);
        uint8_t *out = malloc(sizes[s] + 1);
        for (size_t l = 0; l < sizeof(levels) / sizeof(levels[0]); l++) {
            long n = deflate_zlib(NULL, in, sizes[s], z, cap, levels[l]);
            ASSERT_TRUE(n > 0 && (size_t)n <= cap);
            long got = inflate_zlib(z, (size_t)n, out, sizes[s] + 1);
            ASSERT_EQ_INT((int)sizes[s], (int)got);
            ASSERT_TRUE(memcmp(in, out, sizes[s]) == 0);
        }
        free(in);
        free(z);
        free(out);
    }
}

TEST(deflate_compresses_text_and_reuses_state) {
    uint8_t buf[65536];
    for (size_t i = 0; i < sizeof(buf); i++)
        buf[i] = (uint8_t) "Macintosh "[i % 10];
    deflate_state_t *st = deflate_state_new(sizeof(buf));
    uint8_t z[80000], out[65536];
    for (int round = 0; round < 3; round++) {
        long n = deflate_zlib(st, buf, sizeof(buf), z, sizeof(z), 1);
        ASSERT_TRUE(n > 0 && n < 4096);
        ASSERT_EQ_INT((int)sizeof(buf), (int)inflate_zlib(z, (size_t)n, out, sizeof(out)));
        ASSERT_TRUE(memcmp(buf, out, sizeof(buf)) == 0);
    }
    deflate_state_free(st);
    // Too small an output buffer is an error, not an overrun.
    ASSERT_EQ_INT(-1, (int)deflate_zlib(NULL, buf, sizeof(buf), z, 8, 0));
}

TEST(adler32_known_vector) {
    ASSERT_EQ_INT((int)0x11E60398u, (int)gs_adler32(1, (const uint8_t *)"Wikipedia", 9));
}

TEST(crc32_zeros_matches_the_table) {
    static uint8_t zeros[300000];
    const uint64_t lens[] = {0, 1, 2, 3, 511, 512, 65536, 299999};
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        ASSERT_EQ_INT((int)gs_crc32(0, zeros, (size_t)lens[i]), (int)gs_crc32_zeros(0, lens[i]));
        uint32_t seed = gs_crc32(0, "abc", 3);
        ASSERT_EQ_INT((int)gs_crc32(seed, zeros, (size_t)lens[i]), (int)gs_crc32_zeros(seed, lens[i]));
    }
}

// Round-trip `n` bytes at `level`; the first block's type (BTYPE).
static int round_trip_btype(const uint8_t *in, size_t n, int level) {
    size_t cap = deflate_bound(n);
    uint8_t *z = malloc(cap), *out = malloc(n + 1);
    ASSERT_TRUE(z != NULL && out != NULL);
    long zn = deflate_zlib(NULL, in, n, z, cap, level);
    ASSERT_TRUE(zn > 2);
    ASSERT_EQ_INT((int)n, (int)inflate_zlib(z, (size_t)zn, out, n + 1));
    ASSERT_TRUE(memcmp(in, out, n) == 0);
    int btype = (z[2] >> 1) & 3;
    free(z);
    free(out);
    return btype;
}

// Literal counts in Fibonacci proportion give an unlimited Huffman code
// deeper than deflate's 15 bits: the lengths must be limited and the code
// still complete.
TEST(deflate_dynamic_codes_are_length_limited) {
    static uint8_t buf[320000];
    uint64_t cnt[26] = {1, 1}, total = 0;
    for (int i = 2; i < 26; i++)
        cnt[i] = cnt[i - 1] + cnt[i - 2];
    for (int i = 0; i < 26; i++)
        total += cnt[i];
    size_t n = 0;
    g_rng = 1;
    while (n < sizeof(buf) && total) {
        uint64_t r = rnd() % total;
        int sym = 0;
        while (r >= cnt[sym])
            r -= cnt[sym++];
        buf[n++] = (uint8_t)(sym * 9 + 1);
        cnt[sym]--;
        total--;
    }
    ASSERT_EQ_INT(2, round_trip_btype(buf, n, 1));
    ASSERT_EQ_INT(2, round_trip_btype(buf, n, 9));
}

// Each block takes its smallest form: dynamic codes for skewed data, the
// fixed codes for a tiny input, a stored block for noise.
TEST(deflate_picks_the_smallest_block_form) {
    static uint8_t buf[200000];
    memset(buf, 'A', 100000);
    ASSERT_EQ_INT(2, round_trip_btype(buf, 100000, 1)); // one symbol: still a complete code
    ASSERT_EQ_INT(1, round_trip_btype((const uint8_t *)"Z", 1, 1));
    g_rng = 7;
    for (size_t i = 0; i < sizeof(buf); i++)
        buf[i] = (uint8_t)rnd();
    ASSERT_EQ_INT(0, round_trip_btype(buf, sizeof(buf), 1));
    // Text over several blocks of tokens.
    for (size_t i = 0; i < sizeof(buf); i++)
        buf[i] = (uint8_t) "System Folder Finder "[i % 21] + (uint8_t)(i / 50000);
    ASSERT_EQ_INT(2, round_trip_btype(buf, sizeof(buf), 6));
}

// ---- writer -> reader --------------------------------------------------------

// Write `len` bytes of `content` to `path` in appends of `step` bytes
// (0: random sizes), with chunks of `chunk_sectors`.
static void write_image(const char *path, const uint8_t *content, size_t len, size_t step, uint32_t chunk_sectors,
                        int level, udif_writer_stats_t *st) {
    unlink(path);
    char err[200] = {0};
    udif_writer_opts_t o = {.chunk_sectors = chunk_sectors, .level = level, .source_name = "Disk <1> & co.img"};
    udif_writer_t *w = udif_writer_open(path, &o, err, sizeof(err));
    ASSERT_TRUE(w != NULL);
    g_rng = 12345;
    for (size_t at = 0; at < len;) {
        size_t n = step ? step : 1 + rnd() % 300000;
        if (n > len - at)
            n = len - at;
        ASSERT_EQ_INT(0, udif_writer_append(w, content + at, n));
        at += n;
    }
    ASSERT_EQ_INT(0, udif_writer_finish(w, st));
}

// Every byte of the image read in place equals the content, zero-padded to
// a sector; the verifier agrees; the table CRC is the padded content's.
static void check_image(const char *path, const uint8_t *content, size_t len, const udif_writer_stats_t *st) {
    size_t padded = (len + 511) / 512 * 512;
    gs_source_t *host = gs_source_host(path, NULL);
    ASSERT_TRUE(host != NULL);
    int err = 0;
    gs_source_t *s = udif_source_open(host, &err);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_INT((int)padded, (int)gs_source_size(s));
    uint8_t *got = malloc(padded);
    // Read in odd-sized pieces so reads straddle chunks.
    for (size_t at = 0; at < padded;) {
        size_t n = padded - at < 70001 ? padded - at : 70001;
        ASSERT_EQ_INT(0, gs_source_read_exact(s, at, got + at, n));
        at += n;
    }
    ASSERT_TRUE(memcmp(got, content, len) == 0);
    for (size_t i = len; i < padded; i++)
        ASSERT_TRUE(got[i] == 0);
    ASSERT_EQ_INT((int)gs_crc32(0, got, padded), (int)st->crc);
    free(got);
    udif_writer_stats_t vs;
    char msg[200] = {0};
    ASSERT_EQ_INT(0, udif_verify(host, &vs, msg, sizeof(msg)));
    ASSERT_EQ_INT((int)st->crc, (int)vs.crc);
    ASSERT_EQ_INT((int)st->stored_bytes, (int)file_size(path));
    gs_source_release(s);
    gs_source_release(host);
}

TEST(writer_round_trips_sizes_and_chunks) {
    sandbox();
    const uint32_t chunks[] = {8, 128, 2048};
    for (size_t c = 0; c < 3; c++) {
        size_t cb = (size_t)chunks[c] * 512;
        const size_t sizes[] = {1, 512, cb - 512, cb, cb + 512, 3 * cb + 1000, 700000};
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            uint8_t *in = make_content(sizes[i], 100 + i + c);
            udif_writer_stats_t st;
            const char *p = sb_path("rt.dmg");
            write_image(p, in, sizes[i], 65536, chunks[c], 1, &st);
            ASSERT_EQ_INT((int)sizes[i], (int)st.bytes_in);
            check_image(p, in, sizes[i], &st);
            free(in);
        }
    }
}

TEST(writer_level_zero_stores_raw_and_zero_only) {
    sandbox();
    size_t len = 1u << 20;
    uint8_t *in = make_content(len, 55);
    udif_writer_stats_t st;
    const char *p = sb_path("l0.dmg");
    write_image(p, in, len, 0, 8, 0, &st);
    check_image(p, in, len, &st);
    ASSERT_TRUE(st.zero_bytes > 0);
    free(in);
}

// The file does not depend on how the bytes were split across appends (the
// segment ID, random by design, aside).
TEST(writer_append_granularity_is_invisible) {
    sandbox();
    size_t len = 1500000;
    uint8_t *in = make_content(len, 77);
    const size_t steps[] = {1, 511, 2u << 20, 0};
    uint8_t *first = NULL;
    long first_len = 0;
    for (size_t i = 0; i < 4; i++) {
        udif_writer_stats_t st;
        char name[32];
        snprintf(name, sizeof(name), "g%zu.dmg", i);
        const char *p = sb_path(name);
        write_image(p, in, len, steps[i], 128, 1, &st);
        long n = file_size(p);
        uint8_t *b = malloc((size_t)n);
        FILE *f = fopen(p, "rb");
        ASSERT_TRUE(fread(b, 1, (size_t)n, f) == (size_t)n);
        fclose(f);
        memset(b + n - 512 + 0x40, 0, 16); // the segment ID
        if (!first) {
            first = b;
            first_len = n;
        } else {
            ASSERT_EQ_INT((int)first_len, (int)n);
            ASSERT_TRUE(memcmp(first, b, (size_t)n) == 0);
            free(b);
        }
    }
    free(first);
    free(in);
}

TEST(empty_2gib_is_a_few_kb) {
    sandbox();
    const char *p = sb_path("empty.dmg");
    unlink(p);
    uint64_t size = 2ull << 30;
    ASSERT_EQ_INT(0, udif_create_empty(p, size));
    ASSERT_TRUE(file_size(p) < 4096);
    ASSERT_EQ_INT(-EEXIST, udif_create_empty(p, size));
    gs_source_t *host = gs_source_host(p, NULL);
    gs_source_t *s = udif_source_open(host, NULL);
    ASSERT_TRUE(s != NULL);
    ASSERT_TRUE(gs_source_size(s) == size);
    uint8_t blk[512];
    memset(blk, 0xAA, sizeof(blk));
    ASSERT_EQ_INT(0, gs_source_read_exact(s, size - 1024, blk, sizeof(blk)));
    for (size_t i = 0; i < sizeof(blk); i++)
        ASSERT_TRUE(blk[i] == 0);
    udif_writer_stats_t vs;
    ASSERT_EQ_INT(0, udif_verify(host, &vs, NULL, 0));
    ASSERT_EQ_INT(1, (int)vs.extents);
    ASSERT_EQ_INT((int)gs_crc32_zeros(0, size), (int)vs.crc);
    gs_source_release(s);
    gs_source_release(host);
}

// A flipped byte in a compressed chunk fails the read of that chunk (and the
// verifier), not the reads of its neighbours.
TEST(corrupt_chunk_fails_only_its_range) {
    sandbox();
    size_t len = 128 * 512 * 6;
    uint8_t *in = malloc(len);
    for (size_t i = 0; i < len; i++)
        in[i] = (uint8_t) "Finder"[i % 6] + (uint8_t)(i / 65536);
    udif_writer_stats_t st;
    const char *p = sb_path("bad.dmg");
    write_image(p, in, len, 65536, 128, 1, &st);
    FILE *f = fopen(p, "r+b");
    gs_source_t *probe = gs_source_host(p, NULL);
    udif_writer_stats_t ok;
    ASSERT_EQ_INT(0, udif_verify(probe, &ok, NULL, 0));
    gs_source_release(probe);
    // The six stored chunks are near enough the same size here: a byte at
    // 5/12 of the data fork is inside the third.
    uint8_t k[512];
    fseek(f, -512, SEEK_END);
    ASSERT_TRUE(fread(k, 1, 512, f) == 512);
    uint64_t dlen = 0;
    for (int i = 0; i < 8; i++)
        dlen = dlen << 8 | k[0x20 + i];
    long at = (long)(dlen * 5 / 12);
    fseek(f, at, SEEK_SET);
    int c = fgetc(f);
    fseek(f, at, SEEK_SET);
    fputc(c ^ 0x5A, f);
    fclose(f);
    // A copy under a new name, so no cache holds the good chunk.
    const char *q = sb_path("bad2.dmg");
    unlink(q);
    ASSERT_EQ_INT(0, rename(sb_path("bad.dmg"), q));
    gs_source_t *host = gs_source_host(q, NULL);
    gs_source_t *s = udif_source_open(host, NULL);
    ASSERT_TRUE(s != NULL);
    uint8_t blk[512];
    ASSERT_EQ_INT(0, gs_source_read_exact(s, 0, blk, sizeof(blk)));
    ASSERT_TRUE(memcmp(blk, in, sizeof(blk)) == 0);
    ASSERT_TRUE(gs_source_read_exact(s, 2 * 65536 + 512, blk, sizeof(blk)) != 0);
    ASSERT_EQ_INT(0, gs_source_read_exact(s, 5 * 65536, blk, sizeof(blk)));
    ASSERT_TRUE(memcmp(blk, in + 5 * 65536, sizeof(blk)) == 0);
    ASSERT_TRUE(udif_verify(host, NULL, NULL, 0) != 0);
    gs_source_release(s);
    gs_source_release(host);
    free(in);
}

// A foreign image whose chunks are larger than the in-place bound is
// refused in place; the same image marked as ours opens.
TEST(chunk_bound_applies_to_foreign_images) {
    sandbox();
    size_t len = 4 * 2048 * 512;
    uint8_t *in = make_content(len, 99);
    for (size_t i = 0; i < len; i++)
        if (!in[i])
            in[i] = 1; // no zero chunks: every chunk is stored
    udif_writer_stats_t st;
    const char *p = sb_path("big.dmg");
    write_image(p, in, len, 65536, 2048, 1, &st);
    size_t saved = udif_inplace_max_chunk();
    udif_set_inplace_max_chunk(512 * 1024);

    gs_source_t *host = gs_source_host(p, NULL);
    int err = 0;
    gs_source_t *s = udif_source_open(host, &err);
    ASSERT_TRUE(s != NULL); // ours: bounded by construction
    gs_source_release(s);
    gs_source_release(host);

    // Unmark it (the property list is not checksummed).
    long n = file_size(p);
    uint8_t *b = malloc((size_t)n);
    FILE *f = fopen(p, "rb");
    ASSERT_TRUE(fread(b, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    uint8_t *hit = NULL;
    for (long i = 0; i + 10 < n; i++)
        if (memcmp(b + i, "gs-profile", 10) == 0)
            hit = b + i;
    ASSERT_TRUE(hit != NULL);
    memcpy(hit, "xx-profile", 10);
    const char *q = sb_path("foreign.dmg");
    f = fopen(q, "wb");
    ASSERT_TRUE(fwrite(b, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    free(b);
    host = gs_source_host(q, NULL);
    s = udif_source_open(host, &err);
    ASSERT_TRUE(s == NULL);
    ASSERT_EQ_INT(-EFBIG, err);
    // A converter's explicit bound opens it.
    s = udif_source_open_bounded(host, 64u << 20, &err);
    ASSERT_TRUE(s != NULL);
    gs_source_release(s);
    gs_source_release(host);
    udif_set_inplace_max_chunk(saved);
    free(in);
}

int main(void) {
    RUN(deflate_round_trips_at_every_level);
    RUN(deflate_compresses_text_and_reuses_state);
    RUN(deflate_dynamic_codes_are_length_limited);
    RUN(deflate_picks_the_smallest_block_form);
    RUN(adler32_known_vector);
    RUN(crc32_zeros_matches_the_table);
    RUN(writer_round_trips_sizes_and_chunks);
    RUN(writer_level_zero_stores_raw_and_zero_only);
    RUN(writer_append_granularity_is_invisible);
    RUN(empty_2gib_is_a_few_kb);
    RUN(corrupt_chunk_fails_only_its_range);
    RUN(chunk_bound_applies_to_foreign_images);
    sandbox_remove();
    return 0;
}
