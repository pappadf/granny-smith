// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// nubus.c
// NuBus subsystem: the card-kind registry, the bus controller, and
// slot-IRQ routing.

#include "nubus.h"
#include "card.h"
#include "checkpoint.h"
#include "log.h"
#include "machine_parts.h"
#include "machine_profile.h" // machine_substrate_t (slot-IRQ routing)
#include "system_config.h"
#include "vrom.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("nubus");

#define NUBUS_MAX_SLOTS 16 // slots are numbered $0..$F; we only populate $9..$E

// Concrete bus state.  Hidden behind the opaque nubus_bus_t handle in
// nubus.h so card drivers can't reach into it without the public API.
struct nubus_bus {
    config_t *cfg;
    const nubus_slot_decl_t *slots; // the machine's slot table (topology)
    nubus_card_t *cards[NUBUS_MAX_SLOTS]; // cards[$9..$E]; NULL elsewhere
    // Which KIND seated each slot.  The object layer needs it to call
    // attach_objects without testing card identity; PCI carries the same
    // per-slot record.
    const nubus_card_kind_t *slot_kind[NUBUS_MAX_SLOTS];
    // How each socket was built: the entry its card was seated from, with
    // the card resolved, or `empty`.  The bus's checkpoint block, so a
    // restore seats exactly these.
    slot_opts_t seated[NUBUS_MAX_SLOTS];
};

// The bus-error window constants in nubus.h are spelled as literals because
// board descriptors are static initialisers; these pin them to the helpers
// they are meant to mirror, so a change to one spelling fails the build
// rather than drifting.
_Static_assert(NUBUS_BERR_LO == (0xF0000000u | (0x9u << 24)), "NUBUS_BERR_LO must equal nubus_slot_base(0x9)");
_Static_assert(NUBUS_BERR_HI == ((0xF0000000u | (0xEu << 24)) + 0x00FFFFFFu),
               "NUBUS_BERR_HI must equal nubus_slot_base(0xE) + 0xFFFFFF");
_Static_assert(NUBUS_BERR_HI_EXCL_SLOT_E == ((0xF0000000u | (0xDu << 24)) + 0x00FFFFFFu),
               "NUBUS_BERR_HI_EXCL_SLOT_E must equal nubus_slot_base(0xD) + 0xFFFFFF");

// === Card-kind registry =====================================================
//
// Single explicit list (no linker-section magic).  Adding a card driver
// is one extern + one entry.

extern const nubus_card_kind_t builtin_se30_video_kind; // machines/glue/builtin_se30_video.c
extern const nubus_card_kind_t mdc_8_24_kind; // cards/jmfb.c
extern const nubus_card_kind_t builtin_rbv_video_kind; // machines/mdu/builtin_rbv_video.c
extern const nubus_card_kind_t builtin_rbv_iisi_video_kind; // machines/mdu/builtin_rbv_video.c
extern const nubus_card_kind_t display_card_24ac_kind; // cards/display_card_24ac.c
extern const nubus_card_kind_t display_card_824gc_kind; // cards/display_card_824gc.c

static const nubus_card_kind_t *const g_card_registry[] = {
    &builtin_se30_video_kind,
    &mdc_8_24_kind,
    &builtin_rbv_video_kind,
    &builtin_rbv_iisi_video_kind,
    &display_card_24ac_kind,
    &display_card_824gc_kind,
    NULL,
};

const nubus_card_kind_t *const *nubus_card_registry(void) {
    return g_card_registry;
}

const nubus_card_kind_t *nubus_card_find(const char *id) {
    if (!id)
        return NULL;
    for (const nubus_card_kind_t *const *p = g_card_registry; *p; p++) {
        if (strcmp((*p)->id, id) == 0)
            return *p;
    }
    return NULL;
}

// Compare two ids ignoring underscores — "8_24gc" for "824gc", "mdc_824"
// for "mdc_8_24": a typo that close deserves a suggestion.
static bool ids_match_sans_underscores(const char *a, const char *b) {
    while (*a == '_')
        a++;
    while (*b == '_')
        b++;
    while (*a && *b) {
        if (*a != *b)
            return false;
        a++;
        b++;
        while (*a == '_')
            a++;
        while (*b == '_')
            b++;
    }
    return *a == '\0' && *b == '\0';
}

