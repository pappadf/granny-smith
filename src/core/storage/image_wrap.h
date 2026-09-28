// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_wrap.h
// Bare-volume wrapper: presents a naked HFS volume image (no Driver
// Descriptor Map, no partition map, no driver — the shape Mini vMac and most
// archive.org disk images use) as a bootable SCSI hard disk.  A synthesised
// prefix — block 0 DDM, a three-entry Apple Partition Map, and the in-tree
// GSDisk 68k driver — is served in front of the untouched volume.  The file
// is never modified: the prefix lives in memory, and every block at or past
// it maps to the volume's own storage (base + delta) unchanged.
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

// Build the prefix for a volume of `volume_blocks` 512-byte blocks into
// `out` (IMAGE_WRAP_PREFIX_BLOCKS * 512 bytes).  `build_id` (may be NULL)
// is stamped into the driver.  Pure: exposed for the unit suite.
void image_wrap_build_prefix(uint8_t *out, uint64_t volume_blocks, const char *build_id);

// The driver-partition boot checksum the ROM verifies (pmBootCksum): add
// each byte into a 16-bit sum and rotate it left one bit; 0 becomes 0xFFFF.
uint32_t image_wrap_boot_checksum(const uint8_t *code, size_t len);

// Wrap an open 512-byte-block image in place when it is a bare volume.
// Returns 1 if it was wrapped, 0 if it is not a bare volume (left as is),
// -1 on allocation failure.  Afterwards disk_size()/raw_size include the
// prefix; image->storage still holds only the volume.
int image_wrap_bare_volume(image_t *image);

#endif // IMAGE_WRAP_H
