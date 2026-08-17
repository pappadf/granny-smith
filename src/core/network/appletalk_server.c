// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_server.c
// AFP file server over AppleTalk ASP/ATP protocols.
//
// The wire reference is docs/core/network/appletalk_server.md; the AFP 2.1
// additions (FPGetSrvrMsg, the file-ID calls, FPExchangeFiles, FPCatSearch)
// follow Apple's AppleTalk Filing Protocol v2.1/2.2 specification (AppleShare IP
// 6.3 Developer's Kit, 1999).  Persistent server state lives in three
// companion modules — afp_catalog.c (CNIDs), afp_desktop.c (icons and APPL
// mappings) and afp_meta.c (per-file AppleDouble metadata) — while open
// forks, deny modes and byte-range locks live in afp_fork.c.

#include "afp_catalog.h"
#include "afp_desktop.h"
#include "afp_fork.h"
#include "afp_meta.h"
#include "appledouble.h"
#include "appletalk.h"
#include "appletalk_internal.h"
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

// Logging for this module uses the same category as appletalk.c
LOG_USE_CATEGORY_NAME("appletalk");

// Emit a short hex dump at LOG level 2 for AFP request/response payloads
static void afp_log_hex(const char *label, const uint8_t *buf, int len) {
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

// Common wire helpers (big-endian readers/writers)
static uint16_t rd16be(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static void wr16be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void wr32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | ((uint32_t)p[3]);
}

// ============================================================================
// AFP opcodes and result codes
// ============================================================================

#define AFP_ByteRangeLock   0x01
#define AFP_CloseVol        0x02
#define AFP_CloseDir        0x03
#define AFP_CloseFork       0x04
#define AFP_CopyFile        0x05
#define AFP_CreateDir       0x06
#define AFP_CreateFile      0x07
#define AFP_Delete          0x08
#define AFP_Enumerate       0x09
#define AFP_Flush           0x0A
#define AFP_FlushFork       0x0B
#define AFP_GetForkParms    0x0E
#define AFP_GetSrvrInfo     0x0F
#define AFP_GetSrvrParms    0x10
#define AFP_GetVolParms     0x11
#define AFP_Login           0x12
#define AFP_LoginCont       0x13
#define AFP_Logout          0x14
#define AFP_MapID           0x15
#define AFP_MapName         0x16
#define AFP_MoveAndRename   0x17
#define AFP_OpenVol         0x18
#define AFP_OpenDir         0x19
#define AFP_OpenFork        0x1A
#define AFP_Read            0x1B
#define AFP_Rename          0x1C
#define AFP_SetDirParms     0x1D
#define AFP_SetFileParms    0x1E
#define AFP_SetForkParms    0x1F
#define AFP_SetVolParms     0x20
#define AFP_Write           0x21
#define AFP_GetFileDirParms 0x22
#define AFP_SetFileDirParms 0x23
#define AFP_ChangePassword  0x24
#define AFP_GetUserInfo     0x25
// AFP 2.1 additions (AFP_21_22 single-page.md line ~638)
#define AFP_GetSrvrMsg    0x26
#define AFP_CreateID      0x27
#define AFP_DeleteID      0x28
#define AFP_ResolveID     0x29
#define AFP_ExchangeFiles 0x2A
#define AFP_CatSearch     0x2B
#define AFP_OpenDT        0x30
#define AFP_CloseDT       0x31
#define AFP_GetIcon       0x33
#define AFP_GetIconInfo   0x34
#define AFP_AddAPPL       0x35
#define AFP_RmvAPPL       0x36
#define AFP_GetAPPL       0x37
#define AFP_AddComment    0x38
#define AFP_RmvComment    0x39
#define AFP_GetComment    0x3A
#define AFP_AddIcon       0xC0

// AFP result codes (32-bit signed, two's complement)
#define AFPERR_NoErr            0x00000000u // 0
#define AFPERR_AccessDenied     0xFFFFEC78u // -5000
#define AFPERR_AuthContinue     0xFFFFEC77u // -5001
#define AFPERR_BadUAM           0xFFFFEC76u // -5002
#define AFPERR_BadVersNum       0xFFFFEC75u // -5003
#define AFPERR_BitmapErr        0xFFFFEC74u // -5004
#define AFPERR_CantMove         0xFFFFEC73u // -5005
#define AFPERR_DenyConflict     0xFFFFEC72u // -5006
#define AFPERR_DirNotEmpty      0xFFFFEC71u // -5007
#define AFPERR_DiskFull         0xFFFFEC70u // -5008
#define AFPERR_EOFErr           0xFFFFEC6Fu // -5009
#define AFPERR_FileBusy         0xFFFFEC6Eu // -5010
#define AFPERR_FlatVol          0xFFFFEC6Du // -5011
#define AFPERR_ItemNotFound     0xFFFFEC6Cu // -5012
#define AFPERR_LockErr          0xFFFFEC6Bu // -5013
#define AFPERR_MiscErr          0xFFFFEC6Au // -5014
#define AFPERR_NoMoreLocks      0xFFFFEC69u // -5015
#define AFPERR_NoServer         0xFFFFEC68u // -5016
#define AFPERR_ObjectExists     0xFFFFEC67u // -5017
#define AFPERR_ObjectNotFound   0xFFFFEC66u // -5018
#define AFPERR_ParamErr         0xFFFFEC65u // -5019
#define AFPERR_RangeNotLocked   0xFFFFEC64u // -5020
#define AFPERR_RangeOverlap     0xFFFFEC63u // -5021
#define AFPERR_SessClosed       0xFFFFEC62u // -5022
#define AFPERR_UserNotAuth      0xFFFFEC61u // -5023
#define AFPERR_CallNotSupported 0xFFFFEC60u // -5024
#define AFPERR_ObjectTypeErr    0xFFFFEC5Fu // -5025
#define AFPERR_TooManyFilesOpen 0xFFFFEC5Eu // -5026
#define AFPERR_ServerGoingDown  0xFFFFEC5Du // -5027
#define AFPERR_CantRename       0xFFFFEC5Cu // -5028
#define AFPERR_DirNotFound      0xFFFFEC5Bu // -5029
#define AFPERR_IconTypeError    0xFFFFEC5Au // -5030
#define AFPERR_VolLocked        0xFFFFEC59u // -5031
#define AFPERR_ObjectLocked     0xFFFFEC58u // -5032
// AFP 2.1 result codes (SysErr.a lines 606-648; AFP_21_22 p. 60)
#define AFPERR_ContainsSharedErr 0xFFFFEC57u // -5033
#define AFPERR_IDNotFound        0xFFFFEC56u // -5034
#define AFPERR_IDExists          0xFFFFEC55u // -5035
#define AFPERR_DiffVolErr        0xFFFFEC54u // -5036
#define AFPERR_CatalogChanged    0xFFFFEC53u // -5037
#define AFPERR_SameObjectErr     0xFFFFEC52u // -5038
#define AFPERR_BadIDErr          0xFFFFEC51u // -5039

// AFP versions we speak.  "AFPVersion 2.1" is only advertised because every
// 2.1 command below is implemented (WP-5); the honest-negotiation rule is
// that this list and the dispatch table move together.
static const char *const k_afp_versions[] = {"AFPVersion 2.0", "AFPVersion 2.1"};

// FPGetSrvrInfo / GetStatus Flags word (AFP_21_22 Table 1-4 + Figure 1-4).
#define AFP_SRVR_FLAG_COPYFILE       (1u << 0)
#define AFP_SRVR_FLAG_CHGPWD         (1u << 1)
#define AFP_SRVR_FLAG_NOSAVEPWD      (1u << 2)
#define AFP_SRVR_FLAG_SERVERMESSAGES (1u << 3)

// Volume Attributes word (AFP_21_22 Table 1-5 + Figure 1-5).
#define AFP_VOL_ATTR_READONLY    (1u << 0)
#define AFP_VOL_ATTR_HASPASSWORD (1u << 1)
#define AFP_VOL_ATTR_FILEIDS     (1u << 2)
#define AFP_VOL_ATTR_CATSEARCH   (1u << 3)

// ============================================================================
// Volume table
// ============================================================================

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

// Note that `session` has this volume open.  Repeated FPOpenVol calls from one
// session are idempotent, as the client expects.
static void vol_session_add(vol_t *v, uint16_t session) {
    for (uint32_t i = 0; i < v->n_open_by; i++)
        if (v->open_by[i] == session)
            return;
    if (v->n_open_by < AFP_MAX_SESSIONS)
        v->open_by[v->n_open_by++] = session;
}

// Forget that `session` had this volume open.
static void vol_session_remove(vol_t *v, uint16_t session) {
    for (uint32_t i = 0; i < v->n_open_by; i++) {
        if (v->open_by[i] != session)
            continue;
        v->open_by[i] = v->open_by[--v->n_open_by];
        return;
    }
}

static vol_t g_vols[AFP_MAX_VOLUMES];
static uint16_t g_next_vol_id = 1;
static uint16_t g_next_dt_ref = 0x0100;

// Server identity and enablement (object model: appletalk.afp.*).
static char g_afp_server_object[33] = AFP_ENTITY_OBJECT;
static char g_afp_message[AFP_META_COMMENT_MAX + 1];
static bool g_afp_enabled = true;
static atalk_nbp_entry_t *g_afp_nbp_entry;

// Server-wide counters (object model: appletalk.afp.stats).
static atalk_afp_stats_t g_afp_stats;

// Per-error-code tally, indexed by (0 - code) so -5000..-5039 map to 0..39.
#define AFP_ERR_TALLY_BASE  5000
#define AFP_ERR_TALLY_COUNT 48
static uint64_t g_afp_err_tally[AFP_ERR_TALLY_COUNT];

// Record one command outcome for the stats subtree.
static void afp_count_result(uint32_t result) {
    g_afp_stats.commands_served++;
    if (result == AFPERR_NoErr)
        return;
    g_afp_stats.errors++;
    int32_t code = (int32_t)result;
    int idx = -code - AFP_ERR_TALLY_BASE;
    if (idx >= 0 && idx < AFP_ERR_TALLY_COUNT)
        g_afp_err_tally[idx]++;
}

// ============================================================================
// Volume lifecycle and the object-model accessors
// ============================================================================

// Persisted volume record: only the backup date has no per-file home.
#define AFP_VOLREC_MAGIC 0x47535631u // 'GSV1'

// Path of a volume's control-directory record file.
static bool vol_record_path(const vol_t *v, char *out, size_t cap) {
    return (size_t)snprintf(out, cap, "%s/%s/volume", v->root, AFP_CONTROL_DIR) < cap;
}

static void vol_record_load(vol_t *v) {
    char path[PATH_MAX];
    if (!vol_record_path(v, path, sizeof(path)))
        return;
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    uint8_t rec[8];
    if (fread(rec, 1, sizeof(rec), f) == sizeof(rec) && rd32be(rec) == AFP_VOLREC_MAGIC)
        v->backup_date = rd32be(rec + 4);
    fclose(f);
}

static void vol_record_store(const vol_t *v) {
    char path[PATH_MAX];
    if (!vol_record_path(v, path, sizeof(path)))
        return;
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    uint8_t rec[8];
    wr32be(rec, AFP_VOLREC_MAGIC);
    wr32be(rec + 4, v->backup_date);
    fwrite(rec, 1, sizeof(rec), f);
    fclose(f);
}

static int find_vol_slot_by_name(const char *name) {
    if (!name)
        return -1;
    for (int i = 0; i < AFP_MAX_VOLUMES; i++)
        if (g_vols[i].in_use && strcmp(g_vols[i].name, name) == 0)
            return i;
    return -1;
}

static vol_t *find_vol_by_id(uint16_t id) {
    for (int i = 0; i < AFP_MAX_VOLUMES; i++)
        if (g_vols[i].in_use && g_vols[i].vol_id == id)
            return &g_vols[i];
    return NULL;
}

static vol_t *find_vol_by_name(const char *name) {
    int slot = find_vol_slot_by_name(name);
    return slot < 0 ? NULL : &g_vols[slot];
}

// Report a failure through the caller's message buffer as well as the log, so
// the object model can surface the real reason instead of "see log"
// (object-model proposal §2.1, "errors in-band").
static int vol_fail(char *err, size_t err_len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int vol_fail(char *err, size_t err_len, const char *fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (err && err_len)
        snprintf(err, err_len, "%s", buf);
    LOG(2, "AFP volume: %s", buf);
    return -1;
}

int atalk_afp_volume_max(void) {
    return AFP_MAX_VOLUMES;
}

int atalk_afp_volume_add(const char *name, const char *path, char *err, size_t err_len) {
    if (err && err_len)
        err[0] = '\0';
    if (!name || !*name)
        return vol_fail(err, err_len, "volume name is required");
    if (!path || !*path)
        return vol_fail(err, err_len, "volume path is required");
    if (strlen(name) > 32)
        return vol_fail(err, err_len, "volume name max 32 chars ('%s' is %zu)", name, strlen(name));
    if (strchr(name, ':') || strchr(name, '/'))
        return vol_fail(err, err_len, "volume name may not contain ':' or '/'");
    struct stat st;
    if (stat(path, &st) != 0)
        return vol_fail(err, err_len, "path '%s' does not exist (%s)", path, strerror(errno));
    if (!S_ISDIR(st.st_mode))
        return vol_fail(err, err_len, "path '%s' is not a directory", path);
    if (find_vol_slot_by_name(name) >= 0)
        return vol_fail(err, err_len, "volume '%s' already exists", name);

    int slot = -1;
    for (int i = 0; i < AFP_MAX_VOLUMES; i++)
        if (!g_vols[i].in_use) {
            slot = i;
            break;
        }
    if (slot < 0)
        return vol_fail(err, err_len, "volume table full (max %d)", AFP_MAX_VOLUMES);

    vol_t *v = &g_vols[slot];
    memset(v, 0, sizeof(*v));
    snprintf(v->name, sizeof(v->name), "%s", name);
    char resolved[PATH_MAX];
    snprintf(v->root, sizeof(v->root), "%s", realpath(path, resolved) ? resolved : path);
    v->vol_id = g_next_vol_id++;
    v->catalog = afp_catalog_open(v->root);
    if (!v->catalog) {
        memset(v, 0, sizeof(*v));
        return vol_fail(err, err_len, "cannot open the CNID catalog under '%s'", path);
    }
    v->desktop = afp_desktop_open(v->root);
    vol_record_load(v);
    v->in_use = true;
    LOG(1, "AFP: added volume '%s' -> '%s' (vol %u, %u catalog entries)", v->name, v->root, (unsigned)v->vol_id,
        afp_catalog_count(v->catalog));
    return slot;
}

// Release a volume's live state without touching the table entry itself.
static void vol_teardown(vol_t *v) {
    afp_fork_close_volume(v->vol_id);
    if (v->catalog)
        afp_catalog_close(v->catalog);
    if (v->desktop)
        afp_desktop_close(v->desktop);
    v->catalog = NULL;
    v->desktop = NULL;
}

int atalk_afp_volume_remove(const char *name, char *err, size_t err_len) {
    if (err && err_len)
        err[0] = '\0';
    int slot = find_vol_slot_by_name(name);
    if (slot < 0)
        return vol_fail(err, err_len, "no such volume '%s'", name ? name : "");
    vol_teardown(&g_vols[slot]);
    memset(&g_vols[slot], 0, sizeof(g_vols[slot]));
    LOG(1, "AFP: removed volume '%s'", name);
    return 0;
}

int atalk_afp_volume_find(const char *name) {
    return find_vol_slot_by_name(name);
}

bool atalk_afp_volume_in_use(int slot) {
    return slot >= 0 && slot < AFP_MAX_VOLUMES && g_vols[slot].in_use;
}
const char *atalk_afp_volume_name(int slot) {
    return atalk_afp_volume_in_use(slot) ? g_vols[slot].name : NULL;
}
const char *atalk_afp_volume_path(int slot) {
    return atalk_afp_volume_in_use(slot) ? g_vols[slot].root : NULL;
}
unsigned atalk_afp_volume_vol_id(int slot) {
    return atalk_afp_volume_in_use(slot) ? g_vols[slot].vol_id : 0;
}
unsigned atalk_afp_volume_open_forks(int slot) {
    return atalk_afp_volume_in_use(slot) ? afp_fork_count_volume(g_vols[slot].vol_id) : 0;
}
unsigned atalk_afp_volume_sessions_using(int slot) {
    return atalk_afp_volume_in_use(slot) ? g_vols[slot].n_open_by : 0;
}
unsigned atalk_afp_volume_catalog_generation(int slot) {
    return atalk_afp_volume_in_use(slot) ? afp_catalog_generation(g_vols[slot].catalog) : 0;
}
unsigned atalk_afp_volume_cnid_count(int slot) {
    return atalk_afp_volume_in_use(slot) ? afp_catalog_count(g_vols[slot].catalog) : 0;
}

// ============================================================================
// Server identity, enablement, message
// ============================================================================

const char *atalk_afp_get_name(void) {
    return g_afp_server_object;
}

const char *atalk_server_object_name(void) {
    return g_afp_server_object;
}

bool atalk_afp_get_enabled(void) {
    return g_afp_enabled;
}

const char *atalk_afp_get_message(void) {
    return g_afp_message;
}

const char *const *atalk_afp_versions(int *count) {
    if (count)
        *count = ARRAY_LEN(k_afp_versions);
    return k_afp_versions;
}

const atalk_afp_stats_t *atalk_afp_get_stats(void) {
    g_afp_stats.open_forks = afp_fork_count_total();
    return &g_afp_stats;
}

uint64_t atalk_afp_error_count(int32_t code) {
    int idx = -code - AFP_ERR_TALLY_BASE;
    if (idx < 0 || idx >= AFP_ERR_TALLY_COUNT)
        return 0;
    return g_afp_err_tally[idx];
}

int atalk_afp_error_code_at(int index, int32_t *out_code, uint64_t *out_count) {
    int seen = 0;
    for (int i = 0; i < AFP_ERR_TALLY_COUNT; i++) {
        if (!g_afp_err_tally[i])
            continue;
        if (seen++ != index)
            continue;
        if (out_code)
            *out_code = -(AFP_ERR_TALLY_BASE + i);
        if (out_count)
            *out_count = g_afp_err_tally[i];
        return 0;
    }
    return -1;
}

// Register or update the NBP advertisement to match the current name.
static int afp_nbp_publish(void) {
    atalk_nbp_service_desc_t desc = {.object = g_afp_server_object,
                                     .type = AFP_ENTITY_TYPE,
                                     .zone = "*",
                                     .socket = HOST_AFP_SOCKET,
                                     .node = LLAP_HOST_NODE,
                                     .net = 0};
    if (g_afp_nbp_entry)
        return atalk_nbp_update(g_afp_nbp_entry, &desc);
    return atalk_nbp_register(&desc, &g_afp_nbp_entry);
}

// Withdraw the NBP advertisement so the Chooser stops listing the server.
static void afp_nbp_withdraw(void) {
    if (!g_afp_nbp_entry)
        return;
    atalk_nbp_unregister(g_afp_nbp_entry);
    g_afp_nbp_entry = NULL;
}

int atalk_afp_set_name(const char *name, char *err, size_t err_len) {
    if (err && err_len)
        err[0] = '\0';
    if (!name || !*name)
        return vol_fail(err, err_len, "server name is required");
    if (strlen(name) > 32)
        return vol_fail(err, err_len, "server name max 32 chars ('%s' is %zu)", name, strlen(name));
    snprintf(g_afp_server_object, sizeof(g_afp_server_object), "%s", name);
    if (g_afp_enabled && afp_nbp_publish() != 0)
        return vol_fail(err, err_len, "NBP re-registration failed for '%s'", name);
    LOG(1, "AFP: server name is now '%s'", g_afp_server_object);
    return 0;
}

int atalk_afp_set_enabled(bool enabled, char *err, size_t err_len) {
    if (err && err_len)
        err[0] = '\0';
    if (enabled == g_afp_enabled)
        return 0;
    g_afp_enabled = enabled;
    if (enabled) {
        if (afp_nbp_publish() != 0) {
            g_afp_enabled = false;
            return vol_fail(err, err_len, "NBP registration failed");
        }
        LOG(1, "AFP: server enabled");
    } else {
        // Tell every live client the server is going away, then drop their
        // state: NBP lookups stop resolving and OpenSess is refused.
        atalk_asp_broadcast_attention(ATALK_ATTN_SHUTDOWN);
        afp_nbp_withdraw();
        afp_fork_shutdown();
        atalk_asp_close_all_sessions();
        for (int i = 0; i < AFP_MAX_VOLUMES; i++)
            g_vols[i].n_open_by = 0;
        LOG(1, "AFP: server disabled");
    }
    return 0;
}

int atalk_afp_set_message(const char *message, char *err, size_t err_len) {
    if (err && err_len)
        err[0] = '\0';
    if (message && strlen(message) > AFP_META_COMMENT_MAX)
        return vol_fail(err, err_len, "server message max %d chars", AFP_META_COMMENT_MAX);
    snprintf(g_afp_message, sizeof(g_afp_message), "%s", message ? message : "");
    // Nudge every logged-in client to fetch it (AFP_21_22 Table 1-7, "0010").
    if (g_afp_message[0])
        atalk_asp_broadcast_attention(ATALK_ATTN_SERVER_MSG);
    return 0;
}

void atalk_server_init(void) {
    memset(&g_afp_stats, 0, sizeof(g_afp_stats));
    memset(g_afp_err_tally, 0, sizeof(g_afp_err_tally));
    if (!g_afp_enabled)
        return;
    if (afp_nbp_publish() != 0)
        LOG(1, "AFP: failed to register NBP advertisement");
}

void atalk_server_delete(void) {
    for (int i = 0; i < AFP_MAX_VOLUMES; i++) {
        if (!g_vols[i].in_use)
            continue;
        vol_teardown(&g_vols[i]);
        memset(&g_vols[i], 0, sizeof(g_vols[i]));
    }
    afp_fork_shutdown();
    afp_nbp_withdraw();
}

// ============================================================================
// ASP GetStatus - Service Status Block
// ============================================================================

// Helper: write a Pascal string (length byte + bytes). Returns bytes written.
static size_t write_pstr(uint8_t *dst, const char *cstr) {
    size_t n = cstr ? strlen(cstr) : 0;
    if (n > 255)
        n = 255; // truncate to P-string max
    dst[0] = (uint8_t)n;
    if (n)
        memcpy(dst + 1, cstr, n);
    return 1 + n;
}

// Flags word we advertise.  Every bit here is backed by an implementation:
// FPCopyFile is dispatched, FPChangePassword is not, and server messages are
// live now that FPGetSrvrMsg and ASP Attention exist (WP-5/WP-8).
static uint16_t afp_srvr_flags(void) {
    return (uint16_t)(AFP_SRVR_FLAG_COPYFILE | AFP_SRVR_FLAG_SERVERMESSAGES | AFP_SRVR_FLAG_NOSAVEPWD);
}

// Build Service Status Block per docs/core/network/appletalk_server.md
int atalk_build_status_block(const char *server_name, const char *machine_type, uint8_t **out_buf, size_t *out_len) {
    if (!out_buf || !out_len)
        return -1;
    *out_buf = NULL;
    *out_len = 0;

    const size_t versions_count = ARRAY_LEN(k_afp_versions);
    static const char *const kUams[] = {"No User Authent"};
    const size_t uams_count = ARRAY_LEN(kUams);

    // First, compute total size by simulating layout
    size_t pos = 10; // after the 2-byte offsets (0,2,4,6) + 2-byte Flags (8)
    size_t server_name_len = 1 + (server_name ? (strlen(server_name) > 255 ? 255 : strlen(server_name)) : 0);
    size_t machine_type_len = 1 + (machine_type ? (strlen(machine_type) > 255 ? 255 : strlen(machine_type)) : 0);
    pos += server_name_len; // Server Name P-string
    size_t machine_type_off = pos; // remember offset
    pos += machine_type_len; // Machine Type P-string
    size_t afp_versions_cnt_off = pos; // 1 byte count
    pos += 1;
    for (size_t i = 0; i < versions_count; i++) {
        size_t s = strlen(k_afp_versions[i]);
        if (s > 255)
            s = 255;
        pos += 1 + s;
    }
    size_t uam_cnt_off = pos; // 1 byte count
    pos += 1;
    for (size_t i = 0; i < uams_count; i++) {
        size_t s = strlen(kUams[i]);
        if (s > 255)
            s = 255;
        pos += 1 + s;
    }
    // No icon/mask; offset = 0 and no trailing 256 bytes

    size_t total = pos;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf)
        return -2;
    memset(buf, 0, total);

    wr16be(&buf[0], (uint16_t)machine_type_off);
    wr16be(&buf[2], (uint16_t)afp_versions_cnt_off);
    wr16be(&buf[4], (uint16_t)uam_cnt_off);
    wr16be(&buf[6], 0); // Volume Icon and Mask offset (none)
    wr16be(&buf[8], afp_srvr_flags());

    pos = 10;
    pos += write_pstr(&buf[pos], server_name ? server_name : "");
    pos += write_pstr(&buf[pos], machine_type ? machine_type : "");
    buf[pos++] = (uint8_t)versions_count;
    for (size_t i = 0; i < versions_count; i++)
        pos += write_pstr(&buf[pos], k_afp_versions[i]);
    buf[pos++] = (uint8_t)uams_count;
    for (size_t i = 0; i < uams_count; i++)
        pos += write_pstr(&buf[pos], kUams[i]);

    *out_buf = buf;
    *out_len = total;
    return 0;
}

// ============================================================================
// Path helpers
// ============================================================================

static uint32_t afp_unix_time_to_afp(time_t t) {
    return afp_meta_time_from_unix((int64_t)t);
}

static bool afp_path_pop(char *path) {
    if (!path || !*path)
        return false;
    char *slash = strrchr(path, '/');
    if (!slash) {
        path[0] = '\0';
        return true;
    }
    *slash = '\0';
    return true;
}

static bool afp_append_component(char *path, size_t path_len, const char *component) {
    if (!path || !component || !*component)
        return true;
    size_t curr = strlen(path);
    size_t comp_len = strlen(component);
    size_t needed = curr + (curr ? 1 : 0) + comp_len + 1;
    if (needed > path_len)
        return false;
    if (curr)
        path[curr++] = '/';
    memcpy(path + curr, component, comp_len + 1);
    return true;
}

static bool afp_process_component(char *path, size_t path_len, const char *component) {
    if (!path || !component)
        return false;
    if (*component == '\0' || strcmp(component, "..") == 0) {
        if (*path == '\0')
            return false;
        return afp_path_pop(path);
    }
    if (strcmp(component, ".") == 0)
        return true;
    char clean[AFP_MAX_NAME];
    size_t len = 0;
    for (const char *p = component; *p && len + 1 < sizeof(clean); p++) {
        if (*p == '/' || *p == ':')
            continue;
        clean[len++] = *p;
    }
    clean[len] = '\0';
    if (len == 0)
        return true;
    // Defense in depth: no client path may name the server's control
    // directory, even though FPEnumerate already hides it (§4.1).
    if (afp_meta_is_hidden(clean))
        return false;
    return afp_append_component(path, path_len, clean);
}

static bool afp_normalize_relative_path(const char *base_rel, const char *suffix, char *out, size_t out_len) {
    if (!out || out_len == 0)
        return false;
    out[0] = '\0';
    if (base_rel && *base_rel) {
        if (strlen(base_rel) >= out_len)
            return false;
        strcpy(out, base_rel);
    }
    if (!suffix || !*suffix)
        return true;
    char token[AFP_MAX_NAME];
    size_t token_len = 0;
    for (size_t i = 0;; i++) {
        char ch = suffix[i];
        bool is_sep = (ch == '\0') || ch == ':' || ch == '/' || ch == '\\';
        if (!is_sep) {
            if (token_len + 1 < sizeof(token))
                token[token_len++] = ch;
        }
        if (is_sep) {
            token[token_len] = '\0';
            if (token_len == 0 && ch != '\0') {
                if (*out == '\0')
                    return false;
                if (!afp_path_pop(out))
                    return false;
            } else if (token_len > 0 && !afp_process_component(out, out_len, token)) {
                return false;
            }
            token_len = 0;
            if (ch == '\0')
                break;
        }
    }
    return true;
}

static void afp_extract_parent(const char *rel_path, char *parent, size_t parent_len) {
    if (!parent || parent_len == 0)
        return;
    parent[0] = '\0';
    if (!rel_path || !*rel_path)
        return;
    snprintf(parent, parent_len, "%s", rel_path);
    afp_path_pop(parent);
}

static const char *afp_last_component(const char *rel_path) {
    if (!rel_path || !*rel_path)
        return NULL;
    const char *slash = strrchr(rel_path, '/');
    return slash ? (slash + 1) : rel_path;
}

static bool afp_build_child_path(const char *parent, const char *child, char *out, size_t out_len) {
    if (!out || out_len == 0 || !child)
        return false;
    out[0] = '\0';
    if (parent && *parent) {
        if (strlen(parent) >= out_len)
            return false;
        strcpy(out, parent);
    }
    return afp_append_component(out, out_len, child);
}

static bool afp_full_path(const vol_t *vol, const char *rel, char *out, size_t out_len) {
    if (!vol || !out || out_len == 0)
        return false;
    if (!rel || !*rel)
        return (size_t)snprintf(out, out_len, "%s", vol->root) < out_len;
    const char *sep = (vol->root[0] && vol->root[strlen(vol->root) - 1] == '/') ? "" : "/";
    return (size_t)snprintf(out, out_len, "%s%s%s", vol->root, sep, rel) < out_len;
}

static bool afp_stat_path(vol_t *vol, const char *rel, struct stat *st) {
    char full[PATH_MAX];
    if (!afp_full_path(vol, rel, full, sizeof(full)))
        return false;
    return stat(full, st) == 0;
}

// ============================================================================
// Catalog glue
// ============================================================================

// Volume-relative path of a directory CNID, adopting the root as needed.
// Returns false when the CNID is unknown.
static bool afp_dir_rel_path(vol_t *vol, uint32_t dir_id, char *out, size_t cap) {
    if (!vol || !vol->catalog)
        return false;
    if (dir_id == 0 || dir_id == AFP_CNID_ROOT) {
        out[0] = '\0';
        return true;
    }
    const afp_cat_entry_t *e = afp_catalog_find(vol->catalog, dir_id);
    if (!e)
        return false;
    return afp_catalog_path(vol->catalog, dir_id, out, cap);
}

// Adopt (or find) the catalog entry for a volume-relative path, deciding
// file-vs-directory from the host filesystem.  This is the lazy-adoption
// policy: anything the server touches gets an entry (§4.2).
static const afp_cat_entry_t *afp_entry_for(vol_t *vol, const char *rel_path) {
    if (!vol || !vol->catalog)
        return NULL;
    if (!rel_path || !*rel_path)
        return afp_catalog_find(vol->catalog, AFP_CNID_ROOT);
    struct stat st;
    bool is_dir = afp_stat_path(vol, rel_path, &st) && S_ISDIR(st.st_mode);
    return afp_catalog_resolve_path(vol->catalog, rel_path, true, is_dir);
}

// CNID of a path's parent directory (AFP_CNID_ROOT_PARENT for the root).
static uint32_t afp_parent_cnid(vol_t *vol, const char *rel_path) {
    if (!vol || !rel_path || !*rel_path)
        return AFP_CNID_ROOT_PARENT;
    char parent[AFP_MAX_REL_PATH];
    afp_extract_parent(rel_path, parent, sizeof(parent));
    if (!parent[0])
        return AFP_CNID_ROOT;
    const afp_cat_entry_t *e = afp_entry_for(vol, parent);
    return e ? e->cnid : AFP_CNID_ROOT;
}

// CNID of a path itself (its FileNumber / Directory ID).
static uint32_t afp_cnid_of(vol_t *vol, const char *rel_path) {
    const afp_cat_entry_t *e = afp_entry_for(vol, rel_path);
    return e ? e->cnid : AFP_CNID_ROOT;
}

// Note a catalog-visible change so live FPEnumerate snapshots are rebuilt.
static void afp_vol_touch(vol_t *vol) {
    if (vol)
        vol->mutations++;
}

static uint16_t afp_count_offspring(const char *full_path) {
    if (!full_path)
        return 0;
    DIR *dir = opendir(full_path);
    if (!dir)
        return 0;
    uint32_t count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (afp_meta_is_hidden(ent->d_name))
            continue; // sidecars and .gs-afp are not AFP-visible
        if (++count >= UINT16_MAX)
            break;
    }
    closedir(dir);
    return (uint16_t)count;
}

