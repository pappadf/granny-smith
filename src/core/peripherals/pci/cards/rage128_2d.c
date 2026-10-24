// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rage128_2d.c
// The ATI Rage 128 GL's 2D draw engine: rectangles, blits from VRAM and
// from host data, monochrome expansion, brushes, the full ROP3, the write
// mask, colour-compare transparency, scissors, and Bresenham lines.
//
// The engine is register-programmed (the card's PIO path; the CCE feeds the
// same registers in a later milestone).  A driver loads DP_GUI_MASTER_CNTL —
// which also re-defaults the surfaces and scissors unless told to leave them
// — the colours, the data path and the trajectory, then writes an INITIATOR,
// and the operation runs.  It runs to completion at once: GUI_STAT reports
// the engine idle and the FIFO empty, so a driver's wait-for-idle never
// spins (the card's GUI_STAT read, rage128.c).
//
// Initiators.  The reference names DST_WIDTH_BW (block-write fill) as one;
// the rest follow the ATI register lineage the Mach64 model already pins:
// a rectangle runs when its HEIGHT arrives — DST_HEIGHT, or any combined
// register carrying it (DST_HEIGHT_WIDTH, DST_WIDTH_HEIGHT,
// DST_HEIGHT_WIDTH_8, DST_HEIGHT_WIDTH_BW, DST_HEIGHT_Y) — or with
// DST_WIDTH_X / DST_WIDTH_X_INCY, which start a span of the current height;
// a line runs on DST_BRES_LNTH.  A host-data operation then consumes pixels
// from HOST_DATA0..7 / HOST_DATA_LAST until the rectangle is full.  Every
// operation is logged (video level 3), so a driver that initiates some
// other way announces itself.
//
// Register truth: ATI, *RAGE 128 VR / RAGE 128 GL Register Reference Guide*
// (RRG-G04100-C Rev 0.02, 1999), chapter 7; ATI, *RAGE 128 Software
// Development Guide* (SDK-G04000 Rev 0.01, 1999), chapter 4.

#include "log.h"
#include "rage128_gpu.h"
#include "rage128_priv.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("video");

// ============================================================
// GUI register offsets (RRG chapter 7)
// ============================================================

#define G_DST_OFFSET           0x1404u
#define G_DST_PITCH            0x1408u
#define G_DST_WIDTH            0x140Cu
#define G_DST_HEIGHT           0x1410u
#define G_SRC_X                0x1414u
#define G_SRC_Y                0x1418u
#define G_DST_X                0x141Cu
#define G_DST_Y                0x1420u
#define G_SRC_PITCH_OFFSET     0x1428u
#define G_DST_PITCH_OFFSET     0x142Cu
#define G_SRC_Y_X              0x1434u
#define G_DST_Y_X              0x1438u
#define G_DST_HEIGHT_WIDTH     0x143Cu
#define G_DP_GUI_MASTER_CNTL   0x146Cu
#define G_BRUSH_SCALE          0x1470u
#define G_BRUSH_Y_X            0x1474u
#define G_DP_BRUSH_BKGD_CLR    0x1478u
#define G_DP_BRUSH_FRGD_CLR    0x147Cu
#define G_BRUSH_DATA0          0x1480u
#define G_DST_WIDTH_X          0x1588u
#define G_DST_HEIGHT_WIDTH_8   0x158Cu
#define G_SRC_X_Y              0x1590u
#define G_DST_X_Y              0x1594u
#define G_DST_WIDTH_HEIGHT     0x1598u
#define G_DST_WIDTH_X_INCY     0x159Cu
#define G_DST_HEIGHT_Y         0x15A0u
#define G_SRC_OFFSET           0x15ACu
#define G_SRC_PITCH            0x15B0u
#define G_DST_HEIGHT_WIDTH_BW  0x15B4u
#define G_CLR_CMP_CNTL         0x15C0u
#define G_CLR_CMP_CLR_SRC      0x15C4u
#define G_CLR_CMP_CLR_DST      0x15C8u
#define G_CLR_CMP_MSK          0x15CCu
#define G_DP_SRC_FRGD_CLR      0x15D8u
#define G_DP_SRC_BKGD_CLR      0x15DCu
#define G_DST_BRES_ERR         0x1628u
#define G_DST_BRES_INC         0x162Cu
#define G_DST_BRES_DEC         0x1630u
#define G_DST_BRES_LNTH        0x1634u
#define G_SC_LEFT              0x1640u
#define G_SC_RIGHT             0x1644u
#define G_SC_TOP               0x1648u
#define G_SC_BOTTOM            0x164Cu
#define G_SRC_SC_RIGHT         0x1654u
#define G_SRC_SC_BOTTOM        0x165Cu
#define G_DP_CNTL              0x16C0u
#define G_DP_DATATYPE          0x16C4u
#define G_DP_MIX               0x16C8u
#define G_DP_WRITE_MSK         0x16CCu
#define G_DP_CNTL_XDIR_YDIR    0x16D0u
#define G_DEFAULT_OFFSET       0x16E0u
#define G_DEFAULT_PITCH        0x16E4u
#define G_DEFAULT_SC_BR        0x16E8u
#define G_SC_TOP_LEFT          0x16ECu
#define G_SC_BOTTOM_RIGHT      0x16F0u
#define G_SRC_SC_BOTTOM_RIGHT  0x16F4u
#define G_HOST_DATA0           0x17C0u
#define G_HOST_DATA7           0x17DCu
#define G_HOST_DATA_LAST       0x17E0u
#define G_DP_GUI_MASTER_CNTL_C 0x1C84u
#define G_AUX_SC_CNTL          0x1660u
// The 3D engine's CCE-context copies that alias 2D state (RRG §7.5; the CCE
// 3D-packet supplement): the render target, the scissors, the write mask.
#define G_DST_PITCH_OFFSET_C 0x1C80u
#define G_SC_TOP_LEFT_C      0x1C88u
#define G_SC_BOTTOM_RIGHT_C  0x1C8Cu
#define G_PLANE_3D_MASK_C    0x1D44u
// SCALE_3D_CNTL and MISC_3D_STATE_CNTL_REG share the blend, alpha-test and
// fog-table fields (bits 26:12); SCALE_3D_FN sits at 7:6 in the first and
// 9:8 in the second.
#define G_SCALE_3D_CNTL  0x1A00u
#define G_MISC_3D_STATE  0x1CA0u
#define G_TEX_CNTL_C     0x1C9Cu
#define SHARED_3D_FIELDS 0x07FFF000u

