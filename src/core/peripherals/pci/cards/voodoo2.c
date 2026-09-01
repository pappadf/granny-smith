// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// voodoo2.c
// The 3dfx Voodoo2 ("CVG") — PCI $121A:$0002, a ROM-less 3D-only
// PASS-THROUGH accelerator.  The Mac boards (TechWorks Power3D II, Micro
// Conversions Game Wizard) were the PC reference design with Mac drivers
// and a monitor pass-through cable; none is an Apple product and none
// carries an expansion ROM.
//
// This card is the opposite of the Mach64 GX in nearly every respect.
// There is no FCode and no ndrv: Open Firmware sees a device with one
// BAR and no ROM, allocates address space, and builds a bare node named
// `pci121a,2` (Apple, "Designing PCI Cards and Drivers", printed p.91,
// p.164).  The guest driver is a USER-SPACE CFM Glide library that walks
// the Name Registry, finds the node by PCI ID, and drives the card
// through config space and the single 16 MB aperture.  What this model
// owes the guest is therefore narrow: a correct config header, the
// vendor block at $40-$57, and an aperture that answers.
//
// One BAR, three faces (legacy map, fbiInit7[8]=0 — V2 p.20 §4):
//   $000000..$3FFFFF  register file (wrap aliases, chip select, swizzle)
//   $400000..$7FFFFF  linear frame buffer
//   $800000..$FFFFFF  texture memory (write-only; reads undefined)
//
// THE IDLE CONTRACT is the single most important behaviour here.
// status[9] aggregates every engine and FIFO, and 3dfx's own drivers
// poll it in unbounded loops (V2 p.128 §12.3 mandates three consecutive
// idle reads; sstfb requires five and carries an in-source XXX about
// hanging the machine).  A busy bit that never clears does not fail —
// it spins the guest forever.  The model is therefore INVERTED from
// real silicon: work completes synchronously at the point of issue and
// the card reports idle unless there is a specific, bounded reason not
// to; every such reason must state what clears it.  This is a
// documented divergence (proposal §8 Q3, docs/core/peripherals/pci/
// cards/voodoo2.md).
//
// ENDIANNESS.  The register file and LFB are little-endian PCI domain.
// This card is not a TNT device — it must not use a family macro — so
// it applies its own swap at its own edge: VOODOO2_LE32 below is the
// one sanctioned swap point.  On top of that physical fact sit the
// guest-visible swizzle paths, each controlled separately (V2 p.52-57):
//   registers    fbiInit0[3] enables, REGISTER ADDRESS BIT 20 selects
//                per access (the alias the PPC driver writes through)
//   LFB          lfbMode[12]/[11] on writes, [16]/[15] on reads — and
//                the transform ORDER REVERSES between the directions
//   texture      tLOD[25]/[26]
//
// Register truth: 3dfx, *Voodoo2 Graphics Specification* rev 1.16
// (Dec 1999) — cited "[V2 p.N]"; sequence and constants from 3dfx's own
// released Glide 2.x source (glide2x/cvg/init/, first-party vendor
// source, no code copied) — cited "[3dfx-src]"; enumeration contract
// from Apple's PCI book (revised 1999) — cited "[Apple-doc]"; Linux
// sstfb is cross-check only for hardware facts — cited "[GPL-src]",
// no code copied.  NOTHING here is derived from another emulator:
// dingusppc and MAME's voodoo model sit quarantined in the project's
// do-not-read directory precisely so this boundary is unambiguous.
//
// The fill convention of the (milestone 3c) rasteriser is CHOSEN, not
// known — V2 §7.2 defers the TRIANGLE walk to an SST-1 Programming
// Guide nobody has.  See voodoo2.md for the convention and the
// divergence list.

#include "card.h"
#include "checkpoint.h"
#include "config_space.h"
#include "display.h"
#include "log.h"
#include "memory.h"
#include "object.h"
#include "pci.h"
#include "scheduler.h"
#include "system.h"
#include "system_config.h"
#include "value.h"
#include "voodoo2_raster.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("voodoo2");

// The card's little-endian PCI domain.  The ONE sanctioned swap point in
// this file; see the endianness note above.
#define VOODOO2_LE32(x) __builtin_bswap32((uint32_t)(x))
#define VOODOO2_LE16(x) __builtin_bswap16((uint16_t)(x))

// ============================================================
// Identity and geometry
// ============================================================

#define V2_VENDOR_ID 0x121Au
// NOT $0001 — V2 §6.2's "default is 0x1" is a copy-paste leftover from
// the SST-1 spec (V1 p.72 says the same words).  The shipping value is
// $0002, per the kernel's pci_ids.h and sstfb's own match table, and the
// generated OF node name `pci121a,2` depends on it.  [V2 p.102, GPL-src]
#define V2_DEVICE_ID 0x0002u
// Hardwired $02 for backwards compatibility with Voodoo1; the REAL
// revision lives in initEnable[15:12].  [V2 p.103 §6.5]
#define V2_REVISION 0x02u
// Display controller / other (strap fb_addr_a[6]=0).  A display CLASS
// CODE on a card that must never be the machine's display — which is
// exactly why card_class below says "3d", not "display".  [V2 p.103]
#define V2_CLASS 0x038000u

#define V2_BAR_SIZE 0x1000000u // 16 MB, the only BAR [V2 p.104 §6.11]

// The three faces of the legacy map [V2 p.20 §4].
#define V2_OFF_LFB 0x400000u
#define V2_OFF_TEX 0x800000u

// Fixed board geometry: every retail SKU has a 4 MB framebuffer; the 8 MB
// and 12 MB boards differ in texture memory only (2 or 4 MB per TMU).
#define V2_FB_SIZE  0x400000u
#define V2_NUM_TMUS 2
#define V2_TMU_2MB  0x200000u
#define V2_TMU_4MB  0x400000u

// The real revision (initEnable[15:12]) and fab id (initEnable[19:16]).
// CVG production silicon; the PCI header's $02 is the pinned compat value.
#define V2_CHIP_REVISION 0x2u
#define V2_CHIP_FAB      0x1u

// ============================================================
// Register indices (dword offset/4) — V2 pp.22-26, cross-checked by
// scripts/voodoo2/voodoo2_regs.py --check
// ============================================================

#define R_STATUS        0x00
#define R_INTRCTRL      0x01
#define R_VERTEX_AX     0x02 // ..0x07: vertexAx..vertexCy
#define R_TRIANGLECMD   0x20
#define R_FTRIANGLECMD  0x40
#define R_FBZCOLORPATH  0x41
#define R_FOGMODE       0x42
#define R_ALPHAMODE     0x43
#define R_FBZMODE       0x44
#define R_LFBMODE       0x45
#define R_CLIPLR        0x46
#define R_CLIPTB        0x47
#define R_NOPCMD        0x48
#define R_FASTFILLCMD   0x49
#define R_SWAPBUFCMD    0x4A
#define R_FOGCOLOR      0x4B
#define R_ZACOLOR       0x4C
#define R_CHROMAKEY     0x4D
#define R_CHROMARANGE   0x4E
#define R_USERINTRCMD   0x4F
#define R_STIPPLE       0x50
#define R_COLOR0        0x51
#define R_COLOR1        0x52
#define R_PIXELS_IN     0x53
#define R_CHROMA_FAIL   0x54
#define R_ZFUNC_FAIL    0x55
#define R_AFUNC_FAIL    0x56
#define R_PIXELS_OUT    0x57
#define R_FOGTABLE      0x58 // ..0x77
#define R_CMDFIFO_BASE  0x78 // ..0x7E, non-FIFO
#define R_CMDFIFO_BUMP  0x79
#define R_CMDFIFO_RDPTR 0x7A
#define R_CMDFIFO_AMIN  0x7B
#define R_CMDFIFO_AMAX  0x7C
#define R_CMDFIFO_DEPTH 0x7D
#define R_CMDFIFO_HOLES 0x7E
#define R_FBIINIT4      0x80
#define R_VRETRACE      0x81
#define R_BACKPORCH     0x82
#define R_VIDEODIM      0x83
#define R_FBIINIT0      0x84
#define R_FBIINIT1      0x85
#define R_FBIINIT2      0x86
#define R_FBIINIT3      0x87
#define R_HSYNC         0x88
#define R_VSYNC         0x89
#define R_CLUTDATA      0x8A
#define R_DACDATA       0x8B
#define R_MAXRGBDELTA   0x8C
#define R_HBORDER       0x8D
#define R_VBORDER       0x8E
#define R_BORDERCOLOR   0x8F
#define R_HVRETRACE     0x90
#define R_FBIINIT5      0x91
#define R_FBIINIT6      0x92
#define R_FBIINIT7      0x93
#define R_SWAPHISTORY   0x96
#define R_TRIANGLESOUT  0x97
#define R_SETUPMODE     0x98 // ..0xA9: the on-chip setup block
#define R_SDRAWTRICMD   0xA8
#define R_SBEGINTRICMD  0xA9
#define R_BLT_FIRST     0xB0 // ..0xBF: the 2D BitBLT engine (a NON-GOAL)
#define R_TEXTUREMODE   0xC0 // TMU space begins: 0xC0..0xFF per TMU
#define R_TLOD          0xC1
#define R_TDETAIL       0xC2
#define R_TEXBASE       0xC3
#define R_TEXBASE_1     0xC4
#define R_TEXBASE_2     0xC5
#define R_TEXBASE_38    0xC6
#define R_TREXINIT0     0xC7
#define R_TREXINIT1     0xC8
#define R_NCC0_FIRST    0xC9 // ..0xD4
#define R_NCC1_FIRST    0xD5 // ..0xE0

#define V2_NUM_REGS      256
#define V2_TMU_REG_FIRST R_TEXTUREMODE

// fbiInit0 bits [V2 p.67 §5.52]
#define FBIINIT0_VGA_PASS                                                                                              \
    0x00000001u // 1 = the Voodoo drives the monitor
                // (the sstfb DIS_VGA_PASSTHROUGH
                // convention; see voodoo2.md — one
                // bit drives two complementary pins
                // so a convention must be chosen)
#define FBIINIT0_GRX_RESET  0x00000002u
#define FBIINIT0_FIFO_RESET 0x00000004u
#define FBIINIT0_SWIZZLE_EN 0x00000008u // + address bit 20, per access

// fbiInit1 bits [V2 p.68 §5.53]
#define FBIINIT1_LFB_READ_EN 0x00000008u
#define FBIINIT1_VIDEO_RESET 0x00000100u
#define FBIINIT1_SW_BLANK    0x00001000u
// Output enables 16:13 (data, blank, sync, dclk) — all must be driven
// before anything reaches the monitor; they default to tristate.
#define FBIINIT1_OUT_ENABLES 0x0001E000u

// fbiInit2 [V2 p.69 §5.54]
#define FBIINIT2_DRAM_REFRESH_EN 0x00400000u
#define FBIINIT2_MEMOFF_SHIFT    11
#define FBIINIT2_MEMOFF_MASK     0x1FFu

// fbiInit3 [V2 p.70 §5.55]
#define FBIINIT3_ALT_REGMAP 0x00000001u
#define FBIINIT3_TEXMAP_DIS 0x00000040u

// fbiInit7 [V2 p.73 §5.59]
#define FBIINIT7_CMDFIFO_EN 0x00000100u

// Power-on values, assembled from the spec's own per-bit defaults (each
// register's section) — they agree with Glide's SST_FBIINIT*_DEFAULT
// base values, which is a useful cross-check.  [V2 pp.67-73, 3dfx-src]
#define FBIINIT0_RESET 0x00000410u
#define FBIINIT1_RESET 0x00201102u // one write ws, video reset, BLANKED
#define FBIINIT2_RESET 0x80000040u
#define FBIINIT3_RESET 0x001E4000u
#define FBIINIT4_RESET 0x00000001u
// fbiInit7[7:0] mirrors the fb_data[63:56] straps; Glide derives the
// graphics clock as 50 + (fbiInit7>>2 & $3F) MHz, so the strap byte is
// chosen to yield the Voodoo2's ~90 MHz core.  [3dfx-src sst1InitCalcGrxClk]
#define FBIINIT7_RESET 0x000000A0u

// initEnable — the vendor config register that gates everything
// [V2 pp.106-107 §6.16]
#define INITEN_FBIINIT_WR 0x00000001u // gate on fbiInit* writes
#define INITEN_FIFO_WR    0x00000002u // "must be set for normal operation"
#define INITEN_DAC_REMAP  0x00000004u // fbiInit2 read returns DAC data
// 15:12 real revision and 19:16 fab id are hardware facts, read-only —
// even though the prose calls 31:12 scratch, the bit table wins.
#define INITEN_RO_MASK 0x000FF000u

// Vendor config offsets [V2 pp.102-107 §6]
#define CFG_INIT_ENABLE 0x40u
#define CFG_BUS_SNOOP0  0x44u
#define CFG_BUS_SNOOP1  0x48u
#define CFG_STATUS      0x4Cu
#define CFG_SCRATCH     0x50u
#define CFG_SIPROCESS   0x54u

// lfbMode fields [V2 pp.50-57 §5.21]
#define LFB_FMT(m)         ((m) & 0xFu)
#define LFB_WRITE_BUF(m)   (((m) >> 4) & 3u)
#define LFB_READ_BUF(m)    (((m) >> 6) & 3u)
#define LFB_PIPELINE(m)    (((m) >> 8) & 1u)
#define LFB_LANES(m)       (((m) >> 9) & 3u)
#define LFB_WR_WORDSWAP(m) (((m) >> 11) & 1u)
#define LFB_WR_SWIZZLE(m)  (((m) >> 12) & 1u)
#define LFB_Y_ORIGIN(m)    (((m) >> 13) & 1u)
#define LFB_RD_WORDSWAP(m) (((m) >> 15) & 1u)
#define LFB_RD_SWIZZLE(m)  (((m) >> 16) & 1u)
#define LFB_FMT_ZZ         15u // depth+depth: both halves land in the aux buffer

// The external DAC: one family modelled — the ICS5342 — and modelled
// exactly; the AT&T and TI back-door probes must NOT answer (Glide walks
// ICS -> ATT -> TI and takes the first that answers, so half-answering
// all three would be less faithful than answering one).  [3dfx-src dac.c]
#define DAC_REG_WMA      0x0 // palette write address / backdoor fsm reset
#define DAC_REG_RMR      0x2 // read mask (the ATT/TI backdoor window)
#define DAC_REG_PLL_WR   0x4
#define DAC_REG_PLL_DATA 0x5
#define DAC_REG_CMD      0x6
#define DAC_REG_PLL_RD   0x7
// ICS5342 PLL register file (addressed through PLL_WR/PLL_RD, data pairs
// M then P<<5|N through PLL_DATA).  Power-on M values are the detection
// signature Glide checks: VCLK1=$55, VCLK7=$71, GCLK1=$79.
#define DAC_PLL_VCLK0         0x0
#define DAC_PLL_VCLK1         0x1
#define DAC_PLL_VCLK7         0x7
#define DAC_PLL_GCLK0         0xA
#define DAC_PLL_GCLK1         0xB
#define DAC_PLL_CTRL          0xE
#define DAC_PLL_VCLK1_M_RESET 0x55u
#define DAC_PLL_VCLK7_M_RESET 0x71u
#define DAC_PLL_GCLK1_M_RESET 0x79u

// ============================================================
// Device state
// ============================================================

typedef struct voodoo2 {
    pci_device_t *dev;
    config_t *cfg;

    // Chuck register file, stored in the card's own (already byte-
    // swapped) domain; per-TMU registers live separately because the
    // chip-select field routes writes to one Bruce or both.
    uint32_t reg[V2_NUM_REGS];
    uint32_t tmu_reg[V2_NUM_TMUS][64];

    // Vendor config block.
    uint32_t init_enable;
    uint32_t bus_snoop[2]; // write-only, read as zero [V2 p.107]
    uint32_t cfg_scratch;
    uint32_t si_process;

    // The ICS5342.
    uint8_t dac_direct[8]; // WMA/RMR/CMD latches
    uint8_t dac_pll[16][2]; // [0] = M, [1] = P<<5 | N
    uint8_t dac_pll_wr_addr, dac_pll_rd_addr;
    uint8_t dac_pll_wr_phase, dac_pll_rd_phase;
    uint8_t dac_read_latch; // what a remapped fbiInit2 read returns

    // Memory.  fb_ram is always 4 MB; tex size is the 8/12 MB SKU choice.
    uint8_t *fb_ram;
    uint8_t *tex_ram[V2_NUM_TMUS];
    uint32_t tex_size; // per TMU

    // Per-TMU triangle-parameter latches.  startS/T and their gradients
    // are Bruce-only registers, startW/dWdX/dWdY go to Chuck AND the
    // selected Bruces — so each Bruce keeps its own copy of the dword
    // 0x02..0x1F latch range, routed by the chip-select field.
    uint32_t tmu_param[V2_NUM_TMUS][0x20];

    // NCC tables (two per TMU, 12 raw words each) and the 256-entry
    // texture palette written through nccTable0's I/Q space [V2 §5.92].
    uint32_t ncc[V2_NUM_TMUS][2][12];
    uint32_t palette[V2_NUM_TMUS][256]; // 24-bit RGB entries

    // The on-chip setup engine: the current vertex being assembled from
    // sV*/sARGB/... writes, and the three-vertex window of the strip.
    struct {
        float x, y, r, g, b, a, z, wb, w0, s0, t0, w1, s1, t1;
    } sv_cur, sv[3];
    int sv_count; // vertices accumulated (0..3)
    bool sv_flip; // strip ping-pong: odd triangles reverse the sign

    // The raster backend (proposal §3.6).  The software walker is the
    // default and only in-tree backend; null_raster exists to pin the
    // analytic-timing invariant in the tests.
    const voodoo2_raster_backend_t *raster;

    // Swap bookkeeping: swaps pending count until the frame boundary
    // after issue retires them (status[30:28], fbiSwapHistory), at which
    // point the displayed buffer flips (status[11:10]).
    uint32_t swaps_pending;
    uint64_t swap_issue_frame;
    uint32_t displayed_buffer; // physical colour buffer being scanned

    // The display face (milestone 3d).  The card presents
    // PIXEL_16BPP_565 in the display layer's big-endian convention;
    // fb_ram is the card's little-endian domain, so scanout holds the
    // converted raster (the conversion is this card's own edge, per
    // docs/core/peripherals/pci.md "Endianness at a card's edge").
    display_t display;
    uint8_t *scanout;
    bool driving; // last evaluated pass-through state (edge detection)

    // One-shot log guards.
    bool warned_narrow_reg;
    bool warned_cfg_unknown;
    bool warned_tex_read;
    bool warned_cmdfifo;

    memory_interface_t bar_if;
} voodoo2_t;

// Staged options (consumed by the factory, the mach64 idiom).
static uint32_t s_staged_tex_size = V2_TMU_2MB;
static bool s_staged_null_raster = false; // pci_option="raster=null"

