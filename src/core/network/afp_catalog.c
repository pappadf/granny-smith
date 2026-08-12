// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// afp_catalog.c
// Persistent per-volume CNID catalog (see afp_catalog.h).

#include "afp_catalog.h"

#include "afp_meta.h"
#include "log.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

LOG_USE_CATEGORY_NAME("appletalk");

#define GSC_MAGIC 0x47534331u // 'GSC1'

// Log record opcodes.  RENAME and MOVE carry the same payload; keeping both
// makes a replayed log readable and matches the documented format.
enum {
    GSC_OP_ADD = 1,
    GSC_OP_RENAME = 2,
    GSC_OP_MOVE = 3,
    GSC_OP_DELETE = 4,
    GSC_OP_SET_ID = 5,
    GSC_OP_CLR_ID = 6,
};

// Compact once the log holds more than this multiple of the live entries.
#define GSC_COMPACT_FACTOR 4

// One stored entry.  Tombstoned entries stay in the table so their CNID is
// never handed out again inside a generation.
//
// Each slot carries its own public view rather than the table sharing one
// scratch struct: callers routinely hold two entries at once (a file and its
// new parent, say), and a shared view would silently make the first alias the
// second.  A returned pointer is still invalidated by anything that grows the
// table, which is why every caller copies the CNID it cares about.
typedef struct {
    uint32_t cnid;
    uint32_t parent;
    bool is_dir;
    bool has_file_id;
    bool dead;
    char name[AFP_CAT_MAX_NAME];
    afp_cat_entry_t pub;
} cat_slot_t;

struct afp_catalog {
    char root[PATH_MAX]; // volume root host path
    char log_path[PATH_MAX]; // <root>/.gs-afp/catalog.gsc
    cat_slot_t *slots;
    size_t len;
    size_t cap;
    uint32_t *idx_cnid; // open-addressed cnid -> slot+1
    size_t idx_cap;
    uint32_t generation;
    uint32_t next_cnid;
    uint32_t live;
    FILE *log; // append handle, NULL when the log is unusable
    size_t log_records;
};

