// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_vfs.c
// The mount table and the image/archive VFS backend.  A mount holds a
// namespace (namespace.h) opened on a byte source; the backend turns
// in-mount paths into namespace calls and adds the synthetic resource tree.
// See image_vfs.h.

#include "image_vfs.h"

#include "format_registry.h"
#include "image.h"
#include "namespace.h"
#include "resource_fork.h"
#include "storage_util.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// ---- Mount table ----------------------------------------------------------

// Mounts held at once.  An idle one (no open handle) is evicted, least
// recently used first, when a new mount needs its slot.
#define IMAGE_VFS_MAX_MOUNTS 16

// Components an in-mount path may have.
#define IMAGE_VFS_MAX_COMPONENTS 64

struct image_mount {
    bool in_use;
    char *key; // the source's key: the table's identity
    char *path; // the VFS path it was mounted under
    gs_namespace_t *ns;
    const char *format; // "APM", "HFS", "UFS", "zip", ... (static)
    uint32_t n_root; // entries at the root (partitions of a disk)
    uint32_t refcount; // open handles
    bool unmounting; // unmount requested while handles were live
    int serial; // never-reused mount serial (files.mounts index); valid while in_use
    uint64_t used; // LRU stamp
};

static image_mount_t g_mounts[IMAGE_VFS_MAX_MOUNTS];

// Next mount serial.  Slots are reused; serials are not, so the object
// model's stable-index contract holds for files.mounts[n].
static int g_next_serial = 0;
static uint64_t g_use_clock;

// Guards a slot's identity -- in_use, path, serial -- while a mount is
// created or destroyed (the I/O worker mounts too, through a copy out of an
// image) against the snapshot readers below.
static pthread_mutex_t g_mounts_mu = PTHREAD_MUTEX_INITIALIZER;

// A mount refuses service (-EBUSY) once an unmount is pending, and while an
// image containing its source is attached writable: guest writes land in
// that image's delta, which this read-only mount cannot see, so it would
// serve the stale base.  Asked at every use, so an attach or detach needs
// no notification.
static bool mount_busy(const image_mount_t *m) {
    return m->unmounting || image_key_is_open_writable(m->key);
}

// ---- Resource-fork LRU cache ---------------------------------------------
// Parsed resource maps are held here so repeated reads through the
// synthetic /rsrc/<TYPE>/<id> tree don't re-parse the fork each time.
// Capacity is intentionally small — typical workflows touch one app at a
// time, occasionally a handful, so eight slots cover the common cases
// without holding entire fork buffers around forever.  The cache is
// invalidated when the parent mount is destroyed; mounts are otherwise
// read-only, so no other invalidation hooks are needed.

#define RSRC_CACHE_CAPACITY 8

typedef struct rsrc_cache_entry {
    const image_mount_t *mount; // identity (NULL = slot empty)
    char *path; // the file's in-mount path
    uint8_t *fork_buf; // owned: the read fork bytes
    size_t fork_len;
    rfork_t *parsed; // owned: parsed map index
    uint64_t lru_tick; // monotonic last-touch counter
    int pins; // open handles borrowing fork_buf / parsed; never evicted while > 0
} rsrc_cache_entry_t;

static rsrc_cache_entry_t g_rsrc_cache[RSRC_CACHE_CAPACITY];
static uint64_t g_rsrc_lru_counter;

// Drop one cache entry.  Safe on an empty slot.
static void rsrc_cache_evict(rsrc_cache_entry_t *e) {
    if (!e || !e->mount)
        return;
    rfork_free(e->parsed);
    free(e->fork_buf);
    free(e->path);
    memset(e, 0, sizeof(*e));
}

// Drop every entry associated with `m`.  Called from mount_destroy().
static void rsrc_cache_drop_for_mount(const image_mount_t *m) {
    for (size_t i = 0; i < RSRC_CACHE_CAPACITY; i++) {
        if (g_rsrc_cache[i].mount == m)
            rsrc_cache_evict(&g_rsrc_cache[i]);
    }
}

