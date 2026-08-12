// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// afp_desktop.c
// Persistent per-volume desktop database (see afp_desktop.h).

#include "afp_desktop.h"

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

#define DT_ICON_MAGIC 0x47534931u // 'GSI1'
#define DT_APPL_MAGIC 0x47534131u // 'GSA1'

// Record ops shared by both stores.
enum { DT_OP_PUT = 1, DT_OP_DEL = 2 };

// Rewrite a store once it holds more than this multiple of its live records.
#define DT_COMPACT_FACTOR 4

typedef struct {
    afp_icon_t v;
    uint8_t bytes[AFP_ICON_MAX_BYTES];
    bool dead;
} icon_slot_t;

typedef struct {
    afp_appl_t v;
    bool dead;
} appl_slot_t;

struct afp_desktop {
    char icon_path[PATH_MAX];
    char appl_path[PATH_MAX];
    icon_slot_t *icons;
    size_t icon_len, icon_cap, icon_records;
    appl_slot_t *appls;
    size_t appl_len, appl_cap, appl_records;
    FILE *icon_log;
    FILE *appl_log;
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
static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

// --- icon store ------------------------------------------------------------

// icon record: op(1) creator(4) type(4) icon_type(1) tag(4) size(2) bitmap[size]
#define DT_ICON_FIXED 16

// Append one icon record; drops the log handle on write failure so the store
// degrades to volatile rather than looping on a dead file.
static void icon_log_append(afp_desktop_t *dt, uint8_t op, const afp_icon_t *ic) {
    if (!dt->icon_log)
        return;
    uint8_t hdr[DT_ICON_FIXED];
    hdr[0] = op;
    wr32(hdr + 1, ic->creator);
    wr32(hdr + 5, ic->file_type);
    hdr[9] = ic->icon_type;
    wr32(hdr + 10, ic->tag);
    wr16(hdr + 14, ic->size);
    bool ok = fwrite(hdr, 1, sizeof(hdr), dt->icon_log) == sizeof(hdr);
    if (ok && ic->size && ic->bitmap)
        ok = fwrite(ic->bitmap, 1, ic->size, dt->icon_log) == ic->size;
    if (!ok) {
        LOG(1, "AFP desktop: icon log write failed (%s)", strerror(errno));
        fclose(dt->icon_log);
        dt->icon_log = NULL;
        return;
    }
    fflush(dt->icon_log);
    dt->icon_records++;
}

// Slot index for (creator, type, icon_type), or -1.
static long icon_slot(afp_desktop_t *dt, uint32_t creator, uint32_t file_type, uint8_t icon_type) {
    for (size_t i = 0; i < dt->icon_len; i++) {
        if (dt->icons[i].dead)
            continue;
        if (dt->icons[i].v.creator == creator && dt->icons[i].v.file_type == file_type &&
            dt->icons[i].v.icon_type == icon_type)
            return (long)i;
    }
    return -1;
}

// Apply an icon record to the in-memory table (no log write).
static int icon_apply(afp_desktop_t *dt, uint8_t op, uint32_t creator, uint32_t file_type, uint8_t icon_type,
                      uint32_t tag, const uint8_t *bitmap, uint16_t size) {
    long si = icon_slot(dt, creator, file_type, icon_type);
    if (op == DT_OP_DEL) {
        if (si >= 0)
            dt->icons[si].dead = true;
        return 0;
    }
    if (size > AFP_ICON_MAX_BYTES)
        return -EINVAL;
    if (si < 0) {
        if (dt->icon_len == dt->icon_cap) {
            size_t cap = dt->icon_cap ? dt->icon_cap * 2 : 16;
            icon_slot_t *tmp = (icon_slot_t *)realloc(dt->icons, cap * sizeof(icon_slot_t));
            if (!tmp)
                return -ENOMEM;
            dt->icons = tmp;
            dt->icon_cap = cap;
        }
        si = (long)dt->icon_len++;
        memset(&dt->icons[si], 0, sizeof(dt->icons[si]));
    }
    icon_slot_t *s = &dt->icons[si];
    s->dead = false;
    s->v.creator = creator;
    s->v.file_type = file_type;
    s->v.icon_type = icon_type;
    s->v.tag = tag;
    s->v.size = size;
    if (size && bitmap)
        memcpy(s->bytes, bitmap, size);
    s->v.bitmap = s->bytes;
    return 0;
}

// Replay the icon log.  A truncated tail record simply ends the replay.
static void icon_load(afp_desktop_t *dt) {
    FILE *f = fopen(dt->icon_path, "rb");
    if (!f)
        return;
    uint8_t magic[4];
    if (fread(magic, 1, 4, f) != 4 || rd32(magic) != DT_ICON_MAGIC) {
        fclose(f);
        LOG(1, "AFP desktop: '%s' unreadable — starting a fresh icon store", dt->icon_path);
        remove(dt->icon_path);
        return;
    }
    for (;;) {
        uint8_t hdr[DT_ICON_FIXED];
        if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
            break;
        uint16_t size = rd16(hdr + 14);
        if (size > AFP_ICON_MAX_BYTES)
            break;
        uint8_t bitmap[AFP_ICON_MAX_BYTES];
        if (size && fread(bitmap, 1, size, f) != size)
            break;
        icon_apply(dt, hdr[0], rd32(hdr + 1), rd32(hdr + 5), hdr[9], rd32(hdr + 10), bitmap, size);
        dt->icon_records++;
    }
    fclose(f);
}

// --- APPL store ------------------------------------------------------------

// appl record: op(1) creator(4) cnid(4) tag(4)
#define DT_APPL_FIXED 13

static void appl_log_append(afp_desktop_t *dt, uint8_t op, const afp_appl_t *a) {
    if (!dt->appl_log)
        return;
    uint8_t rec[DT_APPL_FIXED];
    rec[0] = op;
    wr32(rec + 1, a->creator);
    wr32(rec + 5, a->cnid);
    wr32(rec + 9, a->tag);
    if (fwrite(rec, 1, sizeof(rec), dt->appl_log) != sizeof(rec)) {
        LOG(1, "AFP desktop: APPL log write failed (%s)", strerror(errno));
        fclose(dt->appl_log);
        dt->appl_log = NULL;
        return;
    }
    fflush(dt->appl_log);
    dt->appl_records++;
}

static long appl_slot(afp_desktop_t *dt, uint32_t creator, uint32_t cnid) {
    for (size_t i = 0; i < dt->appl_len; i++) {
        if (dt->appls[i].dead)
            continue;
        if (dt->appls[i].v.creator == creator && dt->appls[i].v.cnid == cnid)
            return (long)i;
    }
    return -1;
}

static int appl_apply(afp_desktop_t *dt, uint8_t op, uint32_t creator, uint32_t cnid, uint32_t tag) {
    long si = appl_slot(dt, creator, cnid);
    if (op == DT_OP_DEL) {
        if (cnid == 0) {
            for (size_t i = 0; i < dt->appl_len; i++)
                if (!dt->appls[i].dead && dt->appls[i].v.creator == creator)
                    dt->appls[i].dead = true;
        } else if (si >= 0) {
            dt->appls[si].dead = true;
        }
        return 0;
    }
    if (si < 0) {
        if (dt->appl_len == dt->appl_cap) {
            size_t cap = dt->appl_cap ? dt->appl_cap * 2 : 16;
            appl_slot_t *tmp = (appl_slot_t *)realloc(dt->appls, cap * sizeof(appl_slot_t));
            if (!tmp)
                return -ENOMEM;
            dt->appls = tmp;
            dt->appl_cap = cap;
        }
        si = (long)dt->appl_len++;
        memset(&dt->appls[si], 0, sizeof(dt->appls[si]));
    }
    dt->appls[si].dead = false;
    dt->appls[si].v.creator = creator;
    dt->appls[si].v.cnid = cnid;
    dt->appls[si].v.tag = tag;
    return 0;
}

static void appl_load(afp_desktop_t *dt) {
    FILE *f = fopen(dt->appl_path, "rb");
    if (!f)
        return;
    uint8_t magic[4];
    if (fread(magic, 1, 4, f) != 4 || rd32(magic) != DT_APPL_MAGIC) {
        fclose(f);
        LOG(1, "AFP desktop: '%s' unreadable — starting a fresh APPL store", dt->appl_path);
        remove(dt->appl_path);
        return;
    }
    for (;;) {
        uint8_t rec[DT_APPL_FIXED];
        if (fread(rec, 1, sizeof(rec), f) != sizeof(rec))
            break;
        appl_apply(dt, rec[0], rd32(rec + 1), rd32(rec + 5), rd32(rec + 9));
        dt->appl_records++;
    }
    fclose(f);
}

// --- store lifecycle -------------------------------------------------------

// Open a store for append, writing its magic first when the file is new.
static FILE *store_open(const char *path, uint32_t magic) {
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
    } else {
        f = fopen(path, "wb");
        if (!f) {
            LOG(1, "AFP desktop: cannot create '%s' (%s)", path, strerror(errno));
            return NULL;
        }
        uint8_t m[4];
        wr32(m, magic);
        fwrite(m, 1, 4, f);
        fclose(f);
    }
    return fopen(path, "ab");
}

