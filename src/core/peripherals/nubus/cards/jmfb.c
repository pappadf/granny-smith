// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// jmfb.c
// Apple Macintosh Display Card 8•24 (Rev B, ROM `341-0868`).  See
// proposal-machine-iicx-iix.md §3.2.5 + jmfb.h for the contract.
//
// Implementation status (proposal step 6, minimum-viable):
//   * Card factory loads `Apple-341-0868.vrom` and registers VRAM,
//     declrom, and the register window on the bus.
//   * I/O dispatcher in this file handles all four register blocks at
//     a single switch.  Modelled handlers cover the registers the
//     Monitors-control-panel boot path depends on; everything else is
//     accept-and-log so an OS write that should be a no-op doesn't
//     turn into a bus error.
//   * Sense lines satisfy the JMFB PrimaryInit read so the OS picks
//     up the user's `monitor=` choice.
//   * CLUT writes feed display.clut and bump generation; depth changes
//     via CLUTPBCR feed display.format.
//   * Mode-table parsing inside `Apple-341-0868.vrom` is left to the
//     System 7 driver — we present the bytes; it walks them.

#include "jmfb.h"

#include "card.h"
#include "checkpoint.h"
#include "declrom.h"
#include "display.h"
#include "log.h"
#include "memory.h"
#include "nubus.h"
#include "rom.h"
#include "system.h"
#include "system_config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("jmfb");

// === Forward declarations ===================================================
//
// The monitor list `mdc_8_24_monitors` and the sense→monitor lookup
// live near the bottom of this file (next to the per-card kind
// descriptor that references the list); the JMFB factory body
// further up needs to reach them.  Forward declarations here let the
// factory call `monitor_for_sense` and read `s_pending_sense` /
// `s_pending_sense_set` without reshuffling the file.
typedef struct nubus_monitor nubus_monitor_t_fwd;
static const struct nubus_monitor *monitor_for_sense(uint8_t sense);

// Pending sense code consumed by the next JMFB factory call.  Set
// from the shell via `nubus.video_sense = N` before `machine.boot`;
// reset to the default ($6 = 13" RGB) on consumption so a forgotten
// configuration doesn't leak across machine reinitialisations.
static uint8_t s_pending_sense = 0x6;
static bool s_pending_sense_set = false;

// === Per-card private state =================================================

typedef struct {
    nubus_card_t *card; // back-pointer for IRQ helpers
    uint8_t *vram; // 2 MB
    uint8_t *vrom; // 32 KB declaration ROM bytes
    char *vrom_path; // path the VROM was loaded from
    uint32_t vrom_size; // typically 32 KB; 0 if no VROM loaded
    uint32_t slot_base; // physical bus base (== nubus_slot_base(slot))
    rgba8_t clut[256];
    display_t display;

    // RAMDAC sub-state for CLUT writes — three sequential writes to
    // CLUTDataReg load R, G, B for the indexed palette entry.  After the
    // third write, the palette index auto-increments.
    uint8_t clut_idx; // current palette index
    uint8_t clut_phase; // 0 = R, 1 = G, 2 = B; resets on CLUTAddrReg write
    rgba8_t clut_pending; // R/G partial values waiting for the B write

    // Register-file scratch.  All accept-and-log registers stash their last
    // value here so the inspector can peek at the chip's effective state.
    uint16_t jmfb_csr;
    uint16_t jmfb_lsr;
    uint16_t jmfb_video_base; // raw value; offset = value * 32 bytes
    uint16_t jmfb_row_words; // raw value; stride = value * 4 (≤8bpp) or *32/3 (24bpp)
    uint16_t sw_ic_reg; // SRST | ENVERTI | …
    uint16_t sw_status_reg;
    uint16_t clut_pbcr;
    uint16_t endeavor_m;
    uint16_t endeavor_n;
    uint16_t endeavor_ext_clk;
    uint16_t endeavor_reserved;

    // Sense-line state — set from the user's monitor choice via the
    // bus controller.  Default 13" RGB (sense `110` → bits 9..11 of
    // JMFBCSR = 0x0C00).
    uint8_t sense_code;
} jmfb_priv_t;

// === Helpers ================================================================

// Map the ≤8 bpp depth field in CLUTPBCR (bits 3-4) to a pixel_format_t.
static pixel_format_t depth_to_format(uint16_t pbcr) {
    switch ((pbcr >> 3) & 0x3) {
    case 0:
        return PIXEL_1BPP_MSB;
    case 1:
        return PIXEL_2BPP_MSB;
    case 2:
        return PIXEL_4BPP_MSB;
    case 3:
    default:
        return PIXEL_8BPP;
    }
    // 24 bpp on the 8·24 is depth=3 + bit-1 set; the System 7 driver only
    // toggles bit 1 once it's already in 8bpp, so the depth_to_format
    // result above is the right starting point.  The 24bpp upgrade is a
    // separate `pbcr & 0x2` test in the write handler.
}

