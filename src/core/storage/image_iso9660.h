// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_iso9660.h
// Read-only ISO 9660 (ECMA-119) over a byte source: CD-ROM images that are
// not HFS -- PC and Unix discs, the ISO side of a disc made for both.
//
// Names come from the best description the disc carries: Joliet (a
// supplementary volume descriptor with UCS-2 names) first, else Rock Ridge
// ("NM" entries: POSIX names), else the ISO name less its ";1" version and
// a trailing '.'.  A file's Apple "associated file" (the directory record
// flag Apple's ISO extensions use for a resource fork) is that file's
// resource fork, not an entry of its own; the same extensions' "AA" (or
// original "BA") system use entry is its type, creator and Finder flags.
// Multi-extent and interleaved files are refused rather than misread.

#pragma once
#ifndef IMAGE_ISO9660_H
#define IMAGE_ISO9660_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The standard identifier of every volume descriptor.
#define ISO9660_ID "CD001"
// Volume descriptors start at logical sector 16 (of 2048 bytes).
#define ISO9660_VD_OFF (16u * 2048u)

struct peel_source;
typedef struct iso_volume iso_volume_t;

// One directory entry.  A directory's (extent, size) is what iso_opendir
// takes; a file's are its data, and rsrc_* its associated file's.
typedef struct {
    char name[256]; // UTF-8
    bool is_dir;
    uint32_t extent; // logical block of the data
    uint64_t size;
    uint32_t rsrc_extent;
    uint64_t rsrc_size; // 0: no resource fork
    // Apple ISO 9660 extensions ("AA", or the original "BA"): Finder info.
    bool has_finder_info;
    uint32_t type, creator;
    uint16_t finder_flags;
} iso_dirent_t;

// Does the volume at `off` carry an ISO 9660 primary volume descriptor?
bool iso_probe_source(struct peel_source *src, uint64_t off, uint64_t size);

// Open the volume at `off` (of `size` bytes).  The source is retained.
// NULL when it is not ISO 9660, is corrupt, or memory runs out.
iso_volume_t *iso_open_source(struct peel_source *src, uint64_t off, uint64_t size);
void iso_close(iso_volume_t *vol);

// The volume identifier (UTF-8, trailing spaces dropped).
const char *iso_volume_name(const iso_volume_t *vol);

// Look up a path (UTF-8 components, case-insensitive) from the root;
// `nc` 0 is the root itself.  0, -ENOENT, -ENOTDIR, or another negative
// errno.
int iso_lookup(iso_volume_t *vol, const char *const *comp, size_t nc, iso_dirent_t *out);

// A directory's entries ('.' and '..' skipped, associated files folded into
// their files).  Pair with iso_closedir.
typedef struct iso_dir_iter iso_dir_iter_t;
iso_dir_iter_t *iso_opendir(iso_volume_t *vol, uint32_t extent, uint64_t size);
// 1 with an entry, 0 at the end, or a negative errno.
int iso_readdir_next(iso_dir_iter_t *it, iso_dirent_t *out);
void iso_closedir(iso_dir_iter_t *it);

// Read up to `n` bytes at `off` of the extent `extent` (`size` bytes long).
// Short at its end; *nread is what was filled.  0 or a negative errno.
int iso_read(iso_volume_t *vol, uint32_t extent, uint64_t size, uint64_t off, void *buf, size_t n, size_t *nread);

#endif // IMAGE_ISO9660_H
