// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// RBV built-in video: VDAC (Bt450) palette-port tests.
//
// Builds the card through its kind's init hook (with the bus queries it
// makes stubbed out below) and drives the VDAC registers exactly as mdu's
// register window does.

#include "builtin_rbv_video.h"
#include "card.h"
#include "memory.h"
#include "nubus.h"
#include "rbv.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// VDAC register offsets (builtin_rbv_video.h)
#define WADDR 0x0
#define WDATA 0x4
#define RADDR 0xC

// === Bus stubs ===============================================================

// The card asks the bus which kind is seated, to decode its monitor.
const nubus_card_kind_t *nubus_slot_kind(nubus_bus_t *bus, int slot) {
    (void)bus;
    (void)slot;
    return &builtin_rbv_video_kind;
}

// No slot declaration: the framebuffer stays the card's own.
const nubus_slot_decl_t *nubus_slot_decl_get(nubus_bus_t *bus, int slot) {
    (void)bus;
    (void)slot;
    return NULL;
}

// Never reached without a slot declaration.
uint8_t *ram_native_pointer(memory_map_t *ram, uint32_t addr) {
    (void)ram;
    (void)addr;
    return NULL;
}

// The VBL hook's interrupt; no test drives a VBL.
void rbv_assert_slot_irq(rbv_t *rbv, int slot) {
    (void)rbv;
    (void)slot;
}

// === Helpers =================================================================

// A seated card with the 13" RGB strapped on its sense lines.
static nubus_card_t make_card(void) {
    nubus_card_t card;
    memset(&card, 0, sizeof card);
    card.ops = builtin_rbv_video_kind.ops;
    slot_opts_t opts;
    memset(&opts, 0, sizeof opts);
    opts.sense = 6;
    ASSERT_EQ_INT(0, card.ops->init(&card, NULL, NULL, &opts));
    return card;
}

// Writes one run of `n` components starting at entry `idx`.
static void load(nubus_card_t *card, uint8_t idx, const uint8_t *v, int n) {
    builtin_rbv_video_vdac_write(card, WADDR, idx);
    for (int i = 0; i < n; i++)
        builtin_rbv_video_vdac_write(card, WDATA, v[i]);
}

// === Tests ===================================================================

// A run-read walks consecutive entries, advancing after each blue, as a
// run-write does.  The read port used to step only the phase, returning the
// first entry over and over.
TEST(vdac_run_read_advances) {
    nubus_card_t card = make_card();
    static const uint8_t rgb[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    load(&card, 5, rgb, 6);
    builtin_rbv_video_vdac_write(&card, RADDR, 5);
    for (int i = 0; i < 6; i++) {
        int got = builtin_rbv_video_vdac_read(&card, WDATA); // read once: the read steps the port
        ASSERT_EQ_INT(rgb[i], got);
    }
    ASSERT_EQ_INT(7, builtin_rbv_video_vdac_read(&card, RADDR)); // one past the run
    card.ops->teardown(&card, NULL);
}

// A peek reports the component the next read returns and steps nothing.
TEST(vdac_peek_is_side_effect_free) {
    nubus_card_t card = make_card();
    static const uint8_t rgb[3] = {0xA0, 0xB0, 0xC0};
    load(&card, 0, rgb, 3);
    builtin_rbv_video_vdac_write(&card, RADDR, 0);
    ASSERT_EQ_INT(0xA0, builtin_rbv_video_vdac_peek(&card, WDATA));
    ASSERT_EQ_INT(0xA0, builtin_rbv_video_vdac_peek(&card, WDATA));
    ASSERT_EQ_INT(0xA0, builtin_rbv_video_vdac_read(&card, WDATA));
    ASSERT_EQ_INT(0xB0, builtin_rbv_video_vdac_peek(&card, WDATA));
    card.ops->teardown(&card, NULL);
}

// A partial triplet is held, not stored: an address write drops it.
TEST(vdac_partial_triplet_not_committed) {
    nubus_card_t card = make_card();
    static const uint8_t rgb[3] = {0x10, 0x20, 0x30};
    load(&card, 9, rgb, 3);
    builtin_rbv_video_vdac_write(&card, WADDR, 9);
    builtin_rbv_video_vdac_write(&card, WDATA, 0x77); // R only
    builtin_rbv_video_vdac_write(&card, RADDR, 9);
    for (int i = 0; i < 3; i++) {
        int got = builtin_rbv_video_vdac_read(&card, WDATA);
        ASSERT_EQ_INT(rgb[i], got);
    }
    card.ops->teardown(&card, NULL);
}

int main(void) {
    RUN(vdac_run_read_advances);
    RUN(vdac_peek_is_side_effect_free);
    RUN(vdac_partial_triplet_not_committed);
    fprintf(stderr, "rbv_vdac: all tests passed\n");
    return 0;
}
