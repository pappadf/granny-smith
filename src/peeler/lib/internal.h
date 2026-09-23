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
// flight -- which every decoder that longjmp'd used to (09-storage F-10,
// F-11, F-12: sit15 its decoder state and ~80 MiB of block buffers, sit3 its
// output, hqx a finished data fork when the resource fork failed).
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
// not at all (09-storage F-08, F-65).  These caps bound what an archive may
// declare, stated once and enforced by every format.  They sit far above any
// classic Mac file -- a CD-ROM image is ~700 MB -- and far below the 4 GiB a
// 32-bit size_t can address, so no buffer built under them can overflow its
// own length arithmetic (which is also what closes F-14).
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
// separator or a traversal component into one (09-storage F-13).
void peel_append_segment(char *dst, size_t cap, size_t *pos, const uint8_t *name, size_t n);

// ============================================================================
// Extent Checks
// ============================================================================

// Does [off, off + len) lie inside a buffer of `total` bytes?  Wrap-safe:
// never forms off + len, which a 32-bit size_t (the wasm32 build) wraps for
// lengths an archive can simply claim.  Every format locates its forks with
// lengths read from the archive; this is the one check they all use
// (09-storage F-15).
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
