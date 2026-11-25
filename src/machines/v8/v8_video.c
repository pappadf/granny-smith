// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// v8_video.c
// The V8 family's built-in video: the Ariel CLUT/DAC at $50F24000, the depth
// field of the V8's MonP register, and the scanout presentation over VRAM
// ($50F40000, the V8 and Spice) or a fixed page of main RAM (Eagle's compact
// 512 x 342 screen).
//
// Register truth: Apple, *Macintosh LC Computer* Developer Note (1990),
// "Video" and "VRAM" (pp. 13-15): 256 KB of VRAM runs the 512 x 384 monitor at
// up to 8 bpp and the 640 x 480 one at up to 4 bpp, 512 KB adds 16 bpp and
// 8 bpp; the frame buffer's row stride is fixed by the VRAM fitted (512 bytes
// on 256 KB, 1024 on 512 KB).  The Ariel and its reduced-depth palette window
// are the shared model in runtime/ariel.h.

#include "v8_internal.h"

#include "ariel.h"

#include "display_class.h"
#include "display_timing.h"
#include "log.h"
#include "monitor_catalog.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("video");

// The largest raster any member scans: 640 x 480 at 16 bpp (600 KB) fits
// Sonora's 1 MB VRAM window, which is also what the blank stub must back.
#define V8_VIDEO_MAX_BYTES SONORA_VRAM_WINDOW

// Map a MonP depth code to a packed pixel format (5-7 read as 16 bpp).
static pixel_format_t v8_depth_format(uint8_t code) {
    return ariel_depth_format(code, PIXEL_16BPP_555);
}

// Rebuild the palette the current depth addresses.  A one-gun CRT shows
// luminance; the drivers load equal R/G/B for it, and the green gun is the
// one presented.
static void v8_video_refresh_clut(config_t *cfg) {
    v8_video_t *v = &v8_st(cfg)->video;
    ariel_refresh_view(&v->ariel, &v->display, v->clut_view, v8_board(cfg)->crt_mono ? 1 : ARIEL_COLOUR);
}

// Re-derive the descriptor from the depth and the frame-buffer source.
static void v8_video_update(config_t *cfg) {
    const v8_board_t *b = v8_board(cfg);
    v8_state_t *st = v8_st(cfg);
    v8_video_t *v = &st->video;
    if (!v->blank)
        return;

    const uint8_t *store = NULL;
    size_t store_size = 0;
    uint32_t offset = 0, stride;
    if (b->fb_source == V8_FB_DRAM) {
        // Eagle: 1 bpp only, the screen at a fixed RAM address, rows packed.
        v->display.format = PIXEL_1BPP_MSB;
        stride = v->width / 8u;
        store = ram_native_pointer(cfg->memory_map, 0);
        store_size = memory_ram_size(cfg->memory_map);
        offset = b->dram_fb < store_size ? b->dram_fb : 0;
        // A machine without enough RAM to reach the address scans the
        // top-of-space image of the soldered RAM (v8.c): the same bytes.
        if (b->dram_fb >= store_size && st->pcb_host) {
            store = st->pcb_host;
            store_size = st->pcb_size;
            offset = st->pcb_size - (V8_RAM_SPACE - V8_RAM_IMAGE) + (b->dram_fb - V8_RAM_IMAGE);
        }
    } else if (v8_ops(cfg)->video_ctl) {
        // Sonora: rows packed at the mode's width (the video driver's
        // row bytes are width x depth / 8 in every mode).
        v->display.format = v8_depth_format(v->depth_code);
        stride = (v->width * display_bpp(v->display.format) + 7u) / 8u;
        store = v->vram;
        store_size = b->vram_size;
    } else {
        v->display.format = v8_depth_format(v->depth_code);
        stride = b->vram_size > 0x40000u ? 1024u : 512u; // fixed by the VRAM fitted
        store = v->vram;
        store_size = b->vram_size;
    }
    bool live = !v->blanked && (b->crt_width || v->sense != 7) && v->width && v->height;
    display_set_scanout(&v->display, live ? store : NULL, store_size, offset, stride, v->width, v->height, v->blank,
                        v->blank_size);
    v8_video_refresh_clut(cfg);
    v->display.shape_dirty = true;
    v->display.fb_dirty = true;
}

// framebuffer node: the live descriptor
static display_t *v8_fb_resolve(void *owner) {
    return v8_video_display((config_t *)owner);
}

