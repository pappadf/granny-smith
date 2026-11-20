// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// builtin_se30_video.c
// SE/30 built-in video as a NuBus card living in slot $E.  Implements the
// nubus_card_ops_t vtable plus the SE/30-specific hooks declared in
// builtin_se30_video.h.  See docs/reference/machines/glue/se30.md.
//
// What the card owns:
//   * 64 KB VRAM at $FEE00000
//   * 32 KB declaration ROM at $FEFF8000 (Apple's SE/30 onboard-video vROM
//     when one is offered — found by content via declrom_load_vrom_card —
//     else the emulator's substitute, the generated GS vROM), seated by the
//     bus before card_init (declrom_seat) and read here as card->declrom
//   * the display_t exposed via system_display() — 512×342×1bpp, with
//     `bits` toggled between primary and alternate VRAM offsets when
//     se30_via1_output reports a VIA1 PA6 change

#include "builtin_se30_video.h"
#include "card.h"
#include "checkpoint.h"
#include "declrom.h"
#include "display.h"
#include "gsvrom.h"

#include <stddef.h> // offsetof — the checkpoint range
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SE30_VRAM_SIZE           0x00010000UL // 64 KB
#define SE30_VROM_SIZE           0x00008000UL // 32 KB
#define SE30_FB_PRIMARY_OFFSET   0x8040
#define SE30_FB_ALTERNATE_OFFSET 0x0040

// Per-card private state.  Hangs off card->priv.
// Field order IS the checkpoint format (the via_t / adb_t / asc_t idiom): the
// scalar state goes as ONE range ending at `display`.  A scalar added above
// that line is checkpointed automatically; a POINTER added above it restores a
// stale address, so the pointers sit below the marker.
typedef struct {
    bool main_buf; // true: primary, false: alternate

    // --- Pointers last; NOT in the range above ---
    // `display` leads them because it embeds `bits`/`clut` pointers of its own;
    // its scalar head is checkpointed separately as a display_head_t.
    display_t display;
    uint8_t *vram;
} se30_priv_t;

// The layout above is load-bearing.  If this fires, a member moved across the
// boundary: re-check what the checkpoint range now covers before updating it.
_Static_assert(offsetof(se30_priv_t, display) < offsetof(se30_priv_t, vram),
               "SE/30 built-in video checkpoint range must end before the pointer block");

// === Static helpers =========================================================

// Cold video scans out black, not the white an all-zero 1 bpp buffer gives.
// Blanks BOTH rasters: VIA1 PA6 picks the buffer and may select the
// alternate before the OS draws, so blanking only the primary would still
// flash white.  Raster only — the rest of VRAM is guest-readable memory
// that is not pixels.
static void blank_both_rasters(se30_priv_t *p) {
    display_blank_raster(&p->display);
    const uint8_t *scanned = p->display.bits;
    // The other buffer of the two, whichever is being scanned
    bool primary = scanned == p->vram + SE30_FB_PRIMARY_OFFSET;
    p->display.bits = p->vram + (primary ? SE30_FB_ALTERNATE_OFFSET : SE30_FB_PRIMARY_OFFSET);
    display_blank_raster(&p->display);
    p->display.bits = scanned;
}

// === Card vtable ============================================================

static int card_init(nubus_card_t *card, config_t *cfg, checkpoint_t *cp, const slot_opts_t *opts) {
    (void)cp;
    se30_priv_t *p = calloc(1, sizeof(*p));
    if (!p)
        return -1;
    p->vram = calloc(1, SE30_VRAM_SIZE);
    if (!p->vram) {
        free(p);
        return -1;
    }
    (void)cfg;
    (void)opts;
    // The declaration ROM is card->declrom, seated by the bus (declrom_seat):
    // Apple's when it is offered, else -- or when that file has gone, which
    // the kind's declrom_falls_back turns into the substitute rather than no
    // video -- the generated one.

    // Populate the display descriptor.  Primary buffer at $8040 is the
    // boot-time selection — VIA1 PA6 will toggle it before the OS draws.
    p->main_buf = true;
    p->display.width = 512;
    p->display.height = 342;
    p->display.stride = 512 / 8;
    p->display.format = PIXEL_1BPP_MSB;
    p->display.bits = p->vram + SE30_FB_PRIMARY_OFFSET;
    blank_both_rasters(p);
    p->display.clut = NULL;
    p->display.clut_len = 0;
    p->display.shape_dirty = true;
    p->display.fb_dirty = true;

    card->priv = p;
    return 0;
}

// Power cycle: the slot-$E VRAM is lost, and the card comes up as a cold one
// does -- both rasters black -- rather than showing the last session's frame.
// Not the bus default, which blanks only the raster being scanned.
static void card_power_on(nubus_card_t *card, config_t *cfg) {
    (void)cfg;
    se30_priv_t *p = card->priv;
    if (!p)
        return;
    memset(p->vram, 0, SE30_VRAM_SIZE);
    blank_both_rasters(p);
    p->display.fb_dirty = true;
}

