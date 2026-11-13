// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// inflate.c
// DEFLATE (RFC 1951), zlib (RFC 1950) and gzip (RFC 1952) decoders.  See
// codec.h.
//
// A DEFLATE stream is a run of blocks, each stored (a byte count and the
// bytes), or Huffman-coded with the fixed codes or with codes defined at
// its start (code lengths, themselves Huffman-coded).  A coded block is
// literals and (length, distance) back-references into the last 32 KiB,
// ended by symbol 256.  A zlib stream wraps one DEFLATE stream in a 2-byte
// header and the Adler-32 of the decoded bytes.  A gzip stream is one or
// more members, each a header (with optional extra field, name, comment and
// header CRC), a DEFLATE stream, and the CRC-32 and size (mod 2^32) of its
// bytes; whatever follows the last member that is not another one is
// ignored, as gzip -d ignores it.
//
// Decoding a symbol is one table lookup on the next 10 bits (8 for
// distances); a longer code continues in a subtable reached from its
// prefix's entry.  An entry carries what the symbol means -- a literal, a
// length or distance base with its extra-bit count, the end of the block --
// so the hot loop decodes a whole literal or match with two lookups and
// one refill of the bit accumulator, all in locals.

#include "codec_internal.h"

#include <stdlib.h>

// ============================================================================
// Constants
// ============================================================================

#define INF_WINDOW    32768 // RFC 1951 § 2: back-references reach 32 KiB
#define INF_MATCH_MAX 258 // the longest match: the most one symbol writes
#define LIT_BITS      10 // literal/length primary table index width
#define DIST_BITS     8 // distance primary table index width
#define LIT_TABLE     (1024 + 286 * 32) // primary + subtables, at worst
#define DIST_TABLE    (256 + 30 * 128)

