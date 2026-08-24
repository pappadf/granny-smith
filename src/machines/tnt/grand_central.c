// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// grand_central.c
// Grand Central (343S1125) — the I/O controller behind almost every
// non-video device: a 128 KB window at the base of Bandit 1's PCI I/O
// space ($F3000000) holding the interrupt controller, eleven DBDMA
// engines, and the apertures of every legacy I/O cell.
//
// The chip is LITTLE-ENDIAN behind a big-endian bus with no byte-lane
// swapper: 32-bit registers (the interrupt block, DBDMA, BoxID) are
// reached with lwbrx/stwbrx by the guest and swapped at this model's edge
// (TNT_LE32); byte-wide device cells need no swapping and sit on $10
// centres (VIA: $200) so each occupies its own aligned longword slot —
// they are byte-access only, and wider access logs and reads open bus.
//
// Phase B populates: the interrupt block (+$20..$2C), the VIA1/Cuda
// window (+$16000), BoxID (+$1A000), and the banked NVRAM (+$1D000 port /
// +$1F000 data window).  The remaining apertures log and read open bus
// until their phases land (DBDMA +$8000.. Phase C; AWACS +$14000 and
// RaDACal +$1B000 Phase D; SCSI/MESH +$10000/+$18000 Phase E; MACE, SCC,
// SWIM3 Phase F).
//
// Register truth: the shipping ROM's Open Firmware device tree and 68k
// DecoderInfo tables, the ROM's own NanoKernel interrupt handler, and
// OSF/Apple MkLinux DR3 (powermac_pci.h, whose window offsets match entry
// for entry).  Interrupt semantics: see the interrupt-block comments.

#include "tnt.h"

#include "log.h"
#include "ppc.h"
#include "via.h"

#include <string.h>

LOG_USE_CATEGORY_NAME("gc");

// Island offsets (relative to $F3000000)
#define OFF_INTS    0x00020u // +$20 Events / +$24 Mask / +$28 Clear / +$2C Levels
#define OFF_DBDMA   0x08000u // channels 0-10 at +$8000+n*$100 (Phase C)
#define OFF_SCSI0   0x10000u // 53C94, external bus (Phase E)
#define OFF_MACE    0x11000u // MACE Ethernet (Phase F)
#define OFF_SCCLEG  0x12000u // SCC legacy aperture (Phase F)
#define OFF_ESCC    0x13000u // ESCC: channel B at +0, channel A at +$20 (Phase F)
#define OFF_AWACS   0x14000u // AWACS codec + sound control (Phase D)
#define OFF_SWIM3   0x15000u // SWIM3 floppy (Phase F)
#define OFF_VIA     0x16000u // VIA1/Cuda: 16 byte regs on $200 centres (8 KB)
#define OFF_MESH    0x18000u // MESH, internal bus (Phase E)
#define OFF_EPROM   0x19000u // Ethernet address PROM (Phase F)
#define OFF_BOXID   0x1A000u // machine-identification register (LE)
#define OFF_RADACAL 0x1B000u // RAMDAC colormap bank (Phase D)
#define OFF_NVPORT  0x1D000u // NVRAM bank-select port
#define OFF_NVDATA  0x1F000u // NVRAM data window: byte j at +j*$10

// Interrupt-register offsets within the block
#define INT_EVENTS 0x20u
#define INT_MASK   0x24u
#define INT_CLEAR  0x28u
#define INT_LEVELS 0x2Cu

// The NanoKernel's per-interrupt acknowledge: a write of $80000000 to
// InterruptClear is a MODE acknowledge, not a source clear — it must not
// clear pending device bits or the guest loses interrupts.
#define INT_MODE_ACK 0x80000000u

