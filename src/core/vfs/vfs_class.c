// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// vfs_class.c
// The directory-level file methods of the `files` node (files.ls / .list /
// .mkdir / .cat); the member table lives with the rest of `files` in
// storage/storage_class.c.  Split out from vfs.c so unit tests linking the
// core path-resolver don't pull in object-model dependencies.

#include "vfs_class.h"
#include "gs_out.h"
#include "vfs.h"

#include "image_vfs.h"
#include "object.h"
#include "value.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Each method delegates to the vfs core API.

value_t files_method_ls(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const char *path = (argc >= 1 && argv[0].s && *argv[0].s) ? argv[0].s : vfs_get_cwd();
    vfs_dir_t *dir = NULL;
    const vfs_backend_t *be = NULL;
    int rc = vfs_opendir(path, &dir, &be);
    if (rc < 0) {
        gs_outf("ls: cannot open directory '%s': %s\n", path, strerror(-rc));
        return val_bool(false);
    }
    vfs_dirent_t entry;
    int r;
    while ((r = be->readdir(dir, &entry)) > 0)
        gs_outf("%s\n", entry.name);
    bool ok = (r == 0);
    if (r < 0) {
        // Surface readdir errors instead of silently truncating the listing.
        gs_outf("ls: readdir error in '%s': %s\n", path, strerror(-r));
    }
    be->closedir(dir);
    return val_bool(ok);
}

// `files.list([path])` — like `files.ls`, but returns a structured listing the
// GUI can render instead of printing names to stdout. Result is a list of
//   {name: "...", kind: "file"|"directory", size: <bytes>} maps.
// Descends into disk images through the same resolver as `files.ls`, so a bare
// image path lists its partitions and a partition path lists the HFS/UFS
// volume. Read-only throughout. Returns V_ERROR (falsy via the bridge) when
// the path can't be opened as a directory.
value_t files_method_list(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const char *path = (argc >= 1 && argv[0].s && *argv[0].s) ? argv[0].s : vfs_get_cwd();
    vfs_dir_t *dir = NULL;
    const vfs_backend_t *be = NULL;
    int rc = vfs_opendir(path, &dir, &be);
    if (rc < 0)
        return val_err("files.list: cannot open directory '%s': %s", path, strerror(-rc));

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
        return val_err("files.list: readdir error in '%s': %s", path, strerror(-r));
    }

    return val_list(items, len);
}

value_t files_method_mkdir(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    const char *dir = argv[0].s;
    if (!dir || !*dir)
        return val_err("files.mkdir: expected a non-empty path");
    int rc = vfs_mkdir(dir);
    if (rc == 0) {
        gs_outf("Directory '%s' created\n", dir);
        return val_bool(true);
    }
    gs_outf("mkdir: cannot create directory '%s': %s\n", dir, strerror(-rc));
    return val_bool(false);
}

value_t files_method_cat(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    const char *path = argv[0].s;
    if (!path || !*path)
        return val_err("files.cat: expected a non-empty path");
    vfs_file_t *f = NULL;
    const vfs_backend_t *be = NULL;
    int rc = vfs_open(path, &f, &be);
    if (rc < 0) {
        gs_outf("cat: cannot open '%s': %s\n", path, strerror(-rc));
        return val_bool(false);
    }
    uint8_t buf[4096];
    uint64_t off = 0;
    for (;;) {
        size_t got = 0;
        int rr = be->read(f, off, buf, sizeof(buf), &got);
        if (rr < 0) {
            gs_outf("cat: read error on '%s': %s\n", path, strerror(-rr));
            be->close(f);
            return val_bool(false);
        }
        if (got == 0)
            break;
        // The sink (gs_out.h): the job's output, or stdout.
        gs_out((const char *)buf, got);
        off += got;
    }
    be->close(f);
    return val_bool(true);
}

// `files.cd(path)` -- make a directory the current one: the directory
// relative paths start from, and what files.ls / files.list show when
// given no path.
value_t files_method_cd(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    char abs[VFS_PATH_MAX];
    if (vfs_normalise_path(argv[0].s, abs, sizeof(abs)) < 0)
        return val_err("cd: path too long");
    vfs_stat_t st;
    if (vfs_stat(abs, &st) < 0)
        return val_err("cd: no such directory '%s'", argv[0].s);
    if (!(st.mode & VFS_MODE_DIR))
        return val_err("cd: not a directory '%s'", argv[0].s);
    vfs_set_cwd(abs);
    return val_none();
}

// `files.pwd()` -- the current directory.
value_t files_method_pwd(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    return val_str(vfs_get_cwd());
}
