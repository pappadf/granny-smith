// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_vfs.h
// Auto-mount cache + image-backed VFS backend.  Path resolution treats a
// host file as a pseudo-directory when the shell path continues past it;
// on the first such descent we open the file via image_open(), parse its
// partition map, and register a mount.  The resulting mount plus
// in-image path is what backend methods (stat/opendir/readdir/...)
// consume — identical to how host_vfs handles ordinary POSIX paths.
//
// Read-only in Phase 2: mkdir/unlink/rename return -EROFS structurally,
// never conditionally.

#pragma once

#ifndef IMAGE_VFS_H
#define IMAGE_VFS_H

#include "vfs.h"

#include <stdbool.h>
#include <stdint.h>

// Opaque mount handle.  Each cached image file has exactly one of these.
typedef struct image_mount image_mount_t;

// Accessor that returns the image backend vtable.  ctx passed to its
// methods must be a pointer returned by image_vfs_lookup_mount or
// image_vfs_acquire_mount.
const vfs_backend_t *vfs_image_backend(void);

// Probe `host_path` and, if it looks like a supported image (APM for v1),
// register a mount in the cache or return the existing one.  Returns 0 on
// success and sets *out_mount; returns -ENOTDIR if the file is not a
// recognised image, or a negated errno on other failure.
//
// -EBUSY is returned if the file is currently attached via hd/cdrom; the
// caller must fall through to "cannot descend" behaviour.
int image_vfs_acquire_mount(const char *host_path, image_mount_t **out_mount);

// Explicit force-close.  Drops the cache entry for the given absolute
// path (if any) and invalidates handles.  Returns 0 if something was
// dropped, -ENOENT if no match.
int image_vfs_unmount(const char *host_path);

// Iteration over the current cache, for `image list`.  `cb` is called
// once per live mount; return non-zero to stop early.
typedef void (*image_vfs_list_cb)(const char *host_path, const char *format_name, uint32_t n_partitions,
                                  uint32_t refcount, bool conflicted, void *user);
void image_vfs_list(image_vfs_list_cb cb, void *user);

// Notify the mount cache that `host_path` is now attached to the SCSI
// bus.  Any existing mount for that file is marked conflicted; subsequent
// backend calls return -EBUSY.  A subsequent image_vfs_notify_detached
// clears the flag so fresh mounts can proceed.
void image_vfs_notify_attached(const char *host_path);
void image_vfs_notify_detached(const char *host_path);

// Clear the entire cache.  Intended for tests that want a fresh state.
void image_vfs_reset(void);

#endif // IMAGE_VFS_H
