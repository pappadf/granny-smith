// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// storage_class.c
// Object-model class descriptors for `files` -- host files, disk images,
// image mounts and archives. See `files_members[]` near the bottom of the
// file for the method list (the directory-level ones are implemented in
// vfs/vfs_class.c, the archive child in archive.c). Split out from
// storage.c so the storage block-I/O unit test can link only the core
// delta-storage API without pulling in image / vfs / shell dependencies.

#include "checkpoint.h"
#include "gs_out.h"
#include "io_leaf.h"
#include "storage.h"
#include "io/io_worker.h"
#include "mailbox/mailbox.h"

#include "archive.h"
#include "image.h"
#include "image_apm.h"
#include "image_part.h"
#include "image_vfs.h"
#include "object.h"
#include "root.h"
#include "shell.h"
#include "storage_util.h"
#include "system.h"
#include "system_config.h"
#include "value.h"
#include "vfs.h"
#include "vfs_class.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// === Object-model class descriptors =========================================
//
// `files.images`
// enumerates the cfg->images[] entries. Slot index in the indexed
// child matches the slot in cfg->images[]; n_images is dense from
// 0..n_images-1, so the collection's count() returns cfg->n_images
// and next(prev) advances to prev+1 until n_images.

typedef struct {
    config_t *cfg;
    int slot;
} files_image_data_t;

static files_image_data_t g_image_data[MAX_IMAGES];
static struct object *g_image_objs[MAX_IMAGES];

static image_t *files_image_at(struct object *self) {
    files_image_data_t *d = (files_image_data_t *)object_data(self);
    if (!d || !d->cfg)
        return NULL;
    if (d->slot < 0 || d->slot >= d->cfg->n_images)
        return NULL;
    return d->cfg->images[d->slot];
}

static value_t files_image_attr_index(struct object *self, const member_t *m) {
    (void)m;
    files_image_data_t *d = (files_image_data_t *)object_data(self);
    return val_int(d ? d->slot : -1);
}
static value_t files_image_attr_filename(struct object *self, const member_t *m) {
    (void)m;
    image_t *img = files_image_at(self);
    const char *s = img ? image_get_filename(img) : NULL;
    return val_str(s ? s : "");
}
static value_t files_image_attr_path(struct object *self, const member_t *m) {
    (void)m;
    image_t *img = files_image_at(self);
    const char *s = img ? image_path(img) : NULL;
    return val_str(s ? s : "");
}
static value_t files_image_attr_raw_size(struct object *self, const member_t *m) {
    (void)m;
    image_t *img = files_image_at(self);
    return val_uint(8, img ? (uint64_t)img->raw_size : 0);
}
static value_t files_image_attr_writable(struct object *self, const member_t *m) {
    (void)m;
    image_t *img = files_image_at(self);
    return val_bool(img ? img->writable : false);
}

static value_t files_image_attr_reads(struct object *self, const member_t *m) {
    (void)m;
    image_t *img = files_image_at(self);
    return val_uint(8, img ? img->reads : 0);
}
static value_t files_image_attr_writes(struct object *self, const member_t *m) {
    (void)m;
    image_t *img = files_image_at(self);
    return val_uint(8, img ? img->writes : 0);
}

// Designated-initialiser table keyed by `image_type` so a future enum
// reorder (or a value inserted out of order) keeps the labels aligned.
static const char *const STORAGE_IMAGE_TYPE_NAMES[] = {
    [image_other] = "other", [image_fd_ss] = "fd_ss", [image_fd_ds] = "fd_ds", [image_fd_dd_mfm] = "fd_720k_mfm",
    [image_fd_hd] = "fd_hd", [image_hd] = "hd",       [image_cdrom] = "cdrom",
};

static value_t files_image_attr_type(struct object *self, const member_t *m) {
    (void)m;
    image_t *img = files_image_at(self);
    int t = img ? (int)img->type : 0;
    int max = (int)(sizeof(STORAGE_IMAGE_TYPE_NAMES) / sizeof(STORAGE_IMAGE_TYPE_NAMES[0]));
    if (t < 0 || t >= max || !STORAGE_IMAGE_TYPE_NAMES[t])
        t = 0;
    return val_enum(t, STORAGE_IMAGE_TYPE_NAMES, (size_t)max);
}

static const member_t files_image_members[] = {
    {.kind = M_ATTR,
     .name = "index",
     .doc = "Position in files.images; stable only while no image is added or removed",
     .attr = {.type = V_INT, .get = files_image_attr_index, .set = NULL}                                      },
    {.kind = M_ATTR,
     .name = "filename",
     .doc = "Last path component, for display",
     .attr = {.type = V_STRING, .get = files_image_attr_filename, .set = NULL}                                },
    {.kind = M_ATTR,
     .name = "path",
     .doc = "Full host path or storage URI the image was opened from",
     .attr = {.type = V_STRING, .get = files_image_attr_path, .set = NULL}                                    },
    {.kind = M_ATTR,
     .name = "raw_size",
     .doc = "Logical size of the image in bytes, before any container or compression layer",
     .attr = {.type = V_UINT, .get = files_image_attr_raw_size, .set = NULL}                                  },
    {.kind = M_ATTR,
     .name = "writable",
     .doc = "True when guest writes reach the image (directly or through a checkpoint delta)",
     .attr = {.type = V_BOOL, .get = files_image_attr_writable, .set = NULL}                                  },
    {.kind = M_ATTR,
     .name = "type",
     .doc = "Media the image was identified as: fd_ss, fd_ds, fd_720k_mfm, fd_hd, hd, cdrom, or other",
     .attr = {.type = V_ENUM, .get = files_image_attr_type, .set = NULL}                                      },
    {.kind = M_ATTR,
     .name = "reads",
     .doc = "Drive reads served from the image since it was opened (what lights the activity light)",
     .attr = {.type = V_UINT, .get = files_image_attr_reads, .set = NULL, .presentation_flags = VAL_VOLATILE} },
    {.kind = M_ATTR,
     .name = "writes",
     .doc = "Drive writes to the image since it was opened",
     .attr = {.type = V_UINT, .get = files_image_attr_writes, .set = NULL, .presentation_flags = VAL_VOLATILE}},
};

static const class_desc_t files_image_class = {
    .name = "image",
    .members = files_image_members,
    .n_members = sizeof(files_image_members) / sizeof(files_image_members[0]),
    .doc = "One configured disk image",
};

static struct object *files_images_get(struct object *self, int index) {
    config_t *cfg = (config_t *)object_data(self);
    if (!cfg || index < 0 || index >= MAX_IMAGES)
        return NULL;
    if (index >= cfg->n_images || !cfg->images[index])
        return NULL;
    return g_image_objs[index];
}

