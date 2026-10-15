// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_chunkmap.h
// Chunk-mapped byte sources: the decoded image of a Disk Copy 6 (NDIF),
// UDIF (.dmg) or QEMU QCOW2 file, read on demand.
//
// Both formats describe the decoded disk as runs of sectors, each zero-fill,
// a raw copy of a range of the data fork, or a compressed range (ADC, zlib).
// The source maps a read to its runs: zero runs are zeros, raw runs are read
// straight through, and a compressed run is decoded on first touch into the
// chunk cache (chunk_cache.h) keyed by the source's identity, so it is
// decoded once however many blocks of it are read.  Nothing is written to
// a scratch file; the decoded image never exists whole.
//
// Every run is validated when the map is built -- inside the decoded image,
// inside the data fork, a codec we implement, a compressed chunk under
// NDIF_MAX_CHUNK_BYTES -- so a source that opens reads without surprises
// short of a corrupt chunk.

#ifndef GS_IMAGE_CHUNKMAP_H
#define GS_IMAGE_CHUNKMAP_H

#include "source.h"

// The decoded image of an NDIF file: `data` is its data fork, `rsrc` its
// resource fork (holding the 'bcem' map).  NULL with *err set when `rsrc` is
// not an NDIF map or the map is unusable.
source_t *ndif_source_open(source_t *data, source_t *rsrc, int *err);

// True when `rsrc` carries an NDIF 'bcem' map (reads at most the fork).
bool ndif_source_detect(source_t *rsrc);

// The decoded image of a UDIF file (trailer, XML block map and payload all in
// `data`).  NULL with *err when it is not UDIF or cannot be read.
source_t *udif_source_open(source_t *data, int *err);

// Like udif_source_open, with an explicit bound on a decoded chunk (0: the
// in-place bound below for a foreign image, none for one this emulator
// wrote).  A converter that streams every chunk once passes a large bound.
source_t *udif_source_open_bounded(source_t *data, size_t max_chunk, int *err);

// The bound on a decoded chunk a foreign UDIF is opened in place with:
// every cache miss decodes a whole chunk, so a 64 MB chunk would cost 64 MB
// per miss.  Larger-chunked images are refused (-EFBIG); importing one
// re-chunks it.  Default UDIF_INPLACE_MAX_CHUNK_DEFAULT; files.udif_max_chunk_kb.
#define UDIF_INPLACE_MAX_CHUNK_DEFAULT (4u * 1024u * 1024u)
size_t udif_inplace_max_chunk(void);
void udif_set_inplace_max_chunk(size_t bytes);

// True when a UDIF property list carries the gs-profile key: an image this
// emulator wrote (udif_writer.h).
bool udif_xml_is_gs_profile(const uint8_t *xml, size_t len);

// True when the last 512 bytes (`tail`, `len` of them) are a UDIF trailer.
bool udif_source_detect(const uint8_t *tail, size_t len);

// The guest disk of a QEMU QCOW2 image (image_qcow2.h): its L1 and L2 tables
// are read once, into runs -- raw host ranges, deflated clusters decoded
// through the chunk cache, and zeros for what is unallocated.  NULL with
// *err (-ENOTSUP for a backing file, encryption, an external data file or
// zstd clusters; -EINVAL for a damaged map).
source_t *qcow2_source_open(source_t *data, int *err);

// True when `head` (`len` bytes, the start of a file) is a QCOW2 header.
bool qcow2_source_detect(const uint8_t *head, size_t len);

#endif // GS_IMAGE_CHUNKMAP_H
