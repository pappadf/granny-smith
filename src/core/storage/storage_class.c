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
#include "chunk_cache.h"
#include "format_registry.h"
#include "gs_out.h"
#include "io_leaf.h"
#include "storage.h"
#include "io/io_worker.h"
#include "mailbox/mailbox.h"

#include "archive.h"
#include "crc32.h"
#include "image.h"
#include "image_apm.h"
#include "image_chunkmap.h"
#include "image_hfs.h"
#include "image_iso9660.h"
#include "image_ndif.h"
#include "image_part.h"
#include "image_udif.h"
#include "image_vfs.h"
#include "object.h"
#include "root.h"
#include "shell.h"
#include "storage_util.h"
#include "system.h"
#include "system_config.h"
#include "udif_writer.h"
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
// enumerates the cfg->images[] entries. Slot index in the collection
// matches the slot in cfg->images[]; n_images is dense from
// 0..n_images-1, so the collection's count() returns cfg->n_images.
// Each entry's data is the cfg; its index is its slot.

static image_t *files_image_at(struct object *self) {
    config_t *cfg = (config_t *)object_data(self);
    int slot = object_entry_index(self);
    if (!cfg || slot < 0 || slot >= cfg->n_images)
        return NULL;
    return cfg->images[slot];
}

static DEF_GETTER(files_image_attr_index) {
    return val_int(object_entry_index(self));
}
static DEF_GETTER(files_image_attr_filename) {
    image_t *img = files_image_at(self);
    const char *s = img ? image_get_filename(img) : NULL;
    return val_str(s ? s : "");
}
static DEF_GETTER(files_image_attr_path) {
    image_t *img = files_image_at(self);
    const char *s = img ? image_path(img) : NULL;
    return val_str(s ? s : "");
}
static DEF_GETTER(files_image_attr_raw_size) {
    image_t *img = files_image_at(self);
    return val_uint(8, img ? (uint64_t)img->raw_size : 0);
}
static DEF_GETTER(files_image_attr_writable) {
    image_t *img = files_image_at(self);
    return val_bool(img ? img->writable : false);
}

static DEF_GETTER(files_image_attr_reads) {
    image_t *img = files_image_at(self);
    return val_uint(8, img ? img->reads : 0);
}
static DEF_GETTER(files_image_attr_writes) {
    image_t *img = files_image_at(self);
    return val_uint(8, img ? img->writes : 0);
}

static value_t files_image_attr_format(struct object *self, const member_t *m) {
    (void)m;
    image_t *img = files_image_at(self);
    return val_str(img && img->format ? img->format : "");
}

// Designated-initialiser table keyed by `image_type` so a future enum
// reorder (or a value inserted out of order) keeps the labels aligned.
static const char *const STORAGE_IMAGE_TYPE_NAMES[] = {
    [image_other] = "other", [image_fd_ss] = "fd_ss", [image_fd_ds] = "fd_ds", [image_fd_dd_mfm] = "fd_720k_mfm",
    [image_fd_hd] = "fd_hd", [image_hd] = "hd",       [image_cdrom] = "cdrom",
};

static DEF_GETTER(files_image_attr_type) {
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
     .name = "format",
     .doc = "Wrapper layers peeled to reach the disk, outermost first: raw, dc42, udif, bin+ndif, gz+dc42, ...",
     .attr = {.type = V_STRING, .get = files_image_attr_format, .set = NULL}                                  },
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

// The image entry objects, made on first use; freed with the machine.
static object_cache_t g_images = OBJECT_CACHE(&files_image_class, NULL);

static struct object *files_images_get(struct object *self, int index) {
    config_t *cfg = (config_t *)object_data(self);
    if (!cfg || index < 0 || index >= MAX_IMAGES)
        return NULL;
    if (index >= cfg->n_images || !cfg->images[index])
        return NULL;
    return object_cache_at(&g_images, index, cfg);
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
    return shell_cp_to_host(j->a, j->b, j->flag, j->err, sizeof j->err);
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
    if (rc == IMAGE_CREATE_EXISTS)
        snprintf(j->err, sizeof j->err, "file already exists: %s", j->a);
    else if (rc != 0)
        snprintf(j->err, sizeof j->err, "failed to create blank floppy '%s'", j->a);
    return rc == 0 ? 0 : -EIO;
}

static int work_profile_create(io_leaf_t *j) {
    int rc = image_create_blank_profile(j->a, j->blocks);
    if (rc == IMAGE_CREATE_EXISTS)
        snprintf(j->err, sizeof j->err, "file already exists: %s", j->a);
    else if (rc != 0)
        snprintf(j->err, sizeof j->err, "failed to create blank ProFile image '%s'", j->a);
    return rc == 0 ? 0 : -EIO;
}