// Parse "monitor_Nbpp" against a kind's monitor catalogue -- see card.h.
bool nubus_monitor_mode_lookup(const nubus_monitor_t *list, const char *id, const nubus_monitor_t **out_monitor,
                               int *out_depth_bpp) {
    if (!list || !id || !*id)
        return false;
    // The LAST underscore is the boundary between the monitor name and the
    // "Nbpp" depth suffix -- monitor ids contain underscores themselves.
    const char *underscore_bpp = strrchr(id, '_');
    if (!underscore_bpp)
        return false;
    size_t mon_len = (size_t)(underscore_bpp - id);
    if (mon_len == 0 || mon_len >= NUBUS_VIDEO_MODE_ID_MAX)
        return false;
    char mon_id[NUBUS_VIDEO_MODE_ID_MAX];
    memcpy(mon_id, id, mon_len);
    mon_id[mon_len] = '\0';
    // Validate the bpp value as 1..32 before the (int) cast, which is
    // implementation-defined for out-of-range longs.
    const char *bpp_str = underscore_bpp + 1;
    char *end = NULL;
    long bpp = strtol(bpp_str, &end, 10);
    if (!end || end == bpp_str || strcmp(end, "bpp") != 0)
        return false;
    if (bpp < 1 || bpp > 32)
        return false;
    for (const nubus_monitor_t *m = list; m->id; m++) {
        if (strcmp(m->id, mon_id) != 0)
            continue;
        if (!m->depths)
            return false;
        for (const int *d = m->depths; *d; d++) {
            if ((int)bpp == *d) {
                if (out_monitor)
                    *out_monitor = m;
                if (out_depth_bpp)
                    *out_depth_bpp = (int)bpp;
                return true;
            }
        }
        return false; // monitor matched but depth didn't
    }
    return false;
}

const char *nubus_card_suggest(const char *id) {
    if (!id || !*id)
        return NULL;
    for (const nubus_card_kind_t *const *p = g_card_registry; *p; p++) {
        if (ids_match_sans_underscores((*p)->id, id))
            return (*p)->id;
    }
    return NULL;
}

// Parse a "WxHxD" custom-mode spec into width/height/depth.  Returns true
// on a well-formed spec with each field in range (the numeric limits the
// substitute ROMs can honour); false — with *err set to a static reason —
// otherwise.  Shared by boot-time validation and card_init.
bool nubus_custom_mode_parse(const char *spec, uint32_t *out_w, uint32_t *out_h, uint32_t *out_d, const char **err) {
    const char *reason = NULL;
    uint32_t w = 0, h = 0, d = 0;
    if (!spec || !*spec) {
        reason = "empty custom_mode";
        goto done;
    }
    // Strict "WxHxD" grammar: three unsigned decimals joined by 'x'.
    char *end = NULL;
    long lw = strtol(spec, &end, 10);
    if (end == spec || *end != 'x' || lw <= 0) {
        reason = "expected WxHxD (e.g. 800x600x8)";
        goto done;
    }
    const char *p = end + 1;
    long lh = strtol(p, &end, 10);
    if (end == p || *end != 'x' || lh <= 0) {
        reason = "expected WxHxD (e.g. 800x600x8)";
        goto done;
    }
    p = end + 1;
    long ld = strtol(p, &end, 10);
    if (end == p || *end != '\0' || ld <= 0) {
        reason = "expected WxHxD (e.g. 800x600x8)";
        goto done;
    }
    // Width and height have ceilings: the generated sResource stores both as
    // uint16_t (gsvrom_data.c make_mode, declrom.c's put16), so a larger
    // raster would leave the declaration ROM and the scanout descriptor
    // describing different pictures -- 70000 truncates to 4464 in the ROM
    // while display.height stays 70000.  2048 is the height ceiling DAFB and
    // the Mach64 already enforce; at 1 bpp the rowBytes rule below admits
    // widths up to 131071, so the width needs its own.  Checked on the parsed
    // longs, before the narrowing casts can wrap a huge value small.
    if (lw > UINT16_MAX) {
        reason = "width must be <= 65535";
        goto done;
    }
    if (lh > 2048) {
        reason = "height must be <= 2048";
        goto done;
    }
    w = (uint32_t)lw;
    h = (uint32_t)lh;
    d = ld > 32 ? 0 : (uint32_t)ld; // 0 fails the depth check below
    // Depth must be a supported indexed/direct bit depth.
    if (d != 1 && d != 2 && d != 4 && d != 8 && d != 16 && d != 32) {
        reason = "depth must be 1/2/4/8/16/32";
        goto done;
    }
    // Width must be a multiple of 32 (the gray-fill and stride math work
    // in 32-pixel/long units) and rowBytes must stay under $4000 (the
    // vpRowBytes field's high-bit-clear limit).
    if (w % 32 != 0) {
        reason = "width must be a multiple of 32";
        goto done;
    }
    if ((uint64_t)w * d / 8 >= 0x4000) {
        reason = "rowBytes (width*depth/8) must be < 0x4000";
        goto done;
    }
done:
    if (err)
        *err = reason;
    if (reason)
        return false;
    if (out_w)
        *out_w = w;
    if (out_h)
        *out_h = h;
    if (out_d)
        *out_d = d;
    return true;
}

