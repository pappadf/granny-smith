// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// display_card_24ac.c
// "Apple Macintosh Display Card 24AC" — a 24-bit colour NuBus display
// card with a hardware QuickDraw fill/raster accelerator.  See
// proposal-nubus-card-display-card-24ac.md and the dossier under
// local/gs-docs/24AC/.  Cloned from the 8•24 (jmfb.c) shape, plus the
// acceleration engine the dossier's hardware spec (doc 3) describes.
//
// Two halves (proposal §0):
//   * Phase 1 (display): loads the genuine display-card-24ac.vrom and
//     presents a linear framebuffer + CLUT + VBL slot IRQ.  The card's own
//     System 7 video driver (in the vrom) programs the standard video
//     registers; we model the ones it touches (CLUT, depth/mode latch,
//     VBL mask/ACK, monitor sense) and accept-and-log the rest (CRTC
//     timing file, RAMDAC command, serial PLL).  Register offsets and
//     semantics were reverse-engineered from the vrom driver (see
//     tmp/24ac-vrom-re.md / display_card_24ac.h).
//   * Phase 2 (engine): STATUS/CONFIG/CONTROL registers, the operand
//     aperture (+ commit alias), and the +0x400000 active-bank alias that
//     transforms writes (run-length fill / block copy / ROP).  Modelled as
//     a synchronous software-equivalent straight into the passive VRAM
//     model — its output must match the driver's own CPU fallback (the
//     built-in oracle, proposal §3.5).
//
// Notable differences from the JMFB (per the RE):
//   * No VideoBase / RowWords slot register — the framebuffer base is a
//     driver-private VRAM pointer, so VRAM is modelled as a flat aperture
//     at offset 0.
//   * The standard video registers and the engine share the high slot
//     pages (0xC8/0xD0/0xD4/0xD8xxxx); one dispatcher serves them all.

#include "display_card_24ac.h"

#include "card.h"
#include "checkpoint.h"
#include "declrom.h"
#include "display.h"
#include "log.h"
#include "memory.h"
#include "nubus.h"
#include "rtc.h"
#include "system.h"
#include "system_config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("display_card_24ac");

// === Per-card private state =================================================

typedef struct display_card_24ac_priv display_card_24ac_priv_t;

// A device-region callback context: binds the card to the slot-relative
// base offset of the region it was registered for, so a single
// memory_interface_t can serve every register/engine region and dispatch
// on the full slot offset (region_off + region-relative addr).
typedef struct {
    display_card_24ac_priv_t *p;
    uint32_t region_off; // slot-relative base of this region
} reg_ctx_t;

struct display_card_24ac_priv {
    nubus_card_t *card; // back-pointer for IRQ helpers
    uint8_t *vram; // DISPLAY_CARD_24AC_VRAM_SIZE
    uint8_t *vrom; // 128 KB bus-space declaration ROM
    char *vrom_path; // path the VROM was loaded from
    uint32_t vrom_size; // 128 KB; 0 if no VROM loaded
    uint32_t slot_base; // physical bus base (== nubus_slot_base(slot))
    rgba8_t clut[256];
    display_t display;

    // CLUT / RAMDAC write sub-state.  Both the init (0xC8000E/0xC8000A)
    // and runtime (0xC8001E/0xC8001A) index/data pairs feed this; an index
    // write resets the R/G/B sub-counter, then three data writes load R,G,B.
    uint8_t clut_idx; // current palette index
    uint8_t clut_phase; // 0 = R, 1 = G, 2 = B
    rgba8_t clut_pending; // accumulating entry

    // Standard video register shadows (the few the vrom driver drives).
    uint8_t vidctl; // 0xD00403 shadow (VBL mask / commit / depth bits)
    uint8_t mode_reg; // 0xD80001 shadow (bits 7-5 depth code)
    uint8_t depth_reg; // 0xD80005 shadow (low nibble depth/clock)
    uint8_t status_busy; // toggling STATUS bit 4 (CLUT/VBL-sync poll)
    // Monitor sense (0xD8000D).  The vrom driver does an Apple 3-line + extended
    // sense: it drives the sense lines (writes here) and reads them back; the
    // primary code (bits 7-5, inverted) plus a 6-bit extended code disambiguate
    // the monitor.  We model the connected monitor as a (primary, ext) pair and
    // synthesise the read-back the driver expects for each line it drives.
    uint8_t sense_primary; // primary sense code 0-7 the monitor reports
    uint8_t sense_ext; // 6-bit extended-sense code (for primary 6/7 monitors)
    uint8_t sense_last_write; // last byte written to 0xD8000D (which lines driven)
    bool vbl_enabled; // slot VBL IRQ armed (VIDCTL bit 7 clear)

    // === Phase 2 acceleration engine ===
    bool engine_enabled; // false ⇒ active bank behaves as plain VRAM (oracle)
    uint8_t engine_mode; // latched CONTROL byte ($01 fill / $03 stretch /
                         // $7F copy / computed ROP)
    uint32_t engine_operand; // latched 32-bit operand (fill colour / pattern)
    uint8_t status_depth_code; // STATUS[2:0] — current depth/mode (for the cdev)
    bool status_class_bit; // STATUS[3] — card-class / VRAM-organisation
    bool config_variant_bit; // CONFIG[0] — geometry variant

    // Region contexts (one per registered register/engine region).
    reg_ctx_t ctx_clut; // 0xC80000 — CLUT / RAMDAC
    reg_ctx_t ctx_d00; // 0xD00000 — STATUS + VIDCTL
    reg_ctx_t ctx_d40; // 0xD40000 — CONFIG + CONTROL (engine)
    reg_ctx_t ctx_d80; // 0xD80000 — MODE / DEPTH / SENSE / CRTC
    reg_ctx_t ctx_operand; // 0x3FE000 — engine operand aperture
    reg_ctx_t ctx_active; // 0x400000 — engine active-bank alias
};

// === Display-format helpers =================================================

// Storage bits-per-pixel of a display format.
static uint32_t format_bpp(pixel_format_t f) {
    switch (f) {
    case PIXEL_1BPP_MSB:
        return 1;
    case PIXEL_2BPP_MSB:
        return 2;
    case PIXEL_4BPP_MSB:
        return 4;
    case PIXEL_8BPP:
        return 8;
    case PIXEL_16BPP_555:
        return 16;
    case PIXEL_32BPP_XRGB:
        return 32;
    default:
        return 8;
    }
}

