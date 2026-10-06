// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rage128.c
// The ATI Rage 128 GL as a retail Macintosh PCI display card — the Rage
// Orion / Xclaim VR 128 / Nexus 128, PCI $1002:$5245 ("RE"), Open Firmware
// node `ATY,Rage128v` (Xclaim) or `ATY,Rage128n` (Nexus).
//
// Like the Mach64 GX this card is driven by its OWN firmware: its 128 KB
// expansion ROM carries the FCode Open Firmware runs at probe time and the
// `.Display_Rage128` ndrv Mac OS loads.  The emulator interprets none of
// it.  It answers registers; the guest's firmware does the rest.
//
// This file is milestone 4b of the card: the PCI face, the apertures and
// their byte-order swappers, the register file, the PLL file, monitor sense
// and DDC, the palette, the CRTC turned into a display descriptor, and the
// VBLANK interrupt.  The 2D engine, the Concurrent Command Engine and the
// 3D engine are later milestones; their registers are plain storage here,
// and the engine-status registers report idle.
//
// Software shape — what the card's own FCode asks for (its `reg` property,
// and the decode of its probe):
//
//   * BAR0 ($10): 64 MB prefetchable memory — TWO identical 32 MB linear
//     apertures over the same VRAM, each with its own byte-order swapper
//     (CONFIG_CNTL APER_0_ENDIAN / APER_1_ENDIAN), so a Mac driver can keep
//     a little-endian view for the engines and a big-endian one for
//     QuickDraw.  Aperture 1's base is BAR0 + 32 MB: the low bit of
//     CONFIG_APER_1_BASE's field is hardwired to one.
//   * BAR1 ($14): 256 bytes of I/O — the non-GUI registers $00-$FF.  NOT
//     in the FCode's `reg` property, and load-bearing anyway: at probe time,
//     before BAR0/BAR2 are mapped, the FCode maps THIS BAR and reaches the
//     whole register file indirectly through MM_INDEX / MM_DATA.
//   * BAR2 ($18): 16 KB memory — two identical 8 KB register apertures;
//     the second (BAR2 + 8 KB) is the one CONFIG_CNTL APER_REG_ENDIAN
//     byte-swaps.
//   * The expansion ROM ($30): 128 KB.
//   * A capability list at $50: AGP 1.0 (present on the PCI part too) then
//     power management at $5C, which terminates it.
//
// ENDIANNESS.  The register file and VRAM are little-endian, as the chip
// sees them.  Each aperture applies its swap at its own edge, here; the
// display descriptor is built big-endian (the display layer's convention)
// by swapping direct-colour pixels at scanout.
//
// Register truth: ATI, *RAGE 128 VR / RAGE 128 GL Register Reference
// Guide* (RRG-G04100-C Rev 0.02, 1999) — chapters 2-6 and Appendix A;
// ATI, *RAGE 128 Software Development Guide* (SDK-G04000 Rev 0.01, 1999)
// chapter 3 (mode setting); and the card's own ROM, detokenized.  The
// Linux aty128fb driver was used for orientation only.

#include "checkpoint.h"
#include "config_space.h"
#include "display.h"
#include "display_class.h"
#include "log.h"
#include "memory.h"
#include "object.h"
#include "pci.h"
#include "prom.h"
#include "rage128_priv.h"
#include "scheduler.h"
#include "system.h"
#include "system_config.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("video");

// A monitor without tie resistors: the two undriven lines read their own
// strapped levels.
#define STRAP_EXT01(p) (uint8_t)((((p) >> 1) & 1u) * 2u + ((p) & 1u))
#define STRAP_EXT02(p) (uint8_t)((((p) >> 2) & 1u) * 2u + ((p) & 1u))
#define STRAP_EXT12(p) (uint8_t)((((p) >> 2) & 1u) * 2u + (((p) >> 1) & 1u))

static const r128_monitor_t r128_sense[] = {
    // A VGA monitor that answers DDC: the FCode publishes its EDID.
    {"vga",       false, true,  7, 3,              3,              3             },
    // A VGA monitor with no DDC (or a dumb adapter): VGA, no EDID.
    {"vga_noddc", false, false, 7, 3,              3,              3             },
    // 13" AppleColor High-Resolution RGB on an Apple cable: code $62B.
    {"13in_rgb",  true,  false, 6, STRAP_EXT01(6), STRAP_EXT02(6), STRAP_EXT12(6)},
    // 21" Macintosh Color Display: code $000.
    {"21in_rgb",  true,  false, 0, STRAP_EXT01(0), STRAP_EXT02(0), STRAP_EXT12(0)},
    {NULL,        false, false, 0, 0,              0,              0             },
};

static const int depths_8_16_32[] = {8, 16, 32, 0};

static const struct nubus_monitor r128_monitors[] = {
    {.id = "vga", .monitor = "vga", .width = 640, .height = 480, .depths = depths_8_16_32, .sense_code = 7},
    {.id = "vga_noddc", .monitor = "vga", .width = 640, .height = 480, .depths = depths_8_16_32, .sense_code = 7},
    {.id = "13in_rgb", .monitor = "13in_rgb", .width = 640, .height = 480, .depths = depths_8_16_32, .sense_code = 6},
    {.id = "21in_rgb", .monitor = "21in_rgb", .width = 1152, .height = 870, .depths = depths_8_16_32, .sense_code = 0},
    {.id = NULL},
};

static void r128_update(rage128_t *r);
static void r128_offset_write(rage128_t *r);
static void r128_irq_sync(rage128_t *r);
static uint32_t r128_reg_read(rage128_t *r, uint32_t off, bool peek);
static void r128_reg_write(rage128_t *r, uint32_t off, uint32_t value, uint32_t mask);

// ============================================================
// Raster timing
// ============================================================
// No pixel clock is modelled: the raster is produced at the host frame
// rate, and the CRTC's live status only has to be plausible and ADVANCE —
// a driver polling for vertical blank otherwise spins for ever (the Mach64
// precedent).  The phase of a nominal 60 Hz frame against the programmed
// vertical total is enough.

static uint32_t r128_scanline(const rage128_t *r, uint32_t *out_vtotal) {
    uint32_t vtotal = (r->reg[R_CRTC_V_TOTAL_DISP / 4] & 0x7FFu) + 1u;
    if (vtotal < 2u || vtotal > 4096u)
        vtotal = 525u;
    if (out_vtotal)
        *out_vtotal = vtotal;
    uint64_t frame = r->cfg->machine->freq / 60u;
    if (!frame)
        return 0;
    uint64_t pos = scheduler_cpu_cycles(r->cfg->scheduler) % frame;
    return (uint32_t)(pos * vtotal / frame);
}

static bool r128_in_vblank(const rage128_t *r) {
    uint32_t vtotal = 0;
    uint32_t line = r128_scanline(r, &vtotal);
    uint32_t v_disp = ((r->reg[R_CRTC_V_TOTAL_DISP / 4] >> 16) & 0x7FFu) + 1u;
    if (v_disp >= vtotal)
        v_disp = vtotal - vtotal / 16u; // unprogrammed: a plausible blank
    return line >= v_disp;
}

// ============================================================
// Monitor sense and DDC
// ============================================================

// The level the monitor's side puts on pad `pad` given which pads the card
// is driving low (`driven`, a pad mask).  1 = released (pulled up).
static bool r128_monitor_pad(const rage128_t *r, uint32_t pad, uint32_t driven) {
    const r128_monitor_t *mon = r->mon;
    if (!mon)
        return true; // no cable: every pad floats high
    if (pad == MONID_PAD_LOOP)
        // The Apple cable loops the card's VSYNC polarity back; a VGA cable
        // leaves the pad floating.
        return mon->apple_sense ? (r->reg[R_CRTC_V_SYNC_STRT_WID / 4] & CRTC_V_SYNC_POL) != 0 : true;
    if (!mon->apple_sense)
        return true; // VGA: the sense lines are not tied
    // Pads -> logical sense lines: MONID0 = SENSE0, MONID2 = SENSE1,
    // MONID1 = SENSE2.
    static const uint8_t sense_of_pad[3] = {0, 2, 1};
    uint32_t s = sense_of_pad[pad];
    uint32_t drv = 0; // the driven sense lines
    for (uint32_t p = 0; p < 3; p++)
        if (driven & (1u << p))
            drv |= 1u << sense_of_pad[p];
    switch (drv & ~(1u << s)) {
    case 0: // nothing else driven: the strap
        return (mon->primary >> s) & 1u;
    case 4: // SENSE2 driven: (SENSE1, SENSE0) = ext01
        return s == 1 ? (mon->ext01 >> 1) & 1u : mon->ext01 & 1u;
    case 2: // SENSE1 driven: (SENSE2, SENSE0) = ext02
        return s == 2 ? (mon->ext02 >> 1) & 1u : mon->ext02 & 1u;
    case 1: // SENSE0 driven: (SENSE2, SENSE1) = ext12
        return s == 2 ? (mon->ext12 >> 1) & 1u : mon->ext12 & 1u;
    default:
        return true; // two lines driven: not a step the Apple walk takes
    }
}

