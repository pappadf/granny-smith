// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine.c
// Machine profile registry plus the machine.* object-model surface.

#include "machine.h"

#include "adb.h"
#include "cpu.h"
#include "gs_out.h"
#include "image.h"
#include "log.h"
#include "machine_config.h"
#include "nubus.h"
#include "object.h"
#include "platform.h"
#include "prom.h"
#include "rom.h"
#include "scheduler.h"
#include "scsi.h"
#include "system.h"
#include "system_config.h"
#include "value.h"
#include "vrom.h"
#include "nubus/card.h"
#include "pci/pci.h"

#include "mcu/dafb.h"

LOG_USE_CATEGORY_NAME("setup");

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Registry of built-in machine profiles.  A static const array iterated
// directly: adding a machine is one line here, no runtime
// machine_register(), no MAX_MACHINES cap.  The profiles are defined in each
// family's machine file (glue/se30.c, mdu/iici.c, …).
static const hw_profile_t *const builtin_machines[] = {
    &machine_plus,   &machine_se30,   &machine_iicx,   &machine_iix,    &machine_iifx,   &machine_iici,
    &machine_iisi,   &machine_q700,   &machine_q900,   &machine_q950,   &machine_q840av, &machine_q660av,
    &machine_pm6100, &machine_pm7100, &machine_pm8100, &machine_pm7500, &machine_pm8500, &machine_pm9500,
    &machine_ans500, &machine_ans700, &machine_pmg3dt, &machine_pmg3mt, &machine_lisa,   &machine_macxl,
};
static const size_t builtin_machine_count = sizeof(builtin_machines) / sizeof(builtin_machines[0]);

// Convert a floppy_kind_t to its wire string ("400k" / "800k" / "hd").
const char *floppy_kind_to_string(floppy_kind_t kind) {
    switch (kind) {
    case FLOPPY_400K:
        return "400k";
    case FLOPPY_800K:
        return "800k";
    case FLOPPY_HD:
        return "hd";
    }
    return "";
}

// Convert an mmu_kind_t to its wire string ("none" / "68030_pmmu" / "68040" /
// "ppc_601" / "ppc_604" / "lisa_segment").  This is the value the capability probe exports as
// `mmu.kind` so the debug UI can pick the right register views.
const char *mmu_kind_to_string(mmu_kind_t kind) {
    switch (kind) {
    case MMU_NONE:
        return "none";
    case MMU_68030_PMMU:
        return "68030_pmmu";
    case MMU_LISA_SEGMENT:
        return "lisa_segment";
    case MMU_68040:
        return "68040";
    case MMU_PPC_601:
        return "ppc_601";
    case MMU_PPC_604:
        return "ppc_604";
    }
    return "none";
}

// Convert an hd_bus_t to its wire string ("scsi" / "profile").  The config UI
// reads this to label the HD row and choose the attach call.
const char *hd_bus_to_string(hd_bus_t bus) {
    switch (bus) {
    case HD_BUS_SCSI:
        return "scsi";
    case HD_BUS_PROFILE:
        return "profile";
    }
    return "scsi";
}

// === Media bays ==============================================
// Where a hard disk or a CD goes is a fact the profile already holds -- the
// buses with their bays, the `boot` flag, hd_bus, has_cdrom/cdrom_id -- but
// every consumer derived it for itself, and they disagreed: headless put
// hd=N at SCSI id N, the web dialog at the boot bay, the URL path at N again,
// the Images panel at the default id on bus 0 whatever the bay's bus.  These
// derive it once.

const char *media_bus_name(media_bus_t bus) {
    switch (bus) {
    case MEDIA_BUS_FLOPPY:
        return "floppy";
    case MEDIA_BUS_SCSI:
        return "scsi";
    case MEDIA_BUS_SCSI2:
        return "scsi2";
    case MEDIA_BUS_PROFILE:
        return "profile";
    case MEDIA_BUS_ATA:
        return "ata";
    }
    return "scsi";
}

bool media_bus_parse(const char *name, media_bus_t *out) {
    static const media_bus_t all[] = {MEDIA_BUS_FLOPPY, MEDIA_BUS_SCSI, MEDIA_BUS_SCSI2, MEDIA_BUS_PROFILE,
                                      MEDIA_BUS_ATA};
    for (size_t i = 0; name && i < sizeof(all) / sizeof(all[0]); i++) {
        if (strcmp(name, media_bus_name(all[i])) == 0) {
            *out = all[i];
            return true;
        }
    }
    return false;
}

int profile_hd_bays(const hw_profile_t *p, media_bay_t *out, int max) {
    if (!p || max <= 0)
        return 0;
    if (p->hd_bus == HD_BUS_PROFILE) {
        out[0] = (media_bay_t){.bus = MEDIA_BUS_PROFILE, .unit = 0, .label = "ProFile"};
        return 1;
    }
    // Every declared bay, in declared order, remembering the boot one.
    media_bay_t all[MEDIA_HD_BAYS_MAX];
    int n = 0, boot = 0;
    for (const struct scsi_bus_decl *bus = p->scsi_buses; bus && bus->object; bus++) {
        media_bus_t which;
        if (!media_bus_parse(bus->object, &which) || (which != MEDIA_BUS_SCSI && which != MEDIA_BUS_SCSI2))
            continue;
        for (const struct scsi_slot *s = bus->slots; s && s->label && n < MEDIA_HD_BAYS_MAX; s++) {
            if (s->boot)
                boot = n;
            all[n++] = (media_bay_t){.bus = which, .unit = s->id, .label = s->label};
        }
    }
    // The boot bay first, then the rest in declared order.
    int count = 0;
    if (n > 0)
        out[count++] = all[boot];
    for (int i = 0; i < n && count < max; i++)
        if (i != boot)
            out[count++] = all[i];
    return count;
}

bool profile_default_hd_bay(const hw_profile_t *p, media_bay_t *out) {
    return profile_hd_bays(p, out, 1) == 1;
}

struct scsi *profile_scsi_init(const hw_profile_t *p, checkpoint_t *cp, const image_list_t *images) {
    return scsi_init(cp, images, p->has_cdrom ? p->cdrom_drive : NULL, p->cdrom_id);
}

bool profile_cdrom_bay(const hw_profile_t *p, media_bay_t *out) {
    if (!p || !p->has_cdrom)
        return false;
    // The core seats the CD on the machine's first bus (cfg->scsi).
    *out = (media_bay_t){.bus = MEDIA_BUS_SCSI, .unit = p->cdrom_id, .label = "CD-ROM"};
    return true;
}

int profile_floppy_count(const hw_profile_t *p) {
    int n = 0;
    for (const struct floppy_slot *s = p ? p->floppy_slots : NULL; s && s->label; s++)
        n++;
    return n;
}

// A bay as the profile exports it: {bus, id, label}.
static value_t media_bay_value(const media_bay_t *bay) {
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "bus", val_str(media_bus_name(bay->bus)));
    val_map_put(b, "id", val_int((int64_t)bay->unit));
    val_map_put(b, "label", val_str(bay->label ? bay->label : ""));
    return val_map_finish(b);
}

// Find a machine profile by its id string
const hw_profile_t *machine_find(const char *id) {
    if (!id)
        return NULL;
    for (size_t i = 0; i < builtin_machine_count; ++i) {
        if (strcmp(builtin_machines[i]->id, id) == 0)
            return builtin_machines[i];
    }
    return NULL;
}

// Enumerate the built-in profiles (out_count receives the array length).
const hw_profile_t *const *machine_list(size_t *out_count) {
    if (out_count)
        *out_count = builtin_machine_count;
    return builtin_machines;
}

