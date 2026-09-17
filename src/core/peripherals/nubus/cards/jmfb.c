// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// jmfb.c
// Apple Macintosh Display Card 8•24 (Rev B, ROM `341-0868`).  See
// proposal-machine-iicx-iix.md §3.2.5 + jmfb.h for the contract.
//
// Implementation status (proposal step 6, minimum-viable):
//   * Card factory loads `mdc-8-24-revb-d1629664.vrom` and registers VRAM,
//     declrom, and the register window on the bus.
//   * I/O dispatcher in this file handles all four register blocks at
//     a single switch.  Modelled handlers cover the registers the
//     Monitors-control-panel boot path depends on; everything else is
//     accept-and-log so an OS write that should be a no-op doesn't
//     turn into a bus error.
//   * Sense lines satisfy the JMFB PrimaryInit read so the OS picks
//     up the user's `monitor=` choice.
//   * CLUT writes feed display.clut and set clut_dirty; depth changes
//     via CLUTPBCR feed display.format and set shape_dirty.
//   * Mode-table parsing inside `mdc-8-24-revb-d1629664.vrom` is left to the
//     System 7 driver — we present the bytes; it walks them.

#include "jmfb.h"

#include "jmfb_family.h"

#include "card.h"
#include "checkpoint.h"
#include "declrom.h"
#include "display.h"
#include "gsvrom.h"
#include "log.h"
#include "memory.h"
#include "nubus.h"
#include "rtc.h"
#include "system.h"
#include "system_config.h"

#include <stddef.h> // offsetof — the checkpoint range
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("video");

// === Forward declarations ===================================================
//
// The monitor list `mdc_8_24_monitors` and the sense→monitor lookup
// live near the bottom of this file (next to the per-card kind
// descriptor that references the list); the JMFB factory body
// further up needs to reach them.  Forward declarations here let the
// factory call `monitor_for_sense` and read `s_pending_sense` without
// reshuffling the file.
static const struct nubus_monitor *monitor_for_sense(uint8_t sense);

// Pending sense code consumed by the next JMFB factory call.  Set
// from the shell via `nubus.video_sense = N` before `machine.boot`;
// reset to the default ($6 = 13" RGB) on consumption so a forgotten
// configuration doesn't leak across machine reinitialisations.
// STAGING -- ON DEATH ROW.  This is a construction input travelling as a
// hidden per-module global: the visible per-slot channel
// (machine.nubus.slot[N].video_mode) funnels through here, and the factory
// consumes it destructively.  proposal-construction-inputs.md R1 replaces
// every one of these with a machine_build_opts_t field passed to the factory
// as an ARGUMENT, which is also what proposal-reset-and-nonvolatile-state.md
// R3 means by "no holder, no staged copy, no pending slot".  Do not add
// another one; the per-slot channel is already there to carry it.
static uint8_t s_pending_sense = 0x6;

// Pending video-mode selection set via `machine.video_mode = "id"`
// (mirrors s_pending_sense above; consumed in the same factory
// invocation).  At most 31 chars + NUL fits any "monitor_Nbpp" id.
// Empty string means "no pending mode — fall back to plain sense".
// STAGING -- see the note above; R1 deletes this too.
static char s_pending_video_mode_id[NUBUS_VIDEO_MODE_ID_MAX] = "";

// Pending "WxHxD" custom resolution set via `custom_mode=` (proposal-
// nubus-runtime-vrom §3.6).  The generic kind generates a video
// sResource at this geometry and boots its default monitor on it.
// Empty string means "no custom mode".
// STAGING -- see the note above; R1 deletes this too.
static char s_pending_custom_mode[40] = "";

// === Per-card private state =================================================

// Field order IS the checkpoint format (the via_t / adb_t / asc_t idiom): every
// scalar the card must restore comes first, and the checkpoint is one range
// ending at `display`.  Add a scalar above that line and it is saved
// automatically; add a POINTER above it and a stale address is restored, which
// is why the pointers and the construction facts sit below with a marker.
typedef struct {
    rgba8_t clut[256];

    // RAMDAC sub-state for CLUT writes — three sequential long writes
    // to CLUTDataReg load one palette entry.  After the third write,
    // the palette index auto-increments.  Apple's JMFB driver uses
    // TWO protocols here, picked at runtime via bit $C of
    // driver_private+$10 (the JMFB driver's Control case 2):
    //
    //   * 8/16bpp variant (12"/13"/16" RGB monitors):  three long
    //     writes to (A3) where D2 = 0x00BBGGRR is shifted right by
    //     8 between writes.  The RAMDAC reads byte 0 (LSB) of each
    //     long, yielding R, G, B in that order.
    //
    //   * "24bpp" variant (Portrait B&W, 21" Color, NTSC/PAL):  two
    //     CLR.L (A3) writes followed by one MOVE.L D2,(A3) where
    //     D2 = 0x00BBGGRR.  The RAMDAC reads bytes 0/1/2 of that
    //     single long simultaneously as R/G/B; the two zero writes
    //     are discarded.
    //
    // Protocol detection: track the three full 32-bit values of an
    // entry-update window.  If pending[0] == 0 AND pending[1] == 0
    // AND the upper 24 bits of pending[2] are non-zero, treat
    // pending[2] as a packed triplet; otherwise read byte 0 of each
    // pending[N] as one component.  See clut_finalize_entry below.
    // The JMFB chip's own registers, modelled once for both cards that carry
    // it (jmfb_family.h).  Plain data, so it rides this struct's checkpointed
    // scalar range exactly as the loose fields it replaced did.
    jmfb_regs_t regs;

    // The Endeavor PLL block stays here: it is the one part of the chip the
    // two cards model different amounts of, so it is not shared.
    uint16_t endeavor_m;
    uint16_t endeavor_n;
    uint16_t endeavor_ext_clk;
    uint16_t endeavor_reserved;

    // --- Pointers and construction facts last; NOT part of the range above ---
    // `display` leads them because it embeds `bits`/`clut` pointers of its own;
    // its scalar head is checkpointed separately as a display_head_t.
    display_t display;
    jmfb_bind_t bind; // what `regs` acts on; rebuilt at init, never checkpointed
    nubus_card_t *card; // back-pointer for IRQ helpers
    uint8_t *vram; // 2 MB
    uint8_t *vrom; // 32 KB declaration ROM bytes
    char *vrom_path; // path the VROM was loaded from
    uint32_t vrom_size; // typically 32 KB; 0 if no VROM loaded
    uint32_t slot_base; // physical bus base (== nubus_slot_base(slot))
} jmfb_priv_t;

