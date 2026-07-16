// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_cp.c
// Recursive `cp` command built on the VFS.  Copying between any two
// backends works because both source reads and destination writes go
// through vfs_* — copying out of an auto-mounted image into OPFS or the
// host filesystem is the primary Phase 2 user story.  Destination is
// always the host backend in v1 (image writes return -EROFS
// structurally; see §2.9 in proposal-image-vfs.md).
//
// Fork handling: a file copied OUT of an image that carries a resource fork
// and/or non-trivial Finder Info is materialised as an AppleDouble pair — the
// data fork keeps the plain name, and a sibling "._<name>" header file holds
// the resource fork (entry 2) + Finder Info (entry 9).  This is lossless (an
// NDIF `.img`, whose block map lives in the resource fork, survives a
// round-trip) and interoperates with macOS/Netatalk/tar.  Data-only files stay
// single clean streams.  See proposal-appledouble-support.md §4.3.

#include "appledouble.h"
#include "shell.h"
#include "vfs.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Per-run counters so the final summary is useful at scale.
struct cp_stats {
    uint64_t files_copied;
    uint64_t bytes_copied;
    uint64_t dirs_created;
    // Precise failure detail (which side failed, path, offset) set on the
    // first error so callers can distinguish a source read from a dest write.
    char detail[320];
};

// Concatenate two path components with exactly one separator.
static void path_join(char *dst, size_t cap, const char *a, const char *b) {
    size_t alen = strlen(a);
    bool has_sep = alen > 0 && a[alen - 1] == '/';
    if (b[0] == '/')
        b++;
    snprintf(dst, cap, "%s%s%s", a, has_sep ? "" : "/", b);
}

// Return the basename (last path component) of `path` into `out`.
static void path_basename(const char *path, char *out, size_t cap) {
    const char *slash = strrchr(path, '/');
    snprintf(out, cap, "%s", slash ? slash + 1 : path);
}

// Read an entire VFS file into a freshly malloc'd buffer (caller frees).
// Returns 0 on success, or a negative errno.  A missing/opaque synthetic path
// (e.g. "<hostfile>/rsrc/_raw", which only image-backed sources expose) fails
// cleanly here and the caller treats it as "no fork".  Bounded so a corrupt
// length can't run away.
#define FORK_READ_CAP (64u * 1024u * 1024u)
static int read_vfs_file_all(const char *path, uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    vfs_file_t *f = NULL;
    const vfs_backend_t *be = NULL;
    int rc = vfs_open(path, &f, &be);
    if (rc < 0)
        return rc;
    size_t cap = 0, len = 0;
    uint8_t *buf = NULL;
    for (;;) {
        if (len == cap) {
            size_t ncap = cap ? cap * 2 : 4096;
            if (ncap > FORK_READ_CAP)
                ncap = FORK_READ_CAP;
            if (ncap == cap) {
                rc = -EFBIG;
                break;
            }
            uint8_t *nb = realloc(buf, ncap);
            if (!nb) {
                rc = -ENOMEM;
                break;
            }
            buf = nb;
            cap = ncap;
        }
        size_t got = 0;
        rc = be->read(f, len, buf + len, cap - len, &got);
        if (rc < 0)
            break;
        if (got == 0) {
            rc = 0;
            break;
        }
        len += got;
    }
    be->close(f);
    if (rc < 0) {
        free(buf);
        return rc;
    }
    *out = buf;
    *out_len = len;
    return 0;
}

static bool all_zero(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i])
            return false;
    return true;
}

