// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rage128_raster.c
// The ATI Rage 128 GL's 3D engine: the setup engine's primitives (points,
// lines, triangles from the CCE's vertex walker), the two texture units and
// their combine stages, texture lighting, specular, fog, colour key, alpha
// test, stencil and Z, alpha blending, dither, the 2D datapath's ROP3 and
// brush (polygon stipple) and the write mask.
//
// The engine draws what the CCE hands it (rage128_cce.c decodes the 3D
// packets into vertices) against a snapshot of its registers taken once per
// primitive batch.  The state is the 3D register set plus 2D state the 3D
// copies alias: DST_PITCH_OFFSET_C is DST_PITCH_OFFSET, SC_*_C the
// scissors, PLANE_3D_MASK_C the write mask, DP_GUI_MASTER_CNTL_C the data
// path whose DST_DATATYPE is the colour buffer's format (rage128_2d.c
// keeps those aliases).
//
// Conventions the manuals do not give, CHOSEN here and pinned by the
// rage128-3d test (docs/internals/core/peripherals/pci/cards/rage128.md):
//   * vertex X/Y snap to the sub-pixel grid (SUB_PIX_AMNT: 1/16 or 1/4) by
//     truncation, or rounding with FPU_ROUND_EN, after WINDOW_XY_OFFSET;
//   * a pixel is covered when its centre (x+0.5, y+0.5) is inside the
//     triangle, ties going to top and left edges;
//   * attributes are planes in screen space; S/T are perspective-correct
//     (S·W, T·W and W interpolated, divided per pixel) unless
//     TEXTURE_ST_FORMAT or *_TEX_PERSPECTIVE_DIS says otherwise; colours,
//     fog and Z are affine;
//   * Z = z·(2^N − 1), truncated, at Z_PIX_WIDTH's N;
//   * the mip level is log2 of the largest texel step of the pixel's
//     neighbours, less LOD_BIAS/128; a texel's centre is at (i + 0.5)/size;
//   * 8-bit channel arithmetic, products rounded (x·y + 127)/255;
//   * dither is a 4 x 4 ordered matrix;
//   * lines are DDA, the end pixel left off; points light one pixel.
// Tiled surfaces (DST_TILE, Z_TILE, TEX_n_TILE) are addressed linearly:
// the engine and the 2D engine agree, so a driver that only reaches its Z
// buffer through the engines sees a consistent buffer; one that reads a
// tiled buffer through an aperture would not (a log line says so once).
//
// Truth: ATI, *RAGE 128 Software Development Guide* (SDK-G04000 Rev 0.01,
// 1999), chapter 6 and appendix F; ATI, *RAGE 128 Register Reference
// Supplement: Registers for CCE 3D Packets*; ATI, *RAGE 128 VR / RAGE 128 GL
// Register Reference Guide* (RRG-G04100-C Rev 0.02, 1999).  Where the
// documents leave a field's meaning open, Mesa's classic r128 driver's
// usage decides, and the choice is a comment.

#include "log.h"
#include "rage128_priv.h"

#include <math.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("video");

// ============================================================
// Registers
// ============================================================

#define REG(r, off) ((r)->reg[(off) / 4u])

#define R_PM4_VC_FPU_SETUP 0x071Cu
#define R_DST_OFFSET       0x1404u
#define R_DST_PITCH        0x1408u
#define R_BRUSH_FRGD_CLR   0x147Cu
#define R_SC_LEFT          0x1640u
#define R_SC_RIGHT         0x1644u
#define R_SC_TOP           0x1648u
#define R_SC_BOTTOM        0x164Cu
#define R_AUX_SC_CNTL      0x1660u
#define R_AUX1_SC_LEFT     0x1664u // + 16 per scissor: LEFT, RIGHT, TOP, BOTTOM
#define R_DP_DATATYPE      0x16C4u
#define R_DP_MIX           0x16C8u
#define R_DP_WRITE_MSK     0x16CCu
#define R_SCALE_3D_CNTL    0x1A00u
#define R_FOG_TABLE_INDEX  0x1A14u
#define R_FOG_TABLE_DATA   0x1A18u
#define R_SETUP_CNTL       0x1BC4u
#define R_WINDOW_XY_OFFSET 0x1BCCu
#define R_Z_OFFSET_C       0x1C90u
#define R_Z_PITCH_C        0x1C94u
#define R_Z_STEN_CNTL_C    0x1C98u
#define R_TEX_CNTL_C       0x1C9Cu
#define R_MISC_3D_STATE    0x1CA0u
#define R_TEX_CLR_CMP_CLR  0x1CA4u
#define R_TEX_CLR_CMP_MSK  0x1CA8u
#define R_FOG_COLOR_C      0x1CACu
#define R_PRIM_TEX_CNTL_C  0x1CB0u
#define R_PRIM_COMBINE_C   0x1CB4u
#define R_TEX_SIZE_PITCH_C 0x1CB8u
#define R_PRIM_TEX_0_OFF   0x1CBCu
#define R_SEC_TEX_CNTL_C   0x1D00u
#define R_SEC_COMBINE_C    0x1D04u
#define R_SEC_TEX_0_OFF    0x1D08u
#define R_CONSTANT_COLOR_C 0x1D34u
#define R_PRIM_BORDER_C    0x1D38u
#define R_SEC_BORDER_C     0x1D3Cu
#define R_STEN_REF_MSK_C   0x1D40u

// TEX_CNTL_C.
#define TC_Z_EN        0x00000001u
#define TC_Z_MASK      0x00000002u
#define TC_STENCIL_EN  0x00000008u
#define TC_TEX_EN      0x00000010u
#define TC_SEC_TEX_EN  0x00000020u
#define TC_FOG_EN      0x00000080u
#define TC_DITHER_EN   0x00000100u
#define TC_ALPHA_EN    0x00000200u
#define TC_ALPHA_TST   0x00000400u
#define TC_SPECULAR    0x00000800u
#define TC_CHROMA_KEY  0x00001000u
#define TC_AMASK       0x00002000u
#define TC_LIGHT_FN(v) (((v) >> 14) & 0xFu)
#define TC_ALIGHT(v)   (((v) >> 18) & 7u)
#define TC_LOD_BIAS(v) ((int8_t)((v) >> 24))

// SCALE_3D_CNTL (its own fields; the shared ones are read from MISC).
#define S3_DITHER_TABLE 0x00000002u
#define S3_DITHER_INIT  0x00000008u
#define S3_ROUND_EN     0x00000010u
#define S3_TEX_MAP_AEN  0x40000000u

// MISC_3D_STATE_CNTL_REG.
#define MISC_REF_ALPHA(v)  ((v) & 0xFFu)
#define MISC_3D_FN(v)      (((v) >> 8) & 3u)
#define MISC_COMB_FN(v)    (((v) >> 12) & 3u)
#define MISC_FOG_TABLE     0x00004000u
#define MISC_BLND_SRC(v)   (((v) >> 16) & 0xFu)
#define MISC_BLND_DST(v)   (((v) >> 20) & 0xFu)
#define MISC_TEST_OP(v)    (((v) >> 24) & 7u)
#define MISC_CLR_CMP_FN(v) (((v) >> 30) & 3u)
#define SCALE_3D_TEXMAP    2u

// Z_STEN_CNTL_C.
#define ZS_PIX_WIDTH(v) (((v) >> 1) & 3u)
#define ZS_Z_TEST(v)    (((v) >> 4) & 7u)
#define ZS_S_TEST(v)    (((v) >> 12) & 7u)
#define ZS_SFAIL(v)     (((v) >> 16) & 7u)
#define ZS_ZPASS(v)     (((v) >> 20) & 7u)
#define ZS_ZFAIL(v)     (((v) >> 24) & 7u)

