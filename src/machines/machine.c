// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine.c
// Machine profile registry plus the machine.* object-model surface.

#include "machine.h"
#include "machine_config.h"

#include "adb.h"
#include "checkpoint.h"
#include "cpu.h"
#include "gs_out.h"
#include "image.h"
#include "json_value.h"
#include "log.h"
#include "nubus.h"
#include "object.h"
#include "platform.h"
#include "prom.h"
#include "rom.h"
#include "scheduler.h"
#include "scsi.h"
#include "system.h"
#include "system_internal.h"
#include "value.h"
#include "vrom.h"
#include "nubus/card.h"
#include "pci/pci.h"

LOG_USE_CATEGORY_NAME("system");

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// The built-in machine profiles, each defined in its family's machine file
// (compact/plus.c, glue/{se30,iicx,iix}.c, mdu/{iici,iisi}.c, oss/iifx.c,
// mcu/, av/, pdm/, tnt/, gossamer/, lisa/lisa.c).  Declared here, not in
// machine.h: the registry below is their only reader, so no other file has
// to see every machine.
extern const hw_profile_t machine_plus;
extern const hw_profile_t machine_se30;
extern const hw_profile_t machine_iicx;
extern const hw_profile_t machine_iix;
extern const hw_profile_t machine_iifx;
extern const hw_profile_t machine_iici;
extern const hw_profile_t machine_lisa;
extern const hw_profile_t machine_macxl;
extern const hw_profile_t machine_iisi;
extern const hw_profile_t machine_q700;
extern const hw_profile_t machine_q900;
extern const hw_profile_t machine_q950;
extern const hw_profile_t machine_q840av;
extern const hw_profile_t machine_q660av;
extern const hw_profile_t machine_pm6100;
extern const hw_profile_t machine_pm7100;
extern const hw_profile_t machine_pm8100;
extern const hw_profile_t machine_pm7500;
extern const hw_profile_t machine_pm8500;
extern const hw_profile_t machine_pm9500;
extern const hw_profile_t machine_ans500;
extern const hw_profile_t machine_ans700;
extern const hw_profile_t machine_pmg3dt;
extern const hw_profile_t machine_pmg3mt;

// Registry of built-in machine profiles.  A static const array iterated
// directly: adding a machine is its extern above plus one entry here (this
// file only), no runtime machine_register(), no MAX_MACHINES cap.
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

// === Media bays ==============================================
// "The Nth default hard disk" and "the default CD-ROM drive" -- what
// machine.attach_hd / attach_cdrom and the headless hd= / cdrom= arguments
// mean -- are the profile's default storage devices, in order.  Derived here
// once so no caller branches on a bus.

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

// The default storage devices of `type`, as attach bays, in order.
static int default_bays(const hw_profile_t *p, unsigned type, media_bay_t *out, int max) {
    int n = 0;
    for (const storage_device_decl_t *d = p ? p->default_storage : NULL; d && d->bus && n < max; d++) {
        if (d->type != type)
            continue;
        if (machine_storage_media_bay(p, d->bus, d->unit, &out[n]))
            n++;
    }
    return n;
}

int profile_hd_bays(const hw_profile_t *p, media_bay_t *out, int max) {
    return default_bays(p, STORAGE_DEV_HD, out, max);
}

bool profile_default_hd_bay(const hw_profile_t *p, media_bay_t *out) {
    return profile_hd_bays(p, out, 1) == 1;
}

bool profile_cdrom_bay(const hw_profile_t *p, media_bay_t *out) {
    return default_bays(p, STORAGE_DEV_CD, out, 1) == 1;
}

int profile_floppy_count(const hw_profile_t *p) {
    int n = 0;
    for (const floppy_slot_t *s = p ? p->floppy_slots : NULL; s && s->label; s++)
        n++;
    return n;
}

