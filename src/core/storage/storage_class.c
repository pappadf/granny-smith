// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// storage_class.c
// Object-model class descriptors for `storage` (storage.images,
// storage.list_dir, storage.import). Split out from storage.c so the
// storage block-I/O unit test can link only the core delta-storage API
// without pulling in image / vfs / shell dependencies.

#include "storage.h"

#include "image.h"
#include "image_apm.h"
#include "object.h"
#include "shell.h"
#include "system.h"
#include "system_config.h"
#include "value.h"
#include "vfs.h"

#include <stdlib.h>
#include <string.h>

// === Object-model class descriptors =========================================
//
// Replaces the M2 `storage` stub with a real class. `storage.images`
// enumerates the cfg->images[] entries. Slot index in the indexed
// child matches the slot in cfg->images[]; n_images is dense from
// 0..n_images-1, so the collection's count() returns cfg->n_images
// and next(prev) advances to prev+1 until n_images.

typedef struct {
    config_t *cfg;
    int slot;
} storage_image_data_t;

static storage_image_data_t g_storage_image_data[MAX_IMAGES];
static struct object *g_storage_image_objs[MAX_IMAGES];

static image_t *storage_image_at(struct object *self) {
    storage_image_data_t *d = (storage_image_data_t *)object_data(self);
    if (!d || !d->cfg)
        return NULL;
    if (d->slot < 0 || d->slot >= d->cfg->n_images)
        return NULL;
    return d->cfg->images[d->slot];
}

static value_t storage_image_attr_index(struct object *self, const member_t *m) {
    (void)m;
    storage_image_data_t *d = (storage_image_data_t *)object_data(self);
    return val_int(d ? d->slot : -1);
}
static value_t storage_image_attr_filename(struct object *self, const member_t *m) {
    (void)m;
    image_t *img = storage_image_at(self);
    const char *s = img ? image_get_filename(img) : NULL;
    return val_str(s ? s : "");
}
static value_t storage_image_attr_path(struct object *self, const member_t *m) {
    (void)m;
    image_t *img = storage_image_at(self);
    const char *s = img ? image_path(img) : NULL;
    return val_str(s ? s : "");
}
static value_t storage_image_attr_raw_size(struct object *self, const member_t *m) {
    (void)m;
    image_t *img = storage_image_at(self);
    return val_uint(8, img ? (uint64_t)img->raw_size : 0);
}
static value_t storage_image_attr_writable(struct object *self, const member_t *m) {
    (void)m;
    image_t *img = storage_image_at(self);
    return val_bool(img ? img->writable : false);
}

static const char *const STORAGE_IMAGE_TYPE_NAMES[] = {
    "other", "fd_ss", "fd_ds", "fd_hd", "hd", "cdrom",
};

static value_t storage_image_attr_type(struct object *self, const member_t *m) {
    (void)m;
    image_t *img = storage_image_at(self);
    int t = img ? (int)img->type : 0;
    int max = (int)(sizeof(STORAGE_IMAGE_TYPE_NAMES) / sizeof(STORAGE_IMAGE_TYPE_NAMES[0]));
    if (t < 0 || t >= max)
        t = 0;
    return val_enum(t, STORAGE_IMAGE_TYPE_NAMES, (size_t)max);
}

static const member_t storage_image_members[] = {
    {.kind = M_ATTR,
     .name = "index",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = storage_image_attr_index, .set = NULL}      },
    {.kind = M_ATTR,
     .name = "filename",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = storage_image_attr_filename, .set = NULL}},
    {.kind = M_ATTR,
     .name = "path",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = storage_image_attr_path, .set = NULL}    },
    {.kind = M_ATTR,
     .name = "raw_size",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = storage_image_attr_raw_size, .set = NULL}  },
    {.kind = M_ATTR,
     .name = "writable",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = storage_image_attr_writable, .set = NULL}  },
    {.kind = M_ATTR,
     .name = "type",
     .flags = VAL_RO,
     .attr = {.type = V_ENUM, .get = storage_image_attr_type, .set = NULL}      },
};

const class_desc_t storage_image_class = {
    .name = "image",
    .members = storage_image_members,
    .n_members = sizeof(storage_image_members) / sizeof(storage_image_members[0]),
};

static struct object *storage_images_get(struct object *self, int index) {
    config_t *cfg = (config_t *)object_data(self);
    if (!cfg || index < 0 || index >= MAX_IMAGES)
        return NULL;
    if (index >= cfg->n_images || !cfg->images[index])
        return NULL;
    return g_storage_image_objs[index];
}
static int storage_images_count(struct object *self) {
    config_t *cfg = (config_t *)object_data(self);
    if (!cfg)
        return 0;
    int n = 0;
    for (int i = 0; i < cfg->n_images; i++)
        if (cfg->images[i])
            n++;
    return n;
}
static int storage_images_next(struct object *self, int prev_index) {
    config_t *cfg = (config_t *)object_data(self);
    if (!cfg)
        return -1;
    for (int i = prev_index + 1; i < cfg->n_images; i++)
        if (cfg->images[i])
            return i;
    return -1;
}

