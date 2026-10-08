// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// host_vfs.c
// libc-backed VFS backend.  Every method wraps the POSIX call the shell
// used to invoke directly before the VFS existed.  File I/O uses
// pread-on-fd so vfs_read can accept arbitrary offsets without seek races.

#include "vfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h> // rename() and snprintf()
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// A directory handle is the DIR stream itself, converted to and from the
// opaque vfs_dir_t at the vtable boundary (see vfs.h).
static DIR *as_dir(vfs_dir_t *d) {
    return (DIR *)(void *)d;
}

// An open file: its descriptor.
typedef struct host_vfs_file {
    int fd;
} host_vfs_file_t;

static host_vfs_file_t *as_file(vfs_file_t *f) {
    return (host_vfs_file_t *)(void *)f;
}

// Fill *out from a struct stat.  A symbolic link is VFS_MODE_SYMLINK (only
// lstat ever sees one); any other non-regular, non-directory entry is
// refused with -EINVAL: a FIFO, socket or device would hang or loop the
// commands that read files (cat, cp) -- `/dev/zero` never ends.  The
// directory listing still shows them; they just cannot be read.
static int fill_stat(const struct stat *st, vfs_stat_t *out) {
    memset(out, 0, sizeof(*out));
    if (S_ISDIR(st->st_mode)) {
        out->mode = VFS_MODE_DIR;
    } else if (S_ISREG(st->st_mode)) {
        out->mode = VFS_MODE_FILE;
        out->size = (uint64_t)st->st_size;
    } else if (S_ISLNK(st->st_mode)) {
        out->mode = VFS_MODE_SYMLINK;
    } else {
        return -EINVAL;
    }
#ifdef __EMSCRIPTEN__
    // WasmFS gives a file the time it was first touched this session as its
    // mtime (not OPFS's own), so it is no modification date: report unknown.
    out->mtime = 0;
#else
    out->mtime = (int64_t)st->st_mtime; // time_t is signed: pre-1970 stays negative
#endif
    out->readonly = false;
    return 0;
}

static int host_stat(void *ctx, const char *path, vfs_stat_t *out) {
    (void)ctx;
    if (!path || !out)
        return -EINVAL;
    struct stat st;
    if (stat(path, &st) != 0)
        return -errno;
    return fill_stat(&st, out);
}

static int host_lstat(void *ctx, const char *path, vfs_stat_t *out) {
    (void)ctx;
    if (!path || !out)
        return -EINVAL;
    struct stat st;
    if (lstat(path, &st) != 0)
        return -errno;
    return fill_stat(&st, out);
}

static int host_opendir(void *ctx, const char *path, vfs_dir_t **out) {
    (void)ctx;
    if (!path || !out)
        return -EINVAL;
    DIR *d = opendir(path);
    if (!d)
        return -errno;
    *out = (vfs_dir_t *)(void *)d;
    return 0;
}

// The next entry, skipping "." and ".." -- every listing wants the
// directory's contents, and the image backend has no such entries either.
static int host_readdir(vfs_dir_t *d, vfs_dirent_t *out) {
    if (!d || !out)
        return -EINVAL;
    struct dirent *entry;
    do {
        // A NULL return with errno==0 means EOF; otherwise propagate the errno.
        errno = 0;
        entry = readdir(as_dir(d));
        if (!entry)
            return errno != 0 ? -errno : 0;
    } while (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0);
    memset(out, 0, sizeof(*out));
    int w = snprintf(out->name, sizeof(out->name), "%s", entry->d_name);
    if (w < 0 || (size_t)w >= sizeof(out->name))
        return -ENAMETOOLONG; // caller sees the entry was skipped
    out->has_stat = false;
    return 1;
}

static void host_closedir(vfs_dir_t *d) {
    if (d)
        closedir(as_dir(d));
}

