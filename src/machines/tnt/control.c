// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// control.c
// Control (343S1154) — the TNT onboard video controller on the Chaos
// display (VCI) bus, plus the RaDACal RAMDAC whose byte registers live in
// Grand Central at +$1B000.  The 7200 uses Platinum instead; the AV
// subsystem (planb/sixty6) is a gated follow-up.
//
// Software shape (all boot-verified against the shipping ROM's own probe):
//
//   * PCI device 11 on the Chaos bus.  Open Firmware's probe-slots reads
//     the header through the Chaos config ports, then sizes and assigns
//     exactly two BARs — $14 (the 4 KB register block) and $18 (the 64 MB
//     VRAM aperture) — inside the $90000000 VCI memory space it claims.
//     The BAR sizes are the ROM's own literals (OpenFW reg builder:
//     $02000018/$4000000, $02000014/$1000).
//   * 32 little-endian 32-bit registers on $10 centres (the controlfb
//     register map — struct control_regs, Linux fbdev, from Paul
//     Mackerras's 1996 driver for these exact machines).
//   * VRAM as two 2 MB banks behind a mode-dependent aperture view; the
//     usable framebuffer starts at aperture +$800000 and pixel 0 sits 16
//     bytes in (CTRLFB_OFF).  Both banks are populated here: the sizing
//     probe (write/readback at +$600000 and +0 under vram_attr $31/$39)
//     finds 4 MB.
//   * RaDACal: byte-wide index/data cells on $10 centres at GC +$1B000
//     (MkLinux video_control.c hard-codes the address; NetBSD agrees for
//     the 7200's DACula) — addr +$00, cursor +$10, misc data +$20, CLUT
//     +$30.  Misc register $20 carries the depth (bits 3:2: 0/1/2 =
//     8/16/32 bpp), $21 the VRAM bank select.
//   * VBL is Grand Central interrupt 30 (the control node's
//     AAPL,interrupts), IPL 2 through the NanoKernel mapping.
//
// The pixel clock is programmed over Cuda I2C (device $50) and is not
// visible here — geometry derives from the timing registers and pitch
// (width = pitch/bytes-per-pixel, height = (vsblank-veblank)/2), which is
// behaviorally sufficient (control-chaos-video.md §7).
//
// Register truth: linux/drivers/video/fbdev/controlfb.{c,h} [GPL-src],
// mklinux POWERMAC/video_control.c [GPL-src], the shipping ROM's OpenFW
// control node (FCode at image ~$16400) and its mode tables [ROM-RE],
// Apple "Power Macintosh 7500 and 8500 Computers" Developer Note [Apple-doc].

#include "tnt.h"

#include "log.h"
#include "scheduler.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("control");

// Register indices (offset / $10) — controlfb's struct control_regs.
#define CR_VCOUNT     0 // vertical counter (read)
#define CR_VSBLANK    2 // vertical start blank (end of active, half-lines)
#define CR_VEBLANK    3 // vertical end blank (display start, half-lines)
#define CR_VPERIOD    7 // vertical period (half-lines)
#define CR_HSBLANK    10 // horizontal start blank (end of active, 2-px units)
#define CR_HEBLANK    11 // horizontal end blank (display start, 2-px units)
#define CR_CTRL       18 // display control ($400 blanks; $03/$30 gate syncs)
#define CR_START_ADDR 19 // framebuffer start (low 5 bits zero)
#define CR_PITCH      20 // bytes between scan lines
#define CR_MON_SENSE  21 // monitor sense drive/read
#define CR_VRAM_ATTR  22 // VRAM bank enable/geometry ($31/$39/$51)
#define CR_MODE       23 // depth/mode pair (with RaDACal $20)
#define CR_INTR_ENA   25 // interrupt (VBL) enable
#define CR_INTR_STAT  26 // interrupt status

// Interrupt bit assignment within INTR_ENA/INTR_STAT: the VBL is BIT 2.
// The ROM's control ndrv enables $4 (then $C) and spin-polls INTR_STAT
// bit 2 for the vertical retrace during its mode-set — a status in bit 0
// leaves it polling forever.
#define CONTROL_INT_VBL 0x4u

// BAR geometry: the ROM's own reg-builder literals.
#define CONTROL_BAR_REGS_SIZE 0x1000u
#define CONTROL_BAR_VRAM_SIZE 0x4000000u

