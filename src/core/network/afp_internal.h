// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// afp_internal.h
// What the AFP server's translation units share: appletalk_server.c (the
// command handlers and dispatch), afp_volume.c (the volume table and server
// identity), afp_params.c (the parameter-area codec) and afp_enum.c
// (FPEnumerate).  Not for use outside them.
//
// The server was one 4,000-line file holding five concerns (10-network F-20);
// the split follows the section banners it already had.

#ifndef AFP_INTERNAL_H
#define AFP_INTERNAL_H

#include "afp_catalog.h"
#include "afp_desktop.h"
#include "afp_meta.h"
#include "afp_wire.h"
#include "appletalk.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define AFP_MAX_REL_PATH AFP_CAT_MAX_PATH
#define AFP_MAX_NAME     255
#define AFP_LOG_HEX_MAX  64

#ifndef ARRAY_LEN
#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
#endif

// Volume table size.  One entry per share; the object model exposes the live
// ones as `appletalk.afp.volumes`.
#define AFP_MAX_VOLUMES 8

// Upper bound on concurrent ASP sessions, mirroring the stack's own table.
#define AFP_MAX_SESSIONS 4

// NBP entity strings for the AFP server.
#define AFP_ENTITY_OBJECT "Shared Folders"
#define AFP_ENTITY_TYPE   "AFPServer"

// Wire fields are 32-bit and classic clients cap a volume at 2 GB, so every
// size we report is clamped here (proposal §5 WP-1).
#define AFP_VOL_SIZE_CEILING   0x7FFFFC00u // 2 GB - 1 KB, kept 512-byte aligned
#define AFP_VOL_FALLBACK_TOTAL (1024u * 1024u * 1024u)
#define AFP_VOL_FALLBACK_FREE  (512u * 1024u * 1024u)

// One shared directory, published as an AFP volume.  The share table and the
// server's volume table used to be separate mirrors of each other; they are
// one structure now, so a volume's identity, its catalog and its desktop
// database have a single home.
typedef struct {
    bool in_use;
    uint16_t vol_id; // stable, monotonically assigned
    char name[33];
    char root[PATH_MAX];
    afp_catalog_t *catalog;
    afp_desktop_t *desktop;
    uint16_t dt_ref; // desktop-database refnum handed to clients (0 = closed)
    uint32_t backup_date; // AFP time, persisted in .gs-afp/volume
    uint32_t mutations; // bumped by every catalog-visible change (enum snapshots)
    // Sessions that currently have this volume open.  Tracked by reference
    // rather than as a bare count: a logout has to drop only the volumes that
    // session actually opened, and a stale count is directly visible in the
    // object model.
    uint16_t open_by[AFP_MAX_SESSIONS];
    uint32_t n_open_by;
} vol_t;

// What every handler knows about the session it is answering for: its
// identity, to attribute forks and enumeration snapshots to it.
typedef struct {
    uint16_t session_id;
} afp_ctx_t;

// --- The volume table and the server (afp_volume.c) ----------------------------

extern vol_t g_vols[AFP_MAX_VOLUMES];
extern atalk_afp_stats_t g_afp_stats;
extern bool g_afp_enabled;
extern uint16_t g_next_dt_ref;
extern char g_afp_message[AFP_META_COMMENT_MAX + 1];
void vol_session_add(vol_t *v, uint16_t session);
void vol_session_remove(vol_t *v, uint16_t session);
void vol_record_store(const vol_t *v);
vol_t *find_vol_by_name(const char *name);
vol_t *find_vol_by_id(uint16_t id);
void afp_count_result(uint32_t result);

// --- Paths and parameter blocks (afp_params.c) ---------------------------------

int afp_write_param_area(bool is_dir, uint16_t bm, uint8_t *out, int p, int out_max, int *pos_long_off,
                         int *pos_short_off);
int afp_write_name_vars(uint8_t *out, int vpos, int out_max, int pbase, const char *host_name, uint16_t bm,
                        int pos_long_off, int pos_short_off);
bool afp_stat_path(vol_t *vol, const char *rel, struct stat *st);
int afp_read_pstring(const uint8_t *in, int in_len, int pos, char *dst, size_t dst_len);
bool afp_populate_param_area(bool is_dir, vol_t *vol, const char *rel_path, const struct stat *st, uint16_t bm,
                             uint8_t *out, int pbase);
bool afp_full_path(const vol_t *vol, const char *rel, char *out, size_t out_len);
const afp_cat_entry_t *afp_entry_for(vol_t *vol, const char *rel_path);
bool afp_build_child_path(const char *parent, const char *child, char *out, size_t out_len);
int afp_write_vol_param_block(vol_t *v, uint16_t *bitmap_ptr, uint8_t *out, int out_max, bool afp21);
void afp_vol_touch(vol_t *vol);
uint32_t afp_unix_time_to_afp(time_t t);
uint32_t afp_parent_cnid(vol_t *vol, const char *rel_path);
int afp_param_field_width(bool is_dir, int bit);
int afp_param_field_ptr(bool is_dir, uint16_t bm, int pbase, int target_bit);
// A pathname off the wire (Inside AppleTalk 13-10): the Pascal string's bytes,
// CNode names separated by NULs.
typedef struct {
    int len;
    uint8_t bytes[255];
} afp_path_t;
// Read PathType + pathname at `pos`: the position after it, or -1 (ParamErr)
// for a path type other than 1 or 2 or a length past the request.
int afp_read_path(const uint8_t *in, int in_len, int pos, afp_path_t *out);
// Resolve `path` below `base_rel` into a volume-relative host path.
bool afp_walk_path(const char *base_rel, const afp_path_t *path, char *out, size_t out_len);
// A new name (FPRename, FPMoveAndRename, FPCopyFile): exactly one element, as
// its host name.
bool afp_parse_leaf(const afp_path_t *path, char *out, size_t cap);
// True if the server shows the host name `host_name` to clients: not one of
// its own, and representable as a Mac name (10-network D-1).
bool afp_name_visible(const char *host_name);
// A host name, or a host string, as the Mac bytes that go on the wire (at most
// `cap`); the length.  Names go through the name codec; text only through
// MacRoman.  Either falls back to the raw bytes if it cannot convert.
int afp_mac_name(const char *host_name, uint8_t *out, size_t cap);
int afp_mac_text(const char *text, uint8_t *out, size_t cap);
const char *afp_last_component(const char *rel_path);
int afp_fixed_param_len(bool is_dir, uint16_t bm);
void afp_extract_parent(const char *rel_path, char *parent, size_t parent_len);
bool afp_dir_rel_path(vol_t *vol, uint32_t dir_id, char *out, size_t cap);
uint16_t afp_count_offspring(const char *full_path);
uint16_t afp_attributes_of(const char *host_path, const struct stat *st, const afp_meta_t *meta);

// --- FPEnumerate and its snapshots (afp_enum.c) --------------------------------

uint32_t afp_cmd_enumerate(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len);
void enum_snapshots_drop_volume(uint16_t vol_id);
void enum_snapshots_drop_session(uint16_t session_id);

// --- Handlers and dispatch (appletalk_server.c) --------------------------------

uint32_t afp_resolve_target(uint16_t vol_id, uint32_t dir_id, const afp_path_t *path, vol_t **out_vol, char *out_rel,
                            size_t rel_cap);
void afp_log_hex(const char *label, const uint8_t *buf, int len);

#endif // AFP_INTERNAL_H
