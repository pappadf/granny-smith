// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// nubus.c
// NuBus subsystem skeleton.  Step-3 status (see proposal §4 step 3): the
// types and public API are wired up; the bus controller body is a
// minimal skeleton — no machine creates a bus yet.  The card-kind
// registry is empty until step 4 lands the first card.

#include "nubus.h"
#include "card.h"
#include "jmfb.h" // ONLY for stage_custom_for_kind; see the note there
#include "log.h"
#include "machine_config.h" // the built-from record's per-slot picks
#include "machine_profile.h" // machine_substrate_t (slot-IRQ routing)
#include "system_config.h"

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
    // attach_objects without testing card identity (04-video F-10); PCI has
    // carried the same per-slot record since its own §5.1.
    const nubus_card_kind_t *slot_kind[NUBUS_MAX_SLOTS];
    // Which slots are asserting, as a bitmap ($9..$E).  The bus's OWN record
    // and nothing more: the aggregate that actually drives /SLOTIRQ is the
    // chipset's, because only the chipset can see the non-NuBus contributors
    // (the SE/30's built-in video, the MCU's DAFB and SONIC) --
    // 05-chipsets-irq F-46.  Until that finding this mask also computed an
    // `umbrella_edge` passed down to the substrate, which is why 04-video
    // F-42 had to start checkpointing it; that consumer is gone, so the field
    // is now maintained and saved but not READ for any decision.  Kept
    // because a machine.nubus node would want it, and because deleting state
    // another branch fixed a bug in deserves its own change rather than
    // riding along in this one -- but it is a deletion candidate, and it must
    // not be reintroduced as an edge source.
    uint16_t slot_irq_mask;
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
extern const nubus_card_kind_t builtin_se30_video_generic_kind; // machines/glue ("se30" generic sibling)
extern const nubus_card_kind_t mdc_8_24_kind; // cards/jmfb.c
extern const nubus_card_kind_t jmfb_generic_kind; // cards/jmfb.c ("8_24" generic sibling)
extern const nubus_card_kind_t builtin_rbv_video_kind; // cards/builtin_rbv_video.c
extern const nubus_card_kind_t display_card_24ac_kind; // cards/display_card_24ac.c
extern const nubus_card_kind_t display_card_24ac_generic_kind; // cards/display_card_24ac.c ("24ac")
extern const nubus_card_kind_t display_card_824gc_kind; // cards/display_card_824gc.c
extern const nubus_card_kind_t display_card_824gc_generic_kind; // cards/display_card_824gc.c ("8_24gc")

