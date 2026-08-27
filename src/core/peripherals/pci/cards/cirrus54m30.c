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
#include "display.h"
#include "log.h"
#include "pci.h"
#include "scheduler.h"
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
// The three indexed register blocks, each an index port and a data port,
// plus the DAC's own palette port pair.  These are the classic VGA files;
// the Cirrus extensions live at indices above the VGA range in the same
// blocks (SR07 selects the packed-pixel depth, CR1B/CR1D extend the start
// address), which is why a flat per-block array is enough.
#define C54M30_SEQ_REGS  0x20u
#define C54M30_CRTC_REGS 0x40u
#define C54M30_GR_REGS   0x20u
#define C54M30_ATTR_REGS 0x20u

typedef struct c54m30 {
    pci_device_t *dev;
    config_t *cfg;
    uint8_t *vram; // C54M30_VRAM bytes of fitted display memory
    uint8_t reg[C54M30_REGS]; // the raw I/O-space port shadow
    uint8_t seq[C54M30_SEQ_REGS]; // sequencer   ($3C4 index / $3C5 data)
    uint8_t crtc[C54M30_CRTC_REGS]; // CRTC        ($3D4 index / $3D5 data)
    uint8_t gr[C54M30_GR_REGS]; // graphics    ($3CE index / $3CF data)
    uint8_t attr[C54M30_ATTR_REGS]; // attribute   ($3C0, index/data alternating)
    uint8_t seq_index, crtc_index, gr_index, attr_index;
    bool attr_data; // the attribute port's index/data flip-flop
    uint8_t dac_write_index, dac_read_index, dac_phase;
    uint8_t dac[256][3]; // the palette, in the DAC's own 6-bit values
    display_t display;
    rgba8_t clut[256]; // the palette materialised for the renderer
    memory_interface_t fb_if;
    memory_interface_t io_if;
    memory_interface_t vga_if; // the fixed legacy $3B0-$3DF block
} c54m30_t;

static void c54m30_update(c54m30_t *c);

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
    if (offset < C54M30_VRAM) {
        c->vram[offset] = value;
        c->display.fb_dirty = true;
    }
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
// The VGA I/O ranges — the relocatable one AND the legacy one
// ============================================================
// A VGA-compatible part answers the fixed legacy I/O ports whether or not
// its relocatable window is enabled: "Enable Offset: If a pull-down is
// installed on MD51, this bit will be read as a '1' and relocatable I/O
// addressing will be enabled" (Alpine TRM §4.19), and this board installs
// none.  So the BAR is sized and assigned by Open Firmware — it lands at
// $00010000, above the 16 bits a Bandit even drives — while every actual
// access goes to the legacy addresses.
//
// This is a STRAPPED decode rather than a BAR, which is exactly what
// pci_device_add_fixed_region exists for (the Mach64 GX precedent).  It is
// also load-bearing rather than cosmetic: without it the firmware's write
// to $3C4 (the VGA sequencer index) lands on an unclaimed bus window, takes
// a recoverable transfer error, and the machine check that follows takes
// down the rest of Open Firmware's device installation with it.
#define C54M30_VGA_IO_BASE 0x3B0u // $3B0-$3DF: the legacy VGA port block
#define C54M30_VGA_IO_SPAN 0x030u

// ============================================================
// The relocatable VGA I/O range
// ============================================================
// A 512-byte window carrying the classic VGA register file plus the Alpine
// extensions, all reached through index/data port pairs.  Shadowed
// store-and-readback: nothing on the boot path drives a mode through it —
// POST reads the PCI ID and stops, and Open Firmware's `54m30-config` builds
// the device-tree node — so the honest model records what a guest writes and
// logs, rather than inventing CRTC behaviour nothing has yet exercised.

