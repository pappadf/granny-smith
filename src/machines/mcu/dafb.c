// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// dafb.c
// DAFB built-in video — see dafb.h.  Register semantics follow the
// reference's §11 [R] tables, cross-checked against the boot ROM's captured
// access sequence (local/gs-docs/DAFB/re/q700-rom-dafb-access.log):
//   * frame-buffer base $000/$004 (the ROM's 640×480 set programs $1000),
//     stride $008 (words; ROM uses $100 → 1024-byte rows for the gray screen)
//   * Swatch timing at +$100: HAL/HFP give 640 visible ($88/$308), VAL/VFP
//     halves give 480, VFPEQ/2 the vertical total — the derivations below
//     reproduce the ROM's programmed mode exactly
//   * AC842 CLUT at $200/$210 (RGB component phase, auto-increment; the
//     ROM's white/black gray-screen entries confirm the protocol) and
//     PCBR0 at $220 (depth field & $1C, pixel divider & $60)
//   * DP8531 at +$300: sixteen 4-bit registers, commit on register 15; the
//     reference's frequency formula yields 25.18 MHz for the ROM's 640×480
//     values — validated against the captured nibbles
//   * monitor sense at $01C: per-line drive/release protocol (active-low
//     drive bits); the attached monitor's passive code answers on released
//     lines.  A wrong echo here made the ROM pick a PAL convolution mode
//     during bring-up, so this register must never read back its own write.
// Unknown registers stay accept-and-log with readback (Trap 24).

#include "dafb.h"

#include "log.h"
#include "scheduler.h"
#include "system.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("dafb");

// === Register offsets ===
// DAFB core
#define DAFB_FB_BASE_HI   0x000u // base bits 20:9
#define DAFB_FB_BASE_LO   0x004u // base bits 8:5
#define DAFB_STRIDE       0x008u // row stride in 32-bit words
#define DAFB_TIMING_CTL   0x00Cu // stored [U]
#define DAFB_CONFIG       0x010u // bit 2 interlace, bit 3 convolution [R]
#define DAFB_SENSE        0x01Cu // monitor sense drive/read
#define DAFB_TEST_VERSION 0x02Cu // version bits from 9 up [R]
// Swatch (+$100)
#define SWATCH_MODE         0x100u
#define SWATCH_INTR_ENABLE  0x104u // bit0 VBL, bit1 aux, bit2 cursor
#define SWATCH_INTR_STATUS  0x108u // bit0 VBL pending, bit2 cursor pending
#define SWATCH_CLEAR_CURSOR 0x10Cu
#define SWATCH_CLEAR_VBL    0x114u
#define SWATCH_CURSOR_LINE  0x118u
#define SWATCH_HAL          0x140u // horizontal active start
#define SWATCH_HFP          0x144u // horizontal active end
#define SWATCH_HPIX         0x148u // horizontal total - 2
#define SWATCH_VAL          0x15Cu // vertical active start (half-lines)
#define SWATCH_VFP          0x160u // vertical active end (half-lines)
#define SWATCH_VFPEQ        0x164u // vertical total (half-lines)
// AC842 RAMDAC (+$200)
#define AC842_ADDR  0x200u
#define AC842_DATA  0x210u
#define AC842_PCBR0 0x220u
// DP8531 (+$300): register = (offset >> 4) & 0xF; commit on reg 15

// Phase C fallback frame period until the ROM programs real timing.
#define DAFB_FALLBACK_FRAME_NS 16625000ull

struct dafb {
    uint32_t regs[DAFB_REG_COUNT]; // raw register file ($000-$3FF)
    uint8_t touched[DAFB_REG_COUNT]; // log-once bitmap
    uint8_t *vram; // dedicated VRAM buffer
    uint32_t vram_size; // installed capacity (512K/1M/2M)
    struct scheduler *sched;

    // Scanout state
    display_t display;
    rgba8_t clut[256];