// Find an existing cache entry for (mount, path).  Bumps the LRU tick on
// hit.  Returns NULL on miss.
static rsrc_cache_entry_t *rsrc_cache_find(const image_mount_t *m, const char *path) {
    for (size_t i = 0; i < RSRC_CACHE_CAPACITY; i++) {
        if (g_rsrc_cache[i].mount == m && strcmp(g_rsrc_cache[i].path, path) == 0) {
            g_rsrc_cache[i].lru_tick = ++g_rsrc_lru_counter;
            return &g_rsrc_cache[i];
        }
    }
    return NULL;
}

// Pick a slot to use for a new entry: prefer empty, otherwise evict the least
// recently used entry that nothing is borrowing.  Returns NULL if every entry
// is pinned.
//
// Open handles borrow from an entry -- a resource file reads straight out of
// fork_buf, a resource directory walks parsed -- so an entry a handle holds
// is never evicted; the ninth fork opened while eight are held is refused.
static rsrc_cache_entry_t *rsrc_cache_pick(void) {
    rsrc_cache_entry_t *victim = NULL;
    for (size_t i = 0; i < RSRC_CACHE_CAPACITY; i++) {
        rsrc_cache_entry_t *e = &g_rsrc_cache[i];
        if (!e->mount)
            return e;
        if (e->pins == 0 && (!victim || e->lru_tick < victim->lru_tick))
            victim = e;
    }
    if (victim)
        rsrc_cache_evict(victim);
    return victim;
}

static void rsrc_cache_pin(rsrc_cache_entry_t *e) {
    if (e)
        e->pins++;
}

static void rsrc_cache_unpin(rsrc_cache_entry_t *e) {
    if (e && e->pins > 0)
        e->pins--;
}

// Read and parse the resource fork of the file at `path` (whose dirent is
// `d`) and return the cache entry.  NULL on any failure (read error, OOM,
// corrupt fork, a fork too large to be one).
static rsrc_cache_entry_t *rsrc_cache_acquire(image_mount_t *m, const char *path, const gs_dirent_t *d) {
    if (!d || d->rsrc_size == 0)
        return NULL;
    rsrc_cache_entry_t *e = rsrc_cache_find(m, path);
    if (e)
        return e;
    // The catalog's size is only a claim: refuse one no resource fork can
    // have before allocating it.
    if (d->rsrc_size > RFORK_MAX_FORK_LEN)
        return NULL;
    int err = 0;
    gs_source_t *src = gs_ns_open(m->ns, path, GS_FORK_RSRC, &err);
    if (!src)
        return NULL;
    uint8_t *buf = NULL;
    size_t flen = 0;
    int rc = gs_source_read_all(src, RFORK_MAX_FORK_LEN, &buf, &flen);
    gs_source_release(src);
    if (rc != 0 || flen != d->rsrc_size) {
        free(buf);
        return NULL;
    }
    const char *errmsg = NULL;
    rfork_t *rf = rfork_parse(buf, flen, &errmsg);
    char *p = gs_strdup(path);
    if (!rf || !p) {
        rfork_free(rf);
        free(buf);
        free(p);
        return NULL;
    }
    e = rsrc_cache_pick();
    if (!e) { // every entry is borrowed by an open handle
        rfork_free(rf);
        free(buf);
        free(p);
        return NULL;
    }
    e->mount = m;
    e->path = p;
    e->fork_buf = buf;
    e->fork_len = flen;
    e->parsed = rf;
    e->lru_tick = ++g_rsrc_lru_counter;
    return e;
}

// ---- Helpers --------------------------------------------------------------

// Resolve `path` through realpath() so relative and symlinked inputs map
// onto one form.  Falls back to the input when realpath can't resolve it (a
// path through an image, or a file just deleted).
static void canonicalise(const char *path, char *out, size_t cap) {
    char *resolved = realpath(path, NULL);
    snprintf(out, cap, "%s", resolved ? resolved : path);
    free(resolved);
}

// The live mount whose key is `key`, or NULL.
static image_mount_t *find_mount_by_key(const char *key) {
    for (int i = 0; i < IMAGE_VFS_MAX_MOUNTS; i++)
        if (g_mounts[i].in_use && strcmp(g_mounts[i].key, key) == 0)
            return &g_mounts[i];
    return NULL;
}