// --- big-endian helpers ----------------------------------------------------

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// CRC-32 (IEEE) over one record, so a torn tail write is detected on replay.
static uint32_t crc32_buf(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

// --- index -----------------------------------------------------------------

// Rebuild the cnid -> slot index at `cap` buckets (a power of two).
static bool index_rebuild(afp_catalog_t *cat, size_t cap) {
    uint32_t *idx = (uint32_t *)calloc(cap, sizeof(uint32_t));
    if (!idx)
        return false;
    free(cat->idx_cnid);
    cat->idx_cnid = idx;
    cat->idx_cap = cap;
    for (size_t i = 0; i < cat->len; i++) {
        size_t h = (size_t)(cat->slots[i].cnid * 2654435761u) & (cap - 1);
        while (idx[h])
            h = (h + 1) & (cap - 1);
        idx[h] = (uint32_t)(i + 1);
    }
    return true;
}

// Record a new slot in the cnid index, growing it past 50% load.
static bool index_insert(afp_catalog_t *cat, size_t slot) {
    if (!cat->idx_cnid || (cat->len + 1) * 2 > cat->idx_cap) {
        size_t cap = cat->idx_cap ? cat->idx_cap * 2 : 64;
        while ((cat->len + 1) * 2 > cap)
            cap *= 2;
        if (!index_rebuild(cat, cap))
            return false;
        return true; // rebuild covered every slot, this one included
    }
    size_t h = (size_t)(cat->slots[slot].cnid * 2654435761u) & (cat->idx_cap - 1);
    while (cat->idx_cnid[h])
        h = (h + 1) & (cat->idx_cap - 1);
    cat->idx_cnid[h] = (uint32_t)(slot + 1);
    return true;
}

// Slot index for a CNID, or -1.  Tombstoned slots are still found here so
// mutations can resurrect or re-tombstone them.
static long slot_of(afp_catalog_t *cat, uint32_t cnid) {
    if (!cat->idx_cnid || cat->idx_cap == 0)
        return -1;
    size_t h = (size_t)(cnid * 2654435761u) & (cat->idx_cap - 1);
    for (size_t probe = 0; probe < cat->idx_cap; probe++) {
        uint32_t v = cat->idx_cnid[h];
        if (!v)
            return -1;
        if (cat->slots[v - 1].cnid == cnid)
            return (long)(v - 1);
        h = (h + 1) & (cat->idx_cap - 1);
    }
    return -1;
}

// Append a slot to the table (no log write), returning its index or -1.
static long slot_push(afp_catalog_t *cat, uint32_t cnid, uint32_t parent, bool is_dir, const char *name) {
    if (cat->len == cat->cap) {
        size_t cap = cat->cap ? cat->cap * 2 : 32;
        cat_slot_t *tmp = (cat_slot_t *)realloc(cat->slots, cap * sizeof(cat_slot_t));
        if (!tmp)
            return -1;
        cat->slots = tmp;
        cat->cap = cap;
    }
    cat_slot_t *s = &cat->slots[cat->len];
    memset(s, 0, sizeof(*s));
    s->cnid = cnid;
    s->parent = parent;
    s->is_dir = is_dir;
    snprintf(s->name, sizeof(s->name), "%s", name ? name : "");
    size_t idx = cat->len++;
    if (!index_insert(cat, idx)) {
        cat->len--;
        return -1;
    }
    cat->live++;
    if (cnid >= cat->next_cnid)
        cat->next_cnid = cnid + 1;
    return (long)idx;
}

// --- log I/O ---------------------------------------------------------------

// Serialize one record.  Returns its length, or 0 if the name overflows.
static size_t record_encode(uint8_t *buf, size_t cap, uint8_t op, uint32_t cnid, uint32_t parent, bool is_dir,
                            const char *name) {
    size_t nlen = name ? strlen(name) : 0;
    if (nlen > 255)
        nlen = 255;
    size_t total = 1 + 4 + 4 + 1 + 1 + nlen + 4;
    if (total > cap)
        return 0;
    size_t p = 0;
    buf[p++] = op;
    wr32(buf + p, cnid);
    p += 4;
    wr32(buf + p, parent);
    p += 4;
    buf[p++] = is_dir ? 1 : 0;
    buf[p++] = (uint8_t)nlen;
    if (nlen)
        memcpy(buf + p, name, nlen);
    p += nlen;
    wr32(buf + p, crc32_buf(buf, p));
    p += 4;
    return p;
}

// Append a record and flush it, so a crash loses at most the in-flight write.
static void log_append(afp_catalog_t *cat, uint8_t op, uint32_t cnid, uint32_t parent, bool is_dir, const char *name) {
    if (!cat->log)
        return;
    uint8_t buf[1 + 4 + 4 + 1 + 1 + 255 + 4];
    size_t n = record_encode(buf, sizeof(buf), op, cnid, parent, is_dir, name);
    if (!n)
        return;
    if (fwrite(buf, 1, n, cat->log) != n) {
        LOG(1, "AFP catalog: log write failed for '%s' (%s)", cat->log_path, strerror(errno));
        fclose(cat->log);
        cat->log = NULL;
        return;
    }
    fflush(cat->log);
    cat->log_records++;
}

// Apply one decoded record to the in-memory table.
static void replay(afp_catalog_t *cat, uint8_t op, uint32_t cnid, uint32_t parent, bool is_dir, const char *name) {
    long si = slot_of(cat, cnid);
    switch (op) {
    case GSC_OP_ADD:
        if (si < 0)
            slot_push(cat, cnid, parent, is_dir, name);
        break;
    case GSC_OP_RENAME:
    case GSC_OP_MOVE:
        if (si >= 0) {
            cat->slots[si].parent = parent;
            snprintf(cat->slots[si].name, sizeof(cat->slots[si].name), "%s", name ? name : "");
        }
        break;
    case GSC_OP_DELETE:
        if (si >= 0 && !cat->slots[si].dead) {
            cat->slots[si].dead = true;
            if (cat->live)
                cat->live--;
        }
        break;
    case GSC_OP_SET_ID:
        if (si >= 0)
            cat->slots[si].has_file_id = true;
        break;
    case GSC_OP_CLR_ID:
        if (si >= 0)
            cat->slots[si].has_file_id = false;
        break;
    default:
        break;
    }
}

// Read the whole log back into the table.  Returns false on a corrupt header
// (the caller then starts a fresh catalog); a torn tail record is simply
// dropped, which is the normal crash case.
static bool log_load(afp_catalog_t *cat) {
    FILE *f = fopen(cat->log_path, "rb");
    if (!f)
        return false; // no log yet — a fresh volume
    uint8_t hdr[12];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || rd32(hdr) != GSC_MAGIC) {
        fclose(f);
        LOG(1, "AFP catalog: '%s' has no usable header — rebuilding", cat->log_path);
        return false;
    }
    cat->generation = rd32(hdr + 4);
    cat->next_cnid = rd32(hdr + 8);
    if (cat->next_cnid < AFP_CNID_FIRST)
        cat->next_cnid = AFP_CNID_FIRST;

    for (;;) {
        uint8_t fixed[11];
        size_t got = fread(fixed, 1, sizeof(fixed), f);
        if (got != sizeof(fixed))
            break; // clean EOF or torn record
        uint8_t nlen = fixed[10];
        uint8_t body[255 + 4];
        if (fread(body, 1, (size_t)nlen + 4, f) != (size_t)nlen + 4)
            break;
        uint8_t whole[sizeof(fixed) + 255];
        memcpy(whole, fixed, sizeof(fixed));
        memcpy(whole + sizeof(fixed), body, nlen);
        if (crc32_buf(whole, sizeof(fixed) + nlen) != rd32(body + nlen)) {
            LOG(1, "AFP catalog: dropping torn tail record in '%s'", cat->log_path);
            break;
        }
        char name[AFP_CAT_MAX_NAME];
        size_t copy = nlen < sizeof(name) - 1 ? nlen : sizeof(name) - 1;
        memcpy(name, body, copy);
        name[copy] = '\0';
        replay(cat, fixed[0], rd32(fixed + 1), rd32(fixed + 5), fixed[9] != 0, name);
        cat->log_records++;
    }
    fclose(f);
    return true;
}

