// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// builtin_rbv_video.c
// Macintosh IIci built-in video pseudo-card.  See builtin_rbv_video.h for
// the contract and proposal-machine-iici-iisi.md §3.4 for the RBV/video
// split.  Modelled on jmfb.c (CLUT + depth-switch video) but much smaller:
// the depth/monitor-sense register lives on the RBV chip, there is no slot
// register window, and the framebuffer is registered by the machine at the
// $FBB00000 aperture rather than at nubus_slot_base(slot).

#include "builtin_rbv_video.h"

#include "card.h"
#include "display.h"
#include "log.h"
#include "nubus.h"
#include "rbv.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("rbvvid");

// Built-in 13" RGB panel: 640×480, depths 1/2/4/8 bpp.
#define RBV_VIDEO_WIDTH  640
#define RBV_VIDEO_HEIGHT 480

// === Per-card private state =================================================

typedef struct {
    rbv_t *rbv; // RBV chip — set post-init by the machine (slot-0 IRQ)
    uint8_t *fb; // framebuffer buffer (registered by the machine at $FBB00000)
    bool fb_external; // true if fb points at machine-owned memory (don't free)
    rgba8_t clut[256]; // 256-entry palette fed by the VDAC
    display_t display;

    // VDAC (Bt450) write state: an address write resets the R/G/B counter;
    // three data writes load one entry, then the index auto-increments.
    uint8_t vdac_idx; // current CLUT write index
    uint8_t vdac_phase; // 0 = R, 1 = G, 2 = B
    uint8_t vdac_rgb[3]; // accumulated R/G/B for the in-progress entry
    uint8_t vdac_pix_mask; // pixel read mask (accept-and-log)
} rbv_video_priv_t;

// VDAC register offsets (RBV's Bt450) — see HardwarePrivateEqu.a:927-931.
#define VDAC_WADDR 0x0 // vDACwAddReg — set CLUT index
#define VDAC_WDATA 0x4 // vDACwDataReg — R/G/B sequential
#define VDAC_PIXRD 0x8 // vDACPixRdMask
#define VDAC_RADDR 0xC // vDACrAddReg

// Map a depth code (RvMonP bits 0-2) to a packed pixel format.
static pixel_format_t depth_to_format(int depth_code) {
    switch (depth_code) {
    case 0:
        return PIXEL_1BPP_MSB;
    case 1:
        return PIXEL_2BPP_MSB;
    case 2:
        return PIXEL_4BPP_MSB;
    case 3:
    default:
        return PIXEL_8BPP;
    }
}

static uint32_t format_bpp(pixel_format_t f) {
    switch (f) {
    case PIXEL_1BPP_MSB:
        return 1;
    case PIXEL_2BPP_MSB:
        return 2;
    case PIXEL_4BPP_MSB:
        return 4;
    case PIXEL_8BPP:
    default:
        return 8;
    }
}

// === Card vtable ============================================================

static int card_init(nubus_card_t *card, config_t *cfg, checkpoint_t *cp) {
    (void)cfg;
    (void)cp;
    rbv_video_priv_t *p = calloc(1, sizeof(*p));
    if (!p)
        return -1;
    p->fb = calloc(1, BUILTIN_RBV_VRAM_SIZE);
    if (!p->fb) {
        free(p);
        return -1;
    }

    // Power-up state: 1 bpp, matching the RBV's RvMonP depth default.  The
    // boot ROM grays the screen at this depth before the OS picks 8 bpp.
    p->display.width = RBV_VIDEO_WIDTH;
    p->display.height = RBV_VIDEO_HEIGHT;
    p->display.format = PIXEL_1BPP_MSB;
    p->display.stride = RBV_VIDEO_WIDTH / 8; // 80 bytes/row at 1 bpp
    p->display.bits = p->fb + BUILTIN_RBV_SCREEN_OFFSET;
    p->display.clut = p->clut;
    p->display.clut_len = 256;
    p->display.crt_response = NULL; // 13" RGB gamma is near-identity
    p->display.shape_dirty = true;
    p->display.clut_dirty = true;
    p->display.fb_dirty = true;
    p->display.response_dirty = true;

    // Seed a grayscale ramp so the canvas isn't blank before the OS programs
    // a palette via the VDAC (mirrors jmfb's init ramp).
    for (int i = 0; i < 256; i++) {
        p->clut[i].r = (uint8_t)i;
        p->clut[i].g = (uint8_t)i;
        p->clut[i].b = (uint8_t)i;
        p->clut[i].a = 255;
    }

    card->priv = p;
    return 0;
}

