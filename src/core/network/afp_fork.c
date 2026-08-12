// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// afp_fork.c
// Shared fork backing store, deny modes and byte-range locks (see afp_fork.h).

#include "afp_fork.h"

#include "afp_catalog.h"
#include "afp_meta.h"
#include "log.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

LOG_USE_CATEGORY_NAME("appletalk");

// One byte range held by a handle.
typedef struct {
    uint32_t start;
    uint32_t length; // 0 means "to end of fork"
} lock_range_t;

// The bytes of one (volume, path, fork), shared by every handle on it.
typedef struct afp_backing {
    struct afp_backing *next;
    uint16_t vol_id;
    bool is_resource;
    char host_path[PATH_MAX];
    char rel_path[AFP_CAT_MAX_PATH];
    FILE *f; // data fork: the host file; resource fork: the working file
    int refs;
    bool dirty; // resource fork has unpersisted writes
    bool writable;
} afp_backing_t;

// One open instance.
struct afp_fork {
    struct afp_fork *next;
    afp_backing_t *backing;
    uint16_t ref;
    uint16_t vol_id;
    uint16_t session_id;
    uint16_t access_mode;
    lock_range_t *locks;
    size_t n_locks, cap_locks;
};

static afp_backing_t *g_backings;
static afp_fork_t *g_forks;
static uint16_t g_next_ref = 0x0042;
static uint32_t g_open_count;

// A server can hold this many forks open at once; past it FPOpenFork answers
// afpTooManyFilesOpen instead of exhausting host descriptors.
#define AFP_MAX_OPEN_FORKS 512

// --- backing management ----------------------------------------------------

// Seed a resource fork's working file from the AppleDouble sidecar.
static FILE *open_resource_backing(const char *host_path) {
    FILE *work = tmpfile();
    if (!work)
        return NULL;
    afp_meta_copy_rsrc(host_path, work);
    fflush(work);
    rewind(work);
    return work;
}

// Write a resource fork's working file back into the sidecar, preserving the
// file's other metadata.  Streamed, so fork size never bounds memory.
static void persist_backing(afp_backing_t *b) {
    if (!b || !b->is_resource || !b->f || !b->dirty)
        return;
    fflush(b->f);
    if (fseek(b->f, 0, SEEK_END) != 0)
        return;
    long sz = ftell(b->f);
    if (sz < 0)
        sz = 0;
    rewind(b->f);
    afp_meta_t meta;
    afp_meta_load(b->host_path, &meta); // keep Finder Info, dates, attrs, comment
    if (afp_meta_store_stream(b->host_path, &meta, b->f, (size_t)sz) != 0)
        LOG(1, "AFP: failed to persist resource fork for '%s'", b->rel_path);
    else
        b->dirty = false;
}

// Find the backing for one (path, fork), or NULL.
static afp_backing_t *backing_find(const char *host_path, bool is_resource) {
    for (afp_backing_t *b = g_backings; b; b = b->next)
        if (b->is_resource == is_resource && strcmp(b->host_path, host_path) == 0)
            return b;
    return NULL;
}

// Acquire (creating if needed) the backing for one (path, fork).  `want_write`
// upgrades a read-only data-fork handle in place so a later writer shares the
// same object rather than opening a second one.
static afp_backing_t *backing_acquire(uint16_t vol_id, const char *host_path, const char *rel_path, bool is_resource,
                                      bool want_write) {
    afp_backing_t *b = backing_find(host_path, is_resource);
    if (b) {
        if (want_write && !b->writable && !b->is_resource) {
            FILE *up = fopen(host_path, "r+b");
            if (!up)
                return NULL;
            fclose(b->f);
            b->f = up;
            b->writable = true;
        }
        b->refs++;
        return b;
    }
    b = (afp_backing_t *)calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->vol_id = vol_id;
    b->is_resource = is_resource;
    snprintf(b->host_path, sizeof(b->host_path), "%s", host_path);
    snprintf(b->rel_path, sizeof(b->rel_path), "%s", rel_path ? rel_path : "");
    if (is_resource) {
        b->f = open_resource_backing(host_path);
        b->writable = true; // the working file is always writable
    } else {
        b->f = fopen(host_path, want_write ? "r+b" : "rb");
        b->writable = want_write;
        if (!b->f && want_write) {
            b->f = fopen(host_path, "rb"); // read-only host file; deny writes later
            b->writable = false;
        }
    }
    if (!b->f) {
        free(b);
        return NULL;
    }
    b->refs = 1;
    b->next = g_backings;
    g_backings = b;
    return b;
}