// Rewrite the log as pure ADDs (plus SET_ID) for the live entries only.
static void log_compact(afp_catalog_t *cat) {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", cat->log_path) >= (int)sizeof(tmp))
        return;
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return;
    cat->generation++;
    uint8_t hdr[12];
    wr32(hdr, GSC_MAGIC);
    wr32(hdr + 4, cat->generation);
    wr32(hdr + 8, cat->next_cnid);
    bool ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    size_t records = 0;
    for (size_t i = 0; ok && i < cat->len; i++) {
        if (cat->slots[i].dead || cat->slots[i].cnid == AFP_CNID_ROOT)
            continue;
        uint8_t buf[1 + 4 + 4 + 1 + 1 + 255 + 4];
        size_t n = record_encode(buf, sizeof(buf), GSC_OP_ADD, cat->slots[i].cnid, cat->slots[i].parent,
                                 cat->slots[i].is_dir, cat->slots[i].name);
        ok = n && fwrite(buf, 1, n, f) == n;
        records++;
        if (ok && cat->slots[i].has_file_id) {
            n = record_encode(buf, sizeof(buf), GSC_OP_SET_ID, cat->slots[i].cnid, cat->slots[i].parent,
                              cat->slots[i].is_dir, cat->slots[i].name);
            ok = n && fwrite(buf, 1, n, f) == n;
            records++;
        }
    }
    if (fclose(f) != 0)
        ok = false;
    if (!ok) {
        remove(tmp);
        return;
    }
    if (cat->log) {
        fclose(cat->log);
        cat->log = NULL;
    }
    if (rename(tmp, cat->log_path) != 0) {
        remove(tmp);
    } else {
        cat->log_records = records;
    }
    cat->log = fopen(cat->log_path, "ab");
}

// Open the append handle, writing a header first when the file is new.
static void log_open_append(afp_catalog_t *cat, bool fresh) {
    if (fresh) {
        FILE *f = fopen(cat->log_path, "wb");
        if (!f) {
            LOG(1, "AFP catalog: cannot create '%s' (%s) — IDs will not persist", cat->log_path, strerror(errno));
            return;
        }
        uint8_t hdr[12];
        wr32(hdr, GSC_MAGIC);
        wr32(hdr + 4, cat->generation);
        wr32(hdr + 8, cat->next_cnid);
        fwrite(hdr, 1, sizeof(hdr), f);
        fclose(f);
    }
    cat->log = fopen(cat->log_path, "ab");
    if (!cat->log)
        LOG(1, "AFP catalog: cannot append to '%s' (%s)", cat->log_path, strerror(errno));
}

