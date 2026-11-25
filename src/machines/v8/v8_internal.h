// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// v8_internal.h
// Private state of the V8 family substrate (v8.c) and its built-in video
// (v8_video.c).  Not for use outside src/machines/v8/.

#ifndef GS_MACHINES_V8_V8_INTERNAL_H
#define GS_MACHINES_V8_V8_INTERNAL_H

#include "ariel.h"
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
// VRAM or a DRAM page.  The fields up to `display` are checkpointed, in the
// order v8_video_checkpoint writes them (the Ariel's table first).
typedef struct v8_video {
    ariel_t ariel; // the Ariel register file (runtime/ariel.h)
    uint8_t depth_code; // V8 MonP depth field: 0..4 = 1/2/4/8/16 bpp
    uint8_t mode_code; // Sonora monitor code (video mode register bits 0-3; 0 = from the sense)
    bool blanked; // video switched off: V8 MonP RvVIDOff, Sonora video mode bit 7

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

    // Sonora only (sonora.c): the RAM configuration register lives in
    // ram_exp; the rest of its register file is here.  Field order is the
    // checkpoint format.
    struct {
        uint8_t misc[12]; // $50F26004-$50F2600F: VRAM size, speed, the Ardbeg's extras
        uint8_t vd_mode; // $50F28000: monitor code (bits 0-3), blanking (bit 7)
        uint8_t vd_colr; // $50F28001: frame-buffer depth, 0..4
        uint8_t vd_sense; // $50F28002: the sense lines' output drive (bits 0-2)
    } sonora;
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
// The video switched off (blank raster) or back on (rbv blank callback,
// Sonora's video mode bit 7).
void v8_video_set_blank(config_t *cfg, bool blanked);
// /RESET: the video back to its power-on state (depth 0, the raster the
// sense selects, not blanked).
void v8_video_reset(config_t *cfg);
// The monitor-sense code the V8 latches in MonP bits 3-5.
uint8_t v8_video_sense(config_t *cfg);
// Ariel register access at $50F24000 + off.
uint8_t v8_ariel_read(config_t *cfg, uint32_t off);
uint8_t v8_ariel_peek(config_t *cfg, uint32_t off);
void v8_ariel_write(config_t *cfg, uint32_t off, uint8_t value);
// Sonora: the monitor code in the video mode register (0 = keep the raster
// the sense selected).
void v8_video_set_mode(config_t *cfg, uint8_t mode_code);
// Checkpoint the video's plain-data prefix.
void v8_video_checkpoint(config_t *cfg, checkpoint_t *cp);
void v8_video_restore(config_t *cfg, checkpoint_t *cp);

// === sonora.c ===

// The Sonora decoder's half of the substrate: its I/O rows, RAM layout and
// decode, memory map, checkpoint block and reset.
int sonora_ram_layout(config_t *cfg);
void sonora_ram_cfg_written(void *ctx, uint8_t value);
void sonora_memory_layout_init(config_t *cfg);
void sonora_checkpoint(config_t *cfg, checkpoint_t *cp);
void sonora_restore(config_t *cfg, checkpoint_t *cp);
void sonora_bus_reset(config_t *cfg);

#endif // GS_MACHINES_V8_V8_INTERNAL_H
