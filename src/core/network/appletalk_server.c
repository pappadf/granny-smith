// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_server.c
// AFP file server over AppleTalk ASP/ATP protocols.

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
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define AFP_CNID_ROOT        0x00000002u
#define AFP_CNID_ROOT_PARENT 0x00000001u
#define AFP_MAX_REL_PATH     768
#define AFP_MAX_NAME         255
#define AFP_ENUM_MAX_ENTRIES 512
#define AFP_EPOCH_DELTA      2082844800u
#define AFP_LOG_HEX_MAX      64

#ifndef ARRAY_LEN
#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
#endif

// NBP entity strings for AFP server
#define AFP_ENTITY_OBJECT "Shared Folders"
#define AFP_ENTITY_TYPE   "AFPServer"

// Logging for this module uses the same category as appletalk.c
LOG_USE_CATEGORY_NAME("appletalk");

// Emit a short hex dump at LOG level 2 for AFP request/response payloads
static void afp_log_hex(const char *label, const uint8_t *buf, int len) {
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

// ------------------- ASP GetStatus - Status Block Builder -------------------

// Helper: write a Pascal string (length byte + bytes). Returns bytes written (1+len).
static size_t write_pstr(uint8_t *dst, const char *cstr) {
    size_t n = cstr ? strlen(cstr) : 0;
    if (n > 255)
        n = 255; // truncate to P-string max
    dst[0] = (uint8_t)n;
    if (n)
        memcpy(dst + 1, cstr, n);
    return 1 + n;
}

// Build Service Status Block per docs/errata.md
int atalk_build_status_block(const char *server_name, const char *machine_type, uint8_t **out_buf, size_t *out_len) {
    if (!out_buf || !out_len)
        return -1;
    *out_buf = NULL;
    *out_len = 0;

    // Hard-coded lists per requirement
    static const char *kAfpVersions[] = {"AFPVersion 2.0", "AFPVersion 2.1"};
    static const size_t kAfpVersionsCount = ARRAY_LEN(kAfpVersions);
    static const char *kUams[] = {"No User Authent"};
    static const size_t kUamsCount = ARRAY_LEN(kUams);

    // First, compute total size by simulating layout
    size_t pos = 10; // after the 2-byte offsets (0,2,4,6) + 2-byte Flags (8)
    size_t server_name_len = 1 + (server_name ? (strlen(server_name) > 255 ? 255 : strlen(server_name)) : 0);
    size_t machine_type_len = 1 + (machine_type ? (strlen(machine_type) > 255 ? 255 : strlen(machine_type)) : 0);
    pos += server_name_len; // Server Name P-string
    size_t machine_type_off = pos; // remember offset
    pos += machine_type_len; // Machine Type P-string
    size_t afp_versions_cnt_off = pos; // 1 byte count
    pos += 1;
    for (size_t i = 0; i < kAfpVersionsCount; i++) {
        size_t s = strlen(kAfpVersions[i]);
        if (s > 255)
            s = 255;
        pos += 1 + s;
    }
    size_t uam_cnt_off = pos; // 1 byte count
    pos += 1;
    for (size_t i = 0; i < kUamsCount; i++) {
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

    // Write offsets (big-endian) and flags
    // Offset to Machine Type
    buf[0] = (uint8_t)((machine_type_off >> 8) & 0xFF);
    buf[1] = (uint8_t)(machine_type_off & 0xFF);
    // Offset to count of AFP Versions
    buf[2] = (uint8_t)((afp_versions_cnt_off >> 8) & 0xFF);
    buf[3] = (uint8_t)(afp_versions_cnt_off & 0xFF);
    // Offset to count of UAMs
    buf[4] = (uint8_t)((uam_cnt_off >> 8) & 0xFF);
    buf[5] = (uint8_t)(uam_cnt_off & 0xFF);
    // Offset to Volume Icon and Mask (none)
    buf[6] = 0;
    buf[7] = 0;
    // Flags (bit0=SupportsCopyFile, bit1=SupportsChgPwd); default 0 for now
    buf[8] = 0;
    buf[9] = 0;

    // Write payload sections
    pos = 10;
    pos += write_pstr(&buf[pos], server_name ? server_name : "");
    pos += write_pstr(&buf[pos], machine_type ? machine_type : "");
    // AFP Versions
    buf[pos++] = (uint8_t)kAfpVersionsCount;
    for (size_t i = 0; i < kAfpVersionsCount; i++) {
        pos += write_pstr(&buf[pos], kAfpVersions[i]);
    }
    // UAMs
    buf[pos++] = (uint8_t)kUamsCount;
    for (size_t i = 0; i < kUamsCount; i++) {
        pos += write_pstr(&buf[pos], kUams[i]);
    }

    *out_buf = buf;
    *out_len = total;
    return 0;
}

// ------------------- Simple service registry (shares) -------------------

typedef struct {
    char name[33];
    char path[PATH_MAX];
    uint16_t vol_id; // simple incremental ID for AFP volume listings
    bool in_use;
} share_t;

static void afp_release_volume(uint16_t vol_id);

// Exported to appletalk.c (NBP and AFP helpers)
int MAX_SHARES = 8;
share_t g_shares[8];
static uint16_t g_next_vol_id = 1;

static int find_share_index_by_name(const char *name) {
    for (int i = 0; i < MAX_SHARES; i++) {
        if (g_shares[i].in_use && strncmp(g_shares[i].name, name, sizeof(g_shares[i].name)) == 0) {
            return i;
        }
    }
    return -1;
}

static int first_free_share_slot(void) {
    for (int i = 0; i < MAX_SHARES; i++)
        if (!g_shares[i].in_use)
            return i;
    return -1;
}

int atalk_share_add(const char *name, const char *path) {
    if (!name || !*name || !path || !*path) {
        LOG(1, "atalk: share-add requires <name> <path>");
        return -1;
    }
    if (strlen(name) > 32) {
        LOG(1, "atalk: share name max 32 chars");
        return -1;
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        LOG(1, "atalk: path '%s' not a directory", path);
        return -1;
    }
    if (find_share_index_by_name(name) >= 0) {
        LOG(1, "atalk: share '%s' already exists", name);
        return -1;
    }
    int idx = first_free_share_slot();
    if (idx < 0) {
        LOG(1, "atalk: share table full (max %d)", MAX_SHARES);
        return -1;
    }
    memset(&g_shares[idx], 0, sizeof(g_shares[idx]));
    strncpy(g_shares[idx].name, name, sizeof(g_shares[idx].name) - 1);
    char resolved[PATH_MAX];
    const char *final_path = path;
    if (realpath(path, resolved))
        final_path = resolved;
    strncpy(g_shares[idx].path, final_path, sizeof(g_shares[idx].path) - 1);
    g_shares[idx].vol_id = g_next_vol_id++;
    g_shares[idx].in_use = true;
    LOG(1, "atalk: added share '%s' -> '%s' (vol %u)", g_shares[idx].name, g_shares[idx].path,
        (unsigned)g_shares[idx].vol_id);
    return 0;
}

int atalk_share_remove(const char *name) {
    int idx = find_share_index_by_name(name);
    if (idx < 0) {
        LOG(1, "atalk: no such share '%s'", name);
        return -1;
    }
    afp_release_volume(g_shares[idx].vol_id);
    g_shares[idx].in_use = false;
    g_shares[idx].name[0] = '\0';
    g_shares[idx].path[0] = '\0';
    g_shares[idx].vol_id = 0;
    LOG(1, "atalk: removed share '%s'", name);
    return 0;
}

int atalk_share_list(void) {
    LOG(1, "AppleTalk shares (%d max):", MAX_SHARES);
    for (int i = 0; i < MAX_SHARES; i++) {
        if (!g_shares[i].in_use)
            continue;
        LOG(1, "  - %s  (vol %u)  path=%s", g_shares[i].name, (unsigned)g_shares[i].vol_id, g_shares[i].path);
    }
    return 0;
}

// Exported for appletalk.c (NBP advertisement name)
static char g_afp_server_object[33] = AFP_ENTITY_OBJECT;
static atalk_nbp_entry_t *g_afp_nbp_entry;

const char *atalk_server_object_name(void) {
    return g_afp_server_object;
}

void atalk_server_init(void) {
    atalk_nbp_service_desc_t desc = {.object = g_afp_server_object,
                                     .type = AFP_ENTITY_TYPE,
                                     .zone = "*",
                                     .socket = HOST_AFP_SOCKET,
                                     .node = LLAP_HOST_NODE,
                                     .net = 0};
    if (atalk_nbp_register(&desc, &g_afp_nbp_entry) != 0)
        LOG(1, "AFP: failed to register NBP advertisement");
}

// =============================== AFP (Apple Filing Protocol) ===============================

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

// --- Simple AFP volume and fork tables (debug/minimal wire) ---
typedef struct {
    uint32_t cnid;
    bool is_dir;
    char rel_path[AFP_MAX_REL_PATH];
} catalog_entry_t;

typedef struct {
    bool in_use;
    uint16_t vol_id;
    char name[33];
    char root[PATH_MAX];
    catalog_entry_t *catalog;
    size_t catalog_len;
    size_t catalog_cap;
    uint32_t next_cnid;
} vol_t;

static int afp_param_field_ptr(bool is_dir, uint16_t bm, int pbase, int target_bit);

// ------------------- AFP path / CNID helpers -------------------

static uint32_t afp_unix_time_to_afp(time_t t) {
    if (t < (time_t)-AFP_EPOCH_DELTA)
        return 0;
    uint64_t val = (uint64_t)t + AFP_EPOCH_DELTA;
    if (val > UINT32_MAX)
        return UINT32_MAX;
    return (uint32_t)val;
}

static uint32_t afp_hash_path(const char *rel_path) {
    uint32_t h = 2166136261u;
    if (!rel_path)
        return h;
    while (*rel_path) {
        unsigned char c = (unsigned char)tolower((unsigned char)*rel_path++);
        h ^= c;
        h *= 16777619u;
    }
    if (h == 0)
        h = 1;
    return h;
}

static void afp_free_catalog(vol_t *v) {
    if (!v)
        return;
    free(v->catalog);
    v->catalog = NULL;
    v->catalog_len = 0;
    v->catalog_cap = 0;
}

static void afp_reset_volume(vol_t *v) {
    if (!v)
        return;
    afp_free_catalog(v);
    memset(v, 0, sizeof(*v));
    v->next_cnid = AFP_CNID_ROOT + 1;
}

static catalog_entry_t *afp_catalog_find_by_cnid(vol_t *v, uint32_t cnid) {
    if (!v)
        return NULL;
    for (size_t i = 0; i < v->catalog_len; i++) {
        if (v->catalog[i].cnid == cnid)
            return &v->catalog[i];
    }
    return NULL;
}

static catalog_entry_t *afp_catalog_find_by_path(vol_t *v, const char *rel_path) {
    if (!v || !rel_path)
        return NULL;
    for (size_t i = 0; i < v->catalog_len; i++) {
        if (strcmp(v->catalog[i].rel_path, rel_path) == 0)
            return &v->catalog[i];
    }
    return NULL;
}

// Insert a catalog entry for the given relative path
static catalog_entry_t *afp_catalog_insert(vol_t *v, const char *rel_path, bool is_dir) {
    if (!v || !rel_path)
        return NULL;
    if (v->catalog_len == v->catalog_cap) {
        size_t new_cap = v->catalog_cap ? v->catalog_cap * 2 : 16;
        catalog_entry_t *tmp = (catalog_entry_t *)realloc(v->catalog, new_cap * sizeof(catalog_entry_t));
        if (!tmp)
            return NULL;
        v->catalog = tmp;
        v->catalog_cap = new_cap;
    }
    catalog_entry_t *entry = &v->catalog[v->catalog_len++];
    memset(entry, 0, sizeof(*entry));
    entry->is_dir = is_dir;
    entry->cnid = (rel_path[0] == '\0') ? AFP_CNID_ROOT : v->next_cnid++;
    strncpy(entry->rel_path, rel_path, sizeof(entry->rel_path) - 1);
    LOG(10, "AFP catalog insert: cnid=0x%08X %s path='%s' (count=%zu)", entry->cnid, is_dir ? "dir" : "file", rel_path,
        v->catalog_len);
    return entry;
}

// Ensure a catalog entry exists for the given path (defaults to directory)
static catalog_entry_t *afp_catalog_ensure(vol_t *v, const char *rel_path) {
    if (!v)
        return NULL;
    if (!rel_path || !*rel_path)
        rel_path = "";
    catalog_entry_t *entry = afp_catalog_find_by_path(v, rel_path);
    if (entry)
        return entry;
    return afp_catalog_insert(v, rel_path, true);
}

static catalog_entry_t *afp_catalog_resolve(vol_t *v, uint32_t dir_id) {
    if (!v)
        return NULL;
    if (dir_id == 0 || dir_id == AFP_CNID_ROOT)
        return afp_catalog_ensure(v, "");
    catalog_entry_t *e = afp_catalog_find_by_cnid(v, dir_id);
    LOG(10, "AFP catalog resolve: dir_id=0x%08X -> %s", dir_id, e ? e->rel_path : "(not found)");
    return e;
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
    if (!parent || parent_len == 0) {
        return;
    }
    parent[0] = '\0';
    if (!rel_path || !*rel_path)
        return;
    strncpy(parent, rel_path, parent_len - 1);
    parent[parent_len - 1] = '\0';
    afp_path_pop(parent);
}

static const char *afp_last_component(const char *rel_path) {
    if (!rel_path || !*rel_path)
        return NULL;
    const char *slash = strrchr(rel_path, '/');
    return slash ? (slash + 1) : rel_path;
}

static uint32_t afp_parent_cnid(vol_t *vol, const char *rel_path) {
    if (!vol)
        return AFP_CNID_ROOT_PARENT;
    if (!rel_path || rel_path[0] == '\0')
        return AFP_CNID_ROOT_PARENT;
    char parent[AFP_MAX_REL_PATH];
    parent[0] = '\0';
    afp_extract_parent(rel_path, parent, sizeof(parent));
    if (parent[0] == '\0')
        return AFP_CNID_ROOT;
    catalog_entry_t *entry = afp_catalog_ensure(vol, parent);
    return entry ? entry->cnid : AFP_CNID_ROOT;
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
    if (!rel || !*rel) {
        return snprintf(out, out_len, "%s", vol->root) > 0;
    }
    const char *sep = (vol->root[0] && vol->root[strlen(vol->root) - 1] == '/') ? "" : "/";
    return snprintf(out, out_len, "%s%s%s", vol->root, sep, rel) > 0;
}

static bool afp_stat_path(vol_t *vol, const char *rel, struct stat *st) {
    char full[PATH_MAX];
    if (!afp_full_path(vol, rel, full, sizeof(full)))
        return false;
    return stat(full, st) == 0;
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
        if (++count >= UINT16_MAX)
            break;
    }
    closedir(dir);
    return (uint16_t)count;
}

// Return AFP file attribute bits based on host permissions
static uint16_t afp_file_attributes_from_stat(const struct stat *st) {
    if (!st)
        return 0;
    // Set WriteInhibit (bit 5) only if host file is not writable by owner
    if (!(st->st_mode & S_IWUSR))
        return (uint16_t)(1u << 5);
    return 0;
}

static uint16_t afp_dir_attributes_from_stat(const struct stat *st) {
    (void)st;
    return 0;
}

static uint32_t afp_compute_access_rights(const struct stat *st) {
    if (!st)
        return 0x80000000u;
    uint32_t owner = (uint32_t)((st->st_mode >> 6) & 0x07);
    uint32_t group = (uint32_t)((st->st_mode >> 3) & 0x07);
    uint32_t world = (uint32_t)(st->st_mode & 0x07);
    return 0x80000000u | (owner << 16) | (group << 8) | world;
}

// Per-file/dir Finder Info storage (32 bytes each)
typedef struct {
    bool in_use;
    uint16_t vol_id;
    char rel_path[AFP_MAX_REL_PATH];
    uint8_t finder_info[32];
} finder_info_entry_t;
static finder_info_entry_t g_finder_info[256];

// Look up Finder Info for a file or directory
static finder_info_entry_t *afp_find_finder_info(uint16_t vol_id, const char *rel_path) {
    for (int i = 0; i < ARRAY_LEN(g_finder_info); i++) {
        if (g_finder_info[i].in_use && g_finder_info[i].vol_id == vol_id &&
            strcmp(g_finder_info[i].rel_path, rel_path) == 0)
            return &g_finder_info[i];
    }
    return NULL;
}

static bool afp_populate_param_area(bool is_dir, vol_t *vol, const char *rel_path, const struct stat *st, uint16_t bm,
                                    uint8_t *out, int pbase) {
    if (!st)
        return false;
    int ptr;
    // Finder Info (bit 5): look up stored info or write zeros
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 5)) >= 0) {
        finder_info_entry_t *fi = vol ? afp_find_finder_info(vol->vol_id, rel_path ? rel_path : "") : NULL;
        if (fi)
            memcpy(out + ptr, fi->finder_info, 32);
        else
            memset(out + ptr, 0, 32);
    }
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 0)) >= 0) {
        uint16_t attr = is_dir ? afp_dir_attributes_from_stat(st) : afp_file_attributes_from_stat(st);
        wr16be(out + ptr, attr);
    }
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 1)) >= 0) {
        uint32_t parent = afp_parent_cnid(vol, rel_path);
        wr32be(out + ptr, parent);
    }
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 2)) >= 0) {
        wr32be(out + ptr, afp_unix_time_to_afp(st->st_ctime));
    }
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 3)) >= 0) {
        wr32be(out + ptr, afp_unix_time_to_afp(st->st_mtime));
    }
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 4)) >= 0) {
        wr32be(out + ptr, 0);
    }
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 8)) >= 0) {
        if (is_dir) {
            catalog_entry_t *entry = afp_catalog_ensure(vol, rel_path);
            uint32_t cnid = entry ? entry->cnid : AFP_CNID_ROOT;
            wr32be(out + ptr, cnid);
        } else {
            wr32be(out + ptr, afp_hash_path(rel_path));
        }
    }
    if (!is_dir) {
        if ((ptr = afp_param_field_ptr(false, bm, pbase, 9)) >= 0) {
            wr32be(out + ptr, (uint32_t)st->st_size);
        }
        if ((ptr = afp_param_field_ptr(false, bm, pbase, 10)) >= 0) {
            // Resource fork length: check companion .rsrc file
            uint32_t rsrc_len = 0;
            if (vol && rel_path) {
                char full[PATH_MAX], rsrc_path[PATH_MAX];
                if (afp_full_path(vol, rel_path, full, sizeof(full))) {
                    snprintf(rsrc_path, sizeof(rsrc_path), "%s.rsrc", full);
                    struct stat rsrc_st;
                    if (stat(rsrc_path, &rsrc_st) == 0)
                        rsrc_len = (uint32_t)rsrc_st.st_size;
                }
            }
            wr32be(out + ptr, rsrc_len);
        }
    } else {
        if ((ptr = afp_param_field_ptr(true, bm, pbase, 9)) >= 0) {
            char full[PATH_MAX];
            if (afp_full_path(vol, rel_path, full, sizeof(full))) {
                wr16be(out + ptr, afp_count_offspring(full));
            }
        }
        if ((ptr = afp_param_field_ptr(true, bm, pbase, 10)) >= 0) {
            wr32be(out + ptr, 0);
        }
        if ((ptr = afp_param_field_ptr(true, bm, pbase, 11)) >= 0) {
            wr32be(out + ptr, 0);
        }
        if ((ptr = afp_param_field_ptr(true, bm, pbase, 12)) >= 0) {
            wr32be(out + ptr, 0x07070707u);
        }
    }
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 13)) >= 0) {
        memset(out + ptr, 0, 6);
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
// Open fork descriptor
typedef struct {
    bool in_use;
    uint16_t fork_ref;
    uint16_t vol_id;
    uint16_t access_mode; // AFP access/deny mode bits
    bool is_resource; // true = resource fork, false = data fork
    char path[512]; // full host path
    char rel_path[AFP_MAX_REL_PATH]; // volume-relative path
    FILE *f;
} fork_t;
static vol_t g_vols[8]; // mirror shares at time of open (max shares)
static fork_t g_forks[16];
static uint16_t g_next_fork_ref = 0x0042;

// Close an open fork and release its slot
static void close_fork(fork_t *fk) {
    if (!fk)
        return;
    LOG(7, "AFP fork close: ref=0x%04X path='%s'", fk->fork_ref, fk->rel_path);
    if (fk->f)
        fclose(fk->f);
    memset(fk, 0, sizeof(*fk));
    fk->in_use = false;
}

static void afp_release_volume(uint16_t vol_id) {
    // Close any open forks on this volume
    for (int i = 0; i < 16; i++) {
        if (g_forks[i].in_use && g_forks[i].vol_id == vol_id)
            close_fork(&g_forks[i]);
    }
    for (int i = 0; i < MAX_SHARES; i++) {
        if (g_vols[i].in_use && g_vols[i].vol_id == vol_id) {
            afp_reset_volume(&g_vols[i]);
        }
    }
}

static vol_t *find_vol_by_id(uint16_t id) {
    for (int i = 0; i < MAX_SHARES; i++)
        if (g_vols[i].in_use && g_vols[i].vol_id == id)
            return &g_vols[i];
    return NULL;
}

static vol_t *ensure_vol_by_name(const char *name) {
    // Check existing mapping
    for (int i = 0; i < MAX_SHARES; i++)
        if (g_vols[i].in_use && strcmp(g_vols[i].name, name) == 0)
            return &g_vols[i];
    // Create from share table
    for (int i = 0; i < MAX_SHARES; i++)
        if (g_shares[i].in_use && strcmp(g_shares[i].name, name) == 0) {
            for (int j = 0; j < MAX_SHARES; j++)
                if (!g_vols[j].in_use) {
                    afp_reset_volume(&g_vols[j]);
                    g_vols[j].in_use = true;
                    g_vols[j].vol_id = g_shares[i].vol_id;
                    strncpy(g_vols[j].name, g_shares[i].name, sizeof(g_vols[j].name) - 1);
                    g_vols[j].name[sizeof(g_vols[j].name) - 1] = '\0';
                    strncpy(g_vols[j].root, g_shares[i].path, sizeof(g_vols[j].root) - 1);
                    g_vols[j].root[sizeof(g_vols[j].root) - 1] = '\0';
                    afp_catalog_ensure(&g_vols[j], "");
                    return &g_vols[j];
                }
        }
    return NULL;
}

static vol_t *resolve_vol_by_id(uint16_t vol_id) {
    vol_t *v = find_vol_by_id(vol_id);
    if (v)
        return v;
    for (int i = 0; i < MAX_SHARES; i++) {
        if (!g_shares[i].in_use)
            continue;
        if (g_shares[i].vol_id != vol_id)
            continue;
        return ensure_vol_by_name(g_shares[i].name);
    }
    LOG(7, "AFP resolve_vol: vol_id=0x%04X not found", vol_id);
    return NULL;
}

static int afp_write_vol_param_block(const vol_t *v, uint16_t *bitmap_ptr, uint8_t *out, int out_max) {
    if (!v || out_max < 2)
        return 0;
    uint16_t bitmap = bitmap_ptr ? *bitmap_ptr : 0;
    wr16be(out, bitmap);
    int param_start = 2;
    int fixed_len = 0;
    if (bitmap & 0x0001)
        fixed_len += 2;
    if (bitmap & 0x0002)
        fixed_len += 2;
    if (bitmap & 0x0004)
        fixed_len += 4;
    if (bitmap & 0x0008)
        fixed_len += 4;
    if (bitmap & 0x0010)
        fixed_len += 4;
    if (bitmap & 0x0020)
        fixed_len += 2;
    if (bitmap & 0x0040)
        fixed_len += 4;
    if (bitmap & 0x0080)
        fixed_len += 4;
    if (bitmap & 0x0100)
        fixed_len += 2;
    int var_len = 0;
    size_t name_len = strlen(v->name);
    if (name_len > 255)
        name_len = 255;
    if (bitmap & 0x0100)
        var_len += 1 + (int)name_len;
    int total_len = 2 + fixed_len + var_len;
    if (total_len > out_max) {
        if (bitmap & 0x0100) {
            bitmap &= (uint16_t)~0x0100;
            wr16be(out, bitmap);
            fixed_len -= 2;
            var_len = 0;
            total_len = 2 + fixed_len;
            if (total_len > out_max)
                return 0;
        } else {
            return 0;
        }
    }
    int p = param_start;
    int var_base = param_start + fixed_len;
    uint16_t attr = 0x0000;
    uint16_t sig = 0x0002;
    time_t now = time(NULL);
    uint32_t cdate = (uint32_t)(now & 0xFFFFFFFFu);
    uint32_t mdate = (uint32_t)(now & 0xFFFFFFFFu);
    uint32_t bdate = 0;
    uint16_t vol_id = v->vol_id;
    uint32_t bytes_free = 12u * 1024u * 1024u;
    uint32_t bytes_total = 16u * 1024u * 1024u;
    if (bitmap & 0x0001) {
        wr16be(&out[p], attr);
        p += 2;
    }
    if (bitmap & 0x0002) {
        wr16be(&out[p], sig);
        p += 2;
    }
    if (bitmap & 0x0004) {
        out[p + 0] = (cdate >> 24) & 0xFF;
        out[p + 1] = (cdate >> 16) & 0xFF;
        out[p + 2] = (cdate >> 8) & 0xFF;
        out[p + 3] = cdate & 0xFF;
        p += 4;
    }
    if (bitmap & 0x0008) {
        out[p + 0] = (mdate >> 24) & 0xFF;
        out[p + 1] = (mdate >> 16) & 0xFF;
        out[p + 2] = (mdate >> 8) & 0xFF;
        out[p + 3] = mdate & 0xFF;
        p += 4;
    }
    if (bitmap & 0x0010) {
        out[p + 0] = (bdate >> 24) & 0xFF;
        out[p + 1] = (bdate >> 16) & 0xFF;
        out[p + 2] = (bdate >> 8) & 0xFF;
        out[p + 3] = bdate & 0xFF;
        p += 4;
    }
    if (bitmap & 0x0020) {
        wr16be(&out[p], vol_id);
        p += 2;
    }
    if (bitmap & 0x0040) {
        out[p + 0] = (bytes_free >> 24) & 0xFF;
        out[p + 1] = (bytes_free >> 16) & 0xFF;
        out[p + 2] = (bytes_free >> 8) & 0xFF;
        out[p + 3] = bytes_free & 0xFF;
        p += 4;
    }
    if (bitmap & 0x0080) {
        out[p + 0] = (bytes_total >> 24) & 0xFF;
        out[p + 1] = (bytes_total >> 16) & 0xFF;
        out[p + 2] = (bytes_total >> 8) & 0xFF;
        out[p + 3] = bytes_total & 0xFF;
        p += 4;
    }
    if (bitmap & 0x0100) {
        uint16_t off = (uint16_t)(var_base - param_start);
        wr16be(&out[p], off);
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

// Allocate a fork slot; opens file with appropriate mode
static fork_t *alloc_fork(uint16_t vol_id, const char *path, const char *rel_path, uint16_t access_mode,
                          bool is_resource) {
    for (int i = 0; i < 16; i++)
        if (!g_forks[i].in_use) {
            memset(&g_forks[i], 0, sizeof(g_forks[i]));
            g_forks[i].in_use = true;
            g_forks[i].fork_ref = g_next_fork_ref++;
            g_forks[i].vol_id = vol_id;
            g_forks[i].access_mode = access_mode;
            g_forks[i].is_resource = is_resource;
            strncpy(g_forks[i].path, path, sizeof(g_forks[i].path) - 1);
            if (rel_path)
                strncpy(g_forks[i].rel_path, rel_path, sizeof(g_forks[i].rel_path) - 1);
            if (is_resource) {
                // Resource forks: stored as companion .rsrc file
                char rsrc_path[PATH_MAX];
                snprintf(rsrc_path, sizeof(rsrc_path), "%s.rsrc", path);
                bool want_write = (access_mode & 0x0002);
                if (want_write) {
                    // Try opening existing file for read/write, create if absent
                    g_forks[i].f = fopen(rsrc_path, "r+b");
                    if (!g_forks[i].f)
                        g_forks[i].f = fopen(rsrc_path, "w+b");
                } else {
                    // Read-only; if file doesn't exist, resource fork is empty
                    g_forks[i].f = fopen(rsrc_path, "rb");
                    if (!g_forks[i].f)
                        g_forks[i].f = fopen("/dev/null", "rb");
                }
            } else {
                // Data fork: "r+b" if write requested (bit 1), else "rb"
                bool want_write = (access_mode & 0x0002);
                g_forks[i].f = fopen(path, want_write ? "r+b" : "rb");
            }
            if (!g_forks[i].f) {
                LOG(7, "AFP fork alloc failed: fopen('%s') errno=%d", path, errno);
                g_forks[i].in_use = false;
                return NULL;
            }
            LOG(7, "AFP fork alloc: ref=0x%04X slot=%d vol=%u %s %s mode=0x%04X path='%s'", g_forks[i].fork_ref, i,
                vol_id, is_resource ? "rsrc" : "data", (access_mode & 0x0002) ? "rw" : "ro", access_mode,
                rel_path ? rel_path : "");
            return &g_forks[i];
        }
    LOG(1, "AFP fork alloc failed: no free slots");
    return NULL;
}

static fork_t *find_fork(uint16_t ref) {
    for (int i = 0; i < 16; i++)
        if (g_forks[i].in_use && g_forks[i].fork_ref == ref)
            return &g_forks[i];
    return NULL;
}

// --- Minimal Desktop Database (DT) reference table ---
typedef struct {
    bool in_use;
    uint16_t vol_id;
    uint16_t dt_ref;
} dt_entry_t;
static dt_entry_t g_dt[8];
static uint16_t g_next_dt_ref = 0x0100;

// Store Finder Info for a file or directory
static finder_info_entry_t *afp_set_finder_info(uint16_t vol_id, const char *rel_path, const uint8_t *info) {
    finder_info_entry_t *fi = afp_find_finder_info(vol_id, rel_path);
    if (!fi) {
        for (int i = 0; i < ARRAY_LEN(g_finder_info); i++) {
            if (!g_finder_info[i].in_use) {
                fi = &g_finder_info[i];
                break;
            }
        }
    }
    if (!fi)
        return NULL;
    fi->in_use = true;
    fi->vol_id = vol_id;
    strncpy(fi->rel_path, rel_path ? rel_path : "", sizeof(fi->rel_path) - 1);
    if (info)
        memcpy(fi->finder_info, info, 32);
    return fi;
}

// Desktop DB icon storage
typedef struct {
    bool in_use;
    uint16_t vol_id;
    uint32_t creator; // 4-byte creator code
    uint32_t file_type; // 4-byte type code
    uint8_t icon_type; // icon type tag
    uint16_t icon_size; // bitmap size in bytes
    uint8_t icon_data[1024]; // icon bitmap
} dt_icon_t;
static dt_icon_t g_dt_icons[64];

// Desktop DB APPL mapping storage
typedef struct {
    bool in_use;
    uint16_t vol_id;
    uint32_t creator; // 4-byte creator code
    uint32_t appl_tag; // application tag
    char path[AFP_MAX_REL_PATH]; // path to the application
} dt_appl_t;
static dt_appl_t g_dt_appls[32];

// Desktop DB comment storage
typedef struct {
    bool in_use;
    uint16_t vol_id;
    uint32_t dir_id;
    char path[AFP_MAX_REL_PATH];
    uint8_t comment_len;
    char comment[200];
} dt_comment_t;
static dt_comment_t g_dt_comments[64];

static dt_entry_t *ensure_dt_for_vol(uint16_t vol_id) {
    for (int i = 0; i < MAX_SHARES; i++) {
        if (g_dt[i].in_use && g_dt[i].vol_id == vol_id)
            return &g_dt[i];
    }
    for (int i = 0; i < MAX_SHARES; i++) {
        if (!g_dt[i].in_use) {
            g_dt[i].in_use = true;
            g_dt[i].vol_id = vol_id;
            g_dt[i].dt_ref = g_next_dt_ref++;
            return &g_dt[i];
        }
    }
    return NULL;
}

// AFP opcodes (per specification list)
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
#define AFP_OpenDT          0x30
#define AFP_CloseDT         0x31
#define AFP_GetIcon         0x33
#define AFP_GetIconInfo     0x34
#define AFP_AddAPPL         0x35
#define AFP_RmvAPPL         0x36
#define AFP_GetAPPL         0x37
#define AFP_AddComment      0x38
#define AFP_RmvComment      0x39
#define AFP_GetComment      0x3A
#define AFP_AddIcon         0xC0

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

// Forward decls for helpers used within AFP
static int afp_open_vol(const uint8_t *in, int in_len, uint8_t *out, int out_max);
static int afp_enumerate(const uint8_t *in, int in_len, uint8_t *out, int out_max, uint32_t *result_code);
static int afp_get_srvr_info(uint8_t *out, int max_len);
static int afp_get_srvr_parms(uint8_t *out, int max_len);
static uint32_t afp_cmd_open_dir(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len);
static uint32_t afp_cmd_close_dir(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len);
static int afp_fixed_param_len(bool is_dir, uint16_t bm);
static int afp_write_param_area(bool is_dir, uint16_t bm, uint8_t *out, int p, int out_max, int *pos_long_off,
                                int *pos_short_off);
static int afp_write_name_vars(uint8_t *out, int vpos, int out_max, int pbase, const char *nm, uint16_t bm,
                               int pos_long_off, int pos_short_off, uint8_t long_len, uint8_t short_len);
static int afp_param_field_ptr(bool is_dir, uint16_t bm, int pbase, int target_bit);

// FPOpenVol (AFP 0x18)
static int afp_open_vol(const uint8_t *in, int in_len, uint8_t *out, int out_max) {
    if (in_len < 4)
        return 0;
    afp_log_hex("AFP FPOpenVol req", in, in_len);
    uint16_t bitmap = rd16be(in + 1);
    int pos = 3;
    if (pos >= in_len)
        return 0;
    uint8_t nameLen = in[pos++];
    if (pos + nameLen > in_len)
        nameLen = (uint8_t)((in_len > pos) ? (in_len - pos) : 0);
    char volName[33];
    size_t n = nameLen;
    if (n > 31)
        n = 31;
    memcpy(volName, &in[pos], n);
    volName[n] = '\0';
    pos += nameLen;
    LOG(10, "AFP FPOpenVol: bitmap=0x%04X name='%s'", bitmap, volName);

    vol_t *v = ensure_vol_by_name(volName);
    if (!v)
        return 0;
    afp_catalog_ensure(v, "");

    int written = afp_write_vol_param_block(v, &bitmap, out, out_max);
    if (written <= 0)
        return 0;
    uint32_t bytes_free = 12u * 1024u * 1024u;
    uint32_t bytes_total = 16u * 1024u * 1024u;
    LOG(2, "AFP FPOpenVol: Reply: bitmap=0x%04X volId=0x%04X bytesFree=%u bytesTotal=%u%s%s", bitmap, v->vol_id,
        (unsigned)bytes_free, (unsigned)bytes_total, (bitmap & 0x0100) ? " name='" : "",
        (bitmap & 0x0100) ? v->name : "");
    afp_log_hex("AFP FPOpenVol resp", out, written);
    return written;
}

// Compose a tiny FPGetSrvrInfo payload
static int afp_get_srvr_info(uint8_t *out, int max_len) {
    LOG(10, "AFP FPGetSrvrInfo");
    const char *info = "AFPServer:GrannySmith;UAM=NoUserAuth";
    int n = (int)strlen(info);
    if (n > max_len)
        n = max_len;
    memcpy(out, info, n);
    LOG(10, "AFP FPGetSrvrInfo: Reply: len=%d", n);
    return n;
}

// Compose FPGetSrvrParms payload
static int afp_get_srvr_parms(uint8_t *out, int max_len) {
    int count = 0;
    for (int i = 0; i < MAX_SHARES; i++)
        if (g_shares[i].in_use)
            count++;
    LOG(10, "AFP FPGetSrvrParms: shares=%d", count);
    if (max_len < 5)
        return 0;

    time_t now = time(NULL);
    uint32_t t = (uint32_t)(now & 0xFFFFFFFFu);
    out[0] = (uint8_t)((t >> 24) & 0xFF);
    out[1] = (uint8_t)((t >> 16) & 0xFF);
    out[2] = (uint8_t)((t >> 8) & 0xFF);
    out[3] = (uint8_t)(t & 0xFF);

    int pos = 4;
    out[pos++] = (uint8_t)count;

    for (int i = 0; i < MAX_SHARES; i++)
        if (g_shares[i].in_use) {
            const char *name = g_shares[i].name;
            size_t n = strlen(name);
            if (n > 31)
                n = 31; // HFS name limit
            if (pos + 1 + 1 + (int)n > max_len)
                break; // flags + len + name
            out[pos++] = 0x00; // flags
            out[pos++] = (uint8_t)n; // length
            if (n) {
                memcpy(&out[pos], name, n);
                pos += (int)n;
            }
        }

    LOG(10, "AFP FPGetSrvrParms: Reply: time=%u numvols=%d", (unsigned)t, count);
    return pos;
}

// Common helpers
static int afp_fixed_param_len(bool is_dir, uint16_t bm) {
    int fixed = 0;
    if (!is_dir) {
        if (bm & (1u << 0))
            fixed += 2; // Attr
        if (bm & (1u << 1))
            fixed += 4; // Parent DirID
        if (bm & (1u << 2))
            fixed += 4; // CDate
        if (bm & (1u << 3))
            fixed += 4; // MDate
        if (bm & (1u << 4))
            fixed += 4; // BDate
        if (bm & (1u << 5))
            fixed += 32; // Finder
        if (bm & (1u << 6))
            fixed += 2; // Long Name offset
        if (bm & (1u << 7))
            fixed += 2; // Short Name offset
        if (bm & (1u << 8))
            fixed += 4; // FileNum
        if (bm & (1u << 9))
            fixed += 4; // DataLen
        if (bm & (1u << 10))
            fixed += 4; // RsrcLen
        if (bm & (1u << 13))
            fixed += 6; // ProDOS
    } else {
        if (bm & (1u << 0))
            fixed += 2; // Attr
        if (bm & (1u << 1))
            fixed += 4; // Parent DirID
        if (bm & (1u << 2))
            fixed += 4; // CDate
        if (bm & (1u << 3))
            fixed += 4; // MDate
        if (bm & (1u << 4))
            fixed += 4; // BDate
        if (bm & (1u << 5))
            fixed += 32; // Finder
        if (bm & (1u << 6))
            fixed += 2; // Long Name offset
        if (bm & (1u << 7))
            fixed += 2; // Short Name offset
        if (bm & (1u << 8))
            fixed += 4; // DirID
        if (bm & (1u << 9))
            fixed += 2; // Offspring Count
        if (bm & (1u << 10))
            fixed += 4; // Owner
        if (bm & (1u << 11))
            fixed += 4; // Group
        if (bm & (1u << 12))
            fixed += 4; // Access Rights
        if (bm & (1u << 13))
            fixed += 6; // ProDOS
    }
    return fixed;
}

static int afp_write_param_area(bool is_dir, uint16_t bm, uint8_t *out, int p, int out_max, int *pos_long_off,
                                int *pos_short_off) {
    if (pos_long_off)
        *pos_long_off = -1;
    if (pos_short_off)
        *pos_short_off = -1;
#define AFP_ENSURE_AND_ZERO(nbytes)                                                                                    \
    do {                                                                                                               \
        if (p + (int)(nbytes) > out_max)                                                                               \
            return -1;                                                                                                 \
        memset(out + p, 0, (nbytes));                                                                                  \
        p += (int)(nbytes);                                                                                            \
    } while (0)
    if (!is_dir) {
        if (bm & (1u << 0)) {
            AFP_ENSURE_AND_ZERO(2);
        }
        if (bm & (1u << 1)) {
            AFP_ENSURE_AND_ZERO(4);
        }
        if (bm & (1u << 2)) {
            AFP_ENSURE_AND_ZERO(4);
        }
        if (bm & (1u << 3)) {
            AFP_ENSURE_AND_ZERO(4);
        }
        if (bm & (1u << 4)) {
            AFP_ENSURE_AND_ZERO(4);
        }
        if (bm & (1u << 5)) {
            AFP_ENSURE_AND_ZERO(32);
        }
        if (bm & (1u << 6)) {
            if (p + 2 > out_max)
                return -1;
            if (pos_long_off)
                *pos_long_off = p;
            wr16be(out + p, 0);
            p += 2;
        }
        if (bm & (1u << 7)) {
            if (p + 2 > out_max)
                return -1;
            if (pos_short_off)
                *pos_short_off = p;
            wr16be(out + p, 0);
            p += 2;
        }
        if (bm & (1u << 8)) {
            AFP_ENSURE_AND_ZERO(4);
        }
        if (bm & (1u << 9)) {
            AFP_ENSURE_AND_ZERO(4);
        }
        if (bm & (1u << 10)) {
            AFP_ENSURE_AND_ZERO(4);
        }
        if (bm & (1u << 13)) {
            AFP_ENSURE_AND_ZERO(6);
        }
    } else {
        if (bm & (1u << 0)) {
            AFP_ENSURE_AND_ZERO(2);
        }
        if (bm & (1u << 1)) {
            AFP_ENSURE_AND_ZERO(4);
        }
        if (bm & (1u << 2)) {
            AFP_ENSURE_AND_ZERO(4);
        }
        if (bm & (1u << 3)) {
            AFP_ENSURE_AND_ZERO(4);
        }
        if (bm & (1u << 4)) {
            AFP_ENSURE_AND_ZERO(4);
        }
        if (bm & (1u << 5)) {
            AFP_ENSURE_AND_ZERO(32);
        }
        if (bm & (1u << 6)) {
            if (p + 2 > out_max)
                return -1;
            if (pos_long_off)
                *pos_long_off = p;
            wr16be(out + p, 0);
            p += 2;
        }
        if (bm & (1u << 7)) {
            if (p + 2 > out_max)
                return -1;
            if (pos_short_off)
                *pos_short_off = p;
            wr16be(out + p, 0);
            p += 2;
        }
        if (bm & (1u << 8)) {
            AFP_ENSURE_AND_ZERO(4);
        }
        if (bm & (1u << 9)) {
            AFP_ENSURE_AND_ZERO(2);
        }
        if (bm & (1u << 10)) {
            AFP_ENSURE_AND_ZERO(4);
        }
        if (bm & (1u << 11)) {
            AFP_ENSURE_AND_ZERO(4);
        }
        if (bm & (1u << 12)) {
            if (p + 4 > out_max)
                return -1;
            wr32be(out + p, 0x07070707u);
            p += 4;
        }
        if (bm & (1u << 13)) {
            AFP_ENSURE_AND_ZERO(6);
        }
    }
#undef AFP_ENSURE_AND_ZERO
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

static int afp_param_field_ptr(bool is_dir, uint16_t bm, int pbase, int target_bit) {
    if (!(bm & (1u << target_bit)))
        return -1;
    int off = 0;
    for (int b = 0; b < target_bit; b++) {
        if (!(bm & (1u << b)))
            continue;
        if (!is_dir) {
            switch (b) {
            case 0:
                off += 2;
                break;
            case 1:
                off += 4;
                break;
            case 2:
                off += 4;
                break;
            case 3:
                off += 4;
                break;
            case 4:
                off += 4;
                break;
            case 5:
                off += 32;
                break;
            case 6:
                off += 2;
                break;
            case 7:
                off += 2;
                break;
            case 8:
                off += 4;
                break;
            case 9:
                off += 4;
                break;
            case 10:
                off += 4;
                break;
            case 13:
                off += 6;
                break;
            default:
                break;
            }
        } else {
            switch (b) {
            case 0:
                off += 2;
                break;
            case 1:
                off += 4;
                break;
            case 2:
                off += 4;
                break;
            case 3:
                off += 4;
                break;
            case 4:
                off += 4;
                break;
            case 5:
                off += 32;
                break;
            case 6:
                off += 2;
                break;
            case 7:
                off += 2;
                break;
            case 8:
                off += 4;
                break;
            case 9:
                off += 2;
                break;
            case 10:
                off += 4;
                break;
            case 11:
                off += 4;
                break;
            case 12:
                off += 4;
                break;
            case 13:
                off += 6;
                break;
            default:
                break;
            }
        }
    }
    return pbase + off;
}

typedef uint32_t (*afp_command_handler_fn)(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len);
typedef struct {
    uint8_t opcode;
    const char *name;
    afp_command_handler_fn handler;
} afp_command_handler_t;

static uint32_t afp_cmd_get_srvr_info(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)in;
    (void)in_len;
    int produced = afp_get_srvr_info(out, out_max);
    if (out_len)
        *out_len = produced;
    return AFPERR_NoErr;
}

static uint32_t afp_cmd_get_srvr_parms(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)in;
    (void)in_len;
    int produced = afp_get_srvr_parms(out, out_max);
    if (out_len)
        *out_len = produced;
    return AFPERR_NoErr;
}

static uint32_t afp_cmd_get_vol_parms(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    if (in_len < 5 || out_max < 2)
        return AFPERR_ParamErr;
    // Skip pad byte at in[0]
    uint16_t vol_id = rd16be(in + 1);
    uint16_t bitmap = rd16be(in + 3);
    vol_t *v = resolve_vol_by_id(vol_id);
    if (!v)
        return AFPERR_ObjectNotFound;
    int produced = afp_write_vol_param_block(v, &bitmap, out, out_max);
    if (produced <= 0)
        return AFPERR_ParamErr;
    LOG(10, "AFP FPGetVolParms: vol=0x%04X bitmap=0x%04X reply=%d", vol_id, bitmap, produced);
    if (out_len)
        *out_len = produced;
    return AFPERR_NoErr;
}

static uint32_t afp_cmd_close_vol(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)out;
    (void)out_max;
    if (in_len < 3)
        return AFPERR_ParamErr;
    uint16_t vol_id = rd16be(in + 1);
    LOG(10, "AFP FPCloseVol: volId=0x%04X", vol_id);
    if (out_len)
        *out_len = 0;
    return AFPERR_NoErr;
}

static uint32_t afp_cmd_login(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    if (in_len < 2)
        return AFPERR_ParamErr;
    int pos = 0;
    uint8_t vlen = in[pos++];
    if (pos + vlen > in_len)
        vlen = (uint8_t)((in_len > pos) ? (in_len - pos) : 0);
    char ver[256];
    int vn = (vlen < 255) ? vlen : 255;
    memcpy(ver, &in[pos], vn);
    ver[vn] = '\0';
    pos += vlen;
    if (pos >= in_len)
        return AFPERR_ParamErr;
    uint8_t ulen = in[pos++];
    if (pos + ulen > in_len)
        ulen = (uint8_t)((in_len > pos) ? (in_len - pos) : 0);
    char uam[256];
    int un = (ulen < 255) ? ulen : 255;
    memcpy(uam, &in[pos], un);
    uam[un] = '\0';
    pos += ulen;
    LOG(10, "AFP FPLogin: version='%s' uam='%s'", ver, uam);
    bool ver_ok = (strcmp(ver, "AFPVersion 2.0") == 0) || (strcmp(ver, "AFPVersion 2.1") == 0);
    bool uam_ok = (strcmp(uam, "No User Authent") == 0);
    if (!ver_ok) {
        LOG(7, "AFP FPLogin: unsupported version → BadVersNum");
        return AFPERR_BadVersNum;
    }
    if (!uam_ok) {
        LOG(7, "AFP FPLogin: unsupported UAM → BadUAM");
        return AFPERR_BadUAM;
    }
    if (out_max < 2)
        return AFPERR_ParamErr;
    wr16be(out, 0x0000);
    if (out_len)
        *out_len = 2;
    return AFPERR_NoErr;
}

static uint32_t afp_cmd_open_vol(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    int produced = afp_open_vol(in, in_len, out, out_max);
    if (out_len)
        *out_len = produced > 0 ? produced : 0;
    return AFPERR_NoErr;
}

static uint32_t afp_cmd_open_dir(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    if (in_len < 10 || out_max < 4)
        return AFPERR_ParamErr;
    int pos = 0;
    pos++; // pad
    uint16_t vol_id = rd16be(in + pos);
    pos += 2;
    uint32_t dir_id = rd32be(in + pos);
    pos += 4;
    uint8_t path_type = (pos < in_len) ? in[pos++] : 0;
    (void)path_type;
    char path[AFP_MAX_NAME];
    if ((pos = afp_read_pstring(in, in_len, pos, path, sizeof(path))) < 0)
        return AFPERR_ParamErr;
    vol_t *vol = resolve_vol_by_id(vol_id);
    if (!vol)
        return AFPERR_ObjectNotFound;
    catalog_entry_t *base = afp_catalog_resolve(vol, dir_id);
    if (!base)
        return AFPERR_DirNotFound;
    char target_rel[AFP_MAX_REL_PATH];
    if (!afp_normalize_relative_path(base->rel_path, path, target_rel, sizeof(target_rel)))
        return AFPERR_ParamErr;
    struct stat st;
    if (!afp_stat_path(vol, target_rel, &st))
        return AFPERR_ObjectNotFound;
    if (!S_ISDIR(st.st_mode))
        return AFPERR_ObjectTypeErr;
    catalog_entry_t *entry = afp_catalog_ensure(vol, target_rel);
    if (!entry)
        return AFPERR_MiscErr;
    wr32be(out, entry->cnid);
    if (out_len)
        *out_len = 4;
    LOG(10, "AFP FPOpenDir: vol=0x%04X parent=0x%08X path='%s' → cnid=0x%08X", vol_id, (unsigned)dir_id,
        target_rel[0] ? target_rel : "<root>", entry->cnid);
    return AFPERR_NoErr;
}

static uint32_t afp_cmd_close_dir(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)out;
    (void)out_max;
    if (in_len < 7)
        return AFPERR_ParamErr;
    int pos = 0;
    pos++;
    uint16_t vol_id = rd16be(in + pos);
    pos += 2;
    uint32_t dir_id = rd32be(in + pos);
    pos += 4;
    vol_t *vol = resolve_vol_by_id(vol_id);
    if (!vol)
        return AFPERR_ObjectNotFound;
    if (dir_id != 0 && dir_id != AFP_CNID_ROOT && !afp_catalog_find_by_cnid(vol, dir_id))
        return AFPERR_ParamErr;
    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPCloseDir: vol=0x%04X dir=0x%08X", vol_id, (unsigned)dir_id);
    return AFPERR_NoErr;
}

