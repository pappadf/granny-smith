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

// The legacy VGA window, and where THIS MODEL puts it.  The part decodes it at $A0000, which
// is an address no CPU on this machine can reach across Bandit; the long comment above the
// window code says what that costs and why the top of BAR0's aperture is the answer.
#define C54M30_WIN_BASE 0x00FE0000u // the mirror's base inside the 16 MB aperture
#define C54M30_WIN_SPAN 0x00020000u // 128 KB: $A0000-$BFFFF
#define C54M30_WIN_MMIO 0x00018000u // $B8000 within it: the memory-mapped BLT registers
_Static_assert(C54M30_WIN_BASE >= C54M30_VRAM, "the mirror must sit above the fitted DRAM");
_Static_assert(C54M30_WIN_BASE + C54M30_WIN_SPAN <= C54M30_FB_SPAN, "the mirror must fit inside BAR0");

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
#define C54M30_GR_REGS                                                                                                 \
    0x40u // GR00-GR3F: the VGA nine plus Cirrus's extensions, the BitBLT engine's GR20-GR35 among them
#define C54M30_ATTR_REGS 0x20u

// Two registers in those blocks are not RAM, and every Alpine driver leans
// on both to decide the part is a Cirrus at all (TRM §4.4 and §4.16).
//
// SR06, "Unlock ALL Extensions": writing $12 unlocks the extension
// registers and the register then READS BACK $12; writing anything else
// locks them and it reads back $0F.  A driver's presence test is exactly
// that round trip, so a plain byte of storage here fails it.
#define C54M30_SR06_UNLOCK 0x12u
#define C54M30_SR06_LOCKED 0x0Fu
// CR27, "ID": read-only, bits 7:2 the device and 1:0 the die revision.
// $A0 is the CL-GD5430 (the GD5430/5440-family die this card carries, and
// the value that agrees with its $00A0 PCI device ID), revision 0.
#define C54M30_CR27_ID 0xA0u
// SR15, "DRAM Control": bits 3:0 report how much display memory the board
// fits — 0 = 256 KB, 1 = 512 KB, 2 = 1 MB, 3 = 2 MB, 5 = 3 MB, 4 = 4 MB.
// A driver sizes its mode list from this field instead of probing, so it
// has to agree with the DRAM below, or every mode is judged too large.
#define C54M30_SR15_MEMSIZE 0x2u // 1 MB
_Static_assert(C54M30_VRAM == 0x00100000u, "C54M30_SR15_MEMSIZE must match the fitted DRAM");

// The parameters of one BLT, latched from the register file when the start bit is written.
// Laid out widest-first so the struct has no interior padding: it is checkpointed whole.
typedef struct {
    uint32_t width, height; // destination bytes per scan line, and scan lines
    uint32_t dst, src; // start addresses into display memory
    uint32_t clip; // GR2F[2:0] in bytes: destination skipped at the left of every scan line
    uint32_t pixel; // bytes a colour-expanded pixel occupies
    uint32_t fg, bg; // the expansion colours, low byte first
    uint32_t pat_base, pat_row, pat_pitch; // the 8 x 8 pattern: array, first row, row stride
    uint32_t mono_pitch; // bytes of monochrome source consumed per scan line
    int32_t dpitch, spitch; // scan-line strides
    int32_t step; // +1, or -1 when GR30[0] runs the BLT backwards
    uint8_t mode, rop, truth; // GR30, GR32, and GR32 decoded into a truth table
    uint8_t reserved;
} c54m30_blt_params_t;

// A system-to-screen BLT in flight: the engine is busy and the CPU is still delivering the
// source, one write into display memory at a time.
typedef struct {
    c54m30_blt_params_t p;
    uint32_t x, y; // how far the transfer has got, in destination bytes and scan lines
    uint32_t dst, line; // the current destination address, and where this scan line began
    uint32_t dword; // bytes taken from the current DWORD transfer
    uint32_t skip; // bytes still to be discarded at the end of a scan line
    bool active;
    uint8_t reserved[3];
} c54m30_blt_t;

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
    uint8_t dac[256][3]; // the palette, as written: six bits (VGA) or eight (hidden DAC, below)
    // The Cirrus "hidden DAC register" sits behind the pel mask: four consecutive reads of
    // $3C6 and the fifth access to $3C6 reaches it instead.  Bit 1 puts the palette DAC in its
    // 8-bit mode, and NT's Cirrus display driver loads 8-bit RGB once it has -- with a 6-bit-only
    // model every grey that is a multiple of 64 (the dialog's 192, disabled text's 128) came out
    // black, and GUI Setup's pages drew as a black void with one grey button.
    uint8_t pelmask_reads; // consecutive $3C6 reads so far; any other access resets it
    uint8_t hidden_dac;
    bool dac_8bit; // palette data is 8-bit RGB (hidden DAC bit 1, or seen from the values)
    display_t display;
    rgba8_t clut[256]; // the palette materialised for the renderer
    memory_interface_t fb_if;
    memory_interface_t io_if;
    memory_interface_t vga_if; // the fixed legacy $3B0-$3DF block
    c54m30_blt_t blt; // the BitBLT engine's only state that outlives a register write
    uint32_t win_logged; // accesses to the legacy-window mirror logged loudly so far
} c54m30_t;

static void c54m30_update(c54m30_t *c);

// ============================================================
// The BitBLT engine
// ============================================================
// Alpine TRM 4th ed. (Feb 1995): the registers in sections 9.39-9.42, the engine in
// Appendix D8 "BitBLT", the memory-mapped register block in Appendix B20.
//
// The engine runs SYNCHRONOUSLY: a screen BLT is finished by the time the write that set the
// start bit returns.  Nothing in this emulator needs a blit to take time, and a driver polling
// the status bit sees completion on its first read -- which is the whole point, because the
// driver that prompted this work spins on that bit forever when there is no engine behind it.

