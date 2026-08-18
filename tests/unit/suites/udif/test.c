// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for the UDIF (.dmg) decoder: the shared zlib/DEFLATE
// decompressor (src/core/storage/inflate.c) and the 'koly' trailer + 'mish'
// block-map parser and chunk decoder (src/core/storage/image_udif.c).
//
// The trailer, property list and 'mish' tables are built by hand here so the
// tests need no external image fixture, and the byte offsets are restated
// independently of the parser's own constants.  The layout matches a real
// Toast-mastered UDZO CD image and Jonathan Levin's "Demystifying the DMG
// File Format".

#include "image_udif.h"
#include "inflate.h"
#include "test_assert.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 'koly' trailer offsets, restated from the format rather than the parser.
#define KOLY_DATA_OFFSET   0x18
#define KOLY_DATA_LENGTH   0x20
#define KOLY_SEGMENT_COUNT 0x3C
#define KOLY_XML_OFFSET    0xD8
#define KOLY_XML_LENGTH    0xE0
#define KOLY_SECTORS       0x1EC

// 'mish' block-table offsets.
#define MISH_SECTOR        0x08
#define MISH_SECTOR_COUNT  0x10
#define MISH_DATA_OFFSET   0x18
#define MISH_CHECKSUM_TYPE 0x40
#define MISH_CHECKSUM      0x48
#define MISH_CHUNK_COUNT   0xC8
#define MISH_ENTRIES       0xCC
#define MISH_ENTRY_SIZE    40

static void w_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void w_u64(uint8_t *p, uint64_t v) {
    w_u32(p, (uint32_t)(v >> 32));
    w_u32(p + 4, (uint32_t)v);
}

// ---- zlib / inflate --------------------------------------------------------

// A zlib stream carrying one stored (uncompressed) DEFLATE block.  Trivial to
// build by hand, and the shape most of the chunk tests below need.
static uint8_t *zlib_stored(const uint8_t *payload, uint16_t len, size_t *out_len) {
    uint8_t *s = (uint8_t *)malloc((size_t)len + 11);
    s[0] = 0x78; // CM=8, CINFO=7
    s[1] = 0x01; // FLEVEL=0, FCHECK making (0x7801 % 31) == 0
    s[2] = 0x01; // BFINAL=1, BTYPE=00 (stored)
    s[3] = (uint8_t)len;
    s[4] = (uint8_t)(len >> 8);
    s[5] = (uint8_t)~len;
    s[6] = (uint8_t)(~len >> 8);
    memcpy(s + 7, payload, len);
    memset(s + 7 + len, 0, 4); // adler32 trailer (unverified)
    *out_len = (size_t)len + 11;
    return s;
}

TEST(inflate_stored_block) {
    const uint8_t payload[] = "granny smith";
    size_t slen = 0;
    uint8_t *stream = zlib_stored(payload, sizeof(payload), &slen);
    uint8_t out[64] = {0};
    long n = inflate_zlib(stream, slen, out, sizeof(out));
    ASSERT_EQ_INT((int)sizeof(payload), (int)n);
    ASSERT_EQ_INT(0, memcmp(out, payload, sizeof(payload)));
    free(stream);
}

TEST(inflate_fixed_huffman) {
    // zlib.compress("GRANNY SMITH UDIF TEST PAYLOAD " * 4, level=1) -> BTYPE=1.
    static const uint8_t stream[] = {0x78, 0x01, 0x73, 0x0F, 0x72, 0xF4, 0xF3, 0x8B, 0x54, 0x08, 0xF6,
                                     0xF5, 0x0C, 0xF1, 0x50, 0x08, 0x75, 0xF1, 0x74, 0x53, 0x08, 0x71,
                                     0x0D, 0x0E, 0x51, 0x08, 0x70, 0x8C, 0xF4, 0xF1, 0x77, 0x74, 0x51,
                                     0x70, 0xA7, 0xA5, 0x34, 0x00, 0x40, 0x00, 0x21, 0x99};
    uint8_t out[256] = {0};
    long n = inflate_zlib(stream, sizeof(stream), out, sizeof(out));
    ASSERT_EQ_INT(124, (int)n);
    ASSERT_EQ_INT(0, memcmp(out, "GRANNY SMITH UDIF TEST PAYLOAD GRANNY SMITH UDIF TEST PAYLOAD ", 62));
}