// The depth code (cscSetMode's csMode[2:0]) matching a display format.  The
// 24AC's depth ladder has NO 2-bpp mode (vrom RE: cscSetMode depth table at
// chip 0x1F88 + CountTbl 0x2A28): code 0/1/2/3/4 = 1/4/8/16/32 bpp.  STATUS[2:0]
// and the cdev's stride-table index both follow this code, so keep them aligned.
static uint8_t depth_code_for_format(pixel_format_t f) {
    switch (f) {
    case PIXEL_1BPP_MSB:
        return 0;
    case PIXEL_4BPP_MSB:
        return 1;
    case PIXEL_8BPP:
        return 2;
    case PIXEL_16BPP_555:
        return 3;
    case PIXEL_32BPP_XRGB:
        return 4;
    default:
        return 2; // 8 bpp default (2-bpp is not a 24AC mode)
    }
}

// rowBytes for a flat framebuffer of the current width + depth.  The 24AC
// has no RowWords register; the driver lays VRAM out tightly, so stride =
// width × bpp / 8.
static void recompute_stride(display_card_24ac_priv_t *p) {
    uint32_t bpp = format_bpp(p->display.format);
    p->display.stride = (p->display.width * bpp + 7u) / 8u;
}

// Apply the depth selected by the MODE register's top three bits.  The depth
// ladder is the cscSetMode table the vrom video driver writes from (chip
// 0x1F88; verified against the per-mode VPBlocks and the CountTbl 0x2A28):
//   MODE[7:5]  0x00  0x20  0x40  0xA0  0xC0
//   bpp           1     4     8    16    32
// There is NO 2-bpp mode.  When System 7 selects 8-bpp colour it writes MODE
// bits 0x40 (NOT 0xA0); mapping that to 8 bpp here is what makes the colour
// desktop render.  Other (unlisted) values keep the current format.
static void apply_mode_depth(display_card_24ac_priv_t *p, uint8_t mode_byte) {
    pixel_format_t f = p->display.format;
    switch (mode_byte & 0xE0u) {
    case 0x00u:
        f = PIXEL_1BPP_MSB;
        break;
    case 0x20u:
        f = PIXEL_4BPP_MSB;
        break;
    case 0x40u:
        f = PIXEL_8BPP;
        break;
    case 0xA0u:
        f = PIXEL_16BPP_555;
        break;
    case 0xC0u:
        f = PIXEL_32BPP_XRGB;
        break;
    default:
        break; // unlisted timing-only write — leave format unchanged
    }
    p->status_depth_code = depth_code_for_format(f);
    if (f != p->display.format) {
        p->display.format = f;
        recompute_stride(p);
        p->display.shape_dirty = true;
        LOG(2, "MODE depth → %u bpp (stride %u)", format_bpp(f), p->display.stride);
    }
}

// === CLUT load ==============================================================

// Index-register write: latch the palette index and reset the R/G/B
// sub-counter so the next three data writes load that entry.
static void clut_set_index(display_card_24ac_priv_t *p, uint8_t idx) {
    p->clut_idx = idx;
    p->clut_phase = 0;
}

// Data-register write: accumulate R, G, B over three writes, commit, and
// auto-increment the index for a run-write.
static void clut_write_data(display_card_24ac_priv_t *p, uint8_t comp) {
    switch (p->clut_phase) {
    case 0:
        p->clut_pending.r = comp;
        p->clut_phase = 1;
        break;
    case 1:
        p->clut_pending.g = comp;
        p->clut_phase = 2;
        break;
    case 2:
    default:
        p->clut_pending.b = comp;
        p->clut_pending.a = 255;
        p->clut[p->clut_idx] = p->clut_pending;
        p->clut_idx++; // auto-increment for run-write
        p->clut_phase = 0;
        p->display.clut_dirty = true;
        break;
    }
}

// === Phase 2: the acceleration engine =======================================
//
// Every engine behaviour reduces to "produce the software-equivalent
// result straight into the passive VRAM model".  The driver feeds the
// engine synchronously and never polls a busy flag (hardware spec §7), so
// there is no timing to model.

// Replicate the latched 32-bit operand across `len` bytes of VRAM starting
// at passive offset `dest`, phase-aligned to absolute 4-byte VRAM columns
// (operand byte i lands wherever (addr & 3) == i).  This matches a hardware
// pattern engine that aligns its 4-byte pattern to the framebuffer; for a
// solid fill (operand = colour replicated ×4) the alignment is irrelevant.
static void engine_fill_run(display_card_24ac_priv_t *p, uint32_t dest, uint32_t len) {
    if (dest >= DISPLAY_CARD_24AC_VRAM_SIZE)
        return;
    if (len > DISPLAY_CARD_24AC_VRAM_SIZE - dest)
        len = DISPLAY_CARD_24AC_VRAM_SIZE - dest; // clamp to VRAM
    uint8_t pat[4] = {
        (uint8_t)(p->engine_operand >> 24),
        (uint8_t)(p->engine_operand >> 16),
        (uint8_t)(p->engine_operand >> 8),
        (uint8_t)(p->engine_operand),
    };
    for (uint32_t i = 0; i < len; i++)
        p->vram[dest + i] = pat[(dest + i) & 3];
    p->display.fb_dirty = true;
}

// Store one source longword through the active aperture in copy/ROP mode.
// `dest` is the passive VRAM offset (active addr − 0x400000).  $7F (and
// stretch $03) are a straight copy; computed raster-op codes are not yet
// validated against the live-driver oracle, so they fall back to copy with
// a log.  Because the cdev degrades to the (correct) software path whenever
// its gates fail, a wrong ROP can only make the *accelerated* result
// diverge — which the engine-vs-fallback oracle test (proposal §3.5) would
// catch — never corrupt the displayed image.
static void engine_store_long(display_card_24ac_priv_t *p, uint32_t dest, uint32_t src) {
    if (dest + 4 > DISPLAY_CARD_24AC_VRAM_SIZE)
        return; // out of VRAM — drop (driver never streams past the bank)
    uint32_t out;
    switch (p->engine_mode) {
    case DISPLAY_CARD_24AC_MODE_COPY:
    case DISPLAY_CARD_24AC_MODE_STRETCH:
        out = src; // straight transfer
        break;
    default:
        // Computed ROP code ($00..$3F) — semantics await oracle validation.
        LOG(3, "engine: ROP mode $%02x streamed long $%08x → dest $%06x (copy fallback)", p->engine_mode, src, dest);
        out = src;
        break;
    }
    STORE_BE32(p->vram + dest, out);
    p->display.fb_dirty = true;
}

