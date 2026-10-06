// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rage128_priv.h
// Shared state and register map of the ATI Rage 128 GL card model, split
// between the card (rage128.c: the PCI face, apertures, register file,
// sense/DDC, CRTC and cursor) and its engines (rage128_2d.c: the 2D draw
// engine; rage128_cce.c: the Concurrent Command Engine).  Not a public
// header: only the card's own files include it.

#ifndef PCI_RAGE128_PRIV_H
#define PCI_RAGE128_PRIV_H

#include "config_space.h"
#include "display.h"
#include "display_class.h"
#include "memory.h"
#include "pci.h"

#include <stdbool.h>
#include <stdint.h>

// ============================================================
// Identity and geometry
// ============================================================

#define R128_VENDOR_ID 0x1002u
#define R128_DEVICE_ID 0x5245u // 'RE': Rage 128 GL, PCI
// REVISION_ID $00 = the initial silicon (RRG §4.1).  The A22 production
// revision's value is unknown until a real card is read; drivers key errata
// workarounds on it, so whatever reads it is logged.
#define R128_REVISION 0x00u
#define R128_CLASS    0x030000u // display / VGA-compatible

#define R128_BAR_APER 0 // $10: 64 MB, two 32 MB linear apertures
#define R128_BAR_IO   1 // $14: 256 B, the non-GUI registers
#define R128_BAR_REGS 2 // $18: 16 KB, two 8 KB register apertures

#define R128_APER_SIZE     0x4000000u // BAR0
#define R128_APER_HALF     0x2000000u // one linear aperture (CONFIG_APER_SIZE)
#define R128_IO_SIZE       0x100u // BAR1
#define R128_REGS_SIZE     0x4000u // BAR2
#define R128_REG_APER_SIZE 0x2000u // one register aperture (CONFIG_REG_APER_SIZE)
#define R128_ROM_SIZE      0x20000u // the 128 KB flash
#define R128_NUM_REGS      (R128_REG_APER_SIZE / 4u)

// VRAM: 16 MB on the Rage Orion and the Xclaim VR 128, 32 MB on the Nexus 128.
#define R128_VRAM_16MB 0x1000000u
#define R128_VRAM_32MB 0x2000000u

// ============================================================
// Configuration space beyond the generic header (RRG §4.1-4.2)
// ============================================================

#define CFG_CAP_AGP    0x50u // CAPABILITIES_ID: AGP 1.0, next $5C
#define CFG_AGP_STATUS 0x54u
#define CFG_AGP_CMD    0x58u
#define CFG_CAP_PMI    0x5Cu // PMI_REGISTER: PM 1.0, last capability
#define CFG_PMI_PMCSR  0x60u

#define AGP_CAP_VALUE    0x00105C02u // ID 2, next $5C, AGP major 1 minor 0
#define AGP_STATUS_VALUE 0x1F000203u // RQ $1F, SBA, 2x, 1x
#define AGP_CMD_RESET    0x00000200u // SBA_EN defaults on
#define PMI_CAP_VALUE    0x02010001u // ID 1, last, PMC = version 1, D1 supported
#define CFG_MIN_GNT      8u // MIN_GRANT at $3E
#define CFG_INT_LINE_RST 0xFFu // INTERRUPT_LINE resets to $FF

// ============================================================
// The register file — byte offsets in a register aperture
// ============================================================