// `files.import(host_path, dst_path)` — copy `host_path` to `dst_path`
// through the VFS, e.g. into "/opfs/images/hd/foo.img".  The destination is
// the caller's to choose: the core does not pick where media lives
// (it used to fall back to /opfs/images/<hash>.img).
//
// === Work whose cost is the size of a file: I/O jobs ==========================
//
// A copy, an export or a blank image is proportional to a file, not to
// the machine, so it runs on the I/O worker as an I/O leaf (io_leaf.h):
// the answer comes when the work ends, progress along the way, and a
// cancel stops it at its next chunk.

// A destination under a device is never written over (E_BUSY): the guest
// is using it.
static bool destination_attached(const char *dst) {
    return io_leaf_destination_attached(dst);
}

static int work_cp(io_leaf_t *j) {
    return shell_cp(j->a, j->b, j->flag, j->err, sizeof j->err);
}

static value_t answer_import(io_leaf_t *j) {
    return val_str(j->b);
}

static int work_export_raw(io_leaf_t *j) {
    return vfs_export_raw_image(j->a, j->b, j->err, sizeof j->err);
}

static int work_hd_create(io_leaf_t *j) {
    int rc = system_hd_create(j->a, j->size_str);
    if (rc != 0)
        snprintf(j->err, sizeof j->err, "could not create '%s' (%s)", j->a, j->size_str);
    return rc == 0 ? 0 : -EIO;
}

static int work_fd_create(io_leaf_t *j) {
    int rc = image_create_blank_floppy(j->a, false, j->flag);
    if (rc == -2)
        snprintf(j->err, sizeof j->err, "file already exists: %s", j->a);
    else if (rc != 0)
        snprintf(j->err, sizeof j->err, "failed to create blank floppy '%s'", j->a);
    return rc == 0 ? 0 : -EIO;
}

static int work_profile_create(io_leaf_t *j) {
    int rc = image_create_blank_profile(j->a, j->blocks);
    if (rc == -2)
        snprintf(j->err, sizeof j->err, "file already exists: %s", j->a);
    else if (rc != 0)
        snprintf(j->err, sizeof j->err, "failed to create blank ProFile image '%s'", j->a);
    return rc == 0 ? 0 : -EIO;
}

// Returns the destination path as a V_STRING.
static value_t files_method_import(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    const char *host_path = argv[0].s;
    const char *dst_path = argv[1].s;
    if (!dst_path || !*dst_path)
        return val_err("files.import: a destination path is required");

    if (destination_attached(dst_path))
        return val_err("files.import: '%s' is attached to a device (E_BUSY)", dst_path);
    // The copy is an I/O job; its answer, the destination path, comes when
    // it ends.
    io_leaf_t *j = io_leaf_new(host_path, dst_path);
    if (!j)
        return val_err("files.import: out of memory");
    j->work = work_cp;
    j->answer = answer_import;
    return io_leaf_dispatch(j, "files.import");
}

static const arg_decl_t files_import_args[] = {
    ARG_PATH("host_path", "Host path to read"),
    ARG_PATH("dst_path", "Destination path (e.g. under /opfs/images/<category>/)"),
};

static const member_t files_images_collection_members[] = {
    {.kind = M_CHILD,
     .name = "entries",
     .child =
         {.cls = &files_image_class, .indexed = true, .get = files_images_get, .slots = MAX_IMAGES, .lookup = NULL}},
};

static const class_desc_t files_images_collection_class = {
    .name = "files_images",
    .members = files_images_collection_members,
    .n_members = sizeof(files_images_collection_members) / sizeof(files_images_collection_members[0]),
    .doc = "The machine's configured disk images",
};

// `files.list_dir(path)` — list directory entries via the VFS as a
// V_LIST<V_STRING>. Used by url-media.js to enumerate ROMs in OPFS.
static value_t files_method_list_dir(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    vfs_dir_t *d = NULL;
    const vfs_backend_t *be = NULL;
    int rc = vfs_opendir(argv[0].s, &d, &be);
    if (rc < 0 || !d || !be)
        return val_list(NULL, 0); // empty list (treat unreadable dirs as no entries)
    size_t cap = 16, n = 0;
    value_t *items = (value_t *)calloc(cap, sizeof(value_t));
    if (!items) {
        be->closedir(d);
        return val_err("files.list_dir: out of memory");
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
                return val_err("files.list_dir: out of memory");
            }
            items = nb;
            cap = new_cap;
        }
        items[n++] = val_str(ent.name);
    }
    be->closedir(d);
    return val_list(items, n);
}

static const arg_decl_t files_list_dir_args[] = {
    ARG_PATH("path", "Directory path"),
};

// === Disk-image probe / mount surface =======================================
//
// The methods below read or mutate `cfg->images[]` and the cached
// image-VFS mount table.

// `files.cp(src, dst, [recursive])` — copy host/VFS file to a VFS path.
static value_t files_method_cp(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const char *src = argv[0].s;
    const char *dst = argv[1].s;
    bool recursive = argc > 2 && argv[2].kind == V_BOOL && argv[2].b;
    if (destination_attached(dst))
        return val_err("files.cp: '%s' is attached to a device (E_BUSY)", dst);
    io_leaf_t *j = io_leaf_new(src, dst);
    if (!j)
        return val_err("files.cp: out of memory");
    j->flag = recursive;
    j->work = work_cp;
    return io_leaf_dispatch(j, "files.cp");
}

// `files.export_raw(src, dst)` — write a disk image referenced by a VFS
// path (a host raw/DC42 image, or an image nested inside a mounted image such
// as an NDIF `.img` in a Toast CD) as a flat, decoded RAW image on the host.
// Unlike `cp` — which copies a file's data fork verbatim (for an NDIF `.img`
// that is the still-compressed data fork) — this decodes the image and emits
// its logical block device, ready to re-mount or `dd`.  Refuses to overwrite.
static value_t files_method_export_raw(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    const char *src = argv[0].s;
    const char *dst = argv[1].s;
    if (destination_attached(dst))
        return val_err("files.export_raw: '%s' is attached to a device (E_BUSY)", dst);
    io_leaf_t *j = io_leaf_new(src, dst);
    if (!j)
        return val_err("files.export_raw: out of memory");
    j->work = work_export_raw;
    return io_leaf_dispatch(j, "files.export_raw");
}

// `files.find_media(dir, [dst])` — search a directory for a recognised
// floppy image; if `dst` is given, the image is copied there.
static value_t files_method_find_media(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const char *dir = argv[0].s;
    if (!dir || !*dir)
        return val_err("files.find_media: expected a non-empty directory path");
    const char *dst = (argc >= 2 && argv[1].s && *argv[1].s) ? argv[1].s : NULL;
    int rc = gs_find_media(dir, dst);
    if (rc != 0)
        return val_err("files.find_media: no recognised media found under '%s'", dir);
    return val_bool(true);
}