// === Unified register/engine dispatcher =====================================
// `off` is the full slot-relative offset (region_off + region-relative addr).

static uint32_t reg_read(display_card_24ac_priv_t *p, uint32_t off, unsigned width) {
    switch (off) {
    // --- Display side -------------------------------------------------------
    case DISPLAY_CARD_24AC_STATUS_OFFSET: {
        // STATUS byte: [2:0] depth (cdev), [3] class, [4] busy/sync toggle.
        // Toggle bit 4 each read so the driver's CLUT-safe / VBL-sync poll
        // always sees both edges and exits (mirrors jmfb's VBL toggle).
        p->status_busy ^= DISPLAY_CARD_24AC_STATUS_BUSY;
        return (uint32_t)((p->status_depth_code & 7u) | (p->status_class_bit ? 0x08u : 0x00u) | p->status_busy);
    }
    case DISPLAY_CARD_24AC_VIDCTL_OFFSET:
        return p->vidctl;
    case DISPLAY_CARD_24AC_MODE_REG:
        return p->mode_reg;
    case DISPLAY_CARD_24AC_DEPTH_REG:
        return p->depth_reg;
    case DISPLAY_CARD_24AC_SENSE_CLK: {
        // Monitor sense read-back, keyed by which line the driver last drove
        // (vrom chip 0x9C-0x13A).  Primary probe drives all lines low (last
        // write 0) and expects (~read & 0xE0)>>5 == primary code.  The extended
        // probe drives one line and reads the other two; each pair of read-back
        // bits is inverted into two bits of the 6-bit ext code (see the disasm
        // BTST/BSET sequence — bit set in the read ⇒ 0 in the ext code).
        uint8_t e = p->sense_ext;
        switch (p->sense_last_write & 0xE0u) {
        case 0x00u: // primary probe
            return (uint32_t)(((uint8_t)(~p->sense_primary) & 7u) << 5);
        case 0x80u: // drove bit7 → ext bit5 = !read.bit6, ext bit4 = !read.bit5
            return (uint32_t)(((e & 0x20u) ? 0u : 0x40u) | ((e & 0x10u) ? 0u : 0x20u));
        case 0x40u: // drove bit6 → ext bit3 = !read.bit7, ext bit2 = !read.bit5
            return (uint32_t)(((e & 0x08u) ? 0u : 0x80u) | ((e & 0x04u) ? 0u : 0x20u));
        case 0x20u: // drove bit5 → ext bit1 = !read.bit7, ext bit0 = !read.bit6
            return (uint32_t)(((e & 0x02u) ? 0u : 0x80u) | ((e & 0x01u) ? 0u : 0x40u));
        default:
            return 0;
        }
    }
    // --- Engine side --------------------------------------------------------
    case DISPLAY_CARD_24AC_CONFIG_OFFSET:
        return (uint32_t)(p->config_variant_bit ? 0x01u : 0x00u);
    case DISPLAY_CARD_24AC_OPERAND_APERTURE:
        return p->engine_operand; // driver's 1-entry pattern cache read-back
    default:
        break;
    }
    // Top-of-bank carve-out [VRAM_VISIBLE .. active alias): the VRAM host
    // region stops at DISPLAY_CARD_24AC_VRAM_VISIBLE (so the framebuffer alias clears
    // the register pages and the operand aperture isn't shadowed by the MMU
    // VRAM slot).  Every byte here other than the operand longword is still
    // plain passive VRAM, so serve it from p->vram — otherwise this range
    // would be an unmapped hole.
    if (off >= DISPLAY_CARD_24AC_VRAM_VISIBLE && off < DISPLAY_CARD_24AC_ENGINE_ALIAS_OFFSET) {
        if (width == 4 && off + 4 <= DISPLAY_CARD_24AC_VRAM_SIZE)
            return LOAD_BE32(p->vram + off);
        if (off < DISPLAY_CARD_24AC_VRAM_SIZE)
            return p->vram[off];
        return 0;
    }
    // Active-bank alias reads are just the underlying passive VRAM.
    if (off >= DISPLAY_CARD_24AC_ENGINE_ALIAS_OFFSET &&
        off < DISPLAY_CARD_24AC_ENGINE_ALIAS_OFFSET + DISPLAY_CARD_24AC_VRAM_SIZE) {
        uint32_t dest = off - DISPLAY_CARD_24AC_ENGINE_ALIAS_OFFSET;
        if (width == 4 && dest + 4 <= DISPLAY_CARD_24AC_VRAM_SIZE)
            return LOAD_BE32(p->vram + dest);
        if (dest < DISPLAY_CARD_24AC_VRAM_SIZE)
            return p->vram[dest];
        return 0;
    }
    LOG(3, "unmodeled read off $%06x width %u", off, width);
    return 0;
}