// GR31, Start/Status (TRM 9.40).
#define C54M30_BLT_BUSY     0x01u // bit 0: BLT in progress (read-only)
#define C54M30_BLT_START    0x02u // bit 1: start -- and cleared by the part when the BLT ends
#define C54M30_BLT_RESET    0x04u // bit 2: reset the engine, abandoning any BLT in progress
#define C54M30_BLT_PROGRESS 0x08u // bit 3: progress status (read-only)

// GR30, BLT Mode (TRM 9.39).
#define C54M30_BLT_EXPAND   0x80u // bit 7: the ROP source is colour-expanded from a bitmap
#define C54M30_BLT_PATTERN  0x40u // bit 6: the source is an 8 x 8 pattern, copied repeatedly
#define C54M30_BLT_WIDTH    0x30u // bits 5:4: colour-expand width, 00 = 8 bpp, 01 = 16 bpp
#define C54M30_BLT_TRANSP   0x08u // bit 3: colour expand with transparency
#define C54M30_BLT_SYSTEM   0x04u // bit 2: the source is system memory, delivered by the CPU
#define C54M30_BLT_BACKWARD 0x01u // bit 0: addresses decrement rather than increment

// GR32 selects one of sixteen two-operand raster operations, and TRM Table D8-3 gives each one
// as a truth table over (source, destination).  Returning it as four bits indexed by
// (S << 1) | D lets one table serve every operation and reduces the ROP itself to three lines
// of bitwise algebra.  The SAME code means pattern-op-destination when GR30[6] is set -- "the
// value actually programmed into GR32 is independent of whether a source or pattern is used"
// (Appendix D8 section 4) -- which is why there is one table here and not two.
#define C54M30_ROP_BAD 0xFFu

static uint8_t c54m30_rop_truth(uint8_t code) {
    switch (code) {
    case 0x00u:
        return 0x0u; // 0                 BLACKNESS
    case 0x90u:
        return 0x1u; // ~S & ~D           NOTSRCERASE / DPon
    case 0x50u:
        return 0x2u; // ~S & D            DSna / DPna
    case 0xD0u:
        return 0x3u; // ~S                NOTSRCCOPY / Pn
    case 0x09u:
        return 0x4u; // S & ~D            SRCERASE / PDna
    case 0x0Bu:
        return 0x5u; // ~D                DSTINVERT
    case 0x59u:
        return 0x6u; // S ^ D             SRCINVERT / PATINVERT
    case 0xDAu:
        return 0x7u; // ~S | ~D           DSan / DPan
    case 0x05u:
        return 0x8u; // S & D             SRCAND / DPa
    case 0x95u:
        return 0x9u; // ~(S ^ D)          DSxn / PDxn
    case 0x06u:
        return 0xAu; // D
    case 0xD6u:
        return 0xBu; // ~S | D            MERGEPAINT / DPno
    case 0x0Du:
        return 0xCu; // S                 SRCCOPY / PATCOPY
    case 0xADu:
        return 0xDu; // S | ~D            SDno / PDno
    case 0x6Du:
        return 0xEu; // S | D             SRCPAINT / DPo
    case 0x0Eu:
        return 0xFu; // 1                 WHITENESS
    default:
        return C54M30_ROP_BAD;
    }
}

static inline uint8_t c54m30_rop(uint8_t truth, uint8_t s, uint8_t d) {
    uint8_t r = 0u;
    if (truth & 0x8u)
        r |= (uint8_t)(s & d);
    if (truth & 0x4u)
        r |= (uint8_t)(s & (uint8_t)~d);
    if (truth & 0x2u)
        r |= (uint8_t)((uint8_t)~s & d);
    if (truth & 0x1u)
        r |= (uint8_t)((uint8_t)~s & (uint8_t)~d);
    return r;
}

// Display memory as the engine sees it.  Both start addresses are 21 bits on this part ("Each
// start address is a 21-bit value for the CL-GD5430/'40, allowing up to 2 Mbytes", Appendix D8
// section 2), which is twice the DRAM this board fits.  Anything past the fitted 1 MB reads as
// zero and swallows writes -- the same promise the aperture makes, and the reason a runaway
// blit cannot scribble outside the buffer.
static inline uint8_t blt_peek(const c54m30_t *c, uint32_t addr) {
    return addr < C54M30_VRAM ? c->vram[addr] : 0u;
}

static inline void blt_poke(c54m30_t *c, uint32_t addr, uint8_t value) {
    if (addr < C54M30_VRAM)
        c->vram[addr] = value;
}