// Card ↔ slot compatibility, COMPUTED from the two declarations (see the
// prototype comment in nubus.h): the slot must be user-configurable and the
// kind must attach through a genuine NuBus connector.  Builtin pseudo-cards
// (attach == CARD_ATTACH_BUILTIN, the conservative zero default) never fit
// a socket — they exist only where a BUILTIN slot decl names them.
bool nubus_card_fits_socket(const nubus_slot_decl_t *s, const nubus_card_kind_t *kind) {
    if (!s || !kind)
        return false;
    if (s->kind != NUBUS_SLOT_SOCKET)
        return false;
    return kind->attach == CARD_ATTACH_NUBUS;
}

bool nubus_card_fits_slot(const nubus_slot_decl_t *s, const nubus_card_kind_t *kind) {
    if (!s || !kind)
        return false;
    if (s->kind == NUBUS_SLOT_SOCKET)
        return nubus_card_fits_socket(s, kind);
    return s->kind == NUBUS_SLOT_BUILTIN && s->builtin_card_id && strcmp(s->builtin_card_id, kind->id) == 0;
}

bool nubus_entry_substitute(const nubus_card_kind_t *k, const slot_opts_t *e) {
    if (!k || !k->substitute)
        return false;
    return e->substitute || e->custom_mode[0] || !vrom_card_resolvable(k->id, e->rom[0] ? e->rom : NULL);
}

const nubus_monitor_t *nubus_kind_monitors(const nubus_card_kind_t *k, bool substitute) {
    if (!k)
        return NULL;
    return (substitute && k->substitute_monitors) ? k->substitute_monitors : k->monitors;
}

const nubus_monitor_t *nubus_entry_monitor(const nubus_card_kind_t *k, const slot_opts_t *e) {
    for (const nubus_monitor_t *m = nubus_kind_monitors(k, e->substitute); m && m->id; m++)
        if (strcmp(m->id, e->monitor) == 0)
            return m;
    return NULL;
}

void nubus_entry_default_monitor(const nubus_card_kind_t *k, slot_opts_t *e) {
    const nubus_monitor_t *rows = nubus_kind_monitors(k, e->substitute);
    if (e->monitor[0] || !rows || !rows->id)
        return;
    const nubus_monitor_t *row = rows;
    nubus_monitor_mode_lookup(rows, e->video_mode, &row, NULL);
    snprintf(e->monitor, sizeof e->monitor, "%s", row->id);
    e->sense = row->sense_code;
}

// Format a refusal into the caller's buffer and say no.
__attribute__((format(printf, 3, 4))) static bool refuse(char *why, size_t len, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, len, fmt, ap);
    va_end(ap);
    return false;
}