// === Object-model class descriptor =========================================
//
// machine is a process-singleton namespace: registered once at shell_init
// (machine_init below) and never torn down.  Per-instance attribute getters
// read from `global_emulator` rather than `object_data(self)` so the live
// machine state is reflected regardless of when the object was attached
// and how many cfg lifetimes have come and gone since.  Pre-boot reads
// return V_ERROR — no soft fallbacks; callers gate on `machine.created`.

extern config_t *global_emulator;

// Resolve the active profile or return V_ERROR for the named attribute.
static const hw_profile_t *active_profile_or_error(const char *attr_name, value_t *out_err) {
    config_t *cfg = global_emulator;
    if (!cfg || !cfg->machine) {
        *out_err = val_err("machine.%s: no machine booted; check machine.created first", attr_name);
        return NULL;
    }
    return cfg->machine;
}

static DEF_GETTER(attr_machine_id) {
    value_t err;
    const hw_profile_t *p = active_profile_or_error("id", &err);
    if (!p)
        return err;
    return val_str(p->id ? p->id : "");
}

// `catalog.models` — every registered model id, in registry order.  Answers
// without a running machine, so a script can iterate the roster instead of
// keeping its own copy of it.
static DEF_GETTER(attr_catalog_models) {
    size_t n = 0;
    const hw_profile_t *const *list = machine_list(&n);
    value_t *items = n ? (value_t *)calloc(n, sizeof(value_t)) : NULL;
    if (n && !items)
        return val_err("catalog.models: out of memory");
    for (size_t i = 0; i < n; i++)
        items[i] = val_str(list[i]->id);
    return val_list(items, n);
}

static DEF_GETTER(attr_machine_name) {
    value_t err;
    const hw_profile_t *p = active_profile_or_error("name", &err);
    if (!p)
        return err;
    return val_str(p->name ? p->name : "");
}

static DEF_GETTER(attr_machine_freq) {
    value_t err;
    const hw_profile_t *p = active_profile_or_error("freq", &err);
    if (!p)
        return err;
    return val_uint(4, p->freq);
}

static DEF_GETTER(attr_machine_ram) {
    config_t *cfg = global_emulator;
    if (!cfg || !cfg->machine)
        return val_err("machine.ram: no machine booted; check machine.created first");
    return val_uint(4, cfg->ram_size / 1024u);
}

// `machine.irq` and `machine.ipl` — the family's raw interrupt-source
// bitmap and the level the CPU is actually seeing.
//
// Every family aggregates its controllers into cfg->irq and resolves one
// IPL from it, and neither was readable from anywhere: an investigation
// could see a controller's own view through machine.<chip> and the CPU's
// behaviour, with the step between them invisible.  The bit meanings are
// per family (MAC030_GLUE_IRQ_* and the family equivalents), which is why
// this is a bitmap and not an enum.
static DEF_GETTER(attr_machine_irq) {
    config_t *cfg = global_emulator;
    if (!cfg || !cfg->machine)
        return val_err("machine.irq: no machine");
    value_t v = val_uint(4, (uint64_t)(uint32_t)cfg->irq);
    v.flags |= VAL_HEX;
    return v;
}

static DEF_GETTER(attr_machine_ipl) {
    config_t *cfg = global_emulator;
    if (!cfg || !cfg->machine)
        return val_err("machine.ipl: no machine");
    if (!cfg->cpu)
        return val_uint(1, 0); // a PowerPC machine has an external-interrupt pin, not an IPL
    return val_uint(1, cpu_get_ipl(cfg->cpu));
}

static DEF_GETTER(attr_machine_created) {
    config_t *cfg = global_emulator;
    return val_bool(cfg && cfg->machine != NULL);
}

// Build the `capabilities` map of the profile.  Every field is DERIVED
// from the hardware facts + mmu_kind so it can never drift from
// behaviour: the frontend probes this instead of guessing
// from the model's display name.
static value_t build_capabilities(const hw_profile_t *p) {
    value_map_builder_t *cpu = val_map_new();
    val_map_put(cpu, "model", val_int((int64_t)p->cpu_model)); // 68000 / 68030 / 68040 / 601 / 604
    val_map_put(cpu, "address_bits", val_int((int64_t)p->address_bits));
    val_map_put(cpu, "fpu", val_bool(cpu_has_fpu(p->cpu_model)));

    // Typed, not a bool: the debug panels must tell a 68030 PMMU (show
    // TC/CRP/SRP/TT0/TT1/MMUSR) from the Lisa segment MMU (don't) from none.
    value_map_builder_t *mmu = val_map_new();
    val_map_put(mmu, "present", val_bool(p->mmu_kind != MMU_NONE));
    val_map_put(mmu, "kind", val_str(mmu_kind_to_string(p->mmu_kind)));

    value_map_builder_t *b = val_map_new();
    val_map_put(b, "cpu", val_map_finish(cpu));
    val_map_put(b, "mmu", val_map_finish(mmu));
    // NOTE: video configurability is the video_slots block, NOT "nubus
    // exists" — the two are deliberately not conflated.
    val_map_put(b, "nubus", val_bool(p->nubus_slots != NULL));
    // Same rule for PCI: "the machine has PCI sockets", which is what the
    // dialog's Expansion Slots section probes for.
    val_map_put(b, "pci", val_bool(p->pci_slots != NULL));
    // On-board video digitizer (webcam capture) — gates the camera UI.
    val_map_put(b, "video_in", val_bool(p->has_video_in));
    // On-board audio input (microphone capture) — gates the mic UI.
    val_map_put(b, "audio_in", val_bool(p->has_audio_in));

    // Auxiliary CPU cores (heterogeneous multi-CPU): asserted from data,
    // never from model names — empty list on machines without any.
    value_t *aux = NULL;
    size_t n_aux = 0, cap_aux = 0;
    if (p->aux_cpus) {
        for (const struct aux_cpu_slot *a = p->aux_cpus; a->name; a++) {
            value_map_builder_t *ab = val_map_new();
            val_map_put(ab, "name", val_str(a->name));
            val_map_put(ab, "arch", val_str(a->arch));
            val_map_put(ab, "freq", val_int((int64_t)a->freq));
            val_list_push(&aux, &n_aux, &cap_aux, val_map_finish(ab));
        }
    }
    val_map_put(b, "aux_cpus", val_list(aux, n_aux));
    return val_map_finish(b);
}

// Build one video card map (id, display_name, requires_vrom, monitors).
// requires_vrom is read straight off the card kind — the property the
// dialog drives its VROM row from.
static value_t build_video_card(const char *card_id) {
    const nubus_card_kind_t *kind = card_id ? nubus_card_find(card_id) : NULL;
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "id", val_str(card_id ? card_id : ""));
    val_map_put(b, "display_name",
                val_str((kind && kind->display_name) ? kind->display_name : (card_id ? card_id : "")));
    val_map_put(b, "requires_vrom", val_bool(kind ? kind->requires_vrom : false));
    value_t *mons = NULL;
    size_t n_mons = 0, cap_mons = 0;
    if (kind && kind->monitors) {
        for (const nubus_monitor_t *mon = kind->monitors; mon->id; mon++) {
            value_map_builder_t *mb = val_map_new();
            val_map_put(mb, "id", val_str(mon->id));
            val_map_put(mb, "name", val_str(mon->name ? mon->name : mon->id));
            val_map_put(mb, "width", val_int((int64_t)mon->width));
            val_map_put(mb, "height", val_int((int64_t)mon->height));
            value_t *depths = NULL;
            size_t n_depths = 0, cap_depths = 0;
            if (mon->depths) {
                for (const int *d = mon->depths; *d; d++)
                    val_list_push(&depths, &n_depths, &cap_depths, val_int((int64_t)*d));
            }
            val_map_put(mb, "depths", val_list(depths, n_depths));
            val_list_push(&mons, &n_mons, &cap_mons, val_map_finish(mb));
        }
    }
    val_map_put(b, "monitors", val_list(mons, n_mons));
    return val_map_finish(b);
}

