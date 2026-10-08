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

// A few write-path tests need a medium that accepts writes and reports where
// they landed; everything else sees an empty (size 0) image whose reads fail.
size_t stub_disk_size;
int stub_write_count;
size_t stub_last_write_offset;

size_t disk_read_data(image_t *disk, size_t offset, uint8_t *buf, size_t size) {
    (void)disk;
    (void)offset;
    (void)buf;
    (void)size;
    return 0;
}

size_t disk_write_data(image_t *disk, size_t offset, uint8_t *buf, size_t size) {
    (void)disk;
    (void)buf;
    stub_write_count++;
    stub_last_write_offset = offset;
    return size;
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
    return stub_disk_size;
}

// iwm_tach_signal derives its pulse phase from emulated time.  The codec tests
// never call it, but it is compiled in with the rest of floppy_gcr.c.
double scheduler_time_ns(struct scheduler *restrict scheduler) {
    (void)scheduler;
    return 0.0;
}

// floppy_media_current() consults the drive for the format its medium
// currently carries; these tests drive the geometry helpers directly with
// images they own, so the drive side is stubbed out.
struct floppy;

image_t *floppy_drive_image(const struct floppy *floppy, unsigned drive) {
    (void)floppy;
    (void)drive;
    return NULL;
}

int floppy_drive_format(const struct floppy *floppy, unsigned drive) {
    (void)floppy;
    (void)drive;
    return -1;
}
