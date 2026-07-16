// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpt.c — Compact Pro (.cpt) archive peeler.
//
// Format spec: docs/cpt.md

// Ensure strnlen is declared under strict C99 mode.
#define _POSIX_C_SOURCE 200809L

#include "internal.h"

// ============================================================================
// Constants and Macros
//
// cpt.md § 3.1 "Initial Archive Header" — CP_MAGIC / CP_VOLUME_SINGLE.
// cpt.md § 3.2.3 "Directory Entry — File" — CP_FLAG_RSRC_LZH / CP_FLAG_DATA_LZH / CP_DIR_MARKER.
// cpt.md § 6 "LZH" — CP_WIN_SIZE (8 KiB), CP_BLOCK_COST (0x1FFF0).
// ============================================================================

#define CP_MAGIC         0x01
#define CP_VOLUME_SINGLE 0x01
#define CP_FLAG_ENCRYPT  0x0001
#define CP_FLAG_RSRC_LZH 0x0002
#define CP_FLAG_DATA_LZH 0x0004
#define CP_DIR_MARKER    0x80

#define CP_WIN_SIZE   8192
#define CP_WIN_MASK   (CP_WIN_SIZE - 1)
#define CP_BLOCK_COST 0x1FFF0

#define CP_LIT_COUNT  256
#define CP_LEN_COUNT   64
#define CP_OFF_COUNT  128
#define CP_MAX_CODELEN 15

#define CP_HUFF_POOL_MAX 2048

// ============================================================================
// Byte-supplier callback type
//
// cpt.md § 9.1 "Memory Model" — all byte consumers (bit reader, RLE decoder) use
// this uniform int (*fn)(void *, int *) interface returning 1/0.
// ============================================================================

typedef int (*cp_getbyte_fn)(void *ctx, int *out);

// ============================================================================
// Accumulator-based MSB-first bit reader
//
// cpt.md § 6.2 "Bitstream Conventions" — bytes enter the high bits of a 32-bit accumulator;
// bits are consumed from the top.
// ============================================================================

// Accumulator-based MSB-first bit reader state.
typedef struct {
    uint32_t acc;        // accumulator holding bits in MSB-first order
    int      fill;       // number of valid bits in acc (top fill bits)
    cp_getbyte_fn src;   // callback to pull one byte
    void    *src_ctx;
    size_t   bytes_read; // total bytes consumed from source
    int      eof;        // source exhausted flag
} cp_bits_t;

// Initialize a bit reader from the given byte-supplier callback.
static void cp_bits_init(cp_bits_t *b, cp_getbyte_fn fn, void *ctx) {
    memset(b, 0, sizeof(*b));
    b->src = fn;
    b->src_ctx = ctx;
}

// Pull bytes into the accumulator until we have at least 'need' bits, or EOF.
// cpt.md § 6.2 "Bitstream Conventions" — demand-driven
// refill: bytes enter the high bits of the 32-bit accumulator.
static void cp_bits_refill(cp_bits_t *b, int need) {
    while (b->fill < need && !b->eof) {
        int byte_val;
        if (!b->src(b->src_ctx, &byte_val)) {
            b->eof = 1;
            return;
        }
        b->acc |= ((uint32_t)(byte_val & 0xFF)) << (24 - b->fill);
        b->fill += 8;
        b->bytes_read++;
    }
}

// Read n bits (1..25) from the accumulator, MSB-first. Returns 0 on underflow.
// cpt.md § 6.2 "Bitstream Conventions" — underflow
// returns zero-padded top bits and resets the accumulator to empty.
static unsigned cp_bits_get(cp_bits_t *b, int n) {
    if (n <= 0) return 0;
    cp_bits_refill(b, n);
    if (b->fill < n) {
        // not enough bits — return what we have, padded with zeros
        unsigned val = b->acc >> (32 - n);
        b->acc = 0;
        b->fill = 0;
        return val;
    }
    unsigned val = b->acc >> (32 - n);
    b->acc <<= n;
    b->fill -= n;
    return val;
}

// Check if at least 'n' bits are available.
// cpt.md § 6.2 "Bitstream Conventions" — triggers demand-driven refill, used throughout LZH to
// distinguish end-of-stream from valid data.
static int cp_bits_avail(cp_bits_t *b, int n) {
    cp_bits_refill(b, n);
    return b->fill >= n;
}

// Align to next byte boundary by discarding partial-byte bits.
static void cp_bits_align(cp_bits_t *b) {
    int discard = b->fill & 7;
    if (discard > 0) {
        b->acc <<= discard;
        b->fill -= discard;
    }
}

