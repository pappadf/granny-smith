// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// sit3.c — StuffIt classic method 3 ("StuffIt Huffman") decompressor.
//
// Format spec: sit3.md
//
// This file is an internal helper called by sit.c; not part of the
// public API.

#include "internal.h"

// ============================================================================
// Constants
// ============================================================================

// sit3.md § 2.2 / unsit.c nodelist[512] — a full binary tree with 256
// leaves has at most 2·256 − 1 = 511 nodes.  We allocate one slack slot.
#define M3_MAX_NODES 512

// ============================================================================
// Types
// ============================================================================

// One Huffman tree node.  Internal nodes use `zero`/`one` indices into
// the same pool; leaves have both children set to -1 and carry `symbol`.
typedef struct {
    int16_t zero; // index of 0-branch child, or -1 for leaf
    int16_t one; // index of 1-branch child, or -1 for leaf
    uint8_t symbol; // valid only when zero == -1 && one == -1
} m3_node_t;

// MSB-first bit reader (peeler's shared peel_msb_t, internal.h) and the
// abort context a short stream is reported through.
typedef struct {
    peel_msb_t r;
    decode_ctx_t *ctx;
} m3_bits_t;

// ============================================================================
// Bit Reader
// ============================================================================

// sit3.md § 2.1 — N bits, MSB-first within each byte, bytes consumed in
// order.  Running out of input is an error, not zeros.
static unsigned m3_read_bits(m3_bits_t *b, unsigned n) {
    if (!peel_msb_avail(&b->r, (int)n))
        decode_abort(b->ctx, "SIT3: premature end of compressed stream");
    return (unsigned)peel_msb_get(&b->r, (int)n);
}

static int m3_read_bit(m3_bits_t *b) {
    return (int)m3_read_bits(b, 1);
}

// ============================================================================
// Tree Reader
// ============================================================================

// Static node pool used during tree construction.  next_node tracks the
// first unused slot.  Recursive pre-order serialization:
//   bit 1 → leaf, followed by 8 bits of symbol value
//   bit 0 → internal node, followed by zero-child then one-child
// sit3.md § 2.2 / unsit.c read_tree().
typedef struct {
    m3_node_t nodes[M3_MAX_NODES];
    int next_node;
} m3_tree_t;

static int m3_alloc_node(m3_tree_t *t, decode_ctx_t *ctx) {
    if (t->next_node >= M3_MAX_NODES) {
        decode_abort(ctx, "SIT3: Huffman tree exceeds %d nodes", M3_MAX_NODES);
    }
    int idx = t->next_node++;
    t->nodes[idx].zero = -1;
    t->nodes[idx].one = -1;
    t->nodes[idx].symbol = 0;
    return idx;
}

static int m3_read_node(m3_bits_t *b, m3_tree_t *t) {
    int bit = m3_read_bit(b);
    int idx = m3_alloc_node(t, b->ctx);
    if (bit == 1) {
        // Leaf: read 8-bit symbol.
        t->nodes[idx].symbol = (uint8_t)m3_read_bits(b, 8);
        return idx;
    }
    // Internal: recursively read both subtrees.  Order matters: zero
    // first, then one (matches the recursion order in unsit.c).
    int zero_child = m3_read_node(b, t);
    int one_child = m3_read_node(b, t);
    // Refresh the slot pointer — `nodes[idx]` is still the same entry,
    // but `t->nodes` may have moved had this been a growable buffer.
    // Since the pool is fixed-size we can index directly.
    t->nodes[idx].zero = (int16_t)zero_child;
    t->nodes[idx].one = (int16_t)one_child;
    return idx;
}

// ============================================================================
// Decode Loop -- resumable
// ============================================================================

// A method-3 stream: the tree is read once, then each run() walks it for
// as many symbols as the caller's buffer holds.  The decode context lives
// here, not on a stack, and is re-armed by every run(): an abort ends the
// stream, and its message is the producer's.
typedef struct {
    peel_producer_t base;
    decode_ctx_t ctx;
    m3_bits_t bits;
    m3_tree_t tree;
    int root;
    size_t produced, total;
    bool failed;
} m3_stream_t;

