// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// Stub implementations for image_vfs_* symbols so the VFS unit test binary
// can link without pulling in the full image/storage stack.  These stubs
// short-circuit descent: acquire_mount always reports "not an image" so
// vfs_resolve falls through to host resolution unchanged.  Production
// code provides the real implementations in src/core/vfs/image_vfs.c.

#include "image.h"
#include "image_vfs.h"

#include <errno.h>
#include <stddef.h>

int image_vfs_acquire_mount(const char *host_path, image_mount_t **out_mount) {
    (void)host_path;
    if (out_mount)
        *out_mount = NULL;
    return -ENOTDIR;
}

int image_vfs_acquire_mount_source(const char *path, source_t *data, source_t *rsrc, image_mount_t **out_mount) {
    (void)path;
    (void)data;
    (void)rsrc;
    if (out_mount)
        *out_mount = NULL;
    return -ENOTDIR; // no nested descent in the stubbed VFS unit test
}

source_t *image_vfs_open_source(image_mount_t *m, const char *tail, source_fork_t fork, int *err) {
    (void)m;
    (void)tail;
    (void)fork;
    if (err)
        *err = -ENOENT;
    return NULL;
}

// The namespace formats are not linked: nothing is a namespace here.
void ns_register_formats(void) {}

// Image-layer symbols referenced by vfs_export_raw_image().  The stubbed VFS
// unit test never exercises the export path, so these just satisfy the linker.
image_t *image_open_readonly_source(const char *name, struct peel_source *data, struct peel_source *rsrc) {
    (void)name;
    (void)data;
    (void)rsrc;
    return NULL;
}

int image_export_to(image_t *image, const char *dest_path) {
    (void)image;
    (void)dest_path;
    return -1;
}

void image_close(image_t *image) {
    (void)image;
}

int image_vfs_unmount(const char *host_path) {
    (void)host_path;
    return -ENOENT;
}

void image_vfs_list(image_vfs_list_cb cb, void *user) {
    (void)cb;
    (void)user;
}

const struct vfs_backend *vfs_image_backend(void) {
    return NULL;
}