// Skip exactly n bits (in chunks of up to 25).
// cpt.md § 6.2 "Bitstream Conventions" — skip N bits,
// used by end-of-block flush to skip 2 or 3 padding bytes.
static void cp_bits_skip(cp_bits_t *b, int n) {
    while (n > 0) {
        int take = n < 25 ? n : 25;
        (void)cp_bits_get(b, take);
        n -= take;
    }
}

// Return total bytes consumed from source so far.
static size_t cp_bits_consumed(cp_bits_t *b) {
    return b->bytes_read;
}

// ============================================================================
// Pool-allocated Huffman tree
//
// cpt.md § 6.4.2 "Canonical Huffman Code Construction" — codes are built in canonical order
// (ascending code-length, then ascending symbol value within each
// length) and the in-tree traversal is MSB-first.
// ============================================================================

// Single node in a pool-allocated Huffman tree.
typedef struct {
    int child[2]; // indices into pool; -1 = unused
    int sym;      // >=0 when this is a leaf node
} cp_hnode_t;

// Pool-allocated Huffman decode tree.
typedef struct {
    cp_hnode_t pool[CP_HUFF_POOL_MAX];
    int        used; // next free index
    int        root; // root node index
} cp_htree_t;

// Allocate a new internal (non-leaf) node. Returns index or -1 on overflow.
static int cp_htree_alloc(cp_htree_t *t) {
    if (t->used >= CP_HUFF_POOL_MAX) return -1;
    int idx = t->used++;
    t->pool[idx].child[0] = -1;
    t->pool[idx].child[1] = -1;
    t->pool[idx].sym = -1;
    return idx;
}

// Build a canonical Huffman decode tree from code lengths.
// code_lens[i] = number of bits for symbol i (0 means symbol not present).
// Returns 0 on success, -1 on overflow.
//
// cpt.md § 6.4.2 "Canonical Huffman Code Construction"
// — canonical code assignment (ascending length, then ascending symbol),
// MSB-first tree insertion, pool-allocated nodes (2048 per tree).
static int cp_htree_build(cp_htree_t *t, const int *code_lens, int sym_count) {
    t->used = 0;
    t->root = cp_htree_alloc(t);
    if (t->root < 0) return -1;

    int code = 0;
    for (int len = 1; len <= CP_MAX_CODELEN; len++) {
        for (int sym = 0; sym < sym_count; sym++) {
            if (code_lens[sym] != len) continue;

            // Walk the tree for this code, creating nodes as needed.
            int node = t->root;
            for (int bp = len - 1; bp >= 0; bp--) {
                int bit = (code >> bp) & 1;
                if (t->pool[node].child[bit] < 0) {
                    int nidx = cp_htree_alloc(t);
                    if (nidx < 0) return -1;
                    t->pool[node].child[bit] = nidx;
                }
                node = t->pool[node].child[bit];
            }
            t->pool[node].sym = sym;
            code++;
        }
        code <<= 1;
    }
    return 0;
}

// Decode one symbol from the bit stream using tree walk.
// Returns the symbol value (>=0) or -1 on error/EOF.
//
// cpt.md § 6.4.3 "Decoding with a Binary Tree" — read one bit at a time, traverse
// left (0) or right (1) until a leaf is reached.
static int cp_htree_decode(cp_htree_t *t, cp_bits_t *bits) {
    int node = t->root;
    for (;;) {
        if (t->pool[node].sym >= 0)
            return t->pool[node].sym;
        if (!cp_bits_avail(bits, 1))
            return -1;
        int bit = (int)cp_bits_get(bits, 1);
        int next = t->pool[node].child[bit];
        if (next < 0) return -1;
        node = next;
    }
}

// ============================================================================
// Streaming LZH decoder (LZSS + Huffman, block-based)
//
// cpt.md § 6 "LZH — LZSS + Huffman Compression" — LZSS with an 8 KiB
// sliding window, Huffman-coded literals and match tokens, processed
// in blocks terminated by a cumulative cost counter.
// ============================================================================

// Streaming LZH decoder state (LZSS + Huffman, block-based).
typedef struct {
    cp_bits_t  bits;
    cp_htree_t lit_tree;
    cp_htree_t len_tree;
    cp_htree_t off_tree;
    int        tables_ok;     // nonzero when current block tables are built

    uint8_t    win[CP_WIN_SIZE];
    size_t     wpos;          // next write position in window

    unsigned   blk_cost;      // symbol cost counter for current block
    size_t     blk_byte_start;// byte offset at start of block data portion

    // Streaming match state (replaces pend_buf for correct overlapping).
    size_t     match_src;     // absolute source position for current match
    unsigned   match_rem;     // bytes remaining in current match
} cp_lzh_t;