#define REG(r, off) ((r)->reg[(off) / 4u])

// DP_GUI_MASTER_CNTL (RRG §7.5).
#define GMC_SRC_PITCH_OFFSET_CNTL 0x00000001u // 1 = leave SRC_OFFSET/PITCH alone
#define GMC_DST_PITCH_OFFSET_CNTL 0x00000002u // 1 = leave DST_OFFSET/PITCH alone
#define GMC_SRC_CLIPPING          0x00000004u // 1 = leave the source scissors alone
#define GMC_DST_CLIPPING          0x00000008u // 1 = leave the destination scissors alone
#define GMC_BRUSH_DATATYPE(v)     (((v) >> 4) & 0xFu)
#define GMC_DST_DATATYPE(v)       (((v) >> 8) & 0xFu)
#define GMC_SRC_DATATYPE(v)       (((v) >> 12) & 3u)
#define GMC_BYTE_PIX_ORDER        0x00004000u
#define GMC_ROP3(v)               (((v) >> 16) & 0xFFu)
#define GMC_SRC_SOURCE(v)         (((v) >> 24) & 7u)
#define GMC_3D_FCN_EN             0x08000000u // 1 = leave the 3D function, Z and stencil enables alone
#define GMC_CLR_CMP_CNTL_DIS      0x10000000u
#define GMC_AUX_CLIP_DIS          0x20000000u
#define GMC_WR_MSK_DIS            0x40000000u

// DP_DATATYPE.
#define DT_DST(v)            ((v) & 0xFu)
#define DT_BRUSH(v)          (((v) >> 8) & 0xFu)
#define DT_SRC(v)            (((v) >> 16) & 3u)
#define DT_HOST_BIG_ENDIAN   0x20000000u
#define DT_BYTE_PIX_ORDER    0x40000000u
#define DT_KEEP_ON_GMC_WRITE 0xA0000000u // HOST_BIG_ENDIAN_EN and CONVERSION_TEMP: not GMC fields

// Source datatypes.
#define SRC_MONO_FG_BG 0u
#define SRC_MONO_FG_LA 1u
#define SRC_COLOR      3u

// DP_MIX: the source in 10:8, the ROP3 in 23:16.
#define MIX_SRC_SOURCE(v) (((v) >> 8) & 7u)
#define MIX_ROP3(v)       (((v) >> 16) & 0xFFu)
#define SRC_SOURCE_MEMORY 2u // rectangular trajectory in VRAM
#define SRC_SOURCE_HOST   3u // host data, packed
#define SRC_SOURCE_HOST_B 4u // host data, each line byte-aligned

// DP_CNTL.
#define DPC_X_LTR     0x001u // left to right
#define DPC_Y_TTB     0x002u // top to bottom
#define DPC_Y_MAJOR   0x004u
#define DPC_LAST_PEL  0x020u
#define DPC_BRES_SIGN 0x100u
#define DPC_POLY_LINE 0x8000u

// Brush types (DP_BRUSH_DATATYPE).
#define BR_8X8_MONO_FG_BG   0u
#define BR_8X8_MONO_FG_LA   1u
#define BR_8X1_MONO_FG_BG   2u
#define BR_8X1_MONO_FG_LA   3u
#define BR_1X8_MONO_FG_BG   4u
#define BR_1X8_MONO_FG_LA   5u
#define BR_32X1_MONO_FG_BG  6u
#define BR_32X1_MONO_FG_LA  7u
#define BR_32X32_MONO_FG_BG 8u
#define BR_32X32_MONO_FG_LA 9u
#define BR_8X8_COLOR        10u
#define BR_8X1_COLOR        11u
#define BR_1X8_COLOR        12u
#define BR_SOLID_COLOR      13u
#define BR_SOLID_COLOR_B    14u
#define BR_NONE             15u

// CLR_CMP_CNTL (RRG §7.7).
#define CMP_FN_SRC(v) ((v) & 7u)
#define CMP_FN_DST(v) (((v) >> 8) & 7u)
#define CMP_WHICH(v)  (((v) >> 24) & 3u) // 0 destination, 1 source, 2 both
#define CMP_FALSE     0u // always draw
#define CMP_TRUE      1u // never draw
#define CMP_EQ_COLOR  4u // draw where the pixel equals the reference
#define CMP_NEQ_COLOR 5u // draw where it differs
#define CMP_FN_MASK   0x00000707u

// A 14-bit signed coordinate field.
static int32_t s14(uint32_t v) {
    return (int32_t)((v & 0x3FFFu) ^ 0x2000u) - 0x2000;
}

// ============================================================
// Pixel formats
// ============================================================

