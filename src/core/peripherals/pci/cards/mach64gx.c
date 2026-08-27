// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mach64gx.c
// The Apple Accelerated PCI Graphics Card — ATI 88800GX ("mach64 GX"),
// Apple codename *Spinnaker*, PCI $1002:$4758, Open Firmware node
// `ATY,mach64`.  The display card the Power Macintosh 9500 shipped with,
// and the first pluggable card kind on the generic PCI core.
//
// This card is driven by its OWN firmware.  Its expansion ROM carries an
// IEEE 1275 FCode program that Open Firmware executes at probe time to
// build the card's device-tree node, plus a PowerPC ndrv published as
// `driver,AAPL,MacOS,PowerPC` that Mac OS loads to drive it.  The
// emulator interprets none of it: it answers registers, and the guest's
// own firmware does the rest.  That is what makes the device tree
// trustworthy — what it says about the card is what the card said about
// itself.
//
// Software shape:
//
//   * ONE BAR: BAR0, 16 MB, non-prefetchable memory, plus the
//     expansion-ROM BAR at config $30.  No I/O BAR — the card's own `reg`
//     property declares exactly config space and BAR0, and Apple's dump of
//     a real card in a real 9500 agrees.
//   * ...but PCI I/O space is load-bearing anyway.  The register file has
//     two faces: a sparse I/O map at ((sel << 10) | base) with base
//     $2EC/$1CC/$1C8, and a 1 KB memory alias at aperture + $7FFC00.
//     CONFIG_CNTL — the register that ENABLES the memory aperture — is the
//     one mach64 register with NO memory alias, so the card is unreachable
//     through memory until it has been reached through I/O.  It decodes
//     I/O at a STRAPPED address, not through a BAR
//     (pci_device_add_fixed_region).
//   * The RAMDAC is a discrete IBM RGB514, reached through DAC_REGS' four
//     byte cells crossed with DAC_CNTL's DAC_EXT_SEL supplying RS[2].
//   * Monitor sense is Apple's three-line + extended probe through
//     DAC_CNTL bits 29:24 — present on GX-2 and later silicon only, which
//     is why CONFIG_CHIP_ID must report revision 2.
//
// ENDIANNESS.  mach64 registers are little-endian.  This card is NOT a TNT
// device — it fits any PCI machine — so it must not reach for a family
// macro like TNT_LE32.  It applies its own swap at its own edge, here, and
// nowhere else.
//
// Register truth: ATI, *mach64 Register Reference Guide* (RRG-S00700-05,
// 1994), chapters 1-3 and the chapter 2 cross-reference; ATI, *mach64
// Accelerator Programmer's Guide* (PRG888GX0-01, 1994); IBM, *RGB514
// datasheet* §9-§10; Apple, "Power Macintosh 9500 Computer" Developer
// Note (1995) tables 2-1..2-3; Apple Technote 1062 (1996), which dumps
// this card's node from a real 9500 under Open Firmware; and the card's
// OWN ROM, detokenized with scripts/fcode/detok.py.  Nothing here is
// derived from another emulator, and the GPL/BSD driver corpus was used
// for orientation only — notably NOT for monitor sense, where the Linux
// driver reads Apple's sense lines through GP_IO, a register that does not
// exist on the 88800GX at all.

#include "card.h"
#include "checkpoint.h"
#include "config_space.h"
#include "display.h"
#include "log.h"
#include "memory.h"
#include "object.h"
#include "pci.h"
#include "prom.h"
#include "scheduler.h"
#include "system.h"
#include "system_config.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("mach64");

// The card's little-endian register domain.  The ONE sanctioned swap point
// in this file; see the endianness note above.
#define MACH64_LE32(x) __builtin_bswap32((uint32_t)(x))

// ============================================================
// Identity and geometry
// ============================================================

#define MACH64_VENDOR_ID 0x1002u
#define MACH64_DEVICE_ID 0x4758u // 'GX'
// GX-2.  Not cosmetic: DAC_MON_ID_STATE / _DIR exist on the GX-2 and later
// rows of the manual's bit chart and NOT on GX-0/GX-1, so a card reporting
// an earlier revision sends the FCode down a path where it never senses a
// monitor.  Apple's own dump reports `revision-id 00000002`.
#define MACH64_REVISION 0x02u
#define MACH64_CLASS    0x030000u // display / VGA-compatible / no prog-if

#define MACH64_BAR_APER  0 // config $10 — the 16 MB aperture BAR
#define MACH64_APER_SIZE 0x1000000u // 16 MB, what the card's `reg` declares
#define MACH64_ROM_SIZE  0x8000u // the physical 32 KB chip

// The aperture the FCode selects: 8 MB placed at the assigned BAR0 base,
// with the 1 KB memory-mapped register alias in its last kilobyte.
#define MACH64_APER_8MB       0x800000u
#define MACH64_APER_4MB       0x400000u
#define MACH64_MMIO_OFF_8MB   0x7FFC00u
#define MACH64_MMIO_OFF_4MB   0x3FFC00u
#define MACH64_MMIO_BLOCK_LEN 0x400u

// VRAM: 2 MB soldered (what Apple shipped), 4 MB with the 109-31600-00
// expansion module.
#define MACH64_VRAM_2MB 0x200000u
#define MACH64_VRAM_4MB 0x400000u

// Sparse I/O decode (RRG ch. 1).  The register file is selected by the top
// six bits of a 16-bit I/O address; the bottom ten are the strapped base.
// The match mask is $3FC, not $3FF: each selected register is 32 bits wide,
// so base+0..base+3 are byte lanes of the same register — and the card's
// own FCode uses them, driving CONFIG_CNTL's upper halfword at $6AEE and
// reading the monitor-sense byte at DAC_CNTL+3 = $62EF.
#define MACH64_IO_BASE_DEFAULT 0x2ECu
#define MACH64_IO_MATCH_MASK   0x3FCu
#define MACH64_IO_SPAN         0x10000u

// ============================================================
// The register file — dword offsets (RRG ch. 2 cross-reference)
// ============================================================
// The memory alias indexes these directly; the I/O face maps its select
// through io_select_to_dword[] below.

#define DW_CRTC_H_TOTAL_DISP     0x00
#define DW_CRTC_H_SYNC_STRT_WID  0x01
#define DW_CRTC_V_TOTAL_DISP     0x02
#define DW_CRTC_V_SYNC_STRT_WID  0x03
#define DW_CRTC_VLINE_CRNT_VLINE 0x04
#define DW_CRTC_OFF_PITCH        0x05
#define DW_CRTC_INT_CNTL         0x06
#define DW_CRTC_GEN_CNTL         0x07
#define DW_OVR_CLR               0x10
#define DW_OVR_WID_LEFT_RIGHT    0x11
#define DW_OVR_WID_TOP_BOTTOM    0x12
#define DW_CUR_CLR0              0x18
#define DW_CUR_CLR1              0x19
#define DW_CUR_OFFSET            0x1A
#define DW_CUR_HORZ_VERT_POSN    0x1B
#define DW_CUR_HORZ_VERT_OFF     0x1C
#define DW_SCRATCH_REG0          0x20
#define DW_SCRATCH_REG1          0x21
#define DW_CLOCK_CNTL            0x24
#define DW_BUS_CNTL              0x28
#define DW_MEM_CNTL              0x2C
#define DW_MEM_VGA_WP_SEL        0x2D
#define DW_MEM_VGA_RP_SEL        0x2E
#define DW_DAC_REGS              0x30
#define DW_DAC_CNTL              0x31
#define DW_GEN_TEST_CNTL         0x34
#define DW_CONFIG_CHIP_ID        0x38
#define DW_CONFIG_STAT0          0x39
#define DW_CONFIG_STAT1          0x3A
#define DW_FIFO_STAT             0xC4
#define DW_GUI_STAT              0xCE

// The draw engine occupies dwords $40 and up; its WRITES pass a 16-entry
// command FIFO while everything below $40 is unFIFOed and reads never are
// (PRG, "The Command FIFO").  That documented split is why the model can
// treat < $40 as ordinary registers.
#define DW_DRAW_ENGINE_FIRST 0x40

// --- the draw engine (RRG ch. 2; behaviour per PRG chapters 3-5) -----------
#define DW_DST_OFF_PITCH      0x40
#define DW_DST_X              0x41
#define DW_DST_Y              0x42
#define DW_DST_Y_X            0x43
#define DW_DST_WIDTH          0x44
#define DW_DST_HEIGHT         0x45
#define DW_DST_HEIGHT_WIDTH   0x46
#define DW_DST_X_WIDTH        0x47
#define DW_DST_CNTL           0x4C
#define DW_SRC_OFF_PITCH      0x60
#define DW_SRC_X              0x61
#define DW_SRC_Y              0x62
#define DW_SRC_Y_X            0x63
#define DW_SRC_WIDTH1         0x64
#define DW_SRC_HEIGHT1        0x65
#define DW_SRC_HEIGHT1_WIDTH1 0x66
#define DW_SRC_CNTL           0x6D
#define DW_HOST_DATA0         0x80
#define DW_HOST_DATA_LAST     0x8F
#define DW_HOST_CNTL          0x90
#define DW_SC_LEFT            0xA8
#define DW_SC_RIGHT           0xA9
#define DW_SC_LEFT_RIGHT      0xAA
#define DW_SC_TOP             0xAB
#define DW_SC_BOTTOM          0xAC
#define DW_SC_TOP_BOTTOM      0xAD
#define DW_DP_BKGD_CLR        0xB0
#define DW_DP_FRGD_CLR        0xB1
#define DW_DP_WRITE_MSK       0xB2
#define DW_DP_PIX_WIDTH       0xB4
#define DW_DP_MIX             0xB5
#define DW_DP_SRC             0xB6
#define DW_CONTEXT_LOAD_CNTL  0xCB

// DP_SRC: which generator feeds each of the three muxes in the pixel data
// path (RRG p. 3-40).  Every draw operation colour-expands a MONOCHROME
// source into two colour sources: where the mono bit is 1 the foreground
// colour source and foreground mix are used, where it is 0 the background
// pair is (PRG, "Logical Pixel Data Path").
#define DP_BKGD_SRC(v) ((v) & 7u)
#define DP_FRGD_SRC(v) (((v) >> 8) & 7u)
#define DP_MONO_SRC(v) (((v) >> 16) & 3u)

// Colour-source encodings, shared by DP_BKGD_SRC and DP_FRGD_SRC.
#define DP_SRC_BKGD_CLR 0u
#define DP_SRC_FRGD_CLR 1u
#define DP_SRC_HOST     2u
#define DP_SRC_BLIT     3u
#define DP_SRC_PATTERN  4u

// Monochrome-source encodings (a DIFFERENT table — 1 is the pattern here,
// not the foreground colour).
#define DP_MONO_ALWAYS_1 0u
#define DP_MONO_PATTERN  1u
#define DP_MONO_HOST     2u
#define DP_MONO_BLIT     3u

// PAT_REG0/PAT_REG1 hold an 8 x 8 monochrome pattern, one bit per pixel.
#define DW_PAT_REG0 0xA0
#define DW_PAT_REG1 0xA1
#define DW_PAT_CNTL 0xA2
// DP_MIX: the raster op the pixel ALU applies, foreground in bits 20:16 and
// background in bits 4:0.  The function table is ATI's own (PRG, "Source
// and Destination Mixing Logic") and is implemented in full by
// mach64_mix() — reading it off the manual mattered, because the driver
// uses function 0 ("not D", an invert) heavily and treating everything as
// "source" painted solid blocks of foreground colour over the desktop.
#define DP_FRGD_MIX(v) (((v) >> 16) & 0x1Fu)
#define DP_BKGD_MIX(v) ((v) & 0x1Fu)
#define DP_MIX_SRC     7u
#define DP_MIX_AVERAGE 0x17u // (D+S) >> 1, with DP_CHAIN_MSK breaking carries

// CLR_CMP_CNTL: a per-pixel comparison that can veto the write.  "If the
// result of the comparison is FALSE, the result of the ALU is written to
// the destination; otherwise the destination data is written" (PRG), so a
// TRUE comparison leaves the pixel alone — which is how a transparent blit
// is expressed.
#define DW_CLR_CMP_CLR  0xC0
#define DW_CLR_CMP_MSK  0xC1
#define DW_CLR_CMP_CNTL 0xC2
#define CLR_CMP_FN(v)   ((v) & 7u)
#define CLR_CMP_FALSE   0u // never veto
#define CLR_CMP_TRUE    1u // always veto
#define CLR_CMP_NE      4u // veto where the pixel differs from CLR_CMP_CLR
#define CLR_CMP_EQ      5u // veto where it matches — the transparent blit
#define CLR_CMP_SRC(v)  (((v) >> 24) & 3u) // 0 = compare the destination
// DST_CNTL: which way the trajectory walks.  1 = increasing.
#define DST_X_DIR 0x01u
#define DST_Y_DIR 0x02u

// CONFIG_CNTL has NO memory-mapped alias, so it has no natural dword
// index.  It lives one slot above the 256-dword alias window, which the
// memory face masks to $00-$FF and therefore can never reach.
#define DW_CONFIG_CNTL  0x100
#define MACH64_NUM_REGS 0x101

// --- CONFIG_CHIP_ID (RRG p. 3-8) -------------------------------------------
// CFG_CHIP_TYPE 15:0 = 'GX' = $00D7, CFG_CHIP_CLASS 23:16 = 0,
// CFG_CHIP_REV 31:24 = 2 (GX-2, the revision with the monitor-ID pins).
#define MACH64_CHIP_ID 0x020000D7u

// --- CONFIG_CNTL (RRG p. 3-9) ----------------------------------------------
#define CFG_MEM_AP_SIZE   0x0003u // 0 = disabled, 1 = 4 MB, 2 = 8 MB
#define CFG_MEM_VGA_AP_EN 0x0004u
#define CFG_MEM_AP_LOC    0x3FF0u // aperture location, 4 MB units, at bit 4
#define CFG_VGA_DIS       0x80000u

// --- DAC_CNTL (RRG p. 3-31) ------------------------------------------------
#define DAC_EXT_SEL         0x0003u // RS[2] for the RAMDAC cell select
#define DAC_8BIT_EN         0x0100u
#define DAC_TYPE_SHIFT      16
#define DAC_MON_ID_STATE_SH 24 // bits 26:24 — monitor ID pin state
#define DAC_MON_ID_DIR_SH   27 // bits 29:27 — which pin is an output

// --- MEM_CNTL: MEM_SIZE (bits 2:0) -----------------------------------------
#define MEM_SIZE_512K 0u
#define MEM_SIZE_1M   1u
#define MEM_SIZE_2M   2u
#define MEM_SIZE_4M   3u

