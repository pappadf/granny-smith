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

#include <assert.h>
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

LOG_USE_CATEGORY_NAME("appletalk");

// ============================================================================
// FPEnumerate snapshots (WP-4)
// ============================================================================

// One directory entry captured in a snapshot.
typedef struct {
    char name[AFP_MAX_NAME + 1];
    char rel[AFP_MAX_REL_PATH];
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
    uint32_t generation; // catalog generation at capture
    uint32_t mutations; // volume mutation counter at capture
    enum_entry_t *entries;
    size_t count;
} enum_snapshot_t;

#define AFP_MAX_ENUM_SNAPSHOTS 8
static enum_snapshot_t g_enum_snapshots[AFP_MAX_ENUM_SNAPSHOTS];

static void enum_snapshot_free(enum_snapshot_t *s) {
    free(s->entries);
    memset(s, 0, sizeof(*s));
}

// Drop every snapshot belonging to a session (logout / expiry).
void enum_snapshots_drop_session(uint16_t session_id) {
    for (int i = 0; i < AFP_MAX_ENUM_SNAPSHOTS; i++)
        if (g_enum_snapshots[i].in_use && g_enum_snapshots[i].session_id == session_id)
            enum_snapshot_free(&g_enum_snapshots[i]);
}

// Drop every snapshot belonging to a volume (share removal / checkpoint restore).
void enum_snapshots_drop_volume(uint16_t vol_id) {
    for (int i = 0; i < AFP_MAX_ENUM_SNAPSHOTS; i++)
        if (g_enum_snapshots[i].in_use && g_enum_snapshots[i].vol_id == vol_id)
            enum_snapshot_free(&g_enum_snapshots[i]);
}

// Name order for a stable listing.  Case-insensitive so the guest sees the
// same sequence HFS would produce.
static int enum_entry_cmp(const void *a, const void *b) {
    const enum_entry_t *ea = (const enum_entry_t *)a;
    const enum_entry_t *eb = (const enum_entry_t *)b;
    int rc = strcasecmp(ea->name, eb->name);
    return rc ? rc : strcmp(ea->name, eb->name);
}

// Capture a directory listing into a snapshot slot.  Dynamically sized — the
// old fixed 512-entry cap silently truncated large directories.
static enum_snapshot_t *enum_snapshot_build(afp_ctx_t *ctx, vol_t *vol, uint32_t dir_cnid, const char *dir_rel) {
    enum_snapshot_t *slot = NULL;
    for (int i = 0; i < AFP_MAX_ENUM_SNAPSHOTS; i++) {
        enum_snapshot_t *s = &g_enum_snapshots[i];
        if (s->in_use && s->session_id == ctx->session_id && s->vol_id == vol->vol_id && s->dir_cnid == dir_cnid) {
            slot = s;
            break;
        }
    }
    if (!slot)
        for (int i = 0; i < AFP_MAX_ENUM_SNAPSHOTS; i++)
            if (!g_enum_snapshots[i].in_use) {
                slot = &g_enum_snapshots[i];
                break;
            }
    if (!slot)
        slot = &g_enum_snapshots[0]; // evict the oldest slot rather than fail
    enum_snapshot_free(slot);

    char full_dir[PATH_MAX];
    if (!afp_full_path(vol, dir_rel, full_dir, sizeof(full_dir)))
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
        char child_full[PATH_MAX];
        if (snprintf(child_full, sizeof(child_full), "%s/%s", full_dir, dent->d_name) >= (int)sizeof(child_full))
            continue;
        struct stat child_st;
        if (stat(child_full, &child_st) != 0)
            continue;
        if (count == cap) {
            size_t next = cap * 2;
            enum_entry_t *tmp = (enum_entry_t *)realloc(entries, next * sizeof(enum_entry_t));
            if (!tmp)
                break;
            entries = tmp;
            cap = next;
        }
        enum_entry_t *e = &entries[count];
        snprintf(e->name, sizeof(e->name), "%s", dent->d_name);
        e->is_dir = S_ISDIR(child_st.st_mode);
        e->st = child_st;
        if (!afp_build_child_path(dir_rel, e->name, e->rel, sizeof(e->rel)))
            continue;
        // Adopt each child so its CNID is stable from the first listing on.
        afp_catalog_resolve_path(vol->catalog, e->rel, true, e->is_dir);
        count++;
    }
    closedir(dir);
    qsort(entries, count, sizeof(enum_entry_t), enum_entry_cmp);

    slot->in_use = true;
    slot->session_id = ctx->session_id;
    slot->vol_id = vol->vol_id;
    slot->dir_cnid = dir_cnid;
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
        return s;
    }
    return NULL;
}