afp_desktop_t *afp_desktop_open(const char *host_root) {
    if (!host_root || !*host_root)
        return NULL;
    afp_desktop_t *dt = (afp_desktop_t *)calloc(1, sizeof(*dt));
    if (!dt)
        return NULL;
    char ctrl[PATH_MAX];
    if (snprintf(ctrl, sizeof(ctrl), "%s/%s", host_root, AFP_CONTROL_DIR) >= (int)sizeof(ctrl)) {
        free(dt);
        return NULL;
    }
    mkdir(ctrl, 0755);
    if ((size_t)snprintf(dt->icon_path, sizeof(dt->icon_path), "%s/desktop.icons", ctrl) >= sizeof(dt->icon_path) ||
        (size_t)snprintf(dt->appl_path, sizeof(dt->appl_path), "%s/desktop.appl", ctrl) >= sizeof(dt->appl_path)) {
        free(dt);
        return NULL;
    }
    icon_load(dt);
    appl_load(dt);
    dt->icon_log = store_open(dt->icon_path, DT_ICON_MAGIC);
    dt->appl_log = store_open(dt->appl_path, DT_APPL_MAGIC);
    return dt;
}

// Count live records in a store.
static size_t icon_live(const afp_desktop_t *dt) {
    size_t n = 0;
    for (size_t i = 0; i < dt->icon_len; i++)
        if (!dt->icons[i].dead)
            n++;
    return n;
}
static size_t appl_live(const afp_desktop_t *dt) {
    size_t n = 0;
    for (size_t i = 0; i < dt->appl_len; i++)
        if (!dt->appls[i].dead)
            n++;
    return n;
}