// One pixel's state entering the back half of the pipeline (defined
// here because the LFB path feeds pixels into the same pipeline the
// rasteriser uses; the pipeline itself is with the rasteriser below).
typedef struct v2_pix {
    int32_t x, y; // screen pixel (top-left origin)
    uint32_t r, g, b, a; // clamped/wrapped 8-bit iterated colour
    uint32_t z16; // clamped 16-bit Z
    int64_t z_raw, w_raw; // unclamped iterators (float depth, fog)
    uint32_t w8; // clamped W byte for the ACU/fog
    uint32_t tex_argb; // TMU0 chain output (0 when texture disabled)
    bool have_tex;
} v2_pix_t;

static uint32_t v2_screen_height(const voodoo2_t *v);
static uint16_t v2_pack565(const voodoo2_t *v, int32_t x, int32_t y, uint32_t r, uint32_t g, uint32_t b);
static uint16_t v2_fb_load(const voodoo2_t *v, uint32_t buffer, int32_t x, int32_t y);
static void v2_fb_store(voodoo2_t *v, uint32_t buffer, int32_t x, int32_t y, uint16_t px);
static bool v2_pixel_pipe(voodoo2_t *v, v2_pix_t *p);
static void v2_clip_rect(const voodoo2_t *v, int32_t *x0, int32_t *x1, int32_t *y0, int32_t *y1);
static uint32_t v2_iter_w8(int64_t it, bool clamp);
static float v2_f32(uint32_t bits);
static void v2_fifo_execute(struct voodoo2 *v);

// ============================================================
// Beam position — derived from the scheduler, the mach64_scanline idiom
// ============================================================
// Only monotonic-within-frame matters: vRetrace/hvRetrace/status[6] must
// ADVANCE or a driver spinning on beam position hangs.  The frame is
// nominal 60 Hz against the programmed vertical total; no pixel clock is
// consumed.  (The DAC PLL frequency is modelled for the display timing
// that lands with the pass-through milestone.)

static uint32_t v2_vtotal(const voodoo2_t *v) {
    // vSync packs vSyncOff<<16 | vSyncOn; vSyncOff is the frame's total
    // line count for the shipped timings [V2 §13, 3dfx-src SST_VREZ_*].
    uint32_t total = (v->reg[R_VSYNC] >> 16) & 0x1FFFu;
    if (total < 2u || total > 4096u)
        total = 525u; // unprogrammed: nominal 640x480
    return total;
}

static uint32_t v2_scanline(const voodoo2_t *v, uint32_t *out_vtotal) {
    uint32_t vtotal = v2_vtotal(v);
    if (out_vtotal)
        *out_vtotal = vtotal;
    uint64_t frame = v->cfg->machine->freq / 60u;
    if (!frame)
        return 0;
    uint64_t pos = scheduler_cpu_cycles(v->cfg->scheduler) % frame;
    return (uint32_t)(pos * vtotal / frame);
}

static uint64_t v2_frame_number(const voodoo2_t *v) {
    uint64_t frame = v->cfg->machine->freq / 60u;
    return frame ? scheduler_cpu_cycles(v->cfg->scheduler) / frame : 0;
}

// Vertical retrace is ACTIVE while the beam is inside vSyncOn lines at
// the top of the frame; status[6] is 0 while active [V2 p.29].
static bool v2_in_vretrace(const voodoo2_t *v) {
    uint32_t vsync_on = v->reg[R_VSYNC] & 0x1FFFu;
    if (vsync_on == 0 || vsync_on > 64u)
        vsync_on = 2u;
    return v2_scanline(v, NULL) < vsync_on;
}

// ============================================================
// status — composed live, never stored
// ============================================================
// The synchronous model's answer to the idle contract: the PCI FIFO is
// always empty ($3F free), the memory FIFO always empty, and every busy
// bit reads 0 — work completed when it was issued.  The one live field
// besides the FIFO constants is the retrace bit, and the swap-pending
// count which retires at the frame boundary after issue.

static void v2_retire_swaps(voodoo2_t *v) {
    if (v->swaps_pending && v2_frame_number(v) != v->swap_issue_frame) {
        // Each retired swap flips which physical buffer is scanned out
        // (status[11:10]); an odd pending count nets one flip.
        if (v->swaps_pending & 1u)
            v->displayed_buffer ^= 1u;
        v->swaps_pending = 0;
        v->display.fb_dirty = true;
    }
}

static uint32_t v2_status(voodoo2_t *v) {
    v2_retire_swaps(v);
    uint32_t s = 0x3Fu; // PCI FIFO free space: empty
    if (!v2_in_vretrace(v))
        s |= 1u << 6; // 0 = retrace ACTIVE [V2 p.29]
    // bits 7 (Chuck busy), 8 (Bruce busy), 9 (busy) stay 0: idle.
    s |= (v->displayed_buffer & 3u) << 10;
    s |= 0xFFFFu << 12; // memory FIFO free space: empty
    s |= (v->swaps_pending > 7u ? 7u : v->swaps_pending) << 28;
    return s;
}

// ============================================================
// The ICS5342 and its PLL
// ============================================================

static void v2_dac_reset(voodoo2_t *v) {
    memset(v->dac_direct, 0, sizeof(v->dac_direct));
    v->dac_direct[DAC_REG_RMR] = 0xFFu; // read mask powers up all-ones
    memset(v->dac_pll, 0, sizeof(v->dac_pll));
    v->dac_pll[DAC_PLL_VCLK1][0] = DAC_PLL_VCLK1_M_RESET;
    v->dac_pll[DAC_PLL_VCLK7][0] = DAC_PLL_VCLK7_M_RESET;
    v->dac_pll[DAC_PLL_GCLK1][0] = DAC_PLL_GCLK1_M_RESET;
    // Only the M bytes above are detection-checked [3dfx-src]; the N/P
    // halves of the power-on values are not in our material, so they are
    // CHOSEN to make the unprogrammed clocks plausible (GCLK1 -> ~88 MHz
    // core; the driver reprograms GCLK0/VCLK0 before using either).
    v->dac_pll[DAC_PLL_VCLK1][1] = (1u << 5) | 4u;
    v->dac_pll[DAC_PLL_VCLK7][1] = (1u << 5) | 4u;
    v->dac_pll[DAC_PLL_GCLK1][1] = (2u << 5) | 3u;
    // Power-on clock routing: CLK1 (graphics) from GCLK1 [3dfx-src: the
    // driver programs GCLK0 and then CLEARS the select to switch over].
    v->dac_pll[DAC_PLL_CTRL][0] = 1u << 4;
    v->dac_pll_wr_addr = v->dac_pll_rd_addr = 0;
    v->dac_pll_wr_phase = v->dac_pll_rd_phase = 0;
    v->dac_read_latch = 0;
}

// One dacData write [V2 pp.75-76 §5.68]: data in [7:0], address in
// {13:12, 10:8}, bit 11 = read command.  A read parks its result in the
// latch that a remapped fbiInit2 read returns.
static void v2_dac_access(voodoo2_t *v, uint32_t value) {
    uint32_t addr = ((value >> 8) & 7u) | ((value >> 9) & 0x18u);
    bool is_read = (value >> 11) & 1u;
    if (is_read) {
        uint8_t data = 0;
        switch (addr) {
        case DAC_REG_RMR:
            data = v->dac_direct[DAC_REG_RMR];
            break;
        case DAC_REG_PLL_DATA:
            // Reads walk M then P<<5|N and toggle; a PLL_RD write resets
            // the phase.  This is the ICS detection path: GCLK1/VCLK1/
            // VCLK7's power-on M values are the signature.  [3dfx-src]
            data = v->dac_pll[v->dac_pll_rd_addr][v->dac_pll_rd_phase & 1u];
            v->dac_pll_rd_phase ^= 1u;
            break;
        default:
            data = (addr < 8u) ? v->dac_direct[addr] : 0;
            break;
        }
        v->dac_read_latch = data;
        return;
    }
    uint8_t data = value & 0xFFu;
    switch (addr) {
    case DAC_REG_PLL_WR:
        v->dac_pll_wr_addr = data & 0xFu;
        v->dac_pll_wr_phase = 0;
        break;
    case DAC_REG_PLL_RD:
        v->dac_pll_rd_addr = data & 0xFu;
        v->dac_pll_rd_phase = 0;
        break;
    case DAC_REG_PLL_DATA:
        v->dac_pll[v->dac_pll_wr_addr][v->dac_pll_wr_phase & 1u] = data;
        v->dac_pll_wr_phase ^= 1u;
        break;
    case DAC_REG_WMA:
        // Also resets the AT&T/TI back-door state machine — which this
        // model does not carry, because RMR reads return the read mask
        // and their manufacturer-ID compares therefore fail (the
        // "answer one family exactly" rule).
        v->dac_direct[DAC_REG_WMA] = data;
        break;
    default:
        if (addr < 8u)
            v->dac_direct[addr] = data;
        break;
    }
}

// PLL output frequency in kHz: Fout = Fref x (M+2) / (2^P x (N+2)),
// Fref = 14.318 MHz [proposal §4.4; ICS5342 as programmed by 3dfx-src].
static uint32_t v2_pll_khz(const voodoo2_t *v, int pll_index) {
    uint32_t m = v->dac_pll[pll_index][0];
    uint32_t pn = v->dac_pll[pll_index][1];
    uint32_t p = pn >> 5, n = pn & 0x1Fu;
    return (uint32_t)(14318ull * (m + 2u) / ((1ull << p) * (n + 2u)));
}

static uint32_t v2_video_pll_khz(const voodoo2_t *v) {
    // CLK0 (video) selects VCLK0-7 by the PLL control register's low bits.
    return v2_pll_khz(v, v->dac_pll[DAC_PLL_CTRL][0] & 7u);
}

static uint32_t v2_graphics_pll_khz(const voodoo2_t *v) {
    // CLK1 (graphics): control bit 4 set selects GCLK1, clear GCLK0.
    return v2_pll_khz(v, (v->dac_pll[DAC_PLL_CTRL][0] & 0x10u) ? DAC_PLL_GCLK1 : DAC_PLL_GCLK0);
}

// ============================================================
// LFB addressing
// ============================================================
// Software addresses the LFB as a 1024-pixel logical scanline: offset =
// (y<<11 | x<<1) bytes for 16-bit formats [V2 p.114 §9].  The physical
// placement models the DRAM layout linearly: colour buffer K starts at
// K x memOffset pages (fbiInit2[19:11], 4 KB pages), the aux/depth
// buffer after the colour buffers (count per fbiInit5[10:9]), and a row
// is tilesInX x 32 pixels wide ({fbiInit1[24], fbiInit1[7:4],
// fbiInit6[30]} [V2 pp.68,73]).  The map is injective within the 4 MB
// framebuffer and ALIASES modulo its size — which is load-bearing:
// Glide and sstfb size memory by writing sentinels and watching which
// addresses alias, so unpopulated space must wrap, never read as zero
// or all-ones [3dfx-src fbiMemSize, GPL-src].

static uint32_t v2_tiles_in_x(const voodoo2_t *v) {
    uint32_t t = ((v->reg[R_FBIINIT1] >> 24) & 1u) << 5;
    t |= ((v->reg[R_FBIINIT1] >> 4) & 0xFu) << 1;
    t |= (v->reg[R_FBIINIT6] >> 30) & 1u;
    return t ? t : 20u; // unprogrammed: 640 wide
}

static uint32_t v2_color_buffers(const voodoo2_t *v) {
    // fbiInit5[10:9]: 0 = 2 colour + 1 aux, 1 = 3 + 0, 2 = 3 + 1.
    uint32_t alloc = (v->reg[R_FBIINIT5] >> 9) & 3u;
    return (alloc == 0u) ? 2u : 3u;
}

// Physical byte address of a 16-bit pixel in a selected buffer.
// `buffer` is the SOFTWARE select: 0 = front (the displayed buffer),
// 1 = back (the other colour buffer), 3 = the aux/depth buffer.  The
// front/back names follow the displayed buffer across swaps.
static uint32_t v2_buffer_addr(const voodoo2_t *v, uint32_t buffer, uint32_t x, uint32_t y) {
    uint32_t mem_off_pages = (v->reg[R_FBIINIT2] >> FBIINIT2_MEMOFF_SHIFT) & FBIINIT2_MEMOFF_MASK;
    if (!mem_off_pages)
        mem_off_pages = 150u; // unprogrammed: one 640x480 16bpp buffer
    uint32_t phys;
    if (buffer == 3u)
        phys = v2_color_buffers(v);
    else if (buffer <= 1u)
        phys = buffer ^ v->displayed_buffer;
    else
        phys = buffer;
    uint32_t base = phys * mem_off_pages * 4096u;
    uint32_t stride = v2_tiles_in_x(v) * 32u * 2u;
    return (base + y * stride + x * 2u) & (V2_FB_SIZE - 1u);
}

// Resolve an LFB-face access to (buffer, x, y).  Reads are always two
// 16-bit pixels per doubleword whatever the write format [V2 p.56].
// The Y origin here is lfbMode[13]'s — which governs ALL reads and the
// bypass writes; pipeline-processed writes use fbzMode[17] instead
// (V2 p.53), which the pipeline path applies itself.
static void v2_lfb_locate(const voodoo2_t *v, uint32_t off, bool write, uint32_t *buffer, uint32_t *x, uint32_t *y) {
    uint32_t mode = v->reg[R_LFBMODE];
    bool wide = write && (LFB_FMT(mode) >= 4u && LFB_FMT(mode) <= 5u);
    *x = wide ? ((off >> 2) & 0x3FFu) : ((off >> 1) & 0x3FFu);
    *y = wide ? ((off >> 12) & 0x3FFu) : ((off >> 11) & 0x3FFu);
    // lfbMode[13]'s bottom origin governs all reads and BYPASS writes;
    // a pipeline-processed write flips per fbzMode[17] inside
    // v2_lfb_pixel instead (V2 p.53).
    if (LFB_Y_ORIGIN(mode) && !(write && LFB_PIPELINE(mode))) {
        uint32_t h = v2_screen_height(v);
        *y = (h - 1u - *y) & 0x3FFu;
    }
    if (write)
        *buffer = (LFB_FMT(mode) == LFB_FMT_ZZ) ? 3u : LFB_WRITE_BUF(mode);
    else
        *buffer = (LFB_READ_BUF(mode) == 2u) ? 3u : LFB_READ_BUF(mode);
}

// Expand one 16-bit LFB colour datum to 8-bit channels per the write
// format and colour-lane selection (V2 §5.21.1).  Returns whether the
// format carried an alpha.
static bool v2_lfb_expand16(uint32_t fmt, uint32_t lanes, uint16_t d, uint32_t *r, uint32_t *g, uint32_t *b,
                            uint32_t *a) {
    bool bgr = lanes & 1u; // formats 1/3 exchange red and blue
    uint32_t c1, c2, c3;
    bool has_a = false;
    if (fmt == 0u || fmt == 12u) { // 5-6-5
        c1 = (d >> 11) & 0x1Fu;
        c2 = (d >> 5) & 0x3Fu;
        c3 = d & 0x1Fu;
        c1 = (c1 << 3) | (c1 >> 2);
        c2 = (c2 << 2) | (c2 >> 4);
        c3 = (c3 << 3) | (c3 >> 2);
        *a = 255;
    } else { // x-5-5-5 or 1-5-5-5
        c1 = (d >> 10) & 0x1Fu;
        c2 = (d >> 5) & 0x1Fu;
        c3 = d & 0x1Fu;
        c1 = (c1 << 3) | (c1 >> 2);
        c2 = (c2 << 3) | (c2 >> 2);
        c3 = (c3 << 3) | (c3 >> 2);
        if (fmt == 2u || fmt == 14u) {
            *a = (d >> 15) ? 255u : 0u;
            has_a = true;
        } else {
            *a = 255;
        }
    }
    *r = bgr ? c3 : c1;
    *g = c2;
    *b = bgr ? c1 : c3;
    return has_a;
}

// One pixel entering the LFB path: either written raw (bypass — only
// dithering applies) or pushed through the full pixel pipeline with its
// depth/alpha from the data or zaColor (lfbMode[8]; V2 p.51-52).
static void v2_lfb_pixel(voodoo2_t *v, uint32_t buffer, uint32_t x, uint32_t y, uint32_t r, uint32_t g, uint32_t b,
                         uint32_t a, bool has_z, uint16_t z, bool write_color, bool write_z) {
    uint32_t mode = v->reg[R_LFBMODE];
    if (!LFB_PIPELINE(mode)) {
        if (write_color)
            v2_fb_store(v, buffer, (int32_t)x, (int32_t)y, v2_pack565(v, (int32_t)x, (int32_t)y, r, g, b));
        if (write_z)
            v2_fb_store(v, 3u, (int32_t)x, (int32_t)y, z);
        return;
    }
    // Pipeline-processed: the Y origin is fbzMode[17]'s, clipping
    // applies, and depth/alpha default to zaColor when the format
    // carried none.
    int32_t px = (int32_t)x, py = (int32_t)y;
    if ((v->reg[R_FBZMODE] >> 17) & 1u)
        py = (int32_t)v2_screen_height(v) - 1 - py;
    int32_t cx0, cx1, cy0, cy1;
    v2_clip_rect(v, &cx0, &cx1, &cy0, &cy1);
    if (px < cx0 || px >= cx1 || py < cy0 || py >= cy1)
        return;
    v2_pix_t p;
    p.x = px;
    p.y = py;
    p.r = r;
    p.g = g;
    p.b = b;
    p.a = a;
    p.z16 = has_z ? z : (uint16_t)(v->reg[R_ZACOLOR] & 0xFFFFu);
    p.z_raw = (int64_t)p.z16 << 12;
    // The W handed to the pipeline: the 16 MSBs of the fraction come
    // from the pixel's Z or from zaColor per lfbMode[14] (V2 p.53).
    uint16_t wsrc = (mode & 0x4000u) ? (uint16_t)(v->reg[R_ZACOLOR] & 0xFFFFu) : p.z16;
    p.w_raw = (int64_t)wsrc << 14;
    p.w8 = v2_iter_w8(p.w_raw, (v->reg[R_FBZCOLORPATH] >> 28) & 1u);
    p.tex_argb = 0;
    p.have_tex = false;
    v2_pixel_pipe(v, &p);
}

// The write transforms, applied in the documented order: byte swizzle
// FIRST (lfbMode[12], all incoming data), then 16-bit word swap
// (lfbMode[11]), then lane selection [V2 p.53 "Very Important Note"].
// The read path below applies the REVERSE order — lanes, word swap
// (lfbMode[15]), swizzle (lfbMode[16]) [V2 p.56].  Deliberately two
// functions: a shared helper with a direction flag is exactly the shape
// in which a later edit inverts the order.
static uint32_t v2_lfb_write_transform(const voodoo2_t *v, uint32_t le_value) {
    uint32_t mode = v->reg[R_LFBMODE];
    if (LFB_WR_SWIZZLE(mode))
        le_value = __builtin_bswap32(le_value);
    if (LFB_WR_WORDSWAP(mode))
        le_value = (le_value >> 16) | (le_value << 16);
    // Lane selection (ARGB orderings) applies to the colour formats when
    // pixels are composed; the 16-bit stores modelled here carry their
    // pixels through unchanged (lanes land with the rasteriser's colour
    // formats).
    return le_value;
}