// Directory access rights.  The server is guest-only, so every caller gets
// full Search/Read/Write in all four right bytes (user, owner, group,
// everyone).  Mapping host permissions here instead would hand the guest a
// user-rights byte of 0 and the AppleShare client greys out the volume.
// Real per-directory rights are the deferred WP-13 work.
#define AFP_ACCESS_RIGHTS_ALL 0x07070707u

// The AFP attribute word for one object: the persisted inhibit/visibility
// bits from its sidecar, plus the live DAlreadyOpen / RAlreadyOpen bits and a
// WriteInhibit implied by host permissions.
static uint16_t afp_attributes_of(const char *host_path, const struct stat *st, const afp_meta_t *meta) {
    uint16_t attrs = 0;
    if (meta && meta->has_attrs)
        attrs |= (uint16_t)(meta->attrs & AFP_ATTR_PERSISTED);
    if (st && !(st->st_mode & S_IWUSR))
        attrs |= AFP_ATTR_WRITEINHIBIT;
    attrs |= afp_fork_open_attrs(host_path);
    return attrs;
}

// ============================================================================
// File/directory parameter areas
// ============================================================================

// Width of one bitmap field, keyed by bit.  0 marks an undefined bit.
static int afp_param_field_width(bool is_dir, int bit) {
    switch (bit) {
    case 0:
        return 2; // Attributes
    case 1:
        return 4; // Parent Directory ID
    case 2:
        return 4; // Creation Date
    case 3:
        return 4; // Modification Date
    case 4:
        return 4; // Backup Date
    case 5:
        return 32; // Finder Info
    case 6:
        return 2; // Long Name offset
    case 7:
        return 2; // Short Name offset
    case 8:
        return 4; // File Number / Directory ID
    case 9:
        return is_dir ? 2 : 4; // Offspring Count / Data Fork Length
    case 10:
        return 4; // Owner ID / Resource Fork Length
    case 11:
        return is_dir ? 4 : 0; // Group ID
    case 12:
        return is_dir ? 4 : 0; // Access Rights
    case 13:
        return 6; // ProDOS Info
    default:
        return 0;
    }
}

