// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Zip, gzip, tar and inflate, structure first.
//
// As in test.c, every input is built here: a small DEFLATE encoder (stored,
// fixed and dynamic blocks, with LZ77 matches), zip, gzip/BGZF and tar
// writers, and a counting source that records every read a test makes of
// an archive.
// The encoder is the spec (RFC 1951) written forwards, so decoder and
// encoder meet only in the format.

#include "internal.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void zipgz_tests(void);

// ============================================================================
// Payloads
// ============================================================================

// A deterministic mix: repeated text (long matches), a run of one byte
// (distance-1 overlapping matches), incompressible noise, and zeros.
static uint8_t *dz_payload(size_t n) {
    uint8_t *p = malloc(n);
    ASSERT_TRUE(p != NULL);
    uint64_t x = 0x9e3779b97f4a7c15ull;
    for (size_t i = 0; i < n;) {
        size_t seg = (i / 4096) % 4;
        size_t end = i + 4096 < n ? i + 4096 : n;
        for (; i < end; i++) {
            switch (seg) {
            case 0:
                p[i] = (uint8_t) "the quick brown fox jumps over the lazy dog\n"[i % 44];
                break;
            case 1:
                p[i] = 'a';
                break;
            case 2:
                x = x * 6364136223846793005ull + 1442695040888963407ull;
                p[i] = (uint8_t)(x >> 56);
                break;
            default:
                p[i] = 0;
                break;
            }
        }
    }
    return p;
}

// ============================================================================
// A DEFLATE encoder (RFC 1951)
// ============================================================================

typedef struct {
    uint8_t *buf;
    size_t n, cap;
    uint32_t acc; // pending bits, LSB first
    int fill;
} dz_bits_t;

// Append `n` bits of `v`, least significant first (§ 3.1.1).
static void dz_put(dz_bits_t *w, uint32_t v, int n) {
    for (int i = 0; i < n; i++) {
        w->acc |= ((v >> i) & 1u) << w->fill;
        if (++w->fill == 8) {
            if (w->n == w->cap) {
                w->cap = w->cap ? w->cap * 2 : 4096;
                w->buf = realloc(w->buf, w->cap);
                ASSERT_TRUE(w->buf != NULL);
            }
            w->buf[w->n++] = (uint8_t)w->acc;
            w->acc = 0;
            w->fill = 0;
        }
    }
}

// Pad to a byte boundary.
static void dz_align(dz_bits_t *w) {
    if (w->fill)
        dz_put(w, 0, 8 - w->fill);
}

// A Huffman code: packed most significant bit first (§ 3.1.1).
static void dz_code(dz_bits_t *w, uint32_t code, int len) {
    for (int i = len - 1; i >= 0; i--)
        dz_put(w, (code >> i) & 1u, 1);
}

// Canonical codes from lengths (§ 3.2.2).
static void dz_canon(const uint8_t *len, int n, uint32_t *code) {
    int count[16] = {0};
    uint32_t next[16] = {0};
    for (int i = 0; i < n; i++)
        count[len[i]]++;
    count[0] = 0;
    uint32_t c = 0;
    for (int b = 1; b < 16; b++) {
        c = (c + (uint32_t)count[b - 1]) << 1;
        next[b] = c;
    }
    for (int i = 0; i < n; i++)
        code[i] = len[i] ? next[len[i]]++ : 0;
}

