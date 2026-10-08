// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// namespace.h
// The namespace: something that lists paths and opens their forks as byte
// sources.  A partition map, an HFS/HFS+ or UFS volume and an archive are
// each a namespace over a source (source.h); a file a namespace opens is a
// source another namespace can be opened on.  Alternating the two is all
// nesting is: roms.zip/System 7.sit/Disk.img/partition1/System.
//
// This is the format-facing half of the VFS backend (vfs.h): image_vfs.c
// turns a path into namespace calls and adds the synthetic resource tree
// on top of any namespace whose files have resource forks.
// See docs/internals/core/vfs/namespace.md.

#ifndef GS_NAMESPACE_H
#define GS_NAMESPACE_H

#include "source.h"

#include <stdbool.h>
#include <stdint.h>

// One directory entry.
typedef struct {
    char name[256]; // UTF-8 leaf name; a '/' inside an HFS name is spelled ':'
    bool is_dir;
    uint64_t data_size, rsrc_size;
    uint32_t mtime; // Unix seconds, 0 if unknown
    uint32_t type, creator; // Mac file type and creator
    uint16_t finder_flags;
    bool has_finder_info; // the file has Finder info (a GS_FORK_FINFO fork)
    gs_tier_t tier; // cost of opening this entry's data fork
} gs_dirent_t;

typedef struct gs_namespace gs_namespace_t;

// Paths are '/'-separated and relative to the namespace root ("" is the
// root).  Every method returns 0 / a count, or a negative errno.
typedef struct {
    const char *kind; // "apm", "hfs", "ufs", "zip", ...: the format shown to users
    // List directory `path` into out[0..cap); *count gets the total number
    // of entries (which may exceed cap: call again with a bigger buffer).
    int (*list)(gs_namespace_t *ns, const char *path, gs_dirent_t *out, int cap, int *count);
    int (*stat)(gs_namespace_t *ns, const char *path, gs_dirent_t *out);
    // Open one fork of the file at `path`.  NULL with *err.
    gs_source_t *(*open)(gs_namespace_t *ns, const char *path, gs_fork_t fork, int *err);
    void (*close)(gs_namespace_t *ns);
} gs_namespace_ops_t;

struct gs_namespace {
    const gs_namespace_ops_t *ops;
    void *ctx;
    gs_source_t *src; // the source it was opened on (retained)
    int refs; // the opener's, plus one per source it has handed out
};

// Allocate a namespace over `src` (retained), with one reference.  NULL on
// OOM (ctx is not freed).
gs_namespace_t *gs_namespace_new(const gs_namespace_ops_t *ops, void *ctx, gs_source_t *src);
// References: a source a namespace opens holds one, so the file outlives
// the mount that listed it.  The last release closes (and releases src).
gs_namespace_t *gs_namespace_retain(gs_namespace_t *ns);
void gs_namespace_release(gs_namespace_t *ns);
// The opener's release.  NULL-safe.
void gs_namespace_close(gs_namespace_t *ns);

// Convenience wrappers over the ops.
int gs_ns_list(gs_namespace_t *ns, const char *path, gs_dirent_t **out, int *count); // malloc'd array
int gs_ns_stat(gs_namespace_t *ns, const char *path, gs_dirent_t *out);
gs_source_t *gs_ns_open(gs_namespace_t *ns, const char *path, gs_fork_t fork, int *err);

// Register the namespace formats (a disk, and every peeler archive) with
// the format registry.  Idempotent.
void gs_ns_register_formats(void);

// === The namespace formats ==================================================
//
// Each opener takes a source and returns a namespace, or NULL when the
// source is not that format.  The registry (format_registry.h) chooses.

// A disk image: its Apple Partition Map, or a bare volume exposed as one
// synthetic "partition1".  Entries at the root are "partitionN"; inside one
// is that partition's filesystem (HFS/HFS+/UFS), when it holds one we read.
gs_namespace_t *gs_ns_open_disk(gs_source_t *src);

// A filesystem volume directly (no partitions): HFS/HFS+ or UFS at offset 0.
gs_namespace_t *gs_ns_open_hfs(gs_source_t *src);
gs_namespace_t *gs_ns_open_ufs(gs_source_t *src);

// An archive peeler reads (sit, cpt, zip, and the wrappers hqx, bin, gz):
// the member tree, two-fork members with their resource forks and Finder
// info.  `format` is the peeler format name.
gs_namespace_t *gs_ns_open_archive(gs_source_t *src, const char *format);

// What a namespace is, for listings: a disk's "APM" / "HFS" / "UFS", an
// archive's peeler format ("zip", "sit", "bin", ...).  NULL when `ns` is
// not that kind.
const char *gs_ns_disk_kind(gs_namespace_t *ns);
const char *gs_ns_archive_format(gs_namespace_t *ns);

// Components a namespace path may have; the VFS resolver's own cap
// (VFS_MAX_COMPONENTS in vfs.h), so no layer refuses a path for depth that
// another accepted.
#define GS_NS_MAX_COMPONENTS 128

// Path helpers for implementations: split `path` into components (at most
// `max`, in `buf`), ignoring empty ones.  Returns the count, or -ENAMETOOLONG.
int gs_ns_split(const char *path, char *buf, size_t buf_cap, const char **comps, int max);

// Finder info (32 bytes: FInfo + FXInfo) with the type, creator and flags
// set, the rest zero -- for formats that record only those.
void gs_ns_finder_info(uint32_t type, uint32_t creator, uint16_t flags, uint8_t out[GS_FINDER_INFO_SIZE]);

#endif // GS_NAMESPACE_H