static int afp_fixed_param_len(bool is_dir, uint16_t bm) {
    int fixed = 0;
    for (int b = 0; b < 16; b++)
        if (bm & (1u << b))
            fixed += afp_param_field_width(is_dir, b);
    return fixed;
}

// Offset of one bitmap field inside a parameter area starting at `pbase`,
// or -1 when the bit is not selected.
static int afp_param_field_ptr(bool is_dir, uint16_t bm, int pbase, int target_bit) {
    if (!(bm & (1u << target_bit)))
        return -1;
    int off = 0;
    for (int b = 0; b < target_bit; b++)
        if (bm & (1u << b))
            off += afp_param_field_width(is_dir, b);
    return pbase + off;
}

// Reserve and zero the fixed part of a parameter area, remembering where the
// long/short name offsets go.  Returns the end position, or -1 on overflow.
static int afp_write_param_area(bool is_dir, uint16_t bm, uint8_t *out, int p, int out_max, int *pos_long_off,
                                int *pos_short_off) {
    if (pos_long_off)
        *pos_long_off = -1;
    if (pos_short_off)
        *pos_short_off = -1;
    for (int b = 0; b < 16; b++) {
        if (!(bm & (1u << b)))
            continue;
        int width = afp_param_field_width(is_dir, b);
        if (width == 0)
            continue;
        if (p + width > out_max)
            return -1;
        if (b == 6 && pos_long_off)
            *pos_long_off = p;
        if (b == 7 && pos_short_off)
            *pos_short_off = p;
        memset(out + p, 0, (size_t)width);
        p += width;
    }
    return p;
}

static int afp_write_name_vars(uint8_t *out, int vpos, int out_max, int pbase, const char *nm, uint16_t bm,
                               int pos_long_off, int pos_short_off, uint8_t long_len, uint8_t short_len) {
    if (!nm)
        nm = "";
    if ((bm & (1u << 6))) {
        if (vpos + 1 + (int)long_len > out_max)
            return -1;
        if (pos_long_off >= 0)
            wr16be(out + pos_long_off, (uint16_t)(vpos - pbase));
        out[vpos++] = long_len;
        if (long_len) {
            memcpy(&out[vpos], nm, long_len);
            vpos += long_len;
        }
    }
    if ((bm & (1u << 7))) {
        if (vpos + 1 + (int)short_len > out_max)
            return -1;
        if (pos_short_off >= 0)
            wr16be(out + pos_short_off, (uint16_t)(vpos - pbase));
        out[vpos++] = short_len;
        if (short_len) {
            memcpy(&out[vpos], nm, short_len);
            vpos += short_len;
        }
    }
    return vpos;
}

// Fill a reserved parameter area with the object's real values.  Every field
// a client can ask for is served from the catalog (IDs) or the AppleDouble
// sidecar (dates, Finder Info, attributes) rather than being synthesised.
static bool afp_populate_param_area(bool is_dir, vol_t *vol, const char *rel_path, const struct stat *st, uint16_t bm,
                                    uint8_t *out, int pbase) {
    if (!st || !vol)
        return false;
    char full[PATH_MAX];
    if (!afp_full_path(vol, rel_path ? rel_path : "", full, sizeof(full)))
        return false;
    afp_meta_t meta;
    afp_meta_load(full, &meta);

    int ptr;
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 5)) >= 0)
        memcpy(out + ptr, meta.has_finder ? meta.finder : (const uint8_t[AFP_META_FINDER_SIZE]){0},
               AFP_META_FINDER_SIZE);
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 0)) >= 0)
        wr16be(out + ptr, afp_attributes_of(full, st, &meta));
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 1)) >= 0)
        wr32be(out + ptr, afp_parent_cnid(vol, rel_path));
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 2)) >= 0) {
        // The create date is server-owned metadata; st_ctime is an inode
        // change time and is not it.  Fall back to the modification time so a
        // file that never went through AFP still reports something sane.
        wr32be(out + ptr, meta.has_dates ? meta.create_date : afp_unix_time_to_afp(st->st_mtime));
    }
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 3)) >= 0)
        wr32be(out + ptr, afp_unix_time_to_afp(st->st_mtime)); // host is authoritative
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 4)) >= 0)
        wr32be(out + ptr, meta.has_dates ? meta.backup_date : AFP_DATE_NEVER);
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 8)) >= 0)
        wr32be(out + ptr, afp_cnid_of(vol, rel_path));
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 13)) >= 0)
        memset(out + ptr, 0, 6);

    if (!is_dir) {
        if ((ptr = afp_param_field_ptr(false, bm, pbase, 9)) >= 0)
            wr32be(out + ptr, (uint32_t)st->st_size);
        if ((ptr = afp_param_field_ptr(false, bm, pbase, 10)) >= 0)
            wr32be(out + ptr, afp_meta_rsrc_len(full));
    } else {
        if ((ptr = afp_param_field_ptr(true, bm, pbase, 9)) >= 0)
            wr16be(out + ptr, afp_count_offspring(full));
        if ((ptr = afp_param_field_ptr(true, bm, pbase, 10)) >= 0)
            wr32be(out + ptr, 0); // Owner ID — guest-only server
        if ((ptr = afp_param_field_ptr(true, bm, pbase, 11)) >= 0)
            wr32be(out + ptr, 0); // Group ID
        if ((ptr = afp_param_field_ptr(true, bm, pbase, 12)) >= 0)
            wr32be(out + ptr, AFP_ACCESS_RIGHTS_ALL);
    }
    return true;
}

static int afp_read_pstring(const uint8_t *in, int in_len, int pos, char *dst, size_t dst_len) {
    if (!in || !dst || dst_len == 0 || pos >= in_len)
        return -1;
    uint8_t raw_len = in[pos++];
    if (pos + raw_len > in_len)
        raw_len = (uint8_t)((in_len > pos) ? (in_len - pos) : 0);
    size_t copy_len = raw_len;
    if (copy_len >= dst_len)
        copy_len = dst_len - 1;
    if (copy_len > 0)
        memcpy(dst, &in[pos], copy_len);
    dst[copy_len] = '\0';
    pos += raw_len;
    return pos;
}

// ============================================================================
// Volume parameter block (WP-1: honest sizes and dates)
// ============================================================================

// Free/total bytes for a share, from the host filesystem and clamped to what
// a 32-bit classic client can hold.  A statvfs failure falls back to a fixed
// pair so an OPFS quirk can never make the volume look full.
static void afp_volume_space(const vol_t *v, uint32_t *out_free, uint32_t *out_total) {
    uint64_t free_bytes = AFP_VOL_FALLBACK_FREE;
    uint64_t total_bytes = AFP_VOL_FALLBACK_TOTAL;
    struct statvfs vfs;
    if (statvfs(v->root, &vfs) == 0 && vfs.f_blocks > 0) {
        uint64_t unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
        total_bytes = (uint64_t)vfs.f_blocks * unit;
        free_bytes = (uint64_t)vfs.f_bavail * unit;
    } else {
        LOG(2, "AFP: statvfs('%s') unavailable — reporting the fallback volume size", v->root);
    }
    if (total_bytes > AFP_VOL_SIZE_CEILING)
        total_bytes = AFP_VOL_SIZE_CEILING;
    if (free_bytes > AFP_VOL_SIZE_CEILING)
        free_bytes = AFP_VOL_SIZE_CEILING;
    if (free_bytes > total_bytes)
        free_bytes = total_bytes;
    *out_free = (uint32_t)free_bytes;
    *out_total = (uint32_t)total_bytes;
}

// Volume Attributes word.  The capability bits are per-volume and only set
// when the corresponding code path exists (AFP_21_22 p. 18) — and only for a
// session that negotiated 2.1: in AFP 2.0 every bit above ReadOnly is
// "reserved, must be 0", and a 2.0 client that sees one set reads the word as
// something else entirely (measured: System 6's AppleShare 2.0.2 mounts the
// volume software-locked).
static uint16_t afp_volume_attributes(const vol_t *v, bool afp21) {
    uint16_t attr = 0;
    if (afp21 && v->catalog) {
        attr |= AFP_VOL_ATTR_FILEIDS; // FPCreateID/DeleteID/ResolveID
        attr |= AFP_VOL_ATTR_CATSEARCH; // FPCatSearch
    }
    if (access(v->root, W_OK) != 0)
        attr |= AFP_VOL_ATTR_READONLY;
    return attr;
}

static int afp_write_vol_param_block(vol_t *v, uint16_t *bitmap_ptr, uint8_t *out, int out_max, bool afp21) {
    if (!v || out_max < 2)
        return 0;
    uint16_t bitmap = bitmap_ptr ? *bitmap_ptr : 0;
    int param_start = 2;
    int fixed_len = 0;
    static const int k_vol_widths[9] = {2, 2, 4, 4, 4, 2, 4, 4, 2};
    for (int b = 0; b < 9; b++)
        if (bitmap & (1u << b))
            fixed_len += k_vol_widths[b];

    size_t name_len = strlen(v->name);
    if (name_len > 255)
        name_len = 255;
    int var_len = (bitmap & 0x0100) ? 1 + (int)name_len : 0;
    int total_len = 2 + fixed_len + var_len;
    if (total_len > out_max) {
        // Drop the volume name rather than truncating the fixed block.
        if (bitmap & 0x0100) {
            bitmap &= (uint16_t)~0x0100;
            fixed_len -= 2;
            var_len = 0;
            total_len = 2 + fixed_len;
        }
        if (total_len > out_max)
            return 0;
    }
    wr16be(out, bitmap);

    struct stat root_st;
    bool have_root = stat(v->root, &root_st) == 0;
    uint32_t bytes_free = 0, bytes_total = 0;
    afp_volume_space(v, &bytes_free, &bytes_total);

    int p = param_start;
    int var_base = param_start + fixed_len;
    if (bitmap & 0x0001) {
        wr16be(&out[p], afp_volume_attributes(v, afp21));
        p += 2;
    }
    if (bitmap & 0x0002) {
        wr16be(&out[p], 0x0002); // fixed directory-ID signature
        p += 2;
    }
    if (bitmap & 0x0004) {
        wr32be(&out[p], have_root ? afp_unix_time_to_afp(root_st.st_ctime) : 0);
        p += 4;
    }
    if (bitmap & 0x0008) {
        wr32be(&out[p], have_root ? afp_unix_time_to_afp(root_st.st_mtime) : 0);
        p += 4;
    }
    if (bitmap & 0x0010) {
        wr32be(&out[p], v->backup_date ? v->backup_date : AFP_DATE_NEVER);
        p += 4;
    }
    if (bitmap & 0x0020) {
        wr16be(&out[p], v->vol_id);
        p += 2;
    }
    if (bitmap & 0x0040) {
        wr32be(&out[p], bytes_free);
        p += 4;
    }
    if (bitmap & 0x0080) {
        wr32be(&out[p], bytes_total);
        p += 4;
    }
    if (bitmap & 0x0100) {
        wr16be(&out[p], (uint16_t)(var_base - param_start));
        p += 2;
    }
    int vpos = var_base;
    if (bitmap & 0x0100) {
        if (vpos + 1 + (int)name_len > out_max)
            return vpos;
        out[vpos++] = (uint8_t)name_len;
        if (name_len) {
            memcpy(&out[vpos], v->name, name_len);
            vpos += (int)name_len;
        }
    }
    if (bitmap_ptr)
        *bitmap_ptr = bitmap;
    return vpos;
}

// ============================================================================
// Per-command context and shared resolution
// ============================================================================

// What every handler knows about the session it is answering for.  The AFP
// layer keeps no session table of its own — appletalk.c owns that — but each
// command needs the session's identity to attribute forks and enumeration
// snapshots to it.
typedef struct {
    uint16_t session_id;
} afp_ctx_t;

typedef uint32_t (*afp_command_handler_fn)(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                           int *out_len);

typedef struct {
    uint8_t opcode;
    const char *name;
    afp_command_handler_fn handler;
} afp_command_handler_t;

// True when this session negotiated AFP 2.1, which is what gates the 2.1
// capability advertising as well as the 2.1 commands themselves.
static bool afp_session_is_21(const afp_ctx_t *ctx) {
    const char *ver = ctx ? atalk_asp_session_afp_version(ctx->session_id) : NULL;
    return ver && strcmp(ver, "AFPVersion 2.1") == 0;
}

// Resolve the (Volume ID, Directory ID, Pathname) triple every catalog call
// carries into a volume plus a volume-relative path.  Returns an AFP result
// code; AFPERR_NoErr means `*out_vol` / `out_rel` are usable.
static uint32_t afp_resolve_target(uint16_t vol_id, uint32_t dir_id, const char *path, vol_t **out_vol, char *out_rel,
                                   size_t rel_cap) {
    vol_t *vol = find_vol_by_id(vol_id);
    if (!vol)
        return AFPERR_ParamErr; // unknown volume identifier
    char base[AFP_MAX_REL_PATH];
    if (!afp_dir_rel_path(vol, dir_id, base, sizeof(base)))
        return AFPERR_DirNotFound;
    if (!afp_normalize_relative_path(base, path, out_rel, rel_cap))
        return AFPERR_ParamErr;
    if (out_vol)
        *out_vol = vol;
    return AFPERR_NoErr;
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
    default:
        return AFPERR_MiscErr;
    }
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
static void enum_snapshots_drop_session(uint16_t session_id) {
    for (int i = 0; i < AFP_MAX_ENUM_SNAPSHOTS; i++)
        if (g_enum_snapshots[i].in_use && g_enum_snapshots[i].session_id == session_id)
            enum_snapshot_free(&g_enum_snapshots[i]);
}

