// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ns_disk.c
// A disk image as a namespace: "partitionN" at the root (from the Apple
// Partition Map, or one synthetic partition for a bare volume), and inside a
// partition its filesystem.  Every filesystem sits behind one fs_ops_t, so
// the namespace methods do not branch on HFS vs UFS; what only HFS has --
// resource forks and Finder info -- comes out as the entry's rsrc size and
// Finder info and as GS_FORK_RSRC / GS_FORK_FINFO sources.  See namespace.h.

#include "namespace.h"

#include "image_apm.h"
#include "image_hfs.h"
#include "image_ufs.h"
#include "storage_util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DISK_MAX_COMPONENTS 64

// ============================================================================
// Filesystems
// ============================================================================

// A looked-up path or directory entry, filesystem-neutral.
typedef struct fs_entry {
    char name[256];
    bool is_dir;
    uint64_t size; // data bytes (files; 0 for directories)
    uint64_t id; // what opendir and read take: a CNID (HFS) or an inode (UFS)
    hfs_fork_t data_fork; // HFS only
    hfs_fork_t rsrc_fork; // HFS only
    uint8_t finder_info[GS_FINDER_INFO_SIZE]; // HFS only
    bool has_finder_info;
} fs_entry_t;

typedef struct fs_ops {
    const char *name; // "HFS" / "UFS"
    uint64_t root_id;
    void *(*open)(gs_source_t *src, uint64_t off, uint64_t size);
    void (*close)(void *vol);
    int (*lookup)(void *vol, const char *const *comp, size_t nc, fs_entry_t *out);
    void *(*opendir)(void *vol, uint64_t dir_id);
    int (*readdir)(void *iter, fs_entry_t *out); // 1 = entry, 0 = end, <0 = error
    void (*closedir)(void *iter);
    int (*read)(void *vol, const fs_entry_t *file, gs_fork_t fork, uint64_t off, void *buf, size_t n, size_t *nread);
} fs_ops_t;

static void hfs_entry(const hfs_dirent_t *d, fs_entry_t *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", d->name);
    out->is_dir = d->is_dir;
    out->size = d->is_dir ? 0 : d->data_fork.logical_size;
    out->id = d->cnid;
    out->data_fork = d->data_fork;
    out->rsrc_fork = d->rsrc_fork;
    if (!d->is_dir) {
        memcpy(out->finder_info, d->finder_info, GS_FINDER_INFO_SIZE);
        out->has_finder_info = true;
    }
}
static void *hfs_ops_open(gs_source_t *src, uint64_t off, uint64_t size) {
    return hfs_open_source(src, off, size);
}
static void hfs_ops_close(void *vol) {
    hfs_close(vol);
}
static int hfs_ops_lookup(void *vol, const char *const *comp, size_t nc, fs_entry_t *out) {
    hfs_dirent_t d = {0};
    int rc = hfs_lookup(vol, comp, nc, &d);
    if (rc == 0)
        hfs_entry(&d, out);
    return rc;
}
static void *hfs_ops_opendir(void *vol, uint64_t dir_id) {
    return hfs_opendir_cnid(vol, (uint32_t)dir_id);
}
static int hfs_ops_readdir(void *iter, fs_entry_t *out) {
    hfs_dirent_t d = {0};
    int rc = hfs_readdir_next(iter, &d);
    if (rc > 0)
        hfs_entry(&d, out);
    return rc;
}
static void hfs_ops_closedir(void *iter) {
    hfs_closedir_iter(iter);
}
static int hfs_ops_read(void *vol, const fs_entry_t *file, gs_fork_t fork, uint64_t off, void *buf, size_t n,
                        size_t *nread) {
    return hfs_read_fork(vol, fork == GS_FORK_RSRC ? &file->rsrc_fork : &file->data_fork, off, buf, n, nread);
}