// After a file's data fork has been copied to `dst`, preserve its resource fork
// and Finder Info (if any) as a sibling AppleDouble "._<name>" header file.
// Sources with neither leave `dst` a clean single stream.  Returns 0 on success
// (including the no-fork case) or a negative errno; a fork that exists but
// cannot be preserved is a hard error, since silent loss is the bug being
// fixed.
static int maybe_write_fork_sidecar(const char *src, const char *dst, struct cp_stats *s) {
    char rsrc_path[VFS_PATH_MAX], finf_path[VFS_PATH_MAX];
    snprintf(rsrc_path, sizeof(rsrc_path), "%s/rsrc/_raw", src);
    snprintf(finf_path, sizeof(finf_path), "%s/finf", src);

    uint8_t *rsrc = NULL, *finf = NULL;
    size_t rsrc_len = 0, finf_len = 0;
    (void)read_vfs_file_all(rsrc_path, &rsrc, &rsrc_len); // absent => rsrc_len 0
    (void)read_vfs_file_all(finf_path, &finf, &finf_len);

    // Only Finder Info of exactly 32 bytes and not all-zero is worth carrying.
    const uint8_t *finder = (finf_len == AD_FINDER_INFO_SIZE && !all_zero(finf, finf_len)) ? finf : NULL;
    if (rsrc_len == 0 && !finder) {
        free(rsrc);
        free(finf);
        return 0; // data-only file: no sidecar
    }

    uint8_t *hdr = NULL;
    size_t hdr_len = 0;
    int rc = ad_build_sidecar(rsrc, rsrc_len, finder, &hdr, &hdr_len);
    free(rsrc);
    free(finf);
    if (rc < 0) {
        snprintf(s->detail, sizeof(s->detail), "cannot build AppleDouble header for '%s': %s", dst, strerror(-rc));
        return rc;
    }

    // Sidecar path: "<dir>/._<basename>".
    char sidecar[VFS_PATH_MAX];
    const char *slash = strrchr(dst, '/');
    if (slash) {
        int dirlen = (int)(slash - dst);
        snprintf(sidecar, sizeof(sidecar), "%.*s/._%s", dirlen, dst, slash + 1);
    } else {
        snprintf(sidecar, sizeof(sidecar), "._%s", dst);
    }

    FILE *out = fopen(sidecar, "wb");
    if (!out) {
        int e = errno;
        free(hdr);
        snprintf(s->detail, sizeof(s->detail), "cannot create AppleDouble header '%.240s': %s", sidecar, strerror(e));
        return e ? -e : -EIO;
    }
    size_t wrote = fwrite(hdr, 1, hdr_len, out);
    int close_rc = fclose(out);
    free(hdr);
    if (wrote != hdr_len || close_rc != 0) {
        snprintf(s->detail, sizeof(s->detail), "write error on AppleDouble header '%.260s'", sidecar);
        remove(sidecar);
        return -EIO;
    }
    s->bytes_copied += hdr_len;
    return 0;
}

// Copy bytes from a single source file to a single destination path.
// Returns 0 on success, a negative errno on failure.  Writes counters.
static int copy_file(const char *src, const char *dst, struct cp_stats *s) {
    vfs_file_t *in = NULL;
    const vfs_backend_t *in_be = NULL;
    int rc = vfs_open(src, &in, &in_be);
    if (rc < 0) {
        snprintf(s->detail, sizeof(s->detail), "cannot open source '%s': %s", src, strerror(-rc));
        return rc;
    }

    FILE *out = fopen(dst, "wb");
    if (!out && errno == EIO) {
        // WasmFS stale-inode recovery: a dangling cached inode from an
        // out-of-band OPFS removal makes "wb" fail with EIO even though the
        // path is recreatable — drop the stale entry and retry once. Gated on
        // EIO and never applied to directories, so genuine failures (EISDIR,
        // EACCES, quota) report cleanly with the existing destination intact.
        struct stat st;
        if (stat(dst, &st) != 0 || !S_ISDIR(st.st_mode)) {
            remove(dst);
            out = fopen(dst, "wb");
        }
    }
    if (!out) {
        int e = errno;
        snprintf(s->detail, sizeof(s->detail), "cannot create '%s': %s", dst, strerror(e));
        in_be->close(in);
        return e ? -e : -EIO;
    }

    uint8_t buf[64 * 1024];
    uint64_t off = 0;
    for (;;) {
        size_t got = 0;
        rc = in_be->read(in, off, buf, sizeof(buf), &got);
        if (rc < 0) {
            // Distinguish a source-read failure from a destination-write one so
            // browser/OPFS issues can be told apart from image-read issues.
            snprintf(s->detail, sizeof(s->detail), "read error on '%s' at offset %llu: %s", src,
                     (unsigned long long)off, strerror(-rc));
            fclose(out);
            in_be->close(in);
            return rc;
        }
        if (got == 0)
            break;
        if (fwrite(buf, 1, got, out) != got) {
            int e = errno;
            snprintf(s->detail, sizeof(s->detail), "write error on '%s' at offset %llu: %s", dst,
                     (unsigned long long)off, strerror(e));
            fclose(out);
            in_be->close(in);
            return e ? -e : -EIO;
        }
        off += got;
    }
    fclose(out);
    in_be->close(in);
    s->files_copied++;
    s->bytes_copied += off;

    // Preserve the resource fork / Finder Info as an AppleDouble sidecar when
    // the source carries them (a no-op for data-only files).
    return maybe_write_fork_sidecar(src, dst, s);
}

