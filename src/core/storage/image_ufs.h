// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_ufs.h
// Read-only UFS-1 walker.  Given an image_t plus the byte offset and size of
// a partition, opens the BSD FFS superblock, caches the parameters needed
// for inode and block address math, and exposes path-lookup, directory
// enumerate, and file-read entry points for the image_vfs backend.
//
// Scope (Phase 3):
//   - A/UX 3.0.x UFS-1 partitions (big-endian on 68k).  Targets
//     "Apple_UNIX_SVR2" APM entries that a period install writes.
//   - Direct (12) + single-indirect + double-indirect block pointers.
//     Triple-indirect is declared but not currently needed in realistic
//     A/UX installs; add when a fixture requires it.
//   - 4.3BSD-Tahoe dinode layout: the 32-bit di_size lives at inode offset
//     12, not 8 — Tahoe used quad_t val[0] for di_rdev and val[1] for the
//     size, and A/UX inherited that convention.  Files > 4 GiB are not
//     supported (would need to read the full quad and treat it as 64-bit,
//     but no A/UX file hits that limit).

#pragma once

#ifndef IMAGE_UFS_H
#define IMAGE_UFS_H

#include "image.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Root inode per UFS convention.  Inode 0 is unused, 1 is reserved for bad
// blocks, 2 is the root directory.
#define UFS_ROOT_INO 2

// FS_MAGIC at superblock offset 1372 when the FS is valid.
#define UFS_FS_MAGIC 0x011954

// Superblock offset in bytes within the partition.
#define UFS_SBOFF 8192

// Opaque volume handle.
typedef struct ufs_volume ufs_volume_t;

// One directory entry reported to VFS callers.  `name` is UTF-8 (UFS names
// are already 8-bit clean; we pass them through unchanged).
typedef struct ufs_dirent {
    char name[256];
    bool is_dir;
    bool is_symlink;
    uint32_t ino;
    uint64_t size; // regular files only
    uint16_t mode; // raw dinode mode field (S_IFMT + perms)
} ufs_dirent_t;

// Cheap probe: does byte offset `partition_byte_offset + UFS_SBOFF` look
// like a UFS superblock?  Reads a single 512-byte sector.  Returns true on
// a valid FS_MAGIC in either byte order (A/UX writes big-endian, but we
// accept either — tolerating the occasional LE-rewritten image).
bool ufs_probe(image_t *img, uint64_t partition_byte_offset, uint64_t partition_byte_size);

// Open a UFS volume.  Returns NULL on not-UFS / malformed superblock / OOM.
ufs_volume_t *ufs_open(image_t *img, uint64_t partition_byte_offset, uint64_t partition_byte_size);

// Release the volume and all cached state.  Safe on NULL.
void ufs_close(ufs_volume_t *vol);

// Look up a path (list of UTF-8 components) relative to the volume root.
// `nc == 0` returns the root directory entry.  Returns 0 on hit, -ENOENT on
// miss, -ENOTDIR when a non-terminal component isn't a directory, or
// another negated errno on I/O failure.  Symbolic links are not followed;
// callers see the link itself.
int ufs_lookup(ufs_volume_t *vol, const char *const *components, size_t nc, ufs_dirent_t *out);

// Directory iterator.  Pair with ufs_closedir_iter.
typedef struct ufs_dir_iter ufs_dir_iter_t;

ufs_dir_iter_t *ufs_opendir_ino(ufs_volume_t *vol, uint32_t ino);
// Returns 1 on entry, 0 on EOF, <0 on error.  Skips "." and "..".
int ufs_readdir_next(ufs_dir_iter_t *iter, ufs_dirent_t *out);
void ufs_closedir_iter(ufs_dir_iter_t *iter);

// Read up to `n` bytes from inode `ino` starting at logical offset `off`.
// Short reads past EOF are not errors; *nread receives the number of bytes
// actually filled.  Returns 0 on success, negated errno on I/O failure.
int ufs_read_file(ufs_volume_t *vol, uint32_t ino, uint64_t off, void *buf, size_t n, size_t *nread);

#endif // IMAGE_UFS_H
