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
//     else the emulator's substitute, the generated GS vROM)
//   * the display_t exposed via system_display() — 512×342×1bpp, with
//     `bits` toggled between primary and alternate VRAM offsets when
//     se30_via1_output reports a VIA1 PA6 change

#include "builtin_se30_video.h"
#include "card.h"
#include "checkpoint.h"
#include "declrom.h"
#include "display.h"
#include "gsvrom.h"
#include "log.h"
#include "system.h"
#include "system_config.h"

#include <stddef.h> // offsetof — the checkpoint range
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("video");

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
    uint8_t *vrom;
} se30_priv_t;

// The layout above is load-bearing.  If this fires, a member moved across the
// boundary: re-check what the checkpoint range now covers before updating it.
_Static_assert(offsetof(se30_priv_t, display) < offsetof(se30_priv_t, vram),
               "SE/30 built-in video checkpoint range must end before the pointer block");

// === Card vtable ============================================================

static int card_init(nubus_card_t *card, config_t *cfg, checkpoint_t *cp, const slot_opts_t *opts) {
    (void)cp;
    se30_priv_t *p = calloc(1, sizeof(*p));
    if (!p)
        return -1;
    p->vram = calloc(1, SE30_VRAM_SIZE);
    p->vrom = calloc(1, SE30_VROM_SIZE);
    if (!p->vram || !p->vrom) {
        free(p->vram);
        free(p->vrom);
        free(p);
        return -1;
    }
    (void)cfg;
    // Apple's ROM, which the bus seats only when it is offered.  A file that
    // went away, or a checkpoint whose ROM is not this card's, leaves the
    // SE/30 its substitute rather than no video: the machine around the card
    // stays whole, and a restore fails on the checkpoint's error.
    bool substitute = opts->substitute;
    if (!substitute && !declrom_load_vrom_card(card, builtin_se30_video_kind.id, opts->rom[0] ? opts->rom : NULL,
                                               p->vrom, SE30_VROM_SIZE, NULL)) {
        LOG(0, "SE/30 video: the onboard-video declaration ROM could not be loaded; running the substitute");
        if (cp)
            checkpoint_set_error(cp);
        substitute = card->substitute = true;
    }
    if (substitute) {
        // The substitute ROM: the GS declaration ROM generated here -- a full
        // declaration ROM with a real display driver, records from
        // builtin_se30_monitors[], code fragments spliced, CRC stamped in C
        // (docs/internals/core/peripherals/nubus_generic_vrom.md).  The offer
        // registry is never consulted.
        declrom_builder_t *bld = gsvrom_generate(GSVROM_SE30, builtin_se30_video_kind.monitors);
        size_t img_size = 0;
        const uint8_t *img = bld ? declrom_builder_bytes(bld, &img_size) : NULL;
        if (!img || !declrom_install_builtin(card, builtin_se30_video_kind.id, img, img_size, p->vrom, SE30_VROM_SIZE))
            LOG(0, "SE/30 video: the substitute declaration ROM failed to generate; declaration ROM is zero-filled");
        declrom_builder_free(bld);
    }

    // Publish it, the way jmfb.c / 24ac.c / 824gc.c all do.  Without this the
    // object model reported `slot[$E].card.declrom.present == false` on an
    // SE/30 that has a perfectly good 32 KB declaration ROM.
    // nubus_delete owns and frees the buffer once it is published here.
    card->declrom = p->vrom;
    card->declrom_size = SE30_VROM_SIZE;

    // Populate the display descriptor.  Primary buffer at $8040 is the
    // boot-time selection — VIA1 PA6 will toggle it before the OS draws.
    p->main_buf = true;
    p->display.width = 512;
    p->display.height = 342;
    p->display.stride = 512 / 8;
    p->display.format = PIXEL_1BPP_MSB;
    p->display.bits = p->vram + SE30_FB_PRIMARY_OFFSET;
    // Cold boot scans out black, not the white an all-zero 1 bpp buffer gives.
    // Blank BOTH rasters: VIA1 PA6 picks the buffer and may select the
    // alternate before the OS draws, so blanking only the primary would still
    // flash white.  Raster only — the rest of VRAM is guest-readable memory
    // that is not pixels.
    display_blank_raster(&p->display);
    const uint8_t *primary = p->display.bits;
    p->display.bits = p->vram + SE30_FB_ALTERNATE_OFFSET;
    display_blank_raster(&p->display);
    p->display.bits = primary;
    p->display.clut = NULL;
    p->display.clut_len = 0;
    p->display.shape_dirty = true;
    p->display.fb_dirty = true;

    card->priv = p;
    return 0;
}

static void card_teardown(nubus_card_t *card, config_t *cfg) {
    (void)cfg;
    se30_priv_t *p = card->priv;
    if (!p)
        return;
    free(p->vram);
    // p->vrom is published as card->declrom; nubus_delete owns and frees it.
    free(p);
    card->priv = NULL;
}

static display_t *card_display(nubus_card_t *card) {
    se30_priv_t *p = card->priv;
    return p ? &p->display : NULL;
}

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
    system_write_checkpoint_data(cp, p, offsetof(se30_priv_t, display));
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
    system_read_checkpoint_data(cp, p, offsetof(se30_priv_t, display));
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
    .on_vbl = NULL, // VBL slot-IRQ flow stays in se30_trigger_vbl for v1
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
    se30_priv_t *p = card ? card->priv : NULL;
    return p ? p->vrom : NULL;
}
