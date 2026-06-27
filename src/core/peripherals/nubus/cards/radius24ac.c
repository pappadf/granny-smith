// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// radius24ac.c
// "Apple Macintosh 24AC" — Radius "Boogie" 24-bit colour NuBus display
// card with a hardware QuickDraw fill/raster accelerator.  See
// proposal-nubus-card-radius-24ac.md and the dossier under
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
//     tmp/24ac-vrom-re.md / radius24ac.h).
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

#include "radius24ac.h"

#include "card.h"
#include "checkpoint.h"
#include "declrom.h"
#include "display.h"
#include "log.h"
#include "memory.h"
#include "nubus.h"
#include "system.h"
#include "system_config.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("radius24ac");

// === Per-card private state =================================================

typedef struct radius24ac_priv radius24ac_priv_t;

// A device-region callback context: binds the card to the slot-relative
// base offset of the region it was registered for, so a single
// memory_interface_t can serve every register/engine region and dispatch
// on the full slot offset (region_off + region-relative addr).
typedef struct {
    radius24ac_priv_t *p;
    uint32_t region_off; // slot-relative base of this region
} reg_ctx_t;

struct radius24ac_priv {
    nubus_card_t *card; // back-pointer for IRQ helpers
    uint8_t *vram; // RADIUS24AC_VRAM_SIZE
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
    uint8_t sense_raw; // raw byte 0xD8000D reads back (monitor sense)
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

// The engine's STATUS[2:0] depth code matching a display format (the cdev
// indexes its stride table by this).  Kept consistent with what we present.
static uint8_t depth_code_for_format(pixel_format_t f) {
    switch (f) {
    case PIXEL_1BPP_MSB:
        return 0;
    case PIXEL_2BPP_MSB:
        return 1;
    case PIXEL_4BPP_MSB:
        return 2;
    case PIXEL_8BPP:
        return 3;
    case PIXEL_16BPP_555:
        return 4;
    case PIXEL_32BPP_XRGB:
        return 5;
    default:
        return 3;
    }
}

// rowBytes for a flat framebuffer of the current width + depth.  The 24AC
// has no RowWords register; the driver lays VRAM out tightly, so stride =
// width × bpp / 8.
static void recompute_stride(radius24ac_priv_t *p) {
    uint32_t bpp = format_bpp(p->display.format);
    p->display.stride = (p->display.width * bpp + 7u) / 8u;
}

// Apply the depth selected by the MODE register's top three bits.  The
// observed depth table (vrom RE) maps MODE bits 7-5 → 1/2/4/8/16 bpp;
// hi-res / direct modes (other values) keep the current format.
static void apply_mode_depth(radius24ac_priv_t *p, uint8_t mode_byte) {
    pixel_format_t f = p->display.format;
    switch (mode_byte & 0xE0u) {
    case 0x00u:
        f = PIXEL_1BPP_MSB;
        break;
    case 0x20u:
        f = PIXEL_2BPP_MSB;
        break;
    case 0x40u:
        f = PIXEL_4BPP_MSB;
        break;
    case 0xA0u:
        f = PIXEL_8BPP;
        break;
    case 0xC0u:
        f = PIXEL_16BPP_555;
        break;
    default:
        break; // hi-res / direct — leave format unchanged
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
static void clut_set_index(radius24ac_priv_t *p, uint8_t idx) {
    p->clut_idx = idx;
    p->clut_phase = 0;
}

// Data-register write: accumulate R, G, B over three writes, commit, and
// auto-increment the index for a run-write.
static void clut_write_data(radius24ac_priv_t *p, uint8_t comp) {
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
static void engine_fill_run(radius24ac_priv_t *p, uint32_t dest, uint32_t len) {
    if (dest >= RADIUS24AC_VRAM_SIZE)
        return;
    if (len > RADIUS24AC_VRAM_SIZE - dest)
        len = RADIUS24AC_VRAM_SIZE - dest; // clamp to VRAM
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
static void engine_store_long(radius24ac_priv_t *p, uint32_t dest, uint32_t src) {
    if (dest + 4 > RADIUS24AC_VRAM_SIZE)
        return; // out of VRAM — drop (driver never streams past the bank)
    uint32_t out;
    switch (p->engine_mode) {
    case RADIUS24AC_MODE_COPY:
    case RADIUS24AC_MODE_STRETCH:
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

static uint32_t reg_read(radius24ac_priv_t *p, uint32_t off, unsigned width) {
    switch (off) {
    // --- Display side -------------------------------------------------------
    case RADIUS24AC_STATUS_OFFSET: {
        // STATUS byte: [2:0] depth (cdev), [3] class, [4] busy/sync toggle.
        // Toggle bit 4 each read so the driver's CLUT-safe / VBL-sync poll
        // always sees both edges and exits (mirrors jmfb's VBL toggle).
        p->status_busy ^= RADIUS24AC_STATUS_BUSY;
        return (uint32_t)((p->status_depth_code & 7u) | (p->status_class_bit ? 0x08u : 0x00u) | p->status_busy);
    }
    case RADIUS24AC_VIDCTL_OFFSET:
        return p->vidctl;
    case RADIUS24AC_MODE_REG:
        return p->mode_reg;
    case RADIUS24AC_DEPTH_REG:
        return p->depth_reg;
    case RADIUS24AC_SENSE_CLK:
        // Monitor sense: the driver drives the lines, then reads back; bits
        // 7-5 are the (inverted) sense pins.  Return the configured raw byte.
        return p->sense_raw;
    // --- Engine side --------------------------------------------------------
    case RADIUS24AC_CONFIG_OFFSET:
        return (uint32_t)(p->config_variant_bit ? 0x01u : 0x00u);
    case RADIUS24AC_OPERAND_APERTURE:
        return p->engine_operand; // driver's 1-entry pattern cache read-back
    default:
        break;
    }
    // Top-of-bank carve-out [operand aperture .. active alias): the VRAM host
    // region stops just below the operand aperture (so the aperture page is a
    // pure device region, not shadowed by the MMU VRAM slot).  Every byte here
    // other than the operand longword is still plain passive VRAM, so serve it
    // from p->vram — otherwise this range would be an unmapped hole.
    if (off >= RADIUS24AC_OPERAND_APERTURE && off < RADIUS24AC_ENGINE_ALIAS_OFFSET) {
        if (width == 4 && off + 4 <= RADIUS24AC_VRAM_SIZE)
            return LOAD_BE32(p->vram + off);
        if (off < RADIUS24AC_VRAM_SIZE)
            return p->vram[off];
        return 0;
    }
    // Active-bank alias reads are just the underlying passive VRAM.
    if (off >= RADIUS24AC_ENGINE_ALIAS_OFFSET && off < RADIUS24AC_ENGINE_ALIAS_OFFSET + RADIUS24AC_VRAM_SIZE) {
        uint32_t dest = off - RADIUS24AC_ENGINE_ALIAS_OFFSET;
        if (width == 4 && dest + 4 <= RADIUS24AC_VRAM_SIZE)
            return LOAD_BE32(p->vram + dest);
        if (dest < RADIUS24AC_VRAM_SIZE)
            return p->vram[dest];
        return 0;
    }
    LOG(3, "unmodeled read off $%06x width %u", off, width);
    return 0;
}

static void reg_write(radius24ac_priv_t *p, uint32_t off, uint32_t val, unsigned width) {
    switch (off) {
    // --- CLUT / RAMDAC ------------------------------------------------------
    case RADIUS24AC_CLUT0_ADDR:
    case RADIUS24AC_CLUT_ADDR:
        clut_set_index(p, (uint8_t)val);
        return;
    case RADIUS24AC_CLUT0_DATA:
    case RADIUS24AC_CLUT_DATA:
        clut_write_data(p, (uint8_t)val);
        return;
    case RADIUS24AC_RAMDAC_CMD:
        LOG(3, "RAMDAC command = $%02x (accept-and-log)", (uint8_t)val);
        return;
    case RADIUS24AC_CLUT_CTL:
        LOG(3, "CLUT control strobe = $%02x (accept-and-log)", (uint8_t)val);
        return;
    // --- Display control ----------------------------------------------------
    case RADIUS24AC_VIDCTL_OFFSET:
        // Central video latch.  Bit 7 = slot-VBL-IRQ mask (1 = masked).
        // The driver enables by clearing bit 7, disables by setting it, and
        // the ISR pulses set→clear to acknowledge the pending IRQ — so a set
        // bit 7 always means "release the line now" (deassert), and a clear
        // bit 7 re-arms it for the next VBL.
        p->vidctl = (uint8_t)val;
        if (val & RADIUS24AC_VIDCTL_VBL_MASK) {
            p->vbl_enabled = false;
            nubus_deassert_irq(p->card);
        } else {
            p->vbl_enabled = true;
        }
        return;
    case RADIUS24AC_MODE_REG:
        p->mode_reg = (uint8_t)val;
        apply_mode_depth(p, (uint8_t)val);
        return;
    case RADIUS24AC_DEPTH_REG:
        p->depth_reg = (uint8_t)val;
        return;
    case RADIUS24AC_SENSE_CLK:
        // Serial PLL clock/data bit-bang + sense-line drive; no display
        // state depends on the programmed clock, so accept-and-log.
        LOG(3, "SENSE_CLK write $%02x (PLL/sense, accept-and-log)", (uint8_t)val);
        return;
    // --- Engine -------------------------------------------------------------
    case RADIUS24AC_CONTROL_OFFSET:
        p->engine_mode = (uint8_t)val; // latch op mode for active-bank writes
        LOG(3, "engine: CONTROL = $%02x", p->engine_mode);
        return;
    case RADIUS24AC_OPERAND_APERTURE:
        if (width == 4)
            p->engine_operand = val;
        LOG(3, "engine: operand load = $%08x", p->engine_operand);
        return;
    default:
        break;
    }
    // CRTC timing register file (write-only) — accept-and-log.
    if (off >= RADIUS24AC_CRTC_LO && off <= RADIUS24AC_CRTC_HI) {
        LOG(3, "CRTC[$%06x] = $%02x (accept-and-log)", off, (uint8_t)val);
        return;
    }
    // Top-of-bank carve-out [operand aperture .. active alias): the operand
    // longword is intercepted above; every other byte is plain passive VRAM
    // (the VRAM host region stops below the aperture — see reg_read).
    if (off >= RADIUS24AC_OPERAND_APERTURE && off < RADIUS24AC_ENGINE_ALIAS_OFFSET) {
        if (width == 4 && off + 4 <= RADIUS24AC_VRAM_SIZE)
            STORE_BE32(p->vram + off, val);
        else if (off < RADIUS24AC_VRAM_SIZE)
            p->vram[off] = (uint8_t)val;
        p->display.fb_dirty = true;
        return;
    }
    // Active-bank alias (engine-transforming): off ∈ [0x400000, +VRAM).
    if (off >= RADIUS24AC_ENGINE_ALIAS_OFFSET && off < RADIUS24AC_ENGINE_ALIAS_OFFSET + RADIUS24AC_VRAM_SIZE) {
        uint32_t dest = off - RADIUS24AC_ENGINE_ALIAS_OFFSET;
        // The operand aperture's +0x400000 alias is the commit window: a
        // write of `4` here latches the loaded operand (driver writes twice).
        if (dest == RADIUS24AC_OPERAND_APERTURE) {
            if (val == RADIUS24AC_COMMIT_CMD)
                LOG(3, "engine: operand commit ($%08x)", p->engine_operand);
            return;
        }
        if (!p->engine_enabled || width != 4) {
            // Oracle mode (engine off) or a narrow write the driver never
            // issues through the active bank — behave as plain VRAM so
            // nothing bus-errors and the model can be compared to the
            // driver's software fallback.
            if (width == 4 && dest + 4 <= RADIUS24AC_VRAM_SIZE)
                STORE_BE32(p->vram + dest, val);
            else if (dest < RADIUS24AC_VRAM_SIZE)
                p->vram[dest] = (uint8_t)val;
            p->display.fb_dirty = true;
            return;
        }
        if (p->engine_mode == RADIUS24AC_MODE_FILL)
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

static memory_interface_t s_radius24ac_mem_iface = {
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
static bool load_vrom(radius24ac_priv_t *p) {
    char *path = NULL;
    if (!declrom_load_vrom("display-card-24ac.vrom", RADIUS24AC_DECLROM_CHIP_SIZE, p->vrom, RADIUS24AC_DECLROM_BUS_SIZE,
                           &path))
        return false;
    free(p->vrom_path);
    p->vrom_path = path;
    p->vrom_size = RADIUS24AC_DECLROM_BUS_SIZE;
    return true;
}

// === Card vtable ============================================================

static int card_init(nubus_card_t *card, config_t *cfg, checkpoint_t *cp) {
    (void)cp;
    radius24ac_priv_t *p = calloc(1, sizeof(*p));
    if (!p)
        return -1;
    p->card = card;
    p->slot_base = nubus_slot_base(card->slot);

    p->vram = calloc(1, RADIUS24AC_VRAM_SIZE);
    p->vrom = calloc(1, RADIUS24AC_DECLROM_BUS_SIZE);
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

    // Phase-1 starting state: 8 bpp, 640×480, framebuffer at VRAM offset 0.
    // The vrom's video driver re-programs depth/CLUT/timing at boot.
    p->vidctl = RADIUS24AC_VIDCTL_VBL_MASK; // VBL IRQ starts masked
    p->vbl_enabled = false;
    p->mode_reg = 0xA0u; // 8 bpp depth code in bits 7-5
    p->sense_raw = 0xE0u; // monitor-sense readback (primary 0 → default mode)

    // Engine geometry bits, kept consistent with the large-VRAM framebuffer
    // we present (hardware spec §3/§5).
    p->engine_enabled = true;
    p->engine_mode = RADIUS24AC_MODE_COPY;
    p->status_depth_code = depth_code_for_format(PIXEL_8BPP);
    p->status_class_bit = true; // large-VRAM organisation
    p->config_variant_bit = true; // large-VRAM geometry variant

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
    // The board-config scratch at 0xFFFD8 is left inside the VRAM aperture
    // (it sits above any framebuffer) and reads back what the driver wrote —
    // the simplest faithful model for a scratch block (see RE doc).
    //
    // The VRAM host region stops just below the operand aperture
    // (0x3FE000): the MMU models VRAM as one contiguous range whose
    // translation is re-filled on every _SwapMMUMode TLB flush (PrimaryInit
    // does thousands), which would otherwise SHADOW the operand-aperture
    // device page that lives inside the passive bank.  The JMFB sidesteps
    // this by putting its registers above its VRAM; the 24AC's aperture is
    // genuinely inside the bank, so we carve the bank off at the aperture.
    // The framebuffer (≤ a few MB) sits well below 0x3FE000, so nothing
    // visible is lost; the engine still reaches the full p->vram buffer
    // directly through the active-bank alias.
    memory_map_host_region(cfg->mem_map, "radius24ac_vram", p->vram, p->slot_base, RADIUS24AC_OPERAND_APERTURE,
                           /*writable*/ true);
    memory_map_host_region(cfg->mem_map, "radius24ac_declrom", p->vrom, p->slot_base + RADIUS24AC_DECLROM_BUS_OFFSET,
                           RADIUS24AC_DECLROM_BUS_SIZE, /*writable*/ false);

    // Register/engine regions share the one dispatcher; each region's
    // reg_ctx carries its slot-relative base so reg_read/reg_write see full
    // slot offsets.  Registered AFTER the VRAM host region so the
    // operand-aperture page overlays it (last registration wins per page).
    p->ctx_clut = (reg_ctx_t){.p = p, .region_off = RADIUS24AC_CLUT_PAGE};
    p->ctx_d00 = (reg_ctx_t){.p = p, .region_off = RADIUS24AC_D00_PAGE};
    p->ctx_d40 = (reg_ctx_t){.p = p, .region_off = RADIUS24AC_D40_PAGE};
    p->ctx_d80 = (reg_ctx_t){.p = p, .region_off = RADIUS24AC_D80_PAGE};
    p->ctx_operand = (reg_ctx_t){.p = p, .region_off = RADIUS24AC_OPERAND_APERTURE};
    p->ctx_active = (reg_ctx_t){.p = p, .region_off = RADIUS24AC_ENGINE_ALIAS_OFFSET};

    memory_map_add(cfg->mem_map, p->slot_base + RADIUS24AC_CLUT_PAGE, MEM_PAGE_SIZE, "radius24ac_clut",
                   &s_radius24ac_mem_iface, &p->ctx_clut);
    memory_map_add(cfg->mem_map, p->slot_base + RADIUS24AC_D00_PAGE, MEM_PAGE_SIZE, "radius24ac_d00",
                   &s_radius24ac_mem_iface, &p->ctx_d00);
    memory_map_add(cfg->mem_map, p->slot_base + RADIUS24AC_D40_PAGE, MEM_PAGE_SIZE, "radius24ac_d40",
                   &s_radius24ac_mem_iface, &p->ctx_d40);
    memory_map_add(cfg->mem_map, p->slot_base + RADIUS24AC_D80_PAGE, MEM_PAGE_SIZE, "radius24ac_d80",
                   &s_radius24ac_mem_iface, &p->ctx_d80);
    // Operand aperture + the carved-off top of the passive bank
    // [0x3FE000, 0x400000): the VRAM host region stops at the aperture, so
    // this device region both intercepts the operand longword and serves the
    // remaining passive-VRAM bytes (reg_read/reg_write fall through to
    // p->vram), leaving no unmapped hole below the active alias.
    memory_map_add(cfg->mem_map, p->slot_base + RADIUS24AC_OPERAND_APERTURE,
                   RADIUS24AC_ENGINE_ALIAS_OFFSET - RADIUS24AC_OPERAND_APERTURE, "radius24ac_operand",
                   &s_radius24ac_mem_iface, &p->ctx_operand);
    // Active-bank alias: the engine-transforming mirror of the passive bank
    // (covers the operand commit window at +0x400000 too).
    memory_map_add(cfg->mem_map, p->slot_base + RADIUS24AC_ENGINE_ALIAS_OFFSET, RADIUS24AC_VRAM_SIZE,
                   "radius24ac_engine", &s_radius24ac_mem_iface, &p->ctx_active);

    return 0;
}

static void card_teardown(nubus_card_t *card, config_t *cfg) {
    (void)cfg;
    radius24ac_priv_t *p = card->priv;
    if (!p)
        return;
    free(p->vram);
    free(p->vrom);
    free(p->vrom_path);
    free(p);
    card->priv = NULL;
}

static void card_on_vbl(nubus_card_t *card, config_t *cfg) {
    (void)cfg;
    radius24ac_priv_t *p = card->priv;
    if (!p)
        return;
    if (p->vbl_enabled)
        nubus_assert_irq(card);
    // CPU writes to VRAM go straight through the host_region mapping and
    // don't notify the renderer; this VBL pulse surfaces ongoing drawing.
    p->display.fb_dirty = true;
}

static display_t *card_display(nubus_card_t *card) {
    radius24ac_priv_t *p = card->priv;
    return p ? &p->display : NULL;
}

static const char *card_name(const nubus_card_t *card) {
    (void)card;
    return "Apple Macintosh 24AC";
}

static const nubus_card_ops_t radius_24ac_ops = {
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
    card->ops = &radius_24ac_ops;
    card->slot = slot;
    if (card->ops->init(card, cfg, cp) != 0) {
        free(card);
        return NULL;
    }
    return card;
}

// Advertised modes (from the vrom mode strings).  Sense codes / sister IDs
// are not yet pinned to specific pin patterns (proposal §6 / RE doc) — start
// with the default 640×480 mode to reach a desktop, then fill the table
// from observation.
static const int radius_24ac_depths[] = {1, 2, 4, 8, 0};
static const nubus_monitor_t radius_24ac_monitors[] = {
    {.id = "rgb_640x480",
     .name = "640 × 480 (60 Hz)",
     .width = 640,
     .height = 480,
     .depths = radius_24ac_depths,
     .sense_code = 0x0,
     .srsrc_sister = 0x00},
    {0},
};

const nubus_card_kind_t radius_24ac_kind = {
    .id = "radius_24ac",
    .display_name = "Apple Macintosh 24AC",
    .requires_vrom = true,
    .monitors = radius_24ac_monitors,
    .factory = factory,
};