static void card_teardown(nubus_card_t *card, config_t *cfg) {
    (void)cfg;
    rbv_video_priv_t *p = card->priv;
    if (!p)
        return;
    if (!p->fb_external)
        free(p->fb); // machine-owned (IIsi main-RAM) framebuffers are not ours to free
    free(p);
    card->priv = NULL;
}

static void card_on_vbl(nubus_card_t *card, config_t *cfg) {
    (void)cfg;
    rbv_video_priv_t *p = card->priv;
    if (!p)
        return;
    // Assert the built-in-video vertical-blanking interrupt (RvIRQ0 = slot 0).
    // The boot ROM polls RvSInt bit 6 for this during video init, and the OS
    // VBL manager runs off it.  RBV clears the bit on RvSInt read, so each
    // VBL surfaces as a single pulse.
    if (p->rbv)
        rbv_assert_slot_irq(p->rbv, 0);
    // Mark the framebuffer dirty every VBL so the renderer re-uploads ongoing
    // Mac OS drawing (CPU writes to the framebuffer bypass the renderer).
    p->display.fb_dirty = true;
}

static display_t *card_display(nubus_card_t *card) {
    rbv_video_priv_t *p = card->priv;
    return p ? &p->display : NULL;
}

static const char *card_name(const nubus_card_t *card) {
    (void)card;
    return "Macintosh IIci Built-in Video";
}

static const nubus_card_ops_t builtin_rbv_video_ops = {
    .init = card_init,
    .teardown = card_teardown,
    .on_vbl = card_on_vbl,
    .display = card_display,
    .name = card_name,
};

// === Machine-facing hooks ===================================================

uint8_t *builtin_rbv_video_framebuffer(nubus_card_t *card) {
    rbv_video_priv_t *p = card ? card->priv : NULL;
    return p ? p->fb : NULL;
}

void builtin_rbv_video_set_framebuffer(nubus_card_t *card, uint8_t *aperture, uint32_t screen_offset) {
    rbv_video_priv_t *p = card ? card->priv : NULL;
    if (!p || !aperture)
        return;
    // The IIsi reads its framebuffer directly out of main DRAM (the V8 DMAs
    // Bank A starting at physical 0).  Point the card's framebuffer at the
    // machine-supplied aperture (a window into main RAM) instead of the private
    // buffer, so the renderer and the guest's screen writes share the same
    // storage.  `screen_offset` locates the active screen within the aperture.
    if (!p->fb_external)
        free(p->fb);
    p->fb = aperture;
    p->fb_external = true;
    p->display.bits = p->fb + screen_offset;
    p->display.fb_dirty = true;
}

void builtin_rbv_video_set_rbv(nubus_card_t *card, rbv_t *rbv) {
    rbv_video_priv_t *p = card ? card->priv : NULL;
    if (p)
        p->rbv = rbv;
}

void builtin_rbv_video_set_depth(nubus_card_t *card, int depth_code) {
    rbv_video_priv_t *p = card ? card->priv : NULL;
    if (!p)
        return;
    pixel_format_t f = depth_to_format(depth_code);
    if (p->display.format == f)
        return;
    p->display.format = f;
    p->display.stride = RBV_VIDEO_WIDTH * format_bpp(f) / 8u;
    p->display.shape_dirty = true;
    p->display.fb_dirty = true;
    LOG(2, "depth -> %u bpp (stride %u)", format_bpp(f), p->display.stride);
}

