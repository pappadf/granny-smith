// Stub implementations for image_vfs_* symbols so the VFS unit test binary
// can link without pulling in the full image/storage stack.  These stubs
// short-circuit descent: acquire_mount always reports "not an image" so
// vfs_resolve falls through to host resolution unchanged.  Production
// code provides the real implementations in src/core/vfs/image_vfs.c.

#include "image_vfs.h"

#include <errno.h>

int image_vfs_acquire_mount(const char *host_path, image_mount_t **out_mount) {
    (void)host_path;
    if (out_mount)
        *out_mount = NULL;
    return -ENOTDIR;
}

int image_vfs_unmount(const char *host_path) {
    (void)host_path;
    return -ENOENT;
}

void image_vfs_list(image_vfs_list_cb cb, void *user) {
    (void)cb;
    (void)user;
}

void image_vfs_notify_attached(const char *host_path) {
    (void)host_path;
}

void image_vfs_notify_detached(const char *host_path) {
    (void)host_path;
}

void image_vfs_reset(void) {}

const struct vfs_backend *vfs_image_backend(void) {
    return NULL;
}
