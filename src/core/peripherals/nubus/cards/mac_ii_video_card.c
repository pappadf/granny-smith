// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mac_ii_video_card.c
// Apple Macintosh II Video Card ("Toby").  See mac_ii_video_card.h for the
// card, and docs/reference/hardware/nubus/cards/mac-ii-video-card.md for the
// hardware.
//
// What is modelled, and where it comes from (Apple's TFBDepVideoEqu.a,
// TFBPrimaryInit.a and TFBDriver.a describe every access the card's own
// software makes):
//
//   * VRAM, 512 KB at the bottom of the window, as plain memory.
//   * The TFB registers: sixteen bytes, one per long word.  The model keeps
//     all sixteen and acts on the depth (register 15, bits 5-4) and the page
//     base (registers 2/3, in long words).  The rest are CRT timing, which
//     the driver loads from a fixed table per depth; the raster is always
//     640 x 480.
//   * The Bt453: an address register and the palette, three writes (R, G, B)
//     per entry with auto-increment, readable the same way.
//   * The VBL interrupt: ClrVInt clears and enables it, DisableVInt masks it.
//   * The vertical-sync status, which the driver polls in WaitVSync.
//   * The declaration ROM, 4 KB on byte lane 0 at the slot top.
//
// The card has no monitor sense lines: it drives one monitor.

#include "mac_ii_video_card.h"

#include "card.h"
#include "checkpoint.h"
#include "declrom.h"
#include "display.h"
#include "display_ramdac.h"
#include "log.h"
#include "memory.h"
#include "nubus.h"
#include "system.h"
#include "system_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("video");

// === Per-card state =========================================================

// Field order IS the checkpoint format, as in jmfb.c: the scalars come first
// and the checkpoint is one range ending at `display`.
typedef struct {
    // The TFB's sixteen registers, as the chip holds them (the bus byte
    // complemented).
    uint8_t tfb[16];
    // The Bt453's palette, as the chip holds it, indexed by the chip's own
    // address -- which for a pixel is the complemented VRAM value, left-aligned
    // in the 8-bit index (see rebuild_display_clut).
    uint8_t ramdac[256][3];
    ramdac_seq_t ramdac_seq; // the Bt453 address register and R/G/B phase
    uint8_t vint_enabled; // ClrVInt enables, DisableVInt masks
    uint8_t vint_pending; // the slot line is asserted
    uint8_t vsync; // RdVSync bit 0, toggled per guest read
    // Scanning out black until the driver first programs the page base after
    // a /RESET or power-on: the TFB comes up with no picture.
    uint8_t blanked;
    uint8_t pad[2];

    // --- Pointers and construction facts last; NOT part of the range above ---
    display_t display;
    rgba8_t clut[256]; // the palette by guest pixel value (rebuilt; not saved)
    nubus_card_t *card;
    uint8_t *vram;
    uint8_t *blank; // the black raster a blanked card scans out
    size_t blank_size;
    uint32_t slot_base;
} toby_t;

_Static_assert(offsetof(toby_t, display) < offsetof(toby_t, card), "toby checkpoint range must end before pointers");

// === Display ================================================================

static unsigned depth_log2(const toby_t *t) {
    return (t->tfb[TFB_REG_MODE] >> 4) & 3u;
}

static const pixel_format_t k_formats[4] = {PIXEL_1BPP_MSB, PIXEL_2BPP_MSB, PIXEL_4BPP_MSB, PIXEL_8BPP};