// Initialize an LZH decoder from the given byte-supplier callback.
static void cp_lzh_init(cp_lzh_t *lz, cp_getbyte_fn fn, void *ctx) {
    memset(lz, 0, sizeof(*lz));
    cp_bits_init(&lz->bits, fn, ctx);
    memset(lz->win, 0, sizeof(lz->win));
}

// Read one Huffman code-length table from the bitstream.
// cpt.md § 6.4.1 "Table Serialization Format" — each table is encoded
// as a sequence of nibble-packed code lengths.
static int cp_lzh_read_table(cp_bits_t *bits, int *lens, int sym_count) {
    if (!cp_bits_avail(bits, 8)) return -1;
    unsigned nbytes = cp_bits_get(bits, 8);
    if (nbytes * 2u > (unsigned)sym_count) return -1;

    memset(lens, 0, (size_t)sym_count * sizeof(int));
    for (unsigned i = 0; i < nbytes; i++) {
        if (!cp_bits_avail(bits, 8)) return -1;
        unsigned v = cp_bits_get(bits, 8);
        lens[2 * i]     = (int)(v >> 4);
        lens[2 * i + 1] = (int)(v & 0x0F);
    }
    return 0;
}

// Build the three Huffman tables for a new block.
// cpt.md § 6.4.1 "Table Serialization Format" — three independent Huffman
// trees (literal, length, offset) are each built from nibble-packed
// code lengths.  Each tree gets its own 2048-node pool
// (cpt.md § 9.3 "Huffman Tree Pool Allocation").
static int cp_lzh_build_tables(cp_lzh_t *lz) {
    int lens[CP_LIT_COUNT]; // largest table

    if (cp_lzh_read_table(&lz->bits, lens, CP_LIT_COUNT) < 0) return -1;
    if (cp_htree_build(&lz->lit_tree, lens, CP_LIT_COUNT) < 0) return -1;

    if (cp_lzh_read_table(&lz->bits, lens, CP_LEN_COUNT) < 0) return -1;
    if (cp_htree_build(&lz->len_tree, lens, CP_LEN_COUNT) < 0) return -1;

    if (cp_lzh_read_table(&lz->bits, lens, CP_OFF_COUNT) < 0) return -1;
    if (cp_htree_build(&lz->off_tree, lens, CP_OFF_COUNT) < 0) return -1;

    lz->tables_ok = 1;
    lz->blk_cost = 0;
    lz->blk_byte_start = cp_bits_consumed(&lz->bits);
    return 0;
}

// End-of-block flush: align to byte and skip 2 or 3 bytes.
// cpt.md § 6.8 "End-of-Block Input Flush" — when a block's cumulative cost
// reaches CP_BLOCK_COST, the remaining bits in the current byte are
// discarded.  Even/odd byte parity determines whether an extra padding
// byte must also be skipped.
static void cp_lzh_flush_block(cp_lzh_t *lz) {
    cp_bits_align(&lz->bits);
    size_t consumed = cp_bits_consumed(&lz->bits) - lz->blk_byte_start;
    if (consumed & 1)
        cp_bits_skip(&lz->bits, 24); // skip 3 bytes
    else
        cp_bits_skip(&lz->bits, 16); // skip 2 bytes
    lz->tables_ok = 0;
}