// Drop one reference, persisting and freeing at zero.
static void backing_release(afp_backing_t *b) {
    if (!b)
        return;
    if (--b->refs > 0)
        return;
    persist_backing(b);
    if (b->f)
        fclose(b->f);
    for (afp_backing_t **pp = &g_backings; *pp; pp = &(*pp)->next) {
        if (*pp == b) {
            *pp = b->next;
            break;
        }
    }
    free(b);
}

// --- deny-mode evaluation --------------------------------------------------

// Cumulative access and deny modes of every handle already on this backing.
static void cumulative_modes(const afp_backing_t *b, uint16_t *cam, uint16_t *cdm) {
    *cam = 0;
    *cdm = 0;
    for (afp_fork_t *fk = g_forks; fk; fk = fk->next) {
        if (fk->backing != b)
            continue;
        *cam |= (uint16_t)(fk->access_mode & (AFP_ACCESS_READ | AFP_ACCESS_WRITE));
        *cdm |= (uint16_t)(fk->access_mode & (AFP_DENY_READ | AFP_DENY_WRITE));
    }
}

// Map deny bits onto the access bits they forbid, so the two words can be
// intersected directly (Inside AppleTalk ch. 13 "Synchronization rules").
static uint16_t deny_to_access(uint16_t deny) {
    uint16_t a = 0;
    if (deny & AFP_DENY_READ)
        a |= AFP_ACCESS_READ;
    if (deny & AFP_DENY_WRITE)
        a |= AFP_ACCESS_WRITE;
    return a;
}

// --- open / close ----------------------------------------------------------

afp_fork_status_t afp_fork_open(uint16_t vol_id, uint16_t session_id, const char *host_path, const char *rel_path,
                                bool is_resource, uint16_t access_mode, afp_fork_t **out) {
    if (out)
        *out = NULL;
    if (!host_path)
        return AFP_FORK_IO_ERR;
    if (g_open_count >= AFP_MAX_OPEN_FORKS)
        return AFP_FORK_TOO_MANY;

    // An open with neither Read nor Write is "none" access, which AFP allows.
    uint16_t want_access = (uint16_t)(access_mode & (AFP_ACCESS_READ | AFP_ACCESS_WRITE));
    uint16_t want_deny = (uint16_t)(access_mode & (AFP_DENY_READ | AFP_DENY_WRITE));

    afp_backing_t *existing = backing_find(host_path, is_resource);
    if (existing) {
        uint16_t cam = 0, cdm = 0;
        cumulative_modes(existing, &cam, &cdm);
        // Fail if what we want to do is already denied, or if what we want to
        // deny is already being done.
        if (want_access & deny_to_access(cdm))
            return AFP_FORK_DENY_CONFLICT;
        if (deny_to_access(want_deny) & cam)
            return AFP_FORK_DENY_CONFLICT;
    }

    afp_backing_t *b = backing_acquire(vol_id, host_path, rel_path, is_resource, (want_access & AFP_ACCESS_WRITE) != 0);
    if (!b)
        return AFP_FORK_IO_ERR;
    if ((want_access & AFP_ACCESS_WRITE) && !b->writable) {
        backing_release(b);
        return AFP_FORK_ACCESS_DENIED; // host file is read-only
    }

    afp_fork_t *fk = (afp_fork_t *)calloc(1, sizeof(*fk));
    if (!fk) {
        backing_release(b);
        return AFP_FORK_IO_ERR;
    }
    fk->backing = b;
    fk->ref = g_next_ref++;
    if (fk->ref == 0)
        fk->ref = g_next_ref++;
    fk->vol_id = vol_id;
    fk->session_id = session_id;
    fk->access_mode = access_mode;
    fk->next = g_forks;
    g_forks = fk;
    g_open_count++;
    LOG(7, "AFP fork open: ref=0x%04X vol=%u %s mode=0x%04X path='%s'", fk->ref, vol_id, is_resource ? "rsrc" : "data",
        access_mode, rel_path ? rel_path : "");
    if (out)
        *out = fk;
    return AFP_FORK_OK;
}

