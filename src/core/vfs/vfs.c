// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// vfs.c
// Path resolver and convenience wrappers.  Given a shell-facing path, the
// resolver walks host segments left-to-right; if any segment resolves to
// a regular file and further segments follow, it probes the file as a
// disk image and routes the remaining path into the image backend via an
// auto-mount. The "ls/cd descends into a bare image path" rule lives in
// `vfs_resolve_descend`.

#include "vfs.h"

#include "image_vfs.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// The shell's logical current directory, primed from getcwd() on first use.
// Stays a host absolute path; commands that `cd` into an image path have
// their in-image tail re-resolved each time rather than tracking an
// image-rooted cwd.
static char g_cwd[VFS_PATH_MAX] = "";

static void prime_cwd(void) {
    if (g_cwd[0] != '\0')
        return;
    if (!getcwd(g_cwd, sizeof(g_cwd)))
        snprintf(g_cwd, sizeof(g_cwd), "/");
}

// Normalise `input` (absolute or relative to g_cwd) resolving . and ..
// components. Produces an absolute path starting with '/'. Returns 0 on
// success, -ENAMETOOLONG when the joined cwd+input or the assembled output
// would overflow the destination buffer or the component-count cap.
static int normalise_path(const char *input, char *out, size_t outlen) {
    char buf[VFS_PATH_MAX * 2 + 2];
    int n;
    if (input[0] == '/') {
        n = snprintf(buf, sizeof(buf), "%s", input);
    } else {
        prime_cwd();
        n = snprintf(buf, sizeof(buf), "%s/%s", g_cwd, input);
    }
    if (n < 0 || (size_t)n >= sizeof(buf))
        return -ENAMETOOLONG;

    const char *components[128];
    size_t depth = 0;
    const size_t MAX_DEPTH = sizeof(components) / sizeof(components[0]);
    char *saveptr = NULL;
    char *token = strtok_r(buf, "/", &saveptr);
    while (token) {
        if (strcmp(token, ".") == 0) {
            // skip
        } else if (strcmp(token, "..") == 0) {
            if (depth > 0)
                depth--;
        } else if (depth < MAX_DEPTH) {
            components[depth++] = token;
        } else {
            return -ENAMETOOLONG; // too many components
        }
        token = strtok_r(NULL, "/", &saveptr);
    }
    if (depth == 0) {
        if (outlen < 2)
            return -ENAMETOOLONG;
        out[0] = '/';
        out[1] = '\0';
    } else {
        size_t off = 0;
        for (size_t i = 0; i < depth; i++) {
            int w = snprintf(out + off, outlen - off, "/%s", components[i]);
            if (w < 0 || (size_t)w >= outlen - off)
                return -ENAMETOOLONG;
            off += (size_t)w;
        }
    }
    return 0;
}

// Walk `resolved` from left to right.  At the first intermediate segment
// that resolves to a regular file, probe it as an image; on success set
// *out_prefix_len to the byte length of the image-file prefix and return
// the mount.  Return NULL and rc == 0 if no descent is needed (pure host
// path).  On probe failure mid-path return NULL and rc != 0.
static image_mount_t *walk_for_descent(const char *resolved, size_t *out_prefix_len, int *rc) {
    *rc = 0;
    *out_prefix_len = 0;

    const vfs_backend_t *host = vfs_host_backend();

    // Iterate over each '/' position; the substring [0..pos) is the
    // current prefix.  We start after the leading '/' since the root
    // itself is always a directory.
    size_t i = 1;
    size_t len = strlen(resolved);
    char tmp[VFS_PATH_MAX];
    while (i < len) {
        // Advance to the next '/' or end.
        size_t j = i;
        while (j < len && resolved[j] != '/')
            j++;
        // Prefix spans [0..j).
        if (j >= len)
            break; // last component, no trailing segments
        if (j >= sizeof(tmp))
            return NULL;
        memcpy(tmp, resolved, j);
        tmp[j] = '\0';
        vfs_stat_t st;
        int srv = host->stat(NULL, tmp, &st);
        if (srv < 0) {
            // Intermediate component missing — let the caller's op surface
            // the real error against the host backend.
            return NULL;
        }
        if (st.mode & VFS_MODE_FILE) {
            image_mount_t *mount = NULL;
            int pr = image_vfs_acquire_mount(tmp, &mount);
            if (pr == 0) {
                *out_prefix_len = j;
                return mount;
            }
            // Pass real probe-time errors (OOM, mount-table-full, busy) up
            // to the user; collapse only the benign "not-an-image" verdict
            // to ENOTDIR so subsequent path-walking gives a normal error
            // rather than the misleading "out of memory" wording.
            //
            // image_vfs_acquire_mount currently uses -ENOTDIR for "not an
            // image" and -ENOENT for "file vanished mid-probe" — treat both
            // as the benign case.
            if (pr == -ENOTDIR || pr == -ENOENT)
                *rc = -ENOTDIR;
            else
                *rc = pr; // -EBUSY, -ENOMEM, -ENOSPC, ...
            return NULL;
        }
        // Directory: continue past this slash.
        i = j + 1;
    }
    return NULL;
}

