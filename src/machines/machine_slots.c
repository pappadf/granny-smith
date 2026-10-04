// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_slots.c
// The boot document's expansion-slot configuration, resolved once.
//
// A document configures a machine's NuBus or PCI slots (no machine has both)
// through `slots=` -- one entry per slot -- and through the sugar every older
// caller uses: video_card= / video_mode= / custom_mode= for the first NuBus
// socket (the builtin slot on a socketless machine), pci_card= / pci_option=
// for the first PCI socket, vrom= / prom= for every slot whose card the file
// provides.  machine_slots_resolve turns all of it into validated per-slot
// entries (slot_opts_t) before the running machine is touched; below it
// nothing knows the sugar exists, and the buses seat each slot from its entry.
//
// Grammar of slots=: entries separated by ';', each `SLOT=CARD[,key=value]*`.
// SLOT is the slot number as machine.nubus.slot[N] / machine.pci.slot[N]
// index it (decimal, or hex as $A / 0xA).  CARD is a card-kind id, `none`
// for an empty socket, or empty for the slot's own card.  `mode=`,
// `custom=` and `rom=` set the video mode, the custom geometry and the card's
// ROM file; any other key is a card option.

#include "machine.h"

#include "log.h"
#include "machine_config.h"
#include "machine_profile.h"
#include "nubus.h"
#include "prom.h"
#include "value.h"
#include "vrom.h"
#include "nubus/card.h"
#include "pci/pci.h"
#include "pci/pci_card.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("setup");

// The expansion bus a model has.
typedef enum { SLOTS_NONE, SLOTS_NUBUS, SLOTS_PCI } slots_bus_t;

static slots_bus_t model_bus(const hw_profile_t *p) {
    if (p->nubus_slots && p->nubus_slots[0].slot)
        return SLOTS_NUBUS;
    if (p->pci_slots && p->pci_slots[0].slot)
        return SLOTS_PCI;
    return SLOTS_NONE;
}

// The NuBus declaration of `slot`, or NULL.
static const nubus_slot_decl_t *nubus_decl(const hw_profile_t *p, int slot) {
    for (const nubus_slot_decl_t *d = p->nubus_slots; d && d->slot; d++) {
        if (d->slot == slot)
            return d;
    }
    return NULL;
}

// The PCI declaration of `slot`, or NULL.
static const pci_slot_decl_t *pci_decl(const hw_profile_t *p, int slot) {
    for (const pci_slot_decl_t *d = p->pci_slots; d && d->slot; d++) {
        if (d->slot == slot)
            return d;
    }
    return NULL;
}

// The slot first-socket sugar configures: the first SOCKET, or on a NuBus
// machine with none, its first BUILTIN slot (the SE/30's video).  0: none.
static int sugar_slot(const hw_profile_t *p, slots_bus_t bus) {
    if (bus == SLOTS_NUBUS) {
        for (const nubus_slot_decl_t *d = p->nubus_slots; d->slot; d++) {
            if (d->kind == NUBUS_SLOT_SOCKET)
                return d->slot;
        }
        for (const nubus_slot_decl_t *d = p->nubus_slots; d->slot; d++) {
            if (d->kind == NUBUS_SLOT_BUILTIN)
                return d->slot;
        }
        return 0;
    }
    if (bus == SLOTS_PCI) {
        for (const pci_slot_decl_t *d = p->pci_slots; d->slot; d++) {
            if (d->kind == PCI_SLOT_SOCKET)
                return d->slot;
        }
    }
    return 0;
}

// The entry for `slot` in `o`, created empty when absent.  NULL when full.
static slot_opts_t *entry_for(machine_build_opts_t *o, int slot) {
    for (int i = 0; i < o->n_slots; i++) {
        if (o->slots[i].slot == slot)
            return &o->slots[i];
    }
    if (o->n_slots >= MACHINE_SLOTS_MAX)
        return NULL;
    slot_opts_t *e = &o->slots[o->n_slots++];
    memset(e, 0, sizeof(*e));
    e->slot = slot;
    return e;
}

// Copy `src` into a fixed field, failing when it does not fit.
static bool copy_field(char *dst, size_t size, const char *src) {
    return snprintf(dst, size, "%s", src) < (int)size;
}

