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
        printf("atalk: share-add requires <name> <path>\n");
        return -1;
    }
    if (strlen(name) > 32) {
        printf("atalk: share name max 32 chars\n");
        return -1;
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        printf("atalk: path '%s' not a directory\n", path);
        return -1;
    }
    if (find_share_index_by_name(name) >= 0) {
        printf("atalk: share '%s' already exists\n", name);
        return -1;
    }
    int idx = first_free_share_slot();
    if (idx < 0) {
        printf("atalk: share table full (max %d)\n", MAX_SHARES);
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
    printf("atalk: added share '%s' -> '%s' (vol %u)\n", g_shares[idx].name, g_shares[idx].path,
           (unsigned)g_shares[idx].vol_id);
    return 0;
}

int atalk_share_remove(const char *name) {
    int idx = find_share_index_by_name(name);
    if (idx < 0) {
        printf("atalk: no such share '%s'\n", name);
        return -1;
    }
    afp_release_volume(g_shares[idx].vol_id);
    g_shares[idx].in_use = false;
    g_shares[idx].name[0] = '\0';
    g_shares[idx].path[0] = '\0';
    g_shares[idx].vol_id = 0;
    printf("atalk: removed share '%s'\n", name);
    return 0;
}

int atalk_share_list(void) {
    printf("AppleTalk shares (%d max):\n", MAX_SHARES);
    for (int i = 0; i < MAX_SHARES; i++) {
        if (!g_shares[i].in_use)
            continue;
        printf("  - %s  (vol %u)  path=%s\n", g_shares[i].name, (unsigned)g_shares[i].vol_id, g_shares[i].path);
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

static catalog_entry_t *afp_catalog_insert(vol_t *v, const char *rel_path) {
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
    entry->is_dir = true;
    entry->cnid = (rel_path[0] == '\0') ? AFP_CNID_ROOT : v->next_cnid++;
    strncpy(entry->rel_path, rel_path, sizeof(entry->rel_path) - 1);
    return entry;
}

static catalog_entry_t *afp_catalog_ensure(vol_t *v, const char *rel_path) {
    if (!v)
        return NULL;
    if (!rel_path || !*rel_path)
        rel_path = "";
    catalog_entry_t *entry = afp_catalog_find_by_path(v, rel_path);
    if (entry)
        return entry;
    return afp_catalog_insert(v, rel_path);
}

static catalog_entry_t *afp_catalog_resolve(vol_t *v, uint32_t dir_id) {
    if (!v)
        return NULL;
    if (dir_id == 0 || dir_id == AFP_CNID_ROOT)
        return afp_catalog_ensure(v, "");
    return afp_catalog_find_by_cnid(v, dir_id);
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

static uint16_t afp_file_attributes_from_stat(const struct stat *st) {
    (void)st;
    return (uint16_t)(1u << 5); // WriteInhibit since the volume is read-only
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

static bool afp_populate_param_area(bool is_dir, vol_t *vol, const char *rel_path, const struct stat *st, uint16_t bm,
                                    uint8_t *out, int pbase) {
    if (!st)
        return false;
    int ptr;
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 5)) >= 0) {
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
            wr32be(out + ptr, 0);
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
typedef struct {
    bool in_use;
    uint16_t fork_ref;
    uint16_t vol_id;
    char path[512];
    FILE *f;
} fork_t;
static vol_t g_vols[8]; // mirror shares at time of open (max shares)
static fork_t g_forks[16];
static uint16_t g_next_fork_ref = 0x0042;

static void afp_release_volume(uint16_t vol_id) {
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

static fork_t *alloc_fork(uint16_t vol_id, const char *path) {
    for (int i = 0; i < 16; i++)
        if (!g_forks[i].in_use) {
            g_forks[i].in_use = true;
            g_forks[i].fork_ref = g_next_fork_ref++;
            g_forks[i].vol_id = vol_id;
            strncpy(g_forks[i].path, path, sizeof(g_forks[i].path) - 1);
            g_forks[i].path[sizeof(g_forks[i].path) - 1] = '\0';
            g_forks[i].f = fopen(path, "rb");
            if (!g_forks[i].f) {
                g_forks[i].in_use = false;
                return NULL;
            }
            return &g_forks[i];
        }
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
    LOG(1, "AFP FPOpenVol: bitmap=0x%04X name='%s'", bitmap, volName);

    vol_t *v = ensure_vol_by_name(volName);
    if (!v)
        return 0;
    afp_catalog_ensure(v, "");

    int written = afp_write_vol_param_block(v, &bitmap, out, out_max);
    if (written <= 0)
        return 0;
    uint32_t bytes_free = 12u * 1024u * 1024u;
    uint32_t bytes_total = 16u * 1024u * 1024u;
    LOG(1, "AFP FPOpenVol: Reply: bitmap=0x%04X volId=0x%04X bytesFree=%u bytesTotal=%u%s%s", bitmap, v->vol_id,
        (unsigned)bytes_free, (unsigned)bytes_total, (bitmap & 0x0100) ? " name='" : "",
        (bitmap & 0x0100) ? v->name : "");
    afp_log_hex("AFP FPOpenVol resp", out, written);
    return written;
}

// Compose a tiny FPGetSrvrInfo payload
static int afp_get_srvr_info(uint8_t *out, int max_len) {
    LOG(1, "AFP FPGetSrvrInfo");
    const char *info = "AFPServer:GrannySmith;UAM=NoUserAuth";
    int n = (int)strlen(info);
    if (n > max_len)
        n = max_len;
    memcpy(out, info, n);
    LOG(1, "AFP FPGetSrvrInfo: Reply: len=%d", n);
    return n;
}

// Compose FPGetSrvrParms payload
static int afp_get_srvr_parms(uint8_t *out, int max_len) {
    int count = 0;
    for (int i = 0; i < MAX_SHARES; i++)
        if (g_shares[i].in_use)
            count++;
    LOG(1, "AFP FPGetSrvrParms: shares=%d", count);
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

    LOG(1, "AFP FPGetSrvrParms: Reply: time=%u numvols=%d", (unsigned)t, count);
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
    if (in_len < 4 || out_max < 2)
        return AFPERR_ParamErr;
    uint16_t vol_id = rd16be(in + 0);
    uint16_t bitmap = rd16be(in + 2);
    vol_t *v = resolve_vol_by_id(vol_id);
    if (!v)
        return AFPERR_ObjectNotFound;
    int produced = afp_write_vol_param_block(v, &bitmap, out, out_max);
    if (produced <= 0)
        return AFPERR_ParamErr;
    LOG(1, "AFP FPGetVolParms: Reply: bitmap=0x%04X volId=0x%04X", bitmap, vol_id);
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
    LOG(1, "AFP FPCloseVol: volId=0x%04X", vol_id);
    LOG(1, "AFP FPCloseVol: Reply: <empty>");
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
    LOG(1, "AFP FPLogin: version='%s' uam='%s'", ver, uam);
    bool ver_ok = (strcmp(ver, "AFPVersion 2.0") == 0) || (strcmp(ver, "AFPVersion 2.1") == 0);
    bool uam_ok = (strcmp(uam, "No User Authent") == 0);
    if (!ver_ok) {
        LOG(1, "AFP FPLogin: unsupported version → BadVersNum");
        return AFPERR_BadVersNum;
    }
    if (!uam_ok) {
        LOG(1, "AFP FPLogin: unsupported UAM");
        return AFPERR_BadUAM;
    }
    if (out_max < 2)
        return AFPERR_ParamErr;
    wr16be(out, 0x0000);
    if (out_len)
        *out_len = 2;
    LOG(1, "AFP FPLogin: Reply: uamId=0x0000");
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
    LOG(1, "AFP FPOpenDir: vol=0x%04X parent=0x%08X path='%s' → cnid=0x%08X", vol_id, (unsigned)dir_id,
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
    LOG(1, "AFP FPCloseDir: vol=0x%04X dir=0x%08X", vol_id, (unsigned)dir_id);
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
    LOG(1, "AFP FPOpenDT: volId=0x%04X → DTRef=0x%04X", vol_id, dt->dt_ref);
    return AFPERR_NoErr;
}

static uint32_t afp_cmd_read_only_denied(const uint8_t *in, int in_len, uint8_t *out, int out_max, int *out_len) {
    (void)in;
    (void)in_len;
    (void)out;
    (void)out_max;
    if (out_len)
        *out_len = 0;
    return AFPERR_VolLocked;
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
    LOG(2, "AFP FPGetFileDirParms req detail: vol=0x%04X dir=0x%08X fileBm=0x%04X dirBm=0x%04X path='%s'", vol_id,
        (unsigned)dir_id, file_bm, dir_bm, path[0] ? path : "<root>");
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
    LOG(2, "AFP FPGetFileDirParms resolve: path='%s' is_dir=%d parent=0x%08X cnid=0x%08X size=%" PRIu64 " mode=%o",
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
    LOG(1, "AFP FPGetFileDirParms: vol=0x%04X dir=0x%08X type=%s path='%s' len=%d", vol_id, (unsigned)dir_id,
        is_dir ? "directory" : "file", target_rel[0] ? target_rel : "<root>", vpos);
    return AFPERR_NoErr;
}

static const afp_command_handler_t k_afp_command_handlers[] = {
    {AFP_CopyFile,        "FPCopyFile",        afp_cmd_read_only_denied  },
    {AFP_CreateDir,       "FPCreateDir",       afp_cmd_read_only_denied  },
    {AFP_CreateFile,      "FPCreateFile",      afp_cmd_read_only_denied  },
    {AFP_Delete,          "FPDelete",          afp_cmd_read_only_denied  },
    {AFP_GetSrvrInfo,     "FPGetSrvrInfo",     afp_cmd_get_srvr_info     },
    {AFP_GetSrvrParms,    "FPGetSrvrParms",    afp_cmd_get_srvr_parms    },
    {AFP_GetVolParms,     "FPGetVolParms",     afp_cmd_get_vol_parms     },
    {AFP_CloseVol,        "FPCloseVol",        afp_cmd_close_vol         },
    {AFP_Login,           "FPLogin",           afp_cmd_login             },
    {AFP_OpenVol,         "FPOpenVol",         afp_cmd_open_vol          },
    {AFP_CloseDir,        "FPCloseDir",        afp_cmd_close_dir         },
    {AFP_OpenDir,         "FPOpenDir",         afp_cmd_open_dir          },
    {AFP_OpenDT,          "FPOpenDT",          afp_cmd_open_dt           },
    {AFP_MoveAndRename,   "FPMoveAndRename",   afp_cmd_read_only_denied  },
    {AFP_Enumerate,       "FPEnumerate",       afp_cmd_enumerate         },
    {AFP_Rename,          "FPRename",          afp_cmd_read_only_denied  },
    {AFP_GetFileDirParms, "FPGetFileDirParms", afp_cmd_get_file_dir_parms},
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
    LOG(1, "AFP FPEnumerate: vol=0x%04X path='%s' returned=%u of %u start=%u", vol_id,
        target_rel[0] ? target_rel : "<root>", (unsigned)act, (unsigned)req_count, (unsigned)start_index);
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
    uint32_t result = handler->handler(in, in_len, out, out_max, out_len);
    if (result != AFPERR_NoErr && out_len)
        *out_len = 0;
    return result;
}