// Latch the register file into one BLT's parameters.  False means the BLT cannot be executed
// and the destination must be left alone; the reason is logged.
static bool c54m30_blt_gather(const c54m30_t *c, c54m30_blt_params_t *p) {
    const uint8_t *g = c->gr;
    memset(p, 0, sizeof(*p));
    // Width and height are eleven bits on the '30/'40 -- GR21 and GR23 carry three bits each
    // (Table D8-11) -- and both are programmed one less than the count.  Width counts BYTES of
    // destination, not pixels.
    p->width = ((uint32_t)g[0x20] | ((uint32_t)(g[0x21] & 0x07u) << 8)) + 1u;
    p->height = ((uint32_t)g[0x22] | ((uint32_t)(g[0x23] & 0x07u) << 8)) + 1u;
    // The pitches are thirteen bits.  They are magnitudes: GR30[0] decides whether a scan line
    // steps forward or back, rather than the pitch carrying a sign.
    p->dpitch = (int32_t)((uint32_t)g[0x24] | ((uint32_t)(g[0x25] & 0x1Fu) << 8));
    p->spitch = (int32_t)((uint32_t)g[0x26] | ((uint32_t)(g[0x27] & 0x1Fu) << 8));
    p->dst = (uint32_t)g[0x28] | ((uint32_t)g[0x29] << 8) | ((uint32_t)(g[0x2A] & 0x1Fu) << 16);
    p->src = (uint32_t)g[0x2C] | ((uint32_t)g[0x2D] << 8) | ((uint32_t)(g[0x2E] & 0x1Fu) << 16);
    p->mode = g[0x30];
    p->rop = g[0x32];
    p->step = (p->mode & C54M30_BLT_BACKWARD) ? -1 : 1;

    p->truth = c54m30_rop_truth(p->rop);
    if (p->truth == C54M30_ROP_BAD) {
        LOG(1, "BLT: GR32 $%02X is not one of the sixteen ROPs in TRM Table D8-3; destination left alone", p->rop);
        return false;
    }

    // GR30[5:4] gives the width a colour-expanded pixel expands to.  The '30/'40 have 8 and
    // 16 bpp only (Table D8-4); 24 and 32 belong to the '34/'36 and this part would not have
    // executed them either.
    p->pixel = 1u;
    if (p->mode & C54M30_BLT_EXPAND) {
        uint32_t sel = (uint32_t)(p->mode & C54M30_BLT_WIDTH) >> 4;
        if (sel > 1u) {
            LOG(1, "BLT: colour-expand width %u is a '34/'36 mode this part does not have (GR30 $%02X)", sel, p->mode);
            return false;
        }
        p->pixel = sel + 1u;
    }
    // The expansion colours, low byte first: GR1/GR11 foreground, GR0/GR10 background
    // (Tables D8-5 and D8-6).  On the '30/'40 the background registers are ignored outright
    // when transparency is on (Table D8-7), which falls out of never reading them there.
    p->fg = (uint32_t)g[0x01] | ((uint32_t)g[0x11] << 8);
    p->bg = (uint32_t)g[0x00] | ((uint32_t)g[0x10] << 8);
    // GR2F[2:0] skips the first n PIXELS of every destination scan line, so that a colour
    // expanded source need not be re-aligned to start part-way through a pattern (Appendix D8
    // section 7).  In bytes that is n times the destination pixel size, which for every mode
    // this model presents is one byte, or the expansion width when expanding.
    p->clip = (uint32_t)(g[0x2F] & 0x07u) * p->pixel;
    // The 8 x 8 pattern.  Its three low source-address bits are the VERTICAL PRESET -- the
    // pattern scan line to start on -- so the array itself begins at the address with those
    // bits cleared (Appendix D8 section 9).  Rows are one byte apart when the pattern is
    // monochrome and colour-expanded, and eight bytes apart for the 8 bpp colour pattern that
    // is the only screen depth this model presents (Table D8-10).
    p->pat_base = p->src & ~7u;
    p->pat_row = p->src & 7u;
    p->pat_pitch = (p->mode & C54M30_BLT_EXPAND) ? 1u : 8u;
    // A colour-expanded source in display memory is a string of bytes restarted at a byte
    // boundary on every scan line, and the source pitch is ignored (Appendix D8 section 5).
    p->mono_pitch = ((p->width / p->pixel) + 7u) / 8u;
    return true;
}

// The ROP's source operand for the destination byte at column x of scan line y, where s is the
// source address this column has reached.  This is where the modes differ: a plain BLT reads
// the source rectangle, a pattern copy reads the same 8 x 8 array over and over, and colour
// expansion turns one source BIT into a pixel of foreground or background.  False means
// transparency says this destination byte must not be written at all.
static bool c54m30_blt_source(const c54m30_t *c, const c54m30_blt_params_t *p, uint32_t x, uint32_t y, uint32_t s,
                              uint8_t *out) {
    uint32_t row = (p->pat_row + y) & 7u;
    if (!(p->mode & C54M30_BLT_EXPAND)) {
        *out = (p->mode & C54M30_BLT_PATTERN) ? blt_peek(c, p->pat_base + row * p->pat_pitch + (x % p->pat_pitch))
                                              : blt_peek(c, s);
        return true;
    }
    // "the most-significant-bit of the first source byte will become the first pixel in the
    // screen destination" (TRM 9.39).  A monochrome pattern is eight bytes, one per row.
    uint32_t px = x / p->pixel;
    uint8_t mono = (p->mode & C54M30_BLT_PATTERN) ? blt_peek(c, p->pat_base + row)
                                                  : blt_peek(c, p->src + y * p->mono_pitch + (px >> 3));
    uint32_t colour;
    if ((mono >> (7u - (px & 7u))) & 1u)
        colour = p->fg;
    else if (p->mode & C54M30_BLT_TRANSP)
        return false;
    else
        colour = p->bg;
    *out = (uint8_t)(colour >> (8u * (x % p->pixel)));
    return true;
}

// A BLT whose source is display memory: executed here and now, start to finish.
static void c54m30_blt_screen(c54m30_t *c, const c54m30_blt_params_t *p) {
    uint32_t dline = p->dst, sline = p->src;
    for (uint32_t y = 0; y < p->height; y++) {
        uint32_t d = dline, s = sline;
        for (uint32_t x = 0; x < p->width; x++) {
            uint8_t sv;
            if (x >= p->clip && c54m30_blt_source(c, p, x, y, s, &sv))
                blt_poke(c, d, c54m30_rop(p->truth, sv, blt_peek(c, d)));
            d += (uint32_t)p->step;
            s += (uint32_t)p->step;
        }
        dline += (uint32_t)(p->step * p->dpitch);
        sline += (uint32_t)(p->step * p->spitch);
    }
    c->display.fb_dirty = true;
}