// Bytes per pixel of a datapath type, or 0 for one the engine does not draw.
static uint32_t dt_bytes(uint32_t dt) {
    switch (dt) {
    case 2: // 8 bpp pseudo-colour
    case 7: // 8 bpp RGB 3-3-2
    case 8: // Y8
    case 9: // RGB8 greyscale
        return 1;
    case 3: // 1-5-5-5
    case 4: // 5-6-5
    case 15: // 4-4-4-4
        return 2;
    case 5: // packed 24 bpp
        return 3;
    case 6: // 8-8-8-8
        return 4;
    default:
        return 0;
    }
}

// One pixel of VRAM, little-endian as the chip holds it.
static uint32_t vram_get(rage128_t *r, uint32_t at, uint32_t bpp) {
    r128_vram_access(r, at, bpp, false);
    uint32_t v = 0;
    for (uint32_t i = 0; i < bpp; i++)
        v |= (uint32_t)r->vram[at + i] << (8u * i);
    return v;
}

static void vram_put(rage128_t *r, uint32_t at, uint32_t bpp, uint32_t v) {
    r128_vram_access(r, at, bpp, true);
    for (uint32_t i = 0; i < bpp; i++)
        r->vram[at + i] = (uint8_t)(v >> (8u * i));
}

// The ternary raster operation: each result bit picks bit (P,S,D) of the
// ROP3 code — the Windows 3.1 encoding the engine implements in full.
static uint32_t rop3(uint32_t rop, uint32_t p, uint32_t s, uint32_t d) {
    uint32_t out = 0;
    for (uint32_t i = 0; i < 8; i++) {
        if (!(rop & (1u << i)))
            continue;
        uint32_t t = ((i & 4u) ? p : ~p) & ((i & 2u) ? s : ~s) & ((i & 1u) ? d : ~d);
        out |= t;
    }
    return out;
}

// ============================================================
// One operation
// ============================================================

// Everything an operation reads, gathered once at initiation.
typedef struct op {
    uint32_t bpp; // destination bytes per pixel
    uint32_t mask; // all-ones of the pixel width
    uint32_t dst_base, dst_stride; // bytes
    uint32_t src_base, src_stride; // bytes (mono sources: a line is stride bytes)
    int32_t sc_left, sc_right, sc_top, sc_bottom; // inclusive
    uint32_t rop, write_mask;
    uint32_t brush_type, src_type, src_source;
    bool lsb_first; // DP_BYTE_PIX_ORDER: mono pixels LSB-first within a byte
    bool host_be; // HOST_BIG_ENDIAN_EN
    uint32_t cmp_cntl, cmp_src, cmp_dst, cmp_mask;
    uint32_t brush_fg, brush_bg, src_fg, src_bg;
    bool uses_s, uses_p, uses_d; // which ROP3 operands matter
} op_t;

static void op_gather(rage128_t *r, op_t *op) {
    uint32_t dt = REG(r, G_DP_DATATYPE), mix = REG(r, G_DP_MIX);
    op->bpp = dt_bytes(DT_DST(dt));
    op->mask = op->bpp >= 4 ? 0xFFFFFFFFu : ((1u << (8u * op->bpp)) - 1u);
    // DST_PITCH and SRC_PITCH are in units of 8 pixels (RRG §7.1, §7.2).
    op->dst_base = REG(r, G_DST_OFFSET) & 0x3FFFFF0u;
    op->dst_stride = (REG(r, G_DST_PITCH) & 0x3FFu) * 8u * op->bpp;
    op->src_type = DT_SRC(dt);
    op->src_base = REG(r, G_SRC_OFFSET) & 0x3FFFFF0u;
    op->src_stride = (REG(r, G_SRC_PITCH) & 0x3FFu) * 8u * (op->src_type == SRC_COLOR ? op->bpp : 1u);
    if (op->src_type != SRC_COLOR)
        op->src_stride /= 8u; // a mono line: one bit per pixel
    op->sc_left = s14(REG(r, G_SC_LEFT));
    op->sc_right = s14(REG(r, G_SC_RIGHT));
    op->sc_top = s14(REG(r, G_SC_TOP));
    op->sc_bottom = s14(REG(r, G_SC_BOTTOM));
    op->rop = MIX_ROP3(mix);
    op->write_mask = REG(r, G_DP_WRITE_MSK);
    op->brush_type = DT_BRUSH(dt);
    op->src_source = MIX_SRC_SOURCE(mix);
    op->lsb_first = (dt & DT_BYTE_PIX_ORDER) != 0;
    op->host_be = (dt & DT_HOST_BIG_ENDIAN) != 0;
    op->cmp_cntl = REG(r, G_CLR_CMP_CNTL);
    op->cmp_src = REG(r, G_CLR_CMP_CLR_SRC);
    op->cmp_dst = REG(r, G_CLR_CMP_CLR_DST);
    op->cmp_mask = REG(r, G_CLR_CMP_MSK);
    op->brush_fg = REG(r, G_DP_BRUSH_FRGD_CLR);
    op->brush_bg = REG(r, G_DP_BRUSH_BKGD_CLR);
    op->src_fg = REG(r, G_DP_SRC_FRGD_CLR);
    op->src_bg = REG(r, G_DP_SRC_BKGD_CLR);
    // Which operands the ROP3 reads: an operand matters iff flipping it
    // changes the code's truth table.
    op->uses_p = ((op->rop >> 4) & 0x0Fu) != (op->rop & 0x0Fu);
    op->uses_s = (((op->rop >> 2) & 0x33u) != (op->rop & 0x33u));
    op->uses_d = (((op->rop >> 1) & 0x55u) != (op->rop & 0x55u));
}

