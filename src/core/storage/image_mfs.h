// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_mfs.h
// Read-only MFS (the Macintosh File System of the 64K ROM: 400K floppies,
// System 1 to 3) over a byte source.  Inside Macintosh II, "The File
// Manager": the master directory block at block 2, the allocation block
// map after it, and one flat file directory -- MFS has no folders (the
// Finder kept those in its Desktop file), so every file is in the root.
// Both forks and the Finder info of each file are served.

#pragma once
#ifndef IMAGE_MFS_H
#define IMAGE_MFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// drSigWord, at byte 1024 of the volume.
#define MFS_SIG 0xD2D7

struct peel_source;
typedef struct mfs_volume mfs_volume_t;

// One file.  `name` is UTF-8, a Mac '/' shown as ':' (as for HFS).
typedef struct {
    char name[256];
    uint32_t fnum; // flFlNum
    uint16_t data_start, rsrc_start; // first allocation block of each fork
    uint32_t data_len, rsrc_len; // logical lengths
    uint8_t finder_info[16]; // flUsrWds: type, creator, flags, location, folder
    uint32_t created, modified; // Mac seconds since 1904
} mfs_dirent_t;

// Does the volume at `off` (of `size` bytes) carry the MFS signature?
bool mfs_probe_source(struct peel_source *src, uint64_t off, uint64_t size);

// Open the volume at `off` (of `size` bytes): its directory and allocation
// map are read at once (both are small).  The source is retained.  NULL when
// it is not MFS, is corrupt, or memory runs out.
mfs_volume_t *mfs_open_source(struct peel_source *src, uint64_t off, uint64_t size);
void mfs_close(mfs_volume_t *vol);

// The volume name (UTF-8).
const char *mfs_volume_name(const mfs_volume_t *vol);

// The files, in directory order.
int mfs_count(const mfs_volume_t *vol);
const mfs_dirent_t *mfs_entry(const mfs_volume_t *vol, int i);

// The file named `name` (UTF-8; case-insensitive over ASCII, ':' matching a
// Mac '/').  0, or -ENOENT.
int mfs_lookup(const mfs_volume_t *vol, const char *name, mfs_dirent_t *out);

// Read up to `n` bytes of a fork (`rsrc` false: data) of `file` at `off`.
// Short at the fork's end; *nread is what was filled.  0, or a negative
// errno (-EIO for a broken allocation chain).
int mfs_read_fork(mfs_volume_t *vol, const mfs_dirent_t *file, bool rsrc, uint64_t off, void *buf, size_t n,
                  size_t *nread);

#endif // IMAGE_MFS_H