// RFC 1951 § 3.2.5 length and distance code tables, shared with deflate.c.
const uint16_t codec_deflate_len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                             31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t codec_deflate_len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                             2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t codec_deflate_dist_base[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                              33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                              1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t codec_deflate_dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                              6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// ============================================================================
// Types
// ============================================================================

// A table entry (uint32_t):
//   bits 0-4    the bits it consumes (for a subtable link: the primary width)
//   bits 5-7    its kind (E_*)
//   bits 8-11   extra bits after it (a length or distance), or a subtable's width
//   bits 16-31  its value: a literal, a length or distance base, or a subtable's offset
#define E_LIT     0u
#define E_LEN     1u // a length (literal/length table) or a distance (distance table)
#define E_END     2u
#define E_SUB     3u
#define E_BAD     4u // a code no symbol has (an incomplete code)
#define E_BITS(e) ((e) & 31u)
#define E_KIND(e) (((e) >> 5) & 7u)
#define E_X(e)    (((e) >> 8) & 15u)
#define E_VAL(e)  ((e) >> 16)

static inline uint32_t mk_entry(uint32_t bits, uint32_t kind, uint32_t x, uint32_t val) {
    return bits | kind << 5 | x << 8 | val << 16;
}

#define GZ_FTEXT    0x01
#define GZ_FHCRC    0x02
#define GZ_FEXTRA   0x04
#define GZ_FNAME    0x08
#define GZ_FCOMMENT 0x10
#define GZ_RESERVED 0xE0

typedef enum { W_RAW, W_ZLIB, W_GZIP } inf_wrap_t;

typedef enum { S_ZLIB_HEADER, S_GZ_HEADER, S_BLOCK, S_STORED, S_HUFF, S_TRAILER, S_GZ_NEXT, S_DONE } inf_state_t;

typedef struct {
    inf_state_t state;
    inf_wrap_t wrap; // the container around the DEFLATE stream
    bool last; // the current block is the final one
    uint32_t stored_left; // bytes left in a stored block
    uint32_t sum; // Adler-32 (zlib) or CRC-32 (gzip) of the member's bytes so far
    uint64_t sum_at; // ring.total the sum covers
    uint64_t member_at; // ring.total where the current gzip member began
    bool magic_read; // the next member's 1F 8B has been read already
    uint32_t lit[LIT_TABLE];
    uint32_t dist[DIST_TABLE];
} inf_t;

// ============================================================================
// Huffman tables
// ============================================================================

// Reverse the low `n` bits of `v` (codes are defined MSB first but read
// from an LSB-first stream).
static uint32_t rev_bits(uint32_t v, int n) {
    uint32_t r = 0;
    while (n--) {
        r = r << 1 | (v & 1);
        v >>= 1;
    }
    return r;
}

// The alphabets a table can be for.
typedef enum { T_LIT, T_DIST, T_RAW } table_kind_t;

// What symbol `s` of a table means: a literal/length (T_LIT), a distance
// (T_DIST), or just the symbol (T_RAW, the code-length alphabet: E_LIT).
static uint32_t sym_entry(int s, table_kind_t kind, uint32_t bits) {
    if (kind == T_RAW)
        return mk_entry(bits, E_LIT, 0, (uint32_t)s);
    if (kind == T_LIT) {
        if (s < 256)
            return mk_entry(bits, E_LIT, 0, (uint32_t)s);
        if (s == 256)
            return mk_entry(bits, E_END, 0, 0);
        if (s - 257 < 29)
            return mk_entry(bits, E_LEN, codec_deflate_len_extra[s - 257], codec_deflate_len_base[s - 257]);
        return mk_entry(bits, E_BAD, 0, 0); // 286, 287: never valid
    }
    if (s < 30)
        return mk_entry(bits, E_LEN, codec_deflate_dist_extra[s], codec_deflate_dist_base[s]);
    return mk_entry(bits, E_BAD, 0, 0);
}

// Build a two-level table for `n` symbols with code lengths `len` (0:
// unused), primary width `root`, into t[0 .. cap).  0, or -1 for an
// over-subscribed set or one that does not fit.
static int build_table(uint32_t *t, size_t cap, const uint8_t *len, int n, int root, table_kind_t lit) {
    uint16_t count[16] = {0}, offs[16];
    uint16_t sorted[288];
    for (int i = 0; i < n; i++)
        count[len[i]]++;
    count[0] = 0;
    int left = 1;
    for (int l = 1; l < 16; l++) {
        left = (left << 1) - count[l];
        if (left < 0)
            return -1; // over-subscribed (an incomplete code is legal)
    }
    offs[1] = 0;
    for (int l = 1; l < 15; l++)
        offs[l + 1] = (uint16_t)(offs[l] + count[l]);
    for (int i = 0; i < n; i++)
        if (len[i])
            sorted[offs[len[i]]++] = (uint16_t)i;

    size_t size = (size_t)1 << root;
    for (size_t i = 0; i < size; i++)
        t[i] = mk_entry(0, E_BAD, 0, 0);
    size_t next = size; // the next free subtable slot
    uint32_t code = 0; // the canonical code of the next symbol
    int idx = 0, sub_root = -1; // sorted index; the primary entry of the open subtable
    size_t sub_off = 0;
    int sub_bits = 0;
    for (int l = 1; l <= 15; l++, code <<= 1) {
        for (int k = 0; k < count[l]; k++, idx++, code++) {
            uint32_t r = rev_bits(code, l);
            if (l <= root) {
                uint32_t e = sym_entry(sorted[idx], lit, (uint32_t)l);
                for (size_t f = r; f < size; f += (size_t)1 << l)
                    t[f] = e;
                continue;
            }
            int prefix = (int)(r & (size - 1));
            if (prefix != sub_root) {
                // A new subtable: wide enough for every code sharing this
                // prefix, counted from the lengths still to come.
                int w = l - root, room = 1 << w;
                for (int ll = l; ll < 15; ll++) {
                    room -= (ll == l ? count[l] - k : count[ll]);
                    if (room <= 0)
                        break;
                    w++;
                    room <<= 1;
                }
                if (next + ((size_t)1 << w) > cap)
                    return -1;
                sub_root = prefix;
                sub_off = next;
                sub_bits = w;
                for (size_t i = 0; i < (size_t)1 << w; i++)
                    t[next + i] = mk_entry(0, E_BAD, 0, 0);
                next += (size_t)1 << w;
                t[prefix] = mk_entry((uint32_t)root, E_SUB, (uint32_t)w, (uint32_t)sub_off);
            }
            uint32_t e = sym_entry(sorted[idx], lit, (uint32_t)(l - root));
            for (size_t f = r >> root; f < (size_t)1 << sub_bits; f += (size_t)1 << (l - root))
                t[sub_off + f] = e;
        }
    }
    return 0;
}

// The fixed codes of RFC 1951 § 3.2.6.
static void fixed_tables(inf_t *z) {
    uint8_t ll[288], dl[30];
    for (int i = 0; i < 288; i++)
        ll[i] = (uint8_t)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
    memset(dl, 5, sizeof(dl));
    build_table(z->lit, LIT_TABLE, ll, 288, LIT_BITS, T_LIT);
    build_table(z->dist, DIST_TABLE, dl, 30, DIST_BITS, T_DIST);
}

// Decode one symbol through table `t` (root width `root`); the accumulator
// holds at least 15 bits.  Returns the entry, its bits consumed.
static inline uint32_t table_decode(const uint32_t *t, int root, codec_in_t *in) {
    uint32_t e = t[lsb_peek(in, root)];
    if (E_KIND(e) == E_SUB) {
        lsb_drop(in, root);
        e = t[E_VAL(e) + lsb_peek(in, (int)E_X(e))];
    }
    lsb_drop(in, (int)E_BITS(e));
    return e;
}

// A dynamic block's code definitions (RFC 1951 § 3.2.7).  0 or CODEC_ERR_*.
static int dynamic_tables(codec_dec_t *d, inf_t *z) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    codec_in_t *in = &d->in;
    lsb_fill(in);
    int hlit = (int)lsb_get(in, 5) + 257, hdist = (int)lsb_get(in, 5) + 1, hclen = (int)lsb_get(in, 4) + 4;
    if (hlit > 286 || hdist > 30)
        return codec_fail(d, CODEC_ERR_DATA, "deflate: bad code counts");
    uint8_t cl[19] = {0};
    for (int i = 0; i < hclen; i++)
        cl[order[i]] = (uint8_t)lsb_get(in, 3);
    uint32_t clt[128 + 19 * 16];
    if (build_table(clt, sizeof(clt) / sizeof(clt[0]), cl, 19, 7, T_RAW) != 0)
        return codec_fail(d, CODEC_ERR_DATA, "deflate: bad code-length code");
    uint8_t lengths[286 + 30] = {0};
    int n = 0, total = hlit + hdist;
    while (n < total) {
        lsb_fill(in);
        uint32_t e = table_decode(clt, 7, in);
        if (E_KIND(e) != E_LIT)
            return codec_fail(d, CODEC_ERR_DATA, "deflate: bad code length");
        int sym = (int)E_VAL(e);
        if (sym < 16) {
            lengths[n++] = (uint8_t)sym;
            continue;
        }
        int rep;
        uint8_t val = 0;
        if (sym == 16) {
            if (n == 0)
                return codec_fail(d, CODEC_ERR_DATA, "deflate: repeat with no length before it");
            val = lengths[n - 1];
            rep = 3 + (int)lsb_get(in, 2);
        } else if (sym == 17)
            rep = 3 + (int)lsb_get(in, 3);
        else
            rep = 11 + (int)lsb_get(in, 7);
        if (n + rep > total)
            return codec_fail(d, CODEC_ERR_DATA, "deflate: code lengths overrun");
        memset(lengths + n, val, (size_t)rep);
        n += rep;
    }
    if (codec_in_overrun(in))
        return codec_fail(d, CODEC_ERR_TRUNCATED, "deflate: truncated block header");
    if (lengths[256] == 0)
        return codec_fail(d, CODEC_ERR_DATA, "deflate: no end-of-block code");
    if (build_table(z->lit, LIT_TABLE, lengths, hlit, LIT_BITS, T_LIT) != 0 ||
        build_table(z->dist, DIST_TABLE, lengths + hlit, hdist, DIST_BITS, T_DIST) != 0)
        return codec_fail(d, CODEC_ERR_DATA, "deflate: over-subscribed code");
    return 0;
}

