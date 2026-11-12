// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// sit_lzw.c
// StuffIt method 2: LZW (see codec.h).  Format notes:
// src/peeler/docs/sit.md § 9.
//
// compress(1)-style LZW over an LSB-first bit stream: codes start 9 bits
// wide and widen by one each time the dictionary reaches a power of two,
// up to 14 bits (16 384 entries).  Code 256 clears the dictionary; after
// it, the codes up to the end of the current group of eight are padding.
// Entries 0..255 are the bytes, new entries start at 257, and a code may
// name the entry about to be made (the KwKwK case: the previous string
// plus its own first byte).  The container states the decoded size.

#include "codec_internal.h"

#include <stdlib.h>

// ============================================================================
// Constants
// ============================================================================

#define LZW_MAX_BITS   14
#define LZW_TABLE_CAP  (1 << LZW_MAX_BITS)
#define LZW_CLEAR_CODE 256
#define LZW_FIRST_NEW  257
#define LZW_BURST      LZW_TABLE_CAP // the longest string an entry expands to

// ============================================================================
// Types
// ============================================================================

// The dictionary, as arrays: each entry is its parent entry plus one byte.
typedef struct {
    uint16_t prev_code[LZW_TABLE_CAP]; // the parent entry (UINT16_MAX for a byte)
    uint8_t suffix[LZW_TABLE_CAP]; // the byte this entry appends
    uint8_t head[LZW_TABLE_CAP]; // its string's first byte
    uint16_t chain_len[LZW_TABLE_CAP]; // its string's length
    int tbl_next; // the next free entry
    int code_bits; // the current code width
    int prev; // the previous code (-1: none since a clear)
    int block_count; // codes read since the last clear
    uint8_t stage[LZW_TABLE_CAP]; // a string expanded, last byte first
} lzw_t;

// ============================================================================
// The dictionary
// ============================================================================

// Back to the 256 byte entries.
static void lzw_clear(lzw_t *z) {
    z->tbl_next = LZW_FIRST_NEW;
    z->code_bits = 9;
    z->prev = -1;
    z->block_count = 0;
}

// Add the entry prev + first_byte, widening the code at a power of two.
static void lzw_add(lzw_t *z, int prev, uint8_t first_byte) {
    if (z->tbl_next >= LZW_TABLE_CAP)
        return; // full: no more entries until a clear
    int idx = z->tbl_next++;
    z->prev_code[idx] = (uint16_t)prev;
    z->suffix[idx] = first_byte;
    z->head[idx] = z->head[prev];
    z->chain_len[idx] = (uint16_t)(z->chain_len[prev] + 1);
    if (z->tbl_next < LZW_TABLE_CAP && (z->tbl_next & (z->tbl_next - 1)) == 0 && z->code_bits < LZW_MAX_BITS)
        z->code_bits++;
}

// Append entry `code`'s string to the ring (walked from its last byte).
// `extra` >= 0: then that byte after it (the KwKwK case).
static void lzw_emit(codec_ring_t *r, lzw_t *z, int code, int extra) {
    int len = z->chain_len[code], pos = len;
    for (int cur = code; cur != (int)UINT16_MAX && pos > 0; cur = z->prev_code[cur])
        z->stage[--pos] = z->suffix[cur];
    ring_write(r, z->stage + pos, (size_t)(len - pos));
    if (extra >= 0)
        ring_put(r, (uint8_t)extra);
}

// ============================================================================
// The decoder
// ============================================================================

static int lzw_fill(codec_dec_t *d) {
    lzw_t *z = d->st;
    codec_in_t *in = &d->in;
    codec_ring_t *r = &d->ring;
    while (codec_left(d) > 0) {
        if (ring_room(r) < LZW_BURST + 1)
            return CODEC_MORE;
        if (!lsb_avail(in, 1))
            return codec_fail(d, CODEC_ERR_TRUNCATED, "sit-lzw: the stream ends short of its size");
        int code = (int)lsb_get(in, z->code_bits); // a code past the end reads its missing bits as zero
        z->block_count++;
        if (code == LZW_CLEAR_CODE) {
            // The rest of this group of eight codes is padding.
            if (z->block_count & 7)
                for (int k = z->code_bits * (8 - (z->block_count & 7)); k > 0; k -= 16)
                    lsb_get(in, k < 16 ? k : 16);
            lzw_clear(z);
            continue;
        }
        if (z->prev < 0) {
            // The first code after a clear: a byte (the dictionary is empty).
            if (code >= 256)
                return codec_fail(d, CODEC_ERR_DATA, "sit-lzw: a string code before any string");
            ring_put(r, (uint8_t)code);
            z->prev = code;
            continue;
        }
        // Only the next free entry may be named before it exists (KwKwK).
        if (code > z->tbl_next)
            return codec_fail(d, CODEC_ERR_DATA, "sit-lzw: a code past the dictionary");
        bool kwkwk = code == z->tbl_next;
        uint8_t first = kwkwk ? z->head[z->prev] : z->head[code];
        int prev = z->prev;
        lzw_add(z, prev, first);
        if (!kwkwk)
            lzw_emit(r, z, code, -1);
        else
            lzw_emit(r, z, prev, first);
        z->prev = code;
    }
    return CODEC_END;
}

static int lzw_init(codec_dec_t *d) {
    lzw_t *z = calloc(1, sizeof(*z));
    if (!z)
        return CODEC_ERR_NOMEM;
    d->st = z;
    for (int i = 0; i < 256; i++) {
        z->prev_code[i] = UINT16_MAX;
        z->suffix[i] = (uint8_t)i;
        z->head[i] = (uint8_t)i;
        z->chain_len[i] = 1;
    }
    lzw_clear(z);
    return codec_ring_alloc(d, 0, LZW_BURST + 1);
}

static void lzw_free(codec_dec_t *d) {
    free(d->st);
    d->st = NULL;
}

const codec_dec_ops_t codec_sit_lzw_dec = {lzw_init, lzw_fill, lzw_free, true};