// Recompute display.stride from JMFBRowWords + current format.  The Apple
// driver clears JMFBRowWords to 0 during chip-reset sequences before
// programming the real value; treating raw 0 as stride=0 zeros our
// display and breaks the renderer / PNG / checksum paths.  Keep the
// previous stride value across 0 writes — the next non-zero write fixes
// it.
// Bits-per-pixel for the current display format.  24-bit-direct counts
// as 24 (not 32) because the framebuffer stores packed RGB triples in
// the JMFB's RAMDAC-bypass mode.
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
        return 24; // packed-pixel direct mode
    default:
        return 8; // safe fallback
    }
}

// Recompute display.stride AND display.width every time row_words or
// the pixel format changes.  Width is a pure function of (row_words,
// bpp): row_words counts longs-per-row for ≤8 bpp and 32/3-byte units
// for 24 bpp packed.  Height is a property of the chosen monitor,
// fixed at JMFB factory time from sense_code, and not derived here.
static void recompute_stride(jmfb_priv_t *p) {
    if (p->jmfb_row_words == 0)
        return; // chip-reset sentinel; preserve last good stride+width
    if (p->display.format == PIXEL_32BPP_XRGB) {
        p->display.stride = (uint32_t)p->jmfb_row_words * 32u / 3u;
        // 24bpp packed: stride bytes / 3 = pixels
        p->display.width = p->display.stride / 3u;
    } else {
        p->display.stride = (uint32_t)p->jmfb_row_words * 4u;
        // ≤8 bpp: width = (stride bytes * 8) / bpp = row_words * 32 / bpp
        uint32_t bpp = format_bpp(p->display.format);
        if (bpp > 0)
            p->display.width = (uint32_t)p->jmfb_row_words * 32u / bpp;
    }
}

// === Memory interface (register window I/O) =================================

// Translate the relative address handed to our I/O dispatcher (relative
// to slot_base + JMFB_BLOCK_OFFSET — i.e. the address `memory_map_add`
// passes back to its dev callback) into a (block, in-block-offset) pair.
// Returns -1 if the offset is outside the four-block register window.
static int classify(uint32_t rel_addr, uint32_t slot_base, uint32_t *out_off) {
    (void)slot_base; // kept in the signature to clarify the convention; unused
    if (rel_addr >= 0x400u)
        return -1;
    *out_off = rel_addr & 0xFFu; // each of the four blocks is 256 bytes wide
    return (int)(rel_addr >> 8); // 0=JMFB, 1=Stopwatch, 2=CLUT, 3=Endeavor
}

// JMFBLSR / JMFBVideoBase / JMFBRowWords are 16-bit registers that
// occupy the LOW half of a 32-bit-aligned slot in the JMFB block —
// Apple's bus convention for half-word registers in long-aligned slot
// space.  When the driver writes them with `move.l #N, (slot)`, our
// io_write32 splits into two 16-bit halves: io_write16(slot, hi=0)
// and io_write16(slot+2, lo=N).  The meaningful value lands at
// slot+2; slot is a no-op write of zero.  Likewise for reads — slot+2
// returns the register value, slot returns zero (high half of long).
//
// The .h offsets keep Apple's spec naming (slot offset).  The handler
// dispatches on slot+2 for the actual data, and accepts (no-op-style)
// writes to slot for the high-half pass of a 32-bit access.  Without
// this split the OS's `move.l #$50, (JMFBVideoBase)` clobbered
// jmfb_video_base to 0 (the high-half write hit case JMFBVideoBase
// while the meaningful $50 fell through to the unmodeled default at
// slot+2), pointing display.bits at VRAM+0 instead of VRAM+$A00 and
// shifting the rendered framebuffer ~32 rows up.
static void handle_jmfb_write16(jmfb_priv_t *p, uint32_t off, uint16_t val) {
    switch (off) {
    case JMFBCSR:
        // High 16 bits of the 32-bit CSR.  No documented soft-controlled
        // bits modelled here — accept-and-ignore.
        return;
    case JMFBCSR + 2:
        // Low 16 bits — software-writable control bits live here.
        p->jmfb_csr = (val & ~MaskSenseLine) | (p->jmfb_csr & MaskSenseLine);
        if (val & VRSTB) {
            // Master reset clears software-controlled bits and stops the
            // RAMDAC sub-counter.  Sense lines are a hardware property and
            // are NOT cleared.
            p->jmfb_csr &= MaskSenseLine;
            p->clut_phase = 0;
            LOG(2, "JMFBCSR: VRSTB master reset");
        }
        if (val & REFEN)
            LOG(3, "JMFBCSR: REFEN set");
        if (val & VIDGO)
            LOG(3, "JMFBCSR: VIDGO set (video transfer enabled)");
        return;
    case JMFBLSR:
    case JMFBVideoBase:
    case JMFBRowWords:
        // High half of a long write — bus discards.
        return;
    case JMFBLSR + 2:
        p->jmfb_lsr = val;
        LOG(3, "JMFBLSR write %04x (accept-and-log)", val);
        return;
    case JMFBVideoBase + 2:
        p->jmfb_video_base = val;
        // Encoded value × 32 = byte offset into VRAM
        p->display.bits = p->vram + ((size_t)val * 32u);
        p->display.generation++;
        return;
    case JMFBRowWords + 2:
        p->jmfb_row_words = val;
        recompute_stride(p);
        p->display.generation++;
        return;
    default:
        LOG(2, "JMFB block write at +%02x = %04x (unmodeled)", off, val);
        return;
    }
}