static void card_teardown(nubus_card_t *card, config_t *cfg) {
    (void)cfg;
    se30_priv_t *p = card->priv;
    if (!p)
        return;
    free(p->vram);
    free(p);
    card->priv = NULL;
}

static display_t *card_display(nubus_card_t *card) {
    se30_priv_t *p = card->priv;
    return p ? &p->display : NULL;
}

// The checkpointed plain-data prefix, up to the first host pointer
CHECKPOINT_PREFIX(se30_priv_t, display);

// === Checkpoint ==============================================================
//
// This card owns the SE/30's slot-$E VRAM, saved here.  Its declaration ROM
// is the bus's to carry: the card's block holds Apple's chip image, or
// nothing for the substitute, which card_init generates again.
static void card_checkpoint_save(nubus_card_t *card, checkpoint_t *cp) {
    se30_priv_t *p = card ? card->priv : NULL;
    if (!p)
        return;
    system_write_checkpoint_data(cp, p->vram, SE30_VRAM_SIZE);
    checkpoint_save_prefix(cp, p, se30_priv_t);
    {
        // Fixed widths, not a raw struct prefix: the prefix carried a bare
        // pixel_format_t, whose size is implementation-defined (see
        // display.h).
        display_head_t head = display_head_of(&p->display);
        system_write_checkpoint_data(cp, &head, sizeof head);
    }
}

static void card_checkpoint_restore(nubus_card_t *card, checkpoint_t *cp) {
    se30_priv_t *p = card ? card->priv : NULL;
    if (!p)
        return;
    system_read_checkpoint_data(cp, p->vram, SE30_VRAM_SIZE);
    checkpoint_load_prefix(cp, p, se30_priv_t);
    {
        display_head_t head;
        system_read_checkpoint_data(cp, &head, sizeof head);
        display_head_apply(&p->display, &head);
    }
    // Re-point the scanout at the restored buffer: the VIA redrive that
    // follows reaches select_buffer, which early-returns on an unchanged
    // main_buf and would leave init's primary-buffer pointer in place.
    p->display.bits = p->vram + (p->main_buf ? SE30_FB_PRIMARY_OFFSET : SE30_FB_ALTERNATE_OFFSET);

    p->display.shape_dirty = true;
    p->display.clut_dirty = true;
    p->display.fb_dirty = true;
    p->display.response_dirty = true;
}

static const nubus_card_ops_t builtin_se30_video_ops = {
    .init = card_init,
    .checkpoint_save = card_checkpoint_save,
    .checkpoint_restore = card_checkpoint_restore,
    .teardown = card_teardown,
    // No per-card VBL hook: the SE/30's built-in video has no slot
    // interrupt logic of its own.  The machine raises its pseudo-slot $E
    // VBL from its frame timer (se30_trigger_vbl in se30.c, the board's
    // trigger_vbl), which is where that wiring lives on the real board.
    .on_vbl = NULL,
    .power_on = card_power_on,
    .display = card_display,
};

// === Factory + kind descriptor ==============================================

// One monitor entry — the SE/30 built-in is fixed at 512×342×1bpp; the
// list is included so the dialog's monitor dropdown has *something* to
// show even though the user can't change it.
static const int builtin_se30_depths[] = {1, 0};
static const nubus_monitor_t builtin_se30_monitors[] = {
    {.id = "se30_internal",
     .monitor = "compact_9in",
     .width = 512,
     .height = 342,
     .depths = builtin_se30_depths,
     // The generated GS vROM's functional sResource id (gsvrom_generate
     // uses srsrc_sister as the spID; the assembled images used $80).
     .srsrc_sister = 0x80},
    {0},
};

const nubus_card_kind_t builtin_se30_video_kind = {
    .id = "builtin_se30_video",
    .display_name = "Built-in video",
    .attach = CARD_ATTACH_BUILTIN, // motherboard circuitry — never socketed
    // The SE/30 carries no video declaration ROM in main ROM; it needs a
    // separate onboard-video vROM file, and boots the emulator's substitute
    // without one.
    .requires_vrom = true,
    .substitute = true,
    .monitors = builtin_se30_monitors,
    .declrom_bus_size = SE30_VROM_SIZE,
    .gsvrom = GSVROM_SE30,
    .declrom_falls_back = true, // a board cannot lose its video: no ROM file runs the substitute
    .ops = &builtin_se30_video_ops,
};

// === SE/30-specific public hooks ============================================

void builtin_se30_video_select_buffer(nubus_card_t *card, bool main_buf) {
    if (!card)
        return;
    se30_priv_t *p = card->priv;
    if (!p || p->main_buf == main_buf)
        return;
    p->main_buf = main_buf;
    p->display.bits = p->vram + (main_buf ? SE30_FB_PRIMARY_OFFSET : SE30_FB_ALTERNATE_OFFSET);
    p->display.fb_dirty = true;
}

uint8_t *builtin_se30_video_vram(nubus_card_t *card) {
    se30_priv_t *p = card ? card->priv : NULL;
    return p ? p->vram : NULL;
}

uint8_t *builtin_se30_video_vrom(nubus_card_t *card) {
    return card ? card->declrom : NULL;
}