// The brush (pattern) operand at destination pixel (x, y), or false when
// the brush leaves this pixel alone.
static bool brush_at(const rage128_t *r, const op_t *op, int32_t x, int32_t y, uint32_t *out) {
    uint32_t bx = (uint32_t)x, by = (uint32_t)y;
    uint32_t t = op->brush_type;
    if (t == BR_SOLID_COLOR || t == BR_SOLID_COLOR_B || t == BR_NONE) {
        *out = op->brush_fg;
        return true;
    }
    // The brush registers hold little-endian dwords; byte n of the pattern
    // is byte n of BRUSH_DATA0.. as the chip sees it.
    uint8_t bytes[256];
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t w = r->reg[G_BRUSH_DATA0 / 4u + i];
        bytes[4 * i] = (uint8_t)w;
        bytes[4 * i + 1] = (uint8_t)(w >> 8);
        bytes[4 * i + 2] = (uint8_t)(w >> 16);
        bytes[4 * i + 3] = (uint8_t)(w >> 24);
    }
    bool mono = t <= BR_32X32_MONO_FG_LA;
    if (mono) {
        uint32_t bit;
        switch (t) {
        case BR_8X8_MONO_FG_BG:
        case BR_8X8_MONO_FG_LA: {
            uint8_t row = bytes[by & 7u];
            uint32_t col = bx & 7u;
            bit = (row >> (op->lsb_first ? col : 7u - col)) & 1u;
            break;
        }
        case BR_8X1_MONO_FG_BG:
        case BR_8X1_MONO_FG_LA: {
            uint32_t col = bx & 7u;
            bit = (bytes[0] >> (op->lsb_first ? col : 7u - col)) & 1u;
            break;
        }
        case BR_1X8_MONO_FG_BG:
        case BR_1X8_MONO_FG_LA: {
            bit = (bytes[0] >> (op->lsb_first ? (by & 7u) : 7u - (by & 7u))) & 1u;
            break;
        }
        case BR_32X1_MONO_FG_BG:
        case BR_32X1_MONO_FG_LA: {
            uint32_t col = bx & 31u;
            uint8_t b = bytes[col >> 3];
            bit = (b >> (op->lsb_first ? (col & 7u) : 7u - (col & 7u))) & 1u;
            break;
        }
        default: { // 32 x 32
            uint32_t col = bx & 31u;
            uint8_t b = bytes[(by & 31u) * 4u + (col >> 3)];
            bit = (b >> (op->lsb_first ? (col & 7u) : 7u - (col & 7u))) & 1u;
            break;
        }
        }
        bool leave_alone = (t & 1u) != 0; // the odd mono types are FG / leave-alone
        if (bit) {
            *out = op->brush_fg;
            return true;
        }
        if (leave_alone)
            return false;
        *out = op->brush_bg;
        return true;
    }
    // Colour brushes: 8 x 8 / 8 x 1 / 1 x 8 pixels at the destination depth.
    uint32_t px;
    if (t == BR_8X8_COLOR)
        px = (by & 7u) * 8u + (bx & 7u);
    else if (t == BR_8X1_COLOR)
        px = bx & 7u;
    else
        px = by & 7u;
    uint32_t at = px * op->bpp;
    uint32_t v = 0;
    for (uint32_t i = 0; i < op->bpp && at + i < sizeof(bytes); i++)
        v |= (uint32_t)bytes[at + i] << (8u * i);
    *out = v;
    return true;
}

// Write one pixel through the ROP, the write mask and the colour compare.
// `have_s` false means the source leaves this pixel alone (mono
// leave-alone expansion), which suppresses the write.
static void pixel(rage128_t *r, const op_t *op, int32_t x, int32_t y, uint32_t s, bool have_s) {
    if (x < op->sc_left || x > op->sc_right || y < op->sc_top || y > op->sc_bottom)
        return;
    if (x < 0 || y < 0)
        return;
    uint64_t at = (uint64_t)op->dst_base + (uint64_t)y * op->dst_stride + (uint64_t)x * op->bpp;
    if (at + op->bpp > r->vram_size)
        return;
    if (op->uses_s && !have_s)
        return;
    uint32_t p = 0;
    if (op->uses_p && !brush_at(r, op, x, y, &p))
        return;
    uint32_t d = vram_get(r, (uint32_t)at, op->bpp);
    // Colour compare (RRG §7.7): the comparison gates the write.  The
    // register reference and the SDK's Table 4-2 disagree on the sense of
    // the DESTINATION codes; the reference — "4 = draw when eq, 5 = draw
    // when neq" for both comparators — is followed.
    uint32_t which = CMP_WHICH(op->cmp_cntl);
    if (which == 1 || which == 2) {
        uint32_t fn = CMP_FN_SRC(op->cmp_cntl);
        bool eq = ((s ^ op->cmp_src) & op->cmp_mask & op->mask) == 0;
        if (fn == CMP_TRUE || (fn == CMP_EQ_COLOR && !eq) || (fn == CMP_NEQ_COLOR && eq))
            return;
    }
    if (which == 0 || which == 2) {
        uint32_t fn = CMP_FN_DST(op->cmp_cntl);
        bool eq = ((d ^ op->cmp_dst) & op->cmp_mask & op->mask) == 0;
        if (fn == CMP_TRUE || (fn == CMP_EQ_COLOR && !eq) || (fn == CMP_NEQ_COLOR && eq))
            return;
    }
    uint32_t v = rop3(op->rop, p, s, d);
    v = (v & op->write_mask) | (d & ~op->write_mask);
    vram_put(r, (uint32_t)at, op->bpp, v & op->mask);
}

// A mono bit of a VRAM source line.
static bool mono_bit(rage128_t *r, const op_t *op, uint32_t line_at, uint32_t col) {
    uint32_t at = line_at + (col >> 3);
    if (at >= r->vram_size)
        return false;
    r128_vram_access(r, at, 1, false);
    uint8_t b = r->vram[at];
    return (b >> (op->lsb_first ? (col & 7u) : 7u - (col & 7u))) & 1u;
}