static uint16_t handle_jmfb_read16(jmfb_priv_t *p, uint32_t off) {
    switch (off) {
    case JMFBCSR:
        // JMFBCSR is a 32-bit register at offset $00.  Apple's PrimaryInit
        // reads sense via `BFEXTU (A1,D1.L){20:3},D4` — bits 20..22 of the
        // 32-bit memory value, which is the LOW 16-bit half (bits 9..11
        // LSB-numbered).  So the sense lines live at offset $02 (low
        // half), and offset $00 returns the high half of the CSR.
        // The high half currently has no documented soft-controlled bits
        // we model — return 0.
        return 0;
    case JMFBCSR + 2:
        // Low 16 bits of the 32-bit CSR.  Sense lines occupy bits 9-11
        // (NOT in MaskSenseLine); other bits are software-writable from
        // the JMFBCSR write path.
        return (uint16_t)((p->jmfb_csr & MaskSenseLine) | ((p->sense_code & 7) << 9));
    case JMFBLSR:
    case JMFBVideoBase:
    case JMFBRowWords:
        // High half of a long read — bus drives zero.
        return 0;
    case JMFBLSR + 2:
        return p->jmfb_lsr;
    case JMFBVideoBase + 2:
        return p->jmfb_video_base;
    case JMFBRowWords + 2:
        return p->jmfb_row_words;
    default:
        LOG(2, "JMFB block read at +%02x (unmodeled, returning 0)", off);
        return 0;
    }
}

// Stopwatch block registers (SWICReg / SWClrVInt / SWStatusReg) follow
// the same 16-bit-in-32-bit-slot bus convention as the JMFB block —
// meaningful value lands at slot+2 of the long-word slot, slot+0 is
// the high half (zero on write, zero on read).  SWStatusReg's +2 read
// path was already wired correctly because the Apple driver's
// `BFEXTU (A0){#$1D:#$1}` test made the convention obvious there;
// SWICReg / SWClrVInt writes were silently lost on long-write paths
// before this fix.
static void handle_stopwatch_write16(jmfb_priv_t *p, uint32_t off, uint16_t val) {
    switch (off) {
    case SWICReg:
    case SWClrVInt:
    case SWStatusReg:
        return; // high half of long write — bus discards
    case SWICReg + 2:
        p->sw_ic_reg = val;
        if (val & SRST)
            LOG(2, "SWICReg: soft reset");
        // ENVERTI: 1 = vertical interrupts enabled.  Card decides whether
        // to call nubus_assert_irq based on this in on_vbl.
        return;
    case SWClrVInt + 2:
        // Write any value clears the pending VBL and de-asserts the
        // slot's IRQ on the bus controller.
        nubus_deassert_irq(p->card);
        return;
    case SWStatusReg + 2:
        // Status register is technically read-mostly; the System 7
        // driver writes here to clear bits.  Accept-and-log.
        p->sw_status_reg = val;
        return;
    default:
        LOG(2, "Stopwatch block write at +%02x = %04x (unmodeled)", off, val);
        return;
    }
}