// The live mount mounted under `path` (as given, or canonicalised), or NULL.
static image_mount_t *find_mount_by_path(const char *path) {
    for (int i = 0; i < IMAGE_VFS_MAX_MOUNTS; i++)
        if (g_mounts[i].in_use && strcmp(g_mounts[i].path, path) == 0)
            return &g_mounts[i];
    char canon[PATH_MAX];
    canonicalise(path, canon, sizeof(canon));
    if (strcmp(canon, path) != 0)
        for (int i = 0; i < IMAGE_VFS_MAX_MOUNTS; i++)
            if (g_mounts[i].in_use && strcmp(g_mounts[i].path, canon) == 0)
                return &g_mounts[i];
    return NULL;
}

// Tear down a mount without regard for refcount (callers must guard).
static void mount_destroy(image_mount_t *m) {
    rsrc_cache_drop_for_mount(m);
    gs_namespace_close(m->ns);
    pthread_mutex_lock(&g_mounts_mu);
    free(m->key);
    free(m->path);
    memset(m, 0, sizeof(*m));
    pthread_mutex_unlock(&g_mounts_mu);
}

// A free slot: an empty one, else the least recently used idle mount's.
static image_mount_t *find_free_slot(void) {
    image_mount_t *victim = NULL;
    for (int i = 0; i < IMAGE_VFS_MAX_MOUNTS; i++) {
        image_mount_t *m = &g_mounts[i];
        if (!m->in_use)
            return m;
        if (m->refcount == 0 && (!victim || m->used < victim->used))
            victim = m;
    }
    if (victim)
        mount_destroy(victim);
    return victim;
}

// ---- Mounting -------------------------------------------------------------

int image_vfs_acquire_mount_source(const char *path, gs_source_t *data, gs_source_t *rsrc, image_mount_t **out_mount) {
    if (!path || !data || !out_mount)
        return -EINVAL;
    const char *key = gs_source_key(data);
    image_mount_t *m = find_mount_by_key(key);
    if (m) {
        if (mount_busy(m))
            return -EBUSY;
        m->used = ++g_use_clock;
        *out_mount = m;
        return 0;
    }
    if (image_key_is_open_writable(key))
        return -EBUSY;
    // The same path mounted over a file that has since changed (its key
    // carries size and mtime): the old mount goes once nothing holds it.
    image_mount_t *stale = find_mount_by_path(path);
    if (stale && stale->refcount == 0 && !stale->unmounting)
        mount_destroy(stale);

    int err = 0;
    const char *fmt = NULL;
    gs_namespace_t *ns = gs_format_open_namespace(data, rsrc, &fmt, &err);
    if (!ns)
        return err ? err : -ENOTDIR;
    m = find_free_slot();
    if (!m) {
        gs_namespace_close(ns);
        return -ENOSPC;
    }
    char *k = gs_strdup(key), *p = gs_strdup(path);
    if (!k || !p) {
        free(k);
        free(p);
        gs_namespace_close(ns);
        return -ENOMEM;
    }
    // What listings call it: a disk's partitioning, else the format.
    const char *kind = gs_ns_disk_kind(ns);
    uint32_t n_root = 0;
    gs_dirent_t *root = NULL;
    int count = 0;
    if (gs_ns_list(ns, "", &root, &count) == 0)
        n_root = (uint32_t)count;
    free(root);
    pthread_mutex_lock(&g_mounts_mu);
    memset(m, 0, sizeof(*m));
    m->in_use = true;
    m->key = k;
    m->path = p;
    m->ns = ns;
    m->format = kind ? kind : (gs_ns_archive_format(ns) ? gs_ns_archive_format(ns) : fmt);
    m->n_root = n_root;
    m->serial = g_next_serial++;
    m->used = ++g_use_clock;
    pthread_mutex_unlock(&g_mounts_mu);
    *out_mount = m;
    return 0;
}

int image_vfs_acquire_mount(const char *host_path_in, image_mount_t **out_mount) {
    if (!host_path_in || !out_mount)
        return -EINVAL;
    char host_path[PATH_MAX];
    canonicalise(host_path_in, host_path, sizeof(host_path));
    int err = 0;
    gs_source_t *data = gs_source_host(host_path, &err);
    if (!data)
        return err ? err : -ENOENT;
    gs_source_t *rsrc = gs_source_open_host_path(host_path, GS_FORK_RSRC, NULL);
    int rc = image_vfs_acquire_mount_source(host_path, data, rsrc, out_mount);
    gs_source_release(data);
    gs_source_release(rsrc);
    return rc;
}