// PM4_VC_FPU_SETUP.
#define FPU_FRONT_CCW   0x00000001u
#define FPU_BACK_FN(v)  (((v) >> 1) & 3u)
#define FPU_FRONT_FN(v) (((v) >> 3) & 3u)
#define FPU_COLOR_FN(v) (((v) >> 5) & 3u)
#define FPU_SUB_PIX_4   0x00000080u
#define FPU_FLAT_LAST   0x00004000u
#define FPU_ROUND_EN    0x00008000u
#define CULL_FN_CULL    0u
#define CULL_FN_POINTS  1u
#define CULL_FN_LINES   2u
#define COLOR_FN_SOLID  0u
#define COLOR_FN_FLAT   1u

// SETUP_CNTL.
#define SU_ST_PREMULT 0x00000200u

// PRIM/SEC_TEX_CNTL_C.
#define TX_MIN_FN(v)    (((v) >> 1) & 7u)
#define TX_MAG_FN(v)    (((v) >> 4) & 7u)
#define TX_MIP_DIS      0x00000080u
#define TX_CLAMP_S(v)   (((v) >> 8) & 3u)
#define TX_CLAMP_T(v)   (((v) >> 11) & 3u)
#define TX_PERSP_DIS    0x00004000u
#define TX_DATATYPE(v)  (((v) >> 16) & 0xFu)
#define TX_SEC_SEL_ST   0x00000001u
#define TX_SEC_SEL_W    0x00008000u
#define WRAP_REPEAT     0u
#define WRAP_MIRROR     1u
#define WRAP_CLAMP      2u
#define WRAP_BORDER     3u
#define MIN_NEAREST     0u
#define MIN_LINEAR      1u
#define MIN_NEAREST_MIP 2u
#define MIN_LINEAR_MIP  3u
#define MIN_MIP_NEAREST 4u // between maps by nearest texel, then blended ("1x1")
#define MIN_TRILINEAR   5u

// The once-only logs.
#define TOLD_TILED   0x01u
#define TOLD_FORMAT  0x02u
#define TOLD_ROP     0x04u
#define TOLD_OFF     0x08u
#define TOLD_AGP_DST 0x10u
#define TOLD_PRIM7   0x20u

static void tell_once(rage128_t *r, uint8_t bit, const char *what) {
    if (r->told3d & bit)
        return;
    r->told3d |= bit;
    LOG(1, "Rage 128 3D: %s", what);
}

// ============================================================
// Vertices
// ============================================================

static float f32(uint32_t v) {
    float f;
    memcpy(&f, &v, sizeof(f));
    return isfinite(f) ? f : 0.0f;
}

uint32_t r128_3d_vertex_dwords(uint32_t fmt) {
    uint32_t n = 3; // X, Y, Z
    n += (fmt & VCF_RHW) ? 1 : 0;
    n += (fmt & VCF_DIFFUSE_BGR) ? 3 : 0;
    n += (fmt & VCF_DIFFUSE_A) ? 1 : 0;
    n += (fmt & VCF_DIFFUSE_ARGB) ? 1 : 0;
    n += (fmt & VCF_SPEC_BGR) ? 3 : 0;
    n += (fmt & VCF_SPEC_F) ? 1 : 0;
    n += (fmt & VCF_SPEC_FRGB) ? 1 : 0;
    n += (fmt & VCF_ST) ? 2 : 0;
    n += (fmt & VCF_S2T2) ? 2 : 0;
    n += (fmt & VCF_RHW2) ? 1 : 0;
    return n;
}

static float unit255(float f) {
    f *= 255.0f;
    return f < 0.0f ? 0.0f : f > 255.0f ? 255.0f : f;
}

// Fields absent from the format take neutral values (CHOSEN; the manuals
// are silent): W = 1, an opaque white diffuse, no specular, no fog.
void r128_3d_vertex_decode(uint32_t fmt, const uint32_t *d, r128_vertex_t *v) {
    uint32_t i = 0;
    v->x = f32(d[i++]);
    v->y = f32(d[i++]);
    v->z = f32(d[i++]);
    v->rhw = (fmt & VCF_RHW) ? f32(d[i++]) : 1.0f;
    v->c[0] = v->c[1] = v->c[2] = v->c[3] = 255.0f;
    v->spec[0] = v->spec[1] = v->spec[2] = 0.0f;
    v->fog = 255.0f;
    if (fmt & VCF_DIFFUSE_BGR) {
        v->c[2] = unit255(f32(d[i++]));
        v->c[1] = unit255(f32(d[i++]));
        v->c[0] = unit255(f32(d[i++]));
    }
    if (fmt & VCF_DIFFUSE_A)
        v->c[3] = unit255(f32(d[i++]));
    if (fmt & VCF_DIFFUSE_ARGB) {
        uint32_t c = d[i++];
        v->c[0] = (float)((c >> 16) & 0xFFu);
        v->c[1] = (float)((c >> 8) & 0xFFu);
        v->c[2] = (float)(c & 0xFFu);
        v->c[3] = (float)(c >> 24);
    }
    if (fmt & VCF_SPEC_BGR) {
        v->spec[2] = unit255(f32(d[i++]));
        v->spec[1] = unit255(f32(d[i++]));
        v->spec[0] = unit255(f32(d[i++]));
    }
    if (fmt & VCF_SPEC_F)
        v->fog = unit255(f32(d[i++]));
    if (fmt & VCF_SPEC_FRGB) {
        uint32_t c = d[i++];
        v->spec[0] = (float)((c >> 16) & 0xFFu);
        v->spec[1] = (float)((c >> 8) & 0xFFu);
        v->spec[2] = (float)(c & 0xFFu);
        v->fog = (float)(c >> 24);
    }
    v->s = v->t = v->s2 = v->t2 = 0.0f;
    if (fmt & VCF_ST) {
        v->s = f32(d[i++]);
        v->t = f32(d[i++]);
    }
    if (fmt & VCF_S2T2) {
        v->s2 = f32(d[i++]);
        v->t2 = f32(d[i++]);
    }
    v->rhw2 = (fmt & VCF_RHW2) ? f32(d[i++]) : v->rhw;
}

// ============================================================
// The state snapshot
// ============================================================

typedef struct tex_unit {
    bool on;
    uint32_t cntl, comb, fmt;
    uint32_t lpitch, lheight, lsize, lmin; // log2 of the largest map's pitch (= width), height, max, smallest
    uint32_t off[11];
    uint32_t border;
    bool persp;
    bool sec_st, sec_w; // the secondary unit's coordinate set
} tex_unit_t;

typedef struct st {
    rage128_t *r;
    // The colour buffer.
    uint32_t dst_base, dst_stride, dst_type, dst_bpp;
    int32_t sc_l, sc_t, sc_r, sc_b; // inclusive
    uint32_t aux_cntl;
    int32_t aux[3][4];
    uint32_t write_mask, rop;
    bool brush_masks; // a patterned brush gates pixels (polygon stipple)
    // The 3D state.
    uint32_t scale, misc, tex_cntl, fpu, setup, zs, sten;
    uint32_t z_base, z_stride, z_bytes;
    uint64_t z_max;
    uint32_t fog_color, constant, key, key_mask;
    uint8_t fog_table[256];
    float win_x, win_y, snap;
    bool round_xy;
    tex_unit_t t[2];
} st_t;

// Bytes per pixel of a colour-buffer datatype the 3D engine writes, or 0.
static uint32_t dst_bytes(uint32_t type) {
    switch (type) {
    case 2:
    case 7:
    case 8:
    case 9:
        return 1;
    case 3:
    case 4:
    case 15:
        return 2;
    case 6:
        return 4;
    default:
        return 0;
    }
}

static int32_t s14(uint32_t v) {
    return (int32_t)((v & 0x3FFFu) ^ 0x2000u) - 0x2000;
}