// Produce the next decompressed byte from the LZH stream.
//
// cpt.md § 6.5 "Block Data — Decoding Literals and Matches" — symbols
// 0..255 are literals; 256+ encodes a match with a length/offset pair
// read from the match-length and match-offset Huffman trees.
// cpt.md § 6.6 "Overlapping Matches" —
// match source and destination ranges may overlap, requiring
// byte-by-byte copy.
//
// Returns 1 on success (byte written to *out), 0 on EOF, -1 on error.
static int cp_lzh_next(cp_lzh_t *lz, int *out) {
    // Continue emitting bytes from an in-progress match.
    if (lz->match_rem > 0) {
        uint8_t b = lz->win[lz->match_src & CP_WIN_MASK];
        lz->win[lz->wpos & CP_WIN_MASK] = b;
        lz->wpos++;
        lz->match_src++;
        lz->match_rem--;
        *out = b;
        return 1;
    }

    for (;;) {
        // Check block boundary.
        if (lz->tables_ok && lz->blk_cost >= CP_BLOCK_COST) {
            cp_lzh_flush_block(lz);
        }

        // Build tables for a new block if needed.
        if (!lz->tables_ok) {
            if (!cp_bits_avail(&lz->bits, 8))
                return 0; // end of compressed stream
            if (cp_lzh_build_tables(lz) < 0)
                return 0;
        }

        // Need at least one bit for the literal/match flag.
        if (!cp_bits_avail(&lz->bits, 1))
            return 0;

        unsigned flag = cp_bits_get(&lz->bits, 1);

        if (flag) {
            // Literal byte.
            int sym = cp_htree_decode(&lz->lit_tree, &lz->bits);
            if (sym < 0) return 0;

            uint8_t b = (uint8_t)sym;
            lz->win[lz->wpos & CP_WIN_MASK] = b;
            lz->wpos++;
            lz->blk_cost += 2;
            *out = b;
            return 1;
        } else {
            // Match.
            int mlen_sym = cp_htree_decode(&lz->len_tree, &lz->bits);
            if (mlen_sym < 0) return 0;
            int off_sym = cp_htree_decode(&lz->off_tree, &lz->bits);
            if (off_sym < 0) return 0;
            if (!cp_bits_avail(&lz->bits, 6)) return 0;
            unsigned lower6 = cp_bits_get(&lz->bits, 6);

            unsigned offset = ((unsigned)off_sym << 6) | lower6; // 1-based
            unsigned mlen = (unsigned)mlen_sym;
            if (mlen == 0) return 0;

            lz->blk_cost += 3;

            // Set up streaming match — emit first byte now, track remainder.
            // Source position is absolute; byte-by-byte emission ensures
            // overlapping matches (length > offset) are handled correctly.
            size_t src_start = lz->wpos - (size_t)offset;
            uint8_t first = lz->win[src_start & CP_WIN_MASK];
            lz->win[lz->wpos & CP_WIN_MASK] = first;
            lz->wpos++;

            if (mlen > 1) {
                lz->match_src = src_start + 1;
                lz->match_rem = mlen - 1;
            }

            *out = first;
            return 1;
        }
    }
}

// ============================================================================
// Memory-backed byte source
//
// cpt.md § 9.1 "Memory Model" — the entire archive is kept in memory so fork data can be
// accessed at arbitrary offsets; this adapter feeds bytes sequentially
// to the bit reader.
// ============================================================================

// Memory-backed byte source for sequential archive reads.
typedef struct {
    const uint8_t *base;
    size_t         pos;
    size_t         end;
} cp_memsrc_t;

// Set up a memory source over a byte range within the archive buffer.
static int cp_memsrc_init(cp_memsrc_t *m, const uint8_t *data, size_t archive_len,
                          size_t offset, size_t length) {
    if (!m || !data) return -1;
    if (offset > archive_len || length > archive_len - offset) return -1;
    m->base = data;
    m->pos = offset;
    m->end = offset + length;
    return 0;
}

// Supply the next byte from the memory source; returns 1 on success, 0 at end.
static int cp_memsrc_next(void *ctx, int *out) {
    cp_memsrc_t *m = (cp_memsrc_t *)ctx;
    if (m->pos >= m->end) return 0;
    *out = m->base[m->pos++];
    return 1;
}

// ============================================================================
// RLE decoder with half-escape handling
//
// cpt.md § 5 "RLE — Run-Length Encoding" — escape
// byte 0x81.  The N-2 rule (cpt.md § 5.5 "The N-2 Rule") and half-escape
// semantics (cpt.md § 5.4 "The Half-Escape Mechanism")
// are the two most subtle aspects.
// ============================================================================

// RLE decoder state with half-escape handling.
typedef struct {
    cp_getbyte_fn  src;
    void          *src_ctx;
    int            prev_byte;      // last emitted byte (for RLE runs)
    int            run_left;       // pending repeat count
    int            escape_pending; // injected 0x81 from half-escape
} cp_rle_t;

// Initialize an RLE decoder from the given byte-supplier callback.
static void cp_rle_init(cp_rle_t *r, cp_getbyte_fn fn, void *ctx) {
    memset(r, 0, sizeof(*r));
    r->src = fn;
    r->src_ctx = ctx;
}

