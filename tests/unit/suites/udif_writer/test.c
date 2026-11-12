// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for the compressing half of the UDIF support: the deflate
// encoder and the CRC of a zero run (the codec library, src/codec), the
// streaming UDIF writer and verifier (udif_writer.c), and reading what it
// writes in place through the chunk-mapped source (image_chunkmap.c) -- and,
// given images hdiutil converted from those, reading them back too.

#include "codec.h"
#include "image_chunkmap.h"
#include "image_udif.h"
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
    const char *names[] = {"rt.dmg",        "l0.dmg",  "g0.dmg",   "g1.dmg",  "g2.dmg",      "g3.dmg",
                           "empty.dmg",     "bad.dmg", "bad2.dmg", "big.dmg", "foreign.dmg", "origin.dmg",
                           "no-origin.dmg", "xz.dmg",  "x.dmg",    "x0.dmg",  "z.dmg"};
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
        size_t cap = codec_enc_bound(CODEC_ZLIB, sizes[s]);
        uint8_t *z = malloc(cap);
        uint8_t *out = malloc(sizes[s] + 1);
        for (size_t l = 0; l < sizeof(levels) / sizeof(levels[0]); l++) {
            long n = codec_encode(CODEC_ZLIB, levels[l], in, sizes[s], z, cap);
            ASSERT_TRUE(n > 0 && (size_t)n <= cap);
            int64_t got = codec_decode(CODEC_ZLIB, z, (size_t)n, out, sizes[s] + 1);
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
    codec_enc_t *st = codec_enc_new(CODEC_ZLIB, sizeof(buf), 1, NULL);
    uint8_t z[80000], out[65536];
    for (int round = 0; round < 3; round++) {
        int64_t n = codec_enc_run(st, buf, sizeof(buf), z, sizeof(z));
        ASSERT_TRUE(n > 0 && n < 4096);
        ASSERT_EQ_INT((int)sizeof(buf), (int)codec_decode(CODEC_ZLIB, z, (size_t)n, out, sizeof(out)));
        ASSERT_TRUE(memcmp(buf, out, sizeof(buf)) == 0);
    }
    codec_enc_free(st);
    // Too small an output buffer is an error, not an overrun.
    ASSERT_EQ_INT(CODEC_ERR_OVERFLOW, (int)codec_encode(CODEC_ZLIB, 0, buf, sizeof(buf), z, 8));
}

TEST(adler32_known_vector) {
    ASSERT_EQ_INT((int)0x11E60398u, (int)codec_adler32(1, (const uint8_t *)"Wikipedia", 9));
}

TEST(crc32_zeros_matches_the_table) {
    static uint8_t zeros[300000];
    const uint64_t lens[] = {0, 1, 2, 3, 511, 512, 65536, 299999};
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        ASSERT_EQ_INT((int)codec_crc32(0, zeros, (size_t)lens[i]), (int)codec_crc32_zeros(0, lens[i]));
        uint32_t seed = codec_crc32(0, "abc", 3);
        ASSERT_EQ_INT((int)codec_crc32(seed, zeros, (size_t)lens[i]), (int)codec_crc32_zeros(seed, lens[i]));
    }
}