// Rewrite the header in place so `generation` / `next_cnid` survive a reload.
static void log_sync_header(afp_catalog_t *cat) {
    if (!cat->log)
        return;
    fflush(cat->log);
    FILE *f = fopen(cat->log_path, "r+b");
    if (!f)
        return;
    uint8_t hdr[12];
    wr32(hdr, GSC_MAGIC);
    wr32(hdr + 4, cat->generation);
    wr32(hdr + 8, cat->next_cnid);
    fwrite(hdr, 1, sizeof(hdr), f);
    fclose(f);
}

// --- lifecycle -------------------------------------------------------------

afp_catalog_t *afp_catalog_open(const char *host_root) {
    if (!host_root || !*host_root)
        return NULL;
    afp_catalog_t *cat = (afp_catalog_t *)calloc(1, sizeof(*cat));
    if (!cat)
        return NULL;
    snprintf(cat->root, sizeof(cat->root), "%s", host_root);
    cat->generation = 1;
    cat->next_cnid = AFP_CNID_FIRST;

    char ctrl[PATH_MAX];
    if (snprintf(ctrl, sizeof(ctrl), "%s/%s", cat->root, AFP_CONTROL_DIR) >= (int)sizeof(ctrl)) {
        free(cat);
        return NULL;
    }
    mkdir(ctrl, 0755); // idempotent; a failure only costs persistence
    if ((size_t)snprintf(cat->log_path, sizeof(cat->log_path), "%s/catalog.gsc", ctrl) >= sizeof(cat->log_path)) {
        free(cat);
        return NULL;
    }

    // The root always exists and always has CNID 2.
    if (slot_push(cat, AFP_CNID_ROOT, AFP_CNID_ROOT_PARENT, true, "") < 0) {
        free(cat->slots);
        free(cat->idx_cnid);
        free(cat);
        return NULL;
    }
    cat->next_cnid = AFP_CNID_FIRST;

    bool loaded = log_load(cat);
    if (!loaded) {
        // No usable log: start clean and bump the generation so any cached
        // client CatPosition is rejected rather than silently mis-resumed.
        cat->generation++;
        log_open_append(cat, true);
    } else {
        log_open_append(cat, false);
    }
    LOG(2, "AFP catalog: opened '%s' gen=%u entries=%u next_cnid=%u", cat->log_path, cat->generation, cat->live,
        cat->next_cnid);
    return cat;
}

void afp_catalog_close(afp_catalog_t *cat) {
    if (!cat)
        return;
    if (cat->log && cat->live && cat->log_records > (size_t)GSC_COMPACT_FACTOR * cat->live)
        log_compact(cat);
    log_sync_header(cat);
    if (cat->log)
        fclose(cat->log);
    free(cat->slots);
    free(cat->idx_cnid);
    free(cat);
}

uint32_t afp_catalog_generation(const afp_catalog_t *cat) {
    return cat ? cat->generation : 0;
}

uint32_t afp_catalog_count(const afp_catalog_t *cat) {
    return cat ? cat->live : 0;
}

// --- lookups ---------------------------------------------------------------

// Refresh a slot's public view and hand it back.
static const afp_cat_entry_t *view_of(afp_catalog_t *cat, size_t slot) {
    cat_slot_t *s = &cat->slots[slot];
    s->pub.cnid = s->cnid;
    s->pub.parent = s->parent;
    s->pub.is_dir = s->is_dir;
    s->pub.has_file_id = s->has_file_id;
    s->pub.name = s->name;
    return &s->pub;
}

const afp_cat_entry_t *afp_catalog_find(afp_catalog_t *cat, uint32_t cnid) {
    if (!cat)
        return NULL;
    long si = slot_of(cat, cnid);
    if (si < 0 || cat->slots[si].dead)
        return NULL;
    return view_of(cat, (size_t)si);
}

const afp_cat_entry_t *afp_catalog_find_child(afp_catalog_t *cat, uint32_t parent, const char *name) {
    if (!cat || !name)
        return NULL;
    for (size_t i = 0; i < cat->len; i++) {
        if (cat->slots[i].dead || cat->slots[i].parent != parent)
            continue;
        if (strcmp(cat->slots[i].name, name) == 0)
            return view_of(cat, i);
    }
    return NULL;
}