// `files.hd_create(path, size)` — create a blank SCSI HD image.
// size is a V_NONE-kind slot, so the body discriminates between
// V_STRING (label/size string) and integer (byte count). The size
// string that system_hd_create parses accepts model labels, human
// sizes, and byte counts alike, so integers stringify cleanly.
static value_t files_method_hd_create(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    char size_str[64];
    if (argv[1].kind == V_STRING) {
        snprintf(size_str, sizeof(size_str), "%s", argv[1].s ? argv[1].s : "");
    } else if (argv[1].kind == V_INT) {
        snprintf(size_str, sizeof(size_str), "%lld", (long long)argv[1].i);
    } else if (argv[1].kind == V_UINT) {
        snprintf(size_str, sizeof(size_str), "%llu", (unsigned long long)argv[1].u);
    } else {
        return val_err("files.hd_create: size must be string or integer");
    }
    if (destination_attached(argv[0].s))
        return val_err("files.hd_create: '%s' is attached to a device (E_BUSY)", argv[0].s);
    io_leaf_t *j = io_leaf_new(argv[0].s, NULL);
    if (!j)
        return val_err("files.hd_create: out of memory");
    snprintf(j->size_str, sizeof j->size_str, "%s", size_str);
    j->work = work_hd_create;
    return io_leaf_dispatch(j, "files.hd_create");
}

// Paths files.rm / files.mv must never destroy: the filesystem root and
// the OPFS mount root (all persisted browser state lives under /opfs — a
// recursive rm there wipes every ROM, image and checkpoint). Tolerates a
// trailing slash.
static bool files_path_is_protected(const char *p) {
    if (!p || !*p)
        return true;
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/')
        n--;
    return (n == 1 && p[0] == '/') || (n == 5 && strncmp(p, "/opfs", 5) == 0);
}

// `files.rm(path)` — recursively remove a file or directory. Routing the
// web UI's deletes through here (the worker) instead of the browser's
// main-thread OPFS API keeps the worker's WasmFS inode cache coherent, so a
// later worker-side create at the same path (e.g. re-copying a file out of an
// image after deleting it) doesn't hit a dangling inode.
static value_t files_method_rm(struct object *self, const member_t *m, int argc, const value_t *argv) {
    checkpoint_quick_wait(); // a checkpoint publish in flight lands before anything moves or goes
    (void)self;
    (void)m;
    (void)argc;
    const char *path = argv[0].s;
    if (files_path_is_protected(path))
        return val_err("files.rm: refusing to remove '%s'", path ? path : "(null)");
    int rc = gs_rm_tree(path);
    if (rc < 0)
        return val_err("files.rm: cannot remove '%s': %s", path, strerror(-rc));
    return val_bool(true);
}

// `files.mv(src, dst)` — move/rename within the host filesystem. Like
// files.rm, routing the web UI's moves through the worker (rather than the
// browser's main-thread OPFS API) keeps WasmFS coherent. Tries rename() first
// (fast / atomic on the same volume); falls back to a recursive copy + remove.
static value_t files_method_mv(struct object *self, const member_t *m, int argc, const value_t *argv) {
    checkpoint_quick_wait(); // a checkpoint publish in flight lands before anything moves or goes
    (void)self;
    (void)m;
    (void)argc;
    const char *src = argv[0].s;
    const char *dst = argv[1].s;
    if (!src || !*src || !dst || !*dst)
        return val_err("files.mv: expected (src, dst)");
    if (files_path_is_protected(src) || files_path_is_protected(dst))
        return val_err("files.mv: refusing to move '%s'", src);
    // Moving a directory into its own subtree would recurse forever in the
    // copy fallback (the copy lists the source after creating dst inside it).
    size_t sl = strlen(src);
    if (strncmp(dst, src, sl) == 0 && (dst[sl] == '/' || dst[sl] == '\0'))
        return val_err("files.mv: cannot move '%s' into itself", src);
    // Refuse an existing destination outright. rename() would overwrite a
    // file silently, and the copy fallback would nest a directory under an
    // existing same-named one (dst/X/X) — both surprise the user; make them
    // delete the target first.
    vfs_stat_t st;
    if (vfs_stat(dst, &st) == 0)
        return val_err("files.mv: destination '%s' already exists", dst);
    if (rename(src, dst) == 0)
        return val_bool(true);
    char err[256] = {0};
    if (shell_cp(src, dst, true, err, sizeof(err)) < 0)
        return val_err("files.mv: %s", err[0] ? err : "move failed");
    // The copy succeeded; if the source can't be fully removed the operation
    // is a copy, not a move — report that instead of pretending success.
    int rc = gs_rm_tree(src);
    if (rc < 0)
        return val_err("files.mv: copied, but failed to remove source '%s': %s", src, strerror(-rc));
    return val_bool(true);
}

// `files.fd_create(path, [high_density])` — create a blank (unformatted)
// floppy image: 800 KB by default, 1.4 MB when high_density is true. Unlike
// the `fd create` shell command this does NOT insert the disk into a drive —
// the New Machine dialog persists the file and lets the user select it.
static value_t files_method_fd_create(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const char *path = argv[0].s;
    if (!path || !*path)
        return val_err("files.fd_create: empty path");
    bool high_density = (argc >= 2 && argv[1].kind == V_BOOL) ? argv[1].b : false;
    io_leaf_t *j = io_leaf_new(path, NULL);
    if (!j)
        return val_err("files.fd_create: out of memory");
    j->flag = high_density;
    j->work = work_fd_create;
    return io_leaf_dispatch(j, "files.fd_create");
}

