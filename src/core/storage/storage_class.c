// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// storage_class.c
// Object-model class descriptors for `storage` (storage.images,
// storage.list_dir, storage.import, plus the disk-image probe / mount
// surface — partmap, probe, list_partitions, mounts, unmount, find_media,
// hd_create, hd_download, cp, path_exists, path_size). Split out from
// storage.c so the storage block-I/O unit test can link only the core
// delta-storage API without pulling in image / vfs / shell dependencies.

#include "storage.h"

#include "image.h"
#include "image_apm.h"
#include "image_vfs.h"
#include "object.h"
#include "shell.h"
#include "system.h"
#include "system_config.h"
#include "value.h"
#include "vfs.h"

#include <errno.h>
#include <stdio.h>
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
// V_LIST<V_STRING>. Used by url-media.js to enumerate ROMs in OPFS.
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

// === Disk-image probe / mount surface =======================================
//
// The methods below were previously top-level `partmap`, `probe`, etc.
// root methods in gs_classes.c; they all read or mutate the storage
// view of `cfg->images[]` and the cached image-VFS mount table, so
// they live with the rest of the storage class.

// `storage.cp([-r], src, dst)` — copy host/VFS file to a VFS path.
static value_t storage_method_cp(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    bool recursive = false;
    const char *src = NULL;
    const char *dst = NULL;
    for (int i = 0; i < argc; i++) {
        if (argv[i].kind != V_STRING || !argv[i].s)
            return val_err("storage.cp: expected ([-r], src, dst)");
        const char *s = argv[i].s;
        if (strcmp(s, "-r") == 0 || strcmp(s, "-R") == 0) {
            recursive = true;
        } else if (!src) {
            src = s;
        } else if (!dst) {
            dst = s;
        } else {
            return val_err("storage.cp: too many arguments");
        }
    }
    if (!src || !dst)
        return val_err("storage.cp: expected ([-r], src, dst)");
    char err[256] = {0};
    int rc = shell_cp(src, dst, recursive, err, sizeof(err));
    if (rc < 0)
        return val_err("%s", err[0] ? err : "storage.cp: failed");
    return val_bool(true);
}

// `storage.find_media(dir, [dst])` — search a directory for a recognised
// floppy image; if `dst` is given, the image is copied there.
static value_t storage_method_find_media(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("storage.find_media: expected (dir, [dst])");
    const char *dir = argv[0].s ? argv[0].s : "";
    const char *dst = (argc >= 2 && argv[1].kind == V_STRING && argv[1].s && *argv[1].s) ? argv[1].s : NULL;
    return val_bool(gs_find_media(dir, dst) == 0);
}

// `storage.hd_create(path, size)` — create a blank SCSI HD image.
static value_t storage_method_hd_create(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2 || argv[0].kind != V_STRING)
        return val_err("storage.hd_create: expected (path, size)");
    char line[512];
    int n;
    if (argv[1].kind == V_STRING) {
        n = snprintf(line, sizeof(line), "hd create \"%s\" \"%s\"", argv[0].s, argv[1].s);
    } else if (argv[1].kind == V_INT) {
        n = snprintf(line, sizeof(line), "hd create \"%s\" %lld", argv[0].s, (long long)argv[1].i);
    } else if (argv[1].kind == V_UINT) {
        n = snprintf(line, sizeof(line), "hd create \"%s\" %llu", argv[0].s, (unsigned long long)argv[1].u);
    } else {
        return val_err("storage.hd_create: size must be string or integer");
    }
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("storage.hd_create: arguments too long");
    char *targv[32];
    int targc = tokenize(line, targv, 32);
    if (targc <= 0)
        return val_err("storage.hd_create: tokenisation failed");
    return val_bool(shell_hd_argv(targc, targv) == 0);
}

// `storage.hd_download(src, dst)` — export a hard disk image (base + delta).
static value_t storage_method_hd_download(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2 || argv[0].kind != V_STRING || argv[1].kind != V_STRING)
        return val_err("storage.hd_download: expected (source_path, dest_path)");
    int rc = system_download_hd(argv[0].s ? argv[0].s : "", argv[1].s ? argv[1].s : "");
    return val_bool(rc == 0);
}

static const char *apm_fs_kind_label(enum apm_fs_kind k) {
    switch (k) {
    case APM_FS_HFS:
        return "HFS";
    case APM_FS_UFS:
        return "UFS";
    case APM_FS_PARTITION_MAP:
        return "map";
    case APM_FS_DRIVER:
        return "drvr";
    case APM_FS_FREE:
        return "free";
    case APM_FS_PATCHES:
        return "patch";
    default:
        return "--";
    }
}