bool afp_catalog_path(afp_catalog_t *cat, uint32_t cnid, char *out, size_t cap) {
    if (!cat || !out || cap == 0)
        return false;
    out[0] = '\0';
    if (cnid == AFP_CNID_ROOT)
        return true;
    // Collect the chain root-ward, then emit it forwards.
    const char *parts[AFP_CAT_MAX_PATH / 2];
    size_t n = 0;
    uint32_t cur = cnid;
    while (cur != AFP_CNID_ROOT && n < sizeof(parts) / sizeof(parts[0])) {
        long si = slot_of(cat, cur);
        if (si < 0)
            return false;
        parts[n++] = cat->slots[si].name;
        cur = cat->slots[si].parent;
    }
    if (cur != AFP_CNID_ROOT)
        return false;
    size_t pos = 0;
    for (size_t i = n; i-- > 0;) {
        size_t len = strlen(parts[i]);
        if (pos + (pos ? 1 : 0) + len + 1 > cap)
            return false;
        if (pos)
            out[pos++] = '/';
        memcpy(out + pos, parts[i], len);
        pos += len;
    }
    out[pos] = '\0';
    return true;
}

// --- mutations -------------------------------------------------------------

const afp_cat_entry_t *afp_catalog_add(afp_catalog_t *cat, uint32_t parent, const char *name, bool is_dir) {
    if (!cat || !name || !*name)
        return NULL;
    const afp_cat_entry_t *existing = afp_catalog_find_child(cat, parent, name);
    if (existing)
        return existing;
    uint32_t cnid = cat->next_cnid++;
    long si = slot_push(cat, cnid, parent, is_dir, name);
    if (si < 0)
        return NULL;
    log_append(cat, GSC_OP_ADD, cnid, parent, is_dir, name);
    log_sync_header(cat);
    LOG(10, "AFP catalog: add cnid=%u parent=%u %s '%s'", cnid, parent, is_dir ? "dir" : "file", name);
    return view_of(cat, (size_t)si);
}

bool afp_catalog_rename(afp_catalog_t *cat, uint32_t cnid, const char *new_name) {
    if (!cat || !new_name || !*new_name)
        return false;
    long si = slot_of(cat, cnid);
    if (si < 0 || cat->slots[si].dead)
        return false;
    snprintf(cat->slots[si].name, sizeof(cat->slots[si].name), "%s", new_name);
    log_append(cat, GSC_OP_RENAME, cnid, cat->slots[si].parent, cat->slots[si].is_dir, new_name);
    return true;
}

bool afp_catalog_move(afp_catalog_t *cat, uint32_t cnid, uint32_t new_parent, const char *new_name) {
    if (!cat)
        return false;
    long si = slot_of(cat, cnid);
    if (si < 0 || cat->slots[si].dead)
        return false;
    cat->slots[si].parent = new_parent;
    if (new_name && *new_name)
        snprintf(cat->slots[si].name, sizeof(cat->slots[si].name), "%s", new_name);
    log_append(cat, GSC_OP_MOVE, cnid, new_parent, cat->slots[si].is_dir, cat->slots[si].name);
    return true;
}

// Tombstone one slot without touching the log (callers log their own record).
static void kill_slot(afp_catalog_t *cat, size_t slot) {
    if (cat->slots[slot].dead)
        return;
    cat->slots[slot].dead = true;
    cat->slots[slot].has_file_id = false;
    if (cat->live)
        cat->live--;
}

bool afp_catalog_remove(afp_catalog_t *cat, uint32_t cnid) {
    if (!cat || cnid == AFP_CNID_ROOT)
        return false;
    long si = slot_of(cat, cnid);
    if (si < 0 || cat->slots[si].dead)
        return false;
    bool was_dir = cat->slots[si].is_dir;
    kill_slot(cat, (size_t)si);
    log_append(cat, GSC_OP_DELETE, cnid, cat->slots[si].parent, was_dir, cat->slots[si].name);
    if (was_dir) {
        // Sweep the subtree: repeat until a pass kills nothing, so grandchildren
        // whose parent died later in the table are still reached.
        bool progress = true;
        while (progress) {
            progress = false;
            for (size_t i = 0; i < cat->len; i++) {
                if (cat->slots[i].dead)
                    continue;
                long ps = slot_of(cat, cat->slots[i].parent);
                if (ps < 0 || !cat->slots[ps].dead)
                    continue;
                log_append(cat, GSC_OP_DELETE, cat->slots[i].cnid, cat->slots[i].parent, cat->slots[i].is_dir,
                           cat->slots[i].name);
                kill_slot(cat, i);
                progress = true;
            }
        }
    }
    cat->generation++;
    log_sync_header(cat);
    return true;
}