static uint32_t afp_cmd_open_dt(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    if (in_len < 3 || out_max < 2)
        return AFPERR_ParamErr;
    uint16_t vol_id = rd16be(in + 1);
    dt_entry_t *dt = ensure_dt_for_vol(vol_id);
    if (!dt)
        return AFPERR_ObjectNotFound;
    wr16be(out, dt->dt_ref);
    if (out_len)
        *out_len = 2;
    LOG(10, "AFP FPOpenDT: volId=0x%04X → DTRef=0x%04X", vol_id, dt->dt_ref);
    return AFPERR_NoErr;
}

// FPOpenFork (AFP 0x1A) - Open a data or resource fork
static uint32_t afp_cmd_open_fork(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    if (in_len < 10)
        return AFPERR_ParamErr;
    int pos = 0;
    uint8_t flag = in[pos++]; // bit 7 = resource fork
    bool is_resource = (flag & 0x80) != 0;
    uint16_t vol_id = rd16be(in + pos);
    pos += 2;
    uint32_t dir_id = rd32be(in + pos);
    pos += 4;
    uint16_t bitmap = rd16be(in + pos);
    pos += 2;
    uint16_t access_mode = (pos + 2 <= in_len) ? rd16be(in + pos) : 0x0001;
    pos += 2;
    uint8_t path_type = (pos < in_len) ? in[pos++] : 0;
    (void)path_type;
    char path[AFP_MAX_NAME];
    if ((pos = afp_read_pstring(in, in_len, pos, path, sizeof(path))) < 0)
        return AFPERR_ParamErr;

    vol_t *vol = resolve_vol_by_id(vol_id);
    if (!vol)
        return AFPERR_ObjectNotFound;
    catalog_entry_t *base = afp_catalog_resolve(vol, dir_id);
    if (!base)
        return AFPERR_DirNotFound;

    char target_rel[AFP_MAX_REL_PATH];
    if (!afp_normalize_relative_path(base->rel_path, path, target_rel, sizeof(target_rel)))
        return AFPERR_ParamErr;
    struct stat st;
    if (!afp_stat_path(vol, target_rel, &st))
        return AFPERR_ObjectNotFound;
    if (S_ISDIR(st.st_mode))
        return AFPERR_ObjectTypeErr;

    char full[PATH_MAX];
    if (!afp_full_path(vol, target_rel, full, sizeof(full)))
        return AFPERR_ParamErr;

    LOG(10, "AFP FPOpenFork: vol=0x%04X dir=0x%08X %s bitmap=0x%04X access=0x%04X path='%s'", vol_id, (unsigned)dir_id,
        is_resource ? "rsrc" : "data", bitmap, access_mode, target_rel);

    fork_t *fk = alloc_fork(vol_id, full, target_rel, access_mode, is_resource);
    if (!fk)
        return AFPERR_TooManyFilesOpen;

    // Ensure catalog entry exists for the file
    catalog_entry_t *file_entry = afp_catalog_find_by_path(vol, target_rel);
    if (!file_entry)
        file_entry = afp_catalog_insert(vol, target_rel, false);

    // Reply: bitmap(2) + OForkRefNum(2) + file params
    if (out_max < 4)
        return AFPERR_ParamErr;
    wr16be(out + 0, bitmap);
    wr16be(out + 2, fk->fork_ref);

    int pbase = 4;
    int p = pbase;
    if (bitmap) {
        int pos_long_off = -1, pos_short_off = -1;
        p = afp_write_param_area(false, bitmap, out, pbase, out_max, &pos_long_off, &pos_short_off);
        if (p < 0) {
            close_fork(fk);
            return AFPERR_ParamErr;
        }
        if (!afp_populate_param_area(false, vol, target_rel, &st, bitmap, out, pbase)) {
            close_fork(fk);
            return AFPERR_ParamErr;
        }
        const char *name = afp_last_component(target_rel);
        if (!name)
            name = "";
        uint8_t long_len = (uint8_t)(strlen(name) > 255 ? 255 : strlen(name));
        uint8_t short_len = (uint8_t)(strlen(name) > 31 ? 31 : strlen(name));
        p = afp_write_name_vars(out, p, out_max, pbase, name, bitmap, pos_long_off, pos_short_off, long_len, short_len);
        if (p < 0) {
            close_fork(fk);
            return AFPERR_ParamErr;
        }
    }
    if (p % 2 && p < out_max)
        out[p++] = 0x00;
    if (out_len)
        *out_len = p;
    LOG(2, "AFP FPOpenFork: vol=0x%04X dir=0x%08X %s path='%s' → ref=0x%04X reply=%d", vol_id, (unsigned)dir_id,
        is_resource ? "rsrc" : "data", target_rel, fk->fork_ref, p);
    return AFPERR_NoErr;
}

