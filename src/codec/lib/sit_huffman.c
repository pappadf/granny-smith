// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// sit_huffman.c
// StuffIt method 3: static Huffman (see codec.h).  Format notes:
// src/peeler/docs/sit3.md.
//
// An MSB-first bit stream that starts with the code tree, serialized in
// pre-order -- bit 1: a leaf, followed by its 8-bit symbol; bit 0: an inner
// node, followed by its 0 child, then its 1 child -- and continues with the
// symbols, each a walk from the root one bit per branch.  A tree that is a
// single leaf codes its symbol as the empty string.  The container states
// the decoded size; the final byte's unused bits are padding.

#include "codec_internal.h"

#include <stdlib.h>

#define M3_MAX_NODES 512 // a full binary tree with 256 leaves has 511 nodes
#define M3_BURST     1

typedef struct {
    int16_t zero, one; // child indices, -1 for a leaf
    uint8_t symbol; // a leaf's symbol
} m3_node_t;

typedef struct {
    m3_node_t nodes[M3_MAX_NODES];
    int next_node;
    int root; // -1 until the tree is read
} m3_t;

// Read one node (and its subtrees) from the bit stream; its index, or a
// CODEC_ERR_* code.  `depth` bounds the recursion: a well-formed tree is
// at most 255 deep.
static int read_node(codec_dec_t *d, m3_t *t, int depth) {
    codec_in_t *in = &d->in;
    if (depth > 256)
        return codec_fail(d, CODEC_ERR_DATA, "sit-huffman: tree too deep");
    if (!msb_avail(in, 1))
        return codec_fail(d, CODEC_ERR_TRUNCATED, "sit-huffman: truncated tree");
    int bit = (int)msb_get(in, 1);
    if (t->next_node >= M3_MAX_NODES)
        return codec_fail(d, CODEC_ERR_DATA, "sit-huffman: tree exceeds 512 nodes");
    int idx = t->next_node++;
    t->nodes[idx] = (m3_node_t){-1, -1, 0};
    if (bit == 1) {
        if (!msb_avail(in, 8))
            return codec_fail(d, CODEC_ERR_TRUNCATED, "sit-huffman: truncated tree");
        t->nodes[idx].symbol = (uint8_t)msb_get(in, 8);
        return idx;
    }
    int zero = read_node(d, t, depth + 1);
    if (zero < 0)
        return zero;
    int one = read_node(d, t, depth + 1);
    if (one < 0)
        return one;
    t->nodes[idx].zero = (int16_t)zero;
    t->nodes[idx].one = (int16_t)one;
    return idx;
}

static int m3_fill(codec_dec_t *d) {
    m3_t *t = d->st;
    codec_in_t *in = &d->in;
    codec_ring_t *r = &d->ring;
    if (codec_left(d) == 0)
        return CODEC_END; // nothing to decode: no bits are consumed
    if (t->root < 0) {
        int root = read_node(d, t, 0);
        if (root < 0)
            return root;
        t->root = root;
    }
    const m3_node_t *nodes = t->nodes;
    if (nodes[t->root].zero == -1) {
        // A single leaf: its symbol, as many times as the size says.
        size_t k = ring_room(r) < codec_left(d) ? ring_room(r) : (size_t)codec_left(d);
        ring_fill(r, nodes[t->root].symbol, k);
        return codec_left(d) == 0 ? CODEC_END : CODEC_MORE;
    }
    while (codec_left(d) > 0) {
        if (ring_room(r) < M3_BURST)
            return CODEC_MORE;
        int n = t->root;
        while (nodes[n].zero != -1) {
            if (!msb_avail(in, 1))
                return codec_fail(d, CODEC_ERR_TRUNCATED, "sit-huffman: the stream ends short of its size");
            n = msb_get(in, 1) ? nodes[n].one : nodes[n].zero;
        }
        ring_put(r, nodes[n].symbol);
    }
    return CODEC_END;
}

static int m3_init(codec_dec_t *d) {
    m3_t *t = calloc(1, sizeof(*t));
    if (!t)
        return CODEC_ERR_NOMEM;
    t->root = -1;
    d->st = t;
    return codec_ring_alloc(d, 0, M3_BURST);
}

static void m3_free(codec_dec_t *d) {
    free(d->st);
    d->st = NULL;
}

const codec_dec_ops_t codec_sit_huffman_dec = {m3_init, m3_fill, m3_free, true};
