// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpt.c
// Compact Pro's two stages (see codec.h): LZH, an LZSS + Huffman stream,
// and RLE, run lengths with a 0x81 escape.  A fork is RLE, or LZH then
// RLE (chain the decoders: codec_dec_pull).  Format notes:
// src/peeler/docs/cpt.md §§ 5, 6.
//
// LZH (cpt.md § 6): an MSB-first bit stream of blocks, each three canonical
// Huffman codes (literal, length, offset; code lengths packed in nibbles)
// and then tokens -- a 1 bit and a literal, or a 0 bit, a length, an offset
// code and 6 more offset bits -- into an 8 KiB window that starts
// zero-filled.  Offset 0 means the whole window back.  A block ends once its
// tokens' cost (2 a literal, 3 a match) reaches 0x1FFF0; the input then
// skips to a byte boundary and 2 or 3 bytes more, by the parity of the
// bytes the block took.  The stream ends with its input, where a block or a
// token would begin.
//
// RLE (cpt.md § 5): bytes are themselves except 0x81:
//   0x81 0x82 N   (N > 0) the previous byte N - 1 more times ("N - 2 more"
//                 after the one emitted with the escape); N = 0: 0x81, 0x82
//   0x81 0x81     a literal 0x81, and the second 0x81 starts an escape again
//   0x81 X        a literal 0x81, then X
// The container states the decoded size.

#include "codec_internal.h"

#include <stdlib.h>

// ============================================================================
// LZH
// ============================================================================

#define CP_WIN_SIZE    8192
#define CP_BLOCK_COST  0x1FFF0
#define CP_LIT_COUNT   256
#define CP_LEN_COUNT   64
#define CP_OFF_COUNT   128
#define CP_MAX_CODELEN 15
#define CP_LZH_BURST   (CP_LEN_COUNT - 1) // the longest match

typedef struct {
    codec_htree_t lit, len, off; // one pool per tree
    int lit_root, len_root, off_root;
    bool tables_ok; // the current block's codes are built
    unsigned blk_cost; // the current block's tokens' cost
    uint64_t blk_byte_start; // input bytes consumed when its data began
} cp_lzh_t;

// Walk a tree to a leaf: its symbol, or -1 (the input ran out, or a branch
// leads nowhere).
static int htree_decode(const codec_htree_t *t, int root, codec_in_t *in) {
    int node = root;
    for (;;) {
        int sym = codec_htree_sym(t, node);
        if (sym != CODEC_HTREE_NOSYM)
            return sym;
        if (!msb_avail(in, 1))
            return -1;
        node = codec_htree_child(t, node, (int)msb_get(in, 1));
        if (node < 0)
            return -1;
    }
}

// One code-length table (cpt.md § 6.4.1): a byte count, then two 4-bit
// lengths per byte.  The tree's root, or -1.
static int read_table(codec_in_t *in, codec_htree_t *t, int sym_count) {
    int8_t lens[CP_LIT_COUNT];
    if (!msb_avail(in, 8))
        return -1;
    unsigned nbytes = msb_get(in, 8);
    if (nbytes * 2u > (unsigned)sym_count)
        return -1;
    memset(lens, 0, (size_t)sym_count);
    for (unsigned i = 0; i < nbytes; i++) {
        if (!msb_avail(in, 8))
            return -1;
        unsigned v = msb_get(in, 8);
        lens[2 * i] = (int8_t)(v >> 4);
        lens[2 * i + 1] = (int8_t)(v & 0x0F);
    }
    codec_htree_reset(t);
    return codec_htree_build(t, lens, sym_count, 1, CP_MAX_CODELEN);
}

