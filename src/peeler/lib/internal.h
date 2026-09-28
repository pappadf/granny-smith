// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// internal.h
// Shared internal helpers for libpeeler format implementations.
// This header is NOT part of the public API.

#ifndef PEELER_INTERNAL_H
#define PEELER_INTERNAL_H

#include "peeler.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Error Creation — architecture.md § "Internal Error Creation"
// ============================================================================

// Allocate and populate a peel_err_t with a printf-style message.
peel_err_t *make_err(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// ============================================================================
// setjmp/longjmp Abort Context — architecture.md § "setjmp/longjmp"
// ============================================================================

// Jump-target context for deep-error abort in decompressors -- and the owner
// of everything a decoder allocates while it is armed.
//
// A decoder allocates through dctx_* and registers each block here.  Both of
// its exits call dctx_cleanup, which frees whatever is still registered: the
// abort handler, and the success path after releasing (dctx_release) what it
// returns.
// So an abort can never leak, however deep it fires or whatever was in
// flight -- which every decoder that longjmp'd used to (sit15 its decoder
// state and ~80 MiB of block buffers, sit3 its output, hqx a finished data
// fork when the resource fork failed).
//
// It also removes a setjmp trap: a local assigned after setjmp has an
// indeterminate value after longjmp unless it is volatile, so a handler that
// frees `s` or `out` directly may free garbage.  The handler here reads only
// the context, whose address has escaped into every callee.
#define DCTX_INLINE_OWNED 16

typedef struct {
    jmp_buf jmp;
    char errmsg[256];
    void **owned; // inline_owned, or a heap array once that fills
    int n_owned, cap_owned;
    void *inline_owned[DCTX_INLINE_OWNED];
} decode_ctx_t;

// Arm a context: no owned blocks.  Call before setjmp.
void dctx_init(decode_ctx_t *ctx);

// Allocate and register.  Abort through ctx on failure (so never NULL).
void *dctx_malloc(decode_ctx_t *ctx, size_t size);
void *dctx_calloc(decode_ctx_t *ctx, size_t n, size_t size);

// Resize a registered block, keeping it registered.  Abort on failure.
void *dctx_realloc(decode_ctx_t *ctx, void *p, size_t size);

// A registered block was reallocated behind the context's back (a shrink
// that must not abort on failure): follow it from `old` to `now`.
void dctx_rebind(decode_ctx_t *ctx, void *old, void *now);

// Free a registered block now.  NULL is a no-op.
void dctx_free(decode_ctx_t *ctx, void *p);

// Hand a registered block to the caller: it is no longer the context's to
// free.  Returns p.  NULL is a no-op.
void *dctx_release(decode_ctx_t *ctx, void *p);

// Free every block still registered, and the registry itself.  Every exit
// calls it: the abort handler, and the success path once it has released
// what it returns.
void dctx_cleanup(decode_ctx_t *ctx);

// Format a message into ctx->errmsg and longjmp back to the setjmp site.
void decode_abort(decode_ctx_t *ctx, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((noreturn, format(printf, 2, 3)))
#endif
    ;

// ============================================================================
// Big-Endian Read Helpers
// ============================================================================

// Read a big-endian 16-bit unsigned integer from a byte pointer.
static inline uint16_t rd16be(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);
}

// Read a big-endian 32-bit unsigned integer from a byte pointer.
static inline uint32_t rd32be(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

// ============================================================================
// Limits -- the one place peeler's size and depth caps live
// ============================================================================

// Every format declares its output sizes in its own headers, and every
// decoder used to allocate whatever a header said: a few hundred bytes of
// .hqx could ask for a 4 GiB fork, and each format bounded it differently or
// not at all.  These caps bound what an archive may declare, stated once and
// enforced by every format.  They sit far above any classic Mac file -- a
// CD-ROM image is ~700 MB -- and far below the 4 GiB a 32-bit size_t can
// address, so no buffer built under them can overflow its own length
// arithmetic.
#define PEEL_MAX_FORK      ((uint64_t)1 << 30) // largest fork an archive may declare
#define PEEL_MAX_INPUT     ((uint64_t)1 << 30) // largest file peel_read_file loads
#define PEEL_MAX_DIR_DEPTH 128                 // deepest folder nesting a walker follows

// ============================================================================
// Entry Names
// ============================================================================

// Append one Mac name (n raw bytes) to a path being built in dst[cap] at *pos,
// preceded by '/' if *pos > 0, made safe as one path component (peeler.h,
// peel_file_meta_t.name): '/' and NUL become ':', a dots-only name gets a '_'
// prefix, an empty one becomes "_".  Truncates at cap; always terminates.
// Every format builds its names through this, so no archive can smuggle a
// separator or a traversal component into one.
void peel_append_segment(char *dst, size_t cap, size_t *pos, const uint8_t *name, size_t n);

// ============================================================================
// Extent Checks
// ============================================================================

// Does [off, off + len) lie inside a buffer of `total` bytes?  Wrap-safe:
// never forms off + len, which a 32-bit size_t (the wasm32 build) wraps for
// lengths an archive can simply claim.  Every format locates its forks with
// lengths read from the archive; this is the one check they all use.
static inline bool peel_extent_fits(size_t off, uint64_t len, size_t total) {
    return off <= total && len <= (uint64_t)(total - off);
}

// ============================================================================
// Big-Endian Write Helpers
// ============================================================================

// Write a 16-bit value in big-endian byte order.
static inline void wr16be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

// Write a 32-bit value in big-endian byte order.
static inline void wr32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

// ============================================================================
// CRC Routines
// ============================================================================

// CRC-16/CCITT (polynomial 0x1021, init 0) over a complete buffer.
uint16_t crc16_ccitt(const uint8_t *data, size_t len);

// Update a running CRC-16/CCITT with additional data.
uint16_t crc16_ccitt_update(uint16_t crc, const uint8_t *data, size_t len);

// ============================================================================
// Bit readers
// ============================================================================
//
// Every bit reader in peeler, one per bit order.  The order is the file
// format's, not a choice: StuffIt methods 3 and 15 and Compact Pro pack bits
// most significant first; StuffIt method 13 and method 2 (LZW) least
// significant first.  Each format had its own reader, with three different
// behaviours at the end of the input; now both readers behave one way --
// bits past the end read as zeros -- and a format that must refuse a short
// stream asks peel_*_avail first.
//
// Both refill on demand, pulling only the bytes a read needs, so
// peel_msb_pulled is exactly the input bytes consumed so far: Compact Pro's
// end-of-block padding is computed from it.

// MSB-first: bytes enter the top of a 32-bit window; a read takes its top
// bits.  At most 25 bits per read.
typedef struct {
    const uint8_t *src;
    size_t len;
    size_t pos; // next byte to pull into the window
    uint32_t window; // valid bits left-aligned
    int fill; // valid bits in window
} peel_msb_t;

static inline void peel_msb_init(peel_msb_t *r, const uint8_t *src, size_t len) {
    r->src = src;
    r->len = len;
    r->pos = 0;
    r->window = 0;
    r->fill = 0;
}

static inline void peel_msb_refill(peel_msb_t *r, int need) {
    while (r->fill < need && r->pos < r->len) {
        r->window |= (uint32_t)r->src[r->pos++] << (24 - r->fill);
        r->fill += 8;
    }
}

// True if at least n bits (n <= 25) remain.
static inline bool peel_msb_avail(peel_msb_t *r, int n) {
    peel_msb_refill(r, n);
    return r->fill >= n;
}

// The next n bits (0..25); past the end, zeros -- and the reader is then
// empty.
static inline uint32_t peel_msb_get(peel_msb_t *r, int n) {
    if (n <= 0)
        return 0;
    peel_msb_refill(r, n);
    uint32_t v = r->window >> (32 - n);
    if (r->fill < n) {
        r->window = 0;
        r->fill = 0;
        return v;
    }
    r->window <<= n;
    r->fill -= n;
    return v;
}

// Discard the rest of the current byte.
static inline void peel_msb_align(peel_msb_t *r) {
    int discard = r->fill & 7;
    r->window <<= discard;
    r->fill -= discard;
}

static inline void peel_msb_skip(peel_msb_t *r, int n) {
    for (; n > 0; n -= 25)
        (void)peel_msb_get(r, n < 25 ? n : 25);
}

// Input bytes pulled into the window so far.
static inline size_t peel_msb_pulled(const peel_msb_t *r) {
    return r->pos;
}

// LSB-first: bytes enter above the bits already held; a read takes the low
// bits.  At most 24 bits per read.
typedef struct {
    const uint8_t *src;
    size_t len;
    size_t pos; // next byte to pull
    uint32_t acc; // valid bits at the bottom
    int fill; // valid bits in acc
} peel_lsb_t;

static inline void peel_lsb_init(peel_lsb_t *r, const uint8_t *src, size_t len) {
    r->src = src;
    r->len = len;
    r->pos = 0;
    r->acc = 0;
    r->fill = 0;
}

// The next n bits (0..24); past the end, zeros -- and the reader is then
// empty.
static inline uint32_t peel_lsb_get(peel_lsb_t *r, int n) {
    if (n <= 0)
        return 0;
    while (r->fill < n && r->pos < r->len) {
        r->acc |= (uint32_t)r->src[r->pos++] << r->fill;
        r->fill += 8;
    }
    uint32_t v = r->acc & ((1u << n) - 1);
    if (r->fill < n) {
        r->acc = 0;
        r->fill = 0;
        return v;
    }
    r->acc >>= n;
    r->fill -= n;
    return v;
}

static inline void peel_lsb_skip(peel_lsb_t *r, size_t n) {
    for (; n > 24; n -= 24)
        (void)peel_lsb_get(r, 24);
    (void)peel_lsb_get(r, (int)n);
}

// Bits consumed so far.
static inline uint64_t peel_lsb_consumed(const peel_lsb_t *r) {
    return (uint64_t)r->pos * 8 - (uint64_t)r->fill;
}

// True once every input bit has been consumed.
static inline bool peel_lsb_at_end(const peel_lsb_t *r) {
    return peel_lsb_consumed(r) >= (uint64_t)r->len * 8;
}

// ============================================================================
// Canonical Huffman trees (sit13, cpt)
// ============================================================================
//
// One pool-allocated decode tree for the two formats that build canonical
// Huffman codes; each had its own, and only Compact Pro's bounded its pool.
// A pool holds one or more trees (sit13 keeps four in one).  Formats keep
// their own bit readers and walk a tree with peel_huff_child / peel_huff_sym.

#define PEEL_HUFF_POOL_CAP 2048
#define PEEL_HUFF_NOSYM    ((int16_t)-1)

typedef struct {
    int16_t ch[2]; // child node indices, or -1
    int16_t sym; // leaf symbol, or PEEL_HUFF_NOSYM for an inner node
} peel_hnode_t;

typedef struct {
    peel_hnode_t node[PEEL_HUFF_POOL_CAP];
    int used;
} peel_hpool_t;

// Empty the pool.
void peel_hpool_reset(peel_hpool_t *p);

// A new, empty tree root in `p`: its index, or -1 if the pool is full.
int peel_huff_root(peel_hpool_t *p);

// Place `sym` at the `len`-bit code `code` (MSB first) under `root`.  0, or
// -1 if the pool is full or len is outside 1..31.
int peel_huff_insert(peel_hpool_t *p, int root, uint32_t code, int len, int sym);

// Build a canonical code for lengths[0..nsym) into a new tree in `p`, and
// return its root, or -1 (pool full, or a length that is neither 0 --
// absent -- nor within [min_len, max_len]).  Codes go in ascending length, then ascending
// symbol, over every length from min_len to max_len; only lengths >= 1 are
// placed in the tree.  With min_len 1 that is the usual construction
// (Compact Pro).  StuffIt 13 uses min_len -1: its absent symbols (-1 and 0)
// still consume a code value each, which shifts every longer code.
int peel_huff_build(peel_hpool_t *p, const int8_t *lengths, int nsym, int min_len, int max_len);

// One step down from `node` on `bit`: the child's index, or -1.
static inline int peel_huff_child(const peel_hpool_t *p, int node, int bit) {
    return p->node[node].ch[bit & 1];
}

// The symbol at `node` if it is a leaf, else PEEL_HUFF_NOSYM.
static inline int peel_huff_sym(const peel_hpool_t *p, int node) {
    return p->node[node].sym;
}

// ============================================================================
// Growable Buffer
// ============================================================================

// A dynamically growing output buffer for building results incrementally.
// Its storage is registered with the decode context it was created against,
// so an abort frees it; grow_finish leaves it registered, and the decoder
// releases it (dctx_release) once the whole decode has succeeded.
typedef struct {
    uint8_t *data; // Heap-allocated storage, owned by ctx
    size_t len; // Number of valid bytes written
    size_t cap; // Allocated capacity in bytes
    decode_ctx_t *ctx;
} grow_buf_t;

// Initialise a growable buffer with the given initial capacity.
// Aborts via ctx on allocation failure.
void grow_init(grow_buf_t *g, size_t initial_cap, decode_ctx_t *ctx);

// Append n bytes to the growable buffer, reallocating if needed.
void grow_append(grow_buf_t *g, const uint8_t *src, size_t n, decode_ctx_t *ctx);

// Append a single byte.
void grow_push(grow_buf_t *g, uint8_t byte, decode_ctx_t *ctx);

// Finalise the growable buffer into a peel_buf_t.  Zeroes g.  The data is
// still owned by the context: release it when the decode has succeeded.
peel_buf_t grow_finish(grow_buf_t *g);

// Release a growable buffer without producing a peel_buf_t (for error paths).
void grow_free(grow_buf_t *g);

// ============================================================================
// Format Handler Registration — architecture.md § "Format Handler Registration"
// ============================================================================

// Classification of a format handler.
typedef enum {
    PEEL_FMT_WRAPPER, // One buffer in, one buffer out (e.g. HQX, MacBinary)
    PEEL_FMT_ARCHIVE, // One buffer in, file list out  (e.g. StuffIt, CPT)
} peel_fmt_kind_t;

// A registered format handler entry in the detection table.
typedef struct {
    const char *name;
    peel_fmt_kind_t kind;
    bool (*detect)(const uint8_t *src, size_t len);
    peel_buf_t (*peel_wrapper)(const uint8_t *src, size_t len, peel_err_t **err);
    peel_file_list_t (*peel_archive)(const uint8_t *src, size_t len, peel_err_t **err);
} peel_format_t;

// ============================================================================
// Per-Format Detect Functions
// ============================================================================

// Each format source file defines its own detect function for the handler table.

bool hqx_detect(const uint8_t *src, size_t len);

bool bin_detect(const uint8_t *src, size_t len);

bool sit_detect(const uint8_t *src, size_t len);

bool cpt_detect(const uint8_t *src, size_t len);

#endif // PEELER_INTERNAL_H