// § 3.2.5 tables.
static const uint16_t dz_lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t dz_lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dz_dbase[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                      33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                      1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dz_dext[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

typedef struct {
    uint8_t llen[288], dlen[30];
    uint32_t lcode[288], dcode[30];
} dz_tables_t;

// The fixed codes (§ 3.2.6).
static void dz_fixed(dz_tables_t *t) {
    for (int i = 0; i < 288; i++)
        t->llen[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    for (int i = 0; i < 30; i++)
        t->dlen[i] = 5;
    dz_canon(t->llen, 288, t->lcode);
    dz_canon(t->dlen, 30, t->dcode);
}

// A dynamic block's codes: complete, but not frequency-shaped -- 226 lit/len
// codes of 8 bits and 60 of 9; two distance codes of 4 bits and 28 of 5.
static void dz_dynamic(dz_tables_t *t) {
    memset(t, 0, sizeof(*t));
    for (int i = 0; i < 286; i++)
        t->llen[i] = i < 226 ? 8 : 9;
    for (int i = 0; i < 30; i++)
        t->dlen[i] = i < 2 ? 4 : 5;
    dz_canon(t->llen, 286, t->lcode);
    dz_canon(t->dlen, 30, t->dcode);
}

// The dynamic block header (§ 3.2.7) for dz_dynamic's lengths, written with
// a code-length code over the four lengths used (4, 5, 8, 9), two bits each.
static void dz_dynamic_header(dz_bits_t *w, const dz_tables_t *t) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t cl[19] = {0};
    cl[4] = cl[5] = cl[8] = cl[9] = 2;
    uint32_t cc[19];
    dz_canon(cl, 19, cc);
    dz_put(w, 286 - 257, 5); // HLIT
    dz_put(w, 30 - 1, 5); // HDIST
    dz_put(w, 12 - 4, 4); // HCLEN: through symbol 4, the 12th in order
    for (int i = 0; i < 12; i++)
        dz_put(w, cl[order[i]], 3);
    for (int i = 0; i < 286; i++)
        dz_code(w, cc[t->llen[i]], 2);
    for (int i = 0; i < 30; i++)
        dz_code(w, cc[t->dlen[i]], 2);
}

// Emit `in[from, to)` as literals and greedy LZ77 matches against all of
// `in` before it (the window spans block boundaries, as DEFLATE allows).
static void dz_lz77(dz_bits_t *w, const dz_tables_t *t, const uint8_t *in, size_t from, size_t to, int32_t *head) {
    size_t i = from;
    while (i < to) {
        size_t best = 0, dist = 0;
        if (i + 3 <= to) {
            uint32_t h = ((uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2]) * 2654435761u >> 16;
            int32_t cand = head[h];
            head[h] = (int32_t)i;
            if (cand >= 0 && i - (size_t)cand <= 32768) {
                size_t l = 0;
                while (l < 258 && i + l < to && in[(size_t)cand + l] == in[i + l])
                    l++;
                if (l >= 3) {
                    best = l;
                    dist = i - (size_t)cand;
                }
            }
        }
        if (!best) {
            dz_code(w, t->lcode[in[i]], t->llen[in[i]]);
            i++;
            continue;
        }
        int lc = 28;
        while (dz_lbase[lc] > best)
            lc--;
        dz_code(w, t->lcode[257 + lc], t->llen[257 + lc]);
        dz_put(w, (uint32_t)(best - dz_lbase[lc]), dz_lext[lc]);
        int dc = 29;
        while (dz_dbase[dc] > dist)
            dc--;
        dz_code(w, t->dcode[dc], t->dlen[dc]);
        dz_put(w, (uint32_t)(dist - dz_dbase[dc]), dz_dext[dc]);
        i += best;
    }
}

// Raw DEFLATE of `in`, in blocks of `block` bytes cycling stored, fixed,
// dynamic (`types` limits the cycle: 1 = stored only, 3 = all three).
static uint8_t *dz_deflate(const uint8_t *in, size_t n, size_t block, int types, size_t *out_n) {
    dz_bits_t w = {0};
    int32_t *head = malloc(65536 * sizeof(*head));
    ASSERT_TRUE(head != NULL);
    for (int i = 0; i < 65536; i++)
        head[i] = -1;
    dz_tables_t fixed, dyn;
    dz_fixed(&fixed);
    dz_dynamic(&dyn);
    size_t pos = 0, k = 0;
    do {
        size_t len = n - pos < block ? n - pos : block;
        int last = pos + len == n;
        int type = (int)(k++ % (size_t)types);
        dz_put(&w, (uint32_t)last, 1);
        if (type == 0) { // stored (§ 3.2.4)
            dz_put(&w, 0, 2);
            dz_align(&w);
            dz_put(&w, (uint32_t)len, 16);
            dz_put(&w, (uint32_t)~len & 0xFFFFu, 16);
            for (size_t i = 0; i < len; i++)
                dz_put(&w, in[pos + i], 8);
        } else {
            const dz_tables_t *t = type == 1 ? &fixed : &dyn;
            dz_put(&w, (uint32_t)type, 2);
            if (type == 2)
                dz_dynamic_header(&w, t);
            dz_lz77(&w, t, in, pos, pos + len, head);
            dz_code(&w, t->lcode[256], t->llen[256]);
        }
        pos += len;
    } while (pos < n);
    dz_align(&w);
    free(head);
    *out_n = w.n;
    return w.buf;
}

// ============================================================================
// A counting source over a list of regions (some synthesised)
// ============================================================================

typedef struct {
    uint64_t off, len;
    const uint8_t *bytes; // NULL: synthesised, byte = (off * 7 + 3) & 0xFF
} cnt_region_t;

#define CNT_MAX_READS 256

typedef struct {
    cnt_region_t regions[8];
    int n_regions;
    uint64_t size;
    int reads;
    uint64_t lo[CNT_MAX_READS], hi[CNT_MAX_READS];
} cnt_src_t;

static uint8_t cnt_synth(uint64_t off) {
    return (uint8_t)((off * 7 + 3) & 0xFF);
}

static int64_t cnt_read(peel_source_t *s, uint64_t off, void *buf, size_t len) {
    cnt_src_t *c = s->ctx;
    if (off >= c->size)
        return 0;
    if (len > c->size - off)
        len = (size_t)(c->size - off);
    if (c->reads < CNT_MAX_READS) {
        c->lo[c->reads] = off;
        c->hi[c->reads] = off + len;
    }
    c->reads++;
    uint8_t *o = buf;
    for (size_t i = 0; i < len; i++) {
        uint64_t at = off + i;
        uint8_t b = 0;
        for (int r = 0; r < c->n_regions; r++) {
            const cnt_region_t *g = &c->regions[r];
            if (at >= g->off && at < g->off + g->len) {
                b = g->bytes ? g->bytes[at - g->off] : cnt_synth(at - g->off);
                break;
            }
        }
        o[i] = b;
    }
    return (int64_t)len;
}

static uint64_t cnt_size(peel_source_t *s) {
    return ((cnt_src_t *)s->ctx)->size;
}

static const char *cnt_key(peel_source_t *s) {
    (void)s;
    return "counted.zip";
}

static peel_tier_t cnt_tier(peel_source_t *s) {
    (void)s;
    return PEEL_TIER_RANDOM;
}

static void cnt_close(peel_source_t *s) {
    (void)s; // the test owns the context
}

static const peel_source_ops_t cnt_ops = {cnt_read, cnt_size, cnt_key, cnt_tier, cnt_close};

// True if any read since read number `from` touched [lo, hi).
static int cnt_touched(const cnt_src_t *c, int from, uint64_t lo, uint64_t hi) {
    for (int i = from; i < c->reads && i < CNT_MAX_READS; i++)
        if (c->lo[i] < hi && c->hi[i] > lo)
            return 1;
    return 0;
}

// ============================================================================
// Zip and gzip writers
// ============================================================================

typedef struct {
    uint8_t *buf;
    size_t n, cap;
} zt_buf_t;

static void zt_bytes(zt_buf_t *b, const void *p, size_t n) {
    if (b->n + n > b->cap) {
        while (b->n + n > b->cap)
            b->cap = b->cap ? b->cap * 2 : 4096;
        b->buf = realloc(b->buf, b->cap);
        ASSERT_TRUE(b->buf != NULL);
    }
    memcpy(b->buf + b->n, p, n);
    b->n += n;
}

static void zt_16(zt_buf_t *b, uint32_t v) {
    uint8_t x[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    zt_bytes(b, x, 2);
}

static void zt_32(zt_buf_t *b, uint32_t v) {
    uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    zt_bytes(b, x, 4);
}

static void zt_64(zt_buf_t *b, uint64_t v) {
    zt_32(b, (uint32_t)v);
    zt_32(b, (uint32_t)(v >> 32));
}

typedef struct {
    const char *name;
    const uint8_t *data;
    size_t len;
    int method; // 0 stored, 8 deflate
    int bad_crc; // store a wrong CRC
} zt_member_t;

// A zip of `members` (APPNOTE 4.3), with `prefix` bytes in front whose
// offsets are NOT adjusted (a plain-concatenated self-extractor), and with
// Zip64 extra fields and end records when `zip64`.
static uint8_t *zt_zip(const zt_member_t *m, int n, size_t prefix, int zip64, size_t *out_n) {
    zt_buf_t b = {0}, cd = {0};
    for (size_t i = 0; i < prefix; i++) {
        uint8_t x = (uint8_t)(0x4D + i);
        zt_bytes(&b, &x, 1);
    }
    for (int i = 0; i < n; i++) {
        size_t clen = m[i].len;
        uint8_t *packed = NULL;
        const uint8_t *body = m[i].data;
        if (m[i].method == 8) {
            packed = dz_deflate(m[i].data, m[i].len, 16384, 3, &clen);
            body = packed;
        }
        uint32_t crc = peel_crc32(0, m[i].data, m[i].len) ^ (m[i].bad_crc ? 1u : 0u);
        uint32_t local = (uint32_t)(b.n - prefix);
        size_t nl = strlen(m[i].name);
        // Local header.
        zt_32(&b, 0x04034b50);
        zt_16(&b, zip64 ? 45 : 20);
        zt_16(&b, 0);
        zt_16(&b, (uint32_t)m[i].method);
        zt_16(&b, 0);
        zt_16(&b, 0x2A21); // 2001-01-01
        zt_32(&b, crc);
        zt_32(&b, zip64 ? 0xFFFFFFFFu : (uint32_t)clen);
        zt_32(&b, zip64 ? 0xFFFFFFFFu : (uint32_t)m[i].len);
        zt_16(&b, (uint32_t)nl);
        zt_16(&b, zip64 ? 20 : 0);
        zt_bytes(&b, m[i].name, nl);
        if (zip64) {
            zt_16(&b, 1);
            zt_16(&b, 16);
            zt_64(&b, m[i].len);
            zt_64(&b, clen);
        }
        if (body && clen)
            zt_bytes(&b, body, clen);
        free(packed);
        // Central record.
        zt_32(&cd, 0x02014b50);
        zt_16(&cd, 0x031E);
        zt_16(&cd, zip64 ? 45 : 20);
        zt_16(&cd, 0);
        zt_16(&cd, (uint32_t)m[i].method);
        zt_16(&cd, 0);
        zt_16(&cd, 0x2A21);
        zt_32(&cd, crc);
        zt_32(&cd, zip64 ? 0xFFFFFFFFu : (uint32_t)clen);
        zt_32(&cd, zip64 ? 0xFFFFFFFFu : (uint32_t)m[i].len);
        zt_16(&cd, (uint32_t)nl);
        zt_16(&cd, zip64 ? 28 : 0);
        zt_16(&cd, 0);
        zt_16(&cd, 0);
        zt_16(&cd, 0);
        zt_32(&cd, 0);
        zt_32(&cd, zip64 ? 0xFFFFFFFFu : local);
        zt_bytes(&cd, m[i].name, nl);
        if (zip64) {
            zt_16(&cd, 1);
            zt_16(&cd, 24);
            zt_64(&cd, m[i].len);
            zt_64(&cd, clen);
            zt_64(&cd, local);
        }
    }
    uint64_t cd_off = b.n - prefix;
    zt_bytes(&b, cd.buf, cd.n);
    if (zip64) {
        uint64_t rec = b.n - prefix;
        zt_32(&b, 0x06064b50);
        zt_64(&b, 44);
        zt_16(&b, 45);
        zt_16(&b, 45);
        zt_32(&b, 0);
        zt_32(&b, 0);
        zt_64(&b, (uint64_t)n);
        zt_64(&b, (uint64_t)n);
        zt_64(&b, cd.n);
        zt_64(&b, cd_off);
        zt_32(&b, 0x07064b50);
        zt_32(&b, 0);
        zt_64(&b, rec);
        zt_32(&b, 1);
    }
    zt_32(&b, 0x06054b50);
    zt_16(&b, 0);
    zt_16(&b, 0);
    zt_16(&b, zip64 ? 0xFFFF : (uint32_t)n);
    zt_16(&b, zip64 ? 0xFFFF : (uint32_t)n);
    zt_32(&b, zip64 ? 0xFFFFFFFFu : (uint32_t)cd.n);
    zt_32(&b, zip64 ? 0xFFFFFFFFu : (uint32_t)cd_off);
    zt_16(&b, 0);
    free(cd.buf);
    *out_n = b.n;
    return b.buf;
}

// One gzip member (RFC 1952) holding `data`, with FNAME `name` (or none),
// a BGZF "BC" subfield when `bgzf`, and a wrong CRC when `bad_crc`.
static void zt_gzip_member(zt_buf_t *b, const uint8_t *data, size_t len, const char *name, int bgzf, int bad_crc) {
    size_t clen;
    uint8_t *packed = dz_deflate(data, len, 16384, bgzf ? 1 : 3, &clen);
    uint8_t flg = (uint8_t)((name ? 0x08 : 0) | (bgzf ? 0x04 : 0));
    uint8_t hdr[10] = {0x1f, 0x8b, 8, flg, 0, 0, 0, 0, 0, 0xFF};
    zt_bytes(b, hdr, 10);
    if (bgzf) {
        size_t bsize = 10 + 2 + 6 + clen + 8;
        zt_16(b, 6);
        uint8_t sf[4] = {'B', 'C', 2, 0};
        zt_bytes(b, sf, 4);
        zt_16(b, (uint32_t)(bsize - 1));
    }
    if (name)
        zt_bytes(b, name, strlen(name) + 1);
    zt_bytes(b, packed, clen);
    zt_32(b, peel_crc32(0, data, len) ^ (bad_crc ? 1u : 0u));
    zt_32(b, (uint32_t)len);
    free(packed);
}

// Read all of `s` in chunks of `step` bytes, last chunk first (every read
// but the first goes backwards), and compare with `want`.
static void zt_read_backwards(peel_source_t *s, const uint8_t *want, size_t len, size_t step) {
    uint8_t *got = malloc(len ? len : 1);
    ASSERT_TRUE(got != NULL);
    size_t chunks = (len + step - 1) / step;
    for (size_t k = chunks; k-- > 0;) {
        size_t off = k * step, n = len - off < step ? len - off : step;
        ASSERT_EQ_INT(0, peel_source_read_exact(s, off, got + off, n));
    }
    ASSERT_TRUE(memcmp(got, want, len) == 0);
    free(got);
}

// ============================================================================
// Inflate
// ============================================================================

// Every block type, with matches that cross block boundaries, decodes back.
TEST(test_inflate_every_block_type_round_trip) {
    size_t n = 150000, clen;
    uint8_t *in = dz_payload(n);
    uint8_t *z = dz_deflate(in, n, 10000, 3, &clen);
    ASSERT_TRUE(clen < n); // the matcher found the repeats
    uint8_t *out = malloc(n);
    ASSERT_EQ_INT((int)n, (int)peel_inflate(z, clen, out, n));
    ASSERT_TRUE(memcmp(in, out, n) == 0);
    free(out);
    free(z);
    free(in);
}

typedef struct {
    const uint8_t *s;
    size_t len, pos;
} dz_mem_t;

static int64_t dz_pull(void *ctx, uint8_t *buf, size_t cap) {
    dz_mem_t *m = ctx;
    size_t k = m->len - m->pos < cap ? m->len - m->pos : cap;
    memcpy(buf, m->s + m->pos, k);
    m->pos += k;
    return (int64_t)k;
}

// The inflater stops wherever the caller's buffer fills -- mid-literal run,
// mid-match, mid-stored-block -- and resumes exactly there.
TEST(test_inflate_resumes_at_every_output_boundary) {
    size_t n = 60000, clen;
    uint8_t *in = dz_payload(n);
    uint8_t *z = dz_deflate(in, n, 7000, 3, &clen);
    static const size_t caps[] = {1, 2, 3, 5, 7, 11, 64, 257, 258, 259, 1000, 4093};
    for (size_t c = 0; c < sizeof(caps) / sizeof(caps[0]); c++) {
        dz_mem_t m = {z, clen, 0};
        peel_inflater_t *inf = peel_inflater_new(dz_pull, &m);
        ASSERT_TRUE(inf != NULL);
        uint8_t *out = malloc(n);
        size_t got = 0;
        int rc = 0;
        while (rc == 0) {
            size_t k = 0, want = caps[c] < n - got ? caps[c] : n - got;
            rc = peel_inflater_run(inf, out + got, want ? want : 1, &k);
            ASSERT_TRUE(rc >= 0);
            got += k;
        }
        ASSERT_EQ_INT((int)n, (int)got);
        ASSERT_EQ_INT((int)n, (int)peel_inflater_total_out(inf));
        ASSERT_TRUE(memcmp(in, out, n) == 0);
        free(out);
        peel_inflater_free(inf);
    }
    free(z);
    free(in);
}

// A truncated stream is refused wherever it is cut -- never read past.
TEST(test_inflate_truncated_stream_is_refused) {
    size_t n = 20000, clen;
    uint8_t *in = dz_payload(n);
    uint8_t *z = dz_deflate(in, n, 5000, 3, &clen);
    uint8_t *out = malloc(n);
    for (size_t cut = 0; cut < clen; cut += clen / 37 + 1) {
        uint8_t *part = malloc(cut ? cut : 1); // exact size: ASan catches a read past it
        memcpy(part, z, cut);
        ASSERT_TRUE(peel_inflate(part, cut, out, n) < 0);
        free(part);
    }
    free(out);
    free(z);
    free(in);
}

// A match reaching back before the start of the output is refused, not
// copied from outside the window.
TEST(test_inflate_distance_before_start_is_refused) {
    dz_bits_t w = {0};
    dz_tables_t t;
    dz_fixed(&t);
    dz_put(&w, 1, 1);
    dz_put(&w, 1, 2); // fixed Huffman
    dz_code(&w, t.lcode['x'], t.llen['x']);
    dz_code(&w, t.lcode[257], t.llen[257]); // length 3
    dz_code(&w, t.dcode[4], t.dlen[4]); // distance 5 or 6: before the start
    dz_put(&w, 0, 1);
    dz_code(&w, t.lcode[256], t.llen[256]);
    dz_align(&w);
    uint8_t out[16];
    ASSERT_TRUE(peel_inflate(w.buf, w.n, out, sizeof(out)) < 0);
    free(w.buf);
}

// An over-subscribed code (more codes than its lengths can hold) is
// refused at the header.
TEST(test_inflate_oversubscribed_code_is_refused) {
    dz_bits_t w = {0};
    dz_put(&w, 1, 1);
    dz_put(&w, 2, 2); // dynamic
    dz_put(&w, 0, 5);
    dz_put(&w, 0, 5);
    dz_put(&w, 15, 4); // all 19 code-length codes...
    for (int i = 0; i < 19; i++)
        dz_put(&w, 1, 3); // ...of length 1: 19 codes for 2 slots
    for (int i = 0; i < 40; i++)
        dz_put(&w, 0, 8);
    uint8_t out[16];
    ASSERT_TRUE(peel_inflate(w.buf, w.n, out, sizeof(out)) < 0);
    free(w.buf);
}

// The zlib wrapper (RFC 1950): header checked, Adler-32 trailer checked.
TEST(test_zlib_wrapper_checks_its_trailer) {
    size_t n = 5000, clen;
    uint8_t *in = dz_payload(n);
    uint8_t *raw = dz_deflate(in, n, 5000, 3, &clen);
    uint8_t *z = malloc(clen + 6);
    z[0] = 0x78;
    z[1] = 0x9C; // (0x78 << 8 | 0x9C) % 31 == 0
    memcpy(z + 2, raw, clen);
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + in[i]) % 65521;
        b = (b + a) % 65521;
    }
    uint32_t adler = b << 16 | a;
    z[clen + 2] = (uint8_t)(adler >> 24);
    z[clen + 3] = (uint8_t)(adler >> 16);
    z[clen + 4] = (uint8_t)(adler >> 8);
    z[clen + 5] = (uint8_t)adler;
    uint8_t *out = malloc(n);
    ASSERT_EQ_INT((int)n, (int)peel_zlib_inflate(z, clen + 6, out, n));
    ASSERT_TRUE(memcmp(in, out, n) == 0);
    z[clen + 5] ^= 1;
    ASSERT_TRUE(peel_zlib_inflate(z, clen + 6, out, n) < 0);
    z[clen + 5] ^= 1;
    z[1] ^= 1; // the header check fails
    ASSERT_TRUE(peel_zlib_inflate(z, clen + 6, out, n) < 0);
    free(out);
    free(z);
    free(raw);
    free(in);
}

// ============================================================================
// Zip
// ============================================================================

// Open a zip held in memory.
static peel_archive_t *zt_open(const uint8_t *zip, size_t n, peel_source_t **src) {
    *src = peel_source_memory_keyed(zip, n, false, "t.zip");
    ASSERT_TRUE(*src != NULL);
    peel_err_t *err = NULL;
    peel_archive_t *a = peel_open(*src, NULL, NULL, &err);
    if (err)
        fprintf(stderr, "peel_open: %s\n", peel_err_msg(err));
    ASSERT_TRUE(a != NULL && err == NULL);
    return a;
}

// Every member's data fork, through its source, read backwards.
static void zt_check_members(peel_archive_t *a, const zt_member_t *m, int n) {
    for (int i = 0; i < n; i++) {
        int k = peel_lookup(a, m[i].name);
        ASSERT_TRUE(k >= 0);
        const peel_entry_t *e = peel_entry(a, k);
        ASSERT_EQ_INT((int)m[i].len, (int)e->data_len);
        if (m[i].len)
            ASSERT_EQ_INT(m[i].method == 0 ? PEEL_TIER_RANDOM : PEEL_TIER_EARNED, e->data_tier);
        peel_err_t *err = NULL;
        peel_source_t *s = peel_open_fork(a, k, PEEL_FORK_DATA, &err);
        ASSERT_TRUE(s != NULL && err == NULL);
        ASSERT_EQ_INT((int)m[i].len, (int)peel_source_size(s));
        zt_read_backwards(s, m[i].data, m[i].len, 4097);
        // A decoded member is random access once read through.
        ASSERT_EQ_INT(PEEL_TIER_RANDOM, peel_source_tier(s));
        peel_source_release(s);
    }
}

// Stored, deflated and empty members, a folder, nested paths; through the
// structure-first API and through peel().
TEST(test_zip_round_trip) {
    size_t n = 90000;
    uint8_t *p = dz_payload(n);
    zt_member_t m[] = {
        {"readme.txt",          (const uint8_t *)"hello\n", 6,     0, 0},
        {"data/payload.bin",    p,                          n,     8, 0},
        {"data/raw.bin",        p + 1000,                   30000, 0, 0},
        {"data/deep/empty.txt", (const uint8_t *)"",        0,     0, 0},
    };
    size_t zn;
    uint8_t *zip = zt_zip(m, 4, 0, 0, &zn);
    peel_source_t *src;
    peel_archive_t *a = zt_open(zip, zn, &src);
    ASSERT_TRUE(strcmp(peel_format(a), "zip") == 0);
    // Folders the zip never listed are synthesised.
    int d = peel_lookup(a, "data/deep");
    ASSERT_TRUE(d >= 0 && peel_entry(a, d)->is_dir);
    zt_check_members(a, m, 4);
    peel_close(a);
    peel_source_release(src);

    peel_err_t *err = NULL;
    peel_file_list_t list = peel(zip, zn, &err);
    ASSERT_TRUE(err == NULL);
    ASSERT_EQ_INT(3, list.count); // the empty file is not a file of the result
    for (int i = 0; i < list.count; i++) {
        const peel_file_t *f = &list.files[i];
        const zt_member_t *want = NULL;
        for (int j = 0; j < 4; j++)
            if (strcmp(m[j].name, f->meta.name) == 0)
                want = &m[j];
        ASSERT_TRUE(want != NULL);
        ASSERT_EQ_INT((int)want->len, (int)f->data_fork.size);
        ASSERT_TRUE(memcmp(want->data, f->data_fork.data, want->len) == 0);
    }
    peel_file_list_free(&list);
    free(zip);
    free(p);
}

// A self-extractor stub in front, its offsets left relative to the zip:
// the reader finds the delta from where the central directory really is.
TEST(test_zip_prefixed_stub_is_skipped) {
    size_t n = 20000;
    uint8_t *p = dz_payload(n);
    zt_member_t m[] = {
        {"a.bin", p,                         n, 8, 0},
        {"b.txt", (const uint8_t *)"stored", 6, 0, 0}
    };
    size_t zn;
    uint8_t *zip = zt_zip(m, 2, 12345, 0, &zn);
    peel_source_t *src;
    peel_archive_t *a = zt_open(zip, zn, &src);
    zt_check_members(a, m, 2);
    peel_close(a);
    peel_source_release(src);
    free(zip);
    free(p);
}

// Zip64: sizes and offsets in the extra field, the Zip64 end record and
// locator in front of the classic one.
TEST(test_zip64_records_are_read) {
    size_t n = 40000;
    uint8_t *p = dz_payload(n);
    zt_member_t m[] = {
        {"x/stored.bin", p, n, 0, 0},
        {"x/packed.bin", p, n, 8, 0}
    };
    size_t zn;
    uint8_t *zip = zt_zip(m, 2, 0, 1, &zn);
    peel_source_t *src;
    peel_archive_t *a = zt_open(zip, zn, &src);
    zt_check_members(a, m, 2);
    peel_close(a);
    peel_source_release(src);
    // And behind a stub.
    free(zip);
    zip = zt_zip(m, 2, 777, 1, &zn);
    a = zt_open(zip, zn, &src);
    zt_check_members(a, m, 2);
    peel_close(a);
    peel_source_release(src);
    free(zip);
    free(p);
}

// A deflated member whose CRC does not match is refused once read through.
TEST(test_zip_crc_mismatch_is_refused) {
    size_t n = 30000;
    uint8_t *p = dz_payload(n);
    zt_member_t m[] = {
        {"bad.bin", p, n, 8, 1}
    };
    size_t zn;
    uint8_t *zip = zt_zip(m, 1, 0, 0, &zn);
    peel_source_t *src;
    peel_archive_t *a = zt_open(zip, zn, &src);
    peel_err_t *err = NULL;
    peel_source_t *s = peel_open_fork(a, peel_lookup(a, "bad.bin"), PEEL_FORK_DATA, &err);
    ASSERT_TRUE(s != NULL);
    uint8_t *buf = malloc(n);
    ASSERT_TRUE(peel_source_read_exact(s, 0, buf, n) != 0);
    free(buf);
    peel_source_release(s);
    peel_close(a);
    peel_source_release(src);
    // The buffer API refuses it too.
    peel_file_list_t list = peel(zip, zn, &err);
    ASSERT_TRUE(err != NULL);
    peel_err_free(err);
    peel_file_list_free(&list);
    free(zip);
    free(p);
}

// Member names cannot climb out: "..", absolute paths and backslashes are
// made safe components.
TEST(test_zip_names_cannot_escape) {
    zt_member_t m[] = {
        {"../../etc/passwd",     (const uint8_t *)"x", 1, 0, 0},
        {"/abs/path.txt",        (const uint8_t *)"y", 1, 0, 0},
        {"dir\\..\\..\\win.txt", (const uint8_t *)"z", 1, 0, 0},
    };
    size_t zn;
    uint8_t *zip = zt_zip(m, 3, 0, 0, &zn);
    peel_source_t *src;
    peel_archive_t *a = zt_open(zip, zn, &src);
    int files = 0;
    for (int i = 0; i < peel_count(a); i++) {
        const peel_entry_t *e = peel_entry(a, i);
        ASSERT_TRUE(peel_path_is_confined(e->path));
        files += !e->is_dir;
    }
    ASSERT_EQ_INT(3, files);
    peel_close(a);
    peel_source_release(src);
    free(zip);
}

// The proposal's partial-access contract, on a 50 MB zip that is never
// materialised: opening it reads at most three spans, none in the members'
// data; opening a stored member reads its local header and nothing of its
// data; a read then touches exactly the range read.
TEST(test_zip_open_reads_only_the_directory) {
    const uint64_t big = 50ull * 1024 * 1024;
    zt_buf_t lh = {0}, tail = {0};
    // Local header of the big stored member.
    uint32_t crc = 0;
    {
        uint8_t chunk[65536];
        for (uint64_t off = 0; off < big; off += sizeof(chunk)) {
            size_t k = big - off < sizeof(chunk) ? (size_t)(big - off) : sizeof(chunk);
            for (size_t i = 0; i < k; i++)
                chunk[i] = cnt_synth(off + i);
            crc = peel_crc32(crc, chunk, k);
        }
    }
    zt_32(&lh, 0x04034b50);
    zt_16(&lh, 20);
    zt_16(&lh, 0);
    zt_16(&lh, 0);
    zt_16(&lh, 0);
    zt_16(&lh, 0x2A21);
    zt_32(&lh, crc);
    zt_32(&lh, (uint32_t)big);
    zt_32(&lh, (uint32_t)big);
    zt_16(&lh, 7);
    zt_16(&lh, 0);
    zt_bytes(&lh, "big.img", 7);
    uint64_t data_at = lh.n, cd_at = data_at + big;
    // Central directory and end record.
    zt_32(&tail, 0x02014b50);
    zt_16(&tail, 0x031E);
    zt_16(&tail, 20);
    zt_16(&tail, 0);
    zt_16(&tail, 0);
    zt_16(&tail, 0);
    zt_16(&tail, 0x2A21);
    zt_32(&tail, crc);
    zt_32(&tail, (uint32_t)big);
    zt_32(&tail, (uint32_t)big);
    zt_16(&tail, 7);
    for (int i = 0; i < 4; i++)
        zt_16(&tail, 0);
    zt_32(&tail, 0);
    zt_32(&tail, 0);
    zt_bytes(&tail, "big.img", 7);
    size_t cd_len = tail.n;
    zt_32(&tail, 0x06054b50);
    zt_16(&tail, 0);
    zt_16(&tail, 0);
    zt_16(&tail, 1);
    zt_16(&tail, 1);
    zt_32(&tail, (uint32_t)cd_len);
    zt_32(&tail, (uint32_t)cd_at);
    zt_16(&tail, 0);

    cnt_src_t c = {0};
    c.regions[0] = (cnt_region_t){0, lh.n, lh.buf};
    c.regions[1] = (cnt_region_t){data_at, big, NULL};
    c.regions[2] = (cnt_region_t){cd_at, tail.n, tail.buf};
    c.n_regions = 3;
    c.size = cd_at + tail.n;
    peel_source_t *src = peel_source_new(&cnt_ops, &c, NULL);
    peel_err_t *err = NULL;
    peel_archive_t *a = peel_open(src, NULL, NULL, &err);
    ASSERT_TRUE(a != NULL && err == NULL);
    ASSERT_TRUE(c.reads <= 3);
    // Neither the local header nor the member's bytes beyond the probe's
    // head and tail were read.
    ASSERT_TRUE(!cnt_touched(&c, 0, PEEL_DETECT_BUDGET, c.size - PEEL_DETECT_BUDGET));
    ASSERT_EQ_INT(1, peel_count(a));
    ASSERT_TRUE(peel_entry(a, 0)->data_len == big);

    int before = c.reads;
    peel_source_t *s = peel_open_fork(a, 0, PEEL_FORK_DATA, &err);
    ASSERT_TRUE(s != NULL && err == NULL);
    ASSERT_EQ_INT(PEEL_TIER_RANDOM, peel_source_tier(s));
    ASSERT_EQ_INT(before + 1, c.reads); // the local header...
    ASSERT_TRUE(!cnt_touched(&c, before, data_at, data_at + big)); // ...and none of the data

    before = c.reads;
    uint8_t buf[100];
    uint64_t at = 10ull * 1024 * 1024 + 17;
    ASSERT_EQ_INT(0, peel_source_read_exact(s, at, buf, sizeof(buf)));
    ASSERT_EQ_INT(before + 1, c.reads);
    ASSERT_TRUE(c.lo[before] == data_at + at && c.hi[before] == data_at + at + sizeof(buf));
    for (size_t i = 0; i < sizeof(buf); i++)
        ASSERT_EQ_INT(cnt_synth(at + i), buf[i]);
    peel_source_release(s);
    peel_close(a);
    peel_source_release(src);
    free(lh.buf);
    free(tail.buf);
}

// ============================================================================
// Gzip
// ============================================================================

// Open the one entry of a gzip held in memory, as a source.
static peel_source_t *zt_gz_fork(const uint8_t *gz, size_t n, const char *key, peel_archive_t **a, peel_source_t **src,
                                 const peel_entry_t **e) {
    *src = peel_source_memory_keyed(gz, n, false, key);
    peel_err_t *err = NULL;
    *a = peel_open(*src, NULL, NULL, &err);
    ASSERT_TRUE(*a != NULL && err == NULL);
    ASSERT_TRUE(strcmp(peel_format(*a), "gz") == 0);
    ASSERT_TRUE(peel_is_wrapper(*a));
    ASSERT_EQ_INT(1, peel_count(*a));
    *e = peel_entry(*a, 0);
    peel_source_t *s = peel_open_fork(*a, 0, PEEL_FORK_DATA, &err);
    ASSERT_TRUE(s != NULL && err == NULL);
    return s;
}

// Several members: the tail's ISIZE names only the last, so the listing's
// length is a hint; the fork's source earns the true one, and reads made
// before that (forwards and back) are served as decoding reaches them.
TEST(test_gzip_multi_member_size_is_earned) {
    size_t n = 70000;
    uint8_t *p = dz_payload(n);
    zt_buf_t b = {0};
    zt_gzip_member(&b, p, 50000, "joined.txt", 0, 0);
    zt_gzip_member(&b, p + 50000, 19990, NULL, 0, 0);
    zt_gzip_member(&b, p + 69990, 10, NULL, 0, 0);
    peel_archive_t *a;
    peel_source_t *src;
    const peel_entry_t *e;
    peel_source_t *s = zt_gz_fork(b.buf, b.n, "x.gz", &a, &src, &e);
    ASSERT_TRUE(strcmp(e->path, "joined.txt") == 0); // FNAME of the first member
    ASSERT_EQ_INT(10, (int)e->data_len); // the hint: the last member's ISIZE
    ASSERT_EQ_INT(PEEL_TIER_EARNED, e->data_tier);
    uint8_t mid[300];
    ASSERT_EQ_INT(0, peel_source_read_exact(s, 60000, mid, sizeof(mid))); // into the second member
    ASSERT_TRUE(memcmp(mid, p + 60000, sizeof(mid)) == 0);
    ASSERT_EQ_INT((int)n, (int)peel_source_size(s)); // earned
    zt_read_backwards(s, p, n, 9999);
    peel_source_release(s);
    peel_close(a);
    peel_source_release(src);
    // peel() agrees.
    peel_err_t *err = NULL;
    peel_file_list_t list = peel(b.buf, b.n, &err);
    ASSERT_TRUE(err == NULL);
    ASSERT_EQ_INT(1, list.count);
    ASSERT_TRUE(strcmp(list.files[0].meta.name, "joined.txt") == 0);
    ASSERT_EQ_INT((int)n, (int)list.files[0].data_fork.size);
    ASSERT_TRUE(memcmp(list.files[0].data_fork.data, p, n) == 0);
    peel_file_list_free(&list);
    free(b.buf);
    free(p);
}

// No stored name: the payload is named after the file, less ".gz".
TEST(test_gzip_unnamed_takes_the_file_name) {
    zt_buf_t b = {0};
    zt_gzip_member(&b, (const uint8_t *)"abc", 3, NULL, 0, 0);
    peel_archive_t *a;
    peel_source_t *src;
    const peel_entry_t *e;
    peel_source_t *s = zt_gz_fork(b.buf, b.n, "/some/dir/disk.img.gz", &a, &src, &e);
    ASSERT_TRUE(strcmp(e->path, "disk.img") == 0);
    peel_source_release(s);
    peel_close(a);
    peel_source_release(src);
    free(b.buf);
}

// A stored name cannot climb out either.
TEST(test_gzip_name_cannot_escape) {
    zt_buf_t b = {0};
    zt_gzip_member(&b, (const uint8_t *)"abc", 3, "../../evil", 0, 0);
    peel_archive_t *a;
    peel_source_t *src;
    const peel_entry_t *e;
    peel_source_t *s = zt_gz_fork(b.buf, b.n, "x.gz", &a, &src, &e);
    ASSERT_TRUE(peel_path_is_confined(e->path));
    peel_source_release(s);
    peel_close(a);
    peel_source_release(src);
    free(b.buf);
}

// A member whose CRC does not match is refused.
TEST(test_gzip_crc_mismatch_is_refused) {
    size_t n = 20000;
    uint8_t *p = dz_payload(n);
    zt_buf_t b = {0};
    zt_gzip_member(&b, p, n, NULL, 0, 1);
    peel_archive_t *a;
    peel_source_t *src;
    const peel_entry_t *e;
    peel_source_t *s = zt_gz_fork(b.buf, b.n, "x.gz", &a, &src, &e);
    uint8_t *buf = malloc(n);
    ASSERT_TRUE(peel_source_read_exact(s, 0, buf, n) != 0);
    free(buf);
    peel_source_release(s);
    peel_close(a);
    peel_source_release(src);
    free(b.buf);
    free(p);
}

// BGZF: the block index makes it INDEXED, with an exact size, and a read
// inflates only the block it lands in.
TEST(test_bgzf_reads_one_block) {
    size_t n = 200000, blk = 0xFF00;
    uint8_t *p = dz_payload(n);
    zt_buf_t b = {0};
    uint64_t starts[8];
    int nb = 0;
    for (size_t off = 0; off < n; off += blk) {
        starts[nb++] = b.n;
        zt_gzip_member(&b, p + off, n - off < blk ? n - off : blk, NULL, 1, 0);
    }
    starts[nb] = b.n;
    zt_gzip_member(&b, NULL, 0, NULL, 1, 0); // the EOF block

    cnt_src_t c = {0};
    c.regions[0] = (cnt_region_t){0, b.n, b.buf};
    c.n_regions = 1;
    c.size = b.n;
    peel_source_t *src = peel_source_new(&cnt_ops, &c, NULL);
    peel_err_t *err = NULL;
    peel_archive_t *a = peel_open(src, NULL, NULL, &err);
    ASSERT_TRUE(a != NULL && err == NULL);
    const peel_entry_t *e = peel_entry(a, 0);
    ASSERT_EQ_INT(PEEL_TIER_INDEXED, e->data_tier);
    ASSERT_EQ_INT((int)n, (int)e->data_len);
    peel_source_t *s = peel_open_fork(a, 0, PEEL_FORK_DATA, &err);
    ASSERT_TRUE(s != NULL && err == NULL);
    ASSERT_EQ_INT((int)n, (int)peel_source_size(s));
    int before = c.reads;
    uint8_t buf[64];
    uint64_t at = 2 * blk + 100; // in the third block
    ASSERT_EQ_INT(0, peel_source_read_exact(s, at, buf, sizeof(buf)));
    ASSERT_TRUE(memcmp(buf, p + at, sizeof(buf)) == 0);
    ASSERT_EQ_INT(before + 1, c.reads);
    ASSERT_TRUE(c.lo[before] == starts[2] && c.hi[before] == starts[3]);
    zt_read_backwards(s, p, n, 30001);
    peel_source_release(s);
    peel_close(a);
    peel_source_release(src);
    free(b.buf);
    free(p);
}

// ============================================================================
// Tar
// ============================================================================

// One 512-byte header: `name` (a ustar prefix split off when it is longer
// than 100), `type`, `size`, `link`; magic "ustar\0" "00" unless `gnu`.
static void tt_header(zt_buf_t *b, const char *name, char type, uint64_t size, const char *link, int gnu) {
    uint8_t h[512];
    memset(h, 0, sizeof(h));
    size_t nl = strlen(name);
    if (nl > 100) {
        const char *cut = name + nl - 100;
        cut = strchr(cut, '/');
        ASSERT_TRUE(cut && (size_t)(cut - name) <= 155);
        memcpy(h + 345, name, (size_t)(cut - name));
        memcpy(h, cut + 1, strlen(cut + 1));
    } else {
        memcpy(h, name, nl);
    }
    snprintf((char *)h + 100, 8, "%07o", 0644);
    snprintf((char *)h + 108, 8, "%07o", 0);
    snprintf((char *)h + 116, 8, "%07o", 0);
    snprintf((char *)h + 124, 12, "%011llo", (unsigned long long)size);
    snprintf((char *)h + 136, 12, "%011o", 981173106u);
    h[156] = (uint8_t)type;
    if (link)
        memcpy(h + 157, link, strlen(link));
    if (gnu) {
        memcpy(h + 257, "ustar  ", 8);
    } else {
        memcpy(h + 257, "ustar", 6);
        memcpy(h + 263, "00", 2);
    }
    memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++)
        sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum);
    h[154] = 0;
    h[155] = ' ';
    zt_bytes(b, h, sizeof(h));
}

