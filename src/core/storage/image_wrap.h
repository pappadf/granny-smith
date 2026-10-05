// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_wrap.h
// Volume wrapper: presents an HFS volume the ROM cannot boot as a bootable
// SCSI hard disk.  Two shapes are wrapped:
//   - a bare volume (no Driver Descriptor Map, no partition map, no driver —
//     the shape Mini vMac and most archive.org disk images use);
//   - a driverless partitioned disk (a partition map with one Apple_HFS
//     partition but no driver — the shape Disk Copy / SheepShaver images and
//     many archive.org "hard disk" images use).
// A synthesised prefix — block 0 DDM, a three-entry Apple Partition Map, and
// the in-tree GSDisk 68k driver — is served in front of the untouched volume
// (for a partitioned disk, in place of its own map: the guest sees the HFS
// partition only).  The file is never modified: the prefix lives in memory,
// and every block at or past it maps to the volume's own storage (base +
// delta) unchanged.
// See docs/core/storage/bare-volume-wrapper.md.

#ifndef IMAGE_WRAP_H
#define IMAGE_WRAP_H

#include "image.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Blocks of synthesised prefix in front of the volume: DDM (1) + partition
// map (63) + driver partition (32).  Fixed, so the layout of a wrapped disk
// never depends on the driver's size.
#define IMAGE_WRAP_MAP_START     1u
#define IMAGE_WRAP_MAP_BLOCKS    63u
#define IMAGE_WRAP_DRIVER_START  64u
#define IMAGE_WRAP_DRIVER_BLOCKS 32u
#define IMAGE_WRAP_PREFIX_BLOCKS (IMAGE_WRAP_DRIVER_START + IMAGE_WRAP_DRIVER_BLOCKS)

// True when the first `len` bytes of an image (at least 1536) look like a
// bare HFS or HFS+ volume: a volume header at 1024 ('BD' or 'H+'), and
// neither a Driver Descriptor Map at 0 nor a partition map at 512.
bool image_wrap_is_bare_volume(const uint8_t *head, size_t len);

// The length, in 512-byte blocks, the volume whose first `len` bytes (at
// least 1536) are at `vol` says it has: for HFS the allocation blocks plus
// the alternate MDB and the reserved block after them, for HFS+ its
// totalBlocks.  0 when there is no volume header there or it is malformed.
// An image cut short of this (a trimmed archive download) is opened at this
// length — see image_wrap_extended_blocks.  Pure: exposed for the unit suite.
uint64_t image_wrap_volume_blocks(const uint8_t *vol, size_t len);

// Build the prefix for a volume of `volume_blocks` 512-byte blocks into
// `out` (IMAGE_WRAP_PREFIX_BLOCKS * 512 bytes).  `build_id` (may be NULL)
// is stamped into the driver.  Pure: exposed for the unit suite.
void image_wrap_build_prefix(uint8_t *out, uint64_t volume_blocks, const char *build_id);

// The driver-partition boot checksum the ROM verifies (pmBootCksum): add
// each byte into a 16-bit sum and rotate it left one bit; 0 becomes 0xFFFF.
uint32_t image_wrap_boot_checksum(const uint8_t *code, size_t len);

// True when the first `len` bytes of an image (block 0 on) show a partition
// map with no driver — block 0 is not a DDM or is one naming no drivers, and
// no map entry is an Apple_Driver* — and exactly one Apple_HFS partition.
// That partition's start and length (512-byte blocks) are returned through
// `start` / `blocks`.  `len` must cover the whole map (a map longer than the
// buffer is rejected).  Pure: exposed for the unit suite.
bool image_wrap_find_driverless_hfs(const uint8_t *head, size_t len, uint64_t *start, uint64_t *blocks);

// The most a truncated volume is extended by: 1 GiB of 512-byte blocks.
#define IMAGE_WRAP_MAX_EXTEND_BLOCKS (2u * 1024u * 1024u)

// Reads `size` bytes at byte `offset` of an image's own file; false on a
// short read.
typedef bool (*image_wrap_read_fn)(void *ctx, uint64_t offset, uint8_t *buf, size_t size);

// The length, in 512-byte blocks, an image of `raw_blocks` blocks is to be
// opened at.  An HFS volume the wrapper would present (bare, or the HFS
// partition of a driverless disk) whose own header claims more blocks than
// the file holds — an image trimmed of its free tail — gets its claimed
// length, so the guest sees the volume's whole extent; `volume_start` (may
// be NULL) is then where the volume starts (0 for a bare one).  The image
// layer serves the missing tail as zeros with the alternate MDB / volume
// header in its place, the second-last block, and keeps guest writes there
// in the delta like any others (image.c).  Anything else, or an extension
// past IMAGE_WRAP_MAX_EXTEND_BLOCKS, keeps `raw_blocks`.
uint64_t image_wrap_extended_blocks(image_wrap_read_fn read, void *ctx, uint64_t raw_blocks, uint64_t *volume_start);

// image_wrap_volume results.
#define IMAGE_WRAP_NONE       0 // not a shape the wrapper handles; left as is
#define IMAGE_WRAP_BARE       1 // a bare volume, wrapped
#define IMAGE_WRAP_DRIVERLESS 2 // a driverless partitioned disk, wrapped

// Wrap an open 512-byte-block image in place when it is a bare volume or a
// driverless partitioned disk.  Returns IMAGE_WRAP_BARE /
// IMAGE_WRAP_DRIVERLESS when it was wrapped, IMAGE_WRAP_NONE when it was left
// as is, -1 on allocation failure.  Afterwards disk_size()/raw_size are the
// prefix plus the volume; image->storage is unchanged.
int image_wrap_volume(image_t *image);

#endif // IMAGE_WRAP_H