// --- CRTC_GEN_CNTL ---------------------------------------------------------
#define CRTC_PIX_WIDTH_SH 8 // bits 10:8
#define CRTC_EXT_DISP_EN  0x01000000u
#define CRTC_EN           0x02000000u
#define CRTC_DISPLAY_DIS  0x00000040u

// CRTC_PIX_WIDTH encodings (RRG p. 3-24).
#define CRTC_PIX_4BPP  1u
#define CRTC_PIX_8BPP  2u
#define CRTC_PIX_15BPP 3u // 5,5,5 — the Mac "thousands of colours" mode
#define CRTC_PIX_16BPP 4u // 5,6,5
#define CRTC_PIX_24BPP 5u
#define CRTC_PIX_32BPP 6u

// --- CRTC_INT_CNTL (RRG p. 3-21) -------------------------------------------
// Bit 0 is the live blank state; bits 2 and 4 read as the pending latch and
// write as the acknowledge (write-1-to-clear).
#define CRTC_VBLANK        0x01u
#define CRTC_VBLANK_INT_EN 0x02u
#define CRTC_VBLANK_INT    0x04u
#define CRTC_VLINE_INT_EN  0x08u
#define CRTC_VLINE_INT     0x10u
#define CRTC_INT_ACK_BITS  (CRTC_VBLANK_INT | CRTC_VLINE_INT)

// Is either interrupt both enabled and pending?  The slot line follows this.
#define CRTC_INT_ENABLE_FOR(v)                                                                                         \
    (((((v) & CRTC_VBLANK_INT_EN) && ((v) & CRTC_VBLANK_INT)) ||                                                       \
      (((v) & CRTC_VLINE_INT_EN) && ((v) & CRTC_VLINE_INT)))                                                           \
         ? true                                                                                                        \
         : false)

// ============================================================
// Monitors
// ============================================================
//
// Apple's three-line + extended sense, as the 9500 developer note (tables
// 2-1 and 2-3) publishes it.  `primary` is the 3-bit code read with all
// three pins tri-stated; the three `ext` values are the 2-bit readbacks
// obtained by driving one pin low and reading the other two.
//
// A monitor with no tie resistors does not need table entries at all: its
// extended readbacks fall out of the primary strap, because the two pins
// still being read simply report their own strapped levels.  Deriving them
// that way for the 14" AppleColor (primary 6, pins 1,1,0) yields $2B —
// which is exactly the extended walk the repository already records for
// that same monitor on Control's completely unrelated sense circuit
// (control.c).  Two independent circuits agreeing on one monitor is a good
// sign the engine below is right.
//
// The multiple-scan entries come from Apple's table.  Which raster each
// code actually selects is a driver decision we have not measured yet, and
// the bit order WITHIN each extended pair is not stated by the note — so
// mach64_sense_log() prints the primary and extended codes the engine
// produced on the first run, and the table gets pinned from that (§8 Q7 of
// the proposal).  Nothing about enumeration depends on it: the card's
// FCode gives up only when primary == 7 AND extended == $3F together, i.e.
// when all three pins float high in every configuration, which is what "no
// cable attached" looks like.
typedef struct mach64_monitor_sense {
    const char *id;
    uint8_t primary; // 3-bit code, pins (2,1,0), all tri-stated
    uint8_t ext01; // drive pin 2, read (pin1,pin0) -> pin1*2 + pin0
    uint8_t ext02; // drive pin 1, read (pin2,pin0) -> pin2*2 + pin0
    uint8_t ext12; // drive pin 0, read (pin2,pin1) -> pin2*2 + pin1
} mach64_monitor_sense_t;

// Derived-from-strap extended values for a monitor with no tie resistors:
// with pin k driven low, the other two read their own strapped levels.
#define STRAP_EXT01(p) (uint8_t)((((p) >> 1) & 1u) * 2u + ((p) & 1u))
#define STRAP_EXT02(p) (uint8_t)((((p) >> 2) & 1u) * 2u + ((p) & 1u))
#define STRAP_EXT12(p) (uint8_t)((((p) >> 2) & 1u) * 2u + (((p) >> 1) & 1u))

static const mach64_monitor_sense_t mach64_sense[] = {
    // 14" AppleColor / 12" monochrome — no tie resistors; extended = $2B.
    {"14in_rgb",   6, STRAP_EXT01(6), STRAP_EXT02(6), STRAP_EXT12(6)},
    // The multiple-scan displays answer the extended probe (Apple table 2-3).
    {"15in_multi", 6, 3,              0,              0             },
    {"17in_multi", 6, 3,              2,              0             },
    {"20in_multi", 6, 3,              0,              2             },
    // 21" colour — a primary-only code, like the 14".
    {"21in_color", 0, STRAP_EXT01(0), STRAP_EXT02(0), STRAP_EXT12(0)},
    {NULL,         0, 0,              0,              0             },
};

// The depths Apple documents per raster at 2 MB (9500 developer note table
// 2-1).  These reach the configuration dialog through machine.profile.
static const int depths_8_16_24[] = {8, 16, 24, 0};
static const int depths_8_16[] = {8, 16, 0};

static const struct nubus_monitor mach64_monitors[] = {
    {.id = "14in_rgb",
     .name = "14\" AppleColor / 12\" monochrome",
     .width = 640,
     .height = 480,
     .depths = depths_8_16_24,
     .sense_code = 6},
    {.id = "15in_multi",
     .name = "15\" Multiple Scan",
     .width = 832,
     .height = 624,
     .depths = depths_8_16_24,
     .sense_code = 6},
    {.id = "17in_multi",
     .name = "17\" Multiple Scan",
     .width = 1024,
     .height = 768,
     .depths = depths_8_16,
     .sense_code = 6},
    {.id = "20in_multi",
     .name = "20\" Multiple Scan",
     .width = 1152,
     .height = 870,
     .depths = depths_8_16,
     .sense_code = 6},
    {.id = "21in_color", .name = "21\" Colour", .width = 1152, .height = 870, .depths = depths_8_16, .sense_code = 0},
    {.id = NULL},
};

// ============================================================
// Device state
// ============================================================

typedef struct mach64 {
    pci_device_t *dev;
    config_t *cfg;

    uint32_t reg[MACH64_NUM_REGS]; // the register file, little-endian values
    uint32_t io_base; // strapped sparse-I/O base ($2EC default)

    uint8_t *vram;
    uint32_t vram_size;

    const mach64_monitor_sense_t *mon; // the strapped monitor
    uint8_t mon_id_dir; // DAC_CNTL 29:27, latched from the last write
    uint8_t mon_id_out; // DAC_CNTL 26:24 as WRITTEN (the driven levels)
    bool sense_seen; // the guest has exercised the sense engine
    uint8_t sense_primary; // what it read with all pins input
    uint8_t sense_ext; // the assembled 6-bit extended code

    // IBM RGB514 (see §"The RAMDAC" below)
    uint16_t dac_index; // the 16-bit indexed-register pointer
    uint8_t dac_index_lo; // index-low cell, awaiting its high half
    uint8_t dac_indexed[0x500]; // indexed file + the 1 KB cursor array
    uint8_t dac_pixel_mask; // RS 010
    uint8_t clut[256][3];
    uint8_t clut_addr; // palette address (RS 000 write / 011 read)
    uint8_t clut_phase; // R/G/B byte phase of the palette data cell

    // Expansion ROM (owned by pci_device_t.rom; this is the backing iface)
    memory_interface_t rom_if;
    memory_interface_t aper_if; // BAR0: framebuffer + the register alias
    memory_interface_t io_if; // the sparse I/O face

    // Scanout.  The descriptor is rebuilt from the CRTC registers whenever
    // one of them moves; `blank` is the black stub shown while the raster
    // is disabled, and `compose` carries the hardware-cursor composite.
    display_t display;
    rgba8_t clut_view[256];
    uint8_t *blank;
    uint8_t *compose;
    bool clut_dirty; // palette changed since the last scanout refresh
    bool irq_active; // the slot line this card is currently driving
    bool aperture_warned; // the "guest touched the BAR0 slack" log is once-only
    bool pix_width_warned; // the unsupported-depth log is once-only
    bool host_blit_warned; // the host-data-blit log is once-only
    bool mix_warned; // the unsupported-raster-op log is once-only
    uint64_t blits; // operations the engine has executed (diagnostics)
    // A host-data operation in flight.  Host data is a STREAM: the
    // trajectory is set up, then pixels arrive one HOST_DATA write at a
    // time until the rectangle is full (PRG, "Monochrome Expansion
    // Bitblit").
    struct {
        bool active;
        uint32_t x0, y0, w, h;
        uint32_t col, row;
    } host_op;
} mach64_t;

// The staged options a `pci_card=` boot can carry, consumed by the factory.
static char s_staged_monitor[32];
static uint32_t s_staged_vram = MACH64_VRAM_2MB;

// ============================================================
// The register file — two faces, one file
// ============================================================

// I/O select -> dword offset (RRG ch. 2).  -1 = the select decodes nothing.
// $1A is CONFIG_CNTL, which has no memory alias and lives in its own slot;
// $1F is a documented second select for CRTC_H_TOTAL_DISP.
static const int16_t io_select_to_dword[0x40] = {
    [0x00] = DW_CRTC_H_TOTAL_DISP,
    [0x01] = DW_CRTC_H_SYNC_STRT_WID,
    [0x02] = DW_CRTC_V_TOTAL_DISP,
    [0x03] = DW_CRTC_V_SYNC_STRT_WID,
    [0x04] = DW_CRTC_VLINE_CRNT_VLINE,
    [0x05] = DW_CRTC_OFF_PITCH,
    [0x06] = DW_CRTC_INT_CNTL,
    [0x07] = DW_CRTC_GEN_CNTL,
    [0x08] = DW_OVR_CLR,
    [0x09] = DW_OVR_WID_LEFT_RIGHT,
    [0x0A] = DW_OVR_WID_TOP_BOTTOM,
    [0x0B] = DW_CUR_CLR0,
    [0x0C] = DW_CUR_CLR1,
    [0x0D] = DW_CUR_OFFSET,
    [0x0E] = DW_CUR_HORZ_VERT_POSN,
    [0x0F] = DW_CUR_HORZ_VERT_OFF,
    [0x10] = DW_SCRATCH_REG0,
    [0x11] = DW_SCRATCH_REG1,
    [0x12] = DW_CLOCK_CNTL,
    [0x13] = DW_BUS_CNTL,
    [0x14] = DW_MEM_CNTL,
    [0x15] = DW_MEM_VGA_WP_SEL,
    [0x16] = DW_MEM_VGA_RP_SEL,
    [0x17] = DW_DAC_REGS,
    [0x18] = DW_DAC_CNTL,
    [0x19] = DW_GEN_TEST_CNTL,
    [0x1A] = DW_CONFIG_CNTL,
    [0x1B] = DW_CONFIG_CHIP_ID,
    [0x1C] = DW_CONFIG_STAT0,
    [0x1D] = DW_CONFIG_STAT1,
    [0x1F] = DW_CRTC_H_TOTAL_DISP,
};

// Every entry above is a real assignment; the zero-filled remainder would
// otherwise read as "CRTC_H_TOTAL_DISP", so an unassigned select has to be
// told apart from select 0 explicitly.
static int io_dword_for_select(uint32_t sel) {
    if (sel >= 0x40)
        return -1;
    if (sel == 0x00 || sel == 0x1F)
        return DW_CRTC_H_TOTAL_DISP;
    int dw = io_select_to_dword[sel];
    return dw ? dw : -1;
}

// Forward declarations for the behaviour the register file dispatches into;
// each is defined in its own section below.
static void mach64_sense_step(mach64_t *m);
static uint8_t mach64_mon_id_state(const mach64_t *m);
static uint32_t mach64_current_vline(const mach64_t *m);
static uint32_t mach64_dac_read(mach64_t *m);
static void mach64_dac_write(mach64_t *m, uint32_t value);
static void mach64_aperture_changed(mach64_t *m);
static uint32_t mach64_mem_size_code(const mach64_t *m);
static uint32_t mach64_aperture_size(const mach64_t *m);
static void mach64_clut_changed(mach64_t *m);
static uint32_t mach64_reg_read(mach64_t *m, int dw);
static void mach64_reg_write(mach64_t *m, int dw, uint32_t value);
static void mach64_update(mach64_t *m);
static bool mach64_in_vblank(const mach64_t *m);
static bool mach64_engine_write(mach64_t *m, int dw, uint32_t value);
static void mach64_host_feed(mach64_t *m, uint32_t value);
static uint32_t mach64_bytes_per_pixel(const mach64_t *m);
static void mach64_irq_sync(mach64_t *m);

// --- lane access -----------------------------------------------------------
//
// Almost every register is one 32-bit value whose byte lanes can be poked
// individually, which a read-modify-write models correctly.  DAC_REGS is
// NOT: its four byte lanes are four SEPARATE RAMDAC cells, several of them
// with read side effects (the palette data cell auto-advances).  Reading
// all four to write one would walk the palette pointer three extra times
// and corrupt the CLUT, so the byte and halfword paths route it lane-wise
// instead of through the dword.

static uint8_t mach64_dac_read_lane(mach64_t *m, uint32_t lane);
static void mach64_dac_write_lane(mach64_t *m, uint32_t lane, uint8_t value);

static uint8_t mach64_reg_read_lane(mach64_t *m, int dw, uint32_t lane) {
    if (dw == DW_DAC_REGS)
        return mach64_dac_read_lane(m, lane);
    return (uint8_t)(mach64_reg_read(m, dw) >> (8u * (lane & 3u)));
}

static void mach64_reg_write_lane(mach64_t *m, int dw, uint32_t lane, uint8_t value) {
    if (dw == DW_DAC_REGS) {
        mach64_dac_write_lane(m, lane, value);
        return;
    }
    uint32_t shift = 8u * (lane & 3u);
    uint32_t v = (mach64_reg_read(m, dw) & ~(0xFFu << shift)) | ((uint32_t)value << shift);
    mach64_reg_write(m, dw, v);
}

// --- reads -----------------------------------------------------------------

