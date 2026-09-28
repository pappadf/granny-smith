// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_part.h
// Byte-granular reads from a disk image, and reads bounded by a partition.
// disk_read_data works in whole blocks; the HFS and UFS walkers read records
// at arbitrary byte offsets within one partition.  They each carried a copy
// of these, and only one copy's bounds check was wrap-safe.

#ifndef IMAGE_PART_H
#define IMAGE_PART_H

#include "image.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// True when [off, off + n) lies within [0, size).  Written so a hostile
// off + n cannot wrap past the check.
static inline bool image_range_fits(uint64_t off, uint64_t n, uint64_t size) {
    return n <= size && off <= size - n;
}

// Read n bytes at byte offset `off` of the image, bouncing through a block
// buffer where the range is not block-aligned.  0, -EIO on a short read, or
// -EINVAL for an image whose blocks are not 512 bytes (a Lisa ProFile).
int image_read_bytes(image_t *img, uint64_t off, void *buf, size_t n);

// Read n bytes at `off` within the partition of `part_size` bytes that starts
// at image byte `part_off`.  -EIO if the range leaves the partition.
int image_read_partition(image_t *img, uint64_t part_off, uint64_t part_size, uint64_t off, void *buf, size_t n);

#endif // IMAGE_PART_H
