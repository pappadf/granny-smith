// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_config.c
// The machine-description tree (catalog.profile), the default configuration
// (catalog.default_config) and the boot document's resolution into
// construction arguments.  See machine_config.h.
//
// One rule runs through all of it: the core owns every hardware fact and
// every user-visible string about hardware.  The tree says what can be
// configured and what each thing is called; the document says what this
// machine has; a frontend renders the one and edits the other without
// knowing any machine, bus or card.

#include "machine_config.h"

#include "machine_profile.h"
#include "monitor_catalog.h"
#include "nubus.h"
#include "prom.h"
#include "value.h"
#include "vrom.h"
#include "nubus/card.h"
#include "pci/pci.h"
#include "pci/pci_card.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Small helpers
// ============================================================================

// A list under construction.
typedef struct {
    value_t *items;
    size_t n, cap;
} vlist_t;

static void vl_push(vlist_t *l, value_t v) {
    if (!val_list_push(&l->items, &l->n, &l->cap, v))
        value_free(&v);
}

static value_t vl_finish(vlist_t *l) {
    value_t v = val_list(l->items, l->n);
    *l = (vlist_t){0};
    return v;
}

// {id, label[, detail]} -- the shape every choice in the tree has.
static value_t id_label(const char *id, const char *label, const char *detail) {
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "id", val_str(id ? id : ""));
    val_map_put(b, "label", val_str(label ? label : ""));
    if (detail && *detail)
        val_map_put(b, "detail", val_str(detail));
    return val_map_finish(b);
}

// The string member `key` of map `m`, or NULL when absent or not a string.
static const char *map_str(const value_t *m, const char *key) {
    const value_t *v = m ? value_map_get(m, key) : NULL;
    return (v && v->kind == V_STRING) ? v->s : NULL;
}

// Copy into a fixed buffer; false when it does not fit.
static bool copy_str(char *dst, size_t size, const char *src) {
    return snprintf(dst, size, "%s", src ? src : "") < (int)size;
}

// "8 MB", "1.5 MB", "512 KB".
static void memory_label(uint32_t kb, char *buf, size_t len) {
    if (kb < 1024) {
        snprintf(buf, len, "%u KB", kb);
    } else if (kb % 1024 == 0) {
        snprintf(buf, len, "%u MB", kb / 1024);
    } else {
        // One decimal is all a real SIMM total needs (1.5 MB, 2.5 MB, 9.5 MB).
        unsigned tenths = (unsigned)((kb * 10u + 512u) / 1024u);
        snprintf(buf, len, "%u.%u MB", tenths / 10, tenths % 10);
    }
}

// ============================================================================
// Options
// ============================================================================
//
// Memory comes from the profile's RAM list, AppleTalk from the substrate's
// seeding, the rest from the family's own declarations.  Each is presented
// through the same shape, so the dialog and the document treat them alike.

static const config_value_decl_t k_appletalk_values[] = {
    {.id = "active", .label = "Active"},
    {.id = "inactive", .label = "Inactive"},
    {.id = NULL},
};

// AppleTalk on/off: every Macintosh, default Active (decision D22).
static const config_option_decl_t k_appletalk_option = {
    .id = "appletalk",
    .label = "AppleTalk",
    .values = k_appletalk_values,
    .default_value = "active",
};

// The family's declared options plus AppleTalk, in display order (memory is
// separate: its values are computed).  Writes up to `max` and returns the
// count.
static int scalar_options(const hw_profile_t *p, const config_option_decl_t **out, int max) {
    int n = 0;
    for (const config_option_decl_t *o = p->options; o && o->id && n < max; o++)
        out[n++] = o;
    if (p->appletalk && n < max)
        out[n++] = &k_appletalk_option;
    return n;
}

static bool option_has_value(const config_option_decl_t *o, const char *v) {
    for (const config_value_decl_t *x = o->values; x && x->id; x++) {
        if (strcmp(x->id, v) == 0)
            return true;
    }
    return false;
}

// The memory option as the tree shows it.
static value_t memory_option_value(const hw_profile_t *p) {
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "id", val_str("memory"));
    val_map_put(b, "label", val_str("Memory"));
    val_map_put(b, "kind", val_str("choice"));
    vlist_t vals = {0};
    for (const uint32_t *r = p->ram_options; r && *r; r++) {
        char id[16], label[24];
        snprintf(id, sizeof id, "%u", *r);
        memory_label(*r, label, sizeof label);
        vl_push(&vals, id_label(id, label, NULL));
    }
    val_map_put(b, "values", vl_finish(&vals));
    char def[16];
    snprintf(def, sizeof def, "%u", p->ram_default / 1024u);
    val_map_put(b, "default", val_str(def));
    return val_map_finish(b);
}

static value_t option_value(const config_option_decl_t *o) {
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "id", val_str(o->id));
    val_map_put(b, "label", val_str(o->label));
    if (o->detail)
        val_map_put(b, "detail", val_str(o->detail));
    val_map_put(b, "kind", val_str("choice"));
    vlist_t vals = {0};
    for (const config_value_decl_t *v = o->values; v && v->id; v++)
        vl_push(&vals, id_label(v->id, v->label, v->detail));
    val_map_put(b, "values", vl_finish(&vals));
    val_map_put(b, "default", val_str(o->default_value));
    if (o->requires_value) {
        value_map_builder_t *r = val_map_new();
        val_map_put(r, "value", val_str(o->requires_value));
        val_map_put(r, "option", val_str(o->requires_option));
        val_map_put(r, "not", val_str(o->requires_not));
        val_map_put(b, "requires", val_map_finish(r));
    }
    return val_map_finish(b);
}

// ============================================================================
// Floppy positions
// ============================================================================

static const char *floppy_type_id(floppy_kind_t k) {
    return floppy_kind_to_string(k);
}

static const char *floppy_type_label(floppy_kind_t k) {
    switch (k) {
    case FLOPPY_400K:
        return "400K drive";
    case FLOPPY_800K:
        return "800K drive";
    case FLOPPY_HD:
        return "SuperDrive (1.4 MB)";
    }
    return "";
}

static int floppy_positions(const hw_profile_t *p) {
    int n = 0;
    for (const struct floppy_slot *s = p->floppy_slots; s && s->label; s++)
        n++;
    return n;
}

static const char *floppy_default_type(const struct floppy_slot *s) {
    return (s->optional && s->default_none) ? "none" : floppy_type_id(s->kind);
}

// ============================================================================
// Storage
// ============================================================================

const storage_bus_decl_t *machine_storage_bus(const hw_profile_t *p, const char *id) {
    for (const storage_bus_decl_t *b = p->storage; id && b && b->id; b++) {
        if (strcmp(b->id, id) == 0)
            return b;
    }
    return NULL;
}

bool machine_storage_media_bay(const hw_profile_t *p, const char *bus_id, int unit, media_bay_t *out) {
    const storage_bus_decl_t *b = machine_storage_bus(p, bus_id);
    if (!b)
        return false;
    *out = (media_bay_t){.bus = (media_bus_t)b->media_bus, .unit = unit + b->media_unit_base, .label = b->label};
    return true;
}

static const char *storage_kind_id(storage_kind_t k) {
    switch (k) {
    case STORAGE_KIND_SCSI:
        return "scsi";
    case STORAGE_KIND_ATA:
        return "ata";
    case STORAGE_KIND_PROFILE:
        return "profile";
    }
    return "scsi";
}

static const char *storage_bay_name(const storage_bus_decl_t *b, int unit) {
    for (const storage_bay_decl_t *y = b->bays; y && y->label; y++) {
        if (y->unit == unit)
            return y->label;
    }
    return NULL;
}

// A unit's position text: "ID 0 · Internal hard disk bay",
// "ID 3 · External", "ID 4"; "Master · Hard disk bay" on ATA; the bay name
// alone on the ProFile port.
static void storage_position(const storage_bus_decl_t *b, int unit, char *buf, size_t len);

bool machine_storage_position(const hw_profile_t *p, const char *bus_id, int unit, char *buf, size_t len) {
    const storage_bus_decl_t *b = machine_storage_bus(p, bus_id);
    if (!b)
        return false;
    storage_position(b, unit, buf, len);
    return true;
}

static void storage_position(const storage_bus_decl_t *b, int unit, char *buf, size_t len) {
    const char *bay = storage_bay_name(b, unit);
    const char *where = bay ? bay : (b->external_connector ? "External" : NULL);
    switch (b->kind) {
    case STORAGE_KIND_PROFILE:
        snprintf(buf, len, "%s", where ? where : "External");
        return;
    case STORAGE_KIND_ATA: {
        const char *who = unit == 0 ? "Master" : "Slave";
        if (bay)
            snprintf(buf, len, "%s \xc2\xb7 %s", who, bay);
        else
            snprintf(buf, len, "%s", who);
        return;
    }
    case STORAGE_KIND_SCSI:
        if (where)
            snprintf(buf, len, "ID %d \xc2\xb7 %s", unit, where);
        else
            snprintf(buf, len, "ID %d", unit);
        return;
    }
}

static const char *storage_type_id(unsigned type) {
    return type == STORAGE_DEV_CD ? "cd" : "hd";
}

static const char *storage_type_label(unsigned type) {
    return type == STORAGE_DEV_CD ? "CD-ROM drive" : "Hard disk";
}

static bool storage_type_parse(const char *s, unsigned *out) {
    if (s && strcmp(s, "hd") == 0)
        return (*out = STORAGE_DEV_HD), true;
    if (s && strcmp(s, "cd") == 0)
        return (*out = STORAGE_DEV_CD), true;
    return false;
}

static int unit_count(const storage_bus_decl_t *b) {
    return b->kind == STORAGE_KIND_SCSI ? (b->wide ? 16 : 8) : b->kind == STORAGE_KIND_ATA ? 2 : 1;
}

