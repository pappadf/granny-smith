// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rage128_gpu.h
// The Rage 128's WebGPU takeover: the translator that, while ENGAGED,
// turns the 3D engine's set-up triangles into draws on the host GPU and
// keeps the colour and Z surfaces they render into on the GPU
// (docs/internals/core/peripherals/pci/cards/rage128.md, "The WebGPU
// takeover").  Everything runs on the emulator thread, synchronously:
// a fence blocks on the GPU worker.  The wire protocol is
// rage128_gpu_protocol.h; the host transport the gs_v2gpu_* seam in
// system.h (shared with the Voodoo2's takeover).
//
// VRAM stays the card's memory.  A surface's rows are each in one of
// three states: equal on both sides, newer on the GPU (drawn there, not
// read back), or newer in VRAM (written by something else, not yet
// uploaded).  Every other VRAM access passes r128_vram_access()
// (rage128_priv.h), which reads GPU-newer rows back before anyone sees
// them and marks the rows a write touches VRAM-newer.

#ifndef RAGE128_GPU_H
#define RAGE128_GPU_H

#include "rage128_raster_priv.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct r128_gpu;
typedef struct r128_gpu r128_gpu_t;

// Engagement policy (pci_option "gpu=").
#define R128GPU_ENGAGE_AUTO   0 // a 3D draw into a screen-shaped surface engages
#define R128GPU_ENGAGE_ALWAYS 1 // any 3D draw engages (tests)

// Attach the browser's GPU worker (blocks up to a few seconds).  NULL
// when no transport exists (native builds, no WebGPU adapter, the attach
// timed out): the card keeps the software rasteriser.
r128_gpu_t *r128_gpu_create(rage128_t *r, int engage_policy);
// Drop everything without readback and free.
void r128_gpu_destroy(r128_gpu_t *g);

bool r128_gpu_engaged(const r128_gpu_t *g);

// The 3D engine: a primitive batch under the snapshot `s`.  True when the
// GPU takes it (the walker then hands each surviving triangle to
// r128_gpu_tri and closes with r128_gpu_batch_end); false when the walker
// draws it itself, its VRAM accesses fenced as anyone's.
bool r128_gpu_batch_begin(r128_gpu_t *g, st_t *s, uint32_t prim, const r128_vertex_t *v, uint32_t n);
void r128_gpu_tri(r128_gpu_t *g, const st_t *s, const svtx_t *a, const svtx_t *b, const svtx_t *c);
void r128_gpu_batch_end(r128_gpu_t *g, const st_t *s);

// The 2D engine: a solid fill of [x0,x1) x [y0,y1) (pixels of a surface
// at `base`/`stride`/`bpp`, already clipped) with the packed `value`.
// True when the GPU did it; false: the engine draws it in VRAM.
bool r128_gpu_fill(r128_gpu_t *g, uint32_t base, uint32_t stride, uint32_t bpp, int32_t x0, int32_t y0, int32_t x1,
                   int32_t y1, uint32_t value);

// The slow half of r128_vram_access: [at, at+len) overlaps a surface.
void r128_gpu_fence(r128_gpu_t *g, uint32_t at, uint32_t len, bool write);

// Vertical blank: run the idle/storm rules and present the scanned-out
// surface through the display table.  True: the GPU showed the frame
// (VRAM's copy of it may be stale — display_t.sync_pixels reads it back).
bool r128_gpu_vblank(r128_gpu_t *g);

// Make every GPU-newer row current in VRAM (checkpoint save, screenshot).
void r128_gpu_sync_all(r128_gpu_t *g);
// Leave GPU mode: read back unless `discard` (reset, checkpoint restore).
void r128_gpu_disengage(r128_gpu_t *g, bool discard);

const char *r128_gpu_stats(r128_gpu_t *g, char *buf, size_t n);

#endif // RAGE128_GPU_H