int machine_floppy_count(const struct config *cfg) {
    return cfg->build_opts.n_floppies >= 0 ? cfg->build_opts.n_floppies : profile_floppy_count(cfg->machine);
}

struct scsi *machine_scsi_bus_init(struct config *cfg, checkpoint_t *cp, const char *bus_id) {
    scsi_t *bus = scsi_init_named(cfg->scheduler, cp, CONFIG_IMAGES(cfg), bus_id);
    if (!bus || cp)
        return bus; // a restored bus brings its drives in its own block
    // Power-on: the configuration's CD-ROM drives on this bus, empty.  SCSI is
    // not hot-plug -- the guest's CD driver claims its targets at the boot-time
    // bus scan -- so a drive must exist from power-on, disc or no disc.
    const struct scsi_cd_drive *drive = cfg->machine->cdrom_drive;
    for (int i = 0; drive && i < cfg->build_opts.n_storage; i++) {
        const machine_storage_dev_t *d = &cfg->build_opts.storage[i];
        if (d->type == STORAGE_DEV_CD && strcmp(d->bus, bus_id) == 0)
            scsi_add_cd_drive(bus, d->unit, drive);
    }
    return bus;
}

// A bay as the attach verbs answer it: {bus, id, label}.
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

// Pulse the machine's vertical-blanking line (system.h): the scheduler's
// per-frame tick, dispatched to the substrate.  Here rather than in system.c
// so the scheduler need not know hw_profile_t and system.c carries no
// per-machine dispatch wrappers.
void trigger_vbl(struct config *restrict config) {
    if (config && config->machine && config->machine->substrate->trigger_vbl)
        config->machine->substrate->trigger_vbl(config);
}

// === Object-model class descriptor =========================================
//
// machine is a process-singleton namespace: registered once at shell_init
// (machine_init below) and never torn down.  Per-instance attribute getters
// read from `global_emulator` rather than `object_data(self)` so the live
// machine state is reflected regardless of when the object was attached
// and how many cfg lifetimes have come and gone since.  Pre-boot reads
// return V_ERROR — no soft fallbacks; callers gate on `machine.created`.

// Resolve the active machine's config or return V_ERROR for the named
// attribute.  Every machine.* getter that reads the running machine goes
// through this (or active_profile_or_error), so the "no machine" answer is
// worded once.
static config_t *active_cfg_or_error(const char *attr_name, value_t *out_err) {
    config_t *cfg = global_emulator;
    if (!cfg || !cfg->machine) {
        *out_err = val_err("machine.%s: no machine booted; check machine.created first", attr_name);
        return NULL;
    }
    return cfg;
}