// Returns the destination path as a V_STRING.
static DEF_METHOD(files_method_import) {
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

static const collection_desc_t files_images = {
    .entry = &files_image_class,
    .by_index = {.get = files_images_get, .slots = MAX_IMAGES},
    .name = "files_images",
    .doc = "The machine's configured disk images",
};

// `files.list_dir(path)` — list directory entries via the VFS as a
// V_LIST<V_STRING>. Used by url-media.js to enumerate ROMs in OPFS.
static DEF_METHOD(files_method_list_dir) {
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
// Drop the cached image-VFS mounts of `path` and of anything under it before
// it is removed, moved or overwritten.  A mount holds its file open, and OPFS
// refuses to remove or rename an open file (Linux allows it, so native runs
// never see this): an archive files.archive.extract left mounted made the
// page's move of the disk unpacked over it fail ("copied, but failed to
// remove source").  A mount with live handles stays, and the operation fails
// as it would have.
static void release_cached_mounts(const char *path) {
    if (!path || !*path)
        return;
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/')
        n--;
    for (int s = image_vfs_next_serial(-1); s >= 0; s = image_vfs_next_serial(s)) {
        image_vfs_mount_info_t info;
        if (!image_vfs_mount_info(s, &info))
            continue;
        if (strncmp(info.path, path, n) == 0 && (info.path[n] == '\0' || info.path[n] == '/'))
            image_vfs_unmount(info.path);
    }
}

static DEF_METHOD(files_method_cp) {
    const char *src = argv[0].s;
    const char *dst = argv[1].s;
    bool recursive = argc > 2 && argv[2].kind == V_BOOL && argv[2].b;
    if (destination_attached(dst))
        return val_err("files.cp: '%s' is attached to a device (E_BUSY)", dst);
    release_cached_mounts(dst);
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
static DEF_METHOD(files_method_export_raw) {
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
static DEF_METHOD(files_method_find_media) {
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
static DEF_METHOD(files_method_hd_create) {
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
static DEF_METHOD(files_method_rm) {
    checkpoint_quick_wait(); // a checkpoint publish in flight lands before anything moves or goes
    (void)self;
    (void)m;
    (void)argc;
    const char *path = argv[0].s;
    if (files_path_is_protected(path))
        return val_err("files.rm: refusing to remove '%s'", path ? path : "(null)");
    release_cached_mounts(path);
    int rc = gs_rm_tree(path);
    if (rc < 0)
        return val_err("files.rm: cannot remove '%s': %s", path, strerror(-rc));
    return val_bool(true);
}

// `files.mv(src, dst)` — move/rename within the host filesystem. Like
// files.rm, routing the web UI's moves through the worker (rather than the
// browser's main-thread OPFS API) keeps WasmFS coherent. Tries rename() first
// (fast / atomic on the same volume); falls back to a recursive copy + remove.
static DEF_METHOD(files_method_mv) {
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
    release_cached_mounts(src);
    if (rename(src, dst) == 0)
        return val_bool(true);
    char err[256] = {0};
    if (shell_cp_to_host(src, dst, true, err, sizeof(err)) < 0)
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
static DEF_METHOD(files_method_fd_create) {
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
static DEF_METHOD(files_method_profile_create) {
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

// `files.partmap(path)` — print the Apple Partition Map of an image.
static DEF_METHOD(files_method_partmap) {
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
    // disk_size is whole 512-byte blocks: image_apm_parse refuses any other geometry.
    gs_outf("format: APM (%uB blocks, %zu total)\n", (unsigned)APM_BLOCK_SIZE, disk_size(img) / APM_BLOCK_SIZE);
    // The index column is as wide as the largest index (at least 2), so a
    // map with 100+ entries keeps its columns.
    int iw = 2;
    for (uint32_t i = 0; i < table->n_partitions; i++) {
        int w = snprintf(NULL, 0, "%u", (unsigned)table->partitions[i].index);
        if (w > iw)
            iw = w;
    }
    gs_outf("  %-*s Name                             Type                        Start        Size  FS\n", iw, "#");
    for (uint32_t i = 0; i < table->n_partitions; i++) {
        const apm_partition_t *p = &table->partitions[i];
        gs_outf("  %-*u %-32s %-24s %10llu  %10llu  %s\n", iw, (unsigned)p->index, p->name[0] ? p->name : "(unnamed)",
                p->type[0] ? p->type : "(unknown)", (unsigned long long)p->start_block,
                (unsigned long long)p->size_blocks, image_apm_fs_kind_label(p->fs_kind));
    }
    image_apm_free(table);
    image_close(img);
    return val_bool(true);
}

// The filesystem a partition's volume header names, or NULL.
static const char *partition_volume_kind(image_t *img, size_t size, const apm_partition_t *p) {
    uint64_t at = p->start_block * APM_BLOCK_SIZE + 1024;
    uint8_t hdr[2];
    if (p->size_blocks * APM_BLOCK_SIZE < 1024 + sizeof(hdr) || at + sizeof(hdr) > size ||
        image_read_bytes(img, at, hdr, sizeof(hdr)) != 0)
        return NULL;
    switch (RD_BE16(hdr)) {
    case HFS_SIG_BD:
        return "HFS";
    case HFS_SIG_HP:
        return "HFS+";
    case HFS_SIG_HX:
        return "HFSX";
    default:
        return NULL;
    }
}

// files.probe's lines for an APM disk's filesystem partitions.
static void probe_report_partitions(image_t *img, size_t size) {
    apm_table_t *table = image_apm_parse(img, NULL);
    if (!table)
        return;
    for (uint32_t i = 0; i < table->n_partitions; i++) {
        const apm_partition_t *p = &table->partitions[i];
        if (p->fs_kind != APM_FS_HFS && p->fs_kind != APM_FS_UFS && p->fs_kind != APM_FS_UNKNOWN)
            continue;
        const char *vol = partition_volume_kind(img, size, p);
        if (!vol && p->fs_kind == APM_FS_UNKNOWN)
            continue; // an unrecognised partition with no volume we know
        gs_outf("partition %u: %s (%s)%s%s\n", (unsigned)p->index, p->name[0] ? p->name : "(unnamed)",
                p->type[0] ? p->type : "(unknown)", vol ? ", volume " : "", vol ? vol : "");
    }
    image_apm_free(table);
}

// `files.probe(path)` — identify the format of a disk image.
static DEF_METHOD(files_method_probe) {
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
    // ISO 9660 by the same probe the VFS mounts with, so a disc reported as
    // a hybrid here is one the VFS shows both sides of.
    gs_source_t *isrc = image_source(img);
    bool iso = iso_probe_source(isrc, 0, size);
    gs_source_release(isrc);
    // The volume header at 1024: 'BD' is HFS, 'H+' HFS Plus and 'HX' HFSX.
    const char *hfs = NULL;
    if (!apm && size >= 1024 + 512 && image_read_bytes(img, 1024, block, sizeof(block)) == 0) {
        uint16_t sig = RD_BE16(block);
        if (sig == HFS_SIG_BD)
            hfs = "HFS";
        else if (block[0] == 'H' && (block[1] == '+' || block[1] == 'X'))
            hfs = "HFS+";
    }
    if (apm && iso)
        gs_outf("format: APM + ISO 9660 hybrid (%zu bytes)\n", size);
    else if (apm)
        gs_outf("format: APM (%zu bytes)\n", size);
    else if (hfs && iso)
        gs_outf("format: %s + ISO 9660 hybrid (bare, %zu bytes)\n", hfs, size);
    else if (iso)
        gs_outf("format: ISO 9660 (%zu bytes)\n", size);
    else if (hfs)
        gs_outf("format: %s (bare, %zu bytes)\n", hfs, size);
    else
        gs_outf("format: unrecognised / raw (%zu bytes)\n", size);
    // A partitioned disk's volumes: each partition the map names as a
    // filesystem, and what its own header says it is (an HFS volume's
    // MDB sits 1024 bytes into its partition, as on a bare disk).
    if (apm)
        probe_report_partitions(img, size);
    // What the format registry peeled to reach the disk, and what it finds
    // the disk to be.
    if (img->format && strcmp(img->format, "raw") != 0)
        gs_outf("encoding: %s\n", img->format);
    gs_source_t *src = image_source(img);
    const gs_format_t *contents = gs_format_contents(src, NULL);
    gs_source_release(src);
    if (contents)
        gs_outf("contents: %s\n", contents->doc);
    image_close(img);
    return val_bool(true);
}

// `files.path_exists(path)` — true if the path resolves in the shell VFS.
static DEF_METHOD(files_method_path_exists) {
    vfs_stat_t st;
    return val_bool(vfs_stat(argv[0].s, &st) == 0);
}

// `files.path_size(path)` — file size in bytes (0 on stat failure).
static DEF_METHOD(files_method_path_size) {
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
static DEF_METHOD(files_method_path_compare) {
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

static DEF_GETTER(files_attr_xfer_buffer) {
    return val_uint(4, (uint32_t)(uintptr_t)g_xfer);
}

static DEF_GETTER(files_attr_xfer_size) {
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
static DEF_METHOD(files_method_xfer_write) {
    uint64_t offset = argv[1].u, len = argv[2].u;
    if (len > STORAGE_XFER_BYTES)
        return val_err("files.xfer_write: %llu bytes is more than the %u-byte window", (unsigned long long)len,
                       STORAGE_XFER_BYTES);
    return xfer_dispatch(argv[0].s, offset, len, true, "files.xfer_write");
}

// `files.xfer_read(path, offset, len)` — read up to `len` bytes of `path`
// from `offset` into the window; answers how many (0 at the end).
static DEF_METHOD(files_method_xfer_read) {
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

// === UDIF: the writer, conversion and verification ==========================
//
// Disk images enter /opfs/images compressed: the page streams the decoded
// bytes of an upload or a URL body through the transfer window into a
// writer (udif_open / udif_append / udif_finish), so neither the expanded
// image nor any full-size copy of it ever exists.  `convert` does the same
// from a file the core can open; `verify` reads a UDIF through once.

#define FILES_UDIF_HANDLES 4
static udif_writer_t *g_udif[FILES_UDIF_HANDLES];
static char *g_udif_path[FILES_UDIF_HANDLES];

typedef struct {
    int handle;
    uint32_t chunk_kb;
    int level;
    char *source_name;
    char *origin;
    uint64_t len;
    udif_writer_stats_t st;
    bool raw; // convert: to a flat raw image instead
} udif_job_t;

static void udif_job_cleanup(io_leaf_t *j) {
    udif_job_t *u = (udif_job_t *)j->ud;
    if (u) {
        free(u->source_name);
        free(u->origin);
    }
    free(u);
}

static value_t udif_stats_map(const udif_writer_stats_t *st) {
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "sectors", val_uint(8, st->sectors));
    val_map_put(b, "bytes_in", val_uint(8, st->bytes_in));
    val_map_put(b, "stored_bytes", val_uint(8, st->stored_bytes));
    val_map_put(b, "zero_bytes", val_uint(8, st->zero_bytes));
    val_map_put(b, "extents", val_uint(8, st->extents));
    val_map_put(b, "crc", val_uint(4, st->crc));
    return val_map_finish(b);
}

static value_t answer_udif_stats(io_leaf_t *j) {
    return udif_stats_map(&((udif_job_t *)j->ud)->st);
}

static value_t answer_udif_stored(io_leaf_t *j) {
    return val_uint(8, ((udif_job_t *)j->ud)->st.stored_bytes);
}

static value_t answer_udif_handle(io_leaf_t *j) {
    return val_int(((udif_job_t *)j->ud)->handle);
}

// A message for an errno a writer returned, quota first: in the browser a
// full origin is what ENOSPC means.
static void udif_errmsg(io_leaf_t *j, const char *what, int rc) {
    if (rc == -ENOSPC || rc == -EDQUOT)
        snprintf(j->err, sizeof j->err, "not enough storage to %s '%s'", what, j->a);
    else
        snprintf(j->err, sizeof j->err, "%s '%s' failed: %s", what, j->a, strerror(rc < 0 ? -rc : EIO));
}

static int work_udif_open(io_leaf_t *j) {
    udif_job_t *u = (udif_job_t *)j->ud;
    int h = -1;
    for (int i = 0; i < FILES_UDIF_HANDLES; i++)
        if (!g_udif[i]) {
            h = i;
            break;
        }
    if (h < 0) {
        snprintf(j->err, sizeof j->err, "too many images being written (at most %d)", FILES_UDIF_HANDLES);
        return -EBUSY;
    }
    gs_mkdir_parents(j->a);
    udif_writer_opts_t o = {
        .chunk_sectors = u->chunk_kb * 2, .level = u->level, .source_name = u->source_name, .origin = u->origin};
    g_udif[h] = udif_writer_open(j->a, &o, j->err, sizeof j->err);
    if (!g_udif[h])
        return -EIO;
    g_udif_path[h] = gs_strdup(j->a);
    u->handle = h;
    return 0;
}

static udif_writer_t *udif_handle(io_leaf_t *j, int h) {
    if (h < 0 || h >= FILES_UDIF_HANDLES || !g_udif[h]) {
        snprintf(j->err, sizeof j->err, "no image is being written under handle %d", h);
        return NULL;
    }
    return g_udif[h];
}

static void udif_release(int h) {
    g_udif[h] = NULL;
    free(g_udif_path[h]);
    g_udif_path[h] = NULL;
}

static int work_udif_append(io_leaf_t *j) {
    udif_job_t *u = (udif_job_t *)j->ud;
    udif_writer_t *w = udif_handle(j, u->handle);
    if (!w)
        return -EBADF;
    int rc = udif_writer_append(w, g_xfer, (size_t)u->len);
    if (rc) {
        free(j->a);
        j->a = gs_strdup(g_udif_path[u->handle]);
        udif_errmsg(j, "write", rc);
    }
    udif_writer_progress(w, &u->st);
    return rc;
}

static int work_udif_finish(io_leaf_t *j) {
    udif_job_t *u = (udif_job_t *)j->ud;
    udif_writer_t *w = udif_handle(j, u->handle);
    if (!w)
        return -EBADF;
    free(j->a);
    j->a = gs_strdup(g_udif_path[u->handle]);
    udif_release(u->handle);
    int rc = udif_writer_finish(w, &u->st); // frees the writer either way
    if (rc)
        udif_errmsg(j, "finish", rc);
    return rc;
}

static int work_udif_abort(io_leaf_t *j) {
    udif_job_t *u = (udif_job_t *)j->ud;
    udif_writer_t *w = udif_handle(j, u->handle);
    if (!w)
        return -EBADF;
    udif_release(u->handle);
    udif_writer_abort(w);
    return 0;
}

// The decoded bytes of an image the core can open, for a conversion.  A
// UDIF is opened with no in-place chunk bound -- a conversion passes every
// chunk once, which is how an image with chunks too large to read in place
// gets re-chunked; anything else through the image layer.
static gs_source_t *open_decoded(const char *path, image_t **img, char *err, size_t cap) {
    *img = NULL;
    int e = 0;
    gs_source_t *data = gs_source_open_path(path, GS_FORK_DATA, &e);
    if (!data) {
        snprintf(err, cap, "cannot open '%s': %s", path, strerror(e ? -e : ENOENT));
        return NULL;
    }
    uint64_t size = gs_source_size(data);
    uint8_t tail[UDIF_TRAILER_SIZE];
    if (size >= sizeof(tail) && gs_source_read_exact(data, size - sizeof(tail), tail, sizeof(tail)) == 0 &&
        udif_source_detect(tail, sizeof(tail))) {
        gs_source_t *s = udif_source_open_bounded(data, NDIF_MAX_CHUNK_BYTES, &e);
        gs_source_release(data);
        if (!s)
            snprintf(err, cap, "'%s' is a UDIF image this emulator cannot decode (%s)", path,
                     e == -ENOTSUP ? "unsupported compression" : strerror(e ? -e : EINVAL));
        return s;
    }
    gs_source_release(data);
    *img = image_open_readonly(path);
    if (!*img) {
        snprintf(err, cap, "cannot open '%s' as a disk image", path);
        return NULL;
    }
    return image_source(*img);
}

#define CONVERT_STEP (1u << 20)

static int work_convert(io_leaf_t *j) {
    udif_job_t *u = (udif_job_t *)j->ud;
    vfs_stat_t vst;
    if (vfs_stat(j->b, &vst) == 0) {
        snprintf(j->err, sizeof j->err, "'%s' exists (refuses to overwrite)", j->b);
        return -EEXIST;
    }
    image_t *img = NULL;
    gs_source_t *src = open_decoded(j->a, &img, j->err, sizeof j->err);
    if (!src) {
        image_close(img);
        return -EINVAL;
    }
    uint64_t total = gs_source_size(src);
    uint8_t *buf = malloc(CONVERT_STEP);
    udif_writer_t *w = NULL;
    FILE *raw = NULL;
    int rc = buf ? 0 : -ENOMEM;
    gs_mkdir_parents(j->b);
    if (!rc && u->raw) {
        raw = fopen(j->b, "wb");
        if (!raw) {
            rc = errno ? -errno : -EIO;
            snprintf(j->err, sizeof j->err, "cannot create '%s': %s", j->b, strerror(-rc));
        }
    } else if (!rc) {
        const char *base = strrchr(j->a, '/');
        udif_writer_opts_t o = {.chunk_sectors = u->chunk_kb * 2,
                                .level = u->level,
                                .source_name = u->source_name ? u->source_name
                                               : base         ? base + 1
                                                              : j->a,
                                .origin = u->origin};
        w = udif_writer_open(j->b, &o, j->err, sizeof j->err);
        if (!w)
            rc = -EIO;
    }
    uint32_t crc = 0;
    for (uint64_t at = 0; !rc && at < total;) {
        if (io_check_cancelled()) {
            rc = -ECANCELED;
            snprintf(j->err, sizeof j->err, "cancelled");
            break;
        }
        size_t n = total - at < CONVERT_STEP ? (size_t)(total - at) : CONVERT_STEP;
        rc = gs_source_read_exact(src, at, buf, n);
        if (rc) {
            snprintf(j->err, sizeof j->err, "read of '%s' at %llu failed", j->a, (unsigned long long)at);
            break;
        }
        crc = gs_crc32(crc, buf, n);
        if (raw) {
            if (fwrite(buf, 1, n, raw) != n) {
                rc = -EIO;
                udif_errmsg(j, "write", -ENOSPC);
            }
        } else if ((rc = udif_writer_append(w, buf, n)) != 0) {
            udif_errmsg(j, "write", rc);
        }
        at += n;
        io_report_progress(at, total);
    }
    free(buf);
    gs_source_release(src);
    image_close(img);
    if (raw) {
        if (fclose(raw) != 0 && !rc)
            rc = -EIO;
        if (rc)
            remove(j->b);
        u->st.bytes_in = u->st.stored_bytes = total;
        u->st.sectors = total / UDIF_SECTOR_SIZE;
        u->st.crc = crc;
        return rc;
    }
    if (rc) {
        udif_writer_abort(w);
        return rc;
    }
    rc = udif_writer_finish(w, &u->st);
    if (rc) {
        udif_errmsg(j, "finish", rc);
        return rc;
    }
    // Read what was written back through the verifier: the decoded bytes
    // must be the ones read (whole sectors: a tail is zero-padded).
    gs_source_t *out = gs_source_host(j->b, NULL);
    udif_writer_stats_t vs;
    char msg[200] = {0};
    rc = out ? udif_verify(out, &vs, msg, sizeof msg) : -EIO;
    gs_source_release(out);
    uint32_t want = gs_crc32_zeros(crc, u->st.sectors * UDIF_SECTOR_SIZE - total);
    if (rc == 0 && (vs.crc != u->st.crc || vs.crc != want))
        rc = -EILSEQ, snprintf(msg, sizeof msg, "decoded checksum %08x, the source's is %08x", vs.crc, want);
    if (rc) {
        snprintf(j->err, sizeof j->err, "'%s' did not verify: %s", j->b, msg);
        remove(j->b);
    }
    return rc;
}

static int work_verify(io_leaf_t *j) {
    udif_job_t *u = (udif_job_t *)j->ud;
    int e = 0;
    gs_source_t *s = gs_source_open_path(j->a, GS_FORK_DATA, &e);
    if (!s) {
        snprintf(j->err, sizeof j->err, "cannot open '%s': %s", j->a, strerror(e ? -e : ENOENT));
        return -ENOENT;
    }
    char msg[200] = {0};
    int rc = udif_verify(s, &u->st, msg, sizeof msg);
    gs_source_release(s);
    if (rc)
        snprintf(j->err, sizeof j->err, "%s: %s", j->a, msg);
    return rc;
}

static value_t udif_dispatch(const char *a, const char *b, udif_job_t *u, int (*work)(io_leaf_t *),
                             value_t (*answer)(io_leaf_t *), const char *what) {
    io_leaf_t *j = io_leaf_new(a, b);
    if (!j || !u) {
        if (u)
            free(u->source_name);
        free(u);
        if (j)
            free(j->a), free(j->b), free(j);
        return val_err("%s: out of memory", what);
    }
    j->ud = u;
    j->cleanup = udif_job_cleanup;
    j->work = work;
    j->answer = answer;
    return io_leaf_dispatch(j, what);
}

// An integer argument that may be absent (V_NONE) or given as a string.
static int64_t opt_int(const value_t *v, int64_t dflt) {
    if (v->kind == V_INT)
        return v->i;
    if (v->kind == V_UINT)
        return (int64_t)v->u;
    if (v->kind == V_STRING && v->s && *v->s)
        return strtoll(v->s, NULL, 10);
    return dflt;
}

static bool chunk_kb_ok(int64_t kb) {
    return kb >= 4 && kb <= 1024 && (kb & (kb - 1)) == 0;
}

// `files.xfer_read_disk(path, offset, len)` -- read up to `len` bytes of the
// *decoded* disk an image holds (any format the emulator reads) from
// `offset` into the transfer window; answers how many (0 at the end).  How
// the page downloads a stored .dmg as a raw image without the raw image
// ever existing.  The decoded source stays open between calls for the same
// path (on the I/O worker, which serialises these jobs).
static gs_source_t *g_rd_src;
static image_t *g_rd_img;
static char *g_rd_path;

static void read_disk_close(void) {
    gs_source_release(g_rd_src);
    image_close(g_rd_img);
    free(g_rd_path);
    g_rd_src = NULL;
    g_rd_img = NULL;
    g_rd_path = NULL;
}

static int work_xfer_read_disk(io_leaf_t *j) {
    xfer_job_t *x = (xfer_job_t *)j->ud;
    if (!g_rd_path || strcmp(g_rd_path, j->a) != 0 || x->offset == 0) {
        read_disk_close();
        g_rd_src = open_decoded(j->a, &g_rd_img, j->err, sizeof j->err);
        if (!g_rd_src) {
            read_disk_close();
            return -EINVAL;
        }
        g_rd_path = gs_strdup(j->a);
    }
    uint64_t size = gs_source_size(g_rd_src);
    x->got = 0;
    if (x->offset >= size) {
        read_disk_close(); // done with it
        return 0;
    }
    size_t n = size - x->offset < x->len ? (size_t)(size - x->offset) : (size_t)x->len;
    if (gs_source_read_exact(g_rd_src, x->offset, g_xfer, n) != 0) {
        snprintf(j->err, sizeof j->err, "read of '%s' at %llu failed", j->a, (unsigned long long)x->offset);
        read_disk_close();
        return -EIO;
    }
    x->got = n;
    return 0;
}

static DEF_METHOD(files_method_xfer_read_disk) {
    uint64_t offset = argv[1].u, len = argv[2].u;
    if (len > STORAGE_XFER_BYTES)
        len = STORAGE_XFER_BYTES;
    io_leaf_t *j = io_leaf_new(argv[0].s, NULL);
    xfer_job_t *x = (xfer_job_t *)calloc(1, sizeof *x);
    if (!j || !x) {
        free(x);
        if (j)
            free(j->a), free(j);
        return val_err("files.xfer_read_disk: out of memory");
    }
    x->offset = offset;
    x->len = len;
    j->ud = x;
    j->cleanup = xfer_cleanup;
    j->work = work_xfer_read_disk;
    j->answer = answer_xfer_read;
    return io_leaf_dispatch(j, "files.xfer_read_disk");
}

// `files.udif_open(path, [chunk_kb], [level], [source_name], [origin])` --
// start writing a UDIF at `path` (which must not exist); answers a handle.
// `origin` is recorded as is (gs-origin) and reported by files.udif_info.
static DEF_METHOD(files_method_udif_open) {
    int64_t kb = argc > 1 ? opt_int(&argv[1], 64) : 64;
    int64_t level = argc > 2 ? opt_int(&argv[2], 1) : 1;
    if (!chunk_kb_ok(kb))
        return val_err("files.udif_open: chunk_kb %lld must be a power of two in 4..1024", (long long)kb);
    if (level < 0 || level > 9)
        return val_err("files.udif_open: level %lld out of range (0..9)", (long long)level);
    udif_job_t *u = calloc(1, sizeof *u);
    if (u) {
        u->handle = -1;
        u->chunk_kb = (uint32_t)kb;
        u->level = (int)level;
        if (argc > 3 && argv[3].kind == V_STRING && argv[3].s && *argv[3].s)
            u->source_name = gs_strdup(argv[3].s);
        if (argc > 4 && argv[4].kind == V_STRING && argv[4].s && *argv[4].s)
            u->origin = gs_strdup(argv[4].s);
    }
    return udif_dispatch(argv[0].s, NULL, u, work_udif_open, answer_udif_handle, "files.udif_open");
}

// `files.udif_append(handle, len)` -- append the transfer window's first
// `len` bytes of the decoded image; answers the bytes stored so far.
static DEF_METHOD(files_method_udif_append) {
    uint64_t len = argv[1].u;
    if (len > STORAGE_XFER_BYTES)
        return val_err("files.udif_append: %llu bytes is more than the %u-byte window", (unsigned long long)len,
                       STORAGE_XFER_BYTES);
    udif_job_t *u = calloc(1, sizeof *u);
    if (u) {
        u->handle = (int)argv[0].i;
        u->len = len;
    }
    return udif_dispatch("", NULL, u, work_udif_append, answer_udif_stored, "files.udif_append");
}

// `files.udif_finish(handle)` -- complete the image; answers its stats.
static DEF_METHOD(files_method_udif_finish) {
    udif_job_t *u = calloc(1, sizeof *u);
    if (u)
        u->handle = (int)argv[0].i;
    return udif_dispatch("", NULL, u, work_udif_finish, answer_udif_stats, "files.udif_finish");
}

// `files.udif_abort(handle)` -- abandon the image and remove the partial file.
static DEF_METHOD(files_method_udif_abort) {
    udif_job_t *u = calloc(1, sizeof *u);
    if (u)
        u->handle = (int)argv[0].i;
    return udif_dispatch("", NULL, u, work_udif_abort, NULL, "files.udif_abort");
}

// `files.convert(src, dst, [chunk_kb], [level], [format], [source_name],
// [origin])` -- write the decoded disk of any image the core reads as a UDIF
// (format "udif", the default) or a flat raw image ("raw"), checking the
// result decodes to the same bytes.  A UDIF records `source_name` (default:
// src's own name) and `origin` (as files.udif_open does).
static DEF_METHOD(files_method_convert) {
    int64_t kb = argc > 2 ? opt_int(&argv[2], 64) : 64;
    int64_t level = argc > 3 ? opt_int(&argv[3], 1) : 1;
    const char *fmt = argc > 4 && argv[4].kind == V_STRING && argv[4].s && *argv[4].s ? argv[4].s : "udif";
    if (!chunk_kb_ok(kb))
        return val_err("files.convert: chunk_kb %lld must be a power of two in 4..1024", (long long)kb);
    if (level < 0 || level > 9)
        return val_err("files.convert: level %lld out of range (0..9)", (long long)level);
    if (strcmp(fmt, "udif") != 0 && strcmp(fmt, "raw") != 0)
        return val_err("files.convert: format '%s' is neither udif nor raw", fmt);
    if (destination_attached(argv[1].s))
        return val_err("files.convert: '%s' is attached to a device (E_BUSY)", argv[1].s);
    udif_job_t *u = calloc(1, sizeof *u);
    if (u) {
        u->chunk_kb = (uint32_t)kb;
        u->level = (int)level;
        u->raw = strcmp(fmt, "raw") == 0;
        if (argc > 5 && argv[5].kind == V_STRING && argv[5].s && *argv[5].s)
            u->source_name = gs_strdup(argv[5].s);
        if (argc > 6 && argv[6].kind == V_STRING && argv[6].s && *argv[6].s)
            u->origin = gs_strdup(argv[6].s);
    }
    return udif_dispatch(argv[0].s, argv[1].s, u, work_convert, answer_udif_stats, "files.convert");
}

// `files.verify(path)` -- decode every chunk of a UDIF and check its
// checksums; answers what it saw.
static DEF_METHOD(files_method_verify) {
    udif_job_t *u = calloc(1, sizeof *u);
    return udif_dispatch(argv[0].s, NULL, u, work_verify, answer_udif_stats, "files.verify");
}

// `files.udif_info(path)` -- what a UDIF's trailer and block map say, read
// without decoding: cheap enough to answer at once.
static DEF_METHOD(files_method_udif_info) {
    int e = 0;
    gs_source_t *s = gs_source_open_path(argv[0].s, GS_FORK_DATA, &e);
    if (!s)
        return val_err("files.udif_info: cannot open '%s': %s", argv[0].s, strerror(e ? -e : ENOENT));
    udif_info_t in;
    int rc = udif_info(s, &in);
    uint64_t file_bytes = gs_source_size(s);
    gs_source_release(s);
    if (rc)
        return val_err("files.udif_info: '%s' is not a UDIF image this emulator reads", argv[0].s);
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "sectors", val_uint(8, in.sectors));
    val_map_put(b, "bytes", val_uint(8, in.byte_length));
    val_map_put(b, "stored_bytes", val_uint(8, file_bytes));
    val_map_put(b, "zero_bytes", val_uint(8, in.zero_bytes));
    val_map_put(b, "extents", val_uint(8, in.extents));
    val_map_put(b, "tables", val_uint(4, in.tables));
    val_map_put(b, "crc", val_uint(4, in.crc));
    val_map_put(b, "max_chunk_bytes", val_uint(8, in.max_chunk_bytes));
    val_map_put(b, "gs_profile", val_bool(in.gs_profile));
    val_map_put(b, "in_place", val_bool(in.gs_profile || in.max_chunk_bytes <= udif_inplace_max_chunk()));
    val_map_put(b, "source_name", val_str(in.source_name));
    val_map_put(b, "origin", val_str(in.origin));
    return val_map_finish(b);
}

static DEF_GETTER(files_attr_udif_max_chunk_kb) {
    return val_uint(8, udif_inplace_max_chunk() >> 10);
}

static DEF_SETTER(files_attr_udif_max_chunk_kb_set) {
    if (in.u < 64 || in.u > (NDIF_MAX_CHUNK_BYTES >> 10))
        return val_err("files.udif_max_chunk_kb: %llu out of range (64..%u)", (unsigned long long)in.u,
                       NDIF_MAX_CHUNK_BYTES >> 10);
    udif_set_inplace_max_chunk((size_t)in.u << 10);
    return val_none();
}

static const arg_decl_t files_udif_open_args[] = {
    ARG_PATH("path", "The image to create (must not exist)"),
    {.name = "chunk_kb",
                                                      .kind = V_NONE,
                                                      .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_POLY,
                                                      .doc = "Chunk size in KB, a power of two in 4..1024",
                                                      .default_doc = "64"  },
    {.name = "level",
                                                      .kind = V_NONE,
                                                      .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_POLY,
                                                      .doc = "Deflate effort 1..9; 0 stores zero runs and raw chunks only",
                                                      .default_doc = "1"   },
    {.name = "source_name",
                                                      .kind = V_STRING,
                                                      .validation_flags = OBJ_ARG_OPTIONAL,
                                                      .doc = "The original file name, recorded in the image",
                                                      .default_doc = "none"},
    {.name = "origin",
                                                      .kind = V_STRING,
                                                      .validation_flags = OBJ_ARG_OPTIONAL,
                                                      .doc = "Where the bytes came from (e.g. a URL), recorded in the image as is",
                                                      .default_doc = "none"},
};
static const arg_decl_t files_udif_append_args[] = {
    {.name = "handle", .kind = V_INT,  .doc = "What udif_open answered"                                         },
    {.name = "len",    .kind = V_UINT, .doc = "Bytes of the transfer window to append (at most files.xfer_size)"},
};
static const arg_decl_t files_udif_handle_args[] = {
    {.name = "handle", .kind = V_INT, .doc = "What udif_open answered"},
};
static const arg_decl_t files_convert_args[] = {
    ARG_PATH("src", "Any disk image the emulator reads (raw, DiskCopy, NDIF, UDIF, ...)"),
    ARG_PATH("dst", "The image to write (must not exist)"),
    {.name = "chunk_kb",
                                                    .kind = V_NONE,
                                                    .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_POLY,
                                                    .doc = "Chunk size in KB, a power of two in 4..1024",
                                                    .default_doc = "64"            },
    {.name = "level",
                                                    .kind = V_NONE,
                                                    .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_POLY,
                                                    .doc = "Deflate effort 1..9; 0 stores zero runs and raw chunks only",
                                                    .default_doc = "1"             },
    {.name = "format",
                                                    .kind = V_STRING,
                                                    .validation_flags = OBJ_ARG_OPTIONAL,
                                                    .doc = "udif, or raw for a flat image",
                                                    .default_doc = "udif"          },
    {.name = "source_name",
                                                    .kind = V_STRING,
                                                    .validation_flags = OBJ_ARG_OPTIONAL,
                                                    .doc = "The original file name, recorded in a UDIF",
                                                    .default_doc = "src's own name"},
    {.name = "origin",
                                                    .kind = V_STRING,
                                                    .validation_flags = OBJ_ARG_OPTIONAL,
                                                    .doc = "Where the image came from (e.g. a URL), recorded in a UDIF as is",
                                                    .default_doc = "none"          },
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
static DEF_METHOD(files_method_download) {
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
     .name = "xfer_read_disk",
     .flags = M_CAT_INTERNAL,
     .doc = "Read up to len bytes of an image's decoded disk at offset into the transfer window; answers the count",
     .method =
         {.ui_flags = MM_IO, .args = files_xfer_args, .nargs = 3, .result = V_UINT, .fn = files_method_xfer_read_disk}},
    {.kind = M_METHOD,
     .name = "udif_open",
     .flags = M_CAT_INTERNAL,
     .doc = "Start writing a UDIF (.dmg) image from decoded bytes; answers a handle for udif_append",
     .method =
         {.ui_flags = MM_IO, .args = files_udif_open_args, .nargs = 5, .result = V_INT, .fn = files_method_udif_open}},
    {.kind = M_METHOD,
     .name = "udif_append",
     .flags = M_CAT_INTERNAL,
     .doc = "Append the transfer window's first len bytes to the image being written",
     .method = {.result_doc = "the image's stored (compressed) bytes so far",
                .ui_flags = MM_IO,
                .args = files_udif_append_args,
                .nargs = 2,
                .result = V_UINT,
                .fn = files_method_udif_append}},
    {.kind = M_METHOD,
     .name = "udif_finish",
     .flags = M_CAT_INTERNAL,
     .doc = "Complete the image being written (block map and trailer)",
     .method = {.result_doc = "{sectors, bytes_in, stored_bytes, zero_bytes, extents, crc}",
                .ui_flags = MM_IO,
                .args = files_udif_handle_args,
                .nargs = 1,
                .result = V_MAP,
                .fn = files_method_udif_finish}},
    {.kind = M_METHOD,
     .name = "udif_abort",
     .flags = M_CAT_INTERNAL,
     .doc = "Abandon the image being written and remove the partial file",
     .method = {.ui_flags = MM_IO,
                .args = files_udif_handle_args,
                .nargs = 1,
                .result = V_BOOL,
                .fn = files_method_udif_abort}},
    {.kind = M_METHOD,
     .name = "convert",
     .examples = EXAMPLES("files.convert \"/opfs/images/hd/system.img\" \"/opfs/images/hd/system.dmg\"",
     "files.convert \"/opfs/images/hd/system.dmg\" \"/opfs/raw/system.img\" format=raw"),
     .doc = "Write a disk image as a compact UDIF (.dmg), or as a flat raw image, and check it decodes the same",
     .method = {.result_doc = "{sectors, bytes_in, stored_bytes, zero_bytes, extents, crc}",
                .ui_flags = MM_IO,
                .args = files_convert_args,
                .nargs = 7,
                .result = V_MAP,
                .fn = files_method_convert}},
    {.kind = M_METHOD,
     .name = "verify",
     .examples = EXAMPLES("files.verify \"/opfs/images/hd/system.dmg\""),
     .doc = "Decode every chunk of a UDIF (.dmg) image and check its checksums",
     .method = {.result_doc = "{sectors, bytes_in, stored_bytes, zero_bytes, extents, crc}",
                .ui_flags = MM_IO,
                .args = files_path_arg,
                .nargs = 1,
                .result = V_MAP,
                .fn = files_method_verify}},
    {.kind = M_METHOD,
     .name = "udif_info",
     .examples = EXAMPLES("files.udif_info \"/opfs/images/hd/system.dmg\""),
     .doc = "What a UDIF (.dmg) image's block map says, without decoding it",
     .method = {.result_doc = "{sectors, bytes, stored_bytes, zero_bytes, extents, tables, crc, max_chunk_bytes, "
                              "gs_profile, in_place, source_name, origin}",
                .args = files_path_arg,
                .nargs = 1,
                .result = V_MAP,
                .fn = files_method_udif_info}},
    {.kind = M_ATTR,
     .name = "udif_max_chunk_kb",
     .flags = M_CAT_ADVANCED,
     .doc = "Largest decoded chunk, in KB, a UDIF from another tool is read in place with (larger: convert it)",
     .attr = {.type = V_UINT, .get = files_attr_udif_max_chunk_kb, .set = files_attr_udif_max_chunk_kb_set}},
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
     .doc = "List a directory, descending into disk images and archives",
     .method = {.result_doc = "a list of {name, kind, size, expandable} maps",
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

static struct object *g_files_object = NULL;

// The serial an entry object stands for.
static int mount_entry_serial(struct object *self) {
    return object_entry_index(self);
}

// Snapshot of the entry's mount; false when it has been unmounted since.
static bool mount_entry_info(struct object *self, image_vfs_mount_info_t *info) {
    return image_vfs_mount_info(mount_entry_serial(self), info);
}

// The fields of a mount entry, each attribute's user_data.
enum { MOUNT_PATH, MOUNT_FORMAT, MOUNT_PARTITIONS, MOUNT_REFCOUNT, MOUNT_BUSY, MOUNT_STALE };

// One getter for every mount attribute: the field its user_data names.
static DEF_GETTER(mount_attr_get) {
    image_vfs_mount_info_t info;
    if (!mount_entry_info(self, &info))
        return val_err("mount %d is gone", mount_entry_serial(self));
    switch ((int)(uintptr_t)m->attr.user_data) {
    case MOUNT_PATH:
        return val_str(info.path);
    case MOUNT_FORMAT:
        return val_str(info.format);
    case MOUNT_PARTITIONS:
        return val_uint(4, info.partitions);
    case MOUNT_REFCOUNT:
        return val_uint(4, info.refcount);
    case MOUNT_STALE:
        return val_bool(info.stale);
    default:
        return val_bool(info.busy);
    }
}

// `files.mounts[n].unmount()` — drop this cached image-VFS mount.  With
// handles still open the mount refuses new access and the last handle to
// close drops it.
static DEF_METHOD(mount_method_unmount) {
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
     .attr = {.type = V_STRING, .get = mount_attr_get, .user_data = (const void *)(uintptr_t)MOUNT_PATH}            },
    {.kind = M_ATTR,
     .name = "format",
     .doc = "Container format: APM, HFS, UFS or raw",
     .attr = {.type = V_STRING, .get = mount_attr_get, .user_data = (const void *)(uintptr_t)MOUNT_FORMAT}          },
    {.kind = M_ATTR,
     .name = "partitions",
     .doc = "Partitions the mount exposes",
     .attr =
         {.type = V_UINT, .width = 4, .get = mount_attr_get, .user_data = (const void *)(uintptr_t)MOUNT_PARTITIONS}},
    {.kind = M_ATTR,
     .name = "refcount",
     .doc = "Open handles into the mount",
     .attr = {.type = V_UINT,
              .width = 4,
              .get = mount_attr_get,
              .user_data = (const void *)(uintptr_t)MOUNT_REFCOUNT,
              .presentation_flags = VAL_VOLATILE}                                                                   },
    {.kind = M_ATTR,
     .name = "busy",
     .doc = "True while the mount refuses service (unmount pending, or the image is attached writable)",
     .attr = {.type = V_BOOL,
              .get = mount_attr_get,
              .user_data = (const void *)(uintptr_t)MOUNT_BUSY,
              .presentation_flags = VAL_VOLATILE}                                                                   },
    {.kind = M_ATTR,
     .name = "stale",
     .doc = "True once the image file changed: a newer mount serves it, this one only its open handles",
     .attr = {.type = V_BOOL,
              .get = mount_attr_get,
              .user_data = (const void *)(uintptr_t)MOUNT_STALE,
              .presentation_flags = VAL_VOLATILE}                                                                   },
    {.kind = M_METHOD,
     .name = "unmount",
     .doc = "Drop this cached image mount",
     .method = {.ui_flags = MM_MUTATE, .args = NULL, .nargs = 0, .result = V_BOOL, .fn = mount_method_unmount}      },
};

static const class_desc_t files_mount_class = {
    .name = "mount",
    .members = files_mount_members,
    .n_members = sizeof(files_mount_members) / sizeof(files_mount_members[0]),
    .doc = "One cached disk-image mount",
};

// The mount entry objects, by serial: made when a serial is first handed
// out and freed once its mount is gone.
static object_cache_t g_mounts = OBJECT_CACHE(&files_mount_class, NULL);

// Whether an entry's mount is still there.
static bool mount_entry_live(struct object *entry, void *ud) {
    (void)ud;
    return image_vfs_mount_info(mount_entry_serial(entry), NULL);
}

static struct object *files_mounts_get(struct object *self, int index) {
    (void)self;
    if (index < 0 || !image_vfs_mount_info(index, NULL))
        return NULL;
    struct object *o = object_cache_find(&g_mounts, index);
    if (o)
        return o;
    object_cache_sweep(&g_mounts, mount_entry_live, NULL);
    return object_cache_at(&g_mounts, index, NULL);
}

static int files_mounts_next(struct object *self, int prev_index) {
    (void)self;
    if (prev_index < 0)
        object_cache_sweep(&g_mounts, mount_entry_live, NULL);
    return image_vfs_next_serial(prev_index);
}

// `files.mounts.find(path)` — the index of the mount caching `path`
// (relative, canonical, or through the VFS's own path forms), or -1.
static DEF_METHOD(files_mounts_method_find) {
    const char *path = argv[0].s;
    char resolved[VFS_PATH_MAX];
    const vfs_backend_t *be = NULL;
    void *bctx = NULL;
    const char *tail = NULL;
    if (vfs_resolve(path, resolved, sizeof(resolved), &be, &bctx, &tail) == 0)
        path = resolved;
    return val_int(image_vfs_serial_for_path(path));
}

static const member_t files_mounts_verbs[] = {
    {.kind = M_METHOD,
     .name = "find",
     .examples = EXAMPLES("files.mounts.find \"/opfs/images/hd/system.img\""),
     .doc = "Index of the mount caching an image path, or -1",
     .method = {.args = files_path_arg, .nargs = 1, .result = V_INT, .fn = files_mounts_method_find}},
};

static const collection_desc_t files_mounts = {
    .entry = &files_mount_class,
    .by_index = {.get = files_mounts_get, .next = files_mounts_next},
    .name = "mounts",
    .doc = "Cached disk-image mounts, by mount serial",
    .entries_doc = "Cached image mounts, by mount serial",
    .verbs = files_mounts_verbs,
    .n_verbs = sizeof(files_mounts_verbs) / sizeof(files_mounts_verbs[0]),
};

// The image entries go with the machine.
static void files_images_teardown(void) {
    object_cache_clear(&g_images);
}

// files.images: the storage view of cfg->images, under the process singleton
// `files`, installed with every machine.
static void files_images_install(struct config *cfg) {
    struct object *images = root_attach_stub(g_files_object, object_collection_new(&files_images, cfg, "images"));
    if (!images)
        return;
    object_set_label(images, "Images");
    object_set_order(images, 10);
    object_cache_set_parent(&g_images, images);
}

// `files` is a process singleton created at shell init: the file methods,
// the mounts collection and the archive child live as long as the process.
// `files.images` is the per-machine part, attached by root_install.
// === files.cache: the chunk cache's budgets and counters ======================
//
// Decoded chunks of compressed images and archive members (the chunk cache)
// live in memory up to one budget and spill to scratch files up to another
// (0: unbounded).  Lowering a budget frees what is over it at once; a
// spilled chunk is only ever a faster way to fetch it again.

static uint64_t cache_mib(uint64_t bytes) {
    return bytes >> 20;
}

static DEF_GETTER(cache_attr_memory_mb) {
    size_t mem = 0;
    gs_chunk_cache_budgets(gs_chunk_cache_default(), &mem, NULL);
    return val_uint(8, cache_mib(mem));
}

static DEF_SETTER(cache_attr_memory_mb_set) {
    if (in.u < 1 || in.u > 1u << 20)
        return val_err("files.cache.memory_mb: %llu out of range (1..1048576)", (unsigned long long)in.u);
    uint64_t spill = 0;
    gs_chunk_cache_budgets(gs_chunk_cache_default(), NULL, &spill);
    gs_chunk_cache_set_budgets(gs_chunk_cache_default(), (size_t)(in.u << 20), spill);
    return val_none();
}

static DEF_GETTER(cache_attr_spill_mb) {
    uint64_t spill = 0;
    gs_chunk_cache_budgets(gs_chunk_cache_default(), NULL, &spill);
    return val_uint(8, cache_mib(spill));
}

static DEF_SETTER(cache_attr_spill_mb_set) {
    if (in.u > 1u << 24)
        return val_err("files.cache.spill_mb: %llu out of range (0..16777216)", (unsigned long long)in.u);
    size_t mem = 0;
    gs_chunk_cache_budgets(gs_chunk_cache_default(), &mem, NULL);
    gs_chunk_cache_set_budgets(gs_chunk_cache_default(), mem, in.u << 20);
    return val_none();
}

static DEF_GETTER(cache_attr_image_mb) {
    size_t mem = 0;
    gs_chunk_cache_budgets(gs_chunk_cache_images(), &mem, NULL);
    return val_uint(8, cache_mib(mem));
}

static DEF_SETTER(cache_attr_image_mb_set) {
    if (in.u < 1 || in.u > 1u << 16)
        return val_err("files.cache.image_mb: %llu out of range (1..65536)", (unsigned long long)in.u);
    gs_chunk_cache_set_budgets(gs_chunk_cache_images(), (size_t)(in.u << 20), 0);
    return val_none();
}

// One counter of gs_chunk_cache_stats, picked by the member's name.
static DEF_GETTER(cache_attr_stat) {
    gs_chunk_cache_stats_t st;
    const char *n = m->name;
    bool image = strncmp(n, "image_", 6) == 0;
    gs_chunk_cache_stats(image ? gs_chunk_cache_images() : gs_chunk_cache_default(), &st);
    if (image)
        n += 6;
    uint64_t v = strcmp(n, "memory_bytes") == 0  ? st.mem_bytes
                 : strcmp(n, "spill_bytes") == 0 ? st.spill_bytes
                 : strcmp(n, "hits") == 0        ? st.hits + st.spill_hits
                 : strcmp(n, "misses") == 0      ? st.misses
                                                 : st.evictions;
    return val_uint(8, v);
}

#define CACHE_STAT(nm, what)                                                                                           \
    {                                                                                                                  \
        .kind = M_ATTR, .name = nm, .doc = what, .attr = {                                                             \
            .type = V_UINT,                                                                                            \
            .get = cache_attr_stat,                                                                                    \
            .set = NULL,                                                                                               \
            .presentation_flags = VAL_VOLATILE                                                                         \
        }                                                                                                              \
    }

static const member_t files_cache_members[] = {
    {.kind = M_ATTR,
     .name = "memory_mb",
     .doc = "Memory the chunk cache may hold, in MiB (decoded chunks of compressed images and archive members)",
     .attr = {.type = V_UINT, .get = cache_attr_memory_mb, .set = cache_attr_memory_mb_set}},
    {.kind = M_ATTR,
     .name = "spill_mb",
     .doc = "Scratch space evicted chunks may spill to, in MiB; 0 is unbounded",
     .attr = {.type = V_UINT, .get = cache_attr_spill_mb, .set = cache_attr_spill_mb_set}  },
    {.kind = M_ATTR,
     .name = "image_mb",
     .doc = "Memory, in MiB, for decoded chunks of compressed disk images (UDIF, NDIF); never spilled",
     .attr = {.type = V_UINT, .get = cache_attr_image_mb, .set = cache_attr_image_mb_set}  },
    CACHE_STAT("memory_bytes", "Bytes of chunks held in memory now"),
    CACHE_STAT("spill_bytes", "Bytes of chunks in spill files now"),
    CACHE_STAT("hits", "Reads served without decoding again (memory or spill)"),
    CACHE_STAT("misses", "Reads that needed a chunk fetched"),
    CACHE_STAT("evictions", "Chunks pushed out of memory"),
    CACHE_STAT("image_memory_bytes", "Bytes of decoded disk-image chunks held now"),
    CACHE_STAT("image_hits", "Disk-image reads served without decoding again"),
    CACHE_STAT("image_misses", "Disk-image reads that needed a chunk decoded"),
    CACHE_STAT("image_evictions", "Decoded disk-image chunks dropped for room"),
};

static const class_desc_t files_cache_class = {
    .name = "cache",
    .members = files_cache_members,
    .n_members = sizeof(files_cache_members) / sizeof(files_cache_members[0]),
    .doc = "The chunk cache that decoded image and archive data is served from: its budgets and counters",
};

void files_init(void) {
    if (g_files_object)
        return;
    g_files_object = object_new(&files_class, NULL, "files");
    if (!g_files_object)
        return;
    object_set_label(g_files_object, "Files");
    object_set_order(g_files_object, 30);
    object_attach(object_root(), g_files_object);
    struct object *mounts = object_collection_new(&files_mounts, NULL, "mounts");
    if (mounts) {
        object_set_label(mounts, "Mounts");
        object_set_order(mounts, 20);
        object_attach(g_files_object, mounts);
        object_cache_set_parent(&g_mounts, mounts);
    }
    archive_init(g_files_object);
    struct object *cache = object_new(&files_cache_class, NULL, "cache");
    if (cache) {
        object_set_label(cache, "Cache");
        object_set_order(cache, 40);
        object_attach(g_files_object, cache);
    }
    root_register_install(files_images_install, files_images_teardown);
    vfs_init(); // namespace formats, and the VFS as the path opener
}