TEST(inflate_dynamic_huffman) {
    // 600 bytes of skewed random data, zlib level 9 -> BTYPE=2 (its own code
    // table).  Checked by length + CRC-32 rather than embedding the payload.
    static const uint8_t stream[] = {
        0x78, 0xDA, 0x25, 0x92, 0x89, 0x0D, 0xC4, 0x40, 0x08, 0x03, 0x6B, 0xC3, 0x06, 0xB6, 0xFF, 0x8E, 0x32, 0x26,
        0xCA, 0x49, 0xD9, 0xC7, 0x60, 0x33, 0xB9, 0x56, 0x6F, 0x69, 0xD4, 0x53, 0xE3, 0x52, 0xB7, 0xAC, 0xE9, 0x1A,
        0x79, 0x97, 0x23, 0x96, 0x66, 0xC3, 0xF1, 0x70, 0x3C, 0x8F, 0xCB, 0x59, 0xDF, 0x0E, 0xFD, 0xBC, 0xE9, 0x1E,
        0x44, 0x6D, 0xA7, 0x8C, 0x15, 0x8F, 0x58, 0x45, 0xD2, 0xF5, 0x94, 0xBB, 0x28, 0x38, 0x44, 0xAD, 0xEA, 0xBD,
        0x9A, 0x87, 0x43, 0x3B, 0x4D, 0x10, 0x1A, 0x05, 0xB7, 0x3D, 0x26, 0x85, 0x73, 0x69, 0xD4, 0xF2, 0xC3, 0x79,
        0xEC, 0x26, 0xD5, 0xB5, 0x8E, 0xF5, 0xAB, 0xAB, 0x4F, 0x9E, 0x25, 0x1A, 0x65, 0x29, 0x2A, 0x91, 0xF1, 0x66,
        0x48, 0x1F, 0x11, 0xF0, 0x8F, 0x91, 0x68, 0x92, 0x70, 0x32, 0x4E, 0xE9, 0x80, 0xED, 0x46, 0x63, 0xCE, 0xB8,
        0x5C, 0xC7, 0xC0, 0xCE, 0x9C, 0x95, 0x75, 0x06, 0x08, 0x02, 0xD6, 0xA7, 0x28, 0x6C, 0x56, 0xEF, 0x5A, 0xF4,
        0x76, 0x92, 0x88, 0x51, 0x2B, 0x59, 0xD2, 0xFF, 0x25, 0x95, 0xAA, 0x36, 0xF6, 0x76, 0xD1, 0x6D, 0xD2, 0x1E,
        0x59, 0x81, 0x2B, 0x40, 0xC2, 0xEA, 0x60, 0x55, 0xD0, 0x66, 0xE6, 0x7E, 0x47, 0x84, 0x48, 0x54, 0xA4, 0x4B,
        0x1D, 0xBB, 0x0C, 0x3B, 0xF7, 0xBA, 0x47, 0xBF, 0xDB, 0x0B, 0x21, 0x6C, 0xB4, 0xEF, 0xBE, 0xCD, 0x41, 0x7A,
        0xF1, 0x81, 0x02, 0xB4, 0xD4, 0x95, 0xDA, 0xEA, 0xA3, 0x1F, 0x70, 0xBA, 0x1C, 0xEA, 0x54, 0xFE, 0x08, 0x7C,
        0x98, 0x09, 0x79, 0xF5, 0x4E, 0x84, 0xC3, 0x5F, 0xD9, 0x5D, 0xE8, 0x0D, 0x79, 0xC6, 0x09, 0x6E, 0x57, 0x05,
        0x7C, 0x7E, 0x7A, 0xDB, 0x37, 0x44, 0x07, 0xD3, 0xF2, 0x3F, 0x00, 0x75, 0xF8, 0x92, 0xFB, 0x31, 0x5F, 0xAA,
        0x68, 0xE2, 0xE0, 0xED, 0xA8, 0x2A, 0x64, 0xC9, 0xA2, 0x0F, 0x63, 0x23, 0x9E, 0xA7};
    uint8_t out[1024] = {0};
    long n = inflate_zlib(stream, sizeof(stream), out, sizeof(out));
    ASSERT_EQ_INT(600, (int)n);
    ASSERT_EQ_INT((int)0xDAF73B30u, (int)udif_crc32(0, out, 600));
}