bool nubus_slot_entry_check(const nubus_slot_decl_t *slots, const char *model, const slot_opts_t *e, bool roms_final,
                            char *why, size_t why_len) {
    const nubus_slot_decl_t *d = NULL;
    for (const nubus_slot_decl_t *s = slots; s && s->slot; s++) {
        if (s->slot == e->slot)
            d = s;
    }
    if (!d || (d->kind != NUBUS_SLOT_SOCKET && d->kind != NUBUS_SLOT_BUILTIN))
        return refuse(why, why_len, "NuBus slot $%X on model '%s' takes no card", e->slot, model);
    const nubus_card_kind_t *k = NULL;
    bool named = false;
    if (e->empty) {
        if (d->kind != NUBUS_SLOT_SOCKET)
            return refuse(why, why_len, "NuBus slot $%X on model '%s' is built in and cannot be emptied", e->slot,
                          model);
    } else if (e->card[0]) {
        k = nubus_card_find(e->card);
        if (!k) {
            const char *near = nubus_card_suggest(e->card);
            if (near)
                return refuse(why, why_len, "unknown card id '%s' — did you mean '%s'? (see catalog.nubus_cards)",
                              e->card, near);
            return refuse(why, why_len, "unknown card id '%s' (see catalog.nubus_cards)", e->card);
        }
        if (!nubus_card_fits_slot(d, k))
            return refuse(why, why_len, "card '%s' fits no slot on model '%s' (see catalog.profile(\"%s\").cards)",
                          e->card, model, model);
        named = d->kind == NUBUS_SLOT_SOCKET;
    } else {
        k = nubus_card_find(d->kind == NUBUS_SLOT_SOCKET ? d->default_card : d->builtin_card_id);
    }
    if (e->substitute && !(k && k->substitute))
        return refuse(why, why_len, "slot $%X's card '%s' has no substitute ROM", e->slot, k ? k->id : "(none)");
    if (e->substitute && e->rom[0])
        return refuse(why, why_len, "slot $%X: a ROM file and the substitute ROM cannot both be given", e->slot);
    // The rows the card drives with the ROM it will run.  A boot decides the
    // ROM from the files offered now; a restore's entry already says.
    bool substitute = roms_final ? nubus_entry_substitute(k, e) : e->substitute;
    const nubus_monitor_t *rows = nubus_kind_monitors(k, substitute);
    if (e->video_mode[0]) {
        if (!nubus_video_mode_known(e->video_mode))
            return refuse(why, why_len, "unknown video-mode id '%s'", e->video_mode);
        if (!k || !nubus_monitor_mode_lookup(rows, e->video_mode, NULL, NULL))
            return refuse(why, why_len, "video mode '%s' does not belong to slot $%X's card '%s'%s", e->video_mode,
                          e->slot, k ? k->id : "(none)", substitute ? " with its substitute ROM" : "");
    }
    if (e->custom_mode[0]) {
        const char *bad = NULL;
        uint32_t w = 0, h = 0, depth = 0;
        if (!nubus_custom_mode_parse(e->custom_mode, &w, &h, &depth, &bad))
            return refuse(why, why_len, "custom_mode '%s' invalid: %s", e->custom_mode, bad);
        if (!k || !k->custom_mode_fits)
            return refuse(why, why_len, "slot $%X's card '%s' takes no custom geometry", e->slot, k ? k->id : "(none)");
        if (!k->custom_mode_fits(w, h, depth, &bad))
            return refuse(why, why_len, "custom_mode '%s' on slot $%X's card '%s': %s", e->custom_mode, e->slot, k->id,
                          bad);
    }
    if (e->monitor[0] && strcmp(e->monitor, "none") != 0) {
        bool known = false;
        for (const nubus_monitor_t *m = rows; m && m->id; m++)
            known |= strcmp(m->id, e->monitor) == 0;
        if (!known)
            return refuse(why, why_len, "monitor '%s' is not one slot $%X's card '%s' drives", e->monitor, e->slot,
                          k ? k->id : "(none)");
    }
    if (e->n_options)
        return refuse(why, why_len, "slot $%X's card '%s' takes no option '%s'", e->slot, k ? k->id : "(none)",
                      e->options[0].key);
    if (e->rom[0]) {
        vrom_id_t vid;
        if (!vrom_identify_card(e->rom, &vid))
            return refuse(why, why_len, "vrom '%s' is not a recognised declaration ROM", e->rom);
        if (!k || strcmp(vid.card_id, k->id) != 0)
            return refuse(why, why_len, "vrom '%s' is for card '%s', not slot $%X's '%s'", e->rom, vid.card_id, e->slot,
                          k ? k->id : "(none)");
    }
    // A card the document named must find a declaration ROM before the
    // running machine is touched: Apple's, or its substitute.  A slot's own
    // default without either fails to build, leaving the slot empty with a log.
    if (roms_final && named && k && !substitute && vrom_card_catalogued(k->id) &&
        !vrom_card_resolvable(k->id, e->rom[0] ? e->rom : NULL))
        return refuse(why, why_len, "card '%s' (slot $%X) needs a declaration ROM but no offered vROM file provides it",
                      k->id, e->slot);
    return true;
}