// A member: its header, then its bytes padded to 512.
static void tt_member(zt_buf_t *b, const char *name, char type, const uint8_t *data, size_t len, const char *link) {
    tt_header(b, name, type, len, link, 0);
    if (len)
        zt_bytes(b, data, len);
    static const uint8_t zero[512];
    if (len % 512)
        zt_bytes(b, zero, 512 - len % 512);
}

static void tt_end(zt_buf_t *b) {
    static const uint8_t zero[1024];
    zt_bytes(b, zero, sizeof(zero));
}

// Open a tar held in memory.
static peel_archive_t *tt_open(const uint8_t *t, size_t n, peel_source_t **src) {
    *src = peel_source_memory_keyed(t, n, false, "t.tar");
    peel_err_t *err = NULL;
    peel_archive_t *a = peel_open(*src, NULL, NULL, &err);
    if (err)
        fprintf(stderr, "peel_open: %s\n", peel_err_msg(err));
    ASSERT_TRUE(a != NULL && err == NULL);
    ASSERT_TRUE(strcmp(peel_format(a), "tar") == 0);
    return a;
}

// The data fork of `path`, read whole.
static void tt_check(peel_archive_t *a, const char *path, const uint8_t *want, size_t len) {
    int i = peel_lookup(a, path);
    if (i < 0)
        fprintf(stderr, "no member %s\n", path);
    ASSERT_TRUE(i >= 0);
    const peel_entry_t *e = peel_entry(a, i);
    ASSERT_EQ_INT((int)len, (int)e->data_len);
    ASSERT_EQ_INT(PEEL_TIER_RANDOM, e->data_tier);
    peel_err_t *err = NULL;
    peel_source_t *s = peel_open_fork(a, i, PEEL_FORK_DATA, &err);
    ASSERT_TRUE(s != NULL && err == NULL);
    zt_read_backwards(s, want, len, 1000);
    peel_source_release(s);
}