TEST(inflate_alloc_form_matches) {
    const uint8_t payload[] = "the malloc'ing entry point the PNG reader uses";
    size_t slen = 0;
    uint8_t *stream = zlib_stored(payload, sizeof(payload), &slen);
    size_t out_len = 0;
    uint8_t *out = inflate_zlib_alloc(stream, slen, &out_len);
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ_INT((int)sizeof(payload), (int)out_len);
    ASSERT_EQ_INT(0, memcmp(out, payload, sizeof(payload)));
    free(out);
    free(stream);
}

TEST(inflate_rejects_bad_headers) {
    uint8_t out[64];
    const uint8_t payload[] = "x";
    size_t slen = 0;
    uint8_t *stream = zlib_stored(payload, sizeof(payload), &slen);

    // Compression method must be 8 (deflate).
    uint8_t bad_cm[64];
    memcpy(bad_cm, stream, slen);
    bad_cm[0] = 0x79;
    ASSERT_EQ_INT(-1, (int)inflate_zlib(bad_cm, slen, out, sizeof(out)));

    // The two header bytes must form a multiple of 31.
    uint8_t bad_check[64];
    memcpy(bad_check, stream, slen);
    bad_check[1] = 0x02;
    ASSERT_EQ_INT(-1, (int)inflate_zlib(bad_check, slen, out, sizeof(out)));

    // A preset dictionary (FDICT) is never used by these images.
    uint8_t fdict[64];
    memcpy(fdict, stream, slen);
    fdict[1] = 0x3D; // FDICT set, still a multiple of 31
    ASSERT_EQ_INT(-1, (int)inflate_zlib(fdict, slen, out, sizeof(out)));

    // Truncated mid-block.
    ASSERT_EQ_INT(-1, (int)inflate_zlib(stream, 5, out, sizeof(out)));
    free(stream);
}

TEST(inflate_overflow_is_an_error_not_a_clamp) {
    // A caller with a fixed buffer stated the exact expected size, so a stream
    // that expands past it must fail rather than silently truncate.
    uint8_t payload[64];
    memset(payload, 'Z', sizeof(payload));
    size_t slen = 0;
    uint8_t *stream = zlib_stored(payload, sizeof(payload), &slen);
    uint8_t out[16];
    ASSERT_EQ_INT(-1, (int)inflate_zlib(stream, slen, out, sizeof(out)));
    free(stream);
}

TEST(crc32_known_vector) {
    ASSERT_EQ_INT((int)0xCBF43926u, (int)udif_crc32(0, (const uint8_t *)"123456789", 9));
}

// ---- 'koly' trailer --------------------------------------------------------

// Build a well-formed version-4 trailer; callers patch individual fields.
static void build_trailer(uint8_t t[UDIF_TRAILER_SIZE]) {
    memset(t, 0, UDIF_TRAILER_SIZE);
    memcpy(t, "koly", 4);
    w_u32(t + 0x04, 4); // version
    w_u32(t + 0x08, UDIF_TRAILER_SIZE); // header size
    w_u64(t + KOLY_DATA_OFFSET, 0);
    w_u64(t + KOLY_DATA_LENGTH, 4096);
    w_u32(t + KOLY_SEGMENT_COUNT, 1);
    w_u32(t + 0x50, UDIF_CHECKSUM_CRC32);
    w_u32(t + 0x58, 0xDEADBEEF);
    w_u64(t + KOLY_XML_OFFSET, 4096);
    w_u64(t + KOLY_XML_LENGTH, 512);
    w_u64(t + KOLY_SECTORS, 128);
}