// ============================================================================
// The decoder
// ============================================================================

// Start the next block.  0 or CODEC_ERR_*.
static int block_header(codec_dec_t *d, inf_t *z) {
    codec_in_t *in = &d->in;
    lsb_fill(in);
    z->last = lsb_get(in, 1) != 0;
    uint32_t type = lsb_get(in, 2);
    if (codec_in_overrun(in))
        return codec_fail(d, CODEC_ERR_TRUNCATED, "deflate: truncated stream");
    if (type == 0) {
        // Stored: to a byte boundary, then LEN and its complement.
        lsb_align(in);
        lsb_fill(in);
        uint32_t len = lsb_get(in, 16), nlen = lsb_get(in, 16);
        if (codec_in_overrun(in))
            return codec_fail(d, CODEC_ERR_TRUNCATED, "deflate: truncated stored block");
        if ((len ^ 0xFFFF) != nlen)
            return codec_fail(d, CODEC_ERR_DATA, "deflate: stored block length check");
        z->stored_left = len;
        z->state = S_STORED;
        return 0;
    }
    if (type == 1)
        fixed_tables(z);
    else if (type == 2) {
        int rc = dynamic_tables(d, z);
        if (rc)
            return rc;
    } else
        return codec_fail(d, CODEC_ERR_DATA, "deflate: reserved block type");
    z->state = S_HUFF;
    return 0;
}