static value_t storage_bus_value(const storage_bus_decl_t *b) {
    value_map_builder_t *m = val_map_new();
    val_map_put(m, "id", val_str(b->id));
    val_map_put(m, "label", val_str(b->label));
    if (b->detail)
        val_map_put(m, "detail", val_str(b->detail));
    val_map_put(m, "kind", val_str(storage_kind_id(b->kind)));
    if (b->kind == STORAGE_KIND_SCSI)
        val_map_put(m, "width", val_str(b->wide ? "wide" : "narrow"));
    // Every unit a device may use, with the position text the core composes.
    vlist_t units = {0}, reserved = {0}, bays = {0}, accepts = {0}, shares = {0};
    for (int u = 0; u < unit_count(b); u++) {
        if (b->reserved & (1u << u)) {
            vl_push(&reserved, val_int(u));
            continue;
        }
        if (!(b->units & (1u << u)))
            continue;
        char pos[64];
        storage_position(b, u, pos, sizeof pos);
        value_map_builder_t *ub = val_map_new();
        val_map_put(ub, "unit", val_int(u));
        val_map_put(ub, "label", val_str(pos));
        vl_push(&units, val_map_finish(ub));
    }
    for (const storage_bay_decl_t *y = b->bays; y && y->label; y++) {
        value_map_builder_t *yb = val_map_new();
        val_map_put(yb, "unit", val_int(y->unit));
        val_map_put(yb, "label", val_str(y->label));
        vl_push(&bays, val_map_finish(yb));
    }
    if (b->accepts & STORAGE_DEV_HD)
        vl_push(&accepts, id_label("hd", storage_type_label(STORAGE_DEV_HD), NULL));
    if (b->accepts & STORAGE_DEV_CD)
        vl_push(&accepts, id_label("cd", storage_type_label(STORAGE_DEV_CD), NULL));
    if (b->shares_units_with)
        vl_push(&shares, val_str(b->shares_units_with));
    val_map_put(m, "units", vl_finish(&units));
    val_map_put(m, "reserved", vl_finish(&reserved));
    val_map_put(m, "shares_units_with", vl_finish(&shares));
    val_map_put(m, "external_connector", val_bool(b->external_connector));
    val_map_put(m, "bays", vl_finish(&bays));
    val_map_put(m, "accepts", vl_finish(&accepts));
    val_map_put(m, "startup", val_bool(b->startup_ok));
    return val_map_finish(m);
}

// ============================================================================
// Slots and cards
// ============================================================================

void machine_slot_id(bool pci, int slot, char *buf, size_t len) {
    if (pci)
        snprintf(buf, len, "pci_%d", slot);
    else
        snprintf(buf, len, "nubus_%x", slot);
}

// Parse a slot id of this model's bus into its slot number.
static bool slot_id_parse(const hw_profile_t *p, const char *id, bool *pci, int *slot) {
    if (!id)
        return false;
    char buf[16];
    if (p->nubus_slots) {
        for (const nubus_slot_decl_t *d = p->nubus_slots; d->slot; d++) {
            if (d->kind != NUBUS_SLOT_SOCKET && d->kind != NUBUS_SLOT_BUILTIN)
                continue;
            machine_slot_id(false, d->slot, buf, sizeof buf);
            if (strcmp(buf, id) == 0)
                return (*pci = false), (*slot = d->slot), true;
        }
    }
    if (p->pci_slots) {
        for (const pci_slot_decl_t *d = p->pci_slots; d->slot; d++) {
            machine_slot_id(true, d->slot, buf, sizeof buf);
            if (strcmp(buf, id) == 0)
                return (*pci = true), (*slot = d->slot), true;
        }
    }
    return false;
}

// A socket of either bus, as the tree and the document see it.
typedef struct {
    bool pci;
    int slot;
    char id[16];
    const char *label;
    char detail[32];
    int fill_order;
    int excludes; // a slot number, 0 = none
    const char *default_card;
} socket_t;

#define SOCKETS_MAX 16

// Every user socket of the model, in declaration order.
static int model_sockets(const hw_profile_t *p, socket_t *out, int max) {
    int n = 0;
    for (const nubus_slot_decl_t *d = p->nubus_slots; d && d->slot && n < max; d++) {
        if (d->kind != NUBUS_SLOT_SOCKET)
            continue;
        socket_t *s = &out[n++];
        *s = (socket_t){.pci = false, .slot = d->slot, .fill_order = d->fill_order, .excludes = d->excludes};
        machine_slot_id(false, d->slot, s->id, sizeof s->id);
        s->label = d->label;
        snprintf(s->detail, sizeof s->detail, "Slot ID $%X", d->slot);
        s->default_card = d->default_card;
    }
    for (const pci_slot_decl_t *d = p->pci_slots; d && d->slot && n < max; d++) {
        if (d->kind != PCI_SLOT_SOCKET)
            continue;
        socket_t *s = &out[n++];
        *s = (socket_t){.pci = true, .slot = d->slot, .fill_order = d->fill_order};
        machine_slot_id(true, d->slot, s->id, sizeof s->id);
        s->label = d->label;
        snprintf(s->detail, sizeof s->detail, "%s", d->detail ? d->detail : "");
        s->default_card = d->default_card;
    }
    return n;
}

// The card class a kind presents: every NuBus kind with monitors is a
// display card; a PCI kind says so itself.
static const char *nubus_class(const nubus_card_kind_t *k) {
    return k->monitors ? "display" : "other";
}

static const char *pci_class(const pci_card_kind_t *k) {
    return k->card_class ? k->card_class : (k->monitors ? "display" : "other");
}

// The catalogue ids a monitor table covers, each once, in table order.
static int table_monitors(const nubus_monitor_t *t, const char **out, int max) {
    int n = 0;
    for (const nubus_monitor_t *m = t; m && m->id; m++) {
        if (!m->monitor)
            continue;
        bool seen = false;
        for (int i = 0; i < n; i++)
            seen |= strcmp(out[i], m->monitor) == 0;
        if (!seen && n < max)
            out[n++] = m->monitor;
    }
    return n;
}

#define MONITORS_MAX 24

// The video modes a NuBus card can be told to start up in, per monitor:
// {monitor: [{id: "WxHxD", label}]}.  Only the NuBus display cards seed a
// startup mode today (their slot PRAM record), so only they have modes.
static value_t card_modes_value(const nubus_monitor_t *t) {
    value_map_builder_t *b = val_map_new();
    const char *mons[MONITORS_MAX];
    int nm = table_monitors(t, mons, MONITORS_MAX);
    for (int i = 0; i < nm; i++) {
        const monitor_catalog_entry_t *cat = monitor_catalog_find(mons[i]);
        vlist_t modes = {0};
        for (const nubus_monitor_t *m = t; m && m->id; m++) {
            if (!m->monitor || strcmp(m->monitor, mons[i]) != 0)
                continue;
            for (const int *d = m->depths; d && *d; d++) {
                char id[32], label[64];
                snprintf(id, sizeof id, "%ux%ux%d", m->width, m->height, *d);
                monitor_mode_label(cat, m->width, m->height, *d, label, sizeof label);
                vl_push(&modes, id_label(id, label, NULL));
            }
        }
        val_map_put(b, mons[i], vl_finish(&modes));
    }
    return val_map_finish(b);
}

// One option of a PCI card, as the tree shows it.  The card's monitor is not
// one: the Monitor section owns it.
static value_t pci_option_value(const pci_card_option_t *o) {
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "id", val_str(o->key));
    val_map_put(b, "label", val_str(o->label ? o->label : o->key));
    val_map_put(b, "kind", val_str("choice"));
    vlist_t vals = {0};
    for (size_t i = 0; o->values && o->values[i]; i++) {
        if (o->value_offered && !o->value_offered(o->values[i]))
            continue;
        vl_push(&vals, id_label(o->values[i], (o->labels && o->labels[i]) ? o->labels[i] : o->values[i], NULL));
    }
    val_map_put(b, "values", vl_finish(&vals));
    val_map_put(b, "default", val_str(o->default_value ? o->default_value : ""));
    return val_map_finish(b);
}

// A card's availability with the ROMs offered now: ok, substitute
// (the emulator's generated declaration ROM stands in), or unavailable.
typedef enum { CARD_OK, CARD_SUBSTITUTE, CARD_UNAVAILABLE } card_status_t;

static card_status_t nubus_status(const nubus_card_kind_t *k) {
    if (!k->requires_vrom || vrom_card_resolvable(k->id, NULL))
        return CARD_OK;
    return k->substitute ? CARD_SUBSTITUTE : CARD_UNAVAILABLE;
}

static card_status_t pci_status(const pci_card_kind_t *k) {
    if (!k->requires_prom || prom_card_resolvable(k->id, NULL))
        return CARD_OK;
    return CARD_UNAVAILABLE;
}

static void put_status(value_map_builder_t *b, card_status_t st, bool pci) {
    static const char *const names[] = {"ok", "substitute", "unavailable"};
    val_map_put(b, "status", val_str(names[st]));
    if (st == CARD_SUBSTITUTE)
        val_map_put(b, "reason",
                    val_str("Using the emulator's substitute ROM \xe2\x80\x94 upload the card's ROM to use Apple's."));
    else if (st == CARD_UNAVAILABLE)
        val_map_put(
            b, "reason",
            val_str(pci ? "Needs the card's expansion ROM (.prom)" : "Needs the card's declaration ROM (.vrom)"));
}

static value_t monitor_ids_value(const char *const *ids, int n) {
    vlist_t l = {0};
    for (int i = 0; i < n; i++)
        vl_push(&l, val_str(ids[i]));
    return vl_finish(&l);
}