TEST(udif_detect_and_parse_trailer) {
    uint8_t t[UDIF_TRAILER_SIZE];
    build_trailer(t);
    ASSERT_TRUE(udif_detect(t, sizeof(t)));

    udif_trailer_t tr;
    ASSERT_EQ_INT(0, udif_parse_trailer(t, sizeof(t), &tr));
    ASSERT_EQ_INT(0, (int)tr.data_fork_offset);
    ASSERT_EQ_INT(4096, (int)tr.data_fork_length);
    ASSERT_EQ_INT(4096, (int)tr.xml_offset);
    ASSERT_EQ_INT(512, (int)tr.xml_length);
    ASSERT_EQ_INT(128, (int)tr.sectors);
    ASSERT_EQ_INT((int)UDIF_CHECKSUM_CRC32, (int)tr.checksum_type);
    ASSERT_EQ_INT((int)0xDEADBEEFu, (int)tr.checksum);
}

TEST(udif_detect_rejects_impostors) {
    uint8_t t[UDIF_TRAILER_SIZE];

    // Wrong magic — four payload bytes are not a trailer.
    build_trailer(t);
    memcpy(t, "koln", 4);
    ASSERT_TRUE(!udif_detect(t, sizeof(t)));

    // Right magic, wrong version.
    build_trailer(t);
    w_u32(t + 0x04, 3);
    ASSERT_TRUE(!udif_detect(t, sizeof(t)));

    // Right magic and version, wrong declared header size.
    build_trailer(t);
    w_u32(t + 0x08, 1024);
    ASSERT_TRUE(!udif_detect(t, sizeof(t)));

    // Short buffer.
    build_trailer(t);
    ASSERT_TRUE(!udif_detect(t, 128));
}

TEST(udif_trailer_rejects_unsupported) {
    uint8_t t[UDIF_TRAILER_SIZE];
    udif_trailer_t tr;

    // A spanned image keeps later chunks in sibling .dmgpart files.
    build_trailer(t);
    w_u32(t + KOLY_SEGMENT_COUNT, 3);
    ASSERT_EQ_INT(-ENOTSUP, udif_parse_trailer(t, sizeof(t), &tr));

    // No block map at all.
    build_trailer(t);
    w_u64(t + KOLY_XML_LENGTH, 0);
    ASSERT_EQ_INT(-EINVAL, udif_parse_trailer(t, sizeof(t), &tr));

    // A zero-sector image has nothing to decode.
    build_trailer(t);
    w_u64(t + KOLY_SECTORS, 0);
    ASSERT_EQ_INT(-EINVAL, udif_parse_trailer(t, sizeof(t), &tr));
}

// ---- 'mish' block map inside the property list -----------------------------

// One chunk entry as the test wants to express it.
typedef struct {
    uint32_t type;
    uint64_t sector;
    uint64_t count;
    uint64_t offset;
    uint64_t length;
} test_chunk_t;

// Build a 'mish' block table with `n` entries plus a terminator.
static uint8_t *build_mish(uint64_t base, uint64_t sectors, uint32_t checksum, const test_chunk_t *chunks, size_t n,
                           size_t *out_len) {
    size_t len = MISH_ENTRIES + (n + 1) * MISH_ENTRY_SIZE;
    uint8_t *b = (uint8_t *)calloc(1, len);
    memcpy(b, "mish", 4);
    w_u32(b + 0x04, 1); // version
    w_u64(b + MISH_SECTOR, base);
    w_u64(b + MISH_SECTOR_COUNT, sectors);
    w_u64(b + MISH_DATA_OFFSET, 0);
    w_u32(b + 0x24, 0xFFFFFFFFu); // resource ID, deliberately not a count
    w_u32(b + MISH_CHECKSUM_TYPE, UDIF_CHECKSUM_CRC32);
    w_u32(b + MISH_CHECKSUM, checksum);
    w_u32(b + MISH_CHUNK_COUNT, (uint32_t)(n + 1));
    for (size_t i = 0; i < n; i++) {
        uint8_t *e = b + MISH_ENTRIES + i * MISH_ENTRY_SIZE;
        w_u32(e, chunks[i].type);
        w_u64(e + 8, chunks[i].sector);
        w_u64(e + 16, chunks[i].count);
        w_u64(e + 24, chunks[i].offset);
        w_u64(e + 32, chunks[i].length);
    }
    w_u32(b + MISH_ENTRIES + n * MISH_ENTRY_SIZE, UDIF_CHUNK_END);
    *out_len = len;
    return b;
}

