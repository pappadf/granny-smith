// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_hfs.h
// Read-only HFS catalog walker.  Given an image_t plus the byte offset and
// size of a partition, opens the HFS Master Directory Block, loads the
// catalog file into RAM, and exposes path-lookup, directory-enumerate, and
// fork-read entry points for the image_vfs backend.
//
// Scope (Phase 2):
//   - 400K / 800K / 1.4M floppies with no partition map (partition_offset=0).
//   - HFS partitions inside an APM image.
//   - Data and resource forks via three-extent file record; the extent
//     overflow file is not consulted (fragmented large files will read
//     only the portion captured in the catalog record).  Realistic 800K
//     and A/UX install-era HFS volumes almost never fragment to the point
//     of overflow; revisit if a test fixture hits the limit.
//   - MacRoman -> UTF-8 transcoding on enumeration.

#pragma once

#ifndef IMAGE_HFS_H
#define IMAGE_HFS_H

#include "image.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Root directory CNID per HFS convention.
#define HFS_ROOT_CNID 2

// HFS signature word "BD" at offset 0 of the MDB.
#define HFS_SIG_BD 0x4244

// Max extents captured inline in a catalog file record.  HFS uses exactly 3.
#define HFS_INLINE_EXTENTS 3

// Fork descriptor extracted from a catalog file record.
typedef struct hfs_fork {
    uint64_t logical_size;
    struct {
        uint32_t start_ablock; // allocation block number within volume
        uint32_t num_ablocks;
    } extents[HFS_INLINE_EXTENTS];
} hfs_fork_t;

// One catalog entry as seen by VFS-level callers.  `name` is UTF-8.
typedef struct hfs_dirent {
    char name[256];
    bool is_dir;
    uint32_t cnid; // folder or file CNID
    uint32_t valence; // directory entry count (dirs only)
    hfs_fork_t data_fork; // files only
    hfs_fork_t rsrc_fork; // files only
    uint8_t finder_info[32]; // 16 bytes FInfo + 16 bytes FXInfo (files)
} hfs_dirent_t;

// Opaque volume handle.
typedef struct hfs_volume hfs_volume_t;

// Open an HFS volume backed by `img`.  `partition_byte_offset` is the byte
// offset within the image at which the volume starts (0 for a bare floppy;
// start_block*512 for an APM partition).  `partition_byte_size` caps reads.
// Returns NULL on any error (not HFS, malformed catalog, OOM).
hfs_volume_t *hfs_open(image_t *img, uint64_t partition_byte_offset, uint64_t partition_byte_size);

// Release a volume and all cached state.  Safe on NULL.
void hfs_close(hfs_volume_t *vol);

// Retrieve the on-disk volume name (UTF-8).  Valid for the volume's lifetime.
const char *hfs_volume_name(const hfs_volume_t *vol);

// Look up a file or folder by path components (UTF-8).  `nc == 0` returns the
// root folder record.  Returns 0 on hit and populates *out, -ENOENT on miss,
// or a negated errno on error.
int hfs_lookup(hfs_volume_t *vol, const char *const *components, size_t nc, hfs_dirent_t *out);

// Directory iterator.  Caller must pair opendir_cnid with closedir_iter.
typedef struct hfs_dir_iter hfs_dir_iter_t;

hfs_dir_iter_t *hfs_opendir_cnid(hfs_volume_t *vol, uint32_t parent_cnid);
int hfs_readdir_next(hfs_dir_iter_t *iter, hfs_dirent_t *out); // 1=entry, 0=eof, <0 err
void hfs_closedir_iter(hfs_dir_iter_t *iter);

// Read `n` bytes from a fork starting at logical offset `off`.  Bytes past
// the fork's logical size read as zero.  Returns 0 on success with *nread
// set to the number of bytes filled (may be less than `n` near EOF);
// returns a negated errno on read failure.
int hfs_read_fork(hfs_volume_t *vol, const hfs_fork_t *fork, uint64_t off, void *buf, size_t n, size_t *nread);

#endif // IMAGE_HFS_H
