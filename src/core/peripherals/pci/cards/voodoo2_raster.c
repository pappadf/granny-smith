// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// voodoo2_raster.c
// The Voodoo2's software rasteriser — the normative backend behind the
// seam in voodoo2_raster.h — and the backends that decide WHERE it
// runs (inline, or on a worker thread).
//
// This translation unit deliberately does not include the card's
// definition: everything the pipeline reads arrives in a
// v2_draw_state_t snapshot, everything it writes lives in the
// v2_target_t it owns.  A function here that wanted a live register
// could not get one — which is the rule "the worker must never read
// live card state" (thread proposal §4) enforced by the linker rather
// than by discipline.
//
// Register truth: 3dfx, *Voodoo2 Graphics Specification* rev 1.16 —
// cited "[V2 p.N]"; the fill convention is CHOSEN (voodoo2.md).  The
// per-pixel semantics here are exactly the milestone-3c walker's;
// every rung of the walker-optimization proposal is bit-exact against
// it by construction, and the goldens are the oracle.

#include "voodoo2_raster.h"

#include "log.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__EMSCRIPTEN__)
#define V2_HAVE_THREAD_BACKEND 1
#include <pthread.h>
#include <stdatomic.h>
#else
#define V2_HAVE_THREAD_BACKEND 0
#endif

LOG_USE_CATEGORY_NAME("voodoo2");

#define V2_FB_MASK (V2_RASTER_FB_SIZE - 1u)

// The per-pixel leaf helpers are forced inline (walker proposal §3.6;
// the precedent is the PPC decoder's +34% from the same attribute):
// -O2 across a TU this size otherwise leaves call boundaries in the
// hottest loop of the emulator.
#define V2_INLINE static inline __attribute__((always_inline))

// One pixel's state entering the back half of the pipeline.
typedef struct v2_pix {
    int32_t x, y; // screen pixel (top-left origin)
    uint32_t r, g, b, a; // clamped/wrapped 8-bit iterated colour
    uint32_t z16; // clamped 16-bit Z
    int64_t z_raw, w_raw; // unclamped iterators (float depth, fog)
    uint32_t w8; // clamped W byte for the ACU/fog
    uint32_t tex_argb; // TMU0 chain output (0 when texture disabled)
    bool have_tex;
} v2_pix_t;

// ============================================================
// The pixel-provenance watch (§9.2 instrument, GS_V2_WATCH="x,y")
// ============================================================
// Armed by the pixel pipe for the watched pixel so the texel fetches
// that shade it can identify themselves.  Read once from the
// environment; a threaded backend refuses to start while it is armed
// (the instrument logs from inside the executor, and diagnosis uses
// the synchronous walker — thread proposal §5.7).

static int s_watch_x = -2, s_watch_y = -2;
static bool s_watch_now;

static void v2_watch_init(void) {
    if (s_watch_x == -2) {
        const char *w = getenv("GS_V2_WATCH");
        s_watch_x = s_watch_y = -1;
        if (w)
            sscanf(w, "%d,%d", &s_watch_x, &s_watch_y);
    }
}

bool v2_raster_watch_armed(void) {
    v2_watch_init();
    return s_watch_x >= 0;
}

// ============================================================
// Framebuffer addressing
// ============================================================

// Physical byte address of a 16-bit pixel in a software-selected
// buffer (0 front, 1 back, 3 aux); the bases were resolved by the
// producer against the displayed buffer when the state was snapshotted.
V2_INLINE uint32_t v2_fb_addr(const v2_draw_state_t *st, uint32_t buffer, uint32_t x, uint32_t y) {
    return (st->buf_base[buffer & 3u] + y * st->stride + x * 2u) & V2_FB_MASK;
}

// 16-bit raw framebuffer access at a physical (buffer, x, y).
V2_INLINE uint16_t v2_fb_load(const v2_draw_state_t *st, const v2_target_t *tgt, uint32_t buffer, int32_t x,
                              int32_t y) {
    uint32_t at = v2_fb_addr(st, buffer, (uint32_t)x, (uint32_t)y);
    return (uint16_t)(tgt->fb[at] | ((uint16_t)tgt->fb[(at + 1u) & V2_FB_MASK] << 8));
}

V2_INLINE void v2_fb_store(const v2_draw_state_t *st, v2_target_t *tgt, uint32_t buffer, int32_t x, int32_t y,
                           uint16_t px) {
    // Pixel-provenance watch (§9.2 instrument): GS_V2_WATCH="x,y" logs
    // every color-buffer store to that pixel with the state that shaded
    // it — the tool that traces one wrong pixel back to its texture.
    if (__builtin_expect(x == s_watch_x && y == s_watch_y && buffer <= 1u, 0))
        LOG(1, "watch (%d,%d) buf %u px %04X fbzcp=%08X fbz=%08X alpha=%08X t0mode=%08X t0base=%08X", x, y, buffer, px,
            st->fcp, st->fbz, st->amode, st->tmu[0].mode, st->tmu[0].texbase);
    uint32_t at = v2_fb_addr(st, buffer, (uint32_t)x, (uint32_t)y);
    tgt->fb[at] = (uint8_t)px;
    tgt->fb[(at + 1u) & V2_FB_MASK] = (uint8_t)(px >> 8);
}

// ============================================================
// Texture memory
// ============================================================

// DRAM byte address of texel (s,t) at `lod`.
V2_INLINE uint32_t v2_texel_addr(const v2_tmu_state_t *tm, int lod, uint32_t s, uint32_t t) {
    uint32_t texel_bytes = tm->is8 ? 1u : 2u;
    uint32_t addr = tm->lod_base[lod] + (t * tm->lod_w[lod] + s) * texel_bytes;
    return addr & tm->mask;
}

// A texture-aperture write [V2 p.119].  The PCI address is a FIELD
// ENCODING, not a byte address: {TREX[22:21], LOD[20:17], T[16:9],
// S[8:2], 0[1:0]} — two 16-bit or four 8-bit texels per 32-bit write,
// with S right-aligned to bit 2 and T to bit 9 for smaller maps.
static void v2_tex_write(const v2_draw_state_t *st, v2_target_t *tgt, uint32_t off, uint32_t le_value) {
    int tmu = (off >> 21) & 3u;
    if (tmu >= V2_RASTER_TMUS)
        tmu &= 1; // only two Bruces are populated; the third select aliases
    const v2_tmu_state_t *tm = &st->tmu[tmu];
    // tLOD[25]/[26]: the texture path's own swizzle and word swap, in
    // that order (swizzle first — V2 p.83).
    if (tm->tlod & (1u << 25))
        le_value = __builtin_bswap32(le_value);
    if (tm->tlod & (1u << 26))
        le_value = (le_value >> 16) | (le_value << 16);
    uint32_t lod = (off >> 17) & 0xFu;
    if (lod >= V2_RASTER_LODS)
        lod = V2_RASTER_LODS - 1u; // the address field can name levels past the chain
    uint32_t t = (off >> 9) & 0xFFu;
    uint8_t *ram = tgt->tex[tmu];
    uint32_t mask = tm->mask;
    uint32_t w = tm->lod_w[lod];
    if (!tm->is8) {
        // Two 16-bit texels: S[0] from the halves, S[7:1] from bits 8:2.
        uint32_t s0 = ((off >> 2) & 0x7Fu) << 1;
        for (uint32_t half = 0; half < 2u; half++) {
            uint32_t s = s0 + half;
            if (s >= w)
                continue; // narrow maps inhibit the upper texels
            uint32_t at = v2_texel_addr(tm, (int)lod, s, t) & mask;
            uint16_t px = (uint16_t)(le_value >> (16u * half));
            ram[at] = (uint8_t)px;
            ram[(at + 1u) & mask] = (uint8_t)(px >> 8);
        }
    } else if (tm->mode & (1u << 31)) {
        // seq_8_downld (textureMode[31]): four sequential 8-bit texels
        // per word packed CONTIGUOUSLY — S[8:2] decodes from address
        // bits 8:2, unlike the legacy even-address mode below where
        // each word occupies an 8-byte slot (S from bits 8:3).  The
        // vendor's download code switches its address shift between
        // the two modes (sh=2 vs sh=3) [Glide-src gtexdl.c].  Decoding
        // seq-8 with the legacy shift makes every second word of a row
        // overwrite the previous one: half of each 8-bit texture stale
        // — Quake's LIGHTMAP atlases (I8, uploaded seq-8 by the Mac
        // driver) came out shredded, surfaces went near-black under
        // their broken lightmaps, while all 16-bit textures stayed
        // fine.
        uint32_t s0 = ((off >> 2) & 0x7Fu) << 2;
        for (uint32_t i = 0; i < 4u; i++) {
            uint32_t s = s0 + i;
            if (s >= w)
                continue;
            ram[v2_texel_addr(tm, (int)lod, s, t) & mask] = (uint8_t)(le_value >> (8u * i));
        }
    } else {
        // Even-address 8-bit download: four texels, S[1:0] from the
        // byte lanes and S[7:2] from bits 8:3 (s[1] forced 0 in the
        // encoding — V2 p.119).
        uint32_t s0 = ((off >> 3) & 0x3Fu) << 2;
        for (uint32_t i = 0; i < 4u; i++) {
            uint32_t s = s0 + i;
            if (s >= w)
                continue;
            ram[v2_texel_addr(tm, (int)lod, s, t) & mask] = (uint8_t)(le_value >> (8u * i));
        }
    }
}