static uint16_t handle_stopwatch_read16(jmfb_priv_t *p, uint32_t off) {
    switch (off) {
    case SWStatusReg:
        // Top half of the 32-bit Stopwatch status word.  Real hardware
        // exposes the VBL toggle bit at *long-word* bit 2 (= byte $C3
        // bit 2, big-endian).  The Apple driver polls that bit via
        // BFEXTU (A0){#$1D:#$1} — see the JMFB PrimaryInit code.  The
        // toggle is implemented in the +$C2 read path below; this top-
        // half read is a stable 0 (the chip's status flags live at the
        // bottom of the long).
        return 0;
    case SWStatusReg + 2:
        // Bottom half of the 32-bit Stopwatch status word — bit 2 of
        // this 16-bit value is the VBL toggle the OS polls for.  Flip
        // it on every read so the OS sees both edges.
        p->sw_status_reg ^= 0x0004u;
        return p->sw_status_reg & 0x0004u;
    case SWICReg:
        return 0; // high half — bus drives zero
    case SWICReg + 2:
        return p->sw_ic_reg;
    default:
        LOG(2, "Stopwatch block read at +%02x (unmodeled)", off);
        return 0;
    }
}

// CLUT block registers (CLUTAddrReg / CLUTDataReg / CLUTPBCR) follow
// the same 16-bit-in-32-bit-slot bus convention as the JMFB block —
// the meaningful 16 bits live at slot+2.  Without this the JMFB
// driver's `cscSetEntries` writes hit the unmodeled default at +2
// while our slot+0 cases caught the high-half zero (palette stayed
// pinned to the init grayscale ramp), and `cscSetMode` writes to
// CLUTPBCR likewise lost — depth changes from System 7's Monitors
// control panel never reached display.format.
static void handle_clut_write16(jmfb_priv_t *p, uint32_t off, uint16_t val) {
    switch (off) {
    case CLUTAddrReg:
    case CLUTDataReg:
    case CLUTPBCR:
        // High half of long write — bus discards.
        return;
    case CLUTAddrReg + 2:
        // 8·24 maps the index into the low byte; a write resets the R/G/B
        // sub-counter so the next three CLUTDataReg writes load the new
        // entry.
        p->clut_idx = (uint8_t)(val & 0xFFu);
        p->clut_phase = 0;
        return;
    case CLUTDataReg + 2: {
        uint8_t component = (uint8_t)(val & 0xFFu);
        if (p->clut_phase == 0) {
            p->clut_pending.r = component;
            p->clut_phase = 1;
        } else if (p->clut_phase == 1) {
            p->clut_pending.g = component;
            p->clut_phase = 2;
        } else {
            p->clut_pending.b = component;
            p->clut_pending.a = 255;
            p->clut[p->clut_idx] = p->clut_pending;
            p->clut_idx++; // auto-increment for run-write
            p->clut_phase = 0;
            p->display.generation++; // renderer re-uploads the CLUT
        }
        return;
    }
    case CLUTPBCR + 2: {
        p->clut_pbcr = val;
        pixel_format_t f = depth_to_format(val);
        // Bit 1 = 24bpp packed (RAMDAC bypass) on top of the depth=3 case.
        if ((val & 0x0002u) && f == PIXEL_8BPP)
            f = PIXEL_32BPP_XRGB;
        if (p->display.format != f) {
            p->display.format = f;
            recompute_stride(p);
            p->display.generation++;
        }
        return;
    }
    default:
        LOG(2, "CLUT block write at +%02x = %04x (unmodeled)", off, val);
        return;
    }
}

static uint16_t handle_clut_read16(jmfb_priv_t *p, uint32_t off) {
    switch (off) {
    case CLUTAddrReg:
    case CLUTPBCR:
        return 0; // high half of long read — bus drives zero
    case CLUTAddrReg + 2:
        return p->clut_idx;
    case CLUTPBCR + 2:
        return p->clut_pbcr;
    default:
        LOG(2, "CLUT block read at +%02x (unmodeled)", off);
        return 0;
    }
}

static void handle_endeavor_write16(jmfb_priv_t *p, uint32_t off, uint16_t val) {
    // Endeavor PLL registers are 16-bit-in-32-bit-slot too — meaningful
    // value lands at slot+2 when the driver writes them with `move.l`.
    switch (off) {
    case EndeavorM:
    case EndeavorN:
    case EndeavorExtClkSel:
    case EndeavorReserved:
        return; // high half of long write — bus discards
    case EndeavorM + 2:
        p->endeavor_m = val;
        break;
    case EndeavorN + 2:
        p->endeavor_n = val;
        break;
    case EndeavorExtClkSel + 2:
        p->endeavor_ext_clk = val;
        break;
    case EndeavorReserved + 2:
        p->endeavor_reserved = val;
        break;
    default:
        LOG(2, "Endeavor block write at +%02x = %04x (unmodeled)", off, val);
        return;
    }
    LOG(3, "Endeavor +%02x = %04x (accept-and-log)", off, val);
}

