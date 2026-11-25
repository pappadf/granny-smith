// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// tnt_fb.c
// What the family's two on-board video paths share: Control's RaDACal
// (control.c) and Platinum's DACula (platinum.c) are one RAMDAC design
// behind the same Grand Central +$1B000 cells, scanning a framebuffer the
// same way.  This file holds the common half, and each chip keeps its own:
//
//   * the RAMDAC's three sequenced ports — address (+$00), the eight-entry
//     cursor palette (+$10) and the CLUT data port (+$30).  The indexed
//     register port (+$20) is where the two chips' register files differ,
//     so the chips decode it themselves;
//   * the depth code (control register bits 3:2 = 8/16/32 bpp) and the
//     CLUT materialisation it selects;
//   * the hardware-cursor composite: a 16-pixel, 4-bit image plane in the
//     16 bytes before each scan line's pixel 0, an X position, an implicit
//     Y (control.c documents the scheme as decoded from the driver);
//   * the retrace-period arithmetic the chips' clocks feed, and the scan
//     line it puts the CPU clock at;
//   * the three host buffers (VRAM, the blank stub, the composite) and the
//     machine.video node.
//
// Each chip's CRTC, clock synthesiser, register file, VRAM decode and VBL
// wiring stay in its own file.  The chip state structures keep their own
// layouts (they are checkpointed whole); the palette half is the shared
// tnt_dac_pal_t embedded in both.

#include "tnt.h"

#include "display.h"
#include "log.h"
#include "scheduler.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("video");

// ============================================================
// The RAMDAC ports
// ============================================================

// The cursor-colour port (+$10) runs an R/G/B phase of its own over the
// RAMDAC's one address register: an access borrows the address, and hands
// back where the entry advance left it.
static void crsr_seq_take(ramdac_seq_t *addr, tnt_dac_pal_t *pal) {
    pal->crsr_seq.widx = pal->crsr_seq.ridx = ramdac_seq_addr(addr);
}

static void crsr_seq_give(ramdac_seq_t *addr, tnt_dac_pal_t *pal) {
    addr->widx = addr->ridx = ramdac_seq_addr(&pal->crsr_seq);
}

uint8_t tnt_dac_port_read(ramdac_seq_t *addr, tnt_dac_pal_t *pal, uint32_t offset, bool peek) {
    uint8_t comp, idx;
    switch (offset & 0x30u) {
    case 0x00:
        return ramdac_seq_addr(addr);
    case 0x10: // the cursor palette: the entry auto-advances, like the CLUT port
        crsr_seq_take(addr, pal);
        idx = ramdac_seq_read(&pal->crsr_seq, peek, &comp);
        crsr_seq_give(addr, pal);
        return pal->crsr[idx & 7u][comp];
    default: // +$30: CLUT data, the RGB phase auto-advances
        idx = ramdac_seq_read(addr, peek, &comp);
        return pal->clut[idx][comp];
    }
}

bool tnt_dac_port_write(ramdac_seq_t *addr, tnt_dac_pal_t *pal, uint32_t offset, uint8_t value, uint8_t *clut_idx) {
    uint8_t idx;
    switch (offset & 0x30u) {
    case 0x00:
        ramdac_seq_set_waddr(addr, value);
        ramdac_seq_set_waddr(&pal->crsr_seq, value);
        return false;
    case 0x10: // the driver streams all eight entries in one burst
        crsr_seq_take(addr, pal);
        if (ramdac_seq_write(&pal->crsr_seq, value, &idx))
            memcpy(pal->crsr[idx & 7u], pal->crsr_seq.pending, 3);
        crsr_seq_give(addr, pal);
        return false;
    default: // +$30: CLUT data, latched and stored on B
        if (!ramdac_seq_write(addr, value, &idx))
            return false;
        memcpy(pal->clut[idx], addr->pending, 3);
        *clut_idx = idx;
        return true;
    }
}

// ============================================================
// Depth and the CLUT
// ============================================================