#define R_MM_INDEX             0x0000u
#define R_MM_DATA              0x0004u
#define R_CLOCK_CNTL_INDEX     0x0008u
#define R_CLOCK_CNTL_DATA      0x000Cu
#define R_BIOS_0_SCRATCH       0x0010u
#define R_BUS_CNTL             0x0030u
#define R_GEN_INT_CNTL         0x0040u
#define R_GEN_INT_STATUS       0x0044u
#define R_CRTC_GEN_CNTL        0x0050u
#define R_CRTC_EXT_CNTL        0x0054u
#define R_DAC_CNTL             0x0058u
#define R_CRTC_STATUS          0x005Cu
#define R_GPIO_MONID           0x0068u
#define R_GPIO_MONIDB          0x006Cu
#define R_PALETTE_INDEX        0x00B0u
#define R_PALETTE_DATA         0x00B4u
#define R_CONFIG_CNTL          0x00E0u
#define R_CONFIG_XSTRAP        0x00E4u
#define R_CONFIG_BONDS         0x00E8u
#define R_GEN_RESET_CNTL       0x00F0u
#define R_AGP_BASE             0x0170u
#define R_PCI_GART_PAGE        0x017Cu
#define R_CONFIG_MEMSIZE       0x00F8u
#define R_CONFIG_APER_0_BASE   0x0100u
#define R_CONFIG_APER_1_BASE   0x0104u
#define R_CONFIG_APER_SIZE     0x0108u
#define R_CONFIG_REG_1_BASE    0x010Cu
#define R_CONFIG_REG_APER_SIZE 0x0110u
#define R_CONFIG_MEMSIZE_EMB   0x0114u
#define R_PC_NGUI_CTLSTAT      0x0184u
#define R_CRTC_H_TOTAL_DISP    0x0200u
#define R_CRTC_V_TOTAL_DISP    0x0208u
#define R_CRTC_V_SYNC_STRT_WID 0x020Cu
#define R_CRTC_VLINE_CRNT      0x0210u
#define R_CRTC_CRNT_FRAME      0x0214u
#define R_CRTC_OFFSET          0x0224u
#define R_CRTC_OFFSET_CNTL     0x0228u
#define R_CRTC_PITCH           0x022Cu
#define R_CUR_OFFSET           0x0260u
#define R_CUR_HORZ_VERT_POSN   0x0264u
#define R_CUR_HORZ_VERT_OFF    0x0268u
#define R_CUR_CLR0             0x026Cu
#define R_CUR_CLR1             0x0270u
#define R_CFG_MIRROR           0x0F00u // read-only copy of config space
#define R_CFG_MIRROR_END       0x1000u
#define R_PC_GUI_CTLSTAT       0x1748u
#define R_GUI_STAT             0x1740u
#define R_PM4_STAT             0x07B8u

// BUS_CNTL bit 6: bus mastering disabled (the reset state).
#define BUS_MASTER_DIS 0x00000040u
// GEN_RESET_CNTL bit 0: hold the GUI engines (2D, 3D, CCE) in reset.
#define SOFT_RESET_GUI 0x00000001u
// PCI_GART_PAGE bit 0: the GART is disabled (the reset state).
#define PCI_GART_DIS 0x00000001u

// MM_INDEX (RRG §4.3): bit 31 MM_APER selects linear aperture 0 instead of
// the register file; the address is bits 26:2.
#define MM_APER      0x80000000u
#define MM_ADDR_MASK 0x07FFFFFCu

// CLOCK_CNTL_INDEX (RRG §3.2): bits 5:0 the PLL register, bit 7 write enable.
#define PLL_ADDR_MASK 0x3Fu
#define PLL_WR_EN     0x80u

// The PLL file (indexed through CLOCK_CNTL_INDEX/DATA, RRG §3.2).
#define PLL_PPLL_CNTL    0x02u
#define PLL_PPLL_REF_DIV 0x03u
#define PLL_PPLL_DIV_0   0x04u
#define PLL_PPLL_DIV_3   0x07u
// PPLL_ATOMIC_UPDATE_W/R: the driver sets bit 15 to latch a new divider and
// polls it back to 0 — on silicon it clears at the next pixel-clock edge, so
// here it clears at once.
#define PPLL_ATOMIC_UPDATE 0x8000u

// GEN_INT_CNTL / GEN_INT_STATUS (RRG §3.4): status bits latch and are
// write-1-to-clear; the enables gate the INTA line.
#define INT_CRTC_VBLANK 0x01u
#define INT_CRTC_VLINE  0x02u
#define INT_CRTC_VSYNC  0x04u
#define INT_STATUS_MASK 0x010F000Fu

// CRTC_STATUS: bit 0 the live vertical-blank state, bit 1 "a blank happened
// since last cleared" (write 1 to clear).
#define CRTC_VBLANK_CUR  0x01u
#define CRTC_VBLANK_SAVE 0x02u

// CRTC_GEN_CNTL (RRG §6.1).
#define CRTC_PIX_WIDTH(v)  (((v) >> 8) & 7u)
#define CRTC_CUR_EN        0x00010000u
#define CRTC_EXT_DISP_EN   0x01000000u
#define CRTC_EN            0x02000000u
#define CRTC_DISP_REQ_EN_B 0x04000000u
#define CRTC_PIX_8BPP      2u
#define CRTC_PIX_15BPP     3u
#define CRTC_PIX_16BPP     4u
#define CRTC_PIX_24BPP     5u
#define CRTC_PIX_32BPP     6u

// CRTC_OFFSET / CRTC_OFFSET_CNTL (RRG §6.1).
#define CRTC_OFFSET_MASK      0x01FFFFF8u
#define CRTC_GUI_TRIG_OFFSET  0x40000000u
#define CRTC_OFFSET_LOCK      0x80000000u
#define CRTC_OFFSET_FLIP_CNTL 0x00010000u