// Add option key=value to an entry.  False (with *why) when the key repeats
// or the entry has no room.
static bool add_option(slot_opts_t *e, const char *key, const char *value, const char **why) {
    if (slot_opts_option(e, key)) {
        *why = "option given twice";
        return false;
    }
    if (e->n_options >= SLOT_OPTIONS_MAX) {
        *why = "too many options";
        return false;
    }
    slot_option_t *o = &e->options[e->n_options];
    if (!copy_field(o->key, sizeof o->key, key) || !copy_field(o->value, sizeof o->value, value)) {
        *why = "option too long";
        return false;
    }
    e->n_options++;
    return true;
}

// Parse one slot number: decimal, or hex as $A / 0xA.  -1 on garbage.
static int parse_slot_number(const char *s) {
    char *end = NULL;
    long v = (*s == '$') ? strtol(s + 1, &end, 16) : strtol(s, &end, 0);
    if (!end || end == s || *end != '\0' || v <= 0 || v > 255)
        return -1;
    return (int)v;
}

// Strip leading and trailing blanks in place.
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
        *--e = '\0';
    return s;
}

// Parse the slots= spec into `o`.  Every slot named must exist on the model.
static value_t parse_slots(const hw_profile_t *p, slots_bus_t bus, const char *spec, machine_build_opts_t *o) {
    if (bus == SLOTS_NONE)
        return val_err("machine.boot: model '%s' has no expansion slots for slots=", p->id);
    char *buf = strdup(spec);
    if (!buf)
        return val_err("machine.boot: out of memory");
    value_t err = val_none();
    char *save_entry = NULL;
    for (char *ent = strtok_r(buf, ";", &save_entry); ent; ent = strtok_r(NULL, ";", &save_entry)) {
        ent = trim(ent);
        if (!*ent)
            continue;
        char *save_field = NULL;
        char *head = strtok_r(ent, ",", &save_field);
        char *eq = head ? strchr(head, '=') : NULL;
        if (!eq) {
            err = val_err("machine.boot: slots entry '%s' is not SLOT=CARD", ent);
            break;
        }
        *eq = '\0';
        char *slot_txt = trim(head), *card = trim(eq + 1);
        int slot = parse_slot_number(slot_txt);
        bool declared = bus == SLOTS_NUBUS ? nubus_decl(p, slot) != NULL : pci_decl(p, slot) != NULL;
        if (slot < 0 || !declared) {
            err = val_err("machine.boot: model '%s' has no %s slot '%s'", p->id, bus == SLOTS_NUBUS ? "NuBus" : "PCI",
                          slot_txt);
            break;
        }
        if (machine_build_opts_slot(o, slot)) {
            err = val_err("machine.boot: slots= names slot %d twice", slot);
            break;
        }
        slot_opts_t *e = entry_for(o, slot);
        if (!e) {
            err = val_err("machine.boot: slots= names too many slots");
            break;
        }
        if (strcmp(card, "none") == 0)
            e->empty = true;
        else if (!copy_field(e->card, sizeof e->card, card)) {
            err = val_err("machine.boot: slot %d: card id '%s' is too long", slot, card);
            break;
        }
        for (char *f = strtok_r(NULL, ",", &save_field); f; f = strtok_r(NULL, ",", &save_field)) {
            f = trim(f);
            char *feq = strchr(f, '=');
            if (!feq || feq == f || !feq[1]) {
                err = val_err("machine.boot: slot %d: '%s' is not key=value", slot, f);
                break;
            }
            *feq = '\0';
            const char *key = trim(f), *value = trim(feq + 1);
            bool ok = true;
            const char *why = "value too long";
            if (strcmp(key, "mode") == 0)
                ok = copy_field(e->video_mode, sizeof e->video_mode, value);
            else if (strcmp(key, "custom") == 0)
                ok = copy_field(e->custom_mode, sizeof e->custom_mode, value);
            else if (strcmp(key, "rom") == 0)
                ok = copy_field(e->rom, sizeof e->rom, value);
            else
                ok = add_option(e, key, value, &why);
            if (!ok) {
                err = val_err("machine.boot: slot %d: %s: %s", slot, key, why);
                break;
            }
        }
        if (val_is_error(&err))
            break;
    }
    free(buf);
    return err;
}