// Read up to 'max' decompressed bytes into 'dst'.
// Returns bytes produced (0 on EOF, negative on error).
//
// cpt.md § 5.7 "Complete Decoder Algorithm" — main decode loop: drain pending
// run, inject phantom 0x81 if half-escaped, classify next byte.
// cpt.md § 5.4 "The Half-Escape Mechanism" —
// phantom 0x81 re-enters escape detection, consuming next stream byte.
// cpt.md § 5.5 "The N-2 Rule" — RLE count byte N produces: emit saved once
// now + max(0, N-2) additional copies.
static int cp_rle_read(cp_rle_t *r, uint8_t *dst, size_t max) {
    size_t written = 0;
    while (written < max) {
        // Drain pending run copies first.
        if (r->run_left > 0) {
            r->run_left--;
            dst[written++] = (uint8_t)r->prev_byte;
            continue;
        }

        // Get source byte — either from half-escape injection or from stream.
        int byte_val;
        if (r->escape_pending) {
            byte_val = 0x81;
            r->escape_pending = 0;
        } else {
            if (!r->src(r->src_ctx, &byte_val)) {
                return (int)written;
            }
        }

        if (byte_val != 0x81) {
            // Normal literal byte.
            r->prev_byte = byte_val;
            dst[written++] = (uint8_t)byte_val;
            continue;
        }

        // Escape start (0x81) — read next byte.
        int next;
        if (!r->src(r->src_ctx, &next)) {
            return (int)written;
        }

        if (next == 0x82) {
            // RLE run: 0x81 0x82 <count>
            int count;
            if (!r->src(r->src_ctx, &count)) {
                return (int)written;
            }
            if (count == 0) {
                // Literal 0x81 followed by 0x82.
                dst[written++] = 0x81;
                r->prev_byte = 0x82;
                r->run_left = 1; // emit 0x82 on next iteration
            } else {
                // Repeat prev_byte: emit once now + (count-2) more.
                dst[written++] = (uint8_t)r->prev_byte;
                r->run_left = (count >= 2) ? (count - 2) : 0;
            }
        } else if (next == 0x81) {
            // Half-escape (0x81 0x81): emit one literal 0x81, inject
            // a phantom 0x81 that re-enters the top of the loop and
            // may start another escape sequence.
            // See cpt.md § 5.4 "The Half-Escape Mechanism".
            dst[written++] = 0x81;
            r->prev_byte = 0x81;
            r->escape_pending = 1;
        } else {
            // Simple escape (0x81 <X>): literal 0x81 then X.
            dst[written++] = 0x81;
            r->prev_byte = next;
            r->run_left = 1; // emit 'next' on next iteration
        }
    }
    return (int)written;
}

// ============================================================================
// Fork stream: optional LZH -> mandatory RLE
//
// cpt.md § 2.2 "Compression Pipeline" — each fork is decompressed as:
//   LZH (if the per-fork flag is set) → RLE (always).
// ============================================================================

// Fork decompression stream: optional LZH chained into mandatory RLE.
typedef struct {
    int        use_lzh;
    cp_lzh_t   lzh;        // only used when use_lzh is set
    cp_memsrc_t memsrc;     // raw byte source from archive memory
    cp_rle_t    rle;
    size_t      remain;     // uncompressed bytes left to produce
    int         done;
} cp_fork_t;

// Adapter: pull one byte from the LZH decoder for the RLE layer.
static int cp_lzh_adapter(void *ctx, int *out) {
    return cp_lzh_next((cp_lzh_t *)ctx, out);
}

// Initialize a fork stream for RLE-only decompression.
static void cp_fork_init_rle(cp_fork_t *f, const uint8_t *archive, size_t archive_len,
                             size_t comp_offset, size_t comp_len, size_t uncomp_len) {
    memset(f, 0, sizeof(*f));
    f->use_lzh = 0;
    f->remain = uncomp_len;
    f->done = (uncomp_len == 0);
    cp_memsrc_init(&f->memsrc, archive, archive_len, comp_offset, comp_len);
    cp_rle_init(&f->rle, cp_memsrc_next, &f->memsrc);
}

// cpt.md § 9.5 "Fork Stream Composition" — LZH output is piped through
// an adapter callback into the RLE decoder.
static void cp_fork_init_lzh(cp_fork_t *f, const uint8_t *archive, size_t archive_len,
                             size_t comp_offset, size_t comp_len, size_t uncomp_len) {
    memset(f, 0, sizeof(*f));
    f->use_lzh = 1;
    f->remain = uncomp_len;
    f->done = (uncomp_len == 0);
    cp_memsrc_init(&f->memsrc, archive, archive_len, comp_offset, comp_len);
    cp_lzh_init(&f->lzh, cp_memsrc_next, &f->memsrc);
    cp_rle_init(&f->rle, cp_lzh_adapter, &f->lzh);
}

// cpt.md § 9.5 "Fork Stream Composition" — each fork reads decompressed
// bytes through the RLE decoder, counting down uncompressed remaining.
static int cp_fork_read(cp_fork_t *f, uint8_t *dst, size_t max) {
    if (f->done || f->remain == 0) return 0;
    if (max > f->remain) max = f->remain;
    int n = cp_rle_read(&f->rle, dst, max);
    if (n <= 0) { f->done = 1; return n; }
    f->remain -= (size_t)n;
    if (f->remain == 0) f->done = 1;
    return n;
}