// Resolve the active profile or return V_ERROR for the named attribute.
static const hw_profile_t *active_profile_or_error(const char *attr_name, value_t *out_err) {
    config_t *cfg = active_cfg_or_error(attr_name, out_err);
    return cfg ? cfg->machine : NULL;
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

// The storage devices the running machine was built with, the positions an
// image can be attached to: {bus, bus_label, unit, position, type, present},
// type hd or cd as machine.attach_media takes them, present when an image is
// in it.
static DEF_GETTER(attr_machine_storage) {
    value_t err;
    config_t *cfg = active_cfg_or_error("storage", &err);
    if (!cfg)
        return err;
    size_t n = (size_t)cfg->n_storage;
    value_t *items = n ? (value_t *)calloc(n, sizeof(value_t)) : NULL;
    if (n && !items)
        return val_err("machine.storage: out of memory");
    for (size_t i = 0; i < n; i++) {
        const machine_storage_dev_t *d = &cfg->storage[i];
        const storage_bus_decl_t *b = machine_storage_bus(cfg->machine, d->bus);
        char position[64] = "";
        machine_storage_position(cfg->machine, d->bus, d->unit, position, sizeof position);
        value_map_builder_t *m = val_map_new();
        val_map_put(m, "bus", val_str(d->bus));
        val_map_put(m, "bus_label", val_str(b && b->label ? b->label : d->bus));
        val_map_put(m, "unit", val_int(d->unit));
        val_map_put(m, "position", val_str(position));
        val_map_put(m, "type", val_str(d->type == STORAGE_DEV_CD ? "cd" : "hd"));
        media_bay_t bay;
        const machine_substrate_t *sub = cfg->machine->substrate;
        bool present = machine_storage_media_bay(cfg->machine, d->bus, d->unit, &bay) && sub->media_present &&
                       sub->media_present(cfg, bay.bus, bay.unit);
        val_map_put(m, "present", val_bool(present));
        items[i] = val_map_finish(m);
    }
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
    value_t err;
    config_t *cfg = active_cfg_or_error("ram", &err);
    if (!cfg)
        return err;
    return val_uint(4, cfg->ram_size / 1024u); // exact: ram_size is built as ram_kb * 1024
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
    // Which expansion buses the machine has (what each slot takes is the
    // tree's `slots` and `cards`).
    val_map_put(b, "nubus", val_bool(p->nubus_slots != NULL));
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

// Build the typed profile map for a registered hw_profile_t: its identity,
// the capability probe, and the machine-description tree (machine_config.c).
static value_t build_profile(const hw_profile_t *p) {
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "id", val_str(p->id ? p->id : ""));
    val_map_put(b, "name", val_str(p->name ? p->name : ""));
    val_map_put(b, "freq", val_int((int64_t)p->freq));
    val_map_put(b, "capabilities", build_capabilities(p));
    machine_config_put_tree(p, b);
    return val_map_finish(b);
}

// catalog.profile(id) — the model's machine-description tree as a typed map.
// Card availability follows the ROMs offered now, so a reader re-reads it
// after an upload.  Errors when id is empty or names no registered profile.
static DEF_METHOD(catalog_method_profile) {
    const char *id = argv[0].s; // non-empty: the argument is OBJ_ARG_NONEMPTY
    const hw_profile_t *p = machine_find(id);
    if (!p)
        return val_err("catalog.profile: unknown model '%s'", id);
    return build_profile(p);
}

// catalog.default_config(id) — the model's default configuration, as the
// document machine.boot's config= takes.
static DEF_METHOD(catalog_method_default_config) {
    const char *id = argv[0].s;
    const hw_profile_t *p = (id && *id) ? machine_find(id) : NULL;
    if (!p)
        return val_err("catalog.default_config: unknown model '%s'", id ? id : "");
    return machine_config_defaults(p);
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

// The configuration document (config=, JSON) parsed, with its model and rom
// reconciled against the named arguments.  *out is V_NONE without one.
static value_t boot_read_config(boot_config_t *doc, value_t *out) {
    *out = val_none();
    if (!doc->config || !*doc->config)
        return val_none();
    char why[160];
    value_t v;
    if (!json_value_parse(doc->config, &v, why, sizeof why))
        return val_err("machine.boot: config: %s", why);
    if (v.kind != V_MAP) {
        value_free(&v);
        return val_err("machine.boot: config must be a JSON object");
    }
    static const char *const keys[] = {"model",   "rom",   "options",  "floppies", "storage",
                                       "startup", "cards", "displays", NULL};
    for (size_t i = 0; i < v.map.len; i++) {
        bool ok = false;
        for (const char *const *k = keys; *k; k++)
            ok |= strcmp(*k, v.map.entries[i].key) == 0;
        if (!ok) {
            value_t err = val_err("machine.boot: config: unknown member '%s'", v.map.entries[i].key);
            value_free(&v);
            return err;
        }
    }
    // model and rom may come from either place, but must agree.
    static const char *const ids[] = {"model", "rom"};
    const char **fields[] = {&doc->model, &doc->rom};
    for (int i = 0; i < 2; i++) {
        const value_t *m = value_map_get(&v, ids[i]);
        if (!m)
            continue;
        if (m->kind != V_STRING) {
            value_free(&v);
            return val_err("machine.boot: config: %s must be a string", ids[i]);
        }
        if (*fields[i] && **fields[i] && strcmp(*fields[i], m->s) != 0) {
            value_t err = val_err("machine.boot: %s= and config's %s disagree", ids[i], ids[i]);
            value_free(&v);
            return err;
        }
        *fields[i] = m->s; // borrowed from v, which outlives the boot
    }
    *out = v;
    return val_none();
}

// Apply one boot document: validate, build the new machine beside the
// running one, swap it in, destroy the old one (system_swap_in).  Shared by
// machine.boot and headless startup.  Returns V_NONE on success, V_ERROR (with
// the old machine still running) on rejection.
value_t machine_boot_apply(const boot_config_t *doc_in) {
    boot_config_t doc = *doc_in;
    value_t config;
    value_t cerr = boot_read_config(&doc, &config);
    if (val_is_error(&cerr))
        return cerr;
    value_t result = val_none();

    // 1. Validation — all of it before anything is built.  The document is
    // the whole specification: nothing is filled in from the previous
    // machine (a field the caller did not write must not arrive from
    // somewhere the caller cannot see).
    const hw_profile_t *profile = NULL;
    if (!doc.model || !*doc.model) {
        result = val_err("machine.boot: model is required (machine.restart power-cycles the running machine)");
        goto out;
    }
    profile = machine_find(doc.model);
    if (!profile) {
        result = val_err("machine.boot: unknown model '%s'", doc.model);
        goto out;
    }
    if (!doc.rom || !*doc.rom) {
        result = val_err("machine.boot: rom is required (machine.restart power-cycles the running machine)");
        goto out;
    }
    // video_sense= is a debug override: 0..7 is the passive sense code, 8..14
    // Apple's indexed numbering for the monitors that answer the extended
    // probe (only the DAFB models them).
    if (doc.video_sense < -1 || doc.video_sense >= 15) {
        result = val_err("machine.boot: video_sense must be 0..14, or -1 for unset (got %d)", doc.video_sense);
        goto out;
    }
    // The card ROMs beside this ROM join the offer registry before the slot
    // check reads it.  Offers are content-addressed and persist, so this only
    // adds; a ROM booted from another directory than the platform's startup
    // one used to find none of its siblings (#187).
    platform_offer_sibling_card_roms(doc.rom);

    // Everything but the ROM: memory, options, floppies, storage, the startup
    // device, the cards and the displays, from the document and the named
    // arguments it was given as (machine_config.c).
    machine_build_opts_t build_opts = machine_build_opts_default();
    result = machine_config_resolve(profile, config.kind == V_MAP ? &config : NULL, &doc, &build_opts);
    if (val_is_error(&result))
        goto out;

    // The ROM, read now: the machine is built with it, so a ROM that cannot
    // be read, is unknown, belongs to another model or has the wrong size is
    // a rejected boot, never a machine without a ROM.
    uint8_t *rom_bytes = NULL;
    rom_image_t build_rom;
    rom_identity_t rom_identity;
    result = boot_rom_read(&doc, profile, &rom_bytes, &build_rom, &rom_identity);
    if (val_is_error(&result))
        goto out;
    build_opts.rom = build_rom;

    // 3. Build, then swap, then destroy.  The new machine is built from the
    // document alone while the running one is untouched; only a complete
    // build replaces it.
    config_t *cfg = system_create(profile, &build_opts, NULL);
    free(rom_bytes); // copied into the ROM region
    if (!cfg) {
        result = val_err("machine.boot: failed to create %s", profile->id);
        goto out;
    }

    // 4. The swap: the new machine becomes the active one, and the one it
    // replaces is destroyed.
    system_swap_in(cfg, false, platform_pacing());
    // ram_size is ram_kb * 1024 (system_create), so the division is exact.
    LOG(1, "Machine booted: %s (%s), RAM: %u KB", profile->name, profile->id, cfg->ram_size / 1024u);
out:
    value_free(&config);
    return result;
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
    // ram= is a V_UINT; a value past 32 bits would truncate to some other
    // size (2^32 + 4096 KB reading as 4 MB), so refuse it rather than cast.
    uint64_t ram_kb = boot_uint(&argv[1], 0);
    if (ram_kb > UINT32_MAX)
        return val_err("machine.boot: ram %llu KB is out of range", (unsigned long long)ram_kb);
    boot_config_t doc = {
        .model = boot_str(&argv[0]),
        .ram_kb = (uint32_t)ram_kb,
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
        .config = boot_str(&argv[14]),
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
// all survive because nothing destroyed them.
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
     .doc = "Debug override of the connected display's monitor sense: 0..7 (passive), 8..14 (extended, DAFB)",
     .default_doc = "the connected monitor's"},
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
    {.name = "config",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "The configuration document as JSON (see catalog.default_config); the arguments above are "
            "shorthand for parts of it", .default_doc = "the model's default configuration"},
};

static const arg_decl_t machine_register_args[] = {
    {.name = "id",      .kind = V_STRING, .doc = "Machine identity (UUID-like)"},
    {.name = "created", .kind = V_STRING, .doc = "Creation timestamp"          },
};

static const arg_decl_t catalog_profile_args[] = {
    {.name = "id", .kind = V_STRING, .validation_flags = OBJ_ARG_NONEMPTY, .doc = "Machine model id (plus / se30)"},
};

// === machine.attach_hd / attach_cdrom / attach_media / eject_media =====
// Media by position, not by bus.  attach_hd and attach_cdrom are shorthand
// for "the Nth hard disk / the CD-ROM drive of the model's default
// configuration" (profile_hd_bays / profile_cdrom_bay); attach_media names a
// position as the configuration does, a storage bus id and a unit.  The
// substrate attaches there, on whatever bus that is -- machine.scsi,
// machine.scsi2, the ATA buses or the Lisa's ProFile.  Each answers the place
// it used, {bus, id, label}, which is what eject_media takes back.

static DEF_METHOD(machine_method_attach_hd) {
    config_t *cfg = global_emulator;
    if (!cfg || !cfg->machine)
        return val_err("machine.attach_hd: no machine is running");
    media_bay_t bays[MEDIA_HD_BAYS_MAX];
    int n = profile_hd_bays(cfg->machine, bays, MEDIA_HD_BAYS_MAX);
    int64_t which = (argc >= 2 && argv[1].kind == V_INT) ? argv[1].i : 0;
    if (n == 0)
        return val_err("machine.attach_hd: %s has no hard disk in its default configuration", cfg->machine->name);
    if (which < 0 || which >= n)
        return val_err("machine.attach_hd: hard disk %lld does not exist (%s has %d; machine.attach_media names "
                       "any position)",
                       (long long)which, cfg->machine->name, n);
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
        return val_err("machine.attach_cdrom: %s has no CD-ROM drive in its default configuration", cfg->machine->name);
    char err[256];
    if (system_media_attach_path(cfg, &bay, true, argv[0].s, err, sizeof(err)) != 0)
        return val_err("machine.attach_cdrom: %s", err);
    return media_bay_value(&bay);
}

// machine.attach_media(bus, unit, type, path) -- an image into the device at
// a storage position, named as the configuration names it: the bus's id in
// catalog.profile's storage tree, the unit on it, hd or cd.  The one attach
// a frontend needs, whatever bus the position is on.
static DEF_METHOD(machine_method_attach_media) {
    config_t *cfg = global_emulator;
    if (!cfg || !cfg->machine)
        return val_err("machine.attach_media: no machine is running");
    const char *bus_id = argv[0].s, *type = argv[2].s;
    const storage_bus_decl_t *b = machine_storage_bus(cfg->machine, bus_id);
    if (!b)
        return val_err("machine.attach_media: %s has no storage bus '%s'", cfg->machine->name, bus_id);
    int64_t unit = argv[1].i;
    if (unit < 0 || unit > 31 || !(b->units & (1u << unit)) || (b->reserved & (1u << unit)))
        return val_err("machine.attach_media: \"%s\" has no unit %lld", b->label, (long long)unit);
    bool cd = strcmp(type, "cd") == 0;
    if (!cd && strcmp(type, "hd") != 0)
        return val_err("machine.attach_media: type must be hd or cd");
    if (!(b->accepts & (cd ? STORAGE_DEV_CD : STORAGE_DEV_HD)))
        return val_err("machine.attach_media: \"%s\" takes no %s", b->label, cd ? "CD-ROM drive" : "hard disk");
    media_bay_t bay;
    machine_storage_media_bay(cfg->machine, bus_id, (int)unit, &bay);
    char err[256];
    if (system_media_attach_path(cfg, &bay, cd, argv[3].s, err, sizeof(err)) != 0)
        return val_err("machine.attach_media: %s", err);
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
     .doc = "Hard-disk image path"                                                   },
    {.name = "bay",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_bay0,
     .doc = "Which of the default configuration's hard disks (0 is the startup disk)"},
};

static const arg_decl_t machine_attach_cdrom_args[] = {
    {.name = "path",
     .kind = V_STRING,
     .presentation_flags = VAL_PATH,
     .validation_flags = OBJ_ARG_NONEMPTY,
     .doc = "CD-ROM image path"},
};

static const arg_decl_t machine_attach_media_args[] = {
    {.name = "bus",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_NONEMPTY,
     .doc = "Storage bus id (catalog.profile storage)"},
    {.name = "unit", .kind = V_INT, .doc = "Unit on that bus (SCSI ID; ATA 0 master, 1 slave)"},
    {.name = "type", .kind = V_STRING, .validation_flags = OBJ_ARG_NONEMPTY, .doc = "hd or cd"},
    {.name = "path",
     .kind = V_STRING,
     .presentation_flags = VAL_PATH,
     .validation_flags = OBJ_ARG_NONEMPTY,
     .doc = "Image path"},
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
     .name = "storage",
     .doc = "The storage devices the machine was built with: {bus, bus_label, unit, position, type, present}",
     .attr = {.type = V_LIST, .get = attr_machine_storage, .set = NULL}},
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
     .doc = "Attach a hard-disk image to the Nth hard disk of the model's default configuration",
     .method = {.result_doc = "{bus, id, label}: the bay, as eject_media names it",
                .args = machine_attach_hd_args,
                .nargs = 2,
                .result = V_MAP,
                .fn = machine_method_attach_hd}},
    {.kind = M_METHOD,
     .name = "attach_cdrom",
     .examples = EXAMPLES("machine.attach_cdrom \"images/install.iso\""),
     .doc = "Insert a CD-ROM image into the default configuration's CD-ROM drive",
     .method = {.result_doc = "{bus, id, label}: the bay, as eject_media names it",
                .args = machine_attach_cdrom_args,
                .nargs = 1,
                .result = V_MAP,
                .fn = machine_method_attach_cdrom}},
    {.kind = M_METHOD,
     .name = "attach_media",
     .examples = EXAMPLES("machine.attach_media scsi 4 hd \"images/data.img\""),
     .doc = "Attach an image to the device at a storage position (bus, unit), as the configuration names it",
     .method = {.result_doc = "{bus, id, label}: the medium's place, as eject_media names it",
                .args = machine_attach_media_args,
                .nargs = 4,
                .result = V_MAP,
                .fn = machine_method_attach_media}},
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
    {.kind = M_METHOD,
     .name = "default_config",
     .doc = "A model's default configuration, as the document machine.boot's config= takes",
     .method = {.args = catalog_profile_args, .nargs = 1, .result = V_MAP, .fn = catalog_method_default_config}},
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