static void tex_gather(rage128_t *r, tex_unit_t *u, bool on, uint32_t cntl_reg, uint32_t comb_reg, uint32_t off_reg,
                       uint32_t border_reg, uint32_t size_shift) {
    u->on = on;
    if (!on)
        return;
    u->cntl = REG(r, cntl_reg);
    u->comb = REG(r, comb_reg);
    u->fmt = TX_DATATYPE(u->cntl);
    uint32_t sp = REG(r, R_TEX_SIZE_PITCH_C) >> size_shift;
    u->lpitch = sp & 0xFu;
    u->lsize = (sp >> 4) & 0xFu;
    u->lheight = (sp >> 8) & 0xFu;
    u->lmin = (sp >> 12) & 0xFu;
    if (u->lpitch > 10)
        u->lpitch = 10;
    if (u->lheight > 10)
        u->lheight = 10;
    if (u->lsize > 10)
        u->lsize = 10;
    if (u->lmin > u->lsize)
        u->lmin = u->lsize;
    for (uint32_t i = 0; i < 11; i++) {
        u->off[i] = REG(r, off_reg + 4u * i);
        if (u->off[i] & 0xC0000000u)
            tell_once(r, TOLD_TILED, "a tiled texture, surface or Z buffer is addressed linearly");
    }
    u->border = REG(r, border_reg);
    u->persp = !(u->cntl & TX_PERSP_DIS) && !(REG(r, R_SETUP_CNTL) & SU_ST_PREMULT);
}

static bool gather(rage128_t *r, st_t *s) {
    memset(s, 0, sizeof(*s));
    s->r = r;
    s->misc = REG(r, R_MISC_3D_STATE);
    if (MISC_3D_FN(s->misc) != SCALE_3D_TEXMAP) {
        tell_once(r, TOLD_OFF, "a primitive with SCALE_3D_FCN not set to texture map/shade is not drawn");
        return false;
    }
    s->dst_type = REG(r, R_DP_DATATYPE) & 0xFu;
    s->dst_bpp = dst_bytes(s->dst_type);
    if (!s->dst_bpp) {
        tell_once(r, TOLD_FORMAT, "a colour buffer format the 3D engine does not write — primitive dropped");
        return false;
    }
    s->dst_base = REG(r, R_DST_OFFSET) & 0x3FFFFF0u;
    s->dst_stride = (REG(r, R_DST_PITCH) & 0x3FFu) * 8u * s->dst_bpp;
    if (s->dst_base & 0x2000000u) {
        tell_once(r, TOLD_AGP_DST, "a colour buffer in AGP space is not drawn");
        return false;
    }
    s->sc_l = s14(REG(r, R_SC_LEFT));
    s->sc_r = s14(REG(r, R_SC_RIGHT));
    s->sc_t = s14(REG(r, R_SC_TOP));
    s->sc_b = s14(REG(r, R_SC_BOTTOM));
    s->aux_cntl = REG(r, R_AUX_SC_CNTL);
    for (int k = 0; k < 3; k++)
        for (int e = 0; e < 4; e++)
            s->aux[k][e] = s14(REG(r, R_AUX1_SC_LEFT + 16u * (uint32_t)k + 4u * (uint32_t)e));
    s->write_mask = REG(r, R_DP_WRITE_MSK);
    s->rop = (REG(r, R_DP_MIX) >> 16) & 0xFFu;
    uint32_t brush = (REG(r, R_DP_DATATYPE) >> 8) & 0xFu;
    s->brush_masks = brush < 13u;
    s->scale = REG(r, R_SCALE_3D_CNTL);
    s->tex_cntl = REG(r, R_TEX_CNTL_C);
    s->fpu = REG(r, R_PM4_VC_FPU_SETUP);
    s->setup = REG(r, R_SETUP_CNTL);
    s->zs = REG(r, R_Z_STEN_CNTL_C);
    s->sten = REG(r, R_STEN_REF_MSK_C);
    switch (ZS_PIX_WIDTH(s->zs)) {
    case 0:
        s->z_bytes = 2, s->z_max = 0xFFFFu;
        break;
    case 1:
        s->z_bytes = 4, s->z_max = 0xFFFFFFu;
        break;
    default:
        s->z_bytes = 4, s->z_max = 0xFFFFFFFFu;
        break;
    }
    s->z_base = REG(r, R_Z_OFFSET_C) & 0x3FFFFF0u;
    s->z_stride = (REG(r, R_Z_PITCH_C) & 0x3FFu) * 8u * s->z_bytes;
    s->fog_color = REG(r, R_FOG_COLOR_C);
    s->constant = REG(r, R_CONSTANT_COLOR_C);
    s->key = REG(r, R_TEX_CLR_CMP_CLR);
    s->key_mask = REG(r, R_TEX_CLR_CMP_MSK);
    memcpy(s->fog_table, r->fog_table, sizeof(s->fog_table));
    // WINDOW_XY_OFFSET: signed, with SUB_PIX_AMNT's fraction bits.
    bool sub4 = (s->fpu & FPU_SUB_PIX_4) != 0;
    float unit = sub4 ? 16.0f : 4.0f;
    uint32_t w = REG(r, R_WINDOW_XY_OFFSET);
    s->win_x = (float)(int16_t)(w >> 16) / unit;
    s->win_y = (float)(int16_t)(w & 0xFFFFu) / unit;
    s->snap = unit;
    s->round_xy = (s->fpu & FPU_ROUND_EN) != 0;
    bool t0 = (s->tex_cntl & TC_TEX_EN) != 0;
    tex_gather(r, &s->t[0], t0, R_PRIM_TEX_CNTL_C, R_PRIM_COMBINE_C, R_PRIM_TEX_0_OFF, R_PRIM_BORDER_C, 0);
    tex_gather(r, &s->t[1], t0 && (s->tex_cntl & TC_SEC_TEX_EN), R_SEC_TEX_CNTL_C, R_SEC_COMBINE_C, R_SEC_TEX_0_OFF,
               R_SEC_BORDER_C, 16);
    if (s->t[1].on) {
        s->t[1].sec_st = (s->t[1].cntl & TX_SEC_SEL_ST) != 0;
        s->t[1].sec_w = (s->t[1].cntl & TX_SEC_SEL_W) != 0;
    }
    return true;
}

// ============================================================
// Channel arithmetic
// ============================================================

typedef struct col {
    int32_t c[4]; // R, G, B, A, 0..255
} col_t;

