// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rage128_cce.c
// The ATI Rage 128 GL's Concurrent Command Engine: the PM4 registers, the
// microcode RAM, the PIO FIFO, the bus-mastered ring and indirect buffer,
// and the packet decoder that turns command packets into register writes.
//
// On silicon the CCE is a microcoded processor: the driver uploads 256
// qwords of microcode, and the microengine interprets packets by producing
// register writes into the command FIFO.  The model does not interpret the
// microcode — it stores what the guest uploads, recognises ATI's published
// image by its CRC (and says so in the log), and executes packets natively
// with the effect the SDK's Appendix F gives them:
//   * type 0: COUNT+1 dwords into consecutive registers (or one register);
//   * type 1: two dwords into two registers;
//   * type 2: a one-dword filler;
//   * type 3: an operation.  The 2D ones become exactly the register writes
//     a PIO driver would make — DP_GUI_MASTER_CNTL and the SETTINGS block,
//     then the trajectory and an initiator — into the same 2D engine
//     (rage128_2d.c).  The 3D ones decode their vertices — inline, or
//     fetched by the vertex walker from a list in AGP space, in order or by
//     index — for the 3D engine (rage128_raster.c).
//
// Where packets come from (PM4_BUFFER_CNTL.PM4_BUFFER_MODE, bits 31:28):
//   * PIO modes (1, 3, 5, 7, 15): dwords written to PM4_FIFO_DATA_EVEN/ODD;
//   * bus-master modes (2, 4, 6, 8): a ring of 2^(n+1) dwords at
//     PM4_BUFFER_OFFSET, fetched from PM4_BUFFER_DL_RPTR up to
//     PM4_BUFFER_DL_WPTR when the driver moves WPTR, with the new RPTR
//     written back to PM4_BUFFER_DL_RPTR_ADDR (unless the NOUPDATE bit asks
//     the driver to read the register instead);
//   * the indirect buffer: PM4_IW_INDSIZE dwords at PM4_IW_INDOFF, run when
//     INDSIZE is written — by PIO or by a type-0 packet from the ring.
//
// Card addresses.  The ring and indirect buffer live in the card's 64 MB
// address space: the low 32 MB is the frame buffer, the high 32 MB (bit 25)
// is "AGP space".  On this PCI card that window reaches host memory through
// the PCI GART — PCI_GART_PAGE points at a table of 8192 page addresses,
// one per 4 KB page — or, with the GART disabled, linearly from AGP_BASE.
// Every host access goes through pci_dma_read/write, so it is gated twice:
// on the PCI command register's BUS_MASTER_EN and on BUS_CNTL.BUS_MASTER_DIS.
//
// Pacing.  The fetch runs to completion inside the WPTR write: a driver
// that reads RPTR straight after finds the ring drained, and PM4_STAT and
// GUI_STAT read idle with the FIFO empty.  Every driver polls for idle
// before it relies on completion, so finishing early is indistinguishable
// from finishing fast.
//
// Truth: ATI, *RAGE 128 Software Development Guide* (SDK-G04000 Rev 0.01,
// 1999), chapter 5 and appendix F; ATI, *RAGE 128 VR / RAGE 128 GL Register
// Reference Guide* (RRG-G04100-C Rev 0.02, 1999), PCI_GART_PAGE, BUS_CNTL;
// ATI, *RAGE 128 Register Reference Supplement: CCE and 3D Packets*.

#include "crc32.h"
#include "log.h"
#include "rage128_priv.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("video");

// ============================================================
// Registers
// ============================================================

#define C_PM4_BUFFER_OFFSET       0x0700u
#define C_PM4_BUFFER_CNTL         0x0704u
#define C_PM4_BUFFER_WM_CNTL      0x0708u
#define C_PM4_BUFFER_DL_RPTR_ADDR 0x070Cu
#define C_PM4_BUFFER_DL_RPTR      0x0710u
#define C_PM4_BUFFER_DL_WPTR      0x0714u
#define C_PM4_IW_INDOFF           0x0738u
#define C_PM4_IW_INDSIZE          0x073Cu
#define C_PM4_STAT                0x07B8u
#define C_PM4_MICROCODE_ADDR      0x07D4u
#define C_PM4_MICROCODE_RADDR     0x07D8u
#define C_PM4_MICROCODE_DATAH     0x07DCu
#define C_PM4_MICROCODE_DATAL     0x07E0u
#define C_PM4_BUFFER_ADDR         0x07F0u
#define C_PM4_MICRO_CNTL          0x07FCu
#define C_PM4_FIFO_DATA_EVEN      0x1000u
#define C_PM4_FIFO_DATA_ODD       0x1004u

// PM4_BUFFER_CNTL.
#define PM4_MODE(v)       (((v) >> 28) & 0xFu)
#define PM4_SIZE_L2QW(v)  ((v) & 0x1Fu)
#define PM4_NOUPDATE      0x08000000u
#define PM4_DL_DONE       0x80000000u // in WPTR: "the stream is ending"
#define PM4_MICRO_FREERUN 0x40000000u