static uint16_t handle_endeavor_read16(jmfb_priv_t *p, uint32_t off) {
    switch (off) {
    case EndeavorM:
    case EndeavorN:
    case EndeavorExtClkSel:
    case EndeavorReserved:
        return 0; // high half of long read — bus drives zero
    case EndeavorM + 2:
        return p->endeavor_m;
    case EndeavorN + 2:
        return p->endeavor_n;
    case EndeavorExtClkSel + 2:
        return p->endeavor_ext_clk;
    case EndeavorReserved + 2:
        return EndeavorID;
    default:
        LOG(2, "Endeavor block read at +%02x (unmodeled)", off);
        return 0;
    }
}

// Dispatch table.  Each call checks the block id then forks into
// per-block per-width handlers.

static uint8_t io_read8(void *dev, uint32_t addr) {
    // 8-bit register reads aren't issued by the Apple driver but are
    // tolerated.  Read the underlying 16-bit value and return the byte.
    jmfb_priv_t *p = dev;
    uint32_t off;
    int blk = classify(addr, p->slot_base, &off);
    if (blk < 0)
        return 0;
    uint16_t v;
    switch (blk) {
    case 0:
        v = handle_jmfb_read16(p, off & ~1u);
        break;
    case 1:
        v = handle_stopwatch_read16(p, off & ~1u);
        break;
    case 2:
        v = handle_clut_read16(p, off & ~1u);
        break;
    case 3:
        v = handle_endeavor_read16(p, off & ~1u);
        break;
    default:
        return 0;
    }
    return (uint8_t)((addr & 1) ? (v & 0xFFu) : (v >> 8));
}

static uint16_t io_read16(void *dev, uint32_t addr) {
    jmfb_priv_t *p = dev;
    uint32_t off;
    int blk = classify(addr, p->slot_base, &off);
    if (blk < 0)
        return 0;
    switch (blk) {
    case 0:
        return handle_jmfb_read16(p, off);
    case 1:
        return handle_stopwatch_read16(p, off);
    case 2:
        return handle_clut_read16(p, off);
    case 3:
        return handle_endeavor_read16(p, off);
    }
    return 0;
}

static uint32_t io_read32(void *dev, uint32_t addr) {
    return ((uint32_t)io_read16(dev, addr) << 16) | io_read16(dev, addr + 2);
}

static void io_write16(void *dev, uint32_t addr, uint16_t val);

static void io_write8(void *dev, uint32_t addr, uint8_t val) {
    // Same treatment as io_read8 — promote to a 16-bit write.
    io_write16(dev, addr & ~1u, (uint16_t)((uint16_t)val | ((uint16_t)val << 8)));
}

static void io_write16(void *dev, uint32_t addr, uint16_t val) {
    jmfb_priv_t *p = dev;
    uint32_t off;
    int blk = classify(addr, p->slot_base, &off);
    if (blk < 0)
        return;
    switch (blk) {
    case 0:
        handle_jmfb_write16(p, off, val);
        break;
    case 1:
        handle_stopwatch_write16(p, off, val);
        break;
    case 2:
        handle_clut_write16(p, off, val);
        break;
    case 3:
        handle_endeavor_write16(p, off, val);
        break;
    }
}

static void io_write32(void *dev, uint32_t addr, uint32_t val) {
    io_write16(dev, addr, (uint16_t)(val >> 16));
    io_write16(dev, addr + 2, (uint16_t)(val & 0xFFFFu));
}

static memory_interface_t s_jmfb_mem_iface = {
    .read_uint8 = io_read8,
    .read_uint16 = io_read16,
    .read_uint32 = io_read32,
    .write_uint8 = io_write8,
    .write_uint16 = io_write16,
    .write_uint32 = io_write32,
};

// === Card vtable ============================================================

// Try the explicit pending path first, then a small set of well-known
// search paths.  Returns true on success.  `buf` must hold at least
// JMFB_DECLROM_CHIP_SIZE bytes; the loader reads the raw chip data
// (32 KB).  The byte-lane expansion happens later in load_vrom().
static bool try_load_vrom(const char *path, uint8_t *buf) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    size_t n = fread(buf, 1, JMFB_DECLROM_CHIP_SIZE, f);
    fclose(f);
    return n == JMFB_DECLROM_CHIP_SIZE;
}