static int32_t clamp255(int32_t v) {
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

static int32_t mul255(int32_t a, int32_t b) {
    return (a * b + 127) / 255;
}

static col_t unpack_argb(uint32_t v) {
    col_t c = {
        {(int32_t)((v >> 16) & 0xFFu), (int32_t)((v >> 8) & 0xFFu), (int32_t)(v & 0xFFu), (int32_t)(v >> 24)}
    };
    return c;
}

// ============================================================
// Textures
// ============================================================

static uint32_t texel_bits(uint32_t fmt) {
    switch (fmt) {
    case 1:
        return 4;
    case 2:
    case 7:
    case 8:
    case 9:
        return 8;
    case 3:
    case 4:
    case 10:
    case 11:
    case 12:
    case 15:
        return 16;
    case 6:
    case 14:
        return 32;
    default:
        return 0;
    }
}

static uint32_t mem_read(st_t *s, uint32_t addr, uint32_t bytes) {
    rage128_t *r = s->r;
    if (!(addr & 0x2000000u)) {
        if (addr + bytes > r->vram_size)
            return 0;
        uint32_t v = 0;
        for (uint32_t i = 0; i < bytes; i++)
            v |= (uint32_t)r->vram[addr + i] << (8u * i);
        return v;
    }
    // A texture in AGP space: through the GART, a dword at a time.
    uint32_t w;
    if (!r128_card_read(r, addr & ~3u, &w, 1))
        return 0;
    return (w >> (8u * (addr & 3u))) & (bytes >= 4 ? 0xFFFFFFFFu : ((1u << (8u * bytes)) - 1u));
}

// One texel of `u` at level `lev` (0 = the largest map), as ARGB8888, and
// its raw value for the colour key.
static uint32_t texel(st_t *s, const tex_unit_t *u, uint32_t lev, uint32_t tx, uint32_t ty, uint32_t *raw) {
    uint32_t lw = u->lpitch > lev ? u->lpitch - lev : 0;
    uint32_t bits = texel_bits(u->fmt);
    // PRIM_TEX_0 holds the smallest map: register n is the map whose
    // largest side is 2^(TEX_MIN_SIZE + n) (the CCE supplement; Mesa's
    // uploads agree, the SDK's chapter 6 does not).
    int32_t idx = (int32_t)(u->lsize - u->lmin) - (int32_t)lev;
    if (idx < 0)
        idx = 0;
    if (idx > 10)
        idx = 10;
    uint32_t base = u->off[idx] & 0x3FFFFFFu;
    uint32_t bit = (ty * (1u << lw) + tx) * bits;
    uint32_t v = bits ? mem_read(s, base + bit / 8u, bits >= 8 ? bits / 8u : 1u) : 0;
    if (bits == 4)
        v = (v >> ((bit & 4u) ? 4 : 0)) & 0xFu;
    *raw = v;
    uint32_t a = 0xFFu, rr, g, b;
    switch (u->fmt) {
    case 3: // ARGB1555
        a = (v & 0x8000u) ? 0xFFu : 0u;
        rr = (v >> 10) & 0x1Fu, g = (v >> 5) & 0x1Fu, b = v & 0x1Fu;
        rr = (rr << 3) | (rr >> 2), g = (g << 3) | (g >> 2), b = (b << 3) | (b >> 2);
        break;
    case 4: // RGB565
        rr = (v >> 11) & 0x1Fu, g = (v >> 5) & 0x3Fu, b = v & 0x1Fu;
        rr = (rr << 3) | (rr >> 2), g = (g << 2) | (g >> 4), b = (b << 3) | (b >> 2);
        break;
    case 6: // ARGB8888
        a = v >> 24, rr = (v >> 16) & 0xFFu, g = (v >> 8) & 0xFFu, b = v & 0xFFu;
        break;
    case 7: // RGB332
        rr = ((v >> 5) & 7u) * 255u / 7u, g = ((v >> 2) & 7u) * 255u / 7u, b = (v & 3u) * 255u / 3u;
        break;
    case 8: // Y8
        rr = g = b = v;
        break;
    case 9: // RGB8: one intensity in every channel, alpha included
        rr = g = b = a = v;
        break;
    case 15: // ARGB4444
        a = ((v >> 12) & 0xFu) * 17u, rr = ((v >> 8) & 0xFu) * 17u, g = ((v >> 4) & 0xFu) * 17u, b = (v & 0xFu) * 17u;
        break;
    default:
        // The palettised (CI4, CI8, A:CI) and YUV formats: the texture
        // palette's path is not modelled; the index reads as a grey.
        tell_once(s->r, TOLD_FORMAT, "a palettised or YUV texture reads as grey");
        rr = g = b = v & 0xFFu;
        break;
    }
    if (!(s->scale & S3_TEX_MAP_AEN))
        a = 0xFFu; // textures carry no alpha
    return (a << 24) | (rr << 16) | (g << 8) | b;
}

// A coordinate in texels through the clamp mode: the texel index, or -1
// for the border colour.
static int32_t wrap(int32_t i, uint32_t size, uint32_t mode) {
    int32_t n = (int32_t)size;
    switch (mode) {
    case WRAP_REPEAT:
        return ((i % n) + n) % n;
    case WRAP_MIRROR: {
        int32_t p = ((i % (2 * n)) + 2 * n) % (2 * n);
        return p < n ? p : 2 * n - 1 - p;
    }
    case WRAP_CLAMP:
        return i < 0 ? 0 : i >= n ? n - 1 : i;
    default:
        return (i < 0 || i >= n) ? -1 : i;
    }
}

static uint32_t fetch(st_t *s, const tex_unit_t *u, uint32_t lev, int32_t x, int32_t y, uint32_t *raw) {
    uint32_t w = 1u << (u->lpitch > lev ? u->lpitch - lev : 0);
    uint32_t h = 1u << (u->lheight > lev ? u->lheight - lev : 0);
    int32_t tx = wrap(x, w, TX_CLAMP_S(u->cntl)), ty = wrap(y, h, TX_CLAMP_T(u->cntl));
    if (tx < 0 || ty < 0) {
        *raw = 0;
        return u->border;
    }
    return texel(s, u, lev, (uint32_t)tx, (uint32_t)ty, raw);
}

static uint32_t lerp_argb(uint32_t a, uint32_t b, uint32_t f) { // f in 0..256
    uint32_t out = 0;
    for (int k = 0; k < 32; k += 8) {
        uint32_t x = (a >> k) & 0xFFu, y = (b >> k) & 0xFFu;
        out |= ((x * (256u - f) + y * f + 128u) >> 8) << k;
    }
    return out;
}

// One map, nearest or bilinear, at (s, t) in [0,1] units of the map.
static uint32_t sample_level(st_t *s, const tex_unit_t *u, uint32_t lev, double sc, double tc, bool linear,
                             uint32_t *raw) {
    double w = (double)(1u << (u->lpitch > lev ? u->lpitch - lev : 0));
    double h = (double)(1u << (u->lheight > lev ? u->lheight - lev : 0));
    double uu = sc * w, vv = tc * h;
    if (!linear)
        return fetch(s, u, lev, (int32_t)floor(uu), (int32_t)floor(vv), raw);
    uu -= 0.5, vv -= 0.5;
    double fu = floor(uu), fv = floor(vv);
    int32_t x0 = (int32_t)fu, y0 = (int32_t)fv;
    uint32_t wx = (uint32_t)((uu - fu) * 256.0), wy = (uint32_t)((vv - fv) * 256.0);
    uint32_t r0, r1, r2, r3;
    uint32_t t00 = fetch(s, u, lev, x0, y0, &r0), t10 = fetch(s, u, lev, x0 + 1, y0, &r1);
    uint32_t t01 = fetch(s, u, lev, x0, y0 + 1, &r2), t11 = fetch(s, u, lev, x0 + 1, y0 + 1, &r3);
    *raw = r0;
    return lerp_argb(lerp_argb(t00, t10, wx), lerp_argb(t01, t11, wx), wy);
}

// The unit's filtered texel at (s, t) with level-of-detail `lod` (log2 of
// the texel step on the largest map, LOD_BIAS already applied).
static uint32_t sample(st_t *s, const tex_unit_t *u, double sc, double tc, double lod, uint32_t *raw) {
    uint32_t minf = TX_MIN_FN(u->cntl), magf = TX_MAG_FN(u->cntl);
    bool mip = !(u->cntl & TX_MIP_DIS) && minf >= MIN_NEAREST_MIP;
    if (lod <= 0.0 || u->lsize == u->lmin || !mip) {
        bool linear =
            lod <= 0.0 ? (magf & 1u) != 0 : (minf == MIN_LINEAR || minf == MIN_LINEAR_MIP || minf == MIN_TRILINEAR);
        return sample_level(s, u, 0, sc, tc, linear, raw);
    }
    uint32_t top = u->lsize - u->lmin;
    bool linear = minf == MIN_LINEAR_MIP || minf == MIN_TRILINEAR;
    if (minf == MIN_NEAREST_MIP || minf == MIN_LINEAR_MIP) {
        uint32_t lev = (uint32_t)floor(lod + 0.5);
        if (lev > top)
            lev = top;
        return sample_level(s, u, lev, sc, tc, linear, raw);
    }
    // Between two maps.
    uint32_t lev = (uint32_t)floor(lod);
    if (lev >= top)
        return sample_level(s, u, top, sc, tc, linear, raw);
    uint32_t f = (uint32_t)((lod - floor(lod)) * 256.0);
    uint32_t raw2;
    uint32_t a = sample_level(s, u, lev, sc, tc, linear, raw);
    uint32_t b = sample_level(s, u, lev + 1, sc, tc, linear, &raw2);
    return lerp_argb(a, b, f);
}

// ============================================================
// Combine
// ============================================================

// The colour/alpha combine function (SDK chapter 6, the combine table).
static int32_t comb(uint32_t fn, int32_t a, int32_t b, int32_t pass, int32_t a_interp, int32_t a_tex, int32_t a_const,
                    int32_t a_prev, int32_t c_const) {
    switch (fn) {
    case 0:
        return pass;
    case 1:
        return a;
    case 2:
        return b;
    case 3:
        return mul255(a, b);
    case 4:
        return clamp255(2 * mul255(a, b));
    case 5:
        return clamp255(4 * mul255(a, b));
    case 6:
        return clamp255(a + b);
    case 7:
        return clamp255(a + b - 128);
    case 8:
        return clamp255(mul255(a, a_interp) + mul255(b, 255 - a_interp));
    case 9:
        return clamp255(mul255(a, a_tex) + mul255(b, 255 - a_tex));
    case 10:
        return clamp255(mul255(a, a_const) + mul255(b, 255 - a_const));
    case 11:
        return clamp255(a + mul255(b, 255 - a_tex));
    case 12:
        return clamp255(mul255(a, a_prev) + mul255(b, 255 - a_prev));
    case 13:
        return clamp255(a + mul255(b, a_tex));
    case 14:
        return clamp255(2 * (a + b - 128));
    default: // 15: per-channel constant colour
        return clamp255(mul255(a, c_const) + mul255(b, 255 - c_const));
    }
}

// One texture unit's stage.  `tex` is its filtered texel, `prev` the
// previous stage (the interpolated colour for the primary unit), `diff`
// the interpolated diffuse colour.
static col_t stage(const st_t *s, const tex_unit_t *u, bool secondary, col_t tex, col_t prev, col_t diff) {
    col_t cc = unpack_argb(s->constant), out;
    uint32_t w = u->comb;
    uint32_t fn = w & 0xFu, cf = (w >> 4) & 0xFu, inf = (w >> 10) & 0xFu;
    uint32_t fa = (w >> 14) & 0xFu, af = (w >> 18) & 0xFu, ina = (w >> 25) & 7u;
    int32_t a_prev = secondary ? prev.c[3] : tex.c[3];
    for (int k = 0; k < 3; k++) {
        int32_t a, b;
        switch (cf) { // the first argument (Mesa's encodings for 0, 1 and 8)
        case 0:
            a = cc.c[k];
            break;
        case 1:
            a = 255 - cc.c[k];
            break;
        case 5:
            a = 255 - tex.c[k];
            break;
        case 6:
            a = tex.c[3];
            break;
        case 7:
            a = 255 - tex.c[3];
            break;
        case 8:
            a = prev.c[k];
            break;
        default:
            a = tex.c[k];
            break;
        }
        switch (inf) { // the second
        case 2:
            b = cc.c[k];
            break;
        case 3:
            b = cc.c[3];
            break;
        case 5:
            b = diff.c[3];
            break;
        case 8:
            b = prev.c[k];
            break;
        case 9:
            b = prev.c[3];
            break;
        default:
            b = secondary ? prev.c[k] : diff.c[k];
            break;
        }
        out.c[k] = comb(fn, a, b, tex.c[k], diff.c[3], tex.c[3], cc.c[3], a_prev, cc.c[k]);
    }
    int32_t a = af == 7 ? 255 - tex.c[3] : af == 0 ? cc.c[3] : af == 1 ? 255 - cc.c[3] : tex.c[3];
    int32_t b = ina == 1 ? cc.c[3] : ina == 4 ? prev.c[3] : (secondary && ina != 2) ? prev.c[3] : diff.c[3];
    out.c[3] = comb(fa, a, b, tex.c[3], diff.c[3], tex.c[3], cc.c[3], a_prev, cc.c[3]);
    return out;
}

// ============================================================
// The colour buffer
// ============================================================

static uint32_t dst_read(const st_t *s, uint32_t at) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < s->dst_bpp; i++)
        v |= (uint32_t)s->r->vram[at + i] << (8u * i);
    return v;
}