// Pixel 0 sits 16 bytes into the framebuffer (controlfb's CTRLFB_OFF).
#define CONTROL_FB_OFF 16u

// The monitor on the sense lines: an AppleColor Hi-Res 13"/14" strap —
// line C tied to ground, A/B floating.  Raw sense 6, extended walk $2B,
// which selects the 640x480 timing set in the ROM's own mode table (the
// $2B literal sits at the head of the OpenFW timing-table list).
#define CONTROL_MONITOR_GROUNDED 0x1u // bit mask, lines {A,B,C} = bits {2,1,0}... C = bit 0

static tnt_control_t *ctl(config_t *cfg) {
    return &tnt_st(cfg)->control;
}

// ============================================================
// Presentation — rebuild the display descriptor from the registers
// ============================================================

static uint32_t depth_bpp(const tnt_control_t *c) {
    switch ((c->rad_ctrl >> 2) & 3u) {
    case 1:
        return 16;
    case 2:
        return 32;
    default:
        return 8;
    }
}

static pixel_format_t depth_format(const tnt_control_t *c) {
    switch ((c->rad_ctrl >> 2) & 3u) {
    case 1:
        return PIXEL_16BPP_555;
    case 2:
        return PIXEL_32BPP_XRGB;
    default:
        return PIXEL_8BPP;
    }
}

// Materialize the CLUT for the renderer (8 bpp indexes it directly; the
// direct formats bypass it).
static void control_refresh_clut(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    tnt_control_t *c = &st->control;
    if (depth_bpp(c) > 8)
        return;
    for (uint32_t i = 0; i < 256; i++) {
        st->clut_view[i].r = c->clut[i][0];
        st->clut_view[i].g = c->clut[i][1];
        st->clut_view[i].b = c->clut[i][2];
        st->clut_view[i].a = 255;
    }
    st->display.clut = st->clut_view;
    st->display.clut_len = 256;
    st->display.clut_dirty = true;
}

// Re-derive the whole descriptor from the register file.  Called on init,
// reset, restore and every geometry-relevant register write — all rare.
void tnt_control_update(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    tnt_control_t *c = &st->control;
    if (!st->blank)
        return; // registers poked before init (machine bring-up)

    uint32_t bpp = depth_bpp(c);
    uint32_t pitch = c->reg[CR_PITCH];
    // Active width comes from the horizontal blank pair (2-pixel units;
    // the ROM's 640x480 mode line: hsblank 393, heblank 73 -> 640).  The
    // pitch is the scan-line STRIDE and can carry a pan/scroll margin
    // (the boot's 32 bpp mode programs 2592 = 640*4 + 32), so deriving
    // width from it paints the margin as junk pixels.
    uint32_t hs = c->reg[CR_HSBLANK], he = c->reg[CR_HEBLANK];
    uint32_t width = (hs > he) ? (hs - he) * 2u : ((pitch != 0) ? pitch / (bpp / 8u) : 640u);
    uint32_t vs = c->reg[CR_VSBLANK], ve = c->reg[CR_VEBLANK];
    uint32_t height = (vs > ve) ? (vs - ve) / 2u : 480u;
    if (width == 0 || width > 2048u)
        width = 640u;
    if (height == 0 || height > 1536u)
        height = 480u;

    st->display.width = width;
    st->display.height = height;
    st->display.format = depth_format(c);
    st->display.stride = (pitch != 0) ? pitch : width * (bpp / 8u);
    st->display.par_w = 0;
    st->display.par_h = 0;
    st->display.crt_response = NULL;

    // Scan base inside the 4 MB store: the bank the attribute selects (the
    // 2 MB modes; the $40 bit marks the 4 MB interleaved layout at 0),
    // plus the programmed start address and the 16-byte pixel-0 offset.
    uint32_t attr = c->reg[CR_VRAM_ATTR];
    uint32_t base = (!(attr & 0x40u) && (attr & 0x08u)) ? 0x200000u : 0u;
    base += c->reg[CR_START_ADDR] + CONTROL_FB_OFF;

    // The $400 control bit blanks the raster; an unprogrammed pitch or an
    // out-of-store scan does too (nothing sane is being scanned).
    bool blanked = (c->reg[CR_CTRL] & 0x400u) || pitch == 0;
    uint64_t span = (uint64_t)st->display.stride * height;
    if (base + span > TNT_VRAM_SIZE)
        blanked = true;
    if (blanked) {
        size_t n = (size_t)st->display.stride * height;
        if (n > TNT_VRAM_SIZE)
            n = TNT_VRAM_SIZE;
        memset(st->blank, display_black_fill(st->display.format), n);
        st->display.bits = st->blank;
    } else {
        st->display.bits = st->vram + base;
    }

    if (bpp > 8) {
        st->display.clut = NULL;
        st->display.clut_len = 0;
    } else {
        control_refresh_clut(cfg);
    }
    st->display.shape_dirty = true;
    st->display.fb_dirty = true;
    st->display.clut_dirty = true;
}