// FPCloseFork (AFP 0x04) - Close an open fork
static uint32_t afp_cmd_close_fork(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)out;
    (void)out_max;
    if (in_len < 3)
        return AFPERR_ParamErr;
    uint16_t fork_ref = rd16be(in + 1);
    fork_t *fk = find_fork(fork_ref);
    if (!fk)
        return AFPERR_ParamErr;
    LOG(10, "AFP FPCloseFork: ref=0x%04X path='%s'", fork_ref, fk->rel_path);
    close_fork(fk);
    if (out_len)
        *out_len = 0;
    return AFPERR_NoErr;
}

// FPRead (AFP 0x1B) - Read data from an open fork
static uint32_t afp_cmd_read(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    if (in_len < 13)
        return AFPERR_ParamErr;
    int pos = 0;
    pos++; // pad
    uint16_t fork_ref = rd16be(in + pos);
    pos += 2;
    uint32_t offset = rd32be(in + pos);
    pos += 4;
    uint32_t req_count = rd32be(in + pos);
    pos += 4;
    uint8_t newline_mask = (pos < in_len) ? in[pos++] : 0;
    uint8_t newline_char = (pos < in_len) ? in[pos++] : 0;

    LOG(10, "AFP FPRead: ref=0x%04X off=%u req=%u nlMask=0x%02X nlChar=0x%02X", fork_ref, offset, req_count,
        newline_mask, newline_char);

    fork_t *fk = find_fork(fork_ref);
    if (!fk)
        return AFPERR_ParamErr;

    // Get file size for EOF detection
    fseek(fk->f, 0, SEEK_END);
    long file_size = ftell(fk->f);
    if (file_size < 0)
        file_size = 0;

    // Check if starting beyond EOF
    if ((long)offset >= file_size) {
        if (out_len)
            *out_len = 0;
        return AFPERR_EOFErr;
    }

    fseek(fk->f, (long)offset, SEEK_SET);

    uint32_t avail = (uint32_t)(file_size - (long)offset);
    uint32_t to_read = req_count;
    if (to_read > avail)
        to_read = avail;
    if (to_read > (uint32_t)out_max)
        to_read = (uint32_t)out_max;

    size_t got = fread(out, 1, to_read, fk->f);

    // Handle newline termination if mask is set
    if (newline_mask) {
        for (size_t i = 0; i < got; i++) {
            if ((out[i] & newline_mask) == (newline_char & newline_mask)) {
                got = i + 1;
                break;
            }
        }
    }

    if (out_len)
        *out_len = (int)got;
    LOG(2, "AFP FPRead: ref=0x%04X off=%u req=%u got=%zu", fork_ref, offset, req_count, got);

    // Return EOF only if the read was truncated by end-of-fork
    if (got < req_count && offset + (uint32_t)got >= (uint32_t)file_size)
        return AFPERR_EOFErr;
    return AFPERR_NoErr;
}