// Walk the tree one bit per branch; at a leaf, emit the symbol.
// sit3.md § 2.3 — repeat until uncomp_len bytes have been produced;
// trailing bits in the final compressed byte are discarded.  A tree that
// is a single leaf codes its one symbol as the empty bit string.
static size_t m3_decode(m3_stream_t *m, uint8_t *out, size_t cap) {
    const m3_tree_t *t = &m->tree;
    int root = m->root;
    if (t->nodes[root].zero == -1 && t->nodes[root].one == -1) {
        memset(out, t->nodes[root].symbol, cap);
        return cap;
    }
    for (size_t i = 0; i < cap; i++) {
        int n = root;
        while (t->nodes[n].zero != -1 || t->nodes[n].one != -1) {
            int bit = m3_read_bit(&m->bits);
            n = (bit == 1) ? t->nodes[n].one : t->nodes[n].zero;
            if (n < 0) {
                // Defensive: malformed tree pointer.  read_node() should
                // never leave -1 on an internal node, but guard anyway.
                decode_abort(&m->ctx, "SIT3: corrupt Huffman tree (NULL child)");
            }
        }
        out[i] = t->nodes[n].symbol;
    }
    return cap;
}

static int m3_run(peel_producer_t *p, uint8_t *out, size_t cap, size_t *n) {
    m3_stream_t *m = (m3_stream_t *)p;
    *n = 0;
    if (m->failed)
        return -5;
    if (setjmp(m->ctx.jmp) != 0) {
        m->failed = true;
        snprintf(p->err, sizeof(p->err), "%s", m->ctx.errmsg);
        return -5;
    }
    size_t want = m->total - m->produced < cap ? m->total - m->produced : cap;
    *n = m3_decode(m, out, want);
    m->produced += *n;
    return m->produced == m->total ? 1 : 0;
}

static void m3_free(peel_producer_t *p) {
    m3_stream_t *m = (m3_stream_t *)p;
    dctx_cleanup(&m->ctx);
    free(m);
}

// A producer of a method-3 fork's `uncomp_len` bytes from `src` (borrowed:
// it must outlive the producer).  NULL + *err when the header is bad.
peel_producer_t *sit3_producer(const uint8_t *src, size_t len, size_t uncomp_len, peel_err_t **err) {
    *err = NULL;
    m3_stream_t *m = calloc(1, sizeof(*m));
    if (!m) {
        *err = make_err("SIT3: out of memory");
        return NULL;
    }
    m->base.run = m3_run;
    m->base.free = m3_free;
    m->total = uncomp_len;
    dctx_init(&m->ctx);
    if (uncomp_len == 0)
        return &m->base; // nothing to decode: no bits are consumed
    if (setjmp(m->ctx.jmp) != 0) {
        *err = make_err("%s", m->ctx.errmsg);
        m3_free(&m->base);
        return NULL;
    }
    if (len == 0)
        decode_abort(&m->ctx, "SIT3: zero-length compressed stream but uncomp_len=%zu", uncomp_len);
    m->bits.ctx = &m->ctx;
    peel_msb_init(&m->bits.r, src, len);
    m->tree.next_node = 0;
    m->root = m3_read_node(&m->bits, &m->tree);
    return &m->base;
}

// ============================================================================
// Public Entry Point — called from sit.c decompress_fork() dispatch
// ============================================================================

// Decompress a method-3 (static Huffman) fork.  Returns an owned buffer
// of exactly `uncomp_len` bytes on success, or a zero buffer with *err
// set on failure.  CRC verification against the stored fork CRC is the
// caller's responsibility (sit.c does this for all classic methods).
peel_buf_t peel_sit3(const uint8_t *src, size_t len, size_t uncomp_len, peel_err_t **err) {
    peel_producer_t *p = sit3_producer(src, len, uncomp_len, err);
    return p ? peel_producer_drain(p, uncomp_len, NULL, err) : (peel_buf_t){0};
}