static void ufs_entry(const ufs_dirent_t *d, fs_entry_t *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", d->name);
    out->is_dir = d->is_dir;
    out->size = d->is_dir ? 0 : d->size;
    out->id = d->ino;
}
static void *ufs_ops_open(gs_source_t *src, uint64_t off, uint64_t size) {
    return ufs_open_source(src, off, size);
}
static void ufs_ops_close(void *vol) {
    ufs_close(vol);
}
static int ufs_ops_lookup(void *vol, const char *const *comp, size_t nc, fs_entry_t *out) {
    ufs_dirent_t d = {0};
    int rc = ufs_lookup(vol, comp, nc, &d);
    if (rc == 0)
        ufs_entry(&d, out);
    return rc;
}
static void *ufs_ops_opendir(void *vol, uint64_t dir_id) {
    return ufs_opendir_ino(vol, (uint32_t)dir_id);
}
static int ufs_ops_readdir(void *iter, fs_entry_t *out) {
    ufs_dirent_t d = {0};
    int rc = ufs_readdir_next(iter, &d);
    if (rc > 0)
        ufs_entry(&d, out);
    return rc;
}
static void ufs_ops_closedir(void *iter) {
    ufs_closedir_iter(iter);
}
static int ufs_ops_read(void *vol, const fs_entry_t *file, gs_fork_t fork, uint64_t off, void *buf, size_t n,
                        size_t *nread) {
    if (fork != GS_FORK_DATA)
        return -ENOENT;
    return ufs_read_file(vol, (uint32_t)file->id, off, buf, n, nread);
}

static const fs_ops_t HFS_OPS = {"HFS",           HFS_ROOT_CNID,   hfs_ops_open,     hfs_ops_close, hfs_ops_lookup,
                                 hfs_ops_opendir, hfs_ops_readdir, hfs_ops_closedir, hfs_ops_read};
static const fs_ops_t UFS_OPS = {"UFS",           UFS_ROOT_INO,    ufs_ops_open,     ufs_ops_close, ufs_ops_lookup,
                                 ufs_ops_opendir, ufs_ops_readdir, ufs_ops_closedir, ufs_ops_read};

// The filesystem a partition of this kind holds, or NULL for one we do not
// read (a driver, the map itself, free space, ...).
static const fs_ops_t *fs_ops_for(enum apm_fs_kind kind) {
    switch (kind) {
    case APM_FS_HFS:
        return &HFS_OPS;
    case APM_FS_UFS:
        return &UFS_OPS;
    default:
        return NULL;
    }
}

// ============================================================================
// The disk
// ============================================================================

// One partition's filesystem, opened on first use.
typedef struct {
    const fs_ops_t *ops; // NULL: no filesystem we read
    bool attempted;
    void *vol;
} part_fs_t;

typedef struct {
    apm_table_t *apm; // NULL for a bare volume
    apm_partition_t synthetic; // the one partition of a bare volume
    uint32_t n_parts;
    part_fs_t *parts;
    const char *kind; // "APM", "HFS", "UFS"
} disk_ns_t;

static const apm_partition_t *disk_part(const disk_ns_t *d, uint32_t idx1) {
    if (idx1 == 0 || idx1 > d->n_parts)
        return NULL;
    return d->apm ? &d->apm->partitions[idx1 - 1] : &d->synthetic;
}

// The open filesystem of partition N (1-based), opened on first use.  NULL
// with *err: -ENOENT no such partition, -ENOTDIR no filesystem we read,
// -EIO one that would not open (not tried again).
static part_fs_t *disk_fs(gs_namespace_t *ns, uint32_t idx1, int *err) {
    disk_ns_t *d = ns->ctx;
    const apm_partition_t *p = disk_part(d, idx1);
    if (!p) {
        *err = -ENOENT;
        return NULL;
    }
    part_fs_t *pf = &d->parts[idx1 - 1];
    if (!pf->ops) {
        *err = -ENOTDIR;
        return NULL;
    }
    if (!pf->vol && !pf->attempted) {
        pf->attempted = true;
        pf->vol = pf->ops->open(ns->src, p->start_block * 512, p->size_blocks * 512);
    }
    *err = -EIO;
    return pf->vol ? pf : NULL;
}