// FPGetForkParms (AFP 0x0E) - Get parameters for an open fork
static uint32_t afp_cmd_get_fork_parms(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    if (in_len < 5)
        return AFPERR_ParamErr;
    int pos = 0;
    pos++; // pad
    uint16_t fork_ref = rd16be(in + pos);
    pos += 2;
    uint16_t bitmap = rd16be(in + pos);
    pos += 2;

    fork_t *fk = find_fork(fork_ref);
    if (!fk)
        return AFPERR_ParamErr;

    vol_t *vol = find_vol_by_id(fk->vol_id);
    if (!vol)
        return AFPERR_ParamErr;

    struct stat st;
    if (stat(fk->path, &st) != 0)
        return AFPERR_ObjectNotFound;

    if (out_max < 2)
        return AFPERR_ParamErr;
    wr16be(out + 0, bitmap);

    int pbase = 2;
    int p = pbase;
    if (bitmap) {
        int pos_long_off = -1, pos_short_off = -1;
        p = afp_write_param_area(false, bitmap, out, pbase, out_max, &pos_long_off, &pos_short_off);
        if (p < 0)
            return AFPERR_ParamErr;
        if (!afp_populate_param_area(false, vol, fk->rel_path, &st, bitmap, out, pbase))
            return AFPERR_ParamErr;
        const char *name = afp_last_component(fk->rel_path);
        if (!name)
            name = "";
        uint8_t long_len = (uint8_t)(strlen(name) > 255 ? 255 : strlen(name));
        uint8_t short_len = (uint8_t)(strlen(name) > 31 ? 31 : strlen(name));
        p = afp_write_name_vars(out, p, out_max, pbase, name, bitmap, pos_long_off, pos_short_off, long_len, short_len);
        if (p < 0)
            return AFPERR_ParamErr;
    }
    if (p % 2 && p < out_max)
        out[p++] = 0x00;
    if (out_len)
        *out_len = p;
    LOG(10, "AFP FPGetForkParms: ref=0x%04X bitmap=0x%04X reply=%d", fork_ref, bitmap, p);
    return AFPERR_NoErr;
}

// FPSetForkParms (AFP 0x1F) - Set fork parameters (e.g. truncate)
static uint32_t afp_cmd_set_fork_parms(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)out;
    (void)out_max;
    if (in_len < 7)
        return AFPERR_ParamErr;
    int pos = 0;
    pos++; // pad
    uint16_t fork_ref = rd16be(in + pos);
    pos += 2;
    uint16_t bitmap = rd16be(in + pos);
    pos += 2;

    fork_t *fk = find_fork(fork_ref);
    if (!fk)
        return AFPERR_ParamErr;

    // Data fork length is bit 9, resource fork length is bit 10
    if (bitmap & (1u << 9)) {
        if (pos + 4 > in_len)
            return AFPERR_ParamErr;
        uint32_t new_len = rd32be(in + pos);
        pos += 4;
        int fd = fileno(fk->f);
        if (fd >= 0)
            ftruncate(fd, (off_t)new_len);
    }
    if (bitmap & (1u << 10)) {
        // Resource fork length: truncate the .rsrc companion file
        if (pos + 4 > in_len)
            return AFPERR_ParamErr;
        uint32_t new_len = rd32be(in + pos);
        pos += 4;
        int fd = fileno(fk->f);
        if (fd >= 0)
            ftruncate(fd, (off_t)new_len);
    }
    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPSetForkParms: ref=0x%04X bitmap=0x%04X", fork_ref, bitmap);
    return AFPERR_NoErr;
}

