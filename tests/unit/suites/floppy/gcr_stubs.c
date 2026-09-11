// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Local stubs for the isolated floppy unit suite.
//
// floppy_gcr.c reaches the storage layer to fill a track buffer from an image
// and to write decoded sectors back.  These tests drive the codec directly with
// buffers they own, so they never reach media; trivial stubs satisfy the linker
// without pulling in storage.c, the delta engine and the image format readers.

#include "image.h"
#include "scheduler.h"

#include <stddef.h>
#include <stdint.h>

size_t disk_read_data(image_t *disk, size_t offset, uint8_t *buf, size_t size) {
    (void)disk;
    (void)offset;
    (void)buf;
    (void)size;
    return 0;
}

size_t disk_write_data(image_t *disk, size_t offset, uint8_t *buf, size_t size) {
    (void)disk;
    (void)offset;
    (void)buf;
    (void)size;
    return 0;
}

size_t disk_read_tag(image_t *disk, size_t sector, uint8_t *buf, size_t size) {
    (void)disk;
    (void)sector;
    (void)buf;
    (void)size;
    return 0;
}

size_t disk_write_tag(image_t *disk, size_t sector, const uint8_t *buf, size_t size) {
    (void)disk;
    (void)sector;
    (void)buf;
    (void)size;
    return 0;
}

size_t disk_size(image_t *disk) {
    (void)disk;
    return 0;
}

// iwm_tach_signal derives its pulse phase from emulated time.  The codec tests
// never call it, but it is compiled in with the rest of floppy_gcr.c.
double scheduler_time_ns(struct scheduler *restrict scheduler) {
    (void)scheduler;
    return 0.0;
}