struct display *tnt_control_display(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    if (!st || !st->blank)
        return NULL;
    return &st->display;
}

void tnt_control_host_vbl(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    if (st && st->blank)
        st->display.fb_dirty = true; // guest drawing bypasses the renderer
}

// ============================================================
// VBL — Grand Central interrupt 30, one edge per frame while enabled
// ============================================================

static void control_vbl_event(void *source, uint64_t data) {
    (void)data;
    config_t *cfg = (config_t *)source;
    tnt_control_t *c = ctl(cfg);
    c->vbl_armed = 0;
    if (!(c->reg[CR_INTR_ENA] & CONTROL_INT_VBL))
        return; // disabled while the event was in flight: go quiet
    c->vbl_pending = 1;
    tnt_gc_pulse_event(cfg, TNT_INT_VBL);
    c->vbl_armed = 1;
    scheduler_new_cpu_event(cfg->scheduler, control_vbl_event, cfg, 0, 0, 1000000000ull / 60u);
}

static void control_vbl_arm(config_t *cfg) {
    tnt_control_t *c = ctl(cfg);
    if (c->vbl_armed || !(c->reg[CR_INTR_ENA] & CONTROL_INT_VBL))
        return;
    c->vbl_armed = 1;
    scheduler_new_cpu_event(cfg->scheduler, control_vbl_event, cfg, 0, 0, 1000000000ull / 60u);
}

// ============================================================
// The register block (BAR $14): 32 x 32-bit LE on $10 centres
// ============================================================

// Live vertical counter: the frame phase off the CPU clock against a
// nominal 60 Hz frame, scaled to the programmed vertical period.  Guests
// poll it for vertical-blank waits; only monotonic-within-frame matters.
static uint32_t control_vcount(config_t *cfg) {
    tnt_control_t *c = ctl(cfg);
    uint32_t vtotal = c->reg[CR_VPERIOD] / 2u;
    if (vtotal == 0 || vtotal > 4096u)
        vtotal = 525u;
    uint64_t frame = cfg->machine->freq / 60u;
    uint64_t pos = scheduler_cpu_cycles(cfg->scheduler) % frame;
    return (uint32_t)(pos * vtotal / frame);
}

// Monitor sense readback: bits 5:3 tristate line A/B/C (1 = driver off),
// bits 2:0 the driven levels; lines read back at bits 8:6 (A/B/C), each
// low when driven low or strapped to ground (wired-AND).
static uint32_t control_sense_read(config_t *cfg) {
    tnt_control_t *c = ctl(cfg);
    uint32_t v = c->reg[CR_MON_SENSE] & 0x3Fu;
    uint32_t lines = 0;
    for (int i = 0; i < 3; i++) { // i: 0=A, 1=B, 2=C
        uint32_t drive_off = (v >> (5 - i)) & 1u;
        uint32_t level = (v >> (2 - i)) & 1u;
        uint32_t line = drive_off ? 1u : level;
        if ((CONTROL_MONITOR_GROUNDED >> (2 - i)) & 1u)
            line = 0; // the monitor straps this line to ground
        lines |= line << (8 - i);
    }
    return v | lines;
}

static uint32_t control_reg_read(config_t *cfg, uint32_t offset) {
    tnt_control_t *c = ctl(cfg);
    uint32_t idx = offset >> 4;
    if ((offset & 0xFu) != 0 || idx >= TNT_CONTROL_REGS) {
        LOG(1, "register read off-centre +$%03X", offset);
        return 0;
    }
    switch (idx) {
    case CR_VCOUNT:
        return control_vcount(cfg);
    case CR_MON_SENSE:
        return control_sense_read(cfg);
    case CR_INTR_STAT:
        return c->vbl_pending ? CONTROL_INT_VBL : 0;
    default:
        return c->reg[idx];
    }
}