// Expand a mono source bit to the source operand; false = leave alone.
static bool mono_expand(const op_t *op, bool bit, uint32_t *s) {
    if (bit) {
        *s = op->src_fg;
        return true;
    }
    if (op->src_type == SRC_MONO_FG_LA)
        return false;
    *s = op->src_bg;
    return true;
}

// Advance the destination after a rectangle the way the trajectory does:
// DST_Y moves past the rectangle in the drawing direction, so a sequence of
// spans can be issued with heights alone.
static void after_rect(rage128_t *r, int32_t h) {
    int32_t y = s14(REG(r, G_DST_Y));
    y += (REG(r, G_DP_CNTL) & DPC_Y_TTB) ? h : -h;
    REG(r, G_DST_Y) = (uint32_t)y & 0x3FFFu;
}

// Under the WebGPU takeover, a solid fill — no source, no destination
// read, a solid brush or none, no colour compare, every bit written — of
// a GPU surface is the GPU's to draw (a Z buffer cleared each frame never
// comes back to VRAM).  False: the engine draws it, fenced.
static bool gpu_fill(rage128_t *r, const op_t *op, int32_t dx0, int32_t dy0, int32_t w, int32_t h, int32_t xdir,
                     int32_t ydir) {
    if (op->uses_s || op->uses_d || (op->write_mask & op->mask) != op->mask)
        return false;
    if (op->uses_p && op->brush_type != BR_SOLID_COLOR && op->brush_type != BR_SOLID_COLOR_B &&
        op->brush_type != BR_NONE)
        return false;
    uint32_t which = CMP_WHICH(op->cmp_cntl);
    if ((which == 1 || which == 2) && CMP_FN_SRC(op->cmp_cntl) != CMP_FALSE)
        return false;
    if ((which == 0 || which == 2) && CMP_FN_DST(op->cmp_cntl) != CMP_FALSE)
        return false;
    uint32_t value = rop3(op->rop, op->uses_p ? op->brush_fg : 0u, 0u, 0u) & op->mask;
    int32_t x0 = xdir > 0 ? dx0 : dx0 - w + 1, y0 = ydir > 0 ? dy0 : dy0 - h + 1;
    int32_t x1 = x0 + w, y1 = y0 + h;
    if (x0 < op->sc_left)
        x0 = op->sc_left;
    if (y0 < op->sc_top)
        y0 = op->sc_top;
    if (x1 > op->sc_right + 1)
        x1 = op->sc_right + 1;
    if (y1 > op->sc_bottom + 1)
        y1 = op->sc_bottom + 1;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x0 >= x1 || y0 >= y1)
        return true; // clipped away: the engine would draw nothing either
    return r128_gpu_fill(r->gpu, op->dst_base, op->dst_stride, op->bpp, x0, y0, x1, y1, value);
}

// Run a rectangle: DST_X/Y, DST_WIDTH/HEIGHT, the data path as loaded.
static void run_rect(rage128_t *r) {
    op_t op;
    op_gather(r, &op);
    int32_t w = (int32_t)(REG(r, G_DST_WIDTH) & 0x1FFFu);
    int32_t h = (int32_t)(REG(r, G_DST_HEIGHT) & 0x1FFFu);
    if (!op.bpp) {
        LOG(1, "Rage 128 2D: destination datatype %u is not drawn", DT_DST(REG(r, G_DP_DATATYPE)));
        return;
    }
    uint32_t dpc = REG(r, G_DP_CNTL);
    int32_t xdir = (dpc & DPC_X_LTR) ? 1 : -1, ydir = (dpc & DPC_Y_TTB) ? 1 : -1;
    int32_t dx0 = s14(REG(r, G_DST_X)), dy0 = s14(REG(r, G_DST_Y));
    int32_t sx0 = s14(REG(r, G_SRC_X)), sy0 = s14(REG(r, G_SRC_Y));
    r->blits++;
    LOG(3, "Rage 128 2D: rect %dx%d at (%d,%d) src (%d,%d) %s rop $%02X src %u/%u brush %u bpp %u dir %+d%+d", w, h,
        dx0, dy0, sx0, sy0, op.src_source == SRC_SOURCE_MEMORY ? "vram" : "host", op.rop, op.src_source, op.src_type,
        op.brush_type, op.bpp * 8u, xdir, ydir);
    if (w <= 0 || h <= 0) {
        after_rect(r, h);
        return;
    }
    if (r->gpu && gpu_fill(r, &op, dx0, dy0, w, h, xdir, ydir)) {
        after_rect(r, h);
        return;
    }
    if (op.uses_s && (op.src_source == SRC_SOURCE_HOST || op.src_source == SRC_SOURCE_HOST_B)) {
        // Host data: the pixels arrive later, through HOST_DATA.
        r->host.active = true;
        r->host.x0 = dx0;
        r->host.y0 = dy0;
        r->host.w = w;
        r->host.h = h;
        r->host.col = 0;
        r->host.row = 0;
        r->host.bits = 0;
        r->host.nbits = 0;
        r->host.byte_aligned = op.src_source == SRC_SOURCE_HOST_B;
        after_rect(r, h);
        return;
    }
    // A VRAM source (or none).  The two surfaces may overlap; the driver
    // picks the directions that make an overlapping copy safe, and the
    // walk follows them.
    for (int32_t j = 0; j < h; j++) {
        int32_t dy = dy0 + ydir * j, sy = sy0 + ydir * j;
        for (int32_t i = 0; i < w; i++) {
            int32_t dx = dx0 + xdir * i, sx = sx0 + xdir * i;
            uint32_t s = 0;
            bool have = true;
            if (op.uses_s) {
                if (sx < 0 || sy < 0) {
                    have = false;
                } else if (op.src_type == SRC_COLOR) {
                    uint64_t at = (uint64_t)op.src_base + (uint64_t)sy * op.src_stride + (uint64_t)sx * op.bpp;
                    if (at + op.bpp > r->vram_size)
                        have = false;
                    else
                        s = vram_get(r, (uint32_t)at, op.bpp);
                } else {
                    bool bit = mono_bit(r, &op, op.src_base + (uint32_t)sy * op.src_stride, (uint32_t)sx);
                    have = mono_expand(&op, bit, &s);
                }
            }
            pixel(r, &op, dx, dy, s, have);
        }
    }
    after_rect(r, h);
}