// CRTC_EXT_CNTL.
#define CRTC_DISPLAY_DIS 0x00000400u

// CRTC_V_SYNC_STRT_WID bit 23: vertical sync polarity.
#define CRTC_V_SYNC_POL 0x00800000u

// DAC_CNTL (RRG §6.6).
#define DAC_COMP_EN    0x00000008u
#define DAC_CMP_OUTPUT 0x00000080u // all three comparators below threshold: a load is attached
#define DAC_8BIT_EN    0x00000100u

// CONFIG_CNTL (RRG §4.3).
#define APER_0_ENDIAN(v)  ((v) & 3u)
#define APER_1_ENDIAN(v)  (((v) >> 2) & 3u)
#define APER_REG_ENDIAN   0x10u
#define CONFIG_CNTL_WMASK 0x0000031Fu // the writable fields; CFG_ATI_REV_ID (19:16) reads 0
#define ENDIAN_NONE       0u
#define ENDIAN_16BPP      1u
#define ENDIAN_32BPP      2u

// GUI_STAT (RRG §7.9): the command FIFO's free-entry count in 11:0 and
// GUI_ACTIVE in bit 31.  The engines run to completion when triggered, so
// the FIFO always reads empty — 64 free, the most the drivers wait for.
#define GUI_FIFO_FREE 64u

// GPIO_MONID (RRG §3.1): four pads, each with an out-going value A (3:0),
// a read-back Y (11:8), a direction EN (19:16, 1 = output) and a MASK
// (27:24).  A pad is open-drain against a pull-up: it reads low when the
// card drives it low (EN=1, A=0) or the monitor ties it low.
//
// The FCode's logical sense bits map onto the pads crosswise — SENSE0 is
// MONID0, SENSE1 is MONID2 and SENSE2 is MONID1 — because MONID2/MONID1 are
// also the DDC clock and data.  MONID3 carries the monitor cable's VSYNC
// loopback: an Apple-sense adapter returns the card's own vertical-sync
// polarity on it, and the FCode only trusts the three sense lines when it
// sees that loopback follow CRTC_V_SYNC_STRT_WID bit 23.
#define MONID_PADS     4u
#define MONID_Y_SHIFT  8
#define MONID_EN_SHIFT 16
#define MONID_PAD_SCL  2u // MONID2: DDC clock, SENSE1
#define MONID_PAD_SDA  1u // MONID1: DDC data, SENSE2
#define MONID_PAD_LOOP 3u // MONID3: the VSYNC loopback

// The DDC monitor's EDID EEPROM answers at 7-bit address $50 ($A0/$A1).
#define DDC_EDID_ADDR 0x50u

// ============================================================
// Monitors
// ============================================================
//
// Two kinds of cable reach this card.  A VGA (or multisync) monitor gives
// no VSYNC loopback, so the FCode takes the VGA path — sense code $717,
// 640 x 480 at 60 Hz — and, if the monitor answers DDC, reads its EDID and
// publishes it.  An Apple monitor on an Apple-sense cable loops VSYNC back
// and ties the three sense lines: `primary` is the 3-bit code with nothing
// driven, the ext pairs what the other two lines read while one is driven
// low (the Mach64 card's model of the same Apple scheme).
typedef struct r128_monitor {
    const char *id;
    bool apple_sense; // VSYNC loopback on MONID3 + tied sense lines
    bool ddc; // an EDID EEPROM answers on MONID2/MONID1
    uint8_t primary; // SENSE2..0 with nothing driven
    uint8_t ext01; // SENSE2 driven low: (SENSE1, SENSE0)
    uint8_t ext02; // SENSE1 driven low: (SENSE2, SENSE0)
    uint8_t ext12; // SENSE0 driven low: (SENSE2, SENSE1)
} r128_monitor_t;

// ============================================================
// Device state
// ============================================================

// The DDC slave: a bit-banged I2C EEPROM watching the two pads.
typedef enum ddc_state {
    DDC_IDLE = 0, // waiting for a START
    DDC_RX, // shifting in a byte from the master
    DDC_ACK_OUT, // holding SDA low for our ACK
    DDC_TX, // shifting out a byte
    DDC_ACK_IN, // the master's ACK/NACK clock
} ddc_state_t;

typedef struct r128_ddc {
    uint8_t state; // ddc_state_t
    uint8_t shift; // the byte in flight
    uint8_t nbits; // bits shifted so far
    uint8_t ptr; // EEPROM word address
    bool is_addr; // the byte in flight is the address byte
    bool reading; // the master asked for a read
    bool sda_low; // the slave is pulling SDA low
    bool master_ack; // what the master sent on the last ACK clock
    bool scl, sda; // the line levels last seen
} r128_ddc_t;