// Members are views: opening the archive reads its headers and none of
// its members' bytes; a read of a member reads just that range.
TEST(test_tar_open_reads_headers_only) {
    size_t n = 300000;
    uint8_t *p = dz_payload(n);
    zt_buf_t b = {0};
    tt_member(&b, "a.bin", '0', p, 100000, NULL);
    tt_member(&b, "dir/b.bin", '0', p + 100000, 100000, NULL);
    tt_member(&b, "dir/c.bin", '0', p + 200000, 100000, NULL);
    tt_end(&b);
    cnt_src_t c = {0};
    c.regions[0] = (cnt_region_t){0, b.n, b.buf};
    c.n_regions = 1;
    c.size = b.n;
    peel_source_t *src = peel_source_new(&cnt_ops, &c, NULL);
    peel_err_t *err = NULL;
    peel_archive_t *a = peel_open(src, NULL, NULL, &err);
    ASSERT_TRUE(a != NULL && err == NULL);
    // Beyond the probe's head and tail (reads 0 and 1), only the headers
    // (at 0, 100864 and 201728) and the end block (302592) were read --
    // 512 bytes each, never a member's bytes.
    uint64_t hdrs[] = {0, 100864, 201728, 302592};
    ASSERT_EQ_INT(2 + 4, c.reads);
    for (int i = 2; i < c.reads; i++) {
        bool header = false;
        for (int k = 0; k < 4; k++)
            header |= c.lo[i] == hdrs[k] && c.hi[i] == hdrs[k] + 512;
        ASSERT_TRUE(header);
    }
    ASSERT_TRUE(peel_lookup(a, "dir") >= 0 && peel_entry(a, peel_lookup(a, "dir"))->is_dir); // synthesised
    int i = peel_lookup(a, "dir/b.bin");
    peel_source_t *s = peel_open_fork(a, i, PEEL_FORK_DATA, &err);
    int before = c.reads;
    uint8_t buf[64];
    ASSERT_EQ_INT(0, peel_source_read_exact(s, 5000, buf, sizeof(buf)));
    ASSERT_EQ_INT(before + 1, c.reads);
    ASSERT_TRUE(c.lo[before] == 100864 + 512 + 5000 && c.hi[before] == 100864 + 512 + 5000 + sizeof(buf));
    ASSERT_TRUE(memcmp(buf, p + 100000 + 5000, sizeof(buf)) == 0);
    peel_source_release(s);
    peel_close(a);
    peel_source_release(src);
    free(b.buf);
    free(p);
}