// Base64-encode, wrapping like a real plist does so the decoder's whitespace
// handling is exercised.
static char *base64_encode(const uint8_t *data, size_t len) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out = (char *)malloc(len * 2 + 64);
    size_t o = 0, col = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len)
            v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len)
            v |= data[i + 2];
        out[o++] = alphabet[(v >> 18) & 63];
        out[o++] = alphabet[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? alphabet[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? alphabet[v & 63] : '=';
        if ((col += 4) >= 64) {
            out[o++] = '\n';
            out[o++] = '\t';
            col = 0;
        }
    }
    out[o] = '\0';
    return out;
}

// Wrap one Base64 blob in the plist skeleton hdiutil writes.
static char *build_plist(const char *name, const char *b64) {
    char *xml = (char *)malloc(strlen(b64) + strlen(name) + 1024);
    sprintf(xml,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n"
            "\t<key>resource-fork</key>\n\t<dict>\n\t\t<key>blkx</key>\n\t\t<array>\n"
            "\t\t\t<dict>\n"
            "\t\t\t\t<key>Attributes</key>\n\t\t\t\t<string>0x0050</string>\n"
            "\t\t\t\t<key>CFName</key>\n\t\t\t\t<string>%s</string>\n"
            "\t\t\t\t<key>Data</key>\n\t\t\t\t<data>\n\t\t\t\t%s\n\t\t\t\t</data>\n"
            "\t\t\t\t<key>ID</key>\n\t\t\t\t<string>0</string>\n"
            "\t\t\t\t<key>Name</key>\n\t\t\t\t<string>%s</string>\n"
            "\t\t\t</dict>\n\t\t</array>\n\t</dict>\n</dict>\n</plist>\n",
            name, b64, name);
    return xml;
}

// Build a one-table map from `chunks`, returning the parsed result.
static udif_map_t *parse_one_table(uint64_t base, uint64_t sectors, const test_chunk_t *chunks, size_t n, int *rc_out) {
    size_t mish_len = 0;
    uint8_t *mish = build_mish(base, sectors, 0, chunks, n, &mish_len);
    char *b64 = base64_encode(mish, mish_len);
    char *xml = build_plist("Apple_HFS : 3", b64);
    udif_map_t *m = NULL;
    *rc_out = udif_parse_blkx((const uint8_t *)xml, strlen(xml), &m);
    free(xml);
    free(b64);
    free(mish);
    return m;
}

TEST(udif_parse_blkx_reads_chunks) {
    const test_chunk_t chunks[] = {
        {UDIF_CHUNK_ZLIB, 0, 2, 0,   40 },
        {UDIF_CHUNK_RAW,  2, 1, 40,  512},
        {UDIF_CHUNK_ZERO, 3, 5, 552, 0  },
    };
    int rc = -1;
    udif_map_t *m = parse_one_table(10, 8, chunks, 3, &rc);
    ASSERT_EQ_INT(0, rc);
    ASSERT_TRUE(m != NULL);
    ASSERT_EQ_INT(1, (int)m->n_tables);

    udif_table_t *t = &m->tables[0];
    // The name comes from CFName, not the first <string> in the dict (which
    // is the Attributes word).
    ASSERT_EQ_INT(0, strcmp(t->name, "Apple_HFS : 3"));
    ASSERT_EQ_INT(10, (int)t->base_sector);
    ASSERT_EQ_INT(8, (int)t->sectors);
    // The terminator is dropped; three usable chunks remain.
    ASSERT_EQ_INT(3, (int)t->n_chunks);
    ASSERT_EQ_INT((int)UDIF_CHUNK_ZLIB, (int)t->chunks[0].type);
    ASSERT_EQ_INT(2, (int)t->chunks[0].count);
    ASSERT_EQ_INT(40, (int)t->chunks[0].length);
    // Chunk sectors are relative to the table base: absolute = 10 + 2.
    ASSERT_EQ_INT(2, (int)t->chunks[1].sector);
    ASSERT_EQ_INT(40, (int)t->chunks[1].offset);
    // Total image size spans to the far edge of the last table.
    ASSERT_EQ_INT(18, (int)m->sectors);
    udif_map_free(m);
}