// --- NCC / palette decode ---------------------------------------------------

// Decompress one 8-bit YIQ (4-2-2) texel through the selected NCC table
// [V2 §5.92]: Y indexes the 16-entry Y ramp, I and Q index four 9-bit
// signed RGB deltas each; sum and clamp.
static uint32_t v2_ncc_decode(const v2_target_t *tgt, int tmu, int table, uint8_t texel) {
    const uint32_t *n = tgt->ncc[tmu][table];
    uint32_t y = (texel >> 4) & 0xFu;
    uint32_t i = (texel >> 2) & 0x3u;
    uint32_t q = texel & 0x3u;
    int32_t yv = (int32_t)((n[y >> 2] >> (8u * (y & 3u))) & 0xFFu);
    // I/Q entries: {r[8:0], g[8:0], b[8:0]} in 26:0, 9-bit signed each.
    int32_t ir = (int32_t)((n[4 + i] >> 18) & 0x1FFu) << 23 >> 23;
    int32_t ig = (int32_t)((n[4 + i] >> 9) & 0x1FFu) << 23 >> 23;
    int32_t ib = (int32_t)(n[4 + i] & 0x1FFu) << 23 >> 23;
    int32_t qr = (int32_t)((n[8 + q] >> 18) & 0x1FFu) << 23 >> 23;
    int32_t qg = (int32_t)((n[8 + q] >> 9) & 0x1FFu) << 23 >> 23;
    int32_t qb = (int32_t)(n[8 + q] & 0x1FFu) << 23 >> 23;
    int32_t r = yv + ir + qr, g = yv + ig + qg, b = yv + ib + qb;
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// Expand one raw texel to 32-bit ARGB per the tformat table (V2 p.81).
// Format 10's green expansion is printed there as {g[5:0], r[5:4]} —
// resolved as the obvious typo for {g[5:0], g[5:4]} (proposal §8 Q5).
static inline uint32_t v2_texel_expand(const v2_tmu_state_t *tm, const v2_target_t *tgt, int tmu, uint32_t raw) {
    uint32_t fmt = tm->fmt;
    int table = tm->ncc_table;
    uint32_t a, r, g, b, p;
    switch (fmt) {
    case 0: // 8-bit RGB 3-3-2
        r = (raw >> 5) & 7u;
        g = (raw >> 2) & 7u;
        b = raw & 3u;
        return 0xFF000000u | (((r << 5) | (r << 2) | (r >> 1)) << 16) | (((g << 5) | (g << 2) | (g >> 1)) << 8) |
               (b << 6) | (b << 4) | (b << 2) | b;
    case 1: // 8-bit YIQ
        return 0xFF000000u | v2_ncc_decode(tgt, tmu, table, (uint8_t)raw);
    case 2: // 8-bit alpha
        return (raw << 24) | (raw << 16) | (raw << 8) | raw;
    case 3: // 8-bit intensity
        return 0xFF000000u | (raw << 16) | (raw << 8) | raw;
    case 4: // 8-bit alpha-intensity 4-4
        a = (raw >> 4) & 0xFu;
        g = raw & 0xFu;
        a = (a << 4) | a;
        g = (g << 4) | g;
        return (a << 24) | (g << 16) | (g << 8) | g;
    case 5: // 8-bit palette to RGB
        return 0xFF000000u | (tgt->palette[tmu][raw & 0xFFu] & 0xFFFFFFu);
    case 6: { // 8-bit palette to RGBA (the P6 bit-slicing of V2 p.81)
        p = tgt->palette[tmu][raw & 0xFFu];
        uint32_t pr = (p >> 16) & 0xFFu, pg = (p >> 8) & 0xFFu, pb = p & 0xFFu;
        a = ((pr >> 2) << 2) | (pr >> 6);
        r = ((pr & 3u) << 6) | ((pg >> 4) << 2) | (pr & 3u);
        g = ((pg & 0xFu) << 4) | ((pb >> 6) << 2) | ((pg >> 2) & 3u);
        b = ((pb & 0x3Fu) << 2) | ((pb >> 4) & 3u);
        return (a << 24) | (r << 16) | (g << 8) | b;
    }
    case 8: // 16-bit ARGB 8-3-3-2
        a = (raw >> 8) & 0xFFu;
        r = (raw >> 5) & 7u;
        g = (raw >> 2) & 7u;
        b = raw & 3u;
        return (a << 24) | (((r << 5) | (r << 2) | (r >> 1)) << 16) | (((g << 5) | (g << 2) | (g >> 1)) << 8) |
               (b << 6) | (b << 4) | (b << 2) | b;
    case 9: // 16-bit AYIQ
        return ((raw >> 8) << 24) | v2_ncc_decode(tgt, tmu, table, (uint8_t)raw);
    case 10: // 16-bit RGB 5-6-5
        r = (raw >> 11) & 0x1Fu;
        g = (raw >> 5) & 0x3Fu;
        b = raw & 0x1Fu;
        return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | (b << 3) | (b >> 2);
    case 11: // 16-bit ARGB 1-5-5-5
        a = (raw >> 15) ? 0xFFu : 0u;
        r = (raw >> 10) & 0x1Fu;
        g = (raw >> 5) & 0x1Fu;
        b = raw & 0x1Fu;
        return (a << 24) | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | (b << 3) | (b >> 2);
    case 12: // 16-bit ARGB 4-4-4-4
        a = (raw >> 12) & 0xFu;
        r = (raw >> 8) & 0xFu;
        g = (raw >> 4) & 0xFu;
        b = raw & 0xFu;
        return (((a << 4) | a) << 24) | (((r << 4) | r) << 16) | (((g << 4) | g) << 8) | (b << 4) | b;
    case 13: // 16-bit alpha-intensity 8-8
        a = (raw >> 8) & 0xFFu;
        g = raw & 0xFFu;
        return (a << 24) | (g << 16) | (g << 8) | g;
    case 14: // 16-bit alpha-palette 8-8
        return (((raw >> 8) & 0xFFu) << 24) | (tgt->palette[tmu][raw & 0xFFu] & 0xFFFFFFu);
    default: // 7, 15: reserved — deterministically opaque black
        return 0xFF000000u;
    }
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

// The expansion cache for the 8-bit formats: 256 entries built by the
// normative v2_texel_expand whenever the (format, NCC select, palette/
// NCC generation) key changes — exact by construction, and the palette
// and YIQ decodes leave the per-texel path.
static const uint32_t *v2_expand_lut(const v2_tmu_state_t *tm, v2_target_t *tgt, int tmu) {
    uint32_t key = 1u | ((uint32_t)tm->fmt << 1) | ((uint32_t)tm->ncc_table << 5) | (tgt->pal_gen[tmu] << 8);
    if (tgt->lut_key[tmu] != key) {
        for (uint32_t raw = 0; raw < 256u; raw++)
            tgt->lut[tmu][raw] = v2_texel_expand(tm, tgt, tmu, raw);
        tgt->lut_key[tmu] = key;
    }
    return tgt->lut[tmu];
}

// Exact powers of two: s * s_pow2neg[lod] == ldexp(s, -lod) bit for bit
// (scaling by a power of two is exact in IEEE double; the texel
// coordinates never approach the subnormal range).
static const double s_pow2neg[V2_RASTER_LODS] = {1.0,      1.0 / 2,  1.0 / 4,   1.0 / 8,   1.0 / 16,
                                                 1.0 / 32, 1.0 / 64, 1.0 / 128, 1.0 / 256, 1.0 / 512};

// Sample one TMU at texel-space (s,t) — point or bilinear per the
// filter bits and whether the LOD clamped to lodmin.  The bilinear
// footprint clamps/wraps each coordinate once and fetches the 2x2
// block through the expansion cache for 8-bit formats.
static uint32_t v2_tmu_sample(const v2_tmu_state_t *tm, v2_target_t *tgt, int tmu, double s, double t, int lod,
                              bool magnify) {
    bool bilinear = magnify ? tm->bilin_mag : tm->bilin_min;
    s *= s_pow2neg[lod];
    t *= s_pow2neg[lod];
    uint32_t w = tm->lod_w[lod], h = tm->lod_h[lod];
    const uint32_t *lut = tm->is8 ? v2_expand_lut(tm, tgt, tmu) : NULL;
    if (!bilinear) {
        int32_t si = (int32_t)floor(s), ti = (int32_t)floor(t);
        uint32_t sa = v2_tex_coord(si, w, tm->clamp_s), ta = v2_tex_coord(ti, h, tm->clamp_t);
        uint32_t raw = v2_texel_raw(tm, tgt, tmu, lod, sa, ta);
        uint32_t argb = lut ? lut[raw] : v2_texel_expand(tm, tgt, tmu, raw);
        if (__builtin_expect(s_watch_now, 0))
            LOG(1, "watch texel tmu%d lod%d (s,t)=(%d,%d) addr %06X lodbase %06X raw %04X argb %08X", tmu, lod, (int)sa,
                (int)ta, v2_texel_addr(tm, lod, sa, ta), tm->lod_base[lod], raw, argb);
        return argb;
    }
    // Bilinear: the four closest texels blended by the fractions of the
    // sample point relative to texel centres.
    double fs = s - 0.5, ft = t - 0.5;
    int32_t s0 = (int32_t)floor(fs), t0 = (int32_t)floor(ft);
    uint32_t frac_s = (uint32_t)((fs - s0) * 256.0) & 0xFFu;
    uint32_t frac_t = (uint32_t)((ft - t0) * 256.0) & 0xFFu;
    uint32_t sa = v2_tex_coord(s0, w, tm->clamp_s), sb = v2_tex_coord(s0 + 1, w, tm->clamp_s);
    uint32_t ta = v2_tex_coord(t0, h, tm->clamp_t), tb = v2_tex_coord(t0 + 1, h, tm->clamp_t);
    uint32_t r00 = v2_texel_raw(tm, tgt, tmu, lod, sa, ta), r10 = v2_texel_raw(tm, tgt, tmu, lod, sb, ta);
    uint32_t r01 = v2_texel_raw(tm, tgt, tmu, lod, sa, tb), r11 = v2_texel_raw(tm, tgt, tmu, lod, sb, tb);
    uint32_t c00, c10, c01, c11;
    if (lut) {
        c00 = lut[r00];
        c10 = lut[r10];
        c01 = lut[r01];
        c11 = lut[r11];
    } else {
        c00 = v2_texel_expand(tm, tgt, tmu, r00);
        c10 = v2_texel_expand(tm, tgt, tmu, r10);
        c01 = v2_texel_expand(tm, tgt, tmu, r01);
        c11 = v2_texel_expand(tm, tgt, tmu, r11);
    }
    if (__builtin_expect(s_watch_now, 0)) {
        LOG(1, "watch texel tmu%d lod%d (s,t)=(%d,%d) addr %06X lodbase %06X raw %04X argb %08X", tmu, lod, (int)sa,
            (int)ta, v2_texel_addr(tm, lod, sa, ta), tm->lod_base[lod], r00, c00);
        LOG(1, "watch texel tmu%d lod%d (s,t)=(%d,%d) addr %06X lodbase %06X raw %04X argb %08X", tmu, lod, (int)sb,
            (int)ta, v2_texel_addr(tm, lod, sb, ta), tm->lod_base[lod], r10, c10);
        LOG(1, "watch texel tmu%d lod%d (s,t)=(%d,%d) addr %06X lodbase %06X raw %04X argb %08X", tmu, lod, (int)sa,
            (int)tb, v2_texel_addr(tm, lod, sa, tb), tm->lod_base[lod], r01, c01);
        LOG(1, "watch texel tmu%d lod%d (s,t)=(%d,%d) addr %06X lodbase %06X raw %04X argb %08X", tmu, lod, (int)sb,
            (int)tb, v2_texel_addr(tm, lod, sb, tb), tm->lod_base[lod], r11, c11);
    }
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        uint32_t a = (c00 >> sh) & 0xFFu, bch = (c10 >> sh) & 0xFFu;
        uint32_t c = (c01 >> sh) & 0xFFu, d = (c11 >> sh) & 0xFFu;
        uint32_t top = (a * (256u - frac_s) + bch * frac_s) >> 8;
        uint32_t bot = (c * (256u - frac_s) + d * frac_s) >> 8;
        out |= (((top * (256u - frac_t) + bot * frac_t) >> 8) & 0xFFu) << sh;
    }
    return out;
}

// ============================================================
// The pixel pipeline — the fixed order of V2 p.15
// ============================================================
// texture (TMU1 -> TMU0) -> chroma -> colour/alpha combine -> fog ->
// alpha test -> depth test -> alpha blend -> dither -> write masks.
// All combine units share one 9x9 multiply shape (V2 pp.37-39, p.82):
// truncate, no rounding, clamp 0-$FF.

// The shared combine-unit shape: ((other - sub) * factor) >> 8 + add,
// clamped, optionally inverted.  factor = reverse ? m+1 : 256-m — the
// diagrams' XOR-with-NOT-reverse plus one.
V2_INLINE uint32_t v2_combine(uint32_t other, uint32_t local, uint32_t m, uint32_t ctl_bits, uint32_t add_val) {
    bool zero_other = ctl_bits & 1u;
    bool sub_local = ctl_bits & 2u;
    bool reverse = ctl_bits & 4u;
    bool invert = ctl_bits & 8u;
    int32_t acc = (int32_t)(zero_other ? 0u : other) - (int32_t)(sub_local ? local : 0u);
    uint32_t f = (reverse ? m : (m ^ 0xFFu)) + 1u;
    int32_t o = ((acc * (int32_t)f) >> 8) + (int32_t)add_val;
    o = o < 0 ? 0 : (o > 255 ? 255 : o);
    return invert ? ((uint32_t)o ^ 0xFFu) : (uint32_t)o;
}

// Ordered-dither matrices.  The spec names 4x4 and 2x2 ordered dither
// (fbzMode[8]/[11]) but does not print the matrices; these are the
// classic Bayer orders, and the rule below is CHOSEN (documented in
// voodoo2.md's divergence list): threshold on the truncated remainder,
// monotonic and mean-preserving.
static const uint8_t v2_dither4[4][4] = {
    {0,  8,  2,  10},
    {12, 4,  14, 6 },
    {3,  11, 1,  9 },
    {15, 7,  13, 5 }
};
static const uint8_t v2_dither2[2][2] = {
    {0, 2},
    {3, 1}
};

// The dither rule tabulated: s_dith5[d][v] = (v*31 + 15*d) / 255 and
// s_dith6[d][v] = (v*63 + 13*d) / 255 for every threshold d (0..15) and
// channel value v — built once from the SAME expressions, so the table
// is exact by construction and the two integer divisions leave the
// per-pixel path (walker proposal §3.5).
static uint8_t s_dith5[16][256];
static uint8_t s_dith6[16][256];

static void v2_dither_tables_init(void) {
    static bool ready;
    if (ready)
        return;
    for (uint32_t d = 0; d < 16u; d++) {
        for (uint32_t v = 0; v < 256u; v++) {
            s_dith5[d][v] = (uint8_t)((v * 31u + 15u * d) / 255u);
            s_dith6[d][v] = (uint8_t)((v * 63u + 13u * d) / 255u);
        }
    }
    ready = true;
}

V2_INLINE uint16_t v2_pack565(const v2_draw_state_t *st, int32_t x, int32_t y, uint32_t r, uint32_t g, uint32_t b) {
    uint32_t r5, g6, b5;
    if (st->fbz & 0x100u) { // dithering enabled
        // Linear-rescale ordered dither (CHOSEN — the spec names the
        // modes but not the matrices; voodoo2.md's divergence list).
        // The rule must keep the SUM over one 4x4 tile strictly
        // increasing in the input value with no plateau at either end:
        // Glide calibrates its un-dither tables by rendering each of
        // the 256 values and requiring the 4x4 pixel sums to be UNIQUE
        // (initSumTables' "non-unique r_sum" check), so a dither that
        // saturates early fails the real driver's own self-test.
        // The threshold multipliers (15 for the 5-bit channels, 13 for
        // the 6-bit green) are the values for which the whole property
        // set holds over all 256 inputs — verified exhaustively: unique
        // strictly-increasing tile sums, 0 maps to all-0, 255 to
        // all-max.
        uint32_t d = (st->fbz & 0x800u) ? v2_dither2[y & 1][x & 1] * 4u : v2_dither4[y & 3][x & 3];
        r5 = s_dith5[d][r & 0xFFu];
        g6 = s_dith6[d][g & 0xFFu];
        b5 = s_dith5[d][b & 0xFFu];
    } else {
        r5 = r >> 3;
        g6 = g >> 2;
        b5 = b >> 3;
    }
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

// The chosen 1/W -> 4.12 inverted-mantissa float (fbzMode[3]); the exact
// hardware normalisation is not in our material, so this is documented
// as chosen: exponent counts leading zeros below 1.0, mantissa is the
// next 12 bits inverted so integer comparisons keep their sense.
V2_INLINE uint16_t v2_depth_float(int64_t val) {
    if (val <= 0)
        return 0;
    if (val >= (1ll << 30))
        return 0;
    uint64_t m = (uint64_t)val;
    int e = 0;
    while (m < (1ull << 29) && e < 15) {
        m <<= 1;
        e++;
    }
    uint16_t mant = (uint16_t)((m >> 17) & 0xFFFu);
    return (uint16_t)(((uint32_t)e << 12) | (~mant & 0xFFFu));
}

// One alpha-blend factor (V2 §5.19.2), per channel where needed.
V2_INLINE uint32_t v2_blend_factor(uint32_t code, bool is_src, uint32_t src_a, uint32_t dst_a, uint32_t other_c,
                                   uint32_t prefog_c) {
    switch (code) {
    case 0x0:
        return 0;
    case 0x1:
        return src_a;
    case 0x2:
        return other_c; // "color": dst colour as src factor, src as dst
    case 0x3:
        return dst_a;
    case 0x4:
        return 255;
    case 0x5:
        return 255u - src_a;
    case 0x6:
        return 255u - other_c;
    case 0x7:
        return 255u - dst_a;
    case 0xF:
        // src: alpha-saturate; dst: colour before fog.
        return is_src ? (src_a < 255u - dst_a ? src_a : 255u - dst_a) : prefog_c;
    default:
        return 0;
    }
}

// factor multiply with the 255->256 promotion so AONE is exact.
V2_INLINE uint32_t v2_blend_mul(uint32_t c, uint32_t f) {
    return (c * (f + (f >> 7))) >> 8;
}

// The back half of the pipeline for one pixel.  Returns true if the
// pixel was written.  The caller has already applied clipping and
// computed the iterated values and the texture chain.
static bool v2_pixel_pipe(const v2_draw_state_t *st, v2_target_t *tgt, v2_pix_t *p) {
    s_watch_now = (p->x == s_watch_x && p->y == s_watch_y);
    uint32_t fbz = st->fbz;
    uint32_t fcp = st->fcp;
    uint32_t amode = st->amode;

    tgt->pixels_in = (tgt->pixels_in + 1u) & 0xFFFFFFu;

    // Stipple (fbzMode[2]): rotate mode uses and rotates bit 31; pattern
    // mode indexes the 4x8 pattern by <x,y> (V2 p.47).
    if (fbz & 4u) {
        bool masked;
        if (fbz & 0x1000u) { // pattern mode
            uint32_t row = (tgt->stipple >> (8u * (p->y & 3))) & 0xFFu;
            masked = !((row >> (7 - (p->x & 7))) & 1u);
        } else {
            masked = !(tgt->stipple >> 31);
            tgt->stipple = (tgt->stipple << 1) | (tgt->stipple >> 31);
        }
        if (masked)
            return false;
    } else if (!(fbz & 0x1000u)) {
        // The stipple register rotates in rotate mode even when masking
        // is disabled (V2 p.47).
        tgt->stipple = (tgt->stipple << 1) | (tgt->stipple >> 31);
    }

    // c_other / a_other selection (fbzColorPath[1:0], [3:2]).
    uint32_t oc_r, oc_g, oc_b, oa;
    switch (fcp & 3u) {
    case 1:
        oc_r = (p->tex_argb >> 16) & 0xFFu;
        oc_g = (p->tex_argb >> 8) & 0xFFu;
        oc_b = p->tex_argb & 0xFFu;
        break;
    case 2:
        oc_r = (st->color1 >> 16) & 0xFFu;
        oc_g = (st->color1 >> 8) & 0xFFu;
        oc_b = st->color1 & 0xFFu;
        break;
    default:
        oc_r = p->r;
        oc_g = p->g;
        oc_b = p->b;
        break;
    }
    switch ((fcp >> 2) & 3u) {
    case 1:
        oa = p->tex_argb >> 24;
        break;
    case 2:
        oa = st->color1 >> 24;
        break;
    default:
        oa = p->a;
        break;
    }

    // Chroma-key / chroma-range on c_other, after texture, before the
    // combine units (V2 p.46).
    if (fbz & 2u) {
        uint32_t key = st->chromakey & 0xFFFFFFu;
        uint32_t c = (oc_r << 16) | (oc_g << 8) | oc_b;
        bool match;
        if (st->chromarange & (1u << 28)) {
            // Range compare: key..range inclusive per channel (the
            // mode bits select in/out of range; the inclusive band is
            // the modelled behaviour).
            uint32_t hi = st->chromarange & 0xFFFFFFu;
            match = oc_b >= (key & 0xFFu) && oc_b <= (hi & 0xFFu) && oc_g >= ((key >> 8) & 0xFFu) &&
                    oc_g <= ((hi >> 8) & 0xFFu) && oc_r >= ((key >> 16) & 0xFFu) && oc_r <= ((hi >> 16) & 0xFFu);
        } else {
            match = c == key;
        }
        if (match) {
            tgt->chroma_fail = (tgt->chroma_fail + 1u) & 0xFFFFFFu;
            return false;
        }
    }

    // c_local / a_local (fbzColorPath[4], [6:5], override [7]).
    uint32_t lc_r, lc_g, lc_b, la;
    bool local_is_c0 = (fcp >> 4) & 1u;
    if ((fcp >> 7) & 1u)
        local_is_c0 = (p->tex_argb >> 31) & 1u; // texture alpha bit 7
    if (local_is_c0) {
        lc_r = (st->color0 >> 16) & 0xFFu;
        lc_g = (st->color0 >> 8) & 0xFFu;
        lc_b = st->color0 & 0xFFu;
    } else {
        lc_r = p->r;
        lc_g = p->g;
        lc_b = p->b;
    }
    switch ((fcp >> 5) & 3u) {
    case 1:
        la = st->color0 >> 24;
        break;
    case 2:
        la = p->z16 >> 8; // clamped iterated Z, high byte (chosen)
        break;
    case 3:
        la = p->w8;
        break;
    default:
        la = p->a;
        break;
    }

    // Colour Combine Unit (fbzColorPath[16:8]).
    uint32_t cc_m_r, cc_m_g, cc_m_b;
    switch ((fcp >> 10) & 7u) {
    case 1:
        cc_m_r = lc_r;
        cc_m_g = lc_g;
        cc_m_b = lc_b;
        break;
    case 2:
        cc_m_r = cc_m_g = cc_m_b = oa;
        break;
    case 3:
        cc_m_r = cc_m_g = cc_m_b = la;
        break;
    case 4:
        cc_m_r = cc_m_g = cc_m_b = p->tex_argb >> 24;
        break;
    case 5:
        cc_m_r = (p->tex_argb >> 16) & 0xFFu;
        cc_m_g = (p->tex_argb >> 8) & 0xFFu;
        cc_m_b = p->tex_argb & 0xFFu;
        break;
    default:
        cc_m_r = cc_m_g = cc_m_b = 0;
        break;
    }
    uint32_t cc_ctl = ((fcp >> 8) & 3u) | (((fcp >> 13) & 1u) << 2) | (((fcp >> 16) & 1u) << 3);
    // The add mux: bit14 = cc_add_clocal, bit15 = cc_add_alocal.
    uint32_t add_r, add_g, add_b;
    if ((fcp >> 15) & 1u) {
        add_r = add_g = add_b = la;
    } else if ((fcp >> 14) & 1u) {
        add_r = lc_r;
        add_g = lc_g;
        add_b = lc_b;
    } else {
        add_r = add_g = add_b = 0;
    }
    uint32_t out_r = v2_combine(oc_r, lc_r, cc_m_r, cc_ctl, add_r);
    uint32_t out_g = v2_combine(oc_g, lc_g, cc_m_g, cc_ctl, add_g);
    uint32_t out_b = v2_combine(oc_b, lc_b, cc_m_b, cc_ctl, add_b);

    // Alpha Combine Unit (fbzColorPath[25:17]).
    uint32_t ca_m;
    switch ((fcp >> 19) & 7u) {
    case 1:
    case 3:
        ca_m = la;
        break;
    case 2:
        ca_m = oa;
        break;
    case 4:
        ca_m = p->tex_argb >> 24;
        break;
    default:
        ca_m = 0;
        break;
    }
    uint32_t ca_ctl = ((fcp >> 17) & 3u) | (((fcp >> 22) & 1u) << 2) | (((fcp >> 25) & 1u) << 3);
    uint32_t ca_add = ((fcp >> 24) & 1u) ? la : (((fcp >> 23) & 1u) ? la : 0u);
    uint32_t out_a = v2_combine(oa, la, ca_m, ca_ctl, ca_add);

    uint32_t prefog_r = out_r, prefog_g = out_g, prefog_b = out_b;

    // Fog (fogMode; V2 §5.18).  The table indexing normalisation is
    // chosen (documented): 4-bit exponent of 1/W below 1.0, next two
    // bits of mantissa.
    uint32_t fog = st->fogmode;
    if (fog & 1u) {
        uint32_t fa; // fog alpha
        switch ((fog >> 3) & 3u) {
        case 1:
            fa = p->a;
            break;
        case 2:
            fa = p->z16 >> 8;
            break;
        case 3:
            fa = p->w8;
            break;
        default: {
            int64_t w = p->w_raw;
            uint32_t idx = 0;
            if (w > 0 && w < (1ll << 30)) {
                uint64_t m = (uint64_t)w;
                int e = 0;
                while (m < (1ull << 29) && e < 15) {
                    m <<= 1;
                    e++;
                }
                idx = ((uint32_t)e << 2) | (uint32_t)((m >> 27) & 3u);
                if (idx > 63u)
                    idx = 63u;
            }
            // Two entries per fogTable word; the alpha is the entry's
            // high byte, the low byte its 6.2 delta (interpolation not
            // modelled — chosen).
            uint32_t word = st->fogtable[idx >> 1];
            fa = ((idx & 1u) ? (word >> 24) : (word >> 8)) & 0xFFu;
            break;
        }
        }
        uint32_t fr = (st->fogcolor >> 16) & 0xFFu;
        uint32_t fg = (st->fogcolor >> 8) & 0xFFu;
        uint32_t fb = st->fogcolor & 0xFFu;
        bool fogadd_zero = (fog >> 1) & 1u; // 1 = add zero instead of fog colour
        bool fogmult_zero = (fog >> 2) & 1u; // 1 = multiply zero instead of Cin
        uint32_t add_r2 = fogadd_zero ? 0u : v2_blend_mul(fr, fa);
        uint32_t add_g2 = fogadd_zero ? 0u : v2_blend_mul(fg, fa);
        uint32_t add_b2 = fogadd_zero ? 0u : v2_blend_mul(fb, fa);
        uint32_t mul_r = fogmult_zero ? 0u : v2_blend_mul(out_r, 255u - fa);
        uint32_t mul_g = fogmult_zero ? 0u : v2_blend_mul(out_g, 255u - fa);
        uint32_t mul_b = fogmult_zero ? 0u : v2_blend_mul(out_b, 255u - fa);
        out_r = mul_r + add_r2 > 255u ? 255u : mul_r + add_r2;
        out_g = mul_g + add_g2 > 255u ? 255u : mul_g + add_g2;
        out_b = mul_b + add_b2 > 255u ? 255u : mul_b + add_b2;
    }
    if (s_watch_now)
        LOG(1, "watch pipe tex=%08X iter=(%u,%u,%u,%u) cc=(%u,%u,%u) fogmode=%08X fogcolor=%08X post=(%u,%u,%u)",
            p->tex_argb, p->r, p->g, p->b, p->a, prefog_r, prefog_g, prefog_b, fog, st->fogcolor, out_r, out_g, out_b);

    // Alpha test (alphaMode[3:0]) and the alpha-channel mask
    // (fbzMode[13]) — both count fbiAfuncFail on rejection.
    if (amode & 1u) {
        uint32_t ref = amode >> 24;
        bool pass;
        switch ((amode >> 1) & 7u) {
        case 0:
            pass = false;
            break;
        case 1:
            pass = out_a < ref;
            break;
        case 2:
            pass = out_a == ref;
            break;
        case 3:
            pass = out_a <= ref;
            break;
        case 4:
            pass = out_a > ref;
            break;
        case 5:
            pass = out_a != ref;
            break;
        case 6:
            pass = out_a >= ref;
            break;
        default:
            pass = true;
            break;
        }
        if (!pass) {
            tgt->afunc_fail = (tgt->afunc_fail + 1u) & 0xFFFFFFu;
            return false;
        }
    }
    if ((fbz & 0x2000u) && !(out_a & 1u)) {
        tgt->afunc_fail = (tgt->afunc_fail + 1u) & 0xFFFFFFu;
        return false;
    }

    // Depth test (fbzMode[7:3], [16], [20], [21]; V2 §5.20.1).
    uint32_t depth_write = p->z16;
    if (fbz & 0x10u) {
        uint32_t src;
        if (fbz & 8u)
            src = v2_depth_float((fbz & 0x200000u) ? p->z_raw : p->w_raw);
        else
            src = p->z16;
        if (fbz & 0x10000u) { // depth bias, signed zaColor[15:0]
            int32_t biased = (int32_t)src + (int16_t)(st->zacolor & 0xFFFFu);
            src = biased < 0 ? 0u : (biased > 0xFFFF ? 0xFFFFu : (uint32_t)biased);
        }
        depth_write = src;
        uint32_t cmp_src = (fbz & 0x100000u) ? (st->zacolor & 0xFFFFu) : src;
        uint32_t dst = v2_fb_load(st, tgt, 3u, p->x, p->y);
        bool pass;
        switch ((fbz >> 5) & 7u) {
        case 0:
            pass = false;
            break;
        case 1:
            pass = cmp_src < dst;
            break;
        case 2:
            pass = cmp_src == dst;
            break;
        case 3:
            pass = cmp_src <= dst;
            break;
        case 4:
            pass = cmp_src > dst;
            break;
        case 5:
            pass = cmp_src != dst;
            break;
        case 6:
            pass = cmp_src >= dst;
            break;
        default:
            pass = true;
            break;
        }
        if (!pass) {
            tgt->zfunc_fail = (tgt->zfunc_fail + 1u) & 0xFFFFFFu;
            return false;
        }
    }

    uint32_t draw_buf = (fbz >> 14) & 3u;
    if (draw_buf > 1u)
        draw_buf = 0;

    // Alpha blend (alphaMode[4], factors [23:8]).
    if (amode & 0x10u) {
        uint16_t dst565 = v2_fb_load(st, tgt, draw_buf, p->x, p->y);
        uint32_t dr = ((dst565 >> 11) & 0x1Fu);
        uint32_t dg = ((dst565 >> 5) & 0x3Fu);
        uint32_t db = dst565 & 0x1Fu;
        dr = (dr << 3) | (dr >> 2);
        dg = (dg << 2) | (dg >> 4);
        db = (db << 3) | (db >> 2);
        uint32_t dst_a = 255; // destination alpha planes not enabled
        uint32_t sf = (amode >> 8) & 0xFu, df = (amode >> 12) & 0xFu;
        uint32_t nr = v2_blend_mul(out_r, v2_blend_factor(sf, true, out_a, dst_a, dr, 0)) +
                      v2_blend_mul(dr, v2_blend_factor(df, false, out_a, dst_a, out_r, prefog_r));
        uint32_t ng = v2_blend_mul(out_g, v2_blend_factor(sf, true, out_a, dst_a, dg, 0)) +
                      v2_blend_mul(dg, v2_blend_factor(df, false, out_a, dst_a, out_g, prefog_g));
        uint32_t nb = v2_blend_mul(out_b, v2_blend_factor(sf, true, out_a, dst_a, db, 0)) +
                      v2_blend_mul(db, v2_blend_factor(df, false, out_a, dst_a, out_b, prefog_b));
        out_r = nr > 255u ? 255u : nr;
        out_g = ng > 255u ? 255u : ng;
        out_b = nb > 255u ? 255u : nb;
    }

    // Write masks and the stores (fbzMode[9], [10]).
    if (fbz & 0x200u)
        v2_fb_store(st, tgt, draw_buf, p->x, p->y, v2_pack565(st, p->x, p->y, out_r, out_g, out_b));
    if (fbz & 0x400u)
        v2_fb_store(st, tgt, 3u, p->x, p->y, (uint16_t)depth_write);
    tgt->pixels_out = (tgt->pixels_out + 1u) & 0xFFFFFFu;
    return true;
}

// ============================================================
// The software walker — the normative rasteriser backend
// ============================================================
// THE FILL CONVENTION IS CHOSEN, NOT KNOWN (V2 §7.2 defers the walk to
// the SST-1 Programming Guide nobody holds; proposal §4.5, §8 Q1):
// sample points at pixel integer coordinates, half-open top-left edge
// inclusion, orientation from the command's area sign (a sign that
// disagrees with the geometry draws nothing), and parameter iteration
// from vertex A's truncated position.  Goldens record what THIS
// rasteriser draws.

// Clamp/wrap of an accumulated 12.12 colour iterator (V2 p.40).
V2_INLINE uint32_t v2_iter_rgba(int64_t it, bool clamp) {
    if (clamp) {
        int64_t i = it >> 12;
        return i < 0 ? 0u : (i > 255 ? 255u : (uint32_t)i);
    }
    uint32_t ipart = (uint32_t)(it >> 12) & 0xFFFu;
    if (ipart == 0xFFFu)
        return 0u;
    if (ipart == 0x100u)
        return 0xFFu;
    return (uint32_t)(it >> 12) & 0xFFu;
}

// Clamp/wrap of an accumulated 20.12 Z iterator.
V2_INLINE uint32_t v2_iter_z(int64_t it, bool clamp) {
    if (clamp) {
        int64_t i = it >> 12;
        return i < 0 ? 0u : (i > 0xFFFF ? 0xFFFFu : (uint32_t)i);
    }
    uint32_t ipart = (uint32_t)(it >> 12) & 0xFFFFFu;
    if (ipart == 0xFFFFFu)
        return 0u;
    if (ipart == 0x10000u)
        return 0xFFFFu;
    return (uint32_t)(it >> 12) & 0xFFFFu;
}

// Clamped W byte for the ACU/fog inputs (2.30 iterator; V2 pp.40-41).
V2_INLINE uint32_t v2_iter_w8(int64_t it, bool clamp) {
    if (clamp) {
        int64_t i = it >> 30;
        if (i < 0)
            return 0u;
        return it >= (1ll << 30) ? 0xFFu : (uint32_t)((it >> 22) & 0xFFu);
    }
    return (uint32_t)((it >> 22) & 0xFFu);
}

// The texture chain for one pixel: TMU1 samples and combines first, its
// output feeding TMU0's c_other (single-pass multitexture).
static uint32_t v2_texture_chain(const v2_draw_state_t *st, v2_target_t *tgt, const voodoo2_tri_t *T, int32_t dx,
                                 int32_t dy) {
    uint32_t chain = 0; // most-upstream c_other is zero
    for (int tmu = V2_RASTER_TMUS - 1; tmu >= 0; tmu--) {
        const v2_tmu_state_t *tm = &st->tmu[tmu];
        uint32_t mode = tm->mode;
        uint32_t trex1 = tm->trex1;
        if (trex1 & (1u << 18)) {
            // "Send config": the TMU outputs its configuration word as
            // colour instead of texels — the only way software can ask
            // how many TMUs a board has (Glide's getTmuConfigData
            // renders a triangle and un-dithers the result).  The bit
            // layout is Bruce-internal; this encoding is derived from
            // Glide's own decode: 7 bits per TMU at 7n (revision in the
            // low 3), presence announced at bit 7n-1, and select 5
            // returns {fab, new-revision} bytes at 8n.  [3dfx-src
            // info.c; the Bruce spec nobody holds — divergence list]
            uint32_t sel = (trex1 >> 23) & 7u;
            uint32_t contrib = 0;
            if (sel == 0u) {
                contrib = 2u << (7 * tmu); // old revision
                if (tmu >= 1)
                    contrib |= 1u << (7 * tmu - 1); // presence flag
            } else if (sel == 5u) {
                contrib = ((1u << 4) | 1u) << (8 * tmu); // fab 1, rev+3 = 4
            }
            uint32_t rgb = (chain & 0xFFFFFFu) | contrib;
            chain = 0xFF000000u | rgb;
            continue;
        }
        int64_t s_it = T->s[tmu] + T->dsdx[tmu] * dx + T->dsdy[tmu] * dy;
        int64_t t_it = T->t[tmu] + T->dtdx[tmu] * dx + T->dtdy[tmu] * dy;
        int64_t w_it = T->tw[tmu] + T->dtwdx[tmu] * dx + T->dtwdy[tmu] * dy;
        double s, t, s1, t1, s2, t2;
        if (mode & 1u) { // perspective correct
            if ((mode & 8u) && w_it < 0) { // tclampw
                s = t = 0.0;
                s1 = t1 = s2 = t2 = 0.0;
            } else {
                double w = w_it ? (double)w_it : 1.0;
                s = (double)s_it * 4096.0 / w;
                t = (double)t_it * 4096.0 / w;
                double wx = (w_it + T->dtwdx[tmu]) ? (double)(w_it + T->dtwdx[tmu]) : 1.0;
                double wy = (w_it + T->dtwdy[tmu]) ? (double)(w_it + T->dtwdy[tmu]) : 1.0;
                s1 = (double)(s_it + T->dsdx[tmu]) * 4096.0 / wx;
                t1 = (double)(t_it + T->dtdx[tmu]) * 4096.0 / wx;
                s2 = (double)(s_it + T->dsdy[tmu]) * 4096.0 / wy;
                t2 = (double)(t_it + T->dtdy[tmu]) * 4096.0 / wy;
            }
        } else {
            s = (double)s_it / 262144.0;
            t = (double)t_it / 262144.0;
            s1 = s + (double)T->dsdx[tmu] / 262144.0;
            t1 = t + (double)T->dtdx[tmu] / 262144.0;
            s2 = s + (double)T->dsdy[tmu] / 262144.0;
            t2 = t + (double)T->dtdy[tmu] / 262144.0;
        }
        // Per-pixel LOD from the analytic texel-space steps (chosen —
        // the hardware's exact LOD arithmetic is Bruce-spec material we
        // do not hold).  4.2 fixed, biased and clamped per tLOD.
        uint32_t tlod = tm->tlod;
        double stepx = (s1 - s) * (s1 - s) + (t1 - t) * (t1 - t);
        double stepy = (s2 - s) * (s2 - s) + (t2 - t) * (t2 - t);
        double step2 = stepx > stepy ? stepx : stepy;
        int32_t lod4 = 0;
        if (step2 > 1.0)
            lod4 = (int32_t)(2.0 * log2(step2)); // 0.5*log2 in 4.2 units
        int32_t bias = ((int32_t)((tlod >> 12) & 0x3Fu) << 26) >> 26; // 4.2 signed
        lod4 += bias;
        int32_t lodmin = (int32_t)(tlod & 0x3Fu);
        int32_t lodmax = (int32_t)((tlod >> 6) & 0x3Fu);
        if (lodmax > 32)
            lodmax = 32;
        bool magnify = lod4 <= lodmin;
        lod4 = lod4 < lodmin ? lodmin : (lod4 > lodmax ? lodmax : lod4);
        int level = lod4 >> 2;
        if ((tlod >> 19) & 1u) { // split texture: snap to the loaded parity
            uint32_t odd = (tlod >> 18) & 1u;
            if (((uint32_t)level & 1u) != odd)
                level += 1;
        }
        uint32_t texel = v2_tmu_sample(tm, tgt, tmu, s, t, level, magnify);

        // Texture Combine Unit (textureMode[29:12]; V2 p.82): c_local is
        // this TMU's texel, c_other the downstream chain.
        uint32_t lr = (texel >> 16) & 0xFFu, lg = (texel >> 8) & 0xFFu, lb = texel & 0xFFu, lA = texel >> 24;
        uint32_t or_ = (chain >> 16) & 0xFFu, og = (chain >> 8) & 0xFFu, ob = chain & 0xFFu, oA = chain >> 24;
        uint32_t m_r, m_g, m_b, m_a;
        uint32_t lodfrac = (uint32_t)(lod4 & 3u) << 6;
        switch ((mode >> 14) & 7u) {
        case 1:
            m_r = lr;
            m_g = lg;
            m_b = lb;
            break;
        case 2:
            m_r = m_g = m_b = oA;
            break;
        case 3:
            m_r = m_g = m_b = lA;
            break;
        case 4:
            m_r = m_g = m_b = 0xFFu; // detail factor: not modelled, full
            break;
        case 5:
            m_r = m_g = m_b = lodfrac;
            break;
        default:
            m_r = m_g = m_b = 0;
            break;
        }
        switch ((mode >> 23) & 7u) {
        case 1:
        case 3:
            m_a = lA;
            break;
        case 2:
            m_a = oA;
            break;
        case 4:
            m_a = 0xFFu;
            break;
        case 5:
            m_a = lodfrac;
            break;
        default:
            m_a = 0;
            break;
        }
        uint32_t tc_ctl = ((mode >> 12) & 3u) | (((mode >> 17) & 1u) << 2) | (((mode >> 20) & 1u) << 3);
        uint32_t tca_ctl = ((mode >> 21) & 3u) | (((mode >> 26) & 1u) << 2) | (((mode >> 29) & 1u) << 3);
        uint32_t tc_add_r, tc_add_g, tc_add_b;
        if ((mode >> 19) & 1u) {
            tc_add_r = tc_add_g = tc_add_b = lA; // tc_add_alocal
        } else if ((mode >> 18) & 1u) {
            tc_add_r = lr;
            tc_add_g = lg;
            tc_add_b = lb; // tc_add_clocal
        } else {
            tc_add_r = tc_add_g = tc_add_b = 0;
        }
        uint32_t tca_add = (((mode >> 28) & 1u) || ((mode >> 27) & 1u)) ? lA : 0u;
        uint32_t rr = v2_combine(or_, lr, m_r, tc_ctl, tc_add_r);
        uint32_t rg = v2_combine(og, lg, m_g, tc_ctl, tc_add_g);
        uint32_t rb = v2_combine(ob, lb, m_b, tc_ctl, tc_add_b);
        uint32_t ra = v2_combine(oA, lA, m_a, tca_ctl, tca_add);
        chain = (ra << 24) | (rr << 16) | (rg << 8) | rb;
    }
    return chain;
}

static void v2_sw_triangle(const v2_draw_state_t *st, v2_target_t *tgt, const voodoo2_tri_t *T) {
    // Orientation from the COMMAND's sign — a sign that disagrees with
    // the geometry fails every inside test and draws nothing.
    int64_t o = T->area_sign ? -1 : 1;
    int32_t ex[3][4] = {
        {T->ax, T->ay, T->bx, T->by},
        {T->bx, T->by, T->cx, T->cy},
        {T->cx, T->cy, T->ax, T->ay}
    };

    int32_t minx = T->ax, maxx = T->ax, miny = T->ay, maxy = T->ay;
    if (T->bx < minx)
        minx = T->bx;
    if (T->cx < minx)
        minx = T->cx;
    if (T->bx > maxx)
        maxx = T->bx;
    if (T->cx > maxx)
        maxx = T->cx;
    if (T->by < miny)
        miny = T->by;
    if (T->cy < miny)
        miny = T->cy;
    if (T->by > maxy)
        maxy = T->by;
    if (T->cy > maxy)
        maxy = T->cy;

    int32_t cx0 = st->clip_x0, cx1 = st->clip_x1, cy0 = st->clip_y0, cy1 = st->clip_y1;
    int32_t x0 = minx >> 4, x1 = (maxx + 15) >> 4;
    int32_t y0 = miny >> 4, y1 = (maxy + 15) >> 4;
    if (x0 < cx0)
        x0 = cx0;
    if (x1 > cx1)
        x1 = cx1;
    if (y0 < cy0)
        y0 = cy0;
    if (y1 > cy1)
        y1 = cy1;

    bool clamp = (st->fcp >> 28) & 1u;
    bool tex_on = st->tex_on;
    bool y_flip = (st->fbz >> 17) & 1u;
    int32_t ax_i = T->ax >> 4, ay_i = T->ay >> 4;

    for (int32_t y = y0; y < y1; y++) {
        for (int32_t x = x0; x < x1; x++) {
            int32_t sx = x << 4, sy = y << 4;
            bool inside = true;
            for (int e = 0; e < 3 && inside; e++) {
                int64_t dxe = ex[e][2] - ex[e][0], dye = ex[e][3] - ex[e][1];
                int64_t val = dxe * (sy - ex[e][1]) - dye * (sx - ex[e][0]);
                int64_t t = o * val;
                if (t > 0)
                    continue;
                if (t < 0) {
                    inside = false;
                } else {
                    // Top-left inclusion, derived for this cross/orient
                    // convention: a "left" edge descends (o*dy < 0 in
                    // this sign convention), a "top" edge is horizontal
                    // with o*dx > 0.
                    bool topleft = (o * dye < 0) || (dye == 0 && o * dxe > 0);
                    inside = topleft;
                }
            }
            if (!inside)
                continue;
            int32_t dx = x - ax_i, dy = y - ay_i;
            v2_pix_t p;
            p.x = x;
            p.y = y_flip ? (int32_t)st->screen_h - 1 - y : y;
            p.r = v2_iter_rgba(T->r + (int64_t)T->drdx * dx + (int64_t)T->drdy * dy, clamp);
            p.g = v2_iter_rgba(T->g + (int64_t)T->dgdx * dx + (int64_t)T->dgdy * dy, clamp);
            p.b = v2_iter_rgba(T->b + (int64_t)T->dbdx * dx + (int64_t)T->dbdy * dy, clamp);
            p.a = v2_iter_rgba(T->a + (int64_t)T->dadx * dx + (int64_t)T->dady * dy, clamp);
            p.z_raw = T->z + (int64_t)T->dzdx * dx + (int64_t)T->dzdy * dy;
            p.z16 = v2_iter_z(p.z_raw, clamp);
            p.w_raw = T->w + T->dwdx * dx + T->dwdy * dy;
            p.w8 = v2_iter_w8(p.w_raw, clamp);
            p.have_tex = tex_on;
            s_watch_now = (p.x == s_watch_x && p.y == s_watch_y);
            p.tex_argb = tex_on ? v2_texture_chain(st, tgt, T, dx, dy) : 0u;
            v2_pixel_pipe(st, tgt, &p);
        }
    }
}

static void v2_sw_fastfill(const v2_draw_state_t *st, v2_target_t *tgt) {
    // FASTFILL clears the clip rectangle with color1 (dithered) and/or
    // zaColor[15:0], honouring only the write masks and draw-buffer
    // select — the depth/alpha/blend stages are bypassed (V2 §5.24).
    int32_t x0 = st->fill_x0, x1 = st->fill_x1, y0 = st->fill_y0, y1 = st->fill_y1;
    uint32_t fbz = st->fbz;
    uint32_t draw_buf = (fbz >> 14) & 3u;
    if (draw_buf > 1u)
        draw_buf = 0;
    bool y_flip = (fbz >> 17) & 1u;
    uint32_t cr = (st->color1 >> 16) & 0xFFu;
    uint32_t cg = (st->color1 >> 8) & 0xFFu;
    uint32_t cb = st->color1 & 0xFFu;
    uint16_t za = (uint16_t)(st->zacolor & 0xFFFFu);
    for (int32_t y = y0; y < y1; y++) {
        int32_t py = y_flip ? (int32_t)st->screen_h - 1 - y : y;
        for (int32_t x = x0; x < x1; x++) {
            if (fbz & 0x200u)
                v2_fb_store(st, tgt, draw_buf, x, py, v2_pack565(st, x, py, cr, cg, cb));
            if (fbz & 0x400u)
                v2_fb_store(st, tgt, 3u, x, py, za);
        }
    }
}

// One pixel entering the LFB path: either written raw (bypass — only
// dithering applies) or pushed through the full pixel pipeline with its
// depth/alpha from the data or zaColor (lfbMode[8]; V2 p.51-52).
static void v2_lfb_pixel(const v2_draw_state_t *st, v2_target_t *tgt, const v2_cmd_t *cmd) {
    uint32_t buffer = cmd->u.lfb.buffer, x = cmd->u.lfb.x, y = cmd->u.lfb.y;
    uint32_t r = cmd->u.lfb.r, g = cmd->u.lfb.g, b = cmd->u.lfb.b, a = cmd->u.lfb.a;
    uint32_t mode = st->lfbmode;
    if (!((mode >> 8) & 1u)) { // not pipeline-processed: the bypass
        if (cmd->u.lfb.write_color)
            v2_fb_store(st, tgt, buffer, (int32_t)x, (int32_t)y, v2_pack565(st, (int32_t)x, (int32_t)y, r, g, b));
        if (cmd->u.lfb.write_z)
            v2_fb_store(st, tgt, 3u, (int32_t)x, (int32_t)y, cmd->u.lfb.z);
        return;
    }
    // Pipeline-processed: the Y origin is fbzMode[17]'s, clipping
    // applies, and depth/alpha default to zaColor when the format
    // carried none.
    int32_t px = (int32_t)x, py = (int32_t)y;
    if ((st->fbz >> 17) & 1u)
        py = (int32_t)st->screen_h - 1 - py;
    if (px < st->clip_x0 || px >= st->clip_x1 || py < st->clip_y0 || py >= st->clip_y1)
        return;
    v2_pix_t p;
    p.x = px;
    p.y = py;
    p.r = r;
    p.g = g;
    p.b = b;
    p.a = a;
    p.z16 = cmd->u.lfb.has_z ? cmd->u.lfb.z : (uint16_t)(st->zacolor & 0xFFFFu);
    p.z_raw = (int64_t)p.z16 << 12;
    // The W handed to the pipeline: the 16 MSBs of the fraction come
    // from the pixel's Z or from zaColor per lfbMode[14] (V2 p.53).
    uint16_t wsrc = (mode & 0x4000u) ? (uint16_t)(st->zacolor & 0xFFFFu) : (uint16_t)p.z16;
    p.w_raw = (int64_t)wsrc << 14;
    p.w8 = v2_iter_w8(p.w_raw, (st->fcp >> 28) & 1u);
    p.tex_argb = 0;
    p.have_tex = false;
    v2_pixel_pipe(st, tgt, &p);
}

// The 2D engine's SGRAM block fill (bltCommand FRECTFILL + GO), in page
// space: each "row" is one 4 KB page, x counts 8-byte units, y counts
// pages, and the 16-bit bltColor is replicated across the span
// [Glide-src incsrc/cvgdefs.h SSTG_*].  A memory fill, not the pixel
// pipeline: no stats counters.
static void v2_blt_fill(v2_target_t *tgt, const v2_cmd_t *cmd) {
    uint32_t base = cmd->u.blt.base, x0 = cmd->u.blt.x0, y0 = cmd->u.blt.y0;
    uint16_t color = cmd->u.blt.color;
    for (uint32_t r = 0; r < cmd->u.blt.rows; r++) {
        uint32_t addr = base + (y0 + r) * 4096u + x0 * 8u;
        for (uint32_t i = 0; i < cmd->u.blt.units * 4u; i++) { // four 565 pixels per unit
            uint32_t a = (addr + i * 2u) & V2_FB_MASK;
            tgt->fb[a] = (uint8_t)color;
            tgt->fb[(a + 1u) & V2_FB_MASK] = (uint8_t)(color >> 8);
        }
    }
}

// ============================================================
// The executor entry
// ============================================================

void v2_raster_execute(const v2_draw_state_t *st, v2_target_t *tgt, const v2_cmd_t *cmd) {
    switch ((v2_cmd_kind_t)cmd->kind) {
    case V2_CMD_TRIANGLE:
        v2_sw_triangle(st, tgt, &cmd->u.tri);
        break;
    case V2_CMD_FASTFILL:
        v2_sw_fastfill(st, tgt);
        break;
    case V2_CMD_LFB_PIXEL:
        v2_lfb_pixel(st, tgt, cmd);
        break;
    case V2_CMD_FB_STORE16: {
        uint32_t at = v2_fb_addr(st, cmd->u.store.buffer, cmd->u.store.x, cmd->u.store.y);
        tgt->fb[at] = (uint8_t)cmd->u.store.px;
        tgt->fb[(at + 1u) & V2_FB_MASK] = (uint8_t)(cmd->u.store.px >> 8);
        break;
    }
    case V2_CMD_TEX_WRITE:
        for (uint32_t i = 0; i < cmd->u.tex.n; i++)
            v2_tex_write(st, tgt, cmd->u.tex.off + 4u * i, cmd->u.tex.words[i]);
        break;
    case V2_CMD_PALETTE:
        tgt->palette[cmd->u.pal.tmu][cmd->u.pal.index] = cmd->u.pal.rgb;
        tgt->pal_gen[cmd->u.pal.tmu]++;
        break;
    case V2_CMD_NCC:
        tgt->ncc[cmd->u.ncc.tmu][cmd->u.ncc.table][cmd->u.ncc.off] = cmd->u.ncc.value;
        tgt->pal_gen[cmd->u.ncc.tmu]++;
        break;
    case V2_CMD_BLT_FILL:
        v2_blt_fill(tgt, cmd);
        break;
    case V2_CMD_STAT_CLEAR:
        tgt->pixels_in = tgt->chroma_fail = tgt->zfunc_fail = tgt->afunc_fail = tgt->pixels_out = 0;
        break;
    case V2_CMD_STIPPLE:
        tgt->stipple = cmd->u.stipple.value;
        break;
    }
}

// ============================================================
// Backends — WHERE the executor runs
// ============================================================

#define V2_STATE_SLOTS 1024u // draw-state ring (power of two)
#define V2_QUEUE_DEPTH 4096u // thread backend command ring (power of two)

typedef enum v2_backend_kind { V2_BACKEND_SW, V2_BACKEND_NULL, V2_BACKEND_THREAD } v2_backend_kind_t;

#if V2_HAVE_THREAD_BACKEND
// The worker thread and its bounded single-producer/single-consumer
// ring.  `head` counts commands submitted, `tail` commands RETIRED
// (executed to completion): head == tail is "everything observed",
// head - tail == depth is "full".  Both sides sleep on condition
// variables only when they have announced they are about to (the
// idle/waiting flags), so the steady state — worker busy, producer
// ahead — makes no system calls at all.
typedef struct v2_thread {
    v2_cmd_t *q;
    _Atomic uint64_t head, tail;
    _Atomic int worker_idle; // the worker is (about to be) asleep
    _Atomic int producer_waiting; // the producer is (about to be) asleep
    pthread_mutex_t mu;
    pthread_cond_t cv_work, cv_done;
    bool stop;
    bool started;
    pthread_t thr;
} v2_thread_t;
#endif

struct v2_raster {
    v2_backend_kind_t kind;
    v2_target_t *tgt;
    v2_state_build_fn build;
    void *ctx;
    // The draw-state ring: a slot is rewritten only once every command
    // that referenced it has retired (st_seq holds 1 + the sequence
    // number of the last such command; 0 = never used).
    v2_draw_state_t *st;
    uint64_t *st_seq;
    uint32_t st_cur;
    bool st_dirty;
#if V2_HAVE_THREAD_BACKEND
    v2_thread_t th;
#endif
};

#if V2_HAVE_THREAD_BACKEND
// The worker: drain the ring, execute in order, retire, sleep when dry.
static void *v2_worker_main(void *arg) {
    v2_raster_t *r = (v2_raster_t *)arg;
    v2_thread_t *th = &r->th;
    for (;;) {
        uint64_t t = atomic_load_explicit(&th->tail, memory_order_relaxed);
        uint64_t h = atomic_load_explicit(&th->head, memory_order_acquire);
        if (h == t) {
            pthread_mutex_lock(&th->mu);
            atomic_store_explicit(&th->worker_idle, 1, memory_order_seq_cst);
            // Re-check under the lock: a producer that stored head after
            // our first load either sees worker_idle and signals, or its
            // store is visible here (Dekker on head/worker_idle).
            while (atomic_load_explicit(&th->head, memory_order_seq_cst) == t && !th->stop)
                pthread_cond_wait(&th->cv_work, &th->mu);
            atomic_store_explicit(&th->worker_idle, 0, memory_order_seq_cst);
            bool stop = th->stop && atomic_load_explicit(&th->head, memory_order_seq_cst) == t;
            pthread_mutex_unlock(&th->mu);
            if (stop)
                return NULL;
            continue;
        }
        const v2_cmd_t *cmd = &th->q[t & (V2_QUEUE_DEPTH - 1u)];
        v2_raster_execute(&r->st[cmd->state], r->tgt, cmd);
        atomic_store_explicit(&th->tail, t + 1u, memory_order_seq_cst);
        if (atomic_load_explicit(&th->producer_waiting, memory_order_seq_cst)) {
            pthread_mutex_lock(&th->mu);
            pthread_cond_broadcast(&th->cv_done);
            pthread_mutex_unlock(&th->mu);
        }
    }
}

// Block the producer until `tail` reaches at least `want`.
static void v2_thread_wait_tail(v2_thread_t *th, uint64_t want) {
    if (atomic_load_explicit(&th->tail, memory_order_acquire) >= want)
        return;
    pthread_mutex_lock(&th->mu);
    atomic_store_explicit(&th->producer_waiting, 1, memory_order_seq_cst);
    while (atomic_load_explicit(&th->tail, memory_order_seq_cst) < want)
        pthread_cond_wait(&th->cv_done, &th->mu);
    atomic_store_explicit(&th->producer_waiting, 0, memory_order_seq_cst);
    pthread_mutex_unlock(&th->mu);
}

static bool v2_thread_start(v2_raster_t *r) {
    v2_thread_t *th = &r->th;
    th->q = (v2_cmd_t *)calloc(V2_QUEUE_DEPTH, sizeof(v2_cmd_t));
    if (!th->q)
        return false;
    atomic_store(&th->head, 0);
    atomic_store(&th->tail, 0);
    atomic_store(&th->worker_idle, 0);
    atomic_store(&th->producer_waiting, 0);
    pthread_mutex_init(&th->mu, NULL);
    pthread_cond_init(&th->cv_work, NULL);
    pthread_cond_init(&th->cv_done, NULL);
    th->stop = false;
    if (pthread_create(&th->thr, NULL, v2_worker_main, r) != 0) {
        free(th->q);
        th->q = NULL;
        return false;
    }
    th->started = true;
    return true;
}

static void v2_thread_stop(v2_raster_t *r) {
    v2_thread_t *th = &r->th;
    if (!th->started)
        return;
    v2_thread_wait_tail(th, atomic_load(&th->head));
    pthread_mutex_lock(&th->mu);
    th->stop = true;
    pthread_cond_broadcast(&th->cv_work);
    pthread_mutex_unlock(&th->mu);
    pthread_join(th->thr, NULL);
    pthread_mutex_destroy(&th->mu);
    pthread_cond_destroy(&th->cv_work);
    pthread_cond_destroy(&th->cv_done);
    free(th->q);
    th->q = NULL;
    th->started = false;
}
#endif

v2_raster_t *v2_raster_create(const char *kind, v2_target_t *tgt, v2_state_build_fn build, void *ctx) {
    v2_raster_t *r = (v2_raster_t *)calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->tgt = tgt;
    r->build = build;
    r->ctx = ctx;
    r->st = (v2_draw_state_t *)calloc(V2_STATE_SLOTS, sizeof(v2_draw_state_t));
    r->st_seq = (uint64_t *)calloc(V2_STATE_SLOTS, sizeof(uint64_t));
    if (!r->st || !r->st_seq) {
        free(r->st);
        free(r->st_seq);
        free(r);
        return NULL;
    }
    r->st_dirty = true;
    r->kind = V2_BACKEND_SW;
    v2_watch_init(); // the instrument reads its environment once, here
    v2_dither_tables_init();
    if (kind && strcmp(kind, "null") == 0) {
        r->kind = V2_BACKEND_NULL;
    } else if (kind && strcmp(kind, "thread") == 0) {
#if V2_HAVE_THREAD_BACKEND
        if (v2_raster_watch_armed()) {
            LOG(0, "raster=thread refused while GS_V2_WATCH is armed — using the synchronous walker");
        } else if (v2_thread_start(r)) {
            r->kind = V2_BACKEND_THREAD;
        } else {
            LOG(0, "raster=thread: could not start the worker thread — using the synchronous walker");
        }
#else
        LOG(1, "raster=thread is native-only; the wasm build uses the synchronous walker");
#endif
    } else if (kind && strcmp(kind, "sw") != 0) {
        LOG(0, "unknown raster backend '%s' — using the synchronous walker", kind);
    }
    return r;
}

void v2_raster_destroy(v2_raster_t *r) {
    if (!r)
        return;
#if V2_HAVE_THREAD_BACKEND
    if (r->kind == V2_BACKEND_THREAD)
        v2_thread_stop(r);
#endif
    free(r->st);
    free(r->st_seq);
    free(r);
}

const char *v2_raster_name(const v2_raster_t *r) {
    switch (r->kind) {
    case V2_BACKEND_NULL:
        return "null";
    case V2_BACKEND_THREAD:
        return "thread";
    default:
        return "sw";
    }
}

void v2_raster_state_dirty(v2_raster_t *r) {
    r->st_dirty = true;
}

void v2_raster_sync(v2_raster_t *r) {
#if V2_HAVE_THREAD_BACKEND
    if (r->kind == V2_BACKEND_THREAD)
        v2_thread_wait_tail(&r->th, atomic_load_explicit(&r->th.head, memory_order_relaxed));
#else
    (void)r;
#endif
}

void v2_raster_submit(v2_raster_t *r, v2_cmd_t *cmd) {
    uint64_t seq = 0;
#if V2_HAVE_THREAD_BACKEND
    if (r->kind == V2_BACKEND_THREAD)
        seq = atomic_load_explicit(&r->th.head, memory_order_relaxed);
#endif
    if (r->st_dirty) {
        // Take the next ring slot — after everything that read it has
        // retired — and let the producer fill it from the live registers.
        uint32_t next = (r->st_cur + 1u) & (V2_STATE_SLOTS - 1u);
#if V2_HAVE_THREAD_BACKEND
        if (r->kind == V2_BACKEND_THREAD && r->st_seq[next])
            v2_thread_wait_tail(&r->th, r->st_seq[next]);
#endif
        r->build(r->ctx, &r->st[next]);
        r->st_cur = next;
        r->st_dirty = false;
    }
    cmd->state = (uint16_t)r->st_cur;
    r->st_seq[r->st_cur] = seq + 1u;
    switch (r->kind) {
    case V2_BACKEND_NULL:
        if (cmd->kind == V2_CMD_TRIANGLE)
            return; // the null backend draws nothing — fastfill stays: it is the clear path
        v2_raster_execute(&r->st[r->st_cur], r->tgt, cmd);
        return;
    case V2_BACKEND_SW:
        v2_raster_execute(&r->st[r->st_cur], r->tgt, cmd);
        return;
    case V2_BACKEND_THREAD: {
#if V2_HAVE_THREAD_BACKEND
        v2_thread_t *th = &r->th;
        // Room: the slot we are about to write must have been retired.
        if (seq >= V2_QUEUE_DEPTH)
            v2_thread_wait_tail(th, seq + 1u - V2_QUEUE_DEPTH);
        th->q[seq & (V2_QUEUE_DEPTH - 1u)] = *cmd;
        atomic_store_explicit(&th->head, seq + 1u, memory_order_seq_cst);
        if (atomic_load_explicit(&th->worker_idle, memory_order_seq_cst)) {
            pthread_mutex_lock(&th->mu);
            pthread_cond_signal(&th->cv_work);
            pthread_mutex_unlock(&th->mu);
        }
#endif
        return;
    }
    }
}
