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
#include "resource_fork.h"

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

// ---- Resource-fork LRU cache ---------------------------------------------
// Parsed resource maps are held here so repeated reads through the
// synthetic /rsrc/<TYPE>/<id> tree don't re-parse the fork each time.
// Capacity is intentionally small — typical workflows touch one app at a
// time, occasionally a handful, so eight slots cover the common cases
// without holding entire fork buffers around forever.  The cache is
// invalidated when the parent mount is destroyed; image_vfs is otherwise
// read-only, so no other invalidation hooks are needed today.

#define RSRC_CACHE_CAPACITY 8

typedef struct rsrc_cache_entry {
    const image_mount_t *mount; // identity (NULL = slot empty)
    uint32_t hfs_cnid; // file CNID within the volume
    uint8_t *fork_buf; // owned: the read fork bytes
    size_t fork_len;
    rfork_t *parsed; // owned: parsed map index
    uint64_t lru_tick; // monotonic last-touch counter
} rsrc_cache_entry_t;

static rsrc_cache_entry_t g_rsrc_cache[RSRC_CACHE_CAPACITY];
static uint64_t g_rsrc_lru_counter;

// Drop one cache entry.  Safe on an empty slot.
static void rsrc_cache_evict(rsrc_cache_entry_t *e) {
    if (!e || !e->mount)
        return;
    rfork_free(e->parsed);
    free(e->fork_buf);
    memset(e, 0, sizeof(*e));
}

// Drop every entry associated with `m`.  Called from mount_destroy().
static void rsrc_cache_drop_for_mount(const image_mount_t *m) {
    for (size_t i = 0; i < RSRC_CACHE_CAPACITY; i++) {
        if (g_rsrc_cache[i].mount == m)
            rsrc_cache_evict(&g_rsrc_cache[i]);
    }
}

// Find an existing cache entry for (mount, cnid).  Bumps the LRU tick on
// hit.  Returns NULL on miss.
static rsrc_cache_entry_t *rsrc_cache_find(const image_mount_t *m, uint32_t cnid) {
    for (size_t i = 0; i < RSRC_CACHE_CAPACITY; i++) {
        if (g_rsrc_cache[i].mount == m && g_rsrc_cache[i].hfs_cnid == cnid) {
            g_rsrc_cache[i].lru_tick = ++g_rsrc_lru_counter;
            return &g_rsrc_cache[i];
        }
    }
    return NULL;
}

// Pick a slot to use for a new entry: prefer empty, otherwise evict the LRU.
static rsrc_cache_entry_t *rsrc_cache_pick(void) {
    rsrc_cache_entry_t *victim = &g_rsrc_cache[0];
    for (size_t i = 0; i < RSRC_CACHE_CAPACITY; i++) {
        if (!g_rsrc_cache[i].mount)
            return &g_rsrc_cache[i];
        if (g_rsrc_cache[i].lru_tick < victim->lru_tick)
            victim = &g_rsrc_cache[i];
    }
    rsrc_cache_evict(victim);
    return victim;
}

// Read+parse the resource fork for (mount, hfs, dirent.rsrc_fork) and
// return the cache entry.  cnid is used as the cache key.  Returns NULL on
// any failure (read error, OOM, corrupt fork).
static rsrc_cache_entry_t *rsrc_cache_acquire(image_mount_t *m, hfs_volume_t *hfs, const hfs_dirent_t *d) {
    if (!d || d->rsrc_fork.logical_size == 0)
        return NULL;
    rsrc_cache_entry_t *e = rsrc_cache_find(m, d->cnid);
    if (e)
        return e;
    size_t flen = (size_t)d->rsrc_fork.logical_size;
    uint8_t *buf = malloc(flen);
    if (!buf)
        return NULL;
    size_t got = 0;
    if (hfs_read_fork(hfs, &d->rsrc_fork, 0, buf, flen, &got) != 0 || got != flen) {
        free(buf);
        return NULL;
    }
    const char *errmsg = NULL;
    rfork_t *rf = rfork_parse(buf, flen, &errmsg);
    if (!rf) {
        free(buf);
        return NULL;
    }
    e = rsrc_cache_pick();
    e->mount = m;
    e->hfs_cnid = d->cnid;
    e->fork_buf = buf;
    e->fork_len = flen;
    e->parsed = rf;
    e->lru_tick = ++g_rsrc_lru_counter;
    return e;
}

