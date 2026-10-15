// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for the QCOW2 reader: the header parser and cluster decoder
// (src/core/storage/image_qcow2.c) and the chunk-mapped source over it
// (qcow2_source_open in image_chunkmap.c).  Images are built in memory here,
// laid out as QEMU's docs/interop/qcow2.rst describes, so the suite needs no
// fixtures: raw clusters (adjacent and not), unallocated and zero-flagged
// clusters, deflated clusters packed back to back at unaligned offsets, a
// final cluster cut by the virtual size, a file that ends inside a cluster,
// extended L2 entries, and the features the reader refuses.

#include "deflate.h"
#include "format_registry.h"
#include "image_chunkmap.h"
#include "image_qcow2.h"
#include "test_assert.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void w_u32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (24 - 8 * i));
}
static void w_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (56 - 8 * i));
}

// A version 3 header: one L1 entry's worth of disk unless `l1_size` says more.
static void put_header(uint8_t *f, uint32_t cluster_bits, uint64_t size, uint64_t l1_off, uint32_t l1_size,
                       uint64_t incompat) {
    w_u32(f, QCOW2_MAGIC);
    w_u32(f + 4, 3);
    w_u32(f + 20, cluster_bits);
    w_u64(f + 24, size);
    w_u32(f + 36, l1_size);
    w_u64(f + 40, l1_off);
    w_u64(f + 72, incompat);
    w_u32(f + 96, 4); // refcount_order
    w_u32(f + 100, 112); // header_length, with the compression type byte
}

// `len` bytes of `in` as a raw DEFLATE stream (a zlib stream less its
// 2-byte header and 4-byte Adler-32).  Returns the stream's length.
static size_t raw_deflate(const uint8_t *in, size_t len, uint8_t *out, size_t cap) {
    uint8_t *z = malloc(deflate_bound(len));
    long n = deflate_zlib(NULL, in, len, z, deflate_bound(len), 6);
    ASSERT_TRUE(n > 6 && (size_t)n - 6 <= cap);
    memcpy(out, z + 2, (size_t)n - 6);
    free(z);
    return (size_t)n - 6;
}

// A compressed cluster's L2 entry: the stream at `off`, `len` bytes long.
static uint64_t compressed_entry(uint32_t cluster_bits, uint64_t off, size_t len) {
    uint32_t x = 62 - (cluster_bits - 8);
    uint64_t extra = ((off % 512) + len + 511) / 512 - 1;
    return (1ull << 62) | (extra << x) | off;
}

static uint8_t pattern(uint64_t guest_off) {
    return (uint8_t)((guest_off * 7 + guest_off / 251) & 0xFF);
}

// ---- The standard test image -------------------------------------------------
//
// 4 KB clusters; the virtual disk is 7 clusters less 2 KB.  Host clusters:
// 0 header, 1 L1, 2 L2, 3-4 guest 0-1 (raw, adjacent), 5 a preallocation the
// zero flag hides, 6 the deflated guest 4 and 6 back to back from offset 100,
// 7 guest 5 (raw), of which the file holds only the first 1 KB.
//
// guest 0: 0xA0   1: 0xA1   2: unallocated   3: zero flag   4: pattern
//       5: 0xA5 for 1 KB, then zeros (past the file's end)   6: pattern, cut

#define CB      12u
#define CL      (1u << CB)
#define VSIZE   (7u * CL - 2048u)
#define FILE_SZ (7u * CL + 1024u)

