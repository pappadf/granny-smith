// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_server.c
// AFP file server over AppleTalk ASP/ATP protocols: the command handlers and
// their dispatch.  The volume table and server identity are in afp_volume.c,
// the parameter-area codec in afp_params.c, FPEnumerate in afp_enum.c.
//
// The wire reference is docs/core/network/appletalk_server.md; the AFP 2.1
// additions (FPGetSrvrMsg, the file-ID calls, FPExchangeFiles, FPCatSearch)
// follow Apple's AppleTalk Filing Protocol v2.1/2.2 specification (AppleShare IP
// 6.3 Developer's Kit, 1999).  Persistent server state lives in three
// companion modules — afp_catalog.c (CNIDs), afp_desktop.c (icons and APPL
// mappings) and afp_meta.c (per-file AppleDouble metadata) — while open
// forks, deny modes and byte-range locks live in afp_fork.c.

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
#include "macroman.h"

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

// Logging for this module uses the same category as appletalk.c
LOG_USE_CATEGORY_NAME("afp");

// Emit a short hex dump at LOG level 2 for AFP request/response payloads
void afp_log_hex(const char *label, const uint8_t *buf, int len) {
    (void)label; // only read by LOG(), which the unit harness compiles out
    if (!buf || len <= 0)
        return;
    char line[AFP_LOG_HEX_MAX * 3 + 5];
    int pos = 0;
    int cap = (int)(sizeof(line) - 4);
    int limit = len;
    if (limit > AFP_LOG_HEX_MAX)
        limit = AFP_LOG_HEX_MAX;
    for (int i = 0; i < limit && pos < cap; i++) {
        pos += snprintf(&line[pos], (size_t)(cap - pos), "%02X%s", buf[i], (i + 1 == limit) ? "" : " ");
    }
    if (limit < len && pos < cap)
        snprintf(&line[pos], (size_t)(sizeof(line) - pos), " …");
    LOG(2, "%s (%d bytes): %s", label ? label : "AFP hex", len, line);
}

// ============================================================================
// Per-command context and shared resolution
// ============================================================================

typedef uint32_t (*afp_command_handler_fn)(afp_req_t *r);

// What a session must have done before a command is served.
// The zero value is the common case, so the table names only the exceptions.
typedef enum {
    AFP_GATE_LOGIN = 0, // a logged-in session
    AFP_GATE_SESSION, // any open session: FPLogin and FPLoginCont
    AFP_GATE_21, // a session logged in at AFP 2.1: the 2.1 calls
} afp_gate_t;

typedef struct {
    uint8_t opcode;
    const char *name;
    afp_command_handler_fn handler;
    afp_gate_t gate;
} afp_command_handler_t;

// === Sessions, at the AFP level ===============================================
//
// ASP owns the session -- its id, its node, its liveness; the server owns what
// the session has done in AFP: whether it has logged in, and at which version.
// A record is made when ASP opens the session (the client's on_open) and
// dropped when it closes.  Commands used to be served to any session id,
// logged in or not, including ids that never existed.

typedef enum { AFP_SESS_OPEN, AFP_SESS_LOGGED_IN } afp_sess_state_t;

typedef struct {
    bool in_use;
    uint16_t ref;
    afp_sess_state_t state;
    char version[24]; // negotiated at FPLogin
} afp_session_t;

static afp_session_t g_afp_sessions[AFP_MAX_SESSIONS];

static afp_session_t *afp_session(uint16_t ref) {
    for (int i = 0; i < AFP_MAX_SESSIONS; i++)
        if (g_afp_sessions[i].in_use && g_afp_sessions[i].ref == ref)
            return &g_afp_sessions[i];
    return NULL;
}

static void afp_session_release(uint16_t session_id);

bool afp_session_opened(uint16_t session_ref) {
    if (!g_afp_enabled)
        return false; // a disabled server takes no new sessions
    if (afp_session(session_ref))
        return true;
    for (int i = 0; i < AFP_MAX_SESSIONS; i++)
        if (!g_afp_sessions[i].in_use) {
            memset(&g_afp_sessions[i], 0, sizeof(g_afp_sessions[i]));
            g_afp_sessions[i].in_use = true;
            g_afp_sessions[i].ref = session_ref;
            g_afp_sessions[i].state = AFP_SESS_OPEN;
            return true;
        }
    return false;
}

const char *afp_session_version(uint16_t session_ref) {
    afp_session_t *s = afp_session(session_ref);
    return s ? s->version : NULL;
}

// True when this session negotiated AFP 2.1, which is what gates the 2.1
// capability advertising as well as the 2.1 commands themselves.
static bool afp_session_is_21(const afp_ctx_t *ctx) {
    afp_session_t *s = ctx ? afp_session(ctx->session_id) : NULL;
    return s && s->state == AFP_SESS_LOGGED_IN && strcmp(s->version, "AFPVersion 2.1") == 0;
}

// Resolve the (Volume ID, Directory ID, Pathname) triple every catalog call
// carries into a volume plus a volume-relative path.  Returns an AFP result
// code; AFPERR_NoErr means `*out_vol` / `out_rel` are usable.
uint32_t afp_resolve_target(const afp_ctx_t *ctx, uint16_t vol_id, uint32_t dir_id, const afp_path_t *path,
                            vol_t **out_vol, char *out_rel, size_t rel_cap) {
    vol_t *vol = afp_session_vol(ctx, vol_id);
    if (!vol)
        return AFPERR_ParamErr; // unknown, or not opened by this session
    // Directory ID 1 is the root's parent, and a path from it starts with the
    // volume's name (Inside AppleTalk 13-26, the eighth example); the rest
    // resolves from the root.  System 7's AppleShare client asks this way.
    afp_path_t from_root;
    if (dir_id == AFP_CNID_ROOT_PARENT) {
        int start = (path && path->len > 0 && path->bytes[0] == 0) ? 1 : 0;
        int end = start;
        while (path && end < path->len && path->bytes[end] != 0)
            end++;
        uint8_t mac[AFP_MAX_NAME];
        int n = afp_mac_name(vol->name, mac, sizeof(mac));
        if (!path || end == start || afp_fold_cmp(path->bytes + start, (size_t)(end - start), mac, (size_t)n) != 0)
            return AFPERR_ObjectNotFound;
        from_root.len = path->len - end;
        memcpy(from_root.bytes, path->bytes + end, (size_t)from_root.len);
        path = &from_root;
        dir_id = AFP_CNID_ROOT;
    }
    char base[AFP_MAX_REL_PATH];
    if (!afp_dir_rel_path(vol, dir_id, base, sizeof(base)))
        return AFPERR_DirNotFound;
    if (!afp_walk_path(vol, base, path, out_rel, rel_cap))
        return AFPERR_ParamErr;
    if (out_vol)
        *out_vol = vol;
    return AFPERR_NoErr;
}

uint32_t afp_decode_target(const afp_req_t *r, int path_at, vol_t **out_vol, char *out_rel, size_t rel_cap, int *next) {
    afp_path_t path;
    int end = (r->in_len >= 7) ? afp_read_path(r->in, r->in_len, path_at, &path) : -1;
    if (end < 0)
        return AFPERR_ParamErr;
    uint32_t rc = afp_resolve_target(r->ctx, RD_BE16(r->in + 1), RD_BE32(r->in + 3), &path, out_vol, out_rel, rel_cap);
    if (rc == AFPERR_NoErr && next)
        *next = end;
    return rc;
}

// Map a fork-layer status onto the AFP result code the client expects.
static uint32_t afp_fork_status_to_err(afp_fork_status_t st) {
    switch (st) {
    case AFP_FORK_OK:
        return AFPERR_NoErr;
    case AFP_FORK_DENY_CONFLICT:
        return AFPERR_DenyConflict;
    case AFP_FORK_ACCESS_DENIED:
        return AFPERR_AccessDenied;
    case AFP_FORK_TOO_MANY:
        return AFPERR_TooManyFilesOpen;
    case AFP_FORK_NO_MORE_LOCKS:
        return AFPERR_NoMoreLocks;
    case AFP_FORK_LOCK_ERR:
        return AFPERR_LockErr;
    case AFP_FORK_RANGE_OVERLAP:
        return AFPERR_RangeOverlap;
    case AFP_FORK_RANGE_NOT_LOCKED:
        return AFPERR_RangeNotLocked;
    case AFP_FORK_DISK_FULL:
        return AFPERR_DiskFull;
    default:
        return AFPERR_MiscErr;
    }
}

// The fork a request's OForkRefNum (offset 1) names, for this session and with
// the access the command needs.  ParamErr for a refnum the session did not
// open -- another session's included (Inside AppleTalk ch. 13).
static uint32_t afp_fork_from_req(const afp_req_t *r, uint16_t need_access, afp_fork_t **out) {
    if (r->in_len < 3)
        return AFPERR_ParamErr;
    afp_fork_t *fk = afp_fork_find(RD_BE16(r->in + 1), r->ctx->session_id);
    if (!fk)
        return AFPERR_ParamErr;
    if ((afp_fork_access_mode(fk) & need_access) != need_access)
        return AFPERR_AccessDenied;
    *out = fk;
    return AFPERR_NoErr;
}

// True when an object's persisted attributes forbid a mutation.  `bit` is the
// inhibit being tested (AFP_ATTR_DELETEINHIBIT / RENAMEINHIBIT / WRITEINHIBIT).
static bool afp_inhibited(const char *host_path, uint16_t bit) {
    afp_meta_t meta;
    if (!afp_meta_load(host_path, &meta) || !meta.has_attrs)
        return false;
    return (meta.attrs & bit) != 0;
}

// ============================================================================
// Server-level commands
// ============================================================================

// FPGetSrvrParms (0x10) — server time plus the volume list.
static uint32_t afp_cmd_get_srvr_parms(afp_req_t *r) {
    int count = 0;
    for (int i = 0; i < AFP_MAX_VOLUMES; i++)
        if (g_vols[i].in_use)
            count++;

    WR_BE32(r->out, afp_unix_time_to_afp(time(NULL)));
    int pos = 4;
    r->out[pos++] = (uint8_t)count;
    for (int i = 0; i < AFP_MAX_VOLUMES; i++) {
        if (!g_vols[i].in_use)
            continue;
        uint8_t name[255];
        size_t n = (size_t)afp_mac_name(g_vols[i].name, name, sizeof(name));
        if (n > 31)
            n = 31; // HFS name limit
        if (pos + 2 + (int)n > r->out_max)
            break;
        r->out[pos++] = 0x00; // flags: no volume password, not configured
        r->out[pos++] = (uint8_t)n;
        memcpy(&r->out[pos], name, n);
        pos += (int)n;
    }
    r->out_len = pos;
    LOG(10, "AFP FPGetSrvrParms: numvols=%d reply=%d", count, pos);
    return AFPERR_NoErr;
}

// FPGetVolParms (0x11)
static uint32_t afp_cmd_get_vol_parms(afp_req_t *r) {
    if (r->in_len < 5)
        return AFPERR_ParamErr;
    uint16_t vol_id = RD_BE16(r->in + 1);
    uint16_t bitmap = RD_BE16(r->in + 3);
    vol_t *v = afp_session_vol(r->ctx, vol_id);
    if (!v)
        return AFPERR_ParamErr;
    int produced = afp_write_vol_param_block(v, &bitmap, r->out, r->out_max, afp_session_is_21(r->ctx));
    if (produced <= 0)
        return AFPERR_ParamErr;
    r->out_len = produced;
    LOG(10, "AFP FPGetVolParms: vol=0x%04X bitmap=0x%04X reply=%d", vol_id, bitmap, produced);
    return AFPERR_NoErr;
}

// FPOpenVol (0x18)
static uint32_t afp_cmd_open_vol(afp_req_t *r) {
    if (r->in_len < 4)
        return AFPERR_ParamErr;
    afp_log_hex("AFP FPOpenVol req", r->in, r->in_len);
    uint16_t bitmap = RD_BE16(r->in + 1);
    char mac_name[33];
    int pos = afp_read_pstring(r->in, r->in_len, 3, mac_name, sizeof(mac_name));
    if (pos < 0)
        return AFPERR_ParamErr;
    char vol_name[33 * 3 + 1];
    if (!macroman_name_to_host((const uint8_t *)mac_name, strlen(mac_name), vol_name, sizeof(vol_name)))
        return AFPERR_ObjectNotFound;

    vol_t *v = find_vol_by_name(vol_name);
    if (!v)
        return AFPERR_ObjectNotFound;
    // The client must get the volume ID back to address anything on it.
    bitmap |= 0x0020;
    int written = afp_write_vol_param_block(v, &bitmap, r->out, r->out_max, afp_session_is_21(r->ctx));
    if (written <= 0)
        return AFPERR_ParamErr;
    session_set_add(&v->open_by, r->ctx->session_id);
    r->out_len = written;
    LOG(2, "AFP FPOpenVol: '%s' volId=0x%04X bitmap=0x%04X reply=%d (session 0x%04X)", v->name, v->vol_id, bitmap,
        written, r->ctx->session_id);
    afp_log_hex("AFP FPOpenVol resp", r->out, written);
    return AFPERR_NoErr;
}

// FPCloseVol (0x02)
static uint32_t afp_cmd_close_vol(afp_req_t *r) {
    if (r->in_len < 3)
        return AFPERR_ParamErr;
    uint16_t vol_id = RD_BE16(r->in + 1);
    vol_t *v = afp_session_vol(r->ctx, vol_id);
    if (!v)
        return AFPERR_ParamErr;
    session_set_remove(&v->open_by, r->ctx->session_id);
    enum_snapshots_drop(r->ctx->session_id, v->vol_id); // this volume's only
    LOG(10, "AFP FPCloseVol: volId=0x%04X", vol_id);
    return AFPERR_NoErr;
}

