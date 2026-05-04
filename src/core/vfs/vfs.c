// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// vfs.c
// Path resolver and convenience wrappers.  Given a shell-facing path, the
// resolver walks host segments left-to-right; if any segment resolves to
// a regular file and further segments follow, it probes the file as a
// disk image and routes the remaining path into the image backend via an
// auto-mount (proposal-image-vfs.md §2.9).

#include "vfs.h"

#include "image_vfs.h"
#include "object.h"
#include "value.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// The shell's logical current directory, primed from getcwd() on first use
// (see Phase 1 summary).  Stays a host absolute path; Phase 2 does not yet
// extend it with logical cwds that live inside images — commands that
// `cd` into an image path have their in-image tail re-resolved each time.
static char g_cwd[VFS_PATH_MAX] = "";

static void prime_cwd(void) {
    if (g_cwd[0] != '\0')
        return;
    if (!getcwd(g_cwd, sizeof(g_cwd)))
        snprintf(g_cwd, sizeof(g_cwd), "/");
}

// Normalise `input` (absolute or relative to g_cwd) resolving . and ..
// components.  Produces an absolute path starting with '/'.
static void normalise_path(const char *input, char *out, size_t outlen) {
    char buf[VFS_PATH_MAX * 2 + 2];
    if (input[0] == '/') {
        snprintf(buf, sizeof(buf), "%s", input);
    } else {
        prime_cwd();
        snprintf(buf, sizeof(buf), "%s/%s", g_cwd, input);
    }

    const char *components[128];
    int depth = 0;
    char *saveptr = NULL;
    char *token = strtok_r(buf, "/", &saveptr);
    while (token) {
        if (strcmp(token, ".") == 0) {
            // skip
        } else if (strcmp(token, "..") == 0) {
            if (depth > 0)
                depth--;
        } else if (depth < (int)(sizeof(components) / sizeof(components[0]))) {
            components[depth++] = token;
        }
        token = strtok_r(NULL, "/", &saveptr);
    }
    if (depth == 0) {
        snprintf(out, outlen, "/");
    } else {
        out[0] = '\0';
        for (int i = 0; i < depth; i++) {
            size_t len = strlen(out);
            snprintf(out + len, outlen - len, "/%s", components[i]);
        }
    }
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
            // EBUSY propagates.  Any other probe failure means "not an
            // image with segments past it" — signal ENOTDIR so downstream
            // calls stop rather than trying to readdir a blob.
            *rc = (pr == -EBUSY) ? -EBUSY : -ENOTDIR;
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
    normalise_path(input, resolved, resolved_len);

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
    snprintf(g_cwd, sizeof(g_cwd), "%s", path);
}

// === Object-model class descriptor =========================================
//
// Wraps the shell's filesystem commands (ls, mkdir, cat) under a single
// object so scripts have a typed entry point. Each method delegates to
// the vfs core API.

static value_t vfs_method_ls(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const char *path = (argc >= 1 && argv[0].kind == V_STRING && argv[0].s && *argv[0].s) ? argv[0].s : vfs_get_cwd();
    vfs_dir_t *dir = NULL;
    const vfs_backend_t *be = NULL;
    int rc = vfs_opendir(path, &dir, &be);
    if (rc < 0) {
        printf("ls: cannot open directory '%s': %s\n", path, strerror(-rc));
        return val_bool(true);
    }
    vfs_dirent_t entry;
    int r;
    while ((r = be->readdir(dir, &entry)) > 0)
        printf("%s\n", entry.name);
    be->closedir(dir);
    return val_bool(true);
}

static value_t vfs_method_mkdir(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("vfs.mkdir: expected (path)");
    const char *dir = argv[0].s ? argv[0].s : "";
    int rc = vfs_mkdir(dir);
    if (rc == 0) {
        printf("Directory '%s' created\n", dir);
        return val_bool(true);
    }
    printf("mkdir: cannot create directory '%s': %s\n", dir, strerror(-rc));
    return val_bool(false);
}

static value_t vfs_method_cat(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("vfs.cat: expected (path)");
    const char *path = argv[0].s ? argv[0].s : "";
    vfs_file_t *f = NULL;
    const vfs_backend_t *be = NULL;
    int rc = vfs_open(path, &f, &be);
    if (rc < 0) {
        printf("cat: cannot open '%s': %s\n", path, strerror(-rc));
        return val_bool(false);
    }
    uint8_t buf[4096];
    uint64_t off = 0;
    for (;;) {
        size_t got = 0;
        int rr = be->read(f, off, buf, sizeof(buf), &got);
        if (rr < 0) {
            printf("cat: read error on '%s': %s\n", path, strerror(-rr));
            be->close(f);
            return val_bool(false);
        }
        if (got == 0)
            break;
        fwrite(buf, 1, got, stdout);
        off += got;
    }
    be->close(f);
    return val_bool(true);
}

static const arg_decl_t vfs_path_arg[] = {
    {.name = "path", .kind = V_STRING, .doc = "Filesystem path"},
};
static const arg_decl_t vfs_path_arg_optional[] = {
    {.name = "path", .kind = V_STRING, .flags = OBJ_ARG_OPTIONAL, .doc = "Directory path (default: cwd)"},
};

static const member_t vfs_members[] = {
    {.kind = M_METHOD,
     .name = "ls",
     .doc = "List directory contents (or current directory)",
     .method = {.args = vfs_path_arg_optional, .nargs = 1, .result = V_BOOL, .fn = vfs_method_ls}},
    {.kind = M_METHOD,
     .name = "mkdir",
     .doc = "Create a directory",
     .method = {.args = vfs_path_arg, .nargs = 1, .result = V_BOOL, .fn = vfs_method_mkdir}      },
    {.kind = M_METHOD,
     .name = "cat",
     .doc = "Print the contents of a text file",
     .method = {.args = vfs_path_arg, .nargs = 1, .result = V_BOOL, .fn = vfs_method_cat}        },
};

const class_desc_t vfs_class = {
    .name = "vfs",
    .members = vfs_members,
    .n_members = sizeof(vfs_members) / sizeof(vfs_members[0]),
};