// === Bus controller =========================================================

// The largest declaration-ROM chip a card's block may carry (the 8•24 GC
// v1.1's 64 KB).
#define CARD_ROM_MAX (64u * 1024u)

// A card's block: the declaration ROM it runs -- its source path and the chip
// image, empty for a generated ROM -- then the card's own state.  The image
// is always in the block, so a checkpoint restores the card it was saved with
// whatever ROM files the host offers now.
static void nubus_card_part_save(void *obj, checkpoint_t *cp) {
    nubus_card_t *card = obj;
    bool file_backed = card->rom_chip && card->rom_chip_size;
    checkpoint_write_string(cp, file_backed ? card->rom_path : NULL);
    uint32_t size = file_backed ? (uint32_t)card->rom_chip_size : 0;
    system_write_checkpoint_data(cp, &size, sizeof size, "card.rom");
    if (size)
        system_write_checkpoint_data(cp, card->rom_chip, size, "card.rom");
    if (card->ops->checkpoint_save)
        card->ops->checkpoint_save(card, cp);
}

// Read the ROM a card's block carries: none (*size 0, a generated ROM) or a
// bounded chip image.  False when the block is malformed.
static bool read_card_rom(checkpoint_t *cp, int slot, uint8_t **out, size_t *size, char **path) {
    *out = NULL;
    *size = 0;
    *path = checkpoint_read_string(cp, CHECKPOINT_MAX_PATH, "card ROM path");
    uint32_t n = 0;
    system_read_checkpoint_data(cp, &n, sizeof n, "card.rom");
    if (checkpoint_has_error(cp))
        return false;
    if (!n)
        return true;
    if (n > CARD_ROM_MAX) {
        LOG(0, "Error: the checkpoint's slot $%X card ROM is %u bytes (at most %u)", slot, n, CARD_ROM_MAX);
        return false;
    }
    *out = malloc(n);
    if (!*out)
        return false;
    system_read_checkpoint_data(cp, *out, n, "card.rom");
    *size = n;
    return !checkpoint_has_error(cp);
}

// A restore's slot table is a file the user supplied: every entry that seats
// a card goes through the checks machine.boot applies to a document's, bar
// the ROM file (the card's ROM comes from its own block, not from a path).  A
// bad entry fails the restore, naming the slot.
static bool slot_table_valid(slot_opts_t *seated, const nubus_slot_decl_t *slots, const char *model) {
    for (int n = 0; n < NUBUS_MAX_SLOTS; n++) {
        slot_opts_t *e = &seated[n];
        if (!slot_opts_sanitize(e, n)) {
            LOG(0, "Error: the checkpoint's NuBus slot $%X entry is malformed", n);
            return false;
        }
        if (!e->card[0])
            continue;
        slot_opts_t check = *e;
        check.rom[0] = '\0';
        char why[256];
        if (!nubus_slot_entry_check(slots, model, &check, false, why, sizeof why)) {
            LOG(0, "Error: the checkpoint's NuBus slot $%X: %s", n, why);
            return false;
        }
    }
    return true;
}

static void nubus_slots_part_save(void *obj, checkpoint_t *cp) {
    nubus_bus_t *bus = obj;
    system_write_checkpoint_data(cp, bus->seated, sizeof(bus->seated), "nubus");
}