// ============================================================================
// CPT directory entry (file)
//
// cpt.md § 3.2.3 "Directory Entry — File" — 45-byte metadata block
// per file after the name.  Fields are big-endian.
// cpt.md § 4.3 "Per-File Data CRC-32" — data_crc is stored but not validated.
// ============================================================================

// Parsed metadata for a single file entry in the directory.
typedef struct {
    char     name[256];
    uint8_t  volume;
    uint32_t file_offset;
    uint32_t type;
    uint32_t creator;
    uint32_t create_date;
    uint32_t mod_date;
    uint16_t finder_flags;
    uint32_t data_crc;
    uint16_t flags;
    uint32_t rsrc_uncomp;
    uint32_t data_uncomp;
    uint32_t rsrc_comp;
    uint32_t data_comp;
} cp_entry_t;

// ============================================================================
// Archive Context — holds parsed directory entries
// ============================================================================

// Growable list of parsed file entries from the directory.
typedef struct {
    cp_entry_t *entries;
    size_t      count;
    size_t      cap;
} cp_archive_t;

// ============================================================================
// Static Helpers — Directory Parsing
// ============================================================================

// Append a file entry to the archive's entry list, growing as needed.
static int cp_push_entry(cp_archive_t *ar, const cp_entry_t *e) {
    if (ar->count >= ar->cap) {
        size_t ncap = ar->cap ? ar->cap * 2 : 16;
        void *tmp = realloc(ar->entries, ncap * sizeof(cp_entry_t));
        if (!tmp) return -1;
        ar->entries = (cp_entry_t *)tmp;
        ar->cap = ncap;
    }
    ar->entries[ar->count++] = *e;
    return 0;
}

// Concatenate parent and name into dst (max 256).
// cpt.md § 3.3 "Directory Hierarchy" — paths are reconstructed by
// walking the recursive depth-first entry tree and concatenating segments.
static void cp_join_path(char dst[256], const char *parent, const char *seg, size_t seg_len) {
    size_t dp = 0;
    dst[0] = '\0';
    if (parent && parent[0]) {
        size_t plen = strnlen(parent, 255);
        memcpy(dst, parent, plen);
        dp = plen;
        // Append separator between parent directory and segment
        if (dp < 255) dst[dp++] = '/';
    }
    // Clamp segment length to remaining buffer space
    if (seg_len > 255 - dp) seg_len = 255 - dp;
    if (seg_len > 0) { memcpy(dst + dp, seg, seg_len); dp += seg_len; }
    dst[dp] = '\0';
}

// Recursively parse directory entries from in-memory archive data.
// cpt.md § 3.3 "Directory Hierarchy" — a directory entry with subtree
// count C is followed by C depth-first entries.  Consumes C+1 entries
// from the parent's remaining total.
static int cp_walk_entries(cp_archive_t *ar, const uint8_t *data, size_t size,
                           size_t *cursor, int remaining, const char *parent) {
    while (remaining > 0) {
        if (*cursor >= size) return -1;

        // cpt.md § 3.2.2 "Directory Entry — Directory" — high bit of
        // name-length byte marks a directory.
        uint8_t nl_flag = data[*cursor];
        int nlen = nl_flag & 0x7F;
        int is_dir = (nl_flag & CP_DIR_MARKER) != 0;

        if (*cursor + 1 + (size_t)nlen > size) return -1;

        // Extract entry name
        const char *raw_name = (const char *)(data + *cursor + 1);
        char seg[256];
        size_t cplen = (size_t)nlen < 255 ? (size_t)nlen : 255;
        memcpy(seg, raw_name, cplen);
        seg[cplen] = '\0';
        *cursor += 1 + (size_t)nlen;

        // Build full path by joining parent and segment
        char full[256];
        cp_join_path(full, parent, seg, cplen);

        if (is_dir) {
            // cpt.md § 3.2.2 — directory entry has 2-byte subtree count
            if (*cursor + 2 > size) return -1;
            uint16_t child_cnt = rd16be(data + *cursor);
            *cursor += 2;
            int rc = cp_walk_entries(ar, data, size, cursor, (int)child_cnt, full);
            if (rc < 0) return rc;
            remaining -= (int)child_cnt + 1;
            continue;
        }

        // cpt.md § 3.2.3 "Directory Entry — File" — 45 bytes of metadata
        // after the name: volume(1), file_offset(4), type(4), creator(4),
        // create_date(4), mod_date(4), finder_flags(2), data_crc(4),
        // flags(2), rsrc_uncomp(4), data_uncomp(4), rsrc_comp(4),
        // data_comp(4).
        if (*cursor + 45 > size) return -1;

        const uint8_t *m = data + *cursor;
        cp_entry_t fe;
        memset(&fe, 0, sizeof(fe));
        strncpy(fe.name, full, sizeof(fe.name) - 1);

        // Parse all 45 bytes of file metadata in field order
        size_t off = 0;
        fe.volume       = m[off]; off += 1;
        fe.file_offset  = rd32be(m + off); off += 4;
        fe.type         = rd32be(m + off); off += 4;
        fe.creator      = rd32be(m + off); off += 4;
        fe.create_date  = rd32be(m + off); off += 4;
        fe.mod_date     = rd32be(m + off); off += 4;
        fe.finder_flags = rd16be(m + off); off += 2;
        fe.data_crc     = rd32be(m + off); off += 4;
        fe.flags        = rd16be(m + off); off += 2;
        fe.rsrc_uncomp  = rd32be(m + off); off += 4;
        fe.data_uncomp  = rd32be(m + off); off += 4;
        fe.rsrc_comp    = rd32be(m + off); off += 4;
        fe.data_comp    = rd32be(m + off); off += 4;
        (void)off;

        // Add this file entry to the archive's entry list
        if (cp_push_entry(ar, &fe) < 0) return -1;

        *cursor += 45;
        remaining -= 1;
    }
    return 0;
}