// Decode a coded block's symbols while the ring has room for a match.
// The accumulator and the ring's cursor live in locals here (stores to the
// ring cannot alias them), written back on every way out.
static int huff_run(codec_dec_t *d, inf_t *z) {
    codec_in_t *in = &d->in;
    codec_ring_t *r = &d->ring;
    uint8_t *buf = r->buf;
    const size_t size = r->size;
    size_t w = r->w;
    uint64_t total = r->total, stop = r->drained + size - INF_MATCH_MAX; // write while total <= stop
    uint64_t bits = in->bits;
    int nbits = in->nbits;
    bool tail = false; // the input is nearly out: check every symbol for overrun
    int rc = 0;

#define SYNC_OUT()                                                                                                     \
    do {                                                                                                               \
        in->bits = bits;                                                                                               \
        in->nbits = nbits;                                                                                             \
        r->w = w;                                                                                                      \
        r->total = total;                                                                                              \
    } while (0)
#define SYNC_IN()                                                                                                      \
    do {                                                                                                               \
        bits = in->bits;                                                                                               \
        nbits = in->nbits;                                                                                             \
    } while (0)

    while (total <= stop) {
        // Refill: 8 bytes in one load where the input allows.
        if (nbits <= 56) {
            if (in->len - in->pos >= 8) {
                uint64_t v = codec_le64(in->p + in->pos);
                bits |= v << nbits;
                int k = (63 - nbits) >> 3;
                in->pos += (size_t)k;
                nbits += k * 8;
            } else {
                SYNC_OUT();
                lsb_fill(in);
                SYNC_IN();
                tail = in->over > 0;
            }
        }
        uint32_t e = z->lit[bits & ((1u << LIT_BITS) - 1)];
        if (E_KIND(e) == E_SUB) {
            bits >>= LIT_BITS;
            nbits -= LIT_BITS;
            e = z->lit[E_VAL(e) + (bits & ((1u << E_X(e)) - 1))];
        }
        bits >>= E_BITS(e);
        nbits -= (int)E_BITS(e);
        uint32_t kind = E_KIND(e);
        if (tail && nbits < (int)in->over * 8) {
            rc = codec_fail(d, CODEC_ERR_TRUNCATED, "deflate: truncated stream");
            break;
        }
        if (kind == E_LIT) {
            buf[w] = (uint8_t)E_VAL(e);
            if (++w == size)
                w = 0;
            total++;
            continue;
        }
        if (kind != E_LEN) {
            if (kind == E_END)
                z->state = z->last ? (z->wrap != W_RAW ? S_TRAILER : S_DONE) : S_BLOCK;
            else
                rc = codec_fail(d, CODEC_ERR_DATA, "deflate: invalid literal/length code");
            break;
        }
        uint32_t len = E_VAL(e) + (uint32_t)(bits & ((1u << E_X(e)) - 1));
        bits >>= E_X(e);
        nbits -= (int)E_X(e);
        // Distance: its code and extra bits fit what is left of the refill
        // (15 + 5 used of >= 57; 15 + 13 more).
        e = z->dist[bits & ((1u << DIST_BITS) - 1)];
        if (E_KIND(e) == E_SUB) {
            bits >>= DIST_BITS;
            nbits -= DIST_BITS;
            e = z->dist[E_VAL(e) + (bits & ((1u << E_X(e)) - 1))];
        }
        bits >>= E_BITS(e);
        nbits -= (int)E_BITS(e);
        if (E_KIND(e) != E_LEN) {
            rc = codec_fail(d, CODEC_ERR_DATA, "deflate: invalid distance code");
            break;
        }
        uint32_t dist = E_VAL(e) + (uint32_t)(bits & ((1u << E_X(e)) - 1));
        bits >>= E_X(e);
        nbits -= (int)E_X(e);
        if (tail && nbits < (int)in->over * 8) {
            rc = codec_fail(d, CODEC_ERR_TRUNCATED, "deflate: truncated stream");
            break;
        }
        if (dist > total) {
            rc = codec_fail(d, CODEC_ERR_DATA, "deflate: distance before the start of the stream");
            break;
        }
        r->w = w;
        r->total = total;
        ring_copy(r, dist, len);
        w = r->w;
        total = r->total;
    }
    SYNC_OUT();
#undef SYNC_OUT
#undef SYNC_IN
    if (rc == 0 && codec_in_overrun(in))
        rc = codec_fail(d, CODEC_ERR_TRUNCATED, "deflate: truncated stream");
    return rc;
}

