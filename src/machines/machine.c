// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine.c
// Machine profile registry plus the machine.* object-model surface.

#include "machine.h"

#include "cpu.h"
#include "json_encode.h"
#include "log.h"
#include "machine_config.h"
#include "nubus.h"
#include "object.h"
#include "rom.h"
#include "system.h"
#include "system_config.h"
#include "value.h"
#include "vrom.h"
#include "nubus/card.h"
#include "nubus/cards/jmfb.h"

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

// === Boot document (proposal-named-args-boot-config §4) =====================
//
// machine.boot consumes one atomic configuration document: every argument
// is optional and inherits from the live machine's built-from record; all
// validation runs BEFORE the old machine is torn down, so a rejected boot
// leaves the running machine untouched.

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

// Stamp the record's `created` field with the current UTC time (ISO8601).
static void stamp_created(char *buf, size_t bufsize) {
    time_t now = time(NULL);
    struct tm tm_utc;
    if (gmtime_r(&now, &tm_utc))
        strftime(buf, bufsize, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    else
        snprintf(buf, bufsize, "unknown");
}

// Apply one boot document: inherit → validate → tear down → construct →
// record.  Shared by machine.boot and headless startup.  Returns V_NONE on
// success, V_ERROR (with the old machine still running) on rejection.
value_t machine_boot_apply(const boot_config_t *doc_in) {
    const machine_config_record_t *rec = machine_config_record();
    boot_config_t doc = *doc_in;

    // Inherited strings are copied out of the record: the record is
    // rewritten during construction (rom.load write-back, step 4), so a
    // borrowed pointer into it would alias its own destination.
    char model_buf[MC_ID_MAX], card_buf[MC_ID_MAX], mode_buf[MC_ID_MAX];
    char rom_buf[MC_PATH_MAX], rom2_buf[MC_PATH_MAX], vrom_buf[MC_PATH_MAX];

    // 1. Inheritance: every field not given falls back to the record.
    if (!doc.model || !*doc.model) {
        snprintf(model_buf, sizeof(model_buf), "%s", rec->valid ? rec->model : "");
        doc.model = *model_buf ? model_buf : NULL;
    }
    if (!doc.rom || !*doc.rom) {
        snprintf(rom_buf, sizeof(rom_buf), "%s", rec->valid ? rec->rom : "");
        doc.rom = *rom_buf ? rom_buf : NULL;
    }
    if (!doc.rom2 || !*doc.rom2) {
        snprintf(rom2_buf, sizeof(rom2_buf), "%s", rec->valid ? rec->rom2 : "");
        doc.rom2 = *rom2_buf ? rom2_buf : NULL;
    }
    if (!doc.vrom || !*doc.vrom) {
        snprintf(vrom_buf, sizeof(vrom_buf), "%s", rec->valid ? rec->vrom : "");
        doc.vrom = *vrom_buf ? vrom_buf : NULL;
    }
    if (!doc.video_card || !*doc.video_card) {
        snprintf(card_buf, sizeof(card_buf), "%s", rec->valid ? rec->video_card : "");
        doc.video_card = *card_buf ? card_buf : NULL;
    }
    if (doc.video_sense < 0)
        doc.video_sense = rec->valid ? rec->video_sense : -1;
    if (!doc.video_mode || !*doc.video_mode) {
        snprintf(mode_buf, sizeof(mode_buf), "%s", rec->valid ? rec->video_mode : "");
        doc.video_mode = *mode_buf ? mode_buf : NULL;
    }

    // 2. Validation — all of it before system_destroy.
    if (!doc.model || !*doc.model)
        return val_err("machine.boot: no model given and no machine to inherit from");
    const hw_profile_t *profile = machine_find(doc.model);
    if (!profile)
        return val_err("machine.boot: unknown model '%s'", doc.model);

    uint32_t ram_kb = doc.ram_kb;
    if (ram_kb == 0)
        ram_kb = (rec->valid && strcmp(rec->model, profile->id) == 0) ? rec->ram_kb : profile->ram_default / 1024u;
    if (!ram_option_allowed(profile, ram_kb)) {
        char options[128];
        format_ram_options(options, sizeof(options), profile);
        return val_err("machine.boot: ram %u KB not in profile.ram_options for %s [%s]", ram_kb, profile->name,
                       options);
    }

    if (!doc.rom || !*doc.rom)
        return val_err("machine.boot: no rom given and no machine to inherit from");
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
        if (!nubus_card_find(doc.video_card))
            return val_err("machine.boot: unknown card id '%s' (see nubus.cards())", doc.video_card);
    }
    if (doc.video_sense > 7)
        return val_err("machine.boot: video_sense must be 0..7 (got %d)", doc.video_sense);
    if (doc.video_mode && *doc.video_mode && !nubus_video_mode_known(doc.video_mode))
        return val_err("machine.boot: unknown video-mode id '%s'", doc.video_mode);

    // Explicit vROM pick: the file must identify as a known declaration ROM
    // before it can win the pick order.
    if (doc.vrom && *doc.vrom) {
        vrom_id_t vid;
        if (!vrom_identify_card(doc.vrom, &vid))
            return val_err("machine.boot: vrom '%s' is not a recognised declaration ROM", doc.vrom);
    }

    // Strict resolution for explicitly picked socket cards (per-slot staged
    // entries and the document's wildcard card).
    if (doc.vrom && *doc.vrom)
        vrom_set_path(doc.vrom);
    value_t verr = validate_vrom_resolution(profile, doc.video_card);
    if (val_is_error(&verr))
        return verr;

    // 3. Teardown + atomic construction.
    if (global_emulator) {
        system_destroy(global_emulator);
        global_emulator = NULL;
    }

    // Seed the construction channels from the document. Only fields the
    // document carries are written — a per-slot staged card entry
    // (slot[N].card_id, the surviving multi-card surface) is left alone.
    system_set_pending_ram_kb(ram_kb);
    if (doc.video_card && *doc.video_card)
        nubus_staged_card_set(NUBUS_STAGED_WILDCARD, doc.video_card);
    if (doc.video_mode && *doc.video_mode)
        nubus_staged_mode_set(NUBUS_STAGED_WILDCARD, doc.video_mode);
    if (doc.video_sense >= 0)
        jmfb_pending_sense_set((uint8_t)doc.video_sense);

    machine_config_reset_vroms();
    config_t *cfg = system_create(profile, NULL);
    if (!cfg)
        return val_err("machine.boot: failed to create %s", profile->id);

    int rom_rc;
    if (doc.rom2 && *doc.rom2)
        rom_rc = rom_load_lisa_into_machine(doc.rom, doc.rom2);
    else
        rom_rc = rom_load_into_machine(doc.rom);
    if (rom_rc != 0)
        return val_err("machine.boot: machine created but ROM staging failed for '%s'", doc.rom);

    // 4. The built-from record — the machine's birth certificate.
    machine_config_record_t *w = machine_config_record_mut();
    snprintf(w->model, sizeof(w->model), "%s", profile->id);
    w->ram_kb = cfg->ram_size / 1024u;
    snprintf(w->rom, sizeof(w->rom), "%s", doc.rom);
    w->rom_crc = rom_fi.checksum;
    snprintf(w->rom2, sizeof(w->rom2), "%s", doc.rom2 ? doc.rom2 : "");
    snprintf(w->vrom, sizeof(w->vrom), "%s", doc.vrom ? doc.vrom : "");
    snprintf(w->video_card, sizeof(w->video_card), "%s", doc.video_card ? doc.video_card : "");
    w->video_sense = doc.video_sense;
    snprintf(w->video_mode, sizeof(w->video_mode), "%s", doc.video_mode ? doc.video_mode : "");
    stamp_created(w->created, sizeof(w->created));
    w->valid = true;

    LOG(1, "Machine created: %s (%s), RAM: %u KB", profile->name, profile->id, cfg->ram_size / 1024u);
    return val_none();
}