// One dword of host data into a pending host-data operation.
static void host_feed(rage128_t *r, uint32_t v) {
    if (!r->host.active) {
        LOG(2, "Rage 128 2D: HOST_DATA $%08X with no host-data operation pending — dropped", v);
        return;
    }
    op_t op;
    op_gather(r, &op);
    if (op.src_type == SRC_COLOR) {
        // Colour host data: whole pixels packed little-endian in the dword,
        // or byte-reversed per pixel with HOST_BIG_ENDIAN_EN.
        uint32_t per = op.bpp ? 4u / op.bpp : 0u;
        for (uint32_t k = 0; k < per && r->host.active; k++) {
            uint32_t s = (v >> (8u * op.bpp * k)) & op.mask;
            // HOST_BIG_ENDIAN_EN: the bytes within each 16- or 32-bit pixel
            // are reversed (RRG §7.5, DP_DATATYPE).
            if (op.host_be && op.bpp == 2)
                s = ((s & 0xFFu) << 8) | (s >> 8);
            else if (op.host_be && op.bpp == 4)
                s = __builtin_bswap32(s);
            pixel(r, &op, r->host.x0 + (int32_t)r->host.col, r->host.y0 + (int32_t)r->host.row, s, true);
            if (++r->host.col >= (uint32_t)r->host.w) {
                r->host.col = 0;
                if (++r->host.row >= (uint32_t)r->host.h)
                    r->host.active = false;
                if (r->host.byte_aligned)
                    break; // each line starts in a fresh dword
            }
        }
        return;
    }
    // Mono host data: 32 pixels a dword, byte 0 first, the bit order within a
    // byte per DP_BYTE_PIX_ORDER.
    for (uint32_t b = 0; b < 32 && r->host.active; b++) {
        uint32_t byte = (v >> (8u * (b >> 3))) & 0xFFu;
        uint32_t k = b & 7u;
        bool bit = (byte >> (op.lsb_first ? k : 7u - k)) & 1u;
        uint32_t s = 0;
        bool have = mono_expand(&op, bit, &s);
        pixel(r, &op, r->host.x0 + (int32_t)r->host.col, r->host.y0 + (int32_t)r->host.row, s, have);
        if (++r->host.col >= (uint32_t)r->host.w) {
            r->host.col = 0;
            if (++r->host.row >= (uint32_t)r->host.h)
                r->host.active = false;
            if (r->host.byte_aligned)
                b |= 7u; // the line ends; its next pixel starts the next byte
        }
    }
}

// A Bresenham line (RRG §7.1, the DST_BRES_* registers): DST_BRES_LNTH
// pixels from (DST_X, DST_Y), stepping the major axis every pixel and the
// minor axis whenever the error term is non-negative (zero counts as
// negative with BRES_SIGN), adding DST_BRES_DEC on a minor step and
// DST_BRES_INC otherwise.  The pixel source is the brush.
static void run_line(rage128_t *r, uint32_t len) {
    op_t op;
    op_gather(r, &op);
    if (!op.bpp)
        return;
    uint32_t dpc = REG(r, G_DP_CNTL);
    int32_t xdir = (dpc & DPC_X_LTR) ? 1 : -1, ydir = (dpc & DPC_Y_TTB) ? 1 : -1;
    bool ymajor = (dpc & DPC_Y_MAJOR) != 0;
    int32_t x = s14(REG(r, G_DST_X)), y = s14(REG(r, G_DST_Y));
    // The error terms are 20-bit two's complement.
    int32_t err = (int32_t)(REG(r, G_DST_BRES_ERR) << 12) >> 12;
    int32_t inc = (int32_t)(REG(r, G_DST_BRES_INC) << 12) >> 12;
    int32_t dec = (int32_t)(REG(r, G_DST_BRES_DEC) << 12) >> 12;
    bool zero_negative = (dpc & DPC_BRES_SIGN) != 0;
    uint32_t n = len + ((dpc & DPC_LAST_PEL) ? 1u : 0u);
    r->blits++;
    LOG(3, "Rage 128 2D: line %u px from (%d,%d) %s err %d inc %d dec %d rop $%02X", n, x, y,
        ymajor ? "y-major" : "x-major", err, inc, dec, op.rop);
    for (uint32_t i = 0; i < n; i++) {
        if (i < len || (dpc & DPC_LAST_PEL))
            pixel(r, &op, x, y, op.src_fg, true);
        if (i + 1u >= n)
            break;
        bool minor = zero_negative ? err > 0 : err >= 0;
        if (ymajor) {
            y += ydir;
            if (minor)
                x += xdir;
        } else {
            x += xdir;
            if (minor)
                y += ydir;
        }
        err += minor ? dec : inc;
    }
    REG(r, G_DST_X) = (uint32_t)x & 0x3FFFu;
    REG(r, G_DST_Y) = (uint32_t)y & 0x3FFFu;
    REG(r, G_DST_BRES_ERR) = (uint32_t)err & 0xFFFFFu;
}