static uint32_t v2_lfb_read_transform(const voodoo2_t *v, uint32_t le_value) {
    uint32_t mode = v->reg[R_LFBMODE];
    if (LFB_RD_WORDSWAP(mode))
        le_value = (le_value >> 16) | (le_value << 16);
    if (LFB_RD_SWIZZLE(mode))
        le_value = __builtin_bswap32(le_value);
    return le_value;
}

static void v2_lfb_store16(voodoo2_t *v, uint32_t buffer, uint32_t x, uint32_t y, uint16_t le_pixel) {
    uint32_t at = v2_buffer_addr(v, buffer, x, y);
    v->fb_ram[at] = (uint8_t)le_pixel;
    v->fb_ram[(at + 1u) & (V2_FB_SIZE - 1u)] = (uint8_t)(le_pixel >> 8);
}

static uint16_t v2_lfb_load16(const voodoo2_t *v, uint32_t buffer, uint32_t x, uint32_t y) {
    uint32_t at = v2_buffer_addr(v, buffer, x, y);
    return (uint16_t)(v->fb_ram[at] | ((uint16_t)v->fb_ram[(at + 1u) & (V2_FB_SIZE - 1u)] << 8));
}

// ============================================================
// Texture memory
// ============================================================
// Write-only from PCI; reads return undefined data [V2 p.119].  The
// aperture address selects the TMU in bits 22:21 and the texel address
// below [V2 p.119 §10].  Writes alias modulo the ADDRESSABLE size: the
// populated size, further clamped to 2 MB while trexInit0's second-RAS
// bit is clear — which is exactly what Glide's sense() probe toggles to
// size the memory [3dfx-src sst1InitGetTmuMemory].
#define TREXINIT0_SECOND_RAS 0x00004000u

static uint32_t v2_tmu_addressable(const voodoo2_t *v, int tmu) {
    uint32_t size = v->tex_size;
    if (!(v->tmu_reg[tmu][R_TREXINIT0 - V2_TMU_REG_FIRST] & TREXINIT0_SECOND_RAS) && size > V2_TMU_2MB)
        size = V2_TMU_2MB;
    return size;
}

// Per-TMU register shortcuts.
static uint32_t v2_tmu_r(const voodoo2_t *v, int tmu, int idx) {
    return v->tmu_reg[tmu][idx - V2_TMU_REG_FIRST];
}

// True when the TMU's format is one of the 8-bit texel formats (0-7).
static bool v2_tmu_is8bit(const voodoo2_t *v, int tmu) {
    return ((v2_tmu_r(v, tmu, R_TEXTUREMODE) >> 8) & 0xFu) < 8u;
}

// The size of one LOD level in bytes, from the spec's own table — kept
// VERBATIM, because the small levels have alignment floors that pure
// arithmetic gets wrong (V2 p.118: LOD 5 at 8:1 is 2^2 units, not 2^1).
// Values are log2 of the size in 8-byte units for 16-bit texels; 8-bit
// texels are half, which the table's own examples accumulate in
// half-units ("a remaining half can not be used" by another texture).
static uint32_t v2_lod_bytes(int lod, uint32_t aspect, bool is8) {
    static const uint8_t log2_units[9][4] = {
        {14, 13, 12, 11},
        {12, 11, 10, 9 },
        {10, 9,  8,  7 },
        {8,  7,  6,  5 },
        {6,  5,  4,  3 },
        {4,  3,  2,  2 },
        {2,  1,  1,  1 },
        {0,  0,  0,  0 },
        {0,  0,  0,  0 },
    };
    if (lod > 8)
        lod = 8;
    uint32_t bytes = 8u << log2_units[lod][aspect & 3u];
    return is8 ? bytes / 2u : bytes;
}

// Byte offset of LOD `lod`'s data relative to where LOD 0 would start —
// the mip chain packed contiguously, honouring lod_tsplit/lod_odd (a
// split texture stores only every other level).  This function plus
// v2_lod_bytes IS the address calculator the spec's three worked
// examples pin (V2 p.118; asserted in the integration test).
static uint32_t v2_lod_offset(const voodoo2_t *v, int tmu, int lod) {
    uint32_t tlod = v2_tmu_r(v, tmu, R_TLOD);
    uint32_t aspect = (tlod >> 21) & 3u;
    bool is8 = v2_tmu_is8bit(v, tmu);
    bool tsplit = (tlod >> 19) & 1u;
    uint32_t odd = (tlod >> 18) & 1u;
    uint32_t off = 0;
    for (int l = 0; l < lod && l <= 8; l++) {
        if (tsplit && ((uint32_t)l & 1u) != odd)
            continue; // split textures skip the other parity's levels
        off += v2_lod_bytes(l, aspect, is8);
    }
    return off;
}

// Width and height of LOD `lod` in texels (aspect and s-is-wider).
static void v2_lod_dims(const voodoo2_t *v, int tmu, int lod, uint32_t *w, uint32_t *h) {
    uint32_t tlod = v2_tmu_r(v, tmu, R_TLOD);
    uint32_t aspect = (tlod >> 21) & 3u;
    bool s_wider = (tlod >> 20) & 1u;
    uint32_t major = 256u >> (lod > 8 ? 8 : lod);
    if (!major)
        major = 1;
    uint32_t minor = major >> aspect;
    if (!minor)
        minor = 1;
    *w = s_wider ? major : minor;
    *h = s_wider ? minor : major;
    if (aspect == 0u)
        *w = *h = major; // square
}

// Base DRAM address of LOD `lod`: texBaseAddr (or the supplemental
// registers under tmultibaseaddr) in 8-byte units, plus the packed-chain
// offset.  texbaseaddr legitimately wraps below zero (V2 p.116); the
// modulo of the addressable size makes that work.
static uint32_t v2_tex_lod_base(const voodoo2_t *v, int tmu, int lod) {
    uint32_t tlod = v2_tmu_r(v, tmu, R_TLOD);
    int base_reg = R_TEXBASE;
    if ((tlod >> 24) & 1u) { // tmultibaseaddr
        if (lod == 1)
            base_reg = R_TEXBASE_1;
        else if (lod == 2)
            base_reg = R_TEXBASE_2;
        else if (lod >= 3)
            base_reg = R_TEXBASE_38;
    }
    uint32_t base = (v2_tmu_r(v, tmu, base_reg) & 0x7FFFFu) * 8u;
    if (base_reg == R_TEXBASE)
        base += v2_lod_offset(v, tmu, lod);
    else if (base_reg == R_TEXBASE_38 && lod > 3)
        base += v2_lod_offset(v, tmu, lod) - v2_lod_offset(v, tmu, 3);
    return base;
}

// DRAM byte address of texel (s,t) at `lod`.
static uint32_t v2_texel_addr(const voodoo2_t *v, int tmu, int lod, uint32_t s, uint32_t t) {
    uint32_t w, h;
    v2_lod_dims(v, tmu, lod, &w, &h);
    (void)h;
    uint32_t texel_bytes = v2_tmu_is8bit(v, tmu) ? 1u : 2u;
    uint32_t addr = v2_tex_lod_base(v, tmu, lod) + (t * w + s) * texel_bytes;
    return addr & (v2_tmu_addressable(v, tmu) - 1u);
}

// A texture-aperture write [V2 p.119].  The PCI address is a FIELD
// ENCODING, not a byte address: {TREX[22:21], LOD[20:17], T[16:9],
// S[8:2], 0[1:0]} — two 16-bit or four 8-bit texels per 32-bit write,
// with S right-aligned to bit 2 and T to bit 9 for smaller maps.
static void v2_tex_write(voodoo2_t *v, uint32_t off, uint32_t le_value) {
    int tmu = (off >> 21) & 3u;
    if (tmu >= V2_NUM_TMUS)
        tmu &= 1; // only two Bruces are populated; the third select aliases
    // tLOD[25]/[26]: the texture path's own swizzle and word swap, in
    // that order (swizzle first — V2 p.83).
    uint32_t tlod = v2_tmu_r(v, tmu, R_TLOD);
    if (tlod & (1u << 25))
        le_value = __builtin_bswap32(le_value);
    if (tlod & (1u << 26))
        le_value = (le_value >> 16) | (le_value << 16);
    uint32_t lod = (off >> 17) & 0xFu;
    uint32_t t = (off >> 9) & 0xFFu;
    bool is8 = v2_tmu_is8bit(v, tmu);
    uint32_t mode = v2_tmu_r(v, tmu, R_TEXTUREMODE);
    uint8_t *ram = v->tex_ram[tmu];
    uint32_t mask = v2_tmu_addressable(v, tmu) - 1u;
    uint32_t w, h;
    v2_lod_dims(v, tmu, (int)lod, &w, &h);
    (void)h;
    if (!is8) {
        // Two 16-bit texels: S[0] from the halves, S[7:1] from bits 8:2.
        uint32_t s0 = ((off >> 2) & 0x7Fu) << 1;
        for (uint32_t half = 0; half < 2u; half++) {
            uint32_t s = s0 + half;
            if (s >= w)
                continue; // narrow maps inhibit the upper texels
            uint32_t at = v2_texel_addr(v, tmu, (int)lod, s, t) & mask;
            uint16_t px = (uint16_t)(le_value >> (16u * half));
            ram[at] = (uint8_t)px;
            ram[(at + 1u) & mask] = (uint8_t)(px >> 8);
        }
    } else if (mode & (1u << 31)) {
        // seq_8_downld: four sequential 8-bit texels, S[7:2] from 8:3.
        uint32_t s0 = ((off >> 3) & 0x3Fu) << 2;
        for (uint32_t i = 0; i < 4u; i++) {
            uint32_t s = s0 + i;
            if (s >= w)
                continue;
            ram[v2_texel_addr(v, tmu, (int)lod, s, t) & mask] = (uint8_t)(le_value >> (8u * i));
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
            ram[v2_texel_addr(v, tmu, (int)lod, s, t) & mask] = (uint8_t)(le_value >> (8u * i));
        }
    }
}

// --- NCC / palette decode ---------------------------------------------------

