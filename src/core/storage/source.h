// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// source.h
// The byte source: read(offset, len) plus a size, a stable identity (key)
// and a cost tier.  Every image encoding, partition, filesystem file and
// archive member the emulator reads is one, and adapters stack them: a
// DiskCopy payload is a view of a host file, an NDIF image a chunk-mapped
// source over an HFS fork, a zip member a decode-through source over the
// zip.  storage_t reads its base through one, so any read-only source is a
// writable disk (the delta takes the writes).
//
// Peeler, the lowest layer, defines the type (peel_source_t) and the core
// adopts it unchanged: gs_source_t *is* peel_source_t, so an archive
// member peeler opens is a source the storage engine can mount directly.
// See docs/internals/core/storage/source.md.
//
// Reference counted: every constructor returns one reference;
// gs_source_retain / gs_source_release manage the rest.  A host-file source
// may be read from any thread; other sources are serialised by their user
// (storage_t guards its base with a lock).

#ifndef GS_SOURCE_H
#define GS_SOURCE_H

#include "peeler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef peel_source_t gs_source_t;
typedef peel_source_ops_t gs_source_ops_t;
typedef peel_tier_t gs_tier_t;

#define GS_TIER_RANDOM  PEEL_TIER_RANDOM
#define GS_TIER_INDEXED PEEL_TIER_INDEXED
#define GS_TIER_EARNED  PEEL_TIER_EARNED
#define GS_TIER_STREAM  PEEL_TIER_STREAM
#define GS_TIER_WHOLE   PEEL_TIER_WHOLE
#define GS_EAGAIN       PEEL_EAGAIN

// The forks a path can name: a file's data, its resource fork, and its
// 32-byte Finder info (FInfo + FXInfo).
typedef enum { GS_FORK_DATA, GS_FORK_RSRC, GS_FORK_FINFO } gs_fork_t;

// Size of the Finder info a GS_FORK_FINFO source holds.
#define GS_FINDER_INFO_SIZE 32u

// True when `key` is `parent` or names something inside it: a member
// ("<parent>/partition1/...") or a wrapper layer ("<parent>#dc42").
bool gs_key_within(const char *key, const char *parent);

// True when keys `a` and `b`, made at different times (a checkpoint's and
// now), name the same bytes.  With `ignore_host_times`, the time stamp in
// each host file's "@<size>:<mtime>" is not compared -- only its canonical
// path and size are.
bool gs_key_same(const char *a, const char *b, bool ignore_host_times);

// gs_key_same as this platform needs it.  WasmFS gives a file in OPFS the
// time it was loaded as its mtime, so in the browser the time stamp is no
// identity across a reload and is ignored; natively it is compared.
bool gs_key_same_source(const char *saved, const char *now);

// === Constructors ===========================================================

// A host file, read with pread (so any thread may read it).  Its key is the
// canonical path with the file's size and mtime, so a changed file is a
// different source to every cache.  NULL with *err (negative errno) set when
// it cannot be opened; `err` may be NULL.
gs_source_t *gs_source_host(const char *path, int *err);

// A window of `parent` (pure arithmetic; the parent's tier).  `key` may be
// NULL (the parent's key with the range appended).
gs_source_t *gs_source_view(gs_source_t *parent, uint64_t off, uint64_t len, const char *key);

// Bytes in memory.  With `own`, freed with the source.  `key` may be NULL.
gs_source_t *gs_source_memory(const void *buf, size_t len, bool own, const char *key);

// === Operations =============================================================