nubus_bus_t *nubus_init(config_t *cfg, const nubus_slot_decl_t *slots, checkpoint_t *cp) {
    if (!cfg)
        return NULL;
    nubus_bus_t *bus = calloc(1, sizeof(*bus));
    if (!bus)
        return NULL;
    bus->cfg = cfg;
    bus->slots = slots;

    // What each slot seats.  A boot takes the document's entry (validated
    // before anything was built), else the slot's declared builtin / default
    // card, so a machine boots as many cards as its sockets carry
    // configuration for (multi-display).  Each seat is then made definite:
    // which declaration ROM its card runs, and the monitor on its connector
    // -- built-in video takes the build's monitor and connection, a socket's
    // card the monitor its entry chose or its default.  A restore takes the
    // entries from the bus's own block.
    machine_part_begin(cfg, cp, "nubus");
    if (cp) {
        system_read_checkpoint_data(cp, bus->seated, sizeof(bus->seated), "nubus");
        if (!slot_table_valid(bus->seated, slots, cfg->machine->id))
            checkpoint_set_error(cp);
    } else if (slots) {
        for (const nubus_slot_decl_t *s = slots; s->slot != 0; s++) {
            if (s->slot < 0 || s->slot >= NUBUS_MAX_SLOTS)
                continue;
            slot_opts_t *seat = &bus->seated[s->slot];
            const slot_opts_t *entry = machine_build_opts_slot(&cfg->build_opts, s->slot);
            if (entry)
                *seat = *entry;
            seat->slot = s->slot;
            if (s->kind != NUBUS_SLOT_BUILTIN && s->kind != NUBUS_SLOT_SOCKET)
                seat->empty = true;
            const char *declared = s->kind == NUBUS_SLOT_BUILTIN ? s->builtin_card_id : s->default_card;
            if (!seat->empty && !seat->card[0] && declared)
                snprintf(seat->card, sizeof seat->card, "%s", declared);
            if (seat->empty || !seat->card[0])
                continue;
            const nubus_card_kind_t *kind = nubus_card_find(seat->card);
            seat->substitute = nubus_entry_substitute(kind, seat);
            if (s->kind == NUBUS_SLOT_BUILTIN) {
                // The port's own monitor row -- the one whose code the sense
                // lines read, else its default (a video_sense= override) --
                // unless none is plugged in; the sense lines read what the
                // build resolved.
                seat->connected = cfg->build_opts.builtin_connected;
                if (cfg->build_opts.builtin_sense == MACHINE_SENSE_NONE)
                    snprintf(seat->monitor, sizeof seat->monitor, "none");
                for (const nubus_monitor_t *m = kind ? kind->monitors : NULL; m && m->id && !seat->monitor[0]; m++)
                    if (m->sense_code == cfg->build_opts.builtin_sense)
                        snprintf(seat->monitor, sizeof seat->monitor, "%s", m->id);
                nubus_entry_default_monitor(kind, seat);
                seat->sense = cfg->build_opts.builtin_sense;
            } else {
                nubus_entry_default_monitor(kind, seat);
            }
        }
    }
    machine_part(cfg, cp, "nubus", nubus_slots_part_save, bus);

    for (int n = 0; slots && n < NUBUS_MAX_SLOTS; n++) {
        slot_opts_t *seat = &bus->seated[n];
        if (seat->empty || !seat->card[0])
            continue;
        const nubus_card_kind_t *kind = nubus_card_find(seat->card);
        if (!kind || !kind->ops || !kind->ops->init) {
            seat->empty = true;
            continue;
        }
        bus->slot_kind[n] = kind;
        // The bus owns the allocation, so `bus` and `slot` are populated
        // BEFORE init runs -- a card may assert its slot IRQ, or touch any
        // other bus service, from card_init.
        nubus_card_t *card = calloc(1, sizeof(*card));
        if (!card) {
            LOG(0, "nubus: out of memory seating slot $%X card '%s'", n, kind->id ? kind->id : "?");
            bus->slot_kind[n] = NULL;
            seat->empty = true;
            continue;
        }
        card->ops = kind->ops;
        card->bus = bus;
        card->slot = n;
        card->substitute = seat->substitute;

        // Each card is a part of its own, after the slot table: on a restore
        // its block gives the declaration ROM it runs, which init takes, and
        // then its state.
        char part[32];
        snprintf(part, sizeof part, "nubus.slot.%X", n);
        machine_part_begin(cfg, cp, part);
        uint8_t *rom = NULL;
        char *rom_path = NULL;
        if (cp && !read_card_rom(cp, n, &rom, &card->restored_rom.size, &rom_path))
            checkpoint_set_error(cp);
        card->restored_rom.data = rom;
        card->restored_rom.path = rom_path;
        int rc = card->ops->init(card, cfg, cp, seat);
        card->restored_rom = (rom_image_t){0};
        free(rom);
        free(rom_path);
        if (rc != 0) {
            // Typically a missing/invalid VROM file or out of memory.  Log
            // it, so a boot-time failure does not manifest later as "the
            // card is missing for unclear reasons".  A restore cannot go on
            // without a card its checkpoint carries.
            LOG(1, "nubus: slot $%X card '%s' failed to initialise", n, kind->id ? kind->id : "?");
            if (cp) {
                LOG(0, "Error: the checkpoint's slot $%X card '%s' could not be built", n, kind->id ? kind->id : "?");
                checkpoint_set_error(cp);
            }
            machine_part_cancel(cfg);
            free(card->rom_path);
            free(card->rom_chip);
            free(card);
            bus->slot_kind[n] = NULL;
            seat->empty = true;
            continue;
        }
        bus->cards[n] = card;
        if (cp && card->ops->checkpoint_restore)
            card->ops->checkpoint_restore(card, cp);
        machine_part(cfg, cp, part, nubus_card_part_save, card);
    }
    // The object model's machine.nubus tree is built when the machine becomes
    // the active one (system_swap_in), not here: a build that fails must leave
    // the running machine's tree alone.
    return bus;
}