// The pads the card itself is driving low: EN = 1 and A = 0.
static uint32_t r128_card_driven(const rage128_t *r) {
    uint32_t v = r->reg[R_GPIO_MONID / 4];
    uint32_t en = (v >> MONID_EN_SHIFT) & 0xFu;
    return en & ~v & 0xFu;
}

// The wired-AND level of pad `pad`: the card, the monitor's ties and, on
// the data line, the DDC slave all pull against one pull-up.
static bool r128_pad_level(const rage128_t *r, uint32_t pad) {
    uint32_t driven = r128_card_driven(r);
    if (driven & (1u << pad))
        return false;
    if (r->mon && r->mon->ddc && pad == MONID_PAD_SDA && r->ddc.sda_low)
        return false;
    return r128_monitor_pad(r, pad, driven);
}

// GPIO_MONID as read: the written fields plus the live Y levels.
static uint32_t r128_gpio_monid(const rage128_t *r) {
    uint32_t v = r->reg[R_GPIO_MONID / 4] & ~(0xFu << MONID_Y_SHIFT);
    for (uint32_t p = 0; p < MONID_PADS; p++)
        if (r128_pad_level(r, p))
            v |= 1u << (MONID_Y_SHIFT + p);
    return v;
}

// Load the next EEPROM byte for transmission and put its MSB on the line.
static void ddc_tx_load(rage128_t *r) {
    r128_ddc_t *d = &r->ddc;
    d->shift = r->edid[d->ptr & 0x7Fu];
    d->ptr = (uint8_t)((d->ptr + 1u) & 0x7Fu);
    d->nbits = 0;
    d->sda_low = !(d->shift & 0x80u);
    d->state = DDC_TX;
}

// Step the I2C slave after a GPIO_MONID write moved the lines.  The master
// is bit-banged by the guest, so this only ever sees levels, never timing:
// a START or STOP is SDA moving while SCL is high, a bit is sampled on SCL's
// rising edge, and the slave changes SDA only while SCL is low.
static void r128_ddc_step(rage128_t *r) {
    r128_ddc_t *d = &r->ddc;
    if (!r->mon || !r->mon->ddc)
        return;
    // The master's view of SDA excludes our own pull: a START/STOP is the
    // master's doing.
    bool scl = r128_pad_level(r, MONID_PAD_SCL);
    bool sda_master = !(r128_card_driven(r) & (1u << MONID_PAD_SDA));
    bool sda = sda_master && !d->sda_low;
    if (scl && d->scl && sda_master != d->sda) {
        if (!sda_master) { // START (or repeated START)
            d->state = DDC_RX;
            d->is_addr = true;
            d->nbits = 0;
            d->shift = 0;
            d->sda_low = false;
        } else { // STOP
            d->state = DDC_IDLE;
            d->sda_low = false;
        }
        d->scl = scl;
        d->sda = sda_master;
        return;
    }
    bool rise = scl && !d->scl;
    bool fall = !scl && d->scl;
    d->scl = scl;
    d->sda = sda_master;
    switch ((ddc_state_t)d->state) {
    case DDC_RX:
        if (rise) {
            d->shift = (uint8_t)((d->shift << 1) | (sda ? 1u : 0u));
            d->nbits++;
        } else if (fall && d->nbits == 8) {
            if (d->is_addr) {
                if ((d->shift >> 1) != DDC_EDID_ADDR) {
                    d->state = DDC_IDLE; // not us: no ACK
                    return;
                }
                d->reading = (d->shift & 1u) != 0;
                d->is_addr = false;
            } else {
                d->ptr = (uint8_t)(d->shift & 0x7Fu); // the word address
            }
            d->sda_low = true; // ACK
            d->state = DDC_ACK_OUT;
        }
        break;
    case DDC_ACK_OUT:
        if (fall) {
            if (d->reading) {
                ddc_tx_load(r);
            } else {
                d->sda_low = false;
                d->nbits = 0;
                d->shift = 0;
                d->state = DDC_RX;
            }
        }
        break;
    case DDC_TX:
        if (fall) {
            d->nbits++;
            if (d->nbits == 8) {
                d->sda_low = false; // release for the master's ACK
                d->state = DDC_ACK_IN;
            } else {
                d->sda_low = !((d->shift << d->nbits) & 0x80u);
            }
        }
        break;
    case DDC_ACK_IN:
        if (rise)
            d->master_ack = !sda_master;
        else if (fall) {
            if (d->master_ack)
                ddc_tx_load(r);
            else
                d->state = DDC_IDLE;
        }
        break;
    case DDC_IDLE:
    default:
        break;
    }
}

// A 128-byte EDID 1.3 block for a 640 x 480 VGA monitor: the standard
// header, a made-up "GSM" vendor, the established 640 x 480 @ 60 timing and
// one detailed timing for it, a monitor name, and the checksum.
static void r128_build_edid(rage128_t *r) {
    uint8_t *e = r->edid;
    memset(e, 0, sizeof(r->edid));
    static const uint8_t header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    memcpy(e, header, sizeof(header));
    e[8] = 0x1E; // manufacturer "GSM": 5-bit letters G=7, S=19, M=13
    e[9] = 0x6D;
    e[10] = 0x01; // product code
    e[16] = 1; // week
    e[17] = 9; // year 1999
    e[18] = 1; // EDID 1.3
    e[19] = 3;
    e[20] = 0x0E; // analog input
    e[21] = 32; // 32 x 24 cm
    e[22] = 24;
    e[23] = 120; // gamma 2.2
    e[24] = 0x0A; // RGB colour, preferred timing in DTD 1
    e[35] = 0x20; // established timings: 640 x 480 @ 60
    for (int i = 38; i < 54; i++)
        e[i] = 0x01; // no standard timings
    // Detailed timing 1: 640 x 480 @ 60 Hz, 25.175 MHz.
    uint8_t *t = e + 54;
    t[0] = (uint8_t)(2518 & 0xFF);
    t[1] = (uint8_t)(2518 >> 8);
    t[2] = 640 & 0xFF; // H active
    t[3] = 160 & 0xFF; // H blank
    t[4] = (uint8_t)(((640 >> 8) << 4) | (160 >> 8));
    t[5] = 480 & 0xFF; // V active
    t[6] = 45; // V blank
    t[7] = (uint8_t)(((480 >> 8) << 4) | (45 >> 8));
    t[8] = 16; // H sync offset
    t[9] = 96; // H sync width
    t[10] = (10 << 4) | 2; // V sync offset / width
    t[11] = 0;
    t[12] = 0x40; // 320 x 240 mm
    t[13] = 0xF0;
    t[14] = 0x10;
    t[17] = 0x18; // separate sync, both negative
    // Descriptor 2: monitor name.
    uint8_t *n = e + 72;
    n[3] = 0xFC;
    memcpy(n + 5, "GS VGA\n     ", 13);
    // Descriptors 3, 4: dummy.
    e[90 + 3] = 0x10;
    e[108 + 3] = 0x10;
    uint8_t sum = 0;
    for (int i = 0; i < 127; i++)
        sum = (uint8_t)(sum + e[i]);
    e[127] = (uint8_t)(0x100u - sum);
}

// ============================================================
// The register file
// ============================================================