// `files.profile_create(path, blocks)` — create a blank Lisa/XL ParaPort
// ProFile image: a raw, all-zero file of `blocks` 532-byte blocks. Unlike
// hd_create (which builds a 512-byte/block SCSI image), the ProFile is a
// parallel-port disk with 532-byte blocks, a distinct on-disk format. `blocks`
// is a V_NONE slot accepting an integer or numeric string. Standard sizes:
// 5 MB = 9728 blocks; 10 MB ≈ 19448 (the LOS-documented full Widget capacity).
static value_t files_method_profile_create(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    const char *path = argv[0].s;
    if (!path || !*path)
        return val_err("files.profile_create: empty path");
    unsigned long blocks;
    if (argv[1].kind == V_STRING) {
        if (!argv[1].s)
            return val_err("files.profile_create: missing block count");
        blocks = strtoul(argv[1].s, NULL, 10);
    } else if (argv[1].kind == V_INT) {
        blocks = (argv[1].i > 0) ? (unsigned long)argv[1].i : 0;
    } else if (argv[1].kind == V_UINT) {
        blocks = (unsigned long)argv[1].u;
    } else {
        return val_err("files.profile_create: blocks must be an integer");
    }
    if (blocks == 0)
        return val_err("files.profile_create: block count must be positive");
    io_leaf_t *j = io_leaf_new(path, NULL);
    if (!j)
        return val_err("files.profile_create: out of memory");
    j->blocks = (uint32_t)blocks;
    j->work = work_profile_create;
    return io_leaf_dispatch(j, "files.profile_create");
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

// `files.partmap(path)` — print the Apple Partition Map of an image.
static value_t files_method_partmap(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    const char *path = argv[0].s;
    image_t *img = image_open_readonly(path);
    if (!img)
        return val_err("files.partmap: cannot open image '%s'", path);
    const char *errmsg = NULL;
    apm_table_t *table = image_apm_parse(img, &errmsg);
    if (!table) {
        image_close(img);
        return val_err("files.partmap: not an APM image: %s", errmsg ? errmsg : "unknown error");
    }
    gs_outf("format: APM (512B blocks, %zu total)\n", disk_size(img) / 512);
    gs_outf("  #  Name                             Type                        Start        Size  FS\n");
    for (uint32_t i = 0; i < table->n_partitions; i++) {
        const apm_partition_t *p = &table->partitions[i];
        gs_outf("  %-2u %-32s %-24s %10llu  %10llu  %s\n", (unsigned)p->index, p->name[0] ? p->name : "(unnamed)",
                p->type[0] ? p->type : "(unknown)", (unsigned long long)p->start_block,
                (unsigned long long)p->size_blocks, apm_fs_kind_label(p->fs_kind));
    }
    image_apm_free(table);
    image_close(img);
    return val_bool(true);
}

// `files.probe(path)` — identify the format of a disk image.
static value_t files_method_probe(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    const char *path = argv[0].s;
    image_t *img = image_open_readonly(path);
    if (!img) {
        gs_outf("cannot open image '%s'\n", path);
        return val_bool(false);
    }
    size_t size = disk_size(img);
    uint8_t block[512];
    bool apm = false;
    if (size >= 1024 && image_read_bytes(img, 512, block, sizeof(block)) == 0)
        apm = image_apm_probe_magic(block);
    bool iso = false;
    if (size >= 33280 && image_read_bytes(img, 32768, block, sizeof(block)) == 0)
        iso = (memcmp(block + 1, "CD001", 5) == 0);
    bool hfs = false;
    if (!apm && size >= 1024 + 512 && image_read_bytes(img, 1024, block, sizeof(block)) == 0)
        hfs = (block[0] == 0x42 && block[1] == 0x44);
    if (apm && iso)
        gs_outf("format: APM + ISO 9660 hybrid (%zu bytes)\n", size);
    else if (apm)
        gs_outf("format: APM (%zu bytes)\n", size);
    else if (iso)
        gs_outf("format: ISO 9660 (%zu bytes)\n", size);
    else if (hfs)
        gs_outf("format: HFS (bare, %zu bytes)\n", size);
    else
        gs_outf("format: unrecognised / raw (%zu bytes)\n", size);
    image_close(img);
    return val_bool(true);
}

// `files.path_exists(path)` — true if the path resolves in the shell VFS.
static value_t files_method_path_exists(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    vfs_stat_t st;
    return val_bool(vfs_stat(argv[0].s, &st) == 0);
}

// `files.path_size(path)` — file size in bytes (0 on stat failure).
static value_t files_method_path_size(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    const char *path = argv[0].s;
    vfs_stat_t st = {0};
    int rc = vfs_stat(path, &st);
    if (rc < 0) {
        gs_outf("size: cannot stat '%s': %s\n", path, strerror(-rc));
        return val_uint(8, 0);
    }
    return val_uint(8, st.size);
}

// `files.path_compare(a, b)` — byte-for-byte comparison of two VFS files.
// Returns -1 when they are identical, otherwise the offset of the first
// difference (or of the end of the shorter file).  A fork-fidelity test that
// only asked "are they equal?" would report a bare false; the offset says
// where the round trip lost the bytes.
static value_t files_method_path_compare(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    vfs_file_t *fa = NULL;
    vfs_file_t *fb = NULL;
    const vfs_backend_t *ba = NULL;
    const vfs_backend_t *bb = NULL;
    int rc = vfs_open(argv[0].s, &fa, &ba);
    if (rc < 0)
        return val_err("cannot open '%s': %s", argv[0].s, strerror(-rc));
    rc = vfs_open(argv[1].s, &fb, &bb);
    if (rc < 0) {
        ba->close(fa);
        return val_err("cannot open '%s': %s", argv[1].s, strerror(-rc));
    }

    uint8_t buf_a[8192], buf_b[8192];
    uint64_t offset = 0;
    int64_t diff = -1;
    for (;;) {
        size_t na = 0, nb = 0;
        int ra = ba->read(fa, offset, buf_a, sizeof(buf_a), &na);
        int rb = bb->read(fb, offset, buf_b, sizeof(buf_b), &nb);
        if (ra < 0 || rb < 0) {
            diff = (int64_t)offset;
            break;
        }
        size_t common = na < nb ? na : nb;
        for (size_t i = 0; i < common; i++) {
            if (buf_a[i] != buf_b[i]) {
                diff = (int64_t)(offset + (uint64_t)i);
                break;
            }
        }
        if (diff >= 0)
            break;
        if (na != nb) { // one file ended before the other
            diff = (int64_t)(offset + (uint64_t)common);
            break;
        }
        if (na == 0)
            break; // both hit EOF with everything matched
        offset += (uint64_t)na;
    }
    ba->close(fa);
    bb->close(fb);
    return val_int(diff);
}

static const arg_decl_t files_compare_args[] = {
    {.name = "a", .kind = V_STRING, .doc = "First path (host or VFS)" },
    {.name = "b", .kind = V_STRING, .doc = "Second path (host or VFS)"},
};

static const value_t files_cp_not_recursive = {.kind = V_BOOL, .b = false};
static const arg_decl_t files_cp_args[] = {
    ARG_PATH("src", "Source path (host or VFS)"),
    ARG_PATH("dst", "Destination path"),
    {.name = "recursive",
                                 .kind = V_BOOL,
                                 .validation_flags = OBJ_ARG_OPTIONAL,
                                 .default_value = &files_cp_not_recursive,
                                 .doc = "Copy a directory and everything under it"},
};
// === The file-transfer window =================================================
//
// How the web page moves file bytes in and out without touching the file
// system from its own thread.  Under WasmFS a Module.FS call from the page runs
// ON the page's thread, which then busy-waits for the OPFS backend -- and in
// WebKit a worker's OPFS request is itself served through the page's thread, so
// Safari deadlocked on every upload.  Instead the page copies a chunk into
// this buffer (it is ordinary wasm memory: a plain store, no waiting) and asks
// for it to be written -- or asks for a chunk to be read into it -- through the
// request bridge, so every file access runs here, on the emulator thread, as
// all others do.  One chunk in flight at a time; the page serialises them.
#define STORAGE_XFER_BYTES (2u << 20)
static uint8_t g_xfer[STORAGE_XFER_BYTES];

static value_t files_attr_xfer_buffer(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(4, (uint32_t)(uintptr_t)g_xfer);
}

static value_t files_attr_xfer_size(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(4, STORAGE_XFER_BYTES);
}

// The window's transfers are I/O jobs too (io_leaf.h): the file write or
// read runs on the I/O worker and the page's promise settles when it is
// done.  The page fills or empties the window only between requests (one
// chunk in flight), so the worker owns the window while a job runs.
typedef struct {
    uint64_t offset;
    uint64_t len;
    size_t got;
} xfer_job_t;

static void xfer_cleanup(io_leaf_t *j) {
    free(j->ud);
}

static int work_xfer_write(io_leaf_t *j) {
    xfer_job_t *x = (xfer_job_t *)j->ud;
    FILE *f = fopen(j->a, x->offset == 0 ? "wb" : "r+b");
    if (!f) {
        int e = errno ? errno : EIO;
        snprintf(j->err, sizeof j->err, "cannot open '%s': %s", j->a, strerror(e));
        return -e;
    }
    bool ok = fseeko(f, (off_t)x->offset, SEEK_SET) == 0 && fwrite(g_xfer, 1, (size_t)x->len, f) == (size_t)x->len;
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        snprintf(j->err, sizeof j->err, "write to '%s' at %llu failed", j->a, (unsigned long long)x->offset);
        return -EIO;
    }
    return 0;
}

