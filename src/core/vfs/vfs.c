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

#include "format_registry.h"
#include "image.h"
#include "image_vfs.h"
#include "namespace.h"
#include "source.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// The current directory (see vfs_get_cwd in vfs.h), primed from getcwd()
// on first use.  Always a normalised absolute path; one inside an image
// keeps its in-image part, which the resolver re-walks on each use.
static char s_cwd[VFS_PATH_MAX] = "";

static void prime_cwd(void) {
    if (s_cwd[0] != '\0')
        return;
    if (!getcwd(s_cwd, sizeof(s_cwd)))
        snprintf(s_cwd, sizeof(s_cwd), "/");
}

// Append the components of `path` to the normalised path being built in
// out[0..*len), which holds *depth components ("" while it is the root).
// `.` is skipped and `..` drops the last component (never past the root).
// 0, or -ENAMETOOLONG when a component would not fit in `outlen` (with its
// NUL) or the path would exceed VFS_MAX_COMPONENTS.
static int append_components(const char *path, char *out, size_t outlen, size_t *len, size_t *depth) {
    const char *p = path;
    while (*p) {
        while (*p == '/')
            p++;
        const char *start = p;
        while (*p && *p != '/')
            p++;
        size_t n = (size_t)(p - start);
        if (n == 0 || (n == 1 && start[0] == '.'))
            continue;
        if (n == 2 && start[0] == '.' && start[1] == '.') {
            if (*depth > 0) {
                // Drop "/name": back up to the slash that starts it.
                while (out[--*len] != '/')
                    ;
                (*depth)--;
            }
            continue;
        }
        if (*depth >= VFS_MAX_COMPONENTS || *len + 1 + n >= outlen)
            return -ENAMETOOLONG;
        out[(*len)++] = '/';
        memcpy(out + *len, start, n);
        *len += n;
        (*depth)++;
    }
    return 0;
}

// Normalise `input` (absolute or relative to the current directory)
// resolving . and .. components, straight into `out`: no scratch copy of
// the joined path.  Produces an absolute path starting with '/'.  `input`
// and `out` must not overlap.  Returns 0 on success, -ENAMETOOLONG when the
// result would overflow `out` or exceed VFS_MAX_COMPONENTS.
int vfs_normalise_path(const char *input, char *out, size_t outlen) {
    if (outlen < 2)
        return -ENAMETOOLONG;
    size_t len = 0, depth = 0;
    int rc = 0;
    if (input[0] != '/') {
        prime_cwd();
        rc = append_components(s_cwd, out, outlen, &len, &depth);
    }
    if (rc == 0)
        rc = append_components(input, out, outlen, &len, &depth);
    if (rc < 0)
        return rc;
    if (len == 0)
        out[len++] = '/'; // the root
    out[len] = '\0';
    return 0;
}

// Mount the host file at `path` if it is an image or archive.  0 and *out;
// -ENOTDIR for the benign verdict -- not an image (or the file vanished
// mid-probe); or a real probe-time error (-EBUSY, -ENOMEM, -ENOSPC, ...)
// that the user should see instead of a misleading "not a directory".
// The one probe both the walk and the bare-image rule use.
static int probe_host_file(const char *path, image_mount_t **out) {
    int pr = image_vfs_acquire_mount(path, out);
    if (pr == -ENOTDIR || pr == -ENOENT)
        return -ENOTDIR;
    return pr;
}

// Walk `resolved` from left to right.  At the first intermediate segment
// that resolves to a regular file, probe it as an image or archive; on success set
// *out_prefix_len to the byte length of the image-file prefix and return
// the mount.  Return NULL and rc == 0 if no descent is needed (pure host
// path, or a missing component the caller's own op reports as ENOENT).
// Return NULL and rc != 0 when the walk itself fails: a probe failure, a
// prefix that is neither directory nor file (-ENOTDIR), or a stat error
// other than "missing" (-EACCES, -ELOOP, -EIO, ...), which the caller's op
// would otherwise misreport.
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
            if (srv == -EINVAL)
                *rc = -ENOTDIR; // a FIFO, socket or device has no children
            else if (srv != -ENOENT)
                *rc = srv;
            return NULL;
        }
        if (st.mode & VFS_MODE_FILE) {
            image_mount_t *mount = NULL;
            int pr = probe_host_file(tmp, &mount);
            if (pr == 0) {
                *out_prefix_len = j;
                return mount;
            }
            *rc = pr;
            return NULL;
        }
        // Directory: continue past this slash.
        i = j + 1;
    }
    return NULL;
}