static void reg_write(display_card_24ac_priv_t *p, uint32_t off, uint32_t val, unsigned width) {
    switch (off) {
    // --- CLUT / RAMDAC ------------------------------------------------------
    case DISPLAY_CARD_24AC_CLUT0_ADDR:
    case DISPLAY_CARD_24AC_CLUT_ADDR:
        clut_set_index(p, (uint8_t)val);
        return;
    case DISPLAY_CARD_24AC_CLUT0_DATA:
    case DISPLAY_CARD_24AC_CLUT_DATA:
        clut_write_data(p, (uint8_t)val);
        return;
    case DISPLAY_CARD_24AC_RAMDAC_CMD:
        LOG(3, "RAMDAC command = $%02x (accept-and-log)", (uint8_t)val);
        return;
    case DISPLAY_CARD_24AC_CLUT_CTL:
        LOG(3, "CLUT control strobe = $%02x (accept-and-log)", (uint8_t)val);
        return;
    // --- Display control ----------------------------------------------------
    case DISPLAY_CARD_24AC_VIDCTL_OFFSET:
        // Central video latch.  Bit 7 = slot-VBL-IRQ mask (1 = masked).
        // The driver enables by clearing bit 7, disables by setting it, and
        // the ISR pulses set→clear to acknowledge the pending IRQ — so a set
        // bit 7 always means "release the line now" (deassert), and a clear
        // bit 7 re-arms it for the next VBL.
        p->vidctl = (uint8_t)val;
        if (val & DISPLAY_CARD_24AC_VIDCTL_VBL_MASK) {
            p->vbl_enabled = false;
            nubus_deassert_irq(p->card);
        } else {
            p->vbl_enabled = true;
        }
        return;
    case DISPLAY_CARD_24AC_MODE_REG:
        p->mode_reg = (uint8_t)val;
        apply_mode_depth(p, (uint8_t)val);
        return;
    case DISPLAY_CARD_24AC_DEPTH_REG:
        p->depth_reg = (uint8_t)val;
        return;
    case DISPLAY_CARD_24AC_SENSE_CLK:
        // Drives the sense lines (bits 7-5, read back by the sense probe) and
        // bit-bangs the serial PLL (low bits).  Track the driven lines so the
        // sense read-back above can answer the probe; the clock program itself
        // has no modelled effect.
        p->sense_last_write = (uint8_t)val;
        LOG(3, "SENSE_CLK write $%02x (sense drive / PLL)", (uint8_t)val);
        return;
    // --- Engine -------------------------------------------------------------
    case DISPLAY_CARD_24AC_CONTROL_OFFSET:
        p->engine_mode = (uint8_t)val; // latch op mode for active-bank writes
        LOG(3, "engine: CONTROL = $%02x", p->engine_mode);
        return;
    case DISPLAY_CARD_24AC_OPERAND_APERTURE:
        if (width == 4)
            p->engine_operand = val;
        LOG(3, "engine: operand load = $%08x", p->engine_operand);
        return;
    default:
        break;
    }
    // CRTC timing register file (write-only) — accept-and-log.
    if (off >= DISPLAY_CARD_24AC_CRTC_LO && off <= DISPLAY_CARD_24AC_CRTC_HI) {
        LOG(3, "CRTC[$%06x] = $%02x (accept-and-log)", off, (uint8_t)val);
        return;
    }
    // Top-of-bank carve-out [VRAM_VISIBLE .. active alias): the operand
    // longword is intercepted above; every other byte is plain passive VRAM
    // (the VRAM host region stops at DISPLAY_CARD_24AC_VRAM_VISIBLE — see reg_read).
    if (off >= DISPLAY_CARD_24AC_VRAM_VISIBLE && off < DISPLAY_CARD_24AC_ENGINE_ALIAS_OFFSET) {
        if (width == 4 && off + 4 <= DISPLAY_CARD_24AC_VRAM_SIZE)
            STORE_BE32(p->vram + off, val);
        else if (off < DISPLAY_CARD_24AC_VRAM_SIZE)
            p->vram[off] = (uint8_t)val;
        p->display.fb_dirty = true;
        return;
    }
    // Active-bank alias (engine-transforming): off ∈ [0x400000, +VRAM).
    if (off >= DISPLAY_CARD_24AC_ENGINE_ALIAS_OFFSET &&
        off < DISPLAY_CARD_24AC_ENGINE_ALIAS_OFFSET + DISPLAY_CARD_24AC_VRAM_SIZE) {
        uint32_t dest = off - DISPLAY_CARD_24AC_ENGINE_ALIAS_OFFSET;
        // The operand aperture's +0x400000 alias is the commit window: a
        // write of `4` here latches the loaded operand (driver writes twice).
        if (dest == DISPLAY_CARD_24AC_OPERAND_APERTURE) {
            if (val == DISPLAY_CARD_24AC_COMMIT_CMD)
                LOG(3, "engine: operand commit ($%08x)", p->engine_operand);
            return;
        }
        if (!p->engine_enabled || width != 4) {
            // Oracle mode (engine off) or a narrow write the driver never
            // issues through the active bank — behave as plain VRAM so
            // nothing bus-errors and the model can be compared to the
            // driver's software fallback.
            if (width == 4 && dest + 4 <= DISPLAY_CARD_24AC_VRAM_SIZE)
                STORE_BE32(p->vram + dest, val);
            else if (dest < DISPLAY_CARD_24AC_VRAM_SIZE)
                p->vram[dest] = (uint8_t)val;
            p->display.fb_dirty = true;
            return;
        }
        if (p->engine_mode == DISPLAY_CARD_24AC_MODE_FILL)
            engine_fill_run(p, dest, val); // longword value == run length
        else
            engine_store_long(p, dest, val); // longword value == source pixels
        return;
    }
    LOG(3, "unmodeled write off $%06x = $%08x width %u", off, val, width);
}

// === Memory interface (single dispatcher over every region) =================

static uint8_t io_read8(void *dev, uint32_t addr) {
    reg_ctx_t *c = dev;
    return (uint8_t)reg_read(c->p, c->region_off + addr, 1);
}
static uint16_t io_read16(void *dev, uint32_t addr) {
    reg_ctx_t *c = dev;
    return (uint16_t)reg_read(c->p, c->region_off + addr, 2);
}
static uint32_t io_read32(void *dev, uint32_t addr) {
    reg_ctx_t *c = dev;
    return reg_read(c->p, c->region_off + addr, 4);
}
static void io_write8(void *dev, uint32_t addr, uint8_t val) {
    reg_ctx_t *c = dev;
    reg_write(c->p, c->region_off + addr, val, 1);
}
static void io_write16(void *dev, uint32_t addr, uint16_t val) {
    reg_ctx_t *c = dev;
    reg_write(c->p, c->region_off + addr, val, 2);
}
static void io_write32(void *dev, uint32_t addr, uint32_t val) {
    reg_ctx_t *c = dev;
    reg_write(c->p, c->region_off + addr, val, 4);
}