// Expand a 32 KB chip image with byteLanes = $78 (lane 3 only) into a
// 128 KB bus-space buffer.  Each chip byte at offset i ends up at bus
// offset i*4 + 3 (lane 3 of longword i); lanes 0..2 stay zero.  This is
// what the OS Slot Manager expects when it reads the format block at
// the high end of slot space.
static void expand_lane3(const uint8_t *chip, size_t chip_size, uint8_t *bus_buf) {
    for (size_t i = 0; i < chip_size; i++)
        bus_buf[i * 4 + 3] = chip[i];
}

// Try the well-known search paths and the directory of the loaded ROM
// (which is what the integration test harness sets via the absolute
// `rom=...` arg, so the relative-path entries below don't resolve once
// the harness has cd'd into the per-test directory).  `rom_path` is the
// absolute ROM path the user passed on the command line; we look for
// `Apple-341-0868.vrom` next to it.  Returns true on success.
static bool try_load_vrom_in_rom_dir(uint8_t *chip, char **out_path) {
    const char *rom_path = rom_pending_path();
    if (!rom_path)
        return false;
    const char *slash = strrchr(rom_path, '/');
    if (!slash)
        return false;
    size_t dir_len = (size_t)(slash - rom_path + 1);
    char path[1024];
    if (dir_len + sizeof("Apple-341-0868.vrom") > sizeof(path))
        return false;
    memcpy(path, rom_path, dir_len);
    memcpy(path + dir_len, "Apple-341-0868.vrom", sizeof("Apple-341-0868.vrom"));
    if (try_load_vrom(path, chip)) {
        *out_path = strdup(path);
        return true;
    }
    return false;
}

// Load Apple-341-0868.vrom (32 KB chip image), validate its byteLanes
// byte, and produce the bus-space layout into p->vrom (which is sized
// JMFB_DECLROM_BUS_SIZE = 128 KB).  Returns true on success.
static bool load_vrom(jmfb_priv_t *p) {
    static const char *search_paths[] = {
        "/opfs/images/vrom/Apple-341-0868.vrom",
        "tests/data/roms/Apple-341-0868.vrom",
        "Apple-341-0868.vrom",
        NULL,
    };
    uint8_t *chip = calloc(1, JMFB_DECLROM_CHIP_SIZE);
    if (!chip)
        return false;
    char *rom_dir_path = NULL;
    bool found = try_load_vrom_in_rom_dir(chip, &rom_dir_path);
    for (const char **q = search_paths; !found && *q; q++) {
        if (try_load_vrom(*q, chip)) {
            rom_dir_path = strdup(*q);
            found = true;
        }
    }
    if (!found) {
        free(chip);
        free(rom_dir_path);
        return false;
    }
    // The chip's last byte is the byteLanes value (per spec, the
    // byteLanes byte sits at the highest address of the active lanes —
    // for lane-3-only that's the chip's last byte).
    uint8_t byteLanes = chip[JMFB_DECLROM_CHIP_SIZE - 1];
    if (byteLanes == 0x78u) {
        // Standard 8·24 layout — sparse-expand.
        expand_lane3(chip, JMFB_DECLROM_CHIP_SIZE, p->vrom);
    } else if (byteLanes == 0x0Fu) {
        // 4-lane layout (e.g. a synth ROM) — flat copy into the last
        // 32 KB of the bus buffer; lanes 0..3 all carry data.
        memcpy(p->vrom + JMFB_DECLROM_BUS_SIZE - JMFB_DECLROM_CHIP_SIZE, chip, JMFB_DECLROM_CHIP_SIZE);
    } else {
        LOG(0, "Apple-341-0868.vrom: unsupported byteLanes $%02x (only $78 and $0F handled)", byteLanes);
        free(chip);
        free(rom_dir_path);
        return false;
    }
    free(p->vrom_path);
    p->vrom_path = rom_dir_path;
    p->vrom_size = JMFB_DECLROM_BUS_SIZE;
    free(chip);
    return true;
}

