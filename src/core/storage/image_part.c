// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_part.c
// Byte-granular and partition-bounded image reads.  See image_part.h.

#include "image_part.h"

#include <errno.h>
#include <string.h>

int image_read_bytes(image_t *img, uint64_t off, void *buf, size_t n) {
    // The bounce buffer is one 512-byte block, and disk_read_data reads whole
    // blocks of the image's own size; refuse any other geometry here rather
    // than trip disk_read_data's alignment assert.
    if (disk_block_size(img) != STORAGE_BLOCK_SIZE)
        return -EINVAL;
    uint8_t *dst = buf;
    size_t done = 0;
    uint8_t blk[STORAGE_BLOCK_SIZE];
    while (done < n) {
        uint64_t abs = off + done;
        uint64_t block_off = abs & ~(uint64_t)(STORAGE_BLOCK_SIZE - 1);
        size_t in_block = (size_t)(abs - block_off);
        size_t take = STORAGE_BLOCK_SIZE - in_block;
        if (take > n - done)
            take = n - done;
        if (in_block == 0 && take == STORAGE_BLOCK_SIZE) {
            // Fast path: a whole aligned block goes straight into dst.
            if (disk_read_data(img, (size_t)block_off, dst + done, STORAGE_BLOCK_SIZE) != STORAGE_BLOCK_SIZE)
                return -EIO;
        } else {
            if (disk_read_data(img, (size_t)block_off, blk, STORAGE_BLOCK_SIZE) != STORAGE_BLOCK_SIZE)
                return -EIO;
            memcpy(dst + done, blk + in_block, take);
        }
        done += take;
    }
    return 0;
}

int image_read_partition(image_t *img, uint64_t part_off, uint64_t part_size, uint64_t off, void *buf, size_t n) {
    if (!image_range_fits(off, n, part_size))
        return -EIO;
    return image_read_bytes(img, part_off + off, buf, n);
}
