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
// Fork handling (§2.8) is not implemented in v1: HFS files are extracted
// as their data fork only.  Opening the reserved sub-paths `<file>/rsrc`
// and `<file>/finf` from the shell still works, so resource forks can be
// extracted manually.

#include "cmd_types.h"
#include "shell.h"
#include "vfs.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Per-run counters so the final summary is useful at scale.
struct cp_stats {
    uint64_t files_copied;
    uint64_t bytes_copied;
    uint64_t dirs_created;
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

// Copy bytes from a single source file to a single destination path.
// Returns 0 on success, a negative errno on failure.  Writes counters.
static int copy_file(const char *src, const char *dst, struct cp_stats *s) {
    vfs_file_t *in = NULL;
    const vfs_backend_t *in_be = NULL;
    int rc = vfs_open(src, &in, &in_be);
    if (rc < 0)
        return rc;

    FILE *out = fopen(dst, "wb");
    if (!out) {
        in_be->close(in);
        return -errno;
    }

    uint8_t buf[64 * 1024];
    uint64_t off = 0;
    for (;;) {
        size_t got = 0;
        rc = in_be->read(in, off, buf, sizeof(buf), &got);
        if (rc < 0) {
            fclose(out);
            in_be->close(in);
            return rc;
        }
        if (got == 0)
            break;
        if (fwrite(buf, 1, got, out) != got) {
            int werr = -errno;
            fclose(out);
            in_be->close(in);
            return werr;
        }
        off += got;
    }
    fclose(out);
    in_be->close(in);
    s->files_copied++;
    s->bytes_copied += off;
    return 0;
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
        if (err_buf && err_cap)
            snprintf(err_buf, err_cap, "cp: copy failed: %s", strerror(-rc));
        return rc;
    }
    printf("copied %llu file(s), %llu byte(s)%s\n", (unsigned long long)s.files_copied,
           (unsigned long long)s.bytes_copied, s.dirs_created ? "" : "");
    return 0;
}

// cp [-r] <src> <dst>
// If dst is an existing directory, we append basename(src) to dst before
// recursing (POSIX behaviour).
static void cmd_cp(struct cmd_context *ctx, struct cmd_result *res) {
    bool recursive = false;
    const char *src = NULL;
    const char *dst = NULL;
    for (int i = 1; i < ctx->raw_argc; i++) {
        const char *a = ctx->raw_argv[i];
        if (strcmp(a, "-r") == 0 || strcmp(a, "-R") == 0) {
            recursive = true;
        } else if (!src) {
            src = a;
        } else if (!dst) {
            dst = a;
        } else {
            cmd_err(res, "cp: too many arguments");
            return;
        }
    }
    char err[256] = {0};
    int rc = shell_cp(src, dst, recursive, err, sizeof(err));
    if (rc < 0) {
        cmd_err(res, "%s", err[0] ? err : "cp: failed");
        return;
    }
    cmd_ok(res);
}

// Argument specs for cp.  We rely on ARG_REST since -r mixes with
// positional args; the handler re-parses raw_argv.
static const struct arg_spec cp_args[] = {
    {"args", ARG_REST, "[-r] <src> <dst>"},
};

// Public registration hook, called from shell_init.
void cmd_cp_register(void);
void cmd_cp_register(void) {
    register_command(&(struct cmd_reg){
        .name = "cp",
        .category = "Filesystem",
        .synopsis = "cp [-r] <src> <dst> - copy file or directory",
        .fn = cmd_cp,
        .args = cp_args,
        .nargs = 1,
    });
}
