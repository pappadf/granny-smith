// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_qcow2.h
// Parser for QEMU's QCOW2 disk image format (versions 2 and 3), read-only.
//
// A QCOW2 file maps the guest disk in clusters (512 B .. 2 MB) through a
// two-level table: the L1 table points at L2 tables, one cluster each, whose
// entries say where each guest cluster lives -- a host cluster read as it
// is, a deflate-compressed run of bytes, all zeros, or nowhere (unallocated,
// which reads as zeros when there is no backing file).  This module decodes
// the header and the table entries; image_chunkmap.c walks the tables and
// serves the guest disk as a chunk-mapped source.
//
// What is not read: a backing file (an overlay is only its own changes),
// encryption, an external data file, and zstd-compressed clusters.  An image
// using any of them is refused (-ENOTSUP) rather than read wrong.
//
// Layout per QEMU's docs/interop/qcow2.rst; see docs/reference/formats/qcow2.md.

#ifndef GS_IMAGE_QCOW2_H
#define GS_IMAGE_QCOW2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define QCOW2_MAGIC 0x514649FBu // "QFI\xfb"

// Bytes of header read to parse it: version 3's fields through the
// compression type (byte 104).
#define QCOW2_HEADER_READ 112u

// Cluster sizes accepted: the format's floor, and QEMU's own ceiling.
#define QCOW2_MIN_CLUSTER_BITS 9u
#define QCOW2_MAX_CLUSTER_BITS 21u

// Largest virtual size accepted: the 56 bits of host offset the tables can
// address (64 PB), far past any disk a Mac saw.
#define QCOW2_MAX_SIZE (1ull << 56)

// Incompatible feature bits (header byte 72).
#define QCOW2_INCOMPAT_DIRTY       (1ull << 0) // refcounts may be stale: harmless to a reader
#define QCOW2_INCOMPAT_CORRUPT     (1ull << 1) // metadata may be damaged
#define QCOW2_INCOMPAT_DATA_FILE   (1ull << 2) // guest data in an external file
#define QCOW2_INCOMPAT_COMPRESSION (1ull << 3) // compression_type is set
#define QCOW2_INCOMPAT_EXTL2       (1ull << 4) // 128-bit L2 entries with subclusters
#define QCOW2_INCOMPAT_KNOWN       ((1ull << 5) - 1)

#define QCOW2_COMPRESSION_DEFLATE 0u
#define QCOW2_COMPRESSION_ZSTD    1u

// Subclusters per cluster with extended L2 entries.
#define QCOW2_SUBCLUSTERS 32u

// Table entry fields.
#define QCOW2_OFFSET_MASK   0x00FFFFFFFFFFFE00ull // bits 9-55: an L1 entry's L2, a standard entry's host cluster
#define QCOW2_L2_COMPRESSED (1ull << 62)
#define QCOW2_L2_ZERO       1ull // bit 0 of a standard descriptor (v3, standard L2 entries only)

// The header fields a reader needs.
typedef struct {
    uint32_t version; // 2 or 3
    uint32_t cluster_bits;
    uint64_t size; // virtual disk size in bytes
    uint32_t crypt_method; // 0: none
    uint32_t l1_size; // entries in the active L1 table
    uint64_t l1_offset;
    uint64_t backing_offset; // 0: no backing file
    uint64_t incompat; // incompatible feature bits (0 for version 2)
    uint8_t compression_type; // QCOW2_COMPRESSION_*
    uint32_t header_length;
} qcow2_header_t;

// True when `buf` (`len` bytes, the head of a file) starts with a QCOW2
// header of a version this parser knows.
bool qcow2_detect(const uint8_t *buf, size_t len);

// Parse the header at the start of `buf` (`len` >= 72; QCOW2_HEADER_READ for
// version 3).  -EINVAL when it is no usable QCOW2 header, -ENOTSUP when it
// uses a feature this reader refuses (backing file, encryption, external
// data file, an unknown incompatible bit); *why (may be NULL) then names it.
// 0 with *h filled otherwise.
int qcow2_parse_header(const uint8_t *buf, size_t len, qcow2_header_t *h, const char **why);

// L2 entries per table (one cluster; 16-byte entries with extended L2).
static inline uint64_t qcow2_l2_entries(const qcow2_header_t *h) {
    return ((uint64_t)1 << h->cluster_bits) / ((h->incompat & QCOW2_INCOMPAT_EXTL2) ? 16u : 8u);
}

// L1 entries needed to map the whole virtual disk.
static inline uint64_t qcow2_l1_needed(const qcow2_header_t *h) {
    uint64_t span = qcow2_l2_entries(h) << h->cluster_bits; // guest bytes one L2 table maps
    return (h->size + span - 1) / span;
}

// The host offset and stored length of a compressed cluster's L2 entry
// `entry`: the descriptor's offset, and the sectors it spans counted from
// that offset (the last may hold less, or the start of the next cluster).
void qcow2_compressed_extent(uint64_t entry, uint32_t cluster_bits, uint64_t *off, uint64_t *len);

// Inflate one compressed cluster (a raw DEFLATE stream, no zlib header):
// `src`/`src_len` the stored bytes (they may run past the stream's end),
// into `dst`, of which the first `dst_len` bytes are wanted (at most the
// cluster; less for a final cluster cut by the virtual size).  0, or -EINVAL
// when the stream does not decode to a whole cluster.
int qcow2_decode_cluster(const uint8_t *src, size_t src_len, uint32_t cluster_bits, uint8_t *dst, size_t dst_len);

#endif // GS_IMAGE_QCOW2_H