// Finish the current scan line of a system-to-screen BLT and move to the next.  False once the
// whole transfer is done and the engine has gone idle.
static bool c54m30_blt_next_line(c54m30_t *c) {
    c->display.fb_dirty = true;
    if (++c->blt.y >= c->blt.p.height) {
        c->blt.active = false;
        c->gr[0x31] &= (uint8_t) ~(C54M30_BLT_START | C54M30_BLT_BUSY | C54M30_BLT_PROGRESS);
        LOG(2, "BLT: system-to-screen transfer complete");
        return false;
    }
    c->blt.line += (uint32_t)c->blt.p.dpitch;
    c->blt.dst = c->blt.line;
    c->blt.x = 0u;
    return true;
}

// One byte of a system-to-screen BLT's source, delivered by a CPU write anywhere in display
// memory -- "the address provided by the CPU with such transfers is ignored" (Appendix D8
// section 13).  The CPU is required to transfer DWORDs; the byte counter here is what makes
// the end-of-scan-line discard rules come out right for that, and a driver that wrote bytes
// instead would merely be treated more kindly than the real part treats it.
static void c54m30_blt_feed(c54m30_t *c, uint8_t byte) {
    const c54m30_blt_params_t *p = &c->blt.p;
    c->blt.dword = (c->blt.dword + 1u) & 3u;
    if (c->blt.skip) {
        c->blt.skip--; // discarding the tail of a DWORD at the end of a scan line
        return;
    }
    if (!(p->mode & C54M30_BLT_EXPAND)) {
        // "up to three bytes of the last transfer for each scanline will be ignored... The next
        // scan line will begin with the next DWORD transfer."
        if (c->blt.x >= p->clip)
            blt_poke(c, c->blt.dst, c54m30_rop(p->truth, byte, blt_peek(c, c->blt.dst)));
        c->blt.dst++;
        if (++c->blt.x >= p->width) {
            c->blt.skip = (4u - c->blt.dword) & 3u;
            c54m30_blt_next_line(c);
        }
        return;
    }
    // Colour expansion: eight pixels per source byte.  "up to seven bits of the last (partially
    // used) byte will be ignored at the end of each scanline, and unused bytes will be used at
    // the beginning of the next scan line" -- so the discard is to the end of this BYTE, and
    // the next scan line starts with the next one.  (The DWORD-granular variant is GR33[0],
    // which is a '36 register.)
    for (unsigned b = 0; b < 8u; b++) {
        bool one = ((byte >> (7u - b)) & 1u) != 0u;
        if (one || !(p->mode & C54M30_BLT_TRANSP)) {
            uint32_t colour = one ? p->fg : p->bg;
            for (uint32_t k = 0; k < p->pixel; k++) {
                uint32_t d = c->blt.dst + k;
                if (c->blt.x + k >= p->clip)
                    blt_poke(c, d, c54m30_rop(p->truth, (uint8_t)(colour >> (8u * k)), blt_peek(c, d)));
            }
        }
        c->blt.x += p->pixel;
        c->blt.dst += p->pixel;
        if (c->blt.x >= p->width) {
            if (!c54m30_blt_next_line(c))
                return;
            break; // the rest of this source byte belongs to nothing
        }
    }
}

// The start bit has been written.  Gather the parameters and either run the BLT to completion
// or, for a system-to-screen BLT, leave the engine busy waiting for the CPU to deliver it.
static void c54m30_blt_start(c54m30_t *c) {
    c54m30_blt_params_t p;
    if (!c54m30_blt_gather(c, &p)) {
        c->blt.active = false;
        return;
    }
    LOG(1,
        "BLT start: %u x %u bytes, mode $%02X (%s%s%s%s%s), rop $%02X, dst $%06X pitch %d, src $%06X pitch %d, clip %u",
        p.width, p.height, p.mode, (p.mode & C54M30_BLT_EXPAND) ? "expand " : "",
        (p.mode & C54M30_BLT_PATTERN) ? "pattern " : "", (p.mode & C54M30_BLT_TRANSP) ? "transparent " : "",
        (p.mode & C54M30_BLT_SYSTEM) ? "system-source " : "", (p.mode & C54M30_BLT_BACKWARD) ? "backwards" : "forwards",
        p.rop, p.dst, p.dpitch, p.src, p.spitch, p.clip);
    if (!(p.mode & C54M30_BLT_SYSTEM)) {
        c->blt.active = false;
        c54m30_blt_screen(c, &p);
        return;
    }
    // The source is system memory, so the BLT cannot finish now: the engine stays busy and
    // every subsequent CPU write into display memory is its source.
    memset(&c->blt, 0, sizeof(c->blt));
    c->blt.p = p;
    c->blt.dst = p.dst;
    c->blt.line = p.dst;
    c->blt.active = true;
}