static int work_xfer_read(io_leaf_t *j) {
    xfer_job_t *x = (xfer_job_t *)j->ud;
    FILE *f = fopen(j->a, "rb");
    if (!f) {
        int e = errno ? errno : EIO;
        snprintf(j->err, sizeof j->err, "cannot open '%s': %s", j->a, strerror(e));
        return -e;
    }
    x->got = 0;
    if (fseeko(f, (off_t)x->offset, SEEK_SET) == 0)
        x->got = fread(g_xfer, 1, (size_t)x->len, f);
    fclose(f);
    return 0;
}

static value_t answer_xfer_read(io_leaf_t *j) {
    return val_uint(4, (uint32_t)((xfer_job_t *)j->ud)->got);
}

static value_t xfer_dispatch(const char *path, uint64_t offset, uint64_t len, bool write, const char *what) {
    io_leaf_t *j = io_leaf_new(path, NULL);
    xfer_job_t *x = (xfer_job_t *)calloc(1, sizeof *x);
    if (!j || !x) {
        free(x);
        if (j)
            free(j->a), free(j);
        return val_err("%s: out of memory", what);
    }
    x->offset = offset;
    x->len = len;
    j->ud = x;
    j->cleanup = xfer_cleanup;
    j->work = write ? work_xfer_write : work_xfer_read;
    j->answer = write ? NULL : answer_xfer_read;
    return io_leaf_dispatch(j, what);
}

// `files.xfer_write(path, offset, len)` — write the window's first `len`
// bytes to `path` at `offset`; offset 0 creates (or truncates) the file.
static value_t files_method_xfer_write(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    uint64_t offset = argv[1].u, len = argv[2].u;
    if (len > STORAGE_XFER_BYTES)
        return val_err("files.xfer_write: %llu bytes is more than the %u-byte window", (unsigned long long)len,
                       STORAGE_XFER_BYTES);
    return xfer_dispatch(argv[0].s, offset, len, true, "files.xfer_write");
}

// `files.xfer_read(path, offset, len)` — read up to `len` bytes of `path`
// from `offset` into the window; answers how many (0 at the end).
static value_t files_method_xfer_read(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    uint64_t offset = argv[1].u, len = argv[2].u;
    if (len > STORAGE_XFER_BYTES)
        len = STORAGE_XFER_BYTES;
    return xfer_dispatch(argv[0].s, offset, len, false, "files.xfer_read");
}

static const arg_decl_t files_xfer_args[] = {
    ARG_PATH("path", "File path"),
    {.name = "offset", .kind = V_UINT, .doc = "Byte offset in the file"             },
    {.name = "len",    .kind = V_UINT, .doc = "Byte count (at most files.xfer_size)"},
};

static const arg_decl_t files_export_raw_args[] = {
    ARG_PATH("src", "Source image path (host, or nested inside a mounted image)"),
    ARG_PATH("dst", "Destination host path for the decoded raw image"),
};
static const arg_decl_t files_find_media_args[] = {
    ARG_PATH("dir", "Directory to scan"),
    {.name = "dst",
                                  .kind = V_STRING,
                                  .presentation_flags = VAL_PATH,
                                  .validation_flags = OBJ_ARG_OPTIONAL,
                                  .doc = "Optional path to copy match into"},
};
static const arg_decl_t files_hd_create_args[] = {
    ARG_PATH("path", "Image output path"),
    {.name = "size",
                                   .kind = V_NONE,
                                   .validation_flags = OBJ_ARG_POLY,
                                   .doc = "Size string (e.g. \"HD20SC\", \"40M\") or byte count"},
};
static const arg_decl_t files_rm_args[] = {
    ARG_PATH("path", "Path to remove (recursive)"),
};
static const arg_decl_t files_mv_args[] = {
    ARG_PATH("src", "Source path"),
    ARG_PATH("dst", "Destination path"),
};
static const value_t files_false = {.kind = V_BOOL, .b = false};
static const arg_decl_t files_fd_create_args[] = {
    ARG_PATH("path", "Image output path"),
    {.name = "high_density",
                                   .kind = V_BOOL,
                                   .validation_flags = OBJ_ARG_OPTIONAL,
                                   .default_value = &files_false,
                                   .doc = "true = 1.4 MB, false = 800 KB"},
};
static const arg_decl_t files_profile_create_args[] = {
    ARG_PATH("path", "Image output path"),
    {.name = "blocks",
                                   .kind = V_NONE,
                                   .validation_flags = OBJ_ARG_POLY,
                                   .doc = "ProFile block count, a number or a numeric string (532-byte blocks; 5 MB = 9728)"},
};
static const arg_decl_t files_path_arg[] = {
    ARG_PATH("path", "Image path"),
};
static const arg_decl_t files_dir_arg[] = {
    ARG_PATH("path", "Directory, absolute or relative"),
};
static const arg_decl_t files_any_path_arg[] = {
    ARG_PATH("path", "File or directory path"),
};