static col_t dst_unpack(const st_t *s, uint32_t v) {
    col_t c;
    switch (s->dst_type) {
    case 3:
        c.c[0] = (int32_t)(((v >> 10) & 0x1Fu) * 255u / 31u);
        c.c[1] = (int32_t)(((v >> 5) & 0x1Fu) * 255u / 31u);
        c.c[2] = (int32_t)((v & 0x1Fu) * 255u / 31u);
        c.c[3] = (v & 0x8000u) ? 255 : 0;
        break;
    case 4:
        c.c[0] = (int32_t)(((v >> 11) & 0x1Fu) * 255u / 31u);
        c.c[1] = (int32_t)(((v >> 5) & 0x3Fu) * 255u / 63u);
        c.c[2] = (int32_t)((v & 0x1Fu) * 255u / 31u);
        c.c[3] = 255;
        break;
    case 6:
        c = unpack_argb(v);
        break;
    case 7:
        c.c[0] = (int32_t)(((v >> 5) & 7u) * 255u / 7u);
        c.c[1] = (int32_t)(((v >> 2) & 7u) * 255u / 7u);
        c.c[2] = (int32_t)((v & 3u) * 255u / 3u);
        c.c[3] = 255;
        break;
    case 15:
        c.c[0] = (int32_t)(((v >> 8) & 0xFu) * 17u);
        c.c[1] = (int32_t)(((v >> 4) & 0xFu) * 17u);
        c.c[2] = (int32_t)((v & 0xFu) * 17u);
        c.c[3] = (int32_t)(((v >> 12) & 0xFu) * 17u);
        break;
    default: // the 8-bit single-channel buffers
        c.c[0] = c.c[1] = c.c[2] = (int32_t)(v & 0xFFu);
        c.c[3] = 255;
        break;
    }
    return c;
}

// The 4 x 4 ordered dither (CHOSEN: the matrix is not documented).
static const uint8_t k_bayer[4][4] = {
    {0,  8,  2,  10},
    {12, 4,  14, 6 },
    {3,  11, 1,  9 },
    {15, 7,  13, 5 },
};

// Reduce a channel to `bits`: dither, round or truncate.
static uint32_t reduce(int32_t v, uint32_t bits, int32_t dither) {
    uint32_t drop = 8u - bits;
    if (!drop)
        return (uint32_t)v;
    int32_t step = 1 << drop;
    if (dither >= 0)
        v += (dither * step) / 16;
    v = clamp255(v);
    return (uint32_t)v >> drop;
}

static uint32_t dst_pack(const st_t *s, col_t c, int32_t x, int32_t y, bool blending) {
    int32_t d = -1; // no dither: truncate
    bool dither =
        (s->tex_cntl & TC_DITHER_EN) && !(blending && (s->scale & S3_DITHER_TABLE) && (s->scale & S3_DITHER_INIT));
    if (dither)
        d = k_bayer[y & 3][x & 3];
    else if (s->scale & S3_ROUND_EN)
        d = 8; // half a step
    switch (s->dst_type) {
    case 3:
        return (c.c[3] >= 128 ? 0x8000u : 0u) | (reduce(c.c[0], 5, d) << 10) | (reduce(c.c[1], 5, d) << 5) |
               reduce(c.c[2], 5, d);
    case 4:
        return (reduce(c.c[0], 5, d) << 11) | (reduce(c.c[1], 6, d) << 5) | reduce(c.c[2], 5, d);
    case 6:
        return ((uint32_t)c.c[3] << 24) | ((uint32_t)c.c[0] << 16) | ((uint32_t)c.c[1] << 8) | (uint32_t)c.c[2];
    case 7:
        return (reduce(c.c[0], 3, d) << 5) | (reduce(c.c[1], 3, d) << 2) | reduce(c.c[2], 2, d);
    case 15:
        return (reduce(c.c[3], 4, d) << 12) | (reduce(c.c[0], 4, d) << 8) | (reduce(c.c[1], 4, d) << 4) |
               reduce(c.c[2], 4, d);
    default:
        // Y8 and RGB8 store one channel — green, the reference's choice for
        // RGB8 writes.
        return (uint32_t)c.c[1];
    }
}