// `storage.import(host_path, dst_path?)` — copy `host_path` into the
// emulator's persistent storage. When `dst_path` is empty (or absent
// — second arg is optional), falls back to the content-hash path
// produced by image_persist_volatile (/opfs/images/<hash>.img). When
// `dst_path` is non-empty, the source is copied verbatim through the
// VFS so paths like "/opfs/images/foo.img" can be picked explicitly.
//
// Returns the resolved destination path as a V_STRING.
static value_t storage_method_import(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("storage.import: expected (host_path, [dst_path])");
    const char *host_path = argv[0].s;
    const char *dst_path = (argc >= 2 && argv[1].kind == V_STRING && argv[1].s && *argv[1].s) ? argv[1].s : NULL;

    if (!dst_path) {
        // Hash-named persistence — handles the drag-drop / volatile-
        // path case and is idempotent on repeat imports.
        char *resolved = image_persist_volatile(host_path);
        if (!resolved)
            return val_err("storage.import: failed to persist '%s'", host_path);
        value_t v = val_str(resolved);
        free(resolved);
        return v;
    }

    // Explicit destination — call shell_cp directly so VFS handling
    // stays in one place (no shell_dispatch).
    char err[256] = {0};
    if (shell_cp(host_path, dst_path, false, err, sizeof(err)) < 0)
        return val_err("storage.import: cp '%s' -> '%s' failed: %s", host_path, dst_path, err[0] ? err : "unknown");
    return val_str(dst_path);
}

static const arg_decl_t storage_import_args[] = {
    {.name = "host_path", .kind = V_STRING, .doc = "Host path to read"},
    {.name = "dst_path",
     .kind = V_STRING,
     .flags = OBJ_ARG_OPTIONAL,
     .doc = "Destination path; empty → /opfs/images/<hash>.img"},
};

static const member_t storage_images_collection_members[] = {
    {.kind = M_CHILD,
     .name = "entries",
     .child = {.cls = &storage_image_class,
               .indexed = true,
               .get = storage_images_get,
               .count = storage_images_count,
               .next = storage_images_next,
               .lookup = NULL}},
};

const class_desc_t storage_images_collection_class = {
    .name = "storage_images",
    .members = storage_images_collection_members,
    .n_members = sizeof(storage_images_collection_members) / sizeof(storage_images_collection_members[0]),
};

// `storage.list_dir(path)` — list directory entries via the VFS as a
// V_LIST<V_STRING>. Powers the M10b/c migration of url-media.js's
// legacy `ls $ROMS_DIR` (whose stdout-only output had no typed
// successor until now).
static value_t storage_method_list_dir(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("storage.list_dir: expected (path)");
    vfs_dir_t *d = NULL;
    const vfs_backend_t *be = NULL;
    int rc = vfs_opendir(argv[0].s, &d, &be);
    if (rc < 0 || !d || !be)
        return val_list(NULL, 0); // empty list (treat unreadable dirs as no entries)
    size_t cap = 16, n = 0;
    value_t *items = (value_t *)calloc(cap, sizeof(value_t));
    if (!items) {
        be->closedir(d);
        return val_err("storage.list_dir: out of memory");
    }
    vfs_dirent_t ent;
    while (be->readdir(d, &ent) > 0) {
        if (ent.name[0] == '.' && (ent.name[1] == '\0' || (ent.name[1] == '.' && ent.name[2] == '\0')))
            continue;
        if (n >= cap) {
            size_t new_cap = cap * 2;
            value_t *nb = (value_t *)realloc(items, new_cap * sizeof(value_t));
            if (!nb) {
                for (size_t i = 0; i < n; i++)
                    value_free(&items[i]);
                free(items);
                be->closedir(d);
                return val_err("storage.list_dir: out of memory");
            }
            items = nb;
            cap = new_cap;
        }
        items[n++] = val_str(ent.name);
    }
    be->closedir(d);
    return val_list(items, n);
}

static const arg_decl_t storage_list_dir_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Directory path"},
};

static const member_t storage_members[] = {
    {.kind = M_METHOD,
     .name = "import",
     .doc = "Persist a host file under /images/ (deferred — see proposal §5.7)",
     .method = {.args = storage_import_args, .nargs = 2, .result = V_STRING, .fn = storage_method_import}  },
    {.kind = M_METHOD,
     .name = "list_dir",
     .doc = "List directory entries (V_LIST of V_STRING names)",
     .method = {.args = storage_list_dir_args, .nargs = 1, .result = V_LIST, .fn = storage_method_list_dir}},
};

const class_desc_t storage_class_real = {
    .name = "storage",
    .members = storage_members,
    .n_members = sizeof(storage_members) / sizeof(storage_members[0]),
};

// Per-slot image-entry object setup/teardown for storage.images
// indexed children. Called from gs_classes_install / gs_classes_uninstall.
void storage_object_classes_init(struct config *cfg) {
    for (int i = 0; i < MAX_IMAGES; i++) {
        g_storage_image_data[i].cfg = cfg;
        g_storage_image_data[i].slot = i;
        g_storage_image_objs[i] = object_new(&storage_image_class, &g_storage_image_data[i], NULL);
    }
}

void storage_object_classes_teardown(void) {
    for (int i = 0; i < MAX_IMAGES; i++) {
        if (g_storage_image_objs[i]) {
            object_delete(g_storage_image_objs[i]);
            g_storage_image_objs[i] = NULL;
        }
        g_storage_image_data[i].cfg = NULL;
        g_storage_image_data[i].slot = 0;
    }
}
