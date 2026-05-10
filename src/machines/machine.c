// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine.c
// Machine profile registry plus the machine.* object-model surface.

#include "machine.h"

#include "json_encode.h"
#include "nubus.h"
#include "object.h"
#include "system.h"
#include "system_config.h"
#include "value.h"
#include "nubus/card.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MACHINES 8

// Registry of known machine profiles
static const hw_profile_t *g_machines[MAX_MACHINES];
static int g_machine_count = 0;

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

// Validate that a profile has every declarative field populated.  Profiles
// missing any of these are rejected from the registry — see proposal §3.1's
// "no partial profiles" rule.
static bool profile_is_complete(const hw_profile_t *p) {
    if (!p)
        return false;
    if (!p->id || !*p->id)
        return false;
    if (!p->name || !*p->name)
        return false;
    if (p->freq == 0)
        return false;
    if (p->ram_default == 0 || p->ram_max == 0)
        return false;
    if (!p->ram_options)
        return false;
    if (!p->floppy_slots)
        return false;
    if (!p->scsi_slots)
        return false;
    if (!p->init)
        return false;
    return true;
}

// Register a machine profile with the registry.  Rejects partial profiles.
void machine_register(const hw_profile_t *profile) {
    if (!profile_is_complete(profile)) {
        printf("machine_register: rejected partial profile '%s'\n", (profile && profile->id) ? profile->id : "(null)");
        return;
    }
    if (g_machine_count >= MAX_MACHINES)
        return;
    g_machines[g_machine_count++] = profile;
}

// Find a machine profile by its id string
const hw_profile_t *machine_find(const char *id) {
    if (!id)
        return NULL;
    for (int i = 0; i < g_machine_count; ++i) {
        if (g_machines[i] && strcmp(g_machines[i]->id, id) == 0)
            return g_machines[i];
    }
    return NULL;
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

    json_key(b, "has_cdrom");
    json_bool(b, p->has_cdrom);
    json_key(b, "cdrom_id");
    json_int(b, (int64_t)p->cdrom_id);
    json_key(b, "needs_vrom");
    json_bool(b, p->needs_vrom);

    // video_modes: flat per-(monitor, depth) catalog aggregated across
    // every populated NuBus slot whose card-kind has a non-NULL
    // monitor list.  Each entry carries the data the configuration
    // dialog needs (id, human label, dimensions, depth in bpp); the
    // bytes the JMFB factory needs (sense_code, srsrc_sister) stay
    // C-side and are looked up at machine.boot time when the dialog
    // sets `machine.video_mode = "<id>"`.  Empty array for machines
    // without configurable video.
    json_key(b, "video_modes");
    json_open_arr(b);
    if (p->nubus_slots) {
        for (const struct nubus_slot_decl *s = p->nubus_slots; s->slot; s++) {
            // Slot card-kind ids to query for monitors:
            //   BUILTIN: builtin_card_id (single)
            //   VIDEO:   default_card    (the monitor list is per-card-kind, not
            //                             per-instance, so default_card is enough
            //                             to enumerate the catalog the user can pick)
            const char *card_id = NULL;
            if (s->kind == NUBUS_SLOT_BUILTIN)
                card_id = s->builtin_card_id;
            else if (s->kind == NUBUS_SLOT_VIDEO)
                card_id = s->default_card;
            if (!card_id)
                continue;
            const nubus_card_kind_t *kind = nubus_card_find(card_id);
            if (!kind || !kind->monitors)
                continue;
            for (const nubus_monitor_t *mon = kind->monitors; mon->id; mon++) {
                if (!mon->depths)
                    continue;
                for (const int *d = mon->depths; *d; d++) {
                    char id_buf[64];
                    char label_buf[128];
                    snprintf(id_buf, sizeof id_buf, "%s_%dbpp", mon->id, *d);
                    snprintf(label_buf, sizeof label_buf, "%s · %u\xc3\x97%u · %d bpp", mon->name, mon->width,
                             mon->height, *d);
                    json_open_obj(b);
                    json_key(b, "id");
                    json_str(b, id_buf);
                    json_key(b, "label");
                    json_str(b, label_buf);
                    json_key(b, "width");
                    json_int(b, (int64_t)mon->width);
                    json_key(b, "height");
                    json_int(b, (int64_t)mon->height);
                    json_key(b, "depth_bpp");
                    json_int(b, (int64_t)*d);
                    json_close_obj(b);
                }
            }
        }
    }
    json_close_arr(b);

    // Default mode id matches the first (monitor, depth) tuple — i.e.
    // the card's first monitor at its lowest supported depth.  For the
    // JMFB this is "12in_rgb_1bpp"; the dialog can override.  If no
    // video_modes were emitted (non-video machine) the field is "".
    json_key(b, "video_mode_default");
    {
        const char *first_id = NULL;
        char first_buf[64];
        if (p->nubus_slots) {
            for (const struct nubus_slot_decl *s = p->nubus_slots; s->slot && !first_id; s++) {
                const char *card_id = (s->kind == NUBUS_SLOT_BUILTIN) ? s->builtin_card_id
                                      : (s->kind == NUBUS_SLOT_VIDEO) ? s->default_card
                                                                      : NULL;
                if (!card_id)
                    continue;
                const nubus_card_kind_t *kind = nubus_card_find(card_id);
                if (!kind || !kind->monitors)
                    continue;
                for (const nubus_monitor_t *mon = kind->monitors; mon->id && !first_id; mon++) {
                    if (!mon->depths || !*mon->depths)
                        continue;
                    snprintf(first_buf, sizeof first_buf, "%s_%dbpp", mon->id, *mon->depths);
                    first_id = first_buf;
                }
            }
        }
        json_str(b, first_id ? first_id : "");
    }

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
    printf("Machine created: %s (%s), RAM: %u KB\n", profile->name, profile->id, cfg->ram_size / 1024u);
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

void machine_init(void) {
    if (s_machine_object)
        return;
    s_machine_object = object_new(&machine_class, NULL, "machine");
    if (s_machine_object)
        object_attach(object_root(), s_machine_object);
}

void machine_delete(void) {
    if (s_machine_object) {
        object_detach(s_machine_object);
        object_delete(s_machine_object);
        s_machine_object = NULL;
    }
}
