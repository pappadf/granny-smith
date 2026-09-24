// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// afp_enum.c
// FPEnumerate and the snapshots that make its paging consistent (WP-4).
// Part of the AFP server; afp_internal.h has what its files share.

#include "afp_internal.h"

#include "afp_catalog.h"
#include "afp_desktop.h"
#include "afp_fork.h"
#include "afp_meta.h"
#include "afp_server.h"
#include "afp_wire.h"
#include "appledouble.h"
#include "appletalk.h"
#include "appletalk_asp.h"
#include "appletalk_internal.h"
#include "atalk_id.h"
#include "common.h"
#include "log.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

LOG_USE_CATEGORY_NAME("afp");

// ============================================================================
// FPEnumerate snapshots (WP-4)
// ============================================================================

// One directory entry captured in a snapshot; its path is the snapshot's
// directory and its name.
typedef struct {
    char name[AFP_MAX_NAME + 1];
    bool is_dir;
    struct stat st;
} enum_entry_t;

// A per-(session, directory) listing, taken on the first page and served for
// every subsequent page so concurrent changes can neither skip nor duplicate
// an entry.  The snapshot is discarded when the volume mutates or the client
// restarts the walk.
typedef struct {
    bool in_use;
    uint16_t session_id;
    uint16_t vol_id;
    uint32_t dir_cnid;
    char dir_rel[AFP_MAX_REL_PATH];
    uint32_t generation; // catalog generation at capture
    uint32_t mutations; // volume mutation counter at capture
    uint64_t last_used; // g_enum_clock at the last page served
    enum_entry_t *entries;
    size_t count;
} enum_snapshot_t;

// Snapshots held at once, and by one session: a session walking more
// directories than its share evicts its own oldest, not another session's
// listing in progress (10-network F-23).
#define AFP_MAX_ENUM_SNAPSHOTS         8
#define AFP_ENUM_SNAPSHOTS_PER_SESSION 4
// Entries one listing holds.  StartIndex is 16 bits, so no client can page
// past this; a larger directory is MiscErr rather than a listing cut short.
#define AFP_MAX_ENUM_ENTRIES 65535u
static enum_snapshot_t g_enum_snapshots[AFP_MAX_ENUM_SNAPSHOTS];
static uint64_t g_enum_clock;

static void enum_snapshot_free(enum_snapshot_t *s) {
    free(s->entries);
    memset(s, 0, sizeof(*s));
}

void enum_snapshots_drop(uint32_t session_id, uint32_t vol_id) {
    for (int i = 0; i < AFP_MAX_ENUM_SNAPSHOTS; i++) {
        enum_snapshot_t *s = &g_enum_snapshots[i];
        if (s->in_use && (session_id == ENUM_ANY || s->session_id == session_id) &&
            (vol_id == ENUM_ANY || s->vol_id == vol_id))
            enum_snapshot_free(s);
    }
}

// Name order for a stable listing: by Mac name, folded as AFP compares names
// (D-7), then exactly, so names differing in case alone keep one order.
static int enum_entry_cmp(const void *a, const void *b) {
    const enum_entry_t *ea = (const enum_entry_t *)a;
    const enum_entry_t *eb = (const enum_entry_t *)b;
    int rc = afp_name_fold_cmp(ea->name, eb->name);
    return rc ? rc : strcmp(ea->name, eb->name);
}

// The least recently used of the snapshots `session` holds (any session's
// when ENUM_ANY), or NULL when it holds none.
static enum_snapshot_t *enum_snapshot_lru(uint32_t session_id, int *held) {
    enum_snapshot_t *lru = NULL;
    *held = 0;
    for (int i = 0; i < AFP_MAX_ENUM_SNAPSHOTS; i++) {
        enum_snapshot_t *s = &g_enum_snapshots[i];
        if (!s->in_use || (session_id != ENUM_ANY && s->session_id != session_id))
            continue;
        (*held)++;
        if (!lru || s->last_used < lru->last_used)
            lru = s;
    }
    return lru;
}