// Drop every snapshot belonging to a volume (share removal / checkpoint restore).
static void enum_snapshots_drop_volume(uint16_t vol_id) {
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
        if (afp_meta_is_hidden(dent->d_name))
            continue; // sidecars and the .gs-afp control directory
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
// Server-level commands
// ============================================================================

// FPGetSrvrInfo (0x0F) — the reply the ASP GetStatus block carries.  Clients
// normally read it out-of-band via SPGetStatus; serving the same bytes here
// keeps the two paths from drifting.
static uint32_t afp_cmd_get_srvr_info(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                      int *out_len) {
    (void)ctx;
    (void)in;
    (void)in_len;
    uint8_t *block = NULL;
    size_t block_len = 0;
    if (atalk_build_status_block(g_afp_server_object, "GrannySmith", &block, &block_len) != 0)
        return AFPERR_MiscErr;
    int n = (int)block_len;
    if (n > out_max)
        n = out_max;
    memcpy(out, block, (size_t)n);
    free(block);
    if (out_len)
        *out_len = n;
    LOG(10, "AFP FPGetSrvrInfo: reply=%d", n);
    return AFPERR_NoErr;
}

// FPGetSrvrParms (0x10) — server time plus the volume list.
static uint32_t afp_cmd_get_srvr_parms(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                       int *out_len) {
    (void)ctx;
    (void)in;
    (void)in_len;
    if (out_max < 5)
        return AFPERR_ParamErr;
    int count = 0;
    for (int i = 0; i < AFP_MAX_VOLUMES; i++)
        if (g_vols[i].in_use)
            count++;

    wr32be(out, afp_unix_time_to_afp(time(NULL)));
    int pos = 4;
    out[pos++] = (uint8_t)count;
    for (int i = 0; i < AFP_MAX_VOLUMES; i++) {
        if (!g_vols[i].in_use)
            continue;
        const char *name = g_vols[i].name;
        size_t n = strlen(name);
        if (n > 31)
            n = 31; // HFS name limit
        if (pos + 2 + (int)n > out_max)
            break;
        out[pos++] = 0x00; // flags: no volume password, not configured
        out[pos++] = (uint8_t)n;
        memcpy(&out[pos], name, n);
        pos += (int)n;
    }
    if (out_len)
        *out_len = pos;
    LOG(10, "AFP FPGetSrvrParms: numvols=%d reply=%d", count, pos);
    return AFPERR_NoErr;
}

// FPGetVolParms (0x11)
static uint32_t afp_cmd_get_vol_parms(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                      int *out_len) {
    if (in_len < 5 || out_max < 2)
        return AFPERR_ParamErr;
    uint16_t vol_id = rd16be(in + 1);
    uint16_t bitmap = rd16be(in + 3);
    vol_t *v = find_vol_by_id(vol_id);
    if (!v)
        return AFPERR_ParamErr;
    int produced = afp_write_vol_param_block(v, &bitmap, out, out_max, afp_session_is_21(ctx));
    if (produced <= 0)
        return AFPERR_ParamErr;
    if (out_len)
        *out_len = produced;
    LOG(10, "AFP FPGetVolParms: vol=0x%04X bitmap=0x%04X reply=%d", vol_id, bitmap, produced);
    return AFPERR_NoErr;
}

// FPOpenVol (0x18)
static uint32_t afp_cmd_open_vol(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                 int *out_len) {
    if (in_len < 4)
        return AFPERR_ParamErr;
    afp_log_hex("AFP FPOpenVol req", in, in_len);
    uint16_t bitmap = rd16be(in + 1);
    char vol_name[33];
    int pos = afp_read_pstring(in, in_len, 3, vol_name, sizeof(vol_name));
    if (pos < 0)
        return AFPERR_ParamErr;

    vol_t *v = find_vol_by_name(vol_name);
    if (!v)
        return AFPERR_ObjectNotFound;
    // The client must get the volume ID back to address anything on it.
    bitmap |= 0x0020;
    int written = afp_write_vol_param_block(v, &bitmap, out, out_max, afp_session_is_21(ctx));
    if (written <= 0)
        return AFPERR_ParamErr;
    vol_session_add(v, ctx->session_id);
    if (out_len)
        *out_len = written;
    LOG(2, "AFP FPOpenVol: '%s' volId=0x%04X bitmap=0x%04X reply=%d (session 0x%04X)", v->name, v->vol_id, bitmap,
        written, ctx->session_id);
    afp_log_hex("AFP FPOpenVol resp", out, written);
    return AFPERR_NoErr;
}

// FPCloseVol (0x02)
static uint32_t afp_cmd_close_vol(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                  int *out_len) {
    (void)out;
    (void)out_max;
    if (in_len < 3)
        return AFPERR_ParamErr;
    uint16_t vol_id = rd16be(in + 1);
    vol_t *v = find_vol_by_id(vol_id);
    if (!v)
        return AFPERR_ParamErr;
    vol_session_remove(v, ctx->session_id);
    enum_snapshots_drop_session(ctx->session_id);
    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPCloseVol: volId=0x%04X", vol_id);
    return AFPERR_NoErr;
}

// FPSetVolParms (0x20) — only the backup date is settable, and it is now
// persisted in the volume's control record instead of being dropped.
static uint32_t afp_cmd_set_vol_parms(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                      int *out_len) {
    (void)ctx;
    (void)out;
    (void)out_max;
    if (out_len)
        *out_len = 0;
    if (in_len < 5)
        return AFPERR_ParamErr;
    uint16_t vol_id = rd16be(in + 1);
    uint16_t bitmap = rd16be(in + 3);
    vol_t *v = find_vol_by_id(vol_id);
    if (!v)
        return AFPERR_ParamErr;
    if (bitmap & ~0x0010u)
        return AFPERR_BitmapErr; // backup date is the only settable parameter
    if (in_len < 9)
        return AFPERR_ParamErr;
    v->backup_date = rd32be(in + 5);
    vol_record_store(v);
    LOG(10, "AFP FPSetVolParms: vol=0x%04X backupDate=%u", vol_id, v->backup_date);
    return AFPERR_NoErr;
}

// FPLogin (0x12) — guest-only, but the negotiated version is remembered so
// FPGetSrvrMsg and the other 2.1 calls can gate on it.
static uint32_t afp_cmd_login(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    if (in_len < 2)
        return AFPERR_ParamErr;
    char ver[64];
    int pos = afp_read_pstring(in, in_len, 0, ver, sizeof(ver));
    if (pos < 0)
        return AFPERR_ParamErr;
    char uam[64];
    if (afp_read_pstring(in, in_len, pos, uam, sizeof(uam)) < 0)
        return AFPERR_ParamErr;
    LOG(10, "AFP FPLogin: version='%s' uam='%s'", ver, uam);

    bool ver_ok = false;
    for (int i = 0; i < ARRAY_LEN(k_afp_versions); i++)
        if (strcmp(ver, k_afp_versions[i]) == 0)
            ver_ok = true;
    if (!ver_ok) {
        LOG(7, "AFP FPLogin: unsupported version → BadVersNum");
        return AFPERR_BadVersNum;
    }
    if (strcmp(uam, "No User Authent") != 0) {
        LOG(7, "AFP FPLogin: unsupported UAM → BadUAM");
        return AFPERR_BadUAM;
    }
    atalk_asp_session_set_afp_version(ctx->session_id, ver);
    if (out_max < 2)
        return AFPERR_ParamErr;
    wr16be(out, 0x0000); // guest login carries no user ID
    if (out_len)
        *out_len = 2;
    return AFPERR_NoErr;
}

// FPLoginCont (0x13) — unreachable while the only UAM is "No User Authent".
static uint32_t afp_cmd_login_cont(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                   int *out_len) {
    (void)ctx;
    (void)in;
    (void)in_len;
    (void)out;
    (void)out_max;
    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPLoginCont: rejected (no multi-step UAM)");
    return AFPERR_ParamErr;
}

// FPLogout (0x14) — drop everything this session held.
static uint32_t afp_cmd_logout(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)in;
    (void)in_len;
    (void)out;
    (void)out_max;
    afp_session_closed(ctx->session_id);
    if (out_len)
        *out_len = 0;
    LOG(2, "AFP FPLogout: session 0x%04X", ctx->session_id);
    return AFPERR_NoErr;
}

// FPChangePassword (0x24) — correct answer for a guest-only server.
static uint32_t afp_cmd_change_password(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                        int *out_len) {
    (void)ctx;
    (void)in;
    (void)in_len;
    (void)out;
    (void)out_max;
    if (out_len)
        *out_len = 0;
    return AFPERR_CallNotSupported;
}

// FPMapID (0x15) / FPMapName (0x16) / FPGetUserInfo (0x25) — a consistent
// single-user fiction; WP-13 documents where a real user database would plug in.
static uint32_t afp_cmd_map_id(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)ctx;
    if (in_len < 5)
        return AFPERR_ParamErr;
    uint8_t subfunc = in[0];
    uint32_t id = rd32be(in + 1);
    const char *name = (id == 0) ? "" : (subfunc == 1 ? "guest" : "staff");
    uint8_t name_len = (uint8_t)strlen(name);
    if (out_max < 1 + (int)name_len)
        return AFPERR_ParamErr;
    out[0] = name_len;
    if (name_len)
        memcpy(out + 1, name, name_len);
    if (out_len)
        *out_len = 1 + (int)name_len;
    return AFPERR_NoErr;
}

static uint32_t afp_cmd_map_name(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                 int *out_len) {
    (void)ctx;
    (void)in;
    if (in_len < 1 || out_max < 4)
        return AFPERR_ParamErr;
    wr32be(out, 0); // every name maps to the guest ID
    if (out_len)
        *out_len = 4;
    return AFPERR_NoErr;
}

static uint32_t afp_cmd_get_user_info(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                      int *out_len) {
    (void)ctx;
    if (in_len < 5 || out_max < 6)
        return AFPERR_ParamErr;
    uint16_t bitmap = (in_len >= 7) ? rd16be(in + 5) : 0x0003;
    if (bitmap & ~0x0003u)
        return AFPERR_BitmapErr;
    int p = 0;
    wr16be(out + p, bitmap);
    p += 2;
    if (bitmap & 0x0001) {
        wr32be(out + p, 0);
        p += 4;
    }
    if (bitmap & 0x0002) {
        wr32be(out + p, 0);
        p += 4;
    }
    if (out_len)
        *out_len = p;
    return AFPERR_NoErr;
}

// FPGetSrvrMsg (0x26) — AFP 2.1.  MsgType 0 = logon, 1 = server; the bitmap
// currently selects only the message string itself (AFP_21_22 p. 55).
static uint32_t afp_cmd_get_srvr_msg(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                     int *out_len) {
    (void)ctx;
    if (in_len < 5 || out_max < 4)
        return AFPERR_ParamErr;
    uint16_t msg_type = rd16be(in + 1);
    uint16_t bitmap = rd16be(in + 3);
    if (bitmap & ~0x0001u)
        return AFPERR_BitmapErr;
    if (msg_type > 1)
        return AFPERR_ParamErr;

    const char *msg = g_afp_message;
    size_t len = strlen(msg);
    if (len > AFP_META_COMMENT_MAX)
        len = AFP_META_COMMENT_MAX;
    if (out_max < 4 + 1 + (int)len)
        return AFPERR_ParamErr;
    wr16be(out + 0, msg_type);
    wr16be(out + 2, bitmap);
    out[4] = (uint8_t)len;
    if (len)
        memcpy(out + 5, msg, len);
    if (out_len)
        *out_len = 5 + (int)len;
    LOG(10, "AFP FPGetSrvrMsg: type=%u len=%zu", msg_type, len);
    return AFPERR_NoErr;
}

// ============================================================================
// Directory and parameter commands
// ============================================================================

// FPOpenDir (0x19)
static uint32_t afp_cmd_open_dir(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                 int *out_len) {
    (void)ctx;
    if (in_len < 10 || out_max < 4)
        return AFPERR_ParamErr;
    uint16_t vol_id = rd16be(in + 1);
    uint32_t dir_id = rd32be(in + 3);
    char path[AFP_MAX_NAME];
    if (afp_read_pstring(in, in_len, 8, path, sizeof(path)) < 0)
        return AFPERR_ParamErr;

    vol_t *vol = NULL;
    char target_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_target(vol_id, dir_id, path, &vol, target_rel, sizeof(target_rel));
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
    wr32be(out, entry->cnid);
    if (out_len)
        *out_len = 4;
    LOG(10, "AFP FPOpenDir: vol=0x%04X parent=0x%08X path='%s' → cnid=0x%08X", vol_id, dir_id,
        target_rel[0] ? target_rel : "<root>", entry->cnid);
    return AFPERR_NoErr;
}

// FPCloseDir (0x03) — the server holds no per-open directory state; the call
// exists so a client can retire a Directory ID it no longer needs.
static uint32_t afp_cmd_close_dir(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                  int *out_len) {
    (void)ctx;
    (void)out;
    (void)out_max;
    if (in_len < 7)
        return AFPERR_ParamErr;
    uint16_t vol_id = rd16be(in + 1);
    uint32_t dir_id = rd32be(in + 3);
    vol_t *vol = find_vol_by_id(vol_id);
    if (!vol)
        return AFPERR_ParamErr;
    if (dir_id != 0 && dir_id != AFP_CNID_ROOT && !afp_catalog_find(vol->catalog, dir_id))
        return AFPERR_ParamErr;
    if (out_len)
        *out_len = 0;
    return AFPERR_NoErr;
}

