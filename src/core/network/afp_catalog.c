// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// afp_catalog.c
// Persistent per-volume CNID catalog (see afp_catalog.h).

#include "afp_catalog.h"
#include "afp_applog.h"
#include "common.h"

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

#define GSC_MAGIC 0x47534332u // 'GSC2': afp_applog framing

// Log record opcodes.  RENAME and MOVE carry the same payload; keeping both
// makes a replayed log readable and matches the documented format.
enum {
    GSC_OP_ADD = 1,
    GSC_OP_RENAME = 2,
    GSC_OP_MOVE = 3,
    GSC_OP_DELETE = 4,
    GSC_OP_SET_ID = 5,
    GSC_OP_CLR_ID = 6,
    GSC_OP_STATE = 7, // generation(4) next_cnid(4)
};

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
    uint32_t child_next; // next slot+1 in its (parent, name) chain; 0 ends it
    afp_cat_entry_t pub;
} cat_slot_t;

struct afp_catalog {
    char root[PATH_MAX]; // volume root host path
    cat_slot_t *slots;
    size_t len;
    size_t cap;
    uint32_t *idx_cnid; // open-addressed cnid -> slot+1
    size_t idx_cap;
    uint32_t *idx_child; // (parent, name) -> first slot+1 of a chain; live slots only
    size_t child_cap;
    uint32_t generation;
    uint32_t next_cnid;
    uint32_t live;
    afp_applog_t *log; // <root>/.gs-afp/catalog.gsc
};

// --- big-endian helpers ----------------------------------------------------

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

// --- child index: (parent, name) -> slot ------------------------------------
//
// FPEnumerate adopts every entry it lists and each adoption looks its name up
// under its parent, so a scan here made listing a directory quadratic: 4000
// entries took 0.16 s, twice as many four times as long (10-network F-24).
// Names are matched exactly, as the host does: two host files may differ only
// in case, and each needs its own CNID.

static uint32_t child_hash(uint32_t parent, const char *name) {
    uint32_t h = 2166136261u ^ (parent * 2654435761u); // FNV-1a, seeded with the parent
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        h = (h ^ *p) * 16777619u;
    return h;
}

static uint32_t *child_bucket(afp_catalog_t *cat, uint32_t parent, const char *name) {
    return &cat->idx_child[child_hash(parent, name) & (cat->child_cap - 1)];
}

static void child_link(afp_catalog_t *cat, size_t slot) {
    uint32_t *b = child_bucket(cat, cat->slots[slot].parent, cat->slots[slot].name);
    cat->slots[slot].child_next = *b;
    *b = (uint32_t)(slot + 1);
}

// Take a slot out of its chain, before its parent or name changes or it dies.
static void child_unlink(afp_catalog_t *cat, size_t slot) {
    for (uint32_t *link = child_bucket(cat, cat->slots[slot].parent, cat->slots[slot].name); *link;
         link = &cat->slots[*link - 1].child_next) {
        if (*link == slot + 1) {
            *link = cat->slots[slot].child_next;
            cat->slots[slot].child_next = 0;
            return;
        }
    }
}

// Rebuild the child index at `cap` buckets (a power of two) from the live slots.
static bool child_rebuild(afp_catalog_t *cat, size_t cap) {
    uint32_t *idx = (uint32_t *)calloc(cap, sizeof(uint32_t));
    if (!idx)
        return false;
    free(cat->idx_child);
    cat->idx_child = idx;
    cat->child_cap = cap;
    for (size_t i = 0; i < cat->len; i++)
        if (!cat->slots[i].dead)
            child_link(cat, i);
    return true;
}

// Append a slot to the table (no log write), returning its index or -1.
// CNIDs ascend through the table -- afp_catalog_next searches it by halves --
// so one that does not, from a damaged log, is refused.
static long slot_push(afp_catalog_t *cat, uint32_t cnid, uint32_t parent, bool is_dir, const char *name) {
    if (cat->len && cnid <= cat->slots[cat->len - 1].cnid)
        return -1;
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
    if (cat->live + 1 > cat->child_cap) {
        if (!child_rebuild(cat, cat->child_cap ? cat->child_cap * 2 : 64)) { // covers this slot
            cat->len--;
            index_rebuild(cat, cat->idx_cap);
            return -1;
        }
    } else {
        child_link(cat, idx);
    }
    cat->live++;
    if (cnid >= cat->next_cnid)
        cat->next_cnid = cnid + 1;
    return (long)idx;
}

// Tombstone one slot without touching the log (callers log their own record).
static void kill_slot(afp_catalog_t *cat, size_t slot) {
    if (cat->slots[slot].dead)
        return;
    child_unlink(cat, slot);
    cat->slots[slot].dead = true;
    cat->slots[slot].has_file_id = false;
    if (cat->live)
        cat->live--;
}

