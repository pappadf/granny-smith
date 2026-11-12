// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// codec_internal.h
// The framework every codec is written on, private to the library.
//
// A decoder is a `codec_dec_ops_t`: init (allocate its state and its output
// ring), fill (decode into the ring until it is nearly full or the stream
// ends) and free.  The ring is both the decoder's output buffer and its
// history -- an LZ decoder's window -- so a decoder never has to stop in the
// middle of a symbol: it checks there is room for the largest output one
// symbol can make (its `burst`) before decoding it, and returns when there
// is not.  codec.c drains the ring into the caller's buffers, so every
// decoder is resumable on the output side without suspending itself.
//
// Input is a `codec_in_t`: memory, or pulled as the decoder reads, with a
// 64-bit bit accumulator read LSB-first (lsb_*: DEFLATE, StuffIt's LZW and
// method 13) or MSB-first (msb_*: bzip2, StuffIt's methods 3 and 15,
// Compact Pro's LZH).  A decoder uses one order.  Reading past the end of
// the input yields zeros and is counted, so a decoder checks
// codec_in_overrun() at symbol boundaries rather than after every bit.
//
// An encoder is a `codec_enc_ops_t`: init (state for inputs up to a size,
// at a level), run (one input to one complete stream), free, bound.
//
// Each codec's source file follows one layout: the format in its header
// comment; constants; types; bit and table helpers; the decoder (init,
// fill, free, its ops); the encoder, if any; byte-order and checksum
// helpers come from here and checksum.c, never a private copy.

#ifndef CODEC_INTERNAL_H
#define CODEC_INTERNAL_H

#include "codec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// ============================================================================
// Byte order
// ============================================================================

static inline uint32_t codec_le16(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}
static inline uint32_t codec_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint64_t codec_le64(const uint8_t *p) {
    return (uint64_t)codec_le32(p) | (uint64_t)codec_le32(p + 4) << 32;
}
static inline uint32_t codec_be16(const uint8_t *p) {
    return (uint32_t)p[0] << 8 | (uint32_t)p[1];
}
static inline uint32_t codec_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}
static inline uint64_t codec_be64(const uint8_t *p) {
    return (uint64_t)codec_be32(p) << 32 | codec_be32(p + 4);
}
static inline void codec_wr_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}
static inline void codec_wr_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

// ============================================================================
// Input
// ============================================================================

#define CODEC_IN_CHUNK 65536 // bytes pulled at a time

typedef struct {
    const uint8_t *p; // the bytes in hand: the memory input, or buf
    size_t pos, len; // the next byte of p, and its end
    codec_pull_fn pull; // NULL for memory input
    void *pull_ctx;
    uint8_t *buf; // the pull buffer (CODEC_IN_CHUNK)
    bool eof; // nothing more to come
    int err; // a pull error (negative), 0 if none
    uint64_t base; // the input offset of p[0]
    uint64_t bits; // the bit accumulator (LSB: low bits next; MSB: high bits next)
    int nbits; // valid bits in it
    uint32_t over; // zero bytes put in it past the end of the input
} codec_in_t;

// Get more input into p; false at the end (or on a pull error).
bool codec_in_refill(codec_in_t *in);

// The next byte, or -1 at the end.  Not for use while the bit accumulator
// holds bytes (align and drain it first: lsb_byte / msb_byte).
static inline int codec_in_byte(codec_in_t *in) {
    if (in->pos == in->len && !codec_in_refill(in))
        return -1;
    return in->p[in->pos++];
}

// Up to `n` bytes into dst; returns the count (short only at the end).
size_t codec_in_read(codec_in_t *in, uint8_t *dst, size_t n);

// True once a decoder has used bits past the end of the input.
static inline bool codec_in_overrun(const codec_in_t *in) {
    return in->nbits < (int)(in->over * 8);
}

// Input bytes consumed: taken from p, less whole bytes still in the
// accumulator (and the zero padding).
static inline uint64_t codec_in_consumed(const codec_in_t *in) {
    int buffered = in->nbits / 8 - (int)in->over;
    return in->base + in->pos - (uint64_t)(buffered > 0 ? buffered : 0);
}

// --- LSB-first bits ---------------------------------------------------------