static int card_init(nubus_card_t *card, config_t *cfg, checkpoint_t *cp) {
    (void)cp;
    jmfb_priv_t *p = calloc(1, sizeof(*p));
    if (!p)
        return -1;
    p->card = card;
    p->slot_base = nubus_slot_base(card->slot);

    p->vram = calloc(1, JMFB_VRAM_SIZE);
    p->vrom = calloc(1, JMFB_DECLROM_BUS_SIZE);
    if (!p->vram || !p->vrom) {
        free(p->vram);
        free(p->vrom);
        free(p);
        return -1;
    }

    if (!load_vrom(p)) {
        // requires_vrom is true on this kind, so the dialog gates
        // boot on a real VROM file; reaching here means CI ran without
        // one.  Log loudly and continue with a zero-filled declrom —
        // PrimaryInit won't find a Format Header and the OS will skip
        // the slot, but the rest of the machine still boots.
        LOG(0, "Apple-341-0868.vrom not found; declaration ROM is zero-filled");
    }

    // Monitor sense code — consumed from the pending-sense slot the
    // shell can set via `nubus.video_sense = N` before `machine.boot`.
    // The default is $6 (Standard RGB / 13" AppleColor), which keeps
    // existing tests/integration paths reproducing the same boot we've
    // baselined.  After consumption the pending slot is left at the
    // default so a forgotten configuration doesn't leak into the next
    // machine.boot.
    p->sense_code = s_pending_sense;
    s_pending_sense = 0x6;
    s_pending_sense_set = false;

    const nubus_monitor_t *monitor = monitor_for_sense(p->sense_code);
    uint32_t mon_w = monitor ? monitor->width : 640;
    uint32_t mon_h = monitor ? monitor->height : 480;

    // Default register state from PrimaryInit's expected starting point.
    // The Apple Display Card 8•24 powers up at 1 bpp; PrimaryInit fills
    // VRAM with the canonical $AAAAAAAA / $55555555 gray pattern in that
    // mode, and the OS later switches depth via cscSetMode.  Defaulting
    // to 8 bpp here makes that gray fill render as black/white stripes.
    p->jmfb_csr = 0;
    p->jmfb_video_base = 0xA00 / 32; // driver convention: $A00 byte offset
    p->jmfb_row_words = mon_w / 32u; // 1bpp longs/row for the chosen monitor

    p->display.width = mon_w;
    p->display.height = mon_h;
    p->display.stride = 640 / 8; // 1 bpp: 80 bytes/row
    p->display.format = PIXEL_1BPP_MSB;
    p->display.bits = p->vram + 0xA00;
    p->display.clut = p->clut;
    p->display.clut_len = 256;
    p->display.generation = 1;

    // Initial CLUT — a simple grayscale ramp so the canvas isn't blank
    // before the OS programs a palette.  The driver's first cscSetEntries
    // will overwrite this.
    for (int i = 0; i < 256; i++) {
        p->clut[i].r = (uint8_t)i;
        p->clut[i].g = (uint8_t)i;
        p->clut[i].b = (uint8_t)i;
        p->clut[i].a = 255;
    }

    card->priv = p;

    // Register host-backed regions on the bus map.  VRAM is writable;
    // the declaration ROM is read-only.  The register window goes
    // through memory_map_add with a memory_interface_t since it needs
    // I/O dispatch on every access.
    memory_map_host_region(cfg->mem_map, "jmfb_vram", p->vram, p->slot_base, JMFB_VRAM_SIZE, /*writable*/ true);
    memory_map_host_region(cfg->mem_map, "jmfb_declrom", p->vrom, p->slot_base + JMFB_DECLROM_BUS_OFFSET,
                           JMFB_DECLROM_BUS_SIZE, /*writable*/ false);
    memory_map_add(cfg->mem_map, p->slot_base + JMFB_BLOCK_OFFSET, JMFB_REGISTER_SIZE, "JMFB regs", &s_jmfb_mem_iface,
                   p);

    // VRAM mirror at slot+$900000 — the Mac IIcx ROM, when running in
    // 24-bit Memory Manager Mode, builds framebuffer pointers with the
    // high byte holding master-pointer flags ($F9_______ for slot $9).
    // Apple QuickDraw inner loops dereference these pointers without
    // first calling _StripAddress, so the access goes to the literal
    // 32-bit address $F9900xxx.  Real Mac IIcx hardware: the card's
    // 16 MB slot allocation is decoded such that VRAM is reachable from
    // multiple base offsets; the slot $900000 region is one of those
    // aliases.  Without this mirror, ScrnBase = $F9900A00 reads land in
    // unmapped memory and QuickDraw bus-errors.
    memory_map_host_region_alias(cfg->mem_map, p->slot_base + 0x900000u, p->slot_base);

    return 0;
}

static void card_teardown(nubus_card_t *card, config_t *cfg) {
    (void)cfg;
    jmfb_priv_t *p = card->priv;
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
    jmfb_priv_t *p = card->priv;
    if (!p)
        return;
    if (p->sw_ic_reg & ENVERTI)
        nubus_assert_irq(card);
    // Bump display.generation every VBL so consumers that cache on
    // generation (the WebGL renderer in em_video.c) re-upload the
    // framebuffer.  CPU writes to VRAM happen directly through the
    // host_region mapping and don't otherwise notify the renderer.
    p->display.generation++;
}