static uint8_t *build_standard(size_t *out_len) {
    uint8_t *f = calloc(1, 8 * CL);
    put_header(f, CB, VSIZE, 1 * CL, 1, 0);
    w_u64(f + 1 * CL, (2ull * CL) | (1ull << 63)); // L1[0] -> L2 at cluster 2
    uint8_t *l2 = f + 2 * CL;
    w_u64(l2 + 0, (3ull * CL) | (1ull << 63));
    w_u64(l2 + 8, (4ull * CL) | (1ull << 63));
    w_u64(l2 + 16, 0); // unallocated
    w_u64(l2 + 24, (5ull * CL) | 1); // zero flag over a preallocated cluster
    memset(f + 3 * CL, 0xA0, CL);
    memset(f + 4 * CL, 0xA1, CL);
    memset(f + 5 * CL, 0xEE, CL); // must never be seen
    uint8_t clus[CL];
    for (uint32_t i = 0; i < CL; i++)
        clus[i] = pattern(4ull * CL + i);
    uint64_t at = 6ull * CL + 100;
    size_t n4 = raw_deflate(clus, CL, f + at, CL);
    w_u64(l2 + 32, compressed_entry(CB, at, n4));
    at += n4; // the next stream starts in the same sector
    for (uint32_t i = 0; i < CL; i++)
        clus[i] = pattern(6ull * CL + i);
    size_t n6 = raw_deflate(clus, CL, f + at, CL);
    ASSERT_TRUE(at + n6 <= 7ull * CL);
    w_u64(l2 + 48, compressed_entry(CB, at, n6));
    w_u64(l2 + 40, (7ull * CL) | (1ull << 63));
    memset(f + 7 * CL, 0xA5, CL);
    *out_len = FILE_SZ;
    return f;
}

static uint8_t expected(uint64_t off) {
    uint64_t c = off / CL, in = off % CL;
    switch (c) {
    case 0:
        return 0xA0;
    case 1:
        return 0xA1;
    case 5:
        return in < 1024 ? 0xA5 : 0;
    case 4:
    case 6:
        return pattern(off);
    default:
        return 0;
    }
}

TEST(qcow2_detect_and_parse) {
    size_t len;
    uint8_t *f = build_standard(&len);
    ASSERT_TRUE(qcow2_detect(f, len));
    qcow2_header_t h;
    ASSERT_EQ_INT(0, qcow2_parse_header(f, QCOW2_HEADER_READ, &h, NULL));
    ASSERT_EQ_INT(3, (int)h.version);
    ASSERT_EQ_INT((int)CB, (int)h.cluster_bits);
    ASSERT_EQ_INT((int)VSIZE, (int)h.size);
    ASSERT_EQ_INT(CL, (int)h.l1_offset);
    ASSERT_EQ_INT(512, (int)qcow2_l2_entries(&h));
    ASSERT_EQ_INT(1, (int)qcow2_l1_needed(&h));
    // A raw disk and a different version are no QCOW2.
    uint8_t raw[512] = {0x45, 0x52};
    ASSERT_TRUE(!qcow2_detect(raw, sizeof raw));
    w_u32(f + 4, 4);
    ASSERT_TRUE(!qcow2_detect(f, len));
    free(f);
}

TEST(qcow2_compressed_extent_fields) {
    // 64 KB clusters: x = 54.  Offset 0x12345, two extra sectors.
    uint64_t e = (1ull << 62) | (2ull << 54) | 0x12345;
    uint64_t off, len;
    qcow2_compressed_extent(e, 16, &off, &len);
    ASSERT_EQ_INT(0x12345, (int)off);
    ASSERT_EQ_INT(3 * 512 - 0x145, (int)len);
}

TEST(qcow2_source_round_trip) {
    size_t len;
    uint8_t *f = build_standard(&len);
    source_t *d = source_memory(f, len, true, "qcow2-test");
    int err = 0;
    source_t *img = qcow2_source_open(d, &err);
    ASSERT_TRUE(img != NULL);
    ASSERT_EQ_INT(0, err);
    ASSERT_EQ_INT((int)VSIZE, (int)source_size(img));
    uint8_t *buf = malloc(VSIZE);
    for (int pass = 0; pass < 2; pass++) { // the second from the chunk cache
        ASSERT_EQ_INT(0, source_read_exact(img, 0, buf, VSIZE));
        for (uint64_t i = 0; i < VSIZE; i++)
            if (buf[i] != expected(i)) {
                fprintf(stderr, "byte %llu: %#x, expected %#x\n", (unsigned long long)i, buf[i], expected(i));
                ASSERT_TRUE(0);
            }
    }
    // Reads that straddle runs, and start inside a compressed cluster.
    for (uint64_t off = CL - 7; off < VSIZE; off += 3001) {
        uint8_t small[600];
        size_t n = VSIZE - off < sizeof small ? (size_t)(VSIZE - off) : sizeof small;
        ASSERT_EQ_INT(0, source_read_exact(img, off, small, n));
        for (size_t i = 0; i < n; i++)
            ASSERT_EQ_INT(expected(off + i), small[i]);
    }
    free(buf);
    source_release(img);
    source_release(d);
}

