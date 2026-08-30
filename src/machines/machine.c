// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine.c
// Machine profile registry plus the machine.* object-model surface.

#include "machine.h"

#include "adb.h"
#include "cpu.h"
#include "image.h"
#include "log.h"
#include "machine_config.h"
#include "nubus.h"
#include "object.h"
#include "prom.h"
#include "rom.h"
#include "scheduler.h"
#include "system.h"
#include "system_config.h"
#include "value.h"
#include "vrom.h"
#include "nubus/card.h"
#include "nubus/cards/jmfb.h"
#include "pci/pci.h"

#include "mcu/dafb.h"
#include "pdm/pdm.h" // the built-in monitor strap (profile.builtin_video)

LOG_USE_CATEGORY_NAME("setup");

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Registry of built-in machine profiles.  A static const array iterated
// directly (proposal §4.6): adding a machine is one line here, no runtime
// machine_register(), no MAX_MACHINES cap.  The profiles are defined in each
// family's machine file (glue/se30.c, mdu/iici.c, …).
static const hw_profile_t *const builtin_machines[] = {
    &machine_plus,   &machine_se30,   &machine_iicx,   &machine_iix,    &machine_iifx,   &machine_iici,
    &machine_iisi,   &machine_q700,   &machine_q900,   &machine_q950,   &machine_q840av, &machine_q660av,
    &machine_pm6100, &machine_pm7100, &machine_pm8100, &machine_pm7500, &machine_pm8500, &machine_pm9500,
    &machine_ans500, &machine_ans700, &machine_lisa,   &machine_macxl,
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

// Convert an mmu_kind_t to its wire string ("none" / "68030_pmmu" /
// "lisa_segment").  This is the value the capability probe exports as
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

static value_t attr_machine_id(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    value_t err;
    const hw_profile_t *p = active_profile_or_error("id", &err);
    if (!p)
        return err;
    return val_str(p->id ? p->id : "");
}

static value_t attr_machine_name(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    value_t err;
    const hw_profile_t *p = active_profile_or_error("name", &err);
    if (!p)
        return err;
    return val_str(p->name ? p->name : "");
}

static value_t attr_machine_freq(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    value_t err;
    const hw_profile_t *p = active_profile_or_error("freq", &err);
    if (!p)
        return err;
    return val_uint(4, p->freq);
}

static value_t attr_machine_ram(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    config_t *cfg = global_emulator;
    if (!cfg || !cfg->machine)
        return val_err("machine.ram: no machine booted; check machine.created first");
    return val_uint(4, cfg->ram_size / 1024u);
}

static value_t attr_machine_created(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    config_t *cfg = global_emulator;
    return val_bool(cfg && cfg->machine != NULL);
}

// Build the `capabilities` map of the profile.  Every field is DERIVED
// from the hardware facts + mmu_kind so it can never drift from
// behaviour (proposal §4.4): the frontend probes this instead of guessing
// from the model's display name.
static value_t build_capabilities(const hw_profile_t *p) {
    value_map_builder_t *cpu = val_map_new();
    val_map_put(cpu, "model", val_int((int64_t)p->cpu_model)); // 68000 / 68030
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
// dialog drives its VROM row from (proposal §4.4).
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
// `video_modes` compat view was deleted with that UI (proposal §7 stage 3).
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
            // the registry offers it on every compatible machine
            // (proposal-nubus-computed-card-compatibility.md §5.3).
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
                if (pci_card_fits_socket(s, *k))
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
    val_map_put(b, "display_name", val_str(p->builtin_video));
    value_t *mons = NULL;
    size_t n = 0, cap = 0;
    for (const pdm_monitor_kind_t *m = pdm_monitors; m->id; m++) {
        value_map_builder_t *mb = val_map_new();
        val_map_put(mb, "id", val_str(m->id));
        val_map_put(mb, "name", val_str(m->name));
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

    value_t *scsis = NULL;
    size_t n_scsis = 0, cap_scsis = 0;
    if (p->scsi_slots) {
        for (const struct scsi_slot *s = p->scsi_slots; s->label; s++) {
            value_map_builder_t *sb = val_map_new();
            val_map_put(sb, "label", val_str(s->label));
            val_map_put(sb, "id", val_int((int64_t)s->id));
            val_map_put(sb, "boot", val_bool(s->boot));
            val_list_push(&scsis, &n_scsis, &cap_scsis, val_map_finish(sb));
        }
    }
    val_map_put(b, "scsi_slots", val_list(scsis, n_scsis));

    val_map_put(b, "hd_bus", val_str(hd_bus_to_string(p->hd_bus)));

    val_map_put(b, "has_cdrom", val_bool(p->has_cdrom));
    val_map_put(b, "cdrom_id", val_int((int64_t)p->cdrom_id));

    // Derived capability probe + per-card video-slot shape (proposal §4.4) —
    // the source of truth the frontend consumes.  (The web-legacy compat keys
    // needs_vrom / video_modes / video_mode_default were deleted with that UI;
    // everything derives from video_slots now.)
    val_map_put(b, "capabilities", build_capabilities(p));
    val_map_put(b, "video_slots", build_video_slots(p));
    // PCI expansion topology: one row per declared socket / builtin, with
    // the fitting cards computed per socket (proposal-pci-architecture §8.1).
    val_map_put(b, "pci_slots", build_pci_slots(p));
    // Substrate built-in video, when the machine has one that is NOT a
    // BUILTIN slot pseudo-card (the PDM family's Ariel scanout).  The
    // configuration dialog offers it beside the NuBus cards; picking a card
    // instead means strapping this port unconnected (monitor="none"), which
    // is what a real machine does when you plug the monitor into the card.
    val_map_put(b, "builtin_video", build_builtin_video(p));

    return val_map_finish(b);
}

// machine.profile(id) — static lookup, returns the model's full configuration
// shape as a typed map (see proposal §3.2.2).  Errors when id is empty
// or doesn't match a registered profile.
static value_t machine_method_profile(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    const char *id = argv[0].s;
    if (!id || !*id)
        return val_err("machine.profile: id must be non-empty");
    const hw_profile_t *p = machine_find(id);
    if (!p)
        return val_err("machine.profile: unknown model '%s'", id);
    return build_profile(p);
}

// True if `kb` is one of the values in profile->ram_options.
static bool ram_option_allowed(const hw_profile_t *p, uint32_t kb) {
    if (!p->ram_options)
        return false;
    for (const uint32_t *r = p->ram_options; *r; r++) {
        if (*r == kb)
            return true;
    }
    return false;
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

// === Boot document (proposal-named-args-boot-config §4, revised by
// proposal-boot-vs-reset §3.1) ==============================================
//
// machine.boot consumes one atomic, COMPLETE configuration document: model
// and rom are required, every other field falls back to the model's own
// defaults — never to another machine's record (a field the caller did not
// write must not arrive from somewhere the caller cannot see).  All
// validation runs BEFORE the old machine is torn down, so a rejected boot
// leaves the running machine untouched.  machine.restart is the verb for
// "power-cycle this machine".

// Strict declaration-ROM resolution (§4.1): every catalogued card the
// user EXPLICITLY picked (per-slot staged entry, or the document's
// wildcard card for the first socket) must resolve from the offer
// registry, or the boot is rejected before teardown.  Factory-default
// socket population and BUILTIN cards are exempt: an unsatisfiable
// default degrades to an empty slot with a log (as before), and a
// soldered-down card owns its own fallback policy (the SE/30
// synthesises its onboard vROM when none was offered).
static value_t validate_vrom_resolution(const hw_profile_t *profile, const char *wildcard_card) {
    if (!profile->nubus_slots)
        return val_none();
    bool first_socket = true;
    for (const nubus_slot_decl_t *d = profile->nubus_slots; d->slot; d++) {
        if (d->kind != NUBUS_SLOT_SOCKET)
            continue;
        // Explicit picks only, mirroring nubus_init's precedence: a
        // per-slot staged entry beats the wildcard, and the wildcard is
        // honoured only on the machine's FIRST socket.
        const char *card_id = nubus_staged_card_get(d->slot);
        if ((!card_id || !*card_id) && first_socket && wildcard_card && *wildcard_card)
            card_id = wildcard_card;
        first_socket = false;
        if (!card_id || !*card_id)
            continue;
        if (vrom_card_catalogued(card_id) && !vrom_card_resolvable(card_id)) {
            return val_err("machine.boot: card '%s' (slot $%X) needs a declaration ROM but no offered "
                           "vROM file provides it",
                           card_id, d->slot);
        }
    }
    return val_none();
}

// The same strict-resolution rule for PCI sockets: a card the USER named
// that needs a real FCode expansion ROM must have one resolvable before
// the running machine is touched.  Mirrors validate_vrom_resolution, down
// to the asymmetry — only EXPLICIT picks fail the boot, because a slot
// resolving its own default degrades to an empty slot with a log instead.
static value_t validate_prom_resolution(const hw_profile_t *profile, const char *wildcard_card) {
    if (!profile->pci_slots)
        return val_none();
    bool first_socket = true;
    for (const pci_slot_decl_t *d = profile->pci_slots; d->slot; d++) {
        if (d->kind != PCI_SLOT_SOCKET)
            continue;
        const char *card_id = pci_staged_card_get(d->slot);
        if ((!card_id || !*card_id) && first_socket && wildcard_card && *wildcard_card)
            card_id = wildcard_card;
        first_socket = false;
        if (!card_id || !*card_id)
            continue;
        const pci_card_kind_t *kind = pci_card_find(card_id);
        if (!kind || !kind->requires_prom)
            continue;
        if (!prom_card_resolvable(card_id)) {
            return val_err("machine.boot: card '%s' (PCI slot %d) needs a PCI expansion ROM but no offered "
                           ".prom file provides it",
                           card_id, d->slot);
        }
    }
    return val_none();
}

// Split pci_option="key=value[,key=value]" onto the wildcard socket — the
// same slot pci_card= applies to.  Malformed pairs are dropped with a log
// rather than failing the boot: which keys are meaningful is the card's
// business (its stage_option hook), so this layer cannot tell a typo from
// a key it simply does not know, and refusing the boot would make every
// unknown option fatal.
static void stage_pci_options(const char *spec) {
    if (!spec || !*spec)
        return;
    char buf[MC_PATH_MAX];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *save = NULL, *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ')
            tok++;
        char *eq = strchr(tok, '=');
        if (!eq || eq == tok || !eq[1]) {
            LOG(0, "machine.boot: pci_option '%s' is not key=value — ignored", tok);
            continue;
        }
        *eq = '\0';
        pci_staged_option_set(PCI_STAGED_WILDCARD, tok, eq + 1);
    }
}

// Armed by machine.restart for the duration of its machine_boot_apply call:
// carry the mounted media's open image handles across the teardown
// (proposal-boot-vs-reset §3.3).  A plain machine.boot never transfers —
// a new machine starts with empty drives.
static bool s_transfer_media = false;

// Is machine_boot_apply rebuilding the same machine (machine.restart) rather
// than building a new one?  Machine-specific teardown asks this before
// carrying non-volatile hardware state across the rebuild (machine_config.h).
bool machine_boot_is_restart(void) {
    return s_transfer_media;
}

// Stamp the record's `created` field with the current UTC time (ISO8601).
static void stamp_created(char *buf, size_t bufsize) {
    time_t now = time(NULL);
    struct tm tm_utc;
    if (gmtime_r(&now, &tm_utc))
        strftime(buf, bufsize, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    else
        snprintf(buf, bufsize, "unknown");
}

// Apply one boot document: validate → tear down → construct → record.
// Shared by machine.boot, machine.restart and headless startup.  Returns
// V_NONE on success, V_ERROR (with the old machine still running) on
// rejection.
value_t machine_boot_apply(const boot_config_t *doc_in) {
    boot_config_t doc = *doc_in;

    // 1. Validation — all of it before system_destroy.  The document is the
    // whole specification: nothing is filled in from the previous machine's
    // record (§2 — a field the caller did not write must not arrive from
    // somewhere the caller cannot see).
    if (!doc.model || !*doc.model)
        return val_err("machine.boot: model is required (machine.restart power-cycles the running machine)");
    const hw_profile_t *profile = machine_find(doc.model);
    if (!profile)
        return val_err("machine.boot: unknown model '%s'", doc.model);

    uint32_t ram_kb = doc.ram_kb;
    if (ram_kb == 0)
        ram_kb = profile->ram_default / 1024u;
    if (!ram_option_allowed(profile, ram_kb)) {
        char options[128];
        format_ram_options(options, sizeof(options), profile);
        return val_err("machine.boot: ram %u KB not in profile.ram_options for %s [%s]", ram_kb, profile->name,
                       options);
    }

    if (!doc.rom || !*doc.rom)
        return val_err("machine.boot: rom is required (machine.restart power-cycles the running machine)");
    rom_file_info_t rom_fi = {0};
    if (rom_probe_file(doc.rom, &rom_fi) != 0)
        return val_err("machine.boot: cannot read rom '%s'", doc.rom);
    if (doc.rom2 && *doc.rom2) {
        // Two-chip Lisa/XL form: the chips identify only after interleaving,
        // so per-file identification is skipped here; the loader validates.
        FILE *f = fopen(doc.rom2, "rb");
        if (!f)
            return val_err("machine.boot: cannot read rom2 '%s'", doc.rom2);
        fclose(f);
    } else if (rom_fi.info) {
        bool ok = false;
        for (const char *const *p = rom_fi.info->compatible; *p; p++) {
            if (strcmp(*p, profile->id) == 0) {
                ok = true;
                break;
            }
        }
        if (!ok)
            return val_err("machine.boot: rom '%s' (%s) is not compatible with model '%s'", doc.rom,
                           rom_fi.info->family_name, profile->id);
    } else {
        return val_err("machine.boot: rom '%s' is not a recognised ROM image (checksum %08X)", doc.rom,
                       rom_fi.checksum);
    }

    if (doc.video_card && *doc.video_card) {
        if (!profile->nubus_slots)
            return val_err("machine.boot: model '%s' has no NuBus slots for video_card '%s'", profile->id,
                           doc.video_card);
        if (!nubus_card_find(doc.video_card)) {
            const char *near = nubus_card_suggest(doc.video_card);
            if (near)
                return val_err("machine.boot: unknown card id '%s' — did you mean '%s'? (see nubus.cards())",
                               doc.video_card, near);
            return val_err("machine.boot: unknown card id '%s' (see nubus.cards())", doc.video_card);
        }
    }
    if (doc.pci_card && *doc.pci_card) {
        if (!profile->pci_slots)
            return val_err("machine.boot: model '%s' has no PCI slots for pci_card '%s'", profile->id, doc.pci_card);
        if (!pci_card_find(doc.pci_card)) {
            const char *near = pci_card_suggest(doc.pci_card);
            if (near)
                return val_err("machine.boot: unknown card id '%s' — did you mean '%s'? (see machine.pci.cards())",
                               doc.pci_card, near);
            return val_err("machine.boot: unknown card id '%s' (see machine.pci.cards())", doc.pci_card);
        }
    }
    // 0..7 is the passive sense code; 8..14 is Apple's own indexed numbering
    // for the monitors that answer the EXTENDED (tie-matrix) probe instead
    // (dafb.h's DAFB_SENSE_INDEXED_*).  Only the DAFB models the extended
    // range today, so the JMFB is staged from the passive part only.
    if (doc.video_sense >= (int)DAFB_SENSE_INDEXED_MAX)
        return val_err("machine.boot: video_sense must be 0..%u (got %d)", DAFB_SENSE_INDEXED_MAX - 1u,
                       doc.video_sense);
    if (doc.video_mode && *doc.video_mode && !nubus_video_mode_known(doc.video_mode))
        return val_err("machine.boot: unknown video-mode id '%s'", doc.video_mode);
    if (doc.monitor && *doc.monitor) {
        if (!profile->builtin_video)
            return val_err("machine.boot: model '%s' has no configurable built-in video port", profile->id);
        if (!pdm_monitor_lookup(doc.monitor))
            return val_err("machine.boot: unknown monitor id '%s' (see machine.profile)", doc.monitor);
    }
    if (doc.custom_mode && *doc.custom_mode) {
        const char *why = NULL;
        if (!nubus_custom_mode_parse(doc.custom_mode, NULL, NULL, NULL, &why))
            return val_err("machine.boot: custom_mode '%s' invalid: %s", doc.custom_mode, why);
    }

    // Explicit vROM pick: the file must identify as a known declaration ROM
    // before it can win the pick order.
    if (doc.vrom && *doc.vrom) {
        vrom_id_t vid;
        if (!vrom_identify_card(doc.vrom, &vid))
            return val_err("machine.boot: vrom '%s' is not a recognised declaration ROM", doc.vrom);
    }
    // ...and the same for an explicit PCI expansion-ROM pick.
    if (doc.prom && *doc.prom) {
        prom_id_t pid;
        if (!prom_identify_card(doc.prom, &pid))
            return val_err("machine.boot: prom '%s' is not a recognised PCI expansion ROM "
                           "(see prom.identify for what it is instead)",
                           doc.prom);
    }

    // Strict resolution for explicitly picked socket cards (per-slot staged
    // entries and the document's wildcard card).
    if (doc.vrom && *doc.vrom)
        vrom_set_path(doc.vrom);
    if (doc.prom && *doc.prom)
        prom_set_path(doc.prom);
    value_t verr = validate_vrom_resolution(profile, doc.video_card);
    if (val_is_error(&verr))
        return verr;
    value_t perr = validate_prom_resolution(profile, doc.pci_card);
    if (val_is_error(&perr))
        return perr;

    // 3. Teardown + atomic construction.  On a machine.restart the mounted
    // media's open handles are detached first so they survive
    // system_destroy's close loop (§3.3) — the delta stays with its open
    // instance, so writes survive the power-cycle by construction.
    media_slot_t media[MEDIA_SLOTS_MAX];
    int n_media = 0;
    bool caps_latched = false;
    // Pacing is a property of the HOST harness, not of the emulated machine,
    // so it survives the rebuild: without this the daemon's --speed= setting
    // (and any scheduler.mode a script set) is silently discarded by the
    // first machine.boot, and scheduler.mode then reads back 'paced' on a
    // daemon launched --speed=max.
    enum schedule_mode pacing = schedule_paced;
    bool pacing_known = false;
    if (global_emulator && global_emulator->scheduler) {
        pacing = scheduler_get_mode(global_emulator->scheduler);
        pacing_known = true;
    }
    if (global_emulator) {
        if (s_transfer_media && global_emulator->machine->substrate->media_detach)
            n_media = global_emulator->machine->substrate->media_detach(global_emulator, media, MEDIA_SLOTS_MAX);
        // Caps Lock is a mechanically locking switch: like the mounted
        // media, its state belongs to the hardware that outlives the
        // power-cycle, not to the machine being torn down.  (Booting
        // Copland depends on it: the latch has to be down across the
        // restart into the diverted boot.)
        if (s_transfer_media)
            caps_latched = adb_capslock_latched(global_emulator->adb);
        system_destroy(global_emulator);
        global_emulator = NULL;
    }

    // Seed the construction channels from the document. Only fields the
    // document carries are written — a per-slot staged card entry
    // (slot[N].card_id, the surviving multi-card surface) is left alone.
    system_set_pending_ram_kb(ram_kb);
    if (doc.video_card && *doc.video_card)
        nubus_staged_card_set(NUBUS_STAGED_WILDCARD, doc.video_card);
    if (doc.pci_card && *doc.pci_card)
        pci_staged_card_set(PCI_STAGED_WILDCARD, doc.pci_card);
    stage_pci_options(doc.pci_option);
    if (doc.video_mode && *doc.video_mode)
        nubus_staged_mode_set(NUBUS_STAGED_WILDCARD, doc.video_mode);
    if (doc.custom_mode && *doc.custom_mode)
        nubus_staged_custom_mode_set(NUBUS_STAGED_WILDCARD, doc.custom_mode);
    if (doc.video_sense >= 0) {
        if (doc.video_sense <= 7)
            jmfb_pending_sense_set((uint8_t)doc.video_sense);
        dafb_pending_sense_set((uint8_t)doc.video_sense); // built-in Quadra video
    }
    // The built-in monitor strap: validated above, so the lookup succeeds.
    if (doc.monitor && *doc.monitor)
        pdm_pending_monitor_set(pdm_monitor_lookup(doc.monitor)->sense);

    machine_config_reset_vroms();
    machine_config_reset_slot_cards();
    config_t *cfg = system_create(profile, NULL);
    if (!cfg) {
        for (int i = 0; i < n_media; ++i)
            image_close(media[i].img); // machine gone; nothing to attach to
        return val_err("machine.boot: failed to create %s", profile->id);
    }

    int rom_rc;
    if (doc.rom2 && *doc.rom2)
        rom_rc = rom_load_lisa_into_machine(doc.rom, doc.rom2);
    else
        rom_rc = rom_load_into_machine(doc.rom);
    if (rom_rc != 0) {
        for (int i = 0; i < n_media; ++i)
            image_close(media[i].img); // half-built machine; drop the transfer
        return val_err("machine.boot: machine created but ROM staging failed for '%s'", doc.rom);
    }

    // The carried pacing (see above): re-assert it on the machine's own
    // fresh scheduler, which was constructed in the default paced mode.
    if (pacing_known && cfg->scheduler)
        scheduler_set_mode(cfg->scheduler, pacing);

    // Hand the transferred media handles back through the device attach
    // paths (§3.3).  The rebuilt machine is the same model by construction
    // (the restart document IS the record), so every slot re-resolves.
    for (int i = 0; i < n_media; ++i) {
        if (cfg->machine->substrate->media_attach && cfg->machine->substrate->media_attach(cfg, &media[i]) == 0)
            continue;
        LOG(1, "machine.restart: could not re-attach medium '%s'; closing it",
            image_get_filename(media[i].img) ? image_get_filename(media[i].img) : "(unnamed)");
        image_close(media[i].img);
    }

    // The carried Caps Lock latch (see above): re-latch it before the
    // machine runs, so the ROM's ADB init finds the key already down.
    if (caps_latched)
        adb_capslock_latch(cfg->adb);

    // 4. The built-from record — the machine's birth certificate.
    machine_config_record_t *w = machine_config_record_mut();
    snprintf(w->model, sizeof(w->model), "%s", profile->id);
    w->ram_kb = cfg->ram_size / 1024u;
    snprintf(w->rom, sizeof(w->rom), "%s", doc.rom);
    w->rom_crc = rom_fi.checksum;
    snprintf(w->rom2, sizeof(w->rom2), "%s", doc.rom2 ? doc.rom2 : "");
    snprintf(w->vrom, sizeof(w->vrom), "%s", doc.vrom ? doc.vrom : "");
    snprintf(w->prom, sizeof(w->prom), "%s", doc.prom ? doc.prom : "");
    snprintf(w->video_card, sizeof(w->video_card), "%s", doc.video_card ? doc.video_card : "");
    w->video_sense = doc.video_sense;
    snprintf(w->video_mode, sizeof(w->video_mode), "%s", doc.video_mode ? doc.video_mode : "");
    snprintf(w->custom_mode, sizeof(w->custom_mode), "%s", doc.custom_mode ? doc.custom_mode : "");
    snprintf(w->monitor, sizeof(w->monitor), "%s", doc.monitor ? doc.monitor : "");
    snprintf(w->pci_card, sizeof(w->pci_card), "%s", doc.pci_card ? doc.pci_card : "");
    snprintf(w->pci_option, sizeof(w->pci_option), "%s", doc.pci_option ? doc.pci_option : "");
    stamp_created(w->created, sizeof(w->created));
    w->valid = true;

    LOG(1, "Machine created: %s (%s), RAM: %u KB", profile->name, profile->id, cfg->ram_size / 1024u);
    return val_none();
}

// machine.boot — atomic, self-contained configuration document.  model and
// rom are required; every other field falls back to the model's own
// defaults.  Empty-string / 0 defaults are the "not given" sentinels (an
// explicitly empty value is rejected by the named-argument grammar).  Use
// machine.restart to power-cycle the running machine.
static value_t machine_method_boot(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    boot_config_t doc = {
        .model = argv[0].s,
        .ram_kb = (uint32_t)argv[1].u,
        .rom = argv[2].s,
        .vrom = argv[3].s,
        .video_card = argv[4].s,
        .video_sense = (argv[5].u == 0xFF) ? -1 : (int)argv[5].u,
        .video_mode = argv[6].s,
        .rom2 = argv[7].s,
        .custom_mode = argv[8].s,
        .monitor = argv[9].s,
        .pci_card = argv[10].s,
        .prom = argv[11].s,
        .pci_option = argv[12].s,
    };
    value_t err = machine_boot_apply(&doc);
    if (val_is_error(&err))
        return err;
    value_free(&err);
    return val_bool(true);
}

// machine.restart — power-cycle the running machine (proposal-boot-vs-reset
// §3.2): rebuild the machine described by the built-from record, taking no
// configuration arguments, and keep the mounted media attached by
// transferring the open image handles across the teardown (§3.3).  Errors
// when no machine is running.  Host-side runtime state that is not
// construction configuration (volume, host capture sources) is out of scope
// — the frontend re-asserts it.  Scheduler pacing is the exception every
// rebuild keeps: it is the harness's setting, not the machine's.
static value_t machine_method_restart(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    const machine_config_record_t *rec = machine_config_record();
    if (!global_emulator || !rec->valid)
        return val_err("machine.restart: no machine is running; boot one first");

    // Work from a snapshot: boot_apply rewrites the record in place, so doc
    // pointers into the live record would alias their own destination — and
    // the original `created` stamp must survive (a power-cycle is not a
    // re-birth, §6.3).
    machine_config_record_t snap = *rec;
    boot_config_t doc = {
        .model = snap.model,
        .ram_kb = snap.ram_kb,
        .rom = snap.rom,
        .rom2 = snap.rom2[0] ? snap.rom2 : NULL,
        .vrom = snap.vrom[0] ? snap.vrom : NULL,
        .prom = snap.prom[0] ? snap.prom : NULL,
        .video_card = snap.video_card[0] ? snap.video_card : NULL,
        .video_sense = snap.video_sense,
        .video_mode = snap.video_mode[0] ? snap.video_mode : NULL,
        .custom_mode = snap.custom_mode[0] ? snap.custom_mode : NULL,
        .pci_card = snap.pci_card[0] ? snap.pci_card : NULL,
        .pci_option = snap.pci_option[0] ? snap.pci_option : NULL,
    };
    // Replay the user's per-slot picks: the document's wildcard covers only
    // the first socket, so without these a multi-card machine would come
    // back with empty slots (proposal-pci-architecture §8.2).
    //
    // ONLY the explicit ones.  A slot that resolved its own default must be
    // left to resolve it again: re-staging a default turns it into an
    // explicit pick, and an explicit pick whose declaration ROM cannot be
    // resolved FAILS the boot where a default degrades to an empty slot
    // with a log.  (That asymmetry is deliberate — see
    // validate_vrom_resolution — and replaying defaults made
    // machine.restart reject itself on any machine with no vROM offered.)
    for (int i = 0; i < snap.n_slot_cards; i++) {
        const machine_config_slot_card_t *e = &snap.slot_cards[i];
        if (!e->explicit_pick)
            continue;
        if (e->bus_kind == MC_BUS_PCI)
            pci_staged_card_set(e->slot, e->card_id);
        else
            nubus_staged_card_set(e->slot, e->card_id);
    }
    s_transfer_media = true;
    value_t err = machine_boot_apply(&doc);
    s_transfer_media = false;
    if (val_is_error(&err))
        return err;
    value_free(&err);

    machine_config_record_t *w = machine_config_record_mut();
    snprintf(w->created, sizeof(w->created), "%s", snap.created);
    return val_bool(true);
}

// machine.register(id, created) — record the active machine identity for
// checkpointing. Routes to the platform's gs_register_machine.
static value_t machine_method_register(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    return val_bool(gs_register_machine(argv[0].s, argv[1].s) == 0);
}

// "Not given" sentinels for the boot document's optional fields: empty
// string / 0 / 0xFF mean "use the model's default" (§3.1); model and rom
// are checked as required inside machine_boot_apply so the message can
// point at machine.restart.  An explicitly empty named value (`rom=`) is
// rejected by the shell grammar before binding.
static const value_t k_unset_str = {.kind = V_STRING, .s = (char *)""};
static const value_t k_unset_u32 = {.kind = V_UINT, .u = 0};
static const value_t k_unset_sense = {.kind = V_UINT, .u = 0xFF};

static const arg_decl_t machine_boot_args[] = {
    {.name = "model",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_unset_str,
     .doc = "Machine model id (plus / se30 / ...); required"                        },
    {.name = "ram",
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_unset_u32,
     .doc = "RAM in KB (one of profile.ram_options); default: model default"        },
    {.name = "rom",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_unset_str,
     .doc = "ROM file path; required"                                               },
    {.name = "vrom",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_unset_str,
     .doc = "Explicit declaration-ROM pick; default: auto-resolve from offers"      },
    {.name = "video_card",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_unset_str,
     .doc = "Card id for the first NuBus socket; default: slot default"             },
    {.name = "video_sense",
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_unset_sense,
     .doc = "Monitor sense 0..7; default: card default"                             },
    {.name = "video_mode",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_unset_str,
     .doc = "Video-mode id (see machine.profile); default: card default"            },
    {.name = "rom2",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_unset_str,
     .doc = "Lisa/XL second ROM chip (two-chip form); default: single-file rom"     },
    {.name = "custom_mode",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_unset_str,
     .doc = "Custom resolution WxHxD (generic 8_24 kind); default: none"            },
    {.name = "monitor",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_unset_str,
     .doc = "Monitor on the built-in port ('none' = unconnected, which hands "
            "the screen to a NuBus card); default: model default"                   },
    {.name = "pci_card",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_unset_str,
     .doc = "Card id for the first PCI socket; default: slot default"               },
    {.name = "prom",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_unset_str,
     .doc = "Explicit PCI expansion-ROM pick; default: auto-resolve from offers"    },
    {.name = "pci_option",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_unset_str,
     .doc = "Options for the PCI card, \"key=value[,key=value]\" (e.g. \"vram=4m\")"},
};

static const arg_decl_t machine_register_args[] = {
    {.name = "id",      .kind = V_STRING, .doc = "Machine identity (UUID-like)"},
    {.name = "created", .kind = V_STRING, .doc = "Creation timestamp"          },
};

static const arg_decl_t machine_profile_args[] = {
    {.name = "id", .kind = V_STRING, .validation_flags = OBJ_ARG_NONEMPTY, .doc = "Machine model id (plus / se30)"},
};

static const member_t machine_members[] = {
    {.kind = M_ATTR,
     .name = "id",
     .doc = "Active machine's model id (\"plus\" / \"se30\" / …)",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = attr_machine_id, .set = NULL}},
    {.kind = M_ATTR,
     .name = "name",
     .doc = "Active machine's human-readable name",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = attr_machine_name, .set = NULL}},
    {.kind = M_ATTR,
     .name = "freq",
     .doc = "Active machine's CPU clock in Hz",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = attr_machine_freq, .set = NULL}},
    {.kind = M_ATTR,
     .name = "ram",
     .doc = "Active RAM size in KB",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = attr_machine_ram, .set = NULL}},
    {.kind = M_ATTR,
     .name = "created",
     .doc = "True if a machine has been booted",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = attr_machine_created, .set = NULL}},
    {.kind = M_METHOD,
     .name = "profile",
     .doc = "Look up a registered model's full configuration map",
     .method = {.args = machine_profile_args, .nargs = 1, .result = V_MAP, .fn = machine_method_profile}},
    {.kind = M_METHOD,
     .name = "boot",
     .doc = "Boot a machine from a complete configuration document (model and rom required; other fields default "
            "per model)", .method = {.args = machine_boot_args,
                .nargs = sizeof(machine_boot_args) / sizeof(machine_boot_args[0]),
                .result = V_BOOL,
                .fn = machine_method_boot}},
    {.kind = M_METHOD,
     .name = "restart",
     .doc = "Power-cycle the running machine: rebuild it from machine.config, keeping mounted media attached",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = machine_method_restart}},
    {.kind = M_METHOD,
     .name = "register",
     .doc = "Record the active machine identity for checkpointing",
     .method = {.args = machine_register_args, .nargs = 2, .result = V_BOOL, .fn = machine_method_register}},
};

const class_desc_t machine_class = {
    .name = "machine",
    .members = machine_members,
    .n_members = sizeof(machine_members) / sizeof(machine_members[0]),
};

// === Lifecycle ============================================================
//
// machine is a process-singleton — registered once at shell_init time and
// never detached.  Attribute getters read from global_emulator so the live
// state is reflected regardless of how many cfg lifetimes have come and
// gone since the object was attached.  Both functions are idempotent.

static struct object *s_machine_object = NULL;

// The single `machine` container node (proposal-system-object-model.md §5.1).
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
            object_set_order(s_machine_object, -100); // machine sorts first under the root
            object_attach(object_root(), s_machine_object);
            // The read-only built-from record rides along for the process
            // lifetime, like the machine container itself.
            machine_config_object_init(s_machine_object);
        }
    }
    return s_machine_object;
}

// Update the machine node's display label to the active model name
// ("Macintosh IIcx"), or clear it back to the bare "machine" segment when no
// machine is booted. The profile name is static for the process lifetime, so
// the borrowed pointer stays valid. Called from system_create / system_destroy.
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
