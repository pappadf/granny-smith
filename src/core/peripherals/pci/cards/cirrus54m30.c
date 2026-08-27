// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cirrus54m30.c
// The Cirrus Logic 54M30 — the Apple Network Server's on-board video, and
// the first REAL PCI framebuffer for an Old World Power Macintosh in this
// repository.  Not a Control/Valkyrie-style framebuffer hung off the memory
// controller: a discrete PCI SVGA part on Bandit 1 at IDSEL 15, with a
// standard VGA connector and DDC-2 rather than Apple's DA-15.
//
// Identity is settled three ways: the production ROM's own `54m30-config`
// word; the Alpine family technical reference manual, whose PCI ID reset
// value is `00A0h 1013h`; and AIX 4.1.5's driver fileset
// `devices.pci.pci1013+a0` ("Cirrus Graphics Adapter Software", driver
// `cirrusdd`, config method `cfgcirrus`), whose ODM PdDv record reads
// `devid = "pci1013,a0"`.  The die is a GD5430/5440-family part — the 1 MB
// framebuffer and the 1024x768 ceiling both fit the lower-end member, and
// the register map is family-wide either way.
//
// THE ENDIANNESS TRAP, and the gift inside it.  Apple:
//
// > "This controller implements only a little-endian window into the
// >  packed-pixel frame buffer, hence Big Endian operating systems are
// >  limited to 8 bits per pixel unless low-level transformation routines
// >  are written."
//
// At 8 bpp each pixel is ONE BYTE, so byte order does not matter, and the
// advertised 1024x768x8 maximum is a byte-order ceiling rather than a
// memory one (1 MB holds that with room to spare).  AIX drove the part at
// 8 bpp as its console framebuffer.  So the initial goal needs no new
// pixel format at all; deeper colour needs a little-endian framebuffer
// window in display_t and is a separate piece of work.
//
// NO EXPANSION ROM, AND NO INTERRUPT.  Unlike the Mach64 GX, this card's
// Open Firmware node is built by `54m30-config` in the MAIN ROM, so there
// is no FCode PROM to provision and the expansion-ROM BAR reads zero.  And
// Apple states plainly that the part has no interrupt line — allocating it
// a Grand Central external would corrupt the interrupt map.
//
// NO ACCELERATION.  Apple was candid about why the part was chosen: "Pure
// bit-mapped mode will undoubtedly be visibly slow… Screen savers should be
// discouraged for maximum system performance."  Period software drove it as
// a dumb framebuffer, so a plain linear model is faithful to how the
// machine was actually used.
//
// Register truth: Cirrus Logic, "Alpine VGA Family CL-GD543X/4X Technical
// Reference Manual", 4th ed. (Feb 1995), §4.14-§4.20.

#include "card.h"
#include "log.h"
#include "pci.h"
#include "system.h"
#include "system_config.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("54m30");

// === PCI identity (Alpine TRM §4.14-§4.17) ==================================
#define C54M30_VENDOR_ID 0x1013u // Cirrus Logic
#define C54M30_DEVICE_ID 0x00A0u // CL-GD5430 / CL-GD5440 (and Apple's "54M30")
// "No application program should ever take any action based on the
// contents of this field" (Alpine TRM §4.17), so zero is as good as
// anything and is what a reset part reports.
#define C54M30_REVISION 0x00u
#define C54M30_CLASS    0x030000u // display / VGA-compatible / no prog. interface

// BAR geometry.  Base Address Zero is the display-memory aperture, whose
// base occupies bits 31:24 — a contiguous 16 MB block, of which the fitted
// DRAM occupies the bottom.  Base Address One is the relocatable 512-byte
// VGA I/O range ('30/'40 only), whose base occupies bits 15:10.
#define C54M30_BAR_FB  0
#define C54M30_BAR_IO  1
#define C54M30_FB_SPAN 0x01000000u // the 16 MB aperture the chip decodes
#define C54M30_IO_SPAN 0x200u // 512 relocatable I/O bytes
#define C54M30_VRAM    0x00100000u // 1 MB of fitted DRAM (Apple, HDN §2.8)
#define C54M30_REGS    0x100u // the shadowed VGA/extension register file