// The format registry peels it like any other wrapper.
TEST(qcow2_unwraps_through_the_registry) {
    size_t len;
    uint8_t *f = build_standard(&len);
    source_t *d = source_memory(f, len, true, "qcow2-reg");
    format_unwrapped_t u;
    ASSERT_EQ_INT(0, format_unwrap(d, NULL, &u));
    ASSERT_EQ_INT(0, strcmp(u.chain, "qcow2"));
    ASSERT_TRUE(u.failed_format == NULL);
    ASSERT_EQ_INT((int)VSIZE, (int)source_size(u.data));
    uint8_t b[4];
    ASSERT_EQ_INT(0, source_read_exact(u.data, 0, b, sizeof b));
    ASSERT_EQ_INT(0xA0, b[0]);
    format_unwrapped_free(&u);
    source_release(d);
}

// Extended L2 entries: 16 KB clusters of 32 subclusters of 512 bytes.
TEST(qcow2_extended_l2_subclusters) {
    const uint32_t cb = 14, cl = 1u << cb, sub = cl / 32;
    uint8_t *f = calloc(1, 4 * cl);
    put_header(f, cb, 2ull * cl, cl, 1, QCOW2_INCOMPAT_EXTL2);
    w_u64(f + cl, 2ull * cl);
    uint8_t *l2 = f + 2 * cl;
    // Guest cluster 0: subclusters 0 and 1 allocated, 2 reads as zeros, the
    // rest unallocated.  Guest cluster 1: unallocated.
    w_u64(l2, 3ull * cl);
    w_u64(l2 + 8, (1ull << (32 + 2)) | 0x3);
    memset(f + 3 * cl, 0x5A, cl);
    source_t *d = source_memory(f, 4 * cl, true, "qcow2-extl2");
    int err = 0;
    source_t *img = qcow2_source_open(d, &err);
    ASSERT_TRUE(img != NULL);
    uint8_t *buf = malloc(2 * cl);
    ASSERT_EQ_INT(0, source_read_exact(img, 0, buf, 2 * cl));
    for (uint32_t i = 0; i < 2 * cl; i++)
        ASSERT_EQ_INT(i < 2 * sub ? 0x5A : 0, buf[i]);
    free(buf);
    source_release(img);
    source_release(d);
}

// A version 2 image: 72-byte header, no feature fields.
TEST(qcow2_version_2) {
    uint8_t *f = calloc(1, 4 * CL);
    w_u32(f, QCOW2_MAGIC);
    w_u32(f + 4, 2);
    w_u32(f + 20, CL == 4096 ? 12 : 0);
    w_u64(f + 24, CL);
    w_u32(f + 36, 1);
    w_u64(f + 40, CL);
    w_u64(f + CL, 2ull * CL);
    w_u64(f + 2 * CL, 3ull * CL);
    memset(f + 3 * CL, 0x77, CL);
    source_t *d = source_memory(f, 4 * CL, true, "qcow2-v2");
    int err = 0;
    source_t *img = qcow2_source_open(d, &err);
    ASSERT_TRUE(img != NULL);
    uint8_t b[CL];
    ASSERT_EQ_INT(0, source_read_exact(img, 0, b, CL));
    ASSERT_EQ_INT(0x77, b[0]);
    ASSERT_EQ_INT(0x77, b[CL - 1]);
    source_release(img);
    source_release(d);
}