// Every card kind that fits at least one socket of the model, once.
static value_t cards_value(const hw_profile_t *p, const socket_t *socks, int ns) {
    vlist_t cards = {0};
    for (const nubus_card_kind_t *const *k = nubus_card_registry(); *k; k++) {
        vlist_t fits = {0};
        for (int i = 0; i < ns; i++) {
            if (socks[i].pci)
                continue;
            const nubus_slot_decl_t *d = NULL;
            for (const nubus_slot_decl_t *x = p->nubus_slots; x->slot; x++)
                if (x->slot == socks[i].slot)
                    d = x;
            if (d && nubus_card_fits_socket(d, *k))
                vl_push(&fits, val_str(socks[i].id));
        }
        if (!fits.n) {
            value_t tmp = vl_finish(&fits);
            value_free(&tmp);
            continue;
        }
        value_map_builder_t *b = val_map_new();
        val_map_put(b, "id", val_str((*k)->id));
        val_map_put(b, "label", val_str((*k)->display_name));
        val_map_put(b, "class", val_str(nubus_class(*k)));
        val_map_put(b, "fits", vl_finish(&fits));
        if ((*k)->requires_vrom) {
            value_map_builder_t *r = val_map_new();
            val_map_put(r, "kind", val_str("vrom"));
            val_map_put(r, "substitute", val_bool((*k)->substitute));
            val_map_put(b, "rom", val_map_finish(r));
        } else {
            val_map_put(b, "rom", val_none());
        }
        put_status(b, nubus_status(*k), false);
        const char *mons[MONITORS_MAX];
        int nm = table_monitors((*k)->monitors, mons, MONITORS_MAX);
        val_map_put(b, "monitors", monitor_ids_value(mons, nm));
        val_map_put(b, "modes", card_modes_value((*k)->monitors));
        val_map_put(b, "options", val_list(NULL, 0));
        vl_push(&cards, val_map_finish(b));
    }
    for (const pci_card_kind_t *const *k = pci_card_registry(); *k; k++) {
        if ((*k)->variant_of)
            continue; // reached through another card's option
        if ((*k)->offered && !(*k)->offered())
            continue;
        vlist_t fits = {0};
        for (int i = 0; i < ns; i++) {
            if (!socks[i].pci)
                continue;
            const pci_slot_decl_t *d = NULL;
            for (const pci_slot_decl_t *x = p->pci_slots; x->slot; x++)
                if (x->slot == socks[i].slot)
                    d = x;
            if (d && pci_card_fits_socket(d, *k))
                vl_push(&fits, val_str(socks[i].id));
        }
        if (!fits.n) {
            value_t tmp = vl_finish(&fits);
            value_free(&tmp);
            continue;
        }
        value_map_builder_t *b = val_map_new();
        val_map_put(b, "id", val_str((*k)->id));
        val_map_put(b, "label", val_str((*k)->display_name));
        val_map_put(b, "class", val_str(pci_class(*k)));
        val_map_put(b, "fits", vl_finish(&fits));
        if ((*k)->requires_prom) {
            value_map_builder_t *r = val_map_new();
            val_map_put(r, "kind", val_str("prom"));
            val_map_put(r, "substitute", val_bool(false));
            val_map_put(b, "rom", val_map_finish(r));
        } else {
            val_map_put(b, "rom", val_none());
        }
        put_status(b, pci_status(*k), true);
        const char *mons[MONITORS_MAX];
        int nm = table_monitors((*k)->monitors, mons, MONITORS_MAX);
        val_map_put(b, "monitors", monitor_ids_value(mons, nm));
        val_map_put(b, "modes", val_map_finish(val_map_new()));
        vlist_t opts = {0};
        for (const pci_card_option_t *o = (*k)->options; o && o->key; o++)
            vl_push(&opts, pci_option_value(o));
        val_map_put(b, "options", vl_finish(&opts));
        vl_push(&cards, val_map_finish(b));
    }
    return vl_finish(&cards);
}

static value_t slots_value(const socket_t *socks, int ns) {
    vlist_t l = {0};
    for (int i = 0; i < ns; i++) {
        value_map_builder_t *b = val_map_new();
        val_map_put(b, "id", val_str(socks[i].id));
        val_map_put(b, "label", val_str(socks[i].label ? socks[i].label : socks[i].id));
        if (socks[i].detail[0])
            val_map_put(b, "detail", val_str(socks[i].detail));
        val_map_put(b, "bus", val_str(socks[i].pci ? "pci" : "nubus"));
        val_map_put(b, "kind", val_str("socket"));
        vlist_t ex = {0};
        if (socks[i].excludes) {
            char id[16];
            machine_slot_id(false, socks[i].excludes, id, sizeof id);
            vl_push(&ex, val_str(id));
        }
        // An exclusion holds both ways: list it on the other slot too.
        for (int j = 0; j < ns; j++) {
            if (j != i && !socks[j].pci && socks[j].excludes == socks[i].slot && !socks[i].pci)
                vl_push(&ex, val_str(socks[j].id));
        }
        val_map_put(b, "excludes", vl_finish(&ex));
        // Apple's order first, then declaration order.
        val_map_put(b, "fill_order", val_int(socks[i].fill_order ? socks[i].fill_order : 100 + i));
        vl_push(&l, val_map_finish(b));
    }
    return vl_finish(&l);
}

// ============================================================================
// The built-in display device
// ============================================================================

// The machine's BUILTIN NuBus pseudo-slot holding its video, or NULL.
static const nubus_slot_decl_t *builtin_video_slot(const hw_profile_t *p) {
    for (const nubus_slot_decl_t *d = p->nubus_slots; d && d->slot; d++) {
        if (d->kind != NUBUS_SLOT_BUILTIN)
            continue;
        const nubus_card_kind_t *k = nubus_card_find(d->builtin_card_id);
        if (k && k->monitors)
            return d;
    }
    return NULL;
}

// The catalogue ids the built-in port takes, "none" left out.
static int builtin_monitors(const hw_profile_t *p, const char **out, int max) {
    const builtin_video_desc_t *bv = p->builtin_video;
    if (!bv)
        return 0;
    if (bv->slot_monitors) {
        const nubus_slot_decl_t *d = builtin_video_slot(p);
        const nubus_card_kind_t *k = d ? nubus_card_find(d->builtin_card_id) : NULL;
        return k ? table_monitors(k->monitors, out, max) : 0;
    }
    int n = 0;
    const char *id = NULL, *mon = NULL;
    for (size_t i = 0; bv->monitor_at && bv->monitor_at(i, &id, &mon) && n < max; i++) {
        if (strcmp(mon, MONITOR_NONE) != 0)
            out[n++] = mon;
    }
    return n;
}

// The default monitor of the built-in port.
static const char *builtin_default_monitor(const hw_profile_t *p) {
    const char *mons[MONITORS_MAX];
    int n = builtin_monitors(p, mons, MONITORS_MAX);
    const char *d = p->builtin_video ? p->builtin_video->default_monitor : NULL;
    for (int i = 0; d && i < n; i++)
        if (strcmp(mons[i], d) == 0)
            return d;
    return n ? mons[0] : NULL;
}

// The built-in port's startup modes on catalogue monitor `mon`, or NULL.
static const builtin_startup_t *builtin_startup_row(const hw_profile_t *p, const char *mon) {
    const builtin_video_desc_t *bv = p->builtin_video;
    for (const builtin_startup_t *r = bv ? bv->startup : NULL; r && r->monitor; r++)
        if (strcmp(r->monitor, mon) == 0)
            return r;
    return NULL;
}

// The built-in port's startup modes, by monitor, as the tree lists a card's.
static value_t builtin_modes_value(const hw_profile_t *p) {
    value_map_builder_t *b = val_map_new();
    for (const builtin_startup_t *r = p->builtin_video->startup; r && r->monitor; r++) {
        const monitor_catalog_entry_t *cat = monitor_catalog_find(r->monitor);
        vlist_t modes = {0};
        for (const builtin_startup_mode_t *m = r->modes; m->depth; m++) {
            char id[32], label[64];
            snprintf(id, sizeof id, "%ux%ux%d", r->width, r->height, m->depth);
            monitor_mode_label(cat, r->width, r->height, m->depth, label, sizeof label);
            vl_push(&modes, id_label(id, label, NULL));
        }
        val_map_put(b, r->monitor, vl_finish(&modes));
    }
    return val_map_finish(b);
}

static value_t displays_value(const hw_profile_t *p) {
    value_map_builder_t *b = val_map_new();
    if (p->builtin_video) {
        value_map_builder_t *d = val_map_new();
        val_map_put(d, "id", val_str("builtin"));
        val_map_put(d, "label", val_str("Built-in video"));
        if (p->builtin_video->detail)
            val_map_put(d, "detail", val_str(p->builtin_video->detail));
        const char *mons[MONITORS_MAX];
        int n = builtin_monitors(p, mons, MONITORS_MAX);
        val_map_put(d, "monitors", monitor_ids_value(mons, n));
        val_map_put(d, "default_monitor", val_str(builtin_default_monitor(p) ? builtin_default_monitor(p) : ""));
        val_map_put(d, "modes", builtin_modes_value(p));
        val_map_put(d, "options", val_list(NULL, 0));
        val_map_put(b, "builtin", val_map_finish(d));
    } else {
        val_map_put(b, "builtin", val_none());
    }
    val_map_put(b, "max_connected", val_int(1));
    return val_map_finish(b);
}

// The catalogue entries this model's devices can show, for their labels and
// geometry: every monitor any display device or fitting card lists.
static value_t monitors_value(const hw_profile_t *p, const socket_t *socks, int ns) {
    const char *ids[64];
    int n = 0;
    const char *mons[MONITORS_MAX];
    int nm = builtin_monitors(p, mons, MONITORS_MAX);
    for (int i = 0; i < nm && n < 64; i++)
        ids[n++] = mons[i];
    bool has_nubus = false, has_pci = false;
    for (int i = 0; i < ns; i++)
        socks[i].pci ? (has_pci = true) : (has_nubus = true);
    if (has_nubus) {
        for (const nubus_card_kind_t *const *k = nubus_card_registry(); *k; k++) {
            if ((*k)->attach != CARD_ATTACH_NUBUS)
                continue;
            nm = table_monitors((*k)->monitors, mons, MONITORS_MAX);
            for (int i = 0; i < nm && n < 64; i++)
                ids[n++] = mons[i];
        }
    }
    if (has_pci) {
        for (const pci_card_kind_t *const *k = pci_card_registry(); *k; k++) {
            if ((*k)->attach != PCI_ATTACH_PCI)
                continue;
            nm = table_monitors((*k)->monitors, mons, MONITORS_MAX);
            for (int i = 0; i < nm && n < 64; i++)
                ids[n++] = mons[i];
        }
    }
    vlist_t l = {0};
    for (const monitor_catalog_entry_t *m = monitor_catalog_all(); m->id; m++) {
        bool used = false;
        for (int i = 0; i < n; i++)
            used |= strcmp(ids[i], m->id) == 0;
        if (!used)
            continue;
        value_map_builder_t *b = val_map_new();
        val_map_put(b, "id", val_str(m->id));
        val_map_put(b, "label", val_str(m->label));
        val_map_put(b, "width", val_int(m->width));
        val_map_put(b, "height", val_int(m->height));
        vl_push(&l, val_map_finish(b));
    }
    return vl_finish(&l);
}

