// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// v8_internal.h
// Private state of the V8 family substrate (v8.c) and its built-in video
// (v8_video.c).  Not for use outside src/machines/v8/.

#ifndef GS_MACHINES_V8_V8_INTERNAL_H
#define GS_MACHINES_V8_V8_INTERNAL_H

#include "board_io.h"
#include "display.h"
#include "display_class.h"
#include "display_ramdac.h"
#include "m68k_board_overlay.h"
#include "memory.h"
#include "system_internal.h"
#include "v8.h"

#include <stdbool.h>
#include <stdint.h>

struct asc;
struct egret;
struct cuda;
struct rbv;
struct mmu_state;
struct object;

// The built-in video: the Ariel CLUT/DAC at $50F24000 and the scanout over
// VRAM or a DRAM page.  Field order up to `display` is the checkpoint format.
typedef struct v8_video {
    uint8_t clut[256][3]; // Ariel colour table, R/G/B
    ramdac_seq_t seq; // Ariel address/data sequencer
    uint8_t ctrl; // Ariel control register (low 3 bits: depth)
    uint8_t key; // Ariel key-colour register
    uint8_t depth_code; // V8 MonP depth field: 0..4 = 1/2/4/8/16 bpp

    // --- not checkpointed below here ---
    display_t display;
    rgba8_t clut_view[256]; // the palette the current depth addresses
    uint8_t *vram; // VRAM (V8_FB_VRAM), board-owned
    uint8_t *blank; // black raster presented while nothing scans
    size_t blank_size;
    uint8_t sense; // monitor sense strapped on the port (7 = none)
    uint32_t width, height; // the raster the sense selects
    display_fb_node_t fb_node;
    struct object *video_node;
} v8_video_t;

// The machine state.
typedef struct v8_state {
    struct asc *asc;
    struct egret *egret; // the companion, when Egret
    struct cuda *cuda; // the companion, when Cuda
    struct rbv *rbv; // the V8's pseudo-VIA2 register file
    struct mmu_state *mmu;

    m68k_board_overlay_t overlay; // ROM at zero until the first ROM access
    board_io_t io; // the I/O island dispatch context
    memory_interface_t io_interface;

    // RAM decode.  Host RAM is laid out [SIMM bank][soldered RAM] so the
    // decode the ROM settles on is host-contiguous; `pcb_lo` holds the
    // soldered RAM's bottom 2 MB when the full 10 MB is SIMM-filled and that
    // half has no address.
    uint32_t simm_size; // installed SIMM bank, bytes (0, 2, 4 or 8 MB)
    uint32_t pcb_size; // soldered RAM, bytes
    uint8_t *simm_host; // host base of the SIMM bank
    uint8_t *pcb_host; // host base of the soldered RAM's addressable part
    uint8_t *pcb_hidden; // the soldered RAM's bottom 2 MB when unaddressable
    uint8_t ram_exp; // the size bits last decoded (V8 Exp register)

    v8_video_t video;
} v8_state_t;

static inline v8_state_t *v8_st(config_t *cfg) {
    return (v8_state_t *)cfg->machine_context;
}

static inline const v8_board_t *v8_board(config_t *cfg) {
    return (const v8_board_t *)cfg->machine->board;
}

// === v8_video.c ===

// Build the built-in video: allocate VRAM and the blank raster, decode the
// monitor sense, attach the framebuffer node.  `sense` is the configured
// monitor's code for an external port (ignored for a built-in CRT).
int v8_video_init(config_t *cfg, uint8_t sense);
void v8_video_teardown(config_t *cfg);
// The descriptor system_display() presents (NULL with no monitor).
display_t *v8_video_display(config_t *cfg);
// Once per VBL: the guest may have drawn.
void v8_video_vbl(config_t *cfg);
// The V8 MonP depth field changed (rbv mode callback).
void v8_video_set_depth(config_t *cfg, int depth_code);
// The monitor-sense code the V8 latches in MonP bits 3-5.
uint8_t v8_video_sense(config_t *cfg);
// Ariel register access at $50F24000 + off.
uint8_t v8_ariel_read(config_t *cfg, uint32_t off);
uint8_t v8_ariel_peek(config_t *cfg, uint32_t off);
void v8_ariel_write(config_t *cfg, uint32_t off, uint8_t value);
// Checkpoint the video's plain-data prefix.
void v8_video_checkpoint(config_t *cfg, checkpoint_t *cp);
void v8_video_restore(config_t *cfg, checkpoint_t *cp);

#endif // GS_MACHINES_V8_V8_INTERNAL_H
