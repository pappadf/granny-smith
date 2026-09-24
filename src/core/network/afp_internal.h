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
#include "afp_fork.h"
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
#define AFP_MAX_VOLUMES ATALK_AFP_MAX_VOLUMES

// The least reply buffer the dispatcher accepts: one ATP response packet,
// which ASP always offers.  Every fixed-size reply fits in it.
#define AFP_MIN_REPLY 578

// Upper bound on concurrent ASP sessions, mirroring the stack's own table.
#define AFP_MAX_SESSIONS ATALK_ASP_MAX_SESSIONS

// NBP entity strings for the AFP server.
#define AFP_ENTITY_OBJECT "Shared Folders"
#define AFP_ENTITY_TYPE   "AFPServer"

// Wire fields are 32-bit and classic clients cap a volume at 2 GB, so every
// size we report is clamped here (proposal §5 WP-1).
#define AFP_VOL_SIZE_CEILING   AFP_FORK_MAX_LENGTH // 2 GB - 1 KB, kept 512-byte aligned
#define AFP_VOL_FALLBACK_TOTAL (1024u * 1024u * 1024u)
#define AFP_VOL_FALLBACK_FREE  (512u * 1024u * 1024u)

// The sessions holding one handle -- a volume, or its desktop database.
typedef struct {
    uint16_t ids[AFP_MAX_SESSIONS];
    uint32_t n;
} afp_session_set_t;

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
    uint32_t backup_date; // AFP time, persisted in .gs-afp/volume
    uint32_t mutations; // bumped by every catalog-visible change (enum snapshots)
    // Sessions that have this volume open (FPOpenVol), and its desktop
    // database (FPOpenDT).  A volume ID or DTRefNum is served only to a
    // session in the set: it is that session's handle, not a global name.
    afp_session_set_t open_by;
    afp_session_set_t dt_open_by;
} vol_t;

// What every handler knows about the session it is answering for: its
// identity, to attribute forks and enumeration snapshots to it.
typedef struct {
    uint16_t session_id;
} afp_ctx_t;

// One command as its handler sees it: the session, the request (in[0] is the
// byte after the opcode, a pad or a flag byte), and the reply buffer -- never
// less than AFP_MIN_REPLY, so a handler checks the room only for what can be
// longer.  `out_len` starts at 0; a handler that replies with data sets it.
typedef struct {
    afp_ctx_t *ctx;
    const uint8_t *in;
    int in_len;
    uint8_t *out;
    int out_max;
    int out_len;
} afp_req_t;

// --- The volume table and the server (afp_volume.c) ----------------------------

extern vol_t g_vols[AFP_MAX_VOLUMES];
extern atalk_afp_stats_t g_afp_stats;
extern bool g_afp_enabled;
extern char g_afp_message[AFP_META_COMMENT_MAX + 1];
void session_set_add(afp_session_set_t *set, uint16_t session);
void session_set_remove(afp_session_set_t *set, uint16_t session);
bool session_set_has(const afp_session_set_t *set, uint16_t session);
vol_t *afp_session_vol(const afp_ctx_t *ctx, uint16_t vol_id);
void vol_record_store(const vol_t *v);
vol_t *find_vol_by_name(const char *name);
vol_t *find_vol_by_id(uint16_t id);
void afp_count_result(uint32_t result);

// --- Paths and parameter blocks (afp_params.c) ---------------------------------

// A share-relative path as a host path: afp_host_join under the volume root.
bool afp_host_path(const vol_t *vol, const char *rel, char *out, size_t out_len);
bool afp_stat_path(vol_t *vol, const char *rel, struct stat *st);
// The parameters of one file or directory at `pbase`, as the bitmap selects
// them: the fixed fields, then the names -- `rel`'s last element, or the
// volume's name for its root -- padded to an even offset.  The one writer for
// every reply that carries them.  Returns the end offset, or -1 if it does not
// fit in `out_max`.
int afp_emit_params(bool is_dir, vol_t *vol, const char *rel, const struct stat *st, uint16_t bm, uint8_t *out,
                    int pbase, int out_max);
// One result record of FPEnumerate or FPCatSearch at `w`: StructLength(1),
// FileDir flag(1), then afp_emit_params.  Returns the offset after it, or -1
// if it does not fit -- in `out_max`, or in StructLength's one byte.
int afp_emit_record(bool is_dir, vol_t *vol, const char *rel, const struct stat *st, uint16_t bm, uint8_t *out, int w,
                    int out_max);
int afp_read_pstring(const uint8_t *in, int in_len, int pos, char *dst, size_t dst_len);
const afp_cat_entry_t *afp_entry_for(vol_t *vol, const char *rel_path);
bool afp_build_child_path(const char *parent, const char *child, char *out, size_t out_len);
int afp_write_vol_param_block(vol_t *v, uint16_t *bitmap_ptr, uint8_t *out, int out_max, bool afp21);
void afp_vol_touch(vol_t *vol);
uint32_t afp_unix_time_to_afp(time_t t);
uint32_t afp_parent_cnid(vol_t *vol, const char *rel_path);
int afp_param_field_width(bool is_dir, int bit);
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
bool afp_walk_path(vol_t *vol, const char *base_rel, const afp_path_t *path, char *out, size_t out_len);
// A host name as a client sees it: MacRoman, and at most 31 characters -- a
// longer one is shortened to its first bytes and "#<CNID in hex>" (D-5).
int afp_client_name(const char *host_name, uint32_t cnid, uint8_t *out, size_t cap);
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

uint32_t afp_cmd_enumerate(afp_req_t *r);
// Drop the FPEnumerate snapshots a session holds on a volume; ENUM_ANY for
// either matches every one.  A volume's go when it is withdrawn -- or a later
// volume given its ID would serve its listing (10-network F-09).
#define ENUM_ANY UINT32_MAX
void enum_snapshots_drop(uint32_t session_id, uint32_t vol_id);

// --- Handlers and dispatch (appletalk_server.c) --------------------------------

uint32_t afp_resolve_target(const afp_ctx_t *ctx, uint16_t vol_id, uint32_t dir_id, const afp_path_t *path,
                            vol_t **out_vol, char *out_rel, size_t rel_cap);
// The same for a request laid out Volume ID (offset 1), Directory ID (3),
// Pathname (`path_at`): ParamErr for a bad pathname.  `*next`, if given, is the
// offset after the pathname.
uint32_t afp_decode_target(const afp_req_t *r, int path_at, vol_t **out_vol, char *out_rel, size_t rel_cap, int *next);
void afp_log_hex(const char *label, const uint8_t *buf, int len);

#endif // AFP_INTERNAL_H
