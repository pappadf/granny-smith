// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_vfs.h
// The mount table and the VFS backend for paths inside images and archives.
// Path resolution treats a file as a pseudo-directory when the path
// continues past it; on the first such descent the file is opened as a byte
// source, the format registry turns it into a namespace (a disk, a
// filesystem, an archive -- namespace.h), and a mount is registered.  The
// backend's methods (stat/opendir/readdir/...) turn in-mount paths into
// namespace calls, and add the synthetic resource tree ("rsrc", "finf") to
// any file with a resource fork or Finder info.
//
// Mounts are keyed by the key of the source they were opened on
// (source.h), so one file reached by two paths is one mount.  A file inside
// a mount is mounted in turn straight from its source -- a view of the
// parent, or a decode-through fork -- never copied out.
//
// Read-only: the backend is flagged VFS_BE_RDONLY, so vfs.c refuses
// mkdir/unlink/rename with -EROFS before reaching it.

#pragma once

#ifndef IMAGE_VFS_H
#define IMAGE_VFS_H

#include "source.h"
#include "vfs.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

// Opaque mount handle.
typedef struct image_mount image_mount_t;
struct gs_namespace;

// The backend vtable.  ctx passed to its methods is an image_mount_t *.
const vfs_backend_t *vfs_image_backend(void);

// Mount the host file `host_path` (or return the existing mount).  0 and
// *out_mount, or: -ENOTDIR when it is no image or archive; -EBUSY while an
// image containing it is attached writable (image_key_is_open_writable) or
// an unmount is pending; -ENOSPC when the table is full of busy mounts; or
// another negated errno.  Every backend call on an existing mount refuses
// with -EBUSY the same way.
int image_vfs_acquire_mount(const char *host_path, image_mount_t **out_mount);

// Mount forks already open (a file inside another mount).  `path` is the
// VFS path that reached them, shown in listings.  Same results as above.
int image_vfs_acquire_mount_source(const char *path, gs_source_t *data, gs_source_t *rsrc, image_mount_t **out_mount);

// Open fork `fork` of the in-mount path `tail` as a source.  The synthetic
// leaves work too: "<file>/finf" is the Finder info, "<file>/rsrc/_raw" the
// resource fork.  NULL with *err.
gs_source_t *image_vfs_open_source(image_mount_t *m, const char *tail, gs_fork_t fork, int *err);

// Explicit unmount, by the path it was mounted under (or the host file's
// canonical path).  0; -ENOENT if there is none; -EBUSY with handles still
// open, in which case the mount refuses every new call and the last handle
// to close drops it.
int image_vfs_unmount(const char *path);

// Iteration over the current table, for `image list`.  `stale`: see
// image_vfs_mount_info_t.
typedef void (*image_vfs_list_cb)(const char *path, const char *format_name, uint32_t n_partitions, uint32_t refcount,
                                  bool busy, bool stale, void *user);
void image_vfs_list(image_vfs_list_cb cb, void *user);

// Every mount gets a serial number when it is created: a counter that never
// repeats, even though mount slots are reused.  files.mounts[n] is indexed
// by it.
//
// Snapshot of one mount, copied out under the table lock.
typedef struct {
    int serial;
    char path[PATH_MAX]; // the path it was mounted under
    const char *format; // "APM", "HFS", "UFS", or an archive's ("zip", "sit", ...) (static string)
    uint32_t partitions; // entries at the mount's root (partitions of a disk)
    uint32_t refcount; // open handles
    bool unmounting; // unmount requested while handles were live
    bool busy; // refusing service: unmounting, or inside an image attached writable
    bool stale; // its file changed since: a newer mount serves the path, and this one
                // lives only for the handles already open on it (the last drops it)
} image_vfs_mount_info_t;

// The smallest live serial greater than `prev` (-1 to start), or -1.
int image_vfs_next_serial(int prev);

// Fill *out for the mount with `serial`; false when there is none.
bool image_vfs_mount_info(int serial, image_vfs_mount_info_t *out);

// Serial of the mount at `path` (relative or canonical), or -1.
int image_vfs_serial_for_path(const char *path);

#endif // IMAGE_VFS_H