// A write to a graphics register, from either door -- the $3CE/$3CF index/data pair or the
// memory-mapped block.  GR31 is the one with a side effect.
static void c54m30_gr_write(c54m30_t *c, uint8_t index, uint8_t value) {
    index &= (uint8_t)(C54M30_GR_REGS - 1u);
    if (index != 0x31u) {
        c->gr[index] = value;
        return;
    }
    if (value & C54M30_BLT_RESET) {
        // "the entire BLT engine will be immediately reset and any operation in progress will
        // be terminated... GR31[3] and GR31[0] will be forced to '0'" (Appendix D8 section 14.2).
        c->blt.active = false;
        c->gr[0x31] = (uint8_t)(value & ~(C54M30_BLT_START | C54M30_BLT_BUSY | C54M30_BLT_PROGRESS));
        LOG(2, "BLT engine reset");
        return;
    }
    c->gr[0x31] = value;
    if (!(value & C54M30_BLT_START))
        return;
    c->gr[0x31] |= (uint8_t)(C54M30_BLT_BUSY | C54M30_BLT_PROGRESS);
    c54m30_blt_start(c);
    if (!c->blt.active)
        // The BLT is over before the write returns.  "This bit will be cleared to '0' when the
        // BLT is completed" is said of the START bit (TRM 9.40) as well as of the status bit,
        // so all three go: a driver polling either one sees a finished engine.
        c->gr[0x31] = (uint8_t)(value & ~(C54M30_BLT_START | C54M30_BLT_BUSY | C54M30_BLT_PROGRESS));
}

// ============================================================
// The memory-mapped BLT register block
// ============================================================

// Appendix B20, Table B20-1: the BLT registers seen as 256 bytes of memory.  The offsets are
// NOT the register indices -- the colour registers are interleaved and GR31 sits on its own at
// $40 -- so the mapping is spelled out rather than computed.  $FF marks an offset the table
// leaves reserved.
#define C54M30_MMIO_NONE 0xFFu

static uint8_t c54m30_mmio_reg(uint8_t off) {
    // $00-$07: background bytes 0-3 then foreground bytes 0-3.  Bytes 2 and 3 (GR12-GR15) are
    // "No" for the CL-GD5430/'40 in Table B20-1; they are shadowed anyway, where they cost
    // nothing and make a driver's setup visible, and the engine never reads them.
    static const uint8_t colour[8] = {0x00u, 0x10u, 0x12u, 0x14u, 0x01u, 0x11u, 0x13u, 0x15u};
    if (off < 0x08u)
        return colour[off];
    // $08-$1B run GR20 upwards in step with the offset, with $19 reserved -- GR31 is not there.
    if (off <= 0x1Bu && off != 0x19u)
        return (uint8_t)(0x20u + off - 0x08u);
    if (off == 0x40u)
        return 0x31u;
    return C54M30_MMIO_NONE;
}

// Where SR17 has put the register block, if anywhere.  SR17[2] enables it at $B8000 inside the
// legacy window, aliased at every 256-byte boundary up to $BFF00 because "Address bits 14:8 are
// 'don't care'" (Appendix B20 section 1).  With SR17[6] also set, the '30/'36/'40 move it
// instead to the last 256 bytes of the linear address space, which for this 1 MB board is the
// last 256 bytes of the fitted DRAM.  SR17[6] is documented as a don't-care unless linear
// addressing is enabled; on this model BAR0 *is* the linear aperture and there is no separate
// enable to consult, so it is always taken as enabled.
static bool c54m30_mmio_hit(const c54m30_t *c, uint32_t offset, uint8_t *reg) {
    uint8_t sr17 = c->seq[0x17];
    if (!(sr17 & 0x04u))
        return false;
    if (sr17 & 0x40u) {
        if (offset < C54M30_VRAM - 0x100u || offset >= C54M30_VRAM)
            return false;
    } else {
        if (offset < C54M30_WIN_BASE + C54M30_WIN_MMIO || offset >= C54M30_WIN_BASE + C54M30_WIN_SPAN)
            return false;
    }
    *reg = (uint8_t)(offset & 0xFFu);
    return true;
}

static uint8_t c54m30_mmio_read(const c54m30_t *c, uint8_t off) {
    uint8_t index = c54m30_mmio_reg(off);
    // "All registers are write-only with memory-mapped I/O, except GR31 which is read/write...
    // the data are indeterminate" (Appendix B20 section 1).  Answering every offset with its
    // shadow is a superset of that: it cannot mislead a driver that follows the manual, and it
    // makes a trace of the block readable.
    return index == C54M30_MMIO_NONE ? 0xFFu : c->gr[index];
}

static void c54m30_mmio_write(c54m30_t *c, uint8_t off, uint8_t value) {
    uint8_t index = c54m30_mmio_reg(off);
    if (index == C54M30_MMIO_NONE) {
        LOG(3, "MMIO write to reserved offset +$%02X = $%02X", off, value);
        return;
    }
    c54m30_gr_write(c, index, value);
}

// ============================================================
// The legacy VGA window, mirrored inside BAR0
// ============================================================
// The chip decodes the 128 KB VGA window at $A0000 and memory-maps the BitBLT registers inside
// it at $B8000.  THIS BOARD CANNOT USE THAT ADDRESS.  Bandit does not forward CPU accesses to
// PCI addresses below $80000000, which is why the NT miniport's access range had to be moved in
// the first place, and a window the CPU cannot reach is a driver spinning on all-ones.
//
// So this model exposes the same 128 KB at an address the CPU can reach: the top 128 KB of
// BAR0's 16 MB aperture.  THAT PLACEMENT IS A CONVENTION OF THIS MODEL AND NOT A PROPERTY OF
// THE PART -- a real GD5430 decodes this window at $A0000 and nowhere else.  It sits above the
// 1 MB of fitted DRAM, in aperture space the chip drives nothing into, so nothing real is
// displaced.  Inside the mirror the offsets are the window's own: $00000 is $A0000, and
// $18000 is $B8000.

