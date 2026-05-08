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
#include "system.h"
#include "system_config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("jmfb");

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

// Recompute display.stride from JMFBRowWords + current format.
static void recompute_stride(jmfb_priv_t *p) {
    if (p->display.format == PIXEL_32BPP_XRGB)
        p->display.stride = (uint32_t)p->jmfb_row_words * 32u / 3u;
    else
        p->display.stride = (uint32_t)p->jmfb_row_words * 4u;
}

// === Memory interface (register window I/O) =================================

// Translate a physical address inside the register window into a (block,
// in-block-offset) pair the dispatch can switch over.  Returns -1 if the
// address falls outside any block.
static int classify(uint32_t phys, uint32_t slot_base, uint32_t *out_off) {
    uint32_t window = phys - slot_base;
    if (window < JMFB_BLOCK_OFFSET || window >= JMFB_BLOCK_OFFSET + 0x400u)
        return -1;
    uint32_t off_in_window = window - JMFB_BLOCK_OFFSET;
    *out_off = off_in_window & 0xFFu; // each block is 256 bytes
    return (int)(off_in_window >> 8); // 0=JMFB, 1=Stopwatch, 2=CLUT, 3=Endeavor
}

static void handle_jmfb_write16(jmfb_priv_t *p, uint32_t off, uint16_t val) {
    switch (off) {
    case JMFBCSR:
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
        p->jmfb_lsr = val;
        LOG(3, "JMFBLSR write %04x (accept-and-log)", val);
        return;
    case JMFBVideoBase:
        p->jmfb_video_base = val;
        // Encoded value × 32 = byte offset into VRAM
        p->display.bits = p->vram + ((size_t)val * 32u);
        p->display.generation++;
        return;
    case JMFBRowWords:
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
        // Real hardware: sense lines drive bits 9-11 (the bits NOT in
        // MaskSenseLine).  PrimaryInit reads this value to pick the
        // active monitor's mode table.
        return (uint16_t)((p->jmfb_csr & MaskSenseLine) | ((p->sense_code & 7) << 9));
    case JMFBLSR:
        return p->jmfb_lsr;
    case JMFBVideoBase:
        return p->jmfb_video_base;
    case JMFBRowWords:
        return p->jmfb_row_words;
    default:
        LOG(2, "JMFB block read at +%02x (unmodeled, returning 0)", off);
        return 0;
    }
}

static void handle_stopwatch_write16(jmfb_priv_t *p, uint32_t off, uint16_t val) {
    switch (off) {
    case SWICReg:
        p->sw_ic_reg = val;
        if (val & SRST)
            LOG(2, "SWICReg: soft reset");
        // ENVERTI: 1 = vertical interrupts enabled.  Card decides whether
        // to call nubus_assert_irq based on this in on_vbl.
        return;
    case SWClrVInt:
        // Write any value clears the pending VBL and de-asserts the
        // slot's IRQ on the bus controller.
        nubus_deassert_irq(p->card);
        return;
    case SWStatusReg:
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
        // Return a toggling pattern that satisfies the driver's poll for
        // "VBL ticked".  Two-state value flipped each call mirrors what
        // the real chip's vertical-toggle bit does.
        p->sw_status_reg ^= 0x0001u;
        return p->sw_status_reg;
    case SWICReg:
        return p->sw_ic_reg;
    default:
        LOG(2, "Stopwatch block read at +%02x (unmodeled)", off);
        return 0;
    }
}

static void handle_clut_write16(jmfb_priv_t *p, uint32_t off, uint16_t val) {
    switch (off) {
    case CLUTAddrReg:
        // 8·24 maps the index into the low byte; a write resets the R/G/B
        // sub-counter so the next three CLUTDataReg writes load the new
        // entry.
        p->clut_idx = (uint8_t)(val & 0xFFu);
        p->clut_phase = 0;
        return;
    case CLUTDataReg: {
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
    case CLUTPBCR: {
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
        return p->clut_idx;
    case CLUTPBCR:
        return p->clut_pbcr;
    default:
        LOG(2, "CLUT block read at +%02x (unmodeled)", off);
        return 0;
    }
}

static void handle_endeavor_write16(jmfb_priv_t *p, uint32_t off, uint16_t val) {
    switch (off) {
    case EndeavorM:
        p->endeavor_m = val;
        break;
    case EndeavorN:
        p->endeavor_n = val;
        break;
    case EndeavorExtClkSel:
        p->endeavor_ext_clk = val;
        break;
    case EndeavorReserved:
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
        return p->endeavor_m;
    case EndeavorN:
        return p->endeavor_n;
    case EndeavorExtClkSel:
        return p->endeavor_ext_clk;
    case EndeavorReserved:
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
// search paths.  Returns true on success.
static bool try_load_vrom(const char *path, uint8_t *buf) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    size_t n = fread(buf, 1, JMFB_DECLROM_SIZE, f);
    fclose(f);
    return n == JMFB_DECLROM_SIZE;
}

static bool load_vrom(jmfb_priv_t *p) {
    static const char *search_paths[] = {
        "/opfs/images/vrom/Apple-341-0868.vrom",
        "tests/data/roms/Apple-341-0868.vrom",
        "Apple-341-0868.vrom",
        NULL,
    };
    for (const char **q = search_paths; *q; q++) {
        if (try_load_vrom(*q, p->vrom)) {
            free(p->vrom_path);
            p->vrom_path = strdup(*q);
            p->vrom_size = JMFB_DECLROM_SIZE;
            return true;
        }
    }
    return false;
}

static int card_init(nubus_card_t *card, config_t *cfg, checkpoint_t *cp) {
    (void)cp;
    jmfb_priv_t *p = calloc(1, sizeof(*p));
    if (!p)
        return -1;
    p->card = card;
    p->slot_base = nubus_slot_base(card->slot);

    p->vram = calloc(1, JMFB_VRAM_SIZE);
    p->vrom = calloc(1, JMFB_DECLROM_SIZE);
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

    // Default monitor sense code: 13" RGB (sense `110`).
    p->sense_code = 0x6;

    // Default register state from PrimaryInit's expected starting point.
    p->jmfb_csr = 0;
    p->jmfb_video_base = 0xA00 / 32; // driver convention: $A00 byte offset
    p->jmfb_row_words = 640 / 4; // 13" RGB at 1bpp; OS reprograms on cscSetMode

    // Display: start at 13" RGB / 8 bpp so the canvas comes up colour
    // (the OS then drives any depth change via cscSetMode).
    p->display.width = 640;
    p->display.height = 480;
    p->display.stride = 640;
    p->display.format = PIXEL_8BPP;
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
    memory_map_host_region(cfg->mem_map, "jmfb_declrom", p->vrom, p->slot_base + JMFB_DECLROM_OFFSET, JMFB_DECLROM_SIZE,
                           /*writable*/ false);
    memory_map_add(cfg->mem_map, p->slot_base + JMFB_BLOCK_OFFSET, JMFB_REGISTER_SIZE, "JMFB regs", &s_jmfb_mem_iface,
                   p);

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

const nubus_card_kind_t mdc_8_24_kind = {
    .id = "mdc_8_24",
    .display_name = "Apple Macintosh Display Card 8\xe2\x80\xa2"
                    "24",
    .requires_vrom = true,
    .monitors = mdc_8_24_monitors,
    .factory = factory,
};