// FPGetFileDirParms (0x22)
static uint32_t afp_cmd_get_file_dir_parms(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                           int *out_len) {
    (void)ctx;
    if (in_len < 11)
        return AFPERR_ParamErr;
    afp_log_hex("AFP FPGetFileDirParms req", in, in_len);
    uint16_t vol_id = rd16be(in + 1);
    uint32_t dir_id = rd32be(in + 3);
    uint16_t file_bm = rd16be(in + 7);
    uint16_t dir_bm = rd16be(in + 9);
    char path[AFP_MAX_NAME];
    if (afp_read_pstring(in, in_len, 12, path, sizeof(path)) < 0)
        return AFPERR_ParamErr;
    if (file_bm == 0 && dir_bm == 0)
        return AFPERR_BitmapErr;

    vol_t *vol = NULL;
    char target_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_target(vol_id, dir_id, path, &vol, target_rel, sizeof(target_rel));
    if (rc != AFPERR_NoErr)
        return rc;
    struct stat st;
    if (!afp_stat_path(vol, target_rel, &st))
        return AFPERR_ObjectNotFound;
    bool is_dir = S_ISDIR(st.st_mode);
    uint16_t selected_bm = is_dir ? dir_bm : file_bm;
    const char *name = target_rel[0] ? afp_last_component(target_rel) : vol->name;
    if (!name)
        name = vol->name;

    if (out_max < 6)
        return AFPERR_ParamErr;
    wr16be(out + 0, file_bm);
    wr16be(out + 2, dir_bm);
    out[4] = is_dir ? 0x80 : 0x00;
    out[5] = 0x00;

    int pbase = 6;
    int pos_long_off = -1, pos_short_off = -1;
    int p = pbase;
    if (selected_bm) {
        p = afp_write_param_area(is_dir, selected_bm, out, pbase, out_max, &pos_long_off, &pos_short_off);
        if (p < 0)
            return AFPERR_ParamErr;
        if (!afp_populate_param_area(is_dir, vol, target_rel, &st, selected_bm, out, pbase))
            return AFPERR_ParamErr;
    }
    size_t nlen = strlen(name);
    int vpos = afp_write_name_vars(out, p, out_max, pbase, name, selected_bm, pos_long_off, pos_short_off,
                                   (uint8_t)(nlen > 255 ? 255 : nlen), (uint8_t)(nlen > 31 ? 31 : nlen));
    if (vpos < 0)
        return AFPERR_ParamErr;
    if ((vpos % 2) && vpos < out_max)
        out[vpos++] = 0x00;
    if (out_len)
        *out_len = vpos;
    afp_log_hex("AFP FPGetFileDirParms resp", out, vpos);
    LOG(2, "AFP FPGetFileDirParms: vol=0x%04X type=%s path='%s' reply=%d", vol_id, is_dir ? "dir" : "file",
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
static uint32_t afp_parse_set_parms(const uint8_t *in, int in_len) {
    if (in_len < 9)
        return AFPERR_ParamErr;
    uint16_t vol_id = rd16be(in + 1);
    uint32_t dir_id = rd32be(in + 3);
    uint16_t bitmap = rd16be(in + 7);
    char path[AFP_MAX_NAME];
    int pos = afp_read_pstring(in, in_len, 10, path, sizeof(path));
    if (pos < 0)
        return AFPERR_ParamErr;

    vol_t *vol = NULL;
    char target_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_target(vol_id, dir_id, path, &vol, target_rel, sizeof(target_rel));
    if (rc != AFPERR_NoErr)
        return rc;
    struct stat st;
    if (!afp_stat_path(vol, target_rel, &st))
        return AFPERR_ObjectNotFound;
    bool is_dir = S_ISDIR(st.st_mode);
    char full[PATH_MAX];
    if (!afp_full_path(vol, target_rel, full, sizeof(full)))
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
    int avail = in_len - pos;
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
        if (pos + width > in_len)
            return AFPERR_ParamErr;
        const uint8_t *field = in + pos;
        pos += width;
        switch (bit) {
        case 0: // Attributes
            afp_apply_attribute_word(&meta, rd16be(field));
            touched = true;
            break;
        case 2: // Creation date
            meta.create_date = rd32be(field);
            meta.has_dates = true;
            touched = true;
            break;
        case 3: // Modification date — written through to the host too
            new_mtime = rd32be(field);
            set_mtime = true;
            meta.modify_date = new_mtime;
            meta.has_dates = true;
            touched = true;
            break;
        case 4: // Backup date
            meta.backup_date = rd32be(field);
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
    LOG(2, "AFP SetParms: vol=0x%04X bitmap=0x%04X path='%s'", vol_id, bitmap, target_rel[0] ? target_rel : "<root>");
    return AFPERR_NoErr;
}

static uint32_t afp_cmd_set_file_parms(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                       int *out_len) {
    (void)ctx;
    (void)out;
    (void)out_max;
    if (out_len)
        *out_len = 0;
    return afp_parse_set_parms(in, in_len);
}

static uint32_t afp_cmd_set_dir_parms(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                      int *out_len) {
    (void)ctx;
    (void)out;
    (void)out_max;
    if (out_len)
        *out_len = 0;
    return afp_parse_set_parms(in, in_len);
}

static uint32_t afp_cmd_set_file_dir_parms(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                           int *out_len) {
    (void)ctx;
    (void)out;
    (void)out_max;
    if (out_len)
        *out_len = 0;
    return afp_parse_set_parms(in, in_len);
}

// ============================================================================
// Fork commands
// ============================================================================

// FPOpenFork (0x1A)
static uint32_t afp_cmd_open_fork(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                  int *out_len) {
    if (in_len < 12)
        return AFPERR_ParamErr;
    bool is_resource = (in[0] & 0x80) != 0;
    uint16_t vol_id = rd16be(in + 1);
    uint32_t dir_id = rd32be(in + 3);
    uint16_t bitmap = rd16be(in + 7);
    uint16_t access_mode = rd16be(in + 9);
    char path[AFP_MAX_NAME];
    if (afp_read_pstring(in, in_len, 12, path, sizeof(path)) < 0)
        return AFPERR_ParamErr;

    vol_t *vol = NULL;
    char target_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_target(vol_id, dir_id, path, &vol, target_rel, sizeof(target_rel));
    if (rc != AFPERR_NoErr)
        return rc;
    struct stat st;
    if (!afp_stat_path(vol, target_rel, &st))
        return AFPERR_ObjectNotFound;
    if (S_ISDIR(st.st_mode))
        return AFPERR_ObjectTypeErr;
    char full[PATH_MAX];
    if (!afp_full_path(vol, target_rel, full, sizeof(full)))
        return AFPERR_ParamErr;
    // A WriteInhibit file may still be read, never written.
    if ((access_mode & AFP_ACCESS_WRITE) && afp_inhibited(full, AFP_ATTR_WRITEINHIBIT))
        return AFPERR_AccessDenied;

    afp_fork_t *fk = NULL;
    afp_fork_status_t st_open = afp_fork_open(vol_id, ctx->session_id, full, target_rel, is_resource, access_mode, &fk);
    // On DenyConflict the client still gets the file parameters, so it can
    // work out whether it is the holder (Inside AppleTalk ch. 13, FPOpenFork).
    if (st_open != AFP_FORK_OK && st_open != AFP_FORK_DENY_CONFLICT)
        return afp_fork_status_to_err(st_open);

    afp_catalog_resolve_path(vol->catalog, target_rel, true, false);
    if (out_max < 4)
        return AFPERR_ParamErr;
    wr16be(out + 0, bitmap);
    wr16be(out + 2, fk ? afp_fork_ref(fk) : 0);

    int pbase = 4;
    int p = pbase;
    if (bitmap) {
        int pos_long_off = -1, pos_short_off = -1;
        p = afp_write_param_area(false, bitmap, out, pbase, out_max, &pos_long_off, &pos_short_off);
        if (p < 0 || !afp_populate_param_area(false, vol, target_rel, &st, bitmap, out, pbase)) {
            if (fk)
                afp_fork_close(fk);
            return AFPERR_ParamErr;
        }
        const char *name = afp_last_component(target_rel);
        size_t nlen = name ? strlen(name) : 0;
        p = afp_write_name_vars(out, p, out_max, pbase, name ? name : "", bitmap, pos_long_off, pos_short_off,
                                (uint8_t)(nlen > 255 ? 255 : nlen), (uint8_t)(nlen > 31 ? 31 : nlen));
        if (p < 0) {
            if (fk)
                afp_fork_close(fk);
            return AFPERR_ParamErr;
        }
    }
    if (p % 2 && p < out_max)
        out[p++] = 0x00;
    if (out_len)
        *out_len = p;
    if (st_open == AFP_FORK_DENY_CONFLICT) {
        LOG(2, "AFP FPOpenFork: deny conflict on '%s' (%s)", target_rel, is_resource ? "rsrc" : "data");
        return AFPERR_DenyConflict;
    }
    LOG(2, "AFP FPOpenFork: vol=0x%04X %s path='%s' → ref=0x%04X reply=%d", vol_id, is_resource ? "rsrc" : "data",
        target_rel, afp_fork_ref(fk), p);
    return AFPERR_NoErr;
}

// FPCloseFork (0x04)
static uint32_t afp_cmd_close_fork(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                   int *out_len) {
    (void)ctx;
    (void)out;
    (void)out_max;
    if (in_len < 3)
        return AFPERR_ParamErr;
    afp_fork_t *fk = afp_fork_find(rd16be(in + 1));
    if (!fk)
        return AFPERR_ParamErr;
    afp_fork_close(fk);
    if (out_len)
        *out_len = 0;
    return AFPERR_NoErr;
}

// FPRead (0x1B)
static uint32_t afp_cmd_read(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)ctx;
    if (in_len < 11)
        return AFPERR_ParamErr;
    uint16_t fork_ref = rd16be(in + 1);
    uint32_t offset = rd32be(in + 3);
    uint32_t req_count = rd32be(in + 7);
    uint8_t newline_mask = (in_len > 11) ? in[11] : 0;
    uint8_t newline_char = (in_len > 12) ? in[12] : 0;

    afp_fork_t *fk = afp_fork_find(fork_ref);
    if (!fk)
        return AFPERR_ParamErr;
    if (!(afp_fork_access_mode(fk) & AFP_ACCESS_READ))
        return AFPERR_AccessDenied;

    uint32_t fork_len = afp_fork_length(fk);
    if (offset >= fork_len) {
        if (out_len)
            *out_len = 0;
        return AFPERR_EOFErr;
    }
    uint32_t to_read = req_count;
    if (to_read > fork_len - offset)
        to_read = fork_len - offset;
    if (to_read > (uint32_t)out_max)
        to_read = (uint32_t)out_max;

    uint32_t got = 0;
    afp_fork_status_t st = afp_fork_read(fk, offset, to_read, out, &got);
    if (st != AFP_FORK_OK)
        return afp_fork_status_to_err(st);

    // Newline mode: stop at the first byte matching under the mask.
    if (newline_mask) {
        for (uint32_t i = 0; i < got; i++) {
            if ((out[i] & newline_mask) == (newline_char & newline_mask)) {
                got = i + 1;
                break;
            }
        }
    }
    g_afp_stats.bytes_read += got;
    if (out_len)
        *out_len = (int)got;
    LOG(2, "AFP FPRead: ref=0x%04X off=%u req=%u got=%u", fork_ref, offset, req_count, got);
    if (got < req_count && offset + got >= fork_len)
        return AFPERR_EOFErr;
    return AFPERR_NoErr;
}

// FPWrite (0x21) — the payload follows the 11-byte parameter header.
static uint32_t afp_cmd_write(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)ctx;
    if (in_len < 11 || out_max < 4)
        return AFPERR_ParamErr;
    bool from_end = (in[0] & 0x80) != 0;
    uint16_t fork_ref = rd16be(in + 1);
    uint32_t offset = rd32be(in + 3);
    uint32_t req_count = rd32be(in + 7);

    afp_fork_t *fk = afp_fork_find(fork_ref);
    if (!fk)
        return AFPERR_ParamErr;
    if (!(afp_fork_access_mode(fk) & AFP_ACCESS_WRITE))
        return AFPERR_AccessDenied;

    const uint8_t *payload = in + 11;
    uint32_t payload_len = (uint32_t)(in_len - 11);
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
    wr32be(out, start + written);
    if (out_len)
        *out_len = 4;
    LOG(2, "AFP FPWrite: ref=0x%04X off=%u req=%u wrote=%u", fork_ref, start, req_count, written);
    return AFPERR_NoErr;
}

// FPGetForkParms (0x0E)
static uint32_t afp_cmd_get_fork_parms(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                       int *out_len) {
    (void)ctx;
    if (in_len < 5 || out_max < 2)
        return AFPERR_ParamErr;
    uint16_t fork_ref = rd16be(in + 1);
    uint16_t bitmap = rd16be(in + 3);
    afp_fork_t *fk = afp_fork_find(fork_ref);
    if (!fk)
        return AFPERR_ParamErr;
    vol_t *vol = find_vol_by_id(afp_fork_vol_id(fk));
    if (!vol)
        return AFPERR_ParamErr;
    struct stat st;
    if (stat(afp_fork_host_path(fk), &st) != 0)
        return AFPERR_ObjectNotFound;

    wr16be(out + 0, bitmap);
    int pbase = 2;
    int p = pbase;
    if (bitmap) {
        int pos_long_off = -1, pos_short_off = -1;
        p = afp_write_param_area(false, bitmap, out, pbase, out_max, &pos_long_off, &pos_short_off);
        if (p < 0 || !afp_populate_param_area(false, vol, afp_fork_rel_path(fk), &st, bitmap, out, pbase))
            return AFPERR_ParamErr;
        // Report the open fork's live length: the sidecar only catches up on
        // flush, and the data fork may have grown since the stat above.
        int lp = afp_param_field_ptr(false, bitmap, pbase, afp_fork_is_resource(fk) ? 10 : 9);
        if (lp >= 0)
            wr32be(out + lp, afp_fork_length(fk));
        const char *name = afp_last_component(afp_fork_rel_path(fk));
        size_t nlen = name ? strlen(name) : 0;
        p = afp_write_name_vars(out, p, out_max, pbase, name ? name : "", bitmap, pos_long_off, pos_short_off,
                                (uint8_t)(nlen > 255 ? 255 : nlen), (uint8_t)(nlen > 31 ? 31 : nlen));
        if (p < 0)
            return AFPERR_ParamErr;
    }
    if (p % 2 && p < out_max)
        out[p++] = 0x00;
    if (out_len)
        *out_len = p;
    return AFPERR_NoErr;
}

// FPSetForkParms (0x1F) — the only settable parameter is the fork length.
static uint32_t afp_cmd_set_fork_parms(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                       int *out_len) {
    (void)ctx;
    (void)out;
    (void)out_max;
    if (in_len < 9)
        return AFPERR_ParamErr;
    uint16_t fork_ref = rd16be(in + 1);
    uint16_t bitmap = rd16be(in + 3);
    afp_fork_t *fk = afp_fork_find(fork_ref);
    if (!fk)
        return AFPERR_ParamErr;
    if (bitmap & ~((1u << 9) | (1u << 10)))
        return AFPERR_BitmapErr;
    if (!(bitmap & ((1u << 9) | (1u << 10))))
        return AFPERR_BitmapErr;
    uint32_t new_len = rd32be(in + 5);
    afp_fork_status_t st = afp_fork_truncate(fk, new_len);
    if (st != AFP_FORK_OK)
        return afp_fork_status_to_err(st);
    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPSetForkParms: ref=0x%04X len=%u", fork_ref, new_len);
    return AFPERR_NoErr;
}

// FPFlush (0x0A)
static uint32_t afp_cmd_flush(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)ctx;
    (void)out;
    (void)out_max;
    if (in_len < 3)
        return AFPERR_ParamErr;
    uint16_t vol_id = rd16be(in + 1);
    if (!find_vol_by_id(vol_id))
        return AFPERR_ParamErr;
    afp_fork_flush_volume(vol_id);
    if (out_len)
        *out_len = 0;
    return AFPERR_NoErr;
}

// FPFlushFork (0x0B)
static uint32_t afp_cmd_flush_fork(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                   int *out_len) {
    (void)ctx;
    (void)out;
    (void)out_max;
    if (in_len < 3)
        return AFPERR_ParamErr;
    afp_fork_t *fk = afp_fork_find(rd16be(in + 1));
    if (!fk)
        return AFPERR_ParamErr;
    afp_fork_flush(fk);
    if (out_len)
        *out_len = 0;
    return AFPERR_NoErr;
}