uint32_t tnt_fb_bpp(uint8_t ctrl) {
    switch ((ctrl >> 2) & 3u) {
    case 1:
        return 16;
    case 2:
        return 32;
    default:
        return 8;
    }
}

pixel_format_t tnt_fb_format(uint8_t ctrl) {
    switch ((ctrl >> 2) & 3u) {
    case 1:
        return PIXEL_16BPP_555;
    case 2:
        return PIXEL_32BPP_XRGB;
    default:
        return PIXEL_8BPP;
    }
}

// Materialize the RAMDAC table for the consumers.  At 8 bpp it is the CLUT
// the pixel indexes; in the direct-colour modes it stays in the DAC path as
// one lookup per channel (display_t.dac_lut) -- MkLinux's console draws
// palette indexes into 32 bpp pixels and relies on that expansion, and
// dropping the table there rendered it near-black (#147).
void tnt_fb_refresh_clut(tnt_state_t *st, const tnt_dac_pal_t *pal, uint8_t ctrl) {
    uint32_t bpp = tnt_fb_bpp(ctrl);
    if (bpp > 8) {
        // At 16 bpp the RAMDAC indexes its table with each channel's 5-bit
        // value, and Apple's driver loads only entries 0-31 (its gamma ramp;
        // the rest hold whatever was there).  The consumers look the table up
        // with the channel expanded to 8 bits, so file entry v>>3 under every
        // expansion of v -- expand5(v) >> 3 == v.
        uint32_t shift = bpp == 16 ? 3u : 0u;
        for (uint32_t i = 0; i < 256; i++)
            for (int c = 0; c < 3; c++)
                st->dac_view[c][i] = pal->clut[i >> shift][c];
        st->display.clut = NULL;
        st->display.clut_len = 0;
        st->display.dac_lut = st->dac_view;
        st->display.response_dirty = true;
        return;
    }
    if (st->display.dac_lut) {
        st->display.dac_lut = NULL;
        st->display.response_dirty = true;
    }
    for (uint32_t i = 0; i < 256; i++) {
        st->clut_view[i].r = pal->clut[i][0];
        st->clut_view[i].g = pal->clut[i][1];
        st->clut_view[i].b = pal->clut[i][2];
        st->clut_view[i].a = 255;
    }
    st->display.clut = st->clut_view;
    st->display.clut_len = 256;
    st->display.clut_dirty = true;
}

// ============================================================
// The hardware cursor
// ============================================================

// The CLUT entry nearest an RGB triple (the cursor's colour at 8 bpp): the
// first exact match, or the least squared distance.
static uint8_t clut_match(const tnt_dac_pal_t *pal, const uint8_t *rgb) {
    uint32_t best = 0, bestd = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < 256 && bestd; i++) {
        int dr = (int)pal->clut[i][0] - rgb[0], dg = (int)pal->clut[i][1] - rgb[1], db = (int)pal->clut[i][2] - rgb[2];
        uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
        if (d < bestd) {
            bestd = d;
            best = i;
        }
    }
    return (uint8_t)best;
}