// Top the accumulator up to at least 57 bits.  Eight bytes at a time where
// the input allows: the bits loaded above the count are the next bytes'
// own, so loading them again later changes nothing.
static inline void lsb_fill(codec_in_t *in) {
    if (in->nbits > 56)
        return;
    if (in->len - in->pos >= 8) {
        uint64_t v;
        memcpy(&v, in->p + in->pos, 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        v = codec_le64(in->p + in->pos);
#endif
        in->bits |= v << in->nbits;
        int k = (63 - in->nbits) >> 3;
        in->pos += (size_t)k;
        in->nbits += k * 8;
        return;
    }
    while (in->nbits <= 56) {
        int b = codec_in_byte(in);
        if (b < 0) {
            b = 0;
            in->over++;
        }
        in->bits |= (uint64_t)b << in->nbits;
        in->nbits += 8;
    }
}

// The next `k` (<= 32) bits, not consumed (the accumulator must hold them:
// lsb_fill first).
static inline uint32_t lsb_peek(const codec_in_t *in, int k) {
    return (uint32_t)(in->bits & ((1ull << k) - 1));
}
static inline void lsb_drop(codec_in_t *in, int k) {
    in->bits >>= k;
    in->nbits -= k;
}
// The next `k` (<= 32) bits, consumed.
static inline uint32_t lsb_get(codec_in_t *in, int k) {
    if (in->nbits < k)
        lsb_fill(in);
    uint32_t v = lsb_peek(in, k);
    lsb_drop(in, k);
    return v;
}
// Drop the bits up to the next byte boundary.
// True if at least `k` (<= 56) bits of input remain.
static inline bool lsb_avail(codec_in_t *in, int k) {
    lsb_fill(in);
    return in->nbits - (int)in->over * 8 >= k;
}
static inline void lsb_align(codec_in_t *in) {
    lsb_drop(in, in->nbits & 7);
}
// The next whole byte after lsb_align: from the accumulator while it holds
// input, then from the input itself (the accumulator emptied -- its
// look-ahead bits cleared, as they are about to be read directly).  -1 at
// the end.
static inline int lsb_byte(codec_in_t *in) {
    if (in->nbits / 8 > (int)in->over) {
        int v = (int)(in->bits & 0xFF);
        lsb_drop(in, 8);
        return v;
    }
    if (in->over)
        return -1;
    in->bits = 0;
    in->nbits = 0;
    return codec_in_byte(in);
}

// Up to `n` whole bytes after lsb_align, as lsb_byte takes them; returns
// the count (short only at the end).
size_t lsb_read(codec_in_t *in, uint8_t *dst, size_t n);

// --- MSB-first bits ---------------------------------------------------------

// Top the accumulator (valid bits at the top) up to at least 57 bits.
static inline void msb_fill(codec_in_t *in) {
    if (in->nbits > 56)
        return;
    if (in->len - in->pos >= 8) {
        uint64_t v = codec_be64(in->p + in->pos);
        in->bits |= v >> in->nbits;
        int k = (63 - in->nbits) >> 3;
        in->pos += (size_t)k;
        in->nbits += k * 8;
        return;
    }
    while (in->nbits <= 56) {
        int b = codec_in_byte(in);
        if (b < 0) {
            b = 0;
            in->over++;
        }
        in->bits |= (uint64_t)b << (56 - in->nbits);
        in->nbits += 8;
    }
}
static inline uint32_t msb_peek(const codec_in_t *in, int k) {
    return (uint32_t)(in->bits >> (64 - k));
}
static inline void msb_drop(codec_in_t *in, int k) {
    in->bits <<= k;
    in->nbits -= k;
}
static inline uint32_t msb_get(codec_in_t *in, int k) {
    if (k == 0)
        return 0;
    if (in->nbits < k)
        msb_fill(in);
    uint32_t v = msb_peek(in, k);
    msb_drop(in, k);
    return v;
}
// True if at least `k` (<= 56) bits of input remain.
static inline bool msb_avail(codec_in_t *in, int k) {
    msb_fill(in);
    return in->nbits - (int)in->over * 8 >= k;
}
static inline void msb_align(codec_in_t *in) {
    msb_drop(in, in->nbits & 7);
}
// The next whole byte after msb_align, as lsb_byte takes it.  -1 at the end.
static inline int msb_byte(codec_in_t *in) {
    if (in->nbits / 8 > (int)in->over) {
        int v = (int)(in->bits >> 56);
        msb_drop(in, 8);
        return v;
    }
    if (in->over)
        return -1;
    in->bits = 0;
    in->nbits = 0;
    return codec_in_byte(in);
}

// ============================================================================
// Output ring
// ============================================================================

// The decoded bytes: written at w, drained by codec.c from total - drained
// bytes back.  A byte stays readable as history until `size` more have
// been written after it.
typedef struct {
    uint8_t *buf;
    size_t size; // bytes in buf
    size_t w; // the next byte written
    uint64_t total; // bytes ever written
    uint64_t drained; // bytes handed to the caller
} codec_ring_t;

// Bytes that may be written before the undrained ones would be overwritten.
static inline size_t ring_room(const codec_ring_t *r) {
    return r->size - (size_t)(r->total - r->drained);
}

static inline void ring_put(codec_ring_t *r, uint8_t b) {
    r->buf[r->w] = b;
    if (++r->w == r->size)
        r->w = 0;
    r->total++;
}

// The byte `dist` back (1: the last written).  dist <= min(total, size).
static inline uint8_t ring_back(const codec_ring_t *r, size_t dist) {
    return r->buf[r->w >= dist ? r->w - dist : r->w + r->size - dist];
}

// Append `n` bytes (n <= room).
void ring_write(codec_ring_t *r, const uint8_t *src, size_t n);

// Append `n` copies of byte `b` (n <= room).
void ring_fill(codec_ring_t *r, uint8_t b, size_t n);

// Append `len` bytes copied from `dist` back, overlapping as LZ77 does
// (1 <= dist <= min(total, size), len <= room).
static inline void ring_copy(codec_ring_t *r, size_t dist, size_t len) {
    size_t s = r->w >= dist ? r->w - dist : r->w + r->size - dist;
    if (s + len <= r->size && r->w + len <= r->size) {
        uint8_t *d = r->buf + r->w;
        const uint8_t *q = r->buf + s;
        if (dist >= len)
            memcpy(d, q, len);
        else if (dist == 1)
            memset(d, *q, len);
        else if (dist >= 8) {
            size_t i = 0;
            for (; i + 8 <= len; i += 8)
                memcpy(d + i, q + i, 8); // eight at a time: dist >= 8, so no overlap within a step
            for (; i < len; i++)
                d[i] = q[i];
        } else {
            for (size_t i = 0; i < len; i++)
                d[i] = q[i];
        }
        r->w += len;
        if (r->w == r->size)
            r->w = 0;
        r->total += len;
        return;
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t b = r->buf[s];
        if (++s == r->size)
            s = 0;
        ring_put(r, b);
    }
}

// Append `len` bytes copied from `dist` back, where the bytes before the
// start of the stream read as zeros (a window that starts zero-filled:
// StuffIt 13, Compact Pro).  1 <= dist <= size, len <= room.
void ring_copy_zero(codec_ring_t *r, size_t dist, size_t len);

// ============================================================================
// Huffman trees
// ============================================================================

// Canonical codes walked a bit at a time through a tree of pooled nodes:
// for the formats whose codes are odd enough that a lookup table would
// change what they accept (StuffIt 13's absent symbols take code values,
// and its codes reach 31 bits) or are rare (Compact Pro).  A pool holds one
// or more trees.

#define CODEC_HTREE_CAP   2048
#define CODEC_HTREE_NOSYM ((int16_t) - 1)

typedef struct {
    int16_t ch[2]; // child node indices, or -1
    int16_t sym; // leaf symbol, or CODEC_HTREE_NOSYM for an inner node
} codec_hnode_t;

typedef struct {
    codec_hnode_t node[CODEC_HTREE_CAP];
    int used;
} codec_htree_t;

void codec_htree_reset(codec_htree_t *p);

// A new empty node (a root), or -1 when the pool is full.
int codec_htree_node(codec_htree_t *p);

// Insert `sym` under `root` at the `len`-bit code `code` (MSB first).  0 or -1.
int codec_htree_insert(codec_htree_t *p, int root, uint32_t code, int len, int sym);

// A canonical tree from `lengths` (0 or, below min_len, absent): codes in
// ascending length from min_len, then ascending symbol; an absent symbol
// at a length below 1 still takes a code value.  Returns the root, or -1
// (a length out of range, or the pool full).
int codec_htree_build(codec_htree_t *p, const int8_t *lengths, int nsym, int min_len, int max_len);

static inline int codec_htree_child(const codec_htree_t *p, int node, int bit) {
    return p->node[node].ch[bit];
}
static inline int codec_htree_sym(const codec_htree_t *p, int node) {
    return p->node[node].sym;
}

// ============================================================================
// Decoders
// ============================================================================

#define CODEC_MORE 0 // fill: the ring is (nearly) full, more remains
#define CODEC_END  1 // fill: the stream has ended

typedef struct codec_dec_ops {
    // Allocate d->st and the ring (codec_ring_alloc).  0 or CODEC_ERR_*.
    int (*init)(codec_dec_t *d);
    // Decode into the ring while it has room for one more symbol.
    // CODEC_MORE, CODEC_END, or CODEC_ERR_* (codec_fail).  It is called
    // only with the ring drained, and must produce output or consume input.
    int (*fill)(codec_dec_t *d);
    void (*free)(codec_dec_t *d); // free d->st's contents and d->st (not the ring)
    bool sized; // the stream has no end of its own: opts.out_len is required
} codec_dec_ops_t;

struct codec_dec {
    const codec_dec_ops_t *ops;
    codec_id_t id;
    codec_in_t in;
    codec_ring_t ring;
    uint64_t out_len; // CODEC_LEN_UNKNOWN, or the decoded size (a limit)
    uint32_t flags;
    int status; // 0 running, CODEC_END, or CODEC_ERR_*
    char msg[96]; // why it failed
    void *st; // the codec's own state
};

// Allocate the ring: history of `window` bytes, room for `burst` bytes of
// output per symbol.  0 or CODEC_ERR_NOMEM.
int codec_ring_alloc(codec_dec_t *d, size_t window, size_t burst);

// Fail with `rc` and a message (the first one set stays).  Returns rc.
int codec_fail(codec_dec_t *d, int rc, const char *msg);

// Bytes left to produce of a sized stream (0 once its last symbol has
// reached or passed the size: codec.c cuts the excess).
static inline uint64_t codec_left(const codec_dec_t *d) {
    return d->ring.total >= d->out_len ? 0 : d->out_len - d->ring.total;
}

extern const codec_dec_ops_t codec_copy_dec, codec_deflate_dec, codec_zlib_dec, codec_adc_dec, codec_bzip2_dec,
    codec_lzfse_dec, codec_xz_dec, codec_lzma_alone_dec, codec_rle90_dec, codec_sit_lzw_dec, codec_sit_huffman_dec,
    codec_sit13_dec, codec_sit_arsenic_dec, codec_cpt_rle_dec, codec_cpt_lzh_dec, codec_dcmp_dec, codec_gsrle_dec;

// ============================================================================
// Encoders
// ============================================================================

typedef struct codec_enc_ops {
    int (*init)(codec_enc_t *e, size_t max_len); // allocate e->st
    int64_t (*run)(codec_enc_t *e, const uint8_t *in, size_t len, uint8_t *out, size_t cap);
    void (*free)(codec_enc_t *e);
    size_t (*bound)(size_t len);
    int default_level;
} codec_enc_ops_t;

struct codec_enc {
    const codec_enc_ops_t *ops;
    size_t max_len;
    int level; // 1..9 (0 for codecs without levels)
    void *st;
};

extern const codec_enc_ops_t codec_zlib_enc, codec_xz_enc, codec_gsrle_enc;

// ============================================================================
// Shared tables
// ============================================================================

// RFC 1951 §3.2.5 length and distance code tables (inflate.c; the encoder
// in deflate.c uses them too).
extern const uint16_t codec_deflate_len_base[29];
extern const uint8_t codec_deflate_len_extra[29];
extern const uint16_t codec_deflate_dist_base[30];
extern const uint8_t codec_deflate_dist_extra[30];

#endif // CODEC_INTERNAL_H
