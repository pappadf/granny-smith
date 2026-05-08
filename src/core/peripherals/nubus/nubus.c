// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// nubus.c
// NuBus subsystem skeleton.  Step-3 status (see proposal §4 step 3): the
// types and public API are wired up; the bus controller body is a
// minimal skeleton — no machine creates a bus yet.  The card-kind
// registry is empty until step 4 lands the first card.

#include "nubus.h"
#include "card.h"
#include "system_config.h" // full config_t for VIA pointer access
#include "via.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

static const nubus_card_kind_t *const g_card_registry[] = {
    &builtin_se30_video_kind,
    &mdc_8_24_kind,
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
                // Step 3 falls back to the slot's default; step 5 adds the
                // video.* pending-card lookup that lets the dialog pick.
                kind = nubus_card_find(s->default_card);
                break;
            case NUBUS_SLOT_ABSENT:
            case NUBUS_SLOT_EMPTY:
                continue;
            }
            if (!kind || !kind->factory)
                continue;
            nubus_card_t *card = kind->factory(s->slot, cfg, cp);
            if (!card)
                continue;
            card->bus = bus;
            card->slot = s->slot;
            if (s->slot >= 0 && s->slot < NUBUS_MAX_SLOTS)
                bus->cards[s->slot] = card;
        }
    }
    return bus;
}

void nubus_delete(nubus_bus_t *bus) {
    if (!bus)
        return;
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

const display_t *nubus_primary_display(nubus_bus_t *bus) {
    if (!bus)
        return NULL;
    // First slot in numerical order whose ops->display() returns non-NULL.
    for (int i = 0; i < NUBUS_MAX_SLOTS; i++) {
        nubus_card_t *card = bus->cards[i];
        if (!card || !card->ops || !card->ops->display)
            continue;
        const display_t *d = card->ops->display(card);
        if (d)
            return d;
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

void nubus_assert_irq(nubus_card_t *card) {
    if (!card || !card->bus)
        return;
    nubus_bus_t *bus = card->bus;
    bool any_was_asserted = (bus->slot_irq_mask != 0);
    bus->slot_irq_mask |= (uint16_t)(1u << card->slot);

    via_t *via2 = bus->cfg ? bus->cfg->via2 : NULL;
    if (!via2)
        return;
    int pa_bit = card->slot - 0x9;
    if (pa_bit >= 0 && pa_bit <= 5)
        via_input(via2, /*port A*/ 0, pa_bit, /*active-low*/ 0);

    if (!any_was_asserted)
        via_input_c(via2, /*CA1*/ 0, /*pin*/ 0, /*falling*/ 0);
}

void nubus_deassert_irq(nubus_card_t *card) {
    if (!card || !card->bus)
        return;
    nubus_bus_t *bus = card->bus;
    bus->slot_irq_mask &= (uint16_t) ~(1u << card->slot);

    via_t *via2 = bus->cfg ? bus->cfg->via2 : NULL;
    if (!via2)
        return;
    int pa_bit = card->slot - 0x9;
    if (pa_bit >= 0 && pa_bit <= 5)
        via_input(via2, 0, pa_bit, 1);

    if (bus->slot_irq_mask == 0)
        via_input_c(via2, 0, 0, 1);
}
