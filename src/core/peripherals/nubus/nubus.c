// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// nubus.c
// NuBus subsystem skeleton.  Step-3 status (see proposal §4 step 3): the
// types and public API are wired up; the bus controller body is a
// minimal skeleton — no machine creates a bus yet.  The card-kind
// registry is empty until step 4 lands the first card.

#include "nubus.h"
#include "card.h"
#include "display_card_24ac.h" // staged video-mode routing (stage_mode_for_kind)
#include "display_card_824gc.h"
#include "jmfb.h"
#include "log.h"
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
    uint16_t slot_irq_mask; // bitmap; bit $9 .. bit $E
};

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
static const char *socket_card_id(const nubus_slot_decl_t *s, bool is_first_socket) {
    const char *staged = nubus_staged_card_get(s->slot);
    if (!staged && is_first_socket)
        staged = nubus_staged_card_get(NUBUS_STAGED_WILDCARD);
    if (staged) {
        if (nubus_card_fits_socket(s, nubus_card_find(staged)))
            return staged;
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
    if ((kind == &mdc_8_24_kind || kind == &jmfb_generic_kind) && jmfb_video_mode_lookup(mode, NULL, NULL))
        jmfb_pending_video_mode_set(mode);
    else if ((kind == &display_card_24ac_kind || kind == &display_card_24ac_generic_kind) &&
             display_card_24ac_video_mode_lookup(mode, NULL, NULL))
        display_card_24ac_pending_video_mode_set(mode);
    else if ((kind == &display_card_824gc_kind || kind == &display_card_824gc_generic_kind) &&
             display_card_824gc_video_mode_lookup(mode, NULL, NULL))
        display_card_824gc_pending_video_mode_set(mode);
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
                    if (k && k->attach == CARD_ATTACH_BUILTIN)
                        kind = k;
                    else
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
                kind = nubus_card_find(socket_card_id(s, s->slot == first_socket));
                staged_mode = socket_staged_mode(s, s->slot == first_socket);
                break;
            case NUBUS_SLOT_ABSENT:
            case NUBUS_SLOT_EMPTY:
                continue;
            }
            if (!kind || !kind->factory)
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
            nubus_card_t *card = kind->factory(s->slot, cfg, cp);
            if (!card) {
                // Factory returned NULL — typically a missing/invalid VROM
                // file or out-of-memory. Log so a silent boot-time failure
                // doesn't manifest as "card is missing for unclear reasons".
                LOG(1, "nubus: slot $%X card factory '%s' returned NULL", s->slot, (kind && kind->id) ? kind->id : "?");
                continue;
            }
            card->bus = bus;
            card->slot = s->slot;
            if (s->slot >= 0 && s->slot < NUBUS_MAX_SLOTS)
                bus->cards[s->slot] = card;
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
// owns HOW the line reaches the CPU (GLUE → VIA2; MDU/OSS → its own IRQ
// controller).  nubus.c stays machine-agnostic — no cfg->via2 here.
static void nubus_route_slot_irq(config_t *cfg, int slot, bool active, bool umbrella_edge) {
    if (cfg && cfg->machine && cfg->machine->substrate->nubus_slot_irq)
        cfg->machine->substrate->nubus_slot_irq(cfg, slot, active, umbrella_edge);
}

void nubus_assert_irq(nubus_card_t *card) {
    if (!card || !card->bus)
        return;
    nubus_bus_t *bus = card->bus;
    bool any_was_asserted = (bus->slot_irq_mask != 0);
    bus->slot_irq_mask |= (uint16_t)(1u << card->slot);
    nubus_route_slot_irq(bus->cfg, card->slot, /*active*/ true, /*umbrella_edge*/ !any_was_asserted);
}

void nubus_deassert_irq(nubus_card_t *card) {
    if (!card || !card->bus)
        return;
    nubus_bus_t *bus = card->bus;
    bus->slot_irq_mask &= (uint16_t) ~(1u << card->slot);
    nubus_route_slot_irq(bus->cfg, card->slot, /*active*/ false, /*umbrella_edge*/ bus->slot_irq_mask == 0);
}