// A path split into its partition and the path inside it.
typedef struct {
    uint32_t part; // 0: the disk's root
    const char *comps[DISK_MAX_COMPONENTS];
    int n; // components inside the partition
    char buf[1024];
} disk_path_t;

// Parse `path`.  0, or -ENOENT for a first component that is no partitionN.
static int disk_parse(const char *path, disk_path_t *dp) {
    const char *all[DISK_MAX_COMPONENTS + 1];
    int n = gs_ns_split(path, dp->buf, sizeof(dp->buf), all, DISK_MAX_COMPONENTS + 1);
    if (n < 0)
        return n;
    dp->part = 0;
    dp->n = 0;
    if (n == 0)
        return 0;
    // First component: "partitionN" (case-insensitive, N a positive number).
    if (strncasecmp(all[0], "partition", 9) != 0 || !all[0][9])
        return -ENOENT;
    char *end = NULL;
    unsigned long idx = strtoul(all[0] + 9, &end, 10);
    if (!end || *end || idx == 0 || idx > UINT32_MAX || all[0][9] == '-' || all[0][9] == '+')
        return -ENOENT;
    dp->part = (uint32_t)idx;
    for (int i = 1; i < n; i++)
        dp->comps[dp->n++] = all[i];
    return 0;
}

// Fill a namespace dirent from a filesystem entry.
static void to_dirent(const fs_entry_t *e, gs_dirent_t *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", e->name);
    out->is_dir = e->is_dir;
    out->data_size = e->size;
    out->rsrc_size = e->is_dir ? 0 : e->rsrc_fork.logical_size;
    out->has_finder_info = e->has_finder_info;
    if (e->has_finder_info) {
        out->type = RD_BE32(e->finder_info);
        out->creator = RD_BE32(e->finder_info + 4);
        out->finder_flags = RD_BE16(e->finder_info + 8);
    }
    out->tier = GS_TIER_RANDOM;
}

// A partition as a directory entry.
static void part_dirent(uint32_t idx1, gs_dirent_t *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "partition%u", idx1);
    out->is_dir = true;
}

static int disk_stat(gs_namespace_t *ns, const char *path, gs_dirent_t *out) {
    disk_ns_t *d = ns->ctx;
    disk_path_t dp;
    int rc = disk_parse(path, &dp);
    if (rc < 0)
        return rc;
    if (dp.part == 0) {
        memset(out, 0, sizeof(*out));
        out->is_dir = true;
        return 0;
    }
    if (!disk_part(d, dp.part))
        return -ENOENT;
    if (dp.n == 0) {
        part_dirent(dp.part, out);
        return 0;
    }
    part_fs_t *pf = disk_fs(ns, dp.part, &rc);
    if (!pf)
        return rc;
    fs_entry_t e;
    rc = pf->ops->lookup(pf->vol, dp.comps, (size_t)dp.n, &e);
    if (rc < 0)
        return rc;
    to_dirent(&e, out);
    return 0;
}

static int disk_list(gs_namespace_t *ns, const char *path, gs_dirent_t *out, int cap, int *count) {
    disk_ns_t *d = ns->ctx;
    disk_path_t dp;
    int rc = disk_parse(path, &dp);
    if (rc < 0)
        return rc;
    *count = 0;
    if (dp.part == 0) {
        // Every partition, including ones we cannot descend into (map,
        // driver, free space): they list as directories that do not open.
        for (uint32_t i = 1; i <= d->n_parts; i++) {
            if (*count < cap)
                part_dirent(i, &out[*count]);
            (*count)++;
        }
        return 0;
    }
    if (!disk_part(d, dp.part))
        return -ENOENT;
    part_fs_t *pf = disk_fs(ns, dp.part, &rc);
    if (!pf)
        return rc;
    uint64_t dir_id = pf->ops->root_id;
    if (dp.n > 0) {
        fs_entry_t e;
        rc = pf->ops->lookup(pf->vol, dp.comps, (size_t)dp.n, &e);
        if (rc < 0)
            return rc;
        if (!e.is_dir)
            return -ENOTDIR;
        dir_id = e.id;
    }
    void *it = pf->ops->opendir(pf->vol, dir_id);
    if (!it)
        return -EIO; // OOM or a corrupt directory
    fs_entry_t e;
    while ((rc = pf->ops->readdir(it, &e)) > 0) {
        if (*count < cap)
            to_dirent(&e, &out[*count]);
        (*count)++;
    }
    pf->ops->closedir(it);
    return rc < 0 ? rc : 0;
}

