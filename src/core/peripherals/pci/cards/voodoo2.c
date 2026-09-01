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

#define R_STATUS       0x00
#define R_INTRCTRL     0x01
#define R_VERTEX_AX    0x02 // ..0x07: vertexAx..vertexCy
#define R_TRIANGLECMD  0x20
#define R_FTRIANGLECMD 0x40
#define R_FBZCOLORPATH 0x41
#define R_FOGMODE      0x42
#define R_ALPHAMODE    0x43
#define R_FBZMODE      0x44
#define R_LFBMODE      0x45
#define R_CLIPLR       0x46
#define R_CLIPTB       0x47
#define R_NOPCMD       0x48
#define R_FASTFILLCMD  0x49
#define R_SWAPBUFCMD   0x4A
#define R_FOGCOLOR     0x4B
#define R_ZACOLOR      0x4C
#define R_CHROMAKEY    0x4D
#define R_CHROMARANGE  0x4E
#define R_USERINTRCMD  0x4F
#define R_STIPPLE      0x50
#define R_COLOR0       0x51
#define R_COLOR1       0x52
#define R_PIXELS_IN    0x53
#define R_CHROMA_FAIL  0x54
#define R_ZFUNC_FAIL   0x55
#define R_AFUNC_FAIL   0x56
#define R_PIXELS_OUT   0x57
#define R_FOGTABLE     0x58 // ..0x77
#define R_CMDFIFO_BASE 0x78 // ..0x7E, non-FIFO
#define R_FBIINIT4     0x80
#define R_VRETRACE     0x81
#define R_BACKPORCH    0x82
#define R_VIDEODIM     0x83
#define R_FBIINIT0     0x84
#define R_FBIINIT1     0x85
#define R_FBIINIT2     0x86
#define R_FBIINIT3     0x87
#define R_HSYNC        0x88
#define R_VSYNC        0x89
#define R_CLUTDATA     0x8A
#define R_DACDATA      0x8B
#define R_MAXRGBDELTA  0x8C
#define R_HBORDER      0x8D
#define R_VBORDER      0x8E
#define R_BORDERCOLOR  0x8F
#define R_HVRETRACE    0x90
#define R_FBIINIT5     0x91
#define R_FBIINIT6     0x92
#define R_FBIINIT7     0x93
#define R_SWAPHISTORY  0x96
#define R_TRIANGLESOUT 0x97
#define R_SETUPMODE    0x98 // ..0xA9: the on-chip setup block
#define R_SDRAWTRICMD  0xA8
#define R_SBEGINTRICMD 0xA9
#define R_BLT_FIRST    0xB0 // ..0xBF: the 2D BitBLT engine (a NON-GOAL)
#define R_TEXTUREMODE  0xC0 // TMU space begins: 0xC0..0xFF per TMU
#define R_TLOD         0xC1
#define R_TDETAIL      0xC2
#define R_TEXBASE      0xC3
#define R_TEXBASE_1    0xC4
#define R_TEXBASE_2    0xC5
#define R_TEXBASE_38   0xC6
#define R_TREXINIT0    0xC7
#define R_TREXINIT1    0xC8
#define R_NCC0_FIRST   0xC9 // ..0xD4
#define R_NCC1_FIRST   0xD5 // ..0xE0

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

    // Swap bookkeeping: swaps pending count until the frame boundary
    // after issue retires them (status[30:28], fbiSwapHistory).
    uint32_t swaps_pending;
    uint64_t swap_issue_frame;

    // One-shot log guards.
    bool warned_narrow_reg;
    bool warned_cfg_unknown;
    bool warned_tex_read;
    bool warned_cmdfifo;
    bool warned_draw;

    memory_interface_t bar_if;
} voodoo2_t;

// Staged option (consumed by the factory, the mach64 idiom).
static uint32_t s_staged_tex_size = V2_TMU_2MB;

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
    if (v->swaps_pending && v2_frame_number(v) != v->swap_issue_frame)
        v->swaps_pending = 0;
}

