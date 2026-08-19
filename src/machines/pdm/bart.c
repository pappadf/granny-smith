// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// bart.c
// BART — the NuBus '90 bridge on the Power Macintosh 7100/8100 (on the 6100
// it lives on an optional PDS adapter card, which this model does not
// offer, so the 6100 presents "no BART" and the ROM's probe finds it).
//
// Software sees three things, and only three:
//
//   1. a handful of byte-wide control registers at $F0000000 — a NuBus
//      /RESET pulse, a wait-state bit, the "disable slot $E" latch the ROM
//      writes on every 7100/8100 boot, an ID longword, and one burst-enable
//      byte per slot;
//   2. the address windows BART decodes — standard slot space
//      $Fs000000-$FsFFFFFF and super slot space $s0000000-$sFFFFFFF for the
//      three connectors $B/$C/$D (the middle connector is $D; slot $E is
//      the PDS video pseudo-slot, not a NuBus connector, on these boards);
//   3. the FAULT semantics, which are the whole game: an access BART
//      claims but nothing answers terminates with a recoverable transfer
//      error.  The Slot Manager reads declaration ROMs "under a bus-error
//      catcher inside the 68k emulator" and records an empty slot from the
//      fault; a machine with no BART at all faults the presence probe
//      (`tst.b $F0000000`) and clears its BARTExists flag.  Returning $FF
//      instead would make every empty slot look like a broken card.
//
// Everything else about a slot interrupt is AMIC's: each connector's /NMRQ
// runs straight to an AMIC pin, and the Slot Manager reads the flags from
// the pseudo-VIA2 slot bank (amic.c).  BART is not in that path — see
// pdm_bart_slot_irq below, which routes through AMIC for exactly that
// reason.
//
// Register truth: the shipping 1994-03 ROM (NuBusReset, SlotBlockXferCtl),
// Apple, "Power Macintosh Computers" Developer Note (1994), and the
// Macintosh 8100 schematics (051-0333 rev A, sheets 22-23).  Bus behavior:
// Apple, "Designing Cards and Drivers for the Macintosh Family", 3rd ed.,
// ch. 7-8.

#include "pdm.h"

#include "log.h"
#include "nubus.h"

#include <stdio.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("bart");

// Register-file offsets from $F0000000 (byte-wide unless noted).
#define BART_REG_RESET  0x00u // write $80 = pulse NuBus /RESET (~1 ms)
#define BART_REG_SLOW   0x01u // bit 0 adds wait states; never written by the ROM
#define BART_REG_ID     0x08u // longword read; first-rev silicon returns $43184000
#define BART_REG_SLOT_E 0x11u // write $80 = disable BART's slot-$E path
#define BART_REG_BURST0 0x80u // slot 1 burst enable; slot n at $80 - 8*(n-1)
#define BART_REG_LIMIT  0x88u // first offset the register file does not answer

// The ID value first-revision silicon returns.  Only prototype-era ROM code
// ever read it (the shipped image contains neither this constant nor the
// address), and it gates an interrupt-line swap that must never trigger, so
// the model deliberately answers with something else.
#define BART_ID_PROTOTYPE 0x43184000u

// Slots whose /NMRQ the pseudo-VIA2 slot bank carries: $B/$C/$D on the
// connectors, $E from the PDS (amic.md: bit 2 = $B ... bit 5 = $E).
#define BART_SLOT_IRQ_FIRST 0x0B
#define BART_SLOT_IRQ_LAST  0x0E

// ============================================================
// Fault windows
// ============================================================
// Address ranges BART claims but that no card answers.  Registered as
// device regions so that a card's own regions, registered later by
// nubus_init, take over the pages they occupy — what is left faults.

// One access, one fault — the width does not matter: the bridge times the
// whole transaction out and reports it once.  The data the CPU never
// receives is modelled as the floating bus ($FF), as everywhere else.
static void bart_fault(void *ctx, uint32_t offset, bool write) {
    pdm_bart_window_t *w = (pdm_bart_window_t *)ctx;
    LOG(4, "%s: unclaimed %s $%08X", w->what, write ? "write" : "read", w->base + offset);
    memory_signal_bus_error(w->base + offset, write);
}

static uint8_t bart_fault_read8(void *ctx, uint32_t offset) {
    bart_fault(ctx, offset, false);
    return 0xFF;
}

static uint16_t bart_fault_read16(void *ctx, uint32_t offset) {
    bart_fault(ctx, offset, false);
    return 0xFFFF;
}

static uint32_t bart_fault_read32(void *ctx, uint32_t offset) {
    bart_fault(ctx, offset, false);
    return 0xFFFFFFFFu;
}

static void bart_fault_write8(void *ctx, uint32_t offset, uint8_t value) {
    (void)value;
    bart_fault(ctx, offset, true);
}

static void bart_fault_write16(void *ctx, uint32_t offset, uint16_t value) {
    (void)value;
    bart_fault(ctx, offset, true);
}