// Open a copy of the standard image after `patch` changes it.
static int open_patched(void (*patch)(uint8_t *f)) {
    size_t len;
    uint8_t *f = build_standard(&len);
    patch(f);
    source_t *d = source_memory(f, len, true, "qcow2-bad");
    int err = 0;
    source_t *img = qcow2_source_open(d, &err);
    source_release(img);
    source_release(d);
    return img ? 0 : err;
}

static void with_backing(uint8_t *f) {
    w_u64(f + 8, 512);
    w_u32(f + 16, 4);
}
static void with_encryption(uint8_t *f) {
    w_u32(f + 32, 2);
}
static void with_unknown_feature(uint8_t *f) {
    w_u64(f + 72, 1ull << 7);
}
static void with_data_file(uint8_t *f) {
    w_u64(f + 72, QCOW2_INCOMPAT_DATA_FILE);
}
static void with_zstd(uint8_t *f) {
    w_u64(f + 72, QCOW2_INCOMPAT_COMPRESSION);
    f[104] = QCOW2_COMPRESSION_ZSTD;
}
static void with_l2_past_eof(uint8_t *f) {
    w_u64(f + CL, 64ull * CL);
}
static void with_tiny_l1(uint8_t *f) {
    w_u64(f + 24, 4096ull * CL); // two L1 entries' worth, one declared
}
static void with_bad_cluster_bits(uint8_t *f) {
    w_u32(f + 20, 8);
}
static void with_dirty_bit(uint8_t *f) {
    w_u64(f + 72, QCOW2_INCOMPAT_DIRTY);
}

TEST(qcow2_refuses_what_it_cannot_read) {
    ASSERT_EQ_INT(-ENOTSUP, open_patched(with_backing));
    ASSERT_EQ_INT(-ENOTSUP, open_patched(with_encryption));
    ASSERT_EQ_INT(-ENOTSUP, open_patched(with_unknown_feature));
    ASSERT_EQ_INT(-ENOTSUP, open_patched(with_data_file));
    ASSERT_EQ_INT(-ENOTSUP, open_patched(with_zstd)); // the image has compressed clusters
    ASSERT_EQ_INT(-EINVAL, open_patched(with_l2_past_eof));
    ASSERT_EQ_INT(-EINVAL, open_patched(with_tiny_l1));
    ASSERT_EQ_INT(-EINVAL, open_patched(with_bad_cluster_bits));
    // Refcount state does not matter to a reader.
    ASSERT_EQ_INT(0, open_patched(with_dirty_bit));
}

// A deflated cluster that does not decode to a whole cluster is a read
// error, not a short or padded cluster.
TEST(qcow2_decode_cluster_checks_length) {
    uint8_t in[512], stream[1024], out[CL];
    memset(in, 0x33, sizeof in);
    size_t n = raw_deflate(in, sizeof in, stream, sizeof stream);
    ASSERT_EQ_INT(-EINVAL, qcow2_decode_cluster(stream, n, CB, out, CL));
    uint8_t full[CL];
    memset(full, 0x44, CL);
    n = raw_deflate(full, CL, stream, sizeof stream);
    ASSERT_EQ_INT(0, qcow2_decode_cluster(stream, n, CB, out, 100)); // a cut final cluster
    ASSERT_EQ_INT(0x44, out[99]);
}

int main(void) {
    RUN(qcow2_detect_and_parse);
    RUN(qcow2_compressed_extent_fields);
    RUN(qcow2_source_round_trip);
    RUN(qcow2_unwraps_through_the_registry);
    RUN(qcow2_extended_l2_subclusters);
    RUN(qcow2_version_2);
    RUN(qcow2_refuses_what_it_cannot_read);
    RUN(qcow2_decode_cluster_checks_length);
    fprintf(stderr, "All qcow2 tests passed.\n");
    return 0;
}
