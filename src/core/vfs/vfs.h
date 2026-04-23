// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// vfs.h
// Thin VFS layer: the shell's filesystem commands (ls, cd, cat, ...) call
// through a small backend interface instead of libc directly.  Phase 1 ships
// one backend (host) whose operations are byte-identical to what the FS
// commands used to do; Phase 2 of proposal-image-vfs.md adds an image
// backend plus a resolver that transparently descends into image files.

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
    bool readonly; // true for image-backed paths (Phase 2+)
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
    const char *scheme; // "host" today; "image" in Phase 2

    int (*stat)(void *ctx, const char *path, vfs_stat_t *out);
    int (*opendir)(void *ctx, const char *path, vfs_dir_t **out);
    int (*readdir)(vfs_dir_t *d, vfs_dirent_t *out); // 0=eof, 1=entry, <0 err
    void (*closedir)(vfs_dir_t *d);
    int (*open)(void *ctx, const char *path, vfs_file_t **out);
    int (*read)(vfs_file_t *f, uint64_t off, void *buf, size_t n, size_t *nread);
    void (*close)(vfs_file_t *f);

    // Writable operations.  Image-backed paths return -EROFS in Phase 2+;
    // for now only the host backend implements these.
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
// ergonomic "ls/cd on a bare image path" rule from the proposal (§2.9)
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

// current_dir accessor + setter (backed by the shell's existing static).
// Phase 2 will need to extend this with logical cwds that live inside an
// image; today it is a pure pass-through.
const char *vfs_get_cwd(void);
void vfs_set_cwd(const char *path);

#endif // VFS_H
