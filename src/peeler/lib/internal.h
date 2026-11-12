// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// internal.h
// Shared internal helpers for libpeeler format implementations.
// This header is NOT part of the public API.

#ifndef PEELER_INTERNAL_H
#define PEELER_INTERNAL_H

#include "codec.h"
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

// Jump-target context for deep-error abort in container decoders -- and the
// owner of everything a decoder allocates while it is armed.
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
    codec_dec_t *dec; // a codec decoder the decode drives, freed with the rest (or NULL)
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
#define PEEL_MAX_DIR_DEPTH 128 // deepest folder nesting a walker follows

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
// Per-Format Detect Functions
// ============================================================================

// Each format source file defines its own detect function for the handler table.

bool hqx_detect(const uint8_t *src, size_t len);

bool bin_detect(const uint8_t *src, size_t len);

bool sit_detect(const uint8_t *src, size_t len);

bool cpt_detect(const uint8_t *src, size_t len);

bool zip_detect(const uint8_t *src, size_t len);

bool gz_detect(const uint8_t *src, size_t len);

bool tar_detect(const uint8_t *src, size_t len);

// ============================================================================
// Buffered reader over a source (reader.c)
// ============================================================================
//
// Formats parse their headers through this: a pointer to `n` bytes at an
// offset, served from a window of the source read ahead, so walking an
// archive's small headers costs a few large reads rather than many small
// ones -- and never a read of the whole archive.

#define PEEL_READER_WINDOW (128u * 1024u)

typedef struct {
    peel_source_t *src;
    uint64_t size; // the source's size
    uint8_t *buf; // PEEL_READER_WINDOW bytes
    uint64_t base; // source offset of buf[0]
    size_t len; // valid bytes in buf
} peel_reader_t;

// 0 or -12.  Does not retain `src`: the reader lives inside a call.
int peel_reader_init(peel_reader_t *r, peel_source_t *src);
void peel_reader_free(peel_reader_t *r);

// A pointer to the `n` bytes at `off` (n <= PEEL_READER_WINDOW), valid until
// the next call; NULL when they are not all inside the source or cannot be
// read.
const uint8_t *peel_reader_at(peel_reader_t *r, uint64_t off, size_t n);

// ============================================================================
// Structure-first archives (archive.c) -- the format vtable
// ============================================================================

// A producer emits a fork's decoded bytes in order: run() fills up to `cap`
// bytes and returns 1 when the fork is complete, 0 when more remains, or a
// negative code (with a message in `err`).  The decode-through source drives
// it as reads reach past what the sink already holds.
// A run() that ends neither the fork nor any bytes this many times in a
// row is taken to be stuck: a corrupt stream, never an endless loop.
#define PEEL_PRODUCER_MAX_IDLE 4096

typedef struct peel_producer {
    int (*run)(struct peel_producer *p, uint8_t *out, size_t cap, size_t *n);
    void (*free)(struct peel_producer *p);
    char err[256];
} peel_producer_t;

// Run a producer to the end into an owned buffer of exactly `len` bytes,
// then free it (always).  A producer that fails, or ends short or long,
// is an error: its own message, prefixed by `what` when that is given.
// The buffer APIs (peel_sit3, peel_cpt, ...) are this over the same
// producers the decode-through sources drive, so both decode one way.
peel_buf_t peel_producer_drain(peel_producer_t *p, uint64_t len, const char *what, peel_err_t **err);

// Every compressed fork decodes through the codec library (src/codec,
// codec.h): a producer over `len` packed bytes at `src` (borrowed: they must
// outlive it), through `stages` codecs in order -- each pulling the one
// before, so Compact Pro's fork is {CODEC_CPT_LZH, CODEC_CPT_RLE} -- the
// last making the fork's `out_len` bytes (with `flags`).  `cut`: a stream
// that decodes past out_len is cut there rather than refused (StuffIt's
// forks, whose decoders always stopped at the fork's size).  Messages are
// "<what>: <the codec's>".  NULL + *err if a decoder cannot be created.
// (codec_producer.c)
peel_producer_t *peel_codec_producer(const codec_id_t *stages, int nstages, uint32_t flags, const uint8_t *src,
                                     size_t len, uint64_t out_len, bool cut, const char *what, peel_err_t **err);

typedef struct peel_fmt peel_fmt_t;

struct peel_archive {
    const peel_fmt_t *fmt;
    peel_source_t *src; // retained
    const peel_sink_ops_t *sink;
    void *sink_ctx;
    peel_entry_t *entries;
    int count, cap;
    void *priv; // format-private state
    int refs; // the caller's handle plus every open decode-through fork
};

struct peel_fmt {
    peel_format_desc_t desc;
    // Parse structure only, appending entries (peel_archive_add).  0, or a
    // negative code with *err set.
    int (*open)(peel_archive_t *a, const peel_probe_t *p, peel_err_t **err);
    // Decode a whole fork into an owned buffer.  Error messages are the
    // format's own, the ones the buffer API has always reported.
    peel_buf_t (*decode)(peel_archive_t *a, int i, int fork, peel_err_t **err);
    // Optional: a streaming producer for a compressed fork (zip deflate,
    // gzip).  Absent, or returning NULL with no error: the fork decodes
    // whole through decode().
    peel_producer_t *(*producer)(peel_archive_t *a, int i, int fork, peel_err_t **err);
    // Optional: open a fork as a source of the format's own (BGZF's block
    // index).  NULL with no error: the generic view / decode-through paths.
    peel_source_t *(*open_fork)(peel_archive_t *a, int i, int fork, peel_err_t **err);
    // Optional: false when a fork's declared length is only a hint (a
    // gzip stream's tail ISIZE, which names the last member alone).  Such a
    // fork's decode-through source earns its true size with one full pass.
    bool (*len_exact)(peel_archive_t *a, int i, int fork);
    // Optional: finish an entry's layout before a fork opens (zip learns a
    // stored member's data offset from its local header).  0, or -1 + *err.
    int (*prepare)(peel_archive_t *a, int i, int fork, peel_err_t **err);
    void (*close)(peel_archive_t *a); // free priv
};

// Every format's vtable.
extern const peel_fmt_t peel_fmt_hqx, peel_fmt_bin, peel_fmt_gz, peel_fmt_sit, peel_fmt_cpt, peel_fmt_zip, peel_fmt_tar;

// Append an entry; returns it zeroed (data_off/rsrc_off = UINT64_MAX), or
// NULL on allocation failure.
peel_entry_t *peel_archive_add(peel_archive_t *a);

// Read exactly `len` bytes of the archive's source at `off` into a fresh
// buffer (NULL + *err on failure, `what` naming it in the message).
uint8_t *peel_archive_read(peel_archive_t *a, uint64_t off, uint64_t len, const char *what, peel_err_t **err);

// A source serving a producer's output through the archive's sink.  Takes
// ownership of `p`; retains `a`.
peel_source_t *peel_decode_source(peel_archive_t *a, const char *key, uint64_t len, peel_tier_t tier,
                                  peel_producer_t *p);

// The same, for a fork whose length `hint` is not known to be exact: its
// size() decodes the whole fork (into the sink) to learn the true length.
peel_source_t *peel_decode_source_unsized(peel_archive_t *a, const char *key, uint64_t hint, peel_tier_t tier,
                                          peel_producer_t *p);

// Build the key of fork `fork` of entry `i`: "<archive key>/<path>[/rsrc]".
void peel_fork_key(peel_archive_t *a, int i, int fork, char *out, size_t cap);

#endif // PEELER_INTERNAL_H