afp_fork_t *afp_fork_find(uint16_t ref) {
    for (afp_fork_t *fk = g_forks; fk; fk = fk->next)
        if (fk->ref == ref)
            return fk;
    return NULL;
}

uint16_t afp_fork_ref(const afp_fork_t *fk) {
    return fk ? fk->ref : 0;
}
uint16_t afp_fork_vol_id(const afp_fork_t *fk) {
    return fk ? fk->vol_id : 0;
}
const char *afp_fork_rel_path(const afp_fork_t *fk) {
    return (fk && fk->backing) ? fk->backing->rel_path : "";
}
const char *afp_fork_host_path(const afp_fork_t *fk) {
    return (fk && fk->backing) ? fk->backing->host_path : "";
}
bool afp_fork_is_resource(const afp_fork_t *fk) {
    return fk && fk->backing && fk->backing->is_resource;
}
uint16_t afp_fork_access_mode(const afp_fork_t *fk) {
    return fk ? fk->access_mode : 0;
}

void afp_fork_close(afp_fork_t *fk) {
    if (!fk)
        return;
    LOG(7, "AFP fork close: ref=0x%04X path='%s'", fk->ref, afp_fork_rel_path(fk));
    for (afp_fork_t **pp = &g_forks; *pp; pp = &(*pp)->next) {
        if (*pp == fk) {
            *pp = fk->next;
            break;
        }
    }
    if (g_open_count)
        g_open_count--;
    backing_release(fk->backing); // persists on the last reference
    free(fk->locks);
    free(fk);
}

void afp_fork_close_volume(uint16_t vol_id) {
    afp_fork_t *fk = g_forks;
    while (fk) {
        afp_fork_t *next = fk->next;
        if (fk->vol_id == vol_id)
            afp_fork_close(fk);
        fk = next;
    }
}

void afp_fork_close_session(uint16_t session_id) {
    afp_fork_t *fk = g_forks;
    while (fk) {
        afp_fork_t *next = fk->next;
        if (fk->session_id == session_id)
            afp_fork_close(fk);
        fk = next;
    }
}

void afp_fork_shutdown(void) {
    while (g_forks)
        afp_fork_close(g_forks);
}

uint32_t afp_fork_count_volume(uint16_t vol_id) {
    uint32_t n = 0;
    for (afp_fork_t *fk = g_forks; fk; fk = fk->next)
        if (fk->vol_id == vol_id)
            n++;
    return n;
}

uint32_t afp_fork_count_session(uint16_t session_id) {
    uint32_t n = 0;
    for (afp_fork_t *fk = g_forks; fk; fk = fk->next)
        if (fk->session_id == session_id)
            n++;
    return n;
}

uint32_t afp_fork_count_total(void) {
    return g_open_count;
}

bool afp_fork_path_busy(const char *host_path) {
    if (!host_path)
        return false;
    for (afp_fork_t *fk = g_forks; fk; fk = fk->next)
        if (fk->backing && strcmp(fk->backing->host_path, host_path) == 0)
            return true;
    return false;
}

