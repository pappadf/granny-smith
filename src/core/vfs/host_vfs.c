// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// host_vfs.c
// libc-backed VFS backend.  Every method wraps the POSIX call the shell
// used to invoke directly before the Phase 1 refactor, so behaviour is
// byte-identical.  File I/O uses pread-on-fd so vfs_read can accept
// arbitrary offsets without seek races.

#include "vfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h> // rename() is in <stdio.h> per POSIX
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Concrete shapes of the opaque handles for the host backend.
struct vfs_dir {
    DIR *dir;
};
struct vfs_file {
    int fd;
};

static int host_stat(void *ctx, const char *path, vfs_stat_t *out) {
    (void)ctx;
    if (!path || !out)
        return -EINVAL;
    struct stat st;
    if (stat(path, &st) != 0)
        return -errno;
    memset(out, 0, sizeof(*out));
    if (S_ISDIR(st.st_mode)) {
        out->mode = VFS_MODE_DIR;
        out->size = 0;
    } else if (S_ISREG(st.st_mode)) {
        out->mode = VFS_MODE_FILE;
        out->size = (uint64_t)st.st_size;
    } else {
        // Non-regular non-directory entries (symlinks, sockets, fifos, devices)
        // would otherwise loop the cat/copy commands forever on `/dev/zero` or
        // succeed misleadingly on a socket. Refuse with EINVAL — the directory
        // listing still shows them via readdir, the user just can't read them.
        return -EINVAL;
    }
    out->mtime = (uint32_t)st.st_mtime;
    out->readonly = false;
    return 0;
}

static int host_opendir(void *ctx, const char *path, vfs_dir_t **out) {
    (void)ctx;
    if (!path || !out)
        return -EINVAL;
    DIR *d = opendir(path);
    if (!d)
        return -errno;
    vfs_dir_t *handle = calloc(1, sizeof(*handle));
    if (!handle) {
        closedir(d);
        return -ENOMEM;
    }
    handle->dir = d;
    *out = handle;
    return 0;
}

static int host_readdir(vfs_dir_t *d, vfs_dirent_t *out) {
    if (!d || !d->dir || !out)
        return -EINVAL;
    // A NULL return with errno==0 means EOF; otherwise propagate the errno.
    errno = 0;
    struct dirent *entry = readdir(d->dir);
    if (!entry) {
        if (errno != 0)
            return -errno;
        return 0; // EOF
    }
    memset(out, 0, sizeof(*out));
    int w = snprintf(out->name, sizeof(out->name), "%s", entry->d_name);
    if (w < 0 || (size_t)w >= sizeof(out->name))
        return -ENAMETOOLONG; // caller sees the entry was skipped
    out->has_stat = false;
    return 1;
}

static void host_closedir(vfs_dir_t *d) {
    if (!d)
        return;
    if (d->dir)
        closedir(d->dir);
    free(d);
}

static int host_open(void *ctx, const char *path, vfs_file_t **out) {
    (void)ctx;
    if (!path || !out)
        return -EINVAL;
    // O_CLOEXEC: don't leak the fd across a hypothetical future fork/exec.
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    vfs_file_t *handle = calloc(1, sizeof(*handle));
    if (!handle) {
        close(fd);
        return -ENOMEM;
    }
    handle->fd = fd;
    *out = handle;
    return 0;
}

// Pin the off_t width so this TU doesn't silently miscompile on a 32-bit
// off_t legacy build — large-file support is required.
_Static_assert(sizeof(off_t) >= 8, "host_vfs requires 64-bit off_t (build with _FILE_OFFSET_BITS=64)");

static int host_read(vfs_file_t *f, uint64_t off, void *buf, size_t n, size_t *nread) {
    if (!f || f->fd < 0 || !buf)
        return -EINVAL;
    ssize_t r = pread(f->fd, buf, n, (off_t)off);
    if (r < 0)
        return -errno;
    if (nread)
        *nread = (size_t)r;
    return 0;
}

static void host_close(vfs_file_t *f) {
    if (!f)
        return;
    if (f->fd >= 0)
        close(f->fd);
    free(f);
}

static int host_mkdir(void *ctx, const char *path) {
    (void)ctx;
    if (!path)
        return -EINVAL;
    if (mkdir(path, 0777) != 0)
        return -errno;
    return 0;
}

static int host_unlink(void *ctx, const char *path) {
    (void)ctx;
    if (!path)
        return -EINVAL;
    if (unlink(path) != 0)
        return -errno;
    return 0;
}

static int host_rename(void *ctx, const char *src, const char *dst) {
    (void)ctx;
    if (!src || !dst)
        return -EINVAL;
    if (rename(src, dst) != 0)
        return -errno;
    return 0;
}

// Singleton vtable for the host backend.
static const vfs_backend_t host_backend = {
    .scheme = "host",
    .stat = host_stat,
    .opendir = host_opendir,
    .readdir = host_readdir,
    .closedir = host_closedir,
    .open = host_open,
    .read = host_read,
    .close = host_close,
    .mkdir = host_mkdir,
    .unlink = host_unlink,
    .rename = host_rename,
};

const vfs_backend_t *vfs_host_backend(void) {
    return &host_backend;
}