// ============================================================
// BoxID ($F301A000, little-endian bit numbering — the guest reads lwbrx)
// ============================================================
// Bit map, community-attested and pinned empirically at ladder rung T4:
//   0-5   PCI slot power/present pins (empty slots: 0)
//   6-7   SCC RTS-A/B readback
//   8     factory-test strap; POST tests it (modeled pulled high = normal)
//   9     microphone sense
//   10    Ethernet 10BT link
//   11-12 the 2-bit MODEL code the shared ROM dispatches on
//         (community reading: 9500 = %00, 8500 = %01; the 7500 code is a
//         starting guess, corrected at T4 from the compatible string the
//         ROM's Open Firmware emits)
//   13    composite-video-out / Sixty6 present
//   14    MESH / fast-SCSI present (all three TNT boards)
//   15    unused, pulled high
// The per-model composite values live in the board descriptors (pm7500.c
// etc.); this file just serves the register.

// ============================================================
// Interrupt fabric
// ============================================================

void tnt_gc_recompute(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    tnt_gc_t *gc = &st->gc;
    bool line = ((gc->int_events | gc->int_levels) & gc->int_mask) != 0;
    ppc_set_ext_irq(cfg->ppc, line);
}

void tnt_gc_set_source(config_t *cfg, int n, bool level) {
    tnt_gc_t *gc = &tnt_st(cfg)->gc;
    uint32_t bit = 1u << n;
    bool was = (gc->int_levels & bit) != 0;
    if (level) {
        gc->int_levels |= bit;
        // Events latches source EDGES; a still-asserted source re-latches
        // only on its next assertion edge.
        if (!was)
            gc->int_events |= bit;
    } else {
        gc->int_levels &= ~bit;
    }
    tnt_gc_recompute(cfg);
}

void tnt_gc_pulse_event(config_t *cfg, int n) {
    tnt_gc_t *gc = &tnt_st(cfg)->gc;
    gc->int_events |= 1u << n;
    tnt_gc_recompute(cfg);
}

// The interrupt block is 32-bit little-endian; `value` here is the
// little-endian register value (the dispatcher swaps at the bus edge).
static uint32_t int_read(config_t *cfg, uint32_t offset) {
    tnt_gc_t *gc = &tnt_st(cfg)->gc;
    switch (offset) {
    case INT_EVENTS:
        return gc->int_events;
    case INT_MASK:
        return gc->int_mask;
    case INT_CLEAR:
        return 0; // write-only
    case INT_LEVELS:
        return gc->int_levels; // live, never latched
    default:
        LOG(2, "interrupt-block read of unwired +$%02X", offset);
        return 0;
    }
}

static void int_write(config_t *cfg, uint32_t offset, uint32_t value) {
    tnt_gc_t *gc = &tnt_st(cfg)->gc;
    switch (offset) {
    case INT_EVENTS:
    case INT_CLEAR:
        // W1C into Events — except the mode-acknowledge bit, which clears
        // nothing (see INT_MODE_ACK above).
        gc->int_events &= ~(value & ~INT_MODE_ACK);
        LOG(3, "clear $%08X -> events $%08X", value, gc->int_events);
        break;
    case INT_MASK:
        gc->int_mask = value;
        LOG(3, "mask = $%08X", value);
        break;
    case INT_LEVELS:
        LOG(2, "write to read-only Levels ($%08X) ignored", value);
        break;
    default:
        LOG(2, "interrupt-block write of unwired +$%02X = $%08X", offset, value);
        break;
    }
    tnt_gc_recompute(cfg);
}

// ============================================================
// NVRAM (banked two-aperture model)
// ============================================================
// Port at +$1D000 selects a 32-byte bank (bank = byte offset / 32); the
// data window at +$1F000 exposes the bank's 32 bytes on $10 centres.
// 8 KB total = 256 banks.  POST logs into it before anything else works,
// so it is live from reset; the contents survive machine reset (it is
// non-volatile) and are checkpointed with the family blob.

static uint8_t nvram_read(config_t *cfg, uint32_t offset) {
    tnt_gc_t *gc = &tnt_st(cfg)->gc;
    if ((offset & 0xFu) != 0 || offset >= 32u * 0x10u) {
        LOG(2, "NVRAM data read off-centre +$%03X", offset);
        return 0xFF;
    }
    uint32_t idx = ((uint32_t)gc->nvram_bank * 32u + (offset >> 4)) % TNT_NVRAM_SIZE;
    return gc->nvram[idx];
}