static void control_reg_write(config_t *cfg, uint32_t offset, uint32_t value) {
    tnt_control_t *c = ctl(cfg);
    uint32_t idx = offset >> 4;
    if ((offset & 0xFu) != 0 || idx >= TNT_CONTROL_REGS) {
        LOG(1, "register write off-centre +$%03X = $%08X", offset, value);
        return;
    }
    LOG(2, "reg[%u] = $%08X", idx, value);
    c->reg[idx] = value;
    switch (idx) {
    case CR_CTRL:
    case CR_START_ADDR:
    case CR_PITCH:
    case CR_VRAM_ATTR:
    case CR_MODE:
    case CR_VSBLANK:
    case CR_VEBLANK:
    case CR_HSBLANK:
    case CR_HEBLANK:
        tnt_control_update(cfg);
        break;
    case CR_INTR_ENA:
        if (!(value & CONTROL_INT_VBL))
            c->vbl_pending = 0;
        else
            control_vbl_arm(cfg);
        break;
    case CR_INTR_STAT:
        c->vbl_pending = 0; // any status write acknowledges
        break;
    default:
        break;
    }
}

// ============================================================
// VRAM aperture (BAR $18) — the mode-dependent bank view
// ============================================================
// The 64 MB aperture repeats the 8 MB view; the usable framebuffer is the
// +$800000 half ("the low half is a different view of the same memory").
// In the 2 MB modes bank 1 answers at +0 and bank 2 shows through at
// +$600000 — the sizing probe's contract; absent space between accepts
// writes and does not read back.  The $40 attribute bit selects the 4 MB
// linear layout.

// Map an aperture offset to a store offset; -1 = unbacked (probe hole).
static int64_t vram_map(tnt_control_t *c, uint32_t offset) {
    uint32_t off = offset & 0x7FFFFFu; // 8 MB view, repeated
    uint32_t attr = c->reg[CR_VRAM_ATTR];
    if (attr & 0x40u)
        return off & (TNT_VRAM_SIZE - 1u); // 4 MB linear (attr $51)
    if (off < 0x200000u)
        return off; // bank 1
    if (off >= 0x600000u)
        return 0x200000u + (off & 0x1FFFFFu); // bank 2 shows through
    return -1; // the hole between the banks
}

static uint8_t vram_read8(void *ctx, uint32_t offset) {
    config_t *cfg = (config_t *)ctx;
    int64_t m = vram_map(ctl(cfg), offset);
    return (m >= 0) ? tnt_st(cfg)->vram[m] : 0u;
}

static void vram_write8(void *ctx, uint32_t offset, uint8_t value) {
    config_t *cfg = (config_t *)ctx;
    int64_t m = vram_map(ctl(cfg), offset);
    if (m >= 0)
        tnt_st(cfg)->vram[m] = value;
}

// ============================================================
// PCI config header (Chaos device 11) — bandit.c delegates here
// ============================================================

uint32_t tnt_control_cfg_read(config_t *cfg, uint32_t reg) {
    tnt_control_t *c = ctl(cfg);
    switch (reg) {
    case 0x14:
        return c->bar_regs & ~(CONTROL_BAR_REGS_SIZE - 1u); // 32-bit memory BAR
    case 0x18:
        return c->bar_vram & ~(CONTROL_BAR_VRAM_SIZE - 1u);
    default:
        return 0xFFFFFFFFu; // the restricted offsets read all-ones
    }
}

void tnt_control_cfg_write(config_t *cfg, uint32_t reg, uint32_t byte, uint8_t value) {
    tnt_control_t *c = ctl(cfg);
    uint32_t shift = 8u * (byte & 3u);
    uint32_t mask = 0xFFu << shift;
    switch (reg) {
    case 0x14:
        c->bar_regs = (c->bar_regs & ~mask) | ((uint32_t)value << shift);
        LOG(2, "BAR $14 (registers) = $%08X", c->bar_regs);
        break;
    case 0x18:
        c->bar_vram = (c->bar_vram & ~mask) | ((uint32_t)value << shift);
        LOG(2, "BAR $18 (VRAM) = $%08X", c->bar_vram);
        break;
    default:
        LOG(2, "config write $%02X byte %u = $%02X ignored", reg, byte, value);
        break;
    }
}

// ============================================================
// RaDACal (Grand Central +$1B000) — byte cells on $10 centres
// ============================================================