// ============================================================================
// The normalised document
// ============================================================================
//
// What a configuration document says once every absent node has taken its
// default.  Defaults, the legacy arguments and a caller's document all become
// one of these; it is validated and turned into build options from here.

typedef struct {
    bool pci;
    int slot;
    char card[32];
    int n_options;
    slot_option_t options[SLOT_OPTIONS_MAX];
} doc_card_t;

typedef struct {
    char device[16]; // "builtin" or a slot id
    char monitor[24]; // catalogue id, "none" when unplugged
    char mode[24]; // "WxHxD", "" = the device's own default
} doc_display_t;

typedef struct {
    uint32_t ram_kb;
    int n_options;
    machine_option_value_t options[MACHINE_OPTIONS_MAX];
    int n_floppies;
    char floppy[2][8];
    int n_storage;
    machine_storage_dev_t storage[MACHINE_STORAGE_MAX];
    machine_startup_t startup;
    // The cards: given by the document (cards_given), or left to the slots'
    // declared defaults and the legacy arguments.
    bool cards_given;
    int n_cards;
    doc_card_t cards[SOCKETS_MAX];
    int n_displays;
    doc_display_t displays[SOCKETS_MAX + 1];
} doc_t;

// The storage device the model's defaults put first of `type`, or -1.
static int first_of_type(const doc_t *d, unsigned type) {
    for (int i = 0; i < d->n_storage; i++)
        if (d->storage[i].type == type)
            return i;
    return -1;
}

// Point the startup device at the first hard disk, else at nothing.
static void default_startup(const hw_profile_t *p, doc_t *d) {
    int i = first_of_type(d, STORAGE_DEV_HD);
    if (i >= 0 && machine_storage_bus(p, d->storage[i].bus) && machine_storage_bus(p, d->storage[i].bus)->startup_ok) {
        d->startup = (machine_startup_t){.none = false, .unit = d->storage[i].unit, .type = STORAGE_DEV_HD};
        copy_str(d->startup.bus, sizeof d->startup.bus, d->storage[i].bus);
    } else {
        d->startup = (machine_startup_t){.none = true};
    }
}

// Is the card in this document entry a display device?
static bool doc_card_is_display(const doc_card_t *c) {
    if (c->pci) {
        const pci_card_kind_t *k = pci_card_find(c->card);
        return k && strcmp(pci_class(k), "display") == 0;
    }
    const nubus_card_kind_t *k = nubus_card_find(c->card);
    return k && k->monitors;
}

// The monitors a card offers (catalogue ids).
static int card_monitors(const doc_card_t *c, const char **out, int max) {
    const nubus_monitor_t *t = NULL;
    if (c->pci) {
        const pci_card_kind_t *k = pci_card_find(c->card);
        t = k ? k->monitors : NULL;
    } else {
        const nubus_card_kind_t *k = nubus_card_find(c->card);
        t = k ? k->monitors : NULL;
    }
    return table_monitors(t, out, max);
}

// The model's defaults.
static void doc_defaults(const hw_profile_t *p, doc_t *d) {
    memset(d, 0, sizeof *d);
    d->ram_kb = p->ram_default / 1024u;
    const config_option_decl_t *opts[MACHINE_OPTIONS_MAX];
    int no = scalar_options(p, opts, MACHINE_OPTIONS_MAX);
    for (int i = 0; i < no; i++) {
        copy_str(d->options[i].id, sizeof d->options[i].id, opts[i]->id);
        copy_str(d->options[i].value, sizeof d->options[i].value, opts[i]->default_value);
    }
    d->n_options = no;
    d->n_floppies = floppy_positions(p);
    for (int i = 0; i < d->n_floppies && i < 2; i++)
        copy_str(d->floppy[i], sizeof d->floppy[i], floppy_default_type(&p->floppy_slots[i]));
    for (const storage_device_decl_t *s = p->default_storage; s && s->bus && d->n_storage < MACHINE_STORAGE_MAX; s++) {
        machine_storage_dev_t *e = &d->storage[d->n_storage++];
        copy_str(e->bus, sizeof e->bus, s->bus);
        e->unit = s->unit;
        e->type = s->type;
    }
    default_startup(p, d);
    socket_t socks[SOCKETS_MAX];
    int ns = model_sockets(p, socks, SOCKETS_MAX);
    for (int i = 0; i < ns; i++) {
        if (!socks[i].default_card)
            continue;
        doc_card_t *c = &d->cards[d->n_cards++];
        *c = (doc_card_t){.pci = socks[i].pci, .slot = socks[i].slot};
        copy_str(c->card, sizeof c->card, socks[i].default_card);
    }
    d->cards_given = true;
    // Displays: one monitor, on the built-in video if there is one, else on
    // the first display card (D6).
    bool connected = false;
    if (p->builtin_video && builtin_default_monitor(p)) {
        doc_display_t *x = &d->displays[d->n_displays++];
        copy_str(x->device, sizeof x->device, "builtin");
        copy_str(x->monitor, sizeof x->monitor, builtin_default_monitor(p));
        connected = true;
    }
    for (int i = 0; i < d->n_cards; i++) {
        if (!doc_card_is_display(&d->cards[i]))
            continue;
        doc_display_t *x = &d->displays[d->n_displays++];
        machine_slot_id(d->cards[i].pci, d->cards[i].slot, x->device, sizeof x->device);
        const char *mons[MONITORS_MAX];
        int nm = card_monitors(&d->cards[i], mons, MONITORS_MAX);
        copy_str(x->monitor, sizeof x->monitor, (!connected && nm) ? mons[0] : MONITOR_NONE);
        if (!connected && nm)
            connected = true;
    }
}

// The document as a value (what catalog.default_config returns).
static value_t doc_value(const hw_profile_t *p, const doc_t *d) {
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "model", val_str(p->id));
    value_map_builder_t *o = val_map_new();
    char kb[16];
    snprintf(kb, sizeof kb, "%u", d->ram_kb);
    val_map_put(o, "memory", val_str(kb));
    for (int i = 0; i < d->n_options; i++)
        val_map_put(o, d->options[i].id, val_str(d->options[i].value));
    val_map_put(b, "options", val_map_finish(o));
    value_map_builder_t *f = val_map_new();
    for (int i = 0; i < d->n_floppies && i < 2; i++) {
        char id[16];
        snprintf(id, sizeof id, "fd%d", i);
        val_map_put(f, id, val_str(d->floppy[i]));
    }
    val_map_put(b, "floppies", val_map_finish(f));
    vlist_t st = {0};
    for (int i = 0; i < d->n_storage; i++) {
        value_map_builder_t *e = val_map_new();
        val_map_put(e, "bus", val_str(d->storage[i].bus));
        val_map_put(e, "unit", val_int(d->storage[i].unit));
        val_map_put(e, "type", val_str(storage_type_id(d->storage[i].type)));
        vl_push(&st, val_map_finish(e));
    }
    val_map_put(b, "storage", vl_finish(&st));
    if (d->startup.none || !d->startup.bus[0]) {
        val_map_put(b, "startup", val_none());
    } else {
        value_map_builder_t *s = val_map_new();
        val_map_put(s, "bus", val_str(d->startup.bus));
        val_map_put(s, "unit", val_int(d->startup.unit));
        val_map_put(b, "startup", val_map_finish(s));
    }
    vlist_t cards = {0};
    for (int i = 0; i < d->n_cards; i++) {
        value_map_builder_t *c = val_map_new();
        char id[16];
        machine_slot_id(d->cards[i].pci, d->cards[i].slot, id, sizeof id);
        val_map_put(c, "slot", val_str(id));
        val_map_put(c, "card", val_str(d->cards[i].card));
        value_map_builder_t *co = val_map_new();
        for (int k = 0; k < d->cards[i].n_options; k++)
            val_map_put(co, d->cards[i].options[k].key, val_str(d->cards[i].options[k].value));
        val_map_put(c, "options", val_map_finish(co));
        vl_push(&cards, val_map_finish(c));
    }
    val_map_put(b, "cards", vl_finish(&cards));
    value_map_builder_t *ds = val_map_new();
    for (int i = 0; i < d->n_displays; i++) {
        value_map_builder_t *x = val_map_new();
        val_map_put(x, "monitor", val_str(d->displays[i].monitor));
        if (d->displays[i].mode[0])
            val_map_put(x, "mode", val_str(d->displays[i].mode));
        val_map_put(ds, d->displays[i].device, val_map_finish(x));
    }
    val_map_put(b, "displays", val_map_finish(ds));
    return val_map_finish(b);
}

value_t machine_config_defaults(const hw_profile_t *p) {
    doc_t *d = calloc(1, sizeof *d);
    if (!d)
        return val_err("catalog.default_config: out of memory");
    doc_defaults(p, d);
    value_t v = doc_value(p, d);
    free(d);
    return v;
}