// Recursive copy.  src may be a file or a directory.  dst is the final
// path to write (not a container).
static int copy_recursive(const char *src, const char *dst, struct cp_stats *s) {
    vfs_stat_t st = {0};
    int rc = vfs_stat(src, &st);
    if (rc < 0)
        return rc;

    if (!(st.mode & VFS_MODE_DIR))
        return copy_file(src, dst, s);

    // Create dst as a directory (ignore "already exists").
    rc = vfs_mkdir(dst);
    if (rc < 0 && rc != -EEXIST)
        return rc;
    if (rc == 0)
        s->dirs_created++;

    vfs_dir_t *d = NULL;
    const vfs_backend_t *be = NULL;
    rc = vfs_opendir(src, &d, &be);
    if (rc < 0)
        return rc;

    // Collect entries first, then recurse — some backends can't tolerate
    // interleaved readdir across nested opens, and the per-dir entry count
    // is bounded.
    char(*names)[256] = NULL;
    size_t n = 0, cap = 0;
    vfs_dirent_t e;
    int r;
    while ((r = be->readdir(d, &e)) > 0) {
        if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0)
            continue;
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 32;
            char(*nb)[256] = realloc(names, ncap * 256);
            if (!nb) {
                rc = -ENOMEM;
                goto done;
            }
            names = nb;
            cap = ncap;
        }
        snprintf(names[n++], 256, "%s", e.name);
    }
    if (r < 0) {
        rc = r;
        goto done;
    }
    be->closedir(d);
    d = NULL;

    for (size_t i = 0; i < n; i++) {
        char sub_src[VFS_PATH_MAX];
        char sub_dst[VFS_PATH_MAX];
        path_join(sub_src, sizeof(sub_src), src, names[i]);
        path_join(sub_dst, sizeof(sub_dst), dst, names[i]);
        rc = copy_recursive(sub_src, sub_dst, s);
        if (rc < 0)
            goto done;
    }
    rc = 0;
done:
    if (d)
        be->closedir(d);
    free(names);
    return rc;
}

// Public entry point: copy `src` to `dst`. Returns 0 on success, negative
// errno on failure. `*out_err` (if not NULL) is set to a static error
// message describing the failure (e.g. "omitting directory 'X' (use -r)").
int shell_cp(const char *src, const char *dst, bool recursive, char *err_buf, size_t err_cap) {
    if (err_buf && err_cap)
        err_buf[0] = '\0';
    if (!src || !dst) {
        if (err_buf && err_cap)
            snprintf(err_buf, err_cap, "usage: cp [-r] <src> <dst>");
        return -EINVAL;
    }

    vfs_stat_t src_st = {0};
    int rc = vfs_stat(src, &src_st);
    if (rc < 0) {
        if (err_buf && err_cap)
            snprintf(err_buf, err_cap, "cp: cannot stat '%s': %s", src, strerror(-rc));
        return rc;
    }
    if ((src_st.mode & VFS_MODE_DIR) && !recursive) {
        if (err_buf && err_cap)
            snprintf(err_buf, err_cap, "cp: omitting directory '%s' (use -r)", src);
        return -EISDIR;
    }

    char final_dst[VFS_PATH_MAX];
    snprintf(final_dst, sizeof(final_dst), "%s", dst);
    vfs_stat_t dst_st = {0};
    if (vfs_stat(dst, &dst_st) == 0 && (dst_st.mode & VFS_MODE_DIR)) {
        char base[256];
        path_basename(src, base, sizeof(base));
        if (base[0])
            path_join(final_dst, sizeof(final_dst), dst, base);
    }

    struct cp_stats s = {0};
    rc = copy_recursive(src, final_dst, &s);
    if (rc < 0) {
        if (err_buf && err_cap) {
            if (s.detail[0])
                snprintf(err_buf, err_cap, "cp: %s", s.detail);
            else
                snprintf(err_buf, err_cap, "cp: copy failed: %s", strerror(-rc));
        }
        return rc;
    }
    if (s.dirs_created > 0)
        printf("copied %llu file(s), %llu byte(s), %llu dir(s) created\n", (unsigned long long)s.files_copied,
               (unsigned long long)s.bytes_copied, (unsigned long long)s.dirs_created);
    else
        printf("copied %llu file(s), %llu byte(s)\n", (unsigned long long)s.files_copied,
               (unsigned long long)s.bytes_copied);
    return 0;
}