uint8_t tnt_control_rad_read(config_t *cfg, uint32_t offset) {
    tnt_control_t *c = ctl(cfg);
    switch (offset & 0x30u) {
    case 0x00:
        return c->rad_addr;
    case 0x10:
        return c->crsr[c->rad_addr & 7u][c->crsr_phase];
    case 0x20:
        switch (c->rad_addr) {
        case 0x20:
            return c->rad_ctrl;
        case 0x21:
            return c->rad_bank;
        case 0x10:
        case 0x11:
            return c->rad_misc[c->rad_addr & 1u];
        default:
            return 0;
        }
    default: { // +$30: CLUT data, RGB phase auto-advances
        uint8_t v = c->clut[c->rad_addr][c->rad_phase];
        if (++c->rad_phase == 3) {
            c->rad_phase = 0;
            c->rad_addr++;
        }
        return v;
    }
    }
}

void tnt_control_rad_write(config_t *cfg, uint32_t offset, uint8_t value) {
    tnt_control_t *c = ctl(cfg);
    switch (offset & 0x30u) {
    case 0x00:
        c->rad_addr = value;
        c->rad_phase = 0;
        c->crsr_phase = 0;
        break;
    case 0x10:
        c->crsr[c->rad_addr & 7u][c->crsr_phase] = value;
        if (++c->crsr_phase == 3)
            c->crsr_phase = 0;
        break;
    case 0x20:
        LOG(2, "RaDACal misc[$%02X] = $%02X", c->rad_addr, value);
        switch (c->rad_addr) {
        case 0x20:
            c->rad_ctrl = value; // depth control — geometry follows
            tnt_control_update(cfg);
            break;
        case 0x21:
            c->rad_bank = value;
            break;
        case 0x10:
        case 0x11:
            c->rad_misc[c->rad_addr & 1u] = value;
            break;
        default:
            break;
        }
        break;
    default: // +$30: CLUT data
        c->clut[c->rad_addr][c->rad_phase] = value;
        if (++c->rad_phase == 3) {
            c->rad_phase = 0;
            c->rad_addr++;
            control_refresh_clut(cfg);
        }
        break;
    }
}

// ============================================================
// VCI memory space ($90000000, 256 MB) — dispatch by live BARs
// ============================================================
// Everything the BARs do not claim faults recoverably, exactly like the
// empty Bandit memory space (the probe idioms expect a catchable error).

// Locate an access: 0 = registers, 1 = VRAM, -1 = unclaimed.
static int vci_locate(config_t *cfg, uint32_t offset, uint32_t *sub) {
    tnt_control_t *c = ctl(cfg);
    uint32_t addr = TNT_PCI_MEM_VCI + offset;
    uint32_t regs_base = c->bar_regs & ~(CONTROL_BAR_REGS_SIZE - 1u);
    if (regs_base != 0 && addr - regs_base < CONTROL_BAR_REGS_SIZE) {
        *sub = addr - regs_base;
        return 0;
    }
    uint32_t vram_base = c->bar_vram & ~(CONTROL_BAR_VRAM_SIZE - 1u);
    if (vram_base != 0 && addr - vram_base < CONTROL_BAR_VRAM_SIZE) {
        *sub = addr - vram_base;
        return 1;
    }
    return -1;
}

static void vci_fault(config_t *cfg, uint32_t offset, bool write) {
    LOG(4, "VCI: unclaimed %s $%08X", write ? "write" : "read", TNT_PCI_MEM_VCI + offset);
    memory_signal_bus_error(TNT_PCI_MEM_VCI + offset, write);
}

static uint8_t vci_read8(void *ctx, uint32_t offset) {
    config_t *cfg = (config_t *)ctx;
    uint32_t sub;
    switch (vci_locate(cfg, offset, &sub)) {
    case 0:
        // Byte lane of the LE register (lane j = value bits 8j+7:8j).
        return (uint8_t)(control_reg_read(cfg, sub & ~3u) >> (8u * (sub & 3u)));
    case 1:
        return vram_read8(ctx, sub);
    default:
        vci_fault(cfg, offset, false);
        return 0xFF;
    }
}

static uint16_t vci_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((vci_read8(ctx, offset) << 8) | vci_read8(ctx, offset + 1));
}