// Build the `video_slots` list: the real shape the user navigates — slot →
// card → monitor/depth.  VROM-required-ness is per *card* (the SE/30-vs-IIci
// asymmetry), so the dialog shows the VROM row iff the selected card needs
// one.  This is the ONLY video shape in the profile — the flat web-legacy
// `video_modes` compat view was deleted with that UI.
static value_t build_video_slots(const hw_profile_t *p) {
    value_t *slots = NULL;
    size_t n_slots = 0, cap_slots = 0;
    if (!p->nubus_slots)
        return val_list(NULL, 0);
    for (const struct nubus_slot_decl *s = p->nubus_slots; s->slot; s++) {
        // Only slots that can carry a video card appear here.  Every SOCKET
        // is emitted (a machine may declare several); the dialog's single
        // picker configures the first one, per-socket UI comes later.
        if (s->kind != NUBUS_SLOT_BUILTIN && s->kind != NUBUS_SLOT_SOCKET)
            continue;
        const char *default_card = (s->kind == NUBUS_SLOT_BUILTIN) ? s->builtin_card_id : s->default_card;

        value_map_builder_t *b = val_map_new();
        if (s->kind == NUBUS_SLOT_BUILTIN) {
            val_map_put(b, "slot", val_str("builtin"));
        } else {
            char slot_buf[8];
            snprintf(slot_buf, sizeof slot_buf, "%X", s->slot); // "9".."E"
            val_map_put(b, "slot", val_str(slot_buf));
        }
        // A BUILTIN slot may have sibling kinds (same monitor table, both
        // BUILTIN-attach — the SE/30's generic/real video pair): those are
        // selectable via video_card=, so the slot is only "fixed" when the
        // declared kind has no sibling.
        int builtin_candidates = 1;
        const nubus_card_kind_t *decl_kind =
            (s->kind == NUBUS_SLOT_BUILTIN) ? nubus_card_find(s->builtin_card_id) : NULL;
        if (decl_kind) {
            for (const nubus_card_kind_t *const *k = nubus_card_registry(); *k; k++) {
                if (*k != decl_kind && (*k)->attach == CARD_ATTACH_BUILTIN && (*k)->monitors == decl_kind->monitors)
                    builtin_candidates++;
            }
        }
        val_map_put(b, "fixed", val_bool(s->kind == NUBUS_SLOT_BUILTIN && builtin_candidates == 1));
        val_map_put(b, "default_card", val_str(default_card ? default_card : ""));

        value_t *cards = NULL;
        size_t n_cards = 0, cap_cards = 0;
        if (s->kind == NUBUS_SLOT_BUILTIN) {
            val_list_push(&cards, &n_cards, &cap_cards, build_video_card(s->builtin_card_id));
            // Sibling builtin kinds sharing the monitor table are selectable
            // via video_card=, so offer them alongside the declared one.
            if (decl_kind) {
                for (const nubus_card_kind_t *const *k = nubus_card_registry(); *k; k++) {
                    if (*k != decl_kind && (*k)->attach == CARD_ATTACH_BUILTIN && (*k)->monitors == decl_kind->monitors)
                        val_list_push(&cards, &n_cards, &cap_cards, build_video_card((*k)->id));
                }
            }
        } else {
            // Candidates are COMPUTED from the card registry: every kind
            // whose declared attachment fits this slot and that drives a
            // display.  Machines never enumerate cards — adding a card to
            // the registry offers it on every compatible machine.
            for (const nubus_card_kind_t *const *k = nubus_card_registry(); *k; k++) {
                if (nubus_card_fits_socket(s, *k) && (*k)->monitors)
                    val_list_push(&cards, &n_cards, &cap_cards, build_video_card((*k)->id));
            }
        }
        val_map_put(b, "cards", val_list(cards, n_cards));
        val_list_push(&slots, &n_slots, &cap_slots, val_map_finish(b));
    }
    return val_list(slots, n_slots);
}

// Build one PCI card map (id, display_name, requires_prom, class,
// monitors).  `class` is the UI grouping hint the dialog needs now that
// non-display cards are the point of the bus; it is a property of the
// DRIVER, not of the machine.
static value_t build_pci_card(const char *card_id) {
    const pci_card_kind_t *kind = card_id ? pci_card_find(card_id) : NULL;
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "id", val_str(card_id ? card_id : ""));
    val_map_put(b, "display_name",
                val_str((kind && kind->display_name) ? kind->display_name : (card_id ? card_id : "")));
    val_map_put(b, "requires_prom", val_bool(kind ? kind->requires_prom : false));
    const char *klass =
        (kind && kind->card_class) ? kind->card_class : ((kind && kind->monitors) ? "display" : "other");
    val_map_put(b, "class", val_str(klass));
    // The options this card offers, declared by the KIND so the dialog can
    // render a control per option without knowing which card it is.
    value_t *opts = NULL;
    size_t n_opts = 0, cap_opts = 0;
    for (const pci_card_option_t *o = kind ? kind->options : NULL; o && o->key; o++) {
        value_map_builder_t *ob = val_map_new();
        val_map_put(ob, "key", val_str(o->key));
        val_map_put(ob, "label", val_str(o->label ? o->label : o->key));
        val_map_put(ob, "default_value", val_str(o->default_value ? o->default_value : ""));
        value_t *vals = NULL;
        size_t n_vals = 0, cap_vals = 0;
        for (size_t i = 0; o->values && o->values[i]; i++) {
            value_map_builder_t *vb = val_map_new();
            val_map_put(vb, "id", val_str(o->values[i]));
            val_map_put(vb, "label", val_str((o->labels && o->labels[i]) ? o->labels[i] : o->values[i]));
            val_list_push(&vals, &n_vals, &cap_vals, val_map_finish(vb));
        }
        val_map_put(ob, "values", val_list(vals, n_vals));
        val_list_push(&opts, &n_opts, &cap_opts, val_map_finish(ob));
    }
    val_map_put(b, "options", val_list(opts, n_opts));
    value_t *mons = NULL;
    size_t n_mons = 0, cap_mons = 0;
    if (kind && kind->monitors) {
        for (const nubus_monitor_t *mon = kind->monitors; mon->id; mon++) {
            value_map_builder_t *mb = val_map_new();
            val_map_put(mb, "id", val_str(mon->id));
            val_map_put(mb, "name", val_str(mon->name ? mon->name : mon->id));
            val_map_put(mb, "width", val_int((int64_t)mon->width));
            val_map_put(mb, "height", val_int((int64_t)mon->height));
            value_t *depths = NULL;
            size_t n_depths = 0, cap_depths = 0;
            if (mon->depths) {
                for (const int *d = mon->depths; *d; d++)
                    val_list_push(&depths, &n_depths, &cap_depths, val_int((int64_t)*d));
            }
            val_map_put(mb, "depths", val_list(depths, n_depths));
            val_list_push(&mons, &n_mons, &cap_mons, val_map_finish(mb));
        }
    }
    val_map_put(b, "monitors", val_list(mons, n_mons));
    return val_map_finish(b);
}