    // AC842 write machine: index + RGB component phase (Trap 11: the
    // partial triplet is real state and checkpoints with the device).
    uint8_t dac_idx;
    uint8_t dac_phase;
    uint8_t dac_rgb[3];
    uint8_t pcbr0;

    // AC842a (Q950): PCBR1 lives behind AddrReg==1 at the config register
    // (DAFBDriver.a/PrimaryInit.a [A]).  Bits 7:4 latch; the low nibble is
    // the read-only mfg/rev field (0 = non-Antelope → the driver uses the
    // sparse Trans5to8 CLUT load).  x555 16-bit direct mode is active when
    // the $C0 bits are set and PCBR0 selects direct color.
    bool ac842a; // true = AC842a model (PCBR1 exists)
    uint8_t pcbr1;
    uint8_t version; // DAFB_Test bits 11:9 (0 Q700/Q900, 3 Q950; ref §11.8)

    // DP8531 nibble registers + committed output
    uint8_t clk_reg[16];
    double clock_hz; // committed synthesizer output (0 = never committed)

    // Monitor sense
    uint8_t sense_code; // attached monitor's passive 3-bit code

    // TurboSCSI DRQ observation (per channel; ref §12.4 bit 9)
    dafb_drq_query_fn drq_fn[2];
    void *drq_ctx[2];

    // Derived timing
    uint64_t frame_ns; // full frame period (fallback until valid)
    bool timing_valid;

    // Video IRQ output
    dafb_irq_cb irq_cb;
    void *irq_ctx;
    bool irq_line;
};

// ============================================================
// Video IRQ (level: status & enable; ref §11.18)
// ============================================================

static void update_irq(dafb_t *dafb) {
    uint32_t enable = dafb->regs[SWATCH_INTR_ENABLE >> 2];
    uint32_t status = dafb->regs[SWATCH_INTR_STATUS >> 2];
    bool active = (enable & status & 0x7u) != 0;
    if (active != dafb->irq_line) {
        dafb->irq_line = active;
        if (dafb->irq_cb)
            dafb->irq_cb(dafb->irq_ctx, active);
    }
}

// ============================================================
// Swatch timing derivation (ref §11.9/§11.14)
// ============================================================

static uint32_t pixel_divide(dafb_t *dafb) {
    return 1u << ((dafb->pcbr0 & 0x60u) >> 5);
}

// Map the PCBR0 depth field to a display format (ref §11.11).  Direct color
// is 24-in-32 XRGB, except on an AC842a with PCBR1's x555 bits set —
// then pixels are big-endian x555 16-bit words (DAFBDriver.a writes
// PCBR1 = $C0 when entering the "Thousands" mode).
static bool depth_format(const dafb_t *dafb, pixel_format_t *fmt, uint32_t *bpp) {
    uint8_t pcbr0 = dafb->pcbr0;
    switch (pcbr0 & 0x1Cu) {
    case 0x00:
        *fmt = PIXEL_1BPP_MSB;
        *bpp = 1;
        return true;
    case 0x08:
        *fmt = PIXEL_2BPP_MSB;
        *bpp = 2;
        return true;
    case 0x10:
        *fmt = PIXEL_4BPP_MSB;
        *bpp = 4;
        return true;
    case 0x18:
        *fmt = PIXEL_8BPP;
        *bpp = 8;
        return true;
    case 0x1C:
        if (dafb->ac842a && (dafb->pcbr1 & 0xC0u) == 0xC0u) {
            *fmt = PIXEL_16BPP_555;
            *bpp = 16;
        } else {
            *fmt = PIXEL_32BPP_XRGB;
            *bpp = 32;
        }
        return true;
    default:
        return false; // undefined pattern: log, keep previous mode
    }
}

