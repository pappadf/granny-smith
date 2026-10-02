// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// udif_writer.h
// Write a UDIF (.dmg) image in one forward pass from the decoded disk's
// bytes, and verify one.
//
// The images this emulator stores are UDIF: zero runs cost nothing and the
// rest is deflated in 64 KB chunks, so a mostly-empty 2 GB disk is a few
// MB, and what a user downloads opens in hdiutil, 7-Zip and dmg2img.  UDIF
// is laid out for streaming -- payload first, block map and trailer last --
// so the writer never seeks back and never holds more than one chunk.
//
// The profile written (the "GS profile", marked by a gs-profile key in the
// property list):
//   - one segment, flattened, no resource fork, no encryption;
//   - one whole-disk 'mish' block table, starting at sector 0;
//   - chunks of chunk_sectors sectors (default 128 = 64 KB), each ZERO
//     (all zero; consecutive ones merge into a single entry), ZLIB (a
//     complete zlib stream) or RAW (when deflate does not make it smaller);
//   - the table's CRC-32 over the decoded bytes, the trailer's over the
//     data fork, and the master checksum over the table checksums;
//   - a length that is not a whole number of sectors is zero-padded to the
//     next one, the true length recorded as gs-byte-length.
//
// Pure C, no platform code: built into both the browser and headless.

#ifndef GS_UDIF_WRITER_H
#define GS_UDIF_WRITER_H

#include "source.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct udif_writer udif_writer_t;

#define UDIF_WRITER_CHUNK_SECTORS 128u // 64 KB

typedef struct {
    uint32_t chunk_sectors; // power of two in 8..2048; 0 = UDIF_WRITER_CHUNK_SECTORS
    int level; // 0 = ZERO + RAW only (no deflate); 1..9 deflate effort; < 0 = default (1)
    const char *source_name; // recorded as gs-source (may be NULL)
} udif_writer_opts_t;

// Create `path` (it must not exist) to receive a decoded image.  NULL with a
// message in `err` on failure.
udif_writer_t *udif_writer_open(const char *path, const udif_writer_opts_t *opts, char *err, size_t errcap);

// Append `len` bytes of the decoded image.  Any length: at most one partial
// chunk is kept between calls.  0, or a negative errno (-ENOSPC when the
// file system is full); after an error only abort is meaningful.
int udif_writer_append(udif_writer_t *w, const void *buf, size_t len);

// Append `len` zero bytes without a buffer (a blank disk, a sparse run).
int udif_writer_append_zeros(udif_writer_t *w, uint64_t len);

typedef struct {
    uint64_t bytes_in; // decoded bytes appended (the true length)
    uint64_t sectors; // sectors in the image (bytes_in rounded up)
    uint64_t stored_bytes; // bytes of the finished file
    uint64_t zero_bytes; // decoded bytes stored as ZERO runs
    uint64_t extents; // block-table entries (terminator excluded)
    uint32_t crc; // CRC-32 of the decoded bytes (the table checksum)
} udif_writer_stats_t;

// Progress so far (bytes_in, stored data-fork bytes as stored_bytes).
void udif_writer_progress(const udif_writer_t *w, udif_writer_stats_t *out);

// Pad the tail to a sector, flush the last chunk, write the property list
// and the trailer, close.  Fills *stats (may be NULL).  0 or a negative
// errno; the writer is freed either way, and on failure the partial file is
// removed.  An image of zero bytes is refused (-EINVAL).
int udif_writer_finish(udif_writer_t *w, udif_writer_stats_t *stats);

// Abandon: close and remove the partial file, free the writer.
void udif_writer_abort(udif_writer_t *w);

// Write a GS-profile UDIF of `size` zero bytes (one ZERO extent; a couple of
// KB whatever the size).  0 or a negative errno (-EEXIST when `path` exists).
int udif_create_empty(const char *path, uint64_t size);

// Stream a UDIF through every chunk: decode each, check every block table's
// CRC-32 and the data fork's.  0 when it is sound; a negative errno
// otherwise, with a message in `err`.  `stats` (may be NULL) gets what was
// seen: sectors, extents, stored and zero bytes, the decoded CRC when there
// is one table.
int udif_verify(gs_source_t *data, udif_writer_stats_t *stats, char *err, size_t errcap);

// What a UDIF's trailer and block map say, read without decoding a chunk.
typedef struct {
    uint64_t sectors; // decoded size in sectors
    uint64_t byte_length; // decoded size in bytes (gs-byte-length when padded)
    uint64_t extents; // block-table entries
    uint64_t zero_bytes; // decoded bytes in ZERO / IGNORE runs
    uint64_t data_fork_bytes; // stored payload
    uint32_t tables; // 'mish' block tables
    uint32_t crc; // the block table's checksum, when there is one table
    uint64_t max_chunk_bytes; // largest decoded compressed chunk
    bool gs_profile; // written by this emulator
    char source_name[256]; // gs-source, or ""
} udif_info_t;

// Read a UDIF's trailer and property list.  0, or a negative errno when it
// is not a UDIF this reader understands.
int udif_info(gs_source_t *data, udif_info_t *out);

#endif // GS_UDIF_WRITER_H