// Is `card` the declared card of one of the model's BUILTIN slots?  With a
// socket on the machine, the first-socket sugar naming it is a no-op.
static bool names_builtin(const hw_profile_t *p, slots_bus_t bus, const char *card) {
    if (bus == SLOTS_NUBUS) {
        for (const nubus_slot_decl_t *d = p->nubus_slots; d->slot; d++) {
            if (d->kind == NUBUS_SLOT_BUILTIN && d->builtin_card_id && strcmp(d->builtin_card_id, card) == 0)
                return sugar_slot(p, bus) != d->slot;
        }
        return false;
    }
    for (const pci_slot_decl_t *d = p->pci_slots; d->slot; d++) {
        if (d->kind != PCI_SLOT_SOCKET && d->builtin_card_id && strcmp(d->builtin_card_id, card) == 0)
            return true;
    }
    return false;
}

// Apply the first-socket sugar (video_card=, video_mode=, custom_mode=,
// pci_card=, pci_option=) to the sugar slot.  It may not contradict an
// explicit slots= entry for the same slot.
static value_t apply_socket_sugar(const hw_profile_t *p, slots_bus_t bus, const boot_config_t *doc,
                                  machine_build_opts_t *o) {
    bool nubus_sugar = (doc->video_card && *doc->video_card) || (doc->video_mode && *doc->video_mode) ||
                       (doc->custom_mode && *doc->custom_mode);
    bool pci_sugar = (doc->pci_card && *doc->pci_card) || (doc->pci_option && *doc->pci_option);
    if (nubus_sugar && bus != SLOTS_NUBUS) {
        if (doc->video_card && *doc->video_card)
            return val_err("machine.boot: model '%s' has no NuBus slots for video_card '%s'", p->id, doc->video_card);
        return val_err("machine.boot: model '%s' has no NuBus slots for video_mode= / custom_mode=", p->id);
    }
    if (pci_sugar && bus != SLOTS_PCI) {
        if (doc->pci_card && *doc->pci_card)
            return val_err("machine.boot: model '%s' has no PCI slots for pci_card '%s'", p->id, doc->pci_card);
        return val_err("machine.boot: model '%s' has no PCI slots for pci_option=", p->id);
    }
    if (!nubus_sugar && !pci_sugar)
        return val_none();
    int slot = sugar_slot(p, bus);
    if (!slot)
        return val_err("machine.boot: model '%s' has no slot for video_card= / pci_card=", p->id);
    bool existed = machine_build_opts_slot(o, slot) != NULL;
    slot_opts_t *e = entry_for(o, slot);
    if (!e)
        return val_err("machine.boot: slots= names too many slots");
    const char *card = bus == SLOTS_NUBUS ? doc->video_card : doc->pci_card;
    // Naming a card the model already has built in is accepted and changes
    // nothing: the sugar means "this machine with that card".
    if (card && *card && names_builtin(p, bus, card))
        card = NULL;
    if (card && *card) {
        if (existed && (e->card[0] || e->empty))
            return val_err("machine.boot: %s= and slots= both name the card for slot %d",
                           bus == SLOTS_NUBUS ? "video_card" : "pci_card", slot);
        if (!copy_field(e->card, sizeof e->card, card))
            return val_err("machine.boot: card id '%s' is too long", card);
    }
    if (doc->video_mode && *doc->video_mode) {
        if (e->video_mode[0])
            return val_err("machine.boot: video_mode= and slots= both set the mode of slot %d", slot);
        if (!copy_field(e->video_mode, sizeof e->video_mode, doc->video_mode))
            return val_err("machine.boot: video_mode '%s' is too long", doc->video_mode);
    }
    if (doc->custom_mode && *doc->custom_mode) {
        if (e->custom_mode[0])
            return val_err("machine.boot: custom_mode= and slots= both set the geometry of slot %d", slot);
        if (!copy_field(e->custom_mode, sizeof e->custom_mode, doc->custom_mode))
            return val_err("machine.boot: custom_mode '%s' is too long", doc->custom_mode);
    }
    if (doc->pci_option && *doc->pci_option) {
        char *buf = strdup(doc->pci_option);
        if (!buf)
            return val_err("machine.boot: out of memory");
        value_t err = val_none();
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            tok = trim(tok);
            char *eq = strchr(tok, '=');
            if (!eq || eq == tok || !eq[1]) {
                err = val_err("machine.boot: pci_option '%s' is not key=value", tok);
                break;
            }
            *eq = '\0';
            const char *why = NULL;
            if (!add_option(e, trim(tok), trim(eq + 1), &why)) {
                err = val_err("machine.boot: pci_option %s: %s", tok, why);
                break;
            }
        }
        free(buf);
        if (val_is_error(&err))
            return err;
    }
    return val_none();
}

