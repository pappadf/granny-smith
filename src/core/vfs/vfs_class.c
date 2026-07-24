// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// vfs_class.c
// Object-model class descriptor for `vfs` (vfs.ls / .mkdir / .cat).
// Split out from vfs.c so unit tests linking the core path-resolver
// don't pull in object-model dependencies.

#include "vfs.h"

#include "image_vfs.h"
#include "object.h"
#include "value.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// === Object-model class descriptor =========================================
//
// Wraps the shell's filesystem commands (ls, mkdir, cat) under a single
// object so scripts have a typed entry point. Each method delegates to
// the vfs core API.

static value_t vfs_method_ls(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const char *path = (argc >= 1 && argv[0].s && *argv[0].s) ? argv[0].s : vfs_get_cwd();
    vfs_dir_t *dir = NULL;
    const vfs_backend_t *be = NULL;
    int rc = vfs_opendir(path, &dir, &be);
    if (rc < 0) {
        printf("ls: cannot open directory '%s': %s\n", path, strerror(-rc));
        return val_bool(false);
    }
    vfs_dirent_t entry;
    int r;
    while ((r = be->readdir(dir, &entry)) > 0)
        printf("%s\n", entry.name);
    bool ok = (r == 0);
    if (r < 0) {
        // Surface readdir errors instead of silently truncating the listing.
        printf("ls: readdir error in '%s': %s\n", path, strerror(-r));
    }
    be->closedir(dir);
    return val_bool(ok);
}

// `vfs.list([path])` — like `vfs.ls`, but returns a structured listing the
// GUI can render instead of printing names to stdout. Result is a list of
//   {name: "...", kind: "file"|"directory", size: <bytes>} maps.
// Descends into disk images through the same resolver as `vfs.ls`, so a bare
// image path lists its partitions and a partition path lists the HFS/UFS
// volume. Read-only throughout. Returns V_ERROR (falsy via the bridge) when
// the path can't be opened as a directory.
static value_t vfs_method_list(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const char *path = (argc >= 1 && argv[0].s && *argv[0].s) ? argv[0].s : vfs_get_cwd();
    vfs_dir_t *dir = NULL;
    const vfs_backend_t *be = NULL;
    int rc = vfs_opendir(path, &dir, &be);
    if (rc < 0)
        return val_err("vfs.list: cannot open directory '%s': %s", path, strerror(-rc));

    value_t *items = NULL;
    size_t len = 0, cap = 0;

    vfs_dirent_t entry;
    int r;
    while ((r = be->readdir(dir, &entry)) > 0) {
        // The image backend fills `st` during readdir; the host backend leaves
        // has_stat=false, so stat the child path to classify it (dir vs file)
        // and read its size.
        uint16_t mode = 0;
        uint64_t size = 0;
        if (entry.has_stat) {
            mode = entry.st.mode;
            size = entry.st.size;
        } else {
            char child[VFS_PATH_MAX];
            if (snprintf(child, sizeof(child), "%s/%s", path, entry.name) < (int)sizeof(child)) {
                vfs_stat_t st;
                if (vfs_stat(child, &st) == 0) {
                    mode = st.mode;
                    size = st.size;
                }
            }
        }
        value_map_builder_t *b = val_map_new();
        val_map_put(b, "name", val_str(entry.name));
        val_map_put(b, "kind", val_str((mode & VFS_MODE_DIR) ? "directory" : "file"));
        val_map_put(b, "size", val_int((int64_t)size));
        val_list_push(&items, &len, &cap, val_map_finish(b));
    }
    be->closedir(dir);

    if (r < 0) {
        // readdir failed mid-iteration — discard the partial listing and
        // surface the error rather than returning a truncated one.
        value_t partial = val_list(items, len);
        value_free(&partial);
        return val_err("vfs.list: readdir error in '%s': %s", path, strerror(-r));
    }

    return val_list(items, len);
}

static value_t vfs_method_mkdir(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    const char *dir = argv[0].s;
    if (!dir || !*dir)
        return val_err("vfs.mkdir: expected a non-empty path");
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
    (void)argc;
    const char *path = argv[0].s;
    if (!path || !*path)
        return val_err("vfs.cat: expected a non-empty path");
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
        // Check fwrite return so a closed/redirected stdout doesn't silently
        // drop bytes.
        size_t wrote = fwrite(buf, 1, got, stdout);
        if (wrote != got) {
            printf("cat: write error on stdout (only %zu/%zu bytes)\n", wrote, got);
            be->close(f);
            return val_bool(false);
        }
        off += got;
    }
    be->close(f);
    return val_bool(true);
}

static const arg_decl_t vfs_path_arg[] = {
    {.name = "path", .kind = V_STRING, .doc = "Filesystem path"},
};
static const arg_decl_t vfs_path_arg_optional[] = {
    {.name = "path", .kind = V_STRING, .validation_flags = OBJ_ARG_OPTIONAL, .doc = "Directory path (default: cwd)"},
};

static const member_t vfs_members[] = {
    {.kind = M_METHOD,
     .name = "ls",
     .doc = "List directory contents (or current directory)",
     .method = {.args = vfs_path_arg_optional, .nargs = 1, .result = V_BOOL, .fn = vfs_method_ls}  },
    {.kind = M_METHOD,
     .name = "list",
     .doc = "List a directory as [{name,kind,size}] maps (descends into disk images)",
     .method = {.args = vfs_path_arg_optional, .nargs = 1, .result = V_LIST, .fn = vfs_method_list}},
    {.kind = M_METHOD,
     .name = "mkdir",
     .doc = "Create a directory",
     .method = {.args = vfs_path_arg, .nargs = 1, .result = V_BOOL, .fn = vfs_method_mkdir}        },
    {.kind = M_METHOD,
     .name = "cat",
     .doc = "Print the raw bytes of a file (data fork, rsrc, finder_info)",
     .method = {.args = vfs_path_arg, .nargs = 1, .result = V_BOOL, .fn = vfs_method_cat}          },
};

const class_desc_t vfs_class = {
    .name = "vfs",
    .members = vfs_members,
    .n_members = sizeof(vfs_members) / sizeof(vfs_members[0]),
};

// === Process-singleton lifecycle ============================================
//
// `vfs` is a thin facade attached once to the object root. The methods route
// through the file-level static state in vfs.c (`g_cwd`) and image_vfs.c
// (`g_mounts`); the `vfs` object itself carries no instance data. Register
// once at shell_init.

static struct object *s_vfs_object = NULL;

void vfs_class_register(void) {
    if (s_vfs_object)
        return;
    s_vfs_object = object_new(&vfs_class, NULL, "vfs");
    if (!s_vfs_object) {
        fprintf(stderr, "vfs_class_register: object_new failed; vfs.* unavailable\n");
        return;
    }
    object_attach(object_root(), s_vfs_object);
}

void vfs_class_unregister(void) {
    if (s_vfs_object) {
        object_detach(s_vfs_object);
        object_delete(s_vfs_object);
        s_vfs_object = NULL;
    }
}