// The 2D registers the packets write (RRG chapter 7).
#define G_SRC_PITCH_OFFSET    0x1428u
#define G_DST_PITCH_OFFSET    0x142Cu
#define G_SRC_Y_X             0x1434u
#define G_DST_Y_X             0x1438u
#define G_DST_HEIGHT_WIDTH    0x143Cu
#define G_DP_GUI_MASTER_CNTL  0x146Cu
#define G_BRUSH_Y_X           0x1474u
#define G_DP_BRUSH_BKGD_CLR   0x1478u
#define G_DP_BRUSH_FRGD_CLR   0x147Cu
#define G_BRUSH_DATA0         0x1480u
#define G_SRC_X_Y             0x1590u
#define G_DST_X_Y             0x1594u
#define G_DST_WIDTH_HEIGHT    0x1598u
#define G_CLR_CMP_CNTL        0x15C0u
#define G_CLR_CMP_CLR_SRC     0x15C4u
#define G_CLR_CMP_CLR_DST     0x15C8u
#define G_DP_SRC_FRGD_CLR     0x15D8u
#define G_DP_SRC_BKGD_CLR     0x15DCu
#define G_DST_BRES_ERR        0x1628u
#define G_DST_BRES_INC        0x162Cu
#define G_DST_BRES_DEC        0x1630u
#define G_DST_BRES_LNTH       0x1634u
#define G_DP_CNTL             0x16C0u
#define G_SC_TOP_LEFT         0x16ECu
#define G_SC_BOTTOM_RIGHT     0x16F0u
#define G_SRC_SC_BOTTOM_RIGHT 0x16F4u
#define G_HOST_DATA0          0x17C0u
#define G_HOST_DATA_LAST      0x17E0u

#define DPC_X_LTR    0x001u
#define DPC_Y_TTB    0x002u
#define DPC_Y_MAJOR  0x004u
#define DPC_LAST_PEL 0x020u

// Type-3 opcodes (SDK appendix F, the packet summary).
#define OP_NOP              0x10u
#define OP_NEXTCHAR         0x19u
#define OP_PLY_NEXTSCAN     0x1Du
#define OP_SET_SCISSORS     0x1Eu
#define OP_SET_MODE_24BPP   0x1Fu
#define OP_3D_GEN_INDX_PRIM 0x23u
#define OP_3D_GEN_PRIM      0x25u
#define OP_LOAD_PALETTE     0x2Cu
#define OP_PURGE            0x2Du
#define OP_NEXT_VTX_BUNDLE  0x2Eu
#define OP_PAINT            0x91u
// $92 is not in the SDK's table, but ATI's Mac OS 9 driver sends it for
// ScrollRect-style single blits; its body, as captured from that driver, is
// BITBLT_MULTI's: SETTINGS, then [SRC_X | SRC_Y] [DST_X | DST_Y] [W | H].
// (The Radeon's command processor documents the same number as BITBLT.)
#define OP_BITBLT        0x92u
#define OP_SMALL_TEXT    0x93u
#define OP_HOSTDATA_BLT  0x94u
#define OP_POLYLINE      0x95u
#define OP_SCALE         0x96u
#define OP_TRANS_SCALE   0x97u
#define OP_POLYSCANLINES 0x98u
#define OP_PAINT_MULTI   0x9Au
#define OP_BITBLT_MULTI  0x9Bu
#define OP_TRANS_BITBLT  0x9Cu

// ATI's Rage 128 microcode — the 2048-byte image the Linux r128 driver and
// the linux-firmware r128_cce.bin carry, (DATAH, DATAL) pairs serialised
// big-endian — has this CRC-32.
#define R128_UCODE_CRC 0xFB2E59A2u

// The AGP window: card addresses with bit 25 set.
#define CARD_AGP_BIT  0x02000000u
#define CARD_AGP_MASK 0x01FFFFFFu
#define GART_PAGE     0x1000u

// Fetches move at most this many dwords at a time (the stack is small on
// the web build).
#define FETCH_RUN 256u

#define REG(r, off) ((r)->reg[(off) / 4u])

// ============================================================
// Card-address fetches
// ============================================================

static bool bm_enabled(rage128_t *r) {
    return !(REG(r, R_BUS_CNTL) & BUS_MASTER_DIS);
}

// Translate an AGP-window offset to a host (PCI) address: through the GART
// table, or linearly from AGP_BASE with the GART disabled.
static bool agp_to_host(rage128_t *r, uint32_t agp, uint32_t *host) {
    uint32_t gart = REG(r, R_PCI_GART_PAGE);
    if (gart & PCI_GART_DIS) {
        *host = (REG(r, R_AGP_BASE) & 0xFFC00000u) + agp;
        return true;
    }
    uint8_t e[4];
    uint32_t table = gart & 0xFFFFF000u;
    if (!pci_dma_read(r->dev, table + (agp / GART_PAGE) * 4u, e, 4))
        return false;
    uint32_t page = (uint32_t)e[0] | ((uint32_t)e[1] << 8) | ((uint32_t)e[2] << 16) | ((uint32_t)e[3] << 24);
    *host = (page & 0xFFFFF000u) | (agp & (GART_PAGE - 1u));
    LOG(5, "Rage 128 CCE: GART: AGP $%07X -> host $%08X (entry %u)", agp, *host, agp / GART_PAGE);
    return true;
}