// Round-trip `n` bytes at `level`; the first block's type (BTYPE).
static int round_trip_btype(const uint8_t *in, size_t n, int level) {
    size_t cap = codec_enc_bound(CODEC_ZLIB, n);
    uint8_t *z = malloc(cap), *out = malloc(n + 1);
    ASSERT_TRUE(z != NULL && out != NULL);
    long zn = codec_encode(CODEC_ZLIB, level, in, n, z, cap);
    ASSERT_TRUE(zn > 2);
    ASSERT_EQ_INT((int)n, (int)codec_decode(CODEC_ZLIB, z, (size_t)zn, out, n + 1));
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

// The codec write_image compresses with (a test that wants xz sets it, and
// sets it back).
static udif_codec_t s_codec = UDIF_CODEC_ZLIB;

// Write `len` bytes of `content` to `path` in appends of `step` bytes
// (0: random sizes), with chunks of `chunk_sectors`, in s_codec.
static void write_image(const char *path, const uint8_t *content, size_t len, size_t step, uint32_t chunk_sectors,
                        int level, udif_writer_stats_t *st) {
    unlink(path);
    char err[200] = {0};
    udif_writer_opts_t o = {
        .chunk_sectors = chunk_sectors, .codec = s_codec, .level = level, .source_name = "Disk <1> & co.img"};
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
    source_t *host = source_host(path, NULL);
    ASSERT_TRUE(host != NULL);
    int err = 0;
    source_t *s = udif_source_open(host, &err);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_INT((int)padded, (int)source_size(s));
    uint8_t *got = malloc(padded);
    // Read in odd-sized pieces so reads straddle chunks.
    for (size_t at = 0; at < padded;) {
        size_t n = padded - at < 70001 ? padded - at : 70001;
        ASSERT_EQ_INT(0, source_read_exact(s, at, got + at, n));
        at += n;
    }
    ASSERT_TRUE(memcmp(got, content, len) == 0);
    for (size_t i = len; i < padded; i++)
        ASSERT_TRUE(got[i] == 0);
    ASSERT_EQ_INT((int)codec_crc32(0, got, padded), (int)st->crc);
    free(got);
    udif_writer_stats_t vs;
    char msg[200] = {0};
    ASSERT_EQ_INT(0, udif_verify(host, &vs, msg, sizeof(msg)));
    ASSERT_EQ_INT((int)st->crc, (int)vs.crc);
    ASSERT_EQ_INT((int)st->stored_bytes, (int)file_size(path));
    source_release(s);
    source_release(host);
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
    source_t *host = source_host(p, NULL);
    source_t *s = udif_source_open(host, NULL);
    ASSERT_TRUE(s != NULL);
    ASSERT_TRUE(source_size(s) == size);
    uint8_t blk[512];
    memset(blk, 0xAA, sizeof(blk));
    ASSERT_EQ_INT(0, source_read_exact(s, size - 1024, blk, sizeof(blk)));
    for (size_t i = 0; i < sizeof(blk); i++)
        ASSERT_TRUE(blk[i] == 0);
    udif_writer_stats_t vs;
    ASSERT_EQ_INT(0, udif_verify(host, &vs, NULL, 0));
    ASSERT_EQ_INT(1, (int)vs.extents);
    ASSERT_EQ_INT((int)codec_crc32_zeros(0, size), (int)vs.crc);
    source_release(s);
    source_release(host);
}

// The caller's origin goes into the property list as is (gs-origin) and
// udif_info reads it back verbatim -- XML-special characters and a query
// string included; an image written without one reports "".
TEST(origin_round_trips_through_udif_info) {
    sandbox();
    const char *url = "https://ia800908.us.archive.org/view_archive.php?archive=/12/items/R/R.zip"
                      "&file=9FEB69B3%20-%20Power%20Mac%206100%20%26%207100.ROM#<x>";
    uint8_t content[4096];
    for (size_t i = 0; i < sizeof(content); i++)
        content[i] = (uint8_t)(i * 7);
    for (int with = 0; with <= 1; with++) {
        const char *p = sb_path(with ? "origin.dmg" : "no-origin.dmg");
        unlink(p);
        char err[200] = {0};
        udif_writer_opts_t o = {.level = 1, .source_name = "disk.img", .origin = with ? url : NULL};
        udif_writer_t *w = udif_writer_open(p, &o, err, sizeof(err));
        ASSERT_TRUE(w != NULL);
        ASSERT_EQ_INT(0, udif_writer_append(w, content, sizeof(content)));
        ASSERT_EQ_INT(0, udif_writer_finish(w, NULL));
        source_t *host = source_host(p, NULL);
        ASSERT_TRUE(host != NULL);
        udif_info_t in;
        ASSERT_EQ_INT(0, udif_info(host, &in));
        ASSERT_TRUE(in.is_gs_profile);
        ASSERT_TRUE(strcmp(in.source_name, "disk.img") == 0);
        ASSERT_TRUE(strcmp(in.origin, with ? url : "") == 0);
        source_release(host);
    }
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
    source_t *probe = source_host(p, NULL);
    udif_writer_stats_t ok;
    ASSERT_EQ_INT(0, udif_verify(probe, &ok, NULL, 0));
    source_release(probe);
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
    source_t *host = source_host(q, NULL);
    source_t *s = udif_source_open(host, NULL);
    ASSERT_TRUE(s != NULL);
    uint8_t blk[512];
    ASSERT_EQ_INT(0, source_read_exact(s, 0, blk, sizeof(blk)));
    ASSERT_TRUE(memcmp(blk, in, sizeof(blk)) == 0);
    ASSERT_TRUE(source_read_exact(s, 2 * 65536 + 512, blk, sizeof(blk)) != 0);
    ASSERT_EQ_INT(0, source_read_exact(s, 5 * 65536, blk, sizeof(blk)));
    ASSERT_TRUE(memcmp(blk, in + 5 * 65536, sizeof(blk)) == 0);
    ASSERT_TRUE(udif_verify(host, NULL, NULL, 0) != 0);
    source_release(s);
    source_release(host);
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

    source_t *host = source_host(p, NULL);
    int err = 0;
    source_t *s = udif_source_open(host, &err);
    ASSERT_TRUE(s != NULL); // ours: bounded by construction
    source_release(s);
    source_release(host);

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
    host = source_host(q, NULL);
    s = udif_source_open(host, &err);
    ASSERT_TRUE(s == NULL);
    ASSERT_EQ_INT(-EFBIG, err);
    // A converter's explicit bound opens it.
    s = udif_source_open_bounded(host, 64u << 20, &err);
    ASSERT_TRUE(s != NULL);
    source_release(s);
    source_release(host);
    udif_set_inplace_max_chunk(saved);
    free(in);
}

// ---- interop samples ------------------------------------------------------

// With GS_UDIF_INTEROP_DIR set, write a set of images there for other tools
// to open -- the macOS CI job runs hdiutil on them (.github/workflows/
// udif-interop.yml) -- each <name>.dmg beside <name>.img, the disk it must
// decode to (zero-padded to a sector), and a MANIFEST of the names.  Every
// sample also passes check_image here.  Without the variable it does nothing.
static void emit_sample(const char *dir, FILE *manifest, const char *name, const uint8_t *in, size_t len,
                        uint32_t chunk_sectors, int level, udif_codec_t codec) {
    char dmg[512], img[512];
    snprintf(dmg, sizeof(dmg), "%s/%s.dmg", dir, name);
    snprintf(img, sizeof(img), "%s/%s.img", dir, name);
    udif_writer_stats_t st;
    s_codec = codec;
    write_image(dmg, in, len, 1u << 20, chunk_sectors, level, &st);
    s_codec = UDIF_CODEC_ZLIB;
    check_image(dmg, in, len, &st);
    size_t padded = (len + 511) / 512 * 512;
    FILE *f = fopen(img, "wb");
    ASSERT_TRUE(f != NULL);
    ASSERT_TRUE(fwrite(in, 1, len, f) == len);
    for (size_t i = len; i < padded; i++)
        ASSERT_TRUE(fputc(0, f) == 0);
    int rc = fclose(f);
    ASSERT_EQ_INT(0, rc);
    fprintf(manifest, "%s\n", name);
}

// Text-like content only: long runs of literals and matches, so every
// chunk is dynamic-Huffman coded.
static uint8_t *make_text(size_t len, uint64_t seed) {
    static const char *words[] = {"System", "Folder",   "Finder", "the",  "of",   "Macintosh", "disk",
                                  "file",   "resource", "fork",   "INIT", "cdev", "{",         "}\r"};
    g_rng = seed | 1;
    uint8_t *b = malloc(len ? len : 1);
    ASSERT_TRUE(b != NULL);
    for (size_t at = 0; at < len;) {
        const char *w = words[rnd() % (sizeof(words) / sizeof(words[0]))];
        for (size_t i = 0; w[i] && at < len; i++)
            b[at++] = (uint8_t)w[i];
        if (at < len)
            b[at++] = ' ';
    }
    return b;
}

TEST(writer_emits_interop_samples) {
    const char *dir = getenv("GS_UDIF_INTEROP_DIR");
    if (!dir || !*dir)
        return;
    mkdir(dir, 0777);
    char path[512];
    snprintf(path, sizeof(path), "%s/MANIFEST", dir);
    FILE *m = fopen(path, "w");
    ASSERT_TRUE(m != NULL);

    size_t mixed_len = 8u << 20;
    uint8_t *mixed = make_content(mixed_len, 2024);
    emit_sample(dir, m, "mixed-l1", mixed, mixed_len, 128, 1, UDIF_CODEC_ZLIB);
    emit_sample(dir, m, "mixed-l9", mixed, mixed_len, 128, 9, UDIF_CODEC_ZLIB);
    emit_sample(dir, m, "mixed-l0", mixed, mixed_len, 128, 0, UDIF_CODEC_ZLIB);
    emit_sample(dir, m, "mixed-4k", mixed, mixed_len, 8, 1, UDIF_CODEC_ZLIB);
    emit_sample(dir, m, "mixed-1m", mixed, mixed_len, 2048, 6, UDIF_CODEC_ZLIB);
    emit_sample(dir, m, "odd-length", mixed, 1000123, 128, 1, UDIF_CODEC_ZLIB);
    emit_sample(dir, m, "mixed-xz", mixed, mixed_len, 128, -1, UDIF_CODEC_XZ);
    emit_sample(dir, m, "mixed-1m-xz", mixed, mixed_len, 2048, 9, UDIF_CODEC_XZ);
    free(mixed);

    size_t text_len = 4u << 20;
    uint8_t *text = make_text(text_len, 7);
    emit_sample(dir, m, "text-l6", text, text_len, 128, 6, UDIF_CODEC_ZLIB);
    // xz (ULMO) samples: hdiutil opens them (macOS 10.15 and later); 7-Zip
    // and dmg2img do not, and check-udif-interop.sh skips them there.
    emit_sample(dir, m, "text-xz", text, text_len, 128, -1, UDIF_CODEC_XZ);
    free(text);

    // A blank disk, as files.hd_create makes one, and the zeros it is.
    snprintf(path, sizeof(path), "%s/blank.dmg", dir);
    unlink(path);
    ASSERT_EQ_INT(0, udif_create_empty(path, 64u << 20));
    snprintf(path, sizeof(path), "%s/blank.img", dir);
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    ASSERT_EQ_INT(0, ftruncate(fileno(f), 64 << 20));
    int rc = fclose(f);
    ASSERT_EQ_INT(0, rc);
    fprintf(m, "blank\n");
    rc = fclose(m);
    ASSERT_EQ_INT(0, rc);
}

// ---- foreign images ------------------------------------------------------

// The chunk type an hdiutil format name (the suffix after the last '-' in
// the image's name) is made of.
static uint32_t format_chunk_type(const char *name) {
    static const struct {
        const char *fmt;
        uint32_t type;
    } k[] = {
        {"UDCO", UDIF_CHUNK_ADC  },
        {"UDZO", UDIF_CHUNK_ZLIB },
        {"UDBZ", UDIF_CHUNK_BZ2  },
        {"ULFO", UDIF_CHUNK_LZFSE},
        {"ULMO", UDIF_CHUNK_LZMA }
    };
    const char *dash = strrchr(name, '-');
    for (size_t i = 0; dash && i < sizeof(k) / sizeof(k[0]); i++)
        if (strncmp(dash + 1, k[i].fmt, 4) == 0)
            return k[i].type;
    return 0;
}

// Whether the image at `host` has a chunk of `type` in its block map.
static bool has_chunk_type(source_t *host, uint32_t type) {
    uint64_t size = source_size(host);
    uint8_t t[UDIF_TRAILER_SIZE];
    ASSERT_EQ_INT(0, source_read_exact(host, size - UDIF_TRAILER_SIZE, t, sizeof(t)));
    udif_trailer_t tr;
    ASSERT_EQ_INT(0, udif_parse_trailer(t, sizeof(t), &tr));
    uint8_t *xml = malloc((size_t)tr.xml_length);
    ASSERT_EQ_INT(0, source_read_exact(host, tr.xml_offset, xml, (size_t)tr.xml_length));
    udif_map_t *map = NULL;
    ASSERT_EQ_INT(0, udif_parse_blkx(xml, (size_t)tr.xml_length, &map));
    bool found = false;
    for (size_t i = 0; i < map->n_tables; i++)
        for (size_t c = 0; c < map->tables[i].n_chunks; c++)
            found |= map->tables[i].chunks[c].type == type;
    udif_map_free(map);
    free(xml);
    return found;
}

// The xz codec writes LZMA chunks (hdiutil's ULMO) that read back in place
// and verify, at the default and the extreme levels and every chunk size;
// it is smaller than zlib on the same content, and level 0 still writes
// zero runs and raw chunks only.
TEST(writer_xz_round_trips) {
    sandbox();
    size_t len = 3u << 20;
    uint8_t *in = make_content(len, 77);
    s_codec = UDIF_CODEC_XZ;
    const uint32_t chunks[] = {8, 128, 2048};
    const int levels[] = {-1, 1, 9};
    for (size_t c = 0; c < 3; c++) {
        udif_writer_stats_t st;
        const char *p = sb_path("xz.dmg");
        write_image(p, in, len, 65536, chunks[c], levels[c], &st);
        check_image(p, in, len, &st);
        source_t *host = source_host(p, NULL);
        ASSERT_TRUE(has_chunk_type(host, UDIF_CHUNK_LZMA));
        ASSERT_TRUE(!has_chunk_type(host, UDIF_CHUNK_ZLIB));
        source_release(host);
    }
    // Smaller than zlib on disk-like content.  (On content that is mostly
    // incompressible, as `in` is, the two are within a fraction of a
    // percent: deflate stores such bytes, LZMA codes them at a hair over
    // eight bits.)
    size_t text_len = 2u << 20;
    uint8_t *text = make_text(text_len, 5);
    udif_writer_stats_t sx, sz;
    write_image(sb_path("x.dmg"), text, text_len, 65536, 128, -1, &sx);
    check_image(sb_path("x.dmg"), text, text_len, &sx);
    s_codec = UDIF_CODEC_ZLIB;
    write_image(sb_path("z.dmg"), text, text_len, 65536, 128, -1, &sz);
    ASSERT_TRUE(sx.stored_bytes < sz.stored_bytes - sz.stored_bytes / 10);
    free(text);
    // Level 0: no compression, whatever the codec.
    s_codec = UDIF_CODEC_XZ;
    udif_writer_stats_t s0;
    write_image(sb_path("x0.dmg"), in, len, 65536, 128, 0, &s0);
    source_t *host = source_host(sb_path("x0.dmg"), NULL);
    ASSERT_TRUE(!has_chunk_type(host, UDIF_CHUNK_LZMA));
    source_release(host);
    s_codec = UDIF_CODEC_ZLIB;
    // The codec names the object model takes.
    udif_codec_t k;
    ASSERT_TRUE(udif_codec_parse("xz", &k) == 0 && k == UDIF_CODEC_XZ);
    ASSERT_TRUE(udif_codec_parse(NULL, &k) == 0 && k == UDIF_CODEC_ZLIB);
    ASSERT_TRUE(udif_codec_parse("bzip2", &k) == -1);
    free(in);
}

// With GS_UDIF_FOREIGN_DIR set, read back the images hdiutil made from the
// interop samples (scripts/make-udif-foreign.sh, the macOS job of
// .github/workflows/udif-interop.yml): its MANIFEST names each image and
// the disk it must decode to.  Every image must carry a chunk of its
// format's codec, verify (every chunk decoded, every checksum hdiutil wrote
// matched) and read back, in place, as the disk.  Without the variable it
// does nothing.
TEST(foreign_images_decode) {
    const char *dir = getenv("GS_UDIF_FOREIGN_DIR");
    if (!dir || !*dir)
        return;
    char path[1024], dmg[256], img[256];
    snprintf(path, sizeof(path), "%s/MANIFEST", dir);
    FILE *m = fopen(path, "r");
    ASSERT_TRUE(m != NULL);
    int n_images = 0;
    while (fscanf(m, "%255s %255s", dmg, img) == 2) {
        fprintf(stderr, "       %s\n", dmg);
        snprintf(path, sizeof(path), "%s/%s", dir, img);
        FILE *f = fopen(path, "rb");
        ASSERT_TRUE(f != NULL);
        ASSERT_EQ_INT(0, fseek(f, 0, SEEK_END));
        size_t len = (size_t)ftell(f);
        rewind(f);
        uint8_t *want = malloc(len ? len : 1);
        ASSERT_TRUE(fread(want, 1, len, f) == len);
        fclose(f);

        snprintf(path, sizeof(path), "%s/%s", dir, dmg);
        source_t *host = source_host(path, NULL);
        ASSERT_TRUE(host != NULL);
        // A blank disk is all zero runs, whatever its format.
        uint32_t type = format_chunk_type(dmg);
        bool blank = true;
        for (size_t i = 0; i < len && blank; i++)
            blank = want[i] == 0;
        ASSERT_TRUE(type != 0);
        ASSERT_TRUE(blank || has_chunk_type(host, type));

        udif_writer_stats_t vs;
        char msg[200] = {0};
        int vrc = udif_verify(host, &vs, msg, sizeof(msg));
        if (vrc)
            fprintf(stderr, "       udif_verify: %s\n", msg);
        ASSERT_EQ_INT(0, vrc);
        ASSERT_TRUE(vs.bytes_in == len);
        // hdiutil's chunks may be larger than the in-place bound: open
        // with the converter's.
        int err = 0;
        source_t *s = udif_source_open_bounded(host, 64u << 20, &err);
        ASSERT_TRUE(s != NULL);
        ASSERT_TRUE(source_size(s) == len);
        uint8_t *got = malloc(len ? len : 1);
        for (size_t at = 0; at < len;) {
            size_t n = len - at < 70001 ? len - at : 70001;
            ASSERT_EQ_INT(0, source_read_exact(s, at, got + at, n));
            at += n;
        }
        ASSERT_TRUE(memcmp(got, want, len) == 0);
        free(got);
        free(want);
        source_release(s);
        source_release(host);
        n_images++;
    }
    fclose(m);
    ASSERT_TRUE(n_images > 0);
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
    RUN(origin_round_trips_through_udif_info);
    RUN(corrupt_chunk_fails_only_its_range);
    RUN(chunk_bound_applies_to_foreign_images);
    RUN(writer_emits_interop_samples);
    RUN(writer_xz_round_trips);
    RUN(foreign_images_decode);
    sandbox_remove();
    return 0;
}