uint16_t afp_fork_open_attrs(const char *host_path) {
    uint16_t attrs = 0;
    if (!host_path)
        return 0;
    for (afp_fork_t *fk = g_forks; fk; fk = fk->next) {
        if (!fk->backing || strcmp(fk->backing->host_path, host_path) != 0)
            continue;
        attrs |= fk->backing->is_resource ? AFP_ATTR_RALREADYOPEN : AFP_ATTR_DALREADYOPEN;
    }
    return attrs;
}

void afp_fork_repoint(const char *old_host_path, const char *new_host_path, const char *new_rel_path) {
    if (!old_host_path || !new_host_path)
        return;
    for (afp_backing_t *b = g_backings; b; b = b->next) {
        if (strcmp(b->host_path, old_host_path) != 0)
            continue;
        snprintf(b->host_path, sizeof(b->host_path), "%s", new_host_path);
        if (new_rel_path)
            snprintf(b->rel_path, sizeof(b->rel_path), "%s", new_rel_path);
    }
}

// --- lock bookkeeping ------------------------------------------------------

// Absolute end of a range; UINT32_MAX for an open-ended lock.
static uint32_t range_end(const lock_range_t *r) {
    if (r->length == 0 || r->length == UINT32_MAX)
        return UINT32_MAX;
    uint64_t e = (uint64_t)r->start + r->length;
    return e > UINT32_MAX ? UINT32_MAX : (uint32_t)e;
}

static bool ranges_overlap(uint32_t a_start, uint32_t a_end, const lock_range_t *b) {
    uint32_t b_end = range_end(b);
    return a_start < b_end && b->start < a_end;
}

// True when a range is locked by some handle other than `self` on the same
// backing — the check FPRead/FPWrite make.
static bool foreign_lock_covers(const afp_fork_t *self, uint32_t start, uint32_t end) {
    for (afp_fork_t *fk = g_forks; fk; fk = fk->next) {
        if (fk == self || fk->backing != self->backing)
            continue;
        for (size_t i = 0; i < fk->n_locks; i++)
            if (ranges_overlap(start, end, &fk->locks[i]))
                return true;
    }
    return false;
}

afp_fork_status_t afp_fork_range_lock(afp_fork_t *fk, bool unlock, bool end_relative, int32_t start, uint32_t length,
                                      uint32_t *out_start) {
    if (!fk)
        return AFP_FORK_LOCK_ERR;
    uint32_t fork_len = afp_fork_length(fk);
    int64_t abs_start = end_relative ? (int64_t)fork_len + start : (int64_t)(uint32_t)start;
    if (abs_start < 0)
        return AFP_FORK_LOCK_ERR;
    if (abs_start > UINT32_MAX)
        return AFP_FORK_LOCK_ERR;
    lock_range_t want = {.start = (uint32_t)abs_start, .length = length};
    uint32_t want_end = range_end(&want);
    if (out_start)
        *out_start = want.start;

    if (unlock) {
        for (size_t i = 0; i < fk->n_locks; i++) {
            if (fk->locks[i].start == want.start && fk->locks[i].length == want.length) {
                fk->locks[i] = fk->locks[--fk->n_locks];
                return AFP_FORK_OK;
            }
        }
        return AFP_FORK_RANGE_NOT_LOCKED;
    }

    // Our own overlapping lock is a range-overlap error; someone else's is a
    // plain lock error (appletalk_server.md, FPByteRangeLock result codes).
    for (size_t i = 0; i < fk->n_locks; i++)
        if (ranges_overlap(want.start, want_end, &fk->locks[i]))
            return AFP_FORK_RANGE_OVERLAP;
    if (foreign_lock_covers(fk, want.start, want_end))
        return AFP_FORK_LOCK_ERR;
    if (fk->n_locks >= AFP_MAX_LOCKS_PER_FORK)
        return AFP_FORK_NO_MORE_LOCKS;

    if (fk->n_locks == fk->cap_locks) {
        size_t cap = fk->cap_locks ? fk->cap_locks * 2 : 8;
        lock_range_t *tmp = (lock_range_t *)realloc(fk->locks, cap * sizeof(lock_range_t));
        if (!tmp)
            return AFP_FORK_NO_MORE_LOCKS;
        fk->locks = tmp;
        fk->cap_locks = cap;
    }
    fk->locks[fk->n_locks++] = want;
    return AFP_FORK_OK;
}