// Fold the bytes decoded since the last call into the member's sum.
static void sum_update(codec_dec_t *d, inf_t *z) {
    codec_ring_t *r = &d->ring;
    while (z->sum_at < r->total) {
        size_t pending = (size_t)(r->total - z->sum_at);
        size_t at = r->w >= pending ? r->w - pending : r->w + r->size - pending;
        size_t k = r->size - at < pending ? r->size - at : pending;
        z->sum = z->wrap == W_ZLIB ? codec_adler32(z->sum, r->buf + at, k) : codec_crc32(z->sum, r->buf + at, k);
        z->sum_at += k;
    }
}

// One header byte, folded into the header CRC.  -1 at the end.
static int gz_byte(codec_in_t *in, uint32_t *crc) {
    int b = lsb_byte(in);
    if (b >= 0) {
        uint8_t c = (uint8_t)b;
        *crc = codec_crc32(*crc, &c, 1);
    }
    return b;
}

// RFC 1952 § 2.3: a member header.  0, or a CODEC_ERR_* code.
static int gz_header(codec_dec_t *d, inf_t *z) {
    codec_in_t *in = &d->in;
    uint32_t crc = 0;
    int h[10];
    int i = 0;
    if (z->magic_read) {
        static const uint8_t magic[2] = {0x1F, 0x8B};
        crc = codec_crc32(0, magic, 2);
        h[i++] = 0x1F;
        h[i++] = 0x8B;
        z->magic_read = false;
    }
    for (; i < 10; i++)
        if ((h[i] = gz_byte(in, &crc)) < 0)
            return codec_fail(d, CODEC_ERR_TRUNCATED, "gzip: truncated header");
    if (h[0] != 0x1F || h[1] != 0x8B)
        return codec_fail(d, CODEC_ERR_DATA, "gzip: not a gzip stream");
    if (h[2] != 8)
        return codec_fail(d, CODEC_ERR_UNSUPPORTED, "gzip: compression method is not DEFLATE");
    int flg = h[3];
    if (flg & GZ_RESERVED)
        return codec_fail(d, CODEC_ERR_DATA, "gzip: reserved header flags set");
    if (flg & GZ_FEXTRA) {
        int lo = gz_byte(in, &crc), hi = gz_byte(in, &crc);
        if (hi < 0)
            return codec_fail(d, CODEC_ERR_TRUNCATED, "gzip: truncated extra field");
        for (int n = lo | hi << 8; n > 0; n--)
            if (gz_byte(in, &crc) < 0)
                return codec_fail(d, CODEC_ERR_TRUNCATED, "gzip: truncated extra field");
    }
    for (int f = GZ_FNAME; f <= GZ_FCOMMENT; f <<= 1) {
        if (!(flg & f))
            continue;
        int b;
        while ((b = gz_byte(in, &crc)) > 0) // zero-terminated
            ;
        if (b < 0)
            return codec_fail(d, CODEC_ERR_TRUNCATED, "gzip: truncated name or comment");
    }
    if (flg & GZ_FHCRC) {
        uint32_t want = crc & 0xFFFF, dummy = 0;
        int lo = gz_byte(in, &dummy), hi = gz_byte(in, &dummy);
        if (hi < 0)
            return codec_fail(d, CODEC_ERR_TRUNCATED, "gzip: truncated header CRC");
        if ((uint32_t)(lo | hi << 8) != want)
            return codec_fail(d, CODEC_ERR_CHECKSUM, "gzip: header CRC mismatch");
    }
    return 0;
}