// framebuffer node: the guest physical address of the first displayed byte
static uint64_t v8_fb_base(void *owner) {
    config_t *cfg = (config_t *)owner;
    v8_state_t *st = cfg ? v8_st(cfg) : NULL;
    if (!st)
        return 0;
    const v8_board_t *b = v8_board(cfg);
    if (b->fb_source == V8_FB_DRAM)
        return b->dram_fb;
    return display_guest_addr(&st->video.display, st->video.vram, b->vram_size, v8_ops(cfg)->vram_base);
}

// Latch the monitor strap: the raster the port's sense code selects.
static void v8_video_set_sense(config_t *cfg, uint8_t sense) {
    const v8_board_t *b = v8_board(cfg);
    v8_video_t *v = &v8_st(cfg)->video;
    if (b->crt_width) {
        // A built-in CRT: one raster, one strap.
        v->sense = b->crt_sense;
        v->width = b->crt_width;
        v->height = b->crt_height;
        return;
    }
    v->sense = sense & 7u;
    const display_timing_t *t = display_timing_for_sense(v->sense);
    // The V8 decodes the 12" RGB (010) and the 13" RGB (110); nothing
    // connected selects the 512 x 384 timing (LC Developer Note p. 14).
    if (v->sense != 2 && v->sense != 6)
        t = display_timing_for_sense(2);
    v->width = t ? t->width : 512;
    v->height = t ? t->height : 384;
}

int v8_video_init(config_t *cfg, uint8_t sense) {
    const v8_board_t *b = v8_board(cfg);
    v8_video_t *v = &v8_st(cfg)->video;

    v8_video_set_sense(cfg, sense);
    if (b->fb_source == V8_FB_VRAM) {
        v->vram = calloc(1, b->vram_size);
        if (!v->vram)
            return -1;
    }
    v->blank_size = V8_VIDEO_MAX_BYTES;
    v->blank = calloc(1, v->blank_size);
    if (!v->blank)
        return -1;
    ramdac_seq_init(&v->ariel.seq, 0);
    display_seed_gray_ramp((rgba8_t *)v->clut_view); // replaced on the first update
    for (int i = 0; i < 256; i++)
        v->ariel.clut[i][0] = v->ariel.clut[i][1] = v->ariel.clut[i][2] = (uint8_t)(255 - i);
    v8_video_update(cfg);
    display_blank_raster(&v->display);
    v->display.response_dirty = true;
    LOG(1, "%s video: %ux%u, sense %u, %s", b->chip, v->width, v->height, v->sense,
        b->fb_source == V8_FB_VRAM ? "VRAM" : "DRAM");
    v->fb_node = (display_fb_node_t){.owner = cfg, .resolve = v8_fb_resolve, .base = v8_fb_base};
    v->video_node = display_attach_video_node(&v->fb_node, "Video (built-in)");
    return 0;
}

void v8_video_teardown(config_t *cfg) {
    v8_video_t *v = &v8_st(cfg)->video;
    display_detach_video_node(v->video_node);
    v->video_node = NULL;
    free(v->vram);
    v->vram = NULL;
    free(v->blank);
    v->blank = NULL;
}

display_t *v8_video_display(config_t *cfg) {
    v8_state_t *st = v8_st(cfg);
    if (!st || !st->video.blank || (!v8_board(cfg)->crt_width && st->video.sense == 7))
        return NULL;
    return &st->video.display;
}

void v8_video_vbl(config_t *cfg) {
    v8_state_t *st = v8_st(cfg);
    if (st && st->video.blank)
        st->video.display.fb_dirty = true;
}

void v8_video_set_depth(config_t *cfg, int depth_code) {
    v8_video_t *v = &v8_st(cfg)->video;
    v->depth_code = (uint8_t)depth_code;
    LOG(2, "V8 video: depth code %d", depth_code);
    v8_video_update(cfg);
}

void v8_video_set_blank(config_t *cfg, bool blanked) {
    v8_video_t *v = &v8_st(cfg)->video;
    if (v->blanked == blanked)
        return;
    v->blanked = blanked;
    LOG(2, "V8 video: %s", blanked ? "off (blanked)" : "on");
    v8_video_update(cfg);
}