// Drop a handle's reference; the last one out completes a pending unmount.
static void mount_release(image_mount_t *m) {
    if (!m || m->refcount == 0)
        return;
    if (--m->refcount == 0 && m->unmounting)
        mount_destroy(m);
}

int image_vfs_unmount(const char *path) {
    image_mount_t *m = find_mount_by_path(path);
    if (!m)
        return -ENOENT;
    if (m->refcount > 0) {
        // Refuse new ops; the last handle to close tears it down.
        m->unmounting = true;
        return -EBUSY;
    }
    mount_destroy(m);
    return 0;
}

// The mount holding `serial`, or NULL.  Caller holds g_mounts_mu.
static image_mount_t *mount_by_serial_locked(int serial) {
    for (int i = 0; i < IMAGE_VFS_MAX_MOUNTS; i++)
        if (g_mounts[i].in_use && g_mounts[i].serial == serial)
            return &g_mounts[i];
    return NULL;
}

int image_vfs_next_serial(int prev) {
    int best = -1;
    pthread_mutex_lock(&g_mounts_mu);
    for (int i = 0; i < IMAGE_VFS_MAX_MOUNTS; i++) {
        const image_mount_t *m = &g_mounts[i];
        if (m->in_use && m->serial > prev && (best < 0 || m->serial < best))
            best = m->serial;
    }
    pthread_mutex_unlock(&g_mounts_mu);
    return best;
}

bool image_vfs_mount_info(int serial, image_vfs_mount_info_t *out) {
    pthread_mutex_lock(&g_mounts_mu);
    image_mount_t *m = mount_by_serial_locked(serial);
    char key[1024] = "";
    if (m && out) {
        out->serial = m->serial;
        snprintf(out->path, sizeof(out->path), "%s", m->path ? m->path : "");
        out->format = m->format;
        out->partitions = m->n_root;
        out->refcount = m->refcount;
        out->unmounting = m->unmounting;
        snprintf(key, sizeof(key), "%s", m->key);
    }
    pthread_mutex_unlock(&g_mounts_mu);
    if (!m)
        return false;
    // Asked outside the lock: it consults the image layer's open-file table.
    if (out)
        out->busy = out->unmounting || image_key_is_open_writable(key);
    return true;
}

int image_vfs_serial_for_path(const char *path) {
    if (!path)
        return -1;
    image_mount_t *m = find_mount_by_path(path);
    return m ? m->serial : -1;
}

void image_vfs_list(image_vfs_list_cb cb, void *user) {
    if (!cb)
        return;
    for (int i = 0; i < IMAGE_VFS_MAX_MOUNTS; i++) {
        image_mount_t *m = &g_mounts[i];
        if (!m->in_use)
            continue;
        cb(m->path, m->format, m->n_root, m->refcount, mount_busy(m), user);
    }
}

// ---- In-mount paths -------------------------------------------------------

// An in-mount path split into components.
typedef struct image_path {
    const char *components[IMAGE_VFS_MAX_COMPONENTS];
    size_t n_components;
    char buf[VFS_PATH_MAX]; // owns the storage for component strings
} image_path_t;