// A new gzip member's state: a fresh DEFLATE stream and sum.
static void gz_member_start(codec_dec_t *d, inf_t *z) {
    z->state = S_BLOCK;
    z->last = false;
    z->sum = 0;
    z->sum_at = z->member_at = d->ring.total;
}

static int inf_fill(codec_dec_t *d) {
    inf_t *z = d->st;
    codec_in_t *in = &d->in;
    codec_ring_t *r = &d->ring;
    int rc = 0;
    while (rc == 0 && z->state != S_DONE) {
        switch (z->state) {
        case S_GZ_HEADER:
            rc = gz_header(d, z);
            gz_member_start(d, z);
            break;
        case S_GZ_NEXT: {
            // Another member, or the end: what follows that is not a
            // member's magic is ignored.
            int b = lsb_byte(in);
            if (b < 0) {
                z->state = S_DONE;
                break;
            }
            int b2 = b == 0x1F ? lsb_byte(in) : -1;
            if (b2 != 0x8B) {
                z->state = S_DONE;
                break;
            }
            z->magic_read = true; // the header's first two bytes
            z->state = S_GZ_HEADER;
            break;
        }
        case S_ZLIB_HEADER: {
            // RFC 1950 § 2.2: CM 8, header a multiple of 31, no preset dictionary.
            lsb_fill(in);
            uint32_t cmf = lsb_get(in, 8), flg = lsb_get(in, 8);
            if (codec_in_overrun(in))
                rc = codec_fail(d, CODEC_ERR_TRUNCATED, "zlib: truncated header");
            else if ((cmf & 0x0F) != 8 || (cmf << 8 | flg) % 31 != 0 || (flg & 0x20))
                rc = codec_fail(d, CODEC_ERR_DATA, "zlib: bad header");
            z->state = S_BLOCK;
            break;
        }
        case S_BLOCK:
            rc = block_header(d, z);
            break;
        case S_STORED: {
            if (z->stored_left == 0) {
                z->state = z->last ? (z->wrap != W_RAW ? S_TRAILER : S_DONE) : S_BLOCK;
                break;
            }
            size_t room = ring_room(r);
            if (room == 0)
                goto more;
            // Straight into the ring, in up to two pieces.
            size_t want = z->stored_left < room ? z->stored_left : room;
            size_t k = r->size - r->w < want ? r->size - r->w : want;
            size_t got = lsb_read(in, r->buf + r->w, k);
            r->w += got;
            if (r->w == r->size)
                r->w = 0;
            r->total += got;
            z->stored_left -= (uint32_t)got;
            if (got < k)
                rc = codec_fail(d, CODEC_ERR_TRUNCATED, "deflate: truncated stored block");
            break;
        }
        case S_HUFF:
            if (ring_room(r) < INF_MATCH_MAX)
                goto more;
            rc = huff_run(d, z);
            if (rc == 0 && z->state == S_HUFF)
                goto more; // the ring is full
            break;
        case S_TRAILER: {
            sum_update(d, z);
            lsb_align(in);
            if (z->wrap == W_ZLIB) {
                // The Adler-32, when the stream carries it: some writers
                // stop right after the final block.
                uint8_t t[4];
                size_t got = lsb_read(in, t, 4);
                if (got == 4 && codec_be32(t) != z->sum)
                    rc = codec_fail(d, CODEC_ERR_CHECKSUM, "zlib: Adler-32 mismatch");
                else if (got != 0 && got != 4)
                    rc = codec_fail(d, CODEC_ERR_TRUNCATED, "zlib: truncated Adler-32");
                z->state = S_DONE;
                break;
            }
            // gzip: the member's CRC-32 and size mod 2^32.
            uint8_t t[8];
            if (lsb_read(in, t, 8) != 8)
                rc = codec_fail(d, CODEC_ERR_TRUNCATED, "gzip: truncated trailer");
            else if (codec_le32(t) != z->sum)
                rc = codec_fail(d, CODEC_ERR_CHECKSUM, "gzip: CRC-32 mismatch");
            else if (codec_le32(t + 4) != (uint32_t)(r->total - z->member_at))
                rc = codec_fail(d, CODEC_ERR_CHECKSUM, "gzip: size mismatch");
            z->state = S_GZ_NEXT;
            break;
        }
        case S_DONE:
            break;
        }
    }
    if (rc < 0)
        return rc;
    lsb_align(in); // a container's trailer follows on a byte boundary
    return CODEC_END;
more:
    if (z->wrap != W_RAW)
        sum_update(d, z);
    return CODEC_MORE;
}