// Fetch `n` little-endian dwords at card address `addr`.  False when the
// fetch cannot happen (bus mastering off, a frame-buffer address past VRAM).
bool r128_card_read(rage128_t *r, uint32_t addr, uint32_t *out, uint32_t n) {
    while (n) {
        if (!(addr & CARD_AGP_BIT)) {
            // The frame buffer: the engine reads its own memory.
            uint32_t a = addr & CARD_AGP_MASK;
            if (a + 4u > r->vram_size)
                return false;
            r128_vram_access(r, a, 4, false);
            out[0] = (uint32_t)r->vram[a] | ((uint32_t)r->vram[a + 1] << 8) | ((uint32_t)r->vram[a + 2] << 16) |
                     ((uint32_t)r->vram[a + 3] << 24);
            out++, n--, addr += 4u;
            continue;
        }
        if (!bm_enabled(r))
            return false;
        uint32_t agp = addr & CARD_AGP_MASK;
        uint32_t chunk = (GART_PAGE - (agp & (GART_PAGE - 1u))) / 4u;
        if (chunk > n)
            chunk = n;
        if (chunk > FETCH_RUN)
            chunk = FETCH_RUN;
        uint32_t host;
        uint8_t bytes[FETCH_RUN * 4u];
        if (!agp_to_host(r, agp, &host) || !pci_dma_read(r->dev, host, bytes, chunk * 4u))
            return false;
        for (uint32_t i = 0; i < chunk; i++)
            out[i] = (uint32_t)bytes[4 * i] | ((uint32_t)bytes[4 * i + 1] << 8) | ((uint32_t)bytes[4 * i + 2] << 16) |
                     ((uint32_t)bytes[4 * i + 3] << 24);
        out += chunk, n -= chunk, addr += chunk * 4u;
    }
    return true;
}

// ============================================================
// Packet execution
// ============================================================

static void wr(rage128_t *r, uint32_t off, uint32_t v) {
    r128_reg_store(r, off, v);
}

static void tell_once(rage128_t *r, uint32_t op, const char *what) {
    uint8_t bit = (uint8_t)(1u << (op & 7u));
    if (r->cce.told[op >> 3] & bit)
        return;
    r->cce.told[op >> 3] |= bit;
    LOG(1, "Rage 128 CCE: type-3 opcode $%02X (%s) is not executed — skipped", op, what);
}

// Bytes per pixel of a DST_TYPE, for the colour-brush sizes (SDK appendix F, the pixel-size table).
static uint32_t dst_bytes(uint32_t type) {
    switch (type) {
    case 2:
    case 7:
        return 1;
    case 3:
    case 4:
        return 2;
    case 5:
        return 3;
    case 6:
        return 4;
    default:
        return 0;
    }
}

// The SETTINGS block of a 2D packet (SDK §F.7): GUI_CONTROL goes to
// DP_GUI_MASTER_CNTL, whose low bits say which SETUP_BODY fields follow.
// Returns the dwords consumed, or 0 if the packet is too short for them.
static uint32_t settings(rage128_t *r, const uint32_t *b, uint32_t n) {
    uint32_t i = 0;
#define TAKE(off)                                                                                                      \
    do {                                                                                                               \
        if (i >= n)                                                                                                    \
            return 0;                                                                                                  \
        wr(r, (off), b[i++]);                                                                                          \
    } while (0)
    if (n < 1)
        return 0;
    uint32_t gc = b[i++];
    wr(r, G_DP_GUI_MASTER_CNTL, gc);
    if (gc & 0x1u)
        TAKE(G_SRC_PITCH_OFFSET);
    if (gc & 0x2u)
        TAKE(G_DST_PITCH_OFFSET);
    if (gc & 0x4u)
        TAKE(G_SRC_SC_BOTTOM_RIGHT);
    if (gc & 0x8u) {
        TAKE(G_SC_TOP_LEFT);
        TAKE(G_SC_BOTTOM_RIGHT);
    }
    // BRUSH_PACKET (SDK appendix F, the brush-packet table): background first when the type has one.
    uint32_t bt = (gc >> 4) & 0xFu, npat = 0;
    bool has_bg = false, has_fg = true;
    switch (bt) {
    case 0:
        has_bg = true, npat = 2;
        break;
    case 1:
        npat = 2;
        break;
    case 2:
    case 4:
    case 6:
        has_bg = true, npat = 1;
        break;
    case 3:
    case 5:
    case 7:
        npat = 1;
        break;
    case 8:
        has_bg = true, npat = 32;
        break;
    case 9:
        npat = 32;
        break;
    case 10: {
        uint32_t dt = (gc >> 8) & 0xFu;
        npat = 16u * (dt == 5 ? 4u : dst_bytes(dt)); // 24 bpp still takes 4 bytes a pixel here
        has_fg = false;
        break;
    }
    case 11:
    case 12:
        npat = 2u * dst_bytes((gc >> 8) & 0xFu);
        has_fg = false;
        break;
    case 13:
        break;
    default: // 14 reserved, 15 no brush
        has_fg = false;
        break;
    }
    if (has_bg)
        TAKE(G_DP_BRUSH_BKGD_CLR);
    if (has_fg)
        TAKE(G_DP_BRUSH_FRGD_CLR);
    for (uint32_t k = 0; k < npat; k++)
        TAKE(G_BRUSH_DATA0 + 4u * k);
    if (gc & 0x80000000u)
        TAKE(G_BRUSH_Y_X);
#undef TAKE
    return i;
}

static int32_t lo16(uint32_t v) {
    return (int16_t)(v & 0xFFFFu);
}
static int32_t hi16(uint32_t v) {
    return (int16_t)(v >> 16);
}
static uint32_t pack(int32_t hi, int32_t lo) {
    return (((uint32_t)hi & 0x3FFFu) << 16) | ((uint32_t)lo & 0x3FFFu);
}

static void set_dirs(rage128_t *r, uint32_t want) {
    uint32_t c = REG(r, G_DP_CNTL) & ~(DPC_X_LTR | DPC_Y_TTB | DPC_Y_MAJOR);
    wr(r, G_DP_CNTL, c | want);
}

