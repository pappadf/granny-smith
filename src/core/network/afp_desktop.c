// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// afp_desktop.c
// Persistent per-volume desktop database (see afp_desktop.h).

#include "afp_desktop.h"
#include "common.h"

#include "afp_applog.h"
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

#define DT_ICON_MAGIC 0x47534932u // 'GSI2': afp_applog framing
#define DT_APPL_MAGIC 0x47534132u // 'GSA2'

// Record ops shared by both stores.
enum { DT_OP_PUT = 1, DT_OP_DEL = 2 };

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
    icon_slot_t *icons;
    size_t icon_len, icon_cap;
    appl_slot_t *appls;
    size_t appl_len, appl_cap;
    afp_applog_t *icon_log; // .gs-afp/desktop.icons
    afp_applog_t *appl_log; // .gs-afp/desktop.appl
};

// --- big-endian helpers ----------------------------------------------------

// --- icon store ------------------------------------------------------------

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

// The caller's view of a slot.  The bitmap pointer is set here, on every
// hand-out, because the slot's bytes move whenever the table grows: set once
// at store time, it dangled for every icon stored before the 17th
// (10-network F-03).  The catalog's view_of works the same way.
static const afp_icon_t *icon_view(afp_desktop_t *dt, size_t si) {
    icon_slot_t *s = &dt->icons[si];
    s->v.bitmap = s->bytes;
    return &s->v;
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

// Apply an icon record to the in-memory table (no log write).
static int icon_apply(afp_desktop_t *dt, uint8_t op, uint32_t creator, uint32_t file_type, uint8_t icon_type,
                      uint32_t tag, const uint8_t *bitmap, uint16_t size) {
    long si = icon_slot(dt, creator, file_type, icon_type);
    if (op == DT_OP_DEL) {
        if (si >= 0)
            dt->icons[si].dead = true;
        return 0;
    }
    if (op != DT_OP_PUT)
        return 0; // a later format's record -- it was taken as a PUT (N-16)
    if (size > AFP_ICON_MAX_BYTES)
        return -EINVAL;
    if (si < 0) {
        if (icon_live(dt) >= AFP_MAX_ICONS)
            return -ENOSPC;
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
    return 0;
}

// icon record: creator(4) type(4) icon_type(1) tag(4), then the bitmap to
// the record's end.
#define DT_ICON_FIXED 13

static uint16_t icon_encode(uint8_t *buf, const afp_icon_t *ic) {
    WR_BE32(buf, ic->creator);
    WR_BE32(buf + 4, ic->file_type);
    buf[8] = ic->icon_type;
    WR_BE32(buf + 9, ic->tag);
    if (ic->size)
        memcpy(buf + DT_ICON_FIXED, ic->bitmap, ic->size);
    return (uint16_t)(DT_ICON_FIXED + ic->size);
}

static void icon_log_append(afp_desktop_t *dt, uint8_t op, const afp_icon_t *ic) {
    uint8_t buf[DT_ICON_FIXED + AFP_ICON_MAX_BYTES];
    afp_applog_append(dt->icon_log, op, buf, icon_encode(buf, ic), icon_live(dt));
}

static void icon_replay(void *ctx, uint8_t op, const uint8_t *p, uint16_t len) {
    if (len < DT_ICON_FIXED || (unsigned)(len - DT_ICON_FIXED) > AFP_ICON_MAX_BYTES)
        return;
    icon_apply((afp_desktop_t *)ctx, op, RD_BE32(p), RD_BE32(p + 4), p[8], RD_BE32(p + 9), p + DT_ICON_FIXED,
               (uint16_t)(len - DT_ICON_FIXED));
}

static bool icon_dump(void *ctx, afp_applog_t *log) {
    afp_desktop_t *dt = (afp_desktop_t *)ctx;
    uint8_t buf[DT_ICON_FIXED + AFP_ICON_MAX_BYTES];
    for (size_t i = 0; i < dt->icon_len; i++)
        if (!dt->icons[i].dead && !afp_applog_emit(log, DT_OP_PUT, buf, icon_encode(buf, icon_view(dt, i))))
            return false;
    return true;
}

// --- APPL store ------------------------------------------------------------

// appl record: creator(4) cnid(4) tag(4)
#define DT_APPL_FIXED 12

static void appl_encode(uint8_t *buf, const afp_appl_t *a) {
    WR_BE32(buf, a->creator);
    WR_BE32(buf + 4, a->cnid);
    WR_BE32(buf + 8, a->tag);
}

static void appl_log_append(afp_desktop_t *dt, uint8_t op, const afp_appl_t *a) {
    uint8_t buf[DT_APPL_FIXED];
    appl_encode(buf, a);
    afp_applog_append(dt->appl_log, op, buf, sizeof(buf), appl_live(dt));
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
    if (op != DT_OP_PUT)
        return 0; // a later format's record
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

static void appl_replay(void *ctx, uint8_t op, const uint8_t *p, uint16_t len) {
    if (len >= DT_APPL_FIXED)
        appl_apply((afp_desktop_t *)ctx, op, RD_BE32(p), RD_BE32(p + 4), RD_BE32(p + 8));
}

static bool appl_dump(void *ctx, afp_applog_t *log) {
    afp_desktop_t *dt = (afp_desktop_t *)ctx;
    uint8_t buf[DT_APPL_FIXED];
    for (size_t i = 0; i < dt->appl_len; i++) {
        if (dt->appls[i].dead)
            continue;
        appl_encode(buf, &dt->appls[i].v);
        if (!afp_applog_emit(log, DT_OP_PUT, buf, sizeof(buf)))
            return false;
    }
    return true;
}

// --- store lifecycle -------------------------------------------------------

afp_desktop_t *afp_desktop_open(const char *host_root) {
    if (!host_root || !*host_root)
        return NULL;
    afp_desktop_t *dt = (afp_desktop_t *)calloc(1, sizeof(*dt));
    if (!dt)
        return NULL;
    char path[PATH_MAX];
    if (!afp_meta_control_path(host_root, "desktop.icons", path, sizeof(path))) {
        free(dt);
        return NULL;
    }
    dt->icon_log = afp_applog_open(path, DT_ICON_MAGIC, icon_replay, icon_dump, dt, NULL);
    if (afp_meta_control_path(host_root, "desktop.appl", path, sizeof(path)))
        dt->appl_log = afp_applog_open(path, DT_APPL_MAGIC, appl_replay, appl_dump, dt, NULL);
    return dt;
}

void afp_desktop_close(afp_desktop_t *dt) {
    if (!dt)
        return;
    afp_applog_close(dt->icon_log, icon_live(dt));
    afp_applog_close(dt->appl_log, appl_live(dt));
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
        icon_log_append(dt, DT_OP_PUT, icon_view(dt, (size_t)si));
    return 0;
}

const afp_icon_t *afp_desktop_get_icon(afp_desktop_t *dt, uint32_t creator, uint32_t file_type, uint8_t icon_type) {
    if (!dt)
        return NULL;
    long si = icon_slot(dt, creator, file_type, icon_type);
    return si < 0 ? NULL : icon_view(dt, (size_t)si);
}

const afp_icon_t *afp_desktop_icon_at(afp_desktop_t *dt, uint32_t creator, uint16_t index) {
    if (!dt || index == 0)
        return NULL;
    uint16_t seen = 0;
    for (size_t i = 0; i < dt->icon_len; i++) {
        if (dt->icons[i].dead || dt->icons[i].v.creator != creator)
            continue;
        if (++seen == index)
            return icon_view(dt, i);
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
