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
#include <string.h>

// A few write-path tests need a medium that accepts writes and reports where
// they landed; everything else sees an empty (size 0) image whose reads fail.
size_t stub_disk_size;
int stub_write_count;
size_t stub_last_write_offset;
// When set, writes also land here (stub_disk_size bytes), so a test can check
// what reached each block and not just where the last write went.
uint8_t *stub_disk_buf;

size_t disk_read_data(image_t *disk, uint64_t offset, uint8_t *buf, size_t size) {
    (void)disk;
    (void)offset;
    (void)buf;
    (void)size;
    return 0;
}

size_t disk_write_data(image_t *disk, uint64_t offset, uint8_t *buf, size_t size) {
    (void)disk;
    stub_write_count++;
    stub_last_write_offset = offset;
    if (stub_disk_buf && offset + size <= stub_disk_size)
        memcpy(stub_disk_buf + offset, buf, size);
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

uint64_t disk_size(image_t *disk) {
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
// images they own, so the drive side is stubbed out.  The SWIM write tests
// put their medium in the drive through stub_drive_image.
struct floppy;
image_t *stub_drive_image;

image_t *floppy_drive_image(const struct floppy *floppy, unsigned drive) {
    (void)floppy;
    (void)drive;
    return stub_drive_image;
}

int floppy_drive_format(const struct floppy *floppy, unsigned drive) {
    (void)floppy;
    (void)drive;
    return -1;
}

// floppy_swim.c records the format a write lays down, and hands IWM-mode
// register accesses and the drive's control/status lines to floppy.c; the
// SWIM tests run in ISM mode only and drive neither.
void floppy_media_set_format(struct floppy *floppy, unsigned drive, int format) {
    (void)floppy;
    (void)drive;
    (void)format;
}

uint8_t floppy_iwm_read(struct floppy *floppy, uint32_t offset) {
    (void)floppy;
    (void)offset;
    return 0;
}

void floppy_iwm_write(struct floppy *floppy, uint32_t offset, uint8_t byte) {
    (void)floppy;
    (void)offset;
    (void)byte;
}

uint8_t floppy_iwm_peek(struct floppy *floppy, uint32_t offset) {
    (void)floppy;
    (void)offset;
    return 0;
}

int floppy_disk_status_at(struct floppy *floppy, int drv, uint8_t lines, bool peek) {
    (void)floppy;
    (void)drv;
    (void)lines;
    (void)peek;
    return 0;
}

void floppy_disk_control(struct floppy *floppy) {
    (void)floppy;
}

// The capture store (image_capture.c) is the storage layer's: these suites'
// media have none.
#include "image_capture.h"

bool image_has_capture(const image_t *img) {
    (void)img;
    return false;
}
image_capture_t *image_capture(image_t *img) {
    (void)img;
    return NULL;
}
image_sector_status_t image_sector_status(image_t *img, uint64_t block) {
    (void)img;
    (void)block;
    return IMAGE_SECTOR_OK;
}
image_sector_status_t image_capture_sector_status(const image_capture_t *cap, uint64_t block) {
    (void)cap;
    (void)block;
    return IMAGE_SECTOR_OK;
}
int image_capture_sides(const image_capture_t *cap) {
    (void)cap;
    return 0;
}
uint32_t image_capture_crc(const image_capture_t *cap) {
    (void)cap;
    return 0;
}
size_t image_capture_track(const image_capture_t *cap, int track, int side, uint8_t **nib,
                           moof_sector_t sec[MOOF_MAX_SECTORS]) {
    (void)cap;
    (void)track;
    (void)side;
    (void)sec;
    *nib = NULL;
    return 0;
}
uint32_t image_capture_track_diff(const moof_sector_t sec[MOOF_MAX_SECTORS], int nsec, const uint8_t *data,
                                  const uint8_t *tags, uint32_t in_delta) {
    (void)sec;
    (void)nsec;
    (void)data;
    (void)tags;
    (void)in_delta;
    return 0;
}
// The sector scan (image_moof.c) finds nothing on these suites' tracks: the
// eject flush, its one caller here, writes nothing back.
void moof_scan_track(const uint8_t *rev, size_t n, int track, int side, moof_sector_t sec[MOOF_MAX_SECTORS],
                     uint8_t *format_byte) {
    (void)rev;
    (void)n;
    (void)track;
    (void)side;
    (void)format_byte;
    memset(sec, 0, sizeof(moof_sector_t) * MOOF_MAX_SECTORS);
    for (int k = 0; k < MOOF_MAX_SECTORS; k++)
        sec[k].state = MOOF_SECTOR_MISSING;
}
bool image_block_in_delta(image_t *img, uint64_t block) {
    (void)img;
    (void)block;
    return false;
}