// A horizontal span list: [HEIGHT | TOP] then [END | START] pairs, END
// exclusive (POLYSCANLINES, PLY_NEXTSCAN).
static uint32_t scans(rage128_t *r, const uint32_t *b, uint32_t n, uint32_t segs) {
    if (n < 1u + segs)
        return 0;
    int32_t top = lo16(b[0]), height = hi16(b[0]);
    for (uint32_t k = 0; k < segs; k++) {
        int32_t start = lo16(b[1 + k]), end = hi16(b[1 + k]);
        if (end <= start || height <= 0)
            continue;
        wr(r, G_DST_Y_X, pack(top, start));
        wr(r, G_DST_HEIGHT_WIDTH, pack(height, end - start));
    }
    return 1u + segs;
}

// A host-data rectangle: position, size, then `count` raster dwords.
static void host_rect(rage128_t *r, uint32_t yx, uint32_t hw, const uint32_t *raster, uint32_t count) {
    wr(r, G_DST_Y_X, yx);
    wr(r, G_DST_HEIGHT_WIDTH, hw);
    for (uint32_t k = 0; k < count; k++)
        wr(r, k + 1u == count ? G_HOST_DATA_LAST : G_HOST_DATA0, raster[k]);
}

// One Bresenham segment from (x0,y0) toward (x1,y1), the end pixel drawn
// only if `last`.
static void segment(rage128_t *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, bool last) {
    int32_t dx = x1 - x0, dy = y1 - y0;
    int32_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    bool ymajor = ady > adx;
    int32_t major = ymajor ? ady : adx, minor = ymajor ? adx : ady;
    uint32_t dirs = (dx >= 0 ? DPC_X_LTR : 0u) | (dy >= 0 ? DPC_Y_TTB : 0u) | (ymajor ? DPC_Y_MAJOR : 0u);
    uint32_t c = REG(r, G_DP_CNTL) & ~(DPC_X_LTR | DPC_Y_TTB | DPC_Y_MAJOR | DPC_LAST_PEL);
    wr(r, G_DP_CNTL, c | dirs | (last ? DPC_LAST_PEL : 0u));
    wr(r, G_DST_Y_X, pack(y0, x0));
    wr(r, G_DST_BRES_ERR, (uint32_t)(2 * minor - major) & 0xFFFFFu);
    wr(r, G_DST_BRES_INC, (uint32_t)(2 * minor) & 0xFFFFFu);
    wr(r, G_DST_BRES_DEC, (uint32_t)(2 * (minor - major)) & 0xFFFFFu);
    wr(r, G_DST_BRES_LNTH, (uint32_t)major);
}

// ============================================================
// 3D packets: vertices to the 3D engine (rage128_raster.c)
// ============================================================

#define VC_PRIM(c)   ((c) & 0xFu)
#define VC_WALK(c)   (((c) >> 4) & 3u)
#define VC_NUM(c)    ((c) >> 16)
#define WALK_INDEXED 1u
#define WALK_LIST    2u
#define WALK_RING    3u
#define MAX_VERTICES 0x10000u

// The vertices of a 3D_RNDR_GEN_PRIM, inline in the packet.
static void draw_inline(rage128_t *r, uint32_t fmt, uint32_t cntl, const uint32_t *d, uint32_t n) {
    uint32_t vs = r128_3d_vertex_dwords(fmt), count = VC_NUM(cntl);
    if (VC_WALK(cntl) != WALK_RING)
        LOG(2, "Rage 128 CCE: 3D_RNDR_GEN_PRIM with walk %u — its vertices read inline", VC_WALK(cntl));
    if (count > n / vs) {
        LOG(2, "Rage 128 CCE: 3D_RNDR_GEN_PRIM names %u vertices, the packet holds %u", count, n / vs);
        count = n / vs;
    }
    if (!count)
        return;
    r128_vertex_t *v = malloc(count * sizeof(*v));
    if (!v)
        return;
    for (uint32_t i = 0; i < count; i++)
        r128_3d_vertex_decode(fmt, d + i * vs, &v[i]);
    LOG(4, "Rage 128 CCE: 3D prim %u, %u inline vertices of %u dwords", VC_PRIM(cntl), count, vs);
    r128_3d_draw(r, VC_PRIM(cntl), v, count);
    free(v);
}

// The vertex walker: vertices from the vertex list at VLOFF in AGP space
// ("with respect to the physical address of the AGP space", SDK appendix
// F), in order (list walk) or by 16-bit indices, two to a dword, the first
// in the low half (indexed walk).
static void draw_walked(rage128_t *r, uint32_t vloff, uint32_t vsize, uint32_t fmt, uint32_t cntl, const uint32_t *idx,
                        uint32_t nidx) {
    uint32_t vs = r128_3d_vertex_dwords(fmt), count = VC_NUM(cntl), walk = VC_WALK(cntl);
    uint32_t base = CARD_AGP_BIT | (vloff & CARD_AGP_MASK);
    if (walk == WALK_RING) {
        draw_inline(r, fmt, cntl, idx, nidx);
        return;
    }
    if (walk == WALK_INDEXED && count > nidx * 2u)
        count = nidx * 2u;
    if (count > MAX_VERTICES)
        count = MAX_VERTICES;
    if (!count)
        return;
    r128_vertex_t *v = malloc(count * sizeof(*v));
    uint32_t *raw = malloc(vs * sizeof(uint32_t));
    if (!v || !raw) {
        free(v);
        free(raw);
        return;
    }
    uint32_t got = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t k = i;
        if (walk == WALK_INDEXED)
            k = (idx[i / 2u] >> ((i & 1u) ? 16 : 0)) & 0xFFFFu;
        if (k >= vsize && vsize) {
            LOG(2, "Rage 128 CCE: vertex %u is past the list's %u", k, vsize);
            continue;
        }
        if (!r128_card_read(r, base + k * vs * 4u, raw, vs)) {
            LOG(1, "Rage 128 CCE: vertex fetch at card address $%08X failed", base + k * vs * 4u);
            break;
        }
        r128_3d_vertex_decode(fmt, raw, &v[got++]);
    }
    LOG(4, "Rage 128 CCE: 3D prim %u, %u %s vertices of %u dwords at $%08X", VC_PRIM(cntl), got,
        walk == WALK_INDEXED ? "indexed" : "listed", vs, base);
    r128_3d_draw(r, VC_PRIM(cntl), v, got);
    free(raw);
    free(v);
}