// ---- A file of the disk as a source ----

typedef struct {
    gs_namespace_t *ns; // retained: the volume lives in it
    part_fs_t *pf;
    fs_entry_t entry;
    gs_fork_t fork;
    uint64_t size;
    char *key;
} file_src_t;

static int64_t file_read(gs_source_t *s, uint64_t off, void *buf, size_t len) {
    file_src_t *f = s->ctx;
    if (off >= f->size)
        return 0;
    if (len > f->size - off)
        len = (size_t)(f->size - off);
    size_t got = 0;
    int rc = f->pf->ops->read(f->pf->vol, &f->entry, f->fork, off, buf, len, &got);
    return rc < 0 ? rc : (int64_t)got;
}

static uint64_t file_size(gs_source_t *s) {
    return ((file_src_t *)s->ctx)->size;
}

static const char *file_key(gs_source_t *s) {
    return ((file_src_t *)s->ctx)->key;
}

static gs_tier_t file_tier(gs_source_t *s) {
    return gs_source_tier(((file_src_t *)s->ctx)->ns->src); // extents: what the disk costs
}

static void file_close(gs_source_t *s) {
    file_src_t *f = s->ctx;
    if (!f)
        return;
    gs_namespace_release(f->ns);
    free(f->key);
    free(f);
}

static const gs_source_ops_t file_ops = {file_read, file_size, file_key, file_tier, file_close};

static gs_source_t *disk_open(gs_namespace_t *ns, const char *path, gs_fork_t fork, int *err) {
    disk_path_t dp;
    int rc = disk_parse(path, &dp);
    if (rc < 0 || dp.part == 0 || dp.n == 0) {
        *err = rc < 0 ? rc : -EISDIR;
        return NULL;
    }
    part_fs_t *pf = disk_fs(ns, dp.part, &rc);
    if (!pf) {
        *err = rc;
        return NULL;
    }
    fs_entry_t e;
    rc = pf->ops->lookup(pf->vol, dp.comps, (size_t)dp.n, &e);
    if (rc < 0 || e.is_dir) {
        *err = rc < 0 ? rc : -EISDIR;
        return NULL;
    }
    // The key: the disk's, and the path inside it.
    char *key = gs_str_printf("%s/%s%s", gs_source_key(ns->src), path[0] == '/' ? path + 1 : path,
                              fork == GS_FORK_RSRC    ? "/rsrc"
                              : fork == GS_FORK_FINFO ? "/finf"
                                                      : "");
    if (!key) {
        *err = -ENOMEM;
        return NULL;
    }
    if (fork == GS_FORK_FINFO) {
        if (!e.has_finder_info) {
            free(key);
            *err = -ENOENT;
            return NULL;
        }
        uint8_t *fi = malloc(GS_FINDER_INFO_SIZE);
        if (!fi) {
            free(key);
            *err = -ENOMEM;
            return NULL;
        }
        memcpy(fi, e.finder_info, GS_FINDER_INFO_SIZE);
        gs_source_t *s = gs_source_memory(fi, GS_FINDER_INFO_SIZE, true, key);
        free(key);
        *err = s ? 0 : -ENOMEM;
        return s;
    }
    if (fork == GS_FORK_RSRC && pf->ops != &HFS_OPS) {
        free(key);
        *err = -ENOENT; // only HFS files have resource forks
        return NULL;
    }
    file_src_t *f = calloc(1, sizeof(*f));
    if (!f) {
        free(key);
        *err = -ENOMEM;
        return NULL;
    }
    f->ns = gs_namespace_retain(ns);
    f->pf = pf;
    f->entry = e;
    f->fork = fork;
    f->size = fork == GS_FORK_RSRC ? e.rsrc_fork.logical_size : e.size;
    f->key = key;
    gs_source_t *s = peel_source_new(&file_ops, f, NULL);
    *err = s ? 0 : -ENOMEM;
    return s;
}