// ============================================================
// Per-pixel
// ============================================================

// Interpolated attributes at one pixel.
typedef struct frag {
    double z; // 0..1
    double c[4], spec[3], fog;
    double s[2], t[2], lod[2];
} frag_t;

static bool scissored(const st_t *s, int32_t x, int32_t y) {
    if (x < s->sc_l || x > s->sc_r || y < s->sc_t || y > s->sc_b)
        return true;
    uint32_t ac = s->aux_cntl;
    bool any_add = false, in_add = false;
    for (int k = 0; k < 3; k++) {
        if (!(ac & (1u << (2 * k))))
            continue;
        bool in = x >= s->aux[k][0] && x <= s->aux[k][1] && y >= s->aux[k][2] && y <= s->aux[k][3];
        if (ac & (2u << (2 * k))) {
            if (in)
                return true; // subtractive
        } else {
            any_add = true;
            in_add |= in;
        }
    }
    return any_add && !in_add;
}

static bool cmp_op(uint32_t op, uint64_t a, uint64_t b) {
    switch (op) {
    case 0:
        return false;
    case 1:
        return a < b;
    case 2:
        return a <= b;
    case 3:
        return a == b;
    case 4:
        return a >= b;
    case 5:
        return a > b;
    case 6:
        return a != b;
    default:
        return true;
    }
}

static uint32_t sten_op(uint32_t op, uint32_t v, uint32_t ref) {
    switch (op) {
    case 1:
        return 0;
    case 2:
        return ref;
    case 3:
        return v < 255 ? v + 1 : 255;
    case 4:
        return v > 0 ? v - 1 : 0;
    case 5:
        return ~v & 0xFFu;
    default:
        return v;
    }
}

static int32_t blend_factor(uint32_t f, int k, col_t src, col_t dst, bool for_src, bool *force_dst, uint32_t *dst_f) {
    switch (f) {
    case 0:
        return 0;
    case 1:
        return 255;
    case 2:
        return src.c[k];
    case 3:
        return 255 - src.c[k];
    case 4:
        return src.c[3];
    case 5:
        return 255 - src.c[3];
    case 6:
        return dst.c[3];
    case 7:
        return 255 - dst.c[3];
    case 8:
        return dst.c[k];
    case 9:
        return 255 - dst.c[k];
    case 10:
        return k == 3 ? 255 : (src.c[3] < 255 - dst.c[3] ? src.c[3] : 255 - dst.c[3]);
    case 11:
        if (for_src) {
            *force_dst = true, *dst_f = 5;
            return src.c[3];
        }
        return 0;
    case 12:
        if (for_src) {
            *force_dst = true, *dst_f = 4;
            return 255 - src.c[3];
        }
        return 0;
    default:
        return 0;
    }
}

static void shade(st_t *s, int32_t x, int32_t y, const frag_t *f) {
    rage128_t *r = s->r;
    if (scissored(s, x, y) || x < 0 || y < 0)
        return;
    uint32_t at = s->dst_base + (uint32_t)y * s->dst_stride + (uint32_t)x * s->dst_bpp;
    if (at + s->dst_bpp > r->vram_size)
        return;
    if (s->brush_masks && !r128_2d_brush_covers(r, x, y))
        return; // polygon stipple
    col_t diff, spec;
    for (int k = 0; k < 4; k++)
        diff.c[k] = clamp255((int32_t)f->c[k]);
    for (int k = 0; k < 3; k++)
        spec.c[k] = clamp255((int32_t)f->spec[k]);
    spec.c[3] = 0;
    col_t c = diff;
    uint32_t raw0 = 0;
    if (s->t[0].on) {
        col_t tex[2];
        for (int u = 0; u < 2; u++) {
            if (!s->t[u].on)
                continue;
            uint32_t raw;
            tex[u] = unpack_argb(sample(s, &s->t[u], f->s[u], f->t[u], f->lod[u], &raw));
            if (!u)
                raw0 = raw;
        }
        c = stage(s, &s->t[0], false, tex[0], diff, diff);
        if (s->t[1].on)
            c = stage(s, &s->t[1], true, tex[1], c, diff);
        // Texture lighting: the combined colour against the interpolated one.
        uint32_t lf = TC_LIGHT_FN(s->tex_cntl), la = TC_ALIGHT(s->tex_cntl);
        col_t cc = unpack_argb(s->constant), lit;
        for (int k = 0; k < 3; k++)
            lit.c[k] = comb(lf, c.c[k], diff.c[k], c.c[k], diff.c[3], tex[0].c[3], cc.c[3], c.c[3], cc.c[k]);
        lit.c[3] = comb(la, c.c[3], diff.c[3], c.c[3], diff.c[3], tex[0].c[3], cc.c[3], c.c[3], cc.c[3]);
        c = lit;
        // The colour key and the alpha mask (texel-level kills).
        if (s->tex_cntl & TC_CHROMA_KEY) {
            uint32_t fn = MISC_CLR_CMP_FN(s->misc);
            bool eq = ((raw0 ^ s->key) & s->key_mask) == 0;
            bool draw = fn == 1 || (fn == 2 && !eq) || (fn == 3 && eq);
            if (!draw)
                return;
        }
        if ((s->tex_cntl & TC_AMASK) && !(tex[0].c[3] & 1))
            return;
    }
    if (s->tex_cntl & TC_SPECULAR)
        for (int k = 0; k < 3; k++)
            c.c[k] = clamp255(c.c[k] + spec.c[k]);
    if (s->tex_cntl & TC_FOG_EN) {
        int32_t ff;
        if (s->misc & MISC_FOG_TABLE)
            ff = s->fog_table[(uint32_t)(f->z * 255.0) & 0xFFu];
        else
            ff = clamp255((int32_t)f->fog);
        col_t fc = unpack_argb(s->fog_color);
        for (int k = 0; k < 3; k++)
            c.c[k] = clamp255(mul255(c.c[k], ff) + mul255(fc.c[k], 255 - ff));
    }
    if ((s->tex_cntl & TC_ALPHA_TST) &&
        !cmp_op(MISC_TEST_OP(s->misc), (uint64_t)c.c[3], (uint64_t)MISC_REF_ALPHA(s->misc)))
        return;
    // Stencil and Z.
    bool z_en = (s->tex_cntl & TC_Z_EN) != 0, st_en = (s->tex_cntl & TC_STENCIL_EN) && s->z_max == 0xFFFFFFu;
    if (z_en || st_en) {
        uint32_t zat = s->z_base + (uint32_t)y * s->z_stride + (uint32_t)x * s->z_bytes;
        if (zat + s->z_bytes > r->vram_size)
            return;
        uint32_t zw = 0;
        for (uint32_t i = 0; i < s->z_bytes; i++)
            zw |= (uint32_t)r->vram[zat + i] << (8u * i);
        uint64_t zsrc = (uint64_t)(f->z <= 0.0 ? 0.0 : f->z >= 1.0 ? (double)s->z_max : f->z * (double)s->z_max);
        uint64_t zdst = s->z_max == 0xFFFFFFu ? (zw & 0xFFFFFFu) : zw;
        bool zpass = !z_en || cmp_op(ZS_Z_TEST(s->zs), zsrc, zdst);
        uint32_t nw = zw;
        bool spass = true;
        if (st_en) {
            uint32_t ref = s->sten & 0xFFu, msk = (s->sten >> 16) & 0xFFu, wm = (s->sten >> 24) & 0xFFu;
            uint32_t sv = zw >> 24;
            spass = cmp_op(ZS_S_TEST(s->zs), ref & msk, sv & msk);
            uint32_t op = !spass ? ZS_SFAIL(s->zs) : !zpass ? ZS_ZFAIL(s->zs) : ZS_ZPASS(s->zs);
            uint32_t ns = sten_op(op, sv, ref);
            ns = (ns & wm) | (sv & ~wm);
            nw = (nw & 0x00FFFFFFu) | (ns << 24);
        }
        if (spass && zpass && z_en && (s->tex_cntl & TC_Z_MASK))
            nw = s->z_max == 0xFFFFFFu ? ((nw & 0xFF000000u) | (uint32_t)zsrc) : (uint32_t)zsrc;
        for (uint32_t i = 0; i < s->z_bytes; i++)
            r->vram[zat + i] = (uint8_t)(nw >> (8u * i));
        if (!spass || !zpass)
            return;
    }
    // Blend.
    uint32_t old = dst_read(s, at);
    bool blending = (s->tex_cntl & TC_ALPHA_EN) != 0;
    if (blending) {
        col_t d = dst_unpack(s, old), o;
        uint32_t fs = MISC_BLND_SRC(s->misc), fd = MISC_BLND_DST(s->misc);
        for (int k = 0; k < 4; k++) {
            bool force = false;
            uint32_t fd2 = fd;
            int32_t sf = blend_factor(fs, k, c, d, true, &force, &fd2);
            int32_t df = blend_factor(force ? fd2 : fd, k, c, d, false, &force, &fd2);
            int32_t a = mul255(c.c[k], sf), b = mul255(d.c[k], df);
            switch (MISC_COMB_FN(s->misc)) {
            case 0:
                o.c[k] = clamp255(a + b);
                break;
            case 1:
                o.c[k] = (a + b) & 0xFF; // unclamped: wraps (CHOSEN)
                break;
            case 2:
                o.c[k] = clamp255(a - b);
                break;
            default:
                o.c[k] = (a - b) & 0xFF;
                break;
            }
        }
        c = o;
    }
    uint32_t v = dst_pack(s, c, x, y, blending);
    if (s->rop != 0xCCu) {
        // The 3D pixel is the ROP's source; the brush colour its pattern.
        tell_once(r, TOLD_ROP, "3D pixels go through a ROP3 other than SRCCOPY");
        v = r128_2d_rop3(s->rop, REG(r, R_BRUSH_FRGD_CLR), v, old);
    }
    v = (v & s->write_mask) | (old & ~s->write_mask);
    for (uint32_t i = 0; i < s->dst_bpp; i++)
        r->vram[at + i] = (uint8_t)(v >> (8u * i));
}

