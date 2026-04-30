// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_vfs.c
// Auto-mount cache plus the image-backed VFS backend.  A mount owns one
// image_t and its parsed APM (if any); per-partition filesystem state
// (HFS volumes, raw fallbacks) is built lazily on first access.
//
// The cache is keyed on the canonical absolute host path.  A fresh mount
// records the file's device+inode+mtime so a subsequent stat mismatch
// triggers reprobe; the coarse path key is still enough to detect most
// same-file aliasing (symlinks aside).

#include "image_vfs.h"

#include "image.h"
#include "image_apm.h"
#include "image_hfs.h"
#include "image_ufs.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

// ---- Mount table ----------------------------------------------------------

#define IMAGE_VFS_MAX_MOUNTS 8

// One partition's filesystem state.  fs_kind mirrors APM classification but
// we reuse apm_fs_kind so downstream code can switch on a single enum.
typedef struct partition_fs {
    bool attempted; // true once we've tried to open the FS
    enum apm_fs_kind kind; // copy of parent apm_partition_t.fs_kind
    hfs_volume_t *hfs; // non-NULL iff kind == APM_FS_HFS and open succeeded
    ufs_volume_t *ufs; // non-NULL iff kind == APM_FS_UFS and open succeeded
} partition_fs_t;

struct image_mount {
    bool in_use;
    char *host_path; // strdup'd canonical absolute path
    dev_t device;
    ino_t inode;
    uint32_t mtime;
    image_t *img;
    apm_table_t *apm; // may be NULL for a raw volume with no partition map
    // Synthetic partition list for volumes without APM (plain HFS floppy):
    // we expose a single entry "partition1" covering the whole image.
    bool synthetic_apm;
    apm_partition_t synthetic_part;
    partition_fs_t *parts_fs; // one per partition (n_partitions entries)
    uint32_t n_partitions;
    uint32_t refcount;
    bool conflicted; // set when hd attach acquires the same file
};

static image_mount_t g_mounts[IMAGE_VFS_MAX_MOUNTS];

// ---- Helpers --------------------------------------------------------------

// Resolve `path` through realpath() so relative and symlinked inputs map
// onto the canonical form the mount table keys on.  Falls back to the
// input path on failure so callers still get consistent lookup semantics
// when realpath can't resolve (e.g. the file was just deleted).
static void canonicalise(const char *path, char *out, size_t cap) {
    char tmp[PATH_MAX];
    if (realpath(path, tmp)) {
        snprintf(out, cap, "%s", tmp);
    } else {
        snprintf(out, cap, "%s", path);
    }
}

// Return a pointer to the slot currently holding `path`, or NULL.  Matches
// on exact-stored path first, then retries with a canonicalised form so
// callers passing a relative or symlinked path can still hit the cache.
static image_mount_t *find_mount_by_path(const char *path) {
    for (int i = 0; i < IMAGE_VFS_MAX_MOUNTS; i++) {
        if (g_mounts[i].in_use && g_mounts[i].host_path && strcmp(g_mounts[i].host_path, path) == 0)
            return &g_mounts[i];
    }
    char canon[PATH_MAX];
    canonicalise(path, canon, sizeof(canon));
    if (strcmp(canon, path) != 0) {
        for (int i = 0; i < IMAGE_VFS_MAX_MOUNTS; i++) {
            if (g_mounts[i].in_use && g_mounts[i].host_path && strcmp(g_mounts[i].host_path, canon) == 0)
                return &g_mounts[i];
        }
    }
    return NULL;
}

// Find a free mount slot, or NULL if the table is full.
static image_mount_t *find_free_slot(void) {
    for (int i = 0; i < IMAGE_VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].in_use)
            return &g_mounts[i];
    }
    return NULL;
}

// Number of partitions reported to callers, accounting for synthetic APM.
static uint32_t mount_partition_count(const image_mount_t *m) {
    if (m->apm)
        return m->apm->n_partitions;
    if (m->synthetic_apm)
        return 1;
    return 0;
}

// Return the Nth (1-based) partition descriptor, or NULL if out of range.
static const apm_partition_t *mount_get_partition(const image_mount_t *m, uint32_t index_1_based) {
    if (index_1_based == 0)
        return NULL;
    if (m->apm) {
        if (index_1_based > m->apm->n_partitions)
            return NULL;
        return &m->apm->partitions[index_1_based - 1];
    }
    if (m->synthetic_apm && index_1_based == 1)
        return &m->synthetic_part;
    return NULL;
}