// FPSetVolParms (0x20) — only the backup date is settable, and it is now
// persisted in the volume's control record instead of being dropped.
static uint32_t afp_cmd_set_vol_parms(afp_req_t *r) {
    if (r->in_len < 5)
        return AFPERR_ParamErr;
    uint16_t vol_id = RD_BE16(r->in + 1);
    uint16_t bitmap = RD_BE16(r->in + 3);
    vol_t *v = afp_session_vol(r->ctx, vol_id);
    if (!v)
        return AFPERR_ParamErr;
    if (bitmap & ~0x0010u)
        return AFPERR_BitmapErr; // backup date is the only settable parameter
    if (r->in_len < 9)
        return AFPERR_ParamErr;
    v->backup_date = RD_BE32(r->in + 5);
    vol_record_store(v);
    LOG(10, "AFP FPSetVolParms: vol=0x%04X backupDate=%u", vol_id, v->backup_date);
    return AFPERR_NoErr;
}

// FPLogin (0x12) — guest-only, but the negotiated version is remembered so
// FPGetSrvrMsg and the other 2.1 calls can gate on it.
static uint32_t afp_cmd_login(afp_req_t *r) {
    if (r->in_len < 2)
        return AFPERR_ParamErr;
    char ver[64];
    int pos = afp_read_pstring(r->in, r->in_len, 0, ver, sizeof(ver));
    if (pos < 0)
        return AFPERR_ParamErr;
    char uam[64];
    if (afp_read_pstring(r->in, r->in_len, pos, uam, sizeof(uam)) < 0)
        return AFPERR_ParamErr;
    LOG(10, "AFP FPLogin: version='%s' uam='%s'", ver, uam);

    bool ver_ok = false;
    int n_versions = 0;
    const char *const *versions = atalk_afp_versions(&n_versions);
    for (int i = 0; i < n_versions; i++)
        if (strcmp(ver, versions[i]) == 0)
            ver_ok = true;
    if (!ver_ok) {
        LOG(7, "AFP FPLogin: unsupported version → BadVersNum");
        return AFPERR_BadVersNum;
    }
    if (strcmp(uam, "No User Authent") != 0) {
        LOG(7, "AFP FPLogin: unsupported UAM → BadUAM");
        return AFPERR_BadUAM;
    }
    afp_session_t *s = afp_session(r->ctx->session_id);
    if (s) {
        s->state = AFP_SESS_LOGGED_IN;
        snprintf(s->version, sizeof(s->version), "%.*s", (int)sizeof(s->version) - 1, ver);
    }
    WR_BE16(r->out, 0x0000); // guest login carries no user ID
    r->out_len = 2;
    return AFPERR_NoErr;
}

// FPLoginCont (0x13) — unreachable while the only UAM is "No User Authent".
static uint32_t afp_cmd_login_cont(afp_req_t *r) {
    (void)r;
    LOG(10, "AFP FPLoginCont: rejected (no multi-step UAM)");
    return AFPERR_ParamErr;
}

// FPLogout (0x14) — drop everything this session held.
static uint32_t afp_cmd_logout(afp_req_t *r) {
    afp_session_release(r->ctx->session_id);
    // Logged out: the session stays open, but serves nothing until it logs in
    // again.  It used to keep its version and go on working.
    afp_session_t *s = afp_session(r->ctx->session_id);
    if (s) {
        s->state = AFP_SESS_OPEN;
        s->version[0] = '\0';
    }
    LOG(2, "AFP FPLogout: session 0x%04X", r->ctx->session_id);
    return AFPERR_NoErr;
}

// FPChangePassword (0x24) — correct answer for a guest-only server.
static uint32_t afp_cmd_change_password(afp_req_t *r) {
    (void)r;
    return AFPERR_CallNotSupported;
}

// FPMapID (0x15) / FPMapName (0x16) / FPGetUserInfo (0x25) — a consistent
// single-user fiction; appletalk_server.md §5 says where a real user database
// would plug in.
static uint32_t afp_cmd_map_id(afp_req_t *r) {
    if (r->in_len < 5)
        return AFPERR_ParamErr;
    uint8_t subfunc = r->in[0];
    uint32_t id = RD_BE32(r->in + 1);
    const char *name = (id == 0) ? "" : (subfunc == 1 ? "guest" : "staff");
    uint8_t name_len = (uint8_t)strlen(name);
    r->out[0] = name_len;
    if (name_len)
        memcpy(r->out + 1, name, name_len);
    r->out_len = 1 + (int)name_len;
    return AFPERR_NoErr;
}

static uint32_t afp_cmd_map_name(afp_req_t *r) {
    if (r->in_len < 1)
        return AFPERR_ParamErr;
    WR_BE32(r->out, 0); // every name maps to the guest ID
    r->out_len = 4;
    return AFPERR_NoErr;
}

static uint32_t afp_cmd_get_user_info(afp_req_t *r) {
    if (r->in_len < 5)
        return AFPERR_ParamErr;
    uint16_t bitmap = (r->in_len >= 7) ? RD_BE16(r->in + 5) : 0x0003;
    if (bitmap & ~0x0003u)
        return AFPERR_BitmapErr;
    int p = 0;
    WR_BE16(r->out + p, bitmap);
    p += 2;
    if (bitmap & 0x0001) {
        WR_BE32(r->out + p, 0);
        p += 4;
    }
    if (bitmap & 0x0002) {
        WR_BE32(r->out + p, 0);
        p += 4;
    }
    r->out_len = p;
    return AFPERR_NoErr;
}

// FPGetSrvrMsg (0x26) — AFP 2.1.  MsgType 0 = logon, 1 = server; the bitmap
// currently selects only the message string itself (AFP_21_22 p. 55).
static uint32_t afp_cmd_get_srvr_msg(afp_req_t *r) {
    if (r->in_len < 5)
        return AFPERR_ParamErr;
    uint16_t msg_type = RD_BE16(r->in + 1);
    uint16_t bitmap = RD_BE16(r->in + 3);
    if (bitmap & ~0x0001u)
        return AFPERR_BitmapErr;
    if (msg_type > 1)
        return AFPERR_ParamErr;

    uint8_t msg[AFP_META_COMMENT_MAX];
    size_t len = (size_t)afp_mac_text(g_afp_message, msg, sizeof(msg));
    WR_BE16(r->out + 0, msg_type);
    WR_BE16(r->out + 2, bitmap);
    r->out[4] = (uint8_t)len;
    if (len)
        memcpy(r->out + 5, msg, len);
    r->out_len = 5 + (int)len;
    LOG(10, "AFP FPGetSrvrMsg: type=%u len=%zu", msg_type, len);
    return AFPERR_NoErr;
}

// ============================================================================
// Directory and parameter commands
// ============================================================================

// FPOpenDir (0x19)
static uint32_t afp_cmd_open_dir(afp_req_t *r) {
    if (r->in_len < 9) // Pad VolumeID DirectoryID PathType, and an empty pathname
        return AFPERR_ParamErr;

    vol_t *vol = NULL;
    char target_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_decode_target(r, 7, &vol, target_rel, sizeof(target_rel), NULL);
    if (rc != AFPERR_NoErr)
        return rc;
    struct stat st;
    if (!afp_stat_path(vol, target_rel, &st))
        return AFPERR_ObjectNotFound;
    if (!S_ISDIR(st.st_mode))
        return AFPERR_ObjectTypeErr;
    const afp_cat_entry_t *entry = afp_entry_for(vol, target_rel);
    if (!entry)
        return AFPERR_MiscErr;
    WR_BE32(r->out, entry->cnid);
    r->out_len = 4;
    LOG(10, "AFP FPOpenDir: vol=0x%04X parent=0x%08X path='%s' → cnid=0x%08X", vol->vol_id, RD_BE32(r->in + 3),
        target_rel[0] ? target_rel : "<root>", entry->cnid);
    return AFPERR_NoErr;
}

// FPCloseDir (0x03) — the server holds no per-open directory state; the call
// exists so a client can retire a Directory ID it no longer needs.
static uint32_t afp_cmd_close_dir(afp_req_t *r) {
    if (r->in_len < 7)
        return AFPERR_ParamErr;
    uint16_t vol_id = RD_BE16(r->in + 1);
    uint32_t dir_id = RD_BE32(r->in + 3);
    vol_t *vol = afp_session_vol(r->ctx, vol_id);
    if (!vol)
        return AFPERR_ParamErr;
    if (dir_id != 0 && dir_id != AFP_CNID_ROOT && !afp_catalog_find(vol->catalog, dir_id))
        return AFPERR_ParamErr;
    return AFPERR_NoErr;
}

// FPGetFileDirParms (0x22)
static uint32_t afp_cmd_get_file_dir_parms(afp_req_t *r) {
    if (r->in_len < 11)
        return AFPERR_ParamErr;
    afp_log_hex("AFP FPGetFileDirParms req", r->in, r->in_len);
    uint16_t file_bm = RD_BE16(r->in + 7);
    uint16_t dir_bm = RD_BE16(r->in + 9);
    if (file_bm == 0 && dir_bm == 0)
        return AFPERR_BitmapErr;

    vol_t *vol = NULL;
    char target_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_decode_target(r, 11, &vol, target_rel, sizeof(target_rel), NULL);
    if (rc != AFPERR_NoErr)
        return rc;
    struct stat st;
    if (!afp_stat_path(vol, target_rel, &st))
        return AFPERR_ObjectNotFound;
    bool is_dir = S_ISDIR(st.st_mode);
    WR_BE16(r->out + 0, file_bm);
    WR_BE16(r->out + 2, dir_bm);
    r->out[4] = is_dir ? 0x80 : 0x00;
    r->out[5] = 0x00;
    int vpos = afp_emit_params(is_dir, vol, target_rel, &st, is_dir ? dir_bm : file_bm, r->out, 6, r->out_max);
    if (vpos < 0)
        return AFPERR_ParamErr;
    r->out_len = vpos;
    afp_log_hex("AFP FPGetFileDirParms resp", r->out, vpos);
    LOG(2, "AFP FPGetFileDirParms: vol=0x%04X type=%s path='%s' reply=%d", vol->vol_id, is_dir ? "dir" : "file",
        target_rel[0] ? target_rel : "<root>", vpos);
    return AFPERR_NoErr;
}

// Apply the attributes word from an FPSet*Parms call.  Bit 15 (Set/Clear)
// selects whether the named bits are set or cleared, and the same bit applies
// to all of them (appletalk_server.md FPSetFileParms details).
static void afp_apply_attribute_word(afp_meta_t *meta, uint16_t word) {
    bool set = (word & AFP_ATTR_SETCLEAR) != 0;
    uint16_t mask = (uint16_t)(word & AFP_ATTR_PERSISTED);
    uint16_t cur = meta->has_attrs ? meta->attrs : 0;
    meta->attrs = set ? (uint16_t)(cur | mask) : (uint16_t)(cur & ~mask);
    meta->has_attrs = true;
}

// FPSetFileParms / FPSetDirParms / FPSetFileDirParms (0x1D/0x1E/0x23).
// The parameter block is walked bit by bit through the same width table the
// read path uses, so a Finder Info field preceded by dates lands at the right
// offset — the old hand-rolled 0..4 switch mis-computed exactly that case.
static uint32_t afp_parse_set_parms(const afp_req_t *r) {
    if (r->in_len < 9)
        return AFPERR_ParamErr;
    uint16_t bitmap = RD_BE16(r->in + 7);

    vol_t *vol = NULL;
    char target_rel[AFP_MAX_REL_PATH];
    int pos = 0;
    uint32_t rc = afp_decode_target(r, 9, &vol, target_rel, sizeof(target_rel), &pos);
    if (rc != AFPERR_NoErr)
        return rc;
    struct stat st;
    if (!afp_stat_path(vol, target_rel, &st))
        return AFPERR_ObjectNotFound;
    bool is_dir = S_ISDIR(st.st_mode);
    char full[PATH_MAX];
    if (!afp_host_path(vol, target_rel, full, sizeof(full)))
        return AFPERR_ParamErr;

    // Only the settable bits may appear; the rest are read-only parameters.
    const uint16_t settable = (1u << 0) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 13);
    if (bitmap == 0 || (bitmap & ~settable))
        return AFPERR_BitmapErr;

    // A null byte may follow the pathname to align the parameter block on an
    // even boundary of the AFP command block.  Rather than re-deriving that
    // boundary (our buffer starts one byte past the opcode), detect the pad
    // from the byte count the bitmap accounts for.
    int need = afp_fixed_param_len(is_dir, bitmap);
    int avail = r->in_len - pos;
    if (avail == need + 1)
        pos++;
    else if (avail < need)
        return AFPERR_ParamErr;

    afp_meta_t meta;
    afp_meta_load(full, &meta);
    bool touched = false;
    bool set_mtime = false;
    uint32_t new_mtime = 0;

    for (int bit = 0; bit < 16; bit++) {
        if (!(bitmap & (1u << bit)))
            continue;
        int width = afp_param_field_width(is_dir, bit);
        if (width == 0)
            continue;
        if (pos + width > r->in_len)
            return AFPERR_ParamErr;
        const uint8_t *field = r->in + pos;
        pos += width;
        switch (bit) {
        case 0: // Attributes
            afp_apply_attribute_word(&meta, RD_BE16(field));
            touched = true;
            break;
        case 2: // Creation date
            meta.create_date = RD_BE32(field);
            meta.has_dates = true;
            touched = true;
            break;
        case 3: // Modification date — written through to the host too
            new_mtime = RD_BE32(field);
            set_mtime = true;
            meta.modify_date = new_mtime;
            meta.has_dates = true;
            touched = true;
            break;
        case 4: // Backup date
            meta.backup_date = RD_BE32(field);
            meta.has_dates = true;
            touched = true;
            break;
        case 5: // Finder Info
            memcpy(meta.finder, field, AFP_META_FINDER_SIZE);
            meta.has_finder = true;
            touched = true;
            break;
        case 13: // ProDOS info — accepted and ignored, as a Mac server does
            break;
        default:
            break;
        }
    }

    if (touched) {
        if (!meta.has_dates) {
            meta.create_date = afp_unix_time_to_afp(st.st_mtime);
            meta.modify_date = afp_unix_time_to_afp(st.st_mtime);
            meta.backup_date = AFP_DATE_NEVER;
        }
        if (afp_meta_update(full, &meta) != 0) {
            LOG(1, "AFP SetParms: cannot write metadata for '%s'", target_rel);
            return AFPERR_AccessDenied;
        }
    }
    if (set_mtime) {
        // Keep the host and the Mac in agreement about the modification time.
        struct timeval times[2];
        int64_t secs = afp_meta_time_to_unix(new_mtime);
        times[0].tv_sec = (time_t)secs;
        times[0].tv_usec = 0;
        times[1] = times[0];
        if (utimes(full, times) != 0)
            LOG(2, "AFP SetParms: utimes('%s') failed (%s)", target_rel, strerror(errno));
    }
    LOG(2, "AFP SetParms: vol=0x%04X bitmap=0x%04X path='%s'", vol->vol_id, bitmap,
        target_rel[0] ? target_rel : "<root>");
    return AFPERR_NoErr;
}