// Recompute the scanout shape + frame period from the programmed state.
// Runs on Swatch mode/PCBR0/DP8531-commit/base/stride writes; incomplete
// programming (zeros mid-mode-set) leaves the previous shape (ref §11.14).
static void reconfigure(dafb_t *dafb) {
    uint32_t hal = dafb->regs[SWATCH_HAL >> 2] & 0xFFFu;
    uint32_t hfp = dafb->regs[SWATCH_HFP >> 2] & 0xFFFu;
    uint32_t hpix = dafb->regs[SWATCH_HPIX >> 2] & 0xFFFu;
    uint32_t val = dafb->regs[SWATCH_VAL >> 2] & 0xFFFu;
    uint32_t vfp = dafb->regs[SWATCH_VFP >> 2] & 0xFFFu;
    uint32_t vfpeq = dafb->regs[SWATCH_VFPEQ >> 2] & 0xFFFu;

    if (hfp <= hal || vfp <= val || vfpeq == 0)
        return; // mid-mode-set

    pixel_format_t fmt;
    uint32_t bpp;
    if (!depth_format(dafb, &fmt, &bpp)) {
        LOG(1, "DAFB PCBR0 undefined depth pattern $%02X — keeping previous mode", dafb->pcbr0);
        return;
    }

    uint32_t divide = pixel_divide(dafb);
    uint32_t width = (hfp - hal) / divide;
    uint32_t height = (vfp - val) / 2u;
    // Convolution halves the effective horizontal fetch on the composite
    // modes; v1 renders the literal pixels (documented divergence [R][U]).

    uint32_t fb_base =
        ((dafb->regs[DAFB_FB_BASE_HI >> 2] & 0xFFFu) << 9) | ((dafb->regs[DAFB_FB_BASE_LO >> 2] & 0xFu) << 5);
    fb_base &= 0x001FFFE0u;
    uint32_t stride = (dafb->regs[DAFB_STRIDE >> 2] & 0xFFFu) << 2;

    // Host safety: cap to the aperture and installed VRAM.
    if (width == 0 || width > 2048 || height == 0 || height > 2048 || stride == 0)
        return;
    if (fb_base + (uint64_t)stride * height > dafb->vram_size) {
        LOG(1, "DAFB scanout exceeds installed VRAM (base $%X stride %u height %u) — clamped", fb_base, stride, height);
        if (fb_base >= dafb->vram_size)
            return;
        height = (dafb->vram_size - fb_base) / (stride ? stride : 1);
        if (height == 0)
            return;
    }

    dafb->display.width = width;
    dafb->display.height = height;
    dafb->display.stride = stride;
    dafb->display.format = fmt;
    dafb->display.bits = dafb->vram + fb_base;
    dafb->display.clut = (fmt == PIXEL_32BPP_XRGB || fmt == PIXEL_16BPP_555) ? NULL : dafb->clut;
    dafb->display.clut_len = (fmt == PIXEL_32BPP_XRGB || fmt == PIXEL_16BPP_555) ? 0 : 256;
    dafb->display.shape_dirty = true;
    dafb->display.fb_dirty = true;

    // Frame period: line time = h_total scanout pixels at (clock / divide);
    // v_total lines from the half-line total.
    double dot = dafb->clock_hz > 0 ? dafb->clock_hz / divide : 0;
    uint32_t h_total = hpix + 2u;
    uint32_t v_total = vfpeq / 2u;
    if (dot > 1e5 && h_total > width && v_total >= height) {
        dafb->frame_ns = (uint64_t)(1e9 * (double)h_total * (double)v_total / dot);
        // Host safety: clamp to a sane refresh range (20-200 Hz).
        if (dafb->frame_ns < 5000000ull || dafb->frame_ns > 50000000ull)
            dafb->frame_ns = DAFB_FALLBACK_FRAME_NS;
        dafb->timing_valid = true;
    }

    LOG(2, "DAFB mode: %ux%u %ubpp stride %u base $%X frame %.2f Hz", width, height, bpp, stride, fb_base,
        dafb->frame_ns ? 1e9 / (double)dafb->frame_ns : 0.0);
}

// ============================================================
// Swatch frame event: VBL (+ cursor) pending bits per frame
// ============================================================
// One event per frame raises both the VBL and cursor pending bits (the
// cursor line is inside the frame; a per-scanline model can split these
// later).  Status sets regardless of enables; the IRQ line is
// status & enable (ref §11.10, §22.9).