void machine_config_put_tree(const hw_profile_t *p, value_map_builder_t *b) {
    vlist_t opts = {0};
    vl_push(&opts, memory_option_value(p));
    const config_option_decl_t *decl[MACHINE_OPTIONS_MAX];
    int no = scalar_options(p, decl, MACHINE_OPTIONS_MAX);
    for (int i = 0; i < no; i++)
        vl_push(&opts, option_value(decl[i]));
    val_map_put(b, "options", vl_finish(&opts));

    vlist_t flops = {0};
    int i = 0;
    for (const struct floppy_slot *s = p->floppy_slots; s && s->label; s++, i++) {
        value_map_builder_t *fb = val_map_new();
        char id[16];
        snprintf(id, sizeof id, "fd%d", i);
        val_map_put(fb, "id", val_str(id));
        val_map_put(fb, "label", val_str(s->label));
        vlist_t types = {0};
        if (s->optional)
            vl_push(&types, id_label("none", "None", NULL));
        vl_push(&types, id_label(floppy_type_id(s->kind), floppy_type_label(s->kind), NULL));
        val_map_put(fb, "types", vl_finish(&types));
        val_map_put(fb, "default", val_str(floppy_default_type(s)));
        vl_push(&flops, val_map_finish(fb));
    }
    val_map_put(b, "floppies", vl_finish(&flops));

    vlist_t buses = {0};
    for (const storage_bus_decl_t *s = p->storage; s && s->id; s++)
        vl_push(&buses, storage_bus_value(s));
    val_map_put(b, "storage", vl_finish(&buses));

    socket_t socks[SOCKETS_MAX];
    int ns = model_sockets(p, socks, SOCKETS_MAX);
    val_map_put(b, "slots", slots_value(socks, ns));
    val_map_put(b, "cards", cards_value(p, socks, ns));
    val_map_put(b, "displays", displays_value(p));
    val_map_put(b, "monitors", monitors_value(p, socks, ns));
    val_map_put(b, "defaults", machine_config_defaults(p));
}

// ============================================================================
// Reading a document
// ============================================================================