static void exec_type3(rage128_t *r, uint32_t op, const uint32_t *b, uint32_t n) {
    uint32_t i = 0;
    switch (op) {
    case OP_NOP:
    case OP_PURGE: // the pixel cache is never dirty
        return;
    case OP_SET_SCISSORS:
        if (n >= 2) {
            wr(r, G_SC_TOP_LEFT, b[0]);
            wr(r, G_SC_BOTTOM_RIGHT, b[1]);
        }
        return;
    case OP_SET_MODE_24BPP:
        // A flag inside the microengine for its own 24 bpp arithmetic; the
        // native packets never need it.
        LOG(3, "Rage 128 CCE: SET_MODE_24BPP %u", n ? b[0] : 0u);
        return;
    case OP_PAINT:
        // [TOP | LEFT] [BOTTOM | RIGHT] pairs, the bottom-right exclusive.
        if (!(i = settings(r, b, n)))
            break;
        for (; i + 2u <= n; i += 2u) {
            int32_t l = lo16(b[i]), t = hi16(b[i]), rt = lo16(b[i + 1]), bt = hi16(b[i + 1]);
            if (rt <= l || bt <= t)
                continue;
            wr(r, G_DST_Y_X, pack(t, l));
            wr(r, G_DST_HEIGHT_WIDTH, pack(bt - t, rt - l));
        }
        return;
    case OP_PAINT_MULTI:
        // [X | Y] [W | H] pairs: DST_X_Y and DST_WIDTH_HEIGHT verbatim.
        if (!(i = settings(r, b, n)))
            break;
        for (; i + 2u <= n; i += 2u) {
            wr(r, G_DST_X_Y, b[i]);
            wr(r, G_DST_WIDTH_HEIGHT, b[i + 1]);
        }
        return;
    case OP_BITBLT:
    case OP_BITBLT_MULTI:
        // [SRC_X | SRC_Y] [DST_X | DST_Y] [W | H] triples.  The microengine
        // picks the walk directions that make an overlapping copy safe.
        if (!(i = settings(r, b, n)))
            break;
        for (; i + 3u <= n; i += 3u) {
            int32_t sx = hi16(b[i]), sy = lo16(b[i]), dx = hi16(b[i + 1]), dy = lo16(b[i + 1]);
            int32_t w = (int32_t)((b[i + 2] >> 16) & 0x3FFFu), h = (int32_t)(b[i + 2] & 0x3FFFu);
            if (w <= 0 || h <= 0)
                continue;
            bool rtl = sx < dx, btt = sy < dy;
            set_dirs(r, (rtl ? 0u : DPC_X_LTR) | (btt ? 0u : DPC_Y_TTB));
            if (rtl)
                sx += w - 1, dx += w - 1;
            if (btt)
                sy += h - 1, dy += h - 1;
            wr(r, G_SRC_X_Y, pack(sx, sy));
            wr(r, G_DST_X_Y, pack(dx, dy));
            wr(r, G_DST_WIDTH_HEIGHT, pack(w, h));
        }
        set_dirs(r, DPC_X_LTR | DPC_Y_TTB);
        return;
    case OP_TRANS_BITBLT:
        if (!(i = settings(r, b, n)) || i + 6u > n)
            break;
        wr(r, G_CLR_CMP_CNTL, b[i]);
        wr(r, G_CLR_CMP_CLR_SRC, b[i + 1]);
        wr(r, G_CLR_CMP_CLR_DST, b[i + 2]);
        wr(r, G_SRC_Y_X, b[i + 3]);
        wr(r, G_DST_Y_X, b[i + 4]);
        wr(r, G_DST_HEIGHT_WIDTH, b[i + 5]);
        return;
    case OP_POLYLINE: {
        // Points joined in order; each segment leaves its end pixel to the
        // next, and the last one draws it only if DP_CNTL.LAST_PEL asked.
        if (!(i = settings(r, b, n)))
            break;
        bool last_pel = (REG(r, G_DP_CNTL) & DPC_LAST_PEL) != 0;
        for (; i + 2u <= n; i++)
            segment(r, lo16(b[i]), hi16(b[i]), lo16(b[i + 1]), hi16(b[i + 1]), last_pel && i + 2u == n);
        return;
    }
    case OP_POLYSCANLINES: {
        if (!(i = settings(r, b, n)) || i >= n)
            break;
        uint32_t count = b[i++];
        for (uint32_t k = 0; k < count; k++) {
            if (i >= n)
                break;
            uint32_t segs = b[i++];
            uint32_t used = scans(r, b + i, n - i, segs);
            if (!used)
                break;
            i += used;
        }
        return;
    }
    case OP_PLY_NEXTSCAN:
        if (n >= 1)
            scans(r, b, n, n - 1u);
        return;
    case OP_HOSTDATA_BLT:
        // The source colours, then [BASE_Y | BASE_X] [H | W] [NUMBER] and
        // NUMBER raster dwords per bitmap.
        if (!(i = settings(r, b, n)) || i + 2u > n)
            break;
        wr(r, G_DP_SRC_FRGD_CLR, b[i]);
        wr(r, G_DP_SRC_BKGD_CLR, b[i + 1]);
        i += 2u;
        while (i + 3u <= n) {
            uint32_t count = b[i + 2];
            if (count > n - i - 3u) {
                LOG(2, "Rage 128 CCE: HOSTDATA_BLT bitmap of %u dwords overruns its packet", count);
                count = n - i - 3u;
            }
            host_rect(r, b[i], b[i + 1], b + i + 3, count);
            i += 3u + count;
        }
        return;
    case OP_NEXTCHAR:
        // One mono bitmap with the current settings: [Y | X] [H | W] raster.
        if (n >= 2)
            host_rect(r, b[0], b[1], b + 2, n - 2u);
        return;
    case OP_3D_GEN_PRIM:
        // VC_FORMAT, VC_CNTL, then the vertices inline (the ring walk).
        if (n >= 2)
            draw_inline(r, b[0], b[1], b + 2, n - 2u);
        return;
    case OP_3D_GEN_INDX_PRIM:
        // VLOFF, VSIZE, VC_FORMAT, VC_CNTL, then (indexed walk) the indices.
        if (n < 4)
            break;
        r->cce.vc_vloff = b[0];
        r->cce.vc_format = b[2];
        r->cce.vc_cntl = b[3];
        draw_walked(r, b[0], b[1], b[2], b[3], b + 4, n - 4u);
        return;
    case OP_NEXT_VTX_BUNDLE:
        // More indices for the last indexed primitive.
        draw_walked(r, r->cce.vc_vloff, 0xFFFFu, r->cce.vc_format, (r->cce.vc_cntl & 0xFFFFu) | ((n * 2u) << 16), b, n);
        return;
    case OP_SMALL_TEXT:
        tell_once(r, op, "SMALL_TEXT");
        return;
    case OP_SCALE:
    case OP_TRANS_SCALE:
        tell_once(r, op, "scaler");
        return;
    case OP_LOAD_PALETTE:
        tell_once(r, op, "LOAD_PALETTE (scaler palette)");
        return;
    default:
        tell_once(r, op, "unknown");
        LOG(2, "Rage 128 CCE: opcode $%02X body (%u dwords): %08X %08X %08X %08X %08X %08X %08X %08X", op, n,
            n > 0 ? b[0] : 0u, n > 1 ? b[1] : 0u, n > 2 ? b[2] : 0u, n > 3 ? b[3] : 0u, n > 4 ? b[4] : 0u,
            n > 5 ? b[5] : 0u, n > 6 ? b[6] : 0u, n > 7 ? b[7] : 0u);
        return;
    }
    LOG(2, "Rage 128 CCE: type-3 opcode $%02X: packet of %u dwords is too short for its SETTINGS", op, n);
}