static uint32_t afp_cmd_set_file_parms(afp_req_t *r) {
    return afp_parse_set_parms(r);
}

static uint32_t afp_cmd_set_dir_parms(afp_req_t *r) {
    return afp_parse_set_parms(r);
}

static uint32_t afp_cmd_set_file_dir_parms(afp_req_t *r) {
    return afp_parse_set_parms(r);
}

// ============================================================================
// Fork commands
// ============================================================================

// FPOpenFork (0x1A)
static uint32_t afp_cmd_open_fork(afp_req_t *r) {
    if (r->in_len < 12)
        return AFPERR_ParamErr;
    bool is_resource = (r->in[0] & 0x80) != 0;
    uint16_t vol_id = RD_BE16(r->in + 1);
    uint16_t bitmap = RD_BE16(r->in + 7);
    uint16_t access_mode = RD_BE16(r->in + 9);

    vol_t *vol = NULL;
    char target_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_decode_target(r, 11, &vol, target_rel, sizeof(target_rel), NULL);
    if (rc != AFPERR_NoErr)
        return rc;
    struct stat st;
    if (!afp_stat_path(vol, target_rel, &st))
        return AFPERR_ObjectNotFound;
    if (S_ISDIR(st.st_mode))
        return AFPERR_ObjectTypeErr;
    char full[PATH_MAX];
    if (!afp_host_path(vol, target_rel, full, sizeof(full)))
        return AFPERR_ParamErr;
    // A WriteInhibit file may still be read, never written.
    if ((access_mode & AFP_ACCESS_WRITE) && afp_inhibited(full, AFP_ATTR_WRITEINHIBIT))
        return AFPERR_AccessDenied;

    afp_fork_t *fk = NULL;
    afp_fork_status_t st_open =
        afp_fork_open(vol_id, r->ctx->session_id, full, target_rel, is_resource, access_mode, &fk);
    // On DenyConflict the client still gets the file parameters, so it can
    // work out whether it is the holder (Inside AppleTalk ch. 13, FPOpenFork).
    if (st_open != AFP_FORK_OK && st_open != AFP_FORK_DENY_CONFLICT)
        return afp_fork_status_to_err(st_open);

    afp_catalog_resolve_path(vol->catalog, target_rel, true, false);
    WR_BE16(r->out + 0, bitmap);
    WR_BE16(r->out + 2, fk ? afp_fork_ref(fk) : 0);
    int p = afp_emit_params(false, vol, target_rel, &st, bitmap, r->out, 4, r->out_max);
    if (p < 0) {
        if (fk)
            afp_fork_close(fk);
        return AFPERR_ParamErr;
    }
    r->out_len = p;
    if (st_open == AFP_FORK_DENY_CONFLICT) {
        LOG(2, "AFP FPOpenFork: deny conflict on '%s' (%s)", target_rel, is_resource ? "rsrc" : "data");
        return AFPERR_DenyConflict;
    }
    LOG(2, "AFP FPOpenFork: vol=0x%04X %s path='%s' → ref=0x%04X reply=%d", vol_id, is_resource ? "rsrc" : "data",
        target_rel, afp_fork_ref(fk), p);
    return AFPERR_NoErr;
}

// FPCloseFork (0x04)
static uint32_t afp_cmd_close_fork(afp_req_t *r) {
    afp_fork_t *fk = NULL;
    uint32_t rc = afp_fork_from_req(r, 0, &fk);
    if (rc != AFPERR_NoErr)
        return rc;
    afp_fork_close(fk);
    return AFPERR_NoErr;
}

// FPRead (0x1B)
static uint32_t afp_cmd_read(afp_req_t *r) {
    if (r->in_len < 11)
        return AFPERR_ParamErr;
    uint32_t offset = RD_BE32(r->in + 3);
    uint32_t req_count = RD_BE32(r->in + 7);
    uint8_t newline_mask = (r->in_len > 11) ? r->in[11] : 0;
    uint8_t newline_char = (r->in_len > 12) ? r->in[12] : 0;

    afp_fork_t *fk = NULL;
    uint32_t rc = afp_fork_from_req(r, AFP_ACCESS_READ, &fk);
    if (rc != AFPERR_NoErr)
        return rc;

    uint32_t fork_len = afp_fork_length(fk);
    if (offset >= fork_len)
        return AFPERR_EOFErr;
    uint32_t to_read = req_count;
    if (to_read > fork_len - offset)
        to_read = fork_len - offset;
    if (to_read > (uint32_t)r->out_max)
        to_read = (uint32_t)r->out_max;

    uint32_t got = 0;
    afp_fork_status_t st = afp_fork_read(fk, offset, to_read, r->out, &got);
    if (st != AFP_FORK_OK)
        return afp_fork_status_to_err(st);

    // Newline mode: stop at the first byte matching under the mask.
    if (newline_mask) {
        for (uint32_t i = 0; i < got; i++) {
            if ((r->out[i] & newline_mask) == (newline_char & newline_mask)) {
                got = i + 1;
                break;
            }
        }
    }
    g_afp_stats.bytes_read += got;
    r->out_len = (int)got;
    LOG(2, "AFP FPRead: ref=0x%04X off=%u req=%u got=%u", afp_fork_ref(fk), offset, req_count, got);
    if (got < req_count && offset + got >= fork_len)
        return AFPERR_EOFErr;
    return AFPERR_NoErr;
}

// FPWrite (0x21) — the payload follows the 11-byte parameter header.
static uint32_t afp_cmd_write(afp_req_t *r) {
    if (r->in_len < 11)
        return AFPERR_ParamErr;
    bool from_end = (r->in[0] & 0x80) != 0;
    uint32_t offset = RD_BE32(r->in + 3);
    uint32_t req_count = RD_BE32(r->in + 7);

    afp_fork_t *fk = NULL;
    uint32_t rc = afp_fork_from_req(r, AFP_ACCESS_WRITE, &fk);
    if (rc != AFPERR_NoErr)
        return rc;

    const uint8_t *payload = r->in + 11;
    uint32_t payload_len = (uint32_t)(r->in_len - 11);
    // A partial write is legal: the transport may split a large write across
    // ASP requests, and the reply's LastWritten is exactly how the client
    // learns where to resume (appletalk_server.md FPWrite details).  What is
    // not legal is a non-zero ReqCount carrying no payload at all — that is a
    // truncated request, and writing nothing while reporting success would
    // stall the client forever.
    if (req_count > 0 && payload_len == 0)
        return AFPERR_ParamErr;
    uint32_t to_write = payload_len < req_count ? payload_len : req_count;

    uint32_t start = offset;
    if (from_end) {
        uint32_t len = afp_fork_length(fk);
        int64_t abs = (int64_t)len + (int32_t)offset;
        if (abs < 0)
            return AFPERR_ParamErr;
        start = (uint32_t)abs;
    }

    uint32_t written = 0;
    afp_fork_status_t st = afp_fork_write(fk, start, payload, to_write, &written);
    if (st != AFP_FORK_OK)
        return afp_fork_status_to_err(st);
    g_afp_stats.bytes_written += written;
    WR_BE32(r->out, start + written);
    r->out_len = 4;
    LOG(2, "AFP FPWrite: ref=0x%04X off=%u req=%u wrote=%u", afp_fork_ref(fk), start, req_count, written);
    return AFPERR_NoErr;
}

// FPGetForkParms (0x0E)
static uint32_t afp_cmd_get_fork_parms(afp_req_t *r) {
    if (r->in_len < 5)
        return AFPERR_ParamErr;
    uint16_t bitmap = RD_BE16(r->in + 3);
    afp_fork_t *fk = NULL;
    uint32_t rc = afp_fork_from_req(r, 0, &fk);
    if (rc != AFPERR_NoErr)
        return rc;
    vol_t *vol = find_vol_by_id(afp_fork_vol_id(fk));
    if (!vol)
        return AFPERR_ParamErr;
    struct stat st;
    if (stat(afp_fork_host_path(fk), &st) != 0)
        return AFPERR_ObjectNotFound;

    WR_BE16(r->out + 0, bitmap);
    int p = afp_emit_params(false, vol, afp_fork_rel_path(fk), &st, bitmap, r->out, 2, r->out_max);
    if (p < 0)
        return AFPERR_ParamErr;
    r->out_len = p;
    return AFPERR_NoErr;
}

// FPSetForkParms (0x1F) — the only settable parameter is the fork length.
static uint32_t afp_cmd_set_fork_parms(afp_req_t *r) {
    if (r->in_len < 9)
        return AFPERR_ParamErr;
    uint16_t bitmap = RD_BE16(r->in + 3);
    afp_fork_t *fk = NULL;
    uint32_t rc = afp_fork_from_req(r, 0, &fk);
    if (rc != AFPERR_NoErr)
        return rc;
    if (bitmap & ~((1u << 9) | (1u << 10)))
        return AFPERR_BitmapErr;
    if (!(bitmap & ((1u << 9) | (1u << 10))))
        return AFPERR_BitmapErr;
    uint32_t new_len = RD_BE32(r->in + 5);
    afp_fork_status_t st = afp_fork_truncate(fk, new_len);
    if (st != AFP_FORK_OK)
        return afp_fork_status_to_err(st);
    LOG(10, "AFP FPSetForkParms: ref=0x%04X len=%u", afp_fork_ref(fk), new_len);
    return AFPERR_NoErr;
}

// FPFlush (0x0A)
static uint32_t afp_cmd_flush(afp_req_t *r) {
    if (r->in_len < 3)
        return AFPERR_ParamErr;
    uint16_t vol_id = RD_BE16(r->in + 1);
    if (!afp_session_vol(r->ctx, vol_id))
        return AFPERR_ParamErr;
    afp_fork_flush_volume(vol_id);
    return AFPERR_NoErr;
}

// FPFlushFork (0x0B)
static uint32_t afp_cmd_flush_fork(afp_req_t *r) {
    afp_fork_t *fk = NULL;
    uint32_t rc = afp_fork_from_req(r, 0, &fk);
    if (rc != AFPERR_NoErr)
        return rc;
    afp_fork_flush(fk);
    return AFPERR_NoErr;
}

// FPByteRangeLock (0x01) — real ranges now, checked against every other open
// of the same fork.
static uint32_t afp_cmd_byte_range_lock(afp_req_t *r) {
    if (r->in_len < 11)
        return AFPERR_ParamErr;
    uint8_t flag = r->in[0];
    bool unlock = (flag & 0x01) != 0; // bit 0: 0 = lock, 1 = unlock
    bool end_relative = (flag & 0x80) != 0; // bit 7: offset measured from EOF
    int32_t offset = (int32_t)RD_BE32(r->in + 3);
    uint32_t length = RD_BE32(r->in + 7);

    afp_fork_t *fk = NULL;
    uint32_t rc = afp_fork_from_req(r, 0, &fk);
    if (rc != AFPERR_NoErr)
        return rc;
    uint32_t range_start = 0;
    afp_fork_status_t st = afp_fork_range_lock(fk, unlock, end_relative, offset, length, &range_start);
    if (st != AFP_FORK_OK)
        return afp_fork_status_to_err(st);
    WR_BE32(r->out, range_start);
    r->out_len = 4;
    LOG(10, "AFP FPByteRangeLock: ref=0x%04X %s start=%u len=%u", afp_fork_ref(fk), unlock ? "unlock" : "lock",
        range_start, length);
    return AFPERR_NoErr;
}

// ============================================================================
// File and directory mutations
// ============================================================================

// Move a file's AppleDouble sidecar alongside it.
static void afp_sidecar_rename(const char *old_full, const char *new_full) {
    char old_sc[PATH_MAX], new_sc[PATH_MAX];
    if (afp_meta_sidecar_path(old_full, old_sc, sizeof(old_sc)) &&
        afp_meta_sidecar_path(new_full, new_sc, sizeof(new_sc)))
        rename(old_sc, new_sc);
}