// Release FS state (used on unmount / error paths).
static void release_fs_state(image_mount_t *m) {
    if (!m->parts_fs)
        return;
    for (uint32_t i = 0; i < m->n_partitions; i++) {
        if (m->parts_fs[i].hfs)
            hfs_close(m->parts_fs[i].hfs);
        if (m->parts_fs[i].ufs)
            ufs_close(m->parts_fs[i].ufs);
    }
    free(m->parts_fs);
    m->parts_fs = NULL;
}

// Tear down a mount without regard for refcount (callers must guard).
static void mount_destroy(image_mount_t *m) {
    release_fs_state(m);
    if (m->apm)
        image_apm_free(m->apm);
    if (m->img)
        image_close(m->img);
    free(m->host_path);
    memset(m, 0, sizeof(*m));
}

// ---- Mount open / probe ---------------------------------------------------

// Try to probe an image_t as HFS at offset 0 (bare floppy, no APM).  On
// success, populate the synthetic APM entry so the rest of the code can
// treat this uniformly.
static bool probe_bare_hfs(image_mount_t *m) {
    size_t img_size = disk_size(m->img);
    if (img_size < 1024 + 512)
        return false;
    // disk_read_data wants 512-aligned offset + length; read the full MDB
    // block and just inspect the signature.
    uint8_t mdb[512];
    if (disk_read_data(m->img, 1024, mdb, sizeof(mdb)) != sizeof(mdb))
        return false;
    if (mdb[0] != 0x42 || mdb[1] != 0x44) // "BD"
        return false;
    m->synthetic_apm = true;
    memset(&m->synthetic_part, 0, sizeof(m->synthetic_part));
    m->synthetic_part.index = 1;
    m->synthetic_part.start_block = 0;
    m->synthetic_part.size_blocks = img_size / 512;
    snprintf(m->synthetic_part.name, sizeof(m->synthetic_part.name), "HFS");
    snprintf(m->synthetic_part.type, sizeof(m->synthetic_part.type), "Apple_HFS");
    m->synthetic_part.fs_kind = APM_FS_HFS;
    return true;
}

// Fill in device/inode/mtime for a newly-opened mount, best-effort.
static void capture_identity(image_mount_t *m, const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        m->device = st.st_dev;
        m->inode = st.st_ino;
        m->mtime = (uint32_t)st.st_mtime;
    }
}

int image_vfs_acquire_mount(const char *host_path_in, image_mount_t **out_mount) {
    if (!host_path_in || !out_mount)
        return -EINVAL;

    char host_path[PATH_MAX];
    canonicalise(host_path_in, host_path, sizeof(host_path));

    // Existing cache entry?  Honour stale-identity detection: if the file
    // was swapped, evict.
    image_mount_t *m = find_mount_by_path(host_path);
    if (m) {
        struct stat st;
        if (stat(host_path, &st) == 0) {
            if (st.st_ino != m->inode || (uint32_t)st.st_mtime != m->mtime) {
                if (m->refcount == 0) {
                    mount_destroy(m);
                    m = NULL;
                }
                // If handles are live we still serve the cached mount
                // (dangling fd fallback); the file change will surface
                // when the handles close.
            }
        }
        if (m) {
            if (m->conflicted)
                return -EBUSY;
            *out_mount = m;
            return 0;
        }
    }

    m = find_free_slot();
    if (!m)
        return -ENOSPC;

    image_t *img = image_open_readonly(host_path);
    if (!img)
        return -ENOENT;

    memset(m, 0, sizeof(*m));
    m->in_use = true;
    m->host_path = strdup(host_path);
    if (!m->host_path) {
        image_close(img);
        memset(m, 0, sizeof(*m));
        return -ENOMEM;
    }
    m->img = img;
    capture_identity(m, host_path);

    // Probe APM first, then a bare HFS volume at offset 0.
    const char *errmsg = NULL;
    apm_table_t *apm = image_apm_parse(img, &errmsg);
    if (apm) {
        m->apm = apm;
        m->n_partitions = apm->n_partitions;
    } else if (probe_bare_hfs(m)) {
        m->n_partitions = 1;
    } else {
        mount_destroy(m);
        return -ENOTDIR;
    }

    m->parts_fs = calloc(m->n_partitions, sizeof(partition_fs_t));
    if (!m->parts_fs) {
        mount_destroy(m);
        return -ENOMEM;
    }
    for (uint32_t i = 0; i < m->n_partitions; i++) {
        const apm_partition_t *p = mount_get_partition(m, i + 1);
        m->parts_fs[i].kind = p ? p->fs_kind : APM_FS_UNKNOWN;
    }

    *out_mount = m;
    return 0;
}