// Reads that are not simply the stored value.
static uint32_t r128_reg_read(rage128_t *r, uint32_t off, bool peek) {
    off &= (R128_REG_APER_SIZE - 1u) & ~3u;
    if (!peek)
        LOG(6, "Rage 128: reg $%04X read", off);
    if (r128_cce_owns(off))
        return r128_cce_read(r, off, peek);
    switch (off) {
    case R_MM_DATA: {
        uint32_t idx = r->reg[R_MM_INDEX / 4];
        if (idx & MM_APER) {
            // Linear aperture 0 through the index pair: VRAM, no swap.
            uint32_t a = idx & MM_ADDR_MASK;
            if (a + 4u > r->vram_size)
                return 0xFFFFFFFFu;
            return (uint32_t)r->vram[a] | ((uint32_t)r->vram[a + 1] << 8) | ((uint32_t)r->vram[a + 2] << 16) |
                   ((uint32_t)r->vram[a + 3] << 24);
        }
        uint32_t a = idx & MM_ADDR_MASK;
        if (a >= R128_REG_APER_SIZE || a == R_MM_DATA)
            return 0xFFFFFFFFu;
        return r128_reg_read(r, a, peek);
    }
    case R_CLOCK_CNTL_DATA:
        return r->pll[r->reg[R_CLOCK_CNTL_INDEX / 4] & PLL_ADDR_MASK];
    case R_GEN_INT_STATUS:
        return r->reg[off / 4] & INT_STATUS_MASK;
    case R_CRTC_STATUS:
        return (r->reg[off / 4] & CRTC_VBLANK_SAVE) | (r128_in_vblank(r) ? CRTC_VBLANK_CUR : 0u);
    case R_CRTC_OFFSET:
    case R_CRTC_OFFSET_CNTL:
        return (r->reg[off / 4] & ~CRTC_GUI_TRIG_OFFSET) | (r->flip_pending ? CRTC_GUI_TRIG_OFFSET : 0u);
    case R_CRTC_VLINE_CRNT:
        // Bits 26:16 are the live current line; 10:0 the programmed compare.
        return (r->reg[off / 4] & 0x7FFu) | ((r128_scanline(r, NULL) & 0x7FFu) << 16);
    case R_GPIO_MONID:
        return r128_gpio_monid(r);
    case R_DAC_CNTL: {
        uint32_t v = r->reg[off / 4] & ~DAC_CMP_OUTPUT;
        // Load detection: with the comparators enabled, a terminated input
        // halves the test level and every comparator reads low.
        if ((v & DAC_COMP_EN) && r->mon)
            v |= DAC_CMP_OUTPUT;
        return v;
    }
    case R_PALETTE_INDEX:
        return (uint32_t)r->pal_w | ((uint32_t)r->pal_r << 16);
    case R_PALETTE_DATA: {
        uint8_t i = r->pal_r;
        if (!peek)
            r->pal_r++;
        return ((uint32_t)r->clut[i][0] << 16) | ((uint32_t)r->clut[i][1] << 8) | r->clut[i][2];
    }
    case R_CONFIG_CNTL:
        return r->reg[off / 4] & CONFIG_CNTL_WMASK; // CFG_ATI_REV_ID reads 0
    case R_CONFIG_APER_0_BASE:
        return pci_cfg_bar_base(r->dev, R128_BAR_APER) & 0xFC000000u;
    case R_CONFIG_APER_1_BASE:
        return (pci_cfg_bar_base(r->dev, R128_BAR_APER) & 0xFC000000u) | R128_APER_HALF;
    case R_CONFIG_APER_SIZE:
        return R128_APER_HALF;
    case R_CONFIG_REG_1_BASE:
        return (pci_cfg_bar_base(r->dev, R128_BAR_REGS) & 0xFFFFC000u) | R128_REG_APER_SIZE;
    case R_CONFIG_REG_APER_SIZE:
        return R128_REG_APER_SIZE;
    case R_CONFIG_MEMSIZE_EMB:
        return 0; // no embedded memory
    case R_GUI_STAT:
        return GUI_FIFO_FREE; // idle, FIFO empty
    case R_PC_NGUI_CTLSTAT:
    case R_PC_GUI_CTLSTAT:
        return r->reg[off / 4] & 0x00FFFFFFu; // the pixel cache is never busy
    default:
        break;
    }
    if (off >= R_CFG_MIRROR && off < R_CFG_MIRROR_END) {
        // The read-only copy of configuration space.
        if (off == R_CFG_MIRROR + 8u && !peek && !r->rev_warned) {
            r->rev_warned = true;
            LOG(1, "Rage 128: the guest read REVISION_ID through the register mirror ($%02X)", R128_REVISION);
        }
        return pci_cfg_read(r->dev, off - R_CFG_MIRROR);
    }
    return r->reg[off / 4];
}

// Write `value` to the register at `off`; `mask` names the byte lanes the
// access carried (a byte write to a dword register is "add 1, 2 or 3 to the
// address", RRG Table 2-2).
static void r128_reg_write(rage128_t *r, uint32_t off, uint32_t value, uint32_t mask) {
    off &= (R128_REG_APER_SIZE - 1u) & ~3u;
    uint32_t *p = &r->reg[off / 4];
    uint32_t merged = (*p & ~mask) | (value & mask);
    switch (off) {
    case R_MM_DATA: {
        uint32_t idx = r->reg[R_MM_INDEX / 4];
        uint32_t a = idx & MM_ADDR_MASK;
        if (idx & MM_APER) {
            for (uint32_t i = 0; i < 4; i++)
                if ((mask >> (8u * i)) & 0xFFu && a + i < r->vram_size)
                    r->vram[a + i] = (uint8_t)(value >> (8u * i));
            return;
        }
        if (a >= R128_REG_APER_SIZE || a == R_MM_DATA) {
            LOG(2, "Rage 128: MM_DATA write through an out-of-range MM_INDEX $%08X", idx);
            return;
        }
        r128_reg_write(r, a, value, mask);
        return;
    }
    case R_CLOCK_CNTL_DATA: {
        uint32_t ci = r->reg[R_CLOCK_CNTL_INDEX / 4];
        uint32_t idx = ci & PLL_ADDR_MASK;
        if (!(ci & PLL_WR_EN)) {
            LOG(3, "Rage 128: PLL $%02X write with PLL_WR_EN clear — ignored", idx);
            return;
        }
        uint32_t v = (r->pll[idx] & ~mask) | (value & mask);
        // The atomic-update handshake completes at once.
        if (idx == PLL_PPLL_REF_DIV || (idx >= PLL_PPLL_DIV_0 && idx <= PLL_PPLL_DIV_3))
            v &= ~PPLL_ATOMIC_UPDATE;
        r->pll[idx] = v;
        LOG(4, "Rage 128: PLL $%02X := $%08X", idx, v);
        return;
    }
    case R_GEN_INT_STATUS:
        LOG(5, "Rage 128: GEN_INT_STATUS ack $%08X", value & mask);
        *p &= ~(value & mask); // write 1 to clear
        r128_irq_sync(r);
        return;
    case R_GEN_INT_CNTL:
        *p = merged;
        r128_irq_sync(r);
        return;
    case R_CRTC_STATUS:
        if (value & mask & CRTC_VBLANK_SAVE)
            *p &= ~CRTC_VBLANK_SAVE;
        return;
    case R_GPIO_MONID:
        *p = merged & 0x0F0F000Fu;
        r128_ddc_step(r);
        return;
    case R_PALETTE_INDEX:
        // A byte write sets one index without disturbing the other (the
        // manual recommends exactly that).
        if (mask & 0x000000FFu)
            r->pal_w = (uint8_t)value;
        if (mask & 0x00FF0000u)
            r->pal_r = (uint8_t)(value >> 16);
        return;
    case R_PALETTE_DATA: {
        uint8_t i = r->pal_w++;
        bool eight = (r->reg[R_DAC_CNTL / 4] & DAC_8BIT_EN) != 0;
        for (int c = 0; c < 3; c++) {
            uint8_t v = (uint8_t)(merged >> (16 - 8 * c));
            // In 6-bit mode a write is shifted up two bits (RRG, DAC_8BIT_EN).
            r->clut[i][c] = eight ? v : (uint8_t)(v << 2);
        }
        r->clut_dirty = true;
        return;
    }
    case R_CONFIG_CNTL:
        if ((merged ^ *p) & (APER_REG_ENDIAN | 0xFu))
            LOG(1, "Rage 128: CONFIG_CNTL := $%08X (aperture 0 %u, aperture 1 %u, registers %s)",
                merged & CONFIG_CNTL_WMASK, APER_0_ENDIAN(merged), APER_1_ENDIAN(merged),
                (merged & APER_REG_ENDIAN) ? "swapped" : "straight");
        *p = merged & CONFIG_CNTL_WMASK;
        return;
    case R_GEN_RESET_CNTL:
        // Raising SOFT_RESET_GUI resets the engines' sequencing — a packet
        // half gathered, a host-data operation half fed — not their
        // registers (the drivers' reset dance, SDK §5.2).
        if ((merged & SOFT_RESET_GUI) && !(*p & SOFT_RESET_GUI))
            r128_cce_soft_reset(r);
        *p = merged;
        return;
    case R_CONFIG_APER_0_BASE:
    case R_CONFIG_APER_1_BASE:
    case R_CONFIG_APER_SIZE:
    case R_CONFIG_REG_1_BASE:
    case R_CONFIG_REG_APER_SIZE:
    case R_CONFIG_MEMSIZE_EMB:
    case R_GUI_STAT:
        return; // read-only
    default:
        break;
    }
    if (off >= R_CFG_MIRROR && off < R_CFG_MIRROR_END)
        return; // the config mirror is read-only
    if (r128_cce_owns(off)) {
        r128_cce_write(r, off, merged);
        return;
    }
    *p = merged;
    LOG(5, "Rage 128: reg $%04X := $%08X (lanes $%08X)", off, merged, mask);
    if (off >= R_GUI_FIRST) {
        r128_2d_write(r, off, merged);
        return;
    }
    switch (off) {
    case R_CRTC_OFFSET:
        r128_offset_write(r);
        break;
    case R_CRTC_GEN_CNTL:
    case R_CRTC_EXT_CNTL:
    case R_CRTC_H_TOTAL_DISP:
    case R_CRTC_V_TOTAL_DISP:
    case R_CRTC_OFFSET_CNTL:
    case R_CRTC_PITCH:
    case R_DAC_CNTL:
        r128_update(r);
        break;
    default:
        break;
    }
}

void r128_reg_store(rage128_t *r, uint32_t off, uint32_t value) {
    r128_reg_write(r, off, value, 0xFFFFFFFFu);
}

// One access of `size` bytes at aperture offset `off`, little-endian
// register semantics: lane n of the dword carries bits 8n+7:8n.  `swap`
// selects the APER_REG_ENDIAN 32-bit swap.
static uint32_t regs_access_read(rage128_t *r, uint32_t off, uint32_t size, bool swap, bool peek) {
    uint32_t lane = off & 3u;
    uint32_t v = r128_reg_read(r, off, peek);
    if (swap) // the swapped aperture presents the dword in host order
        v = __builtin_bswap32(v);
    // Assemble big-endian from the addressed bytes: byte (off+k) is lane
    // (lane+k) of the little-endian value.
    uint32_t out = 0;
    for (uint32_t k = 0; k < size; k++)
        out = (out << 8) | ((v >> (8u * ((lane + k) & 3u))) & 0xFFu);
    return out;
}