// GR6[3:2], the VGA memory map select: which part of the window is display memory, and where
// within display memory the access starts from.  The TRM requires '01' -- 64 KB at $A0000 --
// whenever memory-mapped I/O is enabled, which is what leaves $B8000 free for the registers.
static bool c54m30_win_display(const c54m30_t *c, uint32_t o, uint32_t *sa) {
    switch ((c->gr[0x06] >> 2) & 3u) {
    case 0: // $A0000, 128 KB
        *sa = o;
        return true;
    case 1: // $A0000, 64 KB
        *sa = o;
        return o < 0x10000u;
    case 2: // $B0000, 32 KB
        *sa = o - 0x10000u;
        return o >= 0x10000u && o < 0x18000u;
    default: // $B8000, 32 KB
        *sa = o - 0x18000u;
        return o >= 0x18000u;
    }
}

// GR9 and GRA hold the window's offset into display memory and GRB says how they are applied
// (TRM 9.19-9.21): the offset is added to XA[19:12] in 4 KB pages, or to XA[21:14] in 16 KB
// pages when GRB[5] is set, and GRB[0] makes SA15 choose between the two offset registers.
//
// MODELLED FROM THE MANUAL AND UNTESTED.  Nothing on this machine drives the window through a
// bank register: this HAL's console writes BAR0 linearly, and the NT miniport maps the window
// only to reach the BitBLT registers above.  Do not claim this arithmetic is verified on the
// strength of a run that never exercised it.
static uint32_t c54m30_win_translate(const c54m30_t *c, uint32_t sa) {
    uint32_t xa = sa & 0x1FFFFu;
    bool dual = (c->gr[0x0B] & 0x01u) != 0u; // GRB[0]: Offset register 1 enabled
    uint8_t off = (dual && (xa & 0x8000u)) ? c->gr[0x0A] : c->gr[0x09];
    if (dual)
        xa &= ~0x8000u; // "XA[15] = 0" once Offset 1 is enabled (TRM 9.19)
    uint32_t page = (xa >> 12) & 0x1Fu; // XA[16:15] and SA[14:12]
    if (c->gr[0x0B] & 0x20u)
        return (((page + ((uint32_t)off << 2)) & 0x3FFu) << 12) | (xa & 0xFFFu);
    return (((page + off) & 0xFFu) << 12) | (xa & 0xFFFu);
}

// ============================================================
// The display-memory aperture
// ============================================================
// The chip decodes a contiguous 16 MB block and the fitted DRAM occupies
// the bottom 1 MB of it.  Above that the part drives nothing; reads return
// zero and writes vanish, which is what a sizing probe expects to find and
// keeps a runaway blit from scribbling outside the buffer.
//
// Three things are decoded inside this one aperture, in this order: the
// memory-mapped BLT register block wherever SR17 has put it, the legacy VGA
// window mirrored at C54M30_WIN_BASE, and the fitted DRAM at the bottom.
// One BAR and one decode -- the alternative, a second backing interface for
// the mirror, would need Open Firmware to assign it an address the card does
// not really decode.

// One byte into the aperture, with no logging: the decode itself.
static uint8_t aperture_read8(c54m30_t *c, uint32_t offset) {
    uint8_t reg;
    if (c54m30_mmio_hit(c, offset, &reg))
        return c54m30_mmio_read(c, reg);
    if (offset >= C54M30_WIN_BASE && offset < C54M30_WIN_BASE + C54M30_WIN_SPAN) {
        uint32_t sa;
        if (!c54m30_win_display(c, offset - C54M30_WIN_BASE, &sa))
            return 0u; // outside the map GR6[3:2] selects, the window decodes nothing
        return blt_peek(c, c54m30_win_translate(c, sa));
    }
    return offset < C54M30_VRAM ? c->vram[offset] : 0u;
}

static void aperture_write8(c54m30_t *c, uint32_t offset, uint8_t value) {
    uint8_t reg;
    if (c54m30_mmio_hit(c, offset, &reg)) {
        c54m30_mmio_write(c, reg, value);
        return;
    }
    if (c->blt.active) {
        // A system-to-screen BLT is waiting for its source and every write into display memory
        // IS that source, wherever it is aimed (TRM Appendix D8 section 13).  This sits below
        // the register decode so that GR31 stays reachable, which is how such a BLT is paused
        // or reset.
        c54m30_blt_feed(c, value);
        return;
    }
    if (offset >= C54M30_WIN_BASE && offset < C54M30_WIN_BASE + C54M30_WIN_SPAN) {
        uint32_t sa;
        if (!c54m30_win_display(c, offset - C54M30_WIN_BASE, &sa))
            return;
        offset = c54m30_win_translate(c, sa);
    }
    if (offset < C54M30_VRAM) {
        c->vram[offset] = value;
        c->display.fb_dirty = true;
    }
}

// Every access to the mirror, as offset, SIZE and value, before anything interprets it.
//
// This is the calibration the whole legacy-window route turns on.  The NT display driver's
// register block was captured sitting in VRAM at the window's $B8000 with every byte at the
// offset Appendix B20 gives XOR 7, and there are two ways that can happen: the driver computes
// its register addresses pre-XORed, the way a PowerPC NT driver written for a byte-swapping
// host bridge would, or it reaches them with an access size whose byte lanes fall differently.
// Bandit's lane reversal is not a candidate -- pci.c's lane_offset already cancels the
// little-endian CPU's address munge exactly once, and this HAL's own console depends on that,
// writing every pixel as a plain byte store through this same BAR with no compensation.
//
// So the decode above is written against the DOCUMENTED offsets and this log says what really
// arrives.  Byte-sized accesses at offset ^ 7 mean the driver pre-XORs, and the model must NOT
// compensate: a correctly written driver would then be the one that breaks.  Wider accesses
// mean the lane rule is to be worked through from the size instead.
//
// Deliberately at level 1, because the run that answers this turns the category on at level 1
// and nothing else in that run is interpretable until it does.  The answer is in the first few
// hundred lines, so after that the same accesses drop to level 3 rather than filling a log.
static void win_log(c54m30_t *c, uint32_t offset, unsigned size, uint32_t value, bool write) {
    if (offset < C54M30_WIN_BASE || offset >= C54M30_WIN_BASE + C54M30_WIN_SPAN)
        return;
    int level = c->win_logged < 512u ? 1 : 3;
    c->win_logged++;
    LOG(level, "window %s +$%05X.%u = $%X", write ? "write" : "read", offset - C54M30_WIN_BASE, size, value);
}

