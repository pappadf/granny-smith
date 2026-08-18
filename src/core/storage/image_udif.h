// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_udif.h
// Parser for the UDIF (Universal Disk Image Format) block map — the `.dmg`
// container Disk Copy 6.5 and later, and every modern `hdiutil`, produce.
//
// A UDIF image is a single-fork file: payload chunks first, then an XML
// property list, then a fixed 512-byte 'koly' trailer at EOF that points at
// both.  The plist's `resource-fork` → `blkx` array holds one Base64-encoded
// 'mish' block table per partition, each mapping sector runs of the decoded
// image to (possibly compressed) byte ranges of the data fork.
//
// This module is pure in-memory: the caller reads the trailer and the plist
// slice off the host file and supplies the data-fork bytes per chunk, exactly
// as image_ndif.c is driven.  All fields are big-endian, including on the
// PowerPC-era images this mostly exists to read.
//
// On-disk layout verified against a real Toast-mastered 1996 CD image
// (UDZO, version 4) — its 'koly' CRC-32 and every per-table 'mish' CRC-32
// reproduce exactly — and cross-checked with Jonathan Levin's "Demystifying
// the DMG File Format", dmg2img and libdmg-hfsplus.
//
// NB: this is UDIF's 40-byte 'mish' chunk entry with 32-bit type codes — NOT
// the classic Disk Copy 6.x NDIF 'bcem' map in image_ndif.h.  Encrypted
// ('encrcdsa') and multi-segment (`.dmgpart`) images are rejected, not
// misread.

#ifndef GS_IMAGE_UDIF_H
#define GS_IMAGE_UDIF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Every UDIF image ends with exactly this many trailer bytes.
#define UDIF_TRAILER_SIZE 512
#define UDIF_SECTOR_SIZE  512u

// Chunk entry types (mish `EntryType`).  The high-bit codes are compressors.
#define UDIF_CHUNK_ZERO    0x00000000u // zero-fill; no data-fork bytes
#define UDIF_CHUNK_RAW     0x00000001u // uncompressed copy
#define UDIF_CHUNK_IGNORE  0x00000002u // unallocated; reads as zeros
#define UDIF_CHUNK_ADC     0x80000004u // Apple Data Compression (UDCO)
#define UDIF_CHUNK_ZLIB    0x80000005u // zlib / DEFLATE (UDZO)
#define UDIF_CHUNK_BZ2     0x80000006u // bzip2 (UDBZ, unsupported)
#define UDIF_CHUNK_LZFSE   0x80000007u // LZFSE (ULFO, unsupported)
#define UDIF_CHUNK_LZMA    0x80000008u // LZMA (ULMO, unsupported)
#define UDIF_CHUNK_COMMENT 0x7FFFFFFEu // "+beg"/"+end" marker; no blocks
#define UDIF_CHUNK_END     0xFFFFFFFFu // terminator

// Checksum type code used by both the trailer and each block table.
#define UDIF_CHECKSUM_NONE  0u
#define UDIF_CHECKSUM_CRC32 2u

// The 'koly' trailer, reduced to the fields a reader needs.
typedef struct {
    uint64_t data_fork_offset; // where the chunk payload starts (usually 0)
    uint64_t data_fork_length; // its length in bytes
    uint64_t xml_offset; // property-list offset
    uint64_t xml_length; // property-list length
    uint64_t sectors; // decoded image size, in 512-byte sectors
    uint32_t segment_count; // >1 means a multi-part image (rejected)
    uint32_t checksum_type; // UDIF_CHECKSUM_* over the *compressed* data fork
    uint32_t checksum; // the CRC-32 itself, when checksum_type is CRC32
} udif_trailer_t;

// One decoded chunk entry.  `sector` is relative to the owning table's
// `base_sector` — the single easiest thing to get wrong in this format.
typedef struct {
    uint32_t type; // UDIF_CHUNK_*
    uint64_t sector; // start sector within the table
    uint64_t count; // number of sectors this chunk covers
    uint64_t offset; // byte offset of stored data in the data fork
    uint64_t length; // stored byte length (0 for zero-fill / ignored)
} udif_chunk_t;

// One 'mish' block table — one partition of the decoded image.
typedef struct {
    char name[64]; // plist entry name, e.g. "Apple_HFS : 3"
    uint64_t base_sector; // absolute start sector of this table
    uint64_t sectors; // sectors it covers
    uint32_t checksum_type; // UDIF_CHECKSUM_* over this table's decoded bytes
    uint32_t checksum; // the CRC-32 itself
    size_t n_chunks; // usable chunks (terminator dropped)
    udif_chunk_t *chunks; // n_chunks entries
} udif_table_t;

// The whole block map.  Free with udif_map_free().
typedef struct {
    uint64_t sectors; // highest base_sector + sectors across all tables
    size_t n_tables;
    udif_table_t *tables;
} udif_map_t;

// True if `trailer` (the last UDIF_TRAILER_SIZE bytes of a host file) is a
// 'koly' trailer this reader understands — used for format detection.
bool udif_detect(const uint8_t *trailer, size_t len);

// Parse the 'koly' trailer.  Returns 0 and fills *out, or a negative errno:
// -EINVAL for a bad or unsupported trailer, -ENOTSUP for a multi-segment image.
int udif_parse_trailer(const uint8_t *trailer, size_t len, udif_trailer_t *out);

// Parse the `blkx` block tables out of the XML property list.  Returns 0 and
// sets *out (caller frees via udif_map_free), or a negative errno.
int udif_parse_blkx(const uint8_t *xml, size_t xml_len, udif_map_t **out);

void udif_map_free(udif_map_t *m);

// Decode one chunk: `src`/`src_len` are the stored data-fork bytes, `dst`/
// `dst_len` the output (must be >= chunk->count * 512).  Handles ZERO,
// IGNORE, RAW, ADC and ZLIB; returns -ENOTSUP for a codec we do not implement
// (bzip2 / LZFSE / LZMA) and -EINVAL for a malformed chunk.  0 on success.
int udif_decode_chunk(const udif_chunk_t *chunk, const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len);

// Running CRC-32 (the zlib/PNG polynomial UDIF uses), seeded with 0, so
// callers can verify a table's decoded bytes against udif_table_t::checksum.
// Chunks of type UDIF_CHUNK_IGNORE are excluded from that running total.
uint32_t udif_crc32(uint32_t crc, const uint8_t *data, size_t len);

#endif // GS_IMAGE_UDIF_H