// machine.boot — atomic, self-contained configuration document.  All
// arguments optional; unspecified ones inherit from machine.config (the
// built-from record), so a bare `machine.boot` is a plain reboot with the
// ROM re-staged.  Empty-string / 0 defaults are the "not given" sentinels
// (an explicitly empty value is rejected by the named-argument grammar).
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
    };
    value_t err = machine_boot_apply(&doc);
    if (val_is_error(&err))
        return err;
    value_free(&err);
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

// "Not given" sentinels for the all-optional boot document: empty string /
// 0 / 0xFF mean "inherit from machine.config" (§4.1).  An explicitly empty
// named value (`rom=`) is rejected by the shell grammar before binding.
static const value_t k_unset_str = {.kind = V_STRING, .s = (char *)""};
static const value_t k_unset_u32 = {.kind = V_UINT, .u = 0};
static const value_t k_unset_sense = {.kind = V_UINT, .u = 0xFF};

static const arg_decl_t machine_boot_args[] = {
    {.name = "model",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_unset_str,
     .doc = "Machine model id (plus / se30 / ...); default: inherit"              },
    {.name = "ram",
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_unset_u32,
     .doc = "RAM in KB (one of profile.ram_options); default: inherit"            },
    {.name = "rom",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_unset_str,
     .doc = "ROM file path; default: inherit (re-stages the same ROM)"            },
    {.name = "vrom",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_unset_str,
     .doc = "Explicit declaration-ROM pick; default: auto-resolve from offers"    },
    {.name = "video_card",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_unset_str,
     .doc = "Card id for the first NuBus socket; default: inherit / slot default" },
    {.name = "video_sense",
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_unset_sense,
     .doc = "Monitor sense 0..7; default: inherit / card default"                 },
    {.name = "video_mode",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_unset_str,
     .doc = "Video-mode id (see machine.profile); default: inherit / card default"},
    {.name = "rom2",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_unset_str,
     .doc = "Lisa/XL second ROM chip (two-chip form); default: single-file rom"   },
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
     .doc = "Boot a machine from a configuration document; omitted arguments inherit from machine.config",
     .method = {.args = machine_boot_args, .nargs = 8, .result = V_BOOL, .fn = machine_method_boot}},
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