// The palette by the pixel value the guest wrote.  VRAM is held as the CPU
// sees it, but the RAMDAC is fed the bits the TFB scans out of the inverted
// array: a d-bit pixel p reaches the Bt453 as ~p, in the top d bits of its
// 8-bit address with zeros below.  That is the inverse of the mapping the
// driver uses to address an entry (TFBDriver.a, CvtIndex: the index rotated
// into the top bits with ones below, then complemented by the bus), so entry
// i as the driver sets it is the colour of pixel value i.
static void rebuild_display_clut(toby_t *t) {
    unsigned d = 1u << depth_log2(t);
    unsigned n = 1u << d, mask = n - 1u;
    for (unsigned p = 0; p < 256; p++) {
        unsigned chip = ((~p & mask) << (8u - d)) & 0xFFu;
        t->clut[p] = (rgba8_t){t->ramdac[chip][0], t->ramdac[chip][1], t->ramdac[chip][2], 255};
    }
    t->display.clut_dirty = true;
}

// Point the descriptor at the page the registers name, at the depth they
// name.  The stride is fixed by the depth: the TFB scans a 1024-pixel line.
static void apply_scanout(toby_t *t) {
    unsigned lg = depth_log2(t);
    pixel_format_t fmt = k_formats[lg];
    if (t->display.format != fmt)
        t->display.shape_dirty = true;
    t->display.format = fmt;
    uint32_t stride = 128u << lg;
    uint32_t base = (((uint32_t)t->tfb[TFB_REG_BASEHI] << 8) | t->tfb[TFB_REG_BASELO]) * 4u;
    const uint8_t *old = t->display.bits;
    uint32_t old_stride = t->display.stride;
    display_set_scanout(&t->display, t->blanked ? NULL : t->vram, TOBY_VRAM_SIZE, base, stride, TOBY_WIDTH, TOBY_HEIGHT,
                        t->blank, t->blank_size);
    if (t->display.bits != old || t->display.stride != old_stride)
        t->display.shape_dirty = true;
    t->display.fb_dirty = true;
    rebuild_display_clut(t);
}

// === Bus access =============================================================

// A 68020/030 smears a narrow write across the byte lanes it does not drive
// (a byte on all four, a word on both halves), which is how Apple's driver
// reaches registers on lane 3 with byte writes at lane 0 (TFBDepVideoEqu.a,
// <SM2>).  Return the long word as the card sees it on the bus.
static uint32_t smear(uint32_t val, unsigned size) {
    if (size == 1)
        return (val & 0xFFu) * 0x01010101u;
    if (size == 2)
        return (val & 0xFFFFu) * 0x00010001u;
    return val;
}

static void ramdac_write(toby_t *t, unsigned reg, uint8_t v) {
    switch (reg) {
    case 3: // wCLUTAddReg / rCLUTAddReg: the address, restarting at R
        ramdac_seq_set_waddr(&t->ramdac_seq, v);
        break;
    case 2: { // wCLUTDataReg: R, G, B, then on to the next entry
        uint8_t idx;
        if (ramdac_seq_write(&t->ramdac_seq, v, &idx)) {
            memcpy(t->ramdac[idx], t->ramdac_seq.pending, 3);
            rebuild_display_clut(t);
        }
        break;
    }
    default:
        LOG(2, "TFB: RAMDAC write to register %u = $%02X (unmodelled)", reg, v);
        break;
    }
}

static uint8_t ramdac_read(toby_t *t, unsigned reg, bool peek) {
    switch (reg) {
    case 3:
        return ramdac_seq_addr(&t->ramdac_seq);
    case 2: {
        uint8_t comp;
        uint8_t idx = ramdac_seq_read(&t->ramdac_seq, peek, &comp);
        return t->ramdac[idx][comp];
    }
    default:
        return 0;
    }
}

