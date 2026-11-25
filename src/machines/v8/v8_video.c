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
// on 256 KB, 1024 on 512 KB).  The Ariel feeds eight index lines; at a reduced
// depth the pixel drives the HIGH lines and the unused low ones are pulled
// high, so pixel v of a d-bit screen reads CLUT entry (v << (8-d)) | (2^(8-d)-1)
// -- 1 bpp white/black at $7F/$FF.

#include "v8_internal.h"

#include "display_class.h"
#include "display_timing.h"
#include "log.h"
#include "monitor_catalog.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("video");

// The largest raster any member scans: 640 x 480 at 16 bpp rounds up to the
// 512 KB VRAM window, which is also what the blank stub must back.
#define V8_VIDEO_MAX_BYTES V8_VRAM_WINDOW

// Map a MonP depth code to a packed pixel format.
static pixel_format_t v8_depth_format(uint8_t code) {
    switch (code & 0x07u) {
    case 0:
        return PIXEL_1BPP_MSB;
    case 1:
        return PIXEL_2BPP_MSB;
    case 2:
        return PIXEL_4BPP_MSB;
    case 3:
        return PIXEL_8BPP;
    default:
        return PIXEL_16BPP_555;
    }
}

// Rebuild the palette the current depth addresses (see the file header).
static void v8_video_refresh_clut(config_t *cfg) {
    v8_video_t *v = &v8_st(cfg)->video;
    bool mono = v8_board(cfg)->crt_mono;
    uint32_t bpp = display_bpp(v->display.format);
    if (bpp > 8) {
        v->display.clut = NULL;
        v->display.clut_len = 0;
        return;
    }
    uint32_t len = 1u << bpp;
    uint32_t skip = 256u >> bpp;
    for (uint32_t i = 0; i < len; i++) {
        const uint8_t *e = v->clut[(skip - 1) + i * skip];
        // A one-gun CRT shows luminance; the drivers load equal R/G/B for it.
        v->clut_view[i] = (rgba8_t){mono ? e[1] : e[0], e[1], mono ? e[1] : e[2], 255};
    }
    v->display.clut = v->clut_view;
    v->display.clut_len = len;
    v->display.clut_dirty = true;
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
    } else {
        v->display.format = v8_depth_format(v->depth_code);
        stride = b->vram_size > 0x40000u ? 1024u : 512u; // fixed by the VRAM fitted
        store = v->vram;
        store_size = b->vram_size;
    }
    bool live = (b->crt_width || v->sense != 7) && v->width && v->height;
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
    return display_guest_addr(&st->video.display, st->video.vram, b->vram_size, V8_VRAM_BASE);
}

int v8_video_init(config_t *cfg, uint8_t sense) {
    const v8_board_t *b = v8_board(cfg);
    v8_video_t *v = &v8_st(cfg)->video;

    if (b->crt_width) {
        // A built-in CRT: one raster, one strap.
        v->sense = b->crt_sense;
        v->width = b->crt_width;
        v->height = b->crt_height;
    } else {
        v->sense = sense & 7u;
        const display_timing_t *t = display_timing_for_sense(v->sense);
        // The V8 decodes the 12" RGB (010) and the 13" RGB (110); nothing
        // connected selects the 512 x 384 timing (LC Developer Note p. 14).
        if (v->sense != 2 && v->sense != 6)
            t = display_timing_for_sense(2);
        v->width = t ? t->width : 512;
        v->height = t ? t->height : 384;
    }
    if (b->fb_source == V8_FB_VRAM) {
        v->vram = calloc(1, b->vram_size);
        if (!v->vram)
            return -1;
    }
    v->blank_size = V8_VIDEO_MAX_BYTES;
    v->blank = calloc(1, v->blank_size);
    if (!v->blank)
        return -1;
    ramdac_seq_init(&v->seq, 0);
    display_seed_gray_ramp((rgba8_t *)v->clut_view); // replaced on the first update
    for (int i = 0; i < 256; i++)
        v->clut[i][0] = v->clut[i][1] = v->clut[i][2] = (uint8_t)(255 - i);
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

uint8_t v8_video_sense(config_t *cfg) {
    return v8_st(cfg)->video.sense;
}

// A register read; a data read steps the R/G/B phase unless `peek`.
static uint8_t ariel_read(config_t *cfg, uint32_t off, bool peek) {
    v8_video_t *v = &v8_st(cfg)->video;
    switch (off & 3) {
    case 0:
        return ramdac_seq_addr(&v->seq);
    case 1: {
        uint8_t comp;
        uint8_t idx = ramdac_seq_read(&v->seq, peek, &comp);
        return v->clut[idx][comp];
    }
    case 2:
        return v->ctrl;
    default:
        return v->key;
    }
}

uint8_t v8_ariel_read(config_t *cfg, uint32_t off) {
    return ariel_read(cfg, off, false);
}

uint8_t v8_ariel_peek(config_t *cfg, uint32_t off) {
    return ariel_read(cfg, off, true);
}

void v8_ariel_write(config_t *cfg, uint32_t off, uint8_t value) {
    v8_video_t *v = &v8_st(cfg)->video;
    switch (off & 3) {
    case 0:
        ramdac_seq_set_waddr(&v->seq, value); // also the read address: one register
        ramdac_seq_set_raddr(&v->seq, value);
        break;
    case 1: {
        uint8_t idx;
        if (ramdac_seq_write(&v->seq, value, &idx)) {
            memcpy(v->clut[idx], v->seq.pending, 3);
            v8_video_refresh_clut(cfg);
        }
        break;
    }
    case 2:
        v->ctrl = value; // low 3 bits mirror the depth; the V8 MonP drives the scanout
        break;
    default:
        v->key = value;
        break;
    }
}

// The checkpointed plain-data prefix, up to the first host pointer
CHECKPOINT_PREFIX(v8_video_t, display);

void v8_video_checkpoint(config_t *cfg, checkpoint_t *cp) {
    const v8_board_t *b = v8_board(cfg);
    v8_video_t *v = &v8_st(cfg)->video;
    checkpoint_save_prefix(cp, v, v8_video_t);
    if (v->vram)
        system_write_checkpoint_data(cp, v->vram, b->vram_size);
}

void v8_video_restore(config_t *cfg, checkpoint_t *cp) {
    const v8_board_t *b = v8_board(cfg);
    v8_video_t *v = &v8_st(cfg)->video;
    checkpoint_load_prefix(cp, v, v8_video_t);
    if (v->vram)
        system_read_checkpoint_data(cp, v->vram, b->vram_size);
    v8_video_update(cfg);
    display_mark_all_dirty(&v->display);
}