static memory_interface_t s_display_card_24ac_mem_iface = {
    .read_uint8 = io_read8,
    .read_uint16 = io_read16,
    .read_uint32 = io_read32,
    .write_uint8 = io_write8,
    .write_uint16 = io_write16,
    .write_uint32 = io_write32,
};

// === VROM load ==============================================================

// Load display-card-24ac.vrom through the shared declrom loader (search
// paths + byteLanes expansion).  Returns true on success.
static bool load_vrom(display_card_24ac_priv_t *p) {
    char *path = NULL;
    if (!declrom_load_vrom("display-card-24ac.vrom", DISPLAY_CARD_24AC_DECLROM_CHIP_SIZE, p->vrom,
                           DISPLAY_CARD_24AC_DECLROM_BUS_SIZE, &path))
        return false;
    free(p->vrom_path);
    p->vrom_path = path;
    p->vrom_size = DISPLAY_CARD_24AC_DECLROM_BUS_SIZE;
    return true;
}

// === Video-mode selection (machine.nubus.video_mode) ========================
//
// A pending "<monitor>_<N>bpp" id (e.g. "rgb_640x480_8bpp") set before
// machine.boot; consumed by the next card_init, which sets the monitor sense +
// depth and seeds PRAM so the OS boots at that mode (mirrors jmfb.c).  The id
// is resolved against display_card_24ac_monitors[] × its depth list.
static char s_pending_video_mode_id[40] = "";

// bpp → MODE register depth bits (vrom RE depth ladder; no 2-bpp mode).
static uint8_t modebits_for_format(pixel_format_t f) {
    switch (f) {
    case PIXEL_1BPP_MSB:
        return 0x00u;
    case PIXEL_4BPP_MSB:
        return 0x20u;
    case PIXEL_8BPP:
        return 0x40u;
    case PIXEL_16BPP_555:
        return 0xA0u;
    case PIXEL_32BPP_XRGB:
        return 0xC0u;
    default:
        return 0x40u;
    }
}
static pixel_format_t format_for_bpp(int bpp) {
    switch (bpp) {
    case 1:
        return PIXEL_1BPP_MSB;
    case 4:
        return PIXEL_4BPP_MSB;
    case 8:
        return PIXEL_8BPP;
    case 16:
        return PIXEL_16BPP_555;
    case 32:
        return PIXEL_32BPP_XRGB;
    default:
        return PIXEL_8BPP;
    }
}
// bpp → slot sPRAMRec savedMode byte (the depth sub-resource id; vrom RE):
// 1/4/8/16/32 bpp → 0x80/0x81/0x82/0x83/0x84 (no 0x?? for 2 bpp).
static uint8_t savedmode_for_bpp(int bpp) {
    switch (bpp) {
    case 1:
        return 0x80u;
    case 4:
        return 0x81u;
    case 8:
        return 0x82u;
    case 16:
        return 0x83u;
    case 32:
        return 0x84u;
    default:
        return 0x82u;
    }
}
// Monitor sister sRsrc id → the extended-sense code the card reports on
// SENSE_CLK so PrimaryInit lands on that monitor (vrom RE).
static uint8_t ext_for_sister(uint8_t sister) {
    switch (sister) {
    case 0x6Bu:
        return 0x03u; // 640×480
    case 0x6Cu:
        return 0x0Bu; // 800×600
    case 0x6Du:
        return 0x23u; // 832×624
    default:
        return 0x03u;
    }
}

// === Card vtable ============================================================