// `storage.partmap(path)` — print the Apple Partition Map of an image.
static value_t storage_method_partmap(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("storage.partmap: expected (path)");
    const char *path = argv[0].s ? argv[0].s : "";
    image_t *img = image_open_readonly(path);
    if (!img)
        return val_err("storage.partmap: cannot open image '%s'", path);
    const char *errmsg = NULL;
    apm_table_t *table = image_apm_parse(img, &errmsg);
    if (!table) {
        image_close(img);
        return val_err("storage.partmap: not an APM image: %s", errmsg ? errmsg : "unknown error");
    }
    printf("format: APM (512B blocks, %zu total)\n", disk_size(img) / 512);
    printf("  #  Name                             Type                        Start        Size  FS\n");
    for (uint32_t i = 0; i < table->n_partitions; i++) {
        const apm_partition_t *p = &table->partitions[i];
        printf("  %-2u %-32s %-24s %10llu  %10llu  %s\n", (unsigned)p->index, p->name[0] ? p->name : "(unnamed)",
               p->type[0] ? p->type : "(unknown)", (unsigned long long)p->start_block,
               (unsigned long long)p->size_blocks, apm_fs_kind_label(p->fs_kind));
    }
    image_apm_free(table);
    image_close(img);
    return val_bool(true);
}

// `storage.probe(path)` — identify the format of a disk image.
static value_t storage_method_probe(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("storage.probe: expected (path)");
    const char *path = argv[0].s ? argv[0].s : "";
    image_t *img = image_open_readonly(path);
    if (!img) {
        printf("cannot open image '%s'\n", path);
        return val_bool(false);
    }
    size_t size = disk_size(img);
    uint8_t block[512];
    bool apm = false;
    if (size >= 1024 && disk_read_data(img, 512, block, sizeof(block)) == sizeof(block))
        apm = image_apm_probe_magic(block);
    bool iso = false;
    if (size >= 33280 && disk_read_data(img, 32768, block, sizeof(block)) == sizeof(block))
        iso = (memcmp(block + 1, "CD001", 5) == 0);
    bool hfs = false;
    if (!apm && size >= 1024 + 512 && disk_read_data(img, 1024, block, sizeof(block)) == sizeof(block))
        hfs = (block[0] == 0x42 && block[1] == 0x44);
    if (apm && iso)
        printf("format: APM + ISO 9660 hybrid (%zu bytes)\n", size);
    else if (apm)
        printf("format: APM (%zu bytes)\n", size);
    else if (iso)
        printf("format: ISO 9660 (%zu bytes)\n", size);
    else if (hfs)
        printf("format: HFS (bare, %zu bytes)\n", size);
    else
        printf("format: unrecognised / raw (%zu bytes)\n", size);
    image_close(img);
    return val_bool(true);
}

static void storage_list_row_print(const char *path, const char *fmt, uint32_t n_parts, uint32_t refs, bool conflicted,
                                   void *user) {
    bool *header_printed = (bool *)user;
    if (!*header_printed) {
        printf("PATH                                        FMT  PARTS  REFS  STATUS\n");
        *header_printed = true;
    }
    printf("%-44s %-3s %5u %5u  %s\n", path, fmt, n_parts, refs, conflicted ? "busy" : "ok");
}

// `storage.list_partitions()` — print the cached image-VFS mount table.
static value_t storage_method_list_partitions(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    bool header_printed = false;
    image_vfs_list(storage_list_row_print, &header_printed);
    if (!header_printed)
        printf("(no cached image mounts)\n");
    return val_bool(true);
}

// `storage.mounts()` — alias for list_partitions; prints the mount table.
static value_t storage_method_mounts(struct object *self, const member_t *m, int argc, const value_t *argv) {
    return storage_method_list_partitions(self, m, argc, argv);
}

// `storage.unmount(path)` — drop a cached image-VFS mount.
static value_t storage_method_unmount(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("storage.unmount: expected (path)");
    const char *path = argv[0].s ? argv[0].s : "";
    char resolved[VFS_PATH_MAX];
    const vfs_backend_t *be = NULL;
    void *bctx = NULL;
    const char *tail = NULL;
    if (vfs_resolve(path, resolved, sizeof(resolved), &be, &bctx, &tail) == 0)
        path = resolved;
    int rc = image_vfs_unmount(path);
    if (rc == 0) {
        printf("unmounted %s\n", path);
        return val_bool(true);
    }
    if (rc == -ENOENT)
        printf("image unmount: not currently mounted: %s\n", path);
    else if (rc == -EBUSY)
        printf("image unmount: %s has live handles; marked conflicted\n", path);
    else
        printf("image unmount: %s: %s\n", path, strerror(-rc));
    return val_bool(false);
}

// `storage.path_exists(path)` — true if the path resolves in the shell VFS.
static value_t storage_method_path_exists(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("storage.path_exists: expected (path)");
    vfs_stat_t st;
    return val_bool(vfs_stat(argv[0].s ? argv[0].s : "", &st) == 0);
}

