// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rage128_raster_priv.h
// The Rage 128's 3D engine state as rage128_raster.c snapshots it, shared
// with the WebGPU takeover's translator (rage128_gpu.c), which turns the
// same snapshot and the same set-up vertices into GPU draws.  Not a public
// header: only the card's own files include it.

#ifndef PCI_RAGE128_RASTER_PRIV_H
#define PCI_RAGE128_RASTER_PRIV_H

#include "rage128_priv.h"

#include <stdbool.h>
#include <stdint.h>

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

// ============================================================
// The state snapshot
// ============================================================

typedef struct tex_unit {
    bool on;
    uint32_t cntl, comb, fmt;
    uint32_t lpitch, lheight, lsize, lmin; // log2 of the largest map's pitch (= width), height, max, smallest
    uint32_t off[11];
    uint32_t border;
    bool persp; // divide by the interpolated W per pixel
    bool premult; // the vertex S and T are already S·W (TEXTURE_ST_FORMAT)
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
    bool gpu; // the WebGPU takeover draws this batch's triangles (rage128_gpu.c)
} st_t;

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

#endif // PCI_RAGE128_RASTER_PRIV_H