static void disk_close(gs_namespace_t *ns) {
    disk_ns_t *d = ns->ctx;
    if (!d)
        return;
    for (uint32_t i = 0; i < d->n_parts; i++)
        if (d->parts[i].vol)
            d->parts[i].ops->close(d->parts[i].vol);
    free(d->parts);
    image_apm_free(d->apm);
    free(d);
}

static const gs_namespace_ops_t disk_ops = {"disk", disk_list, disk_stat, disk_open, disk_close};

// A volume with no partition map is one synthetic "partition1" covering the
// whole disk.
static void set_synthetic(disk_ns_t *d, uint64_t size, const char *name, const char *type, enum apm_fs_kind kind) {
    memset(&d->synthetic, 0, sizeof(d->synthetic));
    d->synthetic.index = 1;
    d->synthetic.start_block = 0;
    d->synthetic.size_blocks = size / 512;
    snprintf(d->synthetic.name, sizeof(d->synthetic.name), "%s", name);
    snprintf(d->synthetic.type, sizeof(d->synthetic.type), "%s", type);
    d->synthetic.fs_kind = kind;
    d->n_parts = 1;
}

gs_namespace_t *gs_ns_open_disk(gs_source_t *src) {
    if (!src)
        return NULL;
    disk_ns_t *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    uint64_t size = gs_source_size(src);
    // The partition map first, then a bare HFS / HFS+ or UFS volume.
    d->apm = image_apm_parse_source(src, NULL);
    if (d->apm) {
        d->n_parts = d->apm->n_partitions;
        d->kind = "APM";
    } else {
        uint8_t mdb[512];
        uint16_t sig = 0;
        if (size >= 1024 + 512 && gs_source_read_exact(src, 1024, mdb, sizeof(mdb)) == 0)
            sig = RD_BE16(mdb);
        if (sig == HFS_SIG_BD || sig == HFS_SIG_HP || sig == HFS_SIG_HX) {
            set_synthetic(d, size, "HFS", "Apple_HFS", APM_FS_HFS);
            d->kind = "HFS";
        } else if (ufs_probe_source(src, 0, size)) {
            set_synthetic(d, size, "UFS", "Apple_UNIX_SVR2", APM_FS_UFS);
            d->kind = "UFS";
        } else {
            free(d);
            return NULL;
        }
    }
    if (d->n_parts) {
        d->parts = calloc(d->n_parts, sizeof(*d->parts));
        if (!d->parts) {
            image_apm_free(d->apm);
            free(d);
            return NULL;
        }
        for (uint32_t i = 0; i < d->n_parts; i++)
            d->parts[i].ops = fs_ops_for(disk_part(d, i + 1)->fs_kind);
    }
    gs_namespace_t *ns = gs_namespace_new(&disk_ops, d, src);
    if (!ns) {
        gs_namespace_t tmp = {.ctx = d};
        disk_close(&tmp);
    }
    return ns;
}

// The disk's display format ("APM", "HFS", "UFS"), or NULL when `ns` is no
// disk.
const char *gs_ns_disk_kind(gs_namespace_t *ns) {
    return (ns && ns->ops == &disk_ops) ? ((disk_ns_t *)ns->ctx)->kind : NULL;
}

gs_namespace_t *gs_ns_open_hfs(gs_source_t *src) {
    gs_namespace_t *ns = gs_ns_open_disk(src);
    if (ns && strcmp(gs_ns_disk_kind(ns), "HFS") != 0) {
        gs_namespace_close(ns);
        return NULL;
    }
    return ns;
}

gs_namespace_t *gs_ns_open_ufs(gs_source_t *src) {
    gs_namespace_t *ns = gs_ns_open_disk(src);
    if (ns && strcmp(gs_ns_disk_kind(ns), "UFS") != 0) {
        gs_namespace_close(ns);
        return NULL;
    }
    return ns;
}