// True if any '/'-separated component of the in-image path `tail` (which
// starts with '/') is a synthetic fork keyword ("rsrc" or "finf").  Such
// paths address the *outer* file's forks, not a nested image, so nested
// descent must skip them.
static bool tail_has_fork_component(const char *tail) {
    const char *p = tail;
    while (*p) {
        while (*p == '/')
            p++;
        const char *start = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t)(p - start);
        if ((len == 4 && strncmp(start, "rsrc", 4) == 0) || (len == 4 && strncmp(start, "finf", 4) == 0))
            return true;
    }
    return false;
}

// True if the FIRST component of `rem` (which starts with '/') is a synthetic
// fork keyword — i.e. `rem` addresses the just-matched file's own fork rather
// than descending into it as a nested image.  Deeper "rsrc"/"finf" belong to
// files *inside* a nested image and must not disqualify the descent.
static bool first_component_is_fork(const char *rem) {
    const char *p = rem;
    while (*p == '/')
        p++;
    const char *start = p;
    while (*p && *p != '/')
        p++;
    size_t len = (size_t)(p - start);
    return len == 4 && (strncmp(start, "rsrc", 4) == 0 || strncmp(start, "finf", 4) == 0);
}

// Given an outer image mount `m` and its in-image tail (starting with '/'),
// find whether the tail descends through a nested image FILE.  On success
// returns true, sets *split to the byte length within `tail` of that file's
// subpath, and *rem to `tail + split` (the remaining in-image path of the
// nested image, starting with '/').  Fork-suffix tails are left to the image
// backend and do not count as nested descent.
static bool find_nested_split(image_mount_t *m, const char *tail, size_t *split, const char **rem) {
    const vfs_backend_t *ib = vfs_image_backend();
    size_t len = strlen(tail);
    for (size_t j = 1; j < len; j++) {
        if (tail[j] != '/')
            continue;
        char tmp[VFS_PATH_MAX];
        if (j >= sizeof(tmp))
            return false;
        memcpy(tmp, tail, j);
        tmp[j] = '\0';
        vfs_stat_t st;
        if (ib->stat(m, tmp, &st) != 0)
            continue; // not resolvable at this prefix; keep walking
        if (st.mode & VFS_MODE_FILE) {
            const char *r = tail + j; // remaining path, starts with '/'
            if (first_component_is_fork(r))
                return false; // this file's own fork access, not a nested image
            *split = j;
            *rem = r;
            return true;
        }
    }
    return false;
}

// Mount the file at in-mount path `sub` of `outer`, a file whose VFS path is
// the first `path_len` bytes of `resolved`.  0 and *out, or a negated errno
// (-ENOTDIR when it is no image or archive).
static int mount_member(image_mount_t *outer, const char *sub, const char *resolved, size_t path_len,
                        image_mount_t **out) {
    int err = 0;
    source_t *data = image_vfs_open_source(outer, sub, GS_FORK_DATA, &err);
    if (!data)
        return err ? err : -ENOENT;
    source_t *rsrc = image_vfs_open_source(outer, sub, GS_FORK_RSRC, NULL);
    char path[VFS_PATH_MAX];
    snprintf(path, sizeof(path), "%.*s", (int)path_len, resolved);
    int rc = image_vfs_acquire_mount_source(path, data, rsrc, out);
    source_release(data);
    source_release(rsrc);
    return rc;
}