// Long names three ways (the ustar prefix, a GNU 'L' record, a pax path),
// a hard link to an earlier member, one to a member that is not there
// (dropped), a symbolic link (skipped), and a pax size.
TEST(test_tar_names_links_and_extensions) {
    const uint8_t data[] = "hello, tar\n";
    char longp[200], gnu_name[300], pax_name[300], rec[400];
    snprintf(longp, sizeof(longp), "%s/file.txt", "prefixed-folder-name/second-level-folder/third-level-folder-xx");
    memset(gnu_name, 'g', 150);
    snprintf(gnu_name + 150, 150, "/gnu.txt");
    memset(pax_name, 'p', 150);
    snprintf(pax_name + 150, 150, "/pax.txt");
    zt_buf_t b = {0};
    tt_member(&b, longp, '0', data, sizeof(data) - 1, NULL);
    tt_header(&b, "././@LongLink", 'L', strlen(gnu_name) + 1, NULL, 1);
    {
        uint8_t blk[512] = {0};
        memcpy(blk, gnu_name, strlen(gnu_name));
        zt_bytes(&b, blk, sizeof(blk));
    }
    tt_member(&b, "truncated-gnu-name", '0', data, sizeof(data) - 1, NULL);
    size_t body = strlen(pax_name) + 6;
    char lenstr[24];
    snprintf(lenstr, sizeof(lenstr), "%zu", body + 1);
    if (strlen(lenstr) + body + 1 != (size_t)atoi(lenstr))
        snprintf(lenstr, sizeof(lenstr), "%zu", body + strlen(lenstr) + 1);
    snprintf(rec, sizeof(rec), "%s path=%s\n", lenstr, pax_name);
    tt_member(&b, "PaxHeader", 'x', (const uint8_t *)rec, strlen(rec), NULL);
    tt_member(&b, "truncated-pax-name", '0', data, sizeof(data) - 1, NULL);
    tt_member(&b, "link.txt", '1', NULL, 0, longp);
    tt_member(&b, "orphan.txt", '1', NULL, 0, "not/here.txt");
    tt_member(&b, "sym.txt", '2', NULL, 0, "link.txt");
    tt_end(&b);
    peel_source_t *src;
    peel_archive_t *a = tt_open(b.buf, b.n, &src);
    tt_check(a, longp, data, sizeof(data) - 1);
    tt_check(a, gnu_name, data, sizeof(data) - 1);
    tt_check(a, pax_name, data, sizeof(data) - 1);
    tt_check(a, "link.txt", data, sizeof(data) - 1);
    ASSERT_EQ_INT(-1, peel_lookup(a, "orphan.txt"));
    ASSERT_EQ_INT(-1, peel_lookup(a, "sym.txt"));
    ASSERT_EQ_INT(-1, peel_lookup(a, "truncated-gnu-name"));
    peel_close(a);
    peel_source_release(src);
    free(b.buf);
}

