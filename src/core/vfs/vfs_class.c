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
    (void)argc;
    const char *dir = argv[0].s;
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
    {.name = "path", .kind = V_STRING, .validation_flags = OBJ_ARG_OPTIONAL, .doc = "Directory path (default: cwd)"},
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

// === Process-singleton lifecycle ============================================
//
// `vfs` is a stateless facade — its methods route through the global
// VFS registry. Register once at shell_init.

static struct object *s_vfs_object = NULL;

void vfs_class_register(void) {
    if (s_vfs_object)
        return;
    s_vfs_object = object_new(&vfs_class, NULL, "vfs");
    if (s_vfs_object)
        object_attach(object_root(), s_vfs_object);
}

void vfs_class_unregister(void) {
    if (s_vfs_object) {
        object_detach(s_vfs_object);
        object_delete(s_vfs_object);
        s_vfs_object = NULL;
    }
}
