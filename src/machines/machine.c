// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine.c
// Machine profile registry plus the machine.* object-model surface.

#include "machine.h"

#include "cpu.h"
#include "json_encode.h"
#include "log.h"
#include "nubus.h"
#include "object.h"
#include "system.h"
#include "system_config.h"
#include "value.h"
#include "nubus/card.h"

LOG_USE_CATEGORY_NAME("setup");

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Registry of built-in machine profiles.  A static const array iterated
// directly (proposal §4.6): adding a machine is one line here, no runtime
// machine_register(), no MAX_MACHINES cap.  The profiles are defined in each
// family's machine file (glue/se30.c, mdu/iici.c, …).
static const hw_profile_t *const builtin_machines[] = {
    &machine_plus, &machine_se30, &machine_iicx, &machine_iix,   &machine_iifx,
    &machine_iici, &machine_iisi, &machine_lisa, &machine_macxl,
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

// Emit the `capabilities` object into the open profile map.  Every field is
// DERIVED from the hardware facts + mmu_kind so it can never drift from
// behaviour (proposal §4.4): the frontend probes this instead of guessing
// from the model's display name.
static void encode_capabilities(json_builder_t *b, const hw_profile_t *p) {
    json_key(b, "capabilities");
    json_open_obj(b);

    json_key(b, "cpu");
    json_open_obj(b);
    json_key(b, "model");
    json_int(b, (int64_t)p->cpu_model); // 68000 / 68030
    json_key(b, "address_bits");
    json_int(b, (int64_t)p->address_bits);
    json_key(b, "fpu");
    json_bool(b, cpu_has_fpu(p->cpu_model));
    json_close_obj(b);

    json_key(b, "mmu");
    json_open_obj(b);
    // Typed, not a bool: the debug panels must tell a 68030 PMMU (show
    // TC/CRP/SRP/TT0/TT1/MMUSR) from the Lisa segment MMU (don't) from none.
    json_key(b, "present");
    json_bool(b, p->mmu_kind != MMU_NONE);
    json_key(b, "kind");
    json_str(b, mmu_kind_to_string(p->mmu_kind));
    json_close_obj(b);

    // NOTE: video configurability is the video_slots block, NOT "nubus
    // exists" — the two are deliberately not conflated.
    json_key(b, "nubus");
    json_bool(b, p->nubus_slots != NULL);

    json_close_obj(b);
}

// Emit one video card object (id, display_name, requires_vrom, monitors)
// into an open array.  requires_vrom is read straight off the card kind —
// the property the dialog drives its VROM row from (proposal §4.4).
static void encode_video_card(json_builder_t *b, const char *card_id) {
    const nubus_card_kind_t *kind = card_id ? nubus_card_find(card_id) : NULL;
    json_open_obj(b);
    json_key(b, "id");
    json_str(b, card_id ? card_id : "");
    json_key(b, "display_name");
    json_str(b, (kind && kind->display_name) ? kind->display_name : (card_id ? card_id : ""));
    json_key(b, "requires_vrom");
    json_bool(b, kind ? kind->requires_vrom : false);
    json_key(b, "monitors");
    json_open_arr(b);
    if (kind && kind->monitors) {
        for (const nubus_monitor_t *mon = kind->monitors; mon->id; mon++) {
            json_open_obj(b);
            json_key(b, "id");
            json_str(b, mon->id);
            json_key(b, "name");
            json_str(b, mon->name ? mon->name : mon->id);
            json_key(b, "width");
            json_int(b, (int64_t)mon->width);
            json_key(b, "height");
            json_int(b, (int64_t)mon->height);
            json_key(b, "depths");
            json_open_arr(b);
            if (mon->depths) {
                for (const int *d = mon->depths; *d; d++)
                    json_int(b, (int64_t)*d);
            }
            json_close_arr(b);
            json_close_obj(b);
        }
    }
    json_close_arr(b); // monitors
    json_close_obj(b); // card
}

// Emit the `video_slots` block: the real shape the user navigates — slot →
// card → monitor/depth.  VROM-required-ness is per *card* (the SE/30-vs-IIci
// asymmetry), so the dialog shows the VROM row iff the selected card needs
// one.  This is the ONLY video shape in the profile — the flat web-legacy
// `video_modes` compat view was deleted with that UI (proposal §7 stage 3).
static void encode_video_slots(json_builder_t *b, const hw_profile_t *p) {
    json_key(b, "video_slots");
    json_open_arr(b);
    if (!p->nubus_slots) {
        json_close_arr(b);
        return;
    }
    for (const struct nubus_slot_decl *s = p->nubus_slots; s->slot; s++) {
        // Only slots that can carry a video card appear here.  Every SOCKET
        // is emitted (a machine may declare several); the dialog's single
        // picker configures the first one, per-socket UI comes later.
        if (s->kind != NUBUS_SLOT_BUILTIN && s->kind != NUBUS_SLOT_SOCKET)
            continue;
        const char *default_card = (s->kind == NUBUS_SLOT_BUILTIN) ? s->builtin_card_id : s->default_card;

        json_open_obj(b);
        json_key(b, "slot");
        if (s->kind == NUBUS_SLOT_BUILTIN) {
            json_str(b, "builtin");
        } else {
            char slot_buf[8];
            snprintf(slot_buf, sizeof slot_buf, "%X", s->slot); // "9".."E"
            json_str(b, slot_buf);
        }
        json_key(b, "fixed");
        json_bool(b, s->kind == NUBUS_SLOT_BUILTIN);
        json_key(b, "default_card");
        json_str(b, default_card ? default_card : "");

        json_key(b, "cards");
        json_open_arr(b);
        if (s->kind == NUBUS_SLOT_BUILTIN) {
            encode_video_card(b, s->builtin_card_id);
        } else {
            // Candidates are COMPUTED from the card registry: every kind
            // whose declared attachment fits this slot and that drives a
            // display.  Machines never enumerate cards — adding a card to
            // the registry offers it on every compatible machine
            // (proposal-nubus-computed-card-compatibility.md §5.3).
            for (const nubus_card_kind_t *const *k = nubus_card_registry(); *k; k++) {
                if (nubus_card_fits_socket(s, *k) && (*k)->monitors)
                    encode_video_card(b, (*k)->id);
            }
        }
        json_close_arr(b);
        json_close_obj(b);
    }
    json_close_arr(b);
}

// Build the JSON-encoded profile map for a registered hw_profile_t.
static value_t encode_profile(const hw_profile_t *p) {
    json_builder_t *b = json_builder_new();
    if (!b)
        return val_err("machine.profile: out of memory");

    json_open_obj(b);
    json_key(b, "id");
    json_str(b, p->id ? p->id : "");
    json_key(b, "name");
    json_str(b, p->name ? p->name : "");
    json_key(b, "freq");
    json_int(b, (int64_t)p->freq);

    json_key(b, "ram_options");
    json_open_arr(b);
    if (p->ram_options) {
        for (const uint32_t *r = p->ram_options; *r; r++)
            json_int(b, (int64_t)*r);
    }
    json_close_arr(b);

    json_key(b, "ram_default");
    json_int(b, (int64_t)(p->ram_default / 1024u));
    json_key(b, "ram_max");
    json_int(b, (int64_t)(p->ram_max / 1024u));

    json_key(b, "floppy_slots");
    json_open_arr(b);
    if (p->floppy_slots) {
        for (const struct floppy_slot *s = p->floppy_slots; s->label; s++) {
            json_open_obj(b);
            json_key(b, "label");
            json_str(b, s->label);
            json_key(b, "kind");
            json_str(b, floppy_kind_to_string(s->kind));
            json_close_obj(b);
        }
    }
    json_close_arr(b);

    json_key(b, "scsi_slots");
    json_open_arr(b);
    if (p->scsi_slots) {
        for (const struct scsi_slot *s = p->scsi_slots; s->label; s++) {
            json_open_obj(b);
            json_key(b, "label");
            json_str(b, s->label);
            json_key(b, "id");
            json_int(b, (int64_t)s->id);
            json_close_obj(b);
        }
    }
    json_close_arr(b);

    json_key(b, "hd_bus");
    json_str(b, hd_bus_to_string(p->hd_bus));

    json_key(b, "has_cdrom");
    json_bool(b, p->has_cdrom);
    json_key(b, "cdrom_id");
    json_int(b, (int64_t)p->cdrom_id);

    // Derived capability probe + per-card video-slot shape (proposal §4.4) —
    // the source of truth the frontend consumes.  (The web-legacy compat keys
    // needs_vrom / video_modes / video_mode_default were deleted with that UI;
    // everything derives from video_slots now.)
    encode_capabilities(b, p);
    encode_video_slots(b, p);

    json_close_obj(b);
    return json_finish(b);
}

// machine.profile(id) — static lookup, returns the model's full configuration
// shape as a JSON-encoded map (see proposal §3.2.2).  Errors when id is empty
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
    return encode_profile(p);
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

// machine.boot(model, ram) — atomic machine creation.  Tears down any
// existing machine and creates a fresh one with no ROM loaded.  Caller must
// follow up with rom.load(path) to actually run anything.
//
// Both arguments are required: ram must be one of the model's ram_options.
static value_t machine_method_boot(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    const char *model_id = argv[0].s;
    const hw_profile_t *profile = machine_find(model_id);
    if (!profile)
        return val_err("machine.boot: unknown model '%s'", model_id);

    // framework guarantees V_UINT
    uint32_t ram_kb = (uint32_t)argv[1].u;
    if (!ram_option_allowed(profile, ram_kb)) {
        char options[128];
        format_ram_options(options, sizeof(options), profile);
        return val_err("machine.boot: ram %u KB not in profile.ram_options for %s [%s]", ram_kb, profile->name,
                       options);
    }
    system_set_pending_ram_kb(ram_kb);

    if (global_emulator) {
        system_destroy(global_emulator);
        global_emulator = NULL;
    }
    config_t *cfg = system_create(profile, NULL);
    if (!cfg)
        return val_err("machine.boot: failed to create %s", model_id);
    LOG(1, "Machine created: %s (%s), RAM: %u KB", profile->name, profile->id, cfg->ram_size / 1024u);
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

static const arg_decl_t machine_boot_args[] = {
    {.name = "model", .kind = V_STRING, .validation_flags = OBJ_ARG_NONEMPTY, .doc = "Machine model id (plus / se30)"},
    {.name = "ram", .kind = V_UINT, .doc = "RAM in KB; must be one of profile.ram_options"},
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
     .doc = "Look up a registered model's full configuration map (JSON-encoded)",
     .method = {.args = machine_profile_args, .nargs = 1, .result = V_STRING, .fn = machine_method_profile}},
    {.kind = M_METHOD,
     .name = "boot",
     .doc = "Tear down any active machine and create a new one (no ROM loaded yet)",
     .method = {.args = machine_boot_args, .nargs = 2, .result = V_BOOL, .fn = machine_method_boot}},
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