// Give a live slot a new parent and name, keeping the child index in step.
static void slot_rekey(afp_catalog_t *cat, size_t slot, uint32_t parent, const char *name) {
    child_unlink(cat, slot);
    cat->slots[slot].parent = parent;
    snprintf(cat->slots[slot].name, sizeof(cat->slots[slot].name), "%s", name ? name : "");
    child_link(cat, slot);
}

// --- log I/O ---------------------------------------------------------------

// Every entry record carries the same payload: cnid(4) parent(4) is_dir(1)
// name, the name running to the record's end.
static void log_append(afp_catalog_t *cat, uint8_t op, uint32_t cnid, uint32_t parent, bool is_dir, const char *name) {
    uint8_t buf[9 + AFP_CAT_MAX_NAME];
    size_t nlen = name ? strnlen(name, AFP_CAT_MAX_NAME - 1) : 0;
    WR_BE32(buf, cnid);
    WR_BE32(buf + 4, parent);
    buf[8] = is_dir ? 1 : 0;
    memcpy(buf + 9, name ? name : "", nlen);
    afp_applog_append(cat->log, op, buf, (uint16_t)(9 + nlen), cat->live);
}

// The generation and the next CNID.  Replay recovers next_cnid from the ADD
// records too; this record keeps it across the deletes a compaction drops.
static void log_state(afp_catalog_t *cat) {
    uint8_t buf[8];
    WR_BE32(buf, cat->generation);
    WR_BE32(buf + 4, cat->next_cnid);
    afp_applog_append(cat->log, GSC_OP_STATE, buf, sizeof(buf), cat->live);
}

// Apply one replayed record to the in-memory table.
static void replay(void *ctx, uint8_t op, const uint8_t *p, uint16_t len) {
    afp_catalog_t *cat = (afp_catalog_t *)ctx;
    if (op == GSC_OP_STATE) {
        if (len >= 8) {
            cat->generation = RD_BE32(p);
            if (RD_BE32(p + 4) > cat->next_cnid)
                cat->next_cnid = RD_BE32(p + 4);
        }
        return;
    }
    if (len < 9)
        return;
    uint32_t cnid = RD_BE32(p);
    uint32_t parent = RD_BE32(p + 4);
    bool is_dir = p[8] != 0;
    char name[AFP_CAT_MAX_NAME];
    size_t nlen = (size_t)len - 9 < sizeof(name) - 1 ? (size_t)len - 9 : sizeof(name) - 1;
    memcpy(name, p + 9, nlen);
    name[nlen] = '\0';
    long si = slot_of(cat, cnid);
    switch (op) {
    case GSC_OP_ADD:
        if (si < 0)
            slot_push(cat, cnid, parent, is_dir, name);
        break;
    case GSC_OP_RENAME:
    case GSC_OP_MOVE:
        if (si >= 0 && !cat->slots[si].dead)
            slot_rekey(cat, (size_t)si, parent, name);
        break;
    case GSC_OP_DELETE:
        if (si >= 0)
            kill_slot(cat, (size_t)si);
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
        break; // a later format's record
    }
}

// A compaction: the state, then an ADD (and a SET_ID) per live entry.  The
// generation is unchanged -- CNIDs are, so a client's catalog position and
// every FPEnumerate snapshot stay good across it.
static bool dump(void *ctx, afp_applog_t *log) {
    afp_catalog_t *cat = (afp_catalog_t *)ctx;
    uint8_t buf[9 + AFP_CAT_MAX_NAME];
    WR_BE32(buf, cat->generation);
    WR_BE32(buf + 4, cat->next_cnid);
    if (!afp_applog_emit(log, GSC_OP_STATE, buf, 8))
        return false;
    for (size_t i = 0; i < cat->len; i++) {
        cat_slot_t *s = &cat->slots[i];
        if (s->dead || s->cnid == AFP_CNID_ROOT)
            continue;
        size_t nlen = strnlen(s->name, AFP_CAT_MAX_NAME - 1);
        WR_BE32(buf, s->cnid);
        WR_BE32(buf + 4, s->parent);
        buf[8] = s->is_dir ? 1 : 0;
        memcpy(buf + 9, s->name, nlen);
        if (!afp_applog_emit(log, GSC_OP_ADD, buf, (uint16_t)(9 + nlen)) ||
            (s->has_file_id && !afp_applog_emit(log, GSC_OP_SET_ID, buf, (uint16_t)(9 + nlen))))
            return false;
    }
    return true;
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

    char log_path[PATH_MAX];
    if (!afp_meta_control_path(cat->root, "catalog.gsc", log_path, sizeof(log_path))) {
        free(cat);
        return NULL;
    }

    // The root always exists and always has CNID 2.
    if (slot_push(cat, AFP_CNID_ROOT, AFP_CNID_ROOT_PARENT, true, "") < 0) {
        free(cat->slots);
        free(cat->idx_cnid);
        free(cat->idx_child);
        free(cat);
        return NULL;
    }
    cat->next_cnid = AFP_CNID_FIRST;

    bool fresh = false;
    cat->log = afp_applog_open(log_path, GSC_MAGIC, replay, dump, cat, &fresh);
    if (fresh) {
        // No usable log: start clean and bump the generation so any cached
        // client CatPosition is rejected rather than silently mis-resumed.
        cat->generation++;
        log_state(cat);
    }
    if (cat->next_cnid < AFP_CNID_FIRST)
        cat->next_cnid = AFP_CNID_FIRST;
    LOG(2, "AFP catalog: opened '%s' gen=%u entries=%u next_cnid=%u", log_path, cat->generation, cat->live,
        cat->next_cnid);
    return cat;
}