// ============================================================
// Setup: attribute planes
// ============================================================

// The attributes a primitive interpolates, as an array per vertex.
enum {
    A_Z,
    A_R,
    A_G,
    A_B,
    A_A,
    A_SR,
    A_SG,
    A_SB,
    A_FOG,
    A_S0, // S·W (or S), primary set
    A_T0,
    A_W0, // W for the primary set
    A_S1,
    A_T1,
    A_W1,
    A_COUNT
};

typedef struct svtx {
    double x, y; // snapped screen position
    double a[A_COUNT];
} svtx_t;

static double snap(const st_t *s, double v) {
    double q = v * s->snap;
    q = s->round_xy ? floor(q + 0.5) : (q < 0 ? ceil(q) : floor(q));
    return q / s->snap;
}

static void prepare(const st_t *s, const r128_vertex_t *v, const r128_vertex_t *flat, svtx_t *o) {
    o->x = snap(s, (double)v->x + s->win_x);
    o->y = snap(s, (double)v->y + s->win_y);
    o->a[A_Z] = v->z;
    const r128_vertex_t *cv = flat ? flat : v;
    for (int k = 0; k < 4; k++)
        o->a[A_R + k] = cv->c[k];
    for (int k = 0; k < 3; k++)
        o->a[A_SR + k] = cv->spec[k];
    o->a[A_FOG] = v->fog;
    if (FPU_COLOR_FN(s->fpu) == COLOR_FN_SOLID) {
        col_t cc = unpack_argb(s->constant);
        for (int k = 0; k < 4; k++)
            o->a[A_R + k] = cc.c[k];
    }
    for (int u = 0; u < 2; u++) {
        const tex_unit_t *t = &s->t[u];
        bool second = u == 1 && t->sec_st;
        double sc = second ? v->s2 : v->s, tc = second ? v->t2 : v->t;
        double w = (u == 1 && t->sec_w) ? v->rhw2 : v->rhw;
        if (!t->persp)
            w = 1.0;
        o->a[A_S0 + 3 * u] = sc * w;
        o->a[A_T0 + 3 * u] = tc * w;
        o->a[A_W0 + 3 * u] = w;
    }
}

typedef struct plane {
    double a0, ax, ay; // a(x, y) = a0 + ax·x + ay·y
} plane_t;

static void make_frag(const st_t *s, const plane_t *p, double px, double py, frag_t *f) {
    double av[A_COUNT];
    for (int i = 0; i < A_COUNT; i++)
        av[i] = p[i].a0 + p[i].ax * px + p[i].ay * py;
    f->z = av[A_Z];
    for (int k = 0; k < 4; k++)
        f->c[k] = av[A_R + k];
    for (int k = 0; k < 3; k++)
        f->spec[k] = av[A_SR + k];
    f->fog = av[A_FOG];
    for (int u = 0; u < 2; u++) {
        const tex_unit_t *t = &s->t[u];
        f->s[u] = f->t[u] = f->lod[u] = 0.0;
        if (!t->on)
            continue;
        int is = A_S0 + 3 * u, it = A_T0 + 3 * u, iw = A_W0 + 3 * u;
        double w = av[iw];
        if (w == 0.0)
            w = 1e-30;
        double sc = av[is] / w, tc = av[it] / w;
        f->s[u] = sc;
        f->t[u] = tc;
        // The texel step: the pixel's neighbours to the right and below,
        // in texels of the largest map.
        double w1 = av[iw] + p[iw].ax, w2 = av[iw] + p[iw].ay;
        if (w1 == 0.0)
            w1 = 1e-30;
        if (w2 == 0.0)
            w2 = 1e-30;
        double tw = (double)(1u << t->lpitch), th = (double)(1u << t->lheight);
        double dsx = ((av[is] + p[is].ax) / w1 - sc) * tw, dtx = ((av[it] + p[it].ax) / w1 - tc) * th;
        double dsy = ((av[is] + p[is].ay) / w2 - sc) * tw, dty = ((av[it] + p[it].ay) / w2 - tc) * th;
        double rho = fmax(fmax(fabs(dsx), fabs(dtx)), fmax(fabs(dsy), fabs(dty)));
        double lod = rho > 0.0 ? log2(rho) : -64.0;
        f->lod[u] = lod - (double)TC_LOD_BIAS(s->tex_cntl) / 128.0;
    }
}

// ============================================================
// Primitives
// ============================================================

static void draw_point(st_t *s, const svtx_t *v) {
    plane_t p[A_COUNT];
    for (int i = 0; i < A_COUNT; i++)
        p[i] = (plane_t){v->a[i], 0.0, 0.0};
    frag_t f;
    int32_t x = (int32_t)floor(v->x), y = (int32_t)floor(v->y);
    make_frag(s, p, v->x, v->y, &f);
    shade(s, x, y, &f);
}