static int lzh_fill(codec_dec_t *d) {
    cp_lzh_t *z = d->st;
    codec_in_t *in = &d->in;
    codec_ring_t *r = &d->ring;
    while (ring_room(r) >= CP_LZH_BURST) {
        if (z->tables_ok && z->blk_cost >= CP_BLOCK_COST) {
            // The end of a block: to a byte boundary, then 2 or 3 bytes.
            msb_align(in);
            uint64_t took = codec_in_consumed(in) - z->blk_byte_start;
            for (int i = (took & 1) ? 3 : 2; i > 0; i--)
                msb_get(in, 8);
            z->tables_ok = false;
        }
        if (!z->tables_ok) {
            if (!msb_avail(in, 8))
                return CODEC_END; // the end, where a block would begin
            if ((z->lit_root = read_table(in, &z->lit, CP_LIT_COUNT)) < 0 ||
                (z->len_root = read_table(in, &z->len, CP_LEN_COUNT)) < 0 ||
                (z->off_root = read_table(in, &z->off, CP_OFF_COUNT)) < 0)
                return codec_fail(d, CODEC_ERR_DATA, "cpt-lzh: bad code table");
            z->tables_ok = true;
            z->blk_cost = 0;
            z->blk_byte_start = codec_in_consumed(in);
        }
        if (!msb_avail(in, 1))
            return CODEC_END; // the end, where a token would begin
        if (msb_get(in, 1)) {
            int sym = htree_decode(&z->lit, z->lit_root, in);
            if (sym < 0)
                return codec_fail(d, CODEC_ERR_DATA, "cpt-lzh: bad literal code");
            ring_put(r, (uint8_t)sym);
            z->blk_cost += 2;
            continue;
        }
        int mlen = htree_decode(&z->len, z->len_root, in);
        int off_sym = mlen < 0 ? -1 : htree_decode(&z->off, z->off_root, in);
        if (off_sym < 0 || !msb_avail(in, 6))
            return codec_fail(d, CODEC_ERR_DATA, "cpt-lzh: bad match");
        unsigned offset = (unsigned)off_sym << 6 | msb_get(in, 6); // at most 8191
        // Offset 0 is a whole window back: the field is 13 bits, so 8192
        // cannot be written any other way (Compact Pro 1.33 and 1.52 use it).
        if (mlen == 0)
            return codec_fail(d, CODEC_ERR_DATA, "cpt-lzh: a zero-length match");
        z->blk_cost += 3;
        ring_copy_zero(r, offset ? offset : CP_WIN_SIZE, (size_t)mlen);
    }
    return CODEC_MORE;
}

static int lzh_init(codec_dec_t *d) {
    d->st = calloc(1, sizeof(cp_lzh_t));
    if (!d->st)
        return CODEC_ERR_NOMEM;
    return codec_ring_alloc(d, CP_WIN_SIZE, CP_LZH_BURST);
}

static void cpt_free(codec_dec_t *d) {
    free(d->st);
    d->st = NULL;
}

const codec_dec_ops_t codec_cpt_lzh_dec = {lzh_init, lzh_fill, cpt_free, false};

// ============================================================================
// RLE
// ============================================================================

#define CP_RLE_BURST 256 // the most one escape writes

typedef struct {
    int prev; // the last byte (a run repeats it)
    uint32_t run_left; // copies of prev still to emit
    bool escape_pending; // a 0x81 to read again (after 0x81 0x81)
    int literal_next; // a byte to emit next (after 0x81 X), or -1
} cp_rle_t;

static int rle_fill(codec_dec_t *d) {
    cp_rle_t *s = d->st;
    codec_in_t *in = &d->in;
    codec_ring_t *r = &d->ring;
    while (codec_left(d) > 0) {
        if (ring_room(r) < CP_RLE_BURST)
            return CODEC_MORE;
        if (s->run_left) {
            ring_fill(r, (uint8_t)s->prev, s->run_left);
            s->run_left = 0;
            continue;
        }
        if (s->literal_next >= 0) {
            ring_put(r, (uint8_t)s->literal_next);
            s->literal_next = -1;
            continue;
        }
        int b;
        if (s->escape_pending) {
            b = 0x81;
            s->escape_pending = false;
        } else if ((b = codec_in_byte(in)) < 0)
            return CODEC_END; // the end of the input (short of the size: codec.c says so)
        if (b != 0x81) {
            s->prev = b;
            ring_put(r, (uint8_t)b);
            continue;
        }
        int next = codec_in_byte(in);
        if (next < 0)
            return CODEC_END;
        if (next == 0x82) {
            int count = codec_in_byte(in);
            if (count < 0)
                return CODEC_END;
            if (count == 0) {
                // A literal 0x81, then 0x82.
                ring_put(r, 0x81);
                s->prev = 0x82;
                s->literal_next = 0x82;
            } else {
                // The previous byte: once now, count - 2 more (the N - 2 rule).
                ring_put(r, (uint8_t)s->prev);
                s->run_left = count >= 2 ? (uint32_t)count - 2 : 0;
            }
        } else if (next == 0x81) {
            // A literal 0x81; the second 0x81 is read again, and may start
            // another escape (the half-escape).
            ring_put(r, 0x81);
            s->prev = 0x81;
            s->escape_pending = true;
        } else {
            // A literal 0x81, then the byte after it.
            ring_put(r, 0x81);
            s->prev = next;
            s->literal_next = next;
        }
    }
    return CODEC_END;
}

static int rle_init(codec_dec_t *d) {
    cp_rle_t *s = calloc(1, sizeof(*s));
    if (!s)
        return CODEC_ERR_NOMEM;
    s->literal_next = -1;
    d->st = s;
    return codec_ring_alloc(d, 0, CP_RLE_BURST);
}

const codec_dec_ops_t codec_cpt_rle_dec = {rle_init, rle_fill, cpt_free, true};