// Shared body for vfs_resolve / vfs_resolve_descend.  `descend_bare`
// controls the "ls/cd bare image path" rule.
static int resolve_impl(const char *input, char *resolved, size_t resolved_len, const vfs_backend_t **be, void **ctx,
                        const char **tail, bool descend_bare) {
    if (!input || !resolved || resolved_len == 0)
        return -EINVAL;
    int nrc = vfs_normalise_path(input, resolved, resolved_len);
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
            int pr = probe_host_file(resolved, &mount);
            if (pr == 0)
                prefix_len = strlen(resolved);
            else if (pr != -ENOTDIR)
                return pr; // a real failure, not "a plain file"
        }
    }

    // Nested descent: the in-mount tail may itself run through a file that
    // is an image or an archive (an NDIF .img inside a Toast CD, a .sit
    // inside a .zip).  Each such file is opened as a source -- a view of its
    // parent, or a decode-through fork -- and mounted in turn, and the rest
    // of the tail resolves inside it.  Nothing is copied out.  Bounded, so a
    // file that contains itself cannot loop.
    for (int depth = 0; mount && depth < 16; depth++) {
        const char *ntail = resolved + prefix_len;
        size_t split = 0;
        const char *rem = NULL;
        if (!find_nested_split(mount, ntail, &split, &rem))
            break;
        char sub[VFS_PATH_MAX];
        if (split >= sizeof(sub))
            break;
        memcpy(sub, ntail, split);
        sub[split] = '\0';
        image_mount_t *nested = NULL;
        int pr = mount_member(mount, sub, resolved, prefix_len + split, &nested);
        if (pr != 0)
            return (pr == -ENOTDIR || pr == -ENOENT) ? -ENOTDIR : pr;
        mount = nested;
        prefix_len = (size_t)(rem - resolved);
    }

    // Bare nested image (ls/cd on the inner file itself): descend so its
    // root is shown, mirroring the top-level bare-image rule.
    if (mount && descend_bare) {
        const char *ntail = resolved + prefix_len;
        if (ntail[0] == '/' && ntail[1] && !tail_has_fork_component(ntail)) {
            vfs_stat_t st;
            if (vfs_image_backend()->stat(mount, ntail, &st) == 0 && (st.mode & VFS_MODE_FILE)) {
                image_mount_t *nested = NULL;
                if (mount_member(mount, ntail, resolved, strlen(resolved), &nested) == 0) {
                    mount = nested;
                    prefix_len = strlen(resolved);
                }
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

int vfs_lstat(const char *path, vfs_stat_t *out) {
    char resolved[VFS_PATH_MAX];
    const vfs_backend_t *be = NULL;
    void *ctx = NULL;
    const char *tail = NULL;
    int rc = vfs_resolve(path, resolved, sizeof(resolved), &be, &ctx, &tail);
    if (rc)
        return rc;
    return be->lstat ? be->lstat(ctx, tail, out) : be->stat(ctx, tail, out);
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
    // cat/read keep strict semantics: bare image paths read the raw
    // blob, they do not descend.
    int rc = vfs_resolve(path, resolved, sizeof(resolved), &be, &ctx, &tail);
    if (rc)
        return rc;
    rc = be->open(ctx, tail, out);
    if (rc == 0 && be_out)
        *be_out = be;
    return rc;
}

// Resolve `path` for a writable operation: 0 with the backend triple, or
// -EROFS for a read-only backend (the one place that refusal is made), or
// the resolver's error.  `resolved` (VFS_PATH_MAX) holds the path *tail
// points into.
static int resolve_writable(const char *path, char *resolved, const vfs_backend_t **be, void **ctx, const char **tail) {
    int rc = vfs_resolve(path, resolved, VFS_PATH_MAX, be, ctx, tail);
    if (rc)
        return rc;
    return ((*be)->flags & VFS_BE_RDONLY) ? -EROFS : 0;
}

int vfs_mkdir(const char *path) {
    char resolved[VFS_PATH_MAX];
    const vfs_backend_t *be = NULL;
    void *ctx = NULL;
    const char *tail = NULL;
    int rc = resolve_writable(path, resolved, &be, &ctx, &tail);
    if (rc)
        return rc;
    return be->mkdir(ctx, tail);
}

int vfs_unlink(const char *path) {
    char resolved[VFS_PATH_MAX];
    const vfs_backend_t *be = NULL;
    void *ctx = NULL;
    const char *tail = NULL;
    int rc = resolve_writable(path, resolved, &be, &ctx, &tail);
    if (rc)
        return rc;
    return be->unlink(ctx, tail);
}

int vfs_rename(const char *src, const char *dst, unsigned flags) {
    // Both tails point into these two buffers, which outlive the backend
    // call below; nothing keeps a tail past it.
    char src_resolved[VFS_PATH_MAX];
    char dst_resolved[VFS_PATH_MAX];
    const vfs_backend_t *src_be = NULL, *dst_be = NULL;
    void *src_ctx = NULL, *dst_ctx = NULL;
    const char *src_tail = NULL, *dst_tail = NULL;
    int rc = resolve_writable(src, src_resolved, &src_be, &src_ctx, &src_tail);
    if (rc)
        return rc;
    rc = resolve_writable(dst, dst_resolved, &dst_be, &dst_ctx, &dst_tail);
    if (rc)
        return rc;
    // One rename domain is one backend *and* one context: the host backend
    // (ctx NULL), or one mount of a writable image backend.  Anything else
    // would be a copy, which rename is not.
    if (src_be != dst_be || src_ctx != dst_ctx)
        return -EXDEV;
    return src_be->rename(src_ctx, src_tail, dst_tail, flags);
}

static void set_err(char *err, size_t cap, const char *msg) {
    if (err && cap)
        snprintf(err, cap, "%s", msg);
}

int vfs_export_raw_image(const char *src, const char *dst, char *err, size_t err_cap) {
    if (!src || !dst) {
        set_err(err, err_cap, "export_raw: missing src/dst");
        return -EINVAL;
    }

    char resolved[VFS_PATH_MAX];
    const vfs_backend_t *be = NULL;
    void *ctx = NULL;
    const char *tail = NULL;
    // Strict resolve (not descend): a bare image path stays a "file" so a
    // nested `.img` resolves to (image backend, outer mount, in-image tail)
    // rather than descending into its partition list.
    int rc = vfs_resolve(src, resolved, sizeof(resolved), &be, &ctx, &tail);
    if (rc) {
        set_err(err, err_cap, "export_raw: cannot resolve source path");
        return rc;
    }

    // Open the source -- a host file, or a file inside a mount -- through
    // the same opener every image uses, so every format and every nesting
    // flattens alike.
    int oerr = 0;
    source_t *data = NULL, *rsrc = NULL;
    if (be == vfs_image_backend() && ctx) {
        data = image_vfs_open_source((image_mount_t *)ctx, tail, GS_FORK_DATA, &oerr);
        rsrc = data ? image_vfs_open_source((image_mount_t *)ctx, tail, GS_FORK_RSRC, NULL) : NULL;
    } else {
        data = source_open_host_path(resolved, GS_FORK_DATA, &oerr);
        rsrc = data ? source_open_host_path(resolved, GS_FORK_RSRC, NULL) : NULL;
    }
    if (!data) {
        set_err(err, err_cap, "export_raw: source is not a file");
        return oerr ? oerr : -ENOENT;
    }
    image_t *img = image_open_readonly_source(resolved, data, rsrc);
    source_release(data);
    source_release(rsrc);
    if (!img) {
        set_err(err, err_cap, "export_raw: source is not a recognised disk image");
        return -EIO;
    }
    // Flatten its logical media to a new raw file.  image_export_to refuses
    // to overwrite and creates parent directories.
    int erc = image_export_to(img, dst);
    image_close(img);
    if (erc != 0) {
        set_err(err, err_cap, "export_raw: write failed (destination exists or not writable)");
        return -EIO;
    }
    return 0;
}

source_t *vfs_open_source(const char *path, source_fork_t fork, int *err) {
    int e = 0;
    if (!err)
        err = &e;
    char resolved[VFS_PATH_MAX];
    const vfs_backend_t *be = NULL;
    void *ctx = NULL;
    const char *tail = NULL;
    // Strict: a bare image path is the image file, not its contents.
    *err = vfs_resolve(path, resolved, sizeof(resolved), &be, &ctx, &tail);
    if (*err)
        return NULL;
    if (be == vfs_image_backend())
        return image_vfs_open_source((image_mount_t *)ctx, tail, fork, err);
    return source_open_host_path(resolved, fork, err);
}

// The storage engine's path opener: whatever vfs_open_source resolves.
static source_t *vfs_path_opener(const char *path, source_fork_t fork, int *err) {
    return vfs_open_source(path, fork, err);
}

void vfs_init(void) {
    ns_register_formats();
    source_set_path_opener(vfs_path_opener);
}

bool vfs_is_expandable(const char *path) {
    int err = 0;
    source_t *data = vfs_open_source(path, GS_FORK_DATA, &err);
    if (!data)
        return false;
    // Only a file whose head and tail are cheap to read now is probed:
    // deciding for a compressed archive member not yet decoded would mean
    // decoding all of it.
    bool yes = false;
    if (source_tier(data) <= GS_TIER_INDEXED) {
        source_t *rsrc = vfs_open_source(path, GS_FORK_RSRC, NULL);
        yes = format_is_namespace(data, rsrc);
        source_release(rsrc);
    }
    source_release(data);
    return yes;
}

const char *vfs_get_cwd(void) {
    prime_cwd();
    return s_cwd;
}

int vfs_set_cwd(const char *path) {
    if (!path)
        return -EINVAL;
    char abs[VFS_PATH_MAX];
    int rc = vfs_normalise_path(path, abs, sizeof(abs));
    if (rc < 0)
        return rc;
    vfs_stat_t st;
    rc = vfs_stat(abs, &st);
    if (rc < 0)
        return rc;
    if (!(st.mode & VFS_MODE_DIR))
        return -ENOTDIR;
    memcpy(s_cwd, abs, strlen(abs) + 1); // fits: both are VFS_PATH_MAX
    return 0;
}