static int inf_init_common(codec_dec_t *d, inf_wrap_t wrap) {
    inf_t *z = calloc(1, sizeof(*z));
    if (!z)
        return CODEC_ERR_NOMEM;
    d->st = z;
    z->wrap = wrap;
    z->state = wrap == W_ZLIB ? S_ZLIB_HEADER : wrap == W_GZIP ? S_GZ_HEADER : S_BLOCK;
    z->sum = wrap == W_ZLIB ? 1 : 0;
    return codec_ring_alloc(d, INF_WINDOW, INF_MATCH_MAX);
}

static int inf_init(codec_dec_t *d) {
    return inf_init_common(d, W_RAW);
}

static int zlib_init(codec_dec_t *d) {
    return inf_init_common(d, W_ZLIB);
}

static int gzip_init(codec_dec_t *d) {
    return inf_init_common(d, W_GZIP);
}

static void inf_free(codec_dec_t *d) {
    free(d->st);
    d->st = NULL;
}

const codec_dec_ops_t codec_deflate_dec = {inf_init, inf_fill, inf_free, false};
const codec_dec_ops_t codec_zlib_dec = {zlib_init, inf_fill, inf_free, false};
const codec_dec_ops_t codec_gzip_dec = {gzip_init, inf_fill, inf_free, false};