// FPCreateDir (0x06)
static uint32_t afp_cmd_create_dir(afp_req_t *r) {
    if (r->in_len < 8)
        return AFPERR_ParamErr;

    vol_t *vol = NULL;
    char target_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_decode_target(r, 7, &vol, target_rel, sizeof(target_rel), NULL);
    if (rc != AFPERR_NoErr)
        return rc;
    if (!target_rel[0])
        return AFPERR_ParamErr;
    char full[PATH_MAX];
    if (!afp_host_path(vol, target_rel, full, sizeof(full)))
        return AFPERR_ParamErr;
    struct stat st;
    if (stat(full, &st) == 0)
        return AFPERR_ObjectExists;
    if (mkdir(full, 0755) != 0)
        return errno == ENOSPC ? AFPERR_DiskFull : AFPERR_AccessDenied;

    char parent_rel[AFP_MAX_REL_PATH];
    afp_extract_parent(target_rel, parent_rel, sizeof(parent_rel));
    const afp_cat_entry_t *parent = afp_entry_for(vol, parent_rel);
    uint32_t parent_cnid = parent ? parent->cnid : AFP_CNID_ROOT;
    const afp_cat_entry_t *entry = afp_catalog_add(vol->catalog, parent_cnid, afp_last_component(target_rel), true);
    if (!entry)
        return AFPERR_MiscErr;
    afp_vol_touch(vol);
    WR_BE32(r->out, entry->cnid);
    r->out_len = 4;
    LOG(10, "AFP FPCreateDir: '%s' → cnid=0x%08X", target_rel, entry->cnid);
    return AFPERR_NoErr;
}

// FPCreateFile (0x07) — flag bit 7 selects a hard create (overwrite).
static uint32_t afp_cmd_create_file(afp_req_t *r) {
    if (r->in_len < 8)
        return AFPERR_ParamErr;
    bool hard_create = (r->in[0] & 0x80) != 0;

    vol_t *vol = NULL;
    char target_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_decode_target(r, 7, &vol, target_rel, sizeof(target_rel), NULL);
    if (rc != AFPERR_NoErr)
        return rc;
    if (!target_rel[0])
        return AFPERR_ParamErr;
    char full[PATH_MAX];
    if (!afp_host_path(vol, target_rel, full, sizeof(full)))
        return AFPERR_ParamErr;

    struct stat st;
    bool exists = stat(full, &st) == 0;
    if (exists && S_ISDIR(st.st_mode))
        return AFPERR_ObjectTypeErr;
    if (exists) {
        if (!hard_create)
            return AFPERR_ObjectExists;
        if (afp_fork_path_busy(full))
            return AFPERR_FileBusy;
        if (afp_inhibited(full, AFP_ATTR_WRITEINHIBIT))
            return AFPERR_ObjectLocked;
    }
    FILE *f = fopen(full, "wb");
    if (!f)
        return errno == ENOSPC ? AFPERR_DiskFull : AFPERR_AccessDenied;
    fclose(f);
    if (exists) {
        // A hard create resets the file completely, metadata included.
        char sidecar[PATH_MAX];
        if (afp_meta_sidecar_path(full, sidecar, sizeof(sidecar)))
            remove(sidecar);
    }

    // A newly created file gets its dates from the server clock and a backup
    // date of "never" (appletalk_server.md FPCreateFile details).
    afp_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.has_dates = true;
    meta.create_date = afp_unix_time_to_afp(time(NULL));
    meta.modify_date = meta.create_date;
    meta.backup_date = AFP_DATE_NEVER;
    afp_meta_update(full, &meta);

    char parent_rel[AFP_MAX_REL_PATH];
    afp_extract_parent(target_rel, parent_rel, sizeof(parent_rel));
    const afp_cat_entry_t *parent = afp_entry_for(vol, parent_rel);
    uint32_t parent_cnid = parent ? parent->cnid : AFP_CNID_ROOT;
    afp_catalog_add(vol->catalog, parent_cnid, afp_last_component(target_rel), false);
    afp_vol_touch(vol);
    LOG(10, "AFP FPCreateFile: '%s' hard=%d", target_rel, hard_create ? 1 : 0);
    return AFPERR_NoErr;
}

// FPDelete (0x08)
static uint32_t afp_cmd_delete(afp_req_t *r) {
    if (r->in_len < 8)
        return AFPERR_ParamErr;

    vol_t *vol = NULL;
    char target_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_decode_target(r, 7, &vol, target_rel, sizeof(target_rel), NULL);
    if (rc != AFPERR_NoErr)
        return rc;
    if (!target_rel[0])
        return AFPERR_AccessDenied; // the volume root is not deletable
    char full[PATH_MAX];
    if (!afp_host_path(vol, target_rel, full, sizeof(full)))
        return AFPERR_ParamErr;
    struct stat st;
    if (stat(full, &st) != 0)
        return AFPERR_ObjectNotFound;
    if (afp_inhibited(full, AFP_ATTR_DELETEINHIBIT))
        return AFPERR_ObjectLocked;

    if (S_ISDIR(st.st_mode)) {
        // Emptiness is judged the way a client sees the directory: sidecars
        // and the control directory are metadata, not files.
        DIR *dir = opendir(full);
        if (!dir)
            return AFPERR_AccessDenied;
        bool empty = true;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            if (afp_meta_is_hidden(ent->d_name))
                continue;
            empty = false;
            break;
        }
        if (!empty) {
            closedir(dir);
            return AFPERR_DirNotEmpty;
        }
        rewinddir(dir);
        while ((ent = readdir(dir)) != NULL) {
            if (!afp_meta_is_hidden(ent->d_name))
                continue;
            char child_rel[AFP_MAX_REL_PATH], child[PATH_MAX];
            if (afp_build_child_path(target_rel, ent->d_name, child_rel, sizeof(child_rel)) &&
                afp_host_path(vol, child_rel, child, sizeof(child)))
                remove(child);
        }
        closedir(dir);
        if (rmdir(full) != 0)
            return AFPERR_AccessDenied;
    } else {
        // A file with any fork open is busy; the client must close it first.
        if (afp_fork_path_busy(full))
            return AFPERR_FileBusy;
        if (unlink(full) != 0)
            return AFPERR_AccessDenied;
        char sidecar[PATH_MAX];
        if (afp_meta_sidecar_path(full, sidecar, sizeof(sidecar)))
            remove(sidecar);
    }

    const afp_cat_entry_t *entry = afp_catalog_resolve_path(vol->catalog, target_rel, false, false);
    if (entry)
        afp_catalog_remove(vol->catalog, entry->cnid);
    afp_vol_touch(vol);
    LOG(10, "AFP FPDelete: '%s' (%s)", target_rel, S_ISDIR(st.st_mode) ? "dir" : "file");
    return AFPERR_NoErr;
}

// FPRename (0x1C) — same parent, new name; the CNID is preserved.
static uint32_t afp_cmd_rename(afp_req_t *r) {
    if (r->in_len < 8)
        return AFPERR_ParamErr;
    uint16_t vol_id = RD_BE16(r->in + 1);
    uint32_t dir_id = RD_BE32(r->in + 3);
    afp_path_t old_name;
    int pos = afp_read_path(r->in, r->in_len, 7, &old_name);
    if (pos < 0 || pos >= r->in_len)
        return AFPERR_ParamErr;
    // The new name is one element, decoded like any other: "..", a name with a
    // separator in it, or one of the server's own names is a bad NewName.  It
    // went straight into a host path join, so "../../x" renamed a file out of
    // the share.
    afp_path_t new_path;
    char new_name[AFP_MAX_NAME * 3 + 1];
    if (afp_read_path(r->in, r->in_len, pos, &new_path) < 0 || !afp_parse_leaf(&new_path, new_name, sizeof(new_name)))
        return AFPERR_ParamErr;

    vol_t *vol = NULL;
    char old_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_target(r->ctx, vol_id, dir_id, &old_name, &vol, old_rel, sizeof(old_rel));
    if (rc != AFPERR_NoErr)
        return rc;
    if (!old_rel[0])
        return AFPERR_CantRename; // renaming the volume itself is not supported
    char old_full[PATH_MAX];
    if (!afp_host_path(vol, old_rel, old_full, sizeof(old_full)))
        return AFPERR_ParamErr;
    struct stat st;
    if (stat(old_full, &st) != 0)
        return AFPERR_ObjectNotFound;
    if (afp_inhibited(old_full, AFP_ATTR_RENAMEINHIBIT))
        return AFPERR_ObjectLocked;

    char parent_rel[AFP_MAX_REL_PATH];
    afp_extract_parent(old_rel, parent_rel, sizeof(parent_rel));
    char new_rel[AFP_MAX_REL_PATH];
    if (!afp_build_child_path(parent_rel, new_name, new_rel, sizeof(new_rel)))
        return AFPERR_ParamErr;
    char new_full[PATH_MAX];
    if (!afp_host_path(vol, new_rel, new_full, sizeof(new_full)))
        return AFPERR_ParamErr;
    if (strcmp(old_rel, new_rel) == 0)
        return AFPERR_NoErr;
    if (afp_name_taken(vol, parent_rel, new_name, old_rel))
        return AFPERR_ObjectExists;
    if (rename(old_full, new_full) != 0)
        return AFPERR_CantRename;
    afp_sidecar_rename(old_full, new_full);
    afp_fork_repoint(old_full, new_full, new_rel);

    const afp_cat_entry_t *entry = afp_catalog_resolve_path(vol->catalog, old_rel, true, S_ISDIR(st.st_mode));
    if (entry)
        afp_catalog_rename(vol->catalog, entry->cnid, afp_last_component(new_rel));

    afp_vol_touch(vol);
    LOG(10, "AFP FPRename: '%s' → '%s'", old_rel, new_rel);
    return AFPERR_NoErr;
}

// FPMoveAndRename (0x17) — new parent and optionally a new name; the CNID and
// every descendant CNID survive the move.
static uint32_t afp_cmd_move_and_rename(afp_req_t *r) {
    if (r->in_len < 12)
        return AFPERR_ParamErr;
    uint16_t vol_id = RD_BE16(r->in + 1);
    uint32_t src_dir_id = RD_BE32(r->in + 3);
    uint32_t dst_dir_id = RD_BE32(r->in + 7);
    afp_path_t src_path;
    int pos = afp_read_path(r->in, r->in_len, 11, &src_path);
    if (pos < 0 || pos >= r->in_len)
        return AFPERR_ParamErr;
    afp_path_t dst_path;
    pos = afp_read_path(r->in, r->in_len, pos, &dst_path);
    if (pos < 0)
        return AFPERR_ParamErr;
    // An optional new name, checked like FPRename's.
    char new_name[AFP_MAX_NAME * 3 + 1] = "";
    if (pos < r->in_len) {
        afp_path_t new_path;
        if (afp_read_path(r->in, r->in_len, pos, &new_path) < 0)
            return AFPERR_ParamErr;
        if (new_path.len > 0 && !afp_parse_leaf(&new_path, new_name, sizeof(new_name)))
            return AFPERR_ParamErr;
    }

    vol_t *vol = NULL;
    char src_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_target(r->ctx, vol_id, src_dir_id, &src_path, &vol, src_rel, sizeof(src_rel));
    if (rc != AFPERR_NoErr)
        return rc;
    if (!src_rel[0])
        return AFPERR_CantMove;
    char dst_dir_rel[AFP_MAX_REL_PATH];
    rc = afp_resolve_target(r->ctx, vol_id, dst_dir_id, &dst_path, &vol, dst_dir_rel, sizeof(dst_dir_rel));
    if (rc != AFPERR_NoErr)
        return rc;

    char src_full[PATH_MAX];
    if (!afp_host_path(vol, src_rel, src_full, sizeof(src_full)))
        return AFPERR_ParamErr;
    struct stat st;
    if (stat(src_full, &st) != 0)
        return AFPERR_ObjectNotFound;
    if (afp_inhibited(src_full, AFP_ATTR_RENAMEINHIBIT))
        return AFPERR_ObjectLocked;

    const char *final_name = new_name[0] ? new_name : afp_last_component(src_rel);
    if (!final_name)
        return AFPERR_ParamErr;
    char dst_rel[AFP_MAX_REL_PATH];
    if (!afp_build_child_path(dst_dir_rel, final_name, dst_rel, sizeof(dst_rel)))
        return AFPERR_ParamErr;
    // Moving a directory into itself would detach the subtree.
    if (S_ISDIR(st.st_mode) && strncmp(dst_rel, src_rel, strlen(src_rel)) == 0 &&
        (dst_rel[strlen(src_rel)] == '/' || dst_rel[strlen(src_rel)] == '\0'))
        return AFPERR_CantMove;
    char dst_full[PATH_MAX];
    if (!afp_host_path(vol, dst_rel, dst_full, sizeof(dst_full)))
        return AFPERR_ParamErr;
    if (afp_name_taken(vol, dst_dir_rel, final_name, src_rel))
        return AFPERR_ObjectExists;
    if (rename(src_full, dst_full) != 0)
        return AFPERR_CantMove;
    afp_sidecar_rename(src_full, dst_full);
    afp_fork_repoint(src_full, dst_full, dst_rel);

    // Resolve both ends to CNIDs before mutating: adoption can grow the
    // catalog and invalidate an entry pointer taken before it.
    const afp_cat_entry_t *entry = afp_catalog_resolve_path(vol->catalog, src_rel, true, S_ISDIR(st.st_mode));
    uint32_t moved_cnid = entry ? entry->cnid : 0;
    const afp_cat_entry_t *new_parent = afp_entry_for(vol, dst_dir_rel);
    uint32_t new_parent_cnid = new_parent ? new_parent->cnid : AFP_CNID_ROOT;
    if (moved_cnid)
        afp_catalog_move(vol->catalog, moved_cnid, new_parent_cnid, final_name);
    afp_vol_touch(vol);
    LOG(10, "AFP FPMoveAndRename: '%s' → '%s'", src_rel, dst_rel);
    return AFPERR_NoErr;
}