// FPFlush (AFP 0x0A) - Flush all forks on a volume
static uint32_t afp_cmd_flush(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)out;
    (void)out_max;
    if (in_len < 3)
        return AFPERR_ParamErr;
    uint16_t vol_id = rd16be(in + 1);
    for (int i = 0; i < 16; i++) {
        if (g_forks[i].in_use && g_forks[i].vol_id == vol_id && g_forks[i].f)
            fflush(g_forks[i].f);
    }
    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPFlush: vol=0x%04X", vol_id);
    return AFPERR_NoErr;
}

// FPFlushFork (AFP 0x0B) - Flush a single fork
static uint32_t afp_cmd_flush_fork(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)out;
    (void)out_max;
    if (in_len < 3)
        return AFPERR_ParamErr;
    uint16_t fork_ref = rd16be(in + 1);
    fork_t *fk = find_fork(fork_ref);
    if (!fk)
        return AFPERR_ParamErr;
    if (fk->f)
        fflush(fk->f);
    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPFlushFork: ref=0x%04X", fork_ref);
    return AFPERR_NoErr;
}

// Helper to parse vol/dir/bitmap/path and extract Finder Info if present
static uint32_t afp_parse_set_parms(const uint8_t *in, int in_len) {
    int pos = 0;
    pos++; // pad
    if (pos + 2 > in_len)
        return AFPERR_ParamErr;
    uint16_t vol_id = rd16be(in + pos);
    pos += 2;
    if (pos + 4 > in_len)
        return AFPERR_ParamErr;
    uint32_t dir_id = rd32be(in + pos);
    pos += 4;
    // All FPSet*Parms commands use a single bitmap field
    if (pos + 2 > in_len)
        return AFPERR_ParamErr;
    uint16_t bitmap = rd16be(in + pos);
    pos += 2;
    LOG(10, "AFP SetParms: vol=0x%04X dir=0x%08X bitmap=0x%04X", vol_id, dir_id, bitmap);
    uint8_t path_type = (pos < in_len) ? in[pos++] : 0;
    (void)path_type;
    char path[AFP_MAX_NAME];
    if ((pos = afp_read_pstring(in, in_len, pos, path, sizeof(path))) < 0)
        return AFPERR_ParamErr;
    LOG(10, "AFP SetParms: pathType=%u name='%s' remainLen=%d", path_type, path, in_len - pos);

    vol_t *vol = resolve_vol_by_id(vol_id);
    if (!vol)
        return AFPERR_ObjectNotFound;
    catalog_entry_t *base = afp_catalog_resolve(vol, dir_id);
    if (!base)
        return AFPERR_DirNotFound;

    char target_rel[AFP_MAX_REL_PATH];
    if (!afp_normalize_relative_path(base->rel_path, path, target_rel, sizeof(target_rel)))
        return AFPERR_ParamErr;
    struct stat st;
    if (!afp_stat_path(vol, target_rel, &st)) {
        LOG(7, "AFP SetParms: stat failed for '%s'", target_rel);
        return AFPERR_ObjectNotFound;
    }

    // If Finder Info bit (5) is in bitmap, store it
    if (bitmap & (1u << 5)) {
        // Calculate offset to Finder Info in the param data
        bool is_dir = S_ISDIR(st.st_mode);
        int fi_offset = 0;
        for (int b = 0; b < 5; b++) {
            if (!(bitmap & (1u << b)))
                continue;
            if (!is_dir) {
                switch (b) {
                case 0:
                    fi_offset += 2;
                    break;
                case 1:
                    fi_offset += 4;
                    break;
                case 2:
                    fi_offset += 4;
                    break;
                case 3:
                    fi_offset += 4;
                    break;
                case 4:
                    fi_offset += 4;
                    break;
                default:
                    break;
                }
            } else {
                switch (b) {
                case 0:
                    fi_offset += 2;
                    break;
                case 1:
                    fi_offset += 4;
                    break;
                case 2:
                    fi_offset += 4;
                    break;
                case 3:
                    fi_offset += 4;
                    break;
                case 4:
                    fi_offset += 4;
                    break;
                default:
                    break;
                }
            }
        }
        if (pos + fi_offset + 32 <= in_len)
            afp_set_finder_info(vol_id, target_rel, in + pos + fi_offset);
    }

    LOG(2, "AFP SetParms: vol=0x%04X dir=0x%08X bitmap=0x%04X path='%s'", vol_id, (unsigned)dir_id, bitmap,
        target_rel[0] ? target_rel : "<root>");
    return AFPERR_NoErr;
}

// FPSetFileParms (AFP 0x1E) - Set file parameters
static uint32_t afp_cmd_set_file_parms(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)out;
    (void)out_max;
    if (out_len)
        *out_len = 0;
    return afp_parse_set_parms(in, in_len);
}

// FPSetDirParms (AFP 0x1D) - Set directory parameters
static uint32_t afp_cmd_set_dir_parms(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)out;
    (void)out_max;
    if (out_len)
        *out_len = 0;
    return afp_parse_set_parms(in, in_len);
}

// FPSetFileDirParms (AFP 0x23) - Set file or directory parameters (single bitmap per AFP spec)
static uint32_t afp_cmd_set_file_dir_parms(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)out;
    (void)out_max;
    if (out_len)
        *out_len = 0;
    return afp_parse_set_parms(in, in_len);
}

// FPSetVolParms (AFP 0x20) - Set volume parameters (backup date)
static uint32_t afp_cmd_set_vol_parms(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)out;
    (void)out_max;
    if (in_len < 5)
        return AFPERR_ParamErr;
    // Accept silently (backup date bit 4 only)
    if (out_len)
        *out_len = 0;
    uint16_t sv_vol_id = (in_len >= 3) ? rd16be(in + 1) : 0;
    uint16_t sv_bitmap = (in_len >= 5) ? rd16be(in + 3) : 0;
    LOG(10, "AFP FPSetVolParms: vol=0x%04X bitmap=0x%04X accepted", sv_vol_id, sv_bitmap);
    return AFPERR_NoErr;
}

// FPCloseDT (AFP 0x31) - Close desktop database reference
static uint32_t afp_cmd_close_dt(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)out;
    (void)out_max;
    if (in_len < 3)
        return AFPERR_ParamErr;
    uint16_t dt_ref = rd16be(in + 1);
    for (int i = 0; i < MAX_SHARES; i++) {
        if (g_dt[i].in_use && g_dt[i].dt_ref == dt_ref) {
            g_dt[i].in_use = false;
            break;
        }
    }
    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPCloseDT: ref=0x%04X", dt_ref);
    return AFPERR_NoErr;
}

// FPGetIcon (AFP 0x33) - Get icon bitmap from desktop database
static uint32_t afp_cmd_get_icon(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    if (in_len < 14)
        return AFPERR_ParamErr;
    int pos = 0;
    pos++; // pad
    uint16_t dt_ref = rd16be(in + pos);
    pos += 2;
    uint32_t creator = rd32be(in + pos);
    pos += 4;
    uint32_t file_type = rd32be(in + pos);
    pos += 4;
    uint8_t icon_type = in[pos++];
    uint16_t req_size = (pos + 2 <= in_len) ? rd16be(in + pos) : 0;
    pos += 2;

    LOG(10, "AFP FPGetIcon: dt=0x%04X creator=0x%08X type=0x%08X icontype=%u reqSize=%u", dt_ref, creator, file_type,
        icon_type, req_size);

    // Search stored icons
    for (int i = 0; i < ARRAY_LEN(g_dt_icons); i++) {
        if (g_dt_icons[i].in_use && g_dt_icons[i].creator == creator && g_dt_icons[i].file_type == file_type &&
            g_dt_icons[i].icon_type == icon_type) {
            int sz = (int)g_dt_icons[i].icon_size;
            if (sz > out_max)
                sz = out_max;
            memcpy(out, g_dt_icons[i].icon_data, sz);
            if (out_len)
                *out_len = sz;
            return AFPERR_NoErr;
        }
    }
    return AFPERR_ItemNotFound;
}

// FPGetIconInfo (AFP 0x34) - Enumerate icons by creator
static uint32_t afp_cmd_get_icon_info(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    if (in_len < 9 || out_max < 12)
        return AFPERR_ParamErr;
    int pos = 0;
    pos++; // pad
    uint16_t dt_ref = rd16be(in + pos);
    pos += 2;
    uint32_t creator = rd32be(in + pos);
    pos += 4;
    uint16_t icon_index = rd16be(in + pos);
    pos += 2;

    LOG(10, "AFP FPGetIconInfo: dt=0x%04X creator=0x%08X index=%u", dt_ref, creator, icon_index);

    // Find the Nth icon with matching creator
    int found = 0;
    for (int i = 0; i < ARRAY_LEN(g_dt_icons); i++) {
        if (g_dt_icons[i].in_use && g_dt_icons[i].creator == creator) {
            found++;
            if (found == (int)icon_index) {
                // Reply: IconTag(4) + FileType(4) + IconType(1) + pad(1) + Size(2)
                wr32be(out + 0, 0); // tag
                wr32be(out + 4, g_dt_icons[i].file_type);
                out[8] = g_dt_icons[i].icon_type;
                out[9] = 0; // pad
                wr16be(out + 10, g_dt_icons[i].icon_size);
                if (out_len)
                    *out_len = 12;
                return AFPERR_NoErr;
            }
        }
    }
    return AFPERR_ItemNotFound;
}

// FPAddAPPL (AFP 0x35) - Add APPL mapping to desktop database
static uint32_t afp_cmd_add_appl(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)out;
    (void)out_max;
    if (in_len < 11)
        return AFPERR_ParamErr;
    int pos = 0;
    pos++; // pad
    uint16_t dt_ref = rd16be(in + pos);
    pos += 2;
    uint32_t dir_id = rd32be(in + pos);
    pos += 4;
    (void)dir_id;
    uint32_t creator = rd32be(in + pos);
    pos += 4;
    uint32_t appl_tag = (pos + 4 <= in_len) ? rd32be(in + pos) : 0;
    pos += 4;

    // Find vol from dt_ref
    uint16_t vol_id = 0;
    for (int i = 0; i < MAX_SHARES; i++) {
        if (g_dt[i].in_use && g_dt[i].dt_ref == dt_ref) {
            vol_id = g_dt[i].vol_id;
            break;
        }
    }

    // Find empty or existing slot
    dt_appl_t *slot = NULL;
    for (int i = 0; i < ARRAY_LEN(g_dt_appls); i++) {
        if (g_dt_appls[i].in_use && g_dt_appls[i].vol_id == vol_id && g_dt_appls[i].creator == creator) {
            slot = &g_dt_appls[i];
            break;
        }
    }
    if (!slot) {
        for (int i = 0; i < ARRAY_LEN(g_dt_appls); i++) {
            if (!g_dt_appls[i].in_use) {
                slot = &g_dt_appls[i];
                break;
            }
        }
    }
    if (!slot)
        return AFPERR_MiscErr;

    slot->in_use = true;
    slot->vol_id = vol_id;
    slot->creator = creator;
    slot->appl_tag = appl_tag;
    // Read path from remainder
    uint8_t path_type = (pos < in_len) ? in[pos++] : 0;
    (void)path_type;
    char path_name[AFP_MAX_NAME];
    afp_read_pstring(in, in_len, pos, path_name, sizeof(path_name));
    strncpy(slot->path, path_name, sizeof(slot->path) - 1);

    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPAddAPPL: dt=0x%04X creator=0x%08X tag=0x%08X path='%s'", dt_ref, creator, appl_tag, path_name);
    return AFPERR_NoErr;
}

// FPRemoveAPPL (AFP 0x36) - Remove APPL mapping
static uint32_t afp_cmd_remove_appl(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)out;
    (void)out_max;
    if (in_len < 11)
        return AFPERR_ParamErr;
    int pos = 0;
    pos++; // pad
    uint16_t dt_ref = rd16be(in + pos);
    pos += 2;
    uint32_t dir_id = rd32be(in + pos);
    pos += 4;
    (void)dir_id;
    uint32_t creator = rd32be(in + pos);
    pos += 4;

    uint16_t vol_id = 0;
    for (int i = 0; i < MAX_SHARES; i++) {
        if (g_dt[i].in_use && g_dt[i].dt_ref == dt_ref) {
            vol_id = g_dt[i].vol_id;
            break;
        }
    }
    for (int i = 0; i < ARRAY_LEN(g_dt_appls); i++) {
        if (g_dt_appls[i].in_use && g_dt_appls[i].vol_id == vol_id && g_dt_appls[i].creator == creator) {
            memset(&g_dt_appls[i], 0, sizeof(g_dt_appls[i]));
            break;
        }
    }
    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPRemoveAPPL: dt=0x%04X creator=0x%08X", dt_ref, creator);
    return AFPERR_NoErr;
}

// FPGetAPPL (AFP 0x37) - Get APPL mapping by creator and index
static uint32_t afp_cmd_get_appl(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    if (in_len < 9 || out_max < 6)
        return AFPERR_ParamErr;
    int pos = 0;
    pos++; // pad
    uint16_t dt_ref = rd16be(in + pos);
    pos += 2;
    uint32_t creator = rd32be(in + pos);
    pos += 4;
    uint16_t appl_index = rd16be(in + pos);
    pos += 2;
    uint16_t bitmap = (pos + 2 <= in_len) ? rd16be(in + pos) : 0;
    pos += 2;

    LOG(10, "AFP FPGetAPPL: dt=0x%04X creator=0x%08X index=%u bitmap=0x%04X", dt_ref, creator, appl_index, bitmap);

    uint16_t vol_id = 0;
    for (int i = 0; i < MAX_SHARES; i++) {
        if (g_dt[i].in_use && g_dt[i].dt_ref == dt_ref) {
            vol_id = g_dt[i].vol_id;
            break;
        }
    }

    int found = 0;
    for (int i = 0; i < ARRAY_LEN(g_dt_appls); i++) {
        if (g_dt_appls[i].in_use && g_dt_appls[i].vol_id == vol_id && g_dt_appls[i].creator == creator) {
            found++;
            if (found == (int)appl_index) {
                // Reply: bitmap(2) + ApplTag(4) + file params
                wr16be(out + 0, bitmap);
                wr32be(out + 2, g_dt_appls[i].appl_tag);
                if (out_len)
                    *out_len = 6;
                return AFPERR_NoErr;
            }
        }
    }
    return AFPERR_ItemNotFound;
}