static void dafb_frame_event(void *source, uint64_t data) {
    (void)data;
    dafb_t *dafb = (dafb_t *)source;
    dafb->regs[SWATCH_INTR_STATUS >> 2] |= 0x5u; // VBL (bit 0) + cursor (bit 2)
    dafb->display.fb_dirty = true; // guest drew during the frame
    update_irq(dafb);
    scheduler_new_cpu_event(dafb->sched, dafb_frame_event, dafb, 0, 0,
                            dafb->timing_valid ? dafb->frame_ns : DAFB_FALLBACK_FRAME_NS);
}

void dafb_attach_scheduler(dafb_t *dafb, struct scheduler *sched) {
    if (!dafb || !sched)
        return;
    dafb->sched = sched;
    scheduler_new_event_type(sched, "dafb", dafb, "swatch_frame", dafb_frame_event);
    if (!has_event(sched, dafb_frame_event))
        scheduler_new_cpu_event(sched, dafb_frame_event, dafb, 0, 0, DAFB_FALLBACK_FRAME_NS);
}

// ============================================================
// Monitor sense (ref §11.7)
// ============================================================
// The register's low 3 bits drive the sense lines active-low (0 = drive
// low, 1 = release).  A read returns each line's level: low when the host
// drives it low or the passive monitor ties it low; high otherwise.  The
// standard codes have the monitor grounding the zero bits of its code.

static uint8_t sense_read(dafb_t *dafb) {
    uint8_t drive = (uint8_t)(dafb->regs[DAFB_SENSE >> 2] & 0x7u);
    uint8_t lines = 0;
    for (int i = 0; i < 3; i++) {
        bool host_low = !(drive & (1u << i));
        bool monitor_low = !(dafb->sense_code & (1u << i));
        lines |= (uint8_t)((host_low || monitor_low ? 0u : 1u) << i);
    }
    // The register interface presents the line states INVERTED — the
    // DAFBReadSenseLines macro (DepVideoEqu.a) NOTs the byte after reading.
    // Non-inverted readback made the ROM decode the 13" display's cross-
    // drive tuple as an NTSC television (observed in bring-up).
    uint8_t v = (uint8_t)(~lines & 0x7u);
    LOG(3, "sense read: drive=$%X lines=$%X -> $%X", drive, lines, v);
    return v;
}

// ============================================================
// DP8531 clock synthesizer (ref §11.13)
// ============================================================

static void dp8531_commit(dafb_t *dafb) {
    uint32_t R = ((uint32_t)dafb->clk_reg[6] << 8) | ((uint32_t)dafb->clk_reg[5] << 4) | dafb->clk_reg[4];
    uint32_t P = 1u << dafb->clk_reg[9];
    uint32_t modulus = ((uint32_t)dafb->clk_reg[3] << 12) | ((uint32_t)dafb->clk_reg[2] << 8) |
                       ((uint32_t)dafb->clk_reg[1] << 4) | dafb->clk_reg[0];
    uint32_t A = (~modulus) & 0x1Fu;
    uint32_t B = (modulus >> 5) & 0x7FFu;
    if (B < 2)
        B = 2;
    if (A > B)
        A = B;
    uint32_t N = 32u * (B - A) + 31u * (1u + A);
    if (R == 0 || P == 0) {
        LOG(1, "DP8531 commit with R=%u P=%u — ignored", R, P);
        return;
    }
    dafb->clock_hz = (20000000.0 / (double)R) * (double)N / (double)P;
    LOG(2, "DP8531 output %.3f MHz", dafb->clock_hz / 1e6);
    reconfigure(dafb);
}

// ============================================================
// AC842 RAMDAC (ref §11.11)
// ============================================================