// The layout above is load-bearing.  If this fires, a member moved across the
// boundary: re-check what the checkpoint range now covers before updating it.
_Static_assert(offsetof(jmfb_priv_t, display) < offsetof(jmfb_priv_t, card),
               "jmfb checkpoint range must end before the pointer block");

// === Helpers ================================================================

// Map the ≤8 bpp depth field in CLUTPBCR (bits 3-4) to a pixel_format_t.

// Recompute display.stride from JMFBRowWords + current format.  The Apple
// driver clears JMFBRowWords to 0 during chip-reset sequences before
// programming the real value; treating raw 0 as stride=0 zeros our
// display and breaks the renderer / PNG / checksum paths.  Keep the
// previous stride value across 0 writes — the next non-zero write fixes
// it.
// Bits-per-pixel for storage (not visible-colour-depth) of the
// current display format.  PIXEL_32BPP_XRGB returns 32 because the
// framebuffer stores 4 bytes/pixel; the RAMDAC bypass mode discards
// one of those bytes during scan but the storage layout is XRGB.
// Recompute display.stride AND display.width every time row_words or
// the pixel format changes.  Width is a pure function of (row_words,
// bpp); height is a property of the chosen monitor, fixed at JMFB
// factory time from sense_code and not derived here.
//
// Two stride formulas, picked by depth:
//
//   ≤8 bpp:    stride = row_words * 4
//              width  = stride * 8 / bpp = row_words * 32 / bpp
//
//   24 bpp:    stride = row_words * 32 / 3
//              width  = stride / 4
//
// The 24bpp ("PIXEL_32BPP_XRGB") path stores 4 bytes per pixel on the
// JMFB — Mac OS sets PixMap.pixelSize=32 / rowBytes=2560 (= 4 × 640)
// and writes XRGB values; the RAMDAC ignores the X byte during scan
// (per the Designing Cards & Drivers "8•24 24-bit selection: bit 1
// of CLUTPBCR enters RAMDAC bypass and consumes all three colour
// bytes" passage — three of four written, one discarded).  The
// driver's TFBM30Parms encodes rowWords as 240 from
// `(TFBM30RB * 3 / 4) / 4 / 2`; inverted, that gives stride =
// row_words * 32 / 3 = 2560 — the *storage* stride, not the
// 1920-byte RAMDAC scan stride.
// Re-derive the whole scanout from the two registers that describe it, and let
// display_set_scanout decide whether VRAM can back it.
//
// VideoBase and RowWords arrive in separate register writes, so before this
// existed each handler updated its own half of the descriptor and nothing ever
// compared the result against the 2 MB allocation: a 16-bit VideoBase yields
// an offset of up to 5,592,320 bytes, 2.7x past the end (04-video F-20).  Both
// handlers now call this, so `bits` and `stride * height` are always decided
// together.
//
// No blank buffer: the JMFB has only its VRAM, so a refused descriptor scans
// nothing at all (height 0, bits NULL) and every consumer already guards on
// that.  A guest that programs an impossible base gets a black screen, which
// is the honest answer -- the alternative is showing it some other part of
// VRAM and calling that a picture.

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
        LOG(2, "JMFB: Endeavor block write at +%02x = %04x (unmodeled)", off, val);
        return;
    }
    LOG(3, "JMFB: Endeavor +%02x = %04x (accept-and-log)", off, val);
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
        LOG(2, "JMFB: Endeavor block read at +%02x (unmodeled)", off);
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
    uint16_t v = (blk == JMFB_BLK_ENDEAVOR) ? handle_endeavor_read16(p, off & ~1u)
                                            : jmfb_read16(&p->regs, &p->bind, blk, off & ~1u);
    return (uint8_t)((addr & 1) ? (v & 0xFFu) : (v >> 8));
}