// An unknown card id, with the nearest registered spelling when there is one.
static value_t unknown_card(const char *id, const char *near, const char *catalog) {
    if (near)
        return val_err("machine.boot: unknown card id '%s' — did you mean '%s'? (see catalog.%s)", id, near, catalog);
    return val_err("machine.boot: unknown card id '%s' (see catalog.%s)", id, catalog);
}

// Validate one NuBus entry against its slot and resolve the kind it seats
// (*out_kind, NULL for an empty socket).  `named` reports whether the
// document named the card (an explicit card must resolve its declaration ROM).
static value_t check_nubus_entry(const hw_profile_t *p, const slot_opts_t *e, const nubus_card_kind_t **out_kind,
                                 bool *named) {
    const nubus_slot_decl_t *d = nubus_decl(p, e->slot);
    const nubus_card_kind_t *k = NULL;
    *named = false;
    if (d->kind != NUBUS_SLOT_SOCKET && d->kind != NUBUS_SLOT_BUILTIN)
        return val_err("machine.boot: NuBus slot $%X on model '%s' takes no card", e->slot, p->id);
    if (e->empty) {
        if (d->kind != NUBUS_SLOT_SOCKET)
            return val_err("machine.boot: NuBus slot $%X on model '%s' is built in and cannot be emptied", e->slot,
                           p->id);
    } else if (e->card[0]) {
        k = nubus_card_find(e->card);
        if (!k)
            return unknown_card(e->card, nubus_card_suggest(e->card), "nubus_cards");
        bool fits = d->kind == NUBUS_SLOT_SOCKET ? nubus_card_fits_socket(d, k)
                                                 : (k->attach == CARD_ATTACH_BUILTIN ||
                                                    (d->builtin_card_id && strcmp(d->builtin_card_id, k->id) == 0));
        if (!fits)
            return val_err(
                "machine.boot: card '%s' fits no slot on model '%s' (see catalog.profile(\"%s\").video_slots)", e->card,
                p->id, p->id);
        *named = d->kind == NUBUS_SLOT_SOCKET;
    } else {
        k = nubus_card_find(d->kind == NUBUS_SLOT_SOCKET ? d->default_card : d->builtin_card_id);
    }
    *out_kind = k;
    if (e->video_mode[0]) {
        if (!nubus_video_mode_known(e->video_mode))
            return val_err("machine.boot: unknown video-mode id '%s'", e->video_mode);
        if (!k || !nubus_monitor_mode_lookup(k->monitors, e->video_mode, NULL, NULL))
            return val_err("machine.boot: video mode '%s' does not belong to slot $%X's card '%s'", e->video_mode,
                           e->slot, k ? k->id : "(none)");
    }
    if (e->custom_mode[0]) {
        const char *why = NULL;
        if (!nubus_custom_mode_parse(e->custom_mode, NULL, NULL, NULL, &why))
            return val_err("machine.boot: custom_mode '%s' invalid: %s", e->custom_mode, why);
        uint32_t w = 0, h = 0, d = 0;
        nubus_custom_mode_parse(e->custom_mode, &w, &h, &d, NULL);
        if (!k || !k->custom_mode_fits)
            return val_err("machine.boot: slot $%X's card '%s' takes no custom geometry", e->slot,
                           k ? k->id : "(none)");
        if (!k->custom_mode_fits(w, h, d, &why))
            return val_err("machine.boot: custom_mode '%s' on slot $%X's card '%s': %s", e->custom_mode, e->slot, k->id,
                           why);
    }
    if (e->n_options)
        return val_err("machine.boot: slot $%X's card '%s' takes no option '%s'", e->slot, k ? k->id : "(none)",
                       e->options[0].key);
    if (e->rom[0]) {
        vrom_id_t vid;
        if (!vrom_identify_card(e->rom, &vid))
            return val_err("machine.boot: vrom '%s' is not a recognised declaration ROM", e->rom);
        if (!k || strcmp(vid.card_id, k->id) != 0)
            return val_err("machine.boot: vrom '%s' is for card '%s', not slot $%X's '%s'", e->rom, vid.card_id,
                           e->slot, k ? k->id : "(none)");
    }
    // A card the document named must resolve its declaration ROM before the
    // running machine is touched; a slot's own default degrades to an empty
    // slot with a log, and a built-in card owns its fallback (the SE/30
    // synthesises its onboard vROM).
    if (*named && k && vrom_card_catalogued(k->id) && !vrom_card_resolvable(k->id, e->rom[0] ? e->rom : NULL))
        return val_err(
            "machine.boot: card '%s' (slot $%X) needs a declaration ROM but no offered vROM file provides it", k->id,
            e->slot);
    return val_none();
}