static void regs_access_write(rage128_t *r, uint32_t off, uint32_t size, uint32_t value, bool swap) {
    uint32_t lane = off & 3u;
    uint32_t v = 0, mask = 0;
    for (uint32_t k = 0; k < size; k++) {
        uint32_t b = (value >> (8u * (size - 1u - k))) & 0xFFu;
        uint32_t sh = 8u * ((lane + k) & 3u);
        v |= b << sh;
        mask |= 0xFFu << sh;
    }
    if (swap) {
        v = __builtin_bswap32(v);
        mask = __builtin_bswap32(mask);
    }
    r128_reg_write(r, off, v, mask);
}

// ============================================================
// BAR2 — the two register apertures
// ============================================================
// Aperture 0 at +0 is always little-endian; aperture 1 at +8 KB applies the
// APER_REG_ENDIAN swap.  The manual names one swap bit for "the register
// apertures"; which of the two it governs is not stated, and swapping the
// aperture a Linux driver uses would break that driver, so the swap is the
// second aperture's — the one the manual says exists "for the PowerMac".

static bool regs_swap(const rage128_t *r, uint32_t off) {
    return off >= R128_REG_APER_SIZE && (r->reg[R_CONFIG_CNTL / 4] & APER_REG_ENDIAN);
}

static uint8_t regs_rd8(void *ctx, uint32_t off, bool peek) {
    rage128_t *r = (rage128_t *)ctx;
    return (uint8_t)regs_access_read(r, off, 1, regs_swap(r, off), peek);
}
static uint16_t regs_rd16(void *ctx, uint32_t off, bool peek) {
    rage128_t *r = (rage128_t *)ctx;
    return (uint16_t)regs_access_read(r, off, 2, regs_swap(r, off), peek);
}
static uint32_t regs_rd32(void *ctx, uint32_t off, bool peek) {
    rage128_t *r = (rage128_t *)ctx;
    return regs_access_read(r, off, 4, regs_swap(r, off), peek);
}
static void regs_write8(void *ctx, uint32_t off, uint8_t v) {
    rage128_t *r = (rage128_t *)ctx;
    regs_access_write(r, off, 1, v, regs_swap(r, off));
}
static void regs_write16(void *ctx, uint32_t off, uint16_t v) {
    rage128_t *r = (rage128_t *)ctx;
    regs_access_write(r, off, 2, v, regs_swap(r, off));
}
static void regs_write32(void *ctx, uint32_t off, uint32_t v) {
    rage128_t *r = (rage128_t *)ctx;
    regs_access_write(r, off, 4, v, regs_swap(r, off));
}

// ============================================================
// BAR1 — the I/O face: non-GUI registers $00-$FF, little-endian
// ============================================================

static uint8_t io_rd8(void *ctx, uint32_t off, bool peek) {
    return (uint8_t)regs_access_read((rage128_t *)ctx, off & 0xFFu, 1, false, peek);
}
static uint16_t io_rd16(void *ctx, uint32_t off, bool peek) {
    return (uint16_t)regs_access_read((rage128_t *)ctx, off & 0xFFu, 2, false, peek);
}
static uint32_t io_rd32(void *ctx, uint32_t off, bool peek) {
    return regs_access_read((rage128_t *)ctx, off & 0xFFu, 4, false, peek);
}
static void io_write8(void *ctx, uint32_t off, uint8_t v) {
    regs_access_write((rage128_t *)ctx, off & 0xFFu, 1, v, false);
}
static void io_write16(void *ctx, uint32_t off, uint16_t v) {
    regs_access_write((rage128_t *)ctx, off & 0xFFu, 2, v, false);
}
static void io_write32(void *ctx, uint32_t off, uint32_t v) {
    regs_access_write((rage128_t *)ctx, off & 0xFFu, 4, v, false);
}

// ============================================================
// BAR0 — the two linear apertures
// ============================================================
// Each 32 MB aperture maps VRAM from offset 0 to the memory size; the rest
// of the 32 MB is the AGP image on an AGP card and nothing on this one
// (reads float high, writes are dropped).  The swapper of the aperture
// reorders the bytes of each 16- or 32-bit group: CPU byte k of a group
// reaches VRAM byte (group-1-k), so a big-endian host's pixel store lands
// as the little-endian pixel the CRTC scans.

// The VRAM byte a CPU byte address in BAR0 reaches, or -1 for none.
static int64_t aper_map(const rage128_t *r, uint32_t off) {
    uint32_t endian =
        off < R128_APER_HALF ? APER_0_ENDIAN(r->reg[R_CONFIG_CNTL / 4]) : APER_1_ENDIAN(r->reg[R_CONFIG_CNTL / 4]);
    uint32_t a = off & (R128_APER_HALF - 1u);
    if (endian == ENDIAN_16BPP)
        a ^= 1u;
    else if (endian == ENDIAN_32BPP)
        a ^= 3u;
    if (a >= r->vram_size)
        return -1;
    return a;
}

static uint8_t aper_rd8(void *ctx, uint32_t off, bool peek) {
    (void)peek;
    rage128_t *r = (rage128_t *)ctx;
    int64_t a = aper_map(r, off);
    return a < 0 ? 0xFFu : r->vram[a];
}
static uint16_t aper_rd16(void *ctx, uint32_t off, bool peek) {
    return (uint16_t)((aper_rd8(ctx, off, peek) << 8) | aper_rd8(ctx, off + 1u, peek));
}
static uint32_t aper_rd32(void *ctx, uint32_t off, bool peek) {
    return ((uint32_t)aper_rd16(ctx, off, peek) << 16) | aper_rd16(ctx, off + 2u, peek);
}
static void aper_write8(void *ctx, uint32_t off, uint8_t v) {
    rage128_t *r = (rage128_t *)ctx;
    int64_t a = aper_map(r, off);
    if (a < 0) {
        LOG(4, "Rage 128: write past VRAM at aperture offset $%07X — dropped", off);
        return;
    }
    r->vram[a] = v;
}
static void aper_write16(void *ctx, uint32_t off, uint16_t v) {
    aper_write8(ctx, off, (uint8_t)(v >> 8));
    aper_write8(ctx, off + 1u, (uint8_t)v);
}
static void aper_write32(void *ctx, uint32_t off, uint32_t v) {
    aper_write16(ctx, off, (uint16_t)(v >> 16));
    aper_write16(ctx, off + 2u, (uint16_t)v);
}

// ============================================================
// The expansion ROM
// ============================================================

static uint8_t rom_read8(void *ctx, uint32_t off) {
    rage128_t *r = (rage128_t *)ctx;
    if (!r->dev->rom || off >= r->dev->rom_size)
        return 0xFFu;
    return r->dev->rom[off];
}
static uint16_t rom_read16(void *ctx, uint32_t off) {
    return (uint16_t)((rom_read8(ctx, off) << 8) | rom_read8(ctx, off + 1u));
}
static uint32_t rom_read32(void *ctx, uint32_t off) {
    return ((uint32_t)rom_read16(ctx, off) << 16) | rom_read16(ctx, off + 2u);
}
static void rom_write8(void *ctx, uint32_t off, uint8_t v) {
    (void)ctx;
    LOG(2, "Rage 128: write to the expansion ROM at +$%05X = $%02X — ignored", off, v);
}
static void rom_write16(void *ctx, uint32_t off, uint16_t v) {
    rom_write8(ctx, off, (uint8_t)(v >> 8));
}
static void rom_write32(void *ctx, uint32_t off, uint32_t v) {
    rom_write8(ctx, off, (uint8_t)(v >> 24));
}

// Stamp out the read_*/peek_* pairs around a face's rd8/16/32.
#define R128_READ_FACE(face)                                                                                           \
    static uint8_t face##_read8(void *c, uint32_t a) {                                                                 \
        return face##_rd8(c, a, false);                                                                                \
    }                                                                                                                  \
    static uint16_t face##_read16(void *c, uint32_t a) {                                                               \
        return face##_rd16(c, a, false);                                                                               \
    }                                                                                                                  \
    static uint32_t face##_read32(void *c, uint32_t a) {                                                               \
        return face##_rd32(c, a, false);                                                                               \
    }                                                                                                                  \
    static uint8_t face##_peek8(void *c, uint32_t a) {                                                                 \
        return face##_rd8(c, a, true);                                                                                 \
    }                                                                                                                  \
    static uint16_t face##_peek16(void *c, uint32_t a) {                                                               \
        return face##_rd16(c, a, true);                                                                                \
    }                                                                                                                  \
    static uint32_t face##_peek32(void *c, uint32_t a) {                                                               \
        return face##_rd32(c, a, true);                                                                                \
    }
R128_READ_FACE(regs)
R128_READ_FACE(io)
R128_READ_FACE(aper)