// Shared body for vfs_resolve / vfs_resolve_descend.  `descend_bare`
// controls the "ls/cd bare image path" rule.
static int resolve_impl(const char *input, char *resolved, size_t resolved_len, const vfs_backend_t **be, void **ctx,
                        const char **tail, bool descend_bare) {
    if (!input || !resolved || resolved_len == 0)
        return -EINVAL;
    int nrc = normalise_path(input, resolved, resolved_len);
    if (nrc < 0)
        return nrc;

    size_t prefix_len = 0;
    int walk_rc = 0;
    image_mount_t *mount = walk_for_descent(resolved, &prefix_len, &walk_rc);
    if (walk_rc)
        return walk_rc;

    if (!mount && descend_bare) {
        // Try the bare-path exception: if `resolved` itself is a regular
        // file and probes as an image, descend with an empty in-image
        // tail so opendir returns the partition list.
        const vfs_backend_t *host = vfs_host_backend();
        vfs_stat_t st;
        if (host->stat(NULL, resolved, &st) == 0 && (st.mode & VFS_MODE_FILE)) {
            int pr = image_vfs_acquire_mount(resolved, &mount);
            if (pr == 0) {
                prefix_len = strlen(resolved);
            } else if (pr == -EBUSY) {
                return -EBUSY;
            }
        }
    }

    if (mount) {
        if (be)
            *be = vfs_image_backend();
        if (ctx)
            *ctx = mount;
        // tail points at the '/' following the image file path, or at a
        // trailing NUL if the input was a bare image (descend_bare).
        if (tail)
            *tail = resolved + prefix_len;
        return 0;
    }

    if (be)
        *be = vfs_host_backend();
    if (ctx)
        *ctx = NULL;
    if (tail)
        *tail = resolved;
    return 0;
}

int vfs_resolve(const char *input, char *resolved, size_t resolved_len, const vfs_backend_t **be, void **ctx,
                const char **tail) {
    return resolve_impl(input, resolved, resolved_len, be, ctx, tail, false);
}

int vfs_resolve_descend(const char *input, char *resolved, size_t resolved_len, const vfs_backend_t **be, void **ctx,
                        const char **tail) {
    return resolve_impl(input, resolved, resolved_len, be, ctx, tail, true);
}

int vfs_stat(const char *path, vfs_stat_t *out) {
    char resolved[VFS_PATH_MAX];
    const vfs_backend_t *be = NULL;
    void *ctx = NULL;
    const char *tail = NULL;
    int rc = vfs_resolve(path, resolved, sizeof(resolved), &be, &ctx, &tail);
    if (rc)
        return rc;
    return be->stat(ctx, tail, out);
}

int vfs_opendir(const char *path, vfs_dir_t **out, const vfs_backend_t **be_out) {
    char resolved[VFS_PATH_MAX];
    const vfs_backend_t *be = NULL;
    void *ctx = NULL;
    const char *tail = NULL;
    // ls/cd use the descend-bare variant so that `ls foo.img` lists
    // partitions rather than erroring with ENOTDIR.
    int rc = vfs_resolve_descend(path, resolved, sizeof(resolved), &be, &ctx, &tail);
    if (rc)
        return rc;
    rc = be->opendir(ctx, tail, out);
    if (rc == 0 && be_out)
        *be_out = be;
    return rc;
}

int vfs_open(const char *path, vfs_file_t **out, const vfs_backend_t **be_out) {
    char resolved[VFS_PATH_MAX];
    const vfs_backend_t *be = NULL;
    void *ctx = NULL;
    const char *tail = NULL;
    // cat/read keep strict semantics (§2.9): bare image paths read the raw
    // blob, they do not descend.
    int rc = vfs_resolve(path, resolved, sizeof(resolved), &be, &ctx, &tail);
    if (rc)
        return rc;
    rc = be->open(ctx, tail, out);
    if (rc == 0 && be_out)
        *be_out = be;
    return rc;
}

int vfs_mkdir(const char *path) {
    char resolved[VFS_PATH_MAX];
    const vfs_backend_t *be = NULL;
    void *ctx = NULL;
    const char *tail = NULL;
    int rc = vfs_resolve(path, resolved, sizeof(resolved), &be, &ctx, &tail);
    if (rc)
        return rc;
    if (!be->mkdir)
        return -EROFS;
    return be->mkdir(ctx, tail);
}

int vfs_unlink(const char *path) {
    char resolved[VFS_PATH_MAX];
    const vfs_backend_t *be = NULL;
    void *ctx = NULL;
    const char *tail = NULL;
    int rc = vfs_resolve(path, resolved, sizeof(resolved), &be, &ctx, &tail);
    if (rc)
        return rc;
    if (!be->unlink)
        return -EROFS;
    return be->unlink(ctx, tail);
}

int vfs_rename(const char *src, const char *dst) {
    char src_resolved[VFS_PATH_MAX];
    char dst_resolved[VFS_PATH_MAX];
    const vfs_backend_t *src_be = NULL, *dst_be = NULL;
    void *src_ctx = NULL, *dst_ctx = NULL;
    const char *src_tail = NULL, *dst_tail = NULL;
    int rc = vfs_resolve(src, src_resolved, sizeof(src_resolved), &src_be, &src_ctx, &src_tail);
    if (rc)
        return rc;
    rc = vfs_resolve(dst, dst_resolved, sizeof(dst_resolved), &dst_be, &dst_ctx, &dst_tail);
    if (rc)
        return rc;
    if (src_be != dst_be)
        return -EXDEV;
    if (!src_be->rename)
        return -EROFS;
    return src_be->rename(src_ctx, src_tail, dst_tail);
}

const char *vfs_get_cwd(void) {
    prime_cwd();
    return g_cwd;
}

void vfs_set_cwd(const char *path) {
    if (!path)
        return;
    int n = snprintf(g_cwd, sizeof(g_cwd), "%s", path);
    if (n < 0 || (size_t)n >= sizeof(g_cwd)) {
        // Truncation would leave the cwd in a syntactically valid but
        // semantically wrong state. Fall back to root rather than carrying
        // a corrupted path silently. Callers that need stricter validation
        // (path-is-a-directory) do that before invoking us; this is the
        // defensive last line.
        g_cwd[0] = '/';
        g_cwd[1] = '\0';
    }
}
