// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// peeler.h
// Public API for libpeeler — a C99 library for peeling apart classic
// Macintosh archive formats.

#ifndef PEELER_H
#define PEELER_H

#ifdef __cplusplus
extern "C" {
#endif

// === Includes ===

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// === Error Handling ===

// Opaque error object.  NULL means no error.
typedef struct peel_err peel_err_t;

// Return the human-readable error message, or a generic fallback for NULL.
const char *peel_err_msg(const peel_err_t *err);

// Free an error object.  Safe to call with NULL.
void peel_err_free(peel_err_t *err);

// === Byte Buffer ===

// A contiguous byte buffer with explicit ownership tracking.
// Created by the library, freed by the caller via peel_free().
typedef struct {
    uint8_t *data; // Pointer to contents (NULL when size == 0)
    size_t size; // Number of valid bytes
    bool owned; // If true, peel_free() will release data
} peel_buf_t;

// Free the data inside a buffer (if owned) and zero the struct.
void peel_free(peel_buf_t *buf);

// === File Metadata ===

// Metadata for a single file extracted from an archive.
// Fields are best-effort; zeroed when the format does not provide them.
typedef struct {
    // Relative path of the entry, '/' between folder levels -- or just the
    // file's name, for single-file formats.  Each component is a Mac name made
    // safe to write under a directory: a '/' inside a name (legal on HFS)
    // becomes ':', the macOS convention, so it cannot create a separator;
    // a component of only dots ('.', '..', legal Mac names) gets a '_' prefix;
    // an empty one becomes '_'.  So peeler's own names always pass
    // peel_path_is_confined -- but check anyway before writing (below).
    char name[256];
    uint32_t mac_type; // Classic Mac file type  (e.g. 'TEXT')
    uint32_t mac_creator; // Classic Mac creator    (e.g. 'ttxt')
    uint16_t finder_flags; // Finder flags
} peel_file_meta_t;

// === Extracted File ===

// A single file with both forks.  Unused forks have size == 0.
typedef struct {
    peel_file_meta_t meta;
    peel_buf_t data_fork;
    peel_buf_t resource_fork;
} peel_file_t;

// === File List ===

// Flat list of files produced by archive extraction.
typedef struct {
    peel_file_t *files; // Heap-allocated array of extracted files
    int count; // Number of entries
} peel_file_list_t;

// Free every buffer in every file, then the array itself.  Zeroes the struct.
void peel_file_list_free(peel_file_list_t *list);

// === Input Helpers ===

// Read an entire file into an owned buffer.
peel_buf_t peel_read_file(const char *path, peel_err_t **err);

// Copy caller data into a new owned buffer.
peel_buf_t peel_buf_copy(const void *src, size_t len, peel_err_t **err);

// Wrap an existing pointer without copying.
// Caller guarantees pointer lifetime.  peel_free() on the returned buffer
// is a safe no-op for the data pointer.
peel_buf_t peel_buf_wrap(const void *src, size_t len);

// === Format Detection ===

// Identify the outermost format without peeling.
// Returns a short name ("hqx", "bin", "sit", "cpt") or NULL if unknown.
const char *peel_detect(const uint8_t *src, size_t len);

// === Writing extracted files ===

// True if `path` is safe to write beneath an output directory: non-empty, not
// absolute, and no component empty, "." or "..".  Anything that turns peeler
// names into files must check this first -- archive extraction wrote
// attacker-named paths outside its output directory.
bool peel_path_is_confined(const char *path);

// The AppleDouble sidecar ("._NAME") that carries an extracted file's
// resource fork and Finder info (type, creator, flags).  Returns 0 and a
// malloc'd buffer in *out / *out_len; 0 with *out == NULL when the file has
// neither, so no sidecar should be written; or a negative errno.  Both the
// peeler CLI and the emulator's archive extraction write sidecars through
// this, so they are identical.
int peel_build_sidecar(const peel_file_t *f, uint8_t **out, size_t *out_len);

// === Main Entry Points ===

// Detect, peel all layers, return extracted files.
// Handles arbitrarily nested formats (e.g. .sit.hqx).
peel_file_list_t peel(const uint8_t *src, size_t len, peel_err_t **err);

// Convenience: read the file at path, then peel().
peel_file_list_t peel_path(const char *path, peel_err_t **err);

// === Byte Sources =====================================================
//
// A byte source is read at any offset: read(off, len) plus a size, a stable
// identity (key) and a cost tier.  Everything peeler takes as input and
// everything it hands back as a fork is a source, so an archive can be
// listed without extracting it and a member opened on its own.  The core
// emulator adopts this type unchanged as its gs_source_t.
//
// Sources are reference counted: peel_source_new returns one reference,
// peel_source_retain adds one, peel_source_release drops one and closes the
// source (then releases its parent) when the last goes.  A source is not
// thread-safe; callers serialise.

typedef struct peel_source peel_source_t;

// What a byte of this source costs to read (advisory, but honest).
typedef enum {
    PEEL_TIER_RANDOM = 0, // any byte in O(1): memory, a file, a stored member, a view
    PEEL_TIER_INDEXED, // O(1) after a cheap one-time index (chunk table, seek table)
    PEEL_TIER_EARNED, // O(1) after one full sequential pass
    PEEL_TIER_STREAM, // forward-only; backward = restart; cost grows with offset
    PEEL_TIER_WHOLE, // must be fully decoded before the first read
} peel_tier_t;

// read() returns bytes read, 0 at EOF, or a negative errno-style code.
// PEEL_EAGAIN means "not yet" (a remote source whose bytes are still in
// flight): the caller waits with peel_source_poll and reads again.
#define PEEL_EAGAIN (-11)

typedef struct {
    int64_t (*read)(peel_source_t *s, uint64_t off, void *buf, size_t len);
    uint64_t (*size)(peel_source_t *s);
    // Stable identity for caches and sidecars: parent key + member path, a
    // canonical file name, or a content hash.  NUL-terminated.
    const char *(*key)(peel_source_t *s);
    peel_tier_t (*tier)(peel_source_t *s);
    void (*close)(peel_source_t *s); // free ctx; parent is released after
    // Optional, for a source whose read() can return PEEL_EAGAIN: wait up to
    // `timeout_ms` (-1: without limit) until a read may make progress.  0
    // when it may, PEEL_EAGAIN on timeout, or a negative error.  NULL for
    // every source that never says "not yet".
    int (*poll)(peel_source_t *s, int timeout_ms);
} peel_source_ops_t;

struct peel_source {
    const peel_source_ops_t *ops;
    void *ctx;
    peel_source_t *parent; // retained; released after close
    int refs;
};

// A new source with one reference.  Takes a reference on `parent` (may be
// NULL).  Returns NULL on allocation failure (ops->close is then called).
peel_source_t *peel_source_new(const peel_source_ops_t *ops, void *ctx, peel_source_t *parent);
peel_source_t *peel_source_retain(peel_source_t *s);
void peel_source_release(peel_source_t *s);

// Thin wrappers over the ops.
int64_t peel_source_read(peel_source_t *s, uint64_t off, void *buf, size_t len);
uint64_t peel_source_size(peel_source_t *s);
const char *peel_source_key(peel_source_t *s);
peel_tier_t peel_source_tier(peel_source_t *s);

// Wait until a read of `s` that returned PEEL_EAGAIN may make progress:
// the poll of the nearest source in its parent chain that has one (a view
// of a remote file polls the file), else 0 at once.
int peel_source_poll(peel_source_t *s, int timeout_ms);

// Read exactly `len` bytes at `off` (looping over short reads, and waiting
// out PEEL_EAGAIN with peel_source_poll).  0, or a negative code (-5 for a
// short source; PEEL_EAGAIN if "not yet" persists without any progress).
int peel_source_read_exact(peel_source_t *s, uint64_t off, void *buf, size_t len);

// Bytes in memory as a source.  With `own`, the buffer is freed on close.
peel_source_t *peel_source_memory(const void *buf, size_t len, bool own);
// The same, with an explicit key (NULL: a key made from the address).
peel_source_t *peel_source_memory_keyed(const void *buf, size_t len, bool own, const char *key);

// A window [off, off+len) of `parent`: pure offset arithmetic, the parent's
// tier.  `len` is clamped to the parent.  Retains the parent.
peel_source_t *peel_source_view(peel_source_t *parent, uint64_t off, uint64_t len);
// The same with the view's key given explicitly (NULL: parent key + "@off+len").
peel_source_t *peel_source_view_keyed(peel_source_t *parent, uint64_t off, uint64_t len, const char *key);

// A host file as a source (the CLI's and the tests' input; an embedder
// supplies its own).  NULL + *err when it cannot be opened.
peel_source_t *peel_source_file(const char *path, peel_err_t **err);

// Read a whole source into an owned buffer (bounded like peel_read_file).
peel_buf_t peel_source_slurp(peel_source_t *s, peel_err_t **err);

// === Sinks: where decode-through output goes ===========================
//
// Peeler never allocates scratch space for decoded forks itself.  The caller
// supplies a sink factory: create() returns a readable source keyed by `key`
// that write() fills as the decoder produces bytes (always in order, from 0);
// commit() marks it complete.  The emulator's chunk cache and a plain temp
// file both fit.  NULL sink ops mean "the heap".  `expected_len` is
// PEEL_SIZE_UNKNOWN when the fork's length is learned only by decoding it
// (a gzip stream); a sink then reports as its size what has been written.
#define PEEL_SIZE_UNKNOWN UINT64_MAX
typedef struct {
    peel_source_t *(*create)(void *ctx, const char *key, uint64_t expected_len);
    int64_t (*write)(peel_source_t *sink, uint64_t off, const void *buf, size_t len);
    void (*commit)(peel_source_t *sink);
} peel_sink_ops_t;

// The default heap sink.
const peel_sink_ops_t *peel_heap_sink(void);

// === Detection ===========================================================
//
// Detection reads a bounded amount: at most PEEL_DETECT_BUDGET bytes from
// the head and as much from the tail of a source.  The probe holds both.
#define PEEL_DETECT_BUDGET (64u * 1024u)

typedef struct {
    const uint8_t *head;
    size_t head_len;
    const uint8_t *tail; // the last tail_len bytes of the source
    size_t tail_len;
    uint64_t size; // the whole source's size
    uint8_t *owned; // backing storage (freed by peel_probe_free)
} peel_probe_t;

// Read head and tail of `src` into `p`.  0 or a negative code.
int peel_probe_init(peel_probe_t *p, peel_source_t *src);
void peel_probe_free(peel_probe_t *p);

// One registered format.  Wrappers (hqx, bin, gz) hold one file; archives
// hold a tree.  detect() reads a probe and never more.
typedef struct {
    const char *name;
    bool is_wrapper;
    bool (*detect)(const peel_probe_t *p);
} peel_format_desc_t;

// Every format peeler reads, wrappers first, in detection order.
const peel_format_desc_t *peel_formats(int *count);

// The first format whose detect() accepts the probe, or NULL.
const peel_format_desc_t *peel_identify(const peel_probe_t *p);

// === Structure-first archive access ======================================

typedef struct peel_archive peel_archive_t;

// Fork selectors for peel_open_fork.
#define PEEL_FORK_DATA 0
#define PEEL_FORK_RSRC 1

// One entry.  Offsets are packed byte offsets into the archive's source
// (UINT64_MAX when a fork has no contiguous packed range, as in BinHex).
typedef struct {
    char path[512]; // '/'-joined folder path; each component sanitised as peel_file_meta_t.name
    bool is_dir;
    uint64_t data_len, rsrc_len; // unpacked
    uint64_t data_packed, rsrc_packed; // packed (== unpacked when stored)
    uint32_t mac_type, mac_creator;
    uint16_t finder_flags;
    uint32_t mtime; // Unix seconds; 0 if unknown
    uint8_t data_method, rsrc_method; // format-specific method id
    peel_tier_t data_tier, rsrc_tier; // RANDOM for stored, EARNED/STREAM/WHOLE otherwise
    uint64_t data_off, rsrc_off; // packed byte offsets in the archive's source
} peel_entry_t;

// Detect and parse structure only: headers and directories, never fork
// payloads.  `sink` may be NULL (heap).  Retains `src`.  NULL + *err when
// the source is not a recognised format or its structure is corrupt.
peel_archive_t *peel_open(peel_source_t *src, const peel_sink_ops_t *sink, void *sink_ctx, peel_err_t **err);
// The same, with the format already chosen (a name from peel_formats()).
peel_archive_t *peel_open_as(const char *format, peel_source_t *src, const peel_sink_ops_t *sink, void *sink_ctx,
                             peel_err_t **err);

const char *peel_format(const peel_archive_t *a); // "zip", "sit", ...
bool peel_is_wrapper(const peel_archive_t *a); // one entry: hqx, bin, gz
int peel_count(const peel_archive_t *a);
const peel_entry_t *peel_entry(const peel_archive_t *a, int i);
int peel_lookup(const peel_archive_t *a, const char *path); // -1 if absent

// Open one fork of entry `i`.  A stored fork is a view (RANDOM); a
// compressed one is a decode-through source that fills a sink lazily (its
// tier is the entry's until the sink is committed, then RANDOM).  An empty
// fork is an empty source.  The returned source outlives the archive.
peel_source_t *peel_open_fork(peel_archive_t *a, int i, int fork, peel_err_t **err);

// Decode a whole fork into an owned buffer (what peel() does per file).
peel_buf_t peel_read_fork(peel_archive_t *a, int i, int fork, peel_err_t **err);

void peel_close(peel_archive_t *a);

// === Per-Format Entry Points (Wrappers: buf → buf) ===

// BinHex 4.0 (.hqx) — peel wrapper, return data fork only.
peel_buf_t peel_hqx(const uint8_t *src, size_t len, peel_err_t **err);

// BinHex 4.0 — peel wrapper, return file with both forks and metadata.
peel_file_t peel_hqx_file(const uint8_t *src, size_t len, peel_err_t **err);

// MacBinary (.bin) — peel wrapper, return data fork only.
peel_buf_t peel_bin(const uint8_t *src, size_t len, peel_err_t **err);

// MacBinary — peel wrapper, return file with both forks and metadata.
peel_file_t peel_bin_file(const uint8_t *src, size_t len, peel_err_t **err);

// === Per-Format Entry Points (Archives: buf → file list) ===

// StuffIt classic / SIT5 (.sit).
peel_file_list_t peel_sit(const uint8_t *src, size_t len, peel_err_t **err);

// Compact Pro (.cpt).
peel_file_list_t peel_cpt(const uint8_t *src, size_t len, peel_err_t **err);

// Zip (.zip), stored and deflated members.
peel_file_list_t peel_zip(const uint8_t *src, size_t len, peel_err_t **err);

// tar (.tar): ustar, GNU and pax members, and macOS "._" companions folded.
peel_file_list_t peel_tar(const uint8_t *src, size_t len, peel_err_t **err);

// gzip (.gz) — peel wrapper, return the decompressed member(s).
peel_buf_t peel_gz(const uint8_t *src, size_t len, peel_err_t **err);

// === Inflate (RFC 1951) ===
//
// Decompress a raw DEFLATE stream (`src`, `len`) into `dst` (`dst_cap`
// bytes).  Returns the number of bytes produced or a negative code.  Also
// used by the emulator for UDIF's zlib chunks.
int64_t peel_inflate(const uint8_t *src, size_t len, uint8_t *dst, size_t dst_cap);

// A zlib stream (RFC 1950: 2-byte header, deflate, Adler-32) into `dst`.
int64_t peel_zlib_inflate(const uint8_t *src, size_t len, uint8_t *dst, size_t dst_cap);

#ifdef __cplusplus
}
#endif

#endif // PEELER_H