// One write to the register blocks.  `rel` is the offset from TOBY_TFB_BASE
// within the 1 MB window; `bus` the long word on the bus.
static void io_write(toby_t *t, uint32_t rel, uint32_t bus) {
    uint32_t a = (TOBY_TFB_BASE + rel) & 0xFFFFCu;
    uint32_t chip = ~bus; // the card's data bus is inverted
    switch (a & 0xF0000u) {
    case TOBY_TFB_BASE: {
        unsigned reg = (a >> 2) & 0xFu;
        t->tfb[reg] = (uint8_t)chip; // lane 3
        if (reg == TFB_REG_BASEHI || reg == TFB_REG_BASELO) {
            t->blanked = 0;
            apply_scanout(t);
        } else if (reg == TFB_REG_MODE) {
            apply_scanout(t);
        }
        break;
    }
    case TOBY_CLUT_BASE:
        ramdac_write(t, (a >> 2) & 3u, (uint8_t)(chip >> 24)); // lane 0
        break;
    case TOBY_CLRVINT & 0xF0000u:
        if (a & 4u) { // DisableVInt
            t->vint_enabled = 0;
        } else { // ClrVInt
            t->vint_enabled = 1;
        }
        if (t->vint_pending) {
            t->vint_pending = 0;
            nubus_deassert_irq(t->card);
        }
        break;
    default:
        LOG(2, "TFB: write at +$%05X = $%08X (unmodelled)", a, bus);
        break;
    }
}

// One read: the byte the addressed register drives, as the CPU sees it (the
// card drives the same byte on every lane of a register read).
static uint8_t io_read(toby_t *t, uint32_t rel, bool peek) {
    uint32_t a = (TOBY_TFB_BASE + rel) & 0xFFFFCu;
    switch (a & 0xF0000u) {
    case TOBY_CLUT_BASE:
        return (uint8_t)~ramdac_read(t, (a >> 2) & 3u, peek);
    case TOBY_RDVSYNC & 0xF0000u:
        if (a == TOBY_RDVSYNC) {
            // The driver waits for bit 0 to read low and then high: the
            // start of a blanking period.  Like the JMFB's Stopwatch status
            // (jmfb_family.c) it flips on every read, so the wait completes
            // in a few reads and stays deterministic.
            if (!peek)
                t->vsync ^= 1u;
            return t->vsync ? 0xFFu : 0xFEu;
        }
        if (a == TOBY_RDVINT)
            return t->vint_pending ? 0x00u : 0xFFu;
        return 0xFFu;
    default:
        return 0xFFu; // the TFB registers are write-only
    }
}

static uint8_t io_read8(void *dev, uint32_t addr) {
    return io_read(dev, addr, false);
}
static uint16_t io_read16(void *dev, uint32_t addr) {
    uint8_t v = io_read(dev, addr, false);
    return (uint16_t)(v * 0x0101u);
}
static uint32_t io_read32(void *dev, uint32_t addr) {
    uint8_t v = io_read(dev, addr, false);
    return v * 0x01010101u;
}
static uint8_t io_peek8(void *dev, uint32_t addr) {
    return io_read(dev, addr, true);
}
static uint16_t io_peek16(void *dev, uint32_t addr) {
    return (uint16_t)(io_read(dev, addr, true) * 0x0101u);
}
static uint32_t io_peek32(void *dev, uint32_t addr) {
    return io_read(dev, addr, true) * 0x01010101u;
}
static void io_write8(void *dev, uint32_t addr, uint8_t val) {
    io_write(dev, addr, smear(val, 1));
}
static void io_write16(void *dev, uint32_t addr, uint16_t val) {
    io_write(dev, addr, smear(val, 2));
}
static void io_write32(void *dev, uint32_t addr, uint32_t val) {
    io_write(dev, addr, val);
}

static const memory_interface_t s_toby_iface = {
    .read_uint8 = io_read8,
    .read_uint16 = io_read16,
    .read_uint32 = io_read32,
    .write_uint8 = io_write8,
    .write_uint16 = io_write16,
    .write_uint32 = io_write32,
    .peek_uint8 = io_peek8,
    .peek_uint16 = io_peek16,
    .peek_uint32 = io_peek32,
};

// === Card vtable ============================================================