static const arg_decl_t files_path_arg_optional[] = {
    {.name = "path",
     .kind = V_STRING,
     .presentation_flags = VAL_PATH,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Directory path",
     .default_doc = "the current directory"},
};

// `files.download(path)` — trigger a browser file download. Routes to the
// platform-specific gs_download (WASM streams via Blob+anchor); a platform
// with no browser says so.
static value_t files_method_download(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    int rc = gs_download(argv[0].s);
    if (rc == -2)
        return val_err("download: not supported on this platform");
    return val_bool(rc == 0);
}

static const member_t files_members[] = {
    {.kind = M_ATTR,
     .name = "xfer_buffer",
     .flags = M_CAT_INTERNAL,
     .doc = "Address of the file-transfer window in wasm memory (the web page's upload path)",
     .attr = {.type = V_UINT, .get = files_attr_xfer_buffer, .set = NULL}},
    {.kind = M_ATTR,
     .name = "xfer_size",
     .flags = M_CAT_INTERNAL,
     .doc = "Size of the file-transfer window in bytes",
     .attr = {.type = V_UINT, .get = files_attr_xfer_size, .set = NULL}},
    {.kind = M_METHOD,
     .name = "xfer_write",
     .flags = M_CAT_INTERNAL,
     .doc = "Write the transfer window's first len bytes to path at offset (offset 0 creates the file)",
     .method =
         {.ui_flags = MM_IO, .args = files_xfer_args, .nargs = 3, .result = V_BOOL, .fn = files_method_xfer_write}},
    {.kind = M_METHOD,
     .name = "xfer_read",
     .flags = M_CAT_INTERNAL,
     .doc = "Read up to len bytes of path at offset into the transfer window; answers the count",
     .method =
         {.ui_flags = MM_IO, .args = files_xfer_args, .nargs = 3, .result = V_UINT, .fn = files_method_xfer_read}},
    {.kind = M_METHOD,
     .name = "import",
     .examples = EXAMPLES("files.import \"/tmp/upload.img\" \"/opfs/images/hd/upload.img\""),
     .doc = "Copy a host file to a destination path",
     .method = {.result_doc = "the destination path",
                .ui_flags = MM_IO,
                .args = files_import_args,
                .nargs = 2,
                .result = V_STRING,
                .fn = files_method_import}},
    {.kind = M_METHOD,
     .name = "list_dir",
     .examples = EXAMPLES("files.list_dir \"/opfs/images\""),
     .doc = "List a directory's entry names",
     .method = {.result_doc = "the entry names, as strings",
                .args = files_list_dir_args,
                .nargs = 1,
                .result = V_LIST,
                .fn = files_method_list_dir}},
    {.kind = M_METHOD,
     .name = "cp",
     .examples = EXAMPLES("files.cp \"/opfs/images/fd/tools.dsk\" \"/opfs/backup/tools.dsk\"",
     "files.cp \"/opfs/images\" \"/opfs/backup\" recursive=true"),
     .doc = "Copy a host or VFS path to another VFS path",
     .method = {.ui_flags = MM_IO, .args = files_cp_args, .nargs = 3, .result = V_BOOL, .fn = files_method_cp}},
    {.kind = M_METHOD,
     .name = "export_raw",
     .examples = EXAMPLES("files.export_raw \"/opfs/images/fd/disk.image\" \"/opfs/raw/disk.raw\""),
     .doc = "Decode a disk image (incl. NDIF nested in a mounted image) to a flat raw image on the host",
     .method = {.ui_flags = MM_IO,
                .args = files_export_raw_args,
                .nargs = 2,
                .result = V_BOOL,
                .fn = files_method_export_raw}},
    {.kind = M_METHOD,
     .name = "find_media",
     .examples = EXAMPLES("files.find_media \"/opfs/unpacked\"",
     "files.find_media \"/opfs/unpacked\" \"/opfs/images/fd/found.dsk\""),
     .doc = "Find a recognised floppy/disk image in a directory",
     .method = {.args = files_find_media_args, .nargs = 2, .result = V_BOOL, .fn = files_method_find_media}},
    {.kind = M_METHOD,
     .name = "hd_create",
     .examples = EXAMPLES("files.hd_create \"/opfs/images/hd/new.img\" \"40M\"",
     "files.hd_create \"/opfs/images/hd/hd20.img\" \"HD20SC\""),
     .doc = "Create a blank SCSI HD image",
     .method =
         {.ui_flags = MM_IO, .args = files_hd_create_args, .nargs = 2, .result = V_BOOL, .fn = files_method_hd_create}},
    {.kind = M_METHOD,
     .name = "fd_create",
     .examples = EXAMPLES("files.fd_create \"/opfs/images/fd/blank.dsk\"",
     "files.fd_create \"/opfs/images/fd/blank-hd.dsk\" true"),
     .doc = "Create a blank floppy image (800 KB, or 1.4 MB when high_density)",
     .method =
         {.ui_flags = MM_IO, .args = files_fd_create_args, .nargs = 2, .result = V_BOOL, .fn = files_method_fd_create}},
    {.kind = M_METHOD,
     .name = "profile_create",
     .examples = EXAMPLES("files.profile_create \"/opfs/images/hd/profile.img\" 9728"),
     .doc = "Create a blank Lisa/XL ProFile image (raw 532-byte/block zero file)",
     .method = {.args = files_profile_create_args, .nargs = 2, .result = V_BOOL, .fn = files_method_profile_create}},
    {.kind = M_METHOD,
     .name = "rm",
     .examples = EXAMPLES("files.rm \"/opfs/images/hd/old.img\""),
     .doc = "Recursively remove a file or directory (keeps the worker FS coherent)",
     .method = {.ui_flags = MM_IO, .args = files_rm_args, .nargs = 1, .result = V_BOOL, .fn = files_method_rm}},
    {.kind = M_METHOD,
     .name = "mv",
     .examples = EXAMPLES("files.mv \"/opfs/images/hd/new.img\" \"/opfs/images/hd/work.img\""),
     .doc = "Move/rename a file or directory (keeps the worker FS coherent)",
     .method = {.args = files_mv_args, .nargs = 2, .result = V_BOOL, .fn = files_method_mv}},
    {.kind = M_METHOD,
     .name = "partmap",
     .examples = EXAMPLES("files.partmap \"/opfs/images/hd/system.img\""),
     .doc = "Print the Apple Partition Map of an image",
     .method = {.args = files_path_arg, .nargs = 1, .result = V_BOOL, .fn = files_method_partmap}},
    {.kind = M_METHOD,
     .name = "probe",
     .examples = EXAMPLES("files.probe \"/opfs/images/fd/disk.image\""),
     .doc = "Identify the format of a disk image",
     .method = {.args = files_path_arg, .nargs = 1, .result = V_BOOL, .fn = files_method_probe}},
    {.kind = M_METHOD,
     .name = "path_exists",
     .examples = EXAMPLES("files.path_exists \"/opfs/images/hd/system.img\""),
     .doc = "True if the path resolves in the shell VFS",
     .method = {.args = files_any_path_arg, .nargs = 1, .result = V_BOOL, .fn = files_method_path_exists}},
    {.kind = M_METHOD,
     .name = "path_size",
     .examples = EXAMPLES("files.path_size \"/opfs/images/hd/system.img\""),
     .doc = "File size in bytes (0 on stat failure)",
     .method = {.args = files_any_path_arg, .nargs = 1, .result = V_UINT, .fn = files_method_path_size}},
    {.kind = M_METHOD,
     .name = "path_compare",
     .flags = M_CAT_ADVANCED,
     .doc = "Byte-compare two files: -1 if identical, else the first differing offset",
     .method = {.args = files_compare_args, .nargs = 2, .result = V_INT, .fn = files_method_path_compare}},
    {.kind = M_METHOD,
     .name = "ls",
     .examples = EXAMPLES("files.ls", "files.ls \"/opfs/images\""),
     .doc = "Print a directory listing",
     .method = {.args = files_path_arg_optional, .nargs = 1, .result = V_BOOL, .fn = files_method_ls}},
    {.kind = M_METHOD,
     .name = "list",
     .examples = EXAMPLES("files.list", "files.list \"/opfs/images/hd/system.img/System Folder\""),
     .doc = "List a directory, descending into disk images",
     .method = {.result_doc = "a list of {name, kind, size} maps",
                .args = files_path_arg_optional,
                .nargs = 1,
                .result = V_LIST,
                .fn = files_method_list}},
    {.kind = M_METHOD,
     .name = "mkdir",
     .examples = EXAMPLES("files.mkdir \"/opfs/images/cd\""),
     .doc = "Create a directory",
     .method = {.args = files_any_path_arg, .nargs = 1, .result = V_BOOL, .fn = files_method_mkdir}},
    {.kind = M_METHOD,
     .name = "cd",
     .examples = EXAMPLES("files.cd \"/opfs/images\"", "files.cd .."),
     .doc = "Make a directory the current one: where relative paths start, and what ls lists by default",
     .method = {.args = files_dir_arg, .nargs = 1, .result = V_NONE, .fn = files_method_cd}},
    {.kind = M_METHOD,
     .name = "pwd",
     .examples = EXAMPLES("files.pwd"),
     .doc = "The current directory",
     .method = {.args = NULL, .nargs = 0, .result = V_STRING, .fn = files_method_pwd}},
    {.kind = M_METHOD,
     .name = "cat",
     .examples = EXAMPLES("files.cat \"/opfs/notes.txt\""),
     .doc = "Print the raw bytes of a file (data fork, rsrc, finder_info)",
     .method = {.args = files_any_path_arg, .nargs = 1, .result = V_BOOL, .fn = files_method_cat}},
    {.kind = M_METHOD,
     .name = "download",
     .examples = EXAMPLES("files.download \"/opfs/images/hd/system.img\""),
     .doc = "Trigger a browser file download (WASM-only)",
     .method =
         {.ui_flags = MM_IO, .args = files_any_path_arg, .nargs = 1, .result = V_BOOL, .fn = files_method_download}},
};