// ============================================================
// Scanout — the CRTC registers become a display_t
// ============================================================
//
//   width  = (CRTC_H_DISP + 1) x 8      CRTC_H_TOTAL_DISP bits 23:16
//   height =  CRTC_V_DISP + 1           CRTC_V_TOTAL_DISP bits 26:16
//   stride =  CRTC_PITCH x 8 pixels     CRTC_PITCH bits 9:0
//   base   =  CRTC_OFFSET bytes         bits 24:0 (the card's FCode writes
//                                       $8000 and publishes BAR0 + $8000
//                                       as the frame buffer: bytes)
//   on     =  CRTC_EN, CRTC_EXT_DISP_EN, display requests enabled and
//             CRTC_EXT_CNTL.CRTC_DISPLAY_DIS clear
//
// VRAM holds little-endian pixels; the display layer takes big-endian ones,
// so the direct-colour depths are presented through `compose`, swapped.

static uint32_t r128_bytes_per_pixel(uint32_t pix) {
    switch (pix) {
    case CRTC_PIX_15BPP:
    case CRTC_PIX_16BPP:
        return 2;
    case CRTC_PIX_24BPP:
        return 3;
    case CRTC_PIX_32BPP:
        return 4;
    default:
        return 1;
    }
}

static void r128_refresh_clut(rage128_t *r) {
    uint8_t mask = (uint8_t)(r->reg[R_DAC_CNTL / 4] >> 24);
    for (uint32_t i = 0; i < 256; i++) {
        // DAC_MASK gates the index bits before the lookup.
        uint32_t j = i & mask;
        r->clut_view[i] = (rgba8_t){.r = r->clut[j][0], .g = r->clut[j][1], .b = r->clut[j][2], .a = 255};
    }
    r->display.clut = r->clut_view;
    r->display.clut_len = 256;
    r->display.clut_dirty = true;
}

// The hardware cursor (SDK §4.4): a 64 x 64 map in VRAM at CUR_OFFSET, each
// line 16 bytes — eight bytes of AND bits then eight of XOR bits, the
// leftmost pixel in bit 7 of the first byte.  AND/XOR 00 = colour 0, 01 =
// colour 1, 10 = transparent, 11 = the complement of the pixel beneath.
// CUR_HORZ_VERT_OFF says where in the map the visible part starts (the
// driver moves it to clip a cursor off the top or left edge, since a
// negative position is not displayed at all); CUR_HORZ_VERT_POSN is where
// on screen that part's top-left lands.  The colours are always 24-bit RGB.
#define CUR_SIZE       64u
#define CUR_LINE_BYTES 16u
#define CUR_POSN_V(v)  ((v) & 0x7FFu) // CUR_HORZ_VERT_POSN bits 10:0
#define CUR_POSN_H(v)  (((v) >> 16) & 0x7FFu) // ...and 26:16
#define CUR_OFF_V(v)   ((v) & 0x3Fu) // CUR_HORZ_VERT_OFF bits 5:0
#define CUR_OFF_H(v)   (((v) >> 16) & 0x3Fu) // ...and 21:16

// A 24-bit cursor colour in the frame's own (big-endian) pixel format.  In
// 8 bpp the DAC overlays true colour, so the composite — built in the
// frame's depth — takes the nearest palette entry.
static uint32_t r128_cursor_pixel(const rage128_t *r, uint32_t rgb, uint32_t bpp) {
    uint32_t cr = (rgb >> 16) & 0xFFu, cg = (rgb >> 8) & 0xFFu, cb = rgb & 0xFFu;
    if (bpp == 1) {
        uint32_t best = 0, best_d = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < 256; i++) {
            int dr = (int)r->clut_view[i].r - (int)cr, dg = (int)r->clut_view[i].g - (int)cg,
                db = (int)r->clut_view[i].b - (int)cb;
            uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
            if (d < best_d) {
                best_d = d;
                best = i;
            }
        }
        return best;
    }
    if (bpp == 2) {
        if (r->display.format == PIXEL_16BPP_565)
            return ((cr >> 3) << 11) | ((cg >> 2) << 5) | (cb >> 3);
        return ((cr >> 3) << 10) | ((cg >> 3) << 5) | (cb >> 3);
    }
    return rgb & 0xFFFFFFu;
}

// Overlay the cursor on `compose`, which holds the frame in display order.
static void r128_draw_cursor(rage128_t *r, uint32_t bpp) {
    uint32_t src = r->reg[R_CUR_OFFSET / 4] & 0x1FFFFF0u;
    if ((uint64_t)src + CUR_SIZE * CUR_LINE_BYTES > r->vram_size)
        return;
    uint32_t posn = r->reg[R_CUR_HORZ_VERT_POSN / 4], off = r->reg[R_CUR_HORZ_VERT_OFF / 4];
    uint32_t px = CUR_POSN_H(posn), py = CUR_POSN_V(posn);
    uint32_t ox = CUR_OFF_H(off), oy = CUR_OFF_V(off);
    uint32_t clr[2] = {r128_cursor_pixel(r, r->reg[R_CUR_CLR0 / 4], bpp),
                       r128_cursor_pixel(r, r->reg[R_CUR_CLR1 / 4], bpp)};
    uint32_t stride = r->display.stride, width = r->display.width, height = r->display.height;
    for (uint32_t row = oy; row < CUR_SIZE; row++) {
        uint32_t y = py + (row - oy);
        if (y >= height)
            break;
        const uint8_t *line = r->vram + src + row * CUR_LINE_BYTES;
        for (uint32_t col = ox; col < CUR_SIZE; col++) {
            uint32_t x = px + (col - ox);
            if (x >= width)
                break;
            uint32_t bit = 7u - (col & 7u);
            uint32_t and_bit = (line[col >> 3] >> bit) & 1u;
            uint32_t xor_bit = (line[8 + (col >> 3)] >> bit) & 1u;
            if (and_bit && !xor_bit)
                continue; // transparent
            uint8_t *at = r->compose + (size_t)y * stride + (size_t)x * bpp;
            for (uint32_t i = 0; i < bpp; i++) {
                uint32_t shift = 8u * (bpp - 1u - i);
                at[i] = and_bit ? (uint8_t)~at[i] : (uint8_t)(clr[xor_bit] >> shift);
            }
        }
    }
}

// Point the descriptor at what the CRTC is producing this frame: VRAM
// straight through (8 bpp, no cursor), or `compose` — the frame byte-swapped
// to big-endian pixels for the direct-colour depths, with the hardware
// cursor composited on top.  The cursor is a CRTC overlay, so it never
// touches VRAM.
static void r128_present(rage128_t *r) {
    if (r->scan_blanked) {
        r->display.bits = r->blank;
        return;
    }
    const uint8_t *frame = r->vram + r->scan_base;
    bool cursor = (r->reg[R_CRTC_GEN_CNTL / 4] & CRTC_CUR_EN) != 0;
    if (!r->scan_swap && !cursor) {
        r->display.bits = (uint8_t *)frame;
        return;
    }
    // display_set_scanout settled stride x height against vram_size, and
    // `compose` is vram_size.
    size_t span = (size_t)r->display.stride * r->display.height;
    uint32_t bpp = r128_bytes_per_pixel(CRTC_PIX_WIDTH(r->reg[R_CRTC_GEN_CNTL / 4]));
    if (bpp == 1) {
        memcpy(r->compose, frame, span);
    } else if (bpp == 2) {
        for (size_t i = 0; i + 1 < span; i += 2) {
            r->compose[i] = frame[i + 1];
            r->compose[i + 1] = frame[i];
        }
    } else {
        for (size_t i = 0; i + 3 < span; i += 4) {
            r->compose[i] = frame[i + 3];
            r->compose[i + 1] = frame[i + 2];
            r->compose[i + 2] = frame[i + 1];
            r->compose[i + 3] = frame[i];
        }
    }
    if (cursor)
        r128_draw_cursor(r, bpp);
    r->display.bits = r->compose;
}

static void r128_update(rage128_t *r) {
    if (!r->blank)
        return;
    uint32_t gen = r->reg[R_CRTC_GEN_CNTL / 4];
    uint32_t pix = CRTC_PIX_WIDTH(gen);
    pixel_format_t format;
    switch (pix) {
    case CRTC_PIX_8BPP:
        format = PIXEL_8BPP;
        break;
    case CRTC_PIX_15BPP:
        format = PIXEL_16BPP_555;
        break;
    case CRTC_PIX_16BPP:
        format = PIXEL_16BPP_565;
        break;
    case CRTC_PIX_32BPP:
        format = PIXEL_32BPP_XRGB;
        break;
    case 0:
        // Power-on: nothing programmed, nothing wrong.
        r->display.bits = r->blank;
        return;
    default:
        // 4 bpp, packed 24 bpp, or reserved: not depths a Mac driver sets.
        if (!r->depth_warned) {
            r->depth_warned = true;
            LOG(0, "Rage 128: CRTC_PIX_WIDTH = %u is not a depth this model renders — keeping the previous mode", pix);
        }
        return;
    }
    uint32_t bpp = r128_bytes_per_pixel(pix);
    uint32_t width = (((r->reg[R_CRTC_H_TOTAL_DISP / 4] >> 16) & 0xFFu) + 1u) * 8u;
    uint32_t height = ((r->reg[R_CRTC_V_TOTAL_DISP / 4] >> 16) & 0x7FFu) + 1u;
    uint32_t stride = (r->reg[R_CRTC_PITCH / 4] & 0x3FFu) * 8u * bpp;
    uint32_t base = r->crtc_offset_live;
    if (width > 2048u || height > 1536u) {
        LOG(2, "Rage 128: implausible CRTC geometry %ux%u — blanking", width, height);
        width = 640;
        height = 480;
        stride = 0;
    }
    bool on = (gen & CRTC_EN) && (gen & CRTC_EXT_DISP_EN) && !(gen & CRTC_DISP_REQ_EN_B) &&
              !(r->reg[R_CRTC_EXT_CNTL / 4] & CRTC_DISPLAY_DIS);
    bool blanked = !on || stride == 0;

    r->display.format = format;
    r->display.par_w = 0;
    r->display.par_h = 0;
    r->display.crt_response = NULL;
    display_set_scanout(&r->display, blanked ? NULL : r->vram, r->vram_size, base, stride ? stride : width * bpp, width,
                        height, r->blank, r->vram_size);
    r->scan_blanked = r->display.bits == r->blank;
    r->scan_base = base;
    r->scan_swap = bpp > 1;
    r128_present(r);
    if (format == PIXEL_8BPP) {
        r128_refresh_clut(r);
    } else {
        r->display.clut = NULL;
        r->display.clut_len = 0;
    }
    r->display.shape_dirty = true;
    r->display.fb_dirty = true;
    LOG(2, "Rage 128: mode %ux%u %u bpp stride=%u base=$%06X%s", width, height, bpp * 8u, r->display.stride, base,
        r->scan_blanked ? " BLANKED" : "");
}