// Registers whose value is not simply what was last written.
static uint32_t mach64_reg_read(mach64_t *m, int dw) {
    switch (dw) {
    case DW_CONFIG_CHIP_ID:
        // Read-only, and exactly known.  Getting this wrong sends the
        // FCode down the GX-0/GX-1 path, where the monitor-ID pins do not
        // exist and it can never sense a display.
        return MACH64_CHIP_ID;

    case DW_CONFIG_STAT0:
        // Read-only configuration straps (RRG p. 3-10..3-13), GX-2 layout:
        // CFG_BUS_TYPE 2:0 = 0 (PCI), CFG_MEM_TYPE 5:3 = 5 (enhanced VRAM),
        // CFG_VGA_EN 23, CFG_CHIP_EN 25.  CFG_INIT_DAC_TYPE (11:9) has no
        // encoding for an IBM RGB514 — the strap's eight values are all
        // other vendors' parts — which is not a problem, because Apple's
        // FCode is board-specific and its DAC bring-up is unconditional
        // straight-line code that senses nothing.  A plausible value is
        // strapped and every read of the field is logged, so the first run
        // says whether anything actually cares.
        LOG(4, "CONFIG_STAT0 read (CFG_INIT_DAC_TYPE = %u)", (m->reg[DW_CONFIG_STAT0] >> 9) & 7u);
        return m->reg[DW_CONFIG_STAT0];

    case DW_DAC_CNTL: {
        // The monitor-ID pins read back live; everything else is the latch.
        uint32_t v = m->reg[DW_DAC_CNTL] & ~(7u << DAC_MON_ID_STATE_SH);
        return v | ((uint32_t)mach64_mon_id_state(m) << DAC_MON_ID_STATE_SH);
    }

    case DW_FIFO_STAT:
        // ATI's own wait loop is `while ((FIFO_STAT & $FFFF) > ($8000 >> n));`
        // so a low halfword of 0 means "all 16 entries free" and every wait
        // returns immediately (PRG, "The Command FIFO").
        return 0u;

    case DW_GUI_STAT:
        // ATI's own idle loop is `while ((GUI_STAT & 1) != 0);` — bit 0
        // clear is "engine idle", so no draw ever appears to be in flight.
        return 0u;

    case DW_CRTC_VLINE_CRNT_VLINE:
        return mach64_current_vline(m);

    case DW_CRTC_INT_CNTL:
        // Bit 0 is live; the enables and the pending latches are stored.
        return (m->reg[DW_CRTC_INT_CNTL] & ~CRTC_VBLANK) | (mach64_in_vblank(m) ? CRTC_VBLANK : 0);

    case DW_DAC_REGS:
        return mach64_dac_read(m);

    default:
        return m->reg[dw];
    }
}

// --- writes ----------------------------------------------------------------

static void mach64_reg_write(mach64_t *m, int dw, uint32_t value) {
    switch (dw) {
    case DW_CONFIG_CHIP_ID:
    case DW_CONFIG_STAT0:
    case DW_CONFIG_STAT1:
        // Read-only straps.  The card's own init table DOES write
        // CONFIG_STAT0 (value 0 on a GX, 9 on a CT), so this is a normal
        // event, not a guest bug — accept it and discard it.
        LOG(3, "write to read-only register (dword $%02X) = $%08X — ignored", dw, value);
        return;

    case DW_CONFIG_CNTL: {
        uint32_t was = m->reg[DW_CONFIG_CNTL];
        m->reg[DW_CONFIG_CNTL] = value;
        if ((was ^ value) & (CFG_MEM_AP_SIZE | CFG_MEM_AP_LOC))
            mach64_aperture_changed(m);
        return;
    }

    case DW_DAC_CNTL: {
        m->reg[DW_DAC_CNTL] = value;
        uint8_t dir = (uint8_t)((value >> DAC_MON_ID_DIR_SH) & 7u);
        uint8_t out = (uint8_t)((value >> DAC_MON_ID_STATE_SH) & 7u);
        if (dir != m->mon_id_dir || out != m->mon_id_out) {
            m->mon_id_dir = dir;
            m->mon_id_out = out;
            mach64_sense_step(m);
        }
        return;
    }

    case DW_DAC_REGS:
        mach64_dac_write(m, value);
        return;

    case DW_CRTC_H_TOTAL_DISP:
    case DW_CRTC_V_TOTAL_DISP:
    case DW_CRTC_OFF_PITCH:
    case DW_CRTC_GEN_CNTL:
        m->reg[dw] = value;
        mach64_update(m);
        return;

    case DW_CRTC_INT_CNTL: {
        // Bits 2 and 4 are the pending latches: they read as status and a
        // written 1 ACKNOWLEDGES.  Everything else is a plain latch.  The
        // driver's ISR reads this register and writes the bit back, so
        // treating the write as a straight store would leave the interrupt
        // asserted and the handler would loop at interrupt speed.
        uint32_t prev = m->reg[DW_CRTC_INT_CNTL];
        uint32_t kept = prev & CRTC_INT_ACK_BITS & ~value; // 1 = acknowledge
        // Bit 0 is read-only (the live blank state), so never store it.
        m->reg[DW_CRTC_INT_CNTL] = (value & ~(CRTC_INT_ACK_BITS | CRTC_VBLANK)) | kept;
        mach64_irq_sync(m);
        return;
    }

    case DW_MEM_CNTL:
        // MEM_SIZE (bits 2:0) must agree with the buffer we allocated, or
        // the driver offers depths the framebuffer cannot hold.  The card's
        // own init table writes 2 (= 2 MB) on a GX; a 4 MB card is the
        // expansion module.  Keep OUR size authoritative and say so.
        if ((value & 7u) != mach64_mem_size_code(m)) {
            LOG(1, "MEM_CNTL wrote MEM_SIZE=%u but this card has %u MB — keeping %u", value & 7u, m->vram_size >> 20,
                mach64_mem_size_code(m));
        }
        m->reg[DW_MEM_CNTL] = (value & ~7u) | mach64_mem_size_code(m);
        return;

    default:
        if (dw >= DW_DRAW_ENGINE_FIRST && mach64_engine_write(m, dw, value))
            return;
        if (dw >= DW_DRAW_ENGINE_FIRST) {
            // An engine register with no behaviour of its own: pattern,
            // colour-compare, context.  Stored and read back.
            LOG(4, "draw-engine register dword $%02X = $%08X (store only)", dw, value);
        }
        m->reg[dw] = value;
        return;
    }
}

// ============================================================
// Monitor sense — Apple's three-line + extended probe
// ============================================================
//
// DAC_MON_ID_DIR (DAC_CNTL 29:27) selects which single pin is an OUTPUT;
// DAC_MON_ID_STATE (26:24) reads the pin levels back.  The card's own
// FCode, decoded from its ROM, runs exactly four steps — one with all pins
// tri-stated and three each driving one pin low — writing the whole field
// as byte 3 of DAC_CNTL at sparse-I/O $62EF, waiting 1 ms, and reading it
// back.  It then packs the three 2-bit results into a 6-bit extended code.
//
// The bail-out it guards is narrow and worth stating exactly, because it
// was the most-feared failure mode of this whole phase: the FCode prints
// "No monitor" and aborts only when the primary code is 7 AND the extended
// code is $3F — every pin floating high in every configuration, i.e. no
// cable attached.  Any real Apple sense code passes.

// What the three pins read back in the current direction configuration.
static uint8_t mach64_mon_id_state(const mach64_t *m) {
    const mach64_monitor_sense_t *mon = m->mon;
    if (!mon)
        return 7u; // nothing strapped: all pins float high = "no monitor"
    switch (m->mon_id_dir) {
    case 0: // all three tri-stated: the primary code
        return mon->primary;
    case 1: // pin 0 driven: read (pin2, pin1)
        return (uint8_t)((((mon->ext12 >> 1) & 1u) << 2) | ((mon->ext12 & 1u) << 1));
    case 2: // pin 1 driven: read (pin2, pin0)
        return (uint8_t)((((mon->ext02 >> 1) & 1u) << 2) | (mon->ext02 & 1u));
    case 4: // pin 2 driven: read (pin1, pin0)
        return (uint8_t)(mon->ext01 & 3u);
    default:
        // Two or three pins driven at once is reserved (RRG p. 3-31).  Real
        // silicon would report whatever the drivers won; say "all high" and
        // log, so a driver doing something we have not seen announces itself.
        LOG(1, "reserved DAC_MON_ID_DIR value %u — reporting all pins high", m->mon_id_dir);
        return 7u;
    }
}

// Record what the guest is reading, so the first run REPORTS the codes
// rather than leaving them to be guessed at.  Which raster each code
// selects is a driver decision we have not measured; this is the
// instrument that will settle it.
static void mach64_sense_step(mach64_t *m) {
    uint8_t state = mach64_mon_id_state(m);
    if (m->mon_id_dir == 0) {
        // The card's FCode tri-states the pins BETWEEN every step, not just
        // at the start of a walk, so this is not the place to clear the
        // accumulated extended code — doing that left it reading $00 after
        // a complete and correct four-step probe.  Each direction below
        // owns its own two bits and simply overwrites them.
        m->sense_primary = state;
        m->sense_seen = true;
    } else if (m->mon) {
        // Reassemble the same 6-bit code the card's FCode builds:
        // (pin1,pin0) << 4 | (pin2,pin0) << 2 | (pin2,pin1).
        switch (m->mon_id_dir) {
        case 4:
            m->sense_ext = (uint8_t)((m->sense_ext & 0x0Fu) | ((m->mon->ext01 & 3u) << 4));
            break;
        case 2:
            m->sense_ext = (uint8_t)((m->sense_ext & 0x33u) | ((m->mon->ext02 & 3u) << 2));
            break;
        case 1:
            m->sense_ext = (uint8_t)((m->sense_ext & 0x3Cu) | (m->mon->ext12 & 3u));
            break;
        default:
            break;
        }
    }
    LOG(3, "sense: dir=%u -> state=%u (monitor '%s': primary=%u extended=$%02X)", m->mon_id_dir, state,
        m->mon ? m->mon->id : "(none)", m->sense_primary, m->sense_ext);
}

// ============================================================
// The RAMDAC — an IBM RGB514
// ============================================================
//
// The DAC presents eight cells selected by RS[2:0] (datasheet table 9).
// The mach64 reaches all eight through DAC_REGS' four byte cells crossed
// with DAC_CNTL's DAC_EXT_SEL, which supplies RS[2]:
//
//   RS 000 palette address (write mode)   RS 100 index low
//   RS 001 palette data                   RS 101 index high
//   RS 010 pixel mask                     RS 110 index data
//   RS 011 palette address (read mode)    RS 111 index control
//
// The card's FCode writes nine indexed registers at bring-up and sets the
// pixel mask to $FF; the ndrv adds the palette.  The PLL's actual
// frequency is deliberately not modelled — nothing in this pipeline
// consumes a pixel clock, the raster is produced at the host frame rate —
// so the F0-F15 M/N registers latch and read back for the debugger's
// benefit and no more.

// Which of the eight cells a DAC_REGS byte lane currently selects.
static uint32_t mach64_dac_rs(const mach64_t *m, uint32_t lane) {
    uint32_t ext = (m->reg[DW_DAC_CNTL] & DAC_EXT_SEL) ? 4u : 0u;
    return ext | (lane & 3u);
}

// The indexed register file, including the 64x64 2-bpp cursor array the
// datasheet maps at $0100-$04FF.
static uint8_t mach64_dac_indexed_read(mach64_t *m) {
    uint16_t idx = m->dac_index;
    if (idx >= sizeof(m->dac_indexed)) {
        LOG(1, "RGB514 indexed read past the file: $%04X", idx);
        return 0;
    }
    switch (idx) {
    case 0x00:
        return 0x00; // Revision (read-only)
    case 0x01:
        return 0x01; // ID (read-only, reset $01)
    case 0x82:
        // DAC Sense: a genuine read-only comparator.  The monitor detection
        // that matters here runs through the mach64's own pins (above), not
        // this, so return a stable value and log anyone who looks.
        LOG(2, "RGB514 DAC Sense read");
        return 0x00;
    default:
        return m->dac_indexed[idx];
    }
}

static void mach64_dac_indexed_write(mach64_t *m, uint8_t value) {
    uint16_t idx = m->dac_index;
    if (idx >= sizeof(m->dac_indexed)) {
        LOG(1, "RGB514 indexed write past the file: $%04X = $%02X", idx, value);
        return;
    }
    m->dac_indexed[idx] = value;
    LOG(3, "RGB514[$%04X] = $%02X", idx, value);
}

// DAC_REGS is one 32-bit register whose four BYTE lanes are separate DAC
// cells, so it is read and written a lane at a time; the dword paths below
// assemble and dissect around these.
static uint8_t mach64_dac_read_lane(mach64_t *m, uint32_t lane) {
    switch (mach64_dac_rs(m, lane)) {
    case 0: // palette address (write mode)
    case 3: // palette address (read mode)
        return m->clut_addr;
    case 1: { // palette data — R, G, B, then the entry auto-advances
        uint8_t v = m->clut[m->clut_addr][m->clut_phase];
        if (++m->clut_phase == 3) {
            m->clut_phase = 0;
            m->clut_addr++;
        }
        return v;
    }
    case 2:
        return m->dac_pixel_mask;
    case 4:
        return (uint8_t)(m->dac_index & 0xFFu);
    case 5:
        return (uint8_t)(m->dac_index >> 8);
    case 6:
        return mach64_dac_indexed_read(m);
    default: // RS 111 — index control (auto-increment)
        return m->dac_indexed[0x07];
    }
}

static void mach64_dac_write_lane(mach64_t *m, uint32_t lane, uint8_t value) {
    LOG(4, "DAC cell RS=%u (lane %u) = $%02X", mach64_dac_rs(m, lane), lane, value);
    switch (mach64_dac_rs(m, lane)) {
    case 0:
    case 3:
        m->clut_addr = value;
        m->clut_phase = 0;
        return;
    case 1:
        m->clut[m->clut_addr][m->clut_phase] = value;
        if (++m->clut_phase == 3) {
            m->clut_phase = 0;
            LOG(4, "CLUT[$%02X] = %02X %02X %02X", m->clut_addr, m->clut[m->clut_addr][0], m->clut[m->clut_addr][1],
                m->clut[m->clut_addr][2]);
            m->clut_addr++;
            mach64_clut_changed(m);
        }
        return;
    case 2:
        // The card's FCode sets this to $FF (no masking) at bring-up.
        m->dac_pixel_mask = value;
        return;
    case 4:
        m->dac_index_lo = value;
        m->dac_index = (uint16_t)((m->dac_index & 0xFF00u) | value);
        return;
    case 5:
        m->dac_index = (uint16_t)(((uint16_t)value << 8) | m->dac_index_lo);
        return;
    case 6:
        mach64_dac_indexed_write(m, value);
        return;
    default:
        m->dac_indexed[0x07] = value;
        return;
    }
}