static void ac842_write(dafb_t *dafb, uint32_t reg, uint8_t value) {
    switch (reg) {
    case AC842_ADDR:
        dafb->dac_idx = value;
        dafb->dac_phase = 0;
        break;
    case AC842_DATA:
        if (dafb->dac_phase < 3)
            dafb->dac_rgb[dafb->dac_phase] = value;
        dafb->dac_phase++;
        if (dafb->dac_phase >= 3) {
            dafb->clut[dafb->dac_idx].r = dafb->dac_rgb[0];
            dafb->clut[dafb->dac_idx].g = dafb->dac_rgb[1];
            dafb->clut[dafb->dac_idx].b = dafb->dac_rgb[2];
            dafb->clut[dafb->dac_idx].a = 255;
            dafb->dac_idx++;
            dafb->dac_phase = 0;
            dafb->display.clut_dirty = true;
        }
        break;
    case AC842_PCBR0:
        // On an AC842a, AddrReg == 1 routes the config register to PCBR1
        // (Apple's own driver: "Move.l #1,ACDC_AddrReg … Tell ACDC to use
        // PCBR1"); anything else reaches PCBR0.  On the plain AC842 there
        // is no PCBR1 — the write always lands in PCBR0, which is exactly
        // what PrimaryInit's presence probe exploits ($06 into PCBR0,
        // 0 "into PCBR1", read PCBR0 back: $06 ⇒ AC842a, 0 ⇒ AC842).
        if (dafb->ac842a && dafb->dac_idx == 1) {
            dafb->pcbr1 = (uint8_t)((value & 0xF0u) | (dafb->pcbr1 & 0x0Fu)); // low nibble = RO mfg/rev
            LOG(2, "AC842a PCBR1 = $%02X%s", dafb->pcbr1, (dafb->pcbr1 & 0xC0u) == 0xC0u ? " (x555 16bpp)" : "");
        } else {
            dafb->pcbr0 = value;
        }
        reconfigure(dafb);
        break;
    default:
        // AC842a hidden-control accesses and unmodeled slots: latched by the
        // raw register file; logged by the caller.
        break;
    }
}

static uint8_t ac842_read(dafb_t *dafb, uint32_t reg) {
    switch (reg) {
    case AC842_ADDR:
        return dafb->dac_idx;
    case AC842_DATA: {
        const uint8_t *entry = (const uint8_t *)&dafb->clut[dafb->dac_idx];
        uint8_t v = (dafb->dac_phase < 3) ? entry[dafb->dac_phase] : 0;
        dafb->dac_phase = (uint8_t)((dafb->dac_phase + 1) % 3);
        return v;
    }
    case AC842_PCBR0:
        // AC842a: AddrReg == 1 reads PCBR1 (low nibble = mfg/rev, 0 here —
        // the non-Antelope answer, so the driver loads the sparse
        // Trans5to8 CLUT for x555).
        if (dafb->ac842a && dafb->dac_idx == 1)
            return dafb->pcbr1;
        return dafb->pcbr0;
    default:
        return (uint8_t)dafb->regs[reg >> 2];
    }
}

// ============================================================
// Register aperture dispatch
// ============================================================

static inline uint32_t reg_off(uint32_t offset) {
    return offset & 0x3FCu; // longword slot within the mirrored $400
}

static void log_touch(dafb_t *dafb, uint32_t offset, bool write, uint32_t value) {
    uint32_t idx = reg_off(offset) >> 2;
    if (dafb->touched[idx] && !write) {
        LOG(3, "DAFB read  $%03X -> $%08X", reg_off(offset), value); // full trace at level 3
        return;
    }
    dafb->touched[idx] = 1;
    LOG(2, "DAFB %s $%03X %s $%08X", write ? "write" : "read", reg_off(offset), write ? "=" : "->", value);
}