static const class_desc_t files_class = {
    .name = "files",
    .members = files_members,
    .n_members = sizeof(files_members) / sizeof(files_members[0]),
    .doc = "Host files, disk images, image mounts and archives",
};

// === files.mounts =============================================================
//
// The image-VFS auto-mount cache as a collection, indexed by each mount's
// never-reused serial number (image_vfs.h).  image_vfs.c stays free of the
// object model (unit tests link it bare), so the entry objects live here:
// made on demand when a serial is first handed out and freed once its mount
// is gone -- both only ever on the object-model thread, in get()/next().  A
// freed entry fires its invalidators, so a held reference goes stale rather
// than dangling.

#define FILES_MOUNT_ENTRIES 16

typedef struct {
    int serial; // -1 = free
    struct object *obj;
} files_mount_entry_t;

static files_mount_entry_t g_mount_entries[FILES_MOUNT_ENTRIES];
static struct object *g_files_object = NULL;
static struct object *g_files_mounts_object = NULL;

// The serial an entry object stands for.
static int mount_entry_serial(struct object *self) {
    return (int)(intptr_t)object_data(self);
}

// Snapshot of the entry's mount; false when it has been unmounted since.
static bool mount_entry_info(struct object *self, image_vfs_mount_info_t *info) {
    return image_vfs_mount_info(mount_entry_serial(self), info);
}

static value_t mount_attr_path(struct object *self, const member_t *m) {
    (void)m;
    image_vfs_mount_info_t info;
    if (!mount_entry_info(self, &info))
        return val_err("mount %d is gone", mount_entry_serial(self));
    return val_str(info.path);
}

static value_t mount_attr_format(struct object *self, const member_t *m) {
    (void)m;
    image_vfs_mount_info_t info;
    if (!mount_entry_info(self, &info))
        return val_err("mount %d is gone", mount_entry_serial(self));
    return val_str(info.format);
}

static value_t mount_attr_partitions(struct object *self, const member_t *m) {
    (void)m;
    image_vfs_mount_info_t info;
    if (!mount_entry_info(self, &info))
        return val_err("mount %d is gone", mount_entry_serial(self));
    return val_uint(4, info.partitions);
}

static value_t mount_attr_refcount(struct object *self, const member_t *m) {
    (void)m;
    image_vfs_mount_info_t info;
    if (!mount_entry_info(self, &info))
        return val_err("mount %d is gone", mount_entry_serial(self));
    return val_uint(4, info.refcount);
}

static value_t mount_attr_busy(struct object *self, const member_t *m) {
    (void)m;
    image_vfs_mount_info_t info;
    if (!mount_entry_info(self, &info))
        return val_err("mount %d is gone", mount_entry_serial(self));
    return val_bool(info.busy);
}

// `files.mounts[n].unmount()` — drop this cached image-VFS mount.  With
// handles still open the mount refuses new access and the last handle to
// close drops it.
static value_t mount_method_unmount(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    image_vfs_mount_info_t info;
    if (!mount_entry_info(self, &info))
        return val_err("unmount: mount %d is gone", mount_entry_serial(self));
    int rc = image_vfs_unmount(info.path);
    if (rc == 0) {
        gs_outf("unmounted %s\n", info.path);
        return val_bool(true);
    }
    if (rc == -EBUSY)
        gs_outf("image unmount: %s has live handles; refusing new access until they close\n", info.path);
    else
        gs_outf("image unmount: %s: %s\n", info.path, strerror(-rc));
    return val_bool(false);
}