// Copy one host file's bytes.  Returns an AFP result code.
static uint32_t afp_copy_bytes(const char *src, const char *dst) {
    FILE *fin = fopen(src, "rb");
    if (!fin)
        return AFPERR_ObjectNotFound;
    FILE *fout = fopen(dst, "wb");
    if (!fout) {
        fclose(fin);
        return AFPERR_AccessDenied;
    }
    uint8_t buf[64 * 1024];
    size_t n;
    uint32_t rc = AFPERR_NoErr;
    while ((n = fread(buf, 1, sizeof(buf), fin)) > 0) {
        if (fwrite(buf, 1, n, fout) != n) {
            rc = AFPERR_DiskFull;
            break;
        }
    }
    fclose(fin);
    if (fclose(fout) != 0 && rc == AFPERR_NoErr)
        rc = AFPERR_DiskFull;
    if (rc != AFPERR_NoErr)
        remove(dst);
    return rc;
}

// FPCopyFile (0x05) — a server-side copy of both forks and all metadata.
static uint32_t afp_cmd_copy_file(afp_req_t *r) {
    if (r->in_len < 14)
        return AFPERR_ParamErr;
    // Pad(1) SrcVolID(2) SrcDirID(4) DstVolID(2) DstDirID(4) — the two volume
    // IDs are not adjacent; the source's directory ID sits between them.
    uint16_t src_vol_id = RD_BE16(r->in + 1);
    uint32_t src_dir = RD_BE32(r->in + 3);
    uint16_t dst_vol_id = RD_BE16(r->in + 7);
    uint32_t dst_dir = RD_BE32(r->in + 9);
    afp_path_t src_name;
    int pos = afp_read_path(r->in, r->in_len, 13, &src_name);
    if (pos < 0 || pos >= r->in_len)
        return AFPERR_ParamErr;
    afp_path_t dst_name;
    pos = afp_read_path(r->in, r->in_len, pos, &dst_name);
    if (pos < 0)
        return AFPERR_ParamErr;
    // An optional new name, checked like FPRename's.
    char new_name[AFP_MAX_NAME * 3 + 1] = "";
    if (pos < r->in_len) {
        afp_path_t new_path;
        if (afp_read_path(r->in, r->in_len, pos, &new_path) < 0)
            return AFPERR_ParamErr;
        if (new_path.len > 0 && !afp_parse_leaf(&new_path, new_name, sizeof(new_name)))
            return AFPERR_ParamErr;
    }

    vol_t *svol = NULL, *dvol = NULL;
    char src_rel[AFP_MAX_REL_PATH], dst_dir_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_target(r->ctx, src_vol_id, src_dir, &src_name, &svol, src_rel, sizeof(src_rel));
    if (rc != AFPERR_NoErr)
        return rc;
    rc = afp_resolve_target(r->ctx, dst_vol_id, dst_dir, &dst_name, &dvol, dst_dir_rel, sizeof(dst_dir_rel));
    if (rc != AFPERR_NoErr)
        return rc;

    char src_full[PATH_MAX];
    if (!afp_host_path(svol, src_rel, src_full, sizeof(src_full)))
        return AFPERR_ParamErr;
    struct stat st;
    if (stat(src_full, &st) != 0)
        return AFPERR_ObjectNotFound;
    if (S_ISDIR(st.st_mode))
        return AFPERR_ObjectTypeErr;

    const char *final_name = new_name[0] ? new_name : afp_last_component(src_rel);
    if (!final_name)
        return AFPERR_ParamErr;
    char dst_rel[AFP_MAX_REL_PATH];
    if (!afp_build_child_path(dst_dir_rel, final_name, dst_rel, sizeof(dst_rel)))
        return AFPERR_ParamErr;
    char dst_full[PATH_MAX];
    if (!afp_host_path(dvol, dst_rel, dst_full, sizeof(dst_full)))
        return AFPERR_ParamErr;
    if (afp_name_taken(dvol, dst_dir_rel, final_name, NULL))
        return AFPERR_ObjectExists;

    // The source is held for reading with writers denied for the duration of
    // the copy (Inside AppleTalk ch. 13, FPCopyFile: "Read, DenyWrite").
    afp_fork_t *guard = NULL;
    afp_fork_status_t gs = afp_fork_open(src_vol_id, r->ctx->session_id, src_full, src_rel, false,
                                         AFP_ACCESS_READ | AFP_DENY_WRITE, &guard);
    if (gs == AFP_FORK_DENY_CONFLICT)
        return AFPERR_DenyConflict;
    if (gs != AFP_FORK_OK)
        return afp_fork_status_to_err(gs);

    rc = afp_copy_bytes(src_full, dst_full);
    if (rc == AFPERR_NoErr) {
        char src_sc[PATH_MAX], dst_sc[PATH_MAX];
        if (afp_meta_sidecar_path(src_full, src_sc, sizeof(src_sc)) &&
            afp_meta_sidecar_path(dst_full, dst_sc, sizeof(dst_sc)) && access(src_sc, R_OK) == 0)
            afp_copy_bytes(src_sc, dst_sc); // forks, Finder Info, dates, comment
    }
    afp_fork_close(guard);
    if (rc != AFPERR_NoErr)
        return rc;

    const afp_cat_entry_t *parent = afp_entry_for(dvol, dst_dir_rel);
    uint32_t parent_cnid = parent ? parent->cnid : AFP_CNID_ROOT;
    afp_catalog_add(dvol->catalog, parent_cnid, final_name, false);
    afp_vol_touch(dvol);
    LOG(10, "AFP FPCopyFile: '%s' → '%s'", src_rel, dst_rel);
    return AFPERR_NoErr;
}

// ============================================================================
// Desktop database
// ============================================================================

// A volume's DTRefNum is its volume ID: one per volume, never 0, and never
// handed out twice -- unlike the counter it replaces, which wrapped to 0.
static uint16_t afp_dt_ref(const vol_t *v) {
    return v->vol_id;
}

// Resolve a client's DTRefNum to its volume, if this session opened it
// (FPOpenDT); ParamErr for another session's refnum, as for a fork's.
static vol_t *find_vol_by_dt_ref(const afp_ctx_t *ctx, uint16_t dt_ref) {
    vol_t *v = find_vol_by_id(dt_ref);
    return (v && session_set_has(&v->dt_open_by, ctx->session_id)) ? v : NULL;
}

// FPOpenDT (0x30)
static uint32_t afp_cmd_open_dt(afp_req_t *r) {
    if (r->in_len < 3)
        return AFPERR_ParamErr;
    vol_t *v = afp_session_vol(r->ctx, RD_BE16(r->in + 1));
    if (!v)
        return AFPERR_ParamErr;
    if (!v->desktop)
        v->desktop = afp_desktop_open(v->root);
    session_set_add(&v->dt_open_by, r->ctx->session_id);
    WR_BE16(r->out, afp_dt_ref(v));
    r->out_len = 2;
    LOG(10, "AFP FPOpenDT: vol='%s' → DTRef=0x%04X", v->name, afp_dt_ref(v));
    return AFPERR_NoErr;
}

// FPCloseDT (0x31) — the stores stay open (and persistent); only the client's
// reference goes away.
static uint32_t afp_cmd_close_dt(afp_req_t *r) {
    if (r->in_len < 3)
        return AFPERR_ParamErr;
    vol_t *v = find_vol_by_dt_ref(r->ctx, RD_BE16(r->in + 1));
    if (!v)
        return AFPERR_ParamErr;
    session_set_remove(&v->dt_open_by, r->ctx->session_id); // another session's refnum stays open
    return AFPERR_NoErr;
}

// FPAddIcon (0xC0) — arrives as an ASP Write, so the bitmap follows the header.
static uint32_t afp_cmd_add_icon(afp_req_t *r) {
    // Pad(1) DTRefNum(2) FileCreator(4) FileType(4) IconType(1) Pad(1)
    // IconTag(4) BitmapSize(2), then the bitmap streamed via ASP Write.
    if (r->in_len < 19)
        return AFPERR_ParamErr;
    uint16_t dt_ref = RD_BE16(r->in + 1);
    uint32_t creator = RD_BE32(r->in + 3);
    uint32_t file_type = RD_BE32(r->in + 7);
    uint8_t icon_type = r->in[11];
    uint32_t icon_tag = RD_BE32(r->in + 13);
    uint16_t icon_size = RD_BE16(r->in + 17);

    vol_t *v = find_vol_by_dt_ref(r->ctx, dt_ref);
    if (!v || !v->desktop)
        return AFPERR_ParamErr;
    const uint8_t *data = r->in + 19;
    if (icon_size > AFP_ICON_MAX_BYTES)
        return AFPERR_IconTypeError;
    // A bitmap shorter than its BitmapSize is a bad request, as a short FPWrite
    // is -- not a smaller icon.
    if (icon_size > r->in_len - 19)
        return AFPERR_ParamErr;
    // Replacing an existing icon with one of a different size is an error,
    // not a silent resize (appletalk_server.md FPAddIcon details).
    const afp_icon_t *existing = afp_desktop_get_icon(v->desktop, creator, file_type, icon_type);
    if (existing && existing->size != icon_size)
        return AFPERR_IconTypeError;
    if (afp_desktop_put_icon(v->desktop, creator, file_type, icon_type, icon_tag, data, icon_size) != 0)
        return AFPERR_MiscErr;
    LOG(10, "AFP FPAddIcon: creator=0x%08X type=0x%08X iconType=%u size=%u", creator, file_type, icon_type, icon_size);
    return AFPERR_NoErr;
}

// FPGetIcon (0x33)
static uint32_t afp_cmd_get_icon(afp_req_t *r) {
    // Pad(1) DTRefNum(2) FileCreator(4) FileType(4) IconType(1) Pad(1)
    // Length(2) -- Inside AppleTalk p. 13-92.  Length was once read from the
    // pad: 256 asked for 1 byte, 128 got 256.
    if (r->in_len < 15)
        return AFPERR_ParamErr;
    vol_t *v = find_vol_by_dt_ref(r->ctx, RD_BE16(r->in + 1));
    if (!v || !v->desktop)
        return AFPERR_ParamErr;
    uint32_t creator = RD_BE32(r->in + 3);
    uint32_t file_type = RD_BE32(r->in + 7);
    uint8_t icon_type = r->in[11];
    uint16_t req_size = RD_BE16(r->in + 13);

    const afp_icon_t *icon = afp_desktop_get_icon(v->desktop, creator, file_type, icon_type);
    if (!icon)
        return AFPERR_ItemNotFound; // DTDBMgr.a requires afpItemNotFound on a miss
    int sz = icon->size;
    if (req_size && sz > req_size)
        sz = req_size;
    if (sz > r->out_max)
        sz = r->out_max;
    memcpy(r->out, icon->bitmap, (size_t)sz);
    r->out_len = sz;
    return AFPERR_NoErr;
}

// FPGetIconInfo (0x34)
static uint32_t afp_cmd_get_icon_info(afp_req_t *r) {
    if (r->in_len < 9)
        return AFPERR_ParamErr;
    vol_t *v = find_vol_by_dt_ref(r->ctx, RD_BE16(r->in + 1));
    if (!v || !v->desktop)
        return AFPERR_ParamErr;
    uint32_t creator = RD_BE32(r->in + 3);
    uint16_t index = RD_BE16(r->in + 7);
    const afp_icon_t *icon = afp_desktop_icon_at(v->desktop, creator, index);
    if (!icon)
        return AFPERR_ItemNotFound;
    WR_BE32(r->out + 0, icon->tag);
    WR_BE32(r->out + 4, icon->file_type);
    r->out[8] = icon->icon_type;
    r->out[9] = 0;
    WR_BE16(r->out + 10, icon->size);
    r->out_len = 12;
    return AFPERR_NoErr;
}

// FPAddAPPL (0x35) — the mapping is keyed by the application's CNID, so
// renaming it through AFP no longer orphans the entry.
static uint32_t afp_cmd_add_appl(afp_req_t *r) {
    if (r->in_len < 15)
        return AFPERR_ParamErr;
    vol_t *v = find_vol_by_dt_ref(r->ctx, RD_BE16(r->in + 1));
    if (!v || !v->desktop)
        return AFPERR_ParamErr;
    uint32_t dir_id = RD_BE32(r->in + 3);
    uint32_t creator = RD_BE32(r->in + 7);
    uint32_t appl_tag = RD_BE32(r->in + 11);
    afp_path_t path;
    if (afp_read_path(r->in, r->in_len, 15, &path) < 0)
        return AFPERR_ParamErr;

    vol_t *resolved = NULL;
    char target_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_target(r->ctx, v->vol_id, dir_id, &path, &resolved, target_rel, sizeof(target_rel));
    if (rc != AFPERR_NoErr)
        return rc;
    struct stat st;
    if (!afp_stat_path(v, target_rel, &st))
        return AFPERR_ObjectNotFound;
    const afp_cat_entry_t *entry = afp_entry_for(v, target_rel);
    if (!entry)
        return AFPERR_MiscErr;
    if (afp_desktop_put_appl(v->desktop, creator, entry->cnid, appl_tag) != 0)
        return AFPERR_MiscErr;
    LOG(10, "AFP FPAddAPPL: creator=0x%08X cnid=0x%08X path='%s'", creator, entry->cnid, target_rel);
    return AFPERR_NoErr;
}