static const nubus_card_kind_t *const g_card_registry[] = {
    &builtin_se30_video_kind,
    &builtin_se30_video_generic_kind,
    &mdc_8_24_kind,
    &jmfb_generic_kind,
    &builtin_rbv_video_kind,
    &display_card_24ac_kind,
    &display_card_24ac_generic_kind,
    &display_card_824gc_kind,
    &display_card_824gc_generic_kind,
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

// Compare two ids ignoring underscores — "8_24gc" and "824gc" are one
// underscore apart by design (real vs generic sibling, proposal-generic-
// nubus-vrom sec. 11.3), so a typo between them deserves a suggestion.
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

// === Staged per-slot configuration (proposal §5.6, stage 2) =================
//
// One entry per slot ($9..$E) plus the WILDCARD entry [0] meaning "the
// machine's first SOCKET" (the machine-independent channel behind the
// `machine.nubus.video_card` / `video_mode` aliases and the headless
// `video_card=` startup arg).  nubus_init consumes the whole table and
// clears it so a stale pick can't leak into the next boot.
typedef struct nubus_staged_slot {
    char card[32]; // staged card-kind id ("" = none)
    char mode[40]; // staged video-mode id ("" = none)
    char custom[40]; // staged "WxHxD" custom resolution ("" = none)
} nubus_staged_slot_t;
static nubus_staged_slot_t s_staged[NUBUS_MAX_SLOTS];

// Valid staged-table keys: the wildcard, or a physical slot number.
static bool staged_slot_valid(int slot) {
    return slot == NUBUS_STAGED_WILDCARD || (slot >= 9 && slot < NUBUS_MAX_SLOTS);
}

void nubus_staged_card_set(int slot, const char *id) {
    if (!staged_slot_valid(slot))
        return;
    snprintf(s_staged[slot].card, sizeof s_staged[slot].card, "%s", (id && *id) ? id : "");
}

const char *nubus_staged_card_get(int slot) {
    if (!staged_slot_valid(slot))
        return NULL;
    return s_staged[slot].card[0] ? s_staged[slot].card : NULL;
}

void nubus_staged_mode_set(int slot, const char *id) {
    if (!staged_slot_valid(slot))
        return;
    snprintf(s_staged[slot].mode, sizeof s_staged[slot].mode, "%s", (id && *id) ? id : "");
}

const char *nubus_staged_mode_get(int slot) {
    if (!staged_slot_valid(slot))
        return NULL;
    return s_staged[slot].mode[0] ? s_staged[slot].mode : NULL;
}

void nubus_staged_custom_mode_set(int slot, const char *spec) {
    if (!staged_slot_valid(slot))
        return;
    snprintf(s_staged[slot].custom, sizeof s_staged[slot].custom, "%s", (spec && *spec) ? spec : "");
}

const char *nubus_staged_custom_mode_get(int slot) {
    if (!staged_slot_valid(slot))
        return NULL;
    return s_staged[slot].custom[0] ? s_staged[slot].custom : NULL;
}

// Parse a "WxHxD" custom-mode spec into width/height/depth.  Returns true
// on a well-formed spec with each field in range (the numeric limits the
// generic cards can honour); false — with *err set to a static reason —
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
    w = (uint32_t)lw;
    h = (uint32_t)lh;
    d = (uint32_t)ld;
    // Depth must be a supported indexed/direct bit depth.
    if (d != 1 && d != 2 && d != 4 && d != 8 && d != 16 && d != 32) {
        reason = "depth must be 1/2/4/8/16/32";
        goto done;
    }
    // Width must be a multiple of 32 (the gray-fill and stride math work
    // in 32-pixel/long units) and rowBytes must stay under $4000 (the
    // vpRowBytes field's high-bit-clear limit; proposal §4.1).
    if (w % 32 != 0) {
        reason = "width must be a multiple of 32";
        goto done;
    }
    if ((uint64_t)w * d / 8 >= 0x4000) {
        reason = "rowBytes (width*depth/8) must be < 0x4000";
        goto done;
    }
    // Height has a ceiling too: the generated sResource stores it as a
    // uint16_t (gsvrom_data.c make_mode), so a taller raster would leave the
    // declaration ROM and the scanout descriptor describing different
    // pictures -- 70000 truncates to 4464 in the ROM while display.height
    // stays 70000 (04-video F-32).  2048 is the ceiling DAFB and the Mach64
    // already enforce.
    if (h > 2048) {
        reason = "height must be <= 2048";
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

static void staged_clear_all(void) {
    memset(s_staged, 0, sizeof s_staged);
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

// Resolve a SOCKET slot's card id: a staged pick for this exact slot beats
// the wildcard (honoured only on the machine's FIRST socket, preserving the
// single-pending era's semantics); both are honoured iff the named kind
// physically fits the slot; the fallback is the declared default_card
// (NULL = the socket ships empty).  Rejections log at level 0 so a bad pick
// is visible by default instead of silently booting the wrong card.
// *out_explicit reports whether the USER named the winner — the built-from
// record keeps the two apart, because a default that cannot resolve its
// declaration ROM degrades to an empty slot while an explicit one fails the
// boot (machine_config_slot_card_t).
static const char *socket_card_id(const nubus_slot_decl_t *s, bool is_first_socket, bool *out_explicit) {
    *out_explicit = false;
    const char *staged = nubus_staged_card_get(s->slot);
    if (!staged && is_first_socket)
        staged = nubus_staged_card_get(NUBUS_STAGED_WILDCARD);
    if (staged) {
        if (nubus_card_fits_socket(s, nubus_card_find(staged))) {
            *out_explicit = true;
            return staged;
        }
        LOG(0, "nubus: staged card '%s' does not fit slot $%X; using default '%s'", staged, s->slot,
            s->default_card ? s->default_card : "(none)");
    }
    return s->default_card;
}

// The staged video-mode id for a SOCKET slot: this exact slot's entry, or
// the wildcard's on the machine's first socket.
static const char *socket_staged_mode(const nubus_slot_decl_t *s, bool is_first_socket) {
    const char *mode = nubus_staged_mode_get(s->slot);
    if (!mode && is_first_socket)
        mode = nubus_staged_mode_get(NUBUS_STAGED_WILDCARD);
    return mode;
}

// Route a staged video-mode id into the resolved card kind's pending-mode
// channel — the per-driver static its factory consumes at init.  Validated
// against the kind's own catalog so a mode staged for a different card is
// skipped with a log rather than silently mis-seeding the slot PRAM.
static void stage_mode_for_kind(int slot, const nubus_card_kind_t *kind, const char *mode) {
    if (!mode || !*mode || !kind)
        return;
    if (kind->stage_video_mode && nubus_monitor_mode_lookup(kind->monitors, mode, NULL, NULL))
        kind->stage_video_mode(mode);
    else
        LOG(0, "nubus: staged video_mode '%s' does not belong to slot $%X card '%s' — ignored", mode, slot, kind->id);
}

// Route a staged "WxHxD" custom resolution into the resolved kind's
// pending-custom channel.  Only the generic JMFB kind honours it today —
// it generates its declaration ROM at card_init and can boot its default
// monitor at the custom geometry; the real-dump kinds carry fixed images
// (§1.2), and the other generic kinds are a follow-up.
static void stage_custom_for_kind(int slot, const nubus_card_kind_t *kind, const char *spec) {
    if (!spec || !*spec || !kind)
        return;
    // The last identity test in this file, kept DELIBERATELY.  Routing it
    // through a kind hook would mean adding another staging seam, and staging
    // is what proposal-construction-inputs.md R1 deletes outright -- the
    // custom mode becomes a machine_build_opts_t field handed to the factory,
    // at which point this function and jmfb.h's include above both go.  Making
    // a condemned channel more polite is not worth a new hook (04-video F-10).
    if (kind == &jmfb_generic_kind)
        jmfb_pending_custom_mode_set(spec);
    else
        LOG(0, "nubus: custom_mode '%s' unsupported on slot $%X card '%s' — ignored", spec, slot, kind->id);
}

// === Bus controller =========================================================

nubus_bus_t *nubus_init(config_t *cfg, const nubus_slot_decl_t *slots, checkpoint_t *cp) {
    if (!cfg)
        return NULL;
    nubus_bus_t *bus = calloc(1, sizeof(*bus));
    if (!bus)
        return NULL;
    bus->cfg = cfg;
    bus->slots = slots;

    // Walk the slot table.  BUILTIN slots resolve their card via
    // nubus_card_find(.builtin_card_id); each SOCKET resolves its staged
    // pick (or default) independently, so a machine boots as many cards as
    // its sockets carry configuration for (multi-display, proposal §5.6).
    if (slots) {
        // The machine's first SOCKET — the slot the WILDCARD staged entry
        // (the `machine.nubus.video_card` alias) applies to.
        int first_socket = -1;
        for (const nubus_slot_decl_t *s = slots; s->slot != 0; s++) {
            if (s->kind == NUBUS_SLOT_SOCKET) {
                first_socket = s->slot;
                break;
            }
        }
        for (const nubus_slot_decl_t *s = slots; s->slot != 0; s++) {
            const nubus_card_kind_t *kind = NULL;
            const char *staged_mode = NULL;
            bool explicit_pick = false; // did the USER name this card?
            switch (s->kind) {
            case NUBUS_SLOT_BUILTIN: {
                // A BUILTIN slot boots its declared card, but a staged pick
                // (video_card= — this exact slot, or the wildcard on a
                // machine with no sockets) may substitute another BUILTIN-
                // attach sibling: this is how the SE/30 chooses between its
                // generic default and the real-vROM kind (proposal-generic-
                // nubus-vrom sec. 6.2 / 11.5).
                const char *staged = nubus_staged_card_get(s->slot);
                if (!staged && first_socket < 0)
                    staged = nubus_staged_card_get(NUBUS_STAGED_WILDCARD);
                if (staged) {
                    const nubus_card_kind_t *k = nubus_card_find(staged);
                    if (k && k->attach == CARD_ATTACH_BUILTIN) {
                        kind = k;
                        explicit_pick = true;
                    } else
                        LOG(0, "nubus: staged card '%s' cannot replace builtin slot $%X; using '%s'", staged, s->slot,
                            s->builtin_card_id);
                }
                if (!kind)
                    kind = nubus_card_find(s->builtin_card_id);
                staged_mode = (first_socket < 0) ? nubus_staged_mode_get(NUBUS_STAGED_WILDCARD) : NULL;
                if (!staged_mode)
                    staged_mode = nubus_staged_mode_get(s->slot);
                break;
            }
            case NUBUS_SLOT_SOCKET:
                kind = nubus_card_find(socket_card_id(s, s->slot == first_socket, &explicit_pick));
                staged_mode = socket_staged_mode(s, s->slot == first_socket);
                break;
            case NUBUS_SLOT_ABSENT:
            case NUBUS_SLOT_EMPTY:
                continue;
            }
            if (!kind || !kind->ops || !kind->ops->init)
                continue;
            // Route this slot's staged video mode into the kind's pending
            // channel immediately before its factory consumes it, so each
            // socket's mode seeds its own card even with several sockets.
            if (staged_mode)
                stage_mode_for_kind(s->slot, kind, staged_mode);
            // Likewise for a staged custom resolution (§3.6): the generic
            // display kinds generate a sResource for it and boot at its
            // geometry.  Wildcard applies to the first socket / a
            // socketless machine's builtin, same as the mode channel.
            const char *staged_custom = nubus_staged_custom_mode_get(s->slot);
            if (!staged_custom && s->slot == first_socket)
                staged_custom = nubus_staged_custom_mode_get(NUBUS_STAGED_WILDCARD);
            if (!staged_custom && first_socket < 0)
                staged_custom = nubus_staged_custom_mode_get(NUBUS_STAGED_WILDCARD);
            if (staged_custom)
                stage_custom_for_kind(s->slot, kind, staged_custom);
            bus->slot_kind[s->slot] = kind;
            // The bus owns the allocation, so `bus` and `slot` are populated
            // BEFORE init runs -- a card may assert its slot IRQ, or touch any
            // other bus service, from card_init (04-video F-52).
            nubus_card_t *card = calloc(1, sizeof(*card));
            if (!card) {
                LOG(0, "nubus: out of memory seating slot $%X card '%s'", s->slot, kind->id ? kind->id : "?");
                continue;
            }
            card->ops = kind->ops;
            card->bus = bus;
            card->slot = s->slot;
            if (card->ops->init(card, cfg, cp) != 0) {
                // Typically a missing/invalid VROM file or out of memory.  Log
                // it, so a boot-time failure does not manifest later as "the
                // card is missing for unclear reasons".
                LOG(1, "nubus: slot $%X card '%s' failed to initialise", s->slot, kind->id ? kind->id : "?");
                free(card);
                continue;
            }
            if (s->slot >= 0 && s->slot < NUBUS_MAX_SLOTS)
                bus->cards[s->slot] = card;
            // Capture the RESOLVED pick in the built-from record, so
            // machine.restart re-seats every populated slot and not just
            // the wildcard one (proposal-pci-architecture §8.2, the fix
            // for the NuBus record's known wildcard-only gap).
            machine_config_note_slot_card(MC_BUS_NUBUS, s->slot, kind->id, explicit_pick);
        }
    }
    // Consume the whole staged table so a stale selection doesn't leak
    // into the next machine.boot (mirrors jmfb's pending-sense reset).
    staged_clear_all();
    // Project the declared slots into the object model (proposal §3.8):
    // machine.nubus.slot[N].card.{framebuffer,declrom,clut,mode,…} for
    // populated slots, staged card_id/video_mode attrs on empty sockets.
    nubus_objects_build(bus);
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

// Serialise every seated card that implements the hooks, in slot order.
//
// Save and restore walk the slots identically, and a machine restores with the
// same slot table it saved with (the built-from record pins the staging), so
// the stream stays in step without any per-card tagging.  Cards that do not
// implement the hooks contribute nothing, exactly as before.
void nubus_checkpoint_save(nubus_bus_t *bus, checkpoint_t *cp) {
    if (!bus || !cp)
        return;
    // The aggregate, before the cards.  It is bus state, not card state: no
    // card knows whether ANOTHER slot is still asserting, and that is exactly
    // what decides the umbrella edge.  Nothing wrote it, so a machine
    // checkpointed with a slot interrupt asserted came back with the mask at
    // zero and BOTH edges were then computed from a lie (04-video F-42) -- the
    // next assert saw `any_was_asserted` false and raised an umbrella edge that
    // had already been raised, and the next deassert saw `mask == 0` and
    // dropped the umbrella while another slot was still holding it.
    system_write_checkpoint_data(cp, &bus->slot_irq_mask, sizeof(bus->slot_irq_mask));
    for (int i = 0; i < NUBUS_MAX_SLOTS; i++) {
        nubus_card_t *card = bus->cards[i];
        if (card && card->ops && card->ops->checkpoint_save)
            card->ops->checkpoint_save(card, cp);
    }
}

void nubus_checkpoint_restore(nubus_bus_t *bus, checkpoint_t *cp) {
    if (!bus || !cp)
        return;
    system_read_checkpoint_data(cp, &bus->slot_irq_mask, sizeof(bus->slot_irq_mask));
    for (int i = 0; i < NUBUS_MAX_SLOTS; i++) {
        nubus_card_t *card = bus->cards[i];
        if (card && card->ops && card->ops->checkpoint_restore)
            card->ops->checkpoint_restore(card, cp);
    }
}

display_t *nubus_primary_display(nubus_bus_t *bus) {
    if (!bus)
        return NULL;
    // First slot in numerical order whose ops->display() returns non-NULL.
    for (int i = 0; i < NUBUS_MAX_SLOTS; i++) {
        nubus_card_t *card = bus->cards[i];
        if (!card || !card->ops || !card->ops->display)
            continue;
        display_t *d = card->ops->display(card);
        if (d)
            return d;
    }
    return NULL;
}

nubus_card_t *nubus_primary_display_card(nubus_bus_t *bus) {
    if (!bus)
        return NULL;
    for (int i = 0; i < NUBUS_MAX_SLOTS; i++) {
        nubus_card_t *card = bus->cards[i];
        if (!card || !card->ops || !card->ops->display)
            continue;
        if (card->ops->display(card))
            return card;
    }
    return NULL;
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

// === Slot-IRQ aggregation ===================================================
//
// Each NuBus slot's /NMRQ line maps to a VIA2 PA bit (active-low):
//   slot $9 → PA0 ... slot $E → PA5
// The bus controller drives the per-slot bit and pulses CA1 on the
// umbrella transition (no slot asserted → any slot asserted).  Pure
// skeleton at step 3 — no card calls into here yet.

// Drive a slot's /NMRQ line through the machine substrate (proposal §4.4): the
// bus owns the slot-IRQ aggregate mask and the umbrella transition, the chipset
// owns HOW the line reaches the CPU (GLUE/MCU → VIA2; MDU → the RBV; OSS →
// the OSS; AV → the PSC; PDM → BART), including converting the slot number
// into whatever its controller numbers sources by.  nubus.c stays
// machine-agnostic — no cfg->via2 here.
static void nubus_route_slot_irq(config_t *cfg, int slot, bool active) {
    if (cfg && cfg->machine && cfg->machine->substrate->nubus_slot_irq)
        cfg->machine->substrate->nubus_slot_irq(cfg, slot, active);
}

void nubus_assert_irq(nubus_card_t *card) {
    if (!card || !card->bus)
        return;
    nubus_bus_t *bus = card->bus;
    bus->slot_irq_mask |= (uint16_t)(1u << card->slot);
    nubus_route_slot_irq(bus->cfg, card->slot, /*active*/ true);
}

void nubus_deassert_irq(nubus_card_t *card) {
    if (!card || !card->bus)
        return;
    nubus_bus_t *bus = card->bus;
    bus->slot_irq_mask &= (uint16_t) ~(1u << card->slot);
    nubus_route_slot_irq(bus->cfg, card->slot, /*active*/ false);
}