// --- I/O -------------------------------------------------------------------

uint32_t afp_fork_length(afp_fork_t *fk) {
    if (!fk || !fk->backing || !fk->backing->f)
        return 0;
    if (fseek(fk->backing->f, 0, SEEK_END) != 0)
        return 0;
    long sz = ftell(fk->backing->f);
    return sz < 0 ? 0 : (uint32_t)sz;
}

afp_fork_status_t afp_fork_read(afp_fork_t *fk, uint32_t offset, uint32_t count, uint8_t *buf, uint32_t *out_read) {
    if (out_read)
        *out_read = 0;
    if (!fk || !fk->backing || !fk->backing->f || !buf)
        return AFP_FORK_IO_ERR;
    uint64_t end = (uint64_t)offset + count;
    if (end > UINT32_MAX)
        end = UINT32_MAX;
    if (foreign_lock_covers(fk, offset, (uint32_t)end))
        return AFP_FORK_LOCK_ERR;
    if (fseek(fk->backing->f, (long)offset, SEEK_SET) != 0)
        return AFP_FORK_IO_ERR;
    size_t got = fread(buf, 1, count, fk->backing->f);
    if (out_read)
        *out_read = (uint32_t)got;
    return AFP_FORK_OK;
}

afp_fork_status_t afp_fork_write(afp_fork_t *fk, uint32_t offset, const uint8_t *buf, uint32_t count,
                                 uint32_t *out_written) {
    if (out_written)
        *out_written = 0;
    if (!fk || !fk->backing || !fk->backing->f)
        return AFP_FORK_IO_ERR;
    if (!(fk->access_mode & AFP_ACCESS_WRITE))
        return AFP_FORK_ACCESS_DENIED;
    uint64_t end = (uint64_t)offset + count;
    if (end > UINT32_MAX)
        end = UINT32_MAX;
    if (foreign_lock_covers(fk, offset, (uint32_t)end))
        return AFP_FORK_LOCK_ERR;
    if (fseek(fk->backing->f, (long)offset, SEEK_SET) != 0)
        return AFP_FORK_IO_ERR;
    size_t wrote = count ? fwrite(buf, 1, count, fk->backing->f) : 0;
    fflush(fk->backing->f); // make writes visible to every other handle at once
    if (wrote)
        fk->backing->dirty = true;
    if (out_written)
        *out_written = (uint32_t)wrote;
    return AFP_FORK_OK;
}

afp_fork_status_t afp_fork_truncate(afp_fork_t *fk, uint32_t length) {
    if (!fk || !fk->backing || !fk->backing->f)
        return AFP_FORK_IO_ERR;
    if (!(fk->access_mode & AFP_ACCESS_WRITE))
        return AFP_FORK_ACCESS_DENIED;
    fflush(fk->backing->f);
    int fd = fileno(fk->backing->f);
    if (fd < 0 || ftruncate(fd, (off_t)length) != 0)
        return AFP_FORK_IO_ERR;
    fk->backing->dirty = true;
    return AFP_FORK_OK;
}

void afp_fork_flush(afp_fork_t *fk) {
    if (!fk || !fk->backing)
        return;
    if (fk->backing->f)
        fflush(fk->backing->f);
    persist_backing(fk->backing);
}

void afp_fork_flush_volume(uint16_t vol_id) {
    for (afp_backing_t *b = g_backings; b; b = b->next) {
        if (b->vol_id != vol_id)
            continue;
        if (b->f)
            fflush(b->f);
        persist_backing(b);
    }
}