// ============================================================================
// FPEnumerate (0x09)
// ============================================================================

uint32_t afp_cmd_enumerate(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    if (in_len < 18)
        return AFPERR_ParamErr;
    afp_log_hex("AFP FPEnumerate req", in, in_len);
    uint16_t vol_id = RD_BE16(in + 1);
    uint32_t dir_id = RD_BE32(in + 3);
    uint16_t file_bm = RD_BE16(in + 7);
    uint16_t dir_bm = RD_BE16(in + 9);
    uint16_t req_count = RD_BE16(in + 11);
    uint16_t start_index = RD_BE16(in + 13);
    uint16_t max_reply = RD_BE16(in + 15);
    afp_path_t path;
    if (afp_read_path(in, in_len, 17, &path) < 0)
        return AFPERR_ParamErr;
    if (start_index == 0)
        start_index = 1;
    if (file_bm == 0 && dir_bm == 0)
        return AFPERR_BitmapErr;

    vol_t *vol = NULL;
    char target_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_target(vol_id, dir_id, &path, &vol, target_rel, sizeof(target_rel));
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
    enum_snapshot_t *snap = (start_index == 1) ? NULL : enum_snapshot_find(ctx, vol, dir_cnid);
    if (!snap)
        snap = enum_snapshot_build(ctx, vol, dir_cnid, target_rel);
    if (!snap)
        return AFPERR_MiscErr;

    if (snap->count == 0)
        return AFPERR_ObjectNotFound; // an empty directory has nothing to return
    if (start_index > snap->count)
        return AFPERR_ObjectNotFound;

    int max_bytes = max_reply ? (int)max_reply : out_max;
    if (max_bytes > out_max)
        max_bytes = out_max;
    if (max_bytes < 6)
        return AFPERR_ParamErr;

    WR_BE16(out + 0, file_bm);
    WR_BE16(out + 2, dir_bm);
    WR_BE16(out + 4, 0);
    int w = 6;
    uint16_t actual = 0;
    uint16_t left = req_count ? req_count : UINT16_MAX;

    for (size_t i = start_index - 1; i < snap->count && left > 0; i++) {
        enum_entry_t *entry = &snap->entries[i];
        uint16_t bm = entry->is_dir ? dir_bm : file_bm;
        if (bm == 0)
            continue; // this kind was not requested
        int header = w;
        if (header + 2 > max_bytes)
            break;
        out[header] = 0; // struct length, patched below
        out[header + 1] = entry->is_dir ? 0x80 : 0x00;
        int pbase = header + 2;
        int pos_long_off = -1, pos_short_off = -1;
        int p = afp_write_param_area(entry->is_dir, bm, out, pbase, max_bytes, &pos_long_off, &pos_short_off);
        if (p < 0)
            break;
        if (!afp_populate_param_area(entry->is_dir, vol, entry->rel, &entry->st, bm, out, pbase))
            break;
        int vpos = afp_write_name_vars(out, p, max_bytes, pbase, entry->name, bm, pos_long_off, pos_short_off);
        if (vpos < 0)
            break;
        int struct_len = vpos - header;
        if (struct_len <= 0)
            break;
        if (struct_len & 1) {
            if (vpos >= max_bytes)
                break;
            out[vpos++] = 0x00;
            struct_len++;
        }
        if (struct_len > 255)
            break; // the per-entry length field is one byte
        out[header] = (uint8_t)struct_len;
        w = vpos;
        actual++;
        left--;
    }

    WR_BE16(out + 4, actual);
    if (actual == 0)
        return AFPERR_ObjectNotFound;
    if (out_len)
        *out_len = w;
    LOG(10, "AFP FPEnumerate: vol=0x%04X dir='%s' start=%u req=%u returned=%u total=%zu", vol_id,
        target_rel[0] ? target_rel : "<root>", start_index, req_count, actual, snap->count);
    afp_log_hex("AFP FPEnumerate resp", out, w);
    return AFPERR_NoErr;
}