// The PCI counterpart: card fit, options the kind accepts, PROM.
static value_t check_pci_entry(const hw_profile_t *p, const slot_opts_t *e, const pci_card_kind_t **out_kind) {
    const pci_slot_decl_t *d = pci_decl(p, e->slot);
    const pci_card_kind_t *k = NULL;
    bool named = false;
    if (d->kind == PCI_SLOT_ABSENT)
        return val_err("machine.boot: PCI slot %d on model '%s' takes no card", e->slot, p->id);
    if (e->empty) {
        if (d->kind != PCI_SLOT_SOCKET)
            return val_err("machine.boot: PCI slot %d on model '%s' is built in and cannot be emptied", e->slot, p->id);
    } else if (e->card[0]) {
        k = pci_card_find(e->card);
        if (!k)
            return unknown_card(e->card, pci_card_suggest(e->card), "pci_cards");
        bool fits = d->kind == PCI_SLOT_SOCKET ? pci_card_fits_socket(d, k)
                                               : (d->builtin_card_id && strcmp(d->builtin_card_id, k->id) == 0);
        if (!fits)
            return val_err("machine.boot: card '%s' fits no slot on model '%s' (see catalog.profile(\"%s\").pci_slots)",
                           e->card, p->id, p->id);
        named = d->kind == PCI_SLOT_SOCKET;
    } else {
        k = pci_card_find(d->kind == PCI_SLOT_SOCKET ? d->default_card : d->builtin_card_id);
    }
    *out_kind = k;
    if (e->video_mode[0] || e->custom_mode[0])
        return val_err("machine.boot: PCI slot %d takes no video mode (a PCI display card senses its monitor)",
                       e->slot);
    for (int i = 0; i < e->n_options; i++) {
        if (!k || !k->accepts_option || !k->accepts_option(e->options[i].key, e->options[i].value))
            return val_err("machine.boot: slot %d's card '%s' does not take option %s=%s", e->slot,
                           k ? k->id : "(none)", e->options[i].key, e->options[i].value);
    }
    if (e->rom[0]) {
        prom_id_t pid;
        if (!prom_identify_card(e->rom, &pid))
            return val_err("machine.boot: prom '%s' is not a recognised PCI expansion ROM "
                           "(see catalog.proms.identify for what it is instead)",
                           e->rom);
        if (!k || strcmp(pid.card_id, k->id) != 0)
            return val_err("machine.boot: prom '%s' is for card '%s', not slot %d's '%s'", e->rom, pid.card_id, e->slot,
                           k ? k->id : "(none)");
    }
    if (named && k && k->requires_prom && !prom_card_resolvable(k->id, e->rom[0] ? e->rom : NULL))
        return val_err("machine.boot: card '%s' (PCI slot %d) needs a PCI expansion ROM but no offered "
                       ".prom file provides it",
                       k->id, e->slot);
    return val_none();
}