static uint32_t mach64_dac_read(mach64_t *m) {
    uint32_t v = 0;
    for (uint32_t lane = 0; lane < 4; lane++)
        v |= (uint32_t)mach64_dac_read_lane(m, lane) << (8u * lane);
    return v;
}

static void mach64_dac_write(mach64_t *m, uint32_t value) {
    for (uint32_t lane = 0; lane < 4; lane++)
        mach64_dac_write_lane(m, lane, (uint8_t)(value >> (8u * lane)));
}

// ============================================================
// The aperture
// ============================================================

// MEM_CNTL's MEM_SIZE encoding for the buffer this card actually has.
static uint32_t mach64_mem_size_code(const mach64_t *m) {
    return (m->vram_size >= MACH64_VRAM_4MB) ? MEM_SIZE_4M : MEM_SIZE_2M;
}

// How much of BAR0 the aperture currently covers, per CFG_MEM_AP_SIZE.
static uint32_t mach64_aperture_size(const mach64_t *m) {
    switch (m->reg[DW_CONFIG_CNTL] & CFG_MEM_AP_SIZE) {
    case 1:
        return MACH64_APER_4MB;
    case 2:
        return MACH64_APER_8MB;
    default:
        return 0; // aperture disabled
    }
}

// Where the 1 KB register alias sits inside the aperture.
static uint32_t mach64_mmio_offset(const mach64_t *m) {
    return (mach64_aperture_size(m) == MACH64_APER_4MB) ? MACH64_MMIO_OFF_4MB : MACH64_MMIO_OFF_8MB;
}

static void mach64_aperture_changed(mach64_t *m) {
    uint32_t size = mach64_aperture_size(m);
    // CFG_MEM_AP_LOC is an ABSOLUTE address in 4 MB units, and the card's
    // own FCode programs it as (BAR0_base >> 18) | 2 — i.e. the aperture
    // lands exactly on the assigned BAR0 base.  BAR0 is authoritative for
    // the PCI decode (that is what a BAR means), so LOC only selects which
    // sub-range of it maps VRAM.  If a guest ever programs a LOC that is
    // NOT the BAR0 base, that is a real inconsistency between our BAR
    // assignment and its arithmetic — say so loudly rather than silently
    // "fixing it up", which is the failure mode this rule exists for.
    uint32_t loc = ((m->reg[DW_CONFIG_CNTL] & CFG_MEM_AP_LOC) >> 4) * MACH64_APER_4MB;
    uint32_t bar0 = pci_cfg_bar_base(m->dev, MACH64_BAR_APER);
    if (size && bar0 && loc != bar0) {
        LOG(0,
            "CFG_MEM_AP_LOC places the aperture at $%08X but BAR0 is assigned at $%08X — "
            "keeping the BAR-relative view; one of the two is wrong",
            loc, bar0);
    }
    LOG(2, "aperture %s: %u MB at BAR0 base $%08X, register alias at +$%06X", size ? "enabled" : "DISABLED", size >> 20,
        bar0, mach64_mmio_offset(m));
}

// ============================================================
// The draw engine
// ============================================================
//
// §4.8 of the proposal reasoned this could stay store-and-readback: classic
// Mac OS draws through QuickDraw into the framebuffer, and the accelerated
// paths were expected to come from the separately installed ATI Graphics
// Accelerator extension, with the in-ROM ndrv being a display driver only.
//
// The first System 7.6 boot disproved that.  The ndrv issues a steady
// stream of engine operations — 3 467 register writes in a 200 M
// instruction window, overwhelmingly solid rectangle fills — and with the
// engine inert the desktop came up with the startup splash still painted
// over it, because the regions the driver clears through the blitter never
// cleared.  So the engine is modelled, from the register traffic the
// driver actually produces:
//
//   DP_FRGD_CLR / DP_BKGD_CLR   the colours
//   DP_SRC                      which generator feeds the foreground
//   DP_MIX                      the raster op (7 = source, plain overwrite)
//   DST_OFF_PITCH               destination base and pitch — the SAME
//                               $14100040 the CRTC uses, which independently
//                               confirms the field split in mach64_update()
//   DST_Y_X, DST_HEIGHT_WIDTH   the trajectory
//
// The TRIGGER is whichever write supplies DST_WIDTH.  That is not a guess:
// the trace shows both shapes, `DST_Y_X` then `DST_HEIGHT_WIDTH` for a
// rectangle, and `DST_HEIGHT` once then `DST_Y_X`/`DST_WIDTH` repeatedly
// for a run of single-pixel-tall lines.  Treating DST_Y_X as the trigger
// would fire every operation twice, once with a stale size.

// A destination or source surface, decoded from an OFF_PITCH register.
// Same layout as CRTC_OFF_PITCH: pitch in bits 31:22 (pixels / 8), offset
// in bits 19:0 (units of 8 bytes).
typedef struct mach64_surface {
    uint32_t base; // byte offset into VRAM
    uint32_t pitch; // bytes per row
} mach64_surface_t;

static mach64_surface_t mach64_surface(const mach64_t *m, int off_pitch_dw, uint32_t bpp) {
    uint32_t v = m->reg[off_pitch_dw];
    mach64_surface_t s;
    s.base = (v & 0xFFFFFu) * 8u;
    s.pitch = ((v >> 22) & 0x3FFu) * 8u * bpp;
    return s;
}

// Byte address of a pixel, or -1 when it falls outside VRAM.
static int64_t mach64_pixel_at(const mach64_t *m, const mach64_surface_t *s, uint32_t x, uint32_t y, uint32_t bpp) {
    uint64_t off = (uint64_t)s->base + (uint64_t)y * s->pitch + (uint64_t)x * bpp;
    if (off + bpp > m->vram_size)
        return -1;
    return (int64_t)off;
}

// The pixel ALU (PRG, "Source and Destination Mixing Logic").  Every
// function in ATI's table, so nothing has to be approximated by "source".
static uint32_t mach64_mix(uint32_t fn, uint32_t s, uint32_t d, uint32_t bpp) {
    switch (fn) {
    case 0x0:
        return ~d;
    case 0x1:
        return 0u;
    case 0x2:
        return ~0u;
    case 0x3:
        return d;
    case 0x4:
        return ~s;
    case 0x5:
        return d ^ s;
    case 0x6:
        return (~d) ^ s;
    case 0x7:
        return s;
    case 0x8:
        return (~d) | (~s);
    case 0x9:
        return d | (~s);
    case 0xA:
        return (~d) | s;
    case 0xB:
        return d | s;
    case 0xC:
        return d & s;
    case 0xD:
        return (~d) & s;
    case 0xE:
        return d & (~s);
    case 0xF:
        return (~d) & (~s);
    case DP_MIX_AVERAGE: {
        // (D+S) >> 1 with DP_CHAIN_MSK breaking the carry chain.  At every
        // depth this model renders, the documented mask value breaks it at
        // byte boundaries, so a per-byte average IS the specified result.
        uint32_t out = 0;
        for (uint32_t i = 0; i < bpp; i++) {
            uint32_t sh = 8u * i;
            uint32_t avg = ((((s >> sh) & 0xFFu) + ((d >> sh) & 0xFFu)) >> 1) & 0xFFu;
            out |= avg << sh;
        }
        return out;
    }
    default:
        return s;
    }
}

// Read one pixel out of VRAM (big-endian bytes) as a value.
static uint32_t mach64_get_pixel(const mach64_t *m, int64_t at, uint32_t bpp) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < bpp; i++)
        v = (v << 8) | m->vram[at + i];
    return v;
}

static void mach64_put_pixel(mach64_t *m, int64_t at, uint32_t colour, uint32_t bpp, uint32_t mask) {
    for (uint32_t i = 0; i < bpp; i++) {
        // The framebuffer is big-endian bytes; the colour registers hold the
        // pixel value little-endian-wise, so byte i of an n-byte pixel is
        // value bits 8*(n-1-i).
        uint32_t shift = 8u * (bpp - 1u - i);
        uint8_t mbyte = (uint8_t)(mask >> shift);
        if (!mbyte)
            continue;
        uint8_t *dst = &m->vram[at + i];
        *dst = (uint8_t)((*dst & ~mbyte) | ((colour >> shift) & mbyte));
    }
}

// The monochrome mux: one bit per pixel, deciding which colour source and
// which mix this pixel gets (RRG p. 3-40, PRG "Logical Pixel Data Path").
static bool mach64_mono_bit(mach64_t *m, uint32_t mono_sel, uint32_t x, uint32_t y, uint32_t col, uint32_t row,
                            const mach64_surface_t *src, uint32_t bpp) {
    (void)col;
    (void)row;
    switch (mono_sel) {
    case DP_MONO_ALWAYS_1:
        return true;
    case DP_MONO_PATTERN: {
        // An 8 x 8 bit pattern held in PAT_REG0 (rows 0-3) and PAT_REG1
        // (rows 4-7), addressed by the DESTINATION coordinates — which is
        // what makes a dithered fill line up across separate operations.
        uint32_t r = y & 7u;
        uint32_t word = (r < 4u) ? m->reg[DW_PAT_REG0] : m->reg[DW_PAT_REG1];
        uint32_t byte = (word >> (8u * (r & 3u))) & 0xFFu;
        return ((byte >> (7u - (x & 7u))) & 1u) != 0;
    }
    case DP_MONO_BLIT: {
        // The blit source area read as one bit per pixel.
        int64_t from = mach64_pixel_at(m, src, x, y, bpp);
        return from >= 0 && mach64_get_pixel(m, from, bpp) != 0;
    }
    default:
        return true;
    }
}

// Everything the pixel loop needs, gathered once per operation.  The
// streamed host-data path re-gathers it per HOST_DATA write, which is
// correct: a driver does not change the data path mid-stream, and reading
// it fresh means the two paths cannot drift apart.
typedef struct mach64_op {
    uint32_t bpp;
    mach64_surface_t dst, src;
    uint32_t frgd_sel, bkgd_sel, mono_sel;
    uint32_t frgd_mix, bkgd_mix;
    uint32_t sc_left, sc_right, sc_top, sc_bottom;
    uint32_t mask;
    uint32_t src_x0, src_y0, src_w, src_h;
    uint32_t cmp_fn, cmp_clr, cmp_msk;
    bool cmp_on_src;
} mach64_op_t;

static void mach64_op_gather(mach64_t *m, mach64_op_t *op) {
    op->bpp = mach64_bytes_per_pixel(m);
    op->dst = mach64_surface(m, DW_DST_OFF_PITCH, op->bpp);
    op->src = mach64_surface(m, DW_SRC_OFF_PITCH, op->bpp);
    op->frgd_sel = DP_FRGD_SRC(m->reg[DW_DP_SRC]);
    op->bkgd_sel = DP_BKGD_SRC(m->reg[DW_DP_SRC]);
    op->mono_sel = DP_MONO_SRC(m->reg[DW_DP_SRC]);
    op->frgd_mix = DP_FRGD_MIX(m->reg[DW_DP_MIX]);
    op->bkgd_mix = DP_BKGD_MIX(m->reg[DW_DP_MIX]);
    op->sc_left = m->reg[DW_SC_LEFT] & 0xFFFFu;
    op->sc_right = m->reg[DW_SC_RIGHT] & 0xFFFFu;
    op->sc_top = m->reg[DW_SC_TOP] & 0xFFFFu;
    op->sc_bottom = m->reg[DW_SC_BOTTOM] & 0xFFFFu;
    op->mask = m->reg[DW_DP_WRITE_MSK];
    op->src_x0 = m->reg[DW_SRC_X] & 0xFFFFu;
    op->src_y0 = m->reg[DW_SRC_Y] & 0xFFFFu;
    op->src_w = m->reg[DW_SRC_WIDTH1] & 0xFFFFu;
    op->src_h = m->reg[DW_SRC_HEIGHT1] & 0xFFFFu;
    op->cmp_fn = CLR_CMP_FN(m->reg[DW_CLR_CMP_CNTL]);
    op->cmp_clr = m->reg[DW_CLR_CMP_CLR];
    op->cmp_msk = m->reg[DW_CLR_CMP_MSK];
    op->cmp_on_src = CLR_CMP_SRC(m->reg[DW_CLR_CMP_CNTL]) != 0;
}

// One pixel through the whole data path: mono mux -> colour mux -> ALU ->
// colour compare -> write mask.  `mono` is supplied by the caller because
// the two paths source it differently (pattern/blit versus a host stream).
static void mach64_emit(mach64_t *m, const mach64_op_t *op, uint32_t x, uint32_t y, uint32_t col, uint32_t row,
                        bool mono) {
    if (x < op->sc_left || x > op->sc_right || y < op->sc_top || y > op->sc_bottom)
        return;
    int64_t at = mach64_pixel_at(m, &op->dst, x, y, op->bpp);
    if (at < 0)
        return;

    uint32_t colour_sel = mono ? op->frgd_sel : op->bkgd_sel;
    uint32_t mix = mono ? op->frgd_mix : op->bkgd_mix;
    uint32_t source;
    switch (colour_sel) {
    case DP_SRC_BKGD_CLR:
        source = m->reg[DW_DP_BKGD_CLR];
        break;
    case DP_SRC_BLIT: {
        uint32_t sx = op->src_x0 + (op->src_w ? (col % op->src_w) : col);
        uint32_t sy = op->src_y0 + (op->src_h ? (row % op->src_h) : row);
        int64_t from = mach64_pixel_at(m, &op->src, sx, sy, op->bpp);
        if (from < 0)
            return;
        source = mach64_get_pixel(m, from, op->bpp);
        break;
    }
    case DP_SRC_FRGD_CLR:
    case DP_SRC_PATTERN:
    default:
        source = m->reg[DW_DP_FRGD_CLR];
        break;
    }

    uint32_t dest = mach64_get_pixel(m, at, op->bpp);
    // A TRUE comparison leaves the destination as it is.
    if (op->cmp_fn == CLR_CMP_NE || op->cmp_fn == CLR_CMP_EQ) {
        uint32_t probe = (op->cmp_on_src ? source : dest) & op->cmp_msk;
        bool equal = probe == (op->cmp_clr & op->cmp_msk);
        if ((op->cmp_fn == CLR_CMP_EQ) == equal)
            return;
    } else if (op->cmp_fn == CLR_CMP_TRUE) {
        return;
    }
    mach64_put_pixel(m, at, mach64_mix(mix, source, dest, op->bpp), op->bpp, op->mask);
}