// Fail with a message naming the node.
static value_t bad(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static value_t bad(const char *fmt, ...) {
    char msg[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    return val_err("machine.boot: %s", msg);
}

// The integer member `key`, accepting a JSON number.
static bool map_int(const value_t *m, const char *key, int64_t *out) {
    const value_t *v = m ? value_map_get(m, key) : NULL;
    if (!v)
        return false;
    if (v->kind == V_INT)
        return (*out = v->i), true;
    if (v->kind == V_UINT)
        return (*out = (int64_t)v->u), true;
    return false;
}

// Reject members of `m` outside `allowed` (NULL-terminated).
static value_t check_keys(const value_t *m, const char *node, const char *const *allowed) {
    for (size_t i = 0; i < m->map.len; i++) {
        const char *k = m->map.entries[i].key;
        bool ok = false;
        for (const char *const *a = allowed; *a; a++)
            ok |= strcmp(*a, k) == 0;
        if (!ok)
            return bad("%s: unknown member '%s'", node, k);
    }
    return val_none();
}

// options: memory and every scalar option (V1).
static value_t read_options(const hw_profile_t *p, const value_t *m, doc_t *d) {
    if (m->kind != V_MAP)
        return bad("options: expected an object");
    const config_option_decl_t *decl[MACHINE_OPTIONS_MAX];
    int no = scalar_options(p, decl, MACHINE_OPTIONS_MAX);
    for (size_t i = 0; i < m->map.len; i++) {
        const char *k = m->map.entries[i].key;
        const value_t *v = &m->map.entries[i].val;
        char num[24];
        const char *s = v->kind == V_STRING ? v->s : NULL;
        if (!s && v->kind == V_INT) {
            snprintf(num, sizeof num, "%lld", (long long)v->i);
            s = num;
        }
        if (!s)
            return bad("options.%s: expected a value id", k);
        if (strcmp(k, "memory") == 0) {
            char *end = NULL;
            unsigned long kb = strtoul(s, &end, 10);
            if (!end || *end || !hw_profile_ram_option_allowed(p, (uint32_t)kb))
                return bad("options.memory: \"%s\" is not one of the model's memory sizes", s);
            d->ram_kb = (uint32_t)kb;
            continue;
        }
        int at = -1;
        for (int j = 0; j < no; j++)
            if (strcmp(decl[j]->id, k) == 0)
                at = j;
        if (at < 0)
            return bad("options: model '%s' has no option '%s'", p->id, k);
        if (!option_has_value(decl[at], s))
            return bad("options.%s: \"%s\" is not one of its values", k, s);
        copy_str(d->options[at].value, sizeof d->options[at].value, s);
    }
    return val_none();
}

// The cross-option constraints (V1's `requires`).
static value_t check_requires(const hw_profile_t *p, const doc_t *d) {
    const config_option_decl_t *decl[MACHINE_OPTIONS_MAX];
    int no = scalar_options(p, decl, MACHINE_OPTIONS_MAX);
    for (int i = 0; i < no; i++) {
        const config_option_decl_t *o = decl[i];
        if (!o->requires_value || strcmp(d->options[i].value, o->requires_value) != 0)
            continue;
        for (int j = 0; j < no; j++) {
            if (strcmp(decl[j]->id, o->requires_option) == 0 && strcmp(d->options[j].value, o->requires_not) == 0)
                return bad("options.%s: \"%s\" requires %s \xe2\x89\xa0 %s", o->id, o->requires_value,
                           o->requires_option, o->requires_not);
        }
    }
    return val_none();
}

// floppies (V2).
static value_t read_floppies(const hw_profile_t *p, const value_t *m, doc_t *d) {
    if (m->kind != V_MAP)
        return bad("floppies: expected an object");
    int n = floppy_positions(p);
    for (size_t i = 0; i < m->map.len; i++) {
        const char *k = m->map.entries[i].key;
        const value_t *v = &m->map.entries[i].val;
        int at = (k[0] == 'f' && k[1] == 'd' && k[2] >= '0' && k[2] <= '1' && !k[3]) ? k[2] - '0' : -1;
        if (at < 0 || at >= n)
            return bad("floppies: model '%s' has no floppy position '%s'", p->id, k);
        if (v->kind != V_STRING)
            return bad("floppies.%s: expected a drive type", k);
        const struct floppy_slot *s = &p->floppy_slots[at];
        bool ok = strcmp(v->s, floppy_type_id(s->kind)) == 0 || (s->optional && strcmp(v->s, "none") == 0);
        if (!ok)
            return bad("floppies.%s: \"%s\" is not a drive type this position takes", k, v->s);
        copy_str(d->floppy[at], sizeof d->floppy[at], v->s);
    }
    return val_none();
}

// How many drives the machine is built with: positions hold drives in order.
static value_t floppy_count(const hw_profile_t *p, const doc_t *d, int *out) {
    int n = floppy_positions(p), count = 0;
    for (int i = 0; i < n && i < 2; i++) {
        if (strcmp(d->floppy[i], "none") == 0)
            continue;
        if (count != i)
            return bad("floppies.fd%d: \"%s\" needs a drive in \"%s\" first", i, p->floppy_slots[i].label,
                       p->floppy_slots[i - 1].label);
        count++;
    }
    *out = count;
    return val_none();
}

// storage (V3).
static value_t read_storage(const hw_profile_t *p, const value_t *l, doc_t *d) {
    if (l->kind != V_LIST)
        return bad("storage: expected a list");
    d->n_storage = 0;
    for (size_t i = 0; i < l->list.len; i++) {
        const value_t *e = &l->list.items[i];
        if (e->kind != V_MAP)
            return bad("storage[%zu]: expected an object", i);
        static const char *const keys[] = {"bus", "unit", "type", NULL};
        value_t err = check_keys(e, "storage[]", keys);
        if (val_is_error(&err))
            return err;
        const char *bus = map_str(e, "bus");
        int64_t unit = 0;
        unsigned type = 0;
        const storage_bus_decl_t *b = machine_storage_bus(p, bus);
        if (!b)
            return bad("storage[%zu]: model '%s' has no bus '%s'", i, p->id, bus ? bus : "");
        if (!map_int(e, "unit", &unit))
            return bad("storage[%zu]: unit must be a number", i);
        if (!storage_type_parse(map_str(e, "type"), &type))
            return bad("storage[%zu]: type must be \"hd\" or \"cd\"", i);
        char pos[64] = "";
        if (unit >= 0 && unit < unit_count(b))
            storage_position(b, (int)unit, pos, sizeof pos);
        if (unit < 0 || unit >= unit_count(b) || !(b->units & (1u << unit)))
            return bad("storage[%zu]: \"%s\" has no unit %lld", i, b->label, (long long)unit);
        if (b->reserved & (1u << unit))
            return bad("storage[%zu]: unit %lld on \"%s\" is reserved", i, (long long)unit, b->label);
        if (!(b->accepts & type))
            return bad("storage[%zu]: \"%s\" takes no %s", i, b->label, storage_type_label(type));
        for (int j = 0; j < d->n_storage; j++) {
            const machine_storage_dev_t *o = &d->storage[j];
            if (o->unit != unit)
                continue;
            const storage_bus_decl_t *ob = machine_storage_bus(p, o->bus);
            bool same = strcmp(o->bus, b->id) == 0;
            bool shared = (b->shares_units_with && strcmp(b->shares_units_with, o->bus) == 0) ||
                          (ob && ob->shares_units_with && strcmp(ob->shares_units_with, b->id) == 0);
            if (same)
                return bad("storage[%zu]: %s on \"%s\" is already used", i, pos, b->label);
            if (shared)
                return bad("storage[%zu]: %s on \"%s\" is already used on \"%s\" (shared ID space)", i, pos, b->label,
                           ob ? ob->label : o->bus);
        }
        if (d->n_storage >= MACHINE_STORAGE_MAX)
            return bad("storage: too many devices");
        machine_storage_dev_t *o = &d->storage[d->n_storage++];
        copy_str(o->bus, sizeof o->bus, b->id);
        o->unit = (int)unit;
        o->type = type;
    }
    return val_none();
}

// The positions a model has, capped at the two a controller drives.
static int n_floppies_max(const hw_profile_t *p) {
    int n = floppy_positions(p);
    return n > 2 ? 2 : n;
}

// The headless drive= shorthand, "bus:unit:type[;...]", appended to storage
// and checked as the document's own entries are.
static value_t add_drives(const hw_profile_t *p, const char *spec, doc_t *d) {
    value_t *items = NULL;
    size_t n = 0, cap = 0;
    // Keep what storage already has, then add.
    for (int i = 0; i < d->n_storage; i++) {
        value_map_builder_t *b = val_map_new();
        val_map_put(b, "bus", val_str(d->storage[i].bus));
        val_map_put(b, "unit", val_int(d->storage[i].unit));
        val_map_put(b, "type", val_str(storage_type_id(d->storage[i].type)));
        val_list_push(&items, &n, &cap, val_map_finish(b));
    }
    char buf[512];
    if (!copy_str(buf, sizeof buf, spec)) {
        value_t l = val_list(items, n);
        value_free(&l);
        return bad("drive=: too long");
    }
    value_t err = val_none();
    char *save = NULL;
    for (char *e = strtok_r(buf, ";", &save); e; e = strtok_r(NULL, ";", &save)) {
        char *bus = e, *unit = strchr(e, ':'), *type = unit ? strchr(unit + 1, ':') : NULL;
        if (!unit || !type) {
            err = bad("drive=%s: expected bus:unit:type", e);
            break;
        }
        *unit++ = '\0';
        *type++ = '\0';
        value_map_builder_t *b = val_map_new();
        val_map_put(b, "bus", val_str(bus));
        val_map_put(b, "unit", val_int(strtol(unit, NULL, 10)));
        val_map_put(b, "type", val_str(type));
        val_list_push(&items, &n, &cap, val_map_finish(b));
    }
    value_t l = val_list(items, n);
    if (!val_is_error(&err)) {
        err = read_storage(p, &l, d);
        if (!val_is_error(&err))
            default_startup(p, d);
    }
    value_free(&l);
    return err;
}

// startup (V9).
static value_t read_startup(const hw_profile_t *p, const value_t *v, doc_t *d) {
    if (v->kind == V_NONE) {
        d->startup = (machine_startup_t){.none = true};
        return val_none();
    }
    if (v->kind != V_MAP)
        return bad("startup: expected {bus, unit} or null");
    static const char *const keys[] = {"bus", "unit", NULL};
    value_t err = check_keys(v, "startup", keys);
    if (val_is_error(&err))
        return err;
    const char *bus = map_str(v, "bus");
    int64_t unit = 0;
    if (!bus || !map_int(v, "unit", &unit))
        return bad("startup: expected {bus, unit} or null");
    for (int i = 0; i < d->n_storage; i++) {
        if (strcmp(d->storage[i].bus, bus) != 0 || d->storage[i].unit != unit)
            continue;
        const storage_bus_decl_t *b = machine_storage_bus(p, bus);
        if (!b->startup_ok) {
            char pos[64];
            storage_position(b, (int)unit, pos, sizeof pos);
            return bad("startup: %s on \"%s\" \xe2\x80\x94 this machine's startup record cannot name that bus", pos,
                       b->label);
        }
        d->startup = (machine_startup_t){.none = false, .unit = (int)unit, .type = d->storage[i].type};
        copy_str(d->startup.bus, sizeof d->startup.bus, bus);
        return val_none();
    }
    return bad("startup: no device at %s unit %lld in storage", bus, (long long)unit);
}

// cards (V4, V5 in part -- the bus checks the rest).
static value_t read_cards(const hw_profile_t *p, const value_t *l, doc_t *d) {
    if (l->kind != V_LIST)
        return bad("cards: expected a list");
    d->n_cards = 0;
    d->cards_given = true;
    socket_t socks[SOCKETS_MAX];
    int ns = model_sockets(p, socks, SOCKETS_MAX);
    for (size_t i = 0; i < l->list.len; i++) {
        const value_t *e = &l->list.items[i];
        if (e->kind != V_MAP)
            return bad("cards[%zu]: expected an object", i);
        static const char *const keys[] = {"slot", "card", "options", NULL};
        value_t err = check_keys(e, "cards[]", keys);
        if (val_is_error(&err))
            return err;
        const char *slot = map_str(e, "slot"), *card = map_str(e, "card");
        int at = -1;
        for (int s = 0; s < ns; s++)
            if (slot && strcmp(socks[s].id, slot) == 0)
                at = s;
        if (at < 0)
            return bad("cards[%zu]: model '%s' has no slot '%s'", i, p->id, slot ? slot : "");
        if (!card || !*card)
            return bad("cards[%zu]: card is required", i);
        for (int j = 0; j < d->n_cards; j++)
            if (d->cards[j].slot == socks[at].slot && d->cards[j].pci == socks[at].pci)
                return bad("cards[%zu]: \"%s\" is already occupied", i, socks[at].label);
        // The card: one of either bus's registry, fitting this slot.
        const char *card_label = NULL;
        bool fits = false;
        const pci_card_kind_t *pk = NULL;
        if (socks[at].pci) {
            pk = pci_card_find(card);
            const pci_slot_decl_t *sd = NULL;
            for (const pci_slot_decl_t *x = p->pci_slots; x && x->slot; x++)
                if (x->slot == socks[at].slot)
                    sd = x;
            if (pk) {
                card_label = pk->display_name;
                fits = sd && pci_card_fits_socket(sd, pk);
            }
        } else {
            const nubus_card_kind_t *nk = nubus_card_find(card);
            const nubus_slot_decl_t *sd = NULL;
            for (const nubus_slot_decl_t *x = p->nubus_slots; x && x->slot; x++)
                if (x->slot == socks[at].slot)
                    sd = x;
            if (nk) {
                card_label = nk->display_name;
                fits = sd && nubus_card_fits_socket(sd, nk);
            }
        }
        if (!card_label) {
            // A card of the other bus, or none at all.
            const nubus_card_kind_t *other_n = socks[at].pci ? nubus_card_find(card) : NULL;
            const pci_card_kind_t *other_p = socks[at].pci ? NULL : pci_card_find(card);
            if (other_n || other_p)
                return bad("cards[%zu]: \"%s\" does not fit \"%s\"", i,
                           other_n ? other_n->display_name : other_p->display_name, socks[at].label);
            return bad("cards[%zu]: no card '%s'", i, card);
        }
        if (!fits)
            return bad("cards[%zu]: \"%s\" does not fit \"%s\"", i, card_label, socks[at].label);
        doc_card_t *c = &d->cards[d->n_cards++];
        *c = (doc_card_t){.pci = socks[at].pci, .slot = socks[at].slot};
        if (!copy_str(c->card, sizeof c->card, card))
            return bad("cards[%zu]: card id '%s' is too long", i, card);
        const value_t *opts = value_map_get(e, "options");
        if (opts && opts->kind != V_NONE) {
            if (opts->kind != V_MAP)
                return bad("cards[%zu].options: expected an object", i);
            for (size_t k = 0; k < opts->map.len; k++) {
                const value_t *ov = &opts->map.entries[k].val;
                if (ov->kind != V_STRING)
                    return bad("cards[%zu].options.%s: expected a value id", i, opts->map.entries[k].key);
                if (c->n_options >= SLOT_OPTIONS_MAX)
                    return bad("cards[%zu].options: too many", i);
                // A declared option takes one of its declared values (V5);
                // an undeclared one is the card's own to accept or refuse.
                const pci_card_option_t *decl = NULL;
                for (const pci_card_option_t *o = pk ? pk->options : NULL; o && o->key; o++)
                    if (strcmp(o->key, opts->map.entries[k].key) == 0)
                        decl = o;
                if (decl) {
                    bool known = false;
                    for (const char *const *v = decl->values; v && *v; v++)
                        known |= strcmp(*v, ov->s) == 0;
                    if (!known)
                        return bad("cards[%zu].options.%s: \"%s\" is not one of the card's values", i, decl->key,
                                   ov->s);
                }
                slot_option_t *so = &c->options[c->n_options++];
                if (!copy_str(so->key, sizeof so->key, opts->map.entries[k].key) ||
                    !copy_str(so->value, sizeof so->value, ov->s))
                    return bad("cards[%zu].options: %s is too long", i, opts->map.entries[k].key);
            }
        }
    }
    // A slot that is occupied cannot be used while another occupied slot
    // excludes it.
    for (int a = 0; a < d->n_cards; a++) {
        for (int s = 0; s < ns; s++) {
            if (socks[s].pci != d->cards[a].pci || socks[s].slot != d->cards[a].slot || !socks[s].excludes)
                continue;
            for (int b2 = 0; b2 < d->n_cards; b2++) {
                if (!d->cards[b2].pci && d->cards[b2].slot == socks[s].excludes) {
                    const char *other = "";
                    for (int t = 0; t < ns; t++)
                        if (!socks[t].pci && socks[t].slot == socks[s].excludes)
                            other = socks[t].label;
                    return bad("cards: \"%s\" cannot be used while \"%s\" is occupied", socks[s].label, other);
                }
            }
        }
    }
    return val_none();
}

// displays (V7, the device/monitor half; modes are checked by the bus).
static value_t read_displays(const hw_profile_t *p, const value_t *m, doc_t *d) {
    if (m->kind != V_MAP)
        return bad("displays: expected an object");
    // Which members were given, and whether any of them is connected: an
    // absent member then takes "none" (one monitor, D6), otherwise its default.
    bool any_connected = false;
    for (size_t i = 0; i < m->map.len; i++) {
        const value_t *x = &m->map.entries[i].val;
        if (x->kind != V_MAP)
            return bad("displays.%s: expected an object", m->map.entries[i].key);
        const char *mon = map_str(x, "monitor");
        if (mon && strcmp(mon, MONITOR_NONE) != 0)
            any_connected = true;
    }
    for (size_t i = 0; i < m->map.len; i++) {
        const char *dev = m->map.entries[i].key;
        bool known = false;
        for (int j = 0; j < d->n_displays; j++)
            known |= strcmp(d->displays[j].device, dev) == 0;
        if (!known)
            return bad("displays: \"%s\" is not a display device of this configuration "
                       "(built-in video, or a slot holding a display card)",
                       dev);
    }
    for (int j = 0; j < d->n_displays; j++) {
        doc_display_t *x = &d->displays[j];
        const value_t *given = value_map_get(m, x->device);
        if (!given) {
            if (any_connected)
                copy_str(x->monitor, sizeof x->monitor, MONITOR_NONE);
            continue;
        }
        static const char *const keys[] = {"monitor", "mode", NULL};
        value_t err = check_keys(given, "displays.*", keys);
        if (val_is_error(&err))
            return err;
        const char *mon = map_str(given, "monitor");
        const char *mode = map_str(given, "mode");
        if (mon) {
            // The monitor must be one this device takes.
            bool ok = strcmp(mon, MONITOR_NONE) == 0;
            const char *mons[MONITORS_MAX];
            int nm = 0;
            if (strcmp(x->device, "builtin") == 0) {
                nm = builtin_monitors(p, mons, MONITORS_MAX);
            } else {
                for (int c = 0; c < d->n_cards; c++) {
                    char id[16];
                    machine_slot_id(d->cards[c].pci, d->cards[c].slot, id, sizeof id);
                    if (strcmp(id, x->device) == 0)
                        nm = card_monitors(&d->cards[c], mons, MONITORS_MAX);
                }
            }
            for (int k = 0; k < nm; k++)
                ok |= strcmp(mons[k], mon) == 0;
            if (!ok)
                return bad("displays.%s: monitor \"%s\" is not one this device takes", x->device, mon);
            copy_str(x->monitor, sizeof x->monitor, mon);
        }
        if (mode && !copy_str(x->mode, sizeof x->mode, mode))
            return bad("displays.%s: mode \"%s\" is too long", x->device, mode);
    }
    return val_none();
}

// Rebuild the display list from the cards (after `cards` was read): built-in
// video, then each display card, with the default connection rule.
static void displays_from_cards(const hw_profile_t *p, doc_t *d) {
    d->n_displays = 0;
    bool connected = false;
    if (p->builtin_video && builtin_default_monitor(p)) {
        doc_display_t *x = &d->displays[d->n_displays++];
        *x = (doc_display_t){0};
        copy_str(x->device, sizeof x->device, "builtin");
        copy_str(x->monitor, sizeof x->monitor, builtin_default_monitor(p));
        connected = true;
    }
    for (int i = 0; i < d->n_cards; i++) {
        if (!doc_card_is_display(&d->cards[i]))
            continue;
        doc_display_t *x = &d->displays[d->n_displays++];
        *x = (doc_display_t){0};
        machine_slot_id(d->cards[i].pci, d->cards[i].slot, x->device, sizeof x->device);
        const char *mons[MONITORS_MAX];
        int nm = card_monitors(&d->cards[i], mons, MONITORS_MAX);
        copy_str(x->monitor, sizeof x->monitor, (!connected && nm) ? mons[0] : MONITOR_NONE);
        connected |= nm > 0;
    }
}

// ============================================================================
// Into construction arguments
// ============================================================================

// The NuBus card row a display's monitor and mode select, and the depth.
static const nubus_monitor_t *nubus_row(const nubus_card_kind_t *k, const char *monitor, const char *mode, int *depth) {
    unsigned w = 0, h = 0;
    int dd = 0;
    bool want_mode = mode && *mode;
    if (want_mode && sscanf(mode, "%ux%ux%d", &w, &h, &dd) != 3)
        return NULL;
    for (const nubus_monitor_t *m = k ? k->monitors : NULL; m && m->id; m++) {
        if (!m->monitor || strcmp(m->monitor, monitor) != 0)
            continue;
        if (!want_mode) {
            *depth = 0;
            return m;
        }
        if (m->width != w || m->height != h)
            continue;
        for (const int *x = m->depths; x && *x; x++) {
            if (*x == dd) {
                *depth = dd;
                return m;
            }
        }
    }
    return NULL;
}

// Write `doc_card`'s slots= entry (for machine_slots_resolve, which checks
// card fit, options, mode and ROM once for every way a card is named).
static value_t card_slot_spec(const doc_card_t *c, const doc_display_t *disp, char *buf, size_t len) {
    // (A card whose declaration ROM is not offered boots its substitute ROM;
    // the bus decides that as it seats the card.)
    const char *card = c->card;
    int n = snprintf(buf, len, "%d=%s", c->slot, card);
    for (int i = 0; i < c->n_options; i++)
        n += snprintf(buf + n, len - (size_t)n, ",%s=%s", c->options[i].key, c->options[i].value);
    if (disp && disp->mode[0]) {
        if (c->pci)
            return bad("displays: a PCI display card takes no startup mode yet");
        int depth = 0;
        const nubus_card_kind_t *k = nubus_card_find(card);
        const nubus_monitor_t *row = nubus_row(k, disp->monitor, disp->mode, &depth);
        if (!row)
            return bad("displays.%s: mode \"%s\" is not one of %s's modes on that monitor", disp->device, disp->mode,
                       k ? k->display_name : card);
        n += snprintf(buf + n, len - (size_t)n, ",mode=%s_%dbpp", row->id, depth);
    }
    if ((size_t)n >= len)
        return bad("cards: the configuration is too long");
    return val_none();
}

// The display entry of device `id`, or NULL.
static const doc_display_t *find_display(const doc_t *d, const char *id) {
    for (int i = 0; i < d->n_displays; i++)
        if (strcmp(d->displays[i].device, id) == 0)
            return &d->displays[i];
    return NULL;
}

// The code the built-in port's sense lines read with catalogue monitor `mon`
// plugged in: the family's resolver's, MACHINE_SENSE_NONE with none.  A port
// without sense lines has no resolver, and nothing reads its code.
static uint8_t builtin_sense(const hw_profile_t *p, const char *mon) {
    const builtin_video_desc_t *bv = p->builtin_video;
    if (!bv || strcmp(mon, MONITOR_NONE) == 0)
        return MACHINE_SENSE_NONE;
    if (bv->slot_monitors) {
        const nubus_slot_decl_t *d = builtin_video_slot(p);
        const nubus_card_kind_t *k = d ? nubus_card_find(d->builtin_card_id) : NULL;
        for (const nubus_monitor_t *m = k ? k->monitors : NULL; m && m->id; m++)
            if (m->monitor && strcmp(m->monitor, mon) == 0)
                return m->sense_code;
        return MACHINE_SENSE_NONE;
    }
    const char *id = NULL, *cat = NULL;
    for (size_t i = 0; bv->monitor_at && bv->monitor_at(i, &id, &cat); i++) {
        uint8_t s = 0;
        if (strcmp(cat, mon) == 0 && bv->monitor_sense && bv->monitor_sense(id, &s))
            return s;
    }
    return MACHINE_SENSE_NONE;
}

// Apply the displays to the resolved slot entries: each card's monitor and
// sense (or its monitor option, on PCI), the built-in port's, and which
// device is connected.  Every display device gets a definite monitor --
// "none" where the document plugs none in.
static value_t apply_displays(const hw_profile_t *p, const doc_t *d, machine_build_opts_t *out) {
    int connected = 0;
    out->builtin_connected = false;
    out->builtin_sense = MACHINE_SENSE_NONE;
    for (int i = 0; i < d->n_displays; i++) {
        const doc_display_t *x = &d->displays[i];
        bool on = strcmp(x->monitor, MONITOR_NONE) != 0;
        connected += on;
        if (strcmp(x->device, "builtin") == 0) {
            out->builtin_connected = on;
            out->builtin_sense = builtin_sense(p, x->monitor);
            if (x->mode[0]) {
                // The startup mode: the record the port's ROM keeps for it.
                const builtin_startup_t *r = on ? builtin_startup_row(p, x->monitor) : NULL;
                unsigned w = 0, h = 0;
                int dd = 0;
                const builtin_startup_mode_t *m = NULL;
                if (r && sscanf(x->mode, "%ux%ux%d", &w, &h, &dd) == 3 && w == r->width && h == r->height)
                    for (m = r->modes; m->depth && m->depth != dd; m++)
                        ;
                if (!m || !m->depth)
                    return bad("displays.builtin: mode \"%s\" is not one of the built-in video's modes on that monitor",
                               x->mode);
                out->builtin_startup.slot = p->builtin_video->startup_slot;
                memcpy(out->builtin_startup.record, r->record, sizeof r->record);
                out->builtin_startup.record[2] = m->saved_mode;
            }
            continue;
        }
        bool pci = false;
        int slot = 0;
        if (!slot_id_parse(p, x->device, &pci, &slot))
            continue;
        slot_opts_t *e = NULL;
        for (int k = 0; k < out->n_slots; k++)
            if (out->slots[k].slot == slot)
                e = &out->slots[k];
        if (!e)
            continue;
        e->connected = on;
        if (pci) {
            // A PCI display card straps its monitor through its own option.
            const pci_card_kind_t *k = pci_card_find(e->card);
            const char *row_id = NULL;
            for (const nubus_monitor_t *m = k ? k->monitors : NULL; m && m->id; m++)
                if (m->monitor && strcmp(m->monitor, x->monitor) == 0 && !row_id)
                    row_id = m->id;
            if (!on)
                row_id = MONITOR_NONE;
            if (row_id && k && k->accepts_option && k->accepts_option("monitor", row_id) &&
                !slot_opts_option(e, "monitor") && e->n_options < SLOT_OPTIONS_MAX) {
                slot_option_t *o = &e->options[e->n_options++];
                copy_str(o->key, sizeof o->key, "monitor");
                copy_str(o->value, sizeof o->value, row_id);
            }
        } else {
            const nubus_card_kind_t *k = nubus_card_find(e->card);
            int depth = 0;
            const nubus_monitor_t *row = on ? nubus_row(k, x->monitor, x->mode[0] ? x->mode : NULL, &depth) : NULL;
            copy_str(e->monitor, sizeof e->monitor, row ? row->id : MONITOR_NONE);
            e->sense = row ? row->sense_code : MACHINE_SENSE_NONE;
        }
    }
    if (connected > 1)
        return bad("displays: %d monitors connected; this build supports 1", connected);
    return val_none();
}

// The slot entry of `slot`, created when absent.
static slot_opts_t *entry_for(machine_build_opts_t *o, int slot) {
    for (int i = 0; i < o->n_slots; i++)
        if (o->slots[i].slot == slot)
            return &o->slots[i];
    if (o->n_slots >= MACHINE_SLOTS_MAX)
        return NULL;
    slot_opts_t *e = &o->slots[o->n_slots++];
    memset(e, 0, sizeof *e);
    e->slot = slot;
    return e;
}

// video_sense= is a debug override of the connected device's sense code: a
// card's connector (whose monitor row stays, so its geometry does), or the
// built-in port, which on a Quadra's DAFB also takes Apple's indexed codes
// 8..14.  A PCI card senses through its monitor option instead.
static value_t override_sense(const hw_profile_t *p, int sense, machine_build_opts_t *out) {
    for (int i = 0; i < out->n_slots; i++) {
        slot_opts_t *e = &out->slots[i];
        if (!e->connected)
            continue;
        if (p->pci_slots)
            return bad("video_sense=: the connected display is a PCI card, which takes its monitor= option instead");
        if (sense > MACHINE_SENSE_NONE)
            return bad("video_sense=%d: a card's connector reads a code 0..7", sense);
        const nubus_slot_decl_t *d = NULL;
        for (const nubus_slot_decl_t *x = p->nubus_slots; x && x->slot; x++)
            if (x->slot == e->slot)
                d = x;
        const char *card = e->card[0] ? e->card : d ? d->default_card : NULL;
        nubus_entry_default_monitor(card ? nubus_card_find(card) : NULL, e);
        e->sense = (uint8_t)sense;
        return val_none();
    }
    if (!out->builtin_connected)
        return bad("video_sense=: no display device is connected");
    if (sense > MACHINE_SENSE_NONE && !p->builtin_video->indexed_sense)
        return bad("video_sense=%d: model '%s''s built-in video reads a code 0..7", sense, p->id);
    out->builtin_sense = (uint8_t)sense;
    return val_none();
}

// The legacy arguments' connection rule: built-in video unless its monitor
// was unplugged, else the first display card seated (lowest slot).
static void legacy_displays(const hw_profile_t *p, const boot_config_t *legacy, machine_build_opts_t *out) {
    bool builtin_on = p->builtin_video && !(legacy->monitor && strcmp(legacy->monitor, MONITOR_NONE) == 0);
    out->builtin_connected = builtin_on;
    if (builtin_on)
        return;
    // The first display card, in slot order, as the bus will seat it.
    int best = -1;
    for (const nubus_slot_decl_t *d = p->nubus_slots; d && d->slot; d++) {
        if (d->kind != NUBUS_SLOT_SOCKET && d->kind != NUBUS_SLOT_BUILTIN)
            continue;
        const slot_opts_t *e = machine_build_opts_slot(out, d->slot);
        if (e && e->empty)
            continue;
        const char *card = (e && e->card[0])              ? e->card
                           : d->kind == NUBUS_SLOT_SOCKET ? d->default_card
                                                          : d->builtin_card_id;
        const nubus_card_kind_t *k = card ? nubus_card_find(card) : NULL;
        if (k && k->monitors && (best < 0 || d->slot < best))
            best = d->slot;
    }
    for (const pci_slot_decl_t *d = p->pci_slots; best < 0 && d && d->slot; d++) {
        if (d->kind != PCI_SLOT_SOCKET)
            continue;
        const slot_opts_t *e = machine_build_opts_slot(out, d->slot);
        const char *card = (e && e->card[0]) ? e->card : d->default_card;
        const pci_card_kind_t *k = card ? pci_card_find(card) : NULL;
        if (k && strcmp(pci_class(k), "display") == 0)
            best = d->slot;
    }
    if (best >= 0) {
        slot_opts_t *e = entry_for(out, best);
        if (e)
            e->connected = true;
    }
}

value_t machine_config_resolve(const hw_profile_t *p, const value_t *config, const boot_config_t *legacy,
                               machine_build_opts_t *out) {
    doc_t *d = calloc(1, sizeof *d);
    if (!d)
        return bad("out of memory");
    value_t err = val_none();
    doc_defaults(p, d);
    bool have = config && config->kind == V_MAP;
    const value_t *v_opts = have ? value_map_get(config, "options") : NULL;
    const value_t *v_flop = have ? value_map_get(config, "floppies") : NULL;
    const value_t *v_stor = have ? value_map_get(config, "storage") : NULL;
    const value_t *v_start = have ? value_map_get(config, "startup") : NULL;
    const value_t *v_cards = have ? value_map_get(config, "cards") : NULL;
    const value_t *v_disp = have ? value_map_get(config, "displays") : NULL;
    bool legacy_cards = (legacy->video_card && *legacy->video_card) || (legacy->pci_card && *legacy->pci_card) ||
                        (legacy->pci_option && *legacy->pci_option) || (legacy->slots && *legacy->slots) ||
                        (legacy->video_mode && *legacy->video_mode) || (legacy->custom_mode && *legacy->custom_mode);
    bool legacy_monitor = legacy->monitor && *legacy->monitor;

    // V8: a legacy argument and the document node it rewrites into.
    if (v_opts && legacy->ram_kb && v_opts->kind == V_MAP && value_map_get(v_opts, "memory")) {
        err = bad("ram= and config's options.memory both set the memory");
        goto done;
    }
    if (v_cards && legacy_cards) {
        err = bad("config's cards and the legacy card arguments (video_card= / pci_card= / slots= / video_mode= …) "
                  "cannot be mixed");
        goto done;
    }
    if (v_disp && (legacy_monitor || (legacy->video_mode && *legacy->video_mode))) {
        err = bad("config's displays and monitor= / video_mode= cannot be mixed");
        goto done;
    }

    // Memory and the scalar options (V1).
    if (legacy->ram_kb) {
        if (!hw_profile_ram_option_allowed(p, legacy->ram_kb)) {
            err = bad("ram %u KB is not one of the model's memory sizes (see catalog.profile(\"%s\").options)",
                      legacy->ram_kb, p->id);
            goto done;
        }
        d->ram_kb = legacy->ram_kb;
    }
    if (v_opts && (err = read_options(p, v_opts, d), val_is_error(&err)))
        goto done;
    if ((err = check_requires(p, d), val_is_error(&err)))
        goto done;
    // Floppies (V2).
    if (v_flop && (err = read_floppies(p, v_flop, d), val_is_error(&err)))
        goto done;
    int n_floppies = 0;
    // The headless shorthand for "a drive in that position too".
    for (int i = 0; i < legacy->floppies_wanted && i < n_floppies_max(p); i++) {
        if (strcmp(d->floppy[i], "none") == 0)
            copy_str(d->floppy[i], sizeof d->floppy[i], floppy_type_id(p->floppy_slots[i].kind));
    }
    if ((err = floppy_count(p, d, &n_floppies), val_is_error(&err)))
        goto done;
    // Storage (V3) and the startup device (V9).
    if (v_stor) {
        if ((err = read_storage(p, v_stor, d), val_is_error(&err)))
            goto done;
        default_startup(p, d);
    }
    if (legacy->drives && *legacy->drives && (err = add_drives(p, legacy->drives, d), val_is_error(&err)))
        goto done;
    if (v_start && (err = read_startup(p, v_start, d), val_is_error(&err)))
        goto done;

    // Cards: the document's, rewritten into slots= so one validator checks
    // card fit, options, mode and ROM however a card was named (V4-V6).
    boot_config_t slots_doc = *legacy;
    char *spec = NULL;
    if (v_cards) {
        if ((err = read_cards(p, v_cards, d), val_is_error(&err)))
            goto done;
        displays_from_cards(p, d);
    }
    if (v_disp && (err = read_displays(p, v_disp, d), val_is_error(&err)))
        goto done;
    if (have && (v_cards || v_disp)) {
        socket_t socks[SOCKETS_MAX];
        int ns = model_sockets(p, socks, SOCKETS_MAX);
        size_t cap = 4096;
        spec = calloc(1, cap);
        if (!spec) {
            err = bad("out of memory");
            goto done;
        }
        size_t at = 0;
        for (int s = 0; s < ns; s++) {
            const doc_card_t *c = NULL;
            for (int i = 0; i < d->n_cards; i++)
                if (d->cards[i].slot == socks[s].slot && d->cards[i].pci == socks[s].pci)
                    c = &d->cards[i];
            char one[512];
            if (c) {
                char id[16];
                machine_slot_id(c->pci, c->slot, id, sizeof id);
                if ((err = card_slot_spec(c, find_display(d, id), one, sizeof one), val_is_error(&err)))
                    break;
            } else {
                snprintf(one, sizeof one, "%d=none", socks[s].slot);
            }
            at += (size_t)snprintf(spec + at, cap - at, "%s%s", at ? ";" : "", one);
        }
        if (val_is_error(&err)) {
            free(spec);
            goto done;
        }
        slots_doc.slots = spec;
        slots_doc.video_card = slots_doc.pci_card = slots_doc.pci_option = NULL;
        slots_doc.video_mode = slots_doc.custom_mode = NULL;
    }
    err = machine_slots_resolve(p, &slots_doc, out);
    free(spec);
    if (val_is_error(&err))
        goto done;

    // Displays: per-device monitor sense and the connected device.
    if (have && (v_cards || v_disp)) {
        if ((err = apply_displays(p, d, out), val_is_error(&err)))
            goto done;
    } else if (have && !legacy_cards && !legacy_monitor) {
        if ((err = apply_displays(p, d, out), val_is_error(&err)))
            goto done;
    } else {
        // The legacy arguments: monitor= straps the built-in port (a family
        // token, "none" among them), else its default monitor; each card
        // keeps the monitor the bus gives it.
        out->builtin_sense = p->builtin_video ? builtin_sense(p, builtin_default_monitor(p)) : MACHINE_SENSE_NONE;
        if (legacy_monitor) {
            if (!p->builtin_video || p->builtin_video->slot_monitors || !p->builtin_video->monitor_sense) {
                err = bad("model '%s' has no configurable built-in video port", p->id);
                goto done;
            }
            if (!p->builtin_video->monitor_sense(legacy->monitor, &out->builtin_sense)) {
                err = bad("unknown monitor id '%s' (see catalog.profile)", legacy->monitor);
                goto done;
            }
        }
        legacy_displays(p, legacy, out);
    }
    if (legacy->video_sense >= 0 && (err = override_sense(p, legacy->video_sense, out), val_is_error(&err)))
        goto done;

    // Everything else straight across.
    out->ram_kb = d->ram_kb;
    out->n_options = d->n_options;
    memcpy(out->options, d->options, sizeof d->options);
    out->n_floppies = n_floppies;
    out->storage_given = true;
    out->n_storage = d->n_storage;
    memcpy(out->storage, d->storage, sizeof d->storage);
    out->startup = d->startup;
done:
    free(d);
    return err;
}
