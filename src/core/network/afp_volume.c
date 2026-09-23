// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// afp_volume.c
// The server and its volumes: the volume table, publishing and withdrawing a
// share, the object-model accessors, server identity, enablement and message,
// the NBP advertisement, and the ASP GetStatus service status block.
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

// AFP versions we speak.  "AFPVersion 2.1" is only advertised because every
// 2.1 command below is implemented (WP-5); the honest-negotiation rule is
// that this list and the dispatch table move together.
static const char *const k_afp_versions[] = {"AFPVersion 2.0", "AFPVersion 2.1"};

// ============================================================================
// Volume table
// ============================================================================

// Note that `session` has this volume open.  Repeated FPOpenVol calls from one
// session are idempotent, as the client expects.
void vol_session_add(vol_t *v, uint16_t session) {
    for (uint32_t i = 0; i < v->n_open_by; i++)
        if (v->open_by[i] == session)
            return;
    if (v->n_open_by < AFP_MAX_SESSIONS)
        v->open_by[v->n_open_by++] = session;
}

// Forget that `session` had this volume open.
void vol_session_remove(vol_t *v, uint16_t session) {
    for (uint32_t i = 0; i < v->n_open_by; i++) {
        if (v->open_by[i] != session)
            continue;
        v->open_by[i] = v->open_by[--v->n_open_by];
        return;
    }
}

vol_t g_vols[AFP_MAX_VOLUMES];
static uint32_t g_next_vol_id = 1; // atalk_id_alloc cursor
uint16_t g_next_dt_ref = 0x0100;

// Server identity and enablement (object model: appletalk.afp.*).
static char g_afp_server_object[33] = AFP_ENTITY_OBJECT;
char g_afp_message[AFP_META_COMMENT_MAX + 1];
bool g_afp_enabled = true;
static atalk_nbp_entry_t *g_afp_nbp_entry;

// Server-wide counters (object model: appletalk.afp.stats).
atalk_afp_stats_t g_afp_stats;

// Per-error-code tally, indexed by (0 - code) so -5000..-5039 map to 0..39.
#define AFP_ERR_TALLY_BASE  5000
#define AFP_ERR_TALLY_COUNT 48
static uint64_t g_afp_err_tally[AFP_ERR_TALLY_COUNT];

// Record one command outcome for the stats subtree.
void afp_count_result(uint32_t result) {
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
    if (fread(rec, 1, sizeof(rec), f) == sizeof(rec) && RD_BE32(rec) == AFP_VOLREC_MAGIC)
        v->backup_date = RD_BE32(rec + 4);
    fclose(f);
}

void vol_record_store(const vol_t *v) {
    char path[PATH_MAX];
    if (!vol_record_path(v, path, sizeof(path)))
        return;
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    uint8_t rec[8];
    WR_BE32(rec, AFP_VOLREC_MAGIC);
    WR_BE32(rec + 4, v->backup_date);
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

vol_t *find_vol_by_id(uint16_t id) {
    for (int i = 0; i < AFP_MAX_VOLUMES; i++)
        if (g_vols[i].in_use && g_vols[i].vol_id == id)
            return &g_vols[i];
    return NULL;
}

vol_t *find_vol_by_name(const char *name) {
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

static bool vol_id_in_use(uint32_t id, const void *ctx) {
    (void)ctx;
    return find_vol_by_id((uint16_t)id) != NULL;
}

// Publish `path` as volume `name`, with `vol_id` or, when 0, the next free id.
static int vol_add(const char *name, const char *path, uint16_t vol_id, char *err, size_t err_len) {
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
    if (vol_id && find_vol_by_id(vol_id))
        return vol_fail(err, err_len, "volume id %u is already in use", (unsigned)vol_id);

    vol_t *v = &g_vols[slot];
    memset(v, 0, sizeof(*v));
    snprintf(v->name, sizeof(v->name), "%s", name);
    char resolved[PATH_MAX];
    snprintf(v->root, sizeof(v->root), "%s", realpath(path, resolved) ? resolved : path);
    if (vol_id) {
        v->vol_id = vol_id;
        if (vol_id >= g_next_vol_id)
            g_next_vol_id = (uint32_t)vol_id + 1;
    } else {
        uint32_t id = 0;
        if (!atalk_id_alloc(&g_next_vol_id, 1, 0xFFFF, vol_id_in_use, NULL, &id)) {
            memset(v, 0, sizeof(*v)); // cannot happen: at most AFP_MAX_VOLUMES of 65,535 are held
            return vol_fail(err, err_len, "no volume id is free");
        }
        v->vol_id = (uint16_t)id;
    }
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

int atalk_afp_volume_add(const char *name, const char *path, char *err, size_t err_len) {
    return vol_add(name, path, 0, err, err_len);
}

int atalk_afp_volume_restore(const char *name, const char *path, unsigned vol_id, char *err, size_t err_len) {
    if (vol_id == 0 || vol_id > 0xFFFF)
        return vol_fail(err, err_len, "volume id %u is out of range", vol_id);
    return vol_add(name, path, (uint16_t)vol_id, err, err_len);
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

// The ASP client: what a session's commands, status request and close mean.
static uint32_t afp_asp_command(void *ctx, uint16_t session_ref, uint8_t opcode, const uint8_t *in, int in_len,
                                uint8_t *out, int out_max, int *out_len) {
    (void)ctx;
    return afp_handle_command(session_ref, opcode, in, in_len, out, out_max, out_len);
}
static void afp_asp_close(void *ctx, uint16_t session_ref) {
    (void)ctx;
    afp_session_closed(session_ref);
}
static int afp_asp_status(void *ctx, uint8_t **out, size_t *out_len) {
    (void)ctx;
    return atalk_build_status_block(g_afp_server_object, "GrannySmith", out, out_len);
}
static uint32_t afp_asp_open_forks(void *ctx, uint16_t session_ref) {
    (void)ctx;
    return afp_session_open_forks(session_ref);
}
// A disabled server takes no new sessions: the workstation hears ServerBusy.
// (The comment in atalk_afp_set_enabled always said so; nothing did it until
// ASP gained a client.)
static bool afp_asp_open(void *ctx, uint16_t session_ref) {
    (void)ctx;
    return afp_session_opened(session_ref);
}
static const char *afp_asp_version(void *ctx, uint16_t session_ref) {
    (void)ctx;
    return afp_session_version(session_ref);
}
static const asp_client_t k_afp_asp_client = {
    .on_open = afp_asp_open,
    .on_close = afp_asp_close,
    .on_command = afp_asp_command,
    .get_status = afp_asp_status,
    .open_forks = afp_asp_open_forks,
    .session_version = afp_asp_version,
};

void atalk_server_init(void) {
    memset(&g_afp_stats, 0, sizeof(g_afp_stats));
    memset(g_afp_err_tally, 0, sizeof(g_afp_err_tally));
    // Nothing a previous machine's sessions held survives into this one.
    afp_reset_transient_state();
    asp_set_client(&k_afp_asp_client, NULL);
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
    asp_set_client(NULL, NULL);
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

    WR_BE16(&buf[0], (uint16_t)machine_type_off);
    WR_BE16(&buf[2], (uint16_t)afp_versions_cnt_off);
    WR_BE16(&buf[4], (uint16_t)uam_cnt_off);
    WR_BE16(&buf[6], 0); // Volume Icon and Mask offset (none)
    WR_BE16(&buf[8], afp_srvr_flags());

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
