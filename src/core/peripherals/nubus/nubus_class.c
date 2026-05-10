// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// nubus_class.c
// `nubus.*` object-model surface.  Step-3 status: just the `nubus.cards()`
// method, which walks nubus_card_registry() and returns a list of card-id
// strings — empty until step 4 lands the first card.  The richer surface
// described in proposal §3.5.3 (slot.<n>/ children, .primary, per-card
// width/height/format/decl_rom_path) lands once cfg->nubus is non-NULL,
// which only happens after step 4 wires up nubus_init from glue030_init.

#include "card.h"
#include "jmfb.h"
#include "object.h"
#include "value.h"

#include <stddef.h>
#include <stdlib.h>

// `nubus.cards()` — list every registered card-driver id.  Returns
// V_LIST<V_STRING>.  Used by the config dialog in step 5 to populate
// the per-slot card-type dropdown without baking the list into the JS.
static value_t nubus_method_cards(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    const nubus_card_kind_t *const *reg = nubus_card_registry();
    size_t n = 0;
    for (const nubus_card_kind_t *const *p = reg; *p; p++)
        n++;
    if (n == 0)
        return val_list(NULL, 0);
    value_t *items = (value_t *)calloc(n, sizeof(value_t));
    if (!items)
        return val_err("nubus.cards: out of memory");
    size_t i = 0;
    for (const nubus_card_kind_t *const *p = reg; *p; p++)
        items[i++] = val_str((*p)->id);
    return val_list(items, n);
}

// `nubus.video_sense` — get/set the JMFB-card monitor sense for the
// next machine.boot.  Reads return the current pending value (which
// the JMFB factory will consume on its next instantiation); writes
// stage the new value.  Valid raw-sense codes are 0..6 per
// JMFBPrimaryInit.a's sense table (0=21" RGB / 1=15" Mono /
// 2=12" RGB / 3=21" B&W / 4=NTSC / 5=15" RGB / 6=13" RGB / 7=no
// connect).  Settable from integration test scripts via
// `nubus.video_sense = N` before `machine.boot`.
static value_t nubus_attr_video_sense_get(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    value_t v = val_uint(1, jmfb_pending_sense_get());
    v.flags |= VAL_HEX;
    return v;
}

static value_t nubus_attr_video_sense_set(struct object *self, const member_t *m, value_t in) {
    (void)self;
    (void)m;
    if (in.kind != V_UINT) {
        value_free(&in);
        return val_err("nubus.video_sense: expected unsigned integer 0..7");
    }
    if (in.u > 7) {
        value_free(&in);
        return val_err("nubus.video_sense: value must be 0..7 (got %llu)", (unsigned long long)in.u);
    }
    jmfb_pending_sense_set((uint8_t)in.u);
    value_free(&in);
    return val_none();
}

// `nubus.video_mode` — get/set a pending high-level video-mode id
// for the next machine.boot.  The id matches one of the entries in
// `machine.profile(...).video_modes[].id` (e.g. "13in_rgb_8bpp").
// When the JMFB factory consumes the pending id it resolves it to a
// (monitor, depth) tuple, overrides nubus.video_sense to the
// monitor's sense_code, and writes the boot-ROM PRAM validity tokens
// plus the slot-PRAMRec bytes so the Slot Manager's GET_SLOT_DEPTH
// lands on the requested mode.  Reading returns the pending id (or
// the empty string when none is set); writing "" clears it.
static value_t nubus_attr_video_mode_get(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    const char *id = jmfb_pending_video_mode_get();
    return val_str(id ? id : "");
}

static value_t nubus_attr_video_mode_set(struct object *self, const member_t *m, value_t in) {
    (void)self;
    (void)m;
    if (in.kind != V_STRING) {
        value_free(&in);
        return val_err("nubus.video_mode: expected string id (e.g. \"13in_rgb_8bpp\")");
    }
    const char *id = in.s ? in.s : "";
    // Validate against the catalog so the user gets immediate
    // feedback if they pass a typo instead of waiting until the next
    // machine.boot to discover the id didn't match.  Empty string is
    // permitted and clears any pending selection.
    if (*id && !jmfb_video_mode_lookup(id, NULL, NULL)) {
        value_t err = val_err("nubus.video_mode: unknown video-mode id '%s'", id);
        value_free(&in);
        return err;
    }
    jmfb_pending_video_mode_set(id);
    value_free(&in);
    return val_none();
}

static const member_t nubus_members[] = {
    {.kind = M_METHOD,
     .name = "cards",
     .doc = "List the ids of all registered NuBus card drivers",
     .method = {.args = NULL, .nargs = 0, .result = V_LIST, .fn = nubus_method_cards}},
    {.kind = M_ATTR,
     .name = "video_sense",
     .doc = "Pending JMFB monitor sense (0..6, 7 = no connect)",
     .flags = 0,
     .attr = {.type = V_UINT, .get = nubus_attr_video_sense_get, .set = nubus_attr_video_sense_set}},
    {.kind = M_ATTR,
     .name = "video_mode",
     .doc = "Pending video-mode id for next machine.boot (matches profile.video_modes[].id)",
     .flags = 0,
     .attr = {.type = V_STRING, .get = nubus_attr_video_mode_get, .set = nubus_attr_video_mode_set}},
};

const class_desc_t nubus_class = {
    .name = "nubus",
    .members = nubus_members,
    .n_members = sizeof(nubus_members) / sizeof(nubus_members[0]),
};