// ============================================================
// Register side effects
// ============================================================

// DP_GUI_MASTER_CNTL: one write sets the data path and, unless told to leave
// them alone, re-defaults the surfaces and the scissors (RRG §7.5).
static void gmc_write(rage128_t *r, uint32_t v) {
    if (!(v & GMC_SRC_PITCH_OFFSET_CNTL)) {
        REG(r, G_SRC_OFFSET) = REG(r, G_DEFAULT_OFFSET);
        REG(r, G_SRC_PITCH) = REG(r, G_DEFAULT_PITCH);
    }
    if (!(v & GMC_DST_PITCH_OFFSET_CNTL)) {
        REG(r, G_DST_OFFSET) = REG(r, G_DEFAULT_OFFSET);
        REG(r, G_DST_PITCH) = REG(r, G_DEFAULT_PITCH);
    }
    uint32_t dbr = REG(r, G_DEFAULT_SC_BR);
    if (!(v & GMC_SRC_CLIPPING)) {
        REG(r, G_SRC_SC_RIGHT) = dbr & 0x3FFFu;
        REG(r, G_SRC_SC_BOTTOM) = (dbr >> 16) & 0x3FFFu;
    }
    if (!(v & GMC_DST_CLIPPING)) {
        REG(r, G_SC_LEFT) = 0;
        REG(r, G_SC_TOP) = 0;
        REG(r, G_SC_RIGHT) = dbr & 0x3FFFu;
        REG(r, G_SC_BOTTOM) = (dbr >> 16) & 0x3FFFu;
    }
    // The mapped fields of DP_DATATYPE and DP_MIX.
    uint32_t src = GMC_SRC_DATATYPE(v) == 2u ? SRC_COLOR : GMC_SRC_DATATYPE(v); // "2 = colour" is 3 in DP_DATATYPE
    uint32_t dt = REG(r, G_DP_DATATYPE) & DT_KEEP_ON_GMC_WRITE;
    dt |= GMC_DST_DATATYPE(v) | (GMC_BRUSH_DATATYPE(v) << 8) | (src << 16);
    if (v & GMC_BYTE_PIX_ORDER)
        dt |= DT_BYTE_PIX_ORDER;
    REG(r, G_DP_DATATYPE) = dt;
    REG(r, G_DP_MIX) = (GMC_SRC_SOURCE(v) << 8) | (GMC_ROP3(v) << 16);
    if (v & GMC_CLR_CMP_CNTL_DIS)
        REG(r, G_CLR_CMP_CNTL) &= ~CMP_FN_MASK;
    if (v & GMC_AUX_CLIP_DIS)
        REG(r, G_AUX_SC_CNTL) &= ~0x15u; // every AUXn_SC_ENB
    if (!(v & GMC_3D_FCN_EN)) {
        // A 2D master-control write turns the 3D engine off: SCALE_3D_FCN,
        // Z_EN and STENCIL_EN clear (RRG §7.5, GMC_3D_FCN_EN).
        REG(r, G_SCALE_3D_CNTL) &= ~0xC0u;
        REG(r, G_MISC_3D_STATE) &= ~0x300u;
        REG(r, G_TEX_CNTL_C) &= ~0x9u;
    }
    if (v & GMC_WR_MSK_DIS) {
        REG(r, G_DP_WRITE_MSK) = 0xFFFFFFFFu;
        REG(r, G_CLR_CMP_MSK) = 0xFFFFFFFFu;
    }
    // "This bit is set to '1' by a GUI_MASTER_CNTL write": both directions,
    // and the poly-line flag (RRG §7.5, DP_CNTL).
    REG(r, G_DP_CNTL) |= DPC_X_LTR | DPC_Y_TTB | DPC_POLY_LINE;
}

