// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// qd3d_raster.h
// The raster-backend seam of the QuickDraw 3D Accelerator Card
// (docs/internals/core/peripherals/pci/cards/qd3d.md, "The raster seam"),
// shaped like the Voodoo2's (voodoo2_raster.h).
//
// The card (qd3d.c) is the PRODUCER.  It owns the register file, walks the
// control program out of host memory when the go register is written,
// decodes each object record, and turns the walk into commands (q3_cmd_t):
// start a frame, add an object, render a scanline.  Each command names a
// slot in a ring of draw-state snapshots (q3_draw_state_t) holding every
// register the renderer reads.  The EXECUTOR (q3_raster_execute) turns the
// commands into pixels against a q3_target_t -- the active-object list,
// the per-pixel fragment lists and the frame's output stream -- which it
// OWNS, reading the texture SRAM the producer writes only after a sync.
// The executor never sees the card struct and never touches host memory:
// the walk (bus-master reads) and the output (bus-master writes) both stay
// with the producer, so a worker thread never races the guest.
//
// Two invariants keep any backend guest-invisible:
//
//   1. TIMING IS ANALYTIC.  A frame finishes at a time computed from its
//      size (qd3d.c, frame_start), never from the cost of rendering it,
//      and its pixels reach host memory at that moment, on every backend.
//   2. THE TARGET IS READ ONLY AFTER A SYNC.  The producer fences before it
//      streams a frame's output, writes the texture SRAM, or checkpoints.
//
// A backend is only WHERE the executor runs:
//
//   sw      inline at submit -- the NORMATIVE backend: it defines the
//           semantics and produces every golden
//   null    as sw, but OBJECT commands are dropped: every frame is the
//           background alone (pins invariant 1)
//   thread  commands go to a bounded SPSC queue drained by one worker
//           pthread; sync() is a fence.  The DEFAULT on every build (in the
//           browser a Web Worker created on demand).  A build can default
//           to sw with -DGS_QD3D_RASTER_DEFAULT='"sw"', or leave the
//           backend out with -DGS_QD3D_THREAD_BACKEND=0; any boot can pick
//           one with pci_option="raster=...".
//
// Queue order is submission order and every read of the target fences
// first, so the thread backend's output is byte-identical to the walker's.

#ifndef QD3D_RASTER_H
#define QD3D_RASTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Texture SRAM: 2^18 texels (revision 2) at internal address $800000.
#define Q3_TEX_BASE   0x800000u
#define Q3_TEX_TEXELS 0x40000u
// Mip levels of the largest (256 x 256) map: 256 ... 1.
#define Q3_MAP_LEVELS 9
// Most fragments one pixel keeps; the ASIC keeps four layers per 16-pixel
// segment and re-renders a segment that overflows, which software never
// sees -- a few more than four here stands in for that.
#define Q3_LAYERS 8
// Widest output scanline: 13.3 coordinates cap the card at 8K pixels.
#define Q3_MAX_WIDTH 8192u

// One decoded object, live from the scanline it first touches to its last.
enum { Q3_OBJ_GOURAUD = 0, Q3_OBJ_TEXTURED, Q3_OBJ_BITMAP };
enum { Q3_A_DEPTH = 0, Q3_A_ALPHA, Q3_A_R, Q3_A_G, Q3_A_B, Q3_A_U, Q3_A_V, Q3_A_LOD, Q3_A_NATTR };
#define Q3_A_KD Q3_A_R // textured triangles shade with one grey diffuse...
#define Q3_A_KS Q3_A_G // ...and one grey specular intensity

typedef struct q3_obj {
    uint8_t kind;
    uint32_t header;
    int first, last; // scanline range
    uint16_t x13[3], y13[3]; // vertices, 13.3 fixed point
    float x0, y0; // vertex 0 in pixels (the plane-equation origin)
    float a0[Q3_A_NATTR], dx[Q3_A_NATTR], dy[Q3_A_NATTR]; // attribute planes
    // textured: the map
    int tex_slot, tex_size, tex_levels;
    // bitmap tile
    int bx, by;
    uint32_t colour;
    uint32_t mask[12];
} q3_obj_t;