// Build the `pci_slots` list: one entry per declared socket and builtin,
// with the fitting cards COMPUTED from the registry (pci_card_fits_socket)
// exactly as video_slots does for NuBus.  Deliberately NOT folded into
// video_slots — that block is display-only by construction and the
// frontend depends on its shape.
static value_t build_pci_slots(const hw_profile_t *p) {
    value_t *slots = NULL;
    size_t n_slots = 0, cap_slots = 0;
    if (!p->pci_slots)
        return val_list(NULL, 0);
    for (const struct pci_slot_decl *s = p->pci_slots; s->slot; s++) {
        if (s->kind != PCI_SLOT_BUILTIN && s->kind != PCI_SLOT_BUILTIN_FALLBACK && s->kind != PCI_SLOT_SOCKET)
            continue;
        value_map_builder_t *b = val_map_new();
        val_map_put(b, "slot", val_int((int64_t)s->slot));
        val_map_put(b, "label", val_str(s->label ? s->label : ""));
        val_map_put(b, "bus", val_int((int64_t)s->bus));
        val_map_put(b, "device", val_int((int64_t)s->device));
        val_map_put(b, "irq", val_int((int64_t)s->int_line));
        // A builtin is soldered down: the dialog renders it as a label,
        // not a picker.
        bool builtin = s->kind == PCI_SLOT_BUILTIN || s->kind == PCI_SLOT_BUILTIN_FALLBACK;
        val_map_put(b, "fixed", val_bool(builtin));
        // ...and a FALLBACK builtin is not the machine's own hardware at
        // all: it stands in only while no socket supplies a card of the
        // same class, because the real machine has nothing there.  A
        // frontend that cannot tell the two apart shows a Power Macintosh
        // 9500 as having on-board video, which is the one thing that
        // machine is documented not to have.
        val_map_put(b, "fallback", val_bool(s->kind == PCI_SLOT_BUILTIN_FALLBACK));
        const char *default_card = builtin ? s->builtin_card_id : s->default_card;
        val_map_put(b, "default_card", val_str(default_card ? default_card : ""));

        value_t *cards = NULL;
        size_t n_cards = 0, cap_cards = 0;
        if (builtin) {
            val_list_push(&cards, &n_cards, &cap_cards, build_pci_card(s->builtin_card_id));
        } else {
            // Candidates are COMPUTED: every registered kind whose declared
            // attachment fits this socket.  Adding a card driver offers it
            // on every compatible machine with no machine-side edit.
            for (const pci_card_kind_t *const *k = pci_card_registry(); *k; k++) {
                if (!pci_card_fits_socket(s, *k))
                    continue;
                if ((*k)->offered && !(*k)->offered())
                    continue; // needs a host facility this host lacks
                val_list_push(&cards, &n_cards, &cap_cards, build_pci_card((*k)->id));
            }
        }
        val_map_put(b, "cards", val_list(cards, n_cards));
        val_list_push(&slots, &n_slots, &cap_slots, val_map_finish(b));
    }
    return val_list(slots, n_slots);
}

// The machine's own built-in video, plus the monitors its port can be
// strapped with.  `none` is always last and is what switches the port —
// and therefore built-in video — off.  Empty map when the machine has no
// substrate built-in video to choose.
static value_t build_builtin_video(const hw_profile_t *p) {
    value_map_builder_t *b = val_map_new();
    if (!p->builtin_video)
        return val_map_finish(b);
    val_map_put(b, "id", val_str("builtin"));
    val_map_put(b, "display_name", val_str(p->builtin_video->display_name));
    value_t *mons = NULL;
    size_t n = 0, cap = 0;
    // The family owns its monitor list; this walks it without knowing the
    // per-monitor data behind it (the PDM's sense strap).
    const char *mid = NULL, *mname = NULL;
    for (size_t i = 0; p->builtin_video->monitor_at && p->builtin_video->monitor_at(i, &mid, &mname); i++) {
        value_map_builder_t *mb = val_map_new();
        val_map_put(mb, "id", val_str(mid));
        val_map_put(mb, "name", val_str(mname));
        val_list_push(&mons, &n, &cap, val_map_finish(mb));
    }
    val_map_put(b, "monitors", val_list(mons, n));
    return val_map_finish(b);
}

// Build the typed profile map for a registered hw_profile_t.
static value_t build_profile(const hw_profile_t *p) {
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "id", val_str(p->id ? p->id : ""));
    val_map_put(b, "name", val_str(p->name ? p->name : ""));
    val_map_put(b, "freq", val_int((int64_t)p->freq));

    value_t *rams = NULL;
    size_t n_rams = 0, cap_rams = 0;
    if (p->ram_options) {
        for (const uint32_t *r = p->ram_options; *r; r++)
            val_list_push(&rams, &n_rams, &cap_rams, val_int((int64_t)*r));
    }
    val_map_put(b, "ram_options", val_list(rams, n_rams));

    val_map_put(b, "ram_default", val_int((int64_t)(p->ram_default / 1024u)));
    val_map_put(b, "ram_max", val_int((int64_t)(p->ram_max / 1024u)));

    value_t *flops = NULL;
    size_t n_flops = 0, cap_flops = 0;
    if (p->floppy_slots) {
        for (const struct floppy_slot *s = p->floppy_slots; s->label; s++) {
            value_map_builder_t *fb = val_map_new();
            val_map_put(fb, "label", val_str(s->label));
            val_map_put(fb, "kind", val_str(floppy_kind_to_string(s->kind)));
            val_list_push(&flops, &n_flops, &cap_flops, val_map_finish(fb));
        }
    }
    val_map_put(b, "floppy_slots", val_list(flops, n_flops));

    // Buses, each carrying its own bays.  The `object` field is what a
    // consumer attaches media through (machine.<object>.attach_hd), so no
    // caller needs to know which machines have a second controller.
    value_t *buses = NULL;
    size_t n_buses = 0, cap_buses = 0;
    for (const struct scsi_bus_decl *bus = p->scsi_buses; bus && bus->object; bus++) {
        value_t *scsis = NULL;
        size_t n_scsis = 0, cap_scsis = 0;
        for (const struct scsi_slot *s = bus->slots; s && s->label; s++) {
            value_map_builder_t *sb = val_map_new();
            val_map_put(sb, "label", val_str(s->label));
            val_map_put(sb, "id", val_int((int64_t)s->id));
            val_map_put(sb, "boot", val_bool(s->boot));
            val_list_push(&scsis, &n_scsis, &cap_scsis, val_map_finish(sb));
        }
        value_map_builder_t *bb = val_map_new();
        val_map_put(bb, "object", val_str(bus->object));
        val_map_put(bb, "label", val_str(bus->label));
        val_map_put(bb, "slots", val_list(scsis, n_scsis));
        val_list_push(&buses, &n_buses, &cap_buses, val_map_finish(bb));
    }
    val_map_put(b, "scsi_buses", val_list(buses, n_buses));

    val_map_put(b, "hd_bus", val_str(hd_bus_to_string(p->hd_bus)));

    val_map_put(b, "has_cdrom", val_bool(p->has_cdrom));
    val_map_put(b, "cdrom_id", val_int((int64_t)p->cdrom_id));

    // The derived bays: every hard-disk bay in attach order, the default
    // one (hd_bays[0]), and the CD bay -- {bus, id, label}, bus one of
    // "scsi" / "scsi2" / "profile".  What machine.attach_hd / attach_cdrom
    // use, exported so a UI can show the same answer before a boot.
    media_bay_t bays[MEDIA_HD_BAYS_MAX];
    int n_bays = profile_hd_bays(p, bays, MEDIA_HD_BAYS_MAX);
    value_t *bay_vals = NULL;
    size_t n_bay_vals = 0, cap_bay_vals = 0;
    for (int i = 0; i < n_bays; i++)
        val_list_push(&bay_vals, &n_bay_vals, &cap_bay_vals, media_bay_value(&bays[i]));
    val_map_put(b, "hd_bays", val_list(bay_vals, n_bay_vals));
    val_map_put(b, "hd_default", n_bays > 0 ? media_bay_value(&bays[0]) : val_none());
    media_bay_t cd;
    val_map_put(b, "cdrom", profile_cdrom_bay(p, &cd) ? media_bay_value(&cd) : val_none());

    // Derived capability probe + per-card video-slot shape —
    // the source of truth the frontend consumes.  (The web-legacy compat keys
    // needs_vrom / video_modes / video_mode_default were deleted with that UI;
    // everything derives from video_slots now.)
    val_map_put(b, "capabilities", build_capabilities(p));
    val_map_put(b, "video_slots", build_video_slots(p));
    // PCI expansion topology: one row per declared socket / builtin, with
    // the fitting cards computed per socket (docs/internals/core/peripherals/pci.md).
    val_map_put(b, "pci_slots", build_pci_slots(p));
    // Substrate built-in video, when the machine has one that is NOT a
    // BUILTIN slot pseudo-card (the PDM family's Ariel scanout).  The
    // configuration dialog offers it beside the NuBus cards; picking a card
    // instead means strapping this port unconnected (monitor="none"), which
    // is what a real machine does when you plug the monitor into the card.
    val_map_put(b, "builtin_video", build_builtin_video(p));

    return val_map_finish(b);
}