static const member_t files_mount_members[] = {
    {.kind = M_ATTR,
     .name = "path",
     .doc = "Canonical host path of the mounted image file",
     .attr = {.type = V_STRING, .get = mount_attr_path}                                                       },
    {.kind = M_ATTR,
     .name = "format",
     .doc = "Container format: APM, HFS, UFS or raw",
     .attr = {.type = V_STRING, .get = mount_attr_format}                                                     },
    {.kind = M_ATTR,
     .name = "partitions",
     .doc = "Partitions the mount exposes",
     .attr = {.type = V_UINT, .width = 4, .get = mount_attr_partitions}                                       },
    {.kind = M_ATTR,
     .name = "refcount",
     .doc = "Open handles into the mount",
     .attr = {.type = V_UINT, .width = 4, .get = mount_attr_refcount, .presentation_flags = VAL_VOLATILE}     },
    {.kind = M_ATTR,
     .name = "busy",
     .doc = "True while the mount refuses service (unmount pending, or the image is attached writable)",
     .attr = {.type = V_BOOL, .get = mount_attr_busy, .presentation_flags = VAL_VOLATILE}                     },
    {.kind = M_METHOD,
     .name = "unmount",
     .doc = "Drop this cached image mount",
     .method = {.ui_flags = MM_MUTATE, .args = NULL, .nargs = 0, .result = V_BOOL, .fn = mount_method_unmount}},
};

static const class_desc_t files_mount_class = {
    .name = "mount",
    .members = files_mount_members,
    .n_members = sizeof(files_mount_members) / sizeof(files_mount_members[0]),
    .doc = "One cached disk-image mount",
};

// Free entry objects whose mount is gone.
static void mount_entries_sweep(void) {
    for (int i = 0; i < FILES_MOUNT_ENTRIES; i++) {
        files_mount_entry_t *e = &g_mount_entries[i];
        if (e->obj && !image_vfs_mount_info(e->serial, NULL)) {
            object_delete(e->obj);
            e->obj = NULL;
            e->serial = -1;
        }
    }
}

static struct object *files_mounts_get(struct object *self, int index) {
    (void)self;
    if (index < 0 || !image_vfs_mount_info(index, NULL))
        return NULL;
    for (int i = 0; i < FILES_MOUNT_ENTRIES; i++)
        if (g_mount_entries[i].obj && g_mount_entries[i].serial == index)
            return g_mount_entries[i].obj;
    mount_entries_sweep();
    for (int i = 0; i < FILES_MOUNT_ENTRIES; i++) {
        files_mount_entry_t *e = &g_mount_entries[i];
        if (e->obj)
            continue;
        e->obj = object_new(&files_mount_class, (void *)(intptr_t)index, NULL);
        if (!e->obj)
            return NULL;
        e->serial = index;
        object_set_logical_parent(e->obj, g_files_mounts_object, NULL, index, NULL);
        return e->obj;
    }
    return NULL; // more live mounts than entry slots: cannot happen (image_vfs holds 8)
}

static int files_mounts_next(struct object *self, int prev_index) {
    (void)self;
    if (prev_index < 0)
        mount_entries_sweep();
    return image_vfs_next_serial(prev_index);
}

// `files.mounts.find(path)` — the index of the mount caching `path`
// (relative, canonical, or through the VFS's own path forms), or -1.
static value_t files_mounts_method_find(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    const char *path = argv[0].s;
    char resolved[VFS_PATH_MAX];
    const vfs_backend_t *be = NULL;
    void *bctx = NULL;
    const char *tail = NULL;
    if (vfs_resolve(path, resolved, sizeof(resolved), &be, &bctx, &tail) == 0)
        path = resolved;
    return val_int(image_vfs_serial_for_path(path));
}

static const member_t files_mounts_members[] = {
    {.kind = M_CHILD,
     .name = "entries",
     .doc = "Cached image mounts, by mount serial",
     .child = {.cls = &files_mount_class, .indexed = true, .get = files_mounts_get, .next = files_mounts_next}},
    {.kind = M_METHOD,
     .name = "find",
     .examples = EXAMPLES("files.mounts.find \"/opfs/images/hd/system.img\""),
     .doc = "Index of the mount caching an image path, or -1",
     .method = {.args = files_path_arg, .nargs = 1, .result = V_INT, .fn = files_mounts_method_find}},
};

static const class_desc_t files_mounts_class = {
    .name = "mounts",
    .members = files_mounts_members,
    .n_members = sizeof(files_mounts_members) / sizeof(files_mounts_members[0]),
    .doc = "Cached disk-image mounts, by mount serial",
};

// Per-slot image-entry object setup/teardown for files.images
// indexed children.
static void files_images_init(struct config *cfg, struct object *images) {
    for (int i = 0; i < MAX_IMAGES; i++) {
        g_image_data[i].cfg = cfg;
        g_image_data[i].slot = i;
        g_image_objs[i] = object_new(&files_image_class, &g_image_data[i], NULL);
        object_set_logical_parent(g_image_objs[i], images, NULL, i, NULL);
    }
}

static void files_images_teardown(void) {
    for (int i = 0; i < MAX_IMAGES; i++) {
        if (g_image_objs[i]) {
            object_delete(g_image_objs[i]);
            g_image_objs[i] = NULL;
        }
        g_image_data[i].cfg = NULL;
        g_image_data[i].slot = 0;
    }
}

// files.images: the storage view of cfg->images, under the process singleton
// `files`, installed with every machine.
static void files_images_install(struct config *cfg) {
    struct object *images = root_attach_stub(g_files_object, object_new(&files_images_collection_class, cfg, "images"));
    if (!images)
        return;
    object_set_label(images, "Images");
    object_set_order(images, 10);
    files_images_init(cfg, images);
}

// `files` is a process singleton created at shell init: the file methods,
// the mounts collection and the archive child live as long as the process.
// `files.images` is the per-machine part, attached by root_install.
void files_init(void) {
    if (g_files_object)
        return;
    for (int i = 0; i < FILES_MOUNT_ENTRIES; i++)
        g_mount_entries[i].serial = -1;
    g_files_object = object_new(&files_class, NULL, "files");
    if (!g_files_object)
        return;
    object_set_label(g_files_object, "Files");
    object_set_order(g_files_object, 30);
    object_attach(object_root(), g_files_object);
    g_files_mounts_object = object_new(&files_mounts_class, NULL, "mounts");
    if (g_files_mounts_object) {
        object_set_label(g_files_mounts_object, "Mounts");
        object_set_order(g_files_mounts_object, 20);
        object_attach(g_files_object, g_files_mounts_object);
    }
    archive_init(g_files_object);
    root_register_install(files_images_install, files_images_teardown);
}