// FPByteRangeLock (0x01) — real ranges now, checked against every other open
// of the same fork (WP-6).
static uint32_t afp_cmd_byte_range_lock(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                        int *out_len) {
    (void)ctx;
    if (in_len < 11 || out_max < 4)
        return AFPERR_ParamErr;
    uint8_t flag = in[0];
    bool unlock = (flag & 0x01) != 0; // bit 0: 0 = lock, 1 = unlock
    bool end_relative = (flag & 0x80) != 0; // bit 7: offset measured from EOF
    uint16_t fork_ref = rd16be(in + 1);
    int32_t offset = (int32_t)rd32be(in + 3);
    uint32_t length = rd32be(in + 7);

    afp_fork_t *fk = afp_fork_find(fork_ref);
    if (!fk)
        return AFPERR_ParamErr;
    uint32_t range_start = 0;
    afp_fork_status_t st = afp_fork_range_lock(fk, unlock, end_relative, offset, length, &range_start);
    if (st != AFP_FORK_OK)
        return afp_fork_status_to_err(st);
    wr32be(out, range_start);
    if (out_len)
        *out_len = 4;
    LOG(10, "AFP FPByteRangeLock: ref=0x%04X %s start=%u len=%u", fork_ref, unlock ? "unlock" : "lock", range_start,
        length);
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
static uint32_t afp_cmd_create_dir(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                   int *out_len) {
    (void)ctx;
    if (in_len < 8 || out_max < 4)
        return AFPERR_ParamErr;
    uint16_t vol_id = rd16be(in + 1);
    uint32_t dir_id = rd32be(in + 3);
    char path[AFP_MAX_NAME];
    if (afp_read_pstring(in, in_len, 8, path, sizeof(path)) < 0)
        return AFPERR_ParamErr;

    vol_t *vol = NULL;
    char target_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_target(vol_id, dir_id, path, &vol, target_rel, sizeof(target_rel));
    if (rc != AFPERR_NoErr)
        return rc;
    if (!target_rel[0])
        return AFPERR_ParamErr;
    char full[PATH_MAX];
    if (!afp_full_path(vol, target_rel, full, sizeof(full)))
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
    wr32be(out, entry->cnid);
    if (out_len)
        *out_len = 4;
    LOG(10, "AFP FPCreateDir: '%s' → cnid=0x%08X", target_rel, entry->cnid);
    return AFPERR_NoErr;
}

// FPCreateFile (0x07) — flag bit 7 selects a hard create (overwrite).
static uint32_t afp_cmd_create_file(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                    int *out_len) {
    (void)ctx;
    (void)out;
    (void)out_max;
    if (in_len < 8)
        return AFPERR_ParamErr;
    bool hard_create = (in[0] & 0x80) != 0;
    uint16_t vol_id = rd16be(in + 1);
    uint32_t dir_id = rd32be(in + 3);
    char path[AFP_MAX_NAME];
    if (afp_read_pstring(in, in_len, 8, path, sizeof(path)) < 0)
        return AFPERR_ParamErr;

    vol_t *vol = NULL;
    char target_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_target(vol_id, dir_id, path, &vol, target_rel, sizeof(target_rel));
    if (rc != AFPERR_NoErr)
        return rc;
    if (!target_rel[0])
        return AFPERR_ParamErr;
    char full[PATH_MAX];
    if (!afp_full_path(vol, target_rel, full, sizeof(full)))
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
    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPCreateFile: '%s' hard=%d", target_rel, hard_create ? 1 : 0);
    return AFPERR_NoErr;
}

// FPDelete (0x08)
static uint32_t afp_cmd_delete(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)ctx;
    (void)out;
    (void)out_max;
    if (in_len < 8)
        return AFPERR_ParamErr;
    uint16_t vol_id = rd16be(in + 1);
    uint32_t dir_id = rd32be(in + 3);
    char path[AFP_MAX_NAME];
    if (afp_read_pstring(in, in_len, 8, path, sizeof(path)) < 0)
        return AFPERR_ParamErr;

    vol_t *vol = NULL;
    char target_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_target(vol_id, dir_id, path, &vol, target_rel, sizeof(target_rel));
    if (rc != AFPERR_NoErr)
        return rc;
    if (!target_rel[0])
        return AFPERR_AccessDenied; // the volume root is not deletable
    char full[PATH_MAX];
    if (!afp_full_path(vol, target_rel, full, sizeof(full)))
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
            char child[PATH_MAX];
            if (snprintf(child, sizeof(child), "%s/%s", full, ent->d_name) < (int)sizeof(child))
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
    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPDelete: '%s' (%s)", target_rel, S_ISDIR(st.st_mode) ? "dir" : "file");
    return AFPERR_NoErr;
}

// FPRename (0x1C) — same parent, new name; the CNID is preserved.
static uint32_t afp_cmd_rename(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)ctx;
    (void)out;
    (void)out_max;
    if (in_len < 8)
        return AFPERR_ParamErr;
    uint16_t vol_id = rd16be(in + 1);
    uint32_t dir_id = rd32be(in + 3);
    char old_name[AFP_MAX_NAME];
    int pos = afp_read_pstring(in, in_len, 8, old_name, sizeof(old_name));
    if (pos < 0 || pos >= in_len)
        return AFPERR_ParamErr;
    pos++; // new pathname's PathType byte
    char new_name[AFP_MAX_NAME];
    if (afp_read_pstring(in, in_len, pos, new_name, sizeof(new_name)) < 0)
        return AFPERR_ParamErr;

    vol_t *vol = NULL;
    char old_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_target(vol_id, dir_id, old_name, &vol, old_rel, sizeof(old_rel));
    if (rc != AFPERR_NoErr)
        return rc;
    if (!old_rel[0])
        return AFPERR_CantRename; // renaming the volume itself is not supported
    char old_full[PATH_MAX];
    if (!afp_full_path(vol, old_rel, old_full, sizeof(old_full)))
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
    if (!afp_full_path(vol, new_rel, new_full, sizeof(new_full)))
        return AFPERR_ParamErr;
    if (strcmp(old_rel, new_rel) == 0)
        return AFPERR_NoErr;
    struct stat dst_st;
    if (stat(new_full, &dst_st) == 0)
        return AFPERR_ObjectExists;
    if (rename(old_full, new_full) != 0)
        return AFPERR_CantRename;
    afp_sidecar_rename(old_full, new_full);
    afp_fork_repoint(old_full, new_full, new_rel);

    const afp_cat_entry_t *entry = afp_catalog_resolve_path(vol->catalog, old_rel, true, S_ISDIR(st.st_mode));
    if (entry)
        afp_catalog_rename(vol->catalog, entry->cnid, afp_last_component(new_rel));

    afp_vol_touch(vol);
    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPRename: '%s' → '%s'", old_rel, new_rel);
    return AFPERR_NoErr;
}