static void nvram_write(config_t *cfg, uint32_t offset, uint8_t value) {
    tnt_gc_t *gc = &tnt_st(cfg)->gc;
    if ((offset & 0xFu) != 0 || offset >= 32u * 0x10u) {
        LOG(2, "NVRAM data write off-centre +$%03X = $%02X", offset, value);
        return;
    }
    uint32_t idx = ((uint32_t)gc->nvram_bank * 32u + (offset >> 4)) % TNT_NVRAM_SIZE;
    gc->nvram[idx] = value;
}

// ============================================================
// Island dispatch
// ============================================================

void tnt_gc_init(config_t *cfg) {
    tnt_gc_t *gc = &tnt_st(cfg)->gc;
    // Power-on: everything masked, nothing latched.  NVRAM contents are
    // deliberately NOT touched — the store is non-volatile (tnt.c zeroes
    // it once at machine construction).
    gc->int_events = 0;
    gc->int_mask = 0;
    gc->int_levels = 0;
    gc->nvram_bank = 0;
}

uint8_t tnt_gc_read8(config_t *cfg, uint32_t offset) {
    uint32_t block = offset & 0x1F000u;
    switch (block) {
    case OFF_VIA:
    case OFF_VIA + 0x1000: // 16 regs at stride $200 span the 8 KB window
        return via_get_memory_interface(cfg->via1)->read_uint8(cfg->via1, offset - OFF_VIA);
    case OFF_NVPORT:
        return tnt_st(cfg)->gc.nvram_bank;
    case OFF_NVDATA:
        return nvram_read(cfg, offset - OFF_NVDATA);
    case OFF_BOXID: {
        // Byte j of the little-endian register (the ROM reads it both as
        // lwbrx and byte-wise).
        uint8_t b = (uint8_t)(tnt_board(cfg)->boxid >> (8 * (offset & 3u)));
        LOG(3, "BoxID byte read +%u -> $%02X", offset & 3u, b);
        return b;
    }
    default:
        LOG(1, "byte read of unwired island offset +$%05X", offset);
        return 0;
    }
}

void tnt_gc_write8(config_t *cfg, uint32_t offset, uint8_t value) {
    uint32_t block = offset & 0x1F000u;
    switch (block) {
    case OFF_VIA:
    case OFF_VIA + 0x1000:
        via_get_memory_interface(cfg->via1)->write_uint8(cfg->via1, offset - OFF_VIA, value);
        return;
    case OFF_NVPORT:
        tnt_st(cfg)->gc.nvram_bank = value;
        return;
    case OFF_NVDATA:
        nvram_write(cfg, offset - OFF_NVDATA, value);
        return;
    default:
        LOG(1, "byte write of unwired island offset +$%05X = $%02X", offset, value);
        return;
    }
}

// 32-bit access: the LE register blocks.  `value` at this boundary is the
// big-endian bus view; TNT_LE32 recovers the little-endian register value
// the guest composed with stwbrx (and vice versa on reads).
uint32_t tnt_gc_read32(config_t *cfg, uint32_t offset) {
    if (offset >= OFF_INTS && offset < OFF_INTS + 0x10u)
        return TNT_LE32(int_read(cfg, offset));
    if ((offset & 0x1F000u) == OFF_BOXID) {
        LOG(3, "BoxID read -> $%08X", tnt_board(cfg)->boxid);
        return TNT_LE32(tnt_board(cfg)->boxid);
    }
    LOG(1, "long read of unwired island offset +$%05X", offset);
    return 0;
}

void tnt_gc_write32(config_t *cfg, uint32_t offset, uint32_t value) {
    if (offset >= OFF_INTS && offset < OFF_INTS + 0x10u) {
        int_write(cfg, offset, TNT_LE32(value));
        return;
    }
    if ((offset & 0x1F000u) == OFF_NVPORT) {
        // POST selects the NVRAM bank with a 32-bit stwbrx of the bank
        // number (the shipping ROM's logging helper does exactly this).
        tnt_st(cfg)->gc.nvram_bank = (uint8_t)TNT_LE32(value);
        return;
    }
    LOG(1, "long write of unwired island offset +$%05X = $%08X", offset, value);
}