// `storage.path_size(path)` — file size in bytes (0 on stat failure).
static value_t storage_method_path_size(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("storage.path_size: expected (path)");
    const char *path = argv[0].s ? argv[0].s : "";
    vfs_stat_t st = {0};
    int rc = vfs_stat(path, &st);
    if (rc < 0) {
        printf("size: cannot stat '%s': %s\n", path, strerror(-rc));
        return val_uint(8, 0);
    }
    return val_uint(8, st.size);
}

static const arg_decl_t storage_cp_args[] = {
    {.name = "src", .kind = V_STRING, .doc = "Source path (host or VFS)"},
    {.name = "dst", .kind = V_STRING, .doc = "Destination path"},
    {.name = "flag", .kind = V_STRING, .flags = OBJ_ARG_OPTIONAL, .doc = "Optional -r / -R for recursive"},
};
static const arg_decl_t storage_find_media_args[] = {
    {.name = "dir", .kind = V_STRING, .doc = "Directory to scan"},
    {.name = "dst", .kind = V_STRING, .flags = OBJ_ARG_OPTIONAL, .doc = "Optional path to copy match into"},
};
static const arg_decl_t storage_hd_create_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Image output path"                                   },
    {.name = "size", .kind = V_NONE,   .doc = "Size string (e.g. \"HD20SC\", \"40M\") or byte count"},
};
static const arg_decl_t storage_hd_download_args[] = {
    {.name = "src", .kind = V_STRING, .doc = "Mounted HD image path"},
    {.name = "dst", .kind = V_STRING, .doc = "Output flat-file path"},
};
static const arg_decl_t storage_path_arg[] = {
    {.name = "path", .kind = V_STRING, .doc = "Image path"},
};

static const member_t storage_members[] = {
    {.kind = M_METHOD,
     .name = "import",
     .doc = "Persist a host file under /images/ (deferred — see proposal §5.7)",
     .method = {.args = storage_import_args, .nargs = 2, .result = V_STRING, .fn = storage_method_import}        },
    {.kind = M_METHOD,
     .name = "list_dir",
     .doc = "List directory entries (V_LIST of V_STRING names)",
     .method = {.args = storage_list_dir_args, .nargs = 1, .result = V_LIST, .fn = storage_method_list_dir}      },
    {.kind = M_METHOD,
     .name = "cp",
     .doc = "Copy a host or VFS path to another VFS path",
     .method = {.args = storage_cp_args, .nargs = 3, .result = V_BOOL, .fn = storage_method_cp}                  },
    {.kind = M_METHOD,
     .name = "find_media",
     .doc = "Find a recognised floppy/disk image in a directory",
     .method = {.args = storage_find_media_args, .nargs = 2, .result = V_BOOL, .fn = storage_method_find_media}  },
    {.kind = M_METHOD,
     .name = "hd_create",
     .doc = "Create a blank SCSI HD image",
     .method = {.args = storage_hd_create_args, .nargs = 2, .result = V_BOOL, .fn = storage_method_hd_create}    },
    {.kind = M_METHOD,
     .name = "hd_download",
     .doc = "Export a hard-disk image (base + delta) to a flat file",
     .method = {.args = storage_hd_download_args, .nargs = 2, .result = V_BOOL, .fn = storage_method_hd_download}},
    {.kind = M_METHOD,
     .name = "partmap",
     .doc = "Print the Apple Partition Map of an image",
     .method = {.args = storage_path_arg, .nargs = 1, .result = V_BOOL, .fn = storage_method_partmap}            },
    {.kind = M_METHOD,
     .name = "probe",
     .doc = "Identify the format of a disk image",
     .method = {.args = storage_path_arg, .nargs = 1, .result = V_BOOL, .fn = storage_method_probe}              },
    {.kind = M_METHOD,
     .name = "list_partitions",
     .doc = "Print the cached image-VFS mount table",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = storage_method_list_partitions}                },
    {.kind = M_METHOD,
     .name = "mounts",
     .doc = "Alias for list_partitions; prints the mount table",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = storage_method_mounts}                         },
    {.kind = M_METHOD,
     .name = "unmount",
     .doc = "Drop a cached image-VFS mount",
     .method = {.args = storage_path_arg, .nargs = 1, .result = V_BOOL, .fn = storage_method_unmount}            },
    {.kind = M_METHOD,
     .name = "path_exists",
     .doc = "True if the path resolves in the shell VFS",
     .method = {.args = storage_path_arg, .nargs = 1, .result = V_BOOL, .fn = storage_method_path_exists}        },
    {.kind = M_METHOD,
     .name = "path_size",
     .doc = "File size in bytes (0 on stat failure)",
     .method = {.args = storage_path_arg, .nargs = 1, .result = V_UINT, .fn = storage_method_path_size}          },
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