// The slot a new listing goes in: the one this (session, volume, directory)
// already has; the session's oldest when it holds its share; a free one; or
// the oldest of all.  Slot 0 used to be evicted, whoever held it -- the
// session's own listing from a moment before included.
static enum_snapshot_t *enum_snapshot_slot(uint16_t session_id, uint16_t vol_id, uint32_t dir_cnid) {
    for (int i = 0; i < AFP_MAX_ENUM_SNAPSHOTS; i++) {
        enum_snapshot_t *s = &g_enum_snapshots[i];
        if (s->in_use && s->session_id == session_id && s->vol_id == vol_id && s->dir_cnid == dir_cnid)
            return s;
    }
    int held = 0;
    enum_snapshot_t *own = enum_snapshot_lru(session_id, &held);
    if (held >= AFP_ENUM_SNAPSHOTS_PER_SESSION)
        return own;
    for (int i = 0; i < AFP_MAX_ENUM_SNAPSHOTS; i++)
        if (!g_enum_snapshots[i].in_use)
            return &g_enum_snapshots[i];
    return enum_snapshot_lru(ENUM_ANY, &held);
}

// Capture a directory listing into a snapshot slot.  NULL when it cannot be
// taken whole: the directory is unreadable, holds more than
// AFP_MAX_ENUM_ENTRIES, or memory runs out -- a listing silently cut short
// would be worse than none.
static enum_snapshot_t *enum_snapshot_build(afp_ctx_t *ctx, vol_t *vol, uint32_t dir_cnid, const char *dir_rel) {
    enum_snapshot_t *slot = enum_snapshot_slot(ctx->session_id, vol->vol_id, dir_cnid);
    enum_snapshot_free(slot);

    char full_dir[PATH_MAX];
    if (!afp_host_path(vol, dir_rel, full_dir, sizeof(full_dir)))
        return NULL;
    DIR *dir = opendir(full_dir);
    if (!dir)
        return NULL;

    size_t cap = 64, count = 0;
    enum_entry_t *entries = (enum_entry_t *)malloc(cap * sizeof(enum_entry_t));
    if (!entries) {
        closedir(dir);
        return NULL;
    }
    struct dirent *dent;
    while ((dent = readdir(dir)) != NULL) {
        if (strcmp(dent->d_name, ".") == 0 || strcmp(dent->d_name, "..") == 0)
            continue;
        // Sidecars and the .gs-afp control directory, and host names MacRoman
        // cannot hold: listed under a lossy name, such a file could never be
        // addressed again (10-network D-1).
        if (!afp_name_visible(dent->d_name))
            continue;
        char child_rel[AFP_MAX_REL_PATH];
        struct stat child_st;
        if (!afp_build_child_path(dir_rel, dent->d_name, child_rel, sizeof(child_rel)) ||
            !afp_stat_path(vol, child_rel, &child_st))
            continue;
        const char *why = NULL;
        if (count == AFP_MAX_ENUM_ENTRIES)
            why = "more entries than a listing can page";
        else if (count == cap) {
            enum_entry_t *tmp = (enum_entry_t *)realloc(entries, cap * 2 * sizeof(enum_entry_t));
            if (tmp) {
                entries = tmp;
                cap *= 2;
            } else
                why = "out of memory";
        }
        if (why) {
            LOG(1, "AFP FPEnumerate: '%s' not listed -- %s", dir_rel[0] ? dir_rel : "<root>", why);
            free(entries);
            closedir(dir);
            return NULL;
        }
        enum_entry_t *e = &entries[count];
        snprintf(e->name, sizeof(e->name), "%s", dent->d_name);
        e->is_dir = S_ISDIR(child_st.st_mode);
        e->st = child_st;
        // Adopt each child so its CNID is stable from the first listing on.
        afp_catalog_resolve_path(vol->catalog, child_rel, true, e->is_dir);
        count++;
    }
    closedir(dir);
    qsort(entries, count, sizeof(enum_entry_t), enum_entry_cmp);

    slot->in_use = true;
    slot->session_id = ctx->session_id;
    slot->vol_id = vol->vol_id;
    slot->dir_cnid = dir_cnid;
    snprintf(slot->dir_rel, sizeof(slot->dir_rel), "%s", dir_rel);
    slot->last_used = ++g_enum_clock;
    slot->generation = afp_catalog_generation(vol->catalog);
    slot->mutations = vol->mutations;
    slot->entries = entries;
    slot->count = count;
    return slot;
}

// Find a still-valid snapshot for this (session, volume, directory), or NULL.
static enum_snapshot_t *enum_snapshot_find(afp_ctx_t *ctx, vol_t *vol, uint32_t dir_cnid) {
    for (int i = 0; i < AFP_MAX_ENUM_SNAPSHOTS; i++) {
        enum_snapshot_t *s = &g_enum_snapshots[i];
        if (!s->in_use || s->session_id != ctx->session_id || s->vol_id != vol->vol_id || s->dir_cnid != dir_cnid)
            continue;
        if (s->generation != afp_catalog_generation(vol->catalog) || s->mutations != vol->mutations) {
            enum_snapshot_free(s);
            return NULL;
        }
        s->last_used = ++g_enum_clock;
        return s;
    }
    return NULL;
}