// Names cannot climb out, and a bad first header or a member running past
// the archive's end is refused.
TEST(test_tar_refuses_what_it_cannot_trust) {
    const uint8_t data[] = "x";
    zt_buf_t b = {0};
    tt_member(&b, "../../etc/passwd", '0', data, 1, NULL);
    tt_member(&b, "/abs/path.txt", '0', data, 1, NULL);
    tt_member(&b, "a/./b/../c.txt", '0', data, 1, NULL);
    tt_end(&b);
    peel_source_t *src;
    peel_archive_t *a = tt_open(b.buf, b.n, &src);
    int files = 0;
    for (int i = 0; i < peel_count(a); i++) {
        ASSERT_TRUE(peel_path_is_confined(peel_entry(a, i)->path));
        files += !peel_entry(a, i)->is_dir;
    }
    ASSERT_EQ_INT(3, files);
    peel_close(a);
    peel_source_release(src);

    b.buf[148] ^= 1; // the first header's checksum
    src = peel_source_memory(b.buf, b.n, false);
    peel_err_t *err = NULL;
    ASSERT_TRUE(peel_open(src, NULL, NULL, &err) == NULL);
    peel_err_free(err);
    peel_source_release(src);
    free(b.buf);

    memset(&b, 0, sizeof(b));
    tt_header(&b, "big.bin", '0', 1u << 20, NULL, 0); // claims 1 MiB, holds 512 bytes
    static const uint8_t blk[512];
    zt_bytes(&b, blk, sizeof(blk));
    src = peel_source_memory(b.buf, b.n, false);
    ASSERT_TRUE(peel_open(src, NULL, NULL, &err) == NULL);
    peel_err_free(err);
    peel_source_release(src);
    free(b.buf);
}

