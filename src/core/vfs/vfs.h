// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// vfs.h
// Thin VFS layer: the shell's filesystem commands (ls, cd, cat, ...) call
// through a small backend interface instead of libc directly. Two backends
// ship today — `host` for plain filesystem paths and `image` (see image_vfs.h)
// for paths that descend into a Mac disk image or an archive, to any depth.
// The resolver (`vfs_resolve`/`vfs_resolve_descend`) routes between them
// transparently so `ls /tmp/foo.img/partition2/etc/motd` and
// `ls roms.zip/System.sit/Disk.img/partition1` Just Work.

#pragma once

#ifndef VFS_H
#define VFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Maximum path length handled by VFS wrappers.
#define VFS_PATH_MAX 1024

// File-mode bits reported by vfs_stat.
#define VFS_MODE_FILE 0x1
#define VFS_MODE_DIR  0x2

// Stat result.
typedef struct vfs_stat {
    uint64_t size; // bytes (0 for directories)
    uint32_t mtime; // Unix seconds (0 if unavailable)
    uint16_t mode; // VFS_MODE_FILE or VFS_MODE_DIR
    bool readonly; // true for image-backed paths
} vfs_stat_t;

// One directory entry.
typedef struct vfs_dirent {
    char name[256];
    vfs_stat_t st; // name-only backends may leave st zeroed
    bool has_stat; // true when st was populated
} vfs_dirent_t;

// Opaque file and directory handles; their shape is backend-specific.
typedef struct vfs_file vfs_file_t;
typedef struct vfs_dir vfs_dir_t;

// Backend vtable.  Every method receives the backend's own ctx pointer
// (which is NULL for the host backend since it is stateless).
typedef struct vfs_backend {
    const char *scheme; // "host" or "image"

    int (*stat)(void *ctx, const char *path, vfs_stat_t *out);
    int (*opendir)(void *ctx, const char *path, vfs_dir_t **out);
    int (*readdir)(vfs_dir_t *d, vfs_dirent_t *out); // 0=eof, 1=entry, <0 err
    void (*closedir)(vfs_dir_t *d);
    int (*open)(void *ctx, const char *path, vfs_file_t **out);
    int (*read)(vfs_file_t *f, uint64_t off, void *buf, size_t n, size_t *nread);
    void (*close)(vfs_file_t *f);

    // Writable operations. Image-backed paths reject these (the image
    // backend installs static `-EROFS` rejecters); only the host backend
    // actually mutates the filesystem.
    int (*mkdir)(void *ctx, const char *path);
    int (*unlink)(void *ctx, const char *path);
    int (*rename)(void *ctx, const char *src, const char *dst);
} vfs_backend_t;

// Host backend accessor.  Returns a pointer to a static vtable; the ctx
// value used by the host backend is always NULL.
const vfs_backend_t *vfs_host_backend(void);

// Resolve a shell-facing path to a backend plus a path that backend
// understands.  `input` may be relative (interpreted against the shell's
// current_dir) or absolute.  `resolved` is a caller-provided buffer that
// receives the normalised absolute host path; `*be` / `*ctx` / `*tail` are
// the backend triple to invoke.  Returns 0 on success, or a negated errno
// on probe failure (-ENOTDIR when a path continues past a file that isn't
// a recognised image, -EBUSY when descent is blocked by hd attach).
//
// For "/tmp/foo.img/partition2/etc/motd" the resolver opens an auto-mount
// for /tmp/foo.img and returns the image backend with tail
// "/partition2/etc/motd".  For plain host paths it returns the host
// backend with the full resolved path as tail.
int vfs_resolve(const char *input, char *resolved, size_t resolved_len, const vfs_backend_t **be, void **ctx,
                const char **tail);

// Like vfs_resolve, but if the resolved path terminates exactly at an
// image file (no trailing slash, no further segments) the resolver still
// descends into the image's partition-list root.  This implements the
// ergonomic "ls/cd on a bare image path" rule
// without changing the strict semantics of vfs_resolve — cat/size/stat
// keep the "bare image = file" behaviour.
int vfs_resolve_descend(const char *input, char *resolved, size_t resolved_len, const vfs_backend_t **be, void **ctx,
                        const char **tail);

// Convenience helpers that combine vfs_resolve with a backend call.  They
// are the primary entry points for shell commands.  Each returns 0 on
// success and a negated errno on failure (or the backend's own error
// code).
int vfs_stat(const char *path, vfs_stat_t *out);
int vfs_opendir(const char *path, vfs_dir_t **out, const vfs_backend_t **be);
int vfs_open(const char *path, vfs_file_t **out, const vfs_backend_t **be);
int vfs_mkdir(const char *path);
int vfs_unlink(const char *path);
int vfs_rename(const char *src, const char *dst);

// Export a disk image referenced by a VFS path as a flat **raw** image on
// the host.  `src` may be a host image or one inside an image or archive
// (an NDIF `.img` inside a Toast CD, a `.dsk.gz` inside a zip), in any of
// the formats the image opener peels (UDIF, NDIF, DiskCopy 4.2, MacBinary,
// BinHex, gzip).  The result is the decoded logical block device --
// single-fork, portable, directly re-mountable.  Refuses to overwrite an
// existing `dst`.  Returns 0 on success or a negated errno; on failure
// `err`/`err_cap` (optional) receives a human-readable message.
int vfs_export_raw_image(const char *src, const char *dst, char *err, size_t err_cap);

// Open fork `fork` of the file at `path` (strict resolution: a bare image
// path is the image file) as a byte source: a host file and its AppleDouble
// companion, or a file inside an image or archive and its forks.  NULL with
// *err (negated errno; may be NULL).
#include "source.h"
gs_source_t *vfs_open_source(const char *path, gs_fork_t fork, int *err);

// True when the file at `path` is an image or archive the VFS can descend
// into (the listing's "expandable" flag).  Files that are expensive to probe
// (a compressed archive member) answer false.
bool vfs_is_expandable(const char *path);

// Register the namespace formats and install vfs_open_source as the storage
// engine's path opener.  Called once at start-up (files_init).
void vfs_init(void);

// current_dir accessor + setter (backed by the shell's existing static).
// Today the cwd is a host path string; an image-rooted cwd would need
// extending this with the resolver's auto-mount state.
const char *vfs_get_cwd(void);
void vfs_set_cwd(const char *path);

// Normalise `input` (absolute, or relative to the current directory)
// resolving `.` and `..`: an absolute path in `out`.  0 on success,
// -ENAMETOOLONG when it does not fit.
int vfs_normalise_path(const char *input, char *out, size_t outlen);

#endif // VFS_H