int image_vfs_unmount(const char *host_path) {
    image_mount_t *m = find_mount_by_path(host_path);
    if (!m)
        return -ENOENT;
    if (m->refcount > 0) {
        // Mark conflicted so new ops fail, but don't tear down while
        // handles are live.
        m->conflicted = true;
        return -EBUSY;
    }
    mount_destroy(m);
    return 0;
}

void image_vfs_list(image_vfs_list_cb cb, void *user) {
    if (!cb)
        return;
    for (int i = 0; i < IMAGE_VFS_MAX_MOUNTS; i++) {
        image_mount_t *m = &g_mounts[i];
        if (!m->in_use)
            continue;
        const char *fmt = m->apm ? "APM" : (m->synthetic_apm ? "HFS" : "raw");
        // For synthetic single-partition mounts of a bare UFS volume we
        // report UFS instead of HFS so `image list` stays informative.
        if (m->synthetic_apm && m->synthetic_part.fs_kind == APM_FS_UFS)
            fmt = "UFS";
        cb(m->host_path, fmt, m->n_partitions, m->refcount, m->conflicted, user);
    }
}

void image_vfs_notify_attached(const char *host_path) {
    if (!host_path)
        return;
    image_mount_t *m = find_mount_by_path(host_path);
    if (m)
        m->conflicted = true;
}

void image_vfs_notify_detached(const char *host_path) {
    if (!host_path)
        return;
    image_mount_t *m = find_mount_by_path(host_path);
    if (m && m->refcount == 0) {
        // Simplest recovery: drop the cached mount entirely so the next
        // acquire re-opens cleanly against the now-released file.
        mount_destroy(m);
    } else if (m) {
        // Handles still live: keep the mount but clear the conflict.
        m->conflicted = false;
    }
}

void image_vfs_reset(void) {
    for (int i = 0; i < IMAGE_VFS_MAX_MOUNTS; i++) {
        image_mount_t *m = &g_mounts[i];
        if (m->in_use)
            mount_destroy(m);
    }
}

// ---- Partition-level FS state (lazy init) --------------------------------

// Lazy-open the HFS filesystem for partition N (1-based).  Returns NULL if
// this partition isn't HFS or the catalog was unreadable.
static hfs_volume_t *get_partition_hfs(image_mount_t *m, uint32_t idx_1based) {
    if (idx_1based == 0 || idx_1based > m->n_partitions)
        return NULL;
    partition_fs_t *pfs = &m->parts_fs[idx_1based - 1];
    if (pfs->kind != APM_FS_HFS)
        return NULL;
    if (pfs->hfs)
        return pfs->hfs;
    if (pfs->attempted)
        return NULL;
    pfs->attempted = true;

    const apm_partition_t *p = mount_get_partition(m, idx_1based);
    if (!p)
        return NULL;
    pfs->hfs = hfs_open(m->img, (uint64_t)p->start_block * 512, (uint64_t)p->size_blocks * 512);
    return pfs->hfs;
}

// Lazy-open the UFS filesystem for partition N (1-based).  Returns NULL if
// this partition isn't UFS or the superblock was unreadable.
static ufs_volume_t *get_partition_ufs(image_mount_t *m, uint32_t idx_1based) {
    if (idx_1based == 0 || idx_1based > m->n_partitions)
        return NULL;
    partition_fs_t *pfs = &m->parts_fs[idx_1based - 1];
    if (pfs->kind != APM_FS_UFS)
        return NULL;
    if (pfs->ufs)
        return pfs->ufs;
    if (pfs->attempted)
        return NULL;
    pfs->attempted = true;

    const apm_partition_t *p = mount_get_partition(m, idx_1based);
    if (!p)
        return NULL;
    pfs->ufs = ufs_open(m->img, (uint64_t)p->start_block * 512, (uint64_t)p->size_blocks * 512);
    return pfs->ufs;
}

