// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// nubus.c
// NuBus subsystem skeleton.  Step-3 status (see proposal §4 step 3): the
// types and public API are wired up; the bus controller body is a
// minimal skeleton — no machine creates a bus yet.  The card-kind
// registry is empty until step 4 lands the first card.

#include "nubus.h"
#include "card.h"
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
    nubus_card_t *cards[NUBUS_MAX_SLOTS]; // cards[$9..$E]; NULL elsewhere
    uint16_t slot_irq_mask; // bitmap; bit $9 .. bit $E
};

// === Card-kind registry =====================================================
//
// Single explicit list (no linker-section magic).  Adding a card driver
// is one extern + one entry.

extern const nubus_card_kind_t builtin_se30_video_kind; // cards/builtin_se30_video.c
extern const nubus_card_kind_t mdc_8_24_kind; // cards/jmfb.c
extern const nubus_card_kind_t builtin_rbv_video_kind; // cards/builtin_rbv_video.c
extern const nubus_card_kind_t radius_24ac_kind; // cards/radius24ac.c

static const nubus_card_kind_t *const g_card_registry[] = {
    &builtin_se30_video_kind, &mdc_8_24_kind, &builtin_rbv_video_kind, &radius_24ac_kind, NULL,
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

// === Pending video-card selection ===========================================
//
// The VIDEO slot defaults to its declared default_card; this pending slot
// lets the config dialog / a test override the pick for the next boot
// without a machine-framework change.  At most 31 chars + NUL fits any
// card id ("radius_24ac" etc.).
static char s_pending_video_card[32] = "";

void nubus_pending_video_card_set(const char *id) {
    if (!id || !*id) {
        s_pending_video_card[0] = '\0';
        return;
    }
    snprintf(s_pending_video_card, sizeof s_pending_video_card, "%s", id);
}

const char *nubus_pending_video_card_get(void) {
    return s_pending_video_card[0] ? s_pending_video_card : NULL;
}

// Resolve the VIDEO slot's card id: honour the pending selection iff it is
// listed in the slot's available_cards, else fall back to default_card.
static const char *video_slot_card_id(const nubus_slot_decl_t *s) {
    const char *pending = nubus_pending_video_card_get();
    if (pending && s->available_cards) {
        for (const char *const *c = s->available_cards; *c; c++) {
            if (strcmp(*c, pending) == 0)
                return pending;
        }
        LOG(1, "nubus: pending video_card '%s' not in slot $%X available_cards; using default '%s'", pending, s->slot,
            s->default_card ? s->default_card : "(none)");
    }
    return s->default_card;
}

// === Bus controller =========================================================

nubus_bus_t *nubus_init(config_t *cfg, const nubus_slot_decl_t *slots, checkpoint_t *cp) {
    if (!cfg)
        return NULL;
    nubus_bus_t *bus = calloc(1, sizeof(*bus));
    if (!bus)
        return NULL;
    bus->cfg = cfg;

    // Walk the slot table.  Per proposal §3.2.2: BUILTIN slots resolve
    // their card via nubus_card_find(.builtin_card_id); VIDEO slots read
    // the user's pick from the video.* pending statics (added later) and
    // resolve through the same path.  Step 3 leaves both branches as
    // skeletons because the registry is empty.
    if (slots) {
        for (const nubus_slot_decl_t *s = slots; s->slot != 0; s++) {
            const nubus_card_kind_t *kind = NULL;
            switch (s->kind) {
            case NUBUS_SLOT_BUILTIN:
                kind = nubus_card_find(s->builtin_card_id);
                break;
            case NUBUS_SLOT_VIDEO:
                // Honour a pending `nubus.video_card` pick if it is one of
                // this slot's available_cards; otherwise the slot default.
                kind = nubus_card_find(video_slot_card_id(s));
                break;
            case NUBUS_SLOT_ABSENT:
            case NUBUS_SLOT_EMPTY:
                continue;
            }
            if (!kind || !kind->factory)
                continue;
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
    // Consume the pending video-card pick so a stale selection doesn't
    // leak into the next machine.boot (mirrors jmfb's pending-sense reset).
    nubus_pending_video_card_set(NULL);
    // Project the populated slots into the object model (proposal §3.8):
    // machine.nubus.slot[N].card.{framebuffer,declrom,clut,mode,…}.
    nubus_objects_build(bus);
    return bus;
}

void nubus_delete(nubus_bus_t *bus) {
    if (!bus)
        return;
    // Drop the object-model node trees before the cards they read go away.
    nubus_objects_teardown();
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