// Run one trajectory.  `width`/`height` come from whichever register wrote
// last; everything else is the standing data-path state.
static void mach64_engine_run(mach64_t *m) {
    uint32_t width = m->reg[DW_DST_WIDTH] & 0xFFFFu;
    uint32_t height = m->reg[DW_DST_HEIGHT] & 0xFFFFu;
    if (!width || !height)
        return;

    mach64_op_t op;
    mach64_op_gather(m, &op);
    if ((op.frgd_mix > 0xFu && op.frgd_mix != DP_MIX_AVERAGE) && !m->mix_warned) {
        m->mix_warned = true;
        LOG(0,
            "draw engine: DP_MIX foreground function $%02X is not in ATI's table — treating it as "
            "plain source, which will draw the wrong thing wherever it is used",
            op.frgd_mix);
    }

    uint32_t dst_x = m->reg[DW_DST_X] & 0xFFFFu;
    uint32_t dst_y = m->reg[DW_DST_Y] & 0xFFFFu;

    // A host-fed operation does not draw now: its pixels arrive later, one
    // HOST_DATA write at a time.  Arm the cursor and return.
    if (op.mono_sel == DP_MONO_HOST || op.frgd_sel == DP_SRC_HOST || op.bkgd_sel == DP_SRC_HOST) {
        m->host_op.active = true;
        m->host_op.x0 = dst_x;
        m->host_op.y0 = dst_y;
        m->host_op.w = width;
        m->host_op.h = height;
        m->host_op.col = 0;
        m->host_op.row = 0;
        LOG(4, "host-data operation armed: %ux%u at (%u,%u)", width, height, dst_x, dst_y);
        return;
    }

    uint32_t cntl = m->reg[DW_DST_CNTL];
    bool x_inc = (cntl & DST_X_DIR) != 0;
    bool y_inc = (cntl & DST_Y_DIR) != 0;
    for (uint32_t row = 0; row < height; row++) {
        uint32_t y = y_inc ? dst_y + row : dst_y - row;
        for (uint32_t col = 0; col < width; col++) {
            uint32_t x = x_inc ? dst_x + col : dst_x - col;
            bool mono = mach64_mono_bit(m, op.mono_sel, x, y, col, row, &op.src, op.bpp);
            mach64_emit(m, &op, x, y, col, row, mono);
        }
    }
    m->blits++;
    m->display.fb_dirty = true;
    LOG(3, "op #%llu %ux%u at (%u,%u) mono=%u frgd=%u/%X bkgd=%u/%X clr=$%08X/$%08X", (unsigned long long)m->blits,
        width, height, dst_x, dst_y, op.mono_sel, op.frgd_sel, op.frgd_mix, op.bkgd_sel, op.bkgd_mix,
        m->reg[DW_DP_FRGD_CLR], m->reg[DW_DP_BKGD_CLR]);
}

// Feed one dword of host data into the operation in flight.
//
// Bit order: DP_BYTE_PIX_ORDER (DP_PIX_WIDTH bit 31) selects MSBit-first
// (0, the value the driver programs) or LSBit-first within each byte, and
// bytes are consumed low-address first — which for this little-endian
// register domain is the dword's low byte first.
//
// HOST_CNTL's HOST_BYTE_ALIGN makes consumption jump to the next byte
// boundary whenever the trajectory steps in Y, so a glyph whose width is
// not a multiple of 8 still starts each row on a fresh byte.
static void mach64_host_feed(mach64_t *m, uint32_t value) {
    if (!m->host_op.active)
        return;
    mach64_op_t op;
    mach64_op_gather(m, &op);
    bool lsb_first = (m->reg[DW_DP_PIX_WIDTH] & 0x80000000u) != 0;
    bool byte_align = (m->reg[DW_HOST_CNTL] & 0x1u) != 0;

    for (uint32_t bit = 0; bit < 32u && m->host_op.active; bit++) {
        uint32_t byte = bit >> 3;
        uint32_t within = bit & 7u;
        uint32_t shift = 8u * byte + (lsb_first ? within : (7u - within));
        bool mono = ((value >> shift) & 1u) != 0;

        uint32_t x = m->host_op.x0 + m->host_op.col;
        uint32_t y = m->host_op.y0 + m->host_op.row;
        mach64_emit(m, &op, x, y, m->host_op.col, m->host_op.row, mono);

        if (++m->host_op.col >= m->host_op.w) {
            m->host_op.col = 0;
            if (++m->host_op.row >= m->host_op.h) {
                m->host_op.active = false;
                m->blits++;
                m->display.fb_dirty = true;
                LOG(3, "op #%llu HOST %ux%u at (%u,%u)", (unsigned long long)m->blits, m->host_op.w, m->host_op.h,
                    m->host_op.x0, m->host_op.y0);
                return;
            }
            if (byte_align)
                bit = ((bit >> 3) + 1u) * 8u - 1u; // resume on a byte boundary
        }
    }
}

// The engine's register writes.  Returns true when the write was the
// engine's business, so the generic store below is skipped.
static bool mach64_engine_write(mach64_t *m, int dw, uint32_t value) {
    switch (dw) {
    // FIELD ORDER.  In ATI's combined `A_B` register names the FIRST-named
    // field is the LOW halfword and the second is the HIGH halfword, so
    // DST_Y_X is (X << 16) | Y and DST_HEIGHT_WIDTH is (WIDTH << 16) |
    // HEIGHT.  Reading them the other way round is self-consistent for
    // most traffic — a scrollbar drawn as four lines looks equally
    // plausible transposed — which is exactly why it survived until
    // something GEOMETRICALLY IMPOSSIBLE turned up: the desktop erase,
    // $00140000/$028001C7, which transposed reads as a 640-pixel-tall
    // rectangle on a 480-line screen.  Read correctly it is 640 x 455 at
    // (0,20) — the desktop below the menu bar — followed by 638x2, 636x1,
    // 634x1 and 630x1 at y = 475..479, the desktop's rounded bottom
    // corners.  The SC_LEFT_RIGHT decode below always used this order and
    // was corroborated independently ($027F0000 = left 0, right 639).
    case DW_DST_Y_X:
        m->reg[DW_DST_X] = (value >> 16) & 0xFFFFu;
        m->reg[DW_DST_Y] = value & 0xFFFFu;
        m->reg[dw] = value;
        return true;
    case DW_DST_HEIGHT_WIDTH: // and GO
        m->reg[DW_DST_WIDTH] = (value >> 16) & 0xFFFFu;
        m->reg[DW_DST_HEIGHT] = value & 0xFFFFu;
        m->reg[dw] = value;
        mach64_engine_run(m);
        return true;
    case DW_DST_X_WIDTH: // and GO
        m->reg[DW_DST_WIDTH] = (value >> 16) & 0xFFFFu;
        m->reg[DW_DST_X] = value & 0xFFFFu;
        m->reg[dw] = value;
        mach64_engine_run(m);
        return true;
    case DW_DST_WIDTH: // supplies the width on its own — and GO
        m->reg[dw] = value;
        mach64_engine_run(m);
        return true;
    case DW_SRC_Y_X:
        m->reg[DW_SRC_X] = (value >> 16) & 0xFFFFu;
        m->reg[DW_SRC_Y] = value & 0xFFFFu;
        m->reg[dw] = value;
        return true;
    case DW_SRC_HEIGHT1_WIDTH1:
        m->reg[DW_SRC_WIDTH1] = (value >> 16) & 0xFFFFu;
        m->reg[DW_SRC_HEIGHT1] = value & 0xFFFFu;
        m->reg[dw] = value;
        return true;
    // The combined scissor registers: high halfword is the far edge.
    case DW_SC_LEFT_RIGHT:
        m->reg[DW_SC_RIGHT] = (value >> 16) & 0xFFFFu;
        m->reg[DW_SC_LEFT] = value & 0xFFFFu;
        m->reg[dw] = value;
        return true;
    case DW_SC_TOP_BOTTOM:
        m->reg[DW_SC_BOTTOM] = (value >> 16) & 0xFFFFu;
        m->reg[DW_SC_TOP] = value & 0xFFFFu;
        m->reg[dw] = value;
        return true;
    case DW_CONTEXT_LOAD_CNTL:
        // NOT MODELLED, and this is the largest known gap in the engine.
        //
        // The mach64 can save its whole draw-engine state to a 64-DWORD
        // block in memory and reload it with one write (PRG, "Draw Engine
        // Contexts"); DP_MIX is DWORD $16 of that block and DP_SRC is $17.
        // System 7.6's ATI accelerator uses it almost exclusively: one boot
        // issues 941 CONTEXT_LOAD_CNTL writes against 8 direct DP_SRC and 4
        // DP_MIX writes.  Ignoring it leaves the data-path selectors at
        // whatever was last written directly, so operations that should be
        // a patterned two-colour fill run as "invert the destination" — the
        // desktop erase is one of them, which is why the startup splash
        // survives on screen.
        //
        // Two facts are still missing and neither should be guessed at:
        //   * the CONTEXT_LOAD_CNTL field layout.  The driver writes
        //     $00FFC008 / $00FFC00C / $00FFC01E / $00FFC038, and under the
        //     RRG's bit chart (p. 3-15) those all decode as command 3,
        //     "load and initiate Bresenham line", which cannot be right for
        //     traffic that draws rectangles.  That chart is one of the
        //     OCR'd bit-position tables, and its column alignment is not
        //     trustworthy.
        //   * where the blocks live.  ATI says "reverse order from top of
        //     memory", but scanning every 256-byte boundary of VRAM for a
        //     block whose DWORD 2 matches the live DST_OFF_PITCH finds
        //     nothing, at any point in the boot.
        //
        // Until both are pinned from a readable source, loading a context
        // from the wrong address would scribble arbitrary VRAM into the
        // data path — strictly worse than leaving it alone.
        m->reg[dw] = value;
        return true;
    default:
        if (dw >= DW_HOST_DATA0 && dw <= DW_HOST_DATA_LAST) {
            m->reg[dw] = value;
            mach64_host_feed(m, value);
            return true;
        }
        return false;
    }
}

// ============================================================
// The sparse I/O face
// ============================================================
//
// The bus hands this handler the raw PCI I/O address (the region's base is
// 0), so the card does its own decode: the top six bits are the register
// select, the bottom two are the byte lane within a 32-bit register.  The
// congruence match — only addresses whose low ten bits equal the strapped
// base — is applied by the bus (pci_device_add_fixed_region), so anything
// arriving here is genuinely ours.
//
// Byte order is the card's own: the register value is little-endian, so
// lane j of an address carries value bits 8j+7:8j.

static uint8_t io_read8(void *ctx, uint32_t addr) {
    mach64_t *m = (mach64_t *)ctx;
    int dw = io_dword_for_select(addr >> 10);
    if (dw < 0) {
        LOG(1, "I/O read of unassigned select $%02X (address $%04X)", addr >> 10, addr);
        return 0xFFu;
    }
    return mach64_reg_read_lane(m, dw, addr & 3u);
}

static void io_write8(void *ctx, uint32_t addr, uint8_t value) {
    mach64_t *m = (mach64_t *)ctx;
    int dw = io_dword_for_select(addr >> 10);
    if (dw < 0) {
        LOG(1, "I/O write of unassigned select $%02X (address $%04X) = $%02X", addr >> 10, addr, value);
        return;
    }
    // Lane-wise, so byte and halfword pokes of a 32-bit register work —
    // which the card's own firmware relies on — without disturbing the
    // other lanes of a register whose lanes are independent cells.
    mach64_reg_write_lane(m, dw, addr & 3u, value);
}

static uint16_t io_read16(void *ctx, uint32_t addr) {
    return (uint16_t)((io_read8(ctx, addr) << 8) | io_read8(ctx, addr + 1));
}

static void io_write16(void *ctx, uint32_t addr, uint16_t value) {
    io_write8(ctx, addr, (uint8_t)(value >> 8));
    io_write8(ctx, addr + 1, (uint8_t)value);
}

static uint32_t io_read32(void *ctx, uint32_t addr) {
    mach64_t *m = (mach64_t *)ctx;
    int dw = io_dword_for_select(addr >> 10);
    if (dw < 0) {
        LOG(1, "I/O read of unassigned select $%02X (address $%04X)", addr >> 10, addr);
        return 0xFFFFFFFFu;
    }
    return MACH64_LE32(mach64_reg_read(m, dw));
}

static void io_write32(void *ctx, uint32_t addr, uint32_t value) {
    mach64_t *m = (mach64_t *)ctx;
    int dw = io_dword_for_select(addr >> 10);
    if (dw < 0) {
        LOG(1, "I/O write of unassigned select $%02X (address $%04X) = $%08X", addr >> 10, addr, value);
        return;
    }
    mach64_reg_write(m, dw, MACH64_LE32(value));
}

// ============================================================
// BAR0 — the aperture: framebuffer, register alias, and slack
// ============================================================
//
//   BAR0 + $000000 .. $7FFBFF   linear framebuffer (the 8 MB aperture)
//   BAR0 + $7FFC00 .. $7FFFFF   1 KB memory-mapped register alias
//   BAR0 + $800000 .. $FFFFFF   the same 8 MB aperture again
//
// The card asks for a 16 MB BAR because CFG_MEM_AP_LOC has 4 MB
// granularity while an 8 MB aperture needs 8 MB alignment, so 16 MB
// guarantees a legal placement wherever Open Firmware puts it.  What the
// upper half then does was the open question, and the proposal reasoned it
// was alignment SLACK decoding nothing — modelled that way deliberately,
// with a loud log on any access, so that a wrong guess would announce
// itself instead of silently rendering garbage.
//
// It announced itself on the first System 7.6 boot: the driver reads
// BAR0 + $800200 — the same 512-byte framebuffer offset CRTC_OFFSET points
// at, 8 MB higher.  With the upper half decoding nothing, its drawing went
// nowhere and the desktop came up as a uniform field of colour-index 0.
// The aperture REPEATS through the BAR, which is the same shape Control
// has (a 64 MB aperture repeating an 8 MB view, with the usable
// framebuffer in the +$800000 half).  So the upper 8 MB is modelled as
// what it evidently is: another view of the same memory.

// Where an aperture offset lands.  Returns -1 for the register alias and
// -2 for the undecoded slack; otherwise a VRAM offset.
#define MACH64_APER_REGS (-1)
#define MACH64_APER_NONE (-2)

static void aper_repeat_note(mach64_t *m, uint32_t offset);