// One fragment of one pixel.
typedef struct q3_frag {
    float depth;
    float r, g, b, a; // 0..1, colour premultiplied by alpha
    uint8_t flags; // Q3_FRAG_*
} q3_frag_t;

// The draw-state snapshot: every register the renderer reads, copied by
// the producer when one of them changes (not per command), and the ONLY
// card state the executor ever sees.
typedef struct q3_draw_state {
    uint32_t background; // register $08, ARGB8888
    uint32_t csg; // register $24, the CSG truth table
    uint32_t mapbase[Q3_MAP_LEVELS]; // registers $48-$68, the mip level bases
    bool persp; // mode register $0800: depth is 1/w, u and v carry it
    bool dest16; // 16-bpp output (DMACONTROL $0C00 or mode register $0008)
} q3_draw_state_t;

// The executor's target.  `tex` is the card's texture SRAM, which the
// producer writes only after a sync; the rest is the executor's own.
typedef struct q3_target {
    const uint32_t *tex; // [Q3_TEX_TEXELS], RGB quantised to 5-6-5
    // Every object of the frame in walk order, and the indices of those
    // still live, in the same order.
    q3_obj_t *objs;
    uint32_t n_objs, cap_objs;
    uint32_t *live;
    uint32_t n_live, cap_live;
    // The scanline being rendered.
    q3_frag_t *frags; // [Q3_LAYERS * Q3_MAX_WIDTH]
    uint8_t *nfrags; // [Q3_MAX_WIDTH]
    // The frame's output stream: every scanline's pixels, in order, as the
    // descriptor list will carry them.
    uint8_t *out;
    size_t out_len, out_cap;
    bool out_failed; // an allocation failed: the frame's tail is lost
} q3_target_t;

// The command stream.  Order is submission order, always.
typedef enum q3_cmd_kind {
    Q3_CMD_FRAME, // a frame starts: no live objects, an empty output stream
    Q3_CMD_OBJECT, // one decoded object joins the active list
    Q3_CMD_SCANLINE, // render scanline u.line.y, u.line.width pixels wide
} q3_cmd_kind_t;

typedef struct q3_cmd {
    uint8_t kind; // q3_cmd_kind_t
    uint16_t state; // draw-state ring slot this command reads
    union {
        q3_obj_t obj;
        struct {
            int32_t y;
            uint32_t width;
        } line;
    } u;
} q3_cmd_t;

// --- the target ----------------------------------------------------------

// Allocate / free the target's own buffers (not `tex`).
bool q3_target_init(q3_target_t *tgt, const uint32_t *tex);
void q3_target_free(q3_target_t *tgt);
// Make room for `len` bytes of output (a restored checkpoint's pending
// frame); false if it cannot.
bool q3_target_reserve(q3_target_t *tgt, size_t len);

// --- the executor (the normative renderer) -------------------------------

// Execute one command against the target with the given snapshot.
void q3_raster_execute(const q3_draw_state_t *st, q3_target_t *tgt, const q3_cmd_t *cmd);

// --- the backend context ---------------------------------------------------

struct q3_raster;
typedef struct q3_raster q3_raster_t;

// Producer callback: fill a draw-state slot from the live registers.
typedef void (*q3_state_build_fn)(void *ctx, q3_draw_state_t *st);

// Create a backend: `kind` is "sw", "null" or "thread" (unknown kinds and
// an unavailable thread backend fall back to "sw" with a log line).
// `build` is called, with `ctx`, whenever a command needs a fresher
// snapshot than the ring holds.
q3_raster_t *q3_raster_create(const char *kind, q3_target_t *tgt, q3_state_build_fn build, void *ctx);
// Fence, stop the worker if any, free.
void q3_raster_destroy(q3_raster_t *r);
const char *q3_raster_name(const q3_raster_t *r);
// The producer changed a register the snapshot holds.
void q3_raster_state_dirty(q3_raster_t *r);
// Submit one command (copied).  cmd->state is filled in here.
void q3_raster_submit(q3_raster_t *r, q3_cmd_t *cmd);
// Retire every submitted command; on return the target is authoritative
// and the producer may read it, or write the texture SRAM.
void q3_raster_sync(q3_raster_t *r);

#endif // QD3D_RASTER_H