void nubus_delete(nubus_bus_t *bus) {
    if (!bus)
        return;
    // Drop the object-model node trees before the cards they read go away
    // (ownership-checked: on checkpoint restore this bus may already have
    // been superseded by the new machine's tree).
    nubus_objects_teardown_owned(bus);
    for (int i = 0; i < NUBUS_MAX_SLOTS; i++) {
        nubus_card_t *card = bus->cards[i];
        if (!card)
            continue;
        if (card->ops && card->ops->teardown)
            card->ops->teardown(card, bus->cfg);
        free(card->declrom);
        free(card->rom_path);
        free(card->rom_chip);
        free(card);
        bus->cards[i] = NULL;
    }
    free(bus);
}

const nubus_slot_decl_t *nubus_slot_decl_get(nubus_bus_t *bus, int slot) {
    if (!bus || !bus->slots)
        return NULL;
    for (const nubus_slot_decl_t *s = bus->slots; s->slot != 0; s++) {
        if (s->slot == slot)
            return s;
    }
    return NULL;
}

nubus_card_t *nubus_card(nubus_bus_t *bus, int slot) {
    if (!bus || slot < 0 || slot >= NUBUS_MAX_SLOTS)
        return NULL;
    return bus->cards[slot];
}

const nubus_card_kind_t *nubus_slot_kind(nubus_bus_t *bus, int slot) {
    if (!bus || slot < 0 || slot >= NUBUS_MAX_SLOTS)
        return NULL;
    return bus->slot_kind[slot];
}

const slot_opts_t *nubus_seat(nubus_bus_t *bus, int slot) {
    if (!bus || slot < 0 || slot >= NUBUS_MAX_SLOTS)
        return NULL;
    return &bus->seated[slot];
}

bool nubus_startup_record(nubus_bus_t *bus, int slot, uint8_t rec[8]) {
    const nubus_card_kind_t *k = nubus_slot_kind(bus, slot);
    if (!k || !k->startup_record || !bus->cards[slot] || strcmp(bus->seated[slot].monitor, "none") == 0)
        return false;
    return k->startup_record(&bus->seated[slot], rec);
}