static int64_t aper_map(mach64_t *m, uint32_t offset) {
    uint32_t size = mach64_aperture_size(m);
    if (size == 0) {
        // CFG_MEM_AP_SIZE = 0.  The manual places the registers "at the
        // base address of the aperture", which is ambiguous when there is
        // no aperture; the alias answers regardless, because that costs
        // nothing and a driver poking it would otherwise wedge.  Log it, so
        // we learn whether it ever matters.
        LOG(2, "aperture access at +$%06X with CFG_MEM_AP_SIZE = 0", offset);
        if ((offset & ~(MACH64_MMIO_BLOCK_LEN - 1u)) == MACH64_MMIO_OFF_8MB)
            return MACH64_APER_REGS;
        return MACH64_APER_NONE;
    }
    if (offset >= size) {
        aper_repeat_note(m, offset);
        offset %= size; // the aperture repeats through the 16 MB BAR
    }
    if (offset >= mach64_mmio_offset(m))
        return MACH64_APER_REGS;
    // VRAM smaller than the aperture aliases through it, which is what
    // makes the driver's write/read-back sizing probe find the real size.
    return (int64_t)(offset & (m->vram_size - 1u));
}

// Say so, once, when the guest reaches past the first copy of the aperture
// — the observation that settled what the upper half of the BAR is.
static void aper_repeat_note(mach64_t *m, uint32_t offset) {
    if (m->aperture_warned)
        return;
    m->aperture_warned = true;
    LOG(1,
        "access at BAR0 +$%06X is past the %u MB aperture; serving it as the aperture repeated "
        "through the 16 MB BAR (the driver puts its framebuffer in the upper half)",
        offset, mach64_aperture_size(m) >> 20);
}

static uint8_t aper_read8(void *ctx, uint32_t offset) {
    mach64_t *m = (mach64_t *)ctx;
    int64_t at = aper_map(m, offset);
    if (at == MACH64_APER_NONE)
        return 0xFFu;
    if (at == MACH64_APER_REGS)
        return mach64_reg_read_lane(m, (int)((offset & 0x3FFu) >> 2), offset & 3u);
    return m->vram[at];
}

static void aper_write8(void *ctx, uint32_t offset, uint8_t value) {
    mach64_t *m = (mach64_t *)ctx;
    int64_t at = aper_map(m, offset);
    if (at == MACH64_APER_NONE)
        return;
    if (at == MACH64_APER_REGS) {
        mach64_reg_write_lane(m, (int)((offset & 0x3FFu) >> 2), offset & 3u, value);
        return;
    }
    m->vram[at] = value;
}

static uint16_t aper_read16(void *ctx, uint32_t offset) {
    mach64_t *m = (mach64_t *)ctx;
    int64_t at = aper_map(m, offset);
    // Inside VRAM the aperture is a plain byte array on a big-endian bus:
    // a guest halfword load reads two consecutive bytes, MSB first.  Only
    // the REGISTER alias is little-endian.
    if (at >= 0)
        return (uint16_t)(((uint16_t)m->vram[at] << 8) | m->vram[(at + 1) & (m->vram_size - 1u)]);
    return (uint16_t)((aper_read8(ctx, offset) << 8) | aper_read8(ctx, offset + 1));
}

static void aper_write16(void *ctx, uint32_t offset, uint16_t value) {
    mach64_t *m = (mach64_t *)ctx;
    int64_t at = aper_map(m, offset);
    if (at >= 0) {
        m->vram[at] = (uint8_t)(value >> 8);
        m->vram[(at + 1) & (m->vram_size - 1u)] = (uint8_t)value;
        return;
    }
    aper_write8(ctx, offset, (uint8_t)(value >> 8));
    aper_write8(ctx, offset + 1, (uint8_t)value);
}

static uint32_t aper_read32(void *ctx, uint32_t offset) {
    mach64_t *m = (mach64_t *)ctx;
    int64_t at = aper_map(m, offset);
    if (at == MACH64_APER_NONE)
        return 0xFFFFFFFFu;
    if (at == MACH64_APER_REGS)
        return MACH64_LE32(mach64_reg_read(m, (int)((offset & 0x3FFu) >> 2)));
    return ((uint32_t)m->vram[at] << 24) | ((uint32_t)m->vram[(at + 1) & (m->vram_size - 1u)] << 16) |
           ((uint32_t)m->vram[(at + 2) & (m->vram_size - 1u)] << 8) | m->vram[(at + 3) & (m->vram_size - 1u)];
}

static void aper_write32(void *ctx, uint32_t offset, uint32_t value) {
    mach64_t *m = (mach64_t *)ctx;
    int64_t at = aper_map(m, offset);
    if (at == MACH64_APER_NONE)
        return;
    if (at == MACH64_APER_REGS) {
        mach64_reg_write(m, (int)((offset & 0x3FFu) >> 2), MACH64_LE32(value));
        return;
    }
    m->vram[at] = (uint8_t)(value >> 24);
    m->vram[(at + 1) & (m->vram_size - 1u)] = (uint8_t)(value >> 16);
    m->vram[(at + 2) & (m->vram_size - 1u)] = (uint8_t)(value >> 8);
    m->vram[(at + 3) & (m->vram_size - 1u)] = (uint8_t)value;
}

// ============================================================
// The expansion ROM (config $30)
// ============================================================
// A plain big-endian byte array.  The declared size is the physical 32 KB
// chip, which also covers the shorter image the PCI Data Structure
// declares; reads past the image return $FF, as an unprogrammed chip does.

static uint8_t rom_read8(void *ctx, uint32_t offset) {
    mach64_t *m = (mach64_t *)ctx;
    if (!m->dev->rom || offset >= m->dev->rom_size)
        return 0xFFu;
    return m->dev->rom[offset];
}

static uint16_t rom_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((rom_read8(ctx, offset) << 8) | rom_read8(ctx, offset + 1));
}

static uint32_t rom_read32(void *ctx, uint32_t offset) {
    return ((uint32_t)rom_read8(ctx, offset) << 24) | ((uint32_t)rom_read8(ctx, offset + 1) << 16) |
           ((uint32_t)rom_read8(ctx, offset + 2) << 8) | rom_read8(ctx, offset + 3);
}

static void rom_write8(void *ctx, uint32_t offset, uint8_t value) {
    (void)ctx;
    LOG(2, "write to the expansion ROM at +$%04X = $%02X — ignored (it is a ROM)", offset, value);
}

static void rom_write16(void *ctx, uint32_t offset, uint16_t value) {
    rom_write8(ctx, offset, (uint8_t)(value >> 8));
}

static void rom_write32(void *ctx, uint32_t offset, uint32_t value) {
    rom_write8(ctx, offset, (uint8_t)(value >> 24));
}

// ============================================================
// Scanout position
// ============================================================

// CRTC_VLINE_CRNT_VLINE's live half: the current scan line, derived from
// the frame phase against a nominal 60 Hz.  It only has to ADVANCE — a
// driver polling for vertical blank spins forever otherwise — so the
// nominal frame is enough and no pixel clock is needed.
// Which scan line the raster is on, as a phase of a nominal 60 Hz frame
// against the programmed vertical total.  Only monotonic-within-frame
// matters: nothing here consumes a pixel clock, the raster is produced at
// the host frame rate.
static uint32_t mach64_scanline(const mach64_t *m, uint32_t *out_vtotal) {
    uint32_t vtotal = (m->reg[DW_CRTC_V_TOTAL_DISP] & 0x7FFu) + 1u;
    if (vtotal < 2u || vtotal > 4096u)
        vtotal = 525u;
    if (out_vtotal)
        *out_vtotal = vtotal;
    uint64_t frame = m->cfg->machine->freq / 60u;
    if (!frame)
        return 0;
    uint64_t pos = scheduler_cpu_cycles(m->cfg->scheduler) % frame;
    return (uint32_t)(pos * vtotal / frame);
}

static uint32_t mach64_current_vline(const mach64_t *m) {
    uint32_t line = mach64_scanline(m, NULL);
    // The register pairs the programmed compare line (15:0) with the live
    // current line (bits 26:16).
    return (m->reg[DW_CRTC_VLINE_CRNT_VLINE] & 0xFFFFu) | ((line & 0x7FFu) << 16);
}

// CRTC_INT_CNTL bit 0 is the LIVE vertical-blank state, not a latch — it
// is high while the raster is past the last displayed line and it toggles
// every frame whether or not any interrupt is enabled.
//
// This one is load-bearing, and finding out why cost a whole boot: the
// System's video driver spins on it
//
//     lwz r3,0(r4) ; li r4,24 ; bl <read8> ; andi. r3,r3,1 ; beq -
//
// reading byte $18 of the memory-mapped register block — CRTC_INT_CNTL,
// bit 0 — and waits for it to go high before it will proceed.  With the
// bit wired to a latch that only the interrupt path ever set, the driver
// spun there forever and System 7.6 never reached the mount, while the
// card looked perfectly healthy by every other measure.  A live status bit
// that never changes is the same trap the proposal flagged for
// CRTC_VLINE_CRNT_VLINE.
static bool mach64_in_vblank(const mach64_t *m) {
    uint32_t vtotal = 0;
    uint32_t line = mach64_scanline(m, &vtotal);
    uint32_t v_disp = ((m->reg[DW_CRTC_V_TOTAL_DISP] >> 16) & 0x7FFu) + 1u;
    if (v_disp == 0 || v_disp >= vtotal)
        v_disp = vtotal - (vtotal / 16u); // unprogrammed: a plausible blank
    return line >= v_disp;
}

// ============================================================
// Scanout — the CRTC registers become a display_t
// ============================================================
//
// Field positions are the RRG's per-register bit charts (pages 3-17/3-18,
// 3-20, 3-22, 3-24), and the arithmetic has an external check: Apple's
// TN1062 dump of this card reports width $280, height $1E0, depth 8 and
// linebytes $280, so the 640 x 480 mode line must produce H_DISP = 79,
// V_DISP = 479 and CRTC_PITCH = 80.
//
//   width   = ((CRTC_H_TOTAL_DISP >> 16) & $FF)  + 1, in 8-pixel characters
//   height  = ((CRTC_V_TOTAL_DISP >> 16) & $7FF) + 1  lines
//   stride  = ((CRTC_OFF_PITCH    >> 22) & $3FF) x 8 x bytes-per-pixel
//   base    =  (CRTC_OFF_PITCH & $FFFFF) x 8 bytes into VRAM
//   enabled = CRTC_EN && !CRTC_DISPLAY_DIS
//
// CRTC_PITCH is bits 31:22, NOT 31:20, and the external check is what
// settles it: booted on a pm9500 the guest programs CRTC_OFF_PITCH =
// $14100040, and only the 10-bit read gives 80 — the CRTC_PITCH that makes
// linebytes 80 x 8 = $280, exactly what TN1062 reports for this card.  The
// 12-bit read gives 321 and a 2568-byte stride for a 640-pixel raster.
// (CRTC_OFFSET is the other way round: bits 19:0, since the 22-bit read
// puts the scan base 8 MB into a 2 MB framebuffer.)

static uint32_t mach64_pix_width(const mach64_t *m) {
    return (m->reg[DW_CRTC_GEN_CNTL] >> CRTC_PIX_WIDTH_SH) & 7u;
}

static uint32_t mach64_bytes_per_pixel(const mach64_t *m) {
    switch (mach64_pix_width(m)) {
    case CRTC_PIX_15BPP:
    case CRTC_PIX_16BPP:
        return 2;
    case CRTC_PIX_24BPP:
    case CRTC_PIX_32BPP:
        return 4;
    case CRTC_PIX_8BPP:
    case CRTC_PIX_4BPP:
    default:
        return 1;
    }
}

// Materialize the palette for the renderer.  Only the indexed depth reads
// it; the direct formats bypass it entirely.
static void mach64_refresh_clut(mach64_t *m) {
    for (uint32_t i = 0; i < 256; i++) {
        m->clut_view[i].r = m->clut[i][0];
        m->clut_view[i].g = m->clut[i][1];
        m->clut_view[i].b = m->clut[i][2];
        m->clut_view[i].a = 255;
    }
    m->display.clut = m->clut_view;
    m->display.clut_len = 256;
    m->display.clut_dirty = true;
}

static void mach64_clut_changed(mach64_t *m) {
    m->clut_dirty = true;
    if (mach64_pix_width(m) == CRTC_PIX_8BPP)
        mach64_refresh_clut(m);
}

static void mach64_update(mach64_t *m) {
    if (!m->blank)
        return; // registers poked before the buffers exist

    uint32_t bpp = mach64_bytes_per_pixel(m);
    uint32_t width = (((m->reg[DW_CRTC_H_TOTAL_DISP] >> 16) & 0xFFu) + 1u) * 8u;
    uint32_t height = ((m->reg[DW_CRTC_V_TOTAL_DISP] >> 16) & 0x7FFu) + 1u;
    uint32_t pitch_chars = (m->reg[DW_CRTC_OFF_PITCH] >> 22) & 0x3FFu;
    uint32_t stride = pitch_chars * 8u * bpp;
    uint32_t base = (m->reg[DW_CRTC_OFF_PITCH] & 0xFFFFFu) * 8u;

    pixel_format_t format = PIXEL_8BPP;
    switch (mach64_pix_width(m)) {
    case CRTC_PIX_8BPP:
        format = PIXEL_8BPP;
        break;
    case CRTC_PIX_15BPP:
        format = PIXEL_16BPP_555; // the Mac "thousands of colours" mode
        break;
    case CRTC_PIX_24BPP:
    case CRTC_PIX_32BPP:
        format = PIXEL_32BPP_XRGB;
        break;
    case CRTC_PIX_16BPP:
        // 5,6,5.  display.h has no format for it, and Mac OS should never
        // select it, so keep the previous mode rather than render wrong
        // colours silently.  If this ever fires, PIXEL_16BPP_565 is a small
        // addition — but we would want to know it fired.
        if (!m->pix_width_warned) {
            m->pix_width_warned = true;
            LOG(0, "CRTC_PIX_WIDTH = 4 (5,6,5) has no display format here — keeping the previous mode");
        }
        return;
    case 0:
        // The power-on state: nothing is programmed yet, so there is no
        // mode to derive and nothing has gone wrong.  Present the black
        // stub and say nothing — a cold boot passes through here.
        m->display.bits = m->blank;
        return;
    default:
        // 4 bpp, or a reserved encoding.  Same rule as 5,6,5 above.
        if (!m->pix_width_warned) {
            m->pix_width_warned = true;
            LOG(0, "CRTC_PIX_WIDTH = %u is not a depth this model renders — keeping the previous mode",
                mach64_pix_width(m));
        }
        return;
    }

    if (width == 0 || width > 2048u || height == 0 || height > 1536u) {
        LOG(2, "implausible CRTC geometry %ux%u — blanking", width, height);
        width = 640;
        height = 480;
        stride = 0;
    }

    m->display.width = width;
    m->display.height = height;
    m->display.format = format;
    m->display.stride = stride ? stride : width * bpp;
    m->display.par_w = 0;
    m->display.par_h = 0;
    m->display.crt_response = NULL;

    // Blanked when the CRTC is held in reset, when the raster is disabled,
    // when no pitch has been programmed, or when the scan would run off the
    // end of VRAM (nothing sane is being scanned in any of those cases).
    bool blanked =
        !(m->reg[DW_CRTC_GEN_CNTL] & CRTC_EN) || (m->reg[DW_CRTC_GEN_CNTL] & CRTC_DISPLAY_DIS) || stride == 0;
    uint64_t span = (uint64_t)m->display.stride * height;
    if ((uint64_t)base + span > m->vram_size)
        blanked = true;
    if (blanked) {
        size_t n = (size_t)m->display.stride * height;
        if (n > m->vram_size)
            n = m->vram_size;
        memset(m->blank, display_black_fill(format), n);
        m->display.bits = m->blank;
    } else {
        m->display.bits = m->vram + base;
    }

    if (format == PIXEL_8BPP) {
        mach64_refresh_clut(m);
    } else {
        m->display.clut = NULL;
        m->display.clut_len = 0;
    }
    m->display.shape_dirty = true;
    m->display.fb_dirty = true;
    LOG(2, "mode: %ux%u %u bpp stride=%u base=$%06X%s", width, height, bpp * 8u, m->display.stride, base,
        blanked ? " BLANKED" : "");
}