static const pci_config_decl_t c54m30_decl = {
    .vendor_id = C54M30_VENDOR_ID,
    .device_id = C54M30_DEVICE_ID,
    .revision = C54M30_REVISION,
    .class_code = C54M30_CLASS,
    .header_type = 0x00u,
    // NO INTERRUPT LINE (Apple, HDN §4.2).  interrupt_pin stays 0, so the
    // device tree carries no routed `interrupts` property for this node.
    .interrupt_pin = 0u,
    .command_writable = PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE | PCI_CMD_MASTER,
    .bar =
        {
              [C54M30_BAR_FB] = {.size = C54M30_FB_SPAN, .kind = PCI_BAR_MEM_PREFETCH},
              [C54M30_BAR_IO] = {.size = C54M30_IO_SPAN, .kind = PCI_BAR_IO},
              },
    // No expansion ROM: this board fits none, and Open Firmware builds the
    // node from the main ROM's own `54m30-config`.
    .rom_size = 0,
};

// === Card state =============================================================
typedef struct c54m30 {
    pci_device_t *dev;
    config_t *cfg;
    uint8_t *vram; // C54M30_VRAM bytes of fitted display memory
    uint8_t reg[C54M30_REGS]; // the I/O-space register file, shadowed
    uint8_t seq_index; // sequencer index latch (I/O +$04)
    uint8_t crtc_index; // CRTC index latch (I/O +$14 in colour mode)
    uint8_t gr_index; // graphics-controller index latch (I/O +$0E)
    memory_interface_t fb_if;
    memory_interface_t io_if;
} c54m30_t;

// ============================================================
// The display-memory aperture
// ============================================================
// The chip decodes a contiguous 16 MB block and the fitted DRAM occupies
// the bottom 1 MB of it.  Above that the part drives nothing; reads return
// zero and writes vanish, which is what a sizing probe expects to find and
// keeps a runaway blit from scribbling outside the buffer.

static uint8_t fb_read8(void *ctx, uint32_t offset) {
    c54m30_t *c = (c54m30_t *)ctx;
    return offset < C54M30_VRAM ? c->vram[offset] : 0u;
}

static void fb_write8(void *ctx, uint32_t offset, uint8_t value) {
    c54m30_t *c = (c54m30_t *)ctx;
    if (offset < C54M30_VRAM)
        c->vram[offset] = value;
}

static uint16_t fb_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((fb_read8(ctx, offset) << 8) | fb_read8(ctx, offset + 1));
}

static void fb_write16(void *ctx, uint32_t offset, uint16_t value) {
    fb_write8(ctx, offset, (uint8_t)(value >> 8));
    fb_write8(ctx, offset + 1, (uint8_t)value);
}

static uint32_t fb_read32(void *ctx, uint32_t offset) {
    return ((uint32_t)fb_read16(ctx, offset) << 16) | fb_read16(ctx, offset + 2);
}

static void fb_write32(void *ctx, uint32_t offset, uint32_t value) {
    fb_write16(ctx, offset, (uint16_t)(value >> 16));
    fb_write16(ctx, offset + 2, (uint16_t)value);
}

// ============================================================
// The relocatable VGA I/O range
// ============================================================
// A 512-byte window carrying the classic VGA register file plus the Alpine
// extensions, all reached through index/data port pairs.  Shadowed
// store-and-readback: nothing on the boot path drives a mode through it —
// POST reads the PCI ID and stops, and Open Firmware's `54m30-config` builds
// the device-tree node — so the honest model records what a guest writes and
// logs, rather than inventing CRTC behaviour nothing has yet exercised.

static uint8_t io_read8(void *ctx, uint32_t offset) {
    c54m30_t *c = (c54m30_t *)ctx;
    return c->reg[offset & (C54M30_REGS - 1u)];
}

static void io_write8(void *ctx, uint32_t offset, uint8_t value) {
    c54m30_t *c = (c54m30_t *)ctx;
    c->reg[offset & (C54M30_REGS - 1u)] = value;
    LOG(4, "VGA I/O +$%03X = $%02X", offset, value);
}

static uint16_t io_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((io_read8(ctx, offset) << 8) | io_read8(ctx, offset + 1));
}

static void io_write16(void *ctx, uint32_t offset, uint16_t value) {
    io_write8(ctx, offset, (uint8_t)(value >> 8));
    io_write8(ctx, offset + 1, (uint8_t)value);
}

static uint32_t io_read32(void *ctx, uint32_t offset) {
    return ((uint32_t)io_read16(ctx, offset) << 16) | io_read16(ctx, offset + 2);
}