static void bart_fault_write32(void *ctx, uint32_t offset, uint32_t value) {
    (void)value;
    bart_fault(ctx, offset, true);
}

static memory_interface_t bart_fault_iface = {
    .read_uint8 = bart_fault_read8,
    .read_uint16 = bart_fault_read16,
    .read_uint32 = bart_fault_read32,
    .write_uint8 = bart_fault_write8,
    .write_uint16 = bart_fault_write16,
    .write_uint32 = bart_fault_write32,
};

// Claim one address window for BART with nothing behind it: every access
// that survives to here (no card region overlays the page) faults.
static void bart_claim_empty(config_t *cfg, uint32_t base, uint32_t size, const char *what) {
    pdm_state_t *st = pdm_st(cfg);
    if (st->bart_window_count >= PDM_BART_WINDOWS) {
        LOG(0, "bart: window table full; $%08X+$%X (%s) left undecoded", base, size, what);
        return;
    }
    pdm_bart_window_t *w = &st->bart_window[st->bart_window_count++];
    w->base = base;
    snprintf(w->what, sizeof(w->what), "%s", what);
    memory_map_add(cfg->mem_map, base, size, w->what, &bart_fault_iface, w);
}

// ============================================================
// Register file ($F0000000)
// ============================================================

// The per-slot burst-enable byte lives at $80 - 8*(slot-1) for slots 1..14
// (the shipping ROM's _HWPriv selector 12 computes exactly that, and its
// bset/bclr #8 on a byte toggles bit 0).  Map an offset back to its slot,
// or 0 when the offset is not one of the fourteen.
static int burst_slot_for_offset(uint32_t offset) {
    if (offset > BART_REG_BURST0 || offset < BART_REG_BURST0 - 8u * 13u)
        return 0;
    if ((BART_REG_BURST0 - offset) % 8u != 0)
        return 0;
    return 1 + (int)((BART_REG_BURST0 - offset) / 8u);
}

static uint8_t bart_reg_read8(void *ctx, uint32_t offset) {
    config_t *cfg = (config_t *)ctx;
    pdm_bart_t *b = &pdm_st(cfg)->bart;
    if (offset >= BART_REG_LIMIT) {
        // Beyond the register file the chip answers nothing (decode
        // granularity above $87 is unknown; keep the window minimal and
        // fault outside it — bart-nubus.md §12).
        LOG(4, "bart: read above the register file: $%08X", PDM_BART_BASE + offset);
        memory_signal_bus_error(PDM_BART_BASE + offset, false);
        return 0xFF;
    }
    int slot = burst_slot_for_offset(offset);
    if (slot)
        return b->burst[slot - 1];
    switch (offset) {
    case BART_REG_RESET:
        // What the presence probe reads.  The value is immaterial — that
        // the read COMPLETES is the whole signal (`TestForBart`).
        return 0x00;
    case BART_REG_SLOW:
        return b->slow;
    case BART_REG_SLOT_E:
        return b->slot_e_off;
    default:
        return 0x00;
    }
}

static uint32_t bart_reg_read32(void *ctx, uint32_t offset) {
    if (offset == BART_REG_ID) {
        // Anything but the first-rev value: the only reader is the
        // prototype-era interrupt-swap hack, which must never fire.  (Its
        // constants are absent from the shipping ROM entirely.)
        return ~BART_ID_PROTOTYPE;
    }
    return ((uint32_t)bart_reg_read8(ctx, offset) << 24) | ((uint32_t)bart_reg_read8(ctx, offset + 1) << 16) |
           ((uint32_t)bart_reg_read8(ctx, offset + 2) << 8) | bart_reg_read8(ctx, offset + 3);
}

static uint16_t bart_reg_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((bart_reg_read8(ctx, offset) << 8) | bart_reg_read8(ctx, offset + 1));
}

static void bart_reg_write8(void *ctx, uint32_t offset, uint8_t value) {
    config_t *cfg = (config_t *)ctx;
    pdm_bart_t *b = &pdm_st(cfg)->bart;
    if (offset >= BART_REG_LIMIT) {
        LOG(4, "bart: write above the register file: $%08X = $%02X", PDM_BART_BASE + offset, value);
        memory_signal_bus_error(PDM_BART_BASE + offset, true);
        return;
    }
    int slot = burst_slot_for_offset(offset);
    if (slot) {
        // Block-transfer enable for one slot.  Bursts and single beats are
        // indistinguishable to software here, so the bit is a latch the
        // Slot Manager can read back (bart-nubus.md §7).
        b->burst[slot - 1] = value;
        LOG(2, "bart: slot $%X burst transfers %s", slot, (value & 1u) ? "enabled" : "disabled");
        return;
    }
    switch (offset) {
    case BART_REG_RESET:
        if (value & 0x80u) {
            // NuBus /RESET pulse.  The ROM issues it on every start
            // (including a soft restart, where the line is never asserted
            // by hardware), so fan it out to the seated cards exactly as
            // the pin does.
            b->reset_pulses++;
            LOG(2, "bart: NuBus /RESET pulse (%u)", b->reset_pulses);
            if (cfg->nubus)
                nubus_reset(cfg->nubus);
        }
        break;
    case BART_REG_SLOW:
        b->slow = value; // wait-state bit; nothing in the ROM writes it
        break;
    case BART_REG_SLOT_E:
        // "Disable slot $E" — decode plus BART's own interrupt output for
        // it.  Every 7100/8100 boot writes $80 here; a production 6100
        // skips the write (its GetCPUIDReg promotes $3010 to $3011, which
        // is what the ROM's guard tests).  Slot $E on these boards is the
        // PDS video pseudo-slot, whose window the PDS claims on the CPU
        // bus rather than through BART, so this is a pure latch for us.
        b->slot_e_off = value;
        LOG(2, "bart: slot $E path %s", (value & 0x80u) ? "disabled" : "enabled");
        break;
    default:
        break;
    }
}