// catalog.profile(id) — static lookup, returns the model's full configuration
// shape as a typed map.  Errors when id is empty
// or doesn't match a registered profile.
static DEF_METHOD(catalog_method_profile) {
    const char *id = argv[0].s;
    if (!id || !*id)
        return val_err("catalog.profile: id must be non-empty");
    const hw_profile_t *p = machine_find(id);
    if (!p)
        return val_err("catalog.profile: unknown model '%s'", id);
    return build_profile(p);
}

// Build a comma-separated list of allowed RAM sizes for the error message.
static void format_ram_options(char *buf, size_t bufsize, const hw_profile_t *p) {
    size_t pos = 0;
    if (!p->ram_options) {
        snprintf(buf, bufsize, "<none>");
        return;
    }
    for (const uint32_t *r = p->ram_options; *r && pos + 16 < bufsize; r++) {
        int n = snprintf(buf + pos, bufsize - pos, "%s%u", pos ? "," : "", *r);
        if (n < 0)
            break;
        pos += (size_t)n;
    }
    if (pos == 0)
        snprintf(buf, bufsize, "<none>");
}

// === Boot document ==========================================================
//
// machine.boot consumes one atomic, COMPLETE configuration document: model
// and rom are required, every other field falls back to the model's own
// defaults — never to another machine's record (a field the caller did not
// write must not arrive from somewhere the caller cannot see).  All
// validation runs BEFORE the old machine is torn down, so a rejected boot
// leaves the running machine untouched.  machine.restart is the verb for
// "power-cycle this machine".