// FPAddComment (AFP 0x38) - Add comment to desktop database
static uint32_t afp_cmd_add_comment(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)out;
    (void)out_max;
    if (in_len < 7)
        return AFPERR_ParamErr;
    int pos = 0;
    pos++; // pad
    uint16_t dt_ref = rd16be(in + pos);
    pos += 2;
    uint32_t dir_id = rd32be(in + pos);
    pos += 4;
    uint8_t path_type = (pos < in_len) ? in[pos++] : 0;
    (void)path_type;
    char path_name[AFP_MAX_NAME];
    if ((pos = afp_read_pstring(in, in_len, pos, path_name, sizeof(path_name))) < 0)
        return AFPERR_ParamErr;

    // Read the comment (Pascal string after the path)
    char comment[200];
    int comment_len = 0;
    if (pos < in_len) {
        comment_len = in[pos++];
        if (comment_len > (int)sizeof(comment) - 1)
            comment_len = (int)sizeof(comment) - 1;
        if (pos + comment_len > in_len)
            comment_len = in_len - pos;
        if (comment_len > 0)
            memcpy(comment, in + pos, comment_len);
    }
    comment[comment_len] = '\0';

    uint16_t vol_id = 0;
    for (int i = 0; i < MAX_SHARES; i++) {
        if (g_dt[i].in_use && g_dt[i].dt_ref == dt_ref) {
            vol_id = g_dt[i].vol_id;
            break;
        }
    }

    // Find or allocate slot
    dt_comment_t *slot = NULL;
    for (int i = 0; i < ARRAY_LEN(g_dt_comments); i++) {
        if (g_dt_comments[i].in_use && g_dt_comments[i].vol_id == vol_id && g_dt_comments[i].dir_id == dir_id &&
            strcmp(g_dt_comments[i].path, path_name) == 0) {
            slot = &g_dt_comments[i];
            break;
        }
    }
    if (!slot) {
        for (int i = 0; i < ARRAY_LEN(g_dt_comments); i++) {
            if (!g_dt_comments[i].in_use) {
                slot = &g_dt_comments[i];
                break;
            }
        }
    }
    if (!slot)
        return AFPERR_MiscErr;

    slot->in_use = true;
    slot->vol_id = vol_id;
    slot->dir_id = dir_id;
    strncpy(slot->path, path_name, sizeof(slot->path) - 1);
    slot->comment_len = (uint8_t)comment_len;
    memcpy(slot->comment, comment, comment_len);
    slot->comment[comment_len] = '\0';

    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPAddComment: dt=0x%04X dir=0x%08X path='%s' commentLen=%d", dt_ref, dir_id, path_name, comment_len);
    return AFPERR_NoErr;
}

// FPRemoveComment (AFP 0x39) - Remove comment from desktop database
static uint32_t afp_cmd_remove_comment(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)out;
    (void)out_max;
    if (in_len < 7)
        return AFPERR_ParamErr;
    int pos = 0;
    pos++; // pad
    uint16_t dt_ref = rd16be(in + pos);
    pos += 2;
    uint32_t dir_id = rd32be(in + pos);
    pos += 4;
    uint8_t path_type = (pos < in_len) ? in[pos++] : 0;
    (void)path_type;
    char path_name[AFP_MAX_NAME];
    afp_read_pstring(in, in_len, pos, path_name, sizeof(path_name));

    uint16_t vol_id = 0;
    for (int i = 0; i < MAX_SHARES; i++) {
        if (g_dt[i].in_use && g_dt[i].dt_ref == dt_ref) {
            vol_id = g_dt[i].vol_id;
            break;
        }
    }
    for (int i = 0; i < ARRAY_LEN(g_dt_comments); i++) {
        if (g_dt_comments[i].in_use && g_dt_comments[i].vol_id == vol_id && g_dt_comments[i].dir_id == dir_id &&
            strcmp(g_dt_comments[i].path, path_name) == 0) {
            memset(&g_dt_comments[i], 0, sizeof(g_dt_comments[i]));
            break;
        }
    }
    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPRemoveComment: dt=0x%04X dir=0x%08X path='%s'", dt_ref, dir_id, path_name);
    return AFPERR_NoErr;
}

// FPGetComment (AFP 0x3A) - Get comment from desktop database
static uint32_t afp_cmd_get_comment(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    if (in_len < 7)
        return AFPERR_ParamErr;
    int pos = 0;
    pos++; // pad
    uint16_t dt_ref = rd16be(in + pos);
    pos += 2;
    uint32_t dir_id = rd32be(in + pos);
    pos += 4;
    uint8_t path_type = (pos < in_len) ? in[pos++] : 0;
    (void)path_type;
    char path_name[AFP_MAX_NAME];
    afp_read_pstring(in, in_len, pos, path_name, sizeof(path_name));

    LOG(10, "AFP FPGetComment: dt=0x%04X dir=0x%08X path='%s'", dt_ref, dir_id, path_name);

    uint16_t vol_id = 0;
    for (int i = 0; i < MAX_SHARES; i++) {
        if (g_dt[i].in_use && g_dt[i].dt_ref == dt_ref) {
            vol_id = g_dt[i].vol_id;
            break;
        }
    }
    for (int i = 0; i < ARRAY_LEN(g_dt_comments); i++) {
        if (g_dt_comments[i].in_use && g_dt_comments[i].vol_id == vol_id && g_dt_comments[i].dir_id == dir_id &&
            strcmp(g_dt_comments[i].path, path_name) == 0) {
            // Reply: Pascal string
            int clen = (int)g_dt_comments[i].comment_len;
            if (1 + clen > out_max)
                clen = out_max - 1;
            out[0] = (uint8_t)clen;
            if (clen > 0)
                memcpy(out + 1, g_dt_comments[i].comment, clen);
            if (out_len)
                *out_len = 1 + clen;
            return AFPERR_NoErr;
        }
    }
    return AFPERR_ItemNotFound;
}

// FPAddIcon (AFP 0xC0) - Add icon to desktop database (via ASP_WRITE)
static uint32_t afp_cmd_add_icon(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)out;
    (void)out_max;
    // Params: pad(1) + DTRefNum(2) + Creator(4) + FileType(4) + IconType(1) + pad(1) + IconTag(2) + BitmapSize(2) = 17
    if (in_len < 17)
        return AFPERR_ParamErr;
    int pos = 0;
    pos++; // pad
    uint16_t dt_ref = rd16be(in + pos);
    pos += 2;
    uint32_t creator = rd32be(in + pos);
    pos += 4;
    uint32_t file_type = rd32be(in + pos);
    pos += 4;
    uint8_t icon_type = in[pos++];
    pos++; // pad
    pos += 2; // icon tag (unused)
    uint16_t icon_size = rd16be(in + pos);
    pos += 2;

    uint16_t vol_id = 0;
    for (int i = 0; i < MAX_SHARES; i++) {
        if (g_dt[i].in_use && g_dt[i].dt_ref == dt_ref) {
            vol_id = g_dt[i].vol_id;
            break;
        }
    }

    // Icon bitmap data follows params
    const uint8_t *icon_data = in + pos;
    int icon_data_len = in_len - pos;
    if (icon_data_len < 0)
        icon_data_len = 0;
    if (icon_size > (uint16_t)icon_data_len)
        icon_size = (uint16_t)icon_data_len;
    if (icon_size > sizeof(g_dt_icons[0].icon_data))
        icon_size = sizeof(g_dt_icons[0].icon_data);

    // Find or allocate slot
    dt_icon_t *slot = NULL;
    for (int i = 0; i < ARRAY_LEN(g_dt_icons); i++) {
        if (g_dt_icons[i].in_use && g_dt_icons[i].vol_id == vol_id && g_dt_icons[i].creator == creator &&
            g_dt_icons[i].file_type == file_type && g_dt_icons[i].icon_type == icon_type) {
            slot = &g_dt_icons[i];
            break;
        }
    }
    if (!slot) {
        for (int i = 0; i < ARRAY_LEN(g_dt_icons); i++) {
            if (!g_dt_icons[i].in_use) {
                slot = &g_dt_icons[i];
                break;
            }
        }
    }
    if (!slot)
        return AFPERR_MiscErr;

    memset(slot, 0, sizeof(*slot));
    slot->in_use = true;
    slot->vol_id = vol_id;
    slot->creator = creator;
    slot->file_type = file_type;
    slot->icon_type = icon_type;
    slot->icon_size = icon_size;
    if (icon_size > 0)
        memcpy(slot->icon_data, icon_data, icon_size);

    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPAddIcon: dt=0x%04X creator=0x%08X type=0x%08X icontype=%u size=%u dataAvail=%d", dt_ref, creator,
        file_type, icon_type, icon_size, icon_data_len);
    return AFPERR_NoErr;
}

// FPLogout (AFP 0x14) - Close session and all open forks
static uint32_t afp_cmd_logout(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)in;
    (void)in_len;
    (void)out;
    (void)out_max;
    // Close all open forks
    for (int i = 0; i < 16; i++) {
        if (g_forks[i].in_use)
            close_fork(&g_forks[i]);
    }
    if (out_len)
        *out_len = 0;
    LOG(1, "AFP FPLogout");
    return AFPERR_NoErr;
}

// FPLoginCont (AFP 0x13) - Continue multi-step login (stub)
static uint32_t afp_cmd_login_cont(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)in;
    (void)in_len;
    (void)out;
    (void)out_max;
    if (out_len)
        *out_len = 0;
    // No multi-step UAM supported
    LOG(10, "AFP FPLoginCont: rejected (no multi-step UAM)");
    return AFPERR_ParamErr;
}

// FPByteRangeLock (AFP 0x01) - Lock/unlock byte range (single-user: always succeed)
static uint32_t afp_cmd_byte_range_lock(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    if (in_len < 11 || out_max < 4)
        return AFPERR_ParamErr;
    int pos = 0;
    uint8_t flag = in[pos++]; // bit 0 = start/end, bit 7 = lock/unlock
    uint16_t fork_ref = rd16be(in + pos);
    pos += 2;
    uint32_t offset = rd32be(in + pos);
    pos += 4;
    uint32_t length = rd32be(in + pos);
    pos += 4;

    fork_t *fk = find_fork(fork_ref);
    if (!fk)
        return AFPERR_ParamErr;

    // Single-user: always succeed; reply with RangeStart
    wr32be(out, offset);
    if (out_len)
        *out_len = 4;
    LOG(10, "AFP FPByteRangeLock: ref=0x%04X flag=0x%02X offset=%u len=%u", fork_ref, flag, offset, length);
    return AFPERR_NoErr;
}

// FPMapID (AFP 0x15) - Map user/group ID to name
static uint32_t afp_cmd_map_id(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    if (in_len < 5)
        return AFPERR_ParamErr;
    uint8_t subfunc = in[0];
    uint32_t id = rd32be(in + 1);
    const char *name;
    if (id == 0) {
        name = "";
    } else if (subfunc == 1) {
        name = "guest"; // user ID -> name
    } else {
        name = "staff"; // group ID -> name
    }
    uint8_t name_len = (uint8_t)strlen(name);
    if (out_max < 1 + (int)name_len)
        return AFPERR_ParamErr;
    // Reply: Pascal string
    out[0] = name_len;
    if (name_len)
        memcpy(out + 1, name, name_len);
    if (out_len)
        *out_len = 1 + (int)name_len;
    LOG(10, "AFP FPMapID: subfunc=%u id=%u → '%s'", subfunc, id, name);
    return AFPERR_NoErr;
}

// FPMapName (AFP 0x16) - Map name to user/group ID
static uint32_t afp_cmd_map_name(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    if (in_len < 1 || out_max < 4)
        return AFPERR_ParamErr;
    uint8_t subfunc = in[0];
    (void)subfunc;
    // All names map to ID 0
    wr32be(out, 0);
    if (out_len)
        *out_len = 4;
    LOG(10, "AFP FPMapName: subfunc=%u → id=0", subfunc);
    return AFPERR_NoErr;
}

// FPGetUserInfo (AFP 0x25) - Get current user info
static uint32_t afp_cmd_get_user_info(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    if (in_len < 5 || out_max < 6)
        return AFPERR_ParamErr;
    uint8_t flag = in[0];
    uint32_t user_id = rd32be(in + 1);
    uint16_t bitmap = (in_len >= 7) ? rd16be(in + 5) : 0x0003;
    int p = 0;
    // Reply: Bitmap(2) + UserID(4) + PrimaryGroupID(4) - based on bitmap
    wr16be(out + p, bitmap);
    p += 2;
    if (bitmap & 0x0001) { // bit 0 = GetUserID
        wr32be(out + p, 0);
        p += 4;
    }
    if (bitmap & 0x0002) { // bit 1 = GetPrimaryGroupID
        wr32be(out + p, 0);
        p += 4;
    }
    if (out_len)
        *out_len = p;
    LOG(10, "AFP FPGetUserInfo: flag=0x%02X userId=%u bitmap=0x%04X reply=%d", flag, user_id, bitmap, p);
    return AFPERR_NoErr;
}

// FPWrite (AFP 0x21) - Write data to an open fork
static uint32_t afp_cmd_write(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    // Params: Flag(1) + OForkRefNum(2) + Offset(4) + ReqCount(4) = 11 bytes
    if (in_len < 11)
        return AFPERR_ParamErr;
    int pos = 0;
    uint8_t flag = in[pos++]; // bit 7 = EndRelative
    bool from_end = (flag & 0x80) != 0;
    uint16_t fork_ref = rd16be(in + pos);
    pos += 2;
    uint32_t offset = rd32be(in + pos);
    pos += 4;
    uint32_t req_count = rd32be(in + pos);
    pos += 4;

    LOG(10, "AFP FPWrite: ref=0x%04X flag=0x%02X off=%u req=%u fromEnd=%d dataLen=%d", fork_ref, flag, offset,
        req_count, from_end ? 1 : 0, in_len - 11);

    fork_t *fk = find_fork(fork_ref);
    if (!fk)
        return AFPERR_ParamErr;

    // Write payload follows the 11-byte param header
    const uint8_t *write_data = in + 11;
    int write_len = in_len - 11;
    if (write_len < 0)
        write_len = 0;
    if ((uint32_t)write_len > req_count)
        write_len = (int)req_count;

    // Seek to position
    if (from_end) {
        fseek(fk->f, 0, SEEK_END);
        long end_pos = ftell(fk->f);
        fseek(fk->f, end_pos + (long)offset, SEEK_SET);
    } else {
        fseek(fk->f, (long)offset, SEEK_SET);
    }

    size_t written = 0;
    if (write_len > 0)
        written = fwrite(write_data, 1, (size_t)write_len, fk->f);

    // Reply: LastWritten offset (4 bytes)
    if (out_max < 4)
        return AFPERR_ParamErr;
    uint32_t last_written = offset + (uint32_t)written;
    wr32be(out, last_written);
    if (out_len)
        *out_len = 4;
    LOG(2, "AFP FPWrite: ref=0x%04X off=%u req=%u wrote=%zu", fork_ref, offset, req_count, written);
    return AFPERR_NoErr;
}