static inline gs_source_t *gs_source_retain(gs_source_t *s) {
    return peel_source_retain(s);
}
static inline void gs_source_release(gs_source_t *s) {
    peel_source_release(s);
}
static inline int64_t gs_source_read(gs_source_t *s, uint64_t off, void *buf, size_t len) {
    return peel_source_read(s, off, buf, len);
}
// Wait until a read that returned GS_EAGAIN may make progress (see
// peel_source_poll): `timeout_ms` -1 waits without limit.  0, GS_EAGAIN on
// timeout, or a negative errno.
static inline int gs_source_poll(gs_source_t *s, int timeout_ms) {
    return peel_source_poll(s, timeout_ms);
}
static inline uint64_t gs_source_size(gs_source_t *s) {
    return peel_source_size(s);
}
static inline const char *gs_source_key(gs_source_t *s) {
    return peel_source_key(s);
}
static inline gs_tier_t gs_source_tier(gs_source_t *s) {
    return peel_source_tier(s);
}

// Exactly `len` bytes at `off`, waiting out GS_EAGAIN with gs_source_poll:
// 0, or a negative errno (-EIO when short).
int gs_source_read_exact(gs_source_t *s, uint64_t off, void *buf, size_t len);

// The whole source into a malloc'd buffer of at most `max` bytes.  0 or a
// negative errno (-EFBIG over `max`).  An empty source gives *out NULL.
int gs_source_read_all(gs_source_t *s, size_t max, uint8_t **out, size_t *out_len);

// Read the whole data fork of `path` (through the path opener, so the path
// may run through an image or an archive) into a malloc'd buffer of at most
// `max` bytes.  0 or a negative errno.
int gs_read_path(const char *path, size_t max, uint8_t **out, size_t *out_len);

// The tier's name ("random", "indexed", "earned", "stream", "whole").
const char *gs_tier_name(gs_tier_t t);

// === Opening a path =========================================================
//
// The storage engine and the ROM loader open what the user named.  With the
// VFS linked in, a path may continue through an image or an archive
// (roms.zip/Plus.rom, outer.img/partition1/inner.img) and the VFS resolves
// it; without it (a unit test), a path is a host file.  The VFS installs
// itself as the opener; this layer does not depend on it.

typedef gs_source_t *(*gs_path_opener_t)(const char *path, gs_fork_t fork, int *err);

// Install the opener (NULL restores the host-file default).
void gs_source_set_path_opener(gs_path_opener_t opener);

// Open fork `fork` of `path` through the installed opener.  NULL with *err
// (negative errno; may be NULL) when there is no such file or fork.
gs_source_t *gs_source_open_path(const char *path, gs_fork_t fork, int *err);

// The host-file default: the data fork is the file; the resource fork and
// Finder info come from a companion AppleDouble "._NAME" (or legacy
// "%NAME"), or a raw "NAME.rsrc" for the resource fork.
gs_source_t *gs_source_open_host_path(const char *path, gs_fork_t fork, int *err);

// === Decode-through and sinks ==============================================

struct gs_chunk_cache;

// Chunk size decode-through caches in.
#define GS_DECODE_CHUNK (128u * 1024u)

// Wrap an expensive or forward-only source (tier STREAM / WHOLE / EARNED) in
// a cache-backed one: reads fill chunks of GS_DECODE_CHUNK bytes keyed by
// src's key; a read ahead of the decode cursor drives the source forward,
// storing every chunk it passes, so a backward read later is a cache hit
// (and, with spill, never a restart).  Its tier is EARNED.  Retains `src`.
// `cache` NULL means gs_chunk_cache_default().
gs_source_t *gs_source_decode_through(gs_source_t *src, struct gs_chunk_cache *cache);

// Serialise every read of `src` behind a lock of its own, so sources that are
// not thread-safe (a peeler decode, a chunk map) can be read by the emulator
// and the I/O worker at once.  Retains `src`; the result has its key and tier.
gs_source_t *gs_source_locked(gs_source_t *src);

// The sink peeler's decode-through forks fill (peel_open's sink): memory for
// a small fork, an unlinked scratch file under image_scratch_dir() for a
// large one -- the spill area -- so a decoded disk image does not sit in RAM.
const peel_sink_ops_t *gs_scratch_sink(void);

#endif // GS_SOURCE_H
