// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// In-memory HFS+ volumes and resource forks for storage tests.
//
// hfsb_build lays out a bare HFS+ volume (no partition map) with N files in
// its root folder, each with a data fork and optionally a resource fork, in
// 4 KiB allocation blocks: volume header, a two-node catalog (header node +
// one leaf holding the root thread, the root folder and one record per file),
// then each file's forks.  Enough for image_hfs and image_vfs to mount it,
// list it and read every fork -- not a general mkfs.
//
// rforkb_build writes a resource fork holding one resource.

#ifndef HFSPLUS_BUILDER_H
#define HFSPLUS_BUILDER_H

#include <stddef.h>
#include <stdint.h>

#define HFSB_BLOCK     4096u
#define HFSB_MAX_FILES 12 // one catalog leaf node holds this many comfortably

typedef struct {
    const char *name; // ASCII
    const uint8_t *data;
    size_t data_len;
    const uint8_t *rsrc; // NULL: no resource fork
    size_t rsrc_len;
    uint64_t rsrc_logical_override; // nonzero: the catalog claims this size instead
} hfsb_file_t;

// Build into img[cap]; returns the volume's size in bytes, or 0 if it does not
// fit.  The root folder's CNID is 2; file i's CNID is 16 + i.
size_t hfsb_build(uint8_t *img, size_t cap, const char *volname, const hfsb_file_t *files, int n_files);

// One resource of `type`/`id` holding data[len]; returns the fork's length, or
// 0 if it does not fit in cap.
size_t rforkb_build(uint8_t *out, size_t cap, uint32_t type, int16_t id, const uint8_t *data, size_t len);

#endif