// ---- Helpers --------------------------------------------------------------

// Resolve `path` through realpath() so relative and symlinked inputs map
// onto the canonical form the mount table keys on.  Falls back to the
// input path on failure so callers still get consistent lookup semantics
// when realpath can't resolve (e.g. the file was just deleted).
static void canonicalise(const char *path, char *out, size_t cap) {
    // Pass NULL to let realpath() allocate the buffer itself so we don't
    // silently truncate against PATH_MAX on hosts where the kernel's path
    // limit is higher than the build-time PATH_MAX. Fall back to the input
    // on failure (e.g. file just deleted) so callers still get consistent
    // keying.
    char *resolved = realpath(path, NULL);
    if (resolved) {
        snprintf(out, cap, "%s", resolved);
        free(resolved);
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
    rsrc_cache_drop_for_mount(m);
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

    // Propagate the actual errno from image_open_readonly rather than
    // collapsing every failure (EACCES, EISDIR, EIO, ENOENT) to ENOENT.
    errno = 0;
    image_t *img = image_open_readonly(host_path);
    if (!img)
        return -(errno ? errno : ENOENT);

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

    // Skip the alloc entirely for an empty APM. `calloc(0, ...)` is
    // implementation-defined (some libc return NULL, some a 1-byte sentinel);
    // bypassing the call keeps the cleanup paths uniform.
    if (m->n_partitions > 0) {
        m->parts_fs = calloc(m->n_partitions, sizeof(partition_fs_t));
        if (!m->parts_fs) {
            mount_destroy(m);
            return -ENOMEM;
        }
    } else {
        m->parts_fs = NULL;
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
    // Canonicalise so a notify with `/foo/../bar/disk.img` matches the mount
    // keyed on `/bar/disk.img`.
    char canon[PATH_MAX];
    canonicalise(host_path, canon, sizeof(canon));
    image_mount_t *m = find_mount_by_path(canon);
    if (m)
        m->conflicted = true;
}

void image_vfs_notify_detached(const char *host_path) {
    if (!host_path)
        return;
    char canon[PATH_MAX];
    canonicalise(host_path, canon, sizeof(canon));
    image_mount_t *m = find_mount_by_path(canon);
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
    errno = 0;
    long idx = strtol(nstr, &end, 10);
    // Reject: parse failure, trailing junk, non-positive, ERANGE overflow,
    // and values that wouldn't fit in `partition_idx` (uint32_t).
    if (errno == ERANGE || !end || *end != '\0' || idx <= 0 || (unsigned long)idx > UINT32_MAX)
        return -ENOENT;
    out->partition_idx = (uint32_t)idx;

    // Remaining components are in-partition path parts.
    while ((tok = strtok_r(NULL, "/", &saveptr)) != NULL) {
        if (out->n_components >= IMAGE_VFS_MAX_COMPONENTS)
            return -ENAMETOOLONG;
        out->components[out->n_components++] = tok;
    }
    // Fork-suffix ("rsrc"/"finf") handling lives in img_stat / img_open, not
    // here — whether the trailing component is a fork or part of the filename
    // can't be decided without resolving the prior path component.
    return 0;
}

// ---- Synthetic resource-tree path classification --------------------------
//
// Classifies the *suffix* of an in-image HFS path.  Given the full split
// component list, we look for the first occurrence of "rsrc" or "finf" and,
// if found, treat the components after it as either a fork suffix or a
// walk into the synthetic /rsrc/<TYPE>/<id>[.info] tree.  Callers retry
// with a literal HFS interpretation when this lookup misses, mirroring
// the pre-existing fork-suffix retry path.

typedef enum {
    SYNTH_NONE = 0, // No synthetic suffix; treat whole path literally.
    SYNTH_FINF, // <file>/finf — 32-byte Finder info blob (existing).
    SYNTH_RSRC_DIR, // <file>/rsrc — directory enumerating resource types.
    SYNTH_RSRC_RAW, // <file>/rsrc/_raw — raw fork bytes (previously /rsrc).
    SYNTH_RSRC_TYPE_DIR, // <file>/rsrc/<TYPE> — directory of IDs.
    SYNTH_RSRC_DATA, // <file>/rsrc/<TYPE>/<id> — resource bytes.
    SYNTH_RSRC_INFO, // <file>/rsrc/<TYPE>/<id>.info — JSON sidecar.
} synth_kind_t;

// Classify trailing components.  `n_components` is the full count; on
// success `*file_core_count` is the number of leading components that
// make up the HFS file path, `*type_out` (4 bytes) is the resource type
// for type-scoped kinds, and `*id_out` is the resource ID for resource-
// scoped kinds.  Returns SYNTH_NONE if the path looks literal.
static synth_kind_t classify_synth(const char *const *components, size_t n_components, size_t *file_core_count,
                                   uint8_t type_out[4], int16_t *id_out) {
    if (file_core_count)
        *file_core_count = n_components;
    if (n_components == 0)
        return SYNTH_NONE;

    // Search left-to-right for "rsrc" or "finf"; the first hit anchors the
    // synthetic split.  An HFS filename that literally equals "rsrc" or
    // "finf" would match here too, but the caller retries with the full
    // literal path on miss so the literal interpretation still wins when
    // the synthetic one fails.
    size_t anchor = n_components;
    bool is_finf = false;
    for (size_t i = 0; i < n_components; i++) {
        const char *c = components[i];
        if (strcmp(c, "rsrc") == 0) {
            anchor = i;
            is_finf = false;
            break;
        }
        if (strcmp(c, "finf") == 0) {
            anchor = i;
            is_finf = true;
            break;
        }
    }
    if (anchor == n_components)
        return SYNTH_NONE;
    if (file_core_count)
        *file_core_count = anchor;
    size_t after = n_components - anchor - 1;

    if (is_finf)
        return (after == 0) ? SYNTH_FINF : SYNTH_NONE;

    if (after == 0)
        return SYNTH_RSRC_DIR;
    if (after == 1) {
        const char *next = components[anchor + 1];
        if (strcmp(next, "_raw") == 0)
            return SYNTH_RSRC_RAW;
        if (type_out && rfork_type_from_path(next, type_out) == 0)
            return SYNTH_RSRC_TYPE_DIR;
        return SYNTH_NONE;
    }
    if (after == 2) {
        const char *type_str = components[anchor + 1];
        const char *id_str = components[anchor + 2];
        if (!type_out || rfork_type_from_path(type_str, type_out) != 0)
            return SYNTH_NONE;
        size_t id_len = strlen(id_str);
        const char *info_suffix = ".info";
        const size_t info_suffix_len = 5;
        if (id_len > info_suffix_len && strcmp(id_str + id_len - info_suffix_len, info_suffix) == 0) {
            // Strip the ".info" suffix and parse the remainder as an ID.
            char id_only[32];
            if (id_len - info_suffix_len >= sizeof(id_only))
                return SYNTH_NONE;
            memcpy(id_only, id_str, id_len - info_suffix_len);
            id_only[id_len - info_suffix_len] = '\0';
            if (!id_out || rfork_id_from_path(id_only, id_out) != 0)
                return SYNTH_NONE;
            return SYNTH_RSRC_INFO;
        }
        if (!id_out || rfork_id_from_path(id_str, id_out) != 0)
            return SYNTH_NONE;
        return SYNTH_RSRC_DATA;
    }
    return SYNTH_NONE;
}

// ---- Backend method implementations --------------------------------------

// Dir handle: partition-list enumerator at the image root, an HFS or UFS
// directory iterator, or one of the two synthetic-resource directory kinds
// (DIR_RSRC_ROOT for /rsrc, DIR_RSRC_TYPE for /rsrc/<TYPE>).  The resource
// kinds borrow an rfork_t* from the LRU cache; the cache outlives the dir
// handle because the parent mount holds a refcount while the dir is open.
struct vfs_dir {
    enum {
        DIR_PART_LIST,
        DIR_HFS,
        DIR_UFS,
        DIR_RSRC_ROOT,
        DIR_RSRC_TYPE,
    } kind;
    image_mount_t *mount;
    // partition list
    uint32_t next_partition;
    // HFS directory
    hfs_dir_iter_t *hfs_iter;
    // UFS directory
    ufs_dir_iter_t *ufs_iter;
    // Synthetic resource tree (DIR_RSRC_ROOT / DIR_RSRC_TYPE)
    const rfork_t *rfork; // borrowed (cache entry stays live via mount refcount)
    uint8_t rsrc_type[4]; // DIR_RSRC_TYPE only
    size_t rsrc_next_idx; // next type idx (root) or next resource idx (type)
    // Two-emission state machines so a single readdir call can stream both
    // the resource entry and the matching .info sidecar without losing
    // its place.
    bool rsrc_emit_info_next; // DIR_RSRC_TYPE: emit .info after the .bin
    bool rsrc_emit_raw_next; // DIR_RSRC_ROOT: emit _raw entry at the end
    // Cached "size" hint for the upcoming .info emission so we don't have
    // to re-look-up the resource on the follow-up call.
    int16_t rsrc_pending_id;
    size_t rsrc_pending_info_size;
};

// HFS finder info is 16 bytes per fork + 16 bytes extended = 32 bytes total.
#define HFS_FINDER_INFO_SIZE 32

// File handle: data/resource fork, synthetic Finder-info blob, UFS inode,
// or one of the new synthetic-resource leaf kinds.  FILE_RSRC_DATA borrows
// a slice of the cached fork buffer; FILE_RSRC_INFO precomputes the JSON
// once and reads out of an inline buffer.
struct vfs_file {
    image_mount_t *mount;
    enum {
        FILE_HFS_FORK,
        FILE_FINDER_INFO,
        FILE_UFS,
        FILE_RSRC_DATA,
        FILE_RSRC_INFO,
    } kind;
    hfs_volume_t *hfs;
    hfs_fork_t fork; // used for FILE_HFS_FORK
    uint8_t finder_info[HFS_FINDER_INFO_SIZE];
    ufs_volume_t *ufs; // used for FILE_UFS
    uint32_t ufs_ino;
    // FILE_RSRC_DATA: pointer into the cached fork buffer.
    const uint8_t *rsrc_bytes;
    size_t rsrc_size;
    // FILE_RSRC_INFO: precomputed JSON.  512 bytes accommodates a maximally
    // long resource name (255) plus the attrs list and brackets.
    char rsrc_info_buf[512];
    size_t rsrc_info_len;
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

    // HFS lookup path: classify any synthetic suffix first.  classify_synth
    // returns SYNTH_NONE for literal paths and lets the file_core_count
    // tell us how many leading components belong to the HFS file.
    uint8_t syn_type[4] = {0};
    int16_t syn_id = 0;
    size_t core = 0;
    synth_kind_t synth = classify_synth(ip.components, ip.n_components, &core, syn_type, &syn_id);

    if (p->fs_kind != APM_FS_HFS)
        return -ENOTDIR;

    hfs_volume_t *hfs = get_partition_hfs(m, ip.partition_idx);
    if (!hfs)
        return -EIO;
    hfs_dirent_t d = {0};
    if (core == 0 && synth != SYNTH_NONE)
        return -ENOENT; // synthetic suffix anchored at the partition root makes no sense
    if (ip.n_components == 0) {
        out->mode = VFS_MODE_DIR;
        return 0;
    }
    rc = hfs_lookup(hfs, ip.components, core ? core : ip.n_components, &d);
    if (rc < 0) {
        // Synthetic lookup missed (filename literally contains "rsrc" or
        // "finf"); retry the whole thing as a literal HFS path.
        if (synth != SYNTH_NONE) {
            rc = hfs_lookup(hfs, ip.components, ip.n_components, &d);
            if (rc == 0)
                synth = SYNTH_NONE;
        }
        if (rc < 0)
            return rc;
    }

    if (synth != SYNTH_NONE) {
        // All synthetic kinds require a file (forks live on files).
        if (d.is_dir)
            return -ENOENT;
        if (synth == SYNTH_FINF) {
            out->mode = VFS_MODE_FILE;
            out->size = HFS_FINDER_INFO_SIZE;
            return 0;
        }
        if (synth == SYNTH_RSRC_RAW) {
            out->mode = VFS_MODE_FILE;
            out->size = d.rsrc_fork.logical_size;
            return 0;
        }
        // /rsrc directory entries require a non-empty fork to enumerate.
        if (d.rsrc_fork.logical_size == 0)
            return -ENOENT;
        if (synth == SYNTH_RSRC_DIR) {
            out->mode = VFS_MODE_DIR;
            return 0;
        }
        // /rsrc/<TYPE> and deeper need the parsed map.
        rsrc_cache_entry_t *e = rsrc_cache_acquire(m, hfs, &d);
        if (!e)
            return -EIO;
        size_t n_res = rfork_num_resources(e->parsed, syn_type);
        if (synth == SYNTH_RSRC_TYPE_DIR) {
            if (n_res == 0)
                return -ENOENT;
            out->mode = VFS_MODE_DIR;
            return 0;
        }
        // Resource bytes or .info sidecar.
        const uint8_t *bytes = NULL;
        size_t sz = 0;
        const char *name = NULL;
        uint8_t attrs = 0;
        if (rfork_lookup(e->parsed, syn_type, syn_id, &bytes, &sz, &name, &attrs) < 0)
            return -ENOENT;
        if (synth == SYNTH_RSRC_DATA) {
            out->mode = VFS_MODE_FILE;
            out->size = sz;
            return 0;
        }
        // SYNTH_RSRC_INFO: format the JSON to a scratch buffer to take its
        // length.  Identical to what img_open will later do — duplicating
        // 512 bytes of stack work on stat is fine; the caller can avoid it
        // entirely by reading the file directly.
        char tmp[512];
        int w = rfork_info_format(name, attrs, sz, tmp, sizeof(tmp));
        if (w < 0)
            return -EIO;
        out->mode = VFS_MODE_FILE;
        out->size = (uint64_t)w;
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
    // Classify any synthetic suffix and try the synthetic interpretation
    // first.  Synthetic directories we care about are /rsrc (root) and
    // /rsrc/<TYPE>.  Anything else (including the literal HFS case) flows
    // through hfs_opendir_cnid below.
    uint8_t syn_type[4] = {0};
    int16_t syn_id = 0;
    size_t core = 0;
    synth_kind_t synth = classify_synth(ip.components, ip.n_components, &core, syn_type, &syn_id);
    (void)syn_id; // unused at opendir; per-resource leaves are files not dirs

    if (synth == SYNTH_RSRC_DIR || synth == SYNTH_RSRC_TYPE_DIR) {
        if (core == 0) {
            free(d);
            return -ENOENT;
        }
        hfs_dirent_t de = {0};
        int lr = hfs_lookup(hfs, ip.components, core, &de);
        if (lr < 0) {
            // Retry literal in case the path looked synthetic but isn't.
            lr = hfs_lookup(hfs, ip.components, ip.n_components, &de);
            if (lr == 0)
                synth = SYNTH_NONE;
            else {
                free(d);
                return lr;
            }
        }
        if (synth != SYNTH_NONE) {
            if (de.is_dir || de.rsrc_fork.logical_size == 0) {
                free(d);
                return -ENOENT;
            }
            rsrc_cache_entry_t *e = rsrc_cache_acquire(m, hfs, &de);
            if (!e) {
                free(d);
                return -EIO;
            }
            if (synth == SYNTH_RSRC_TYPE_DIR) {
                if (rfork_num_resources(e->parsed, syn_type) == 0) {
                    free(d);
                    return -ENOENT;
                }
                d->kind = DIR_RSRC_TYPE;
                memcpy(d->rsrc_type, syn_type, 4);
            } else {
                d->kind = DIR_RSRC_ROOT;
            }
            d->rfork = e->parsed;
            d->rsrc_next_idx = 0;
            m->refcount++;
            *out = d;
            return 0;
        }
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
        // `hfs_opendir_cnid` doesn't surface an errno; the failure mode is
        // either OOM or a corrupt catalog. `-EIO` is the closer match for the
        // latter (the most likely case once the volume has cached past init).
        return -EIO;
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
        // Include all partitions in the listing, including ones we can't
        // descend into (map, driver, free, patches). They stat as empty
        // read-only directories; opendir on them returns ENOTDIR. Users
        // who want the full layout can use `image partmap`.
        if (d->next_partition > total)
            return 0;
        int w = snprintf(out->name, sizeof(out->name), "partition%u", d->next_partition);
        if (w < 0 || (size_t)w >= sizeof(out->name))
            return -ENAMETOOLONG;
        out->st.mode = VFS_MODE_DIR;
        out->st.readonly = true;
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
    if (d->kind == DIR_RSRC_ROOT) {
        size_t n_types = rfork_num_types(d->rfork);
        if (d->rsrc_next_idx < n_types) {
            const uint8_t *cc = rfork_type_at(d->rfork, d->rsrc_next_idx);
            d->rsrc_next_idx++;
            char buf[16];
            rfork_type_to_path(cc, buf, sizeof(buf));
            int w = snprintf(out->name, sizeof(out->name), "%s", buf);
            if (w < 0 || (size_t)w >= sizeof(out->name))
                return -ENAMETOOLONG;
            out->st.mode = VFS_MODE_DIR;
            out->st.readonly = true;
            out->has_stat = true;
            return 1;
        }
        // Emit the "_raw" escape-hatch entry once after the types are
        // exhausted, then EOF.
        if (!d->rsrc_emit_raw_next) {
            d->rsrc_emit_raw_next = true;
            int w = snprintf(out->name, sizeof(out->name), "_raw");
            if (w < 0 || (size_t)w >= sizeof(out->name))
                return -ENAMETOOLONG;
            out->st.mode = VFS_MODE_FILE;
            out->st.readonly = true;
            out->has_stat = true;
            return 1;
        }
        return 0;
    }
    if (d->kind == DIR_RSRC_TYPE) {
        // Interleave each `<id>` entry with its `<id>.info` sidecar so
        // listings group naturally.  rsrc_emit_info_next flags the second
        // emission; rsrc_pending_* carries the id+info-size across calls.
        if (d->rsrc_emit_info_next) {
            char id_str[16];
            rfork_id_to_path(d->rsrc_pending_id, id_str, sizeof(id_str));
            int w = snprintf(out->name, sizeof(out->name), "%s.info", id_str);
            if (w < 0 || (size_t)w >= sizeof(out->name))
                return -ENAMETOOLONG;
            out->st.mode = VFS_MODE_FILE;
            out->st.readonly = true;
            out->st.size = d->rsrc_pending_info_size;
            out->has_stat = true;
            d->rsrc_emit_info_next = false;
            return 1;
        }
        size_t n_res = rfork_num_resources(d->rfork, d->rsrc_type);
        if (d->rsrc_next_idx >= n_res)
            return 0;
        int16_t id = rfork_id_at(d->rfork, d->rsrc_type, d->rsrc_next_idx);
        d->rsrc_next_idx++;
        const uint8_t *bytes = NULL;
        size_t sz = 0;
        const char *name = NULL;
        uint8_t attrs = 0;
        if (rfork_lookup(d->rfork, d->rsrc_type, id, &bytes, &sz, &name, &attrs) < 0)
            return -EIO; // shouldn't happen — id came from the same fork
        char id_str[16];
        rfork_id_to_path(id, id_str, sizeof(id_str));
        int w = snprintf(out->name, sizeof(out->name), "%s", id_str);
        if (w < 0 || (size_t)w >= sizeof(out->name))
            return -ENAMETOOLONG;
        out->st.mode = VFS_MODE_FILE;
        out->st.size = sz;
        out->st.readonly = true;
        out->has_stat = true;
        // Compute the sidecar size now so the next readdir call can emit
        // the .info entry without re-reading the fork.
        char tmp[512];
        int sidecar = rfork_info_format(name, attrs, sz, tmp, sizeof(tmp));
        d->rsrc_pending_info_size = (sidecar > 0) ? (size_t)sidecar : 0;
        d->rsrc_pending_id = id;
        d->rsrc_emit_info_next = true;
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

    // Classify synthetic suffix.  /rsrc on its own is a directory now and
    // returns -EISDIR below; /rsrc/_raw maps to the old raw-fork-bytes
    // behaviour; /rsrc/<TYPE> is a directory; the leaf cases produce
    // FILE_RSRC_DATA / FILE_RSRC_INFO handles.
    uint8_t syn_type[4] = {0};
    int16_t syn_id = 0;
    size_t core = 0;
    synth_kind_t synth = classify_synth(ip.components, ip.n_components, &core, syn_type, &syn_id);

    hfs_volume_t *hfs = get_partition_hfs(m, ip.partition_idx);
    if (!hfs)
        return -EIO;
    hfs_dirent_t d = {0};
    if (core == 0 && synth != SYNTH_NONE)
        return -ENOENT;
    rc = hfs_lookup(hfs, ip.components, core ? core : ip.n_components, &d);
    if (rc < 0) {
        if (synth != SYNTH_NONE) {
            rc = hfs_lookup(hfs, ip.components, ip.n_components, &d);
            if (rc == 0)
                synth = SYNTH_NONE;
        }
        if (rc < 0)
            return rc;
    }
    if (d.is_dir)
        return -EISDIR;

    if (synth == SYNTH_RSRC_DIR || synth == SYNTH_RSRC_TYPE_DIR)
        return -EISDIR;

    vfs_file_t *f = calloc(1, sizeof(*f));
    if (!f)
        return -ENOMEM;
    f->mount = m;
    f->hfs = hfs;

    if (synth == SYNTH_FINF) {
        f->kind = FILE_FINDER_INFO;
        memcpy(f->finder_info, d.finder_info, HFS_FINDER_INFO_SIZE);
    } else if (synth == SYNTH_RSRC_RAW) {
        f->kind = FILE_HFS_FORK;
        f->fork = d.rsrc_fork;
    } else if (synth == SYNTH_RSRC_DATA || synth == SYNTH_RSRC_INFO) {
        if (d.rsrc_fork.logical_size == 0) {
            free(f);
            return -ENOENT;
        }
        rsrc_cache_entry_t *e = rsrc_cache_acquire(m, hfs, &d);
        if (!e) {
            free(f);
            return -EIO;
        }
        const uint8_t *bytes = NULL;
        size_t sz = 0;
        const char *name = NULL;
        uint8_t attrs = 0;
        if (rfork_lookup(e->parsed, syn_type, syn_id, &bytes, &sz, &name, &attrs) < 0) {
            free(f);
            return -ENOENT;
        }
        if (synth == SYNTH_RSRC_DATA) {
            f->kind = FILE_RSRC_DATA;
            f->rsrc_bytes = bytes;
            f->rsrc_size = sz;
        } else {
            f->kind = FILE_RSRC_INFO;
            int w = rfork_info_format(name, attrs, sz, f->rsrc_info_buf, sizeof(f->rsrc_info_buf));
            if (w < 0) {
                free(f);
                return -EIO;
            }
            f->rsrc_info_len = (size_t)w;
        }
    } else {
        f->kind = FILE_HFS_FORK;
        f->fork = d.data_fork;
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
        if (off < HFS_FINDER_INFO_SIZE) {
            size_t avail = HFS_FINDER_INFO_SIZE - (size_t)off;
            size_t take = n < avail ? n : avail;
            memcpy(buf, f->finder_info + off, take);
            got = take;
        }
        if (nread)
            *nread = got;
        return 0;
    }
    if (f->kind == FILE_RSRC_DATA) {
        size_t got = 0;
        if (off < f->rsrc_size) {
            size_t avail = f->rsrc_size - (size_t)off;
            size_t take = n < avail ? n : avail;
            memcpy(buf, f->rsrc_bytes + off, take);
            got = take;
        }
        if (nread)
            *nread = got;
        return 0;
    }
    if (f->kind == FILE_RSRC_INFO) {
        size_t got = 0;
        if (off < f->rsrc_info_len) {
            size_t avail = f->rsrc_info_len - (size_t)off;
            size_t take = n < avail ? n : avail;
            memcpy(buf, f->rsrc_info_buf + off, take);
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