// The card decodes only 20 address bits, so its 1 MB window repeats across
// the slot.  The machine has a small, fixed number of host regions, so not
// every repeat is mapped -- only those the Mac OS reaches: the window at the
// slot base (where the 32-bit Slot Manager and A/UX address it), the one at
// +$900000 (a 24-bit $9xxxxx pointer used as a 32-bit address, as on the
// JMFB -- see jmfb.c), and the declaration ROM at the slot top.
static void map_regions(toby_t *t, config_t *cfg) {
    memory_map_host_region(cfg->memory_map, "toby_vram", t->vram, t->slot_base, TOBY_VRAM_SIZE, true);
    memory_map_host_region(cfg->memory_map, "toby_declrom", t->card->declrom,
                           t->slot_base + 0x1000000u - TOBY_DECLROM_BUS_SIZE, TOBY_DECLROM_BUS_SIZE, false);
    memory_map_host_region_alias(cfg->memory_map, t->slot_base + 0x900000u, t->slot_base);
    memory_map_add(cfg->memory_map, t->slot_base + TOBY_TFB_BASE, TOBY_IO_END - TOBY_TFB_BASE, "TFB regs",
                   &s_toby_iface, t);
    memory_map_add(cfg->memory_map, t->slot_base + 0x900000u + TOBY_TFB_BASE, TOBY_IO_END - TOBY_TFB_BASE,
                   "TFB regs (24-bit alias)", &s_toby_iface, t);
}

// The chip's power-on state: no picture until the driver programs it, the
// VBL interrupt masked, 1 bpp, a palette of black.
static void power_on_registers(toby_t *t) {
    memset(t->tfb, 0, sizeof t->tfb);
    ramdac_seq_init(&t->ramdac_seq, 0);
    t->vint_enabled = 0;
    t->vint_pending = 0;
    t->vsync = 0;
    t->blanked = 1;
}

static int card_init(nubus_card_t *card, config_t *cfg, checkpoint_t *cp, const slot_opts_t *opts) {
    (void)cp;
    (void)opts;
    toby_t *t = calloc(1, sizeof *t);
    if (!t)
        return -1;
    t->card = card;
    t->slot_base = nubus_slot_base(card->slot);
    t->vram = calloc(1, TOBY_VRAM_SIZE);
    t->blank_size = (size_t)(128u << 3) * TOBY_HEIGHT; // the widest stride
    t->blank = calloc(1, t->blank_size);
    if (!t->vram || !t->blank) {
        free(t->vram);
        free(t->blank);
        free(t);
        return -1;
    }
    // The declaration ROM is card->declrom, seated by the bus (declrom_seat)

    power_on_registers(t);
    t->display.format = PIXEL_1BPP_MSB;
    t->display.clut = t->clut;
    t->display.clut_len = 256;
    apply_scanout(t);
    t->display.shape_dirty = true;
    t->display.response_dirty = true;

    card->priv = t;
    map_regions(t, cfg);
    return 0;
}

static void card_teardown(nubus_card_t *card, config_t *cfg) {
    (void)cfg;
    toby_t *t = card->priv;
    if (!t)
        return;
    free(t->vram);
    free(t->blank);
    free(t);
    card->priv = NULL;
}

// NuBus /RESET: the TFB and the RAMDAC return to their power-on state; VRAM
// and the palette contents are kept.
static void card_reset(nubus_card_t *card, config_t *cfg) {
    (void)cfg;
    toby_t *t = card->priv;
    if (!t)
        return;
    nubus_deassert_irq(card);
    power_on_registers(t);
    apply_scanout(t);
}

// Power cycle: VRAM and the palette are lost.
static void card_power_on(nubus_card_t *card, config_t *cfg) {
    toby_t *t = card->priv;
    if (!t)
        return;
    memset(t->vram, 0, TOBY_VRAM_SIZE);
    memset(t->ramdac, 0, sizeof t->ramdac);
    card_reset(card, cfg);
}

static void card_on_vbl(nubus_card_t *card, config_t *cfg) {
    (void)cfg;
    toby_t *t = card->priv;
    if (!t)
        return;
    if (t->vint_enabled && !t->vint_pending) {
        t->vint_pending = 1;
        nubus_assert_irq(card);
    }
    // VRAM is written straight through its host mapping; surface it once a
    // frame.
    t->display.fb_dirty = true;
}