bool afp_catalog_set_file_id(afp_catalog_t *cat, uint32_t cnid, bool present) {
    if (!cat)
        return false;
    long si = slot_of(cat, cnid);
    if (si < 0 || cat->slots[si].dead)
        return false;
    cat->slots[si].has_file_id = present;
    log_append(cat, present ? GSC_OP_SET_ID : GSC_OP_CLR_ID, cnid, cat->slots[si].parent, cat->slots[si].is_dir,
               cat->slots[si].name);
    return true;
}

// --- path resolution / adoption --------------------------------------------

// Host path for a catalog-relative path.  False on overflow.
static bool host_path_of(afp_catalog_t *cat, const char *rel, char *out, size_t cap) {
    if (!rel || !*rel)
        return (size_t)snprintf(out, cap, "%s", cat->root) < cap;
    return (size_t)snprintf(out, cap, "%s/%s", cat->root, rel) < cap;
}

const afp_cat_entry_t *afp_catalog_resolve_path(afp_catalog_t *cat, const char *rel_path, bool adopt, bool is_dir) {
    if (!cat)
        return NULL;
    if (!rel_path || !*rel_path)
        return afp_catalog_find(cat, AFP_CNID_ROOT);
    if (strlen(rel_path) >= AFP_CAT_MAX_PATH)
        return NULL;

    char work[AFP_CAT_MAX_PATH];
    snprintf(work, sizeof(work), "%s", rel_path);
    uint32_t parent = AFP_CNID_ROOT;
    const afp_cat_entry_t *entry = NULL;
    char *save = NULL;
    for (char *tok = strtok_r(work, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        char *peek = save;
        bool last = (peek == NULL || *peek == '\0');
        entry = afp_catalog_find_child(cat, parent, tok);
        if (!entry) {
            if (!adopt)
                return NULL;
            entry = afp_catalog_add(cat, parent, tok, last ? is_dir : true);
            if (!entry)
                return NULL;
        }
        parent = entry->cnid;
    }
    return entry;
}

// --- iteration and sweeping ------------------------------------------------

const afp_cat_entry_t *afp_catalog_next(afp_catalog_t *cat, uint32_t after) {
    if (!cat)
        return NULL;
    long best = -1;
    for (size_t i = 0; i < cat->len; i++) {
        if (cat->slots[i].dead || cat->slots[i].cnid <= after)
            continue;
        if (best < 0 || cat->slots[i].cnid < cat->slots[best].cnid)
            best = (long)i;
    }
    return best < 0 ? NULL : view_of(cat, (size_t)best);
}

uint32_t afp_catalog_sweep(afp_catalog_t *cat) {
    if (!cat)
        return 0;
    uint32_t killed = 0;
    for (size_t i = 0; i < cat->len; i++) {
        if (cat->slots[i].dead || cat->slots[i].cnid == AFP_CNID_ROOT)
            continue;
        char rel[AFP_CAT_MAX_PATH];
        char host[PATH_MAX];
        struct stat st;
        if (!afp_catalog_path(cat, cat->slots[i].cnid, rel, sizeof(rel)) ||
            !host_path_of(cat, rel, host, sizeof(host)) || stat(host, &st) != 0) {
            log_append(cat, GSC_OP_DELETE, cat->slots[i].cnid, cat->slots[i].parent, cat->slots[i].is_dir,
                       cat->slots[i].name);
            kill_slot(cat, i);
            killed++;
        }
    }
    if (killed) {
        cat->generation++;
        log_sync_header(cat);
        LOG(2, "AFP catalog: swept %u stale entries (gen=%u)", killed, cat->generation);
    }
    return killed;
}