// macOS's "._<name>" companion folds into its file: the resource fork a
// view of the companion's bytes, the Finder info on the entry, and the
// companion itself not listed.
TEST(test_tar_folds_macos_companions) {
    uint8_t rsrc[700];
    for (size_t i = 0; i < sizeof(rsrc); i++)
        rsrc[i] = (uint8_t)(i * 3);
    uint8_t finfo[32] = {'T', 'E', 'X', 'T', 'R', '*', 'c', 'h', 0x01, 0x00};
    // AppleDouble: header, two descriptors (Finder info, resource fork).
    zt_buf_t ad = {0};
    uint8_t hdr[26 + 24] = {0x00, 0x05, 0x16, 0x07, 0x00, 0x02, 0x00, 0x00};
    hdr[25] = 2;
    uint32_t off = 26 + 24;
    uint8_t *d = hdr + 26;
    d[3] = 9;
    d[4] = (uint8_t)(off >> 24), d[5] = (uint8_t)(off >> 16), d[6] = (uint8_t)(off >> 8), d[7] = (uint8_t)off;
    d[11] = 32;
    d[15] = 2;
    off += 32;
    d[16] = (uint8_t)(off >> 24), d[17] = (uint8_t)(off >> 16), d[18] = (uint8_t)(off >> 8), d[19] = (uint8_t)off;
    d[22] = (uint8_t)(sizeof(rsrc) >> 8), d[23] = (uint8_t)sizeof(rsrc);
    zt_bytes(&ad, hdr, sizeof(hdr));
    zt_bytes(&ad, finfo, sizeof(finfo));
    zt_bytes(&ad, rsrc, sizeof(rsrc));
    zt_buf_t b = {0};
    tt_member(&b, "docs/._notes.txt", '0', ad.buf, ad.n, NULL);
    tt_member(&b, "docs/notes.txt", '0', (const uint8_t *)"notes", 5, NULL);
    tt_end(&b);
    peel_source_t *src;
    peel_archive_t *a = tt_open(b.buf, b.n, &src);
    ASSERT_EQ_INT(-1, peel_lookup(a, "docs/._notes.txt"));
    int i = peel_lookup(a, "docs/notes.txt");
    ASSERT_TRUE(i >= 0);
    const peel_entry_t *e = peel_entry(a, i);
    ASSERT_TRUE(e->mac_type == 0x54455854u && e->mac_creator == 0x522A6368u); // 'TEXT' 'R*ch'
    ASSERT_EQ_INT((int)sizeof(rsrc), (int)e->rsrc_len);
    ASSERT_EQ_INT(PEEL_TIER_RANDOM, e->rsrc_tier);
    peel_err_t *err = NULL;
    peel_source_t *r = peel_open_fork(a, i, PEEL_FORK_RSRC, &err);
    ASSERT_TRUE(r != NULL && err == NULL);
    zt_read_backwards(r, rsrc, sizeof(rsrc), 99);
    peel_source_release(r);
    peel_close(a);
    peel_source_release(src);
    free(ad.buf);
    free(b.buf);
}

void zipgz_tests(void) {
    RUN(test_inflate_every_block_type_round_trip);
    RUN(test_inflate_resumes_at_every_output_boundary);
    RUN(test_inflate_truncated_stream_is_refused);
    RUN(test_inflate_distance_before_start_is_refused);
    RUN(test_inflate_oversubscribed_code_is_refused);
    RUN(test_zlib_wrapper_checks_its_trailer);
    RUN(test_zip_round_trip);
    RUN(test_zip_prefixed_stub_is_skipped);
    RUN(test_zip64_records_are_read);
    RUN(test_zip_crc_mismatch_is_refused);
    RUN(test_zip_names_cannot_escape);
    RUN(test_zip_open_reads_only_the_directory);
    RUN(test_gzip_multi_member_size_is_earned);
    RUN(test_gzip_unnamed_takes_the_file_name);
    RUN(test_gzip_name_cannot_escape);
    RUN(test_gzip_crc_mismatch_is_refused);
    RUN(test_bgzf_reads_one_block);
    RUN(test_tar_open_reads_headers_only);
    RUN(test_tar_names_links_and_extensions);
    RUN(test_tar_refuses_what_it_cannot_trust);
    RUN(test_tar_folds_macos_companions);
}