// Input Status Register 1 ($3BA mono / $3DA colour) — the one VGA register
// that MUST NOT be store-and-readback, because software does not read it
// for a value, it reads it for an EDGE.  Every VGA console waits on bit 3
// (vertical retrace) or bit 0 (display enable inactive) before touching the
// CRTC or the palette, and a register that never toggles turns that wait
// into a hang with nothing to diagnose.
//
// The bits are derived from the scheduler's cycle count rather than from a
// read counter, so the duty cycle is right for code that measures the
// blanking interval as well as for code that merely waits for it — and so
// the answer is a function of emulated time, which keeps a run
// deterministic.
#define C54M30_STATUS1_MONO   0xBAu // $3BA
#define C54M30_STATUS1_COLOUR 0xDAu // $3DA
#define C54M30_STAT_DE        0x01u // display enable INACTIVE (blanking)
#define C54M30_STAT_VR        0x08u // vertical retrace in progress
#define C54M30_FRAME_HZ       60u

static uint8_t status1_value(c54m30_t *c) {
    if (!c->cfg || !c->cfg->scheduler)
        return 0;
    uint64_t freq = c->cfg->machine ? c->cfg->machine->freq : 0;
    if (!freq)
        return 0;
    uint64_t frame = freq / C54M30_FRAME_HZ;
    uint64_t pos = scheduler_cpu_cycles(c->cfg->scheduler) % (frame ? frame : 1u);
    // A ~7% vertical blanking interval, which is close enough to a real
    // 640x480 timing for an edge-waiting loop and is not pretending to be
    // a pixel-accurate raster.
    bool vr = pos >= (frame - frame / 14u);
    return (uint8_t)((vr ? (C54M30_STAT_VR | C54M30_STAT_DE) : 0u));
}

// The VGA port map, as low bytes of the legacy block.
#define C54M30_ATTR       0xC0u // $3C0: attribute index/data, alternating
#define C54M30_ATTR_READ  0xC1u // $3C1: attribute data read-back
#define C54M30_SEQ_INDEX  0xC4u // $3C4 / $3C5
#define C54M30_SEQ_DATA   0xC5u
#define C54M30_DAC_RINDEX 0xC7u // $3C7: palette read index
#define C54M30_DAC_WINDEX 0xC8u // $3C8: palette write index
#define C54M30_DAC_DATA   0xC9u // $3C9: palette data, R-G-B per entry
#define C54M30_GR_INDEX   0xCEu // $3CE / $3CF
#define C54M30_GR_DATA    0xCFu
#define C54M30_CRTC_INDEX 0xD4u // $3D4 / $3D5 (colour; $3B4/$3B5 mono)
#define C54M30_CRTC_DATA  0xD5u

static uint8_t io_read8(void *ctx, uint32_t offset) {
    c54m30_t *c = (c54m30_t *)ctx;
    uint32_t port = offset & (C54M30_REGS - 1u);
    switch (port) {
    case C54M30_STATUS1_MONO:
    case C54M30_STATUS1_COLOUR:
        // Reading Input Status 1 also resets the attribute controller's
        // index/data flip-flop, which is how software resynchronises it.
        c->attr_data = false;
        return status1_value(c);
    case C54M30_SEQ_DATA:
        return c->seq[c->seq_index & (C54M30_SEQ_REGS - 1u)];
    case C54M30_CRTC_DATA:
        return c->crtc[c->crtc_index & (C54M30_CRTC_REGS - 1u)];
    case C54M30_GR_DATA:
        return c->gr[c->gr_index & (C54M30_GR_REGS - 1u)];
    case C54M30_ATTR_READ:
        return c->attr[c->attr_index & (C54M30_ATTR_REGS - 1u)];
    case C54M30_DAC_DATA: {
        uint8_t v = c->dac[c->dac_read_index][c->dac_phase];
        if (++c->dac_phase == 3) {
            c->dac_phase = 0;
            c->dac_read_index++;
        }
        return v;
    }
    default:
        return c->reg[port];
    }
}