static int card_init(nubus_card_t *card, config_t *cfg, checkpoint_t *cp) {
    (void)cp;
    display_card_24ac_priv_t *p = calloc(1, sizeof(*p));
    if (!p)
        return -1;
    p->card = card;
    p->slot_base = nubus_slot_base(card->slot);

    // Consume a pending video-mode pick (machine.nubus.video_mode = "..."):
    // it overrides the power-on sense/geometry/depth below and seeds PRAM at
    // the end of init so the OS boots at the chosen monitor + depth.
    const nubus_monitor_t *seeded_monitor = NULL;
    int seeded_depth_bpp = 0;
    if (s_pending_video_mode_id[0] &&
        display_card_24ac_video_mode_lookup(s_pending_video_mode_id, &seeded_monitor, &seeded_depth_bpp))
        s_pending_video_mode_id[0] = '\0'; // consume on match (ignore foreign ids)
    else
        seeded_monitor = NULL;

    p->vram = calloc(1, DISPLAY_CARD_24AC_VRAM_SIZE);
    p->vrom = calloc(1, DISPLAY_CARD_24AC_DECLROM_BUS_SIZE);
    if (!p->vram || !p->vrom) {
        free(p->vram);
        free(p->vrom);
        free(p);
        return -1;
    }

    if (!load_vrom(p)) {
        // requires_vrom gates the dialog on a real file; reaching here means
        // CI ran without one.  Log loudly and continue with a zero declrom —
        // PrimaryInit finds no Format Header and the OS skips the slot.
        LOG(0, "display-card-24ac.vrom not found; declaration ROM is zero-filled");
    }

    // Publish the declaration ROM on the generic card handle so the
    // object-model `slot[N].card.declrom` node (proposal §3.8) reads it
    // without reaching into card-private state.
    card->declrom = p->vrom;
    card->declrom_size = p->vrom_size;

    // Phase-1 starting state: 8 bpp, 640×480, framebuffer at VRAM offset 0.
    // The vrom's video driver re-programs depth/CLUT/timing at boot.
    //
    // VIDCTL power-on default: low 3 bits = 2.  PrimaryInit's monitor-sense
    // path reads VIDCTL ($D00403) at vrom chip 0x176 and, if its low 3 bits
    // are NOT 2, forces the monitor id to the "$47 standard-monitor" marker —
    // which makes the board self-test (vrom 0x3A2) get skipped, leaving
    // BOARDCFG[0]=0, which makes the driver's Open routine return openErr(-23)
    // (vrom 0x1AC6).  Seeding low bits = 2 here lets the sensed monitor stand,
    // the self-test run, and the driver open.  (Bit 7 = VBL IRQ masked.)
    p->vidctl = DISPLAY_CARD_24AC_VIDCTL_VBL_MASK | 0x02u;
    p->vbl_enabled = false;
    p->mode_reg = 0x40u; // 8 bpp depth code in bits 7-5 (0x40 = code 2, vrom RE)
    // Default monitor: 640×480 multisync (sRsrc id $6B = primary sense 6 +
    // extended-sense code $03).  Unlike the $40 "two-page" monitor (1152×870,
    // grayscale 1bpp only), the multisync display supports colour depths.
    p->sense_primary = 6;
    p->sense_ext = 0x03;

    // Engine geometry bits, kept consistent with the large-VRAM framebuffer
    // we present (hardware spec §3/§5).
    p->engine_enabled = true;
    p->engine_mode = DISPLAY_CARD_24AC_MODE_COPY;
    p->status_depth_code = depth_code_for_format(PIXEL_8BPP);
    p->status_class_bit = true; // large-VRAM organisation
    p->config_variant_bit = true; // large-VRAM geometry variant

    // Default 640×480 8 bpp (the sensed multisync monitor).  cscSetMode
    // (→ apply_mode_depth) re-derives the pixel format and stride as the OS
    // changes depth; the power-on default matches the MODE register seed above
    // so the framebuffer alias has the right 8-bpp stride before the OS runs.
    // The framebuffer sits at VRAM offset 0 (ScrnBase = slot+0x900000 → the
    // alias → VRAM[0]).
    p->display.width = 640;
    p->display.height = 480;
    p->display.format = PIXEL_8BPP;
    recompute_stride(p); // 640 bytes/row at 8 bpp
    p->display.bits = p->vram;
    p->display.clut = p->clut;
    p->display.clut_len = 256;
    p->display.crt_response = NULL; // identity until a monitor needs gamma
    p->display.shape_dirty = true;
    p->display.clut_dirty = true;
    p->display.fb_dirty = true;
    p->display.response_dirty = true;

    // Apply a pending video-mode pick over the power-on defaults: report the
    // chosen monitor on the sense lines and bring the framebuffer up at the
    // chosen depth/geometry (the OS re-confirms both via the sResource + the
    // PRAM seed below).
    if (seeded_monitor) {
        p->sense_primary = 6;
        p->sense_ext = ext_for_sister(seeded_monitor->srsrc_sister);
        p->display.width = seeded_monitor->width;
        p->display.height = seeded_monitor->height;
        pixel_format_t f = format_for_bpp(seeded_depth_bpp);
        p->display.format = f;
        p->mode_reg = modebits_for_format(f);
        p->status_depth_code = depth_code_for_format(f);
        recompute_stride(p);
    }

    // Initial CLUT — grayscale ramp so the canvas isn't blank before the OS
    // programs a palette; the driver's first cscSetEntries overwrites it.
    for (int i = 0; i < 256; i++) {
        p->clut[i].r = (uint8_t)i;
        p->clut[i].g = (uint8_t)i;
        p->clut[i].b = (uint8_t)i;
        p->clut[i].a = 255;
    }

    card->priv = p;

    // Host-backed regions: VRAM (writable), declaration ROM (read-only).
    //
    // The VRAM host region covers only DISPLAY_CARD_24AC_VRAM_VISIBLE (the
    // framebuffer area).  Two reasons it stops short of the full 4 MB:
    //   * the MMU models VRAM as one contiguous range whose translation is
    //     re-filled on every _SwapMMUMode TLB flush (PrimaryInit does
    //     thousands), which would SHADOW the operand-aperture device page if
    //     that page were inside the host range (the JMFB sidesteps this by
    //     putting its registers above its VRAM; the 24AC's aperture is inside
    //     the bank); and
    //   * the 24-bit framebuffer alias below mirrors this same extent at
    //     slot+0x900000, and it must end at/below the first register page
    //     (0xC80000) so it never shadows a card register.
    // The board-config scratch at 0xFFFD8 and the operand aperture both sit
    // above DISPLAY_CARD_24AC_VRAM_VISIBLE and are served by the operand device
    // region's passive fall-through (reg_read/reg_write), so the whole bank
    // remains addressable.
    memory_map_host_region(cfg->mem_map, "display_card_24ac_vram", p->vram, p->slot_base,
                           DISPLAY_CARD_24AC_VRAM_VISIBLE,
                           /*writable*/ true);
    memory_map_host_region(cfg->mem_map, "display_card_24ac_declrom", p->vrom,
                           p->slot_base + DISPLAY_CARD_24AC_DECLROM_BUS_OFFSET, DISPLAY_CARD_24AC_DECLROM_BUS_SIZE,
                           /*writable*/ false);
    // 24-bit Memory Manager mode framebuffer alias (mirrors VRAM at
    // slot+0x900000 = ScrnBase).  Same mechanism the 8•24 uses.  Sized by the
    // host region above (DISPLAY_CARD_24AC_VRAM_VISIBLE), so it ends at 0xC80000.
    memory_map_host_region_alias(cfg->mem_map, p->slot_base + DISPLAY_CARD_24AC_FB_ALIAS_OFFSET, p->slot_base);

    // Register/engine regions share the one dispatcher; each region's
    // reg_ctx carries its slot-relative base so reg_read/reg_write see full
    // slot offsets.  Registered AFTER the VRAM host region so the
    // operand-aperture page overlays it (last registration wins per page).
    p->ctx_clut = (reg_ctx_t){.p = p, .region_off = DISPLAY_CARD_24AC_CLUT_PAGE};
    p->ctx_d00 = (reg_ctx_t){.p = p, .region_off = DISPLAY_CARD_24AC_D00_PAGE};
    p->ctx_d40 = (reg_ctx_t){.p = p, .region_off = DISPLAY_CARD_24AC_D40_PAGE};
    p->ctx_d80 = (reg_ctx_t){.p = p, .region_off = DISPLAY_CARD_24AC_D80_PAGE};
    p->ctx_operand = (reg_ctx_t){.p = p, .region_off = DISPLAY_CARD_24AC_VRAM_VISIBLE};
    p->ctx_active = (reg_ctx_t){.p = p, .region_off = DISPLAY_CARD_24AC_ENGINE_ALIAS_OFFSET};

    memory_map_add(cfg->mem_map, p->slot_base + DISPLAY_CARD_24AC_CLUT_PAGE, MEM_PAGE_SIZE, "display_card_24ac_clut",
                   &s_display_card_24ac_mem_iface, &p->ctx_clut);
    memory_map_add(cfg->mem_map, p->slot_base + DISPLAY_CARD_24AC_D00_PAGE, MEM_PAGE_SIZE, "display_card_24ac_d00",
                   &s_display_card_24ac_mem_iface, &p->ctx_d00);
    memory_map_add(cfg->mem_map, p->slot_base + DISPLAY_CARD_24AC_D40_PAGE, MEM_PAGE_SIZE, "display_card_24ac_d40",
                   &s_display_card_24ac_mem_iface, &p->ctx_d40);
    memory_map_add(cfg->mem_map, p->slot_base + DISPLAY_CARD_24AC_D80_PAGE, MEM_PAGE_SIZE, "display_card_24ac_d80",
                   &s_display_card_24ac_mem_iface, &p->ctx_d80);
    // Carved-off top of the passive bank [DISPLAY_CARD_24AC_VRAM_VISIBLE,
    // 0x400000): the VRAM host region stops at DISPLAY_CARD_24AC_VRAM_VISIBLE so the
    // framebuffer alias clears the register pages.  This device region covers
    // the rest of the bank up to the active alias — it intercepts the operand
    // longword (0x3FE000) and serves every other byte as plain passive VRAM
    // (reg_read/reg_write fall through to p->vram), leaving no unmapped hole.
    memory_map_add(cfg->mem_map, p->slot_base + DISPLAY_CARD_24AC_VRAM_VISIBLE,
                   DISPLAY_CARD_24AC_ENGINE_ALIAS_OFFSET - DISPLAY_CARD_24AC_VRAM_VISIBLE, "display_card_24ac_operand",
                   &s_display_card_24ac_mem_iface, &p->ctx_operand);
    // Active-bank alias: the engine-transforming mirror of the passive bank
    // (covers the operand commit window at +0x400000 too).
    memory_map_add(cfg->mem_map, p->slot_base + DISPLAY_CARD_24AC_ENGINE_ALIAS_OFFSET, DISPLAY_CARD_24AC_VRAM_SIZE,
                   "display_card_24ac_engine", &s_display_card_24ac_mem_iface, &p->ctx_active);

    // Seed PRAM for the picked video mode (mirrors jmfb.c).  The key is a
    // *complete* valid PRAM, not just the two validity tokens: stamp the
    // 'NuMc' XPRAM signature so CkNewPram preserves what we write, reproduce
    // PRAMInitTbl (OS type + boot drive = "any") so the Start Manager still
    // finds and boots a SCSI volume, and write the slot sPRAMRec (savedMode =
    // depth, saved monitor = sister) so GET_SLOT_DEPTH lands on the chosen
    // depth.  This is exactly what lets the new-machine dialog set a graphics
    // mode AND boot a configured SCSI HD in one shot.  (rtc.pram.validate
    // writes only the validity tokens — an incomplete PRAM with a zeroed boot
    // device, which is why a bare validate cannot SCSI-boot; see the dossier.)
    if (seeded_monitor && seeded_depth_bpp > 0) {
        rtc_t *rtc = system_rtc();
        if (rtc) {
            uint8_t saved_mode = savedmode_for_bpp(seeded_depth_bpp);
            // 'NuMc' XPRAM validity signature ($0C..$0F) only — leave the
            // low-PRAM validity byte invalid so _InitUtil still cold-inits the
            // SysParam block (caret-blink / double-click defaults).
            rtc_pram_write(rtc, 0x0C, 0x4E); // 'N'
            rtc_pram_write(rtc, 0x0D, 0x75); // 'u'
            rtc_pram_write(rtc, 0x0E, 0x4D); // 'M'
            rtc_pram_write(rtc, 0x0F, 0x63); // 'c'
            // PRAMInitTbl ($76..$89): $77 = default OS (Mac), $78..$7B = boot
            // drive/partition "any" ($FFFFFFDF).  CkNewPram skips writing this
            // once 'NuMc' is present, so reproduce it or D3 reaches SCSILoad as
            // $00000000 and the boot-driver match never fires (→ "?" floppy).
            static const uint8_t pram_init_tbl[] = {
                0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xDF, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            };
            for (size_t i = 0; i < sizeof(pram_init_tbl); i++)
                rtc_pram_write(rtc, (uint8_t)(0x76 + i), pram_init_tbl[i]);
            // Per-slot sPRAMRec at $46 + (slot-9)*8: $46/$47 BoardID ($05FA),
            // $48 savedMode (depth), $4C/$4D saved monitor sister.  $4C/$4D
            // must equal the sensed monitor or PrimaryInit resets the depth.
            uint8_t off = (uint8_t)(0x46 + (card->slot - 9) * 8);
            rtc_pram_write(rtc, off + 0, 0x05);
            rtc_pram_write(rtc, off + 1, 0xFA);
            rtc_pram_write(rtc, off + 2, saved_mode);
            rtc_pram_write(rtc, off + 3, 0x00);
            rtc_pram_write(rtc, off + 4, 0x00);
            rtc_pram_write(rtc, off + 5, 0x00);
            rtc_pram_write(rtc, off + 6, seeded_monitor->srsrc_sister);
            rtc_pram_write(rtc, off + 7, seeded_monitor->srsrc_sister);
            LOG(1, "display_card_24ac: seeded slot-%d PRAM for video mode '%s' (savedMode=$%02x sister=$%02x)",
                card->slot, seeded_monitor->id, saved_mode, seeded_monitor->srsrc_sister);
        }
    }

    return 0;
}