static uint16_t io_read16(void *dev, uint32_t addr) {
    jmfb_priv_t *p = dev;
    uint32_t off;
    int blk = classify(addr, p->slot_base, &off);
    if (blk < 0)
        return 0;
    // Blocks 0-2 are the shared chip model; the Endeavor PLL is this card's
    // own (jmfb_family.h).
    if (blk == JMFB_BLK_ENDEAVOR)
        return handle_endeavor_read16(p, off);
    return jmfb_read16(&p->regs, &p->bind, blk, off);
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
    if (blk == JMFB_BLK_ENDEAVOR)
        handle_endeavor_write16(p, off, val);
    else
        jmfb_write16(&p->regs, &p->bind, blk, off, val);
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

// Load the 8•24 declaration ROM (32 KB chip image) through the shared
// content-driven declrom loader (vrom.c Format-Block-CRC catalog): the
// explicit machine.vrom.load path first (any filename), then the catalog
// name in the standard vrom paths + the ROM directory; validates the
// byteLanes byte and lays the chip out into p->vrom (sized
// JMFB_DECLROM_BUS_SIZE = 128 KB).  Returns true on success.
static bool load_vrom(jmfb_priv_t *p) {
    char *path = NULL;
    if (!declrom_load_vrom_card(mdc_8_24_kind.id, p->vrom, JMFB_DECLROM_BUS_SIZE, &path))
        return false;
    free(p->vrom_path);
    p->vrom_path = path;
    p->vrom_size = JMFB_DECLROM_BUS_SIZE;
    return true;
}

static int card_init_common(nubus_card_t *card, config_t *cfg, checkpoint_t *cp, bool generic) {
    (void)cp;
    jmfb_priv_t *p = calloc(1, sizeof(*p));
    if (!p)
        return -1;
    p->card = card;
    p->slot_base = nubus_slot_base(card->slot);
    // VBL IRQ starts masked — bit 1 is active-high disable.  Mac OS's
    // InstallSlotInterrupt clears it once the SlotIQE is installed.
    p->regs.sw_ic = VINT_DISABLE;

    p->vram = calloc(1, JMFB_VRAM_SIZE);
    p->vrom = calloc(1, JMFB_DECLROM_BUS_SIZE);
    if (!p->vram || !p->vrom) {
        free(p->vram);
        free(p->vrom);
        free(p);
        return -1;
    }

    // Bind the shared chip model to what THIS card's registers act on.  The
    // 8*24 scans its own VRAM; the GC's copy of the same chip addresses a
    // different store, which is the whole reason the bindings are separate
    // from the register state (jmfb_family.h).
    p->bind = (jmfb_bind_t){.display = &p->display,
                            .store = p->vram,
                            .store_size = JMFB_VRAM_SIZE,
                            .clut = p->clut,
                            .card = card,
                            .tag = "JMFB"};

    // A staged custom resolution overrides the default monitor's geometry
    // (proposal-nubus-runtime-vrom §3.6): the card senses its default 13"
    // RGB monitor, but that monitor's video sResource — and the HLE
    // display — carry the WxHxD the user asked for.  Consumed here so the
    // generic build below emits records for it; validated against this
    // card's framebuffer window.  custom_monitors backs the pointers in
    // the runtime monitor list; it is read only within this call (the
    // builder copies what it needs and the display geometry is captured
    // into p->display), so a local is enough.
    nubus_monitor_t custom_monitors[5];
    const nubus_monitor_t *gen_monitors = generic ? jmfb_generic_kind.monitors : NULL;
    uint32_t custom_w = 0, custom_h = 0, custom_d = 0;
    bool custom_active = false;
    if (generic && s_pending_custom_mode[0]) {
        const char *why = NULL;
        if (!nubus_custom_mode_parse(s_pending_custom_mode, &custom_w, &custom_h, &custom_d, &why)) {
            LOG(0, "JMFB: 8_24: custom_mode '%s' rejected: %s", s_pending_custom_mode, why);
        } else if (custom_d != 1 && custom_d != 2 && custom_d != 4 && custom_d != 8) {
            LOG(0, "JMFB: 8_24: custom_mode depth %u unsupported (this card has no direct modes; use 1/2/4/8)",
                custom_d);
        } else if ((uint64_t)custom_w * custom_h * custom_d / 8 + 0xA00 > JMFB_VRAM_SIZE) {
            LOG(0, "JMFB: 8_24: custom_mode %ux%ux%u framebuffer exceeds the %u-byte window", custom_w, custom_h,
                custom_d, (unsigned)JMFB_VRAM_SIZE);
        } else {
            // Copy the generic monitor list and rewrite the default (13" RGB,
            // sense $6) entry to the custom geometry; the rest stay so their
            // sResources still exist (the sensed one wins at boot).
            size_t n = 0;
            for (const nubus_monitor_t *mm = jmfb_generic_kind.monitors; mm->id && n < 4; mm++)
                custom_monitors[n++] = *mm;
            for (size_t i = 0; i < n; i++) {
                if (custom_monitors[i].sense_code == 0x6) {
                    custom_monitors[i].width = custom_w;
                    custom_monitors[i].height = custom_h;
                    custom_monitors[i].name = "Custom";
                }
            }
            custom_monitors[n] = (nubus_monitor_t){0};
            gen_monitors = custom_monitors;
            custom_active = true;
        }
    }
    s_pending_custom_mode[0] = '\0';

    if (generic) {
        // Generic sibling kind ("8_24"): generate the GS declaration ROM at
        // card_init — records from the (possibly custom-overridden) monitor
        // list, code fragments spliced, CRC stamped in C (proposal-nubus-
        // runtime-vrom §4); the offer registry is never consulted.
        declrom_builder_t *bld = gsvrom_generate(GSVROM_JMFB, gen_monitors);
        size_t img_size = 0;
        const uint8_t *img = bld ? declrom_builder_bytes(bld, &img_size) : NULL;
        if (img && declrom_install_builtin(jmfb_generic_kind.id, img, img_size, p->vrom, JMFB_DECLROM_BUS_SIZE))
            p->vrom_size = JMFB_DECLROM_BUS_SIZE;
        else
            LOG(0, "JMFB: 8_24: built-in declaration ROM failed to generate; declaration ROM is zero-filled");
        declrom_builder_free(bld);
    } else if (!load_vrom(p)) {
        // requires_vrom is true on this kind, so the dialog gates
        // boot on a real VROM file; reaching here means CI ran without
        // one.  Log loudly and continue with a zero-filled declrom —
        // PrimaryInit won't find a Format Header and the OS will skip
        // the slot, but the rest of the machine still boots.
        LOG(0, "JMFB: mdc-8-24-revb-d1629664.vrom not found; declaration ROM is zero-filled");
    }
    // Publish the declaration ROM on the card struct (drives the
    // slot[N].card.declrom object-model node, same as the 24AC / 8•24 GC);
    // nubus_delete owns and frees card->declrom after card_teardown.
    card->declrom = p->vrom;
    card->declrom_size = p->vrom_size;

    // If a pending video-mode id was set (via `machine.video_mode =
    // "13in_rgb_8bpp"`), resolve it now — it overrides the pending
    // sense and triggers PRAM seeding below.
    const nubus_monitor_t *seeded_monitor = NULL;
    int seeded_depth_bpp = 0;
    if (s_pending_video_mode_id[0]) {
        if (jmfb_video_mode_lookup(s_pending_video_mode_id, &seeded_monitor, &seeded_depth_bpp)) {
            s_pending_sense = seeded_monitor->sense_code;
        } else {
            LOG(1, "jmfb: pending video_mode '%s' did not match any catalog entry; ignored", s_pending_video_mode_id);
        }
        s_pending_video_mode_id[0] = '\0';
    }
    // A validated custom resolution overrode the default monitor above:
    // sense the default 13" RGB ($6) and seed PRAM to its sister ($A6) at
    // the requested depth, exactly like a video_mode pick but with the
    // geometry the generated records now carry.
    if (custom_active) {
        for (size_t i = 0; custom_monitors[i].id; i++) {
            if (custom_monitors[i].sense_code == 0x6) {
                seeded_monitor = &custom_monitors[i];
                break;
            }
        }
        seeded_depth_bpp = (int)custom_d;
        s_pending_sense = 0x6;
    }

    // Monitor sense code — consumed from the pending-sense slot the
    // shell can set via `nubus.video_sense = N` before `machine.boot`.
    // The default is $6 (Standard RGB / 13" AppleColor), which keeps
    // existing tests/integration paths reproducing the same boot we've
    // baselined.  After consumption the pending slot is left at the
    // default so a forgotten configuration doesn't leak into the next
    // machine.boot.
    p->regs.sense_code = s_pending_sense;
    s_pending_sense = 0x6;

    const nubus_monitor_t *monitor = monitor_for_sense(p->regs.sense_code);
    uint32_t mon_w = monitor ? monitor->width : 640;
    uint32_t mon_h = monitor ? monitor->height : 480;
    // The custom resolution rides the sensed default monitor's slot, so
    // the HLE display geometry follows the override, not the stock 13".
    if (custom_active) {
        mon_w = custom_w;
        mon_h = custom_h;
    }

    // Default register state from PrimaryInit's expected starting point.
    // The Apple Display Card 8•24 powers up at 1 bpp; PrimaryInit fills
    // VRAM with the canonical $AAAAAAAA / $55555555 gray pattern in that
    // mode, and the OS later switches depth via cscSetMode.  Defaulting
    // to 8 bpp here makes that gray fill render as black/white stripes.
    p->regs.csr = 0;
    p->regs.video_base = 0xA00 / 32; // driver convention: $A00 byte offset
    p->regs.row_words = mon_w / 32u; // 1bpp longs/row for the chosen monitor

    p->regs.raster_h = mon_h;
    p->display.format = PIXEL_1BPP_MSB;
    // Derive the descriptor from the registers just set, the same way every
    // later write does.  The old hard-coded `stride = 640/8` described a
    // 640-wide raster no matter which monitor was sensed, so on the 1152-wide
    // Kong the register said 36 row-words and the descriptor said 80 bytes
    // until the driver first wrote RowWords: the blank below covered 80x870
    // of a 144x870 raster (the rest stayed white at 1 bpp -- the exact cold
    // boot flash this blank exists to prevent) and every consumer sheared its
    // rows walking width=1152 over a stride-80 row (04-video F-29).
    jmfb_apply_scanout(&p->regs, &p->bind);
    // Cold boot scans out black, not the white an all-zero 1 bpp buffer gives.
    display_blank_raster(&p->display);
    p->display.clut = p->clut;
    p->display.clut_len = 256;
    p->display.shape_dirty = true;
    p->display.clut_dirty = true;
    p->display.fb_dirty = true;
    // CRT response for the attached monitor — see display_t::crt_response.
    // NULL means "identity gamma", which is the right model for 12"/13"
    // RGB and Portrait B&W (their gamma tables are near-identity in our
    // CLUT trace, so a software display without monitor compensation
    // renders neutral grays correctly).  Kong's CRT amplified blue more
    // than R/G, so its non-NULL kong_crt_response table inverts Apple's
    // gamma pre-correction at display time.
    // The generic kind always uses identity response: the GS vROM ships
    // identity gamma for every monitor, so there is no Apple gamma
    // pre-correction to invert.  This is the ONLY place that distinction is
    // made -- both siblings share mdc_8_24_monitors, so the decision is the
    // kind's, not a second table's.
    p->display.crt_response = (!generic && monitor) ? monitor->crt_response : NULL;
    p->display.response_dirty = true;

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

    // If the user picked a video mode via `machine.video_mode = "id"`,
    // seed PRAM so the Slot Manager's GET_SLOT_DEPTH lands on it at
    // boot.  Mirrors the dance `tests/integration/iicx-video-modes/
    // test.script` does shell-side.  PRAM is already alive at this
    // point — RTC is initialised earlier in the machine init sequence.
    if (seeded_monitor && seeded_depth_bpp > 0) {
        rtc_t *rtc = system_rtc();
        if (rtc) {
            uint8_t spDepth = 0x80;
            switch (seeded_depth_bpp) {
            case 1:
                spDepth = 0x80;
                break;
            case 2:
                spDepth = 0x81;
                break;
            case 4:
                spDepth = 0x82;
                break;
            case 8:
                spDepth = 0x83;
                break;
            default:
                LOG(1, "jmfb: unexpected video-mode depth=%d; PRAM seed using $80 (1bpp)", seeded_depth_bpp);
                break;
            }
            // Stamp ONLY the XPRAM 'NuMc' validity signature ($0C..$0F) so
            // the boot ROM's CkNewPram skips its XPRAM cold-init pass and
            // preserves the slot-9 sPRAMRec we seed below (plus PRAMInitTbl).
            // We deliberately do NOT stamp the low-PRAM validity byte: leaving
            // it invalid lets `_InitUtil` cold-init the 20-byte SysParam block,
            // so caret-blink / double-click (SPClikCaret), key-repeat (SPKbd)
            // and the application font come up at their correct ROM defaults.
            // On the extended RTC the SysParam block lives at physical
            // $08..$0B / $10..$1F (see rtc.c legacy_pram_addr), well clear of
            // the 'NuMc' bytes at $0C..$0F — so the validity signature and the
            // SysParam settings coexist with no overlap, exactly as on real
            // hardware.  (Stamping low-PRAM valid here instead would skip the
            // SysParam cold-init and leave those fields junk — that was the
            // cause of the IIcx/IIfx strobing-caret / dead-double-click bug.)
            rtc_pram_write(rtc, 0x0C, 0x4E); // 'N'
            rtc_pram_write(rtc, 0x0D, 0x75); // 'u'
            rtc_pram_write(rtc, 0x0E, 0x4D); // 'M'
            rtc_pram_write(rtc, 0x0F, 0x63); // 'c'
            // The Start Manager reads the default OS type and boot device from
            // PRAMInitTbl ($76..$89); CkNewPram skips writing it once 'NuMc'
            // is present, so reproduce it here.  Without it OSType=$77,
            // DriveId=$78 and PartitionId=$79 stay 0, so D3 reaches SCSILoad as
            // $00000000 instead of $0001FFFF and the boot-driver DDM match
            // never fires (A/UX falls back to floppy).  Bytes $7C..$89 are
            // zero (already cold-zero) but are written for an exact mirror.
            static const uint8_t pram_init_tbl[] = {
                0x00, // $76 reserved
                0x01, // $77 default OS (Mac)
                0xFF, 0xFF, // $78-$79 default boot drive / partition ("any")
                0xFF, 0xDF, // $7A-$7B
                0x00, 0x00, // $7C-$7D sound alert id
                0x00, 0x00, // $7E-$7F hierarchical menu display / drag
                0x00, 0x00, // $80-$81 default video
                0x00, 0x00, 0x00, // $82-$87 default hilite colour (black)
                0x00, 0x00, 0x00, //
                0x00, 0x00, // $88-$89 reserved
            };
            for (size_t i = 0; i < sizeof(pram_init_tbl); i++)
                rtc_pram_write(rtc, (uint8_t)(0x76 + i), pram_init_tbl[i]);
            // Per-slot sPRAMRec layout (8 bytes): each slot's record
            // lives at offset (0x46 + (slot - 9) * 8) in PRAM (see
            // docs/core/memory/pram.md §6).  $46..$47 = BoardID, $48 = savedMode,
            // $49/$4A = savedSRsrcID / savedRawSRsrcID, $4B..$4D = 0.
            uint8_t pram_off = (uint8_t)(0x46 + (card->slot - 9) * 8);
            rtc_pram_write(rtc, pram_off + 0, 0x00);
            rtc_pram_write(rtc, pram_off + 1, 0x27); // BoardID = $0027 (JMFB)
            rtc_pram_write(rtc, pram_off + 2, spDepth);
            rtc_pram_write(rtc, pram_off + 3, seeded_monitor->srsrc_sister);
            rtc_pram_write(rtc, pram_off + 4, seeded_monitor->srsrc_sister);
            rtc_pram_write(rtc, pram_off + 5, 0x00);
            rtc_pram_write(rtc, pram_off + 6, 0x00);
            rtc_pram_write(rtc, pram_off + 7, 0x00);
            LOG(1, "jmfb: seeded slot-%d PRAM for video mode '%s' (sister=$%02X spDepth=$%02X)", card->slot,
                seeded_monitor->id, seeded_monitor->srsrc_sister, spDepth);
        }
    }

    return 0;
}

static void card_teardown(nubus_card_t *card, config_t *cfg) {
    (void)cfg;
    jmfb_priv_t *p = card->priv;
    if (!p)
        return;
    free(p->vram);
    // p->vrom is published as card->declrom; nubus_delete owns and frees it.
    free(p->vrom_path);
    free(p);
    card->priv = NULL;
}

static void card_on_vbl(nubus_card_t *card, config_t *cfg) {
    (void)cfg;
    jmfb_priv_t *p = card->priv;
    if (!p)
        return;
    if (!(p->regs.sw_ic & VINT_DISABLE))
        nubus_assert_irq(card);
    // Mark the framebuffer dirty every VBL so the renderer re-uploads.
    // CPU writes to VRAM happen directly through the host_region mapping
    // and don't otherwise notify the renderer, so this VBL pulse is what
    // surfaces ongoing Mac OS drawing to the canvas.
    p->display.fb_dirty = true;
}

static display_t *card_display(nubus_card_t *card) {
    jmfb_priv_t *p = card->priv;
    return p ? &p->display : NULL;
}

static const char *card_name(const nubus_card_t *card) {
    (void)card;
    return "Apple Macintosh Display Card 8\xe2\x80\xa2"
           "24"; // "8•24"
}

// Thin per-kind init wrappers — the sibling pair shares one HLE model
// (card_init_common); only the declROM source differs (proposal-generic-
// nubus-vrom sec. 6.1: "one HLE model per pair — hard rule").
static int card_init_real(nubus_card_t *card, config_t *cfg, checkpoint_t *cp) {
    return card_init_common(card, cfg, cp, /*generic*/ false);
}

static int card_init_generic(nubus_card_t *card, config_t *cfg, checkpoint_t *cp) {
    return card_init_common(card, cfg, cp, /*generic*/ true);
}

static const char *card_name_generic(const nubus_card_t *card) {
    (void)card;
    return "Apple Macintosh Display Card 8\xe2\x80\xa2"
           "24 (generic video ROM)";
}

// === Checkpoint =============================================================
// VRAM is a private calloc (card_init), not part of the RAM image
// memory_map_checkpoint saves, so without these a restored machine comes back
// with a blank screen and a default palette until the guest happens to redraw.
//
// The four pointer members (card, vram, vrom, vrom_path) and the construction
// facts beside them (vrom_size, slot_base) are deliberately NOT in the stream:
// they are rebuilt by card_init before the restore runs, and writing them back
// from a checkpoint would install stale addresses.
//
// Save and restore share ONE field list, walked in both directions.  Two
// hand-mirrored lists are how a checkpoint stream silently goes out of step.
static void card_checkpoint_save(nubus_card_t *card, checkpoint_t *cp) {
    jmfb_priv_t *p = card ? card->priv : NULL;
    if (!p)
        return;
    system_write_checkpoint_data(cp, p->vram, JMFB_VRAM_SIZE);
    system_write_checkpoint_data(cp, p, offsetof(jmfb_priv_t, display));
    {
        // Fixed widths, not a raw struct prefix: the prefix carried a bare
        // pixel_format_t, whose size is implementation-defined (04-video
        // F-41; see display.h).
        display_head_t head = display_head_of(&p->display);
        system_write_checkpoint_data(cp, &head, sizeof head);
    }
}

static void card_checkpoint_restore(nubus_card_t *card, checkpoint_t *cp) {
    jmfb_priv_t *p = card ? card->priv : NULL;
    if (!p)
        return;
    system_read_checkpoint_data(cp, p->vram, JMFB_VRAM_SIZE);
    system_read_checkpoint_data(cp, p, offsetof(jmfb_priv_t, display));
    {
        display_head_t head;
        system_read_checkpoint_data(cp, &head, sizeof head);
        display_head_apply(&p->display, &head);
    }

    // stride, width and the scan base are all derived from row_words,
    // video_base and the restored format -- and the restore must land on a
    // descriptor VRAM can back, the same as any register write would.
    jmfb_apply_scanout(&p->regs, &p->bind);

    // display.bits still points into p->vram (card_init set it and the buffer
    // has not moved), but everything the frontend caches about this display is
    // now stale.
    p->display.shape_dirty = true;
    p->display.clut_dirty = true;
    p->display.fb_dirty = true;
    p->display.response_dirty = true;
}

static const nubus_card_ops_t mdc_8_24_ops = {
    .init = card_init_real,
    .teardown = card_teardown,
    .on_vbl = card_on_vbl,
    .display = card_display,
    .name = card_name,
    .checkpoint_save = card_checkpoint_save,
    .checkpoint_restore = card_checkpoint_restore,
};

static const nubus_card_ops_t jmfb_generic_ops = {
    .init = card_init_generic,
    .teardown = card_teardown,
    .on_vbl = card_on_vbl,
    .display = card_display,
    .name = card_name_generic,
    .checkpoint_save = card_checkpoint_save,
    .checkpoint_restore = card_checkpoint_restore,
};

// === Factory + kind descriptor ==============================================

// Monitor types the Rev B ROM supports (proposal §3.2.5 + the mode
// catalog Apple ships in chip[$4000..$502B] of the JMFB VROM).
//
// `depths` lists the supported bit-depths that are PRAM-reachable on
// this card — i.e. modes the user could pick in the Monitors control
// panel and have survive a reboot.  24 bpp is *not* listed: its
// spDepth4 entry only exists in the INACTIVE Ax-pair sister sResources
// and `_SlotManager $06 sReadFHeader` rejects them at boot.  Apple's
// "Millions of Colors" mode was runtime-only on this card.
//
// `sense_code` is the value the card's monitor-sense lines report
// when this display is connected.  `srsrc_sister` is the top-level
// "Ax" sister sResource ID that PrimaryInit selects for this
// monitor (the ACTIVE list when 32-bit QuickDraw isn't loaded; on
// stock System 7.0.1 we stay on Ax).  Both bytes were verified
// empirically by the iicx-video-modes integration test.
// 13" RGB is listed first so it's the catalog default — matches the
// JMFB's cold-init sense ($6 = 13" RGB) when no monitor is explicitly
// chosen.  monitor_for_sense() looks up by sense_code, not list order.
static const int mdc_8_24_4depths[] = {1, 2, 4, 8, 0};

// CRT response curves for monitors whose JMFB gamma table is NOT
// near-identity.  Mac System 7's JMFB driver gamma-pre-corrects CLUT
// writes for each monitor via Apple's per-display 'gama' resource (six tables at
// chip[$4A86..$502B] of the JMFB VROM).  On real hardware the CRT phosphor/electron-
// gun gamma response cancels the pre-correction and the user sees a
// neutral image.  In software we apply the inverse here.
//
// 13"/12" RGB and Portrait B&W happen to ship with near-identity
// gamma tables — their CLUT entries come out as true grays in our
// trace, so leaving `crt_response = NULL` (identity) renders the
// emulated framebuffer correctly with no further work.
//
// 21" RGB Kong ships with a deliberately B-channel-attenuated gamma
// table (Kong's blue phosphor was more efficient than R/G, so Apple
// pre-multiplied B by ~0.84 to compensate; the CRT then boosts B
// back by ~1.19 for a neutral display).  Without modelling the
// Kong CRT here, the page shows a yellow-tinted screenshot.
//
// Provenance: the three tables below were derived offline by booting
// the IIcx at sense=$0 (Kong) and sense=$6 (13" RGB) and capturing
// the gamma-pre-corrected CLUTs Mac OS uploads via the JMFB driver.
// Treating the 13" RGB CLUT as the perceptually-neutral ground truth
// (its gamma is near-identity in our trace), the per-channel inverse
// LUT for Kong is:
//     kong_crt_response_R[kong_CLUT_R[i]] = rgb13_CLUT_R[i]
//     (and same for G, B).
// Round-trip self-check confirmed 256/256 sample indices recover the
// 13"-RGB-equivalent neutral grays within ±1 byte.  See the offline
// derivation in this commit's discussion thread.
static const uint8_t kong_crt_response[3][256] = {
    // R
    {0x00, 0x01, 0x02, 0x03, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x14, 0x15,
     0x16, 0x17, 0x18, 0x19, 0x1A, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x24, 0x25, 0x26, 0x27, 0x28, 0x28, 0x29,
     0x2A, 0x2B, 0x2B, 0x2C, 0x2D, 0x2D, 0x2E, 0x2F, 0x2F, 0x30, 0x31, 0x32, 0x32, 0x33, 0x34, 0x34, 0x35, 0x36, 0x37,
     0x37, 0x38, 0x39, 0x39, 0x3A, 0x3B, 0x3B, 0x3C, 0x3D, 0x3E, 0x3E, 0x3F, 0x40, 0x42, 0x43, 0x44, 0x46, 0x47, 0x48,
     0x4A, 0x4B, 0x4C, 0x4D, 0x4F, 0x50, 0x51, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5E, 0x5F,
     0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x70, 0x71, 0x72, 0x73,
     0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86,
     0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
     0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC,
     0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF,
     0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF, 0xD0, 0xD1, 0xD2,
     0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5,
     0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xED, 0xEE, 0xEF, 0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7,
     0xF8, 0xF9, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF},
    // G
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
     0x11, 0x12, 0x13, 0x14, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21,
     0x22, 0x23, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33,
     0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x42, 0x43, 0x44, 0x46, 0x47, 0x48,
     0x4A, 0x4B, 0x4C, 0x4D, 0x4F, 0x50, 0x51, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5E, 0x5F,
     0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72, 0x74,
     0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
     0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9B, 0x9C,
     0x9D, 0x9E, 0x9F, 0xA0, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1,
     0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC0, 0xC1, 0xC2, 0xC3, 0xC4,
     0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF, 0xD0, 0xD1, 0xD2, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8,
     0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE0, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC,
     0xED, 0xED, 0xEE, 0xEF, 0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE,
     0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
    // B
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12,
     0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
     0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x3A,
     0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x41, 0x42, 0x44, 0x45, 0x47, 0x49, 0x4A, 0x4C, 0x4E, 0x4F, 0x51, 0x52, 0x54, 0x55,
     0x57, 0x58, 0x5A, 0x5B, 0x5C, 0x5E, 0x5F, 0x60, 0x62, 0x63, 0x65, 0x66, 0x67, 0x68, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E,
     0x70, 0x71, 0x72, 0x73, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7B, 0x7C, 0x7D, 0x7E, 0x80, 0x81, 0x82, 0x83, 0x85, 0x86,
     0x87, 0x88, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x90, 0x91, 0x92, 0x94, 0x95, 0x96, 0x97, 0x98, 0x9A, 0x9B, 0x9C, 0x9E,
     0x9F, 0xA0, 0xA1, 0xA2, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4,
     0xB5, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBE, 0xBF, 0xC0, 0xC1, 0xC2, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCB,
     0xCC, 0xCD, 0xCE, 0xD0, 0xD1, 0xD2, 0xD4, 0xD5, 0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDE, 0xDF, 0xE0, 0xE1, 0xE2,
     0xE3, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
     0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
     0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
     0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
};
static const nubus_monitor_t mdc_8_24_monitors[] = {
    {.id = "13in_rgb",
     .name = "13\" AppleColor",
     .width = 640,
     .height = 480,
     .depths = mdc_8_24_4depths,
     .sense_code = 0x6,
     .srsrc_sister = 0xA6},
    {.id = "12in_rgb",
     .name = "12\" RGB",
     .width = 512,
     .height = 384,
     .depths = mdc_8_24_4depths,
     .sense_code = 0x2,
     .srsrc_sister = 0xA2},
    {.id = "15in_bw",
     .name = "15\" Portrait B&W",
     .width = 640,
     .height = 870,
     .depths = mdc_8_24_4depths,
     .sense_code = 0x1,
     .srsrc_sister = 0xA1},
    {.id = "21in_rgb",
     .name = "21\" RGB",
     .width = 1152,
     .height = 870,
     .depths = mdc_8_24_4depths,
     .sense_code = 0x0,
     .srsrc_sister = 0xA7,
     .crt_response = kong_crt_response},
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

// (s_pending_sense defined near the top of this
// file alongside the matching forward declarations.)

void jmfb_pending_sense_set(uint8_t sense) {
    s_pending_sense = sense & 7;
}

uint8_t jmfb_pending_sense_get(void) {
    return s_pending_sense;
}

void jmfb_pending_video_mode_set(const char *id) {
    if (!id || !*id) {
        s_pending_video_mode_id[0] = '\0';
        return;
    }
    snprintf(s_pending_video_mode_id, sizeof s_pending_video_mode_id, "%s", id);
}

void jmfb_pending_custom_mode_set(const char *spec) {
    if (!spec || !*spec) {
        s_pending_custom_mode[0] = '\0';
        return;
    }
    snprintf(s_pending_custom_mode, sizeof s_pending_custom_mode, "%s", spec);
}

// Parse "monitor_Nbpp" into (monitor, N).  monitor portion is matched
// case-sensitively against entries in mdc_8_24_monitors[]; N is parsed
// as a decimal integer and validated against the monitor's depth list.
bool jmfb_video_mode_lookup(const char *id, const nubus_monitor_t **out_monitor, int *out_depth_bpp) {
    return nubus_monitor_mode_lookup(mdc_8_24_monitors, id, out_monitor, out_depth_bpp);
}

const nubus_card_kind_t mdc_8_24_kind = {
    .id = "mdc_8_24",
    .display_name = "Apple Macintosh Display Card 8\xe2\x80\xa2"
                    "24",
    .attach = CARD_ATTACH_NUBUS,
    .requires_vrom = true,
    .monitors = mdc_8_24_monitors,
    .ops = &mdc_8_24_ops,
    .stage_video_mode = jmfb_pending_video_mode_set,
};

// Generic sibling kind: always-available twin of mdc_8_24 with a built-in
// declaration ROM — zero-configuration by construction (proposal-generic-
// nubus-vrom sec. 6.1).  The short id is what users type in boot documents.
const nubus_card_kind_t jmfb_generic_kind = {
    .id = "8_24",
    .display_name = "Apple Macintosh Display Card 8\xe2\x80\xa2"
                    "24 (generic video ROM)",
    .attach = CARD_ATTACH_NUBUS,
    .requires_vrom = false,
    // ONE table for both siblings.  The generic copy repeated all four rows to
    // drop a single field (21" Kong's crt_response), and the copy was already
    // redundant: card_init's `(!generic && monitor) ? monitor->crt_response
    // : NULL` decides identity gamma from the kind, not from the row (04-video
    // F-08).  Two tables feeding one GS vROM generator is how a geometry fix
    // lands on one sibling and not the other.
    .monitors = mdc_8_24_monitors,
    .ops = &jmfb_generic_ops,
    .stage_video_mode = jmfb_pending_video_mode_set,
};