void afp_catalog_close(afp_catalog_t *cat) {
    if (!cat)
        return;
    afp_applog_close(cat->log, cat->live);
    free(cat->slots);
    free(cat->idx_cnid);
    free(cat->idx_child);
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
    if (!cat->idx_child)
        return NULL;
    for (uint32_t v = *child_bucket(cat, parent, name); v; v = cat->slots[v - 1].child_next)
        if (cat->slots[v - 1].parent == parent && strcmp(cat->slots[v - 1].name, name) == 0)
            return view_of(cat, v - 1);
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
    LOG(10, "AFP catalog: add cnid=%u parent=%u %s '%s'", cnid, parent, is_dir ? "dir" : "file", name);
    return view_of(cat, (size_t)si);
}

bool afp_catalog_rename(afp_catalog_t *cat, uint32_t cnid, const char *new_name) {
    if (!cat || !new_name || !*new_name)
        return false;
    long si = slot_of(cat, cnid);
    if (si < 0 || cat->slots[si].dead)
        return false;
    slot_rekey(cat, (size_t)si, cat->slots[si].parent, new_name);
    log_append(cat, GSC_OP_RENAME, cnid, cat->slots[si].parent, cat->slots[si].is_dir, new_name);
    return true;
}

bool afp_catalog_move(afp_catalog_t *cat, uint32_t cnid, uint32_t new_parent, const char *new_name) {
    if (!cat)
        return false;
    long si = slot_of(cat, cnid);
    if (si < 0 || cat->slots[si].dead)
        return false;
    char name[AFP_CAT_MAX_NAME];
    snprintf(name, sizeof(name), "%s", (new_name && *new_name) ? new_name : cat->slots[si].name);
    slot_rekey(cat, (size_t)si, new_parent, name);
    log_append(cat, GSC_OP_MOVE, cnid, new_parent, cat->slots[si].is_dir, cat->slots[si].name);
    return true;
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
                kill_slot(cat, i); // before the record: an append may compact
                log_append(cat, GSC_OP_DELETE, cat->slots[i].cnid, cat->slots[i].parent, cat->slots[i].is_dir,
                           cat->slots[i].name);
                progress = true;
            }
        }
    }
    cat->generation++;
    log_state(cat);
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

bool afp_host_element(const char *name, size_t len) {
    if (!name || len == 0 || memchr(name, '/', len))
        return false;
    return !(len == 1 && name[0] == '.') && !(len == 2 && name[0] == '.' && name[1] == '.');
}

bool afp_host_join(const char *root, const char *rel, char *out, size_t cap) {
    if (!root || !*root || !out || cap == 0)
        return false;
    size_t root_len = strlen(root);
    while (root_len > 1 && root[root_len - 1] == '/')
        root_len--;
    if (!rel || !*rel)
        return (size_t)snprintf(out, cap, "%.*s", (int)root_len, root) < cap;
    for (const char *p = rel;;) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (!afp_host_element(p, len))
            return false;
        if (!slash)
            break;
        p = slash + 1;
    }
    const char *sep = (root_len == 1 && root[0] == '/') ? "" : "/";
    return (size_t)snprintf(out, cap, "%.*s%s%s", (int)root_len, root, sep, rel) < cap;
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
    // CNIDs ascend through the table (slot_push), so the first above `after`
    // is found by halves; it was a full scan per call, and FPCatSearch calls
    // this once per entry.
    size_t lo = 0, hi = cat->len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cat->slots[mid].cnid <= after)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (size_t i = lo; i < cat->len; i++)
        if (!cat->slots[i].dead)
            return view_of(cat, i);
    return NULL;
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
            !afp_host_join(cat->root, rel, host, sizeof(host)) || stat(host, &st) != 0) {
            kill_slot(cat, i); // before the record: an append may compact
            log_append(cat, GSC_OP_DELETE, cat->slots[i].cnid, cat->slots[i].parent, cat->slots[i].is_dir,
                       cat->slots[i].name);
            killed++;
        }
    }
    if (killed) {
        cat->generation++;
        log_state(cat);
        LOG(2, "AFP catalog: swept %u stale entries (gen=%u)", killed, cat->generation);
    }
    return killed;
}