TEST(udif_parse_blkx_rejects_malformed) {
    // A chunk running past the table's own sector count.
    const test_chunk_t overrun[] = {
        {UDIF_CHUNK_RAW, 6, 4, 0, 2048}
    };
    int rc = 0;
    udif_map_t *m = parse_one_table(0, 8, overrun, 1, &rc);
    ASSERT_EQ_INT(-EINVAL, rc);
    ASSERT_TRUE(m == NULL);

    // A blob whose 'mish' magic is wrong must not be read as a table.
    const test_chunk_t ok[] = {
        {UDIF_CHUNK_ZERO, 0, 1, 0, 0}
    };
    size_t mish_len = 0;
    uint8_t *mish = build_mish(0, 1, 0, ok, 1, &mish_len);
    memcpy(mish, "hsim", 4);
    char *b64 = base64_encode(mish, mish_len);
    char *xml = build_plist("bogus", b64);
    m = NULL;
    ASSERT_EQ_INT(-EINVAL, udif_parse_blkx((const uint8_t *)xml, strlen(xml), &m));
    ASSERT_TRUE(m == NULL);
    free(xml);
    free(b64);

    // A non-zero mish DataOffset would silently shift every read.
    mish = build_mish(0, 1, 0, ok, 1, &mish_len);
    w_u64(mish + MISH_DATA_OFFSET, 512);
    b64 = base64_encode(mish, mish_len);
    xml = build_plist("shifted", b64);
    m = NULL;
    ASSERT_EQ_INT(-ENOTSUP, udif_parse_blkx((const uint8_t *)xml, strlen(xml), &m));
    free(xml);
    free(b64);
    free(mish);

    // A plist with no blkx key is not a block map.
    const char *empty = "<?xml version=\"1.0\"?><plist><dict></dict></plist>";
    m = NULL;
    ASSERT_EQ_INT(-ENOENT, udif_parse_blkx((const uint8_t *)empty, strlen(empty), &m));
}

// ---- chunk decoding --------------------------------------------------------

TEST(udif_decode_zero_ignore_raw) {
    uint8_t dst[1024];

    // Zero-fill and ignored chunks both read as zeros.
    udif_chunk_t zero = {.type = UDIF_CHUNK_ZERO, .sector = 0, .count = 2, .offset = 0, .length = 0};
    memset(dst, 0xAA, sizeof(dst));
    ASSERT_EQ_INT(0, udif_decode_chunk(&zero, NULL, 0, dst, sizeof(dst)));
    for (size_t i = 0; i < 1024; i++)
        ASSERT_EQ_INT(0, dst[i]);

    udif_chunk_t ignore = {.type = UDIF_CHUNK_IGNORE, .sector = 0, .count = 1, .offset = 0, .length = 0};
    memset(dst, 0xAA, sizeof(dst));
    ASSERT_EQ_INT(0, udif_decode_chunk(&ignore, NULL, 0, dst, 512));
    for (size_t i = 0; i < 512; i++)
        ASSERT_EQ_INT(0, dst[i]);

    // Raw copies verbatim.
    uint8_t raw[512];
    memset(raw, 0x5A, sizeof(raw));
    udif_chunk_t rawc = {.type = UDIF_CHUNK_RAW, .sector = 0, .count = 1, .offset = 0, .length = 512};
    ASSERT_EQ_INT(0, udif_decode_chunk(&rawc, raw, sizeof(raw), dst, sizeof(dst)));
    ASSERT_EQ_INT(0, memcmp(dst, raw, 512));

    // A raw chunk whose source is short of a full sector run is malformed.
    ASSERT_EQ_INT(-EINVAL, udif_decode_chunk(&rawc, raw, 256, dst, sizeof(dst)));
}

