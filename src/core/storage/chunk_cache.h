// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// chunk_cache.h
// A bounded in-memory LRU of decoded chunks keyed by (source key, chunk
// index), with request coalescing and optional spill to a directory.
//
// Whatever is expensive to produce -- an NDIF or UDIF chunk decoded from
// ADC or zlib, a stretch of a forward-only decode -- is kept here keyed by
// the identity of the source it came from, so a second read is a copy.
// Chunks are variable-sized (a UDIF chunk is what its table says); the
// budget is in bytes.
//
// Coalescing: when several threads ask for the same absent chunk at once
// (the emulator reading a disk while the I/O worker exports it), one of
// them fetches and the others wait for that fetch rather than repeating it.
//
// Spill: with a spill directory, a chunk evicted from memory is appended to
// a per-key spill file instead of being dropped, so a forward-only decode
// never has to restart to get it back.  The spill budget bounds the files;
// past it, eviction drops.

#ifndef GS_CHUNK_CACHE_H
#define GS_CHUNK_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct gs_chunk_cache gs_chunk_cache_t;

// Fill `buf` (`cap` bytes) with chunk `idx` of the source the caller keys.
// Returns the chunk's length (<= cap), or a negative errno.
typedef int64_t (*gs_chunk_fetch_fn)(void *ctx, uint64_t idx, uint8_t *buf, size_t cap);

// A cache holding at most `mem_budget` bytes in memory.  `spill_dir` may be
// NULL (no spill); `spill_budget` 0 means unbounded.
gs_chunk_cache_t *gs_chunk_cache_new(size_t mem_budget, const char *spill_dir, uint64_t spill_budget);
void gs_chunk_cache_free(gs_chunk_cache_t *c);

// The process-wide cache the storage adapters share.  Memory budget
// GS_CHUNK_CACHE_MB (default 64 MiB); spill under image_scratch_dir()/chunks
// with budget GS_CHUNK_SPILL_MB (default 512 MiB on WASM, unbounded
// natively).
gs_chunk_cache_t *gs_chunk_cache_default(void);

// Copy `n` bytes at `in_off` within chunk (key, idx) into `out`, fetching
// the chunk (into a buffer of `chunk_cap` bytes) through `fetch` if it is in
// neither memory nor spill.  Returns the bytes copied -- fewer than `n` only
// at the chunk's end, 0 past it -- or a negative errno (the fetch's).
// Thread-safe; concurrent requests for one chunk share a single fetch.
int64_t gs_chunk_cache_get(gs_chunk_cache_t *c, const char *key, uint64_t idx, size_t chunk_cap, uint64_t in_off,
                           void *out, size_t n, gs_chunk_fetch_fn fetch, void *ctx);

// Store a chunk the caller produced anyway (a sequential decode passing
// over chunks on its way to the one asked for).  0 or a negative errno.
int gs_chunk_cache_put(gs_chunk_cache_t *c, const char *key, uint64_t idx, const void *data, size_t len);

// True (and the length in *len) when chunk (key, idx) is held, in memory or
// spilled; copies it into `out` when `out` is non-NULL.
bool gs_chunk_cache_peek(gs_chunk_cache_t *c, const char *key, uint64_t idx, void *out, size_t cap, size_t *len);

// Forget every chunk of `key` (memory and spill).
void gs_chunk_cache_drop_key(gs_chunk_cache_t *c, const char *key);

// Counters, for the unit suite and diagnostics.
typedef struct {
    uint64_t hits; // served from memory
    uint64_t spill_hits; // served from a spill file
    uint64_t misses; // needed a fetch
    uint64_t fetches; // fetches actually run (misses less coalesced waits)
    uint64_t coalesced; // requests that waited on another's fetch
    uint64_t evictions; // chunks pushed out of memory
    uint64_t spilled; // of those, written to a spill file
    size_t mem_bytes; // bytes held in memory now
    uint64_t spill_bytes; // bytes in spill files now
} gs_chunk_cache_stats_t;

void gs_chunk_cache_stats(gs_chunk_cache_t *c, gs_chunk_cache_stats_t *out);

#endif // GS_CHUNK_CACHE_H
