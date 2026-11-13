// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// internal.h
// Shared internal helpers for libpeeler format implementations.
// This header is NOT part of the public API.

#ifndef PEELER_INTERNAL_H
#define PEELER_INTERNAL_H

#include "codec.h"
#include "peeler.h"

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

// A compressed fork streams through one type: the codec library's decoder
// (codec.h, codec_dec_t).  Run with a buffer of any size it fills as far as
// it can and stops there; the decode-through source drives it as reads reach
// past what the sink already holds.  A container's own checks and chains are
// the decoder's options -- zip's CRC-32 and StuffIt's CRC-16 are
// codec_dec_opts_t.check, StuffIt's "the fork's size wins" is .exact,
// Compact Pro's RLE over LZH is codec_dec_stack, gzip's members are
// CODEC_GZIP -- so no format needs a stream type of its own.

// A decoder over `len` packed bytes of `src` at `off`, pulled as it needs
// them (the source is retained, and released with the decoder).  `opts`
// gives the rest; its input fields are ignored.  NULL + *err ("<what>: ...")
// if it cannot be created.  (fork_decoder.c)
codec_dec_t *peel_fork_decoder(peel_source_t *src, uint64_t off, uint64_t len, codec_id_t id,
                               const codec_dec_opts_t *opts, const char *what, peel_err_t **err);

// Run a decoder to the end into an owned buffer of exactly `len` bytes, then
// free it (always).  A decoder that fails, or ends short or long, is an
// error: its own message, prefixed by `what` when that is given.  The buffer
// APIs are this over the same decoders the decode-through sources drive, so
// both decode one way.
peel_buf_t peel_decoder_drain(codec_dec_t *d, uint64_t len, const char *what, peel_err_t **err);

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
    // Optional: a streaming decoder for a compressed fork (StuffIt, Compact
    // Pro, zip deflate, gzip).  Absent, or returning NULL with no error: the
    // fork decodes whole through decode().
    codec_dec_t *(*decoder)(peel_archive_t *a, int i, int fork, peel_err_t **err);
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

// Build the key of fork `fork` of entry `i`: "<archive key>/<path>[/rsrc]".
void peel_fork_key(peel_archive_t *a, int i, int fork, char *out, size_t cap);

#endif // PEELER_INTERNAL_H