// A CRTC_OFFSET write.  With the CRTC off (a mode set) it applies at once;
// with it running, at the next vertical blank — the double-buffer flip
// (CRTC_OFFSET_FLIP_CNTL asks for the next line instead, which at this
// model's frame granularity is "now").
static void r128_offset_write(rage128_t *r) {
    uint32_t v = r->reg[R_CRTC_OFFSET / 4] & CRTC_OFFSET_MASK;
    bool running = (r->reg[R_CRTC_GEN_CNTL / 4] & CRTC_EN) != 0;
    if (!running || (r->reg[R_CRTC_OFFSET_CNTL / 4] & CRTC_OFFSET_FLIP_CNTL)) {
        r->crtc_offset_live = v;
        r->flip_pending = false;
        r128_update(r);
        return;
    }
    r->flip_pending = v != r->crtc_offset_live;
}

// Vertical blank: a pending flip takes effect unless a lock holds it.
static void r128_flip_latch(rage128_t *r) {
    if (!r->flip_pending)
        return;
    if ((r->reg[R_CRTC_OFFSET_CNTL / 4] | r->reg[R_CRTC_OFFSET / 4]) & CRTC_OFFSET_LOCK)
        return;
    r->crtc_offset_live = r->reg[R_CRTC_OFFSET / 4] & CRTC_OFFSET_MASK;
    r->flip_pending = false;
    LOG(4, "Rage 128: flip to $%06X", r->crtc_offset_live);
    r128_update(r);
}

static display_t *r128_display(pci_device_t *dev) {
    rage128_t *r = (rage128_t *)dev->priv;
    return (r && r->blank) ? &r->display : NULL;
}

// ============================================================
// Interrupts
// ============================================================
// GEN_INT_STATUS latches the event whether or not it is enabled; the
// enables in GEN_INT_CNTL gate only the INTA line, which is level and held
// until the driver acknowledges.

static void r128_irq_sync(rage128_t *r) {
    bool want = (r->reg[R_GEN_INT_STATUS / 4] & r->reg[R_GEN_INT_CNTL / 4] & INT_STATUS_MASK) != 0;
    if (want == r->irq_active)
        return;
    r->irq_active = want;
    if (want)
        pci_assert_irq(r->dev);
    else
        pci_deassert_irq(r->dev);
}

static void r128_on_vbl(pci_device_t *dev, config_t *cfg) {
    (void)cfg;
    rage128_t *r = (rage128_t *)dev->priv;
    if (!r || !r->blank)
        return;
    r128_flip_latch(r);
    // CPU stores to VRAM bypass the renderer: re-upload every frame.
    r->display.fb_dirty = true;
    r128_present(r);
    if (r->clut_dirty) {
        r->clut_dirty = false;
        if (CRTC_PIX_WIDTH(r->reg[R_CRTC_GEN_CNTL / 4]) == CRTC_PIX_8BPP)
            r128_refresh_clut(r);
    }
    r->reg[R_CRTC_CRNT_FRAME / 4] = (r->reg[R_CRTC_CRNT_FRAME / 4] + 1u) & 0x1FFFFFu;
    r->reg[R_CRTC_STATUS / 4] |= CRTC_VBLANK_SAVE;
    r->reg[R_GEN_INT_STATUS / 4] |= INT_CRTC_VBLANK | INT_CRTC_VSYNC;
    r128_irq_sync(r);
}

// ============================================================
// The PCI face
// ============================================================

static const pci_config_decl_t rage128_decl = {
    .vendor_id = R128_VENDOR_ID,
    .device_id = R128_DEVICE_ID,
    .revision = R128_REVISION,
    .class_code = R128_CLASS,
    .header_type = 0x00u,
    .interrupt_pin = 1,
    .command_writable = PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE | PCI_CMD_MASTER,
    // CAP_LIST | PCI_66_EN | FAST_BACK_CAPABLE | DEVSEL medium (RRG §4.1).
    .status_reset = 0x02B0u,
    .cap_ptr = CFG_CAP_AGP,
    .bar =
        {
              [R128_BAR_APER] = {.size = R128_APER_SIZE, .kind = PCI_BAR_MEM_PREFETCH},
              [R128_BAR_IO] = {.size = R128_IO_SIZE, .kind = PCI_BAR_IO},
              [R128_BAR_REGS] = {.size = R128_REGS_SIZE, .kind = PCI_BAR_MEM},
              },
    .rom_size = R128_ROM_SIZE,
};

// Configuration space beyond the generic header: the command register's
// hardwired AD_STEPPING, MIN_GNT, and the capability list.
static bool r128_cfg_read(pci_device_t *dev, uint32_t reg, uint32_t *out) {
    rage128_t *r = (rage128_t *)dev->priv;
    switch (reg) {
    case PCI_CFG_COMMAND:
        *out = ((uint32_t)dev->cfg.status << 16) | dev->cfg.command | 0x80u; // AD_STEPPING
        return true;
    case PCI_CFG_INTERRUPT:
        *out = (CFG_MIN_GNT << 16) | ((uint32_t)dev->decl->interrupt_pin << 8) | dev->cfg.interrupt_line;
        return true;
    case CFG_CAP_AGP:
        *out = AGP_CAP_VALUE;
        return true;
    case CFG_AGP_STATUS:
        *out = AGP_STATUS_VALUE;
        return true;
    case CFG_AGP_CMD:
        *out = r ? r->agp_cmd : AGP_CMD_RESET;
        return true;
    case CFG_CAP_PMI:
        *out = PMI_CAP_VALUE;
        return true;
    case CFG_PMI_PMCSR:
        *out = r ? r->pmcsr : 0;
        return true;
    default:
        return false;
    }
}

static bool r128_cfg_write(pci_device_t *dev, uint32_t reg, uint32_t byte, uint8_t value) {
    rage128_t *r = (rage128_t *)dev->priv;
    uint32_t sh = 8u * (byte & 3u);
    switch (reg) {
    case CFG_AGP_CMD:
        if (r)
            r->agp_cmd = (r->agp_cmd & ~(0xFFu << sh)) | ((uint32_t)value << sh);
        return true;
    case CFG_PMI_PMCSR:
        if (r && byte == 0)
            r->pmcsr = value & 3u; // POWER_STATE
        return true;
    case CFG_CAP_AGP:
    case CFG_AGP_STATUS:
    case CFG_CAP_PMI:
        return true; // read-only
    default:
        return false;
    }
}

static const char *r128_name(const pci_device_t *dev) {
    (void)dev;
    return "ATI Rage 128 GL";
}

// PCI RST#: the chip returns to its power-on state.  VRAM survives, as
// SDRAM does across a warm restart.
static void r128_reset(pci_device_t *dev, config_t *cfg) {
    (void)cfg;
    rage128_t *r = (rage128_t *)dev->priv;
    memset(r->reg, 0, sizeof(r->reg));
    memset(r->pll, 0, sizeof(r->pll));
    dev->cfg.interrupt_line = CFG_INT_LINE_RST;
    r->agp_cmd = AGP_CMD_RESET;
    r->pmcsr = 0;
    // Register reset values the manual states (RRG chapters 3-6).
    r->reg[R_CRTC_GEN_CNTL / 4] = CRTC_DISP_REQ_EN_B;
    r->reg[R_DAC_CNTL / 4] = 0xFF000000u; // DAC_MASK
    r->reg[R_CONFIG_MEMSIZE / 4] = r->vram_size;
    r->reg[R_BUS_CNTL / 4] = BUS_MASTER_DIS;
    r->reg[R_PCI_GART_PAGE / 4] = PCI_GART_DIS;
    r->reg[R_CRTC_EXT_CNTL / 4] = 0x00200000u; // DFIFO_EXTSENSE
    memset(r->clut, 0, sizeof(r->clut));
    r->pal_w = r->pal_r = 0;
    memset(&r->ddc, 0, sizeof(r->ddc));
    r->ddc.scl = r->ddc.sda = true;
    r128_2d_reset(r);
    r128_cce_reset(r);
    memset(r->fog_table, 0, sizeof(r->fog_table));
    r->fog_index = 0;
    r->crtc_offset_live = 0;
    r->flip_pending = false;
    r->prims3d = 0;
    r->told3d = 0;
    r->depth_warned = false;
    if (r->irq_active) {
        r->irq_active = false;
        pci_deassert_irq(dev);
    }
    r128_update(r);
}