// A complete packet: header in buf[0], body after it.
static void exec_packet(rage128_t *r, const uint32_t *p, uint32_t n) {
    uint32_t h = p[0];
    r->cce.packets++;
    switch (h >> 30) {
    case 0: {
        uint32_t base = (h & 0x7FFu) * 4u;
        bool one = (h & 0x8000u) != 0;
        LOG(4, "Rage 128 CCE: type 0: %u dword(s) to $%04X%s", n - 1u, base, one ? " (one register)" : "");
        for (uint32_t k = 1; k < n; k++) {
            uint32_t off = one ? base : base + 4u * (k - 1u);
            if (off >= 0x1000u && off < 0x1400u) {
                LOG(2, "Rage 128 CCE: a packet writes the CCE FIFO ($%04X) — dropped", off);
                continue;
            }
            wr(r, off & (R128_REG_APER_SIZE - 4u), p[k]);
        }
        return;
    }
    case 1:
        LOG(4, "Rage 128 CCE: type 1: $%04X, $%04X", (h & 0x7FFu) * 4u, ((h >> 11) & 0x7FFu) * 4u);
        wr(r, (h & 0x7FFu) * 4u, p[1]);
        wr(r, ((h >> 11) & 0x7FFu) * 4u, p[2]);
        return;
    case 3: {
        uint32_t op = (h >> 8) & 0xFFu;
        LOG(4, "Rage 128 CCE: type 3: opcode $%02X, %u dwords", op, n - 1u);
        exec_type3(r, op, p + 1, n - 1u);
        return;
    }
    default:
        return;
    }
}

// One dword into a stream: a header sizes the packet, the last dword runs it.
static void feed(rage128_t *r, r128_cce_stream_t *s, uint32_t v) {
    if (!s->need) {
        switch (v >> 30) {
        case 0:
        case 3:
            s->need = ((v >> 16) & 0x3FFFu) + 2u;
            break;
        case 1:
            s->need = 3u;
            break;
        default: // type 2: the header is the whole packet
            return;
        }
        s->len = 0;
    }
    s->buf[s->len++] = v;
    if (s->len == s->need) {
        s->need = 0;
        exec_packet(r, s->buf, s->len);
    }
}