TEST(udif_decode_zlib_chunk) {
    uint8_t sector[512];
    for (int i = 0; i < 512; i++)
        sector[i] = (uint8_t)(i * 7);
    size_t slen = 0;
    uint8_t *stream = zlib_stored(sector, sizeof(sector), &slen);

    udif_chunk_t c = {.type = UDIF_CHUNK_ZLIB, .sector = 0, .count = 1, .offset = 0, .length = slen};
    uint8_t dst[512] = {0};
    ASSERT_EQ_INT(0, udif_decode_chunk(&c, stream, slen, dst, sizeof(dst)));
    ASSERT_EQ_INT(0, memcmp(dst, sector, sizeof(sector)));

    // A stream that decodes to the wrong length disagrees with the map, which
    // must fail rather than yield a half-filled sector.
    udif_chunk_t two = {.type = UDIF_CHUNK_ZLIB, .sector = 0, .count = 2, .offset = 0, .length = slen};
    uint8_t big[1024] = {0};
    ASSERT_EQ_INT(-EINVAL, udif_decode_chunk(&two, stream, slen, big, sizeof(big)));
    free(stream);
}

TEST(udif_decode_adc_chunk) {
    // ADC: 0x80|len-1 literal run.  Emit 512 bytes as four 128-byte runs.
    uint8_t src[4 * 129];
    uint8_t expect[512];
    for (int r = 0; r < 4; r++) {
        src[r * 129] = 0x80 | 127; // literal run of 128
        for (int i = 0; i < 128; i++) {
            uint8_t v = (uint8_t)(r * 128 + i);
            src[r * 129 + 1 + i] = v;
            expect[r * 128 + i] = v;
        }
    }
    udif_chunk_t c = {.type = UDIF_CHUNK_ADC, .sector = 0, .count = 1, .offset = 0, .length = sizeof(src)};
    uint8_t dst[512] = {0};
    ASSERT_EQ_INT(0, udif_decode_chunk(&c, src, sizeof(src), dst, sizeof(dst)));
    ASSERT_EQ_INT(0, memcmp(dst, expect, sizeof(expect)));
}

TEST(udif_decode_rejects_unimplemented_codecs) {
    uint8_t src[16] = {0};
    uint8_t dst[512];
    // Real codecs we do not implement must say so, not zero-fill silently.
    const uint32_t codecs[] = {UDIF_CHUNK_BZ2, UDIF_CHUNK_LZFSE, UDIF_CHUNK_LZMA};
    for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++) {
        udif_chunk_t c = {.type = codecs[i], .sector = 0, .count = 1, .offset = 0, .length = sizeof(src)};
        ASSERT_EQ_INT(-ENOTSUP, udif_decode_chunk(&c, src, sizeof(src), dst, sizeof(dst)));
    }
    // An unknown type code is malformed rather than merely unsupported.
    udif_chunk_t bogus = {.type = 0x12345678u, .sector = 0, .count = 1, .offset = 0, .length = sizeof(src)};
    ASSERT_EQ_INT(-EINVAL, udif_decode_chunk(&bogus, src, sizeof(src), dst, sizeof(dst)));

    // A destination smaller than the chunk's sector run is a caller error.
    udif_chunk_t zero = {.type = UDIF_CHUNK_ZERO, .sector = 0, .count = 4, .offset = 0, .length = 0};
    ASSERT_EQ_INT(-EINVAL, udif_decode_chunk(&zero, NULL, 0, dst, sizeof(dst)));
}

int main(void) {
    RUN(inflate_stored_block);
    RUN(inflate_fixed_huffman);
    RUN(inflate_dynamic_huffman);
    RUN(inflate_alloc_form_matches);
    RUN(inflate_rejects_bad_headers);
    RUN(inflate_overflow_is_an_error_not_a_clamp);
    RUN(crc32_known_vector);
    RUN(udif_detect_and_parse_trailer);
    RUN(udif_detect_rejects_impostors);
    RUN(udif_trailer_rejects_unsupported);
    RUN(udif_parse_blkx_reads_chunks);
    RUN(udif_parse_blkx_rejects_malformed);
    RUN(udif_decode_zero_ignore_raw);
    RUN(udif_decode_zlib_chunk);
    RUN(udif_decode_adc_chunk);
    RUN(udif_decode_rejects_unimplemented_codecs);
    fprintf(stderr, "All udif tests passed.\n");
    return 0;
}