static void r128_teardown(pci_device_t *dev, config_t *cfg) {
    (void)cfg;
    rage128_t *r = (rage128_t *)dev->priv;
    if (!r)
        return;
    free(r->vram);
    free(r->blank);
    free(r->compose);
    free(r);
    dev->priv = NULL;
}

// ============================================================
// Checkpoints
// ============================================================
// The generic header is the bus's; the chip's state is saved here, VRAM
// last.  The ROM travels in the slot's checkpoint part, not here.

typedef struct r128_ckpt {
    uint32_t reg[R128_NUM_REGS];
    uint32_t pll[64];
    uint32_t agp_cmd, pmcsr;
    uint32_t vram_size;
    uint8_t clut[256][3];
    uint8_t pal_w, pal_r;
    r128_ddc_t ddc;
    uint8_t host[sizeof(((rage128_t *)0)->host)]; // a 2D host-data operation in flight
    uint8_t fog_table[256]; // the 3D engine's fog table and its index
    uint8_t fog_index;
    uint32_t crtc_offset_live; // the displayed CRTC_OFFSET, and a flip waiting for blank
    uint8_t flip_pending;
} r128_ckpt_t;

static void r128_checkpoint_save(pci_device_t *dev, checkpoint_t *cp) {
    rage128_t *r = (rage128_t *)dev->priv;
    r128_ckpt_t c;
    memset(&c, 0, sizeof(c));
    memcpy(c.reg, r->reg, sizeof(c.reg));
    memcpy(c.pll, r->pll, sizeof(c.pll));
    c.agp_cmd = r->agp_cmd;
    c.pmcsr = r->pmcsr;
    c.vram_size = r->vram_size;
    memcpy(c.clut, r->clut, sizeof(c.clut));
    c.pal_w = r->pal_w;
    c.pal_r = r->pal_r;
    c.ddc = r->ddc;
    memcpy(c.host, &r->host, sizeof(c.host));
    memcpy(c.fog_table, r->fog_table, sizeof(c.fog_table));
    c.fog_index = r->fog_index;
    c.crtc_offset_live = r->crtc_offset_live;
    c.flip_pending = r->flip_pending;
    system_write_checkpoint_data(cp, &c, sizeof(c));
    // The CCE (microcode, partial packets) is large: written on its own,
    // not through the stack-allocated header.
    system_write_checkpoint_data(cp, &r->cce, sizeof(r->cce));
    system_write_checkpoint_data(cp, r->vram, r->vram_size);
}

static void r128_checkpoint_restore(pci_device_t *dev, checkpoint_t *cp) {
    rage128_t *r = (rage128_t *)dev->priv;
    r128_ckpt_t c;
    system_read_checkpoint_data(cp, &c, sizeof(c));
    memcpy(r->reg, c.reg, sizeof(r->reg));
    memcpy(r->pll, c.pll, sizeof(r->pll));
    r->agp_cmd = c.agp_cmd;
    r->pmcsr = c.pmcsr;
    memcpy(r->clut, c.clut, sizeof(r->clut));
    r->pal_w = c.pal_w;
    r->pal_r = c.pal_r;
    r->ddc = c.ddc;
    memcpy(&r->host, c.host, sizeof(r->host));
    memcpy(r->fog_table, c.fog_table, sizeof(r->fog_table));
    r->fog_index = c.fog_index;
    r->crtc_offset_live = c.crtc_offset_live;
    r->flip_pending = c.flip_pending != 0;
    system_read_checkpoint_data(cp, &r->cce, sizeof(r->cce));
    if (c.vram_size != r->vram_size)
        // The card was built from the staged memory option; a stream from a
        // card of another size fails the size-tagged read below, loudly.
        LOG(0, "Rage 128: restore: checkpoint VRAM is %u bytes, this card has %u", c.vram_size, r->vram_size);
    system_read_checkpoint_data(cp, r->vram, r->vram_size);
    r128_refresh_clut(r);
    r128_update(r);
    r128_irq_sync(r);
}

static const pci_device_ops_t r128_ops = {
    .teardown = r128_teardown,
    .reset = r128_reset,
    .cfg_read = r128_cfg_read,
    .cfg_write = r128_cfg_write,
    .name = r128_name,
    .display = r128_display,
    .on_vbl = r128_on_vbl,
    .checkpoint_save = r128_checkpoint_save,
    .checkpoint_restore = r128_checkpoint_restore,
};

// ============================================================
// Options
// ============================================================

static const char *const r128_memory_values[] = {"16m", "32m", NULL};
static const char *const r128_memory_labels[] = {"16 MB (Rage Orion, Xclaim VR 128)", "32 MB (Nexus 128)", NULL};
static const char *const r128_monitor_values[] = {"vga", "vga_noddc", "13in_rgb", "21in_rgb", NULL};
static const char *const r128_monitor_labels[] = {"VGA monitor (DDC)", "VGA monitor (no DDC)",
                                                  "13\" AppleColor RGB (Apple sense)",
                                                  "21\" Macintosh color display (Apple sense)", NULL};
static const pci_card_option_t r128_options[] = {
    {.key = "memory",
     .label = "Video memory",
     .values = r128_memory_values,
     .labels = r128_memory_labels,
     .default_value = "16m"},
    {.key = "monitor",
     .label = "Monitor",
     .values = r128_monitor_values,
     .labels = r128_monitor_labels,
     .default_value = "vga"},
    {.key = NULL},
};

static const r128_monitor_t *r128_monitor_by_id(const char *value) {
    for (const r128_monitor_t *m = r128_sense; m->id; m++)
        if (strcmp(m->id, value) == 0)
            return m;
    return NULL;
}

static uint32_t r128_memory_by_id(const char *value) {
    if (strcmp(value, "16m") == 0)
        return R128_VRAM_16MB;
    if (strcmp(value, "32m") == 0)
        return R128_VRAM_32MB;
    return 0;
}

static bool r128_accepts_option(const char *key, const char *value) {
    if (strcmp(key, "monitor") == 0)
        return r128_monitor_by_id(value) != NULL;
    if (strcmp(key, "memory") == 0)
        return r128_memory_by_id(value) != 0;
    return false;
}

// ============================================================
// The object model — machine.pci.slot[N].card.*
// ============================================================

static rage128_t *node_card(struct object *self) {
    return (rage128_t *)object_data(self);
}

static DEF_GETTER(mon_attr_id) {
    rage128_t *c = node_card(self);
    return val_str((c && c->mon) ? c->mon->id : "");
}
static DEF_GETTER(mon_attr_ddc) {
    rage128_t *c = node_card(self);
    return val_bool(c && c->mon && c->mon->ddc);
}
static DEF_GETTER(mon_attr_apple_sense) {
    rage128_t *c = node_card(self);
    return val_bool(c && c->mon && c->mon->apple_sense);
}

static const member_t monitor_members[] = {
    {.kind = M_ATTR, .name = "id", .doc = "Attached monitor id",         .attr = {.type = V_STRING, .get = mon_attr_id}},
    {.kind = M_ATTR,
     .name = "ddc",
     .doc = "The monitor answers DDC with an EDID",
     .attr = {.type = V_BOOL, .get = mon_attr_ddc}                                                                     },
    {.kind = M_ATTR,
     .name = "apple_sense",
     .doc = "An Apple-sense cable: VSYNC loopback and tied sense lines",
     .attr = {.type = V_BOOL, .get = mon_attr_apple_sense}                                                             },
};
static const class_desc_t r128_monitor_class = {
    .name = "monitor", .members = monitor_members, .n_members = sizeof(monitor_members) / sizeof(monitor_members[0])};

static DEF_METHOD(regs_method_read) {
    rage128_t *c = node_card(self);
    int64_t off = argv[0].i;
    if (!c || off < 0 || off >= (int64_t)R128_REG_APER_SIZE)
        return val_err("regs.read: offset must be 0..$1FFC");
    return val_uint(4, r128_reg_read(c, (uint32_t)off, true));
}
static DEF_METHOD(regs_method_pll) {
    rage128_t *c = node_card(self);
    int64_t idx = argv[0].i;
    if (!c || idx < 0 || idx > (int64_t)PLL_ADDR_MASK)
        return val_err("regs.pll: index must be 0..$3F");
    return val_uint(4, c->pll[idx]);
}
static DEF_GETTER(regs_attr_vram) {
    rage128_t *c = node_card(self);
    return val_uint(4, c ? c->vram_size : 0);
}
static DEF_GETTER(regs_attr_crtc_gen) {
    rage128_t *c = node_card(self);
    return val_uint(4, c ? c->reg[R_CRTC_GEN_CNTL / 4] : 0);
}
static DEF_GETTER(regs_attr_config_cntl) {
    rage128_t *c = node_card(self);
    return val_uint(4, c ? r128_reg_read(c, R_CONFIG_CNTL, true) : 0);
}