// ============================================================
// The ring and the indirect buffer
// ============================================================

static bool mode_is_bm(uint32_t mode) {
    return mode == 2 || mode == 4 || mode == 6 || mode == 8;
}
static bool mode_is_pio(uint32_t mode) {
    return mode == 1 || mode == 3 || mode == 5 || mode == 7 || mode == 15;
}

// Free entries in the CCE FIFO when it is empty: its share of the 192.
static uint32_t fifo_size(uint32_t mode) {
    switch (mode) {
    case 3:
    case 4:
        return 128;
    case 5:
    case 6:
    case 7:
    case 8:
    case 15:
        return 64;
    default:
        return 192;
    }
}

// Fetch and execute the ring from RPTR to WPTR, then report RPTR.
static void ring_run(rage128_t *r) {
    uint32_t cntl = REG(r, C_PM4_BUFFER_CNTL);
    if (r->cce.in_ring || !mode_is_bm(PM4_MODE(cntl)) || !(REG(r, C_PM4_MICRO_CNTL) & PM4_MICRO_FREERUN))
        return;
    uint32_t size = 2u << PM4_SIZE_L2QW(cntl);
    if (size > 0x800000u)
        size = 0x800000u; // the 32 MB window
    uint32_t mask = size - 1u;
    uint32_t base = REG(r, C_PM4_BUFFER_OFFSET);
    uint32_t rptr = REG(r, C_PM4_BUFFER_DL_RPTR) & mask;
    r->cce.in_ring = true;
    uint32_t moved = 0;
    while (rptr != (REG(r, C_PM4_BUFFER_DL_WPTR) & mask)) {
        uint32_t wptr = REG(r, C_PM4_BUFFER_DL_WPTR) & mask;
        // A run of dwords up to WPTR or the end of the ring.
        uint32_t run = (wptr > rptr ? wptr : size) - rptr;
        if (run > FETCH_RUN)
            run = FETCH_RUN;
        uint32_t chunk[FETCH_RUN];
        if (!r128_card_read(r, base + rptr * 4u, chunk, run)) {
            LOG(1, "Rage 128 CCE: ring fetch at card address $%08X failed (bus mastering %s) — the ring stalls",
                base + rptr * 4u, bm_enabled(r) ? "on" : "disabled by BUS_CNTL");
            break;
        }
        for (uint32_t k = 0; k < run; k++)
            feed(r, &r->cce.main, chunk[k]);
        rptr = (rptr + run) & mask;
        moved += run;
        // A runaway: a packet in the ring rewrote WPTR to chase itself.
        if (moved > 4u * size) {
            LOG(1, "Rage 128 CCE: the ring will not drain — stopped");
            break;
        }
    }
    r->cce.in_ring = false;
    REG(r, C_PM4_BUFFER_DL_RPTR) = rptr;
    uint32_t to = REG(r, C_PM4_BUFFER_DL_RPTR_ADDR);
    if (moved && !(cntl & PM4_NOUPDATE) && to) {
        uint8_t le[4] = {(uint8_t)rptr, (uint8_t)(rptr >> 8), (uint8_t)(rptr >> 16), (uint8_t)(rptr >> 24)};
        if (bm_enabled(r))
            pci_dma_write(r->dev, to & ~3u, le, 4);
    }
    LOG(4, "Rage 128 CCE: ring drained %u dwords, RPTR %u", moved, rptr);
}

// The indirect buffer: INDSIZE dwords at INDOFF, linear, no wrap.
static void indirect_run(rage128_t *r) {
    // INDOFF is an offset in the AGP window — "from the base of the indirect
    // buffer" space, SDK §5.3.3 — never a frame-buffer address: ATI's Mac OS
    // driver writes it without bit 25 ($00070000 and up).
    uint32_t addr = CARD_AGP_BIT | (REG(r, C_PM4_IW_INDOFF) & CARD_AGP_MASK);
    uint32_t count = REG(r, C_PM4_IW_INDSIZE) & 0x7FFFFFu;
    if (r->cce.in_indirect) {
        LOG(1, "Rage 128 CCE: the indirect buffer calls itself — ignored");
        return;
    }
    LOG(4, "Rage 128 CCE: indirect buffer, %u dwords at $%08X", count, addr);
    r->cce.in_indirect = true;
    r->cce.ind.len = r->cce.ind.need = 0;
    while (count) {
        uint32_t run = count > FETCH_RUN ? FETCH_RUN : count;
        uint32_t chunk[FETCH_RUN];
        if (!r128_card_read(r, addr, chunk, run)) {
            LOG(1, "Rage 128 CCE: indirect-buffer fetch at card address $%08X failed", addr);
            break;
        }
        for (uint32_t k = 0; k < run; k++)
            feed(r, &r->cce.ind, chunk[k]);
        addr += run * 4u, count -= run;
    }
    if (r->cce.ind.need)
        LOG(2, "Rage 128 CCE: the indirect buffer ends inside a packet (%u of %u dwords)", r->cce.ind.len,
            r->cce.ind.need);
    r->cce.ind.len = r->cce.ind.need = 0;
    r->cce.in_indirect = false;
}

// ============================================================
// Microcode
// ============================================================