static void draw_line(st_t *s, const svtx_t *a, const svtx_t *b) {
    double dx = b->x - a->x, dy = b->y - a->y;
    double len = fmax(fabs(dx), fabs(dy));
    int32_t n = (int32_t)floor(len + 0.5);
    if (n <= 0)
        return;
    for (int32_t i = 0; i < n; i++) {
        double t = (double)i / (double)n;
        double x = a->x + dx * t, y = a->y + dy * t;
        plane_t p[A_COUNT];
        for (int k = 0; k < A_COUNT; k++)
            p[k] = (plane_t){a->a[k] + (b->a[k] - a->a[k]) * t, 0.0, 0.0};
        frag_t f;
        make_frag(s, p, 0.0, 0.0, &f);
        // A line samples its texture at the largest map.
        f.lod[0] = f.lod[1] = -64.0;
        shade(s, (int32_t)floor(x + 0.5), (int32_t)floor(y + 0.5), &f);
    }
}

static bool top_left(int64_t dx, int64_t dy) {
    return (dy == 0 && dx > 0) || dy < 0;
}

static void draw_tri(st_t *s, const svtx_t *v0, const svtx_t *v1, const svtx_t *v2) {
    // Exact coverage on the sub-pixel grid: positions in 1/16 pixel.
    int64_t X[3], Y[3];
    const svtx_t *vv[3] = {v0, v1, v2};
    for (int i = 0; i < 3; i++) {
        X[i] = (int64_t)floor(vv[i]->x * 16.0 + 0.5);
        Y[i] = (int64_t)floor(vv[i]->y * 16.0 + 0.5);
    }
    int64_t area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
    if (area == 0)
        return;
    if (area < 0) {
        int64_t tx = X[1], ty = Y[1];
        X[1] = X[2], Y[1] = Y[2];
        X[2] = tx, Y[2] = ty;
        const svtx_t *t = vv[1];
        vv[1] = vv[2];
        vv[2] = t;
        area = -area;
    }
    // Attribute planes from the (possibly reordered) vertices.
    plane_t p[A_COUNT];
    double x0 = vv[0]->x, y0 = vv[0]->y;
    double x1 = vv[1]->x - x0, y1 = vv[1]->y - y0, x2 = vv[2]->x - x0, y2 = vv[2]->y - y0;
    double det = x1 * y2 - x2 * y1;
    if (det == 0.0)
        return;
    for (int i = 0; i < A_COUNT; i++) {
        double a0 = vv[0]->a[i], d1 = vv[1]->a[i] - a0, d2 = vv[2]->a[i] - a0;
        double ax = (d1 * y2 - d2 * y1) / det, ay = (d2 * x1 - d1 * x2) / det;
        p[i] = (plane_t){a0 - ax * x0 - ay * y0, ax, ay};
    }
    int64_t minx = X[0], maxx = X[0], miny = Y[0], maxy = Y[0];
    for (int i = 1; i < 3; i++) {
        minx = X[i] < minx ? X[i] : minx, maxx = X[i] > maxx ? X[i] : maxx;
        miny = Y[i] < miny ? Y[i] : miny, maxy = Y[i] > maxy ? Y[i] : maxy;
    }
    int32_t px0 = (int32_t)(minx >> 4), px1 = (int32_t)(maxx >> 4), py0 = (int32_t)(miny >> 4),
            py1 = (int32_t)(maxy >> 4);
    if (px0 < s->sc_l)
        px0 = s->sc_l;
    if (px1 > s->sc_r)
        px1 = s->sc_r;
    if (py0 < s->sc_t)
        py0 = s->sc_t;
    if (py1 > s->sc_b)
        py1 = s->sc_b;
    for (int32_t py = py0; py <= py1; py++) {
        int64_t cy = (int64_t)py * 16 + 8;
        for (int32_t px = px0; px <= px1; px++) {
            int64_t cx = (int64_t)px * 16 + 8;
            bool in = true;
            for (int e = 0; e < 3 && in; e++) {
                int a = e, b = (e + 1) % 3;
                int64_t ex = X[b] - X[a], ey = Y[b] - Y[a];
                int64_t E = ex * (cy - Y[a]) - ey * (cx - X[a]);
                in = E > 0 || (E == 0 && top_left(ex, ey));
            }
            if (!in)
                continue;
            frag_t f;
            make_frag(s, p, (double)px + 0.5, (double)py + 0.5, &f);
            shade(s, px, py, &f);
        }
    }
}

// A triangle through culling: which face it shows, and that face's mode.
static void triangle(st_t *s, const r128_vertex_t *a, const r128_vertex_t *b, const r128_vertex_t *c) {
    const r128_vertex_t *flat = NULL;
    if (FPU_COLOR_FN(s->fpu) == COLOR_FN_FLAT)
        flat = (s->fpu & FPU_FLAT_LAST) ? c : a;
    svtx_t v[3];
    prepare(s, a, flat, &v[0]);
    prepare(s, b, flat, &v[1]);
    prepare(s, c, flat, &v[2]);
    double area = (v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[2].x - v[0].x) * (v[1].y - v[0].y);
    // On the y-down screen a counter-clockwise triangle has negative area.
    bool front = (s->fpu & FPU_FRONT_CCW) ? area < 0.0 : area > 0.0;
    uint32_t mode = front ? FPU_FRONT_FN(s->fpu) : FPU_BACK_FN(s->fpu);
    s->r->prims3d++;
    switch (mode) {
    case CULL_FN_CULL:
        return;
    case CULL_FN_POINTS:
        for (int i = 0; i < 3; i++)
            draw_point(s, &v[i]);
        return;
    case CULL_FN_LINES:
        draw_line(s, &v[0], &v[1]);
        draw_line(s, &v[1], &v[2]);
        draw_line(s, &v[2], &v[0]);
        return;
    default:
        draw_tri(s, &v[0], &v[1], &v[2]);
        return;
    }
}

static void line(st_t *s, const r128_vertex_t *a, const r128_vertex_t *b) {
    const r128_vertex_t *flat = NULL;
    if (FPU_COLOR_FN(s->fpu) == COLOR_FN_FLAT)
        flat = (s->fpu & FPU_FLAT_LAST) ? b : a;
    svtx_t v[2];
    prepare(s, a, flat, &v[0]);
    prepare(s, b, flat, &v[1]);
    s->r->prims3d++;
    draw_line(s, &v[0], &v[1]);
}

void r128_3d_draw(rage128_t *r, uint32_t prim, const r128_vertex_t *v, uint32_t n) {
    st_t s;
    if (!n || !gather(r, &s))
        return;
    switch (prim) {
    case 1: // points
        for (uint32_t i = 0; i < n; i++) {
            svtx_t sv;
            prepare(&s, &v[i], NULL, &sv);
            r->prims3d++;
            draw_point(&s, &sv);
        }
        break;
    case 2: // independent lines
        for (uint32_t i = 0; i + 1 < n; i += 2)
            line(&s, &v[i], &v[i + 1]);
        break;
    case 3: // polyline
        for (uint32_t i = 0; i + 1 < n; i++)
            line(&s, &v[i], &v[i + 1]);
        break;
    case 7: // "type-2 triangles": undocumented; drawn as a list
        tell_once(r, TOLD_PRIM7, "primitive type 7 is drawn as a triangle list");
        // fall through
    case 4: // triangle list
        for (uint32_t i = 0; i + 2 < n; i += 3)
            triangle(&s, &v[i], &v[i + 1], &v[i + 2]);
        break;
    case 5: // fan
        for (uint32_t i = 1; i + 1 < n; i++)
            triangle(&s, &v[0], &v[i], &v[i + 1]);
        break;
    case 6: // strip: every other triangle reversed to keep the winding
        for (uint32_t i = 0; i + 2 < n; i++) {
            if (i & 1u)
                triangle(&s, &v[i + 1], &v[i], &v[i + 2]);
            else
                triangle(&s, &v[i], &v[i + 1], &v[i + 2]);
        }
        break;
    default:
        break;
    }
}