void tnt_fb_compose(tnt_state_t *st, const tnt_dac_pal_t *pal, uint8_t ctrl, uint32_t cursor_x, uint64_t base,
                    uint32_t vram_size) {
    if (!st->blank || !st->compose || !st->display.bits || st->display.bits == st->blank)
        return; // not built yet, or blanked: nothing to composite
    if (!(ctrl & TNT_DAC_CURSOR_ON))
        return; // cursor off: update() already points at live VRAM
    uint32_t bpx = tnt_fb_bpp(ctrl) / 8u;
    uint32_t stride = st->display.stride, w = st->display.width, h = st->display.height;
    uint64_t span = (uint64_t)stride * h;
    if (base + span > vram_size)
        return;
    memcpy(st->compose, st->vram + base, (size_t)span);
    // The plane's row stride is the CURRENT pitch: every depth's mode line
    // programs a 32-byte row margin (640-wide: 2592/1312/672 at 32/16/8
    // bpp), and the plane rides in the 16 bytes before each row's pixel 0.
    // (First decoded in a 32 bpp-only mode as "fixed width*4+32" -- live
    // Monitors switches to 16/8 bpp disproved that: the stale 2592-stride
    // read drew the old plane plus framebuffer bytes as a junk cursor
    // column.)
    uint32_t x = cursor_x + 1u; // hotspot bias
    for (uint32_t y = 0; y < h; y++) {
        uint64_t poff = base + (uint64_t)y * stride;
        if (poff < 16u || poff + 8u > vram_size)
            continue;
        const uint8_t *row = st->vram + poff - 16u;
        uint8_t *line = st->compose + (size_t)y * stride;
        for (uint32_t i = 0; i < 16u; i++) {
            uint8_t nib = (i & 1u) ? (row[i >> 1] & 0x0Fu) : (row[i >> 1] >> 4);
            if (!(nib & 0x8u))
                continue; // transparent
            uint32_t px = x + i;
            // The width is the guest's, not clamped to the pitch: a pixel
            // past the row's stride would write past the composite buffer.
            if (px >= w || (uint64_t)(px + 1u) * bpx > stride)
                continue;
            const uint8_t *rgb = pal->crsr[nib & 7u];
            uint8_t *dst = line + (size_t)px * bpx;
            switch (bpx) {
            case 1:
                dst[0] = clut_match(pal, rgb);
                break;
            case 2: {
                uint16_t v = display_pack555(rgb[0], rgb[1], rgb[2]);
                dst[0] = (uint8_t)(v >> 8);
                dst[1] = (uint8_t)v;
                break;
            }
            default:
                dst[0] = 0;
                dst[1] = rgb[0];
                dst[2] = rgb[1];
                dst[3] = rgb[2];
                break;
            }
        }
    }
    st->display.bits = st->compose;
}

// ============================================================
// Retrace timing
// ============================================================

uint64_t tnt_fb_period_ns(uint64_t htotal, uint64_t vtotal, uint64_t dot_hz) {
    uint64_t ns = htotal * vtotal * 1000000000ull / dot_hz;
    if (ns < TNT_VBL_MIN_NS || ns > TNT_VBL_MAX_NS)
        return TNT_VBL_FALLBACK_NS; // a mode line caught half-written
    return ns;
}

bool tnt_fb_scan_line(config_t *cfg, uint64_t frame_ns, uint32_t vtotal, uint32_t *line) {
    uint64_t frame = cfg->machine->freq * frame_ns / 1000000000ull; // cycles per frame
    if (!frame)
        return false; // a machine with no clock yet -- % 0 is undefined
    *line = (uint32_t)((scheduler_cpu_cycles(cfg->scheduler) % frame) * vtotal / frame);
    return true;
}

// ============================================================
// The host buffers and machine.video
// ============================================================

int tnt_fb_alloc(config_t *cfg, const char *chip) {
    tnt_state_t *st = tnt_st(cfg);
    st->vram = calloc(1, TNT_VRAM_SIZE);
    st->blank = calloc(1, TNT_VRAM_SIZE);
    st->compose = calloc(1, TNT_VRAM_SIZE);
    if (!st->vram || !st->blank || !st->compose) {
        LOG(0, "Error: out of memory allocating %s's three %u-byte framebuffers", chip, (unsigned)TNT_VRAM_SIZE);
        tnt_fb_free(cfg);
        return -1;
    }
    return 0;
}

void tnt_fb_attach_node(config_t *cfg, display_t *(*resolve)(void *owner), uint64_t (*base)(void *owner),
                        const char *label) {
    tnt_state_t *st = tnt_st(cfg);
    st->fb_node = (display_fb_node_t){.owner = cfg, .resolve = resolve, .base = base};
    st->video_node = display_attach_video_node(&st->fb_node, label);
}

void tnt_fb_free(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    display_detach_video_node(st->video_node);
    st->video_node = NULL;
    free(st->vram);
    st->vram = NULL;
    free(st->blank);
    st->blank = NULL;
    free(st->compose);
    st->compose = NULL;
}