static void ucode_check(rage128_t *r) {
    uint8_t be[2048];
    for (uint32_t a = 0; a < 256; a++)
        for (uint32_t h = 0; h < 2; h++) {
            uint32_t v = r->cce.ucode[a][h];
            uint8_t *o = be + 8u * a + 4u * h;
            o[0] = (uint8_t)(v >> 24), o[1] = (uint8_t)(v >> 16), o[2] = (uint8_t)(v >> 8), o[3] = (uint8_t)v;
        }
    uint32_t crc = gs_crc32(0, be, sizeof(be));
    r->cce.ucode_state = crc == R128_UCODE_CRC ? R128_UCODE_KNOWN : R128_UCODE_UNKNOWN;
    if (r->cce.ucode_state == R128_UCODE_KNOWN)
        LOG(1, "Rage 128 CCE: microcode uploaded — ATI's published Rage 128 image (CRC $%08X)", crc);
    else
        LOG(1, "Rage 128 CCE: microcode uploaded — not the known image (CRC $%08X); packets still run natively", crc);
}

const char *r128_cce_microcode_name(const rage128_t *r) {
    switch (r->cce.ucode_state) {
    case R128_UCODE_KNOWN:
        return "known";
    case R128_UCODE_UNKNOWN:
        return "unknown";
    default:
        return "none";
    }
}

// ============================================================
// Register interface
// ============================================================

void r128_cce_write(rage128_t *r, uint32_t off, uint32_t v) {
    switch (off) {
    case C_PM4_MICROCODE_ADDR:
        r->cce.ucode_addr = v & 0xFFu;
        break;
    case C_PM4_MICROCODE_RADDR:
        r->cce.ucode_raddr = v & 0xFFu;
        break;
    case C_PM4_MICROCODE_DATAH:
        r->cce.ucode[r->cce.ucode_addr][0] = v;
        break;
    case C_PM4_MICROCODE_DATAL:
        // The pair is complete: the address moves on, and the 256th pair
        // completes an image.
        r->cce.ucode[r->cce.ucode_addr][1] = v;
        r->cce.ucode_addr = (r->cce.ucode_addr + 1u) & 0xFFu;
        if (!r->cce.ucode_addr)
            ucode_check(r);
        break;
    case C_PM4_BUFFER_CNTL:
        if (PM4_MODE(v) != PM4_MODE(REG(r, off)))
            LOG(2, "Rage 128 CCE: PM4_BUFFER_CNTL mode %u (%s), ring %u dwords", PM4_MODE(v),
                mode_is_bm(PM4_MODE(v))    ? "bus master"
                : mode_is_pio(PM4_MODE(v)) ? "PIO"
                                           : "CCE off",
                2u << PM4_SIZE_L2QW(v));
        REG(r, off) = v;
        return;
    case C_PM4_BUFFER_DL_WPTR:
        REG(r, off) = v;
        ring_run(r);
        return;
    case C_PM4_MICRO_CNTL:
        REG(r, off) = v;
        ring_run(r); // a ring loaded before the engine ran
        return;
    case C_PM4_IW_INDSIZE:
        REG(r, off) = v;
        indirect_run(r);
        return;
    case C_PM4_FIFO_DATA_EVEN:
    case C_PM4_FIFO_DATA_ODD: {
        uint32_t mode = PM4_MODE(REG(r, C_PM4_BUFFER_CNTL));
        if (!mode_is_pio(mode)) {
            if (!(r->cce.told[0] & 1u)) {
                r->cce.told[0] |= 1u; // opcode $00 is never a type-3 op: reuse its bit
                LOG(1, "Rage 128 CCE: PIO FIFO write in PM4 mode %u — dropped", mode);
            }
            return;
        }
        feed(r, &r->cce.main, v);
        return;
    }
    default:
        break;
    }
    REG(r, off) = v;
}

uint32_t r128_cce_read(rage128_t *r, uint32_t off, bool peek) {
    switch (off) {
    case C_PM4_STAT:
        // Idle: the FIFO empty, neither the microengine nor the GUI busy.
        return fifo_size(PM4_MODE(REG(r, C_PM4_BUFFER_CNTL)));
    case C_PM4_BUFFER_ADDR: {
        uint32_t cntl = REG(r, C_PM4_BUFFER_CNTL);
        uint32_t mask = (2u << PM4_SIZE_L2QW(cntl)) - 1u;
        return REG(r, C_PM4_BUFFER_OFFSET) + (REG(r, C_PM4_BUFFER_DL_RPTR) & mask) * 4u;
    }
    case C_PM4_MICROCODE_ADDR:
        return r->cce.ucode_addr;
    case C_PM4_MICROCODE_RADDR:
        return r->cce.ucode_raddr;
    case C_PM4_MICROCODE_DATAH:
        return r->cce.ucode[r->cce.ucode_raddr][0];
    case C_PM4_MICROCODE_DATAL: {
        uint32_t v = r->cce.ucode[r->cce.ucode_raddr][1];
        if (!peek)
            r->cce.ucode_raddr = (r->cce.ucode_raddr + 1u) & 0xFFu;
        return v;
    }
    default:
        return REG(r, off);
    }
}

void r128_cce_reset(rage128_t *r) {
    memset(&r->cce, 0, sizeof(r->cce));
}

void r128_cce_soft_reset(rage128_t *r) {
    if (r->cce.main.need || r->cce.ind.need || r->host.active)
        LOG(2, "Rage 128 CCE: SOFT_RESET_GUI drops a packet or host-data operation in flight");
    r->cce.main.len = r->cce.main.need = 0;
    r->cce.ind.len = r->cce.ind.need = 0;
    r->host.active = false;
}