// FPRemoveAPPL (0x36)
static uint32_t afp_cmd_remove_appl(afp_req_t *r) {
    if (r->in_len < 11)
        return AFPERR_ParamErr;
    vol_t *v = find_vol_by_dt_ref(r->ctx, RD_BE16(r->in + 1));
    if (!v || !v->desktop)
        return AFPERR_ParamErr;
    uint32_t dir_id = RD_BE32(r->in + 3);
    uint32_t creator = RD_BE32(r->in + 7);
    // The application is named by its path.  One that did not resolve left
    // the CNID 0, which the store takes as "every mapping for this creator".
    afp_path_t path;
    if (afp_read_path(r->in, r->in_len, 11, &path) < 0)
        return AFPERR_ParamErr;
    vol_t *resolved = NULL;
    char target_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_target(r->ctx, v->vol_id, dir_id, &path, &resolved, target_rel, sizeof(target_rel));
    if (rc != AFPERR_NoErr)
        return rc;
    struct stat st;
    if (!afp_stat_path(v, target_rel, &st))
        return AFPERR_ObjectNotFound;
    const afp_cat_entry_t *entry = afp_catalog_resolve_path(v->catalog, target_rel, false, false);
    if (!entry || afp_desktop_remove_appl(v->desktop, creator, entry->cnid) == 0)
        return AFPERR_ItemNotFound;
    return AFPERR_NoErr;
}

// FPGetAPPL (0x37)
static uint32_t afp_cmd_get_appl(afp_req_t *r) {
    if (r->in_len < 9)
        return AFPERR_ParamErr;
    vol_t *v = find_vol_by_dt_ref(r->ctx, RD_BE16(r->in + 1));
    if (!v || !v->desktop)
        return AFPERR_ParamErr;
    uint32_t creator = RD_BE32(r->in + 3);
    uint16_t index = RD_BE16(r->in + 7);
    uint16_t bitmap = (r->in_len >= 11) ? RD_BE16(r->in + 9) : 0;

    const afp_appl_t *appl = afp_desktop_appl_at(v->desktop, creator, index);
    if (!appl)
        return AFPERR_ItemNotFound;
    char rel[AFP_MAX_REL_PATH];
    if (!afp_catalog_path(v->catalog, appl->cnid, rel, sizeof(rel)))
        return AFPERR_ItemNotFound;
    struct stat st;
    if (!afp_stat_path(v, rel, &st))
        return AFPERR_ItemNotFound; // the application is gone; the mapping is stale

    WR_BE16(r->out + 0, bitmap);
    WR_BE32(r->out + 2, appl->tag);
    int p = afp_emit_params(false, v, rel, &st, bitmap, r->out, 6, r->out_max);
    if (p < 0)
        return AFPERR_ParamErr;
    r->out_len = p;
    return AFPERR_NoErr;
}

// Resolve the (DTRefNum, DirectoryID, Pathname) triple the comment calls use.
static uint32_t afp_resolve_dt_target(const afp_req_t *r, vol_t **out_vol, char *out_rel, size_t rel_cap,
                                      int *out_pos) {
    if (r->in_len < 8)
        return AFPERR_ParamErr;
    vol_t *v = find_vol_by_dt_ref(r->ctx, RD_BE16(r->in + 1));
    if (!v)
        return AFPERR_ParamErr;
    uint32_t dir_id = RD_BE32(r->in + 3);
    afp_path_t path;
    int pos = afp_read_path(r->in, r->in_len, 7, &path);
    if (pos < 0)
        return AFPERR_ParamErr;
    if (out_pos)
        *out_pos = pos;
    vol_t *resolved = NULL;
    uint32_t rc = afp_resolve_target(r->ctx, v->vol_id, dir_id, &path, &resolved, out_rel, rel_cap);
    if (rc != AFPERR_NoErr)
        return rc;
    if (out_vol)
        *out_vol = v;
    return AFPERR_NoErr;
}

// FPAddComment (0x38) — comments live in the file's sidecar, so they follow it
// through renames and copies for free.
static uint32_t afp_cmd_add_comment(afp_req_t *r) {
    vol_t *v = NULL;
    char rel[AFP_MAX_REL_PATH];
    int pos = 0;
    uint32_t rc = afp_resolve_dt_target(r, &v, rel, sizeof(rel), &pos);
    if (rc != AFPERR_NoErr)
        return rc;
    char full[PATH_MAX];
    struct stat st;
    if (!afp_host_path(v, rel, full, sizeof(full)) || stat(full, &st) != 0)
        return AFPERR_ObjectNotFound;

    afp_meta_t meta;
    afp_meta_load(full, &meta);
    meta.comment_len = 0;
    meta.comment[0] = '\0';
    meta.has_comment = true;
    // A pad follows the pathname when the comment would start on an odd
    // offset of the command block -- which begins one byte before r->in, with
    // the opcode.  It was read as the comment's length.
    if (pos % 2 == 0)
        pos++;
    if (pos < r->in_len) {
        int len = r->in[pos++];
        if (len > AFP_META_COMMENT_MAX)
            len = AFP_META_COMMENT_MAX;
        if (pos + len > r->in_len)
            len = r->in_len - pos;
        if (len > 0)
            memcpy(meta.comment, r->in + pos, (size_t)len);
        meta.comment[len] = '\0';
        meta.comment_len = (uint8_t)len;
    }
    if (afp_meta_update(full, &meta) != 0)
        return AFPERR_AccessDenied;
    LOG(10, "AFP FPAddComment: '%s' len=%u", rel, meta.comment_len);
    return AFPERR_NoErr;
}

// FPRemoveComment (0x39)
static uint32_t afp_cmd_remove_comment(afp_req_t *r) {
    vol_t *v = NULL;
    char rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_dt_target(r, &v, rel, sizeof(rel), NULL);
    if (rc != AFPERR_NoErr)
        return rc;
    char full[PATH_MAX];
    if (!afp_host_path(v, rel, full, sizeof(full)))
        return AFPERR_ParamErr;
    afp_meta_t meta;
    afp_meta_load(full, &meta);
    if (!meta.has_comment)
        return AFPERR_ItemNotFound;
    meta.has_comment = false;
    meta.comment_len = 0;
    meta.comment[0] = '\0';
    if (afp_meta_update(full, &meta) != 0)
        return AFPERR_AccessDenied;
    return AFPERR_NoErr;
}

// FPGetComment (0x3A)
static uint32_t afp_cmd_get_comment(afp_req_t *r) {
    vol_t *v = NULL;
    char rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_dt_target(r, &v, rel, sizeof(rel), NULL);
    if (rc != AFPERR_NoErr)
        return rc;
    char full[PATH_MAX];
    if (!afp_host_path(v, rel, full, sizeof(full)))
        return AFPERR_ParamErr;
    afp_meta_t meta;
    afp_meta_load(full, &meta);
    if (!meta.has_comment || meta.comment_len == 0)
        return AFPERR_ItemNotFound;
    int clen = meta.comment_len;
    r->out[0] = (uint8_t)clen;
    memcpy(r->out + 1, meta.comment, (size_t)clen);
    r->out_len = 1 + clen;
    return AFPERR_NoErr;
}

// ============================================================================
// AFP 2.1 file-ID calls
// ============================================================================

// FPCreateID (0x27) — attach a file-ID thread to a file.  The CNID is already
// the file's FileNumber; the thread is what makes it resolvable.
static uint32_t afp_cmd_create_id(afp_req_t *r) {
    if (r->in_len < 8)
        return AFPERR_ParamErr;

    vol_t *vol = NULL;
    char rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_decode_target(r, 7, &vol, rel, sizeof(rel), NULL);
    if (rc != AFPERR_NoErr)
        return rc;
    struct stat st;
    if (!afp_stat_path(vol, rel, &st))
        return AFPERR_ObjectNotFound;
    if (S_ISDIR(st.st_mode))
        return AFPERR_ObjectTypeErr;
    const afp_cat_entry_t *entry = afp_entry_for(vol, rel);
    if (!entry)
        return AFPERR_MiscErr;
    uint32_t cnid = entry->cnid;
    bool existed = entry->has_file_id;
    WR_BE32(r->out, cnid);
    r->out_len = 4;
    if (existed)
        return AFPERR_IDExists; // the ID is still returned, per the spec
    if (!afp_catalog_set_file_id(vol->catalog, cnid, true))
        return AFPERR_MiscErr;
    LOG(10, "AFP FPCreateID: '%s' → id=0x%08X", rel, cnid);
    return AFPERR_NoErr;
}

// FPDeleteID (0x28)
static uint32_t afp_cmd_delete_id(afp_req_t *r) {
    if (r->in_len < 7)
        return AFPERR_ParamErr;
    vol_t *vol = afp_session_vol(r->ctx, RD_BE16(r->in + 1));
    if (!vol)
        return AFPERR_ParamErr;
    uint32_t file_id = RD_BE32(r->in + 3);
    const afp_cat_entry_t *entry = afp_catalog_find(vol->catalog, file_id);
    if (!entry)
        return AFPERR_IDNotFound;
    if (entry->is_dir)
        return AFPERR_ObjectTypeErr;
    if (!entry->has_file_id)
        return AFPERR_IDNotFound;
    afp_catalog_set_file_id(vol->catalog, file_id, false);
    LOG(10, "AFP FPDeleteID: id=0x%08X", file_id);
    return AFPERR_NoErr;
}

// FPResolveID (0x29) — parameters for the file a file ID names.
static uint32_t afp_cmd_resolve_id(afp_req_t *r) {
    if (r->in_len < 9)
        return AFPERR_ParamErr;
    vol_t *vol = afp_session_vol(r->ctx, RD_BE16(r->in + 1));
    if (!vol)
        return AFPERR_ParamErr;
    uint32_t file_id = RD_BE32(r->in + 3);
    uint16_t bitmap = RD_BE16(r->in + 7);

    const afp_cat_entry_t *entry = afp_catalog_find(vol->catalog, file_id);
    if (!entry || entry->is_dir || !entry->has_file_id)
        return AFPERR_BadIDErr;
    char rel[AFP_MAX_REL_PATH];
    if (!afp_catalog_path(vol->catalog, file_id, rel, sizeof(rel)))
        return AFPERR_IDNotFound;
    struct stat st;
    if (!afp_stat_path(vol, rel, &st))
        return AFPERR_IDNotFound; // dangling thread — the file is gone

    WR_BE16(r->out + 0, bitmap);
    int p = afp_emit_params(false, vol, rel, &st, bitmap, r->out, 2, r->out_max);
    if (p < 0)
        return AFPERR_ParamErr;
    r->out_len = p;
    LOG(10, "AFP FPResolveID: id=0x%08X → '%s'", file_id, rel);
    return AFPERR_NoErr;
}