static uint32_t vci_read32(void *ctx, uint32_t offset) {
    config_t *cfg = (config_t *)ctx;
    uint32_t sub;
    switch (vci_locate(cfg, offset, &sub)) {
    case 0:
        return TNT_LE32(control_reg_read(cfg, sub));
    case 1:
        return ((uint32_t)vram_read8(ctx, sub) << 24) | ((uint32_t)vram_read8(ctx, sub + 1) << 16) |
               ((uint32_t)vram_read8(ctx, sub + 2) << 8) | vram_read8(ctx, sub + 3);
    default:
        vci_fault(cfg, offset, false);
        return 0xFFFFFFFFu;
    }
}

static void vci_write8(void *ctx, uint32_t offset, uint8_t value) {
    config_t *cfg = (config_t *)ctx;
    uint32_t sub;
    switch (vci_locate(cfg, offset, &sub)) {
    case 0: {
        // Read-modify-write the byte lane so byte pokes of a register work.
        uint32_t reg = sub & ~3u;
        uint32_t shift = 8u * (sub & 3u);
        uint32_t v = (control_reg_read(cfg, reg) & ~(0xFFu << shift)) | ((uint32_t)value << shift);
        control_reg_write(cfg, reg, v);
        break;
    }
    case 1:
        vram_write8(ctx, sub, value);
        break;
    default:
        vci_fault(cfg, offset, true);
        break;
    }
}

static void vci_write16(void *ctx, uint32_t offset, uint16_t value) {
    vci_write8(ctx, offset, (uint8_t)(value >> 8));
    vci_write8(ctx, offset + 1, (uint8_t)value);
}

static void vci_write32(void *ctx, uint32_t offset, uint32_t value) {
    config_t *cfg = (config_t *)ctx;
    uint32_t sub;
    switch (vci_locate(cfg, offset, &sub)) {
    case 0:
        control_reg_write(cfg, sub, TNT_LE32(value));
        break;
    case 1:
        vram_write8(ctx, sub, (uint8_t)(value >> 24));
        vram_write8(ctx, sub + 1, (uint8_t)(value >> 16));
        vram_write8(ctx, sub + 2, (uint8_t)(value >> 8));
        vram_write8(ctx, sub + 3, (uint8_t)value);
        break;
    default:
        vci_fault(cfg, offset, true);
        break;
    }
}

// ============================================================
// Lifecycle
// ============================================================

void tnt_control_register_events(config_t *cfg) {
    scheduler_new_event_type(cfg->scheduler, "control", cfg, "vbl", control_vbl_event);
}

void tnt_control_reset(config_t *cfg) {
    tnt_control_t *c = ctl(cfg);
    // Power-on registers.  VRAM contents survive reset (real DRAM decays
    // over seconds; a warm restart sees the old frame).  The armed flag
    // mirrors the scheduler's pending event, which reset does not cancel —
    // a stale VBL fires once into a disabled controller and goes quiet.
    memset(c->reg, 0, sizeof(c->reg));
    c->bar_regs = 0;
    c->bar_vram = 0;
    c->vbl_pending = 0;
    c->rad_addr = 0;
    c->rad_phase = 0;
    c->crsr_phase = 0;
    c->rad_ctrl = 0;
    c->rad_bank = 0;
    memset(c->rad_misc, 0, sizeof(c->rad_misc));
    memset(c->clut, 0, sizeof(c->clut));
    memset(c->crsr, 0, sizeof(c->crsr));
    tnt_control_update(cfg);
}

void tnt_control_init(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    st->vram = calloc(1, TNT_VRAM_SIZE);
    st->blank = calloc(1, TNT_VRAM_SIZE);
    assert(st->vram != NULL && st->blank != NULL);

    // The VCI memory space: control's BARs answer inside it, everything
    // else takes the recoverable transfer error the probe idioms expect.
    st->vci_interface.read_uint8 = vci_read8;
    st->vci_interface.read_uint16 = vci_read16;
    st->vci_interface.read_uint32 = vci_read32;
    st->vci_interface.write_uint8 = vci_write8;
    st->vci_interface.write_uint16 = vci_write16;
    st->vci_interface.write_uint32 = vci_write32;
    memory_map_add(cfg->mem_map, TNT_PCI_MEM_VCI, 0x10000000u, "VCI memory (Control)", &st->vci_interface, cfg);

    tnt_control_update(cfg);
    st->display.response_dirty = true;
}

void tnt_control_teardown(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    free(st->vram);
    st->vram = NULL;
    free(st->blank);
    st->blank = NULL;
}