// Open a regular file for reading.  Symbolic links are followed (the shell
// acts for the user; see vfs.h on sandboxing).  Anything but a regular file
// is refused, so a read never meets a FIFO, socket or device: the open is
// non-blocking (a FIFO with no writer would otherwise block here) and the
// type is checked on the descriptor before it is used.
static int host_open(void *ctx, const char *path, vfs_file_t **out) {
    (void)ctx;
    if (!path || !out)
        return -EINVAL;
    // O_CLOEXEC: don't leak the fd across a hypothetical future fork/exec.
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return -errno;
    struct stat st;
    int rc = 0;
    if (fstat(fd, &st) != 0)
        rc = -errno;
    else if (S_ISDIR(st.st_mode))
        rc = -EISDIR;
    else if (!S_ISREG(st.st_mode))
        rc = -EINVAL;
    host_vfs_file_t *handle = rc == 0 ? calloc(1, sizeof(*handle)) : NULL;
    if (rc == 0 && !handle)
        rc = -ENOMEM;
    if (rc != 0) {
        close(fd);
        return rc;
    }
    handle->fd = fd;
    *out = (vfs_file_t *)(void *)handle;
    return 0;
}

// Pin the off_t width so this TU doesn't silently miscompile on a 32-bit
// off_t legacy build — large-file support is required.
_Static_assert(sizeof(off_t) >= 8, "host_vfs requires 64-bit off_t (build with _FILE_OFFSET_BITS=64)");

// One pread.  host_open admits regular files only, so a short count means
// end of file (0 exactly at it) -- never "try again".
static int host_read(vfs_file_t *f, uint64_t off, void *buf, size_t n, size_t *nread) {
    host_vfs_file_t *h = as_file(f);
    if (!h || h->fd < 0 || !buf)
        return -EINVAL;
    if (off > (uint64_t)INT64_MAX)
        return -EINVAL; // beyond any off_t
    ssize_t r = pread(h->fd, buf, n, (off_t)off);
    if (r < 0)
        return -errno;
    if (nread)
        *nread = (size_t)r;
    return 0;
}

static void host_close(vfs_file_t *f) {
    host_vfs_file_t *h = as_file(f);
    if (!h)
        return;
    if (h->fd >= 0)
        close(h->fd);
    free(h);
}

// Create a directory with mode 0777 less the process umask -- the POSIX
// default, what `mkdir` in a terminal does.  The emulator sets no umask of
// its own, so the result is whatever the user's environment asks for.
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

// POSIX rename, or with VFS_RENAME_NOREPLACE one that refuses an existing
// `dst`: atomically through renameat2() where the host has it (Linux), and
// elsewhere (WASM, macOS) by checking first.  A concurrent creator could
// slip in between the check and the rename; nothing else writes the host
// tree under the emulator, so that is acceptable.
static int host_rename(void *ctx, const char *src, const char *dst, unsigned flags) {
    (void)ctx;
    if (!src || !dst)
        return -EINVAL;
    if (flags & ~(unsigned)VFS_RENAME_NOREPLACE)
        return -EINVAL;
    if (flags & VFS_RENAME_NOREPLACE) {
#if defined(__linux__) && !defined(__EMSCRIPTEN__) && defined(RENAME_NOREPLACE)
        if (renameat2(AT_FDCWD, src, AT_FDCWD, dst, RENAME_NOREPLACE) == 0)
            return 0;
        // ENOSYS / EINVAL: the kernel or filesystem lacks the flag, so fall
        // through to the check.
        if (errno != ENOSYS && errno != EINVAL)
            return -errno;
#endif
        struct stat st;
        if (lstat(dst, &st) == 0)
            return -EEXIST;
    }
    if (rename(src, dst) != 0)
        return -errno;
    return 0;
}

// Singleton vtable for the host backend.
static const vfs_backend_t s_host_backend = {
    .scheme = "host",
    .flags = 0,
    .stat = host_stat,
    .lstat = host_lstat,
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
    return &s_host_backend;
}