static void card_teardown(nubus_card_t *card, config_t *cfg) {
    (void)cfg;
    display_card_24ac_priv_t *p = card->priv;
    if (!p)
        return;
    free(p->vram);
    // p->vrom is published as card->declrom (for the object-model declrom
    // node); nubus_delete owns and frees card->declrom after this teardown,
    // so we must NOT free it here (doing so double-frees).
    free(p->vrom_path);
    free(p);
    card->priv = NULL;
}

static void card_on_vbl(nubus_card_t *card, config_t *cfg) {
    (void)cfg;
    display_card_24ac_priv_t *p = card->priv;
    if (!p)
        return;
    if (p->vbl_enabled)
        nubus_assert_irq(card);
    // CPU writes to VRAM go straight through the host_region mapping and
    // don't notify the renderer; this VBL pulse surfaces ongoing drawing.
    p->display.fb_dirty = true;
}

static display_t *card_display(nubus_card_t *card) {
    display_card_24ac_priv_t *p = card->priv;
    return p ? &p->display : NULL;
}

static const char *card_name(const nubus_card_t *card) {
    (void)card;
    return "Apple Macintosh Display Card 24AC";
}

static const nubus_card_ops_t display_card_24ac_ops = {
    .init = card_init,
    .teardown = card_teardown,
    .on_vbl = card_on_vbl,
    .display = card_display,
    .name = card_name,
};