void v8_video_reset(config_t *cfg) {
    v8_video_t *v = &v8_st(cfg)->video;
    v->depth_code = 0;
    v->mode_code = 0;
    v->blanked = false;
    v8_video_set_sense(cfg, v->sense); // the raster the strap selects again
    v8_video_update(cfg);
}

void v8_video_set_mode(config_t *cfg, uint8_t mode_code) {
    v8_video_t *v = &v8_st(cfg)->video;
    // 0 = none written (the sense decides); a code with no timing set is ignored
    if (mode_code == v->mode_code || !ariel_mode_geometry(mode_code, &v->width, &v->height))
        return;
    v->mode_code = mode_code;
    LOG(2, "Sonora video: monitor code %u, %ux%u", mode_code, v->width, v->height);
    v8_video_update(cfg);
}

uint8_t v8_video_sense(config_t *cfg) {
    return v8_st(cfg)->video.sense;
}

uint8_t v8_ariel_read(config_t *cfg, uint32_t off) {
    return ariel_read(&v8_st(cfg)->video.ariel, off, false, 0xFF);
}

uint8_t v8_ariel_peek(config_t *cfg, uint32_t off) {
    return ariel_read(&v8_st(cfg)->video.ariel, off, true, 0xFF);
}

void v8_ariel_write(config_t *cfg, uint32_t off, uint8_t value) {
    if (ariel_write(&v8_st(cfg)->video.ariel, off, value))
        v8_video_refresh_clut(cfg);
}

// The checkpointed plain-data prefix, up to the first host pointer.  It is
// one block in the layout it had before the Ariel became a shared struct:
// the colour table, the sequencer, control and key, then the V8's own
// fields, zero-padded to the prefix's size.
CHECKPOINT_PREFIX(v8_video_t, display);
#define V8_VIDEO_CP_FIELDS(OP, buf, v)                                                                                 \
    OP(buf, (v)->ariel.clut, sizeof(v)->ariel.clut);                                                                   \
    OP(buf, &(v)->ariel.seq, sizeof(v)->ariel.seq);                                                                    \
    OP(buf, &(v)->ariel.ctrl, 1);                                                                                      \
    OP(buf, &(v)->ariel.key, 1);                                                                                       \
    OP(buf, &(v)->depth_code, 1);                                                                                      \
    OP(buf, &(v)->mode_code, 1);                                                                                       \
    OP(buf, &(v)->blanked, 1)
_Static_assert(sizeof(ariel_t) + 3 <= v8_video_t_cp_prefix, "the V8 video prefix holds the packed fields");
#define V8_CP_PACK(buf, src, n)   (memcpy((buf), (src), (n)), (buf) += (n))
#define V8_CP_UNPACK(buf, dst, n) (memcpy((dst), (buf), (n)), (buf) += (n))

void v8_video_checkpoint(config_t *cfg, checkpoint_t *cp) {
    const v8_board_t *b = v8_board(cfg);
    v8_video_t *v = &v8_st(cfg)->video;
    uint8_t block[v8_video_t_cp_prefix] = {0};
    uint8_t *p = block;
    V8_VIDEO_CP_FIELDS(V8_CP_PACK, p, v);
    system_write_checkpoint_data(cp, block, sizeof block, "v8_video_t");
    system_write_checkpoint_data(cp, &v->sense, 1); // the strap the machine was built with
    if (v->vram)
        system_write_checkpoint_data(cp, v->vram, b->vram_size);
}

void v8_video_restore(config_t *cfg, checkpoint_t *cp) {
    const v8_board_t *b = v8_board(cfg);
    v8_video_t *v = &v8_st(cfg)->video;
    uint8_t block[v8_video_t_cp_prefix];
    system_read_checkpoint_data(cp, block, sizeof block, "v8_video_t");
    const uint8_t *p = block;
    V8_VIDEO_CP_FIELDS(V8_CP_UNPACK, p, v);
    uint8_t sense = v->sense;
    system_read_checkpoint_data(cp, &sense, 1);
    v8_video_set_sense(cfg, sense);
    uint8_t mode = v->mode_code; // Sonora: the raster the monitor code selected
    v->mode_code = 0;
    if (mode)
        v8_video_set_mode(cfg, mode);
    if (v->vram)
        system_read_checkpoint_data(cp, v->vram, b->vram_size);
    v8_video_update(cfg);
    display_mark_all_dirty(&v->display);
}
