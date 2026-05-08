// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// card.h
// NuBus card abstraction shared by every card driver in cards/.  See
// proposal-machine-iicx-iix.md §3.2.1 for the full design.  Step-3 status:
// types and registry accessors exist; the registry is empty until the
// SE/30 built-in card moves out in step 4.

#ifndef NUBUS_CARD_H
#define NUBUS_CARD_H

#include "common.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct config;
struct checkpoint;
struct display;
typedef struct config config_t;
typedef struct checkpoint checkpoint_t;
typedef struct display display_t;

struct nubus_bus;
struct nubus_card;
typedef struct nubus_bus nubus_bus_t;
typedef struct nubus_card nubus_card_t;

// Per-card vtable.  Cards implement whichever entry-points they need;
// NULL hooks are safe and skipped by the bus controller.
typedef struct nubus_card_ops {
    // Called once during machine init.  Returns 0 on success.  The card
    // may allocate VRAM, register host-backed regions on the bus map, and
    // populate any internal state.
    int (*init)(nubus_card_t *card, config_t *cfg, checkpoint_t *cp);

    // Called during machine teardown, in inverse init order.
    void (*teardown)(nubus_card_t *card, config_t *cfg);

    // Called from the family VBL trigger (via nubus_tick_vbl) once per
    // VBL.  Cards that drive their own VSync IRQ call nubus_assert_irq()
    // here.
    void (*on_vbl)(nubus_card_t *card, config_t *cfg);

    // Optional: present this card's framebuffer as the primary display
    // surface.  Returns NULL if the card has no display.  The pointer is
    // stable across the card's lifetime; the descriptor's contents are
    // live-mutable (see display.h for the generation-bump contract).
    const display_t *(*display)(nubus_card_t *card);

    // Checkpoint hooks; the bus controller calls these for every populated
    // slot in canonical slot order.
    void (*checkpoint_save)(nubus_card_t *card, checkpoint_t *cp);
    void (*checkpoint_restore)(nubus_card_t *card, checkpoint_t *cp);

    // Card-name introspection — used by the config dialog and the
    // `machine.slot.<n>.card` object-model surface.
    const char *(*name)(const nubus_card_t *card);
} nubus_card_ops_t;

// Concrete card instance.  Per-card private state hangs off `.private`;
// the bus controller never reads it.  `declrom` / `declrom_size` are
// optional — a card without a declaration ROM (e.g. a hypothetical SCSI
// expansion card) leaves them NULL/0.
struct nubus_card {
    const nubus_card_ops_t *ops;
    int slot; // $9..$E
    nubus_bus_t *bus; // owning bus (back-pointer)
    void *priv; // card-private state (named "priv"
                // because `private` is a C++ keyword
                // that snags some tooling).
    uint8_t *declrom; // 8 KB or larger declaration ROM
    size_t declrom_size;
};

// Per-card constructor signature.  The bus controller calls this once per
// populated slot during nubus_init().  Returns the new card on success,
// NULL on failure.  The bus takes ownership and calls ops->teardown()
// during nubus_delete.
typedef nubus_card_t *(*nubus_card_factory_fn)(int slot, config_t *cfg, checkpoint_t *cp);

// One monitor a card advertises (resolution + supported depths).  Used
// by the card-kind registry so the dialog can populate a monitor / depth
// dropdown without knowing about the card driver.  Sentinel-terminated
// arrays end at the entry whose `id` is NULL.
typedef struct nubus_monitor {
    const char *id; // "13in_rgb"
    const char *name; // "13\" AppleColor"
    uint32_t width; // pixels
    uint32_t height; // pixels
    const int *depths; // 0-terminated array of supported bpp values
} nubus_monitor_t;

// Per-card driver descriptor — one static instance per registered driver.
// The dialog reads this via nubus.cards(); the bus controller reads it
// via nubus_card_find() to resolve a card id to a factory.
typedef struct nubus_card_kind {
    const char *id; // "mdc_8_24"
    const char *display_name; // "Apple Macintosh Display Card 8•6 / 8•24"
    bool requires_vrom; // dialog shows VROM picker iff true
    const nubus_monitor_t *monitors; // sentinel-terminated; NULL for non-display cards
    nubus_card_factory_fn factory; // bus controller calls this once per populated slot
} nubus_card_kind_t;

// Registry accessors.  The registry itself is an explicit list in
// nubus.c (see proposal §3.2.1 "explicit list, no linker constructors").
// Returns NULL / empty array until step 4 lands the first card.
const nubus_card_kind_t *nubus_card_find(const char *id);
const nubus_card_kind_t *const *nubus_card_registry(void);

#endif // NUBUS_CARD_H