static void io_write8(void *ctx, uint32_t offset, uint8_t value) {
    c54m30_t *c = (c54m30_t *)ctx;
    uint32_t port = offset & (C54M30_REGS - 1u);
    c->reg[port] = value;
    LOG(5, "VGA I/O +$%03X = $%02X", port, value);
    switch (port) {
    case C54M30_SEQ_INDEX:
        c->seq_index = value;
        return;
    case C54M30_SEQ_DATA:
        c->seq[c->seq_index & (C54M30_SEQ_REGS - 1u)] = value;
        c54m30_update(c);
        return;
    case C54M30_CRTC_INDEX:
        c->crtc_index = value;
        return;
    case C54M30_CRTC_DATA:
        c->crtc[c->crtc_index & (C54M30_CRTC_REGS - 1u)] = value;
        c54m30_update(c);
        return;
    case C54M30_GR_INDEX:
        c->gr_index = value;
        return;
    case C54M30_GR_DATA:
        c->gr[c->gr_index & (C54M30_GR_REGS - 1u)] = value;
        c54m30_update(c);
        return;
    case C54M30_ATTR:
        // One port, alternating index and data, with the flip-flop reset by
        // a read of Input Status 1.
        if (!c->attr_data)
            c->attr_index = value & 0x1Fu;
        else
            c->attr[c->attr_index] = value;
        c->attr_data = !c->attr_data;
        return;
    case C54M30_DAC_WINDEX:
        c->dac_write_index = value;
        c->dac_phase = 0;
        return;
    case C54M30_DAC_RINDEX:
        c->dac_read_index = value;
        c->dac_phase = 0;
        return;
    case C54M30_DAC_DATA:
        // Three writes per entry, R then G then B, and the index
        // auto-advances — which is how a driver loads 256 colours with one
        // index write and 768 data writes.
        c->dac[c->dac_write_index][c->dac_phase] = value & 0x3Fu;
        if (++c->dac_phase == 3) {
            c->dac_phase = 0;
            // DAC values are SIX bits.  Scale to eight by replicating the
            // top two into the bottom, so $3F maps to $FF exactly — the
            // conventional expansion, and the one that makes white white.
            uint8_t i = c->dac_write_index;
            for (int ch = 0; ch < 3; ch++) {
                uint8_t v6 = c->dac[i][ch];
                uint8_t v8 = (uint8_t)((v6 << 2) | (v6 >> 4));
                if (ch == 0)
                    c->clut[i].r = v8;
                else if (ch == 1)
                    c->clut[i].g = v8;
                else
                    c->clut[i].b = v8;
            }
            c->clut[i].a = 0xFFu;
            c->display.clut_dirty = true;
            c->dac_write_index++;
        }
        return;
    default:
        return;
    }
}

// ============================================================
// Deriving the mode
// ============================================================
// The part is a VGA, so the mode lives in the CRTC, the sequencer and the
// Cirrus extension registers rather than in anything resembling a mode
// register.  What Open Firmware 1.1.22 actually programs on this machine —
// captured by logging every port write across a cold boot — is a plain
// 640x480 packed-pixel 8 bpp:
//
//   SR01 = $01   8 dots per character clock
//   SR07 = $F1   Cirrus extended mode, bits [3:1] = 000 = 8 bpp
//   CR01 = $4F   horizontal display end 79 -> (79 + 1) * 8 = 640 pixels
//   CR12 = $DF   vertical display end 223, with CR07 bit 1 = VDE bit 8
//                -> 479 -> 480 lines
//   CR13 = $50   offset 80 -> 80 * 8 = 640 bytes per scan line
//   CR0C/CR0D    start address 0
//   GR05 = $40   256-colour shift mode
//
// which is exactly the 8 bpp Apple's own note says a big-endian host is
// limited to on this part — and at one byte per pixel, byte order does not
// matter, so no new pixel format is needed.

// The Cirrus packed-pixel depth: SR07 bit 0 enables the extended modes and
// bits [3:1] select the depth.  Anything but 8 bpp needs the little-endian
// framebuffer window this display layer does not have, so it is reported
// and the mode is left at the last good one.
static uint32_t c54m30_bpp(const c54m30_t *c) {
    if (!(c->seq[0x07] & 0x01u))
        return 0; // plain VGA text/planar: not a packed-pixel mode
    switch ((c->seq[0x07] >> 1) & 7u) {
    case 0:
        return 8;
    case 1:
        return 16; // 5-5-5
    case 2:
        return 24;
    case 3:
        return 32;
    case 5:
        return 16; // 5-6-5
    default:
        return 0;
    }
}