// Rewrite each store as pure PUTs of its live records.
static void desktop_compact(afp_desktop_t *dt) {
    char tmp[PATH_MAX];
    if (dt->icon_log && snprintf(tmp, sizeof(tmp), "%s.tmp", dt->icon_path) < (int)sizeof(tmp)) {
        FILE *f = fopen(tmp, "wb");
        if (f) {
            uint8_t m[4];
            wr32(m, DT_ICON_MAGIC);
            bool ok = fwrite(m, 1, 4, f) == 4;
            for (size_t i = 0; ok && i < dt->icon_len; i++) {
                if (dt->icons[i].dead)
                    continue;
                uint8_t hdr[DT_ICON_FIXED];
                hdr[0] = DT_OP_PUT;
                wr32(hdr + 1, dt->icons[i].v.creator);
                wr32(hdr + 5, dt->icons[i].v.file_type);
                hdr[9] = dt->icons[i].v.icon_type;
                wr32(hdr + 10, dt->icons[i].v.tag);
                wr16(hdr + 14, dt->icons[i].v.size);
                ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
                if (ok && dt->icons[i].v.size)
                    ok = fwrite(dt->icons[i].bytes, 1, dt->icons[i].v.size, f) == dt->icons[i].v.size;
            }
            if (fclose(f) != 0)
                ok = false;
            if (ok) {
                fclose(dt->icon_log);
                dt->icon_log = NULL;
                if (rename(tmp, dt->icon_path) != 0)
                    remove(tmp);
                dt->icon_log = fopen(dt->icon_path, "ab");
            } else {
                remove(tmp);
            }
        }
    }
    if (dt->appl_log && snprintf(tmp, sizeof(tmp), "%s.tmp", dt->appl_path) < (int)sizeof(tmp)) {
        FILE *f = fopen(tmp, "wb");
        if (f) {
            uint8_t m[4];
            wr32(m, DT_APPL_MAGIC);
            bool ok = fwrite(m, 1, 4, f) == 4;
            for (size_t i = 0; ok && i < dt->appl_len; i++) {
                if (dt->appls[i].dead)
                    continue;
                uint8_t rec[DT_APPL_FIXED];
                rec[0] = DT_OP_PUT;
                wr32(rec + 1, dt->appls[i].v.creator);
                wr32(rec + 5, dt->appls[i].v.cnid);
                wr32(rec + 9, dt->appls[i].v.tag);
                ok = fwrite(rec, 1, sizeof(rec), f) == sizeof(rec);
            }
            if (fclose(f) != 0)
                ok = false;
            if (ok) {
                fclose(dt->appl_log);
                dt->appl_log = NULL;
                if (rename(tmp, dt->appl_path) != 0)
                    remove(tmp);
                dt->appl_log = fopen(dt->appl_path, "ab");
            } else {
                remove(tmp);
            }
        }
    }
}