// 32-bit-value write side effects, shared by all lanes once assembled.
static void reg_write_effects(dafb_t *dafb, uint32_t off, uint32_t value) {
    switch (off) {
    case DAFB_FB_BASE_HI:
    case DAFB_FB_BASE_LO:
    case DAFB_STRIDE:
    case DAFB_CONFIG:
    case SWATCH_MODE:
        reconfigure(dafb);
        break;
    case SWATCH_INTR_ENABLE:
        update_irq(dafb);
        break;
    case SWATCH_CLEAR_CURSOR:
        dafb->regs[SWATCH_INTR_STATUS >> 2] &= ~0x4u;
        update_irq(dafb);
        break;
    case SWATCH_CLEAR_VBL:
        dafb->regs[SWATCH_INTR_STATUS >> 2] &= ~0x1u;
        update_irq(dafb);
        break;
    default:
        if (off >= 0x200u && off < 0x300u)
            ac842_write(dafb, off, (uint8_t)value);
        else if (off >= 0x300u) {
            // DP8531: nibble register on the (offset>>4) index; only the
            // low byte lane carries data in the modeled interface.
            uint32_t creg = (off >> 4) & 0xFu;
            dafb->clk_reg[creg] = (uint8_t)(value & 0xFu);
            if (creg == 15)
                dp8531_commit(dafb);
        }
        break;
    }
}

// Reads with special sourcing (sense, RAMDAC, status side effects).
static bool reg_read_special(dafb_t *dafb, uint32_t off, uint32_t *out) {
    if (off == DAFB_SENSE) {
        *out = sense_read(dafb);
        return true;
    }
    if (off == DAFB_TEST_VERSION) {
        // Version rides bits 11:9 of the 12-bit test register (ref §11.8;
        // the driver's 33 MHz path shifts by 9 and compares to DAFB3Vers).
        *out = (dafb->regs[off >> 2] & 0x1FFu) | ((uint32_t)(dafb->version & 0x7u) << 9);
        return true;
    }
    if (off == 0x024u || off == 0x028u) {
        // TurboSCSI control readback: stored control bits + live DRQ in
        // bit 9 (ref §12.4).
        int chan = (off == 0x028u) ? 1 : 0;
        bool drq = dafb->drq_fn[chan] && dafb->drq_fn[chan](dafb->drq_ctx[chan]);
        *out = (dafb->regs[off >> 2] & 0x1FFu) | (drq ? 0x200u : 0u);
        return true;
    }
    if (off >= 0x200u && off < 0x300u) {
        *out = ac842_read(dafb, off);
        return true;
    }
    if (off == SWATCH_CLEAR_CURSOR) {
        dafb->regs[SWATCH_INTR_STATUS >> 2] &= ~0x4u; // access clears
        update_irq(dafb);
        *out = 0;
        return true;
    }
    if (off == SWATCH_CLEAR_VBL) {
        dafb->regs[SWATCH_INTR_STATUS >> 2] &= ~0x1u;
        update_irq(dafb);
        *out = 0;
        return true;
    }
    return false;
}

static uint32_t dafb_read32(void *ctx, uint32_t offset) {
    dafb_t *dafb = (dafb_t *)ctx;
    uint32_t off = reg_off(offset);
    uint32_t v;
    if (!reg_read_special(dafb, off, &v))
        v = dafb->regs[off >> 2];
    log_touch(dafb, offset, false, v);
    return v;
}

static uint8_t dafb_read8(void *ctx, uint32_t offset) {
    uint32_t v = dafb_read32(ctx, offset);
    return (uint8_t)(v >> (8 * (3 - (offset & 3))));
}

static uint16_t dafb_read16(void *ctx, uint32_t offset) {
    uint32_t v = dafb_read32(ctx, offset & ~1u);
    return (uint16_t)(v >> (8 * (2 - (offset & 2))));
}

static void dafb_write32(void *ctx, uint32_t offset, uint32_t value) {
    dafb_t *dafb = (dafb_t *)ctx;
    uint32_t off = reg_off(offset);
    dafb->regs[off >> 2] = value;
    log_touch(dafb, offset, true, value);
    reg_write_effects(dafb, off, value);
}