// FPExchangeFiles (0x2A) — the safe-save primitive.  Only the filename, parent
// directory ID, file ID and creation date are exchanged (AFP_21_22 Fig 1-17);
// the bytes, the byte-range locks and the deny modes stay with the fork
// reference that owns them.  On a path-addressed host filesystem that means
// physically swapping the two files (and their sidecars), keeping each name's
// catalog entry — hence its ID — in place, restoring each name's creation
// date, and re-pointing any open fork at wherever its bytes moved.
static uint32_t afp_cmd_exchange_files(afp_req_t *r) {
    if (r->in_len < 12)
        return AFPERR_ParamErr;
    uint16_t vol_id = RD_BE16(r->in + 1);
    uint32_t src_dir = RD_BE32(r->in + 3);
    uint32_t dst_dir = RD_BE32(r->in + 7);
    afp_path_t src_path;
    int pos = afp_read_path(r->in, r->in_len, 11, &src_path);
    if (pos < 0 || pos >= r->in_len)
        return AFPERR_ParamErr;
    afp_path_t dst_path;
    if (afp_read_path(r->in, r->in_len, pos, &dst_path) < 0)
        return AFPERR_ParamErr;

    vol_t *vol = NULL;
    char src_rel[AFP_MAX_REL_PATH], dst_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_target(r->ctx, vol_id, src_dir, &src_path, &vol, src_rel, sizeof(src_rel));
    if (rc != AFPERR_NoErr)
        return rc;
    rc = afp_resolve_target(r->ctx, vol_id, dst_dir, &dst_path, &vol, dst_rel, sizeof(dst_rel));
    if (rc != AFPERR_NoErr)
        return rc;
    if (strcmp(src_rel, dst_rel) == 0)
        return AFPERR_SameObjectErr;

    char src_full[PATH_MAX], dst_full[PATH_MAX];
    if (!afp_host_path(vol, src_rel, src_full, sizeof(src_full)) ||
        !afp_host_path(vol, dst_rel, dst_full, sizeof(dst_full)))
        return AFPERR_ParamErr;
    struct stat src_st, dst_st;
    if (stat(src_full, &src_st) != 0 || stat(dst_full, &dst_st) != 0)
        return AFPERR_ObjectNotFound;
    if (S_ISDIR(src_st.st_mode) || S_ISDIR(dst_st.st_mode))
        return AFPERR_ObjectTypeErr;

    // Creation dates belong to the names, so capture them before the swap.
    afp_meta_t src_meta, dst_meta;
    bool src_has = afp_meta_load(src_full, &src_meta);
    bool dst_has = afp_meta_load(dst_full, &dst_meta);
    uint32_t src_create =
        (src_has && src_meta.has_dates) ? src_meta.create_date : afp_unix_time_to_afp(src_st.st_mtime);
    uint32_t dst_create =
        (dst_has && dst_meta.has_dates) ? dst_meta.create_date : afp_unix_time_to_afp(dst_st.st_mtime);

    // Three-way rename of the data files, then of the sidecars.
    char tmp_full[PATH_MAX];
    if ((size_t)snprintf(tmp_full, sizeof(tmp_full), "%s.gsxchg", src_full) >= sizeof(tmp_full))
        return AFPERR_ParamErr;
    if (rename(src_full, tmp_full) != 0)
        return AFPERR_AccessDenied;
    if (rename(dst_full, src_full) != 0) {
        rename(tmp_full, src_full); // put the source back
        return AFPERR_AccessDenied;
    }
    if (rename(tmp_full, dst_full) != 0) {
        rename(src_full, dst_full);
        rename(tmp_full, src_full);
        return AFPERR_AccessDenied;
    }
    char src_sc[PATH_MAX], dst_sc[PATH_MAX], tmp_sc[PATH_MAX];
    if (afp_meta_sidecar_path(src_full, src_sc, sizeof(src_sc)) &&
        afp_meta_sidecar_path(dst_full, dst_sc, sizeof(dst_sc)) &&
        (size_t)snprintf(tmp_sc, sizeof(tmp_sc), "%s.gsxchg", src_sc) < sizeof(tmp_sc)) {
        // Either sidecar may be absent; rename() simply fails harmlessly then.
        rename(src_sc, tmp_sc);
        rename(dst_sc, src_sc);
        rename(tmp_sc, dst_sc);
    }

    // Give each name its own creation date back.
    afp_meta_t after;
    afp_meta_load(src_full, &after);
    after.create_date = src_create;
    after.has_dates = true;
    afp_meta_update(src_full, &after);
    afp_meta_load(dst_full, &after);
    after.create_date = dst_create;
    after.has_dates = true;
    afp_meta_update(dst_full, &after);

    // Open forks follow their bytes, which have swapped places.
    afp_fork_repoint(src_full, tmp_full, NULL); // park the source handles
    afp_fork_repoint(dst_full, src_full, src_rel);
    afp_fork_repoint(tmp_full, dst_full, dst_rel);

    // Both names keep their catalog entries — and so their file IDs.
    afp_catalog_resolve_path(vol->catalog, src_rel, true, false);
    afp_catalog_resolve_path(vol->catalog, dst_rel, true, false);
    afp_vol_touch(vol);
    LOG(2, "AFP FPExchangeFiles: '%s' <-> '%s'", src_rel, dst_rel);
    return AFPERR_NoErr;
}

// ============================================================================
// FPCatSearch (0x2B)
// ============================================================================

// The bits of RequestBitmap this server can actually search on — everything
// FPGetFileDirParms serves from the catalog or the sidecar.  Anything else in
// the request is a bitmap error rather than a silently ignored criterion.
#define AFP_CATSEARCH_SUPPORTED                                                                                        \
    ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 9) | (1u << 10))

// Bit 31 of RequestBitmap selects partial-name matching (AFP_21_22 p. 36).
#define AFP_CATSEARCH_PARTIAL_NAME 0x80000000u

// One decoded search specification.  Specification1 carries values and range
// lower bounds; Specification2 carries masks and range upper bounds.
typedef struct {
    uint16_t attrs;
    uint16_t attrs_mask;
    uint32_t parent;
    uint32_t create_lo, create_hi;
    uint32_t modify_lo, modify_hi;
    uint32_t backup_lo, backup_hi;
    uint8_t finder[AFP_META_FINDER_SIZE];
    uint8_t finder_mask[AFP_META_FINDER_SIZE];
    char name[AFP_MAX_NAME * 3 + 1]; // as a host name
    uint32_t dlen_lo, dlen_hi;
    uint32_t rlen_lo, rlen_hi;
} catsearch_spec_t;

// Walk one specification record, handing each present field to `store`.
// The record is Size(1) + filler(1) + parameters packed in bitmap order, with
// variable-length values addressed by offsets from the parameter start.
static bool catsearch_parse_spec(const uint8_t *in, int in_len, int pos, uint32_t request_bm, bool is_dir,
                                 catsearch_spec_t *spec, bool second, int *out_next) {
    if (pos >= in_len)
        return false;
    // StructLength counts the parameters after it and its filler byte, not
    // those two: System 7.5's AppleShare 3.5 sends 0x28, 40 bytes of
    // parameters, and its Specification2 starts 42 bytes on.
    int size = in[pos];
    int base = pos + 2;
    int end = base + size;
    if (end > in_len)
        return false;
    if (out_next)
        *out_next = end;
    int p = base;
    for (int bit = 0; bit < 16; bit++) {
        if (!(request_bm & (1u << bit)))
            continue;
        int width = afp_param_field_width(is_dir, bit);
        if (width == 0)
            continue;
        if (p + width > end)
            return false;
        const uint8_t *f = in + p;
        p += width;
        switch (bit) {
        case 0:
            if (second)
                spec->attrs_mask = RD_BE16(f);
            else
                spec->attrs = RD_BE16(f);
            break;
        case 1:
            if (!second)
                spec->parent = RD_BE32(f);
            break;
        case 2:
            if (second)
                spec->create_hi = RD_BE32(f);
            else
                spec->create_lo = RD_BE32(f);
            break;
        case 3:
            if (second)
                spec->modify_hi = RD_BE32(f);
            else
                spec->modify_lo = RD_BE32(f);
            break;
        case 4:
            if (second)
                spec->backup_hi = RD_BE32(f);
            else
                spec->backup_lo = RD_BE32(f);
            break;
        case 5:
            memcpy(second ? spec->finder_mask : spec->finder, f, AFP_META_FINDER_SIZE);
            break;
        case 6: {
            // Specification2 must carry a nil name field, so only the first
            // record's offset is followed.
            if (second)
                break;
            uint16_t off = RD_BE16(f);
            int np = base + off;
            if (off == 0 || np >= end)
                break;
            int nlen = in[np];
            if (np + 1 + nlen > end)
                break;
            if (nlen > AFP_MAX_NAME)
                nlen = AFP_MAX_NAME;
            // The criterion is a Mac name; compare it as the host name it
            // stands for.  One that cannot be a host name matches nothing.
            if (!macroman_name_to_host(in + np + 1, (size_t)nlen, spec->name, sizeof(spec->name))) {
                memcpy(spec->name, in + np + 1, (size_t)nlen);
                spec->name[nlen] = '\0';
            }
            break;
        }
        case 9:
            if (second)
                spec->dlen_hi = (width == 2) ? RD_BE16(f) : RD_BE32(f);
            else
                spec->dlen_lo = (width == 2) ? RD_BE16(f) : RD_BE32(f);
            break;
        case 10:
            if (second)
                spec->rlen_hi = RD_BE32(f);
            else
                spec->rlen_lo = RD_BE32(f);
            break;
        default:
            break;
        }
    }
    return true;
}

// Test one candidate against the decoded specifications.
static bool catsearch_matches(vol_t *vol, const char *rel, const char *name, bool is_dir, const struct stat *st,
                              uint32_t request_bm, const catsearch_spec_t *s1, const catsearch_spec_t *s2,
                              bool partial_name) {
    char full[PATH_MAX];
    if (!afp_host_path(vol, rel, full, sizeof(full)))
        return false;
    afp_meta_t meta;
    afp_meta_load(full, &meta);

    if (request_bm & (1u << 0)) {
        uint16_t attrs = afp_attributes_of(full, st, &meta);
        uint16_t mask = s2->attrs_mask;
        if ((attrs & mask) != (s1->attrs & mask))
            return false;
    }
    if ((request_bm & (1u << 1)) && afp_parent_cnid(vol, rel) != s1->parent)
        return false;
    if (request_bm & (1u << 2)) {
        uint32_t v = meta.has_dates ? meta.create_date : afp_unix_time_to_afp(st->st_mtime);
        if (v < s1->create_lo || v > s2->create_hi)
            return false;
    }
    if (request_bm & (1u << 3)) {
        uint32_t v = afp_unix_time_to_afp(st->st_mtime);
        if (v < s1->modify_lo || v > s2->modify_hi)
            return false;
    }
    if (request_bm & (1u << 4)) {
        uint32_t v = meta.has_dates ? meta.backup_date : AFP_DATE_NEVER;
        if (v < s1->backup_lo || v > s2->backup_hi)
            return false;
    }
    if (request_bm & (1u << 5)) {
        for (int i = 0; i < AFP_META_FINDER_SIZE; i++) {
            uint8_t have = meta.has_finder ? meta.finder[i] : 0;
            if ((have & s2->finder_mask[i]) != (s1->finder[i] & s2->finder_mask[i]))
                return false;
        }
    }
    if (request_bm & (1u << 6)) {
        // Folded as Mac names: case-insensitive, diacritical-sensitive.
        if (partial_name ? !afp_name_fold_contains(name, s1->name) : afp_name_fold_cmp(name, s1->name) != 0)
            return false;
    }
    if (request_bm & (1u << 9)) {
        uint32_t v = is_dir ? afp_count_offspring(full) : (uint32_t)st->st_size;
        if (v < s1->dlen_lo || v > s2->dlen_hi)
            return false;
    }
    if ((request_bm & (1u << 10)) && !is_dir) {
        uint32_t v = afp_meta_rsrc_len(full);
        if (v < s1->rlen_lo || v > s2->rlen_hi)
            return false;
    }
    return true;
}

static uint32_t afp_cmd_cat_search(afp_req_t *r) {
    if (r->in_len < 35)
        return AFPERR_ParamErr;
    uint16_t vol_id = RD_BE16(r->in + 1);
    uint32_t req_matches = RD_BE32(r->in + 3);
    // in + 7: Reserved (must be zero)
    const uint8_t *catpos = r->in + 11; // 16 bytes
    uint16_t file_bm = RD_BE16(r->in + 27);
    uint16_t dir_bm = RD_BE16(r->in + 29);
    uint32_t request_bm = RD_BE32(r->in + 31);
    int pos = 35;

    vol_t *vol = afp_session_vol(r->ctx, vol_id);
    if (!vol || !vol->catalog)
        return AFPERR_ParamErr;
    if (file_bm == 0 && dir_bm == 0)
        return AFPERR_BitmapErr;
    bool partial_name = (request_bm & AFP_CATSEARCH_PARTIAL_NAME) != 0;
    uint32_t criteria = request_bm & 0xFFFFu;
    if (criteria == 0 || (criteria & ~(uint32_t)AFP_CATSEARCH_SUPPORTED))
        return AFPERR_BitmapErr;
    // Attribute searches are only defined when one of files/directories is
    // being searched, not both (AFP_21_22 p. 42).
    if ((criteria & (1u << 0)) && file_bm && dir_bm)
        return AFPERR_BitmapErr;

    // CatPosition: a zero first word restarts the walk; otherwise it carries
    // the generation the client last saw plus the CNID it stopped at.
    uint16_t pos_valid = RD_BE16(catpos);
    uint32_t last_cnid = 0;
    if (pos_valid) {
        uint32_t gen = RD_BE32(catpos + 4);
        last_cnid = RD_BE32(catpos + 8);
        if (gen != afp_catalog_generation(vol->catalog))
            return AFPERR_CatalogChanged;
    } else {
        // A fresh search reconciles the catalog with the host tree first, so
        // out-of-band deletions cannot surface as phantom matches.
        afp_catalog_sweep(vol->catalog);
    }

    bool search_dirs = dir_bm != 0;
    catsearch_spec_t s1, s2;
    memset(&s1, 0, sizeof(s1));
    memset(&s2, 0, sizeof(s2));
    s2.create_hi = s2.modify_hi = s2.backup_hi = UINT32_MAX;
    s2.dlen_hi = s2.rlen_hi = UINT32_MAX;
    memset(s2.finder_mask, 0xFF, sizeof(s2.finder_mask));
    s2.attrs_mask = 0xFFFF;
    int next = pos;
    if (!catsearch_parse_spec(r->in, r->in_len, pos, criteria, search_dirs, &s1, false, &next))
        return AFPERR_ParamErr;
    if (!catsearch_parse_spec(r->in, r->in_len, next, criteria, search_dirs, &s2, true, NULL))
        return AFPERR_ParamErr;

    // Reply header: CatPosition(16) FileRsltBitmap(2) DirRsltBitmap(2)
    // ActualCount(4), then the result records.
    int w = 24;
    uint32_t actual = 0;
    uint32_t cursor = last_cnid;
    bool exhausted = true;
    if (req_matches == 0)
        req_matches = UINT32_MAX;

    // The cursor moves past an entry only once it is written or rejected: an
    // entry the reply had no room for, or that came after the last match
    // asked for, is where the next call resumes.  It moved first, so every
    // resume lost one match (1999 of 2000).
    for (const afp_cat_entry_t *e = afp_catalog_next(vol->catalog, cursor); e;
         e = afp_catalog_next(vol->catalog, cursor)) {
        if (actual >= req_matches) {
            exhausted = false;
            break;
        }
        // Copied now: matching adopts ancestors into the catalog, which can
        // move `e`.
        uint32_t cnid = e->cnid;
        char name[AFP_CAT_MAX_NAME + 1];
        snprintf(name, sizeof(name), "%s", e->name);
        char rel[AFP_MAX_REL_PATH];
        struct stat st;
        bool is_dir = false;
        bool candidate = cnid != AFP_CNID_ROOT && afp_name_visible(name) &&
                         afp_catalog_path(vol->catalog, cnid, rel, sizeof(rel)) &&
                         afp_stat_path(vol, rel, &st); // a vanished file is swept by the next full search
        if (candidate) {
            is_dir = S_ISDIR(st.st_mode);
            candidate = (is_dir ? dir_bm : file_bm) != 0 &&
                        catsearch_matches(vol, rel, name, is_dir, &st, criteria, &s1, &s2, partial_name);
        }
        if (candidate) {
            // Result records use FPEnumerate's framing.
            int end = afp_emit_record(is_dir, vol, rel, &st, is_dir ? dir_bm : file_bm, r->out, w, r->out_max);
            if (end < 0) {
                exhausted = false;
                break;
            }
            w = end;
            actual++;
        }
        cursor = cnid;
    }

    memset(r->out, 0, 16);
    WR_BE16(r->out + 0, 1); // a real catalog position, not a hint
    WR_BE32(r->out + 4, afp_catalog_generation(vol->catalog));
    WR_BE32(r->out + 8, cursor);
    WR_BE16(r->out + 16, file_bm);
    WR_BE16(r->out + 18, dir_bm);
    WR_BE32(r->out + 20, actual);
    r->out_len = w;
    LOG(10, "AFP FPCatSearch: vol=0x%04X requestBm=0x%08X matches=%u cursor=0x%08X%s", vol_id, request_bm, actual,
        cursor, exhausted ? " (end)" : "");
    // afpEofError means "the whole tree has been walked", not "no matches".
    return exhausted ? AFPERR_EOFErr : AFPERR_NoErr;
}