static void bart_reg_write16(void *ctx, uint32_t offset, uint16_t value) {
    bart_reg_write8(ctx, offset, (uint8_t)(value >> 8));
    bart_reg_write8(ctx, offset + 1, (uint8_t)value);
}

static void bart_reg_write32(void *ctx, uint32_t offset, uint32_t value) {
    bart_reg_write16(ctx, offset, (uint16_t)(value >> 16));
    bart_reg_write16(ctx, offset + 2, (uint16_t)value);
}

// ============================================================
// Wiring
// ============================================================

// Build the bridge's address decode.  Called from the family memory layout,
// BEFORE nubus_init: every window is claimed empty here, and each card's
// own regions then overlay the pages it answers.
void pdm_bart_init(config_t *cfg) {
    pdm_state_t *st = pdm_st(cfg);
    memset(&st->bart, 0, sizeof(st->bart));
    st->bart_window_count = 0;

    st->bart_reg_interface.read_uint8 = bart_reg_read8;
    st->bart_reg_interface.read_uint16 = bart_reg_read16;
    st->bart_reg_interface.read_uint32 = bart_reg_read32;
    st->bart_reg_interface.write_uint8 = bart_reg_write8;
    st->bart_reg_interface.write_uint16 = bart_reg_write16;
    st->bart_reg_interface.write_uint32 = bart_reg_write32;

    const nubus_slot_decl_t *slots = cfg->machine->nubus_slots;
    if (!slots) {
        // No BART on this board (a 6100 without the NuBus adapter): the
        // presence probe must fault.  `tst.b $F0000000` bus-errors, the ROM
        // clears BARTExists, and the machine boots with zero NuBus slots.
        // The slot windows stay undecoded — with no bridge, nothing on the
        // board claims them at all.
        bart_claim_empty(cfg, PDM_BART_BASE, MEM_PAGE_SIZE, "BART absent");
        return;
    }

    // The register file.  Page granularity is ours, not the chip's; inside
    // the page the file answers $00..$87 and faults above it.
    memory_map_add(cfg->mem_map, PDM_BART_BASE, MEM_PAGE_SIZE, "BART registers", &st->bart_reg_interface, cfg);

    // Standard slot space (16 MB per slot) and super slot space (256 MB per
    // slot) for every declared connector, named per slot so a fault log and
    // the memory map both say which one.
    for (const nubus_slot_decl_t *s = slots; s->slot != 0; s++) {
        char name[24];
        snprintf(name, sizeof(name), "NuBus slot $%X", s->slot);
        bart_claim_empty(cfg, nubus_slot_base(s->slot), 0x01000000u, name);
        snprintf(name, sizeof(name), "NuBus super slot $%X", s->slot);
        bart_claim_empty(cfg, nubus_super_slot_base(s->slot), 0x10000000u, name);
    }

    // Slot $E is the PDS video pseudo-slot on these boards, not a NuBus
    // connector — so it is not in the slot table above.  Its window still
    // has to answer, because the Start Manager probes slot $E's declaration
    // ROM for a "VidReset" signature on EVERY boot, before the Slot Manager
    // runs.  With no PDS card seated, that probe must fault recoverably.
    bart_claim_empty(cfg, nubus_slot_base(0xE), 0x01000000u, "PDS slot space");
}

// Drive one slot's /NMRQ.  The line does not pass through BART at all: it
// runs from the connector to an AMIC pin, and the Slot Manager reads it
// from the pseudo-VIA2 slot bank.  This is the bus controller's entry point
// (machine_substrate_t.nubus_slot_irq), so it lives with the bridge and
// forwards to AMIC.
void pdm_bart_slot_irq(config_t *cfg, int slot, bool active) {
    if (slot < BART_SLOT_IRQ_FIRST || slot > BART_SLOT_IRQ_LAST) {
        LOG(1, "bart: slot $%X has no interrupt line on this board", slot);
        return;
    }
    LOG(3, "bart: slot $%X /NMRQ %s", slot, active ? "asserted" : "released");
    pdm_amic_set_slot_irq(cfg, slot, active);
}