// The interface proper.  The wider accessors decompose into aperture_read8/aperture_write8
// rather than into each other, so that one bus access produces exactly one log line carrying
// its true size.  The byte order is this interface's usual one -- most significant byte at the
// lowest offset -- which is what pci.c hands down after undoing the bus's lane reversal.

static uint8_t fb_read8(void *ctx, uint32_t offset) {
    c54m30_t *c = (c54m30_t *)ctx;
    uint8_t v = aperture_read8(c, offset);
    win_log(c, offset, 1, v, false);
    return v;
}

static void fb_write8(void *ctx, uint32_t offset, uint8_t value) {
    c54m30_t *c = (c54m30_t *)ctx;
    win_log(c, offset, 1, value, true);
    aperture_write8(c, offset, value);
}

static uint16_t fb_read16(void *ctx, uint32_t offset) {
    c54m30_t *c = (c54m30_t *)ctx;
    uint16_t v = (uint16_t)(((uint16_t)aperture_read8(c, offset) << 8) | aperture_read8(c, offset + 1));
    win_log(c, offset, 2, v, false);
    return v;
}

static void fb_write16(void *ctx, uint32_t offset, uint16_t value) {
    c54m30_t *c = (c54m30_t *)ctx;
    win_log(c, offset, 2, value, true);
    aperture_write8(c, offset, (uint8_t)(value >> 8));
    aperture_write8(c, offset + 1, (uint8_t)value);
}

static uint32_t fb_read32(void *ctx, uint32_t offset) {
    c54m30_t *c = (c54m30_t *)ctx;
    uint32_t v = ((uint32_t)aperture_read8(c, offset) << 24) | ((uint32_t)aperture_read8(c, offset + 1) << 16) |
                 ((uint32_t)aperture_read8(c, offset + 2) << 8) | aperture_read8(c, offset + 3);
    win_log(c, offset, 4, v, false);
    return v;
}