static display_t *mach64_display(pci_device_t *dev) {
    mach64_t *m = (mach64_t *)dev->priv;
    if (!m || !m->blank)
        return NULL;
    return &m->display;
}

// ============================================================
// The vertical-blank interrupt
// ============================================================
//
// The ndrv installs a VBL service through VSLNewInterruptService, so
// CRTC_INT_CNTL's enable/acknowledge must work and the slot's strapped
// INTA-D line must reach Grand Central 23.  The line is LEVEL-sensitive and
// is held until the driver acknowledges: a driver that services its VBL
// from an interrupt handler needs the source still asserted when the
// kernel samples it, or the handler never runs and the cursor task chain
// starves.  (That exact failure is what the Control VBL investigation
// chased on line 26.)

static void mach64_irq_sync(mach64_t *m) {
    bool want = CRTC_INT_ENABLE_FOR(m->reg[DW_CRTC_INT_CNTL]);
    if (want == m->irq_active)
        return;
    m->irq_active = want;
    if (want)
        pci_assert_irq(m->dev);
    else
        pci_deassert_irq(m->dev);
}

static void mach64_on_vbl(pci_device_t *dev, config_t *cfg) {
    (void)cfg;
    mach64_t *m = (mach64_t *)dev->priv;
    if (!m || !m->blank)
        return;
    // Guest CPU writes into VRAM bypass the renderer, so every host frame
    // has to be re-uploaded regardless of whether the card raised anything.
    m->display.fb_dirty = true;
    if (m->clut_dirty) {
        m->clut_dirty = false;
        if (mach64_pix_width(m) == CRTC_PIX_8BPP)
            mach64_refresh_clut(m);
    }
    if (m->reg[DW_CRTC_INT_CNTL] & CRTC_VBLANK_INT_EN) {
        m->reg[DW_CRTC_INT_CNTL] |= CRTC_VBLANK_INT;
        mach64_irq_sync(m);
    }
}

// ============================================================
// The PCI face
// ============================================================

static const pci_config_decl_t mach64gx_decl = {
    .vendor_id = MACH64_VENDOR_ID,
    .device_id = MACH64_DEVICE_ID,
    .revision = MACH64_REVISION,
    .class_code = MACH64_CLASS,
    .header_type = 0x00u,
    .interrupt_pin = 1, // INTA; the slots strap INTA-D together
    // Open Firmware sets memory space, and the ndrv rewrites the command
    // register at run time through ExpMgrConfigWriteLong, so it must stay
    // writable after the firmware is done.  I/O space is writable because
    // the card's own FCode turns its sparse register file on and off there
    // — it sets bit 0 before touching CONFIG_CNTL and clears it again on
    // the way out.
    .command_writable = PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE | PCI_CMD_MASTER,
    // Medium DEVSEL (bits 10:9 = 01), which Open Firmware publishes as the
    // node's `devsel-speed` property.  Apple's dump of a real card in a
    // real 9500 reports 1; the generic header would otherwise say "fast".
    .status_reset = 0x0200u,
    .bar = {[MACH64_BAR_APER] = {.size = MACH64_APER_SIZE, .kind = PCI_BAR_MEM}},
    .rom_size = MACH64_ROM_SIZE,
};

static const char *mach64_name(const pci_device_t *dev) {
    (void)dev;
    return "ATI Mach64 GX";
}

// PCI RST#: the chip returns to its power-on straps.  VRAM survives, as
// real DRAM does across a warm restart.
static void mach64_reset(pci_device_t *dev, config_t *cfg) {
    (void)cfg;
    mach64_t *m = (mach64_t *)dev->priv;
    memset(m->reg, 0, sizeof(m->reg));
    // Power-on strap values.  CONFIG_STAT0: PCI bus type (2:0 = 0),
    // enhanced VRAM (5:3 = 5), VGA enabled (bit 23), chip enabled (bit 25).
    // CFG_INIT_DAC_TYPE (11:9) has no IBM RGB514 encoding — see the read
    // path — so it is strapped to a plausible value and logged, not faked
    // into something the manual does not define.
    m->reg[DW_CONFIG_STAT0] = (5u << 3) | (1u << 23) | (1u << 25);
    m->reg[DW_MEM_CNTL] = mach64_mem_size_code(m);
    m->mon_id_dir = 0;
    m->mon_id_out = 7;
    m->sense_seen = false;
    m->sense_primary = 0;
    m->sense_ext = 0;
    m->dac_index = 0;
    m->dac_index_lo = 0;
    m->dac_pixel_mask = 0xFFu;
    m->clut_addr = 0;
    m->clut_phase = 0;
    memset(m->dac_indexed, 0, sizeof(m->dac_indexed));
    m->dac_indexed[0x01] = 0x01; // RGB514 ID register reset value
    memset(m->clut, 0, sizeof(m->clut));
    m->aperture_warned = false;
    m->host_blit_warned = false;
    m->mix_warned = false;
    m->host_op.active = false;
    // The scissor powers up wide open, so an operation from a driver that
    // never programs it is not clipped to a single pixel at the origin.
    m->reg[DW_SC_RIGHT] = 0xFFFFu;
    m->reg[DW_SC_BOTTOM] = 0xFFFFu;
    m->reg[DW_DP_WRITE_MSK] = 0xFFFFFFFFu;
    if (m->irq_active) {
        m->irq_active = false;
        pci_deassert_irq(dev);
    }
    mach64_update(m);
}

static void mach64_teardown(pci_device_t *dev, config_t *cfg) {
    (void)cfg;
    mach64_t *m = (mach64_t *)dev->priv;
    if (!m)
        return;
    free(m->vram);
    free(m->blank);
    free(m->compose);
    free(m);
    dev->priv = NULL;
}

// ============================================================
// Checkpoints
// ============================================================
// The generic header (command, BARs, the ROM BAR, $3C) is saved by
// pci_checkpoint_save, and pci_device_regions_changed() replays the decode
// on restore.  What belongs to the chip is saved here, VRAM last — the
// control.c tail-blob shape.  The ROM image is deliberately NOT saved: it
// is provisioned content, re-resolved from the offer registry on restore,
// exactly as vROMs are.

typedef struct mach64_ckpt {
    uint32_t reg[MACH64_NUM_REGS];
    uint32_t io_base;
    uint32_t vram_size;
    uint8_t mon_id_dir, mon_id_out, sense_primary, sense_ext;
    uint8_t sense_seen;
    uint16_t dac_index;
    uint8_t dac_index_lo, dac_pixel_mask, clut_addr, clut_phase;
    uint8_t dac_indexed[0x500];
    uint8_t clut[256][3];
} mach64_ckpt_t;

static void mach64_checkpoint_save(pci_device_t *dev, checkpoint_t *cp) {
    mach64_t *m = (mach64_t *)dev->priv;
    mach64_ckpt_t c;
    memset(&c, 0, sizeof(c));
    memcpy(c.reg, m->reg, sizeof(c.reg));
    c.io_base = m->io_base;
    c.vram_size = m->vram_size;
    c.mon_id_dir = m->mon_id_dir;
    c.mon_id_out = m->mon_id_out;
    c.sense_primary = m->sense_primary;
    c.sense_ext = m->sense_ext;
    c.sense_seen = m->sense_seen ? 1u : 0u;
    c.dac_index = m->dac_index;
    c.dac_index_lo = m->dac_index_lo;
    c.dac_pixel_mask = m->dac_pixel_mask;
    c.clut_addr = m->clut_addr;
    c.clut_phase = m->clut_phase;
    memcpy(c.dac_indexed, m->dac_indexed, sizeof(c.dac_indexed));
    memcpy(c.clut, m->clut, sizeof(c.clut));
    system_write_checkpoint_data(cp, &c, sizeof(c));
    system_write_checkpoint_data(cp, m->vram, m->vram_size);
}

static void mach64_checkpoint_restore(pci_device_t *dev, checkpoint_t *cp) {
    mach64_t *m = (mach64_t *)dev->priv;
    mach64_ckpt_t c;
    system_read_checkpoint_data(cp, &c, sizeof(c));
    memcpy(m->reg, c.reg, sizeof(m->reg));
    m->io_base = c.io_base;
    m->mon_id_dir = c.mon_id_dir;
    m->mon_id_out = c.mon_id_out;
    m->sense_primary = c.sense_primary;
    m->sense_ext = c.sense_ext;
    m->sense_seen = c.sense_seen != 0;
    m->dac_index = c.dac_index;
    m->dac_index_lo = c.dac_index_lo;
    m->dac_pixel_mask = c.dac_pixel_mask;
    m->clut_addr = c.clut_addr;
    m->clut_phase = c.clut_phase;
    memcpy(m->dac_indexed, c.dac_indexed, sizeof(m->dac_indexed));
    memcpy(m->clut, c.clut, sizeof(m->clut));
    // A checkpoint taken on a 4 MB card must not be restored into a 2 MB
    // buffer: resize rather than truncate the stream.
    if (c.vram_size != m->vram_size) {
        uint8_t *grown = (uint8_t *)calloc(1, c.vram_size);
        if (grown) {
            free(m->vram);
            m->vram = grown;
            m->vram_size = c.vram_size;
        }
    }
    system_read_checkpoint_data(cp, m->vram, m->vram_size);
    mach64_refresh_clut(m);
    mach64_update(m);
    mach64_irq_sync(m);
}

static const pci_device_ops_t mach64_ops = {
    .teardown = mach64_teardown,
    .reset = mach64_reset,
    .name = mach64_name,
    .display = mach64_display,
    .on_vbl = mach64_on_vbl,
    .checkpoint_save = mach64_checkpoint_save,
    .checkpoint_restore = mach64_checkpoint_restore,
};

// ============================================================
// Staged options
// ============================================================
// Routed through the KIND's stage_option hook, so the generic layer never
// learns this card's identity.  Returning false makes the generic layer
// log the key as not understood and drop it.

static bool mach64_stage_option(const char *key, const char *value) {
    if (!key || !value)
        return false;
    if (strcmp(key, "monitor") == 0) {
        for (const mach64_monitor_sense_t *s = mach64_sense; s->id; s++) {
            if (strcmp(s->id, value) == 0) {
                snprintf(s_staged_monitor, sizeof s_staged_monitor, "%s", value);
                return true;
            }
        }
        LOG(0,
            "unknown monitor id '%s' — the card offers 14in_rgb, 15in_multi, 17in_multi, "
            "20in_multi, 21in_color",
            value);
        return true; // the key IS ours; the value was the problem
    }
    if (strcmp(key, "vram") == 0) {
        if (strcmp(value, "2m") == 0) {
            s_staged_vram = MACH64_VRAM_2MB;
            return true;
        }
        if (strcmp(value, "4m") == 0) {
            s_staged_vram = MACH64_VRAM_4MB;
            return true;
        }
        LOG(0,
            "unknown vram size '%s' — the card takes 2m (soldered) or 4m (the "
            "109-31600-00 expansion module)",
            value);
        return true;
    }
    return false;
}

// ============================================================
// The object model — machine.pci.slot[N].card.*
// ============================================================

static mach64_t *node_card(struct object *self) {
    return (mach64_t *)object_data(self);
}

static value_t mon_attr_id(struct object *self, const member_t *m) {
    (void)m;
    mach64_t *c = node_card(self);
    return val_str((c && c->mon) ? c->mon->id : "");
}
static value_t mon_attr_primary(struct object *self, const member_t *m) {
    (void)m;
    mach64_t *c = node_card(self);
    return val_uint(1, (c && c->mon) ? c->mon->primary : 7);
}
static value_t mon_attr_sensed_primary(struct object *self, const member_t *m) {
    (void)m;
    mach64_t *c = node_card(self);
    return val_uint(1, c ? c->sense_primary : 0);
}
static value_t mon_attr_sensed_ext(struct object *self, const member_t *m) {
    (void)m;
    mach64_t *c = node_card(self);
    return val_uint(1, c ? c->sense_ext : 0);
}
static value_t mon_attr_probed(struct object *self, const member_t *m) {
    (void)m;
    mach64_t *c = node_card(self);
    return val_bool(c && c->sense_seen);
}

static const member_t monitor_members[] = {
    {.kind = M_ATTR,
     .name = "id",
     .doc = "Strapped monitor id (see machine.profile for the card's list)",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = mon_attr_id}                                     },
    {.kind = M_ATTR,
     .name = "sense_code",
     .doc = "The monitor's 3-bit primary sense code",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = mon_attr_primary}                                  },
    {.kind = M_ATTR,
     .name = "probed",
     .doc = "True once the guest has driven the monitor-ID pins",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = mon_attr_probed}                                   },
    {.kind = M_ATTR,
     .name = "sensed_primary",
     .doc = "The primary code the guest actually read back",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = mon_attr_sensed_primary}                           },
    {.kind = M_ATTR,
     .name = "sensed_extended",
     .doc = "The 6-bit extended code the guest's four-step walk assembled",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = mon_attr_sensed_ext}},
};
static const class_desc_t mach64_monitor_class = {
    .name = "monitor", .members = monitor_members, .n_members = sizeof(monitor_members) / sizeof(monitor_members[0])};