static void c54m30_update(c54m30_t *c) {
    if (!c->vram)
        return;
    if (c54m30_bpp(c) != 8)
        return; // not a mode this layer can present; keep the last good one

    // Horizontal: CR01 is the display end in character clocks, and SR01
    // bit 0 selects 8 dots per clock (the only setting this ROM uses).
    uint32_t dots = (c->seq[0x01] & 0x01u) ? 8u : 9u;
    uint32_t width = ((uint32_t)c->crtc[0x01] + 1u) * dots;
    // Vertical: CR12 plus its two overflow bits in CR07 (bit 1 = VDE bit 8,
    // bit 6 = VDE bit 9).
    uint32_t vde = c->crtc[0x12];
    if (c->crtc[0x07] & 0x02u)
        vde |= 0x100u;
    if (c->crtc[0x07] & 0x40u)
        vde |= 0x200u;
    uint32_t height = vde + 1u;
    // Stride: the Offset register counts in units of eight bytes in a
    // 256-colour mode (mode 13h's 40 for 320 pixels is the canonical case).
    uint32_t stride = (uint32_t)c->crtc[0x13] * 8u;
    // Start address: CR0C/CR0D, extended upward by the Cirrus CR1B/CR1D
    // bits so the whole 1 MB is reachable.
    uint32_t start = ((uint32_t)c->crtc[0x0C] << 8) | c->crtc[0x0D];
    start |= ((uint32_t)(c->crtc[0x1B] & 0x01u) << 16);
    start |= ((uint32_t)(c->crtc[0x1B] & 0x0Cu) >> 2) << 17;
    start *= 4u; // the address counter steps a doubleword per unit

    if (width == 0 || width > 2048u || height == 0 || height > 1536u || stride < width)
        return; // a half-programmed CRTC mid-mode-set; wait for the rest
    if ((uint64_t)start + (uint64_t)stride * height > C54M30_VRAM)
        start = 0;

    if (c->display.width != width || c->display.height != height || c->display.stride != stride ||
        c->display.bits != c->vram + start) {
        c->display.width = width;
        c->display.height = height;
        c->display.stride = stride;
        c->display.format = PIXEL_8BPP;
        c->display.bits = c->vram + start;
        c->display.shape_dirty = true;
        LOG(2, "mode set: %ux%u 8 bpp, stride %u, start $%05X", width, height, stride, start);
    }
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

// The legacy block, indexed by the real port number so both windows land
// in one register file.
static uint8_t vga_read8(void *ctx, uint32_t offset) {
    return io_read8(ctx, (C54M30_VGA_IO_BASE + offset) & 0xFFu);
}

static void vga_write8(void *ctx, uint32_t offset, uint8_t value) {
    io_write8(ctx, (C54M30_VGA_IO_BASE + offset) & 0xFFu, value);
}

static uint16_t vga_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((vga_read8(ctx, offset) << 8) | vga_read8(ctx, offset + 1));
}

static void vga_write16(void *ctx, uint32_t offset, uint16_t value) {
    vga_write8(ctx, offset, (uint8_t)(value >> 8));
    vga_write8(ctx, offset + 1, (uint8_t)value);
}

static uint32_t vga_read32(void *ctx, uint32_t offset) {
    return ((uint32_t)vga_read16(ctx, offset) << 16) | vga_read16(ctx, offset + 2);
}

static void vga_write32(void *ctx, uint32_t offset, uint32_t value) {
    vga_write16(ctx, offset, (uint16_t)(value >> 16));
    vga_write16(ctx, offset + 2, (uint16_t)value);
}

// ============================================================
// Device lifecycle
// ============================================================

static const char *c54m30_name(const pci_device_t *dev) {
    (void)dev;
    return "Cirrus 54M30";
}

// The primary display, once a mode has been programmed.  Before that the
// descriptor has no geometry and the card advertises nothing, which is what
// lets `pci_primary_display` fall through to whatever else a machine has.
static display_t *c54m30_display(pci_device_t *dev) {
    c54m30_t *c = (c54m30_t *)dev->priv;
    return (c && c->display.width && c->display.height) ? &c->display : NULL;
}