void afp_desktop_close(afp_desktop_t *dt) {
    if (!dt)
        return;
    size_t il = icon_live(dt), al = appl_live(dt);
    if (dt->icon_records > DT_COMPACT_FACTOR * (il + 1) || dt->appl_records > DT_COMPACT_FACTOR * (al + 1))
        desktop_compact(dt);
    if (dt->icon_log)
        fclose(dt->icon_log);
    if (dt->appl_log)
        fclose(dt->appl_log);
    free(dt->icons);
    free(dt->appls);
    free(dt);
}

// --- public icon API -------------------------------------------------------

int afp_desktop_put_icon(afp_desktop_t *dt, uint32_t creator, uint32_t file_type, uint8_t icon_type, uint32_t tag,
                         const uint8_t *bitmap, uint16_t size) {
    if (!dt)
        return -EINVAL;
    if (size > AFP_ICON_MAX_BYTES)
        return -EINVAL;
    int rc = icon_apply(dt, DT_OP_PUT, creator, file_type, icon_type, tag, bitmap, size);
    if (rc != 0)
        return rc;
    long si = icon_slot(dt, creator, file_type, icon_type);
    if (si >= 0)
        icon_log_append(dt, DT_OP_PUT, &dt->icons[si].v);
    return 0;
}

const afp_icon_t *afp_desktop_get_icon(afp_desktop_t *dt, uint32_t creator, uint32_t file_type, uint8_t icon_type) {
    if (!dt)
        return NULL;
    long si = icon_slot(dt, creator, file_type, icon_type);
    return si < 0 ? NULL : &dt->icons[si].v;
}

const afp_icon_t *afp_desktop_icon_at(afp_desktop_t *dt, uint32_t creator, uint16_t index) {
    if (!dt || index == 0)
        return NULL;
    uint16_t seen = 0;
    for (size_t i = 0; i < dt->icon_len; i++) {
        if (dt->icons[i].dead || dt->icons[i].v.creator != creator)
            continue;
        if (++seen == index)
            return &dt->icons[i].v;
    }
    return NULL;
}

// --- public APPL API -------------------------------------------------------

int afp_desktop_put_appl(afp_desktop_t *dt, uint32_t creator, uint32_t cnid, uint32_t tag) {
    if (!dt)
        return -EINVAL;
    int rc = appl_apply(dt, DT_OP_PUT, creator, cnid, tag);
    if (rc != 0)
        return rc;
    afp_appl_t a = {.creator = creator, .cnid = cnid, .tag = tag};
    appl_log_append(dt, DT_OP_PUT, &a);
    return 0;
}

int afp_desktop_remove_appl(afp_desktop_t *dt, uint32_t creator, uint32_t cnid) {
    if (!dt)
        return 0;
    int removed = 0;
    for (size_t i = 0; i < dt->appl_len; i++) {
        if (dt->appls[i].dead || dt->appls[i].v.creator != creator)
            continue;
        if (cnid != 0 && dt->appls[i].v.cnid != cnid)
            continue;
        removed++;
    }
    if (!removed)
        return 0;
    afp_appl_t a = {.creator = creator, .cnid = cnid, .tag = 0};
    appl_apply(dt, DT_OP_DEL, creator, cnid, 0);
    appl_log_append(dt, DT_OP_DEL, &a);
    return removed;
}

const afp_appl_t *afp_desktop_appl_at(afp_desktop_t *dt, uint32_t creator, uint16_t index) {
    if (!dt || index == 0)
        return NULL;
    uint16_t seen = 0;
    for (size_t i = 0; i < dt->appl_len; i++) {
        if (dt->appls[i].dead || dt->appls[i].v.creator != creator)
            continue;
        if (++seen == index)
            return &dt->appls[i].v;
    }
    return NULL;
}

void afp_desktop_prune_appls(afp_desktop_t *dt, bool (*alive)(uint32_t cnid, void *ud), void *ud) {
    if (!dt || !alive)
        return;
    for (size_t i = 0; i < dt->appl_len; i++) {
        if (dt->appls[i].dead)
            continue;
        if (alive(dt->appls[i].v.cnid, ud))
            continue;
        afp_appl_t a = dt->appls[i].v;
        dt->appls[i].dead = true;
        appl_log_append(dt, DT_OP_DEL, &a);
    }
}