// FPCreateDir (AFP 0x06) - Create a new directory
static uint32_t afp_cmd_create_dir(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    if (in_len < 8 || out_max < 4)
        return AFPERR_ParamErr;
    int pos = 0;
    pos++; // pad
    uint16_t vol_id = rd16be(in + pos);
    pos += 2;
    uint32_t dir_id = rd32be(in + pos);
    pos += 4;
    uint8_t path_type = (pos < in_len) ? in[pos++] : 0;
    (void)path_type;
    char path[AFP_MAX_NAME];
    if ((pos = afp_read_pstring(in, in_len, pos, path, sizeof(path))) < 0)
        return AFPERR_ParamErr;

    vol_t *vol = resolve_vol_by_id(vol_id);
    if (!vol)
        return AFPERR_ObjectNotFound;
    catalog_entry_t *base = afp_catalog_resolve(vol, dir_id);
    if (!base)
        return AFPERR_DirNotFound;

    char target_rel[AFP_MAX_REL_PATH];
    if (!afp_normalize_relative_path(base->rel_path, path, target_rel, sizeof(target_rel)))
        return AFPERR_ParamErr;

    char full[PATH_MAX];
    if (!afp_full_path(vol, target_rel, full, sizeof(full)))
        return AFPERR_ParamErr;

    struct stat st;
    if (stat(full, &st) == 0)
        return AFPERR_ObjectExists;

    if (mkdir(full, 0755) != 0)
        return AFPERR_AccessDenied;

    catalog_entry_t *entry = afp_catalog_insert(vol, target_rel, true);
    if (!entry)
        return AFPERR_MiscErr;

    // Reply: NewDirID (4)
    wr32be(out, entry->cnid);
    if (out_len)
        *out_len = 4;
    LOG(10, "AFP FPCreateDir: vol=0x%04X dir=0x%08X path='%s' → cnid=0x%08X", vol_id, (unsigned)dir_id, target_rel,
        entry->cnid);
    return AFPERR_NoErr;
}

// FPCreateFile (AFP 0x07) - Create a new file
static uint32_t afp_cmd_create_file(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)out;
    (void)out_max;
    if (in_len < 8)
        return AFPERR_ParamErr;
    int pos = 0;
    uint8_t flag = in[pos++]; // bit 0 = soft/hard create
    bool hard_create = (flag & 0x01) != 0;
    uint16_t vol_id = rd16be(in + pos);
    pos += 2;
    uint32_t dir_id = rd32be(in + pos);
    pos += 4;
    uint8_t path_type = (pos < in_len) ? in[pos++] : 0;
    (void)path_type;
    char path[AFP_MAX_NAME];
    if ((pos = afp_read_pstring(in, in_len, pos, path, sizeof(path))) < 0)
        return AFPERR_ParamErr;

    vol_t *vol = resolve_vol_by_id(vol_id);
    if (!vol)
        return AFPERR_ObjectNotFound;
    catalog_entry_t *base = afp_catalog_resolve(vol, dir_id);
    if (!base)
        return AFPERR_DirNotFound;

    char target_rel[AFP_MAX_REL_PATH];
    if (!afp_normalize_relative_path(base->rel_path, path, target_rel, sizeof(target_rel)))
        return AFPERR_ParamErr;

    char full[PATH_MAX];
    if (!afp_full_path(vol, target_rel, full, sizeof(full)))
        return AFPERR_ParamErr;

    struct stat st;
    if (stat(full, &st) == 0) {
        if (!hard_create)
            return AFPERR_ObjectExists;
        // Hard create: check if file is busy (open fork)
        for (int i = 0; i < 16; i++) {
            if (g_forks[i].in_use && strcmp(g_forks[i].path, full) == 0)
                return AFPERR_FileBusy;
        }
        // Truncate existing file and remove companion resource fork
        FILE *f = fopen(full, "wb");
        if (!f)
            return AFPERR_AccessDenied;
        fclose(f);
        char rsrc_path[PATH_MAX];
        snprintf(rsrc_path, sizeof(rsrc_path), "%s.rsrc", full);
        unlink(rsrc_path);
    } else {
        // Create new file
        FILE *f = fopen(full, "wb");
        if (!f)
            return AFPERR_AccessDenied;
        fclose(f);
    }

    // Ensure catalog entry for the file
    catalog_entry_t *entry = afp_catalog_find_by_path(vol, target_rel);
    if (!entry)
        afp_catalog_insert(vol, target_rel, false);

    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPCreateFile: vol=0x%04X dir=0x%08X path='%s' hard=%d", vol_id, (unsigned)dir_id, target_rel,
        hard_create ? 1 : 0);
    return AFPERR_NoErr;
}

// FPDelete (AFP 0x08) - Delete a file or directory
static uint32_t afp_cmd_delete(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)out;
    (void)out_max;
    if (in_len < 8)
        return AFPERR_ParamErr;
    int pos = 0;
    pos++; // pad
    uint16_t vol_id = rd16be(in + pos);
    pos += 2;
    uint32_t dir_id = rd32be(in + pos);
    pos += 4;
    uint8_t path_type = (pos < in_len) ? in[pos++] : 0;
    (void)path_type;
    char path[AFP_MAX_NAME];
    if ((pos = afp_read_pstring(in, in_len, pos, path, sizeof(path))) < 0)
        return AFPERR_ParamErr;

    vol_t *vol = resolve_vol_by_id(vol_id);
    if (!vol)
        return AFPERR_ObjectNotFound;
    catalog_entry_t *base = afp_catalog_resolve(vol, dir_id);
    if (!base)
        return AFPERR_DirNotFound;

    char target_rel[AFP_MAX_REL_PATH];
    if (!afp_normalize_relative_path(base->rel_path, path, target_rel, sizeof(target_rel)))
        return AFPERR_ParamErr;

    char full[PATH_MAX];
    if (!afp_full_path(vol, target_rel, full, sizeof(full)))
        return AFPERR_ParamErr;

    struct stat st;
    if (stat(full, &st) != 0)
        return AFPERR_ObjectNotFound;

    if (S_ISDIR(st.st_mode)) {
        // Check directory is empty
        DIR *dir = opendir(full);
        if (!dir)
            return AFPERR_AccessDenied;
        bool empty = true;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            empty = false;
            break;
        }
        closedir(dir);
        if (!empty)
            return AFPERR_DirNotEmpty;
        if (rmdir(full) != 0)
            return AFPERR_AccessDenied;
    } else {
        // Check file is not open
        for (int i = 0; i < 16; i++) {
            if (g_forks[i].in_use && strcmp(g_forks[i].path, full) == 0)
                return AFPERR_FileBusy;
        }
        if (unlink(full) != 0)
            return AFPERR_AccessDenied;
        // Remove companion resource fork file if it exists
        char rsrc_path[PATH_MAX];
        snprintf(rsrc_path, sizeof(rsrc_path), "%s.rsrc", full);
        unlink(rsrc_path);
    }

    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPDelete: vol=0x%04X dir=0x%08X path='%s' type=%s", vol_id, (unsigned)dir_id, target_rel,
        S_ISDIR(st.st_mode) ? "dir" : "file");
    return AFPERR_NoErr;
}

// FPRename (AFP 0x1C) - Rename a file or directory
static uint32_t afp_cmd_rename(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)out;
    (void)out_max;
    if (in_len < 8)
        return AFPERR_ParamErr;
    int pos = 0;
    pos++; // pad
    uint16_t vol_id = rd16be(in + pos);
    pos += 2;
    uint32_t dir_id = rd32be(in + pos);
    pos += 4;
    uint8_t path_type = (pos < in_len) ? in[pos++] : 0;
    (void)path_type;
    char old_name[AFP_MAX_NAME];
    if ((pos = afp_read_pstring(in, in_len, pos, old_name, sizeof(old_name))) < 0)
        return AFPERR_ParamErr;
    uint8_t new_path_type = (pos < in_len) ? in[pos++] : 0;
    (void)new_path_type;
    char new_name[AFP_MAX_NAME];
    if ((pos = afp_read_pstring(in, in_len, pos, new_name, sizeof(new_name))) < 0)
        return AFPERR_ParamErr;

    vol_t *vol = resolve_vol_by_id(vol_id);
    if (!vol)
        return AFPERR_ObjectNotFound;
    catalog_entry_t *base = afp_catalog_resolve(vol, dir_id);
    if (!base)
        return AFPERR_DirNotFound;

    // Build old full path
    char old_rel[AFP_MAX_REL_PATH];
    if (!afp_normalize_relative_path(base->rel_path, old_name, old_rel, sizeof(old_rel)))
        return AFPERR_ParamErr;
    char old_full[PATH_MAX];
    if (!afp_full_path(vol, old_rel, old_full, sizeof(old_full)))
        return AFPERR_ParamErr;

    // Build new full path (same parent directory, new name)
    char parent_rel[AFP_MAX_REL_PATH];
    afp_extract_parent(old_rel, parent_rel, sizeof(parent_rel));
    char new_rel[AFP_MAX_REL_PATH];
    if (!afp_build_child_path(parent_rel, new_name, new_rel, sizeof(new_rel)))
        return AFPERR_ParamErr;
    char new_full[PATH_MAX];
    if (!afp_full_path(vol, new_rel, new_full, sizeof(new_full)))
        return AFPERR_ParamErr;

    if (rename(old_full, new_full) != 0)
        return AFPERR_CantRename;

    // Rename companion resource fork file if it exists
    char old_rsrc[PATH_MAX], new_rsrc[PATH_MAX];
    snprintf(old_rsrc, sizeof(old_rsrc), "%s.rsrc", old_full);
    snprintf(new_rsrc, sizeof(new_rsrc), "%s.rsrc", new_full);
    rename(old_rsrc, new_rsrc);

    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPRename: vol=0x%04X dir=0x%08X '%s' → '%s'", vol_id, (unsigned)dir_id, old_rel, new_rel);
    return AFPERR_NoErr;
}

// FPMoveAndRename (AFP 0x17) - Move and/or rename a file or directory
static uint32_t afp_cmd_move_and_rename(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)out;
    (void)out_max;
    if (in_len < 12)
        return AFPERR_ParamErr;
    int pos = 0;
    pos++; // pad
    uint16_t vol_id = rd16be(in + pos);
    pos += 2;
    uint32_t src_dir_id = rd32be(in + pos);
    pos += 4;
    uint32_t dst_dir_id = rd32be(in + pos);
    pos += 4;
    uint8_t path_type = (pos < in_len) ? in[pos++] : 0;
    (void)path_type;
    char src_path[AFP_MAX_NAME];
    if ((pos = afp_read_pstring(in, in_len, pos, src_path, sizeof(src_path))) < 0)
        return AFPERR_ParamErr;
    uint8_t dst_path_type = (pos < in_len) ? in[pos++] : 0;
    (void)dst_path_type;
    char dst_path[AFP_MAX_NAME];
    if ((pos = afp_read_pstring(in, in_len, pos, dst_path, sizeof(dst_path))) < 0)
        return AFPERR_ParamErr;
    // Optional new name
    uint8_t new_name_type = (pos < in_len) ? in[pos++] : 0;
    (void)new_name_type;
    char new_name[AFP_MAX_NAME];
    new_name[0] = '\0';
    if (pos < in_len)
        afp_read_pstring(in, in_len, pos, new_name, sizeof(new_name));

    vol_t *vol = resolve_vol_by_id(vol_id);
    if (!vol)
        return AFPERR_ObjectNotFound;

    // Resolve source
    catalog_entry_t *src_base = afp_catalog_resolve(vol, src_dir_id);
    if (!src_base)
        return AFPERR_DirNotFound;
    char src_rel[AFP_MAX_REL_PATH];
    if (!afp_normalize_relative_path(src_base->rel_path, src_path, src_rel, sizeof(src_rel)))
        return AFPERR_ParamErr;
    char src_full[PATH_MAX];
    if (!afp_full_path(vol, src_rel, src_full, sizeof(src_full)))
        return AFPERR_ParamErr;

    // Resolve destination directory
    catalog_entry_t *dst_base = afp_catalog_resolve(vol, dst_dir_id);
    if (!dst_base)
        return AFPERR_DirNotFound;
    char dst_dir_rel[AFP_MAX_REL_PATH];
    if (!afp_normalize_relative_path(dst_base->rel_path, dst_path, dst_dir_rel, sizeof(dst_dir_rel)))
        return AFPERR_ParamErr;

    // Determine final name
    const char *final_name = (new_name[0] != '\0') ? new_name : afp_last_component(src_rel);
    if (!final_name)
        return AFPERR_ParamErr;

    char dst_rel[AFP_MAX_REL_PATH];
    if (!afp_build_child_path(dst_dir_rel, final_name, dst_rel, sizeof(dst_rel)))
        return AFPERR_ParamErr;
    char dst_full[PATH_MAX];
    if (!afp_full_path(vol, dst_rel, dst_full, sizeof(dst_full)))
        return AFPERR_ParamErr;

    if (rename(src_full, dst_full) != 0)
        return AFPERR_CantMove;

    // Move companion resource fork file if it exists
    char src_rsrc[PATH_MAX], dst_rsrc[PATH_MAX];
    snprintf(src_rsrc, sizeof(src_rsrc), "%s.rsrc", src_full);
    snprintf(dst_rsrc, sizeof(dst_rsrc), "%s.rsrc", dst_full);
    rename(src_rsrc, dst_rsrc);

    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPMoveAndRename: vol=0x%04X srcDir=0x%08X dstDir=0x%08X '%s' → '%s'", vol_id, (unsigned)src_dir_id,
        (unsigned)dst_dir_id, src_rel, dst_rel);
    return AFPERR_NoErr;
}

// FPCopyFile (AFP 0x05) - Copy a file
static uint32_t afp_cmd_copy_file(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)out;
    (void)out_max;
    if (in_len < 14)
        return AFPERR_ParamErr;
    int pos = 0;
    pos++; // pad
    uint16_t src_vol = rd16be(in + pos);
    pos += 2;
    uint16_t dst_vol = rd16be(in + pos);
    pos += 2;
    uint32_t src_dir = rd32be(in + pos);
    pos += 4;
    uint32_t dst_dir = rd32be(in + pos);
    pos += 4;
    uint8_t path_type = (pos < in_len) ? in[pos++] : 0;
    (void)path_type;
    char src_name[AFP_MAX_NAME];
    if ((pos = afp_read_pstring(in, in_len, pos, src_name, sizeof(src_name))) < 0)
        return AFPERR_ParamErr;
    uint8_t dst_path_type = (pos < in_len) ? in[pos++] : 0;
    (void)dst_path_type;
    char dst_name[AFP_MAX_NAME];
    if ((pos = afp_read_pstring(in, in_len, pos, dst_name, sizeof(dst_name))) < 0)
        return AFPERR_ParamErr;
    // Optional new name
    uint8_t new_name_type = (pos < in_len) ? in[pos++] : 0;
    (void)new_name_type;
    char new_name[AFP_MAX_NAME];
    new_name[0] = '\0';
    if (pos < in_len)
        afp_read_pstring(in, in_len, pos, new_name, sizeof(new_name));

    vol_t *svol = resolve_vol_by_id(src_vol);
    if (!svol)
        return AFPERR_ObjectNotFound;
    vol_t *dvol = resolve_vol_by_id(dst_vol);
    if (!dvol)
        return AFPERR_ObjectNotFound;

    // Resolve source
    catalog_entry_t *src_base = afp_catalog_resolve(svol, src_dir);
    if (!src_base)
        return AFPERR_DirNotFound;
    char src_rel[AFP_MAX_REL_PATH];
    if (!afp_normalize_relative_path(src_base->rel_path, src_name, src_rel, sizeof(src_rel)))
        return AFPERR_ParamErr;
    char src_full[PATH_MAX];
    if (!afp_full_path(svol, src_rel, src_full, sizeof(src_full)))
        return AFPERR_ParamErr;

    // Resolve destination
    catalog_entry_t *dst_base = afp_catalog_resolve(dvol, dst_dir);
    if (!dst_base)
        return AFPERR_DirNotFound;
    char dst_dir_rel[AFP_MAX_REL_PATH];
    if (!afp_normalize_relative_path(dst_base->rel_path, dst_name, dst_dir_rel, sizeof(dst_dir_rel)))
        return AFPERR_ParamErr;

    const char *final_name = (new_name[0] != '\0') ? new_name : afp_last_component(src_rel);
    if (!final_name)
        return AFPERR_ParamErr;
    char dst_rel[AFP_MAX_REL_PATH];
    if (!afp_build_child_path(dst_dir_rel, final_name, dst_rel, sizeof(dst_rel)))
        return AFPERR_ParamErr;
    char dst_full[PATH_MAX];
    if (!afp_full_path(dvol, dst_rel, dst_full, sizeof(dst_full)))
        return AFPERR_ParamErr;

    // Copy file data
    FILE *fin = fopen(src_full, "rb");
    if (!fin)
        return AFPERR_ObjectNotFound;
    FILE *fout = fopen(dst_full, "wb");
    if (!fout) {
        fclose(fin);
        return AFPERR_AccessDenied;
    }
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fin)) > 0) {
        if (fwrite(buf, 1, n, fout) != n) {
            fclose(fin);
            fclose(fout);
            return AFPERR_DiskFull;
        }
    }
    fclose(fin);
    fclose(fout);

    // Copy companion resource fork file if it exists
    char src_rsrc[PATH_MAX], dst_rsrc[PATH_MAX];
    snprintf(src_rsrc, sizeof(src_rsrc), "%s.rsrc", src_full);
    snprintf(dst_rsrc, sizeof(dst_rsrc), "%s.rsrc", dst_full);
    FILE *rfin = fopen(src_rsrc, "rb");
    if (rfin) {
        FILE *rfout = fopen(dst_rsrc, "wb");
        if (rfout) {
            while ((n = fread(buf, 1, sizeof(buf), rfin)) > 0)
                fwrite(buf, 1, n, rfout);
            fclose(rfout);
        }
        fclose(rfin);
    }

    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPCopyFile: srcVol=0x%04X dstVol=0x%04X '%s' → '%s'", src_vol, dst_vol, src_rel, dst_rel);
    return AFPERR_NoErr;
}