static void io_write32(void *ctx, uint32_t offset, uint32_t value) {
    io_write16(ctx, offset, (uint16_t)(value >> 16));
    io_write16(ctx, offset + 2, (uint16_t)value);
}

// ============================================================
// Device lifecycle
// ============================================================

static const char *c54m30_name(const pci_device_t *dev) {
    (void)dev;
    return "Cirrus 54M30";
}

static void c54m30_reset(pci_device_t *dev, config_t *cfg) {
    (void)cfg;
    c54m30_t *c = (c54m30_t *)dev->priv;
    if (!c)
        return;
    // PCI RST# clears the register file; display memory is DRAM and its
    // contents are not defined by reset, so the buffer is left alone.
    memset(c->reg, 0, sizeof(c->reg));
    c->seq_index = 0;
    c->crtc_index = 0;
    c->gr_index = 0;
}

static void c54m30_teardown(pci_device_t *dev, config_t *cfg) {
    (void)cfg;
    c54m30_t *c = (c54m30_t *)dev->priv;
    if (!c)
        return;
    free(c->vram);
    free(c);
    dev->priv = NULL;
}

static void c54m30_checkpoint_save(pci_device_t *dev, checkpoint_t *cp) {
    c54m30_t *c = (c54m30_t *)dev->priv;
    if (!c || !cp)
        return;
    system_write_checkpoint_data(cp, c->reg, sizeof(c->reg));
    system_write_checkpoint_data(cp, c->vram, C54M30_VRAM);
}

static void c54m30_checkpoint_restore(pci_device_t *dev, checkpoint_t *cp) {
    c54m30_t *c = (c54m30_t *)dev->priv;
    if (!c || !cp)
        return;
    system_read_checkpoint_data(cp, c->reg, sizeof(c->reg));
    system_read_checkpoint_data(cp, c->vram, C54M30_VRAM);
}

static const pci_device_ops_t c54m30_ops = {
    .reset = c54m30_reset,
    .teardown = c54m30_teardown,
    .checkpoint_save = c54m30_checkpoint_save,
    .checkpoint_restore = c54m30_checkpoint_restore,
    .name = c54m30_name,
};

static pci_device_t *c54m30_factory(int slot_index, config_t *cfg, checkpoint_t *cp) {
    (void)cp;
    pci_device_t *dev = (pci_device_t *)calloc(1, sizeof(*dev));
    c54m30_t *c = (c54m30_t *)calloc(1, sizeof(*c));
    uint8_t *vram = (uint8_t *)calloc(1, C54M30_VRAM);
    if (!dev || !c || !vram) {
        free(dev);
        free(c);
        free(vram);
        return NULL;
    }
    dev->ops = &c54m30_ops;
    dev->decl = &c54m30_decl;
    dev->priv = c;
    pci_cfg_reset(dev);
    c->dev = dev;
    c->cfg = cfg;
    c->vram = vram;

    c->fb_if.read_uint8 = fb_read8;
    c->fb_if.read_uint16 = fb_read16;
    c->fb_if.read_uint32 = fb_read32;
    c->fb_if.write_uint8 = fb_write8;
    c->fb_if.write_uint16 = fb_write16;
    c->fb_if.write_uint32 = fb_write32;
    c->io_if.read_uint8 = io_read8;
    c->io_if.read_uint16 = io_read16;
    c->io_if.read_uint32 = io_read32;
    c->io_if.write_uint8 = io_write8;
    c->io_if.write_uint16 = io_write16;
    c->io_if.write_uint32 = io_write32;

    pci_bar_backing_iface(dev, C54M30_BAR_FB, &c->fb_if, c);
    pci_bar_backing_iface(dev, C54M30_BAR_IO, &c->io_if, c);

    LOG(1, "seated in slot %d: %u KB display memory, no interrupt line", slot_index, C54M30_VRAM >> 10);
    return dev;
}

// BUILTIN: soldered down on the Network Server logic board, instantiable
// only where a machine's slot table names it.
const pci_card_kind_t cirrus_54m30_kind = {
    .id = "cirrus_54m30",
    .display_name = "Cirrus Logic 54M30 on-board video",
    .attach = PCI_ATTACH_BUILTIN,
    .card_class = "display",
    .factory = c54m30_factory,
};