static display_t *card_display(nubus_card_t *card) {
    toby_t *t = card->priv;
    return t ? &t->display : NULL;
}

// The guest physical address of the first displayed byte: the VRAM sits at
// the bottom of the card's slot space, and the TFB base registers pick the
// page scanned.  0 while the blank is scanned.
static uint64_t card_fb_base(nubus_card_t *card) {
    toby_t *t = card->priv;
    return t ? display_guest_addr(&t->display, t->vram, TOBY_VRAM_SIZE, t->slot_base) : 0;
}

// The checkpointed plain-data prefix, up to the first host pointer
CHECKPOINT_PREFIX(toby_t, display);

static void card_checkpoint_save(nubus_card_t *card, checkpoint_t *cp) {
    toby_t *t = card ? card->priv : NULL;
    if (!t)
        return;
    system_write_checkpoint_data(cp, t->vram, TOBY_VRAM_SIZE);
    checkpoint_save_prefix(cp, t, toby_t);
}

static void card_checkpoint_restore(nubus_card_t *card, checkpoint_t *cp) {
    toby_t *t = card ? card->priv : NULL;
    if (!t)
        return;
    system_read_checkpoint_data(cp, t->vram, TOBY_VRAM_SIZE);
    checkpoint_load_prefix(cp, t, toby_t);
    if (t->vint_pending)
        nubus_assert_irq(card);
    apply_scanout(t);
    t->display.shape_dirty = true;
    t->display.response_dirty = true;
}

static const nubus_card_ops_t mac_ii_video_card_ops = {
    .init = card_init,
    .reset = card_reset,
    .power_on = card_power_on,
    .teardown = card_teardown,
    .on_vbl = card_on_vbl,
    .display = card_display,
    .fb_base = card_fb_base,
    .checkpoint_save = card_checkpoint_save,
    .checkpoint_restore = card_checkpoint_restore,
};

// === Kind descriptor ========================================================

static const int toby_depths[] = {1, 2, 4, 8, 0};

// The one monitor the card drives: Apple's 13" RGB (640 x 480, 66.7 Hz).
// The card has no sense lines; the code is the 13" RGB's for the catalogue.
static const nubus_monitor_t toby_monitors[] = {
    {.id = "13in_rgb", .monitor = "13in_rgb", .width = 640, .height = 480, .depths = toby_depths, .sense_code = 0x6},
    {0},
};

// The slot's startup-mode record, the sPRAMRec the Monitors control panel
// saves: BoardID $0005 (TFBBoardId), savedMode (spDepth $80 + log2 of the
// depth), and the video sResource -- sRsrcVideo8 ($80), the one a card with
// the 512 KB expansion kit keeps -- as savedSRsrcID and savedRawSRsrcID.
static bool toby_startup_record(const slot_opts_t *e, uint8_t rec[8]) {
    const nubus_monitor_t *row = NULL;
    int depth = 0;
    if (!e->video_mode[0] || !nubus_monitor_mode_lookup(toby_monitors, e->video_mode, &row, &depth))
        return false;
    uint8_t sp_depth = nubus_spdepth_for_bpp(depth, 0x80);
    const uint8_t r[8] = {0x00, 0x05, sp_depth, 0x80, 0x80, 0, 0, 0};
    memcpy(rec, r, sizeof r);
    return true;
}

const nubus_card_kind_t mac_ii_video_card_kind = {
    .id = "mac_ii_video_card",
    .display_name = "Macintosh II Video Card",
    .attach = CARD_ATTACH_NUBUS,
    .requires_vrom = true,
    .substitute = false,
    .monitors = toby_monitors,
    .declrom_bus_size = TOBY_DECLROM_BUS_SIZE,
    .ops = &mac_ii_video_card_ops,
    .startup_record = toby_startup_record,
};