static value_t regs_attr_chip_id(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(4, MACH64_CHIP_ID);
}
static value_t regs_attr_config_cntl(struct object *self, const member_t *m) {
    (void)m;
    mach64_t *c = node_card(self);
    return val_uint(4, c ? c->reg[DW_CONFIG_CNTL] : 0);
}
static value_t regs_attr_aperture(struct object *self, const member_t *m) {
    (void)m;
    mach64_t *c = node_card(self);
    return val_uint(4, c ? mach64_aperture_size(c) : 0);
}
static value_t regs_attr_vram(struct object *self, const member_t *m) {
    (void)m;
    mach64_t *c = node_card(self);
    return val_uint(4, c ? c->vram_size : 0);
}
static value_t regs_attr_crtc_gen(struct object *self, const member_t *m) {
    (void)m;
    mach64_t *c = node_card(self);
    return val_uint(4, c ? c->reg[DW_CRTC_GEN_CNTL] : 0);
}
static value_t regs_attr_mem_cntl(struct object *self, const member_t *m) {
    (void)m;
    mach64_t *c = node_card(self);
    return val_uint(4, c ? c->reg[DW_MEM_CNTL] : 0);
}
static value_t regs_attr_dac_cntl(struct object *self, const member_t *m) {
    (void)m;
    mach64_t *c = node_card(self);
    return val_uint(4, c ? mach64_reg_read(c, DW_DAC_CNTL) : 0);
}
static value_t regs_method_read(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    mach64_t *c = node_card(self);
    int64_t dw = argv[0].i;
    if (!c || dw < 0 || dw >= MACH64_NUM_REGS)
        return val_err("regs.read: dword index must be 0..$%X (or $%X for CONFIG_CNTL)", MACH64_NUM_REGS - 2,
                       DW_CONFIG_CNTL);
    return val_uint(4, mach64_reg_read(c, (int)dw));
}

static const arg_decl_t regs_read_arg[] = {
    {.name = "dword", .kind = V_INT, .doc = "Register DWORD offset (ATI RRG chapter 2)"},
};

static const member_t regs_members[] = {
    {.kind = M_ATTR,
     .name = "chip_id",
     .doc = "CONFIG_CHIP_ID: 'GX', class 0, revision 2 (GX-2)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = regs_attr_chip_id}},
    {.kind = M_ATTR,
     .name = "config_cntl",
     .doc = "CONFIG_CNTL (I/O only): aperture size, location and VGA disable",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = regs_attr_config_cntl}},
    {.kind = M_ATTR,
     .name = "aperture_size",
     .doc = "Bytes of BAR0 the aperture currently covers (0 = disabled)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = regs_attr_aperture}},
    {.kind = M_ATTR,
     .name = "vram_size",
     .doc = "Bytes of video memory on this card",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = regs_attr_vram}},
    {.kind = M_ATTR,
     .name = "crtc_gen_cntl",
     .doc = "CRTC_GEN_CNTL: pixel width, extended-display and CRTC enables",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = regs_attr_crtc_gen}},
    {.kind = M_ATTR,
     .name = "mem_cntl",
     .doc = "MEM_CNTL: MEM_SIZE in bits 2:0",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = regs_attr_mem_cntl}},
    {.kind = M_ATTR,
     .name = "dac_cntl",
     .doc = "DAC_CNTL, monitor-ID pins read live in bits 26:24",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = regs_attr_dac_cntl}},
    {.kind = M_METHOD,
     .name = "read",
     .doc = "Read any register by its DWORD offset",
     .method = {.args = regs_read_arg, .nargs = 1, .result = V_UINT, .fn = regs_method_read}},
};
static const class_desc_t mach64_regs_class = {
    .name = "regs", .members = regs_members, .n_members = sizeof(regs_members) / sizeof(regs_members[0])};

static value_t dac_attr_index(struct object *self, const member_t *m) {
    (void)m;
    mach64_t *c = node_card(self);
    return val_uint(2, c ? c->dac_index : 0);
}
static value_t dac_attr_pixel_format(struct object *self, const member_t *m) {
    (void)m;
    mach64_t *c = node_card(self);
    return val_uint(1, c ? c->dac_indexed[0x0A] : 0);
}
static value_t dac_attr_misc2(struct object *self, const member_t *m) {
    (void)m;
    mach64_t *c = node_card(self);
    return val_uint(1, c ? c->dac_indexed[0x71] : 0);
}
static value_t dac_attr_pixel_mask(struct object *self, const member_t *m) {
    (void)m;
    mach64_t *c = node_card(self);
    return val_uint(1, c ? c->dac_pixel_mask : 0);
}
static value_t dac_method_read(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    mach64_t *c = node_card(self);
    int64_t idx = argv[0].i;
    if (!c || idx < 0 || idx >= (int64_t)sizeof(c->dac_indexed))
        return val_err("dac.read: index must be 0..$4FF");
    return val_uint(1, c->dac_indexed[idx]);
}

static const arg_decl_t dac_read_arg[] = {
    {.name = "index", .kind = V_INT, .doc = "RGB514 indexed register ($00-$4FF)"},
};

static const member_t dac_members[] = {
    {.kind = M_ATTR,
     .name = "index",
     .doc = "The RGB514's 16-bit indexed-register pointer",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = dac_attr_index}},
    {.kind = M_ATTR,
     .name = "pixel_format",
     .doc = "Indexed $0A Pixel Format (3 = 8 bpp, 4 = 15/16, 5 = 24, 6 = 32)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = dac_attr_pixel_format}},
    {.kind = M_ATTR,
     .name = "misc_control_2",
     .doc = "Indexed $71: pixel-clock select, colour resolution, port select",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = dac_attr_misc2}},
    {.kind = M_ATTR,
     .name = "pixel_mask",
     .doc = "The RS 010 pixel mask ($FF = no masking)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = dac_attr_pixel_mask}},
    {.kind = M_METHOD,
     .name = "read",
     .doc = "Read an RGB514 indexed register",
     .method = {.args = dac_read_arg, .nargs = 1, .result = V_UINT, .fn = dac_method_read}},
};
static const class_desc_t mach64_dac_class = {
    .name = "dac", .members = dac_members, .n_members = sizeof(dac_members) / sizeof(dac_members[0])};

// --- the framebuffer node ---------------------------------------------------
// Mirrors what the NuBus cards expose, so `machine.screen.source.depth`
// reads the same on either bus.

static value_t fb_attr_width(struct object *self, const member_t *m) {
    (void)m;
    mach64_t *c = node_card(self);
    return val_uint(4, c ? c->display.width : 0);
}
static value_t fb_attr_height(struct object *self, const member_t *m) {
    (void)m;
    mach64_t *c = node_card(self);
    return val_uint(4, c ? c->display.height : 0);
}
static value_t fb_attr_stride(struct object *self, const member_t *m) {
    (void)m;
    mach64_t *c = node_card(self);
    return val_uint(4, c ? c->display.stride : 0);
}
static value_t fb_attr_depth(struct object *self, const member_t *m) {
    (void)m;
    mach64_t *c = node_card(self);
    return val_uint(4, c ? mach64_bytes_per_pixel(c) * 8u : 0);
}
static value_t fb_attr_base(struct object *self, const member_t *m) {
    (void)m;
    mach64_t *c = node_card(self);
    return val_uint(4, c ? (c->reg[DW_CRTC_OFF_PITCH] & 0xFFFFFu) * 8u : 0);
}
static value_t fb_attr_size(struct object *self, const member_t *m) {
    (void)m;
    mach64_t *c = node_card(self);
    return val_uint(4, c ? (uint64_t)c->display.stride * c->display.height : 0);
}

static const member_t fb_members[] = {
    {.kind = M_ATTR,
     .name = "width",
     .doc = "Active raster width in pixels",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = fb_attr_width}                              },
    {.kind = M_ATTR,
     .name = "height",
     .doc = "Active raster height in lines",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = fb_attr_height}                             },
    {.kind = M_ATTR,
     .name = "stride",
     .doc = "Bytes per scan line (CRTC_PITCH x 8 x bytes-per-pixel)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = fb_attr_stride}                             },
    {.kind = M_ATTR,
     .name = "depth",
     .doc = "Bits per pixel, from CRTC_PIX_WIDTH",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = fb_attr_depth}                              },
    {.kind = M_ATTR,
     .name = "base",
     .doc = "Scan base as a byte offset into VRAM (CRTC_OFFSET x 8)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = fb_attr_base}},
    {.kind = M_ATTR,
     .name = "size",
     .doc = "Active framebuffer size in bytes (stride x height)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = fb_attr_size}                               },
};
static const class_desc_t mach64_fb_class = {
    .name = "framebuffer", .members = fb_members, .n_members = sizeof(fb_members) / sizeof(fb_members[0])};

static void mach64_attach_objects(pci_device_t *dev, struct object *card_node) {
    mach64_t *m = (mach64_t *)dev->priv;
    if (!m || !card_node)
        return;
    struct object *fb = object_new(&mach64_fb_class, m, "framebuffer");
    if (fb) {
        object_set_label(fb, "Framebuffer");
        object_set_order(fb, 10);
        object_attach(card_node, fb);
        // Nominate it: `machine.screen.source` resolves to whichever
        // framebuffer node belongs to the current primary display, and the
        // generic layer must not have to guess which child that is.
        pci_card_set_framebuffer_object(dev, fb);
    }
    struct object *mon = object_new(&mach64_monitor_class, m, "monitor");
    if (mon) {
        object_set_label(mon, "Monitor");
        object_set_order(mon, 20);
        object_attach(card_node, mon);
    }
    struct object *regs = object_new(&mach64_regs_class, m, "regs");
    if (regs) {
        object_set_label(regs, "Registers");
        object_set_order(regs, 30);
        object_set_category(regs, M_CAT_ADVANCED);
        object_attach(card_node, regs);
    }
    struct object *dac = object_new(&mach64_dac_class, m, "dac");
    if (dac) {
        object_set_label(dac, "RAMDAC (IBM RGB514)");
        object_set_order(dac, 40);
        object_set_category(dac, M_CAT_ADVANCED);
        object_attach(card_node, dac);
    }
}

// ============================================================
// The factory and the card kind
// ============================================================

static pci_device_t *mach64_factory(int slot_index, config_t *cfg, checkpoint_t *cp) {
    (void)cp;
    pci_device_t *dev = (pci_device_t *)calloc(1, sizeof(*dev));
    mach64_t *m = (mach64_t *)calloc(1, sizeof(*m));
    if (!dev || !m) {
        free(dev);
        free(m);
        return NULL;
    }
    dev->ops = &mach64_ops;
    dev->decl = &mach64gx_decl;
    dev->priv = m;
    pci_cfg_reset(dev); // power-on header state, including status_reset
    m->dev = dev;
    m->cfg = cfg;
    m->io_base = MACH64_IO_BASE_DEFAULT;

    // The card is useless without its own firmware: Open Firmware runs the
    // FCode to build the node, and Mac OS loads the ndrv the same image
    // publishes.  An explicitly picked card that cannot resolve one has
    // already failed machine.boot's validation; reaching here with nothing
    // means a slot DEFAULT could not resolve, which degrades to an empty
    // slot with a log rather than killing the boot.
    uint8_t *rom = NULL;
    size_t rom_size = 0;
    char *rom_path = NULL;
    if (!prom_load_card("mach64_gx", &rom, &rom_size, &rom_path)) {
        LOG(0,
            "slot %d: no expansion ROM available for the Mach64 GX — the card cannot enumerate "
            "without its own FCode, so the slot is left empty",
            slot_index);
        free(m);
        free(dev);
        return NULL;
    }
    dev->rom = rom;
    dev->rom_size = rom_size;
    free(rom_path);

    m->vram_size = s_staged_vram;
    m->vram = (uint8_t *)calloc(1, m->vram_size);
    m->blank = (uint8_t *)calloc(1, m->vram_size);
    m->compose = (uint8_t *)calloc(1, m->vram_size);
    if (!m->vram || !m->blank || !m->compose) {
        free(m->vram);
        free(m->blank);
        free(m->compose);
        free(dev->rom);
        free(m);
        free(dev);
        return NULL;
    }

    // The strapped monitor: the staged pick if the user made one, else the
    // 14" AppleColor, which is the safe default — a primary code of 6 can
    // never trip the card's "No monitor" bail (that needs 7 AND $3F).
    m->mon = &mach64_sense[0];
    if (s_staged_monitor[0]) {
        for (const mach64_monitor_sense_t *s = mach64_sense; s->id; s++) {
            if (strcmp(s->id, s_staged_monitor) == 0) {
                m->mon = s;
                break;
            }
        }
    }
    s_staged_monitor[0] = '\0';
    s_staged_vram = MACH64_VRAM_2MB;

    mach64_reset(dev, cfg);

    m->io_if.read_uint8 = io_read8;
    m->io_if.read_uint16 = io_read16;
    m->io_if.read_uint32 = io_read32;
    m->io_if.write_uint8 = io_write8;
    m->io_if.write_uint16 = io_write16;
    m->io_if.write_uint32 = io_write32;
    m->aper_if.read_uint8 = aper_read8;
    m->aper_if.read_uint16 = aper_read16;
    m->aper_if.read_uint32 = aper_read32;
    m->aper_if.write_uint8 = aper_write8;
    m->aper_if.write_uint16 = aper_write16;
    m->aper_if.write_uint32 = aper_write32;
    m->rom_if.read_uint8 = rom_read8;
    m->rom_if.read_uint16 = rom_read16;
    m->rom_if.read_uint32 = rom_read32;
    m->rom_if.write_uint8 = rom_write8;
    m->rom_if.write_uint16 = rom_write16;
    m->rom_if.write_uint32 = rom_write32;

    pci_bar_backing_iface(dev, MACH64_BAR_APER, &m->aper_if, m);
    pci_bar_backing_iface(dev, PCI_ROM_BAR_INDEX, &m->rom_if, m);
    // The sparse I/O register file: not a BAR, a strapped decode.  Only
    // addresses congruent to the strapped base modulo 1024 are ours.
    pci_device_add_fixed_region(dev, PCI_SPACE_IO, 0, MACH64_IO_SPAN, MACH64_IO_MATCH_MASK, m->io_base, &m->io_if, m);

    LOG(1, "seated in slot %d: %u MB VRAM, monitor '%s' (primary sense %u), %zu-byte expansion ROM", slot_index,
        m->vram_size >> 20, m->mon->id, m->mon->primary, rom_size);
    return dev;
}

const pci_card_kind_t mach64_gx_kind = {
    .id = "mach64_gx",
    .display_name = "Apple Accelerated PCI Graphics Card (ATI Mach64 GX)",
    .attach = PCI_ATTACH_PCI,
    .requires_prom = true,
    .card_class = "display",
    .monitors = mach64_monitors,
    .factory = mach64_factory,
    .stage_option = mach64_stage_option,
    .attach_objects = mach64_attach_objects,
};