// Parse the directory at the given offset.
// cpt.md § 3.2.1 "Second Header" — 4-byte CRC, 2-byte total entry count,
// 1-byte comment length, then the recursive entry tree.
static int cp_parse_directory(cp_archive_t *ar, const uint8_t *data,
                              size_t size, uint32_t dir_off) {
    if ((size_t)dir_off + 7 > size) return -1;

    // Skip the 4-byte directory CRC (not validated)
    uint16_t total = rd16be(data + dir_off + 4);
    uint8_t comment_len = data[dir_off + 6];
    size_t cursor = (size_t)dir_off + 7 + comment_len;
    if (cursor > size) return -1;

    return cp_walk_entries(ar, data, size, &cursor, (int)total, "");
}

// ============================================================================
// Static Helpers — Fork Decompression
// ============================================================================

// Decompress a single fork into an owned buffer.
// cpt.md § 2.2 "Compression Pipeline" — RLE-only forks go straight
// through the RLE decoder; LZH forks pass through LZH then RLE.
static peel_buf_t cp_decompress_fork(const uint8_t *archive, size_t archive_len,
                                     size_t comp_offset, size_t comp_len,
                                     size_t uncomp_len, bool use_lzh,
                                     decode_ctx_t *ctx) {
    // Set up the fork stream
    cp_fork_t fork;
    if (use_lzh) {
        cp_fork_init_lzh(&fork, archive, archive_len,
                         comp_offset, comp_len, uncomp_len);
    } else {
        cp_fork_init_rle(&fork, archive, archive_len,
                         comp_offset, comp_len, uncomp_len);
    }

    // Allocate output buffer to exact uncompressed size
    grow_buf_t out;
    grow_init(&out, uncomp_len, ctx);

    // Read decompressed bytes in chunks
    uint8_t chunk[8192];
    for (;;) {
        int n = cp_fork_read(&fork, chunk, sizeof(chunk));
        if (n <= 0) break;
        grow_append(&out, chunk, (size_t)n, ctx);
    }

    return grow_finish(&out);
}

// ============================================================================
// Operations (Public API) — Detection
// ============================================================================

// cpt.md § 3.1 "Initial Archive Header" — byte 0 is CP_MAGIC (0x01),
// byte 1 is CP_VOLUME_SINGLE (0x01), and bytes 4..7 hold the directory
// offset which must be at least 8 and no more than 256 MiB.
bool cpt_detect(const uint8_t *src, size_t len) {
    if (!src || len < 8) return false;

    // Check magic and volume bytes
    if (src[0] != CP_MAGIC || src[1] != CP_VOLUME_SINGLE) return false;

    // Validate directory offset is within sane bounds
    uint32_t dir_off = rd32be(src + 4);
    if (dir_off < 8 || dir_off > 0x10000000) return false;
    return true;
}

// ============================================================================
// Operations (Public API) — Archive Extraction
// ============================================================================