// ---- In-image path parsing ------------------------------------------------

// Split an in-image path ("/partitionN/a/b") into a partition index and a
// list of UTF-8 components.  The caller must pre-allocate the components
// array; we return the component count via *nc.
//
// Returns:
//   0 on success.
//   1 if the path targets the mount root (list of partitions).
//   -ENOENT / -EINVAL on parse error.
#define IMAGE_VFS_MAX_COMPONENTS 32

typedef struct image_path {
    uint32_t partition_idx; // 0 for root, otherwise 1-based
    const char *components[IMAGE_VFS_MAX_COMPONENTS];
    size_t n_components;
    bool trailing_fork; // true if the last component is /rsrc or /finf
    char fork_name[8]; // "rsrc" or "finf"
    char buf[VFS_PATH_MAX]; // owns the storage for component strings
} image_path_t;

static int parse_image_path(const char *path, image_path_t *out) {
    if (!path || !out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    // Skip leading slashes.
    while (*path == '/')
        path++;
    if (!*path)
        return 1; // mount root — list partitions
    // Copy to mutable buffer and split on '/'.
    snprintf(out->buf, sizeof(out->buf), "%s", path);
    // Trim trailing slash.
    size_t blen = strlen(out->buf);
    while (blen > 0 && out->buf[blen - 1] == '/')
        out->buf[--blen] = '\0';
    if (blen == 0)
        return 1;

    char *saveptr = NULL;
    char *tok = strtok_r(out->buf, "/", &saveptr);
    if (!tok)
        return 1;
    // First component must be "partitionN".
    if (strncasecmp(tok, "partition", 9) != 0)
        return -ENOENT;
    const char *nstr = tok + 9;
    if (!*nstr)
        return -ENOENT;
    char *end = NULL;
    long idx = strtol(nstr, &end, 10);
    if (!end || *end != '\0' || idx <= 0)
        return -ENOENT;
    out->partition_idx = (uint32_t)idx;

    // Remaining components are in-partition path parts.
    while ((tok = strtok_r(NULL, "/", &saveptr)) != NULL) {
        if (out->n_components >= IMAGE_VFS_MAX_COMPONENTS)
            return -ENAMETOOLONG;
        out->components[out->n_components++] = tok;
    }
    // Detect fork suffix: the last component may be "rsrc" or "finf" and
    // only counts as a fork if the preceding path resolves to a file.  The
    // actual split is performed during lookup.
    return 0;
}

// ---- Backend method implementations --------------------------------------

// Dir handle: either a partition-list enumerator (at image root) or an HFS
// directory iterator.
struct vfs_dir {
    enum { DIR_PART_LIST, DIR_HFS, DIR_UFS } kind;
    image_mount_t *mount;
    // partition list
    uint32_t next_partition;
    // HFS directory
    hfs_dir_iter_t *hfs_iter;
    // UFS directory
    ufs_dir_iter_t *ufs_iter;
};

// File handle: data/resource fork, synthetic Finder-info blob, or UFS inode.
struct vfs_file {
    image_mount_t *mount;
    enum { FILE_HFS_FORK, FILE_FINDER_INFO, FILE_UFS } kind;
    hfs_volume_t *hfs;
    hfs_fork_t fork; // used for FILE_HFS_FORK
    uint8_t finder_info[32];
    ufs_volume_t *ufs; // used for FILE_UFS
    uint32_t ufs_ino;
};

// Forward declarations for backend functions.
static int img_stat(void *ctx, const char *path, vfs_stat_t *out);
static int img_opendir(void *ctx, const char *path, vfs_dir_t **out);
static int img_readdir(vfs_dir_t *d, vfs_dirent_t *out);
static void img_closedir(vfs_dir_t *d);
static int img_open(void *ctx, const char *path, vfs_file_t **out);
static int img_read(vfs_file_t *f, uint64_t off, void *buf, size_t n, size_t *nread);
static void img_close(vfs_file_t *f);
static int img_readonly(void *ctx, const char *path);
static int img_readonly2(void *ctx, const char *a, const char *b);

// stat: understand partition roots, HFS directories/files, fork sub-paths.
static int img_stat(void *ctx, const char *path, vfs_stat_t *out) {
    image_mount_t *m = (image_mount_t *)ctx;
    if (!m || !out)
        return -EINVAL;
    if (m->conflicted)
        return -EBUSY;
    memset(out, 0, sizeof(*out));
    out->readonly = true;

    image_path_t ip;
    int rc = parse_image_path(path, &ip);
    if (rc == 1) {
        // Mount root — treat as directory containing partitionN entries.
        out->mode = VFS_MODE_DIR;
        return 0;
    }
    if (rc < 0)
        return rc;

    const apm_partition_t *p = mount_get_partition(m, ip.partition_idx);
    if (!p)
        return -ENOENT;
    if (ip.n_components == 0) {
        out->mode = VFS_MODE_DIR;
        return 0;
    }

    // UFS partitions have no forks — dispatch before peeling any suffix.
    if (p->fs_kind == APM_FS_UFS) {
        ufs_volume_t *ufs = get_partition_ufs(m, ip.partition_idx);
        if (!ufs)
            return -EIO;
        ufs_dirent_t d = {0};
        int urc = ufs_lookup(ufs, ip.components, ip.n_components, &d);
        if (urc < 0)
            return urc;
        out->mode = d.is_dir ? VFS_MODE_DIR : VFS_MODE_FILE;
        out->size = d.is_dir ? 0 : d.size;
        return 0;
    }

    // HFS lookup path: peel off optional fork suffix first.
    bool rsrc = false, finf = false;
    size_t core = ip.n_components;
    const char *last = ip.components[core - 1];
    if (core >= 1 && (strcmp(last, "rsrc") == 0 || strcmp(last, "finf") == 0)) {
        rsrc = strcmp(last, "rsrc") == 0;
        finf = strcmp(last, "finf") == 0;
        core--;
    }

    if (p->fs_kind != APM_FS_HFS)
        return -ENOTDIR;

    hfs_volume_t *hfs = get_partition_hfs(m, ip.partition_idx);
    if (!hfs)
        return -EIO;
    hfs_dirent_t d = {0};
    if (core == 0) {
        // Fork suffix on the partition root makes no sense.
        if (rsrc || finf)
            return -ENOENT;
        out->mode = VFS_MODE_DIR;
        return 0;
    }
    rc = hfs_lookup(hfs, ip.components, core, &d);
    if (rc < 0) {
        // If the suffix lookup fails, maybe the last component was part of
        // the filename (not a fork).  Retry with the full component list.
        if ((rsrc || finf) && core < ip.n_components) {
            rc = hfs_lookup(hfs, ip.components, ip.n_components, &d);
            if (rc == 0) {
                rsrc = finf = false;
            }
        }
        if (rc < 0)
            return rc;
    }

    if (rsrc) {
        if (d.is_dir)
            return -ENOENT;
        out->mode = VFS_MODE_FILE;
        out->size = d.rsrc_fork.logical_size;
        return 0;
    }
    if (finf) {
        if (d.is_dir)
            return -ENOENT;
        out->mode = VFS_MODE_FILE;
        out->size = 32;
        return 0;
    }
    out->mode = d.is_dir ? VFS_MODE_DIR : VFS_MODE_FILE;
    out->size = d.is_dir ? 0 : d.data_fork.logical_size;
    return 0;
}

// opendir: enumerate partitions at root, or HFS directory children.
static int img_opendir(void *ctx, const char *path, vfs_dir_t **out) {
    image_mount_t *m = (image_mount_t *)ctx;
    if (!m || !out)
        return -EINVAL;
    if (m->conflicted)
        return -EBUSY;

    image_path_t ip;
    int rc = parse_image_path(path, &ip);
    if (rc < 0)
        return rc;

    vfs_dir_t *d = calloc(1, sizeof(*d));
    if (!d)
        return -ENOMEM;
    d->mount = m;

    if (rc == 1) {
        d->kind = DIR_PART_LIST;
        d->next_partition = 1;
        m->refcount++;
        *out = d;
        return 0;
    }

    const apm_partition_t *p = mount_get_partition(m, ip.partition_idx);
    if (!p) {
        free(d);
        return -ENOENT;
    }
    if (p->fs_kind == APM_FS_UFS) {
        ufs_volume_t *ufs = get_partition_ufs(m, ip.partition_idx);
        if (!ufs) {
            free(d);
            return -EIO;
        }
        uint32_t parent_ino = UFS_ROOT_INO;
        if (ip.n_components > 0) {
            ufs_dirent_t de = {0};
            int lr = ufs_lookup(ufs, ip.components, ip.n_components, &de);
            if (lr < 0) {
                free(d);
                return lr;
            }
            if (!de.is_dir) {
                free(d);
                return -ENOTDIR;
            }
            parent_ino = de.ino;
        }
        d->kind = DIR_UFS;
        d->ufs_iter = ufs_opendir_ino(ufs, parent_ino);
        if (!d->ufs_iter) {
            free(d);
            return -EIO;
        }
        m->refcount++;
        *out = d;
        return 0;
    }
    if (p->fs_kind != APM_FS_HFS) {
        free(d);
        return -ENOTDIR;
    }
    hfs_volume_t *hfs = get_partition_hfs(m, ip.partition_idx);
    if (!hfs) {
        free(d);
        return -EIO;
    }
    uint32_t parent_cnid = HFS_ROOT_CNID;
    if (ip.n_components > 0) {
        hfs_dirent_t de = {0};
        int lr = hfs_lookup(hfs, ip.components, ip.n_components, &de);
        if (lr < 0) {
            free(d);
            return lr;
        }
        if (!de.is_dir) {
            free(d);
            return -ENOTDIR;
        }
        parent_cnid = de.cnid;
    }
    d->kind = DIR_HFS;
    d->hfs_iter = hfs_opendir_cnid(hfs, parent_cnid);
    if (!d->hfs_iter) {
        free(d);
        return -ENOMEM;
    }
    m->refcount++;
    *out = d;
    return 0;
}

static int img_readdir(vfs_dir_t *d, vfs_dirent_t *out) {
    if (!d || !out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (d->kind == DIR_PART_LIST) {
        uint32_t total = mount_partition_count(d->mount);
        // Skip partitions with fs_kind we can't descend into (map, driver,
        // free, patches) — the proposal suggests hiding them from listings
        // for non-debug use.  For v1 we include them; they stat as empty
        // directories but opendir returns ENOTDIR.  Users can still see
        // the full layout via `image partmap`.
        if (d->next_partition > total)
            return 0;
        snprintf(out->name, sizeof(out->name), "partition%u", d->next_partition);
        out->st.mode = VFS_MODE_DIR;
        out->has_stat = true;
        d->next_partition++;
        return 1;
    }
    if (d->kind == DIR_HFS) {
        hfs_dirent_t hde = {0};
        int rc = hfs_readdir_next(d->hfs_iter, &hde);
        if (rc <= 0)
            return rc;
        snprintf(out->name, sizeof(out->name), "%s", hde.name);
        out->st.mode = hde.is_dir ? VFS_MODE_DIR : VFS_MODE_FILE;
        out->st.size = hde.is_dir ? 0 : hde.data_fork.logical_size;
        out->st.readonly = true;
        out->has_stat = true;
        return 1;
    }
    if (d->kind == DIR_UFS) {
        ufs_dirent_t ude = {0};
        int rc = ufs_readdir_next(d->ufs_iter, &ude);
        if (rc <= 0)
            return rc;
        snprintf(out->name, sizeof(out->name), "%s", ude.name);
        out->st.mode = ude.is_dir ? VFS_MODE_DIR : VFS_MODE_FILE;
        out->st.size = ude.is_dir ? 0 : ude.size;
        out->st.readonly = true;
        out->has_stat = true;
        return 1;
    }
    return -EINVAL;
}

static void img_closedir(vfs_dir_t *d) {
    if (!d)
        return;
    if (d->hfs_iter)
        hfs_closedir_iter(d->hfs_iter);
    if (d->ufs_iter)
        ufs_closedir_iter(d->ufs_iter);
    if (d->mount && d->mount->refcount > 0)
        d->mount->refcount--;
    free(d);
}

// open: data fork (bare path), resource fork (/rsrc), Finder info (/finf).
static int img_open(void *ctx, const char *path, vfs_file_t **out) {
    image_mount_t *m = (image_mount_t *)ctx;
    if (!m || !out)
        return -EINVAL;
    if (m->conflicted)
        return -EBUSY;

    image_path_t ip;
    int rc = parse_image_path(path, &ip);
    if (rc == 1)
        return -EISDIR;
    if (rc < 0)
        return rc;

    const apm_partition_t *p = mount_get_partition(m, ip.partition_idx);
    if (!p)
        return -ENOENT;
    if (ip.n_components == 0)
        return -EISDIR;

    if (p->fs_kind == APM_FS_UFS) {
        ufs_volume_t *ufs = get_partition_ufs(m, ip.partition_idx);
        if (!ufs)
            return -EIO;
        ufs_dirent_t d = {0};
        int urc = ufs_lookup(ufs, ip.components, ip.n_components, &d);
        if (urc < 0)
            return urc;
        if (d.is_dir)
            return -EISDIR;
        vfs_file_t *f = calloc(1, sizeof(*f));
        if (!f)
            return -ENOMEM;
        f->mount = m;
        f->kind = FILE_UFS;
        f->ufs = ufs;
        f->ufs_ino = d.ino;
        m->refcount++;
        *out = f;
        return 0;
    }

    if (p->fs_kind != APM_FS_HFS)
        return -ENOTDIR;

    bool rsrc = false, finf = false;
    size_t core = ip.n_components;
    const char *last = ip.components[core - 1];
    if (core >= 1 && (strcmp(last, "rsrc") == 0 || strcmp(last, "finf") == 0)) {
        rsrc = strcmp(last, "rsrc") == 0;
        finf = strcmp(last, "finf") == 0;
        core--;
    }

    hfs_volume_t *hfs = get_partition_hfs(m, ip.partition_idx);
    if (!hfs)
        return -EIO;
    hfs_dirent_t d = {0};
    if (core == 0)
        return -EISDIR;
    rc = hfs_lookup(hfs, ip.components, core, &d);
    if (rc < 0) {
        if ((rsrc || finf) && core < ip.n_components) {
            rc = hfs_lookup(hfs, ip.components, ip.n_components, &d);
            if (rc == 0)
                rsrc = finf = false;
        }
        if (rc < 0)
            return rc;
    }
    if (d.is_dir)
        return -EISDIR;

    vfs_file_t *f = calloc(1, sizeof(*f));
    if (!f)
        return -ENOMEM;
    f->mount = m;
    f->hfs = hfs;
    if (finf) {
        f->kind = FILE_FINDER_INFO;
        memcpy(f->finder_info, d.finder_info, 32);
    } else {
        f->kind = FILE_HFS_FORK;
        f->fork = rsrc ? d.rsrc_fork : d.data_fork;
    }
    m->refcount++;
    *out = f;
    return 0;
}

static int img_read(vfs_file_t *f, uint64_t off, void *buf, size_t n, size_t *nread) {
    if (!f || !buf)
        return -EINVAL;
    if (f->mount && f->mount->conflicted)
        return -EBUSY;
    if (f->kind == FILE_FINDER_INFO) {
        size_t got = 0;
        if (off < 32) {
            size_t avail = 32 - (size_t)off;
            size_t take = n < avail ? n : avail;
            memcpy(buf, f->finder_info + off, take);
            got = take;
        }
        if (nread)
            *nread = got;
        return 0;
    }
    if (f->kind == FILE_UFS)
        return ufs_read_file(f->ufs, f->ufs_ino, off, buf, n, nread);
    return hfs_read_fork(f->hfs, &f->fork, off, buf, n, nread);
}

static void img_close(vfs_file_t *f) {
    if (!f)
        return;
    if (f->mount && f->mount->refcount > 0)
        f->mount->refcount--;
    free(f);
}

// Writable operations — always refused for image paths.
static int img_readonly(void *ctx, const char *path) {
    (void)ctx;
    (void)path;
    return -EROFS;
}

static int img_readonly2(void *ctx, const char *a, const char *b) {
    (void)ctx;
    (void)a;
    (void)b;
    return -EROFS;
}

static const vfs_backend_t image_backend = {
    .scheme = "image",
    .stat = img_stat,
    .opendir = img_opendir,
    .readdir = img_readdir,
    .closedir = img_closedir,
    .open = img_open,
    .read = img_read,
    .close = img_close,
    .mkdir = img_readonly,
    .unlink = img_readonly,
    .rename = img_readonly2,
};

const vfs_backend_t *vfs_image_backend(void) {
    return &image_backend;
}