static DEF_GETTER(regs_attr_microcode) {
    rage128_t *c = node_card(self);
    return val_str(c ? r128_cce_microcode_name(c) : "none");
}
static DEF_GETTER(regs_attr_prims3d) {
    rage128_t *c = node_card(self);
    return val_uint(8, c ? c->prims3d : 0);
}
static DEF_GETTER(regs_attr_cce_packets) {
    rage128_t *c = node_card(self);
    return val_uint(8, c ? c->cce.packets : 0);
}

static const arg_decl_t regs_off_arg[] = {
    {.name = "offset", .kind = V_INT, .doc = "Register byte offset in an aperture ($0000-$1FFC)"},
};
static const arg_decl_t regs_pll_arg[] = {
    {.name = "index", .kind = V_INT, .doc = "PLL register index ($00-$3F)"},
};

static const member_t regs_members[] = {
    {.kind = M_ATTR,
     .name = "vram_size",
     .doc = "Bytes of video memory on this card",
     .attr = {.type = V_UINT, .get = regs_attr_vram}                                       },
    {.kind = M_ATTR,
     .name = "crtc_gen_cntl",
     .doc = "CRTC_GEN_CNTL: pixel width, extended display and CRTC enables",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = regs_attr_crtc_gen}    },
    {.kind = M_ATTR,
     .name = "config_cntl",
     .doc = "CONFIG_CNTL: the aperture and register byte-order swappers",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = regs_attr_config_cntl} },
    {.kind = M_ATTR,
     .name = "microcode",
     .doc = "CCE microcode the guest uploaded: known, unknown or none",
     .attr = {.type = V_STRING, .get = regs_attr_microcode}                                },
    {.kind = M_ATTR,
     .name = "cce_packets",
     .doc = "CCE command packets executed since reset",
     .attr = {.type = V_UINT, .get = regs_attr_cce_packets}                                },
    {.kind = M_ATTR,
     .name = "prims3d",
     .doc = "3D primitives the engine has rasterised since reset",
     .attr = {.type = V_UINT, .get = regs_attr_prims3d}                                    },
    {.kind = M_METHOD,
     .name = "read",
     .doc = "Read a register by its byte offset (no side effects)",
     .method = {.args = regs_off_arg, .nargs = 1, .result = V_UINT, .fn = regs_method_read}},
    {.kind = M_METHOD,
     .name = "pll",
     .doc = "Read a PLL register by index",
     .method = {.args = regs_pll_arg, .nargs = 1, .result = V_UINT, .fn = regs_method_pll} },
};
static const class_desc_t r128_regs_class = {
    .name = "regs", .members = regs_members, .n_members = sizeof(regs_members) / sizeof(regs_members[0])};

static display_t *r128_fb_resolve(void *owner) {
    rage128_t *c = (rage128_t *)owner;
    return c ? &c->display : NULL;
}
static uint64_t r128_fb_base(void *owner) {
    rage128_t *c = (rage128_t *)owner;
    return c ? (uint64_t)c->crtc_offset_live : 0;
}

static void r128_attach_objects(pci_device_t *dev, struct object *card_node) {
    rage128_t *r = (rage128_t *)dev->priv;
    if (!r || !card_node)
        return;
    r->fb_node = (display_fb_node_t){.owner = r, .resolve = r128_fb_resolve, .base = r128_fb_base};
    struct object *fb = object_new(&display_fb_class, &r->fb_node, "framebuffer");
    if (fb) {
        object_set_label(fb, "Framebuffer");
        object_set_order(fb, 10);
        object_attach(card_node, fb);
        pci_card_set_framebuffer_object(dev, fb);
    }
    struct object *mon = object_new(&r128_monitor_class, r, "monitor");
    if (mon) {
        object_set_label(mon, "Monitor");
        object_set_order(mon, 20);
        object_attach(card_node, mon);
    }
    struct object *regs = object_new(&r128_regs_class, r, "regs");
    if (regs) {
        object_set_label(regs, "Registers");
        object_set_order(regs, 30);
        object_set_category(regs, M_CAT_ADVANCED);
        object_attach(card_node, regs);
    }
}

// ============================================================
// The factory and the card kind
// ============================================================

static pci_device_t *r128_factory(int slot_index, config_t *cfg, const rom_image_t *rom, const slot_opts_t *opts) {
    pci_device_t *dev = (pci_device_t *)calloc(1, sizeof(*dev));
    rage128_t *r = (rage128_t *)calloc(1, sizeof(*r));
    if (!dev || !r) {
        free(dev);
        free(r);
        return NULL;
    }
    dev->ops = &r128_ops;
    dev->decl = &rage128_decl;
    dev->priv = r;
    pci_cfg_reset(dev);
    r->dev = dev;
    r->cfg = cfg;

    // The card cannot enumerate without its own FCode (see mach64gx.c for
    // why a missing ROM degrades to an empty slot rather than a failed boot).
    uint8_t *prom = NULL;
    size_t prom_size = 0;
    char *prom_path = NULL;
    if (rom && rom->data && rom->size) {
        prom = (uint8_t *)malloc(rom->size);
        if (prom) {
            memcpy(prom, rom->data, rom->size);
            prom_size = rom->size;
        }
    }
    if (!prom && !prom_load_card("rage128", opts->rom[0] ? opts->rom : NULL, &prom, &prom_size, &prom_path)) {
        LOG(0,
            "slot %d: no expansion ROM available for the Rage 128 — the card cannot enumerate without its own "
            "FCode, so the slot is left empty",
            slot_index);
        free(r);
        free(dev);
        return NULL;
    }
    dev->rom = prom;
    dev->rom_size = prom_size;
    free(prom_path);

    const char *memory = slot_opts_option(opts, "memory");
    r->vram_size = memory ? r128_memory_by_id(memory) : R128_VRAM_16MB;
    r->vram = (uint8_t *)calloc(1, r->vram_size);
    r->blank = (uint8_t *)calloc(1, r->vram_size);
    r->compose = (uint8_t *)calloc(1, r->vram_size);
    if (!r->vram || !r->blank || !r->compose) {
        free(r->vram);
        free(r->blank);
        free(r->compose);
        free(dev->rom);
        free(r);
        free(dev);
        return NULL;
    }

    const char *monitor = slot_opts_option(opts, "monitor");
    r->mon = monitor ? r128_monitor_by_id(monitor) : &r128_sense[0];
    r128_build_edid(r);

    r128_reset(dev, cfg);

    r->regs_if = (memory_interface_t){.read_uint8 = regs_read8,
                                      .read_uint16 = regs_read16,
                                      .read_uint32 = regs_read32,
                                      .write_uint8 = regs_write8,
                                      .write_uint16 = regs_write16,
                                      .write_uint32 = regs_write32,
                                      .peek_uint8 = regs_peek8,
                                      .peek_uint16 = regs_peek16,
                                      .peek_uint32 = regs_peek32};
    r->io_if = (memory_interface_t){.read_uint8 = io_read8,
                                    .read_uint16 = io_read16,
                                    .read_uint32 = io_read32,
                                    .write_uint8 = io_write8,
                                    .write_uint16 = io_write16,
                                    .write_uint32 = io_write32,
                                    .peek_uint8 = io_peek8,
                                    .peek_uint16 = io_peek16,
                                    .peek_uint32 = io_peek32};
    r->aper_if = (memory_interface_t){.read_uint8 = aper_read8,
                                      .read_uint16 = aper_read16,
                                      .read_uint32 = aper_read32,
                                      .write_uint8 = aper_write8,
                                      .write_uint16 = aper_write16,
                                      .write_uint32 = aper_write32,
                                      .peek_uint8 = aper_peek8,
                                      .peek_uint16 = aper_peek16,
                                      .peek_uint32 = aper_peek32};
    r->rom_if = (memory_interface_t){.read_uint8 = rom_read8,
                                     .read_uint16 = rom_read16,
                                     .read_uint32 = rom_read32,
                                     .write_uint8 = rom_write8,
                                     .write_uint16 = rom_write16,
                                     .write_uint32 = rom_write32};

    pci_bar_backing_iface(dev, R128_BAR_APER, &r->aper_if, r);
    pci_bar_backing_iface(dev, R128_BAR_IO, &r->io_if, r);
    pci_bar_backing_iface(dev, R128_BAR_REGS, &r->regs_if, r);
    pci_bar_backing_iface(dev, PCI_ROM_BAR_INDEX, &r->rom_if, r);

    LOG(1, "Rage 128: seated in slot %d: %u MB VRAM, monitor '%s', %zu-byte expansion ROM", slot_index,
        r->vram_size >> 20, r->mon->id, prom_size);
    return dev;
}

const pci_card_kind_t rage128_kind = {
    .id = "rage128",
    .display_name = "ATI Rage 128 GL (Rage Orion / Xclaim VR 128 / Nexus 128)",
    .attach = PCI_ATTACH_PCI,
    .requires_prom = true,
    .card_class = "display",
    .monitors = r128_monitors,
    .factory = r128_factory,
    .options = r128_options,
    .accepts_option = r128_accepts_option,
    .attach_objects = r128_attach_objects,
};