// vrom= / prom=: the file is the ROM of every slot whose card it provides --
// configured or declared.  A file whose card the machine does not seat is
// noted and changes nothing.
static value_t apply_rom_sugar(const hw_profile_t *p, slots_bus_t bus, const boot_config_t *doc,
                               machine_build_opts_t *o) {
    const char *path = NULL, *card_id = NULL;
    if (doc->vrom && *doc->vrom) {
        vrom_id_t vid;
        if (!vrom_identify_card(doc->vrom, &vid))
            return val_err("machine.boot: vrom '%s' is not a recognised declaration ROM", doc->vrom);
        path = doc->vrom;
        card_id = vid.card_id;
        if (bus != SLOTS_NUBUS) {
            LOG(1, "machine.boot: vrom '%s' (card '%s') — model '%s' has no NuBus card; unused", path, card_id, p->id);
            path = NULL;
        }
    }
    if (doc->prom && *doc->prom) {
        prom_id_t pid;
        if (!prom_identify_card(doc->prom, &pid))
            return val_err("machine.boot: prom '%s' is not a recognised PCI expansion ROM "
                           "(see catalog.proms.identify for what it is instead)",
                           doc->prom);
        if (bus == SLOTS_PCI) {
            path = doc->prom;
            card_id = pid.card_id;
        } else {
            LOG(1, "machine.boot: prom '%s' (card '%s') — model '%s' has no PCI card; unused", doc->prom, pid.card_id,
                p->id);
        }
    }
    if (!path)
        return val_none();
    int used = 0;
    // Every declared slot whose resolved card the file provides.
    for (int i = 0;; i++) {
        int slot = 0;
        const char *kind_id = NULL;
        if (bus == SLOTS_NUBUS) {
            const nubus_slot_decl_t *d = &p->nubus_slots[i];
            if (!d->slot)
                break;
            slot = d->slot;
            const slot_opts_t *e = machine_build_opts_slot(o, slot);
            kind_id = (e && e->empty)                 ? NULL
                      : (e && e->card[0])             ? e->card
                      : d->kind == NUBUS_SLOT_SOCKET  ? d->default_card
                      : d->kind == NUBUS_SLOT_BUILTIN ? d->builtin_card_id
                                                      : NULL;
        } else {
            const pci_slot_decl_t *d = &p->pci_slots[i];
            if (!d->slot)
                break;
            slot = d->slot;
            const slot_opts_t *e = machine_build_opts_slot(o, slot);
            kind_id = (e && e->empty)              ? NULL
                      : (e && e->card[0])          ? e->card
                      : d->kind == PCI_SLOT_SOCKET ? d->default_card
                                                   : d->builtin_card_id;
        }
        if (!kind_id || strcmp(kind_id, card_id) != 0)
            continue;
        slot_opts_t *e = entry_for(o, slot);
        if (!e)
            return val_err("machine.boot: slots= names too many slots");
        if (e->rom[0])
            continue; // slots= named this slot's ROM itself
        if (!copy_field(e->rom, sizeof e->rom, path))
            return val_err("machine.boot: ROM path '%s' is too long", path);
        used++;
    }
    if (!used)
        LOG(1, "machine.boot: '%s' provides card '%s', which model '%s' does not seat; unused", path, card_id, p->id);
    return val_none();
}

value_t machine_slots_resolve(const hw_profile_t *profile, const boot_config_t *doc, machine_build_opts_t *out) {
    slots_bus_t bus = model_bus(profile);
    out->n_slots = 0;
    value_t err = val_none();
    if (doc->slots && *doc->slots) {
        err = parse_slots(profile, bus, doc->slots, out);
        if (val_is_error(&err))
            return err;
    }
    err = apply_socket_sugar(profile, bus, doc, out);
    if (val_is_error(&err))
        return err;
    // The cards, modes and options, slot by slot -- before the ROM sugar,
    // which needs to know what every slot seats.
    for (int i = 0; i < out->n_slots; i++) {
        if (bus == SLOTS_NUBUS) {
            const nubus_card_kind_t *k = NULL;
            bool named = false;
            err = check_nubus_entry(profile, &out->slots[i], &k, &named);
        } else {
            const pci_card_kind_t *k = NULL;
            err = check_pci_entry(profile, &out->slots[i], &k);
        }
        if (val_is_error(&err))
            return err;
    }
    err = apply_rom_sugar(profile, bus, doc, out);
    if (val_is_error(&err))
        return err;
    // The ROM sugar may have added entries for declared cards: check them too
    // (their ROM must provide their card, which it does by construction, and
    // a named card must still resolve).
    for (int i = 0; i < out->n_slots; i++) {
        if (bus == SLOTS_NUBUS) {
            const nubus_card_kind_t *k = NULL;
            bool named = false;
            err = check_nubus_entry(profile, &out->slots[i], &k, &named);
        } else {
            const pci_card_kind_t *k = NULL;
            err = check_pci_entry(profile, &out->slots[i], &k);
        }
        if (val_is_error(&err))
            return err;
    }
    return val_none();
}
