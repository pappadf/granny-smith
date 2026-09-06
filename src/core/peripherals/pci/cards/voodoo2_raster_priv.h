// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// voodoo2_raster_priv.h
// Helpers shared between the executor (voodoo2_raster.c) and the WebGPU
// translator (voodoo2_gpu.c): the framebuffer and texture-memory
// addressing, and the normative texel expansion the translator reuses
// to build GPU textures from the shadow texture RAM.  Private to the
// two translation units; the card never includes this.

#ifndef VOODOO2_RASTER_PRIV_H
#define VOODOO2_RASTER_PRIV_H

#include "voodoo2_raster.h"

#define V2_FB_MASK (V2_RASTER_FB_SIZE - 1u)

// The per-pixel leaf helpers are forced inline (walker proposal §3.6).
#define V2_INLINE static inline __attribute__((always_inline))

// Physical byte address of a 16-bit pixel in a software-selected
// buffer (0 front, 1 back, 3 aux); the bases were resolved by the
// producer against the displayed buffer when the state was snapshotted.
V2_INLINE uint32_t v2_fb_addr(const v2_draw_state_t *st, uint32_t buffer, uint32_t x, uint32_t y) {
    return (st->buf_base[buffer & 3u] + y * st->stride + x * 2u) & V2_FB_MASK;
}

// DRAM byte address of texel (s,t) at `lod`.
V2_INLINE uint32_t v2_texel_addr(const v2_tmu_state_t *tm, int lod, uint32_t s, uint32_t t) {
    uint32_t texel_bytes = tm->is8 ? 1u : 2u;
    uint32_t addr = tm->lod_base[lod] + (t * tm->lod_w[lod] + s) * texel_bytes;
    return addr & tm->mask;
}

// Clamp or wrap one texel coordinate against a level dimension.
V2_INLINE uint32_t v2_tex_coord(int32_t c, uint32_t dim, bool clamp) {
    if (clamp)
        return (uint32_t)(c < 0 ? 0 : (c >= (int32_t)dim ? (int32_t)dim - 1 : c));
    return (uint32_t)c & (dim - 1u);
}

// Read the raw texel at an already clamped/wrapped (s,t).
V2_INLINE uint32_t v2_texel_raw(const v2_tmu_state_t *tm, const v2_target_t *tgt, int tmu, int lod, uint32_t s,
                                uint32_t t) {
    uint32_t at = v2_texel_addr(tm, lod, s, t);
    const uint8_t *ram = tgt->tex[tmu];
    if (tm->is8)
        return ram[at];
    return ram[at] | ((uint32_t)ram[(at + 1u) & tm->mask] << 8);
}

// Expand one raw texel to 32-bit ARGB per the tformat table (V2 p.81).
uint32_t v2_texel_expand(const v2_tmu_state_t *tm, const v2_target_t *tgt, int tmu, uint32_t raw);
// The 256-entry expansion cache for the 8-bit formats, rebuilt when the
// (format, NCC select, palette/NCC generation) key changes.
const uint32_t *v2_expand_lut(const v2_tmu_state_t *tm, v2_target_t *tgt, int tmu);

#endif // VOODOO2_RASTER_PRIV_H