// Stamp the record's `created` field with the current UTC time (ISO8601).
static void stamp_created(char *buf, size_t bufsize) {
    time_t now = time(NULL);
    struct tm tm_utc;
    if (gmtime_r(&now, &tm_utc))
        strftime(buf, bufsize, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    else
        snprintf(buf, bufsize, "unknown");
}

// Read the document's ROM -- the file, or the two Lisa/XL chips interleaved --
// and check it is a recognised ROM for an emulated machine, compatible with
// `profile`, and exactly the model's ROM size.  On success *bytes is the
// caller's to free and *rom describes it.
static value_t boot_rom_read(const boot_config_t *doc, const hw_profile_t *profile, uint8_t **bytes, rom_image_t *rom,
                             rom_identity_t *id) {
    size_t size = 0;
    uint8_t *data = (doc->rom2 && *doc->rom2) ? rom_load_lisa_pair(doc->rom, doc->rom2, &size)
                                              : rom_read_file(doc->rom, &size, true);
    if (!data) {
        if (doc->rom2 && *doc->rom2)
            return val_err("machine.boot: cannot read the ROM chip pair '%s' / '%s'", doc->rom, doc->rom2);
        return val_err("machine.boot: cannot read rom '%s'", doc->rom);
    }
    const rom_info_t *info = rom_identify_data(data, size, id);
    value_t err = val_none();
    if (!info) {
        err = val_err("machine.boot: rom '%s' is not a recognised ROM image (id %s)", doc->rom,
                      id->id[0] ? id->id : "none");
    } else if (!rom_is_supported(info)) {
        // A real ROM we know, for a machine that is not emulated.
        err = val_err("machine.boot: rom '%s' is the %s, for a machine Granny Smith does not emulate", doc->rom,
                      info->family_name);
    } else {
        bool ok = false;
        for (const char *const *p = info->compatible; *p; p++) {
            if (strcmp(*p, profile->id) == 0) {
                ok = true;
                break;
            }
        }
        if (!ok)
            err = val_err("machine.boot: rom '%s' (%s) is not compatible with model '%s'", doc->rom, info->family_name,
                          profile->id);
        else if (size != profile->rom_size)
            err = val_err("machine.boot: rom '%s' is %zu bytes; %s takes a %u-byte ROM", doc->rom, size, profile->id,
                          profile->rom_size);
    }
    if (val_is_error(&err)) {
        free(data);
        return err;
    }
    gs_outf("ROM: %s (id %s)\n", info->family_name, id->id);
    // A damaged dump still boots -- research on damaged or hand-edited images
    // is a legitimate use -- but says which part does not verify.
    if (!id->intact)
        gs_outf("Warning: ROM %s — the dump is probably damaged\n", id->reason);
    *bytes = data;
    *rom = (rom_image_t){.data = data, .size = size, .path = doc->rom};
    return val_none();
}

// Apply one boot document: validate → tear down → construct → record.
// Shared by machine.boot, machine.restart and headless startup.  Returns
// V_NONE on success, V_ERROR (with the old machine still running) on
// rejection.
value_t machine_boot_apply(const boot_config_t *doc_in) {
    boot_config_t doc = *doc_in;

    // 1. Validation — all of it before system_destroy.  The document is the
    // whole specification: nothing is filled in from the previous machine's
    // record (a field the caller did not write must not arrive from
    // somewhere the caller cannot see).
    if (!doc.model || !*doc.model)
        return val_err("machine.boot: model is required (machine.restart power-cycles the running machine)");
    const hw_profile_t *profile = machine_find(doc.model);
    if (!profile)
        return val_err("machine.boot: unknown model '%s'", doc.model);

    uint32_t ram_kb = doc.ram_kb;
    if (ram_kb == 0)
        ram_kb = profile->ram_default / 1024u;
    if (!hw_profile_ram_option_allowed(profile, ram_kb)) {
        char options[128];
        format_ram_options(options, sizeof(options), profile);
        return val_err("machine.boot: ram %u KB not in profile.ram_options for %s [%s]", ram_kb, profile->name,
                       options);
    }

    if (!doc.rom || !*doc.rom)
        return val_err("machine.boot: rom is required (machine.restart power-cycles the running machine)");
    // The ROM itself is read, identified and checked last (boot_rom_read),
    // just before the running machine is touched: the new machine is built
    // with its bytes.

    // 0..7 is the passive sense code; 8..14 is Apple's own indexed numbering
    // for the monitors that answer the EXTENDED (tie-matrix) probe instead
    // (dafb.h's DAFB_SENSE_INDEXED_*).  Only the DAFB models the extended
    // range today, so the JMFB takes the passive part only.
    // Both bounds.  The ceiling was checked and the floor was not, so any
    // negative other than the -1 "unset" sentinel fell through to the
    // `video_sense >= 0` guard below and was silently ignored --
    // demonstrated: video_sense=-7 booted with no diagnostic, and presents to
    // the caller as "my video_sense= was ignored".
    if (doc.video_sense < -1 || doc.video_sense >= (int)DAFB_SENSE_INDEXED_MAX)
        return val_err("machine.boot: video_sense must be 0..%u, or -1 for unset (got %d)", DAFB_SENSE_INDEXED_MAX - 1u,
                       doc.video_sense);
    if (doc.monitor && *doc.monitor) {
        if (!profile->builtin_video)
            return val_err("machine.boot: model '%s' has no configurable built-in video port", profile->id);
        // Validated HERE, against the family's own list, because everything in
        // machine_boot_apply must be rejected before the running machine is
        // touched.
        bool known = false;
        const char *mid = NULL, *mname = NULL;
        for (size_t i = 0; profile->builtin_video->monitor_at && profile->builtin_video->monitor_at(i, &mid, &mname);
             i++) {
            if (strcmp(mid, doc.monitor) == 0) {
                known = true;
                break;
            }
        }
        if (!known)
            return val_err("machine.boot: unknown monitor id '%s' (see catalog.profile)", doc.monitor);
    }
    // The card ROMs beside this ROM join the offer registry before the slot
    // check reads it.  Offers are content-addressed and persist, so this only
    // adds; a ROM booted from another directory than the platform's startup
    // one used to find none of its siblings (#187).
    platform_offer_sibling_card_roms(doc.rom);

    // The expansion slots: slots= and its sugar (video_card=, video_mode=,
    // custom_mode=, pci_card=, pci_option=, vrom=, prom=) resolved into one
    // validated entry per configured slot -- card fit, mode, geometry,
    // options and ROM -- before the running machine is touched.
    machine_build_opts_t build_opts = machine_build_opts_default();
    value_t serr = machine_slots_resolve(profile, &doc, &build_opts);
    if (val_is_error(&serr))
        return serr;

    // The ROM, read now: the machine is built with it, so a ROM that cannot
    // be read, is unknown, belongs to another model or has the wrong size is
    // a rejected boot, never a machine without a ROM.
    uint8_t *rom_bytes = NULL;
    rom_image_t build_rom;
    rom_identity_t rom_identity;
    value_t rerr = boot_rom_read(&doc, profile, &rom_bytes, &build_rom, &rom_identity);
    if (val_is_error(&rerr))
        return rerr;

    // One channel for every video model that needs the sense at construction
    // -- the JMFB cards, the Quadras' DAFB, PDM's Ariel.
    build_opts.ram_kb = ram_kb; // validated and defaulted above
    build_opts.rom = build_rom; // read and validated above
    if (doc.video_sense >= 0)
        build_opts.video_sense = doc.video_sense;
    // The built-in monitor strap resolves to a sense code and joins the other
    // build options.  Validated above, so this cannot fail.
    if (doc.monitor && *doc.monitor) {
        uint8_t mon_sense = 0;
        if (profile->builtin_video->monitor_sense(doc.monitor, &mon_sense))
            build_opts.video_sense = mon_sense;
    }

    // 3. Build, then swap, then destroy.  The new machine is built from the
    // document alone while the running one is untouched; only a complete
    // build replaces it.
    machine_config_reset_vroms();
    machine_config_reset_slot_cards();
    config_t *cfg = system_create(profile, &build_opts, NULL);
    free(rom_bytes); // copied into the ROM region
    if (!cfg)
        return val_err("machine.boot: failed to create %s", profile->id);

    // 4. The built-from record — the machine's birth certificate.
    machine_config_record_t *w = machine_config_record_mut();
    snprintf(w->model, sizeof(w->model), "%s", profile->id);
    w->ram_kb = cfg->ram_size / 1024u;
    snprintf(w->rom, sizeof(w->rom), "%s", doc.rom);
    snprintf(w->rom_id, sizeof(w->rom_id), "%s", rom_identity.id);
    snprintf(w->rom2, sizeof(w->rom2), "%s", doc.rom2 ? doc.rom2 : "");
    snprintf(w->video_card, sizeof(w->video_card), "%s", doc.video_card ? doc.video_card : "");
    w->video_sense = doc.video_sense;
    snprintf(w->video_mode, sizeof(w->video_mode), "%s", doc.video_mode ? doc.video_mode : "");
    snprintf(w->custom_mode, sizeof(w->custom_mode), "%s", doc.custom_mode ? doc.custom_mode : "");
    snprintf(w->monitor, sizeof(w->monitor), "%s", doc.monitor ? doc.monitor : "");
    snprintf(w->pci_card, sizeof(w->pci_card), "%s", doc.pci_card ? doc.pci_card : "");
    snprintf(w->pci_option, sizeof(w->pci_option), "%s", doc.pci_option ? doc.pci_option : "");
    w->n_slots = build_opts.n_slots;
    memcpy(w->slots, build_opts.slots, sizeof(w->slots));
    stamp_created(w->created, sizeof(w->created));
    w->valid = true;

    // 5. The swap: the new machine becomes the active one, and the one it
    // replaces is destroyed.
    system_swap_in(cfg, false);

    LOG(1, "Machine created: %s (%s), RAM: %u KB", profile->name, profile->id, cfg->ram_size / 1024u);
    return val_none();
}

// A boot-document field the caller left out arrives as V_NONE;
// read it as the "not given" value the document uses: "" or 0.
static const char *boot_str(const value_t *v) {
    return (v->kind == V_STRING && v->s) ? v->s : "";
}
static uint64_t boot_uint(const value_t *v, uint64_t unset) {
    return v->kind == V_UINT ? v->u : unset;
}

// machine.boot — atomic, self-contained configuration document.  model and
// rom are required; every other field falls back to the model's own
// defaults.  An explicitly empty value is rejected by the named-argument
// grammar.  Use machine.restart to power-cycle the running machine.
static DEF_METHOD(machine_method_boot) {
    uint64_t sense = boot_uint(&argv[5], 0xFF);
    boot_config_t doc = {
        .model = boot_str(&argv[0]),
        .ram_kb = (uint32_t)boot_uint(&argv[1], 0),
        .rom = boot_str(&argv[2]),
        .vrom = boot_str(&argv[3]),
        .video_card = boot_str(&argv[4]),
        .video_sense = (sense == 0xFF) ? -1 : (int)sense,
        .video_mode = boot_str(&argv[6]),
        .rom2 = boot_str(&argv[7]),
        .custom_mode = boot_str(&argv[8]),
        .monitor = boot_str(&argv[9]),
        .pci_card = boot_str(&argv[10]),
        .prom = boot_str(&argv[11]),
        .pci_option = boot_str(&argv[12]),
        .slots = boot_str(&argv[13]),
    };
    value_t err = machine_boot_apply(&doc);
    if (val_is_error(&err))
        return err;
    value_free(&err);
    return val_bool(true);
}

// machine.reset — level 2, a warm reset: the board's /RESET net plus the CPU
// back to its reset vector, with the machine left standing.  Nothing is torn
// down and nothing is rebuilt, so RAM, the PRAM/NVRAM, mounted media and the
// object tree all survive; this is the reset button.
static DEF_METHOD(machine_method_reset) {
    if (!global_emulator)
        return val_err("machine.reset: no machine is running; boot one first");
    system_machine_reset();
    return val_bool(true);
}

// machine.restart — level 3, a power cycle: the same complete reset as
// machine.reset, with the RAM cold.  The machine is NOT torn down: switching a
// real machine off and on does not replace its chips, so the PRAM/NVRAM, the
// RTC (still ticking), mounted media, the Caps Lock latch and the LaserWriter
// all survive because nothing destroyed them.  The built-from record and
// `created` are untouched for the same reason.
static DEF_METHOD(machine_method_restart) {
    if (!global_emulator)
        return val_err("machine.restart: no machine is running; boot one first");
    system_machine_power_cycle();
    return val_bool(true);
}

// machine.register(id, created) — record the active machine identity for
// checkpointing. Routes to the platform's gs_register_machine.
static DEF_METHOD(machine_method_register) {
    return val_bool(gs_register_machine(argv[0].s, argv[1].s) == 0);
}

// Every field is optional to the binder (a field left out, even before a later
// named one, arrives as V_NONE): model and rom are checked as required inside
// machine_boot_apply so the message can point at machine.restart; the others
// fall back to the model's defaults.

static const arg_decl_t machine_boot_args[] = {
    {.name = "model",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Machine model id (plus / se30 / ...); required"},
    {.name = "ram",
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "RAM in KB (one of profile.ram_options)",
     .default_doc = "the model's"},
    {.name = "rom",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .presentation_flags = VAL_PATH,
     .doc = "ROM file path; required"},
    {.name = "vrom",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .presentation_flags = VAL_PATH,
     .doc = "Declaration-ROM file: the ROM of every slot whose card it provides",
     .default_doc = "resolved from the offers"},
    {.name = "video_card",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Card id for the first NuBus socket",
     .default_doc = "the slot's"},
    {.name = "video_sense",
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Monitor sense 0..7 (passive), 8..14 (extended)",
     .default_doc = "the card's"},
    {.name = "video_mode",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Video-mode id (see catalog.profile)",
     .default_doc = "the card's"},
    {.name = "rom2",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .presentation_flags = VAL_PATH,
     .doc = "Lisa/XL second ROM chip (two-chip form)",
     .default_doc = "a single-file rom"},
    {.name = "custom_mode",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Custom resolution WxHxD (generic 8_24 kind)"},
    {.name = "monitor",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Monitor on the built-in port ('none' = unconnected, which hands "
            "the screen to a NuBus card)", .default_doc = "the model's"},
    {.name = "pci_card",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Card id for the first PCI socket",
     .default_doc = "the slot's"},
    {.name = "prom",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .presentation_flags = VAL_PATH,
     .doc = "PCI expansion-ROM file: the ROM of every slot whose card it provides",
     .default_doc = "resolved from the offers"},
    {.name = "pci_option",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Options for the PCI card, \"key=value[,key=value]\" (e.g. \"vram=4m\")"},
    {.name = "slots",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Per-slot cards, \"SLOT=CARD[,key=value]*;...\" (keys mode / custom / rom, or card options; "
            "CARD none empties a socket)", .default_doc = "the slots' own cards"},
};

static const arg_decl_t machine_register_args[] = {
    {.name = "id",      .kind = V_STRING, .doc = "Machine identity (UUID-like)"},
    {.name = "created", .kind = V_STRING, .doc = "Creation timestamp"          },
};

static const arg_decl_t catalog_profile_args[] = {
    {.name = "id", .kind = V_STRING, .validation_flags = OBJ_ARG_NONEMPTY, .doc = "Machine model id (plus / se30)"},
};

// === machine.attach_hd / attach_cdrom / eject_media ===========
// Media by bay, not by bus: the running machine's profile says where a hard
// disk or a CD goes (profile_hd_bays / profile_cdrom_bay), and the substrate
// attaches it there, on whatever bus that is -- machine.scsi, machine.scsi2 or
// the Lisa's ProFile.  Each answers the bay it used, {bus, id, label}, which
// is what eject_media takes back.

static DEF_METHOD(machine_method_attach_hd) {
    config_t *cfg = global_emulator;
    if (!cfg || !cfg->machine)
        return val_err("machine.attach_hd: no machine is running");
    media_bay_t bays[MEDIA_HD_BAYS_MAX];
    int n = profile_hd_bays(cfg->machine, bays, MEDIA_HD_BAYS_MAX);
    int64_t which = (argc >= 2 && argv[1].kind == V_INT) ? argv[1].i : 0;
    if (n == 0)
        return val_err("machine.attach_hd: %s has no hard-disk bay", cfg->machine->name);
    if (which < 0 || which >= n)
        return val_err("machine.attach_hd: bay %lld does not exist (%s has %d)", (long long)which, cfg->machine->name,
                       n);
    char err[256];
    if (system_media_attach_path(cfg, &bays[which], false, argv[0].s, err, sizeof(err)) != 0)
        return val_err("machine.attach_hd: %s", err);
    return media_bay_value(&bays[which]);
}

static DEF_METHOD(machine_method_attach_cdrom) {
    config_t *cfg = global_emulator;
    if (!cfg || !cfg->machine)
        return val_err("machine.attach_cdrom: no machine is running");
    media_bay_t bay;
    if (!profile_cdrom_bay(cfg->machine, &bay))
        return val_err("machine.attach_cdrom: %s has no CD-ROM bay", cfg->machine->name);
    char err[256];
    if (system_media_attach_path(cfg, &bay, true, argv[0].s, err, sizeof(err)) != 0)
        return val_err("machine.attach_cdrom: %s", err);
    return media_bay_value(&bay);
}

static DEF_METHOD(machine_method_eject_media) {
    config_t *cfg = global_emulator;
    if (!cfg || !cfg->machine)
        return val_err("machine.eject_media: no machine is running");
    media_bus_t bus;
    if (!media_bus_parse(argv[0].s, &bus))
        return val_err("machine.eject_media: unknown bus '%s' (floppy, scsi, scsi2 or profile)", argv[0].s);
    int unit = (argc >= 2 && argv[1].kind == V_INT) ? (int)argv[1].i : 0;
    int rc = system_media_eject(cfg, bus, unit);
    if (rc == -2)
        return val_err("machine.eject_media: the guest has locked %s %d", argv[0].s, unit);
    if (rc != 0)
        return val_err("machine.eject_media: nothing to eject at %s %d", argv[0].s, unit);
    return val_none();
}

// The bay index's default: a named argument after it must stay reachable.
static const value_t k_bay0 = {.kind = V_INT, .i = 0};

static const arg_decl_t machine_attach_hd_args[] = {
    {.name = "path",
     .kind = V_STRING,
     .presentation_flags = VAL_PATH,
     .validation_flags = OBJ_ARG_NONEMPTY,
     .doc = "Hard-disk image path"                                   },
    {.name = "bay",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_bay0,
     .doc = "Which bay, in profile.hd_bays order (0 is the boot bay)"},
};

static const arg_decl_t machine_attach_cdrom_args[] = {
    {.name = "path",
     .kind = V_STRING,
     .presentation_flags = VAL_PATH,
     .validation_flags = OBJ_ARG_NONEMPTY,
     .doc = "CD-ROM image path"},
};

static const arg_decl_t machine_eject_media_args[] = {
    {.name = "bus", .kind = V_STRING, .validation_flags = OBJ_ARG_NONEMPTY, .doc = "floppy, scsi, scsi2 or profile"},
    {.name = "id",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_bay0,
     .doc = "Drive index or SCSI id"},
};

static const member_t machine_members[] = {
    {.kind = M_ATTR,
     .name = "id",
     .doc = "Active machine's model id (\"plus\" / \"se30\" / …)",
     .attr = {.type = V_STRING, .get = attr_machine_id, .set = NULL}},
    {.kind = M_ATTR,
     .name = "name",
     .doc = "Active machine's human-readable name",
     .attr = {.type = V_STRING, .get = attr_machine_name, .set = NULL}},
    {.kind = M_ATTR,
     .name = "freq",
     .doc = "Active machine's CPU clock in Hz",
     .attr = {.type = V_UINT, .get = attr_machine_freq, .set = NULL}},
    {.kind = M_ATTR,
     .name = "ram",
     .doc = "Active RAM size in KB",
     .attr = {.type = V_UINT, .get = attr_machine_ram, .set = NULL}},
    {.kind = M_ATTR,
     .name = "irq",
     .doc = "Raw interrupt-source bitmap the family aggregates (bit meanings are per family)",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX | VAL_VOLATILE, .get = attr_machine_irq, .set = NULL}},
    {.kind = M_ATTR,
     .name = "ipl",
     .doc = "CPU interrupt level asserted now (0 on a PowerPC machine, which has a single pin)",
     .attr = {.type = V_UINT, .presentation_flags = VAL_VOLATILE, .get = attr_machine_ipl, .set = NULL}},
    {.kind = M_ATTR,
     .name = "created",
     .doc = "True if a machine has been booted",
     .attr = {.type = V_BOOL, .get = attr_machine_created, .set = NULL}},
    {.kind = M_METHOD,
     .name = "boot",
     .examples = EXAMPLES("machine.boot model=plus rom=\"roms/plus.rom\"",
     "machine.boot model=se30 rom=\"roms/se30.rom\" ram=8192"),
     .doc = "Boot a machine from a complete configuration document; fields left out take the model's defaults",
     .method = {.args = machine_boot_args,
                .nargs = sizeof(machine_boot_args) / sizeof(machine_boot_args[0]),
                .result = V_BOOL,
                .fn = machine_method_boot}},
    {.kind = M_METHOD,
     .name = "reset",
     .examples = EXAMPLES("machine.reset"),
     .doc = "Warm-reset the running machine: the /RESET net plus the CPU, keeping RAM, PRAM and media",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = machine_method_reset}},
    {.kind = M_METHOD,
     .name = "restart",
     .examples = EXAMPLES("machine.restart"),
     .doc = "Power-cycle the running machine: a reset with the RAM cold; nothing is rebuilt, so PRAM/NVRAM, "
            "the clock and media survive", .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = machine_method_restart}},
    {.kind = M_METHOD,
     .name = "register",
     .flags = M_CAT_ADVANCED,
     .doc = "Record the active machine identity for checkpointing",
     .method = {.args = machine_register_args, .nargs = 2, .result = V_BOOL, .fn = machine_method_register}},
    {.kind = M_METHOD,
     .name = "attach_hd",
     .examples = EXAMPLES("machine.attach_hd \"images/system.img\"", "machine.attach_hd \"images/data.img\" 1"),
     .doc = "Attach a hard-disk image to a bay, on whatever bus the bay is",
     .method = {.result_doc = "{bus, id, label}: the bay, as eject_media names it",
                .args = machine_attach_hd_args,
                .nargs = 2,
                .result = V_MAP,
                .fn = machine_method_attach_hd}},
    {.kind = M_METHOD,
     .name = "attach_cdrom",
     .examples = EXAMPLES("machine.attach_cdrom \"images/install.iso\""),
     .doc = "Insert a CD-ROM image into the machine's CD bay",
     .method = {.result_doc = "{bus, id, label}: the bay, as eject_media names it",
                .args = machine_attach_cdrom_args,
                .nargs = 1,
                .result = V_MAP,
                .fn = machine_method_attach_cdrom}},
    {.kind = M_METHOD,
     .name = "eject_media",
     .examples = EXAMPLES("machine.eject_media floppy", "machine.eject_media scsi 3"),
     .doc = "Take the medium out of a bay, named as attach_hd/attach_cdrom answer it (bus, id)",
     .method = {.args = machine_eject_media_args, .nargs = 2, .result = V_NONE, .fn = machine_method_eject_media}},
};

static const class_desc_t machine_class = {
    .name = "machine",
    .members = machine_members,
    .n_members = sizeof(machine_members) / sizeof(machine_members[0]),
    .doc = "The emulated computer",
};

// === Lifecycle ============================================================
//
// machine is a process-singleton — registered once at shell_init time and
// never detached.  Attribute getters read from global_emulator so the live
// state is reflected regardless of how many cfg lifetimes have come and
// gone since the object was attached.  Both functions are idempotent.

static struct object *s_machine_object = NULL;

// The single `machine` container node (docs/internals/core/object/object-model.md).
// All emulated hardware nests under it; the emulator's own service objects
// (scheduler/debug/storage/…) and the simulated network (appletalk) stay at
// the root as its siblings. Created lazily on first use because some
// hardware singletons (rom_init, vrom_init) run before machine_init in
// shell_init and attach to it. The node is a process-singleton: per-cfg
// hardware attaches/detaches across machine.boot cycles, but the container
// itself persists for the process lifetime.
struct object *machine_object(void) {
    if (!s_machine_object) {
        s_machine_object = object_new(&machine_class, NULL, "machine");
        if (s_machine_object) {
            object_set_order(s_machine_object, 0); // machine sorts first under the root
            object_set_domain(s_machine_object, OBJ_DOMAIN_MACHINE);
            object_attach(object_root(), s_machine_object);
            // The read-only built-from record rides along for the process
            // lifetime, like the machine container itself.
            machine_config_object_init(s_machine_object);
        }
    }
    return s_machine_object;
}

// === catalog =================================================================
//
// What the emulator can build or fit, as opposed to `machine`, the computer
// that exists now: the model roster and each model's configuration shape,
// the card drivers, and the registries of option-ROM files a card can be
// given.  A process singleton created at shell init; every member answers
// without a machine.

static DEF_GETTER(attr_catalog_nubus_cards) {
    return nubus_cards_list();
}

static DEF_GETTER(attr_catalog_pci_cards) {
    return pci_cards_list();
}

static const member_t catalog_members[] = {
    {.kind = M_ATTR,
     .name = "models",
     .doc = "Every registered model id, in registry order (no machine needed)",
     .attr = {.type = V_LIST, .get = attr_catalog_models, .set = NULL}},
    {.kind = M_METHOD,
     .name = "profile",
     .doc = "A model's full configuration shape (typed map, static)",
     .method = {.args = catalog_profile_args, .nargs = 1, .result = V_MAP, .fn = catalog_method_profile}},
    {.kind = M_ATTR,
     .name = "nubus_cards",
     .doc = "The ids of all registered NuBus card drivers",
     .flags = M_CAT_ADVANCED,
     .attr = {.type = V_LIST, .get = attr_catalog_nubus_cards, .set = NULL}},
    {.kind = M_ATTR,
     .name = "pci_cards",
     .doc = "The ids of all registered PCI card drivers",
     .flags = M_CAT_ADVANCED,
     .attr = {.type = V_LIST, .get = attr_catalog_pci_cards, .set = NULL}},
};

static const class_desc_t catalog_class = {
    .name = "catalog",
    .members = catalog_members,
    .n_members = sizeof(catalog_members) / sizeof(catalog_members[0]),
    .doc = "What the emulator can build or fit: models, card drivers, option ROMs",
};

static struct object *s_catalog_object = NULL;

void catalog_init(void) {
    if (s_catalog_object)
        return;
    s_catalog_object = object_new(&catalog_class, NULL, "catalog");
    if (!s_catalog_object)
        return;
    object_set_label(s_catalog_object, "Catalog");
    object_set_order(s_catalog_object, 70);
    object_attach(object_root(), s_catalog_object);
    vrom_init(s_catalog_object); // catalog.vroms
    prom_init(s_catalog_object); // catalog.proms
}

// Update the machine node's display label to the active model name
// ("Macintosh IIcx"), or clear it back to the bare "machine" segment when no
// machine is booted. The profile name is static for the process lifetime, so
// the borrowed pointer stays valid. Called from the swap step (system_swap_in).
void machine_set_active_label(const char *name) {
    object_set_label(machine_object(), name);
}

void machine_init(void) {
    (void)machine_object();
}

void machine_delete(void) {
    if (s_machine_object) {
        // Never cascade here: the hardware children are per-cfg and owned by
        // their modules (freed in system_destroy). This only drops the
        // persistent container wrapper. In practice machine_delete is never
        // called — the container outlives every cfg.
        object_detach(s_machine_object);
        object_delete(s_machine_object);
        s_machine_object = NULL;
    }
}