static void dafb_write8(void *ctx, uint32_t offset, uint8_t value) {
    dafb_t *dafb = (dafb_t *)ctx;
    uint32_t off = reg_off(offset);
    uint32_t shift = 8 * (3 - (offset & 3));
    uint32_t v = (dafb->regs[off >> 2] & ~(0xFFu << shift)) | ((uint32_t)value << shift);
    dafb->regs[off >> 2] = v;
    log_touch(dafb, offset, true, v);
    reg_write_effects(dafb, off, v);
}

static void dafb_write16(void *ctx, uint32_t offset, uint16_t value) {
    dafb_write8(ctx, offset, (uint8_t)(value >> 8));
    dafb_write8(ctx, offset + 1, (uint8_t)value);
}

static const memory_interface_t dafb_reg_iface = {
    .read_uint8 = dafb_read8,
    .read_uint16 = dafb_read16,
    .read_uint32 = dafb_read32,
    .write_uint8 = dafb_write8,
    .write_uint16 = dafb_write16,
    .write_uint32 = dafb_write32,
};

// ============================================================
// Lifecycle
// ============================================================

dafb_t *dafb_init(uint32_t vram_size, checkpoint_t *cp) {
    dafb_t *dafb = (dafb_t *)calloc(1, sizeof(dafb_t));
    if (!dafb)
        return NULL;
    dafb->vram_size = vram_size;
    dafb->vram = (uint8_t *)calloc(1, vram_size);
    if (!dafb->vram) {
        free(dafb);
        return NULL;
    }

    dafb->sense_code = 6; // 13" 640×480 RGB by default
    // The sense drive lines wake up tristated: the ROM's very first $01C
    // read (before it writes anything) must return the monitor's passive
    // code.  A reset value of 0 reads as "all driven low" → code 0 → the
    // ROM silently configures a 21" two-page display (observed).
    dafb->regs[DAFB_SENSE >> 2] = 0x7u;

    // Pre-mode-set display: 640×480×1 over the fallback frame period, so a
    // capture before the ROM's first mode set shows a sane blank raster.
    dafb->display.width = 640;
    dafb->display.height = 480;
    dafb->display.format = PIXEL_1BPP_MSB;
    dafb->display.stride = 1024;
    dafb->display.bits = dafb->vram + 0x1000;
    dafb->display.clut = dafb->clut;
    dafb->display.clut_len = 256;
    dafb->display.shape_dirty = true;
    dafb->display.clut_dirty = true;
    dafb->display.fb_dirty = true;
    dafb->display.response_dirty = true;
    for (int i = 0; i < 256; i++) {
        dafb->clut[i].r = dafb->clut[i].g = dafb->clut[i].b = (uint8_t)i;
        dafb->clut[i].a = 255;
    }

    if (cp) {
        system_read_checkpoint_data(cp, dafb->regs, sizeof(dafb->regs));
        system_read_checkpoint_data(cp, dafb->clut, sizeof(dafb->clut));
        system_read_checkpoint_data(cp, &dafb->dac_idx, sizeof(dafb->dac_idx));
        system_read_checkpoint_data(cp, &dafb->dac_phase, sizeof(dafb->dac_phase));
        system_read_checkpoint_data(cp, dafb->dac_rgb, sizeof(dafb->dac_rgb));
        system_read_checkpoint_data(cp, &dafb->pcbr0, sizeof(dafb->pcbr0));
        system_read_checkpoint_data(cp, &dafb->pcbr1, sizeof(dafb->pcbr1));
        system_read_checkpoint_data(cp, dafb->clk_reg, sizeof(dafb->clk_reg));
        system_read_checkpoint_data(cp, &dafb->clock_hz, sizeof(dafb->clock_hz));
        system_read_checkpoint_data(cp, dafb->vram, vram_size);
        reconfigure(dafb);
    }
    return dafb;
}

void dafb_delete(dafb_t *dafb) {
    if (!dafb)
        return;
    if (dafb->sched)
        remove_event(dafb->sched, dafb_frame_event, dafb);
    free(dafb->vram);
    free(dafb);
}