static uint32_t v2_status(voodoo2_t *v) {
    v2_retire_swaps(v);
    uint32_t s = 0x3Fu; // PCI FIFO free space: empty
    if (!v2_in_vretrace(v))
        s |= 1u << 6; // 0 = retrace ACTIVE [V2 p.29]
    // bits 7 (Chuck busy), 8 (Bruce busy), 9 (busy) stay 0: idle.
    // bits 11:10 displayed buffer: 0 until the display lands.
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
// `buffer` 0/1/2 = colour buffers, 3 = the aux/depth buffer.
static uint32_t v2_buffer_addr(const voodoo2_t *v, uint32_t buffer, uint32_t x, uint32_t y) {
    uint32_t mem_off_pages = (v->reg[R_FBIINIT2] >> FBIINIT2_MEMOFF_SHIFT) & FBIINIT2_MEMOFF_MASK;
    if (!mem_off_pages)
        mem_off_pages = 150u; // unprogrammed: one 640x480 16bpp buffer
    uint32_t base = (buffer == 3u ? v2_color_buffers(v) : buffer) * mem_off_pages * 4096u;
    uint32_t stride = v2_tiles_in_x(v) * 32u * 2u;
    return (base + y * stride + x * 2u) & (V2_FB_SIZE - 1u);
}

// Resolve an LFB-face access to (buffer, x, y).  Reads are always two
// 16-bit pixels per doubleword whatever the write format [V2 p.56], and
// v1 of this model carries the 16-bit-per-pixel formats — the ones every
// held driver and probe uses; 32-bit LFB formats join with the
// rasteriser milestone.
static void v2_lfb_locate(const voodoo2_t *v, uint32_t off, bool write, uint32_t *buffer, uint32_t *x, uint32_t *y) {
    uint32_t mode = v->reg[R_LFBMODE];
    *x = (off >> 1) & 0x3FFu;
    *y = (off >> 11) & 0x3FFu;
    if (LFB_Y_ORIGIN(mode))
        *y = (0x3FFu - *y) & 0x3FFu;
    if (write)
        *buffer = (LFB_FMT(mode) == LFB_FMT_ZZ) ? 3u : LFB_WRITE_BUF(mode);
    else
        *buffer = (LFB_READ_BUF(mode) == 2u) ? 3u : LFB_READ_BUF(mode);
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

static void v2_tex_write(voodoo2_t *v, uint32_t off, uint32_t le_value) {
    int tmu = (off >> 21) & 3u;
    if (tmu >= V2_NUM_TMUS)
        tmu &= 1; // only two Bruces are populated; the third select aliases
    // tLOD[25]/[26]: the texture path's own swizzle and word swap.
    uint32_t tlod = v->tmu_reg[tmu][R_TLOD - V2_TMU_REG_FIRST];
    if (tlod & (1u << 25))
        le_value = __builtin_bswap32(le_value);
    if (tlod & (1u << 26))
        le_value = (le_value >> 16) | (le_value << 16);
    uint32_t at = (off & 0x1FFFFCu) & (v2_tmu_addressable(v, tmu) - 1u);
    uint8_t *t = v->tex_ram[tmu];
    t[at] = (uint8_t)le_value;
    t[at + 1] = (uint8_t)(le_value >> 8);
    t[at + 2] = (uint8_t)(le_value >> 16);
    t[at + 3] = (uint8_t)(le_value >> 24);
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

// One register write, already decoded: `chip_mask` bit 0 = Chuck, bits
// 1..2 = the two Bruces (0 = everything) [V2 p.21 §5].
static void v2_reg_write(voodoo2_t *v, int idx, uint32_t chip_mask, uint32_t value) {
    if (chip_mask == 0)
        chip_mask = 0x7u; // 0 selects all chips

    // The TMU block routes by chip select; everything below is Chuck.
    if (idx >= V2_TMU_REG_FIRST) {
        for (int t = 0; t < V2_NUM_TMUS; t++) {
            if (chip_mask & (2u << t))
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
    case R_FTRIANGLECMD:
    case R_FASTFILLCMD:
    case R_SDRAWTRICMD:
    case R_SBEGINTRICMD:
        // The draw path lands with the rasteriser milestone; a draw
        // issued before then is logged once, completes "instantly", and
        // leaves the statistics counters untouched.
        if (!v->warned_draw) {
            v->warned_draw = true;
            LOG(2, "draw command reg $%03X issued — the rasteriser is not modelled yet", idx * 4);
        }
        return;
    default:
        break;
    }
    if (idx >= R_CMDFIFO_BASE && idx <= R_CMDFIFO_BASE + 6 && !v->warned_cmdfifo) {
        v->warned_cmdfifo = true;
        LOG(2, "CMDFIFO register write — the CMDFIFO engine is not on the Glide 2.x path");
    }
    v->reg[idx] = value;
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
    // With the CMDFIFO map enabled, direct writes outside the permitted
    // init/video/CMDFIFO set are accepted by the PCI slave and silently
    // dropped [V2 p.121].
    if ((v->reg[R_FBIINIT7] & FBIINIT7_CMDFIFO_EN) && idx < R_CMDFIFO_BASE) {
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
    if (off < V2_OFF_LFB)
        return VOODOO2_LE32(v2_reg_face_read(v, off));
    if (off < V2_OFF_TEX) {
        // LFB reads are gated by fbiInit1[3], which starts CLEAR so a
        // random powerup read cannot hang the machine [V2 p.68]; they
        // return two 16-bit pixels whatever the write format [V2 p.56].
        if (!(v->reg[R_FBIINIT1] & FBIINIT1_LFB_READ_EN))
            return 0;
        uint32_t buffer, x, y;
        v2_lfb_locate(v, off - V2_OFF_LFB, false, &buffer, &x, &y);
        uint32_t le = v2_lfb_load16(v, buffer, x, y) | ((uint32_t)v2_lfb_load16(v, buffer, x + 1u, y) << 16);
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
        v2_reg_face_write(v, off, le);
        return;
    }
    if (off < V2_OFF_TEX) {
        uint32_t buffer, x, y;
        v2_lfb_locate(v, off - V2_OFF_LFB, true, &buffer, &x, &y);
        uint32_t t = v2_lfb_write_transform(v, le);
        v2_lfb_store16(v, buffer, x, y, (uint16_t)t);
        v2_lfb_store16(v, buffer, x + 1u, y, (uint16_t)(t >> 16));
        return;
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
        uint32_t buffer, x, y;
        v2_lfb_locate(v, off - V2_OFF_LFB, false, &buffer, &x, &y);
        return VOODOO2_LE16(v2_lfb_load16(v, buffer, x, y));
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
        // A single 16-bit pixel; the Glide memory-sizing probes drive
        // exactly this path [3dfx-src fbiMemSize].
        uint32_t buffer, x, y;
        v2_lfb_locate(v, off - V2_OFF_LFB, true, &buffer, &x, &y);
        v2_lfb_store16(v, buffer, x, y, VOODOO2_LE16(data));
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
    v2_dac_reset(v);
}

static void v2_teardown(pci_device_t *dev, config_t *cfg) {
    (void)cfg;
    voodoo2_t *v = (voodoo2_t *)dev->priv;
    if (!v)
        return;
    free(v->fb_ram);
    for (int t = 0; t < V2_NUM_TMUS; t++)
        free(v->tex_ram[t]);
    free(v);
    dev->priv = NULL;
}

static const char *v2_name(const pci_device_t *dev) {
    (void)dev;
    return "3dfx Voodoo2";
}

// The pass-through contract: this card is never the machine's display
// device until it takes the monitor, and the take/release lands with the
// display milestone — until then it always yields, which is also the
// power-on truth (fbiInit0[0] straps to pass-through).
static display_t *v2_display(pci_device_t *dev) {
    (void)dev;
    return NULL;
}

// ============================================================
// Checkpoints — POD struct, then the memories as tail blobs
// ============================================================

typedef struct v2_ckpt {
    uint32_t reg[V2_NUM_REGS];
    uint32_t tmu_reg[V2_NUM_TMUS][64];
    uint32_t init_enable, bus_snoop[2], cfg_scratch, si_process;
    uint8_t dac_direct[8];
    uint8_t dac_pll[16][2];
    uint8_t dac_pll_wr_addr, dac_pll_rd_addr, dac_pll_wr_phase, dac_pll_rd_phase;
    uint8_t dac_read_latch;
    uint8_t pad[3];
    uint32_t swaps_pending;
    uint64_t swap_issue_frame;
    uint32_t tex_size;
} v2_ckpt_t;

static void v2_checkpoint_save(pci_device_t *dev, checkpoint_t *cp) {
    voodoo2_t *v = (voodoo2_t *)dev->priv;
    v2_ckpt_t c;
    memset(&c, 0, sizeof(c));
    memcpy(c.reg, v->reg, sizeof(c.reg));
    memcpy(c.tmu_reg, v->tmu_reg, sizeof(c.tmu_reg));
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
    system_read_checkpoint_data(cp, v->fb_ram, V2_FB_SIZE);
    for (int t = 0; t < V2_NUM_TMUS; t++)
        system_read_checkpoint_data(cp, v->tex_ram[t], v->tex_size);
}

static const pci_device_ops_t v2_ops = {
    .teardown = v2_teardown,
    .reset = v2_reset,
    .name = v2_name,
    .display = v2_display,
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

static void v2_attach_objects(pci_device_t *dev, struct object *card_node) {
    voodoo2_t *v = (voodoo2_t *)dev->priv;
    if (!v || !card_node)
        return;
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
    v->fb_ram = (uint8_t *)calloc(1, V2_FB_SIZE);
    v->tex_ram[0] = (uint8_t *)calloc(1, v->tex_size);
    v->tex_ram[1] = (uint8_t *)calloc(1, v->tex_size);
    if (!v->fb_ram || !v->tex_ram[0] || !v->tex_ram[1]) {
        free(v->fb_ram);
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