// FPChangePassword (AFP 0x24) - Stub: not supported
static uint32_t afp_cmd_change_password(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)in;
    (void)in_len;
    (void)out;
    (void)out_max;
    if (out_len)
        *out_len = 0;
    LOG(10, "AFP FPChangePassword: rejected (not supported)");
    return AFPERR_CallNotSupported;
}

static uint32_t afp_cmd_enumerate(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    uint32_t result = AFPERR_NoErr;
    int produced = afp_enumerate(in, in_len, out, out_max, &result);
    if (result == AFPERR_NoErr && out_len)
        *out_len = produced;
    return result;
}

static uint32_t afp_cmd_get_file_dir_parms(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    if (in_len < 11)
        return AFPERR_ParamErr;
    afp_log_hex("AFP FPGetFileDirParms req", in, in_len);
    int pos = 0;
    pos++; // pad
    uint16_t vol_id = rd16be(in + pos);
    pos += 2;
    uint32_t dir_id = rd32be(in + pos);
    pos += 4;
    uint16_t file_bm = rd16be(in + pos);
    pos += 2;
    uint16_t dir_bm = rd16be(in + pos);
    pos += 2;
    uint8_t path_type = (pos < in_len) ? in[pos++] : 0;
    (void)path_type;
    char path[AFP_MAX_NAME];
    if ((pos = afp_read_pstring(in, in_len, pos, path, sizeof(path))) < 0)
        return AFPERR_ParamErr;
    LOG(10, "AFP FPGetFileDirParms: vol=0x%04X dir=0x%08X fileBm=0x%04X dirBm=0x%04X pathType=%u path='%s'", vol_id,
        (unsigned)dir_id, file_bm, dir_bm, path_type, path[0] ? path : "<root>");
    if (file_bm == 0 && dir_bm == 0)
        return AFPERR_BitmapErr;

    vol_t *vol = resolve_vol_by_id(vol_id);
    if (!vol)
        return AFPERR_ObjectNotFound;
    catalog_entry_t *base = afp_catalog_resolve(vol, dir_id);
    if (!base)
        return AFPERR_DirNotFound;

    char target_rel[AFP_MAX_REL_PATH];
    if (!afp_normalize_relative_path(base->rel_path, path, target_rel, sizeof(target_rel)))
        return AFPERR_ParamErr;
    struct stat st;
    if (!afp_stat_path(vol, target_rel, &st))
        return AFPERR_ObjectNotFound;
    bool is_dir = S_ISDIR(st.st_mode);
    uint16_t selected_bm = is_dir ? dir_bm : file_bm;
    const char *name = NULL;
    catalog_entry_t *dir_entry = NULL;
    if (is_dir) {
        dir_entry = afp_catalog_ensure(vol, target_rel);
        if (!dir_entry)
            return AFPERR_MiscErr;
    }
    uint32_t this_cnid = is_dir ? dir_entry->cnid : afp_hash_path(target_rel);
    uint32_t parent_cnid = afp_parent_cnid(vol, target_rel);
    if (target_rel[0] == '\0')
        name = vol->name;
    else
        name = afp_last_component(target_rel);
    if (!name)
        name = vol->name;
    LOG(10, "AFP FPGetFileDirParms resolve: path='%s' is_dir=%d parent=0x%08X cnid=0x%08X size=%" PRIu64 " mode=%o",
        target_rel[0] ? target_rel : "<root>", is_dir ? 1 : 0, parent_cnid, this_cnid, (uint64_t)st.st_size,
        st.st_mode);

    if (out_max < 6)
        return AFPERR_ParamErr;
    wr16be(out + 0, file_bm);
    wr16be(out + 2, dir_bm);
    out[4] = is_dir ? 0x80 : 0x00;
    out[5] = 0x00;

    int pbase = 6;
    int pos_long_off = -1;
    int pos_short_off = -1;
    int p = pbase;
    if (selected_bm) {
        p = afp_write_param_area(is_dir, selected_bm, out, pbase, out_max, &pos_long_off, &pos_short_off);
        if (p < 0)
            return AFPERR_ParamErr;
        if (!afp_populate_param_area(is_dir, vol, target_rel, &st, selected_bm, out, pbase))
            return AFPERR_ParamErr;
    }
    int vpos = p;
    uint8_t long_len = (uint8_t)(strlen(name) > 255 ? 255 : strlen(name));
    uint8_t short_len = (uint8_t)(strlen(name) > 31 ? 31 : strlen(name));
    vpos = afp_write_name_vars(out, vpos, out_max, pbase, name, selected_bm, pos_long_off, pos_short_off, long_len,
                               short_len);
    if (vpos < 0)
        return AFPERR_ParamErr;
    if ((vpos % 2) && vpos < out_max)
        out[vpos++] = 0x00;
    if (out_len)
        *out_len = vpos;
    afp_log_hex("AFP FPGetFileDirParms resp", out, vpos);
    LOG(2, "AFP FPGetFileDirParms: vol=0x%04X dir=0x%08X type=%s path='%s' reply=%d", vol_id, (unsigned)dir_id,
        is_dir ? "dir" : "file", target_rel[0] ? target_rel : "<root>", vpos);
    return AFPERR_NoErr;
}

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
    for (size_t i = 0; i < ARRAY_LEN(k_afp_command_handlers); i++) {
        if (k_afp_command_handlers[i].opcode == opcode)
            return &k_afp_command_handlers[i];
    }
    return NULL;
}

// FPEnumerate - Directory listing (AFP 0x09)
static int afp_enumerate(const uint8_t *in, int in_len, uint8_t *out, int out_max, uint32_t *result_code) {
    if (result_code)
        *result_code = AFPERR_NoErr;
    if (in_len < 18)
        return 0;
    afp_log_hex("AFP FPEnumerate req", in, in_len);
    int pos = 0;
    pos++; // pad
    uint16_t vol_id = rd16be(in + pos);
    pos += 2;
    uint32_t dir_id = rd32be(in + pos);
    pos += 4;
    uint16_t file_bm = rd16be(in + pos);
    pos += 2;
    uint16_t dir_bm = rd16be(in + pos);
    pos += 2;
    uint16_t req_count = rd16be(in + pos);
    pos += 2;
    uint16_t start_index = rd16be(in + pos);
    pos += 2;
    if (start_index == 0)
        start_index = 1;
    uint16_t max_reply = rd16be(in + pos);
    pos += 2;
    uint8_t path_type = (pos < in_len) ? in[pos++] : 0;
    (void)path_type;
    char rel_path[AFP_MAX_NAME];
    if ((pos = afp_read_pstring(in, in_len, pos, rel_path, sizeof(rel_path))) < 0)
        return 0;
    if (file_bm == 0 && dir_bm == 0) {
        if (result_code)
            *result_code = AFPERR_BitmapErr;
        return 0;
    }

    vol_t *vol = resolve_vol_by_id(vol_id);
    if (!vol) {
        if (result_code)
            *result_code = AFPERR_ObjectNotFound;
        return 0;
    }
    catalog_entry_t *base = afp_catalog_resolve(vol, dir_id);
    if (!base) {
        if (result_code)
            *result_code = AFPERR_DirNotFound;
        return 0;
    }

    char target_rel[AFP_MAX_REL_PATH];
    if (!afp_normalize_relative_path(base->rel_path, rel_path, target_rel, sizeof(target_rel))) {
        if (result_code)
            *result_code = AFPERR_ParamErr;
        return 0;
    }
    struct stat dir_st;
    if (!afp_stat_path(vol, target_rel, &dir_st) || !S_ISDIR(dir_st.st_mode)) {
        if (result_code)
            *result_code = AFPERR_ObjectTypeErr;
        return 0;
    }

    char full_dir[PATH_MAX];
    if (!afp_full_path(vol, target_rel, full_dir, sizeof(full_dir))) {
        if (result_code)
            *result_code = AFPERR_ParamErr;
        return 0;
    }

    DIR *dir = opendir(full_dir);
    if (!dir) {
        if (result_code)
            *result_code = AFPERR_ObjectNotFound;
        return 0;
    }

    typedef struct {
        char name[AFP_MAX_NAME];
        bool is_dir;
        char rel[AFP_MAX_REL_PATH];
        struct stat st;
    } enum_entry_t;
    enum_entry_t entries[AFP_ENUM_MAX_ENTRIES];
    int entry_count = 0;
    struct dirent *dent;
    while ((dent = readdir(dir)) != NULL) {
        if (strcmp(dent->d_name, ".") == 0 || strcmp(dent->d_name, "..") == 0)
            continue;
        char child_full[PATH_MAX];
        if (snprintf(child_full, sizeof(child_full), "%s/%s", full_dir, dent->d_name) >= (int)sizeof(child_full))
            continue;
        struct stat child_st;
        if (stat(child_full, &child_st) != 0)
            continue;
        bool child_is_dir = S_ISDIR(child_st.st_mode);
        if ((child_is_dir && dir_bm == 0) || (!child_is_dir && file_bm == 0))
            continue;
        if (entry_count >= AFP_ENUM_MAX_ENTRIES)
            break;
        enum_entry_t *entry = &entries[entry_count++];
        strncpy(entry->name, dent->d_name, sizeof(entry->name) - 1);
        entry->name[sizeof(entry->name) - 1] = '\0';
        entry->is_dir = child_is_dir;
        entry->st = child_st;
        if (!afp_build_child_path(target_rel, entry->name, entry->rel, sizeof(entry->rel))) {
            entry_count--;
            continue;
        }
        if (entry->is_dir)
            afp_catalog_ensure(vol, entry->rel);
    }
    closedir(dir);

    if (entry_count == 0) {
        if (result_code)
            *result_code = AFPERR_ObjectNotFound;
        return 0;
    }

    if (start_index > (uint16_t)entry_count) {
        if (result_code)
            *result_code = AFPERR_ObjectNotFound;
        return 0;
    }

    int max_bytes = max_reply ? (int)max_reply : out_max;
    if (max_bytes > out_max)
        max_bytes = out_max;
    if (max_bytes < 6) {
        if (result_code)
            *result_code = AFPERR_ParamErr;
        return 0;
    }

    wr16be(out + 0, file_bm);
    wr16be(out + 2, dir_bm);
    wr16be(out + 4, 0);
    int w = 6;
    uint16_t act = 0;
    uint16_t left = req_count;
    if (left == 0)
        left = UINT16_MAX;
    int idx = (int)start_index - 1;

    for (int i = idx; i < entry_count && left > 0; i++) {
        enum_entry_t *entry = &entries[i];
        uint16_t bm = entry->is_dir ? dir_bm : file_bm;
        if (bm == 0)
            continue;
        if (w + 3 >= max_bytes)
            break;
        uint8_t long_len = (uint8_t)(strlen(entry->name) > 255 ? 255 : strlen(entry->name));
        uint8_t short_len = (uint8_t)(strlen(entry->name) > 31 ? 31 : strlen(entry->name));
        int header = w;
        int struct_header_len = 2;
        if (header + struct_header_len > max_bytes)
            break;
        out[header] = 0; // struct length placeholder (1 byte)
        out[header + 1] = entry->is_dir ? 0x80 : 0x00;
        int pbase = header + struct_header_len;
        int pos_long_off = -1;
        int pos_short_off = -1;
        int p = afp_write_param_area(entry->is_dir, bm, out, pbase, max_bytes, &pos_long_off, &pos_short_off);
        if (p < 0)
            break;
        if (p > max_bytes)
            break;
        if (!afp_populate_param_area(entry->is_dir, vol, entry->rel, &entry->st, bm, out, pbase))
            break;
        int vpos = afp_write_name_vars(out, p, out_max, pbase, entry->name, bm, pos_long_off, pos_short_off, long_len,
                                       short_len);
        if (vpos < 0)
            break;
        if (vpos > max_bytes)
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
            break;
        out[header] = (uint8_t)struct_len;
        w = vpos;
        act++;
        left--;
    }

    wr16be(out + 4, act);
    if (act == 0) {
        if (result_code)
            *result_code = (idx >= entry_count) ? AFPERR_ObjectNotFound : AFPERR_NoErr;
    }
    LOG(10,
        "AFP FPEnumerate: vol=0x%04X dir=0x%08X path='%s' fileBm=0x%04X dirBm=0x%04X start=%u req=%u returned=%u "
        "total=%d",
        vol_id, (unsigned)dir_id, target_rel[0] ? target_rel : "<root>", file_bm, dir_bm, (unsigned)start_index,
        (unsigned)req_count, (unsigned)act, entry_count);
    afp_log_hex("AFP FPEnumerate resp", out, w);
    return w;
}

uint32_t afp_handle_command(uint8_t opcode, const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    if (out_len)
        *out_len = 0;
    const afp_command_handler_t *handler = afp_find_handler(opcode);
    if (!handler) {
        LOG(1, "AFP unknown opcode 0x%02X (len=%d)", opcode, in_len);
        return AFPERR_CallNotSupported;
    }
    LOG(10, "AFP >> %s (0x%02X) in_len=%d", handler->name, opcode, in_len);
    uint32_t result = handler->handler(in, in_len, out, out_max, out_len);
    int reply_len = (out_len ? *out_len : 0);
    if (result == AFPERR_NoErr)
        LOG(3, "AFP << %s OK reply=%d", handler->name, reply_len);
    else
        LOG(3, "AFP << %s ERR=0x%08X reply=%d", handler->name, result, reply_len);
    return result;
}