// Detect, parse, and extract all files from a Compact Pro archive.
// Returns a flat list of extracted files with both forks decompressed.
peel_file_list_t peel_cpt(const uint8_t *src, size_t len, peel_err_t **err) {
    *err = NULL;

    // Validate header
    if (!src || len < 8) {
        *err = make_err("CPT: input too short (%zu bytes)", len);
        return (peel_file_list_t){0};
    }
    if (src[0] != CP_MAGIC || src[1] != CP_VOLUME_SINGLE) {
        *err = make_err("CPT: bad magic (0x%02X 0x%02X)", src[0], src[1]);
        return (peel_file_list_t){0};
    }

    // cpt.md § 3.1 "Initial Archive Header" — directory offset at bytes 4..7
    uint32_t dir_off = rd32be(src + 4);
    if (dir_off < 8 || dir_off > 0x10000000 || (size_t)dir_off >= len) {
        *err = make_err("CPT: directory offset out of range (%u)", dir_off);
        return (peel_file_list_t){0};
    }

    // Parse directory into a flat entry list
    cp_archive_t ar;
    memset(&ar, 0, sizeof(ar));
    if (cp_parse_directory(&ar, src, len, dir_off) < 0) {
        free(ar.entries);
        *err = make_err("CPT: failed to parse directory");
        return (peel_file_list_t){0};
    }

    if (ar.count == 0) {
        free(ar.entries);
        return (peel_file_list_t){.files = NULL, .count = 0};
    }

    // Count entries that have at least one non-empty fork
    int file_count = 0;
    for (size_t i = 0; i < ar.count; i++) {
        const cp_entry_t *e = &ar.entries[i];
        if (e->data_uncomp > 0 || e->rsrc_uncomp > 0) {
            file_count++;
        }
    }

    // Allocate the output file array
    peel_file_t *files = calloc((size_t)file_count, sizeof(peel_file_t));
    if (!files) {
        free(ar.entries);
        *err = make_err("CPT: out of memory for %d files", file_count);
        return (peel_file_list_t){0};
    }

    // Use setjmp/longjmp for deep-error abort in decompressors
    decode_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (setjmp(ctx.jmp) != 0) {
        // Decompression failure — clean up and report
        for (int j = 0; j < file_count; j++) {
            peel_free(&files[j].data_fork);
            peel_free(&files[j].resource_fork);
        }
        free(files);
        free(ar.entries);
        *err = make_err("CPT: %s", ctx.errmsg);
        return (peel_file_list_t){0};
    }

    // Decompress each file's forks
    int fi = 0;
    for (size_t i = 0; i < ar.count && fi < file_count; i++) {
        const cp_entry_t *e = &ar.entries[i];

        // Skip entries with no non-empty forks
        if (e->data_uncomp == 0 && e->rsrc_uncomp == 0) continue;

        // Check for encrypted files (cpt.md § 3.2.3 — flag bit 0)
        if (e->flags & CP_FLAG_ENCRYPT) {
            decode_abort(&ctx, "file '%s' is encrypted (unsupported)", e->name);
        }

        peel_file_t *f = &files[fi];

        // Copy metadata
        strncpy(f->meta.name, e->name, sizeof(f->meta.name) - 1);
        f->meta.mac_type     = e->type;
        f->meta.mac_creator  = e->creator;
        f->meta.finder_flags = e->finder_flags;

        // cpt.md § 3.4 "Fork Data Layout" — resource fork at file_offset,
        // data fork at file_offset + rsrc_comp.
        size_t rsrc_offset = (size_t)e->file_offset;
        size_t data_offset = rsrc_offset + (size_t)e->rsrc_comp;

        // Validate fork data fits within the archive
        if (rsrc_offset + e->rsrc_comp > len) {
            decode_abort(&ctx, "resource fork of '%s' extends past archive", e->name);
        }
        if (data_offset + e->data_comp > len) {
            decode_abort(&ctx, "data fork of '%s' extends past archive", e->name);
        }

        // Decompress resource fork
        if (e->rsrc_uncomp > 0) {
            bool lzh = (e->flags & CP_FLAG_RSRC_LZH) != 0;
            f->resource_fork = cp_decompress_fork(
                src, len, rsrc_offset, e->rsrc_comp,
                e->rsrc_uncomp, lzh, &ctx);
        }

        // Decompress data fork
        if (e->data_uncomp > 0) {
            bool lzh = (e->flags & CP_FLAG_DATA_LZH) != 0;
            f->data_fork = cp_decompress_fork(
                src, len, data_offset, e->data_comp,
                e->data_uncomp, lzh, &ctx);
        }

        fi++;
    }

    free(ar.entries);
    return (peel_file_list_t){.files = files, .count = file_count};
}