// The CCE (rage128_cce.c): one packet stream being assembled — the ring
// and the PIO FIFO share one, the indirect buffer has its own (a type-0
// packet in the ring can call the indirect buffer mid-stream).  The longest
// packet is a 14-bit count plus its header.
#define R128_CCE_MAX_PACKET (0x4000u + 1u)
typedef struct r128_cce_stream {
    uint32_t len; // dwords gathered
    uint32_t need; // dwords the packet holds (0: waiting for a header)
    uint32_t buf[R128_CCE_MAX_PACKET];
} r128_cce_stream_t;

typedef struct r128_cce {
    uint32_t ucode[256][2]; // the microcode RAM: (DATAH, DATAL) per address
    uint32_t ucode_addr; // PM4_MICROCODE_ADDR, auto-incrementing per pair
    uint32_t ucode_raddr; // PM4_MICROCODE_RADDR, the same for reads
    uint8_t ucode_state; // R128_UCODE_*
    bool in_ring; // a ring fetch is running (re-entry guard)
    bool in_indirect; // an indirect buffer is running (re-entry guard)
    uint64_t packets; // packets executed (diagnostics)
    uint8_t told[32]; // once-only logs, a bit per type-3 opcode
    // The last 3D_RNDR_GEN_INDX_PRIM, which NEXT_VERTEX_BUNDLE continues.
    uint32_t vc_vloff, vc_format, vc_cntl;
    r128_cce_stream_t main; // the ring and the PIO FIFO
    r128_cce_stream_t ind; // the indirect buffer
} r128_cce_t;

#define R128_UCODE_NONE    0u // nothing uploaded since reset
#define R128_UCODE_KNOWN   1u // ATI's published Rage 128 microcode
#define R128_UCODE_UNKNOWN 2u // 256 pairs that are not it

typedef struct rage128 {
    pci_device_t *dev;
    config_t *cfg;

    uint32_t reg[R128_NUM_REGS]; // the register file, little-endian values
    uint32_t pll[64]; // the PLL file behind CLOCK_CNTL_INDEX/DATA
    uint32_t agp_cmd; // AGP_COMMAND ($58)
    uint32_t pmcsr; // PMI_PMCSR ($60)

    uint8_t *vram;
    uint32_t vram_size;

    // The palette: 256 x 8:8:8, through PALETTE_INDEX / PALETTE_DATA.
    uint8_t clut[256][3];
    uint8_t pal_w; // PALETTE_W_INDEX, auto-incrementing
    uint8_t pal_r; // PALETTE_R_INDEX, auto-incrementing

    const r128_monitor_t *mon; // the attached monitor
    uint8_t edid[128];
    r128_ddc_t ddc;

    memory_interface_t aper_if; // BAR0
    memory_interface_t io_if; // BAR1
    memory_interface_t regs_if; // BAR2
    memory_interface_t rom_if; // expansion ROM

    // Scanout, rebuilt from the CRTC registers whenever one moves.
    display_t display;
    display_fb_node_t fb_node;
    rgba8_t clut_view[256];
    uint8_t dac_view[3][256]; // the palette as the direct-colour DAC table (display_t.dac_lut)
    uint8_t *blank; // black stub while the raster is off (vram_size)
    uint8_t *compose; // big-endian copy for the direct-colour depths (vram_size)
    uint32_t scan_base;
    // CRTC_OFFSET as the display uses it: a write while the CRTC runs takes
    // effect at the next vertical blank (a page flip), and until then
    // CRTC_GUI_TRIG_OFFSET reads 1.
    uint32_t crtc_offset_live;
    bool flip_pending;
    bool scan_blanked;
    bool scan_swap; // direct colour: present() must byte-swap into `compose`
    bool clut_dirty;
    bool irq_active;
    bool depth_warned; // the unsupported-depth log is once-only
    bool rev_warned; // the revision-read log is once-only

    // The 2D engine (rage128_2d.c): a host-data operation in flight — the
    // rectangle is set up, then its pixels arrive one HOST_DATA write at a
    // time until it is full.
    struct {
        bool active;
        bool byte_aligned; // each line starts in a fresh dword / byte
        int32_t x0, y0, w, h;
        uint32_t col, row;
        uint32_t bits, nbits;
    } host;
    uint64_t blits; // operations the engine has run (diagnostics)

    r128_cce_t cce; // the Concurrent Command Engine (rage128_cce.c)
    uint64_t prims3d; // 3D primitives rasterised (diagnostics)
    uint8_t fog_table[256]; // FOG_TABLE_DATA, through FOG_TABLE_INDEX
    uint8_t fog_index;
    uint8_t told3d; // once-only 3D logs (rage128_raster.c)
} rage128_t;