static void fb_write32(void *ctx, uint32_t offset, uint32_t value) {
    c54m30_t *c = (c54m30_t *)ctx;
    win_log(c, offset, 4, value, true);
    aperture_write8(c, offset, (uint8_t)(value >> 24));
    aperture_write8(c, offset + 1, (uint8_t)(value >> 16));
    aperture_write8(c, offset + 2, (uint8_t)(value >> 8));
    aperture_write8(c, offset + 3, (uint8_t)value);
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
// extensions, all reached through index/data port pairs.  Store-and-readback
// for the mode registers, which the model reads a display geometry back out
// of; the exceptions are the handful of registers that are not memory on the
// real part — Input Status 1 below, and SR06 / SR15 / CR27 above, which a
// driver uses to decide the chip is there and how much memory it has.

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
#define C54M30_ATTR            0xC0u // $3C0: attribute index/data, alternating
#define C54M30_ATTR_READ       0xC1u // $3C1: attribute data read-back
#define C54M30_SEQ_INDEX       0xC4u // $3C4 / $3C5
#define C54M30_SEQ_DATA        0xC5u
#define C54M30_DAC_RINDEX      0xC7u // $3C7: palette read index
#define C54M30_DAC_WINDEX      0xC8u // $3C8: palette write index
#define C54M30_DAC_DATA        0xC9u // $3C9: palette data, R-G-B per entry
#define C54M30_GR_INDEX        0xCEu // $3CE / $3CF
#define C54M30_GR_DATA         0xCFu
#define C54M30_CRTC_INDEX      0xD4u // $3D4 / $3D5 (colour; $3B4/$3B5 mono)
#define C54M30_CRTC_DATA       0xD5u
#define C54M30_CRTC_INDEX_MONO 0xB4u // $3B4 / $3B5: the monochrome CRTC pair
#define C54M30_CRTC_DATA_MONO  0xB5u
// Miscellaneous Output: written at $3C2, read back at $3CC.  Bit 0 is the
// I/O Address Select that puts the CRTC pair and Input Status 1 at the
// colour addresses; a driver reads $3CC to learn which pair to use, so the
// read address has to answer with what was written rather than with the
// zero a store-and-readback shadow would give it.  Both CRTC pairs are
// decoded here whatever bit 0 says: no guest on this machine depends on the
// pair it did not choose being dead, and answering both keeps software that
// never writes $3C2 working.
#define C54M30_MISC_WRITE 0xC2u
#define C54M30_MISC_READ  0xCCu

// Fold the monochrome CRTC addresses onto the colour ones.
static uint32_t c54m30_port(uint32_t port) {
    if (port == C54M30_CRTC_INDEX_MONO)
        return C54M30_CRTC_INDEX;
    if (port == C54M30_CRTC_DATA_MONO)
        return C54M30_CRTC_DATA;
    return port;
}

// One palette entry, from the DAC's stored value to the renderer's: six-bit values are
// expanded by replicating the top two bits (so $3F is $FF exactly), eight-bit ones are as is.
static void c54m30_materialise(c54m30_t *c, unsigned i) {
    for (int ch = 0; ch < 3; ch++) {
        uint8_t v = c->dac[i][ch];
        uint8_t v8 = c->dac_8bit ? v : (uint8_t)(((v & 0x3Fu) << 2) | ((v & 0x3Fu) >> 4));
        if (ch == 0)
            c->clut[i].r = v8;
        else if (ch == 1)
            c->clut[i].g = v8;
        else
            c->clut[i].b = v8;
    }
    c->clut[i].a = 0xFFu;
}

#define C54M30_DAC_MASK 0xC6u // $3C6: pel mask, and the door to the hidden DAC register

static uint8_t io_read8(void *ctx, uint32_t offset) {
    c54m30_t *c = (c54m30_t *)ctx;
    uint32_t port = c54m30_port(offset & (C54M30_REGS - 1u));
    if (port == C54M30_DAC_MASK) {
        if (c->pelmask_reads >= 4) {
            c->pelmask_reads = 0;
            return c->hidden_dac;
        }
        c->pelmask_reads++;
        return c->reg[C54M30_DAC_MASK];
    }
    c->pelmask_reads = 0;
    switch (port) {
    case C54M30_MISC_READ:
        return c->reg[C54M30_MISC_WRITE];
    case C54M30_STATUS1_MONO:
    case C54M30_STATUS1_COLOUR:
        // Reading Input Status 1 also resets the attribute controller's
        // index/data flip-flop, which is how software resynchronises it.
        c->attr_data = false;
        return status1_value(c);
    case C54M30_SEQ_DATA:
        if ((c->seq_index & (C54M30_SEQ_REGS - 1u)) == 0x15u)
            return (uint8_t)((c->seq[0x15] & 0xF0u) | C54M30_SR15_MEMSIZE);
        return c->seq[c->seq_index & (C54M30_SEQ_REGS - 1u)];
    case C54M30_CRTC_DATA:
        if ((c->crtc_index & (C54M30_CRTC_REGS - 1u)) == 0x27u)
            return C54M30_CR27_ID;
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
    uint32_t port = c54m30_port(offset & (C54M30_REGS - 1u));
    LOG(5, "VGA I/O +$%03X = $%02X", port, value);
    if (port == C54M30_DAC_MASK && c->pelmask_reads >= 4) {
        c->pelmask_reads = 0;
        c->hidden_dac = value;
        bool eight = (value & 0x02u) != 0;
        LOG(1, "hidden DAC register = $%02X (%d-bit palette)", value, eight ? 8 : 6);
        if (eight != c->dac_8bit) {
            c->dac_8bit = eight;
            for (unsigned i = 0; i < 256; i++)
                c54m30_materialise(c, i);
            c->display.clut_dirty = true;
        }
        return;
    }
    c->pelmask_reads = 0;
    c->reg[port] = value;
    switch (port) {
    case C54M30_SEQ_INDEX:
        c->seq_index = value;
        return;
    case C54M30_SEQ_DATA:
        if ((c->seq_index & (C54M30_SEQ_REGS - 1u)) == 0x06u) {
            c->seq[0x06] = (value == C54M30_SR06_UNLOCK) ? C54M30_SR06_UNLOCK : C54M30_SR06_LOCKED;
            return;
        }
        // SR17[2] is what puts the BitBLT register block into memory, and SR17[6] is where.
        // Worth a line at level 1: if a driver never sets bit 2, none of its register writes
        // are register writes at all -- on the real part or here -- and the log says so before
        // anything else is worth reading.
        if ((c->seq_index & (C54M30_SEQ_REGS - 1u)) == 0x17u)
            LOG(1, "SR17 = $%02X: memory-mapped BLT registers %s", value,
                (value & 0x04u) ? ((value & 0x40u) ? "at the top of the linear aperture" : "at $B8000") : "disabled");
        c->seq[c->seq_index & (C54M30_SEQ_REGS - 1u)] = value;
        c54m30_update(c);
        return;
    case C54M30_CRTC_INDEX:
        c->crtc_index = value;
        return;
    case C54M30_CRTC_DATA:
        if ((c->crtc_index & (C54M30_CRTC_REGS - 1u)) == 0x27u)
            return; // CR27 is the read-only ID
        c->crtc[c->crtc_index & (C54M30_CRTC_REGS - 1u)] = value;
        c54m30_update(c);
        return;
    case C54M30_GR_INDEX:
        c->gr_index = value;
        return;
    case C54M30_GR_DATA:
        // The same door as the memory-mapped block above, and the same side effects: a BLT can
        // be started through $3CF as well, even though no driver on this machine has been seen
        // to do it.
        c54m30_gr_write(c, c->gr_index, value);
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
        // Stored as written; the hidden DAC register decides whether six or eight bits count.
        // (A guess from the values -- "above $3F must mean 8-bit" -- was tried and was wrong:
        // one stray $FF darkened a driver's whole 6-bit palette.)
        c->dac[c->dac_write_index][c->dac_phase] = value;
        if (++c->dac_phase == 3) {
            c->dac_phase = 0;
            c54m30_materialise(c, c->dac_write_index);
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
    memset(&c->blt, 0, sizeof(c->blt)); // and no BLT survives RST#
    c->win_logged = 0;
    c->seq[0x06] = C54M30_SR06_LOCKED; // extensions locked out of reset
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
    // A system-to-screen BLT half-fed by the CPU is the only engine state not already in the
    // register shadows; without it a restore would leave the guest polling a status bit that
    // no longer has a transfer behind it.
    system_write_checkpoint_data(cp, &c->blt, sizeof(c->blt));
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
    system_read_checkpoint_data(cp, &c->blt, sizeof(c->blt));
    // The palette view and the scanout descriptor are DERIVED: rebuild them
    // rather than checkpointing pointers into a buffer that has moved.
    for (unsigned i = 0; i < 256; i++)
        c54m30_materialise(c, i);
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