// Split `path`.  0, or -ENAMETOOLONG: refuse rather than truncate, since a
// truncated path is a different path and can name a real file the caller
// did not ask for.
static int parse_image_path(const char *path, image_path_t *out) {
    if (!path || !out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (strlen(path) >= sizeof(out->buf))
        return -ENAMETOOLONG;
    int n = gs_ns_split(path, out->buf, sizeof(out->buf), out->components, IMAGE_VFS_MAX_COMPONENTS);
    if (n < 0)
        return n;
    out->n_components = (size_t)n;
    return 0;
}

// Join the first `n` components into "a/b/c" (the namespace's path form).
static int join_path(const image_path_t *ip, size_t n, char *out, size_t cap) {
    size_t pos = 0;
    out[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        int w = snprintf(out + pos, cap - pos, "%s%s", i ? "/" : "", ip->components[i]);
        if (w < 0 || (size_t)w >= cap - pos)
            return -ENAMETOOLONG;
        pos += (size_t)w;
    }
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

// Dir handle: a namespace directory's entries (read at opendir), or one of
// the two synthetic-resource directory kinds (DIR_RSRC_ROOT for /rsrc,
// DIR_RSRC_TYPE for /rsrc/<TYPE>).  The resource kinds borrow an rfork_t*
// from the LRU cache, which the handle pins.
struct vfs_dir {
    enum {
        DIR_NS,
        DIR_RSRC_ROOT,
        DIR_RSRC_TYPE,
    } kind;
    image_mount_t *mount;
    // DIR_NS
    gs_dirent_t *entries;
    int n_entries, next_entry;
    // Synthetic resource tree (DIR_RSRC_ROOT / DIR_RSRC_TYPE)
    rfork_t *rfork; // borrowed from rsrc_entry, which this handle pins
    struct rsrc_cache_entry *rsrc_entry;
    uint8_t rsrc_type[4]; // DIR_RSRC_TYPE only
    size_t rsrc_next_idx; // next type idx (root) or next resource idx (type)
    // Two-emission state machines so a single readdir call can stream both
    // the resource entry and the matching .info sidecar without losing
    // its place.
    bool rsrc_emit_info_next; // DIR_RSRC_TYPE: emit .info after the .bin
    bool rsrc_emit_raw_next; // DIR_RSRC_ROOT: emit _raw entry at the end
    int16_t rsrc_pending_id;
    size_t rsrc_pending_info_size;
};

// File handle: a byte source (a file's data or resource fork, its Finder
// info), or one of the synthetic-resource leaf kinds.  FILE_RSRC_DATA
// borrows a slice of the cached fork buffer; FILE_RSRC_INFO precomputes the
// JSON once and reads out of an inline buffer.
struct vfs_file {
    image_mount_t *mount;
    enum {
        FILE_SOURCE,
        FILE_RSRC_DATA,
        FILE_RSRC_INFO,
    } kind;
    gs_source_t *src; // FILE_SOURCE
    // FILE_RSRC_DATA: pointer into the cached fork buffer of rsrc_entry,
    // which this handle pins until it closes.
    const uint8_t *rsrc_bytes;
    size_t rsrc_size;
    struct rsrc_cache_entry *rsrc_entry;
    // FILE_RSRC_INFO: precomputed JSON.  512 bytes accommodates a maximally
    // long resource name (255) plus the attrs list and brackets.
    char rsrc_info_buf[512];
    size_t rsrc_info_len;
};

// A dirent as a VFS stat.
static void to_stat(const gs_dirent_t *d, vfs_stat_t *out) {
    memset(out, 0, sizeof(*out));
    out->mode = d->is_dir ? VFS_MODE_DIR : VFS_MODE_FILE;
    out->size = d->is_dir ? 0 : d->data_size;
    out->mtime = d->mtime;
    out->readonly = true;
}

// The synthetic part of a path, resolved: the file it hangs off (`core`
// components) and its dirent.  Returns 1 when the path is not synthetic --
// or only looked it (the file does not exist, so it is a name that happens
// to contain "rsrc" or "finf"): the caller treats it as a plain path.
typedef struct {
    synth_kind_t kind;
    char core[VFS_PATH_MAX];
    gs_dirent_t file;
    uint8_t type[4];
    int16_t id;
} synth_t;

static int resolve_synthetic(image_mount_t *m, const image_path_t *ip, synth_t *sy) {
    size_t core = 0;
    sy->kind = classify_synth(ip->components, ip->n_components, &core, sy->type, &sy->id);
    if (sy->kind == SYNTH_NONE)
        return 1;
    if (core == 0)
        return -ENOENT; // a synthetic suffix at the mount root makes no sense
    if (join_path(ip, core, sy->core, sizeof(sy->core)) != 0)
        return -ENAMETOOLONG;
    if (gs_ns_stat(m->ns, sy->core, &sy->file) < 0)
        return 1; // a name that only looked synthetic
    if (sy->file.is_dir)
        return -ENOENT; // forks live on files
    return 0;
}

// The synthetic part of stat.  1 when the path is not synthetic.
static int stat_synthetic(image_mount_t *m, const image_path_t *ip, vfs_stat_t *out) {
    synth_t sy;
    int rc = resolve_synthetic(m, ip, &sy);
    if (rc != 0)
        return rc;
    if (sy.kind == SYNTH_FINF) {
        if (!sy.file.has_finder_info)
            return -ENOENT;
        out->mode = VFS_MODE_FILE;
        out->size = GS_FINDER_INFO_SIZE;
        return 0;
    }
    if (sy.kind == SYNTH_RSRC_RAW) {
        out->mode = VFS_MODE_FILE;
        out->size = sy.file.rsrc_size;
        return 0;
    }
    // /rsrc directory entries require a non-empty fork to enumerate.
    if (sy.file.rsrc_size == 0)
        return -ENOENT;
    if (sy.kind == SYNTH_RSRC_DIR) {
        out->mode = VFS_MODE_DIR;
        return 0;
    }
    // /rsrc/<TYPE> and deeper need the parsed map.
    rsrc_cache_entry_t *e = rsrc_cache_acquire(m, sy.core, &sy.file);
    if (!e)
        return -EIO;
    size_t n_res = rfork_num_resources(e->parsed, sy.type);
    if (sy.kind == SYNTH_RSRC_TYPE_DIR) {
        if (n_res == 0)
            return -ENOENT;
        out->mode = VFS_MODE_DIR;
        return 0;
    }
    const uint8_t *bytes = NULL;
    size_t sz = 0;
    const char *name = NULL;
    uint8_t attrs = 0;
    if (rfork_lookup(e->parsed, sy.type, sy.id, &bytes, &sz, &name, &attrs) < 0)
        return -ENOENT;
    if (sy.kind == SYNTH_RSRC_DATA) {
        out->mode = VFS_MODE_FILE;
        out->size = sz;
        return 0;
    }
    // SYNTH_RSRC_INFO: format the JSON to a scratch buffer to take its length.
    char tmp[512];
    int w = rfork_info_format(name, attrs, sz, tmp, sizeof(tmp));
    if (w < 0)
        return -EIO;
    out->mode = VFS_MODE_FILE;
    out->size = (uint64_t)w;
    return 0;
}

// stat: directories and files of the namespace, and the synthetic leaves.
static int img_stat(void *ctx, const char *path, vfs_stat_t *out) {
    image_mount_t *m = (image_mount_t *)ctx;
    if (!m || !out)
        return -EINVAL;
    if (mount_busy(m))
        return -EBUSY;
    memset(out, 0, sizeof(*out));
    out->readonly = true;

    image_path_t ip;
    int rc = parse_image_path(path, &ip);
    if (rc < 0)
        return rc;
    if (ip.n_components > 0) {
        rc = stat_synthetic(m, &ip, out);
        if (rc != 1)
            return rc;
    }
    char p[VFS_PATH_MAX];
    if (join_path(&ip, ip.n_components, p, sizeof(p)) != 0)
        return -ENAMETOOLONG;
    gs_dirent_t d;
    rc = gs_ns_stat(m->ns, p, &d);
    if (rc < 0)
        return rc;
    to_stat(&d, out);
    return 0;
}

// The synthetic part of opendir: /rsrc and /rsrc/<TYPE> list a file's
// resource fork.  1 when the path is not one of those.
static int opendir_synthetic(image_mount_t *m, const image_path_t *ip, vfs_dir_t *d) {
    synth_t sy;
    int rc = resolve_synthetic(m, ip, &sy);
    if (rc != 0)
        return rc;
    if (sy.kind != SYNTH_RSRC_DIR && sy.kind != SYNTH_RSRC_TYPE_DIR)
        return 1;
    if (sy.file.rsrc_size == 0)
        return -ENOENT;
    rsrc_cache_entry_t *e = rsrc_cache_acquire(m, sy.core, &sy.file);
    if (!e)
        return -EIO;
    if (sy.kind == SYNTH_RSRC_TYPE_DIR) {
        if (rfork_num_resources(e->parsed, sy.type) == 0)
            return -ENOENT;
        d->kind = DIR_RSRC_TYPE;
        memcpy(d->rsrc_type, sy.type, 4);
    } else {
        d->kind = DIR_RSRC_ROOT;
    }
    d->rfork = e->parsed;
    d->rsrc_entry = e;
    rsrc_cache_pin(e);
    return 0;
}

// opendir: a namespace directory, or a file's resource tree.
static int img_opendir(void *ctx, const char *path, vfs_dir_t **out) {
    image_mount_t *m = (image_mount_t *)ctx;
    if (!m || !out)
        return -EINVAL;
    if (mount_busy(m))
        return -EBUSY;

    image_path_t ip;
    int rc = parse_image_path(path, &ip);
    if (rc < 0)
        return rc;
    vfs_dir_t *d = calloc(1, sizeof(*d));
    if (!d)
        return -ENOMEM;
    d->mount = m;
    if (ip.n_components > 0) {
        rc = opendir_synthetic(m, &ip, d);
        if (rc < 0)
            goto fail;
        if (rc == 0)
            goto done;
    }
    char p[VFS_PATH_MAX];
    rc = join_path(&ip, ip.n_components, p, sizeof(p));
    if (rc == 0)
        rc = gs_ns_list(m->ns, p, &d->entries, &d->n_entries);
    if (rc < 0)
        goto fail;
    d->kind = DIR_NS;
done:
    m->refcount++;
    *out = d;
    return 0;
fail:
    free(d);
    return rc;
}

static int img_readdir(vfs_dir_t *d, vfs_dirent_t *out) {
    if (!d || !out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (d->kind == DIR_NS) {
        if (d->next_entry >= d->n_entries)
            return 0;
        const gs_dirent_t *e = &d->entries[d->next_entry++];
        snprintf(out->name, sizeof(out->name), "%s", e->name);
        to_stat(e, &out->st);
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
            snprintf(out->name, sizeof(out->name), "_raw");
            out->st.mode = VFS_MODE_FILE;
            out->st.readonly = true;
            out->has_stat = true;
            return 1;
        }
        return 0;
    }
    if (d->kind == DIR_RSRC_TYPE) {
        // Interleave each `<id>` entry with its `<id>.info` sidecar so
        // listings group naturally.
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
        snprintf(out->name, sizeof(out->name), "%s", id_str);
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
    rsrc_cache_unpin(d->rsrc_entry);
    free(d->entries);
    mount_release(d->mount);
    free(d);
}

// The synthetic part of open: Finder info (/finf), the raw resource fork
// (/rsrc/_raw), one resource or its .info sidecar.  1 when the path is not
// one of those.
static int open_synthetic(image_mount_t *m, const image_path_t *ip, vfs_file_t *f) {
    synth_t sy;
    int rc = resolve_synthetic(m, ip, &sy);
    if (rc != 0)
        return rc;
    if (sy.kind == SYNTH_RSRC_DIR || sy.kind == SYNTH_RSRC_TYPE_DIR)
        return -EISDIR;
    if (sy.kind == SYNTH_FINF || sy.kind == SYNTH_RSRC_RAW) {
        f->kind = FILE_SOURCE;
        f->src = gs_ns_open(m->ns, sy.core, sy.kind == SYNTH_FINF ? GS_FORK_FINFO : GS_FORK_RSRC, &rc);
        if (!f->src && sy.kind == SYNTH_RSRC_RAW && sy.file.rsrc_size == 0)
            f->src = gs_source_memory(NULL, 0, false, "empty"); // an empty fork reads as empty
        return f->src ? 0 : (rc ? rc : -ENOENT);
    }
    if (sy.file.rsrc_size == 0)
        return -ENOENT;
    rsrc_cache_entry_t *e = rsrc_cache_acquire(m, sy.core, &sy.file);
    if (!e)
        return -EIO;
    const uint8_t *bytes = NULL;
    size_t sz = 0;
    const char *name = NULL;
    uint8_t attrs = 0;
    if (rfork_lookup(e->parsed, sy.type, sy.id, &bytes, &sz, &name, &attrs) < 0)
        return -ENOENT;
    if (sy.kind == SYNTH_RSRC_DATA) {
        f->kind = FILE_RSRC_DATA;
        f->rsrc_bytes = bytes;
        f->rsrc_size = sz;
        f->rsrc_entry = e;
        rsrc_cache_pin(e);
        return 0;
    }
    f->kind = FILE_RSRC_INFO;
    int w = rfork_info_format(name, attrs, sz, f->rsrc_info_buf, sizeof(f->rsrc_info_buf));
    if (w < 0)
        return -EIO;
    f->rsrc_info_len = (size_t)w;
    return 0;
}

// open: a namespace file (its data fork), or a synthetic leaf.
static int img_open(void *ctx, const char *path, vfs_file_t **out) {
    image_mount_t *m = (image_mount_t *)ctx;
    if (!m || !out)
        return -EINVAL;
    if (mount_busy(m))
        return -EBUSY;

    image_path_t ip;
    int rc = parse_image_path(path, &ip);
    if (rc < 0)
        return rc;
    if (ip.n_components == 0)
        return -EISDIR;
    vfs_file_t *f = calloc(1, sizeof(*f));
    if (!f)
        return -ENOMEM;
    f->mount = m;
    rc = open_synthetic(m, &ip, f);
    if (rc == 1) {
        char p[VFS_PATH_MAX];
        rc = join_path(&ip, ip.n_components, p, sizeof(p));
        if (rc == 0) {
            f->kind = FILE_SOURCE;
            f->src = gs_ns_open(m->ns, p, GS_FORK_DATA, &rc);
            if (!f->src && rc == 0)
                rc = -ENOENT;
        }
    }
    if (rc != 0) {
        free(f);
        return rc;
    }
    m->refcount++;
    *out = f;
    return 0;
}

// Copy [off, off+n) of an in-memory buffer of `len` bytes.
static size_t copy_out(const void *src, size_t len, uint64_t off, void *buf, size_t n) {
    if (off >= len)
        return 0;
    size_t take = len - (size_t)off < n ? len - (size_t)off : n;
    memcpy(buf, (const uint8_t *)src + off, take);
    return take;
}

static int img_read(vfs_file_t *f, uint64_t off, void *buf, size_t n, size_t *nread) {
    if (!f || !buf)
        return -EINVAL;
    if (f->mount && mount_busy(f->mount))
        return -EBUSY;
    size_t got = 0;
    if (f->kind == FILE_RSRC_DATA) {
        got = copy_out(f->rsrc_bytes, f->rsrc_size, off, buf, n);
    } else if (f->kind == FILE_RSRC_INFO) {
        got = copy_out(f->rsrc_info_buf, f->rsrc_info_len, off, buf, n);
    } else {
        // As much as the source gives, up to n (short only at its end).
        while (got < n) {
            int64_t k = gs_source_read(f->src, off + got, (uint8_t *)buf + got, n - got);
            if (k < 0)
                return (int)k;
            if (k == 0)
                break;
            got += (size_t)k;
        }
    }
    if (nread)
        *nread = got;
    return 0;
}

static void img_close(vfs_file_t *f) {
    if (!f)
        return;
    rsrc_cache_unpin(f->rsrc_entry);
    gs_source_release(f->src);
    mount_release(f->mount);
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

// ---- Opening a file as a source --------------------------------------------

gs_source_t *image_vfs_open_source(image_mount_t *m, const char *tail, gs_fork_t fork, int *err) {
    int e = 0;
    if (!err)
        err = &e;
    if (!m || !tail) {
        *err = -EINVAL;
        return NULL;
    }
    if (mount_busy(m)) {
        *err = -EBUSY;
        return NULL;
    }
    image_path_t ip;
    *err = parse_image_path(tail, &ip);
    if (*err < 0)
        return NULL;
    if (ip.n_components == 0) {
        *err = -EISDIR;
        return NULL;
    }
    // The synthetic fork leaves name a fork of their file.
    synth_t sy;
    int rc = resolve_synthetic(m, &ip, &sy);
    if (rc < 0) {
        *err = rc;
        return NULL;
    }
    if (rc == 0 && fork == GS_FORK_DATA && (sy.kind == SYNTH_FINF || sy.kind == SYNTH_RSRC_RAW))
        return gs_ns_open(m->ns, sy.core, sy.kind == SYNTH_FINF ? GS_FORK_FINFO : GS_FORK_RSRC, err);
    char p[VFS_PATH_MAX];
    *err = join_path(&ip, ip.n_components, p, sizeof(p));
    if (*err < 0)
        return NULL;
    gs_source_t *s = gs_ns_open(m->ns, p, fork, err);
    if (!s && *err == 0)
        *err = -ENOENT;
    return s;
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