// Decompress one 8-bit YIQ (4-2-2) texel through the selected NCC table
// [V2 §5.92]: Y indexes the 16-entry Y ramp, I and Q index four 9-bit
// signed RGB deltas each; sum and clamp.
static uint32_t v2_ncc_decode(const voodoo2_t *v, int tmu, int table, uint8_t texel) {
    const uint32_t *n = v->ncc[tmu][table];
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
static uint32_t v2_texel_expand(const voodoo2_t *v, int tmu, uint32_t raw) {
    uint32_t fmt = (v2_tmu_r(v, tmu, R_TEXTUREMODE) >> 8) & 0xFu;
    int table = (v2_tmu_r(v, tmu, R_TEXTUREMODE) >> 5) & 1u; // tnccselect
    uint32_t a, r, g, b, p;
    switch (fmt) {
    case 0: // 8-bit RGB 3-3-2
        r = (raw >> 5) & 7u;
        g = (raw >> 2) & 7u;
        b = raw & 3u;
        return 0xFF000000u | (((r << 5) | (r << 2) | (r >> 1)) << 16) | (((g << 5) | (g << 2) | (g >> 1)) << 8) |
               (b << 6) | (b << 4) | (b << 2) | b;
    case 1: // 8-bit YIQ
        return 0xFF000000u | v2_ncc_decode(v, tmu, table, (uint8_t)raw);
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
        return 0xFF000000u | (v->palette[tmu][raw & 0xFFu] & 0xFFFFFFu);
    case 6: { // 8-bit palette to RGBA (the P6 bit-slicing of V2 p.81)
        p = v->palette[tmu][raw & 0xFFu];
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
        return ((raw >> 8) << 24) | v2_ncc_decode(v, tmu, table, (uint8_t)raw);
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
        return (((raw >> 8) & 0xFFu) << 24) | (v->palette[tmu][raw & 0xFFu] & 0xFFFFFFu);
    default: // 7, 15: reserved — deterministically opaque black
        return 0xFF000000u;
    }
}

// Fetch and expand the texel at integer (s,t) with clamp/wrap applied.
static uint32_t v2_texel_fetch(const voodoo2_t *v, int tmu, int lod, int32_t s, int32_t t) {
    uint32_t w, h;
    v2_lod_dims(v, tmu, lod, &w, &h);
    uint32_t mode = v2_tmu_r(v, tmu, R_TEXTUREMODE);
    if (mode & (1u << 6)) // tclamps
        s = s < 0 ? 0 : (s >= (int32_t)w ? (int32_t)w - 1 : s);
    else
        s &= (int32_t)(w - 1u);
    if (mode & (1u << 7)) // tclampt
        t = t < 0 ? 0 : (t >= (int32_t)h ? (int32_t)h - 1 : t);
    else
        t &= (int32_t)(h - 1u);
    uint32_t at = v2_texel_addr(v, tmu, lod, (uint32_t)s, (uint32_t)t);
    uint32_t mask = v2_tmu_addressable(v, tmu) - 1u;
    uint32_t raw;
    if (v2_tmu_is8bit(v, tmu))
        raw = v->tex_ram[tmu][at & mask];
    else
        raw = v->tex_ram[tmu][at & mask] | ((uint32_t)v->tex_ram[tmu][(at + 1u) & mask] << 8);
    return v2_texel_expand(v, tmu, raw);
}

// Sample one TMU at texel-space (s,t) — point or bilinear per the
// filter bits and whether the LOD clamped to lodmin.
static uint32_t v2_tmu_sample(const voodoo2_t *v, int tmu, double s, double t, int lod, bool magnify) {
    uint32_t mode = v2_tmu_r(v, tmu, R_TEXTUREMODE);
    bool bilinear = magnify ? ((mode >> 2) & 1u) : ((mode >> 1) & 1u);
    s = ldexp(s, -lod);
    t = ldexp(t, -lod);
    if (!bilinear) {
        return v2_texel_fetch(v, tmu, lod, (int32_t)floor(s), (int32_t)floor(t));
    }
    // Bilinear: the four closest texels blended by the fractions of the
    // sample point relative to texel centres.
    double fs = s - 0.5, ft = t - 0.5;
    int32_t s0 = (int32_t)floor(fs), t0 = (int32_t)floor(ft);
    uint32_t frac_s = (uint32_t)((fs - s0) * 256.0) & 0xFFu;
    uint32_t frac_t = (uint32_t)((ft - t0) * 256.0) & 0xFFu;
    uint32_t c00 = v2_texel_fetch(v, tmu, lod, s0, t0);
    uint32_t c10 = v2_texel_fetch(v, tmu, lod, s0 + 1, t0);
    uint32_t c01 = v2_texel_fetch(v, tmu, lod, s0, t0 + 1);
    uint32_t c11 = v2_texel_fetch(v, tmu, lod, s0 + 1, t0 + 1);
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
static uint32_t v2_combine(uint32_t other, uint32_t local, uint32_t m, uint32_t ctl_bits, uint32_t add_val) {
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

// Screen height for the Y-origin flip, from videoDimensions.
static uint32_t v2_screen_height(const voodoo2_t *v) {
    uint32_t h = (v->reg[R_VIDEODIM] >> 16) & 0x7FFu;
    return h ? h : 480u;
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

static uint16_t v2_pack565(const voodoo2_t *v, int32_t x, int32_t y, uint32_t r, uint32_t g, uint32_t b) {
    uint32_t r5, g6, b5;
    if (v->reg[R_FBZMODE] & 0x100u) { // dithering enabled
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
        uint32_t d = (v->reg[R_FBZMODE] & 0x800u) ? v2_dither2[y & 1][x & 1] * 4u : v2_dither4[y & 3][x & 3];
        r5 = (r * 31u + 15u * d) / 255u;
        g6 = (g * 63u + 13u * d) / 255u;
        b5 = (b * 31u + 15u * d) / 255u;
    } else {
        r5 = r >> 3;
        g6 = g >> 2;
        b5 = b >> 3;
    }
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

// 16-bit raw framebuffer access at a physical (buffer, x, y).
static uint16_t v2_fb_load(const voodoo2_t *v, uint32_t buffer, int32_t x, int32_t y) {
    uint32_t at = v2_buffer_addr(v, buffer, (uint32_t)x, (uint32_t)y);
    return (uint16_t)(v->fb_ram[at] | ((uint16_t)v->fb_ram[(at + 1u) & (V2_FB_SIZE - 1u)] << 8));
}

static void v2_fb_store(voodoo2_t *v, uint32_t buffer, int32_t x, int32_t y, uint16_t px) {
    uint32_t at = v2_buffer_addr(v, buffer, (uint32_t)x, (uint32_t)y);
    v->fb_ram[at] = (uint8_t)px;
    v->fb_ram[(at + 1u) & (V2_FB_SIZE - 1u)] = (uint8_t)(px >> 8);
}

// The chosen 1/W -> 4.12 inverted-mantissa float (fbzMode[3]); the exact
// hardware normalisation is not in our material, so this is documented
// as chosen: exponent counts leading zeros below 1.0, mantissa is the
// next 12 bits inverted so integer comparisons keep their sense.
static uint16_t v2_depth_float(int64_t val) {
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
static uint32_t v2_blend_factor(uint32_t code, bool is_src, uint32_t src_a, uint32_t dst_a, uint32_t other_c,
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
static uint32_t v2_blend_mul(uint32_t c, uint32_t f) {
    return (c * (f + (f >> 7))) >> 8;
}

// The back half of the pipeline for one pixel.  Returns true if the
// pixel was written.  The caller has already applied clipping and
// computed the iterated values and the texture chain.
static bool v2_pixel_pipe(voodoo2_t *v, v2_pix_t *p) {
    uint32_t fbz = v->reg[R_FBZMODE];
    uint32_t fcp = v->reg[R_FBZCOLORPATH];
    uint32_t amode = v->reg[R_ALPHAMODE];

    v->reg[R_PIXELS_IN] = (v->reg[R_PIXELS_IN] + 1u) & 0xFFFFFFu;

    // Stipple (fbzMode[2]): rotate mode uses and rotates bit 31; pattern
    // mode indexes the 4x8 pattern by <x,y> (V2 p.47).
    if (fbz & 4u) {
        bool masked;
        if (fbz & 0x1000u) { // pattern mode
            uint32_t row = (v->reg[R_STIPPLE] >> (8u * (p->y & 3))) & 0xFFu;
            masked = !((row >> (7 - (p->x & 7))) & 1u);
        } else {
            masked = !(v->reg[R_STIPPLE] >> 31);
            v->reg[R_STIPPLE] = (v->reg[R_STIPPLE] << 1) | (v->reg[R_STIPPLE] >> 31);
        }
        if (masked)
            return false;
    } else if (!(fbz & 0x1000u)) {
        // The stipple register rotates in rotate mode even when masking
        // is disabled (V2 p.47).
        v->reg[R_STIPPLE] = (v->reg[R_STIPPLE] << 1) | (v->reg[R_STIPPLE] >> 31);
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
        oc_r = (v->reg[R_COLOR1] >> 16) & 0xFFu;
        oc_g = (v->reg[R_COLOR1] >> 8) & 0xFFu;
        oc_b = v->reg[R_COLOR1] & 0xFFu;
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
        oa = v->reg[R_COLOR1] >> 24;
        break;
    default:
        oa = p->a;
        break;
    }

    // Chroma-key / chroma-range on c_other, after texture, before the
    // combine units (V2 p.46).
    if (fbz & 2u) {
        uint32_t key = v->reg[R_CHROMAKEY] & 0xFFFFFFu;
        uint32_t c = (oc_r << 16) | (oc_g << 8) | oc_b;
        bool match;
        if (v->reg[R_CHROMARANGE] & (1u << 28)) {
            // Range compare: key..range inclusive per channel (the
            // mode bits select in/out of range; the inclusive band is
            // the modelled behaviour).
            uint32_t hi = v->reg[R_CHROMARANGE] & 0xFFFFFFu;
            match = oc_b >= (key & 0xFFu) && oc_b <= (hi & 0xFFu) && oc_g >= ((key >> 8) & 0xFFu) &&
                    oc_g <= ((hi >> 8) & 0xFFu) && oc_r >= ((key >> 16) & 0xFFu) && oc_r <= ((hi >> 16) & 0xFFu);
        } else {
            match = c == key;
        }
        if (match) {
            v->reg[R_CHROMA_FAIL] = (v->reg[R_CHROMA_FAIL] + 1u) & 0xFFFFFFu;
            return false;
        }
    }

    // c_local / a_local (fbzColorPath[4], [6:5], override [7]).
    uint32_t lc_r, lc_g, lc_b, la;
    bool local_is_c0 = (fcp >> 4) & 1u;
    if ((fcp >> 7) & 1u)
        local_is_c0 = (p->tex_argb >> 31) & 1u; // texture alpha bit 7
    if (local_is_c0) {
        lc_r = (v->reg[R_COLOR0] >> 16) & 0xFFu;
        lc_g = (v->reg[R_COLOR0] >> 8) & 0xFFu;
        lc_b = v->reg[R_COLOR0] & 0xFFu;
    } else {
        lc_r = p->r;
        lc_g = p->g;
        lc_b = p->b;
    }
    switch ((fcp >> 5) & 3u) {
    case 1:
        la = v->reg[R_COLOR0] >> 24;
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
    uint32_t fog = v->reg[R_FOGMODE];
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
            uint32_t word = v->reg[R_FOGTABLE + (idx >> 1)];
            fa = ((idx & 1u) ? (word >> 24) : (word >> 8)) & 0xFFu;
            break;
        }
        }
        uint32_t fr = (v->reg[R_FOGCOLOR] >> 16) & 0xFFu;
        uint32_t fg = (v->reg[R_FOGCOLOR] >> 8) & 0xFFu;
        uint32_t fb = v->reg[R_FOGCOLOR] & 0xFFu;
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
            v->reg[R_AFUNC_FAIL] = (v->reg[R_AFUNC_FAIL] + 1u) & 0xFFFFFFu;
            return false;
        }
    }
    if ((fbz & 0x2000u) && !(out_a & 1u)) {
        v->reg[R_AFUNC_FAIL] = (v->reg[R_AFUNC_FAIL] + 1u) & 0xFFFFFFu;
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
            int32_t biased = (int32_t)src + (int16_t)(v->reg[R_ZACOLOR] & 0xFFFFu);
            src = biased < 0 ? 0u : (biased > 0xFFFF ? 0xFFFFu : (uint32_t)biased);
        }
        depth_write = src;
        uint32_t cmp_src = (fbz & 0x100000u) ? (v->reg[R_ZACOLOR] & 0xFFFFu) : src;
        uint32_t dst = v2_fb_load(v, 3u, p->x, p->y);
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
            v->reg[R_ZFUNC_FAIL] = (v->reg[R_ZFUNC_FAIL] + 1u) & 0xFFFFFFu;
            return false;
        }
    }

    uint32_t draw_buf = (fbz >> 14) & 3u;
    if (draw_buf > 1u)
        draw_buf = 0;

    // Alpha blend (alphaMode[4], factors [23:8]).
    if (amode & 0x10u) {
        uint16_t dst565 = v2_fb_load(v, draw_buf, p->x, p->y);
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
        v2_fb_store(v, draw_buf, p->x, p->y, v2_pack565(v, p->x, p->y, out_r, out_g, out_b));
    if (fbz & 0x400u)
        v2_fb_store(v, 3u, p->x, p->y, (uint16_t)depth_write);
    v->reg[R_PIXELS_OUT] = (v->reg[R_PIXELS_OUT] + 1u) & 0xFFFFFFu;
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
static uint32_t v2_iter_rgba(int64_t it, bool clamp) {
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
static uint32_t v2_iter_z(int64_t it, bool clamp) {
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
static uint32_t v2_iter_w8(int64_t it, bool clamp) {
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
static uint32_t v2_texture_chain(voodoo2_t *v, const voodoo2_tri_t *T, int32_t dx, int32_t dy) {
    uint32_t chain = 0; // most-upstream c_other is zero
    for (int tmu = V2_NUM_TMUS - 1; tmu >= 0; tmu--) {
        uint32_t mode = v2_tmu_r(v, tmu, R_TEXTUREMODE);
        uint32_t trex1 = v2_tmu_r(v, tmu, R_TREXINIT1);
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
        uint32_t tlod = v2_tmu_r(v, tmu, R_TLOD);
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
        uint32_t texel = v2_tmu_sample(v, tmu, s, t, level, magnify);

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

// Clip rectangle in top-of-screen coordinates (always applied: the spec
// says rendering outside the screen with clipping off is undefined, so
// the model clips to the rectangle when enabled and to the raster
// otherwise).
static void v2_clip_rect(const voodoo2_t *v, int32_t *x0, int32_t *x1, int32_t *y0, int32_t *y1) {
    if (v->reg[R_FBZMODE] & 1u) {
        *x0 = (int32_t)((v->reg[R_CLIPLR] >> 16) & 0xFFFu);
        *x1 = (int32_t)(v->reg[R_CLIPLR] & 0xFFFu);
        *y0 = (int32_t)((v->reg[R_CLIPTB] >> 16) & 0xFFFu);
        *y1 = (int32_t)(v->reg[R_CLIPTB] & 0xFFFu);
    } else {
        *x0 = 0;
        *x1 = 1024;
        *y0 = 0;
        *y1 = (int32_t)v2_screen_height(v);
    }
}

static void v2_sw_triangle(voodoo2_t *v, const voodoo2_tri_t *T) {
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

    int32_t cx0, cx1, cy0, cy1;
    v2_clip_rect(v, &cx0, &cx1, &cy0, &cy1);
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

    bool clamp = (v->reg[R_FBZCOLORPATH] >> 28) & 1u;
    bool tex_on = ((v->reg[R_FBZCOLORPATH] >> 27) & 1u) && !(v->reg[R_FBIINIT3] & FBIINIT3_TEXMAP_DIS);
    bool y_flip = (v->reg[R_FBZMODE] >> 17) & 1u;
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
            p.y = y_flip ? (int32_t)v2_screen_height(v) - 1 - y : y;
            p.r = v2_iter_rgba(T->r + (int64_t)T->drdx * dx + (int64_t)T->drdy * dy, clamp);
            p.g = v2_iter_rgba(T->g + (int64_t)T->dgdx * dx + (int64_t)T->dgdy * dy, clamp);
            p.b = v2_iter_rgba(T->b + (int64_t)T->dbdx * dx + (int64_t)T->dbdy * dy, clamp);
            p.a = v2_iter_rgba(T->a + (int64_t)T->dadx * dx + (int64_t)T->dady * dy, clamp);
            p.z_raw = T->z + (int64_t)T->dzdx * dx + (int64_t)T->dzdy * dy;
            p.z16 = v2_iter_z(p.z_raw, clamp);
            p.w_raw = T->w + T->dwdx * dx + T->dwdy * dy;
            p.w8 = v2_iter_w8(p.w_raw, clamp);
            p.have_tex = tex_on;
            p.tex_argb = tex_on ? v2_texture_chain(v, T, dx, dy) : 0u;
            v2_pixel_pipe(v, &p);
        }
    }
}

static void v2_sw_fastfill(voodoo2_t *v) {
    // FASTFILL clears the clip rectangle with color1 (dithered) and/or
    // zaColor[15:0], honouring only the write masks and draw-buffer
    // select — the depth/alpha/blend stages are bypassed (V2 §5.24).
    int32_t x0 = (int32_t)((v->reg[R_CLIPLR] >> 16) & 0xFFFu);
    int32_t x1 = (int32_t)(v->reg[R_CLIPLR] & 0xFFFu);
    int32_t y0 = (int32_t)((v->reg[R_CLIPTB] >> 16) & 0xFFFu);
    int32_t y1 = (int32_t)(v->reg[R_CLIPTB] & 0xFFFu);
    uint32_t fbz = v->reg[R_FBZMODE];
    uint32_t draw_buf = (fbz >> 14) & 3u;
    if (draw_buf > 1u)
        draw_buf = 0;
    bool y_flip = (fbz >> 17) & 1u;
    uint32_t cr = (v->reg[R_COLOR1] >> 16) & 0xFFu;
    uint32_t cg = (v->reg[R_COLOR1] >> 8) & 0xFFu;
    uint32_t cb = v->reg[R_COLOR1] & 0xFFu;
    uint16_t za = (uint16_t)(v->reg[R_ZACOLOR] & 0xFFFFu);
    for (int32_t y = y0; y < y1; y++) {
        int32_t py = y_flip ? (int32_t)v2_screen_height(v) - 1 - y : y;
        for (int32_t x = x0; x < x1; x++) {
            if (fbz & 0x200u)
                v2_fb_store(v, draw_buf, x, py, v2_pack565(v, x, py, cr, cg, cb));
            if (fbz & 0x400u)
                v2_fb_store(v, 3u, x, py, za);
        }
    }
}

static void v2_sw_sync(voodoo2_t *v) {
    (void)v; // the software walker renders directly into the shadow
}

static const voodoo2_raster_backend_t v2_sw_backend = {
    .name = "sw",
    .triangle = v2_sw_triangle,
    .fastfill = v2_sw_fastfill,
    .sync = v2_sw_sync,
};

// The null backend draws nothing; it exists so a test can pin invariant
// 1 of the seam (§3.6): the guest-visible instruction stream must be
// identical whichever backend is installed.
static void v2_null_triangle(voodoo2_t *v, const voodoo2_tri_t *T) {
    (void)v;
    (void)T;
}
static const voodoo2_raster_backend_t v2_null_backend = {
    .name = "null",
    .triangle = v2_null_triangle,
    .fastfill = v2_sw_fastfill, // fastfill stays: it is the clear path
    .sync = v2_sw_sync,
};

// ============================================================
// Triangle submission — both routes converge on voodoo2_tri_t
// ============================================================

// Sign extension helpers for the latch formats.
static int32_t v2_sx16(uint32_t x) {
    return (int32_t)(int16_t)x;
}
static int32_t v2_sx24(uint32_t x) {
    return ((int32_t)(x << 8)) >> 8;
}

// The host-setup route: build the triangle from the Chuck and per-TMU
// latches and hand it to the backend.  With sub-pixel correction
// enabled (fbzColorPath[26]) the correction is applied TO THE LATCHES —
// so a second triangle issued without resending its start parameters is
// corrected twice, exactly as V2 p.40 documents the hardware doing
// (proposal §8 Q9: a model that caches uncorrected values would be more
// correct than the hardware and disagree with it).
static void v2_triangle_cmd(voodoo2_t *v, bool sign_bit) {
    if ((v->reg[R_FBZCOLORPATH] >> 26) & 1u) {
        int32_t fx = v2_sx16(v->reg[0x02]) & 0xF;
        int32_t fy = v2_sx16(v->reg[0x03]) & 0xF;
        if (fx || fy) {
            for (int i = 0; i < 8; i++) { // the 8 start parameters
                int64_t s = (i < 5) ? v2_sx24(v->reg[0x08 + i]) : (int32_t)v->reg[0x08 + i];
                int64_t ddx = (i < 5) ? v2_sx24(v->reg[0x10 + i]) : (int32_t)v->reg[0x10 + i];
                int64_t ddy = (i < 5) ? v2_sx24(v->reg[0x18 + i]) : (int32_t)v->reg[0x18 + i];
                v->reg[0x08 + i] = (uint32_t)(s - ((ddx * fx + ddy * fy) >> 4));
            }
            for (int t = 0; t < V2_NUM_TMUS; t++) {
                for (int i = 5; i < 8; i++) { // S, T, W per TMU
                    int64_t s = (int32_t)v->tmu_param[t][0x08 + i];
                    int64_t ddx = (int32_t)v->tmu_param[t][0x10 + i];
                    int64_t ddy = (int32_t)v->tmu_param[t][0x18 + i];
                    v->tmu_param[t][0x08 + i] = (uint32_t)(s - ((ddx * fx + ddy * fy) >> 4));
                }
            }
        }
    }
    voodoo2_tri_t T;
    T.ax = v2_sx16(v->reg[0x02]);
    T.ay = v2_sx16(v->reg[0x03]);
    T.bx = v2_sx16(v->reg[0x04]);
    T.by = v2_sx16(v->reg[0x05]);
    T.cx = v2_sx16(v->reg[0x06]);
    T.cy = v2_sx16(v->reg[0x07]);
    T.r = v2_sx24(v->reg[0x08]);
    T.g = v2_sx24(v->reg[0x09]);
    T.b = v2_sx24(v->reg[0x0A]);
    T.z = (int32_t)v->reg[0x0B];
    T.a = v2_sx24(v->reg[0x0C]);
    T.drdx = v2_sx24(v->reg[0x10]);
    T.dgdx = v2_sx24(v->reg[0x11]);
    T.dbdx = v2_sx24(v->reg[0x12]);
    T.dzdx = (int32_t)v->reg[0x13];
    T.dadx = v2_sx24(v->reg[0x14]);
    T.drdy = v2_sx24(v->reg[0x18]);
    T.dgdy = v2_sx24(v->reg[0x19]);
    T.dbdy = v2_sx24(v->reg[0x1A]);
    T.dzdy = (int32_t)v->reg[0x1B];
    T.dady = v2_sx24(v->reg[0x1C]);
    T.w = (int32_t)v->reg[0x0F];
    T.dwdx = (int32_t)v->reg[0x17];
    T.dwdy = (int32_t)v->reg[0x1F];
    for (int t = 0; t < V2_NUM_TMUS; t++) {
        T.s[t] = (int32_t)v->tmu_param[t][0x0D];
        T.t[t] = (int32_t)v->tmu_param[t][0x0E];
        T.tw[t] = (int32_t)v->tmu_param[t][0x0F];
        T.dsdx[t] = (int32_t)v->tmu_param[t][0x15];
        T.dtdx[t] = (int32_t)v->tmu_param[t][0x16];
        T.dtwdx[t] = (int32_t)v->tmu_param[t][0x17];
        T.dsdy[t] = (int32_t)v->tmu_param[t][0x1D];
        T.dtdy[t] = (int32_t)v->tmu_param[t][0x1E];
        T.dtwdy[t] = (int32_t)v->tmu_param[t][0x1F];
    }
    T.area_sign = sign_bit;
    v->raster->triangle(v, &T);
    v->reg[R_TRIANGLESOUT] = (v->reg[R_TRIANGLESOUT] + 1u) & 0xFFFFFFu;
}

// ============================================================
// The on-chip setup engine (V2 §5.69-5.85)
// ============================================================
// The host writes per-vertex data only; the engine computes the
// gradients, culls, and sequences strips and fans, then enters the SAME
// walker.

static float v2_f32(uint32_t bits) {
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

// Compute the gradients from the three float vertices and draw.
static void v2_setup_draw(voodoo2_t *v) {
    uint32_t sm = v->reg[R_SETUPMODE];
    float x0 = v->sv[0].x, y0 = v->sv[0].y;
    float x1 = v->sv[1].x, y1 = v->sv[1].y;
    float x2 = v->sv[2].x, y2 = v->sv[2].y;
    float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    bool sign = area < 0.0f;
    if ((sm >> 16) & 1u) {
        // fan mode keeps vertex 0; strips flip winding every other
        // triangle unless the ping-pong correction is disabled.
    }
    bool strip_flip = v->sv_flip && !((sm >> 19) & 1u) && !((sm >> 16) & 1u);
    bool eff_sign = sign ^ strip_flip;
    if ((sm >> 17) & 1u) { // culling enabled: reject the matching sign
        bool cull_sign = (sm >> 18) & 1u;
        if (eff_sign == cull_sign)
            return;
    }
    if (area == 0.0f)
        return;
    float ooa = 1.0f / area;
#define V2_GRAD(field, dxout, dyout)                                                                                   \
    do {                                                                                                               \
        float p0 = v->sv[0].field, p1 = v->sv[1].field, p2 = v->sv[2].field;                                           \
        dxout = ((p1 - p0) * (y2 - y0) - (p2 - p0) * (y1 - y0)) * ooa;                                                 \
        dyout = ((p2 - p0) * (x1 - x0) - (p1 - p0) * (x2 - x0)) * ooa;                                                 \
    } while (0)
    voodoo2_tri_t T;
    memset(&T, 0, sizeof(T));
    T.ax = (int32_t)(x0 * 16.0f);
    T.ay = (int32_t)(y0 * 16.0f);
    T.bx = (int32_t)(x1 * 16.0f);
    T.by = (int32_t)(y1 * 16.0f);
    T.cx = (int32_t)(x2 * 16.0f);
    T.cy = (int32_t)(y2 * 16.0f);
    float ddx, ddy;
    if (sm & 1u) { // RGB
        V2_GRAD(r, ddx, ddy);
        T.r = (int32_t)(v->sv[0].r * 4096.0f);
        T.drdx = (int32_t)(ddx * 4096.0f);
        T.drdy = (int32_t)(ddy * 4096.0f);
        V2_GRAD(g, ddx, ddy);
        T.g = (int32_t)(v->sv[0].g * 4096.0f);
        T.dgdx = (int32_t)(ddx * 4096.0f);
        T.dgdy = (int32_t)(ddy * 4096.0f);
        V2_GRAD(b, ddx, ddy);
        T.b = (int32_t)(v->sv[0].b * 4096.0f);
        T.dbdx = (int32_t)(ddx * 4096.0f);
        T.dbdy = (int32_t)(ddy * 4096.0f);
    }
    if (sm & 2u) { // alpha
        V2_GRAD(a, ddx, ddy);
        T.a = (int32_t)(v->sv[0].a * 4096.0f);
        T.dadx = (int32_t)(ddx * 4096.0f);
        T.dady = (int32_t)(ddy * 4096.0f);
    }
    if (sm & 4u) { // Z
        V2_GRAD(z, ddx, ddy);
        T.z = (int32_t)(v->sv[0].z * 4096.0f);
        T.dzdx = (int32_t)(ddx * 4096.0f);
        T.dzdy = (int32_t)(ddy * 4096.0f);
    }
    if (sm & 8u) { // global W
        V2_GRAD(wb, ddx, ddy);
        T.w = (int64_t)(v->sv[0].wb * 1073741824.0);
        T.dwdx = (int64_t)(ddx * 1073741824.0);
        T.dwdy = (int64_t)(ddy * 1073741824.0);
    }
    for (int t = 0; t < V2_NUM_TMUS; t++) {
        int wbit = t == 0 ? 4 : 6, stbit = t == 0 ? 5 : 7;
        if (sm & (1u << wbit)) {
            if (t == 0)
                V2_GRAD(w0, ddx, ddy);
            else
                V2_GRAD(w1, ddx, ddy);
            T.tw[t] = (int64_t)((t == 0 ? v->sv[0].w0 : v->sv[0].w1) * 1073741824.0);
            T.dtwdx[t] = (int64_t)(ddx * 1073741824.0);
            T.dtwdy[t] = (int64_t)(ddy * 1073741824.0);
        } else if (sm & 8u) {
            T.tw[t] = T.w;
            T.dtwdx[t] = T.dwdx;
            T.dtwdy[t] = T.dwdy;
        }
        if (sm & (1u << stbit)) {
            if (t == 0)
                V2_GRAD(s0, ddx, ddy);
            else
                V2_GRAD(s1, ddx, ddy);
            T.s[t] = (int64_t)((t == 0 ? v->sv[0].s0 : v->sv[0].s1) * 262144.0);
            T.dsdx[t] = (int64_t)(ddx * 262144.0);
            T.dsdy[t] = (int64_t)(ddy * 262144.0);
            if (t == 0)
                V2_GRAD(t0, ddx, ddy);
            else
                V2_GRAD(t1, ddx, ddy);
            T.t[t] = (int64_t)((t == 0 ? v->sv[0].t0 : v->sv[0].t1) * 262144.0);
            T.dtdx[t] = (int64_t)(ddx * 262144.0);
            T.dtdy[t] = (int64_t)(ddy * 262144.0);
        }
    }
#undef V2_GRAD
    T.area_sign = eff_sign;
    v->raster->triangle(v, &T);
    v->reg[R_TRIANGLESOUT] = (v->reg[R_TRIANGLESOUT] + 1u) & 0xFFFFFFu;
}

static void v2_setup_vertex(voodoo2_t *v) {
    uint32_t sm = v->reg[R_SETUPMODE];
    if (v->sv_count < 3) {
        v->sv[v->sv_count++] = v->sv_cur;
        if (v->sv_count < 3)
            return;
        v->sv_flip = false;
    } else if ((sm >> 16) & 1u) { // fan: keep vertex 0
        v->sv[1] = v->sv[2];
        v->sv[2] = v->sv_cur;
    } else { // strip: slide the window, flipping winding
        v->sv[0] = v->sv[1];
        v->sv[1] = v->sv[2];
        v->sv[2] = v->sv_cur;
        v->sv_flip = !v->sv_flip;
    }
    v2_setup_draw(v);
}

// ============================================================
// The register file
// ============================================================

// Alternate triangle mapping [V2 pp.27-29]: with fbiInit3[0]=1 and
// address bit 21 set, each parameter's start/dX/dY become adjacent.
// Generated from the same rule voodoo2_regs.py cross-validates: alt
// base+3i/+1/+2 -> std base+i / base+8+i / base+16+i, for the integer
// block at dword 0x08 and the float block at 0x28.
static int v2_alt_to_std(int idx) {
    if (idx >= 0x08 && idx < 0x20) {
        int i = (idx - 0x08) / 3, part = (idx - 0x08) % 3;
        return 0x08 + part * 8 + i;
    }
    if (idx >= 0x28 && idx < 0x40) {
        int i = (idx - 0x28) / 3, part = (idx - 0x28) % 3;
        return 0x28 + part * 8 + i;
    }
    if (idx == R_INTRCTRL)
        return -1; // $004 is reserved in the alternate map
    return idx; // status, vertices, both triangleCMDs, and 0x41+ unchanged
}

static bool v2_is_fbiinit(int idx) {
    return idx == R_FBIINIT4 || (idx >= R_FBIINIT0 && idx <= R_FBIINIT3) || (idx >= R_FBIINIT5 && idx <= R_FBIINIT7);
}

static uint32_t v2_reg_read(voodoo2_t *v, int idx) {
    switch (idx) {
    case R_STATUS:
        return v2_status(v);
    case R_INTRCTRL:
        return v->reg[R_INTRCTRL]; // enables/flags stored; pin state lands
                                   // with the interrupt wiring
    case R_FBIINIT2:
        // The DAC read-back detour [V2 p.76]: while initEnable[2] remaps
        // {fbiInit2,fbiInit3}->{dacRead,videoChecksum}, bits 7:0 return
        // the last value read from the external DAC and 31:8 are
        // undefined (deterministically zero here).
        if (v->init_enable & INITEN_DAC_REMAP)
            return v->dac_read_latch;
        return v->reg[R_FBIINIT2];
    case R_VRETRACE: {
        // vSyncOff counter, 0 while vsync is active [V2 p.62].
        if (v2_in_vretrace(v))
            return 0;
        return v2_scanline(v, NULL) & 0x1FFFu;
    }
    case R_HVRETRACE: {
        // Horizontal and vertical beam counters [V2 p.65].  The
        // horizontal count only has to advance; it is derived from the
        // scanline phase so both halves move together.
        uint32_t line = v2_scanline(v, NULL);
        uint32_t hpos = (uint32_t)(scheduler_cpu_cycles(v->cfg->scheduler) & 0x1FFu);
        return (line << 16) | hpos;
    }
    case R_SWAPHISTORY:
        v2_retire_swaps(v);
        return v->reg[R_SWAPHISTORY];
    default:
        break;
    }
    if (idx >= V2_TMU_REG_FIRST)
        return 0; // Bruce-specific registers read undefined [V2 p.22];
                  // deterministically zero, and reads come from Chuck
    return v->reg[idx];
}

// Convert one IEEE-single float-mirror write into the corresponding
// fixed-point latch value (truncation toward zero — the conversion
// rounding is not in our material, so it is chosen and documented).
static uint32_t v2_float_to_latch(int fixed_idx, uint32_t bits) {
    double f = (double)v2_f32(bits);
    if (fixed_idx >= 0x02 && fixed_idx <= 0x07)
        return (uint32_t)(int32_t)(f * 16.0); // 12.4 vertices
    switch (fixed_idx & 7) {
    case 5:
    case 6:
        return (uint32_t)(int32_t)(f * 262144.0); // 14.18 S/W, T/W
    case 7:
        return (uint32_t)(int64_t)(f * 1073741824.0); // 2.30 1/W
    default:
        return (uint32_t)(int32_t)(f * 4096.0); // 12.12 colour, 20.12 Z
    }
}

// One register write, already decoded: `chip_mask` bit 0 = Chuck, bits
// 1..2 = the two Bruces (0 = everything) [V2 p.21 §5].
static void v2_reg_write(voodoo2_t *v, int idx, uint32_t chip_mask, uint32_t value) {
    if (chip_mask == 0)
        chip_mask = 0x7u; // 0 selects all chips

    // Triangle-parameter latches (vertices, starts, gradients) route by
    // chip select — startS/T and their gradients are Bruce-only, the
    // rest go to Chuck and/or the selected Bruces; every chip keeps its
    // own copy.  The float mirrors convert into the SAME latches.
    if (idx >= 0x02 && idx <= 0x1F) {
        if (chip_mask & 1u)
            v->reg[idx] = value;
        for (int t = 0; t < V2_NUM_TMUS; t++) {
            if (chip_mask & (2u << t))
                v->tmu_param[t][idx] = value;
        }
        return;
    }
    if (idx >= 0x22 && idx <= 0x3F) {
        uint32_t conv = v2_float_to_latch(idx - 0x20, value);
        if (chip_mask & 1u)
            v->reg[idx - 0x20] = conv;
        for (int t = 0; t < V2_NUM_TMUS; t++) {
            if (chip_mask & (2u << t))
                v->tmu_param[t][idx - 0x20] = conv;
        }
        return;
    }

    // The TMU block routes by chip select; everything below is Chuck.
    if (idx >= V2_TMU_REG_FIRST) {
        for (int t = 0; t < V2_NUM_TMUS; t++) {
            if (!(chip_mask & (2u << t)))
                continue;
            // NCC table writes with the data MSB set are PALETTE writes
            // (V2 §5.92.2): index[7:1] from data 30:24 [3dfx-src: Glide
            // writes 0x80000000 | (index & 0xFE) << 23 | RGB], index[0]
            // from the I/Q register's address parity.
            if (idx >= R_NCC0_FIRST && idx <= R_NCC0_FIRST + 11) {
                int off = idx - R_NCC0_FIRST;
                if (off >= 4 && (value & 0x80000000u)) {
                    uint32_t pi = ((value >> 23) & 0xFEu) | ((uint32_t)off & 1u);
                    v->palette[t][pi] = value & 0xFFFFFFu;
                } else {
                    v->ncc[t][0][off] = value;
                }
                continue;
            }
            if (idx >= R_NCC1_FIRST && idx <= R_NCC1_FIRST + 11) {
                v->ncc[t][1][idx - R_NCC1_FIRST] = value;
                continue;
            }
            v->tmu_reg[t][idx - V2_TMU_REG_FIRST] = value;
        }
        return;
    }
    if (!(chip_mask & 1u))
        return;

    // fbiInit writes are silently dropped while initEnable[0] is clear —
    // the bit that bites: a model that always accepts appears to work
    // and a model that never accepts never leaves reset [V2 §5.52-5.59].
    if (v2_is_fbiinit(idx) && !(v->init_enable & INITEN_FBIINIT_WR))
        return;

    switch (idx) {
    case R_STATUS:
        return; // read-only on Voodoo2 (was R/W on Voodoo1) [V2 p.29]
    case R_VRETRACE:
    case R_HVRETRACE:
    case R_SWAPHISTORY:
    case R_TRIANGLESOUT:
    case R_PIXELS_IN:
    case R_CHROMA_FAIL:
    case R_ZFUNC_FAIL:
    case R_AFUNC_FAIL:
    case R_PIXELS_OUT:
        return; // read-only counters
    case R_DACDATA:
        v2_dac_access(v, value);
        return;
    case R_CLUTDATA:
        // Ignored while the video unit is in reset [V2 p.75, p.129
        // §12.5]; the gamma table itself is consumed by the display
        // path when it lands.
        if (v->reg[R_FBIINIT1] & FBIINIT1_VIDEO_RESET)
            return;
        v->reg[R_CLUTDATA] = value;
        return;
    case R_NOPCMD:
        // Flush (synchronous: nothing to flush); bit 0 clears the pixel
        // statistics, bit 1 the triangle counter [V2 p.57].
        if (value & 1u) {
            v->reg[R_PIXELS_IN] = v->reg[R_CHROMA_FAIL] = 0;
            v->reg[R_ZFUNC_FAIL] = v->reg[R_AFUNC_FAIL] = 0;
            v->reg[R_PIXELS_OUT] = 0;
        }
        if (value & 2u)
            v->reg[R_TRIANGLESOUT] = 0;
        return;
    case R_SWAPBUFCMD: {
        // Queued like any command; [9] (new on Voodoo2) disables the
        // actual swap [V2 p.58].  Synchronous model: the pending count
        // stands until the next frame boundary retires it.
        v2_retire_swaps(v);
        if (!(value & (1u << 9))) {
            v->swaps_pending++;
            v->swap_issue_frame = v2_frame_number(v);
            // fbiSwapHistory shifts a 4-bit vsync count per swap.
            v->reg[R_SWAPHISTORY] = (v->reg[R_SWAPHISTORY] << 4) | 1u;
        }
        return;
    }
    case R_TRIANGLECMD:
        // Only bit 31 — the area sign — is used [V2 §5.16].
        v2_triangle_cmd(v, (value >> 31) & 1u);
        return;
    case R_FTRIANGLECMD:
        // The IEEE mirror: the float's sign bit is the same bit 31.
        v2_triangle_cmd(v, (value >> 31) & 1u);
        return;
    case R_FASTFILLCMD:
        v->raster->fastfill(v);
        return;
    case R_SDRAWTRICMD:
        v2_setup_vertex(v);
        return;
    case R_SBEGINTRICMD:
        // Begin a new strip at the current vertex; no drawing yet.
        v->sv[0] = v->sv_cur;
        v->sv_count = 1;
        v->sv_flip = false;
        return;
    // The on-chip setup vertex registers assemble sv_cur (V2 §5.69+).
    case 0x99: // sVx
        v->sv_cur.x = v2_f32(value);
        return;
    case 0x9A: // sVy
        v->sv_cur.y = v2_f32(value);
        return;
    case 0x9B: // sARGB: four packed bytes
        v->sv_cur.a = (float)(value >> 24);
        v->sv_cur.r = (float)((value >> 16) & 0xFFu);
        v->sv_cur.g = (float)((value >> 8) & 0xFFu);
        v->sv_cur.b = (float)(value & 0xFFu);
        return;
    case 0x9C: // sRed
        v->sv_cur.r = v2_f32(value);
        return;
    case 0x9D: // sGreen
        v->sv_cur.g = v2_f32(value);
        return;
    case 0x9E: // sBlue
        v->sv_cur.b = v2_f32(value);
        return;
    case 0x9F: // sAlpha
        v->sv_cur.a = v2_f32(value);
        return;
    case 0xA0: // sVz
        v->sv_cur.z = v2_f32(value);
        return;
    case 0xA1: // sWb
        v->sv_cur.wb = v2_f32(value);
        return;
    case 0xA2: // sWtmu0
        v->sv_cur.w0 = v2_f32(value);
        return;
    case 0xA3: // sS/W0
        v->sv_cur.s0 = v2_f32(value);
        return;
    case 0xA4: // sT/W0
        v->sv_cur.t0 = v2_f32(value);
        return;
    case 0xA5: // sWtmu1
        v->sv_cur.w1 = v2_f32(value);
        return;
    case 0xA6: // sS/Wtmu1
        v->sv_cur.s1 = v2_f32(value);
        return;
    case 0xA7: // sT/Wtmu1
        v->sv_cur.t1 = v2_f32(value);
        return;
    case R_CMDFIFO_BUMP:
        // Software-managed depth: the CPU announces N new words and the
        // parser runs (V2 §11.3.1.1).
        v->reg[R_CMDFIFO_DEPTH] += value & 0xFFFFu;
        v2_fifo_execute(v);
        return;
    default:
        break;
    }
    v->reg[idx] = value;
}

// ============================================================
// The CMDFIFO engine (V2 §11, pp.120-127)
// ============================================================
// With fbiInit7[8] set, the second 2 MB of the aperture becomes a
// write-only command port: writes land in a circular buffer in offscreen
// framebuffer memory (cmdFifoBaseAddr's page range), the hardware counts
// them in (with hole counting for out-of-order CPU write buffers, or a
// software-managed bump), and an on-chip parser executes the packet
// stream, advancing cmdFifoRdPtr.  Mac Glide's whole RENDER path runs
// through this engine — grSstWinOpen polls cmdFifoRdPtr for fifo room,
// so a model that never advances it hangs the guest (the idle-contract
// lesson, §12.3, in different clothes).
//
// The execution model is the synchronous one used everywhere else in
// this file: after every fifo write (or bump), all COMPLETE packets are
// parsed and executed immediately, so the read pointer is always caught
// up and depth returns to zero.  Partial packets wait for their tail.

static uint32_t v2_fifo_base_bytes(const voodoo2_t *v) {
    return (v->reg[R_CMDFIFO_BASE] & 0x3FFu) << 12; // pageStart
}
static uint32_t v2_fifo_end_bytes(const voodoo2_t *v) {
    return ((((v->reg[R_CMDFIFO_BASE] >> 16) & 0x3FFu) + 1u) << 12); // pageEnd, inclusive
}

static uint32_t v2_fifo_read32(const voodoo2_t *v, uint32_t addr) {
    addr &= V2_FB_SIZE - 1u;
    return (uint32_t)v->fb_ram[addr] | ((uint32_t)v->fb_ram[(addr + 1) & (V2_FB_SIZE - 1u)] << 8) |
           ((uint32_t)v->fb_ram[(addr + 2) & (V2_FB_SIZE - 1u)] << 16) |
           ((uint32_t)v->fb_ram[(addr + 3) & (V2_FB_SIZE - 1u)] << 24);
}

// Words per vertex of a packet-3 data group, from the parameter mask.
static uint32_t v2_pkt3_vertex_words(uint32_t w0) {
    uint32_t mask = (w0 >> 10) & 0xFFu;
    bool packed = (w0 >> 28) & 1u;
    uint32_t n = 2; // X, Y
    if (mask & 0x01u)
        n += packed ? 1u : 3u; // ARGB packed, or R,G,B
    if ((mask & 0x02u) && !packed)
        n += 1; // separate alpha
    if (mask & 0x04u)
        n += 1; // Z
    if (mask & 0x08u)
        n += 1; // Wb
    if (mask & 0x10u)
        n += 1; // W0
    if (mask & 0x20u)
        n += 2; // S0, T0
    if (mask & 0x40u)
        n += 1; // W1
    if (mask & 0x80u)
        n += 2; // S1, T1
    return n;
}

// Total length in words of the packet whose header is `w0`, or 0 when
// the type is unknown (the stream is broken; the parser stops).
static uint32_t v2_packet_words(uint32_t w0) {
    switch (w0 & 7u) {
    case 0:
        return ((w0 >> 3) & 7u) == 4u ? 2u : 1u; // JMP AGP takes 2 words
    case 1:
        return 1u + (w0 >> 16);
    case 2:
        return 1u + (uint32_t)__builtin_popcount(w0 >> 3);
    case 3:
        return 1u + ((w0 >> 6) & 0xFu) * v2_pkt3_vertex_words(w0) + ((w0 >> 29) & 7u);
    case 4:
        return 1u + (uint32_t)__builtin_popcount((w0 >> 15) & 0x3FFFu) + ((w0 >> 29) & 7u);
    case 5:
        return 2u + ((w0 >> 3) & 0x7FFFFu);
    default:
        return 0;
    }
}

static void v2_bar_write32(void *ctx, uint32_t off, uint32_t data);

// Execute complete packets from the fifo until depth runs dry.
static void v2_fifo_execute(voodoo2_t *v) {
    int guard = 1 << 20; // a bounded parser, never a hung emulator
    while (v->reg[R_CMDFIFO_DEPTH] > 0 && guard-- > 0) {
        uint32_t rd = v->reg[R_CMDFIFO_RDPTR];
        uint32_t w0 = v2_fifo_read32(v, rd);
        uint32_t len = v2_packet_words(w0);
        if (len == 0 || len > v->reg[R_CMDFIFO_DEPTH])
            return; // unknown type or incomplete packet: wait
        uint32_t type = w0 & 7u;
        uint32_t p = rd + 4;
        switch (type) {
        case 0: {
            uint32_t func = (w0 >> 3) & 7u;
            if (func == 3u) { // JMP LOCAL FRAME BUFFER
                v->reg[R_CMDFIFO_DEPTH] -= len;
                v->reg[R_CMDFIFO_RDPTR] = ((w0 >> 6) & 0x7FFFFFu) << 2;
                continue; // depth accounting done; do not fall through
            }
            // NOP (and the JSR/RET/AGP functions nothing on these
            // machines uses) just advance.
            break;
        }
        case 1: {
            uint32_t n = w0 >> 16;
            bool inc = (w0 >> 15) & 1u;
            uint32_t base = (w0 >> 3) & 0xFFFu;
            uint32_t chip = (base >> 8) & 0xFu, regn = base & 0xFFu;
            for (uint32_t i = 0; i < n; i++, p += 4)
                v2_reg_write(v, (int)((regn + (inc ? i : 0u)) & 0xFFu), chip, v2_fifo_read32(v, p));
            break;
        }
        case 2: {
            uint32_t mask = w0 >> 3;
            for (int n = 0; n < 29; n++) {
                if (mask & (1u << n)) {
                    v2_reg_write(v, R_BLT_FIRST + n, 1u, v2_fifo_read32(v, p));
                    p += 4;
                }
            }
            break;
        }
        case 3: {
            // On-chip-setup vertices: write sSetupMode from the header
            // (parameter mask into [11:0], the mode bits into [19:16] —
            // the header's own layout, mapped onto §5.69's fields), then
            // feed vertices through the same begin/draw sequencing the
            // registers use.
            uint32_t smode = ((w0 >> 10) & 0xFFFu) | (((w0 >> 22) & 0xFu) << 16);
            v2_reg_write(v, R_SETUPMODE, 1u, smode);
            uint32_t nvert = (w0 >> 6) & 0xFu;
            uint32_t cmd = (w0 >> 3) & 7u;
            uint32_t mask = (w0 >> 10) & 0xFFu;
            bool packed = (w0 >> 28) & 1u;
            for (uint32_t i = 0; i < nvert; i++) {
                v2_reg_write(v, 0x99, 1u, v2_fifo_read32(v, p)); // sVx
                p += 4;
                v2_reg_write(v, 0x9A, 1u, v2_fifo_read32(v, p)); // sVy
                p += 4;
                if (mask & 0x01u) {
                    if (packed) {
                        v2_reg_write(v, 0x9B, 1u, v2_fifo_read32(v, p));
                        p += 4;
                    } else {
                        for (int c = 0; c < 3; c++, p += 4)
                            v2_reg_write(v, 0x9C + c, 1u, v2_fifo_read32(v, p));
                    }
                }
                if ((mask & 0x02u) && !packed) {
                    v2_reg_write(v, 0x9F, 1u, v2_fifo_read32(v, p));
                    p += 4;
                }
                if (mask & 0x04u) {
                    v2_reg_write(v, 0xA0, 1u, v2_fifo_read32(v, p));
                    p += 4;
                }
                if (mask & 0x08u) {
                    v2_reg_write(v, 0xA1, 1u, v2_fifo_read32(v, p));
                    p += 4;
                }
                if (mask & 0x10u) {
                    v2_reg_write(v, 0xA2, 1u, v2_fifo_read32(v, p));
                    p += 4;
                }
                if (mask & 0x20u) {
                    v2_reg_write(v, 0xA3, 1u, v2_fifo_read32(v, p));
                    p += 4;
                    v2_reg_write(v, 0xA4, 1u, v2_fifo_read32(v, p));
                    p += 4;
                }
                if (mask & 0x40u) {
                    v2_reg_write(v, 0xA5, 1u, v2_fifo_read32(v, p));
                    p += 4;
                }
                if (mask & 0x80u) {
                    v2_reg_write(v, 0xA6, 1u, v2_fifo_read32(v, p));
                    p += 4;
                    v2_reg_write(v, 0xA7, 1u, v2_fifo_read32(v, p));
                    p += 4;
                }
                // The implied command sequencing (V2 p.126): independent
                // triangles B D D | B D D..., a new strip B D D D...,
                // a continued strip D D D...
                bool begin;
                if (cmd == 0u)
                    begin = (i % 3u) == 0u;
                else if (cmd == 1u)
                    begin = i == 0u;
                else
                    begin = false;
                v2_reg_write(v, begin ? R_SBEGINTRICMD : R_SDRAWTRICMD, 1u, 1u);
            }
            break;
        }
        case 4: {
            uint32_t mask = (w0 >> 15) & 0x3FFFu;
            uint32_t base = (w0 >> 3) & 0xFFFu;
            uint32_t chip = (base >> 8) & 0xFu, regn = base & 0xFFu;
            for (int n = 0; n < 14; n++) {
                if (mask & (1u << n)) {
                    v2_reg_write(v, (int)((regn + n) & 0xFFu), chip, v2_fifo_read32(v, p));
                    p += 4;
                }
            }
            break;
        }
        case 5: {
            uint32_t nwords = (w0 >> 3) & 0x7FFFFu;
            uint32_t space = w0 >> 30;
            uint32_t base = (v2_fifo_read32(v, p) & 0x1FFFFFFu) << 2;
            p += 4;
            for (uint32_t i = 0; i < nwords; i++, p += 4) {
                uint32_t d = v2_fifo_read32(v, p);
                if (space == 3u)
                    v2_tex_write(v, (base + 4u * i) & (V2_OFF_TEX - 1u), d);
                else if (space == 2u)
                    v2_bar_write32(v, V2_OFF_LFB + ((base + 4u * i) & (V2_OFF_TEX - V2_OFF_LFB - 1u)), VOODOO2_LE32(d));
            }
            break;
        }
        default:
            break;
        }
        v->reg[R_CMDFIFO_DEPTH] -= len;
        v->reg[R_CMDFIFO_RDPTR] = rd + len * 4u;
        // Never run past the fifo's end page: software always JMPs back
        // before that, so hitting it means a broken stream — park.
        if (v->reg[R_CMDFIFO_RDPTR] >= v2_fifo_end_bytes(v))
            v->reg[R_CMDFIFO_RDPTR] = v2_fifo_base_bytes(v);
    }
}

// One write into the CMDFIFO port: store into the offscreen buffer,
// account it (hole counting per the driver's-eye description on V2
// p.123, or wait for the software bump when fbiInit7[10] disables it),
// and run the parser.
static void v2_cmdfifo_write(voodoo2_t *v, uint32_t off, uint32_t le_value) {
    if (off & (1u << 18))
        le_value = __builtin_bswap32(le_value); // per-access byte swizzle
    uint32_t fb_addr = (v2_fifo_base_bytes(v) + (off & 0x3FFFCu)) & (V2_FB_SIZE - 1u);
    v->fb_ram[fb_addr] = (uint8_t)le_value;
    v->fb_ram[(fb_addr + 1) & (V2_FB_SIZE - 1u)] = (uint8_t)(le_value >> 8);
    v->fb_ram[(fb_addr + 2) & (V2_FB_SIZE - 1u)] = (uint8_t)(le_value >> 16);
    v->fb_ram[(fb_addr + 3) & (V2_FB_SIZE - 1u)] = (uint8_t)(le_value >> 24);
    if (v->reg[R_FBIINIT7] & (1u << 10))
        return; // software-managed: depth moves on cmdFifoBump only
    uint32_t amin = v->reg[R_CMDFIFO_AMIN], amax = v->reg[R_CMDFIFO_AMAX];
    if (fb_addr == amax + 4u || fb_addr == v2_fifo_base_bytes(v)) {
        // In-order (or the explicit wrap back to the base after the
        // guest's JMP packet): the common path.
        v->reg[R_CMDFIFO_AMAX] = v->reg[R_CMDFIFO_AMIN] = fb_addr;
        v->reg[R_CMDFIFO_DEPTH] += 1u;
    } else if (fb_addr > amax + 4u) {
        // A hole opened: count the skipped words, hold aMin back.
        v->reg[R_CMDFIFO_HOLES] += (fb_addr - amax - 4u) >> 2;
        v->reg[R_CMDFIFO_AMAX] = fb_addr;
    } else {
        // A hole filled.
        if (v->reg[R_CMDFIFO_HOLES] > 0)
            v->reg[R_CMDFIFO_HOLES] -= 1u;
        if (v->reg[R_CMDFIFO_HOLES] == 0 && amax > amin) {
            v->reg[R_CMDFIFO_DEPTH] += (amax - amin) >> 2;
            v->reg[R_CMDFIFO_AMIN] = amax;
        }
    }
    v2_fifo_execute(v);
}

// ============================================================
// BAR0 — the aperture, three faces
// ============================================================

// Decode a register-face offset [V2 p.21 §5].  The wrap field (19:14) is
// 64 identical aliases and is deliberately discarded (it exists to
// defeat host write-combining).  Bit 21 selects the alternate mapping
// only if fbiInit3[0]; bit 20 requests the per-access byte swizzle only
// if fbiInit0[3].  The chip field is IGNORED for reads — reads always
// come from Chuck.
static uint32_t v2_reg_face_read(voodoo2_t *v, uint32_t off) {
    int idx = (off >> 2) & 0xFFu;
    if ((off & (1u << 21)) && (v->reg[R_FBIINIT3] & FBIINIT3_ALT_REGMAP)) {
        idx = v2_alt_to_std(idx);
        if (idx < 0)
            return 0;
    }
    uint32_t value = v2_reg_read(v, idx);
    LOG(6, "rd $%03X -> %08X", idx * 4, value);
    if ((off & (1u << 20)) && (v->reg[R_FBIINIT0] & FBIINIT0_SWIZZLE_EN))
        value = __builtin_bswap32(value);
    return value;
}

static void v2_reg_face_write(voodoo2_t *v, uint32_t off, uint32_t le_value) {
    int idx = (off >> 2) & 0xFFu;
    if ((off & (1u << 20)) && (v->reg[R_FBIINIT0] & FBIINIT0_SWIZZLE_EN))
        le_value = __builtin_bswap32(le_value);
    if ((off & (1u << 21)) && (v->reg[R_FBIINIT3] & FBIINIT3_ALT_REGMAP)) {
        idx = v2_alt_to_std(idx);
        if (idx < 0)
            return;
    }
    uint32_t chip_mask = (off >> 10) & 0xFu;
    // The §9.2 instrument: writes to the non-FIFO'd init/video/DAC
    // block at level 4, everything else at level 5 (read the offsets
    // through scripts/voodoo2/voodoo2_regs.py for names).
    LOG(idx >= R_CMDFIFO_BASE && idx < V2_TMU_REG_FIRST ? 4 : 5, "wr $%03X = %08X (chip %X)", idx * 4, le_value,
        chip_mask);
    // With the CMDFIFO map enabled, direct writes outside the permitted
    // init/video/CMDFIFO set are accepted by the PCI slave and silently
    // dropped [V2 p.121].
    if ((v->reg[R_FBIINIT7] & FBIINIT7_CMDFIFO_EN) && idx < R_CMDFIFO_BASE && idx != R_INTRCTRL) {
        if (!v->warned_cmdfifo) {
            v->warned_cmdfifo = true;
            LOG(2, "direct register write $%03X dropped while CMDFIFO mode is enabled", idx * 4);
        }
        return;
    }
    v2_reg_write(v, idx, chip_mask, le_value);
}

// --- the memory_interface_t face -------------------------------------------
// Handlers receive the BAR-relative offset and big-endian bus values
// (MSB = lowest addressed byte); the card swaps into its own domain at
// this edge and nowhere else.

static uint32_t v2_bar_read32(void *ctx, uint32_t off) {
    voodoo2_t *v = (voodoo2_t *)ctx;
    if (off < V2_OFF_LFB) {
        // With the CMDFIFO map, the second 2 MB is the write-only
        // command port; reads from it return undefined data.
        if ((v->reg[R_FBIINIT7] & FBIINIT7_CMDFIFO_EN) && off >= 0x200000u)
            return 0xFFFFFFFFu;
        return VOODOO2_LE32(v2_reg_face_read(v, off));
    }
    if (off < V2_OFF_TEX) {
        // LFB reads are gated by fbiInit1[3], which starts CLEAR so a
        // random powerup read cannot hang the machine [V2 p.68]; they
        // return two 16-bit pixels whatever the write format [V2 p.56],
        // are blocking (automatic in the synchronous model) and read
        // through the authoritative shadow (the seam's invariant 2).
        if (!(v->reg[R_FBIINIT1] & FBIINIT1_LFB_READ_EN))
            return 0;
        v->raster->sync(v);
        uint32_t buffer, x, y;
        v2_lfb_locate(v, off - V2_OFF_LFB, false, &buffer, &x, &y);
        uint16_t p0 = v2_lfb_load16(v, buffer, x, y);
        uint16_t p1 = v2_lfb_load16(v, buffer, x + 1u, y);
        // Colour-lane selection applies to reads of the colour buffers:
        // the BGR orderings exchange the red and blue fields [V2 p.115].
        if (buffer != 3u && (LFB_LANES(v->reg[R_LFBMODE]) & 1u)) {
            p0 = (uint16_t)(((p0 & 0x1Fu) << 11) | (p0 & 0x7E0u) | (p0 >> 11));
            p1 = (uint16_t)(((p1 & 0x1Fu) << 11) | (p1 & 0x7E0u) | (p1 >> 11));
        }
        uint32_t le = (uint32_t)p0 | ((uint32_t)p1 << 16);
        LOG(5, "lfb rd32 +%06X -> %08X", off, v2_lfb_read_transform(v, le));
        return VOODOO2_LE32(v2_lfb_read_transform(v, le));
    }
    // Texture memory is write-only; reads return undefined data
    // [V2 p.119] — deterministically all-ones here, logged once.
    if (!v->warned_tex_read) {
        v->warned_tex_read = true;
        LOG(3, "read from write-only texture aperture at +$%06X", off);
    }
    return 0xFFFFFFFFu;
}

static void v2_bar_write32(void *ctx, uint32_t off, uint32_t data) {
    voodoo2_t *v = (voodoo2_t *)ctx;
    uint32_t le = VOODOO2_LE32(data);
    if (off < V2_OFF_LFB) {
        if ((v->reg[R_FBIINIT7] & FBIINIT7_CMDFIFO_EN) && off >= 0x200000u)
            v2_cmdfifo_write(v, off - 0x200000u, le);
        else
            v2_reg_face_write(v, off, le);
        return;
    }
    if (off < V2_OFF_TEX) {
        uint32_t mode = v->reg[R_LFBMODE];
        uint32_t fmt = LFB_FMT(mode);
        uint32_t buffer, x, y;
        v2_lfb_locate(v, off - V2_OFF_LFB, true, &buffer, &x, &y);
        uint32_t t = v2_lfb_write_transform(v, le);
        LOG(5, "lfb wr32 +%06X = %08X (lfbMode %05X buf %u x %u y %u)", off, le, mode, buffer, x, y);
        uint32_t r, g, b, a;
        switch (fmt) {
        case 0:
        case 1:
        case 2: { // two 16-bit colour pixels: left in the low half
            bool ha = v2_lfb_expand16(fmt, LFB_LANES(mode), (uint16_t)t, &r, &g, &b, &a);
            v2_lfb_pixel(v, buffer, x, y, r, g, b, ha ? a : (v->reg[R_ZACOLOR] >> 24), false, 0, true, false);
            ha = v2_lfb_expand16(fmt, LFB_LANES(mode), (uint16_t)(t >> 16), &r, &g, &b, &a);
            v2_lfb_pixel(v, buffer, x + 1u, y, r, g, b, ha ? a : (v->reg[R_ZACOLOR] >> 24), false, 0, true, false);
            return;
        }
        case 4:
        case 5: { // one 24/32-bit pixel; lanes reorder the byte channels
            uint32_t lanes = LFB_LANES(mode);
            if (lanes == 0u || fmt == 4u) { // ARGB (format 4 carries no A)
                a = fmt == 5u ? t >> 24 : (v->reg[R_ZACOLOR] >> 24);
                r = (t >> 16) & 0xFFu;
                g = (t >> 8) & 0xFFu;
                b = t & 0xFFu;
            } else if (lanes == 1u) { // ABGR
                a = t >> 24;
                b = (t >> 16) & 0xFFu;
                g = (t >> 8) & 0xFFu;
                r = t & 0xFFu;
            } else if (lanes == 2u) { // RGBA
                r = t >> 24;
                g = (t >> 16) & 0xFFu;
                b = (t >> 8) & 0xFFu;
                a = t & 0xFFu;
            } else { // BGRA
                b = t >> 24;
                g = (t >> 16) & 0xFFu;
                r = (t >> 8) & 0xFFu;
                a = t & 0xFFu;
            }
            v2_lfb_pixel(v, buffer, x, y, r, g, b, a, false, 0, true, false);
            return;
        }
        case 12:
        case 13:
        case 14: { // 16-bit depth + 16-bit colour (Z high after transform)
            bool ha = v2_lfb_expand16(fmt, LFB_LANES(mode), (uint16_t)t, &r, &g, &b, &a);
            v2_lfb_pixel(v, buffer, x, y, r, g, b, ha ? a : (v->reg[R_ZACOLOR] >> 24), true, (uint16_t)(t >> 16), true,
                         true);
            return;
        }
        case LFB_FMT_ZZ: // two depth values into the aux buffer
            v2_lfb_store16(v, 3u, x, y, (uint16_t)t);
            v2_lfb_store16(v, 3u, x + 1u, y, (uint16_t)(t >> 16));
            return;
        default: // reserved formats: the write vanishes
            return;
        }
    }
    v2_tex_write(v, off - V2_OFF_TEX, le);
}

static uint16_t v2_bar_read16(void *ctx, uint32_t off) {
    voodoo2_t *v = (voodoo2_t *)ctx;
    if (off < V2_OFF_LFB) {
        // Register accesses must be 32-bit [V2 p.22, p.128 §12.2]; a
        // narrower access is not a bus behaviour the spec defines, so it
        // is logged once and answers all-ones rather than inventing one.
        if (!v->warned_narrow_reg) {
            v->warned_narrow_reg = true;
            LOG(3, "narrow access to the register face at +$%06X — registers are 32-bit only", off);
        }
        return 0xFFFFu;
    }
    if (off < V2_OFF_TEX) {
        if (!(v->reg[R_FBIINIT1] & FBIINIT1_LFB_READ_EN))
            return 0;
        v->raster->sync(v);
        uint32_t buffer, x, y;
        v2_lfb_locate(v, off - V2_OFF_LFB, false, &buffer, &x, &y);
        uint16_t p = v2_lfb_load16(v, buffer, x, y);
        if (buffer != 3u && (LFB_LANES(v->reg[R_LFBMODE]) & 1u))
            p = (uint16_t)(((p & 0x1Fu) << 11) | (p & 0x7E0u) | (p >> 11));
        LOG(5, "lfb rd16 +%06X -> %04X (buf %u x %u y %u)", off, p, buffer, x, y);
        return VOODOO2_LE16(p);
    }
    if (!v->warned_tex_read) {
        v->warned_tex_read = true;
        LOG(3, "read from write-only texture aperture at +$%06X", off);
    }
    return 0xFFFFu;
}

static void v2_bar_write16(void *ctx, uint32_t off, uint16_t data) {
    voodoo2_t *v = (voodoo2_t *)ctx;
    if (off < V2_OFF_LFB) {
        if (!v->warned_narrow_reg) {
            v->warned_narrow_reg = true;
            LOG(3, "narrow access to the register face at +$%06X — registers are 32-bit only", off);
        }
        return;
    }
    if (off < V2_OFF_TEX) {
        // A single 16-bit datum; the Glide memory-sizing probes drive
        // exactly this path [3dfx-src fbiMemSize].
        LOG(5, "lfb wr16 +%06X = %04X (lfbMode %05X)", off, data, v->reg[R_LFBMODE]);
        uint32_t mode = v->reg[R_LFBMODE];
        uint32_t fmt = LFB_FMT(mode);
        uint32_t buffer, x, y;
        v2_lfb_locate(v, off - V2_OFF_LFB, true, &buffer, &x, &y);
        uint16_t d = VOODOO2_LE16(data);
        if (LFB_WR_SWIZZLE(mode))
            d = (uint16_t)((d >> 8) | (d << 8));
        if (fmt <= 2u) {
            uint32_t r, g, b, a;
            bool ha = v2_lfb_expand16(fmt, LFB_LANES(mode), d, &r, &g, &b, &a);
            v2_lfb_pixel(v, buffer, x, y, r, g, b, ha ? a : (v->reg[R_ZACOLOR] >> 24), false, 0, true, false);
        } else if (fmt == LFB_FMT_ZZ) {
            v2_lfb_store16(v, 3u, x, y, d);
        } else {
            // A halfword into a 32-bit format is not a defined bus
            // shape; store raw into the write buffer (chosen).
            v2_lfb_store16(v, buffer, x, y, d);
        }
        return;
    }
    // Texture writes are dword transactions on real hardware; a halfword
    // is folded into the addressed half.
    uint32_t at = off - V2_OFF_TEX;
    int tmu = (at >> 21) & 1u;
    uint32_t masked = (at & 0x1FFFFEu) & (v2_tmu_addressable(v, tmu) - 1u);
    uint16_t le = VOODOO2_LE16(data);
    v->tex_ram[tmu][masked] = (uint8_t)le;
    v->tex_ram[tmu][masked + 1] = (uint8_t)(le >> 8);
}

static uint8_t v2_bar_read8(void *ctx, uint32_t off) {
    voodoo2_t *v = (voodoo2_t *)ctx;
    if (off < V2_OFF_LFB) {
        if (!v->warned_narrow_reg) {
            v->warned_narrow_reg = true;
            LOG(3, "narrow access to the register face at +$%06X — registers are 32-bit only", off);
        }
        return 0xFFu;
    }
    if (off < V2_OFF_TEX) {
        if (!(v->reg[R_FBIINIT1] & FBIINIT1_LFB_READ_EN))
            return 0;
        v->raster->sync(v);
        uint32_t buffer, x, y;
        v2_lfb_locate(v, (off - V2_OFF_LFB) & ~1u, false, &buffer, &x, &y);
        uint16_t px = v2_lfb_load16(v, buffer, x, y);
        return (off & 1u) ? (uint8_t)(px >> 8) : (uint8_t)px;
    }
    return 0xFFu;
}

static void v2_bar_write8(void *ctx, uint32_t off, uint8_t data) {
    voodoo2_t *v = (voodoo2_t *)ctx;
    if (off < V2_OFF_LFB) {
        if (!v->warned_narrow_reg) {
            v->warned_narrow_reg = true;
            LOG(3, "narrow access to the register face at +$%06X — registers are 32-bit only", off);
        }
        return;
    }
    if (off < V2_OFF_TEX) {
        uint32_t buffer, x, y;
        v2_lfb_locate(v, (off - V2_OFF_LFB) & ~1u, true, &buffer, &x, &y);
        uint32_t at = v2_buffer_addr(v, buffer, x, y);
        v->fb_ram[(at + (off & 1u)) & (V2_FB_SIZE - 1u)] = data;
        return;
    }
    uint32_t at = off - V2_OFF_TEX;
    int tmu = (at >> 21) & 1u;
    v->tex_ram[tmu][(at & 0x1FFFFFu) & (v2_tmu_addressable(v, tmu) - 1u)] = data;
}

// ============================================================
// Vendor configuration space ($40-$57)
// ============================================================
// The card claims exactly its vendor block and falls through everywhere
// else, so the generic header keeps answering the standard registers —
// including the subsystem IDs at $2C, which MUST read zero: a nonzero
// subsystem renames the generated OF node after the subsystem vendor and
// the Glide library's match silently misses [Apple-doc p.100].  The
// generic layer's unimplemented-register behaviour (reads zero, writes
// vanish) is also exactly what the undocumented $C0/$E0 pokes that
// sstfb makes need — they must not fault [GPL-src; both specs call
// $58-$FF reserved].

static bool v2_cfg_read(pci_device_t *dev, uint32_t reg, uint32_t *out) {
    voodoo2_t *v = (voodoo2_t *)dev->priv;
    switch (reg) {
    case CFG_INIT_ENABLE:
        *out = (v->init_enable & ~INITEN_RO_MASK) | (V2_CHIP_REVISION << 12) | (V2_CHIP_FAB << 16);
        return true;
    case CFG_BUS_SNOOP0:
    case CFG_BUS_SNOOP1:
        *out = 0; // write-only, read as zero [V2 p.107 §6.17]
        return true;
    case CFG_STATUS:
        // An alias of the memory-mapped status register — readable
        // before the aperture is mapped [V2 p.107 §6.18].
        *out = v2_status(v);
        return true;
    case CFG_SCRATCH:
        *out = v->cfg_scratch;
        return true;
    case CFG_SIPROCESS:
        // Manufacturing telemetry; read-as-written is sufficient.
        *out = v->si_process;
        return true;
    default:
        return false;
    }
}

static bool v2_cfg_write(pci_device_t *dev, uint32_t reg, uint32_t byte, uint8_t value) {
    voodoo2_t *v = (voodoo2_t *)dev->priv;
    uint32_t shift = 8u * byte, mask = 0xFFu << shift;
    switch (reg) {
    case CFG_INIT_ENABLE:
        v->init_enable = (v->init_enable & ~mask) | ((uint32_t)value << shift);
        return true;
    case CFG_BUS_SNOOP0:
        v->bus_snoop[0] = (v->bus_snoop[0] & ~mask) | ((uint32_t)value << shift);
        return true;
    case CFG_BUS_SNOOP1:
        v->bus_snoop[1] = (v->bus_snoop[1] & ~mask) | ((uint32_t)value << shift);
        return true;
    case CFG_STATUS:
        return true; // the alias is read-only
    case CFG_SCRATCH:
        v->cfg_scratch = (v->cfg_scratch & ~mask) | ((uint32_t)value << shift);
        return true;
    case CFG_SIPROCESS:
        v->si_process = (v->si_process & ~mask) | ((uint32_t)value << shift);
        return true;
    default:
        if (reg >= 0x58u && !v->warned_cfg_unknown) {
            v->warned_cfg_unknown = true;
            LOG(4, "write to reserved config offset $%02X ignored (sstfb pokes $C0/$E0)", reg);
        }
        return false; // generic behaviour: the write vanishes, no fault
    }
}

// ============================================================
// Lifecycle
// ============================================================

static void v2_reset(pci_device_t *dev, config_t *cfg) {
    (void)cfg;
    voodoo2_t *v = (voodoo2_t *)dev->priv;
    memset(v->reg, 0, sizeof(v->reg));
    memset(v->tmu_reg, 0, sizeof(v->tmu_reg));
    memset(v->tmu_param, 0, sizeof(v->tmu_param));
    memset(v->ncc, 0, sizeof(v->ncc));
    memset(v->palette, 0, sizeof(v->palette));
    memset(&v->sv_cur, 0, sizeof(v->sv_cur));
    memset(v->sv, 0, sizeof(v->sv));
    v->sv_count = 0;
    v->sv_flip = false;
    // Strapped power-on values [V2 pp.67-73].  fbiInit0[0]'s reset value
    // comes from the fb_addr_a[4] strap, modelled as 0: the board powers
    // on PASSING THROUGH, handing the monitor to the 2D card — which is
    // also what busSnoop-based reset protection exists to guarantee.
    v->reg[R_FBIINIT0] = FBIINIT0_RESET;
    v->reg[R_FBIINIT1] = FBIINIT1_RESET;
    v->reg[R_FBIINIT2] = FBIINIT2_RESET;
    v->reg[R_FBIINIT3] = FBIINIT3_RESET;
    v->reg[R_FBIINIT4] = FBIINIT4_RESET;
    v->reg[R_FBIINIT7] = FBIINIT7_RESET;
    v->init_enable = 0;
    v->bus_snoop[0] = v->bus_snoop[1] = 0;
    v->cfg_scratch = 0;
    v->si_process = 0;
    v->swaps_pending = 0;
    v->swap_issue_frame = 0;
    v->displayed_buffer = 0;
    // PCI RST# hands the monitor back: fbiInit0[0] returns to its
    // pass-through strap, so the predicate reads false and Control (or
    // whatever else) is the display again.
    v->driving = false;
    v->display.shape_dirty = true;
    v2_dac_reset(v);
}

static void v2_teardown(pci_device_t *dev, config_t *cfg) {
    (void)cfg;
    voodoo2_t *v = (voodoo2_t *)dev->priv;
    if (!v)
        return;
    free(v->fb_ram);
    free(v->scanout);
    for (int t = 0; t < V2_NUM_TMUS; t++)
        free(v->tex_ram[t]);
    free(v);
    dev->priv = NULL;
}

static const char *v2_name(const pci_device_t *dev) {
    (void)dev;
    return "3dfx Voodoo2";
}

// ============================================================
// The pass-through switch and the display face (milestone 3d)
// ============================================================

static uint32_t v2_screen_width(const voodoo2_t *v) {
    // videoDimensions packs (x-1) in the low field [V2 §5.47].
    uint32_t w = (v->reg[R_VIDEODIM] & 0x7FFu) + 1u;
    return (w > 1u && w <= 1024u) ? w : 640u;
}

// Does the card drive the monitor this frame?  fbiInit0[0] is the
// vga_pass/vga_pass_n control (V2 p.67 §5.52); fbiInit6[29:28] can
// override the pin outright on Voodoo2 (V2 p.73).  A card in video
// reset, or with software blanking set (fbiInit1[12], which DEFAULTS to
// blank), or with its output enables tristated (fbiInit1[16:13]), is
// not driving anything whatever the pass-through bit says.
//
// Convention: 1 = the Voodoo drives the monitor.  The single bit drives
// two complementary pins, so a convention has to be chosen; this is the
// driver's (sstfb calls the bit DIS_VGA_PASSTHROUGH and SETS it to take
// the display).  The fbiInit6 override maps accordingly: forcing
// vga_pass_n low (2) drives, forcing it high (3) passes through.
static bool v2_drives_monitor(const voodoo2_t *v) {
    uint32_t ovr = (v->reg[R_FBIINIT6] >> 28) & 3u;
    if (ovr == 2u)
        return true;
    if (ovr == 3u)
        return false;
    if (!(v->reg[R_FBIINIT0] & FBIINIT0_VGA_PASS))
        return false;
    if (v->reg[R_FBIINIT1] & FBIINIT1_VIDEO_RESET)
        return false;
    if (v->reg[R_FBIINIT1] & FBIINIT1_SW_BLANK)
        return false;
    if ((v->reg[R_FBIINIT1] & FBIINIT1_OUT_ENABLES) != FBIINIT1_OUT_ENABLES)
        return false;
    return true;
}

// Re-derive the descriptor and convert the displayed buffer into the
// big-endian scanout raster the display layer consumes.  The internal
// 33-entry gamma CLUT (clutData) is not applied — identity scanout, a
// chosen simplification recorded in voodoo2.md.
static void v2_display_update(voodoo2_t *v) {
    uint32_t w = v2_screen_width(v), h = v2_screen_height(v);
    if (v->display.width != w || v->display.height != h || v->display.stride != w * 2u) {
        v->display.width = w;
        v->display.height = h;
        v->display.stride = w * 2u;
        v->display.shape_dirty = true;
    }
    v->display.format = PIXEL_16BPP_565;
    v->display.bits = v->scanout;
    v->display.clut = NULL;
    v->display.clut_len = 0;
    v->display.crt_response = NULL;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t src = v2_buffer_addr(v, 0u, 0u, y); // front = displayed
        uint8_t *dst = v->scanout + (size_t)y * w * 2u;
        for (uint32_t x = 0; x < w; x++) {
            // Little-endian card domain -> the display layer's
            // big-endian 5-6-5.
            uint32_t at = (src + 2u * x) & (V2_FB_SIZE - 1u);
            dst[2 * x] = v->fb_ram[(at + 1u) & (V2_FB_SIZE - 1u)];
            dst[2 * x + 1] = v->fb_ram[at];
        }
    }
    v->display.fb_dirty = true;
}

// The pass-through contract (§3.2): re-resolved every frame by
// pci_primary_display_card(), which calls this as its test.  Returning
// NULL yields the monitor to the 2D card; the ONE obligation on the
// card is to flag shape_dirty on BOTH edges of the switch, because two
// sources of different geometry share one screen texture.
static display_t *v2_display(pci_device_t *dev) {
    voodoo2_t *v = (voodoo2_t *)dev->priv;
    bool drives = v2_drives_monitor(v);
    if (drives != v->driving) {
        v->driving = drives;
        v->display.shape_dirty = true;
        if (drives)
            v2_display_update(v);
    }
    return drives ? &v->display : NULL;
}

// Per-frame housekeeping while driving: retire due swaps and re-convert
// the scanout (the guest may have drawn into the displayed buffer at
// any time, so the raster is re-presented each frame).
static void v2_on_vbl(pci_device_t *dev, config_t *cfg) {
    (void)cfg;
    voodoo2_t *v = (voodoo2_t *)dev->priv;
    v2_retire_swaps(v);
    if (v->driving)
        v2_display_update(v);
}

// ============================================================
// Checkpoints — POD struct, then the memories as tail blobs
// ============================================================

typedef struct v2_ckpt {
    uint32_t reg[V2_NUM_REGS];
    uint32_t tmu_reg[V2_NUM_TMUS][64];
    uint32_t tmu_param[V2_NUM_TMUS][0x20];
    uint32_t ncc[V2_NUM_TMUS][2][12];
    uint32_t palette[V2_NUM_TMUS][256];
    float sv_cur[14], sv[3][14];
    int32_t sv_count;
    uint8_t sv_flip;
    uint8_t pad2[3];
    uint32_t init_enable, bus_snoop[2], cfg_scratch, si_process;
    uint8_t dac_direct[8];
    uint8_t dac_pll[16][2];
    uint8_t dac_pll_wr_addr, dac_pll_rd_addr, dac_pll_wr_phase, dac_pll_rd_phase;
    uint8_t dac_read_latch;
    uint8_t pad[3];
    uint32_t swaps_pending;
    uint64_t swap_issue_frame;
    uint32_t displayed_buffer;
    uint8_t driving;
    uint8_t pad3[3];
    uint32_t tex_size;
} v2_ckpt_t;

static void v2_checkpoint_save(pci_device_t *dev, checkpoint_t *cp) {
    voodoo2_t *v = (voodoo2_t *)dev->priv;
    v2_ckpt_t c;
    memset(&c, 0, sizeof(c));
    memcpy(c.reg, v->reg, sizeof(c.reg));
    memcpy(c.tmu_reg, v->tmu_reg, sizeof(c.tmu_reg));
    memcpy(c.tmu_param, v->tmu_param, sizeof(c.tmu_param));
    memcpy(c.ncc, v->ncc, sizeof(c.ncc));
    memcpy(c.palette, v->palette, sizeof(c.palette));
    memcpy(c.sv_cur, &v->sv_cur, sizeof(c.sv_cur));
    memcpy(c.sv, v->sv, sizeof(c.sv));
    c.sv_count = v->sv_count;
    c.sv_flip = v->sv_flip ? 1u : 0u;
    c.init_enable = v->init_enable;
    c.bus_snoop[0] = v->bus_snoop[0];
    c.bus_snoop[1] = v->bus_snoop[1];
    c.cfg_scratch = v->cfg_scratch;
    c.si_process = v->si_process;
    memcpy(c.dac_direct, v->dac_direct, sizeof(c.dac_direct));
    memcpy(c.dac_pll, v->dac_pll, sizeof(c.dac_pll));
    c.dac_pll_wr_addr = v->dac_pll_wr_addr;
    c.dac_pll_rd_addr = v->dac_pll_rd_addr;
    c.dac_pll_wr_phase = v->dac_pll_wr_phase;
    c.dac_pll_rd_phase = v->dac_pll_rd_phase;
    c.dac_read_latch = v->dac_read_latch;
    c.swaps_pending = v->swaps_pending;
    c.swap_issue_frame = v->swap_issue_frame;
    c.displayed_buffer = v->displayed_buffer;
    c.driving = v->driving ? 1u : 0u;
    c.tex_size = v->tex_size;
    system_write_checkpoint_data(cp, &c, sizeof(c));
    system_write_checkpoint_data(cp, v->fb_ram, V2_FB_SIZE);
    for (int t = 0; t < V2_NUM_TMUS; t++)
        system_write_checkpoint_data(cp, v->tex_ram[t], v->tex_size);
}

static void v2_checkpoint_restore(pci_device_t *dev, checkpoint_t *cp) {
    voodoo2_t *v = (voodoo2_t *)dev->priv;
    v2_ckpt_t c;
    system_read_checkpoint_data(cp, &c, sizeof(c));
    memcpy(v->reg, c.reg, sizeof(v->reg));
    memcpy(v->tmu_reg, c.tmu_reg, sizeof(v->tmu_reg));
    memcpy(v->tmu_param, c.tmu_param, sizeof(v->tmu_param));
    memcpy(v->ncc, c.ncc, sizeof(v->ncc));
    memcpy(v->palette, c.palette, sizeof(v->palette));
    memcpy(&v->sv_cur, c.sv_cur, sizeof(v->sv_cur));
    memcpy(v->sv, c.sv, sizeof(v->sv));
    v->sv_count = c.sv_count;
    v->sv_flip = c.sv_flip != 0;
    v->init_enable = c.init_enable;
    v->bus_snoop[0] = c.bus_snoop[0];
    v->bus_snoop[1] = c.bus_snoop[1];
    v->cfg_scratch = c.cfg_scratch;
    v->si_process = c.si_process;
    memcpy(v->dac_direct, c.dac_direct, sizeof(v->dac_direct));
    memcpy(v->dac_pll, c.dac_pll, sizeof(v->dac_pll));
    v->dac_pll_wr_addr = c.dac_pll_wr_addr;
    v->dac_pll_rd_addr = c.dac_pll_rd_addr;
    v->dac_pll_wr_phase = c.dac_pll_wr_phase;
    v->dac_pll_rd_phase = c.dac_pll_rd_phase;
    v->dac_read_latch = c.dac_read_latch;
    v->swaps_pending = c.swaps_pending;
    v->swap_issue_frame = c.swap_issue_frame;
    // A checkpoint from a 12 MB board must not restore into 8 MB
    // buffers: resize rather than truncate (the mach64gx.c pattern).
    if (c.tex_size != v->tex_size) {
        for (int t = 0; t < V2_NUM_TMUS; t++) {
            uint8_t *grown = (uint8_t *)calloc(1, c.tex_size);
            if (grown) {
                free(v->tex_ram[t]);
                v->tex_ram[t] = grown;
            }
        }
        v->tex_size = c.tex_size;
    }
    v->displayed_buffer = c.displayed_buffer;
    v->driving = c.driving != 0;
    system_read_checkpoint_data(cp, v->fb_ram, V2_FB_SIZE);
    for (int t = 0; t < V2_NUM_TMUS; t++)
        system_read_checkpoint_data(cp, v->tex_ram[t], v->tex_size);
    // The scanout raster is derived state: rebuild it and flag both
    // dirty bits so the renderer re-uploads whatever the restore shows.
    v->display.shape_dirty = true;
    if (v->driving)
        v2_display_update(v);
}

static const pci_device_ops_t v2_ops = {
    .teardown = v2_teardown,
    .reset = v2_reset,
    .name = v2_name,
    .display = v2_display,
    .on_vbl = v2_on_vbl,
    .cfg_read = v2_cfg_read,
    .cfg_write = v2_cfg_write,
    .checkpoint_save = v2_checkpoint_save,
    .checkpoint_restore = v2_checkpoint_restore,
};

// ============================================================
// Config declaration
// ============================================================

// The Voodoo2's PCI face.  One BAR, 16 MB, prefetchable memory; no I/O
// BAR ("All I/O accesses to Voodoo2 Graphics are ignored", V2 p.128
// §12.1); no expansion ROM (rom_size 0 — the card is claimed from disk
// by PCI ID, Apple printed p.164).  Subsystem IDs at $2C read zero from
// the generic header — DO NOT "fill them in": a nonzero subsystem
// renames the guest's node and the driver match silently misses
// [Apple-doc p.100].
static const pci_config_decl_t v2_decl = {
    .vendor_id = V2_VENDOR_ID,
    .device_id = V2_DEVICE_ID,
    .revision = V2_REVISION,
    .class_code = V2_CLASS,
    .header_type = 0x00,
    .interrupt_pin = 0x01, // INTA#; declared sane, polled in practice —
                           // no Glide build installs an interrupt handler
    .command_writable = PCI_CMD_MEM_SPACE, // bit 1 is the ONLY writable
                                           // Command bit [V2 p.103 §6.3]
    .command_reset = 0x0000, // Apple clears Memory Space Enable before
                             // the OS loads; the driver sets it itself
                             // through ExpMgrConfigWrite [Apple-doc p.97]
    .status_reset = 0x0200, // medium DEVSEL
    .bar[0] = {.size = V2_BAR_SIZE, .kind = PCI_BAR_MEM_PREFETCH},
    .rom_size = 0, // no expansion ROM at all
};

// ============================================================
// Object model
// ============================================================

static voodoo2_t *node_card(struct object *self) {
    return (voodoo2_t *)object_data(self);
}

static value_t regs_attr_status(struct object *self, const member_t *m) {
    (void)m;
    voodoo2_t *v = node_card(self);
    return val_uint(4, v ? v2_status(v) : 0);
}
static value_t regs_attr_init_enable(struct object *self, const member_t *m) {
    (void)m;
    voodoo2_t *v = node_card(self);
    if (!v)
        return val_uint(4, 0);
    return val_uint(4, (v->init_enable & ~INITEN_RO_MASK) | (V2_CHIP_REVISION << 12) | (V2_CHIP_FAB << 16));
}
static value_t regs_attr_fbiinit0(struct object *self, const member_t *m) {
    (void)m;
    voodoo2_t *v = node_card(self);
    return val_uint(4, v ? v->reg[R_FBIINIT0] : 0);
}
static value_t regs_attr_fbiinit1(struct object *self, const member_t *m) {
    (void)m;
    voodoo2_t *v = node_card(self);
    return val_uint(4, v ? v->reg[R_FBIINIT1] : 0);
}
static value_t regs_attr_fbiinit2(struct object *self, const member_t *m) {
    (void)m;
    voodoo2_t *v = node_card(self);
    return val_uint(4, v ? v->reg[R_FBIINIT2] : 0);
}
static value_t regs_attr_fbiinit3(struct object *self, const member_t *m) {
    (void)m;
    voodoo2_t *v = node_card(self);
    return val_uint(4, v ? v->reg[R_FBIINIT3] : 0);
}
static value_t regs_attr_fbiinit7(struct object *self, const member_t *m) {
    (void)m;
    voodoo2_t *v = node_card(self);
    return val_uint(4, v ? v->reg[R_FBIINIT7] : 0);
}
static value_t regs_attr_fb_size(struct object *self, const member_t *m) {
    (void)m;
    (void)self;
    return val_uint(4, V2_FB_SIZE);
}
static value_t regs_attr_tmu_size(struct object *self, const member_t *m) {
    (void)m;
    voodoo2_t *v = node_card(self);
    return val_uint(4, v ? v->tex_size : 0);
}

static const arg_decl_t regs_tex_offset_args[] = {
    {.name = "tmu", .kind = V_INT, .doc = "Which Bruce (0 or 1)"},
    {.name = "lod", .kind = V_INT, .doc = "LOD level (0-8)"     },
};
static value_t regs_method_tex_offset(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    voodoo2_t *v = node_card(self);
    int64_t tmu = argv[0].i, lod = argv[1].i;
    if (!v || tmu < 0 || tmu >= V2_NUM_TMUS || lod < 0 || lod > 8)
        return val_err("regs.tex_offset: tmu 0..1, lod 0..8");
    return val_uint(4, v2_lod_offset(v, (int)tmu, (int)lod));
}

static const arg_decl_t regs_read_arg[] = {
    {.name = "offset", .kind = V_INT, .doc = "Register byte offset ($000-$3FC, V2 spec pp.22-26)"},
};
static value_t regs_method_read(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    voodoo2_t *v = node_card(self);
    int64_t off = argv[0].i;
    if (!v || off < 0 || off > 0x3FC)
        return val_err("regs.read: offset must be $000..$3FC");
    return val_uint(4, v2_reg_read(v, (int)(off >> 2)));
}

static const member_t regs_members[] = {
    {.kind = M_ATTR,
     .name = "status",
     .doc = "status ($000): FIFO free space, retrace, busy bits, swaps pending",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = regs_attr_status}},
    {.kind = M_ATTR,
     .name = "init_enable",
     .doc = "initEnable (config $40): fbiInit gate, FIFO gate, DAC remap, real revision in 15:12",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = regs_attr_init_enable}},
    {.kind = M_ATTR,
     .name = "fbi_init0",
     .doc = "fbiInit0: bit 0 VGA pass-through, bit 3 register swizzle enable, resets",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = regs_attr_fbiinit0}},
    {.kind = M_ATTR,
     .name = "fbi_init1",
     .doc = "fbiInit1: video reset, LFB read enable, blanking, output enables",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = regs_attr_fbiinit1}},
    {.kind = M_ATTR,
     .name = "fbi_init2",
     .doc = "fbiInit2: DRAM control, buffer offset, refresh (DAC data while remapped)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = regs_attr_fbiinit2}},
    {.kind = M_ATTR,
     .name = "fbi_init3",
     .doc = "fbiInit3: bit 0 alternate register mapping, texture disable",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = regs_attr_fbiinit3}},
    {.kind = M_ATTR,
     .name = "fbi_init7",
     .doc = "fbiInit7: bit 8 CMDFIFO enable; 7:0 the graphics-clock strap byte",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = regs_attr_fbiinit7}},
    {.kind = M_ATTR,
     .name = "fb_size",
     .doc = "Framebuffer memory in bytes (4 MB on every retail SKU)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = regs_attr_fb_size}},
    {.kind = M_ATTR,
     .name = "tmu_size",
     .doc = "Texture memory per TMU in bytes (the 8/12 MB SKU choice)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = regs_attr_tmu_size}},
    {.kind = M_METHOD,
     .name = "read",
     .doc = "Read any Chuck register by its byte offset",
     .method = {.args = regs_read_arg, .nargs = 1, .result = V_UINT, .fn = regs_method_read}},
    {.kind = M_METHOD,
     .name = "tex_offset",
     .doc = "Byte offset of a LOD level in the packed mip chain, per the TMU's live tLOD "
            "(the V2 p.118 size-table arithmetic; the spec's worked examples pin it)", .method = {.args = regs_tex_offset_args, .nargs = 2, .result = V_UINT, .fn = regs_method_tex_offset}},
};

static const class_desc_t v2_regs_class = {
    .name = "voodoo2_regs",
    .members = regs_members,
    .n_members = sizeof(regs_members) / sizeof(regs_members[0]),
};

static value_t dac_attr_video_khz(struct object *self, const member_t *m) {
    (void)m;
    voodoo2_t *v = node_card(self);
    return val_uint(4, v ? v2_video_pll_khz(v) : 0);
}
static value_t dac_attr_graphics_khz(struct object *self, const member_t *m) {
    (void)m;
    voodoo2_t *v = node_card(self);
    return val_uint(4, v ? v2_graphics_pll_khz(v) : 0);
}
static value_t dac_attr_read_latch(struct object *self, const member_t *m) {
    (void)m;
    voodoo2_t *v = node_card(self);
    return val_uint(4, v ? v->dac_read_latch : 0);
}

static const member_t dac_members[] = {
    {.kind = M_ATTR,
     .name = "video_khz",
     .doc = "CLK0 (video) PLL output, from the ICS5342's programmed M/N/P",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = dac_attr_video_khz}                                },
    {.kind = M_ATTR,
     .name = "graphics_khz",
     .doc = "CLK1 (graphics) PLL output",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = dac_attr_graphics_khz}                             },
    {.kind = M_ATTR,
     .name = "read_latch",
     .doc = "Last byte read from the DAC (what a remapped fbiInit2 returns)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = dac_attr_read_latch}},
};

static const class_desc_t v2_dac_class = {
    .name = "voodoo2_dac",
    .members = dac_members,
    .n_members = sizeof(dac_members) / sizeof(dac_members[0]),
};

static value_t fb_attr_width(struct object *self, const member_t *m) {
    (void)m;
    voodoo2_t *v = node_card(self);
    return val_uint(4, v ? v2_screen_width(v) : 0);
}
static value_t fb_attr_height(struct object *self, const member_t *m) {
    (void)m;
    voodoo2_t *v = node_card(self);
    return val_uint(4, v ? v2_screen_height(v) : 0);
}
static value_t fb_attr_depth(struct object *self, const member_t *m) {
    (void)m;
    (void)self;
    return val_uint(4, 16); // the framebuffer is natively 5-6-5
}
static value_t fb_attr_stride(struct object *self, const member_t *m) {
    (void)m;
    voodoo2_t *v = node_card(self);
    return val_uint(4, v ? v2_screen_width(v) * 2u : 0);
}
static value_t fb_attr_displayed(struct object *self, const member_t *m) {
    (void)m;
    voodoo2_t *v = node_card(self);
    return val_uint(4, v ? v->displayed_buffer : 0);
}

static const member_t fb_members[] = {
    {.kind = M_ATTR,
     .name = "width",
     .doc = "Active raster width in pixels (videoDimensions)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = fb_attr_width}    },
    {.kind = M_ATTR,
     .name = "height",
     .doc = "Active raster height in lines",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = fb_attr_height}   },
    {.kind = M_ATTR,
     .name = "depth",
     .doc = "Bits per pixel (always 16 — the framebuffer is 5-6-5)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = fb_attr_depth}    },
    {.kind = M_ATTR,
     .name = "stride",
     .doc = "Scanout bytes per row",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = fb_attr_stride}   },
    {.kind = M_ATTR,
     .name = "displayed_buffer",
     .doc = "Physical colour buffer being scanned (status[11:10])",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = fb_attr_displayed}},
};
static const class_desc_t v2_fb_class = {
    .name = "voodoo2_fb",
    .members = fb_members,
    .n_members = sizeof(fb_members) / sizeof(fb_members[0]),
};

static value_t video_attr_drives(struct object *self, const member_t *m) {
    (void)m;
    voodoo2_t *v = node_card(self);
    return val_bool(v && v2_drives_monitor(v));
}
static value_t video_attr_swaps_pending(struct object *self, const member_t *m) {
    (void)m;
    voodoo2_t *v = node_card(self);
    if (!v)
        return val_uint(4, 0);
    v2_retire_swaps(v);
    return val_uint(4, v->swaps_pending);
}

static const member_t video_members[] = {
    {.kind = M_ATTR,
     .name = "drives_monitor",
     .doc = "The pass-through predicate: true while the Voodoo drives the monitor "
            "(fbiInit0[0] set, video running, unblanked, outputs driven)",   .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = video_attr_drives}       },
    {.kind = M_ATTR,
     .name = "swaps_pending",
     .doc = "swapbufferCMDs issued and not yet retired at a frame boundary",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = video_attr_swaps_pending}},
};
static const class_desc_t v2_video_class = {
    .name = "voodoo2_video",
    .members = video_members,
    .n_members = sizeof(video_members) / sizeof(video_members[0]),
};

static void v2_attach_objects(pci_device_t *dev, struct object *card_node) {
    voodoo2_t *v = (voodoo2_t *)dev->priv;
    if (!v || !card_node)
        return;
    struct object *fb = object_new(&v2_fb_class, v, "framebuffer");
    if (fb) {
        object_set_label(fb, "Framebuffer");
        object_set_order(fb, 10);
        object_attach(card_node, fb);
        // Nominate it: machine.screen.source resolves to whichever
        // framebuffer node belongs to the current primary display, so
        // it follows the pass-through switch automatically.
        pci_card_set_framebuffer_object(dev, fb);
    }
    struct object *video = object_new(&v2_video_class, v, "video");
    if (video) {
        object_set_label(video, "Video / Pass-through");
        object_set_order(video, 20);
        object_attach(card_node, video);
    }
    struct object *regs = object_new(&v2_regs_class, v, "regs");
    if (regs) {
        object_set_label(regs, "Registers");
        object_set_order(regs, 30);
        object_set_category(regs, M_CAT_ADVANCED);
        object_attach(card_node, regs);
    }
    struct object *dac = object_new(&v2_dac_class, v, "dac");
    if (dac) {
        object_set_label(dac, "DAC (ICS5342)");
        object_set_order(dac, 40);
        object_set_category(dac, M_CAT_ADVANCED);
        object_attach(card_node, dac);
    }
}

// ============================================================
// Options, factory, and the card kind
// ============================================================

static const char *const v2_mem_values[] = {"8m", "12m", NULL};
static const char *const v2_mem_labels[] = {"8 MB (2 MB per TMU)", "12 MB (4 MB per TMU)", NULL};
static const pci_card_option_t v2_options[] = {
    {.key = "memory", .label = "Board Memory", .values = v2_mem_values, .labels = v2_mem_labels, .default_value = "8m"},
    {.key = NULL},
};

static bool v2_stage_option(const char *key, const char *value) {
    if (!key || !value)
        return false;
    if (strcmp(key, "memory") == 0) {
        if (strcmp(value, "8m") == 0) {
            s_staged_tex_size = V2_TMU_2MB;
            return true;
        }
        if (strcmp(value, "12m") == 0) {
            s_staged_tex_size = V2_TMU_4MB;
            return true;
        }
        LOG(0, "unknown memory size '%s' — the card offers 8m, 12m", value);
        return true; // the key IS ours; the value was the problem
    }
    if (strcmp(key, "raster") == 0) {
        // Deliberately NOT in the advertised options: a test affordance
        // that installs the null backend to pin the seam's
        // analytic-timing invariant (voodoo2_raster.h).
        s_staged_null_raster = strcmp(value, "null") == 0;
        return true;
    }
    return false;
}

static pci_device_t *v2_factory(int slot_index, config_t *cfg, checkpoint_t *cp) {
    (void)cp;
    pci_device_t *dev = (pci_device_t *)calloc(1, sizeof(*dev));
    voodoo2_t *v = (voodoo2_t *)calloc(1, sizeof(*v));
    if (!dev || !v) {
        free(dev);
        free(v);
        return NULL;
    }
    dev->ops = &v2_ops;
    dev->decl = &v2_decl;
    dev->priv = v;
    pci_cfg_reset(dev); // power-on header state, including status_reset
    v->dev = dev;
    v->cfg = cfg;

    v->tex_size = s_staged_tex_size;
    s_staged_tex_size = V2_TMU_2MB;
    v->raster = s_staged_null_raster ? &v2_null_backend : &v2_sw_backend;
    s_staged_null_raster = false;
    v->fb_ram = (uint8_t *)calloc(1, V2_FB_SIZE);
    // Big-endian scanout raster for the display layer, sized for the
    // widest mode the part reaches (1024-pixel logical line).
    v->scanout = (uint8_t *)calloc(1, 1024u * 1024u * 2u);
    v->tex_ram[0] = (uint8_t *)calloc(1, v->tex_size);
    v->tex_ram[1] = (uint8_t *)calloc(1, v->tex_size);
    if (!v->fb_ram || !v->scanout || !v->tex_ram[0] || !v->tex_ram[1]) {
        free(v->fb_ram);
        free(v->scanout);
        free(v->tex_ram[0]);
        free(v->tex_ram[1]);
        free(v);
        free(dev);
        return NULL;
    }

    v2_reset(dev, cfg);

    v->bar_if.read_uint8 = v2_bar_read8;
    v->bar_if.read_uint16 = v2_bar_read16;
    v->bar_if.read_uint32 = v2_bar_read32;
    v->bar_if.write_uint8 = v2_bar_write8;
    v->bar_if.write_uint16 = v2_bar_write16;
    v->bar_if.write_uint32 = v2_bar_write32;
    pci_bar_backing_iface(dev, 0, &v->bar_if, v);

    LOG(1, "seated in slot %d: 4 MB framebuffer + 2 x %u MB texture (%u MB board)", slot_index, v->tex_size >> 20,
        (V2_FB_SIZE + 2u * v->tex_size) >> 20);
    return dev;
}

const pci_card_kind_t voodoo2_kind = {
    .id = "voodoo2",
    .display_name = "3dfx Voodoo2",
    .attach = PCI_ATTACH_PCI,
    .requires_prom = false, // no expansion ROM exists for this card
    .card_class = "3d", // NOT "display" — a pass-through 3D card must
                        // never retire a machine's builtin-fallback video
    .monitors = NULL, // it drives a monitor, but it is not the machine's
                      // display device
    .factory = v2_factory,
    .options = v2_options,
    .stage_option = v2_stage_option,
    .attach_objects = v2_attach_objects,
};
