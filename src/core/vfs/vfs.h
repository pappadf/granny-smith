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
//
// No sandbox: the resolver collapses `.` and `..` lexically and follows
// symbolic links, so a path reaches anything the emulator process can read
// or write.  That is right for the shell and the UI, which act for the
// user; a caller that serves paths on behalf of the guest (or any other
// less trusted party) must confine them itself before calling in.  (The
// AppleTalk file server does not use the VFS; it confines its own paths.)
//
// Threading: the VFS has no locks of its own beyond the mount table's
// slot-identity mutex (image_vfs.c).  The current directory, the resolver
// and every backend call run on one thread at a time -- the shell's job
// thread; a caller on another thread must serialise with it.

#pragma once

#ifndef VFS_H
#define VFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Maximum path length handled by VFS wrappers, including the NUL.  This is
// the project's own cap, deliberately independent of the host's PATH_MAX
// (4096 on Linux): it bounds the fixed-size buffers the resolver and the
// image backend keep, and it is the same on every host and in WASM.  A
// path that does not fit is refused with -ENAMETOOLONG, never truncated.
#define VFS_PATH_MAX 1024

// Maximum number of '/'-separated components in a resolved path; the
// resolver and the image backend share it, and both refuse a deeper path
// with -ENAMETOOLONG.
#define VFS_MAX_COMPONENTS 128

// File-mode bits reported by vfs_stat / vfs_lstat.  `mode` is a bit set:
// test it with `&`, never `==`.  vfs_stat reports exactly one of FILE or
// DIR (it follows symbolic links, and refuses other special files with
// -EINVAL); vfs_lstat reports VFS_MODE_SYMLINK alone for a link.
#define VFS_MODE_FILE    0x1
#define VFS_MODE_DIR     0x2
#define VFS_MODE_SYMLINK 0x4

// Stat result.
typedef struct vfs_stat {
    uint64_t size; // bytes (0 for directories)
    int64_t mtime; // Unix seconds (0 if unavailable; negative before 1970)
    uint32_t mode; // VFS_MODE_* bits
    bool readonly; // true for image-backed paths
} vfs_stat_t;

// One directory entry.
typedef struct vfs_dirent {
    char name[256]; // NUL-terminated; a longer name is an -ENAMETOOLONG readdir
    vfs_stat_t st; // name-only backends may leave st zeroed
    bool has_stat; // true when st was populated
} vfs_dirent_t;

// Opaque file and directory handles.  These two struct types are never
// defined: each backend keeps its own handle type (host_vfs.c and
// image_vfs.c name theirs differently) and converts the pointer at the
// vtable boundary, so no two translation units define one tag two ways.
// A handle is only ever passed back to the backend that returned it.
typedef struct vfs_file vfs_file_t;
typedef struct vfs_dir vfs_dir_t;

// Backend capability flags (vfs_backend_t.flags).
#define VFS_BE_RDONLY 0x1 // no writable operations: vfs.c answers them -EROFS

// vfs_rename flags.
#define VFS_RENAME_NOREPLACE 0x1 // fail with -EEXIST rather than replace `dst`

// Backend vtable.  Every method receives the backend's own ctx pointer
// (which is NULL for the host backend since it is stateless).
//
// Return conventions: every method returns 0 on success or a negated errno,
// except readdir, which streams entries and so has three outcomes:
//   1   an entry was written to *out (call again for the next one)
//   0   end of directory (*out untouched)
//   <0  a negated errno; the listing is incomplete
// The canonical loop is `while ((r = be->readdir(d, &e)) > 0) {...}`
// followed by a check of `r < 0`.
typedef struct vfs_backend {
    const char *scheme; // "host" or "image"
    uint32_t flags; // VFS_BE_* capabilities

    int (*stat)(void *ctx, const char *path, vfs_stat_t *out);
    // Like stat, but a symbolic link is reported as itself (VFS_MODE_SYMLINK)
    // instead of followed.  Optional: NULL for a backend without links, in
    // which case vfs_lstat uses stat.
    int (*lstat)(void *ctx, const char *path, vfs_stat_t *out);
    int (*opendir)(void *ctx, const char *path, vfs_dir_t **out);
    int (*readdir)(vfs_dir_t *d, vfs_dirent_t *out); // 1 = entry, 0 = end, <0 = error (see above)
    void (*closedir)(vfs_dir_t *d);
    int (*open)(void *ctx, const char *path, vfs_file_t **out);
    // Read up to n bytes at `off`; *nread < n only at end of file, and 0
    // there.
    int (*read)(vfs_file_t *f, uint64_t off, void *buf, size_t n, size_t *nread);
    void (*close)(vfs_file_t *f);

    // Writable operations.  A VFS_BE_RDONLY backend leaves them NULL and
    // vfs.c refuses them with -EROFS; only the host backend mutates the
    // filesystem.
    int (*mkdir)(void *ctx, const char *path);
    int (*unlink)(void *ctx, const char *path);
    int (*rename)(void *ctx, const char *src, const char *dst, unsigned flags); // VFS_RENAME_* flags
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
// vfs_stat that reports a symbolic link at `path` as VFS_MODE_SYMLINK
// rather than following it (links before the last component are followed).
int vfs_lstat(const char *path, vfs_stat_t *out);
int vfs_opendir(const char *path, vfs_dir_t **out, const vfs_backend_t **be);
int vfs_open(const char *path, vfs_file_t **out, const vfs_backend_t **be);
int vfs_mkdir(const char *path);
int vfs_unlink(const char *path);
// Rename within one backend and mount (-EXDEV across them).  `flags` is
// VFS_RENAME_* (0: POSIX rename, which replaces an existing `dst`).
int vfs_rename(const char *src, const char *dst, unsigned flags);

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

// The current directory: part of the VFS's resolver state, not the
// shell's.  It is what a relative path is resolved against, here and in
// every vfs_* call, so it lives with the resolver rather than being
// threaded through each caller.  It is always a normalised absolute path;
// it may be a directory inside an image (`cd foo.img/partition1`), whose
// in-image part the resolver re-walks on each use.  Primed from the
// process's getcwd() on first use.
const char *vfs_get_cwd(void);
// Make `path` (absolute, or relative to the current directory) the current
// directory.  0; -ENAMETOOLONG when it does not fit; -ENOTDIR when it is
// not a directory; or the error stat-ing it gave.  The current directory
// is unchanged on failure.
int vfs_set_cwd(const char *path);

// Normalise `input` (absolute, or relative to the current directory)
// resolving `.` and `..`: an absolute path in `out`.  0 on success,
// -ENAMETOOLONG when it does not fit.
int vfs_normalise_path(const char *input, char *out, size_t outlen);

#endif // VFS_H