// === Factory + kind descriptor ==============================================

static nubus_card_t *factory(int slot, config_t *cfg, checkpoint_t *cp) {
    nubus_card_t *card = calloc(1, sizeof(*card));
    if (!card)
        return NULL;
    card->ops = &display_card_24ac_ops;
    card->slot = slot;
    if (card->ops->init(card, cfg, cp) != 0) {
        free(card);
        return NULL;
    }
    return card;
}

// Advertised modes (vrom identity strings, proposal §3.1): 640×480,
// 800×600, 832×624.  All are the multisync monitor family — primary sense
// code 6 plus a per-mode extended-sense code that the vrom video driver
// reads back from the SENSE_CLK line (0xD8000D); the resulting top-level
// "sister" sResource ids are 0x6B / 0x6C / 0x6D (see the vrom RE doc and the
// stateful sense model in reg_read).  The card is a 24-bit colour board (4 MB
// VRAM), so every mode supports 1/4/8/16/32 bpp — there is NO 2-bpp mode (vrom
// RE depth ladder).  The default-sensed monitor is the 640×480 multisync
// (sense_primary 6, ext 0x03 → sRsrc 0x6B).
static const int display_card_24ac_depths[] = {1, 4, 8, 16, 32, 0};
static const nubus_monitor_t display_card_24ac_monitors[] = {
    {.id = "rgb_640x480",
     .name = "640 × 480 (67 Hz)",
     .width = 640,
     .height = 480,
     .depths = display_card_24ac_depths,
     .sense_code = 6,
     .srsrc_sister = 0x6B},
    {.id = "rgb_800x600",
     .name = "800 × 600 (60 Hz)",
     .width = 800,
     .height = 600,
     .depths = display_card_24ac_depths,
     .sense_code = 6,
     .srsrc_sister = 0x6C},
    {.id = "rgb_832x624",
     .name = "832 × 624 (75 Hz)",
     .width = 832,
     .height = 624,
     .depths = display_card_24ac_depths,
     .sense_code = 6,
     .srsrc_sister = 0x6D},
    {0},
};

const nubus_card_kind_t display_card_24ac_kind = {
    .id = "display_card_24ac",
    .display_name = "Apple Macintosh Display Card 24AC",
    .requires_vrom = true,
    .monitors = display_card_24ac_monitors,
    .factory = factory,
};

// === Video-mode selection (machine.nubus.video_mode) ========================

void display_card_24ac_pending_video_mode_set(const char *id) {
    if (!id || !*id) {
        s_pending_video_mode_id[0] = '\0';
        return;
    }
    snprintf(s_pending_video_mode_id, sizeof s_pending_video_mode_id, "%s", id);
}

const char *display_card_24ac_pending_video_mode_get(void) {
    return s_pending_video_mode_id[0] ? s_pending_video_mode_id : NULL;
}

// Parse "<monitor>_<N>bpp" (e.g. "rgb_640x480_8bpp") into (monitor, N): the
// monitor name is matched against display_card_24ac_monitors[] and N validated
// against that monitor's depth list.  Mirrors jmfb_video_mode_lookup.
bool display_card_24ac_video_mode_lookup(const char *id, const nubus_monitor_t **out_monitor, int *out_depth_bpp) {
    if (!id || !*id)
        return false;
    const char *underscore_bpp = strrchr(id, '_');
    if (!underscore_bpp)
        return false;
    size_t mon_len = (size_t)(underscore_bpp - id);
    if (mon_len == 0 || mon_len >= 32)
        return false;
    char mon_id[32];
    memcpy(mon_id, id, mon_len);
    mon_id[mon_len] = '\0';
    const char *bpp_str = underscore_bpp + 1;
    char *end = NULL;
    long bpp = strtol(bpp_str, &end, 10);
    if (!end || end == bpp_str || strcmp(end, "bpp") != 0)
        return false;
    if (bpp < 1 || bpp > 32)
        return false;
    for (const nubus_monitor_t *m = display_card_24ac_monitors; m->id; m++) {
        if (strcmp(m->id, mon_id) != 0)
            continue;
        if (!m->depths)
            return false;
        for (const int *d = m->depths; *d; d++) {
            if ((int)bpp == *d) {
                if (out_monitor)
                    *out_monitor = m;
                if (out_depth_bpp)
                    *out_depth_bpp = (int)bpp;
                return true;
            }
        }
        return false; // monitor matched but depth didn't
    }
    return false;
}

// === Engine introspection (object model) ====================================

bool display_card_24ac_is_card(const nubus_card_t *card) {
    return card && card->ops == &display_card_24ac_ops;
}

bool display_card_24ac_engine_enabled(const nubus_card_t *card) {
    if (!display_card_24ac_is_card(card))
        return false;
    const display_card_24ac_priv_t *p = card->priv;
    return p && p->engine_enabled;
}

void display_card_24ac_engine_set_enabled(nubus_card_t *card, bool enabled) {
    if (!display_card_24ac_is_card(card))
        return;
    display_card_24ac_priv_t *p = card->priv;
    if (p)
        p->engine_enabled = enabled;
}

uint8_t display_card_24ac_engine_mode(const nubus_card_t *card) {
    if (!display_card_24ac_is_card(card))
        return 0;
    const display_card_24ac_priv_t *p = card->priv;
    return p ? p->engine_mode : 0;
}

uint32_t display_card_24ac_engine_operand(const nubus_card_t *card) {
    if (!display_card_24ac_is_card(card))
        return 0;
    const display_card_24ac_priv_t *p = card->priv;
    return p ? p->engine_operand : 0;
}