// ============================================================================
// FPEnumerate (0x09)
// ============================================================================

uint32_t afp_cmd_enumerate(afp_req_t *r) {
    if (r->in_len < 18)
        return AFPERR_ParamErr;
    afp_log_hex("AFP FPEnumerate req", r->in, r->in_len);
    uint16_t file_bm = RD_BE16(r->in + 7);
    uint16_t dir_bm = RD_BE16(r->in + 9);
    uint16_t req_count = RD_BE16(r->in + 11);
    uint16_t start_index = RD_BE16(r->in + 13);
    uint16_t max_reply = RD_BE16(r->in + 15);
    if (start_index == 0)
        start_index = 1;
    if (file_bm == 0 && dir_bm == 0)
        return AFPERR_BitmapErr;

    vol_t *vol = NULL;
    char target_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_decode_target(r, 17, &vol, target_rel, sizeof(target_rel), NULL);
    if (rc != AFPERR_NoErr)
        return rc;
    struct stat dir_st;
    if (!afp_stat_path(vol, target_rel, &dir_st))
        return AFPERR_ObjectNotFound;
    if (!S_ISDIR(dir_st.st_mode))
        return AFPERR_ObjectTypeErr;
    const afp_cat_entry_t *dir_entry = afp_entry_for(vol, target_rel);
    uint32_t dir_cnid = dir_entry ? dir_entry->cnid : AFP_CNID_ROOT;

    // The listing is captured once, on the first page, and every later page is
    // served from that capture — otherwise a concurrent create or delete
    // shifts the indices and the client skips or repeats an entry.
    enum_snapshot_t *snap = (start_index == 1) ? NULL : enum_snapshot_find(r->ctx, vol, dir_cnid);
    if (!snap)
        snap = enum_snapshot_build(r->ctx, vol, dir_cnid, target_rel);
    if (!snap)
        return AFPERR_MiscErr;

    if (snap->count == 0)
        return AFPERR_ObjectNotFound; // an empty directory has nothing to return
    if (start_index > snap->count)
        return AFPERR_ObjectNotFound;

    int max_bytes = max_reply ? (int)max_reply : r->out_max;
    if (max_bytes > r->out_max)
        max_bytes = r->out_max;
    if (max_bytes < 6)
        return AFPERR_ParamErr;

    WR_BE16(r->out + 0, file_bm);
    WR_BE16(r->out + 2, dir_bm);
    WR_BE16(r->out + 4, 0);
    int w = 6;
    uint16_t actual = 0;
    uint16_t left = req_count ? req_count : UINT16_MAX;

    // StartIndex counts the entries of the kinds asked for: with a null file
    // bitmap it indexes the directories alone, with a null directory bitmap
    // the files.  It indexed the mixed listing, so a directories-only walk
    // repeated entries (10-network N-13a).
    size_t i = 0, seen = 0;
    if (file_bm && dir_bm)
        i = seen = start_index - 1u; // every entry counts: go straight there
    for (; i < snap->count && left > 0; i++) {
        enum_entry_t *entry = &snap->entries[i];
        uint16_t bm = entry->is_dir ? dir_bm : file_bm;
        if (bm == 0)
            continue; // this kind was not requested
        if (++seen < start_index)
            continue;
        char rel[AFP_MAX_REL_PATH];
        if (!afp_build_child_path(snap->dir_rel, entry->name, rel, sizeof(rel)))
            break;
        int end = afp_emit_record(entry->is_dir, vol, rel, &entry->st, bm, r->out, w, max_bytes);
        if (end < 0)
            break;
        w = end;
        actual++;
        left--;
    }

    WR_BE16(r->out + 4, actual);
    if (actual == 0)
        return AFPERR_ObjectNotFound;
    r->out_len = w;
    LOG(10, "AFP FPEnumerate: vol=0x%04X dir='%s' start=%u req=%u returned=%u total=%zu", vol->vol_id,
        target_rel[0] ? target_rel : "<root>", start_index, req_count, actual, snap->count);
    afp_log_hex("AFP FPEnumerate resp", r->out, w);
    return AFPERR_NoErr;
}