void dafb_checkpoint(dafb_t *dafb, checkpoint_t *cp) {
    if (!dafb || !cp)
        return;
    system_write_checkpoint_data(cp, dafb->regs, sizeof(dafb->regs));
    system_write_checkpoint_data(cp, dafb->clut, sizeof(dafb->clut));
    system_write_checkpoint_data(cp, &dafb->dac_idx, sizeof(dafb->dac_idx));
    system_write_checkpoint_data(cp, &dafb->dac_phase, sizeof(dafb->dac_phase));
    system_write_checkpoint_data(cp, dafb->dac_rgb, sizeof(dafb->dac_rgb));
    system_write_checkpoint_data(cp, &dafb->pcbr0, sizeof(dafb->pcbr0));
    system_write_checkpoint_data(cp, &dafb->pcbr1, sizeof(dafb->pcbr1));
    system_write_checkpoint_data(cp, dafb->clk_reg, sizeof(dafb->clk_reg));
    system_write_checkpoint_data(cp, &dafb->clock_hz, sizeof(dafb->clock_hz));
    system_write_checkpoint_data(cp, dafb->vram, dafb->vram_size);
}

void dafb_set_irq_callback(dafb_t *dafb, dafb_irq_cb cb, void *context) {
    if (!dafb)
        return;
    dafb->irq_cb = cb;
    dafb->irq_ctx = context;
    if (cb)
        cb(context, dafb->irq_line);
}

void dafb_set_monitor_sense(dafb_t *dafb, uint8_t code) {
    if (dafb)
        dafb->sense_code = code & 0x7u;
}

// Pending monitor sense consumed by the next Quadra construction — the
// built-in-video mirror of the JMFB pending slot, fed from
// `machine.boot video_sense=N` (machine.c).  Reset to the default $6
// (13" RGB) on consumption so a forgotten setting doesn't leak into a
// later boot.
static uint8_t s_dafb_pending_sense = 0x6;

void dafb_pending_sense_set(uint8_t code) {
    s_dafb_pending_sense = code & 0x7u;
}

uint8_t dafb_consume_pending_sense(void) {
    uint8_t code = s_dafb_pending_sense;
    s_dafb_pending_sense = 0x6;
    return code;
}

void dafb_set_version(dafb_t *dafb, uint8_t version) {
    if (dafb)
        dafb->version = version & 0x7u;
}

void dafb_set_ac842a(dafb_t *dafb, bool ac842a) {
    if (dafb)
        dafb->ac842a = ac842a;
}

void dafb_set_scsi_drq_query(dafb_t *dafb, int chan, dafb_drq_query_fn fn, void *context) {
    if (!dafb || chan < 0 || chan > 1)
        return;
    dafb->drq_fn[chan] = fn;
    dafb->drq_ctx[chan] = context;
}

uint8_t *dafb_vram(dafb_t *dafb) {
    return dafb ? dafb->vram : NULL;
}

uint32_t dafb_vram_size(dafb_t *dafb) {
    return dafb ? dafb->vram_size : 0;
}

display_t *dafb_display(dafb_t *dafb) {
    return dafb ? &dafb->display : NULL;
}

void dafb_reset(dafb_t *dafb) {
    if (!dafb)
        return;
    memset(dafb->regs, 0, sizeof(dafb->regs));
    memset(dafb->touched, 0, sizeof(dafb->touched));
    dafb->regs[DAFB_SENSE >> 2] = 0x7u; // sense drives tristate at reset
    memset(dafb->clk_reg, 0, sizeof(dafb->clk_reg));
    dafb->pcbr0 = 0;
    dafb->pcbr1 = 0;
    dafb->dac_idx = 0;
    dafb->dac_phase = 0;
    dafb->clock_hz = 0;
    dafb->timing_valid = false;
    dafb->irq_line = false;
    if (dafb->irq_cb)
        dafb->irq_cb(dafb->irq_ctx, false);
}

const memory_interface_t *dafb_reg_interface(dafb_t *dafb) {
    (void)dafb;
    return &dafb_reg_iface;
}