void builtin_rbv_video_vdac_write(nubus_card_t *card, uint32_t off, uint8_t val) {
    rbv_video_priv_t *p = card ? card->priv : NULL;
    if (!p)
        return;
    switch (off & 0xF) {
    case VDAC_WADDR:
        // Set the CLUT write index; reset the R/G/B sub-counter.
        p->vdac_idx = val;
        p->vdac_phase = 0;
        return;
    case VDAC_WDATA:
        // Accumulate R, then G, then B; commit the entry on the third write
        // and auto-increment the index (Bt450 run-write protocol).
        if (p->vdac_phase < 3)
            p->vdac_rgb[p->vdac_phase] = val;
        p->vdac_phase++;
        if (p->vdac_phase >= 3) {
            p->clut[p->vdac_idx].r = p->vdac_rgb[0];
            p->clut[p->vdac_idx].g = p->vdac_rgb[1];
            p->clut[p->vdac_idx].b = p->vdac_rgb[2];
            p->clut[p->vdac_idx].a = 255;
            p->vdac_idx++;
            p->vdac_phase = 0;
            p->display.clut_dirty = true;
        }
        return;
    case VDAC_PIXRD:
        p->vdac_pix_mask = val;
        return;
    case VDAC_RADDR:
        // Read-address register write: resets the read sub-counter (we don't
        // model CLUT readback beyond the address latch).
        p->vdac_idx = val;
        p->vdac_phase = 0;
        return;
    default:
        LOG(2, "VDAC write at +%X = $%02X (unmodeled)", off, val);
        return;
    }
}

uint8_t builtin_rbv_video_vdac_read(nubus_card_t *card, uint32_t off) {
    rbv_video_priv_t *p = card ? card->priv : NULL;
    if (!p)
        return 0xFF;
    switch (off & 0xF) {
    case VDAC_PIXRD:
        return p->vdac_pix_mask;
    case VDAC_WADDR:
    case VDAC_RADDR:
        return p->vdac_idx;
    case VDAC_WDATA: {
        // Return the current entry's components in R/G/B sequence.
        uint8_t v = (p->vdac_phase < 3) ? ((const uint8_t *)&p->clut[p->vdac_idx])[p->vdac_phase] : 0;
        p->vdac_phase = (uint8_t)((p->vdac_phase + 1) % 3);
        return v;
    }
    default:
        return 0;
    }
}

// === Factory + kind descriptor ==============================================

static nubus_card_t *factory(int slot, config_t *cfg, checkpoint_t *cp) {
    nubus_card_t *card = calloc(1, sizeof(*card));
    if (!card)
        return NULL;
    card->ops = &builtin_rbv_video_ops;
    card->slot = slot;
    if (card->ops->init(card, cfg, cp) != 0) {
        free(card);
        return NULL;
    }
    return card;
}

// Built-in monitor: 13" RGB, sense 6, depths 1/2/4/8 — for machine.profile.
static const int builtin_rbv_depths[] = {1, 2, 4, 8, 0};

static const nubus_monitor_t builtin_rbv_monitors[] = {
    {.id = "13in_rgb",
     .name = "13\" AppleColor RGB",
     .width = RBV_VIDEO_WIDTH,
     .height = RBV_VIDEO_HEIGHT,
     .depths = builtin_rbv_depths,
     .sense_code = 6,
     .srsrc_sister = 0,
     .crt_response = NULL},
    {0},
};

const nubus_card_kind_t builtin_rbv_video_kind = {
    .id = "builtin_rbv_video",
    .display_name = "Macintosh IIci Built-in Video",
    .requires_vrom = false,
    .monitors = builtin_rbv_monitors,
    .factory = factory,
};
