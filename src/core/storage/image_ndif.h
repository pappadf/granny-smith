// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_ndif.h
// Parser for the Disk Copy 6.x / NDIF (New Disk Image Format) block map.
//
// An NDIF image is a dual-fork Mac file: the data fork holds the payload
// chunks and the resource fork holds a 'bcem' resource (ID 128) describing
// how to reassemble them.  This module parses that 'bcem' block map and
// decodes individual chunks (zero-fill / raw / ADC).  It does NOT know how
// the two forks are obtained — the caller supplies the resource-fork bytes
// (from the containing HFS volume's resource fork, an AppleDouble sidecar,
// etc.) and reads the data-fork slices.
//
// On-disk layout (big-endian), verified against real Disk Copy 6.3.3 images
// and cross-checked with Aaru's Aaru.Images/NDIF Structs.cs / Constants.cs.
// NB: this is the classic NDIF 'bcem' — a 128-byte header + 12-byte chunk
// descriptors with 8-bit type codes — NOT UDIF's 40-byte 'mish' entries.

#ifndef GS_IMAGE_NDIF_H
#define GS_IMAGE_NDIF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// NDIF chunk type codes (low byte of the descriptor's first word).
#define NDIF_CHUNK_ZERO    0x00 // zero-fill; no data-fork bytes
#define NDIF_CHUNK_COPY    0x02 // raw / uncompressed copy
#define NDIF_CHUNK_KENCODE 0x80 // KenCode (unsupported)
#define NDIF_CHUNK_RLE     0x81 // RLE (unsupported)
#define NDIF_CHUNK_LZH     0x82 // LZH (unsupported)
#define NDIF_CHUNK_ADC     0x83 // Apple Data Compression
#define NDIF_CHUNK_STUFFIT 0xF0 // StuffIt (unsupported)
#define NDIF_CHUNK_END     0xFF // terminator

// One decoded chunk descriptor.
typedef struct {
    uint32_t sector; // starting logical sector (512-byte units)
    uint32_t count; // number of logical sectors this chunk covers
    uint8_t type; // NDIF_CHUNK_*
    uint32_t offset; // byte offset of stored data in the data fork (incl. data_offset)
    uint32_t length; // stored byte length in the data fork (0 for zero-fill)
} ndif_chunk_t;

// Parsed block map.  Free with ndif_map_free().
typedef struct {
    uint32_t sectors; // total logical sectors of the decoded image
    uint32_t crc; // "CRC28" (== CRC-32) of the decoded image, informational
    char volume_name[64]; // image name from the 'bcem' header (MacRoman bytes)
    size_t n_chunks; // usable chunks (END terminator dropped)
    ndif_chunk_t *chunks; // n_chunks entries
} ndif_map_t;

// True if `rfork`/`rfork_len` is a resource fork carrying an NDIF 'bcem'
// block map (used for format detection).
bool ndif_detect(const uint8_t *rfork, size_t rfork_len);

// Parse the 'bcem' block map from a resource fork.  Returns 0 and sets *out
// (caller frees via ndif_map_free) on success, or a negative errno.
int ndif_parse(const uint8_t *rfork, size_t rfork_len, ndif_map_t **out);

void ndif_map_free(ndif_map_t *m);

// Decode one chunk: `src`/`src_len` are the stored data-fork bytes for the
// chunk, `dst`/`dst_len` the output (must be >= chunk->count*512).  Handles
// ZERO, COPY, and ADC; returns -EINVAL for unsupported types or a decode
// error.  Returns 0 on success.
int ndif_decode_chunk(const ndif_chunk_t *chunk, const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len);

#endif // GS_IMAGE_NDIF_H