// ============================================================================
// Dispatch
// ============================================================================

static const afp_command_handler_t k_afp_command_handlers[] = {
    {AFP_ByteRangeLock,   "FPByteRangeLock",   afp_cmd_byte_range_lock,    AFP_GATE_LOGIN  },
    {AFP_CloseVol,        "FPCloseVol",        afp_cmd_close_vol,          AFP_GATE_LOGIN  },
    {AFP_CloseDir,        "FPCloseDir",        afp_cmd_close_dir,          AFP_GATE_LOGIN  },
    {AFP_CloseFork,       "FPCloseFork",       afp_cmd_close_fork,         AFP_GATE_LOGIN  },
    {AFP_CopyFile,        "FPCopyFile",        afp_cmd_copy_file,          AFP_GATE_LOGIN  },
    {AFP_CreateDir,       "FPCreateDir",       afp_cmd_create_dir,         AFP_GATE_LOGIN  },
    {AFP_CreateFile,      "FPCreateFile",      afp_cmd_create_file,        AFP_GATE_LOGIN  },
    {AFP_Delete,          "FPDelete",          afp_cmd_delete,             AFP_GATE_LOGIN  },
    {AFP_Enumerate,       "FPEnumerate",       afp_cmd_enumerate,          AFP_GATE_LOGIN  },
    {AFP_Flush,           "FPFlush",           afp_cmd_flush,              AFP_GATE_LOGIN  },
    {AFP_FlushFork,       "FPFlushFork",       afp_cmd_flush_fork,         AFP_GATE_LOGIN  },
    {AFP_GetForkParms,    "FPGetForkParms",    afp_cmd_get_fork_parms,     AFP_GATE_LOGIN  },
    {AFP_GetSrvrParms,    "FPGetSrvrParms",    afp_cmd_get_srvr_parms,     AFP_GATE_LOGIN  },
    {AFP_GetVolParms,     "FPGetVolParms",     afp_cmd_get_vol_parms,      AFP_GATE_LOGIN  },
    {AFP_Login,           "FPLogin",           afp_cmd_login,              AFP_GATE_SESSION},
    {AFP_LoginCont,       "FPLoginCont",       afp_cmd_login_cont,         AFP_GATE_SESSION},
    {AFP_Logout,          "FPLogout",          afp_cmd_logout,             AFP_GATE_LOGIN  },
    {AFP_MapID,           "FPMapID",           afp_cmd_map_id,             AFP_GATE_LOGIN  },
    {AFP_MapName,         "FPMapName",         afp_cmd_map_name,           AFP_GATE_LOGIN  },
    {AFP_MoveAndRename,   "FPMoveAndRename",   afp_cmd_move_and_rename,    AFP_GATE_LOGIN  },
    {AFP_OpenVol,         "FPOpenVol",         afp_cmd_open_vol,           AFP_GATE_LOGIN  },
    {AFP_OpenDir,         "FPOpenDir",         afp_cmd_open_dir,           AFP_GATE_LOGIN  },
    {AFP_OpenFork,        "FPOpenFork",        afp_cmd_open_fork,          AFP_GATE_LOGIN  },
    {AFP_Read,            "FPRead",            afp_cmd_read,               AFP_GATE_LOGIN  },
    {AFP_Rename,          "FPRename",          afp_cmd_rename,             AFP_GATE_LOGIN  },
    {AFP_SetDirParms,     "FPSetDirParms",     afp_cmd_set_dir_parms,      AFP_GATE_LOGIN  },
    {AFP_SetFileParms,    "FPSetFileParms",    afp_cmd_set_file_parms,     AFP_GATE_LOGIN  },
    {AFP_SetForkParms,    "FPSetForkParms",    afp_cmd_set_fork_parms,     AFP_GATE_LOGIN  },
    {AFP_SetVolParms,     "FPSetVolParms",     afp_cmd_set_vol_parms,      AFP_GATE_LOGIN  },
    {AFP_Write,           "FPWrite",           afp_cmd_write,              AFP_GATE_LOGIN  },
    {AFP_GetFileDirParms, "FPGetFileDirParms", afp_cmd_get_file_dir_parms, AFP_GATE_LOGIN  },
    {AFP_SetFileDirParms, "FPSetFileDirParms", afp_cmd_set_file_dir_parms, AFP_GATE_LOGIN  },
    {AFP_ChangePassword,  "FPChangePassword",  afp_cmd_change_password,    AFP_GATE_LOGIN  },
    {AFP_GetUserInfo,     "FPGetUserInfo",     afp_cmd_get_user_info,      AFP_GATE_LOGIN  },
    {AFP_GetSrvrMsg,      "FPGetSrvrMsg",      afp_cmd_get_srvr_msg,       AFP_GATE_21     },
    {AFP_CreateID,        "FPCreateID",        afp_cmd_create_id,          AFP_GATE_21     },
    {AFP_DeleteID,        "FPDeleteID",        afp_cmd_delete_id,          AFP_GATE_21     },
    {AFP_ResolveID,       "FPResolveID",       afp_cmd_resolve_id,         AFP_GATE_21     },
    {AFP_ExchangeFiles,   "FPExchangeFiles",   afp_cmd_exchange_files,     AFP_GATE_21     },
    {AFP_CatSearch,       "FPCatSearch",       afp_cmd_cat_search,         AFP_GATE_21     },
    {AFP_OpenDT,          "FPOpenDT",          afp_cmd_open_dt,            AFP_GATE_LOGIN  },
    {AFP_CloseDT,         "FPCloseDT",         afp_cmd_close_dt,           AFP_GATE_LOGIN  },
    {AFP_GetIcon,         "FPGetIcon",         afp_cmd_get_icon,           AFP_GATE_LOGIN  },
    {AFP_GetIconInfo,     "FPGetIconInfo",     afp_cmd_get_icon_info,      AFP_GATE_LOGIN  },
    {AFP_AddAPPL,         "FPAddAPPL",         afp_cmd_add_appl,           AFP_GATE_LOGIN  },
    {AFP_RmvAPPL,         "FPRemoveAPPL",      afp_cmd_remove_appl,        AFP_GATE_LOGIN  },
    {AFP_GetAPPL,         "FPGetAPPL",         afp_cmd_get_appl,           AFP_GATE_LOGIN  },
    {AFP_AddComment,      "FPAddComment",      afp_cmd_add_comment,        AFP_GATE_LOGIN  },
    {AFP_RmvComment,      "FPRemoveComment",   afp_cmd_remove_comment,     AFP_GATE_LOGIN  },
    {AFP_GetComment,      "FPGetComment",      afp_cmd_get_comment,        AFP_GATE_LOGIN  },
    {AFP_AddIcon,         "FPAddIcon",         afp_cmd_add_icon,           AFP_GATE_LOGIN  },
};

static const afp_command_handler_t *afp_find_handler(uint8_t opcode) {
    for (size_t i = 0; i < ARRAY_LEN(k_afp_command_handlers); i++)
        if (k_afp_command_handlers[i].opcode == opcode)
            return &k_afp_command_handlers[i];
    return NULL;
}

int atalk_afp_ok_command_at(int index, const char **out_name, uint64_t *out_count) {
    int seen = 0;
    for (size_t i = 0; i < ARRAY_LEN(k_afp_command_handlers); i++) {
        uint64_t n = afp_ok_count(k_afp_command_handlers[i].opcode);
        if (!n || seen++ != index)
            continue;
        if (out_name)
            *out_name = k_afp_command_handlers[i].name;
        if (out_count)
            *out_count = n;
        return 0;
    }
    return -1;
}

// The 2.1 calls are only legal once the session has negotiated 2.1; before
// that the client must use its 2.0 fallbacks (AFP_21_22 result codes).
uint32_t afp_handle_command(uint16_t session_id, uint8_t opcode, const uint8_t *in, int in_len, uint8_t *out,
                            int out_max, int *out_len) {
    if (out_len)
        *out_len = 0;
    // Handlers write their fixed-size replies without checking the room: the
    // reply buffer is never smaller than one ATP packet.  They used to check
    // it one by one, some only after acting -- FPOpenFork after opening the
    // fork, FPLogin after recording the version.
    if (!out || out_max < AFP_MIN_REPLY) {
        LOG(1, "AFP: command 0x%02X refused — reply buffer of %d bytes", opcode, out_max);
        afp_count_result(opcode, AFPERR_ParamErr);
        return AFPERR_ParamErr;
    }
    if (!g_afp_enabled) {
        LOG(2, "AFP: command 0x%02X refused — the server is disabled", opcode);
        return AFPERR_ServerGoingDown;
    }
    const afp_command_handler_t *handler = afp_find_handler(opcode);
    if (!handler) {
        LOG(1, "AFP unknown opcode 0x%02X (len=%d)", opcode, in_len);
        afp_count_result(opcode, AFPERR_CallNotSupported);
        return AFPERR_CallNotSupported;
    }
    // The gate: every command needs an open session, and all but FPLogin and
    // FPLoginCont a logged-in one; the 2.1 calls need a 2.1 login.
    afp_session_t *sess = afp_session(session_id);
    uint32_t refused = AFPERR_NoErr;
    if (!sess)
        refused = AFPERR_SessClosed;
    else if (handler->gate != AFP_GATE_SESSION && sess->state != AFP_SESS_LOGGED_IN)
        refused = AFPERR_UserNotAuth;
    else if (handler->gate == AFP_GATE_21 && strcmp(sess->version, "AFPVersion 2.1") != 0)
        refused = AFPERR_CallNotSupported;
    if (refused != AFPERR_NoErr) {
        LOG(2, "AFP %s from session 0x%04X refused (0x%08X)", handler->name, session_id, refused);
        afp_count_result(opcode, refused);
        return refused;
    }
    afp_ctx_t ctx = {.session_id = session_id};
    afp_req_t req = {.ctx = &ctx, .in = in, .in_len = in_len, .out = out, .out_max = out_max};
    LOG(10, "AFP >> %s (0x%02X) in_len=%d session=0x%04X", handler->name, opcode, in_len, session_id);
    uint32_t result = handler->handler(&req);
    afp_count_result(opcode, result);
    if (out_len)
        *out_len = req.out_len;
    if (result == AFPERR_NoErr)
        LOG(3, "AFP << %s OK reply=%d", handler->name, req.out_len);
    else
        LOG(3, "AFP << %s ERR=0x%08X reply=%d", handler->name, result, req.out_len);
    return result;
}

// Release everything a departing session owned.  Called from the ASP layer on
// CloseSess and on tickle expiry.
// Release what a session holds: its forks, snapshots, and volume and desktop
// references.
static void afp_session_release(uint16_t session_id) {
    afp_fork_close_session(session_id);
    enum_snapshots_drop(session_id, ENUM_ANY);
    for (int i = 0; i < AFP_MAX_VOLUMES; i++) {
        if (!g_vols[i].in_use)
            continue;
        session_set_remove(&g_vols[i].open_by, session_id);
        session_set_remove(&g_vols[i].dt_open_by, session_id);
    }
}

void afp_session_closed(uint16_t session_id) {
    afp_session_release(session_id);
    afp_session_t *s = afp_session(session_id);
    if (s)
        memset(s, 0, sizeof(*s));
}

uint32_t afp_session_open_forks(uint16_t session_id) {
    return afp_fork_count_session(session_id);
}

// Drop every volume-scoped cache — used when a checkpoint restore replaces
// the machine underneath a live mount.
void afp_reset_transient_state(void) {
    for (int i = 0; i < AFP_MAX_VOLUMES; i++) {
        if (!g_vols[i].in_use)
            continue;
        enum_snapshots_drop(ENUM_ANY, g_vols[i].vol_id);
        g_vols[i].open_by.n = 0;
        g_vols[i].dt_open_by.n = 0;
        g_vols[i].mutations++;
    }
    afp_fork_shutdown();
}