nubus_card_t *nubus_connected_display_card(nubus_bus_t *bus) {
    for (int i = 0; bus && i < NUBUS_MAX_SLOTS; i++) {
        nubus_card_t *card = bus->cards[i];
        if (card && bus->seated[i].connected && card->ops && card->ops->display)
            return card;
    }
    return NULL;
}

display_t *nubus_connected_display(nubus_bus_t *bus) {
    nubus_card_t *card = nubus_connected_display_card(bus);
    return card ? card->ops->display(card) : NULL;
}

void nubus_tick_vbl(nubus_bus_t *bus) {
    if (!bus)
        return;
    for (int i = 0; i < NUBUS_MAX_SLOTS; i++) {
        nubus_card_t *card = bus->cards[i];
        if (card && card->ops && card->ops->on_vbl)
            card->ops->on_vbl(card, bus->cfg);
    }
}

// Assert the NuBus /RESET line: reset every populated card to its power-on
// state.  Called from the CPU RESET instruction (system_reset_devices) so a
// warm restart brings the cards back to power-on, exactly as the /RESET pin
// does in hardware.  Cards without a reset hook keep their state (safe).
void nubus_reset(nubus_bus_t *bus) {
    if (!bus)
        return;
    for (int i = 0; i < NUBUS_MAX_SLOTS; i++) {
        nubus_card_t *card = bus->cards[i];
        if (card && card->ops && card->ops->reset)
            card->ops->reset(card, bus->cfg);
    }
}

// A power cycle: every card's power_on hook (machine.restart; the /RESET
// that follows is nubus_reset).
void nubus_power_on(nubus_bus_t *bus) {
    if (!bus)
        return;
    for (int i = 0; i < NUBUS_MAX_SLOTS; i++) {
        nubus_card_t *card = bus->cards[i];
        if (card && card->ops && card->ops->power_on)
            card->ops->power_on(card, bus->cfg);
    }
}

// === Slot-IRQ aggregation ===================================================
//
// Each NuBus slot's /NMRQ line maps to a VIA2 PA bit (active-low):
//   slot $9 → PA0 ... slot $E → PA5
// The bus controller drives the per-slot bit and pulses CA1 on the
// umbrella transition (no slot asserted → any slot asserted).

// Drive a slot's /NMRQ line through the machine substrate: the bus owns the
// slot-IRQ aggregate mask and the umbrella transition, the chipset owns HOW
// the line reaches the CPU (GLUE/MCU → VIA2; MDU → the RBV; OSS → the OSS;
// AV → the PSC; PDM → BART), including converting the slot number into
// whatever its controller numbers sources by.  nubus.c stays machine-agnostic
// — no cfg->via2 here.
static void nubus_route_slot_irq(config_t *cfg, int slot, bool active) {
    if (cfg && cfg->machine && cfg->machine->substrate->nubus_slot_irq)
        cfg->machine->substrate->nubus_slot_irq(cfg, slot, active);
}

// The bus keeps NO aggregate of which slots are asserting, deliberately.
//
// It used to: a `slot_irq_mask` here computed an `umbrella_edge` that was
// passed down to the substrate.  That was wrong, because only the chipset can
// see the non-NuBus contributors to /SLOTIRQ -- the SE/30's built-in video,
// the MCU's DAFB on PA6 and SONIC on PA0 -- so a bus-side OR was always a
// partial one.  With the edge consumer gone the mask was maintained and
// checkpointed but read by nothing, and it is now deleted.
//
// Do not bring it back as an edge source.  Its checkpointing exists in the
// history for a reason (restoring it as zero while a slot was still asserting
// made both edges compute from a lie), and reintroducing the mask without
// that fix reintroduces the bug.  The chipset owns the OR.
void nubus_assert_irq(nubus_card_t *card) {
    if (!card || !card->bus)
        return;
    nubus_route_slot_irq(card->bus->cfg, card->slot, /*active*/ true);
}

void nubus_deassert_irq(nubus_card_t *card) {
    if (!card || !card->bus)
        return;
    nubus_route_slot_irq(card->bus->cfg, card->slot, /*active*/ false);
}