// === The register file (rage128.c) =========================================

// A full-dword register write with all its side effects — what the CCE's
// type-0/1 packets and its 2D packets do to the register file.
void r128_reg_store(rage128_t *r, uint32_t off, uint32_t value);

// === The 2D draw engine (rage128_2d.c) =====================================

// The first GUI register offset; everything from here up is the engines'.
#define R_GUI_FIRST 0x1400u

// A write to GUI register `off` has been stored in reg[]: apply its side
// effects (the DP_GUI_MASTER_CNTL defaults, the combined coordinate
// registers, and the initiators that run an operation).
void r128_2d_write(rage128_t *r, uint32_t off, uint32_t value);

// The engine's power-on register state (scissors open, write mask all ones).
void r128_2d_reset(rage128_t *r);

// The data path's brush at (x, y): false where a mono foreground/leave-alone
// brush leaves the pixel alone (the 3D engine's polygon stipple).
bool r128_2d_brush_covers(rage128_t *r, int32_t x, int32_t y);
// The ternary raster operation on pattern, source and destination.
uint32_t r128_2d_rop3(uint32_t rop, uint32_t p, uint32_t s, uint32_t d);

// === The 3D engine (rage128_raster.c) ======================================

// One FTLVERTEX, decoded: position in screen pixels (before the window
// offset), Z in [0,1], RHW, colours as 0..255 floats, the fog factor, and
// two texture coordinate sets.
typedef struct r128_vertex {
    float x, y, z, rhw;
    float c[4]; // diffuse R, G, B, A
    float spec[3]; // specular R, G, B
    float fog; // 255 = no fog
    float s, t, s2, t2, rhw2;
} r128_vertex_t;

// VC_FORMAT (SDK appendix F, 3D_RNDR_GEN_PRIM): which fields a vertex has.
#define VCF_RHW          0x001u
#define VCF_DIFFUSE_BGR  0x002u
#define VCF_DIFFUSE_A    0x004u
#define VCF_DIFFUSE_ARGB 0x008u
#define VCF_SPEC_BGR     0x010u
#define VCF_SPEC_F       0x020u
#define VCF_SPEC_FRGB    0x040u
#define VCF_ST           0x080u
#define VCF_S2T2         0x100u
#define VCF_RHW2         0x200u

// Dwords one vertex of format `fmt` occupies.
uint32_t r128_3d_vertex_dwords(uint32_t fmt);
// Decode one vertex from its dwords.
void r128_3d_vertex_decode(uint32_t fmt, const uint32_t *dw, r128_vertex_t *v);
// Rasterise `n` vertices as primitive type `prim` (VC_CNTL.VC_PRIM_TYPE:
// 1 points, 2 lines, 3 polyline, 4 triangles, 5 fan, 6 strip) with the
// engine's current state.
void r128_3d_draw(rage128_t *r, uint32_t prim, const r128_vertex_t *v, uint32_t n);

// === The Concurrent Command Engine (rage128_cce.c) =========================

// The CCE's registers: the PM4 block at $0700-$07FF and the PIO FIFO ports
// at $1000-$13FF.
static inline bool r128_cce_owns(uint32_t off) {
    return (off >= 0x0700u && off < 0x0800u) || (off >= 0x1000u && off < 0x1400u);
}
// A write to a CCE register (`value` already lane-merged); stores it and
// runs whatever it starts.
void r128_cce_write(rage128_t *r, uint32_t off, uint32_t value);
// A read of a CCE register; `peek` suppresses side effects.
uint32_t r128_cce_read(rage128_t *r, uint32_t off, bool peek);
// RST#: the engine's power-on state.  SOFT_RESET_GUI: drop partial packets.
void r128_cce_reset(rage128_t *r);
void r128_cce_soft_reset(rage128_t *r);
// Fetch `n` little-endian dwords at a card address (frame buffer below
// 32 MB, the AGP window above it, through the GART); false if it cannot.
bool r128_card_read(rage128_t *r, uint32_t addr, uint32_t *out, uint32_t n);
// "known", "unknown" or "none": what the guest uploaded as microcode.
const char *r128_cce_microcode_name(const rage128_t *r);

#endif // PCI_RAGE128_PRIV_H