void r128_2d_write(rage128_t *r, uint32_t off, uint32_t v) {
    switch (off) {
    case G_DP_GUI_MASTER_CNTL:
    case G_DP_GUI_MASTER_CNTL_C:
        gmc_write(r, v);
        return;
    case G_DST_PITCH_OFFSET_C:
    case G_DST_PITCH_OFFSET:
        // Offset in 32-byte units in 20:0, pitch (8 pixels) in 30:21.
        REG(r, G_DST_OFFSET) = (v & 0x1FFFFFu) << 5;
        REG(r, G_DST_PITCH) = (v >> 21) & 0x3FFu;
        return;
    case G_SRC_PITCH_OFFSET:
        REG(r, G_SRC_OFFSET) = (v & 0x1FFFFFu) << 5;
        REG(r, G_SRC_PITCH) = (v >> 21) & 0x3FFu;
        return;
    case G_DST_Y_X:
        REG(r, G_DST_X) = v & 0x3FFFu;
        REG(r, G_DST_Y) = (v >> 16) & 0x3FFFu;
        return;
    case G_DST_X_Y:
        REG(r, G_DST_Y) = v & 0x3FFFu;
        REG(r, G_DST_X) = (v >> 16) & 0x3FFFu;
        return;
    case G_SRC_Y_X:
        REG(r, G_SRC_X) = v & 0x3FFFu;
        REG(r, G_SRC_Y) = (v >> 16) & 0x3FFFu;
        return;
    case G_SRC_X_Y:
        REG(r, G_SRC_Y) = v & 0x3FFFu;
        REG(r, G_SRC_X) = (v >> 16) & 0x3FFFu;
        return;
    case G_SC_TOP_LEFT_C:
    case G_SC_TOP_LEFT:
        REG(r, G_SC_LEFT) = v & 0x3FFFu;
        REG(r, G_SC_TOP) = (v >> 16) & 0x3FFFu;
        return;
    case G_SC_BOTTOM_RIGHT_C:
    case G_SC_BOTTOM_RIGHT:
        REG(r, G_SC_RIGHT) = v & 0x3FFFu;
        REG(r, G_SC_BOTTOM) = (v >> 16) & 0x3FFFu;
        return;
    case G_SRC_SC_BOTTOM_RIGHT:
        REG(r, G_SRC_SC_RIGHT) = v & 0x3FFFu;
        REG(r, G_SRC_SC_BOTTOM) = (v >> 16) & 0x3FFFu;
        return;
    case 0x1A14u: // FOG_TABLE_INDEX
        r->fog_index = (uint8_t)v;
        return;
    case 0x1A18u: // FOG_TABLE_DATA: one 8-bit entry per write, the index advancing
        r->fog_table[r->fog_index++] = (uint8_t)v;
        return;
    case G_PLANE_3D_MASK_C:
        REG(r, G_DP_WRITE_MSK) = v;
        return;
    case G_SCALE_3D_CNTL:
        REG(r, G_MISC_3D_STATE) =
            (REG(r, G_MISC_3D_STATE) & ~(SHARED_3D_FIELDS | 0x300u)) | (v & SHARED_3D_FIELDS) | ((v >> 6 & 3u) << 8);
        return;
    case G_MISC_3D_STATE:
        REG(r, G_SCALE_3D_CNTL) =
            (REG(r, G_SCALE_3D_CNTL) & ~(SHARED_3D_FIELDS | 0xC0u)) | (v & SHARED_3D_FIELDS) | ((v >> 8 & 3u) << 6);
        return;
    case G_DP_CNTL_XDIR_YDIR: {
        // The same three bits as DP_CNTL, at other positions.
        uint32_t c = REG(r, G_DP_CNTL) & ~(DPC_X_LTR | DPC_Y_TTB | DPC_Y_MAJOR);
        if (v & 0x80000000u)
            c |= DPC_X_LTR;
        if (v & 0x00008000u)
            c |= DPC_Y_TTB;
        if (v & 0x00000004u)
            c |= DPC_Y_MAJOR;
        REG(r, G_DP_CNTL) = c;
        return;
    }
    // --- initiators ---------------------------------------------------------
    case G_DST_HEIGHT:
        run_rect(r);
        return;
    case G_DST_HEIGHT_WIDTH:
    case G_DST_HEIGHT_WIDTH_BW:
        REG(r, G_DST_WIDTH) = v & 0x3FFFu;
        REG(r, G_DST_HEIGHT) = (v >> 16) & 0x3FFFu;
        run_rect(r);
        return;
    case G_DST_WIDTH_HEIGHT:
        REG(r, G_DST_HEIGHT) = v & 0x3FFFu;
        REG(r, G_DST_WIDTH) = (v >> 16) & 0x3FFFu;
        run_rect(r);
        return;
    case G_DST_HEIGHT_WIDTH_8:
        REG(r, G_DST_WIDTH) = (v >> 16) & 0xFFu;
        REG(r, G_DST_HEIGHT) = (v >> 24) & 0xFFu;
        run_rect(r);
        return;
    case G_DST_HEIGHT_Y:
        REG(r, G_DST_Y) = v & 0x3FFFu;
        REG(r, G_DST_HEIGHT) = (v >> 16) & 0x3FFFu;
        run_rect(r);
        return;
    case G_DST_WIDTH_X:
    case G_DST_WIDTH_X_INCY:
        REG(r, G_DST_X) = v & 0x3FFFu;
        REG(r, G_DST_WIDTH) = (v >> 16) & 0x3FFFu;
        run_rect(r);
        return;
    case G_DST_BRES_LNTH:
        run_line(r, v & 0x3FFFu);
        return;
    default:
        break;
    }
    if (off >= G_HOST_DATA0 && off <= G_HOST_DATA_LAST) {
        host_feed(r, v);
        if (off == G_HOST_DATA_LAST && r->host.active) {
            LOG(2, "Rage 128 2D: HOST_DATA_LAST with %u x %u pixels still owed — the operation ends short",
                r->host.w - r->host.col, r->host.h - r->host.row);
            r->host.active = false;
        }
    }
}

bool r128_2d_brush_covers(rage128_t *r, int32_t x, int32_t y) {
    op_t op;
    op_gather(r, &op);
    uint32_t v;
    return brush_at(r, &op, x, y, &v);
}

uint32_t r128_2d_rop3(uint32_t rop, uint32_t p, uint32_t s, uint32_t d) {
    return rop3(rop, p, s, d);
}

void r128_2d_reset(rage128_t *r) {
    // Open scissors and an all-ones write mask, so a driver that never
    // programs them is not clipped to nothing or masked to nothing.
    REG(r, G_SC_RIGHT) = 0x1FFFu;
    REG(r, G_SC_BOTTOM) = 0x1FFFu;
    REG(r, G_DEFAULT_SC_BR) = (0x1FFFu << 16) | 0x1FFFu;
    REG(r, G_SRC_SC_RIGHT) = 0x1FFFu;
    REG(r, G_SRC_SC_BOTTOM) = 0x1FFFu;
    REG(r, G_DP_WRITE_MSK) = 0xFFFFFFFFu;
    REG(r, G_CLR_CMP_MSK) = 0xFFFFFFFFu;
    REG(r, G_DP_CNTL) = DPC_X_LTR | DPC_Y_TTB;
    memset(&r->host, 0, sizeof(r->host));
    r->blits = 0;
}