// FPMoveAndRename (0x17) — new parent and optionally a new name; the CNID and
// every descendant CNID survive the move.
static uint32_t afp_cmd_move_and_rename(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                        int *out_len) {
    (void)ctx;
    (void)out;
    (void)out_max;
    if (in_len < 12)
        return AFPERR_ParamErr;
    uint16_t vol_id = rd16be(in + 1);
    uint32_t src_dir_id = rd32be(in + 3);
    uint32_t dst_dir_id = rd32be(in + 7);
    char src_path[AFP_MAX_NAME];
    int pos = afp_read_pstring(in, in_len, 12, src_path, sizeof(src_path));
    if (pos < 0 || pos >= in_len)
        return AFPERR_ParamErr;
    pos++; // destination PathType
    char dst_path[AFP_MAX_NAME];
    pos = afp_read_pstring(in, in_len, pos, dst_path, sizeof(dst_path));
    if (pos < 0)
        return AFPERR_ParamErr;
    char new_name[AFP_MAX_NAME];
    new_name[0] = '\0';
    if (pos < in_len) {
        pos++; // new-name PathType
        afp_read_pstring(in, in_len, pos, new_name, sizeof(new_name));
    }

    vol_t *vol = NULL;
    char src_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_target(vol_id, src_dir_id, src_path, &vol, src_rel, sizeof(src_rel));
    if (rc != AFPERR_NoErr)
        return rc;
    if (!src_rel[0])
        return AFPERR_CantMove;
    char dst_dir_rel[AFP_MAX_REL_PATH];
    rc = afp_resolve_target(vol_id, dst_dir_id, dst_path, &vol, dst_dir_rel, sizeof(dst_dir_rel));
    if (rc != AFPERR_NoErr)
        return rc;

    char src_full[PATH_MAX];
    if (!afp_full_path(vol, src_rel, src_full, sizeof(src_full)))
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
    if (!afp_full_path(vol, dst_rel, dst_full, sizeof(dst_full)))
        return AFPERR_ParamErr;
    struct stat dst_st;
    if (stat(dst_full, &dst_st) == 0)
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
    if (out_len)
        *out_len = 0;
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
static uint32_t afp_cmd_copy_file(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                  int *out_len) {
    (void)out;
    (void)out_max;
    if (in_len < 14)
        return AFPERR_ParamErr;
    // Pad(1) SrcVolID(2) SrcDirID(4) DstVolID(2) DstDirID(4) — the two volume
    // IDs are not adjacent; the source's directory ID sits between them.
    uint16_t src_vol_id = rd16be(in + 1);
    uint32_t src_dir = rd32be(in + 3);
    uint16_t dst_vol_id = rd16be(in + 7);
    uint32_t dst_dir = rd32be(in + 9);
    char src_name[AFP_MAX_NAME];
    int pos = afp_read_pstring(in, in_len, 14, src_name, sizeof(src_name));
    if (pos < 0 || pos >= in_len)
        return AFPERR_ParamErr;
    pos++; // destination PathType
    char dst_name[AFP_MAX_NAME];
    pos = afp_read_pstring(in, in_len, pos, dst_name, sizeof(dst_name));
    if (pos < 0)
        return AFPERR_ParamErr;
    char new_name[AFP_MAX_NAME];
    new_name[0] = '\0';
    if (pos < in_len) {
        pos++; // new-name PathType
        afp_read_pstring(in, in_len, pos, new_name, sizeof(new_name));
    }

    vol_t *svol = NULL, *dvol = NULL;
    char src_rel[AFP_MAX_REL_PATH], dst_dir_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_target(src_vol_id, src_dir, src_name, &svol, src_rel, sizeof(src_rel));
    if (rc != AFPERR_NoErr)
        return rc;
    rc = afp_resolve_target(dst_vol_id, dst_dir, dst_name, &dvol, dst_dir_rel, sizeof(dst_dir_rel));
    if (rc != AFPERR_NoErr)
        return rc;

    char src_full[PATH_MAX];
    if (!afp_full_path(svol, src_rel, src_full, sizeof(src_full)))
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
    if (!afp_full_path(dvol, dst_rel, dst_full, sizeof(dst_full)))
        return AFPERR_ParamErr;
    struct stat dst_st;
    if (stat(dst_full, &dst_st) == 0)
        return AFPERR_ObjectExists;

    // The source is held for reading with writers denied for the duration of
    // the copy (Inside AppleTalk ch. 13, FPCopyFile: "Read, DenyWrite").
    afp_fork_t *guard = NULL;
    afp_fork_status_t gs =
        afp_fork_open(src_vol_id, ctx->session_id, src_full, src_rel, false, AFP_ACCESS_READ | AFP_DENY_WRITE, &guard);
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
    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPCopyFile: '%s' → '%s'", src_rel, dst_rel);
    return AFPERR_NoErr;
}

// ============================================================================
// FPEnumerate (0x09)
// ============================================================================

static uint32_t afp_cmd_enumerate(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                  int *out_len) {
    if (in_len < 18)
        return AFPERR_ParamErr;
    afp_log_hex("AFP FPEnumerate req", in, in_len);
    uint16_t vol_id = rd16be(in + 1);
    uint32_t dir_id = rd32be(in + 3);
    uint16_t file_bm = rd16be(in + 7);
    uint16_t dir_bm = rd16be(in + 9);
    uint16_t req_count = rd16be(in + 11);
    uint16_t start_index = rd16be(in + 13);
    uint16_t max_reply = rd16be(in + 15);
    char path[AFP_MAX_NAME];
    if (afp_read_pstring(in, in_len, 18, path, sizeof(path)) < 0)
        return AFPERR_ParamErr;
    if (start_index == 0)
        start_index = 1;
    if (file_bm == 0 && dir_bm == 0)
        return AFPERR_BitmapErr;

    vol_t *vol = NULL;
    char target_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_target(vol_id, dir_id, path, &vol, target_rel, sizeof(target_rel));
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

    wr16be(out + 0, file_bm);
    wr16be(out + 2, dir_bm);
    wr16be(out + 4, 0);
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
        size_t nlen = strlen(entry->name);
        int vpos = afp_write_name_vars(out, p, max_bytes, pbase, entry->name, bm, pos_long_off, pos_short_off,
                                       (uint8_t)(nlen > 255 ? 255 : nlen), (uint8_t)(nlen > 31 ? 31 : nlen));
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

    wr16be(out + 4, actual);
    if (actual == 0)
        return AFPERR_ObjectNotFound;
    if (out_len)
        *out_len = w;
    LOG(10, "AFP FPEnumerate: vol=0x%04X dir='%s' start=%u req=%u returned=%u total=%zu", vol_id,
        target_rel[0] ? target_rel : "<root>", start_index, req_count, actual, snap->count);
    afp_log_hex("AFP FPEnumerate resp", out, w);
    return AFPERR_NoErr;
}

// ============================================================================
// Desktop database (WP-9)
// ============================================================================

// Resolve a client's DTRefNum to the volume that issued it.
static vol_t *find_vol_by_dt_ref(uint16_t dt_ref) {
    if (!dt_ref)
        return NULL;
    for (int i = 0; i < AFP_MAX_VOLUMES; i++)
        if (g_vols[i].in_use && g_vols[i].dt_ref == dt_ref)
            return &g_vols[i];
    return NULL;
}

// FPOpenDT (0x30)
static uint32_t afp_cmd_open_dt(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                int *out_len) {
    (void)ctx;
    if (in_len < 3 || out_max < 2)
        return AFPERR_ParamErr;
    vol_t *v = find_vol_by_id(rd16be(in + 1));
    if (!v)
        return AFPERR_ParamErr;
    if (!v->dt_ref)
        v->dt_ref = g_next_dt_ref++;
    if (!v->desktop)
        v->desktop = afp_desktop_open(v->root);
    wr16be(out, v->dt_ref);
    if (out_len)
        *out_len = 2;
    LOG(10, "AFP FPOpenDT: vol='%s' → DTRef=0x%04X", v->name, v->dt_ref);
    return AFPERR_NoErr;
}

// FPCloseDT (0x31) — the stores stay open (and persistent); only the client's
// reference goes away.
static uint32_t afp_cmd_close_dt(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                 int *out_len) {
    (void)ctx;
    (void)out;
    (void)out_max;
    if (in_len < 3)
        return AFPERR_ParamErr;
    vol_t *v = find_vol_by_dt_ref(rd16be(in + 1));
    if (!v)
        return AFPERR_ParamErr;
    v->dt_ref = 0;
    if (out_len)
        *out_len = 0;
    return AFPERR_NoErr;
}

// FPAddIcon (0xC0) — arrives as an ASP Write, so the bitmap follows the header.
static uint32_t afp_cmd_add_icon(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                 int *out_len) {
    (void)ctx;
    (void)out;
    (void)out_max;
    // Pad(1) DTRefNum(2) FileCreator(4) FileType(4) IconType(1) Pad(1)
    // IconTag(4) BitmapSize(2), then the bitmap streamed via ASP Write.
    if (in_len < 19)
        return AFPERR_ParamErr;
    uint16_t dt_ref = rd16be(in + 1);
    uint32_t creator = rd32be(in + 3);
    uint32_t file_type = rd32be(in + 7);
    uint8_t icon_type = in[11];
    uint32_t icon_tag = rd32be(in + 13);
    uint16_t icon_size = rd16be(in + 17);

    vol_t *v = find_vol_by_dt_ref(dt_ref);
    if (!v || !v->desktop)
        return AFPERR_ParamErr;
    const uint8_t *data = in + 19;
    int avail = in_len - 19;
    if (avail < 0)
        avail = 0;
    if (icon_size > avail)
        icon_size = (uint16_t)avail;
    if (icon_size > AFP_ICON_MAX_BYTES)
        return AFPERR_IconTypeError;
    // Replacing an existing icon with one of a different size is an error,
    // not a silent resize (appletalk_server.md FPAddIcon details).
    const afp_icon_t *existing = afp_desktop_get_icon(v->desktop, creator, file_type, icon_type);
    if (existing && existing->size != icon_size)
        return AFPERR_IconTypeError;
    if (afp_desktop_put_icon(v->desktop, creator, file_type, icon_type, icon_tag, data, icon_size) != 0)
        return AFPERR_MiscErr;
    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPAddIcon: creator=0x%08X type=0x%08X iconType=%u size=%u", creator, file_type, icon_type, icon_size);
    return AFPERR_NoErr;
}

// FPGetIcon (0x33)
static uint32_t afp_cmd_get_icon(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                 int *out_len) {
    (void)ctx;
    if (in_len < 14)
        return AFPERR_ParamErr;
    vol_t *v = find_vol_by_dt_ref(rd16be(in + 1));
    if (!v || !v->desktop)
        return AFPERR_ParamErr;
    uint32_t creator = rd32be(in + 3);
    uint32_t file_type = rd32be(in + 7);
    uint8_t icon_type = in[11];
    uint16_t req_size = rd16be(in + 12);

    const afp_icon_t *icon = afp_desktop_get_icon(v->desktop, creator, file_type, icon_type);
    if (!icon)
        return AFPERR_ItemNotFound; // DTDBMgr.a requires afpItemNotFound on a miss
    int sz = icon->size;
    if (req_size && sz > req_size)
        sz = req_size;
    if (sz > out_max)
        sz = out_max;
    memcpy(out, icon->bitmap, (size_t)sz);
    if (out_len)
        *out_len = sz;
    return AFPERR_NoErr;
}

// FPGetIconInfo (0x34)
static uint32_t afp_cmd_get_icon_info(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                      int *out_len) {
    (void)ctx;
    if (in_len < 9 || out_max < 12)
        return AFPERR_ParamErr;
    vol_t *v = find_vol_by_dt_ref(rd16be(in + 1));
    if (!v || !v->desktop)
        return AFPERR_ParamErr;
    uint32_t creator = rd32be(in + 3);
    uint16_t index = rd16be(in + 7);
    const afp_icon_t *icon = afp_desktop_icon_at(v->desktop, creator, index);
    if (!icon)
        return AFPERR_ItemNotFound;
    wr32be(out + 0, icon->tag);
    wr32be(out + 4, icon->file_type);
    out[8] = icon->icon_type;
    out[9] = 0;
    wr16be(out + 10, icon->size);
    if (out_len)
        *out_len = 12;
    return AFPERR_NoErr;
}

// FPAddAPPL (0x35) — the mapping is keyed by the application's CNID, so
// renaming it through AFP no longer orphans the entry.
static uint32_t afp_cmd_add_appl(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                 int *out_len) {
    (void)ctx;
    (void)out;
    (void)out_max;
    if (in_len < 15)
        return AFPERR_ParamErr;
    vol_t *v = find_vol_by_dt_ref(rd16be(in + 1));
    if (!v || !v->desktop)
        return AFPERR_ParamErr;
    uint32_t dir_id = rd32be(in + 3);
    uint32_t creator = rd32be(in + 7);
    uint32_t appl_tag = rd32be(in + 11);
    char path[AFP_MAX_NAME];
    if (afp_read_pstring(in, in_len, 16, path, sizeof(path)) < 0)
        return AFPERR_ParamErr;

    vol_t *resolved = NULL;
    char target_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_target(v->vol_id, dir_id, path, &resolved, target_rel, sizeof(target_rel));
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
    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPAddAPPL: creator=0x%08X cnid=0x%08X path='%s'", creator, entry->cnid, target_rel);
    return AFPERR_NoErr;
}

// FPRemoveAPPL (0x36)
static uint32_t afp_cmd_remove_appl(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                    int *out_len) {
    (void)ctx;
    (void)out;
    (void)out_max;
    if (in_len < 11)
        return AFPERR_ParamErr;
    vol_t *v = find_vol_by_dt_ref(rd16be(in + 1));
    if (!v || !v->desktop)
        return AFPERR_ParamErr;
    uint32_t dir_id = rd32be(in + 3);
    uint32_t creator = rd32be(in + 7);
    char path[AFP_MAX_NAME];
    uint32_t cnid = 0;
    if (in_len > 12 && afp_read_pstring(in, in_len, 12, path, sizeof(path)) >= 0 && path[0]) {
        vol_t *resolved = NULL;
        char target_rel[AFP_MAX_REL_PATH];
        if (afp_resolve_target(v->vol_id, dir_id, path, &resolved, target_rel, sizeof(target_rel)) == AFPERR_NoErr) {
            const afp_cat_entry_t *entry = afp_catalog_resolve_path(v->catalog, target_rel, false, false);
            if (entry)
                cnid = entry->cnid;
        }
    }
    if (afp_desktop_remove_appl(v->desktop, creator, cnid) == 0)
        return AFPERR_ItemNotFound;
    if (out_len)
        *out_len = 0;
    return AFPERR_NoErr;
}

// FPGetAPPL (0x37)
static uint32_t afp_cmd_get_appl(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                 int *out_len) {
    (void)ctx;
    if (in_len < 9 || out_max < 6)
        return AFPERR_ParamErr;
    vol_t *v = find_vol_by_dt_ref(rd16be(in + 1));
    if (!v || !v->desktop)
        return AFPERR_ParamErr;
    uint32_t creator = rd32be(in + 3);
    uint16_t index = rd16be(in + 7);
    uint16_t bitmap = (in_len >= 11) ? rd16be(in + 9) : 0;

    const afp_appl_t *appl = afp_desktop_appl_at(v->desktop, creator, index);
    if (!appl)
        return AFPERR_ItemNotFound;
    char rel[AFP_MAX_REL_PATH];
    if (!afp_catalog_path(v->catalog, appl->cnid, rel, sizeof(rel)))
        return AFPERR_ItemNotFound;
    struct stat st;
    if (!afp_stat_path(v, rel, &st))
        return AFPERR_ItemNotFound; // the application is gone; the mapping is stale

    wr16be(out + 0, bitmap);
    wr32be(out + 2, appl->tag);
    int pbase = 6;
    int p = pbase;
    if (bitmap) {
        int pos_long_off = -1, pos_short_off = -1;
        p = afp_write_param_area(false, bitmap, out, pbase, out_max, &pos_long_off, &pos_short_off);
        if (p < 0 || !afp_populate_param_area(false, v, rel, &st, bitmap, out, pbase))
            return AFPERR_ParamErr;
        const char *name = afp_last_component(rel);
        size_t nlen = name ? strlen(name) : 0;
        p = afp_write_name_vars(out, p, out_max, pbase, name ? name : "", bitmap, pos_long_off, pos_short_off,
                                (uint8_t)(nlen > 255 ? 255 : nlen), (uint8_t)(nlen > 31 ? 31 : nlen));
        if (p < 0)
            return AFPERR_ParamErr;
    }
    if (p % 2 && p < out_max)
        out[p++] = 0x00;
    if (out_len)
        *out_len = p;
    return AFPERR_NoErr;
}

// Resolve the (DTRefNum, DirectoryID, Pathname) triple the comment calls use.
static uint32_t afp_resolve_dt_target(const uint8_t *in, int in_len, vol_t **out_vol, char *out_rel, size_t rel_cap,
                                      int *out_pos) {
    if (in_len < 8)
        return AFPERR_ParamErr;
    vol_t *v = find_vol_by_dt_ref(rd16be(in + 1));
    if (!v)
        return AFPERR_ParamErr;
    uint32_t dir_id = rd32be(in + 3);
    char path[AFP_MAX_NAME];
    int pos = afp_read_pstring(in, in_len, 8, path, sizeof(path));
    if (pos < 0)
        return AFPERR_ParamErr;
    if (out_pos)
        *out_pos = pos;
    vol_t *resolved = NULL;
    uint32_t rc = afp_resolve_target(v->vol_id, dir_id, path, &resolved, out_rel, rel_cap);
    if (rc != AFPERR_NoErr)
        return rc;
    if (out_vol)
        *out_vol = v;
    return AFPERR_NoErr;
}

// FPAddComment (0x38) — comments live in the file's sidecar, so they follow it
// through renames and copies for free.
static uint32_t afp_cmd_add_comment(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                    int *out_len) {
    (void)ctx;
    (void)out;
    (void)out_max;
    vol_t *v = NULL;
    char rel[AFP_MAX_REL_PATH];
    int pos = 0;
    uint32_t rc = afp_resolve_dt_target(in, in_len, &v, rel, sizeof(rel), &pos);
    if (rc != AFPERR_NoErr)
        return rc;
    char full[PATH_MAX];
    struct stat st;
    if (!afp_full_path(v, rel, full, sizeof(full)) || stat(full, &st) != 0)
        return AFPERR_ObjectNotFound;

    afp_meta_t meta;
    afp_meta_load(full, &meta);
    meta.comment_len = 0;
    meta.comment[0] = '\0';
    meta.has_comment = true;
    if (pos < in_len) {
        int len = in[pos++];
        if (len > AFP_META_COMMENT_MAX)
            len = AFP_META_COMMENT_MAX;
        if (pos + len > in_len)
            len = in_len - pos;
        if (len > 0)
            memcpy(meta.comment, in + pos, (size_t)len);
        meta.comment[len] = '\0';
        meta.comment_len = (uint8_t)len;
    }
    if (afp_meta_update(full, &meta) != 0)
        return AFPERR_AccessDenied;
    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPAddComment: '%s' len=%u", rel, meta.comment_len);
    return AFPERR_NoErr;
}

// FPRemoveComment (0x39)
static uint32_t afp_cmd_remove_comment(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                       int *out_len) {
    (void)ctx;
    (void)out;
    (void)out_max;
    vol_t *v = NULL;
    char rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_dt_target(in, in_len, &v, rel, sizeof(rel), NULL);
    if (rc != AFPERR_NoErr)
        return rc;
    char full[PATH_MAX];
    if (!afp_full_path(v, rel, full, sizeof(full)))
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
    if (out_len)
        *out_len = 0;
    return AFPERR_NoErr;
}

// FPGetComment (0x3A)
static uint32_t afp_cmd_get_comment(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                    int *out_len) {
    (void)ctx;
    vol_t *v = NULL;
    char rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_dt_target(in, in_len, &v, rel, sizeof(rel), NULL);
    if (rc != AFPERR_NoErr)
        return rc;
    char full[PATH_MAX];
    if (!afp_full_path(v, rel, full, sizeof(full)))
        return AFPERR_ParamErr;
    afp_meta_t meta;
    afp_meta_load(full, &meta);
    if (!meta.has_comment || meta.comment_len == 0)
        return AFPERR_ItemNotFound;
    int clen = meta.comment_len;
    if (1 + clen > out_max)
        clen = out_max - 1;
    if (clen < 0)
        return AFPERR_ParamErr;
    out[0] = (uint8_t)clen;
    memcpy(out + 1, meta.comment, (size_t)clen);
    if (out_len)
        *out_len = 1 + clen;
    return AFPERR_NoErr;
}

// ============================================================================
// AFP 2.1 file-ID calls (WP-5)
// ============================================================================

// FPCreateID (0x27) — attach a file-ID thread to a file.  The CNID is already
// the file's FileNumber; the thread is what makes it resolvable.
static uint32_t afp_cmd_create_id(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                  int *out_len) {
    (void)ctx;
    if (in_len < 8 || out_max < 4)
        return AFPERR_ParamErr;
    uint16_t vol_id = rd16be(in + 1);
    uint32_t dir_id = rd32be(in + 3);
    char path[AFP_MAX_NAME];
    if (afp_read_pstring(in, in_len, 8, path, sizeof(path)) < 0)
        return AFPERR_ParamErr;

    vol_t *vol = NULL;
    char rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_target(vol_id, dir_id, path, &vol, rel, sizeof(rel));
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
    wr32be(out, cnid);
    if (out_len)
        *out_len = 4;
    if (existed)
        return AFPERR_IDExists; // the ID is still returned, per the spec
    if (!afp_catalog_set_file_id(vol->catalog, cnid, true))
        return AFPERR_MiscErr;
    LOG(10, "AFP FPCreateID: '%s' → id=0x%08X", rel, cnid);
    return AFPERR_NoErr;
}

// FPDeleteID (0x28)
static uint32_t afp_cmd_delete_id(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                  int *out_len) {
    (void)ctx;
    (void)out;
    (void)out_max;
    if (in_len < 7)
        return AFPERR_ParamErr;
    vol_t *vol = find_vol_by_id(rd16be(in + 1));
    if (!vol)
        return AFPERR_ParamErr;
    uint32_t file_id = rd32be(in + 3);
    const afp_cat_entry_t *entry = afp_catalog_find(vol->catalog, file_id);
    if (!entry)
        return AFPERR_IDNotFound;
    if (entry->is_dir)
        return AFPERR_ObjectTypeErr;
    if (!entry->has_file_id)
        return AFPERR_IDNotFound;
    afp_catalog_set_file_id(vol->catalog, file_id, false);
    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPDeleteID: id=0x%08X", file_id);
    return AFPERR_NoErr;
}

// FPResolveID (0x29) — parameters for the file a file ID names.
static uint32_t afp_cmd_resolve_id(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                   int *out_len) {
    (void)ctx;
    if (in_len < 9 || out_max < 2)
        return AFPERR_ParamErr;
    vol_t *vol = find_vol_by_id(rd16be(in + 1));
    if (!vol)
        return AFPERR_ParamErr;
    uint32_t file_id = rd32be(in + 3);
    uint16_t bitmap = rd16be(in + 7);

    const afp_cat_entry_t *entry = afp_catalog_find(vol->catalog, file_id);
    if (!entry || entry->is_dir || !entry->has_file_id)
        return AFPERR_BadIDErr;
    char rel[AFP_MAX_REL_PATH];
    if (!afp_catalog_path(vol->catalog, file_id, rel, sizeof(rel)))
        return AFPERR_IDNotFound;
    struct stat st;
    if (!afp_stat_path(vol, rel, &st))
        return AFPERR_IDNotFound; // dangling thread — the file is gone

    wr16be(out + 0, bitmap);
    int pbase = 2;
    int p = pbase;
    if (bitmap) {
        int pos_long_off = -1, pos_short_off = -1;
        p = afp_write_param_area(false, bitmap, out, pbase, out_max, &pos_long_off, &pos_short_off);
        if (p < 0 || !afp_populate_param_area(false, vol, rel, &st, bitmap, out, pbase))
            return AFPERR_ParamErr;
        const char *name = afp_last_component(rel);
        size_t nlen = name ? strlen(name) : 0;
        p = afp_write_name_vars(out, p, out_max, pbase, name ? name : "", bitmap, pos_long_off, pos_short_off,
                                (uint8_t)(nlen > 255 ? 255 : nlen), (uint8_t)(nlen > 31 ? 31 : nlen));
        if (p < 0)
            return AFPERR_ParamErr;
    }
    if (p % 2 && p < out_max)
        out[p++] = 0x00;
    if (out_len)
        *out_len = p;
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
static uint32_t afp_cmd_exchange_files(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                       int *out_len) {
    (void)ctx;
    (void)out;
    (void)out_max;
    if (in_len < 12)
        return AFPERR_ParamErr;
    uint16_t vol_id = rd16be(in + 1);
    uint32_t src_dir = rd32be(in + 3);
    uint32_t dst_dir = rd32be(in + 7);
    char src_path[AFP_MAX_NAME];
    int pos = afp_read_pstring(in, in_len, 12, src_path, sizeof(src_path));
    if (pos < 0 || pos >= in_len)
        return AFPERR_ParamErr;
    pos++; // destination PathType
    char dst_path[AFP_MAX_NAME];
    if (afp_read_pstring(in, in_len, pos, dst_path, sizeof(dst_path)) < 0)
        return AFPERR_ParamErr;

    vol_t *vol = NULL;
    char src_rel[AFP_MAX_REL_PATH], dst_rel[AFP_MAX_REL_PATH];
    uint32_t rc = afp_resolve_target(vol_id, src_dir, src_path, &vol, src_rel, sizeof(src_rel));
    if (rc != AFPERR_NoErr)
        return rc;
    rc = afp_resolve_target(vol_id, dst_dir, dst_path, &vol, dst_rel, sizeof(dst_rel));
    if (rc != AFPERR_NoErr)
        return rc;
    if (strcmp(src_rel, dst_rel) == 0)
        return AFPERR_SameObjectErr;

    char src_full[PATH_MAX], dst_full[PATH_MAX];
    if (!afp_full_path(vol, src_rel, src_full, sizeof(src_full)) ||
        !afp_full_path(vol, dst_rel, dst_full, sizeof(dst_full)))
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
    if (out_len)
        *out_len = 0;
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
    char name[AFP_MAX_NAME + 1];
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
    int size = in[pos];
    int base = pos + 2; // skip the size byte and its filler
    if (size < 2 || pos + size > in_len)
        return false;
    if (out_next)
        *out_next = pos + size;
    int p = base;
    for (int bit = 0; bit < 16; bit++) {
        if (!(request_bm & (1u << bit)))
            continue;
        int width = afp_param_field_width(is_dir, bit);
        if (width == 0)
            continue;
        if (p + width > pos + size)
            return false;
        const uint8_t *f = in + p;
        p += width;
        switch (bit) {
        case 0:
            if (second)
                spec->attrs_mask = rd16be(f);
            else
                spec->attrs = rd16be(f);
            break;
        case 1:
            if (!second)
                spec->parent = rd32be(f);
            break;
        case 2:
            if (second)
                spec->create_hi = rd32be(f);
            else
                spec->create_lo = rd32be(f);
            break;
        case 3:
            if (second)
                spec->modify_hi = rd32be(f);
            else
                spec->modify_lo = rd32be(f);
            break;
        case 4:
            if (second)
                spec->backup_hi = rd32be(f);
            else
                spec->backup_lo = rd32be(f);
            break;
        case 5:
            memcpy(second ? spec->finder_mask : spec->finder, f, AFP_META_FINDER_SIZE);
            break;
        case 6: {
            // Specification2 must carry a nil name field, so only the first
            // record's offset is followed.
            if (second)
                break;
            uint16_t off = rd16be(f);
            int np = base + off;
            if (off == 0 || np >= pos + size)
                break;
            int nlen = in[np];
            if (np + 1 + nlen > pos + size)
                break;
            if (nlen > AFP_MAX_NAME)
                nlen = AFP_MAX_NAME;
            memcpy(spec->name, in + np + 1, (size_t)nlen);
            spec->name[nlen] = '\0';
            break;
        }
        case 9:
            if (second)
                spec->dlen_hi = (width == 2) ? rd16be(f) : rd32be(f);
            else
                spec->dlen_lo = (width == 2) ? rd16be(f) : rd32be(f);
            break;
        case 10:
            if (second)
                spec->rlen_hi = rd32be(f);
            else
                spec->rlen_lo = rd32be(f);
            break;
        default:
            break;
        }
    }
    return true;
}

// Case-insensitive substring test used for partial-name matching.
static bool name_contains(const char *haystack, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0)
        return true;
    for (const char *p = haystack; *p; p++)
        if (strncasecmp(p, needle, nl) == 0)
            return true;
    return false;
}

// Test one candidate against the decoded specifications.
static bool catsearch_matches(vol_t *vol, const char *rel, const char *name, bool is_dir, const struct stat *st,
                              uint32_t request_bm, const catsearch_spec_t *s1, const catsearch_spec_t *s2,
                              bool partial_name) {
    char full[PATH_MAX];
    if (!afp_full_path(vol, rel, full, sizeof(full)))
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
        if (partial_name ? !name_contains(name, s1->name) : strcasecmp(name, s1->name) != 0)
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

static uint32_t afp_cmd_cat_search(afp_ctx_t *ctx, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                   int *out_len) {
    (void)ctx;
    if (in_len < 35 || out_max < 24)
        return AFPERR_ParamErr;
    uint16_t vol_id = rd16be(in + 1);
    uint32_t req_matches = rd32be(in + 3);
    // in + 7: Reserved (must be zero)
    const uint8_t *catpos = in + 11; // 16 bytes
    uint16_t file_bm = rd16be(in + 27);
    uint16_t dir_bm = rd16be(in + 29);
    uint32_t request_bm = rd32be(in + 31);
    int pos = 35;

    vol_t *vol = find_vol_by_id(vol_id);
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
    uint16_t pos_valid = rd16be(catpos);
    uint32_t last_cnid = 0;
    if (pos_valid) {
        uint32_t gen = rd32be(catpos + 4);
        last_cnid = rd32be(catpos + 8);
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
    if (!catsearch_parse_spec(in, in_len, pos, criteria, search_dirs, &s1, false, &next))
        return AFPERR_ParamErr;
    if (!catsearch_parse_spec(in, in_len, next, criteria, search_dirs, &s2, true, NULL))
        return AFPERR_ParamErr;

    // Reply header: CatPosition(16) FileRsltBitmap(2) DirRsltBitmap(2)
    // ActualCount(4), then the result records.
    int w = 24;
    uint32_t actual = 0;
    uint32_t cursor = last_cnid;
    bool exhausted = true;
    if (req_matches == 0)
        req_matches = UINT32_MAX;

    for (const afp_cat_entry_t *e = afp_catalog_next(vol->catalog, cursor); e;
         e = afp_catalog_next(vol->catalog, cursor)) {
        cursor = e->cnid;
        if (actual >= req_matches) {
            exhausted = false;
            break;
        }
        if (e->cnid == AFP_CNID_ROOT)
            continue;
        char rel[AFP_MAX_REL_PATH];
        if (!afp_catalog_path(vol->catalog, e->cnid, rel, sizeof(rel)))
            continue;
        struct stat st;
        if (!afp_stat_path(vol, rel, &st))
            continue; // swept lazily by the next full search
        bool is_dir = S_ISDIR(st.st_mode);
        if ((is_dir && !dir_bm) || (!is_dir && !file_bm))
            continue;
        if (!catsearch_matches(vol, rel, e->name, is_dir, &st, criteria, &s1, &s2, partial_name))
            continue;

        // Result records use FPEnumerate's framing: length, File/Dir flag,
        // then the parameters the result bitmap selects.
        uint16_t bm = is_dir ? dir_bm : file_bm;
        int header = w;
        if (header + 2 > out_max) {
            exhausted = false;
            break;
        }
        out[header] = 0;
        out[header + 1] = is_dir ? 0x80 : 0x00;
        int pbase = header + 2;
        int pos_long_off = -1, pos_short_off = -1;
        int p = afp_write_param_area(is_dir, bm, out, pbase, out_max, &pos_long_off, &pos_short_off);
        if (p < 0) {
            exhausted = false;
            break;
        }
        if (!afp_populate_param_area(is_dir, vol, rel, &st, bm, out, pbase)) {
            exhausted = false;
            break;
        }
        size_t nlen = strlen(e->name);
        int vpos = afp_write_name_vars(out, p, out_max, pbase, e->name, bm, pos_long_off, pos_short_off,
                                       (uint8_t)(nlen > 255 ? 255 : nlen), (uint8_t)(nlen > 31 ? 31 : nlen));
        if (vpos < 0) {
            exhausted = false;
            break;
        }
        int struct_len = vpos - header;
        if (struct_len & 1) {
            if (vpos >= out_max) {
                exhausted = false;
                break;
            }
            out[vpos++] = 0x00;
            struct_len++;
        }
        if (struct_len > 255) {
            exhausted = false;
            break;
        }
        out[header] = (uint8_t)struct_len;
        w = vpos;
        actual++;
    }

    memset(out, 0, 16);
    wr16be(out + 0, 1); // a real catalog position, not a hint
    wr32be(out + 4, afp_catalog_generation(vol->catalog));
    wr32be(out + 8, cursor);
    wr16be(out + 16, file_bm);
    wr16be(out + 18, dir_bm);
    wr32be(out + 20, actual);
    if (out_len)
        *out_len = w;
    LOG(10, "AFP FPCatSearch: vol=0x%04X requestBm=0x%08X matches=%u cursor=0x%08X%s", vol_id, request_bm, actual,
        cursor, exhausted ? " (end)" : "");
    // afpEofError means "the whole tree has been walked", not "no matches".
    return exhausted ? AFPERR_EOFErr : AFPERR_NoErr;
}

// ============================================================================
// Dispatch
// ============================================================================

static const afp_command_handler_t k_afp_command_handlers[] = {
    {AFP_ByteRangeLock,   "FPByteRangeLock",   afp_cmd_byte_range_lock   },
    {AFP_CloseVol,        "FPCloseVol",        afp_cmd_close_vol         },
    {AFP_CloseDir,        "FPCloseDir",        afp_cmd_close_dir         },
    {AFP_CloseFork,       "FPCloseFork",       afp_cmd_close_fork        },
    {AFP_CopyFile,        "FPCopyFile",        afp_cmd_copy_file         },
    {AFP_CreateDir,       "FPCreateDir",       afp_cmd_create_dir        },
    {AFP_CreateFile,      "FPCreateFile",      afp_cmd_create_file       },
    {AFP_Delete,          "FPDelete",          afp_cmd_delete            },
    {AFP_Enumerate,       "FPEnumerate",       afp_cmd_enumerate         },
    {AFP_Flush,           "FPFlush",           afp_cmd_flush             },
    {AFP_FlushFork,       "FPFlushFork",       afp_cmd_flush_fork        },
    {AFP_GetForkParms,    "FPGetForkParms",    afp_cmd_get_fork_parms    },
    {AFP_GetSrvrInfo,     "FPGetSrvrInfo",     afp_cmd_get_srvr_info     },
    {AFP_GetSrvrParms,    "FPGetSrvrParms",    afp_cmd_get_srvr_parms    },
    {AFP_GetVolParms,     "FPGetVolParms",     afp_cmd_get_vol_parms     },
    {AFP_Login,           "FPLogin",           afp_cmd_login             },
    {AFP_LoginCont,       "FPLoginCont",       afp_cmd_login_cont        },
    {AFP_Logout,          "FPLogout",          afp_cmd_logout            },
    {AFP_MapID,           "FPMapID",           afp_cmd_map_id            },
    {AFP_MapName,         "FPMapName",         afp_cmd_map_name          },
    {AFP_MoveAndRename,   "FPMoveAndRename",   afp_cmd_move_and_rename   },
    {AFP_OpenVol,         "FPOpenVol",         afp_cmd_open_vol          },
    {AFP_OpenDir,         "FPOpenDir",         afp_cmd_open_dir          },
    {AFP_OpenFork,        "FPOpenFork",        afp_cmd_open_fork         },
    {AFP_Read,            "FPRead",            afp_cmd_read              },
    {AFP_Rename,          "FPRename",          afp_cmd_rename            },
    {AFP_SetDirParms,     "FPSetDirParms",     afp_cmd_set_dir_parms     },
    {AFP_SetFileParms,    "FPSetFileParms",    afp_cmd_set_file_parms    },
    {AFP_SetForkParms,    "FPSetForkParms",    afp_cmd_set_fork_parms    },
    {AFP_SetVolParms,     "FPSetVolParms",     afp_cmd_set_vol_parms     },
    {AFP_Write,           "FPWrite",           afp_cmd_write             },
    {AFP_GetFileDirParms, "FPGetFileDirParms", afp_cmd_get_file_dir_parms},
    {AFP_SetFileDirParms, "FPSetFileDirParms", afp_cmd_set_file_dir_parms},
    {AFP_ChangePassword,  "FPChangePassword",  afp_cmd_change_password   },
    {AFP_GetUserInfo,     "FPGetUserInfo",     afp_cmd_get_user_info     },
    {AFP_GetSrvrMsg,      "FPGetSrvrMsg",      afp_cmd_get_srvr_msg      },
    {AFP_CreateID,        "FPCreateID",        afp_cmd_create_id         },
    {AFP_DeleteID,        "FPDeleteID",        afp_cmd_delete_id         },
    {AFP_ResolveID,       "FPResolveID",       afp_cmd_resolve_id        },
    {AFP_ExchangeFiles,   "FPExchangeFiles",   afp_cmd_exchange_files    },
    {AFP_CatSearch,       "FPCatSearch",       afp_cmd_cat_search        },
    {AFP_OpenDT,          "FPOpenDT",          afp_cmd_open_dt           },
    {AFP_CloseDT,         "FPCloseDT",         afp_cmd_close_dt          },
    {AFP_GetIcon,         "FPGetIcon",         afp_cmd_get_icon          },
    {AFP_GetIconInfo,     "FPGetIconInfo",     afp_cmd_get_icon_info     },
    {AFP_AddAPPL,         "FPAddAPPL",         afp_cmd_add_appl          },
    {AFP_RmvAPPL,         "FPRemoveAPPL",      afp_cmd_remove_appl       },
    {AFP_GetAPPL,         "FPGetAPPL",         afp_cmd_get_appl          },
    {AFP_AddComment,      "FPAddComment",      afp_cmd_add_comment       },
    {AFP_RmvComment,      "FPRemoveComment",   afp_cmd_remove_comment    },
    {AFP_GetComment,      "FPGetComment",      afp_cmd_get_comment       },
    {AFP_AddIcon,         "FPAddIcon",         afp_cmd_add_icon          },
};

static const afp_command_handler_t *afp_find_handler(uint8_t opcode) {
    for (size_t i = 0; i < ARRAY_LEN(k_afp_command_handlers); i++)
        if (k_afp_command_handlers[i].opcode == opcode)
            return &k_afp_command_handlers[i];
    return NULL;
}

// The 2.1 calls are only legal once the session has negotiated 2.1; before
// that the client must use its 2.0 fallbacks (AFP_21_22 result codes).
static bool afp_opcode_is_21(uint8_t opcode) {
    return opcode >= AFP_GetSrvrMsg && opcode <= AFP_CatSearch;
}

uint32_t afp_handle_command(uint16_t session_id, uint8_t opcode, const uint8_t *in, int in_len, uint8_t *out,
                            int out_max, int *out_len) {
    if (out_len)
        *out_len = 0;
    if (!g_afp_enabled) {
        LOG(2, "AFP: command 0x%02X refused — the server is disabled", opcode);
        return AFPERR_ServerGoingDown;
    }
    const afp_command_handler_t *handler = afp_find_handler(opcode);
    if (!handler) {
        LOG(1, "AFP unknown opcode 0x%02X (len=%d)", opcode, in_len);
        afp_count_result(AFPERR_CallNotSupported);
        return AFPERR_CallNotSupported;
    }
    if (afp_opcode_is_21(opcode)) {
        const char *ver = atalk_asp_session_afp_version(session_id);
        if (!ver || strcmp(ver, "AFPVersion 2.1") != 0) {
            LOG(2, "AFP %s: refused — session negotiated '%s'", handler->name, ver ? ver : "(none)");
            afp_count_result(AFPERR_CallNotSupported);
            return AFPERR_CallNotSupported;
        }
    }
    afp_ctx_t ctx = {.session_id = session_id};
    LOG(10, "AFP >> %s (0x%02X) in_len=%d session=0x%04X", handler->name, opcode, in_len, session_id);
    uint32_t result = handler->handler(&ctx, in, in_len, out, out_max, out_len);
    afp_count_result(result);
    if (result == AFPERR_NoErr)
        LOG(3, "AFP << %s OK reply=%d", handler->name, out_len ? *out_len : 0);
    else
        LOG(3, "AFP << %s ERR=0x%08X reply=%d", handler->name, result, out_len ? *out_len : 0);
    return result;
}

// Release everything a departing session owned.  Called from the ASP layer on
// CloseSess and on tickle expiry (WP-8).
void afp_session_closed(uint16_t session_id) {
    afp_fork_close_session(session_id);
    enum_snapshots_drop_session(session_id);
    for (int i = 0; i < AFP_MAX_VOLUMES; i++)
        if (g_vols[i].in_use)
            vol_session_remove(&g_vols[i], session_id);
}

uint32_t afp_session_open_forks(uint16_t session_id) {
    return afp_fork_count_session(session_id);
}

// Drop every volume-scoped cache — used when a checkpoint restore replaces
// the machine underneath a live mount (WP-11).
void afp_reset_transient_state(void) {
    for (int i = 0; i < AFP_MAX_VOLUMES; i++) {
        if (!g_vols[i].in_use)
            continue;
        enum_snapshots_drop_volume(g_vols[i].vol_id);
        g_vols[i].n_open_by = 0;
        g_vols[i].dt_ref = 0;
        g_vols[i].mutations++;
    }
    afp_fork_shutdown();
}
