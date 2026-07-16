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
#define M3_MAX_NODES  512

// ============================================================================
// Types
// ============================================================================

// One Huffman tree node.  Internal nodes use `zero`/`one` indices into
// the same pool; leaves have both children set to -1 and carry `symbol`.
typedef struct {
    int16_t zero;    // index of 0-branch child, or -1 for leaf
    int16_t one;     // index of 1-branch child, or -1 for leaf
    uint8_t symbol;  // valid only when zero == -1 && one == -1
} m3_node_t;

// MSB-first bit reader over a fixed input buffer.
typedef struct {
    const uint8_t *src;
    size_t         len;
    size_t         byte_pos;  // index of byte currently being drained
    unsigned       bit_pos;   // 0..7, position of *next* bit within byte (0 = MSB)
    decode_ctx_t  *ctx;
} m3_bits_t;

// ============================================================================
// Bit Reader
// ============================================================================

// sit3.md § 2.1 — MSB-first within each byte, bytes consumed in order.
static int m3_read_bit(m3_bits_t *b) {
    if (b->byte_pos >= b->len) {
        decode_abort(b->ctx, "SIT3: premature end of compressed stream");
    }
    uint8_t byte = b->src[b->byte_pos];
    unsigned shift = 7u - b->bit_pos;
    int bit = (byte >> shift) & 1;
    b->bit_pos++;
    if (b->bit_pos == 8) {
        b->bit_pos = 0;
        b->byte_pos++;
    }
    return bit;
}

// Read N bits MSB-first as an unsigned integer.
// Used only for the 8-bit symbol values in the tree header.
static unsigned m3_read_bits(m3_bits_t *b, unsigned n) {
    unsigned v = 0;
    for (unsigned i = 0; i < n; i++) {
        v = (v << 1) | (unsigned)m3_read_bit(b);
    }
    return v;
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
    int       next_node;
} m3_tree_t;

static int m3_alloc_node(m3_tree_t *t, decode_ctx_t *ctx) {
    if (t->next_node >= M3_MAX_NODES) {
        decode_abort(ctx, "SIT3: Huffman tree exceeds %d nodes", M3_MAX_NODES);
    }
    int idx = t->next_node++;
    t->nodes[idx].zero = -1;
    t->nodes[idx].one  = -1;
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
    int one_child  = m3_read_node(b, t);
    // Refresh the slot pointer — `nodes[idx]` is still the same entry,
    // but `t->nodes` may have moved had this been a growable buffer.
    // Since the pool is fixed-size we can index directly.
    t->nodes[idx].zero = (int16_t)zero_child;
    t->nodes[idx].one  = (int16_t)one_child;
    return idx;
}

// ============================================================================
// Decode Loop
// ============================================================================

// Walk the tree one bit per branch.  At a leaf, emit the symbol.
// sit3.md § 2.3 — repeat until uncomp_len bytes have been produced;
// trailing bits in the final compressed byte are discarded.
static void m3_decode(m3_bits_t *b, const m3_tree_t *t, int root,
                      uint8_t *out, size_t uncomp_len) {
    // Degenerate single-leaf tree: the code for the only symbol is the
    // empty bit string.  Emit `uncomp_len` copies without reading bits.
    if (t->nodes[root].zero == -1 && t->nodes[root].one == -1) {
        memset(out, t->nodes[root].symbol, uncomp_len);
        return;
    }
    for (size_t i = 0; i < uncomp_len; i++) {
        int n = root;
        while (t->nodes[n].zero != -1 || t->nodes[n].one != -1) {
            int bit = m3_read_bit(b);
            n = (bit == 1) ? t->nodes[n].one : t->nodes[n].zero;
            if (n < 0) {
                // Defensive: malformed tree pointer.  read_node() should
                // never leave -1 on an internal node, but guard anyway.
                decode_abort(b->ctx, "SIT3: corrupt Huffman tree (NULL child)");
            }
        }
        out[i] = t->nodes[n].symbol;
    }
}

// ============================================================================
// Public Entry Point — called from sit.c decompress_fork() dispatch
// ============================================================================

// Decompress a method-3 (static Huffman) fork.  Returns an owned buffer
// of exactly `uncomp_len` bytes on success, or a zero buffer with *err
// set on failure.  CRC verification against the stored fork CRC is the
// caller's responsibility (sit.c does this for all classic methods).
peel_buf_t peel_sit3(const uint8_t *src, size_t len, size_t uncomp_len,
                     peel_err_t **err) {
    *err = NULL;

    decode_ctx_t ctx;
    if (setjmp(ctx.jmp) != 0) {
        *err = make_err("%s", ctx.errmsg);
        return (peel_buf_t){0};
    }

    // Empty fork — nothing to decode, but the spec still requires us to
    // not consume any bits.  Return an empty owned buffer.
    if (uncomp_len == 0) {
        return (peel_buf_t){.data = NULL, .size = 0, .owned = false};
    }

    if (len == 0) {
        decode_abort(&ctx, "SIT3: zero-length compressed stream but "
                           "uncomp_len=%zu", uncomp_len);
    }

    uint8_t *out = malloc(uncomp_len);
    if (!out) {
        *err = make_err("SIT3: out of memory allocating %zu bytes for fork",
                        uncomp_len);
        return (peel_buf_t){0};
    }

    m3_bits_t bits = {.src = src, .len = len, .byte_pos = 0, .bit_pos = 0,
                      .ctx = &ctx};
    m3_tree_t tree = {.next_node = 0};

    int root = m3_read_node(&bits, &tree);
    m3_decode(&bits, &tree, root, out, uncomp_len);

    return (peel_buf_t){.data = out, .size = uncomp_len, .owned = true};
}