static const display_t *card_display(nubus_card_t *card) {
    jmfb_priv_t *p = card->priv;
    return p ? &p->display : NULL;
}

static const char *card_name(const nubus_card_t *card) {
    (void)card;
    return "Apple Macintosh Display Card 8\xe2\x80\xa2"
           "24"; // "8•24"
}

static const nubus_card_ops_t mdc_8_24_ops = {
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
    card->ops = &mdc_8_24_ops;
    card->slot = slot;
    if (card->ops->init(card, cfg, cp) != 0) {
        free(card);
        return NULL;
    }
    return card;
}

// Five monitor types per the Rev B ROM mode matrix (proposal §3.2.5).
static const int mdc_full_depths[] = {1, 2, 4, 8, 24, 0};
static const int mdc_8bpp_depths[] = {1, 2, 4, 8, 0};
static const nubus_monitor_t mdc_8_24_monitors[] = {
    {.id = "12in_rgb", .name = "12\" RGB", .width = 512, .height = 384, .depths = mdc_full_depths},
    {.id = "13in_rgb", .name = "13\" AppleColor", .width = 640, .height = 480, .depths = mdc_full_depths},
    {.id = "15in_bw", .name = "15\" Portrait B&W", .width = 640, .height = 870, .depths = mdc_8bpp_depths},
    {.id = "16in_rgb", .name = "16\" RGB", .width = 832, .height = 624, .depths = mdc_8bpp_depths},
    {.id = "21in_rgb", .name = "21\" RGB", .width = 1152, .height = 870, .depths = mdc_8bpp_depths},
    {0},
};

// Map the JMFB's 3-bit raw monitor sense to the (width, height) the
// JMFB driver's PrimaryInit will configure once it scans the sense
// lines and picks the matching mode-list sRsrc out of the declaration
// ROM.  The table is a literal transcription of the comment block at
// JMFBPrimaryInit.a:78-95 ("Sense(2:0) Raw / Reformatted Sense /
// Monitor Type"):
//
//   000 = RGB Workstation (Kong)        — 21" RGB,  1152x870
//   001 = B/W Full Page (Portrait)      — 15" Mono, 640x870
//   010 = Modified Apple II-GS (Rubik)  — 12" RGB,  512x384
//   011 = B/W Workstation (Kong)        — 21" B&W,  1152x870
//   100 = NTSC (Interlaced)             — 640x480 approximation
//   101 = RGB Full Page (Portrait)      — 15" RGB,  640x870
//   110 = Standard RGB                  — 13" RGB,  640x480  (default)
//   111 = no connect / extended sense   — unsupported here
//
// Returns the matching nubus_monitor_t for a given sense code, or NULL
// for the no-connect code so the caller can fall back to safe defaults.
// (Extended-sense detection — for the 16" RGB and Sarnoff NTSC box —
// would key off this NULL return; we don't model it.)
static const nubus_monitor_t *monitor_for_sense(uint8_t sense) {
    static const struct {
        uint8_t sense;
        const char *id;
    } map[] = {
        {0x0, "21in_rgb"}, // RGB Workstation
        {0x1, "15in_bw" }, // B&W Portrait
        {0x2, "12in_rgb"}, // Rubik
        {0x3, "21in_rgb"}, // B&W Workstation (same dimensions as RGB Kong)
        {0x4, "13in_rgb"}, // NTSC — approximate as 640x480
        {0x5, "15in_bw" }, // RGB Portrait (same dimensions as B&W Portrait)
        {0x6, "13in_rgb"}, // Standard RGB
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (map[i].sense != (sense & 7))
            continue;
        for (const nubus_monitor_t *m = mdc_8_24_monitors; m->id; m++) {
            if (strcmp(m->id, map[i].id) == 0)
                return m;
        }
    }
    return NULL;
}

// (s_pending_sense / s_pending_sense_set defined near the top of this
// file alongside the matching forward declarations.)

void jmfb_pending_sense_set(uint8_t sense) {
    s_pending_sense = sense & 7;
    s_pending_sense_set = true;
}

uint8_t jmfb_pending_sense_get(void) {
    return s_pending_sense;
}

const nubus_card_kind_t mdc_8_24_kind = {
    .id = "mdc_8_24",
    .display_name = "Apple Macintosh Display Card 8\xe2\x80\xa2"
                    "24",
    .requires_vrom = true,
    .monitors = mdc_8_24_monitors,
    .factory = factory,
};
