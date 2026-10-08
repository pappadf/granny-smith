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
            // Fast path: every whole aligned block from here on goes straight
            // into dst in one read; only a partial head or tail block goes
            // through the bounce buffer.
            size_t whole = (n - done) & ~(size_t)(STORAGE_BLOCK_SIZE - 1);
            if (disk_read_data(img, (size_t)block_off, dst + done, whole) != whole)
                return -EIO;
            take = whole;
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

// ---- The image as a source ------------------------------------------------

static int64_t img_src_read(gs_source_t *s, uint64_t off, void *buf, size_t len) {
    image_t *img = s->ctx;
    uint64_t size = disk_size(img);
    if (off >= size)
        return 0;
    if (len > size - off)
        len = (size_t)(size - off);
    int rc = image_read_bytes(img, off, buf, len);
    return rc < 0 ? rc : (int64_t)len;
}

static uint64_t img_src_size(gs_source_t *s) {
    return disk_size(s->ctx);
}

static const char *img_src_key(gs_source_t *s) {
    image_t *img = s->ctx;
    return img->source_key ? img->source_key : (img->filename ? img->filename : "image");
}

static gs_tier_t img_src_tier(gs_source_t *s) {
    (void)s;
    return GS_TIER_RANDOM;
}

static void img_src_close(gs_source_t *s) {
    (void)s; // the image is the caller's
}

static const gs_source_ops_t img_src_ops = {img_src_read, img_src_size, img_src_key, img_src_tier, img_src_close};

gs_source_t *image_source(image_t *img) {
    return img ? peel_source_new(&img_src_ops, img, NULL) : NULL;
}

int source_read_partition(gs_source_t *src, uint64_t part_off, uint64_t part_size, uint64_t off, void *buf, size_t n) {
    if (!image_range_fits(off, n, part_size))
        return -EIO;
    return gs_source_read_exact(src, part_off + off, buf, n) == 0 ? 0 : -EIO;
}