static void c54m30_reset(pci_device_t *dev, config_t *cfg) {
    (void)cfg;
    c54m30_t *c = (c54m30_t *)dev->priv;
    if (!c)
        return;
    // PCI RST# clears the register file; display memory is DRAM and its
    // contents are not defined by reset, so the buffer is left alone.
    memset(c->reg, 0, sizeof(c->reg));
    memset(c->seq, 0, sizeof(c->seq));
    memset(c->crtc, 0, sizeof(c->crtc));
    memset(c->gr, 0, sizeof(c->gr));
    memset(c->attr, 0, sizeof(c->attr));
    c->seq_index = 0;
    c->crtc_index = 0;
    c->gr_index = 0;
    c->attr_index = 0;
    c->attr_data = false;
    c->dac_write_index = 0;
    c->dac_read_index = 0;
    c->dac_phase = 0;
    // The palette powers up all-zero — i.e. black — which is what a
    // monitor shows before the video circuitry drives it.
    memset(c->dac, 0, sizeof(c->dac));
    for (int i = 0; i < 256; i++)
        c->clut[i] = (rgba8_t){0, 0, 0, 0xFFu};
    c->display.clut = c->clut;
    c->display.clut_len = 256;
    c->display.format = PIXEL_8BPP;
    c->display.clut_dirty = true;
    c->display.shape_dirty = true;
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
    system_write_checkpoint_data(cp, c->seq, sizeof(c->seq));
    system_write_checkpoint_data(cp, c->crtc, sizeof(c->crtc));
    system_write_checkpoint_data(cp, c->gr, sizeof(c->gr));
    system_write_checkpoint_data(cp, c->attr, sizeof(c->attr));
    system_write_checkpoint_data(cp, c->dac, sizeof(c->dac));
    system_write_checkpoint_data(cp, c->vram, C54M30_VRAM);
}

static void c54m30_checkpoint_restore(pci_device_t *dev, checkpoint_t *cp) {
    c54m30_t *c = (c54m30_t *)dev->priv;
    if (!c || !cp)
        return;
    system_read_checkpoint_data(cp, c->reg, sizeof(c->reg));
    system_read_checkpoint_data(cp, c->seq, sizeof(c->seq));
    system_read_checkpoint_data(cp, c->crtc, sizeof(c->crtc));
    system_read_checkpoint_data(cp, c->gr, sizeof(c->gr));
    system_read_checkpoint_data(cp, c->attr, sizeof(c->attr));
    system_read_checkpoint_data(cp, c->dac, sizeof(c->dac));
    system_read_checkpoint_data(cp, c->vram, C54M30_VRAM);
    // The palette view and the scanout descriptor are DERIVED: rebuild them
    // rather than checkpointing pointers into a buffer that has moved.
    for (int i = 0; i < 256; i++) {
        for (int ch = 0; ch < 3; ch++) {
            uint8_t v6 = c->dac[i][ch];
            uint8_t v8 = (uint8_t)((v6 << 2) | (v6 >> 4));
            if (ch == 0)
                c->clut[i].r = v8;
            else if (ch == 1)
                c->clut[i].g = v8;
            else
                c->clut[i].b = v8;
        }
        c->clut[i].a = 0xFFu;
    }
    c->display.bits = NULL; // force c54m30_update to re-derive
    c->display.width = 0;
    c54m30_update(c);
    c->display.clut_dirty = true;
    c->display.shape_dirty = true;
    c->display.fb_dirty = true;
}

static const pci_device_ops_t c54m30_ops = {
    .display = c54m30_display,
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

    c->vga_if.read_uint8 = vga_read8;
    c->vga_if.read_uint16 = vga_read16;
    c->vga_if.read_uint32 = vga_read32;
    c->vga_if.write_uint8 = vga_write8;
    c->vga_if.write_uint16 = vga_write16;
    c->vga_if.write_uint32 = vga_write32;

    c54m30_reset(dev, cfg);

    pci_bar_backing_iface(dev, C54M30_BAR_FB, &c->fb_if, c);
    pci_bar_backing_iface(dev, C54M30_BAR_IO, &c->io_if, c);
    // The legacy VGA block: a contiguous strapped claim (match mask 0), not
    // a BAR.  Faking a BAR for it would be worse than doing nothing —
    // Open Firmware would size it, assign it, and invent an address the
    // card does not decode.
    pci_device_add_fixed_region(dev, PCI_SPACE_IO, C54M30_VGA_IO_BASE, C54M30_VGA_IO_SPAN, 0, 0, &c->vga_if, c);

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
