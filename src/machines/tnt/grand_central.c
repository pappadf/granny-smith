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
// Populated so far: the interrupt block (+$20..$2C), the DBDMA channel
// windows (+$8000+n*$100, Phase C — the engine itself is dbdma.c), the
// VIA1/Cuda window (+$16000), BoxID (+$1A000), the banked NVRAM
// (+$1D000 port / +$1F000 data window), AWACS (+$14000, Phase D) and the
// RaDACal RAMDAC (+$1B000 — control.c, Phase D part 2).  The remaining
// apertures log and read open bus until their phases land (SCSI/MESH
// +$10000/+$18000 Phase E; MACE, SCC, SWIM3 Phase F).
//
// Register truth: the shipping ROM's Open Firmware device tree and 68k
// DecoderInfo tables, the ROM's own NanoKernel interrupt handler, and
// OSF/Apple MkLinux DR3 (powermac_pci.h, whose window offsets match entry
// for entry).  Interrupt semantics: see the interrupt-block comments.

#include "tnt.h"

#include "dbdma.h"
#include "log.h"
#include "pci.h"
#include "ppc.h"
#include "scc.h"
#include "scsi_53c96.h"
#include "via.h"

#include <string.h>

LOG_USE_CATEGORY_NAME("gc");

// Island offsets (relative to $F3000000)
#define OFF_INTS      0x00020u // +$20 Events / +$24 Mask / +$28 Clear / +$2C Levels
#define OFF_DBDMA     0x08000u // channels 0-10 at +$8000+n*$100 (dbdma.c)
#define OFF_DBDMA_END (OFF_DBDMA + 0x100u * TNT_DBDMA_CHANNELS)
#define OFF_SCSI0     0x10000u // 53C94, external bus (Phase E)
#define OFF_MACE      0x11000u // MACE Ethernet (Phase F)
#define OFF_SCCLEG    0x12000u // SCC legacy aperture (Phase F)
#define OFF_ESCC      0x13000u // ESCC: channel B at +0, channel A at +$20 (Phase F)
#define OFF_AWACS     0x14000u // AWACS codec + sound control (Phase D)
#define OFF_SWIM3     0x15000u // SWIM3 floppy (Phase F)
#define OFF_VIA       0x16000u // VIA1/Cuda: 16 byte regs on $200 centres (8 KB)
#define OFF_MESH      0x18000u // MESH, internal bus (Phase E)
#define OFF_EPROM     0x19000u // Ethernet address PROM (+ the ANS MP doorbell)
#define OFF_BOXID     0x1A000u // machine-identification register (LE)
#define OFF_RADACAL   0x1B000u // RAMDAC colormap bank (Phase D)
#define OFF_LCDGB     0x1C000u // GBUS device 3: ANS front-panel LCD (lcd.c)
#define OFF_BREG2     0x1E000u // ANS Board Register 2: environment (gbus.c)
#define OFF_NVPORT    0x1D000u // NVRAM bank-select port
#define OFF_NVDATA    0x1F000u // NVRAM data window: byte j at +j*$10

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
// Bit map, pinned empirically at ladder rungs T4/T6 (the community's
// "bits 11-12 model code" reading is dead — both identification halves
// decoded from the ROM):
//   0-5   PCI slot power/present pins (empty slots: 0)
//   6-7   SCC RTS-A/B readback
//   8     factory-test strap; POST tests it (modeled pulled high = normal;
//         set sends the boot into the ROM's serial test monitor)
//   9     microphone sense
//   10    Ethernet 10BT link
//   11    SET = 8500 (the 68k identification routine at ROM $FFC14844)
//   12    unread by either identification
//   13    SET = 7500 (Open Firmware's model decode splits the 7500/8500
//         class — selected by Hammerhead +$20 bit 31 — on this bit)
//   14    MESH / fast-SCSI present (all three TNT boards)
//   15    unused, pulled high
// The per-model composite values live in the board descriptors (pm7500.c
// etc.); bits 0-5 are added here from the live slot population, because
// they are not a board constant — they are the power/present pins of the
// machine's PCI sockets, and an empty socket reads 0.

// The BoxID as software sees it: the board's straps plus one presence bit
// per POPULATED socket (bit n-1 for slot n, in the machine's declared slot
// order).  The builtin VCI entry is not a socket and contributes nothing.
uint32_t tnt_gc_boxid(config_t *cfg) {
    uint32_t id = tnt_board(cfg)->boxid;
    for (const pci_slot_decl_t *d = cfg->machine->pci_slots; d && d->slot; d++) {
        if (d->kind != PCI_SLOT_SOCKET || d->slot < 1 || d->slot > 6)
            continue;
        if (pci_slot_device(cfg->pci, d->slot))
            id |= 1u << (d->slot - 1);
    }
    // On a Network Server this same register is Board Register 1, and its
    // top byte is live board state rather than straps: the two active-low
    // keyswitch lines and TwoSuppliesH (gbus.c).  Bits 14 and 15 are NOT
    // the Macintosh boards' "MESH present" / idle-high pair there.
    if (tnt_board(cfg)->has_gbus)
        id |= tnt_gbus_boxid_bits(cfg);
    return id;
}

// ============================================================
// Interrupt fabric
// ============================================================

void tnt_gc_recompute(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    tnt_gc_t *gc = &st->gc;
    // Clear-mode 1 (the NanoKernel's scheme, selected by the $80000000
    // acknowledge): the CPU line is an OUTPUT LATCH, held PER SOURCE — a
    // source's bit is set by any CHANGE of it while enabled (assertion or
    // deassertion; see tnt_gc_set_source) or by an enabled event edge,
    // cleared by the acknowledge, re-asserted only by the NEXT change.
    // The handler classifies from Levels & Mask; a level a guest leaves
    // unserviced does not re-fire until its next change, and the kernel's
    // rfi does not land straight back in the handler.
    //
    // The mask gates the OUTPUT and not just the latching, because a guest
    // that masks a source has to be able to quiet it: AIX polls the
    // fast/wide SCSI controllers and leaves their externals masked, and a
    // latch the mask could not reach held the line up for a source the
    // guest had deliberately turned off.
    // Mode 0 (power-on; the MkLinux scheme): combinational
    // ((events | levels) & mask), with Events cleared by explicit W1C.
    bool line;
    if (gc->int_mode1)
        line = (gc->int_latch & gc->int_mask) != 0;
    else
        line = ((gc->int_events | gc->int_levels) & gc->int_mask) != 0;
    ppc_set_ext_irq(cfg->ppc, line);
}

// An enabled source edge sets the mode-1 output latch.
static void gc_edge(tnt_gc_t *gc, uint32_t bit) {
    gc->int_events |= bit;
    if (gc->int_mask & bit)
        gc->int_latch |= bit;
}

void tnt_gc_set_source(config_t *cfg, int n, bool level) {
    tnt_gc_t *gc = &tnt_st(cfg)->gc;
    uint32_t bit = 1u << n;
    bool was = (gc->int_levels & bit) != 0;
    if (level) {
        gc->int_levels |= bit;
        // Events latch source EDGES; a still-asserted source re-latches
        // only on its next assertion edge.
        if (!was)
            gc_edge(gc, bit);
    } else {
        gc->int_levels &= ~bit;
        // DEASSERTION of an enabled source latches too: mode 1 is an
        // interrupt-on-CHANGE scheme (same law as AMIC INTMODE=1, amic.c).
        // The deassert interrupt is how the NanoKernel learns a source went
        // away — ExtIntHandlerTNT re-reads Levels, finds them quiet, and
        // stores 68k IPL 0 through EmuIntLevelPtr.  Nothing else in the
        // kernel/emulator contract lowers the posted IPL (the emulator's
        // delivery path never touches it, and the TNT handler stores the
        // IplValue without the $8000 reprioritize flag), so without this
        // the 68k emulator redelivers the stale level forever and the
        // boot's base context never runs again after its first unmask.
        if (was && (gc->int_mask & bit))
            gc->int_latch |= bit;
    }
    if (was != level)
        LOG(3, "source %d %s (events=$%08X levels=$%08X mask=$%08X latch=$%08X)", n, level ? "asserted" : "cleared",
            gc->int_events, gc->int_levels, gc->int_mask, gc->int_latch);
    tnt_gc_recompute(cfg);
}

void tnt_gc_pulse_event(config_t *cfg, int n) {
    tnt_gc_t *gc = &tnt_st(cfg)->gc;
    gc_edge(gc, 1u << n);
    tnt_gc_recompute(cfg);
}

// The interrupt block is 32-bit little-endian; `value` here is the
// little-endian register value (the dispatcher swaps at the bus edge).
static uint32_t int_read(config_t *cfg, uint32_t offset) {
    tnt_gc_t *gc = &tnt_st(cfg)->gc;
    LOG(4, "int read +$%02X (events=$%08X levels=$%08X mask=$%08X latch=$%08X)", offset, gc->int_events, gc->int_levels,
        gc->int_mask, gc->int_latch);
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
        // no device bits (see INT_MODE_ACK above).  A Clear write also
        // selects the clear mode from bit 31: the NanoKernel writes
        // $80000000 on every interrupt (mode 1), MkLinux writes the event
        // bits themselves (mode 0).
        gc->int_events &= ~(value & ~INT_MODE_ACK);
        if (offset == INT_CLEAR) {
            gc->int_mode1 = (value & INT_MODE_ACK) != 0;
            if (gc->int_mode1)
                gc->int_latch = 0; // the acknowledge drops the line
        }
        LOG(3, "clear $%08X -> events $%08X (mode %d)", value, gc->int_events, gc->int_mode1);
        break;
    case INT_MASK: {
        // Enabling a source whose event or level is already pending
        // counts as an edge for the mode-1 latch (the enable is the first
        // moment the controller may assert for it).
        uint32_t newly = value & ~gc->int_mask;
        gc->int_mask = value;
        gc->int_latch |= (gc->int_events | gc->int_levels) & newly;
        LOG(3, "mask = $%08X (pc=%08X)", value, ppc_get_pc(cfg->ppc));
        break;
    }
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

// The data window decodes FIVE address bits for the cell — (offset >> 4) &
// $1F — and ignores the rest, so the window WRAPS every $200 bytes rather
// than ending.  POST leans on that: its NVRAM pattern test writes 4-byte
// values at cells $1D..$1F with its plain "+$10 per byte" accessor, so the
// last byte lands at +$200 and must alias cell 0 of the same bank (as the
// part wires it) for the read-back to match.  A model that dropped it
// failed every warm boot's "MainLBU NVRAM" test.
static uint32_t nvram_cell(uint32_t offset) {
    return (offset >> 4) & 0x1Fu;
}

static uint8_t nvram_read(config_t *cfg, uint32_t offset) {
    tnt_gc_t *gc = &tnt_st(cfg)->gc;
    if ((offset & 0xFu) != 0) {
        LOG(2, "NVRAM data read off-centre +$%03X", offset);
        return 0xFF;
    }
    uint32_t idx = ((uint32_t)gc->nvram_bank * 32u + nvram_cell(offset)) % TNT_NVRAM_SIZE;
    if (idx >= 0x1300u && idx < 0x1400u)
        LOG(4, "XPRAM read nv[$%04X] -> $%02X", idx, gc->nvram[idx]);
    return gc->nvram[idx];
}

static void nvram_write(config_t *cfg, uint32_t offset, uint8_t value) {
    tnt_gc_t *gc = &tnt_st(cfg)->gc;
    if ((offset & 0xFu) != 0) {
        LOG(2, "NVRAM data write off-centre +$%03X = $%02X", offset, value);
        return;
    }
    uint32_t idx = ((uint32_t)gc->nvram_bank * 32u + nvram_cell(offset)) % TNT_NVRAM_SIZE;
    if (idx >= 0x1300u && idx < 0x1400u)
        LOG(4, "XPRAM write nv[$%04X] = $%02X (pc=%08X)", idx, value, ppc_get_pc(cfg->ppc));
    else
        LOG(3, "NVRAM write nv[$%04X] = $%02X (pc=%08X)", idx, value, ppc_get_pc(cfg->ppc));
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

// ============================================================
// Grand Central's PCI presence — device 16 on Bandit 1
// ============================================================
// Apple's own device tree calls it /gc@10, and Open Firmware does issue a
// command + BAR write at IDSEL 16 during probe-slots (handover-phase-d
// §1).  What that write lands on is a documented open question
// (proposal-pci-architecture §14 Q7): the chip's real config identity is
// unrecovered, and the boot we have is known-good with a device that
// accepts every write and answers every config read ALL-ONES.  So that is
// exactly what this device does — as a named device instead of an
// accident of a `dev != 11` literal, which makes flipping it to a real
// header a two-line change once the identity is known.
//
// Note also what Grand Central is NOT: its 128 KB island at $F3000000 is
// reached by PASS-THROUGH MEMORY cycles, not through a BAR (ANS developer
// note §4.2.1), so tnt.c maps it at a fixed address and no BAR is
// declared here.
static bool gc_cfg_read(pci_device_t *dev, uint32_t reg, uint32_t *out) {
    (void)dev;
    (void)reg;
    *out = 0xFFFFFFFFu;
    return true;
}

static const char *gc_pci_name(const pci_device_t *dev) {
    (void)dev;
    return "Grand Central";
}

static const pci_config_decl_t gc_decl = {
    // No ids and no class: every config read answers all-ones above.
    .header_type = 0x00u,
    // The command register still latches what the firmware writes, so the
    // model is honest about accepting the cycle.
    .command_writable = PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE | PCI_CMD_MASTER,
};

static const pci_device_ops_t gc_pci_ops = {
    .cfg_read = gc_cfg_read,
    .name = gc_pci_name,
};

void tnt_gc_pci_attach(config_t *cfg, pci_bus_t *bus) {
    tnt_state_t *st = tnt_st(cfg);
    st->gc_dev.ops = &gc_pci_ops;
    st->gc_dev.decl = &gc_decl;
    st->gc_dev.priv = cfg;
    pci_cfg_reset(&st->gc_dev);
    pci_bus_add_device(bus, &st->gc_dev, 16);
}

// Map an ESCC-aperture offset (+$13000: B ctl +$00 / B data +$10 /
// A ctl +$20 / A data +$30) onto the SCC cell's classic address pins
// (A/B on address bit 1, D/C on bit 2 — the legacy-aperture layout the
// shared model decodes natively).
static uint32_t escc_pins(uint32_t off) {
    uint32_t ab = (off >> 5) & 1u; // A channel at +$20
    uint32_t dc = (off >> 4) & 1u; // data register at +$10
    return (ab << 1) | (dc << 2);
}

uint8_t tnt_gc_read8(config_t *cfg, uint32_t offset) {
    uint32_t block = offset & 0x1F000u;
    switch (block) {
    case OFF_VIA:
    case OFF_VIA + 0x1000: // 16 regs at stride $200 span the 8 KB window
        return via_get_memory_interface(cfg->via1)->read_uint8(cfg->via1, offset - OFF_VIA);
    case OFF_SCCLEG:
        // Legacy aperture: +0 bCtl / +2 aCtl / +4 bData / +6 aData — the
        // low offset bits carry the chip's A/B and D/C pins directly.
        return scc_get_memory_interface(cfg->scc)->read_uint8(cfg->scc, offset - OFF_SCCLEG);
    case OFF_ESCC:
        return scc_get_memory_interface(cfg->scc)->read_uint8(cfg->scc, escc_pins(offset - OFF_ESCC));
    case OFF_NVPORT:
        return tnt_st(cfg)->gc.nvram_bank;
    case OFF_NVDATA:
        return nvram_read(cfg, offset - OFF_NVDATA);
    case OFF_BOXID: {
        // Byte j of the little-endian register (the ROM reads it both as
        // lwbrx and byte-wise).  On the Network Servers this is Board
        // Register 1 and the ROM reads bytes +0 and +1 only.
        uint8_t b = (uint8_t)(tnt_gc_boxid(cfg) >> (8 * (offset & 3u)));
        LOG(3, "BoxID byte read +%u -> $%02X", offset & 3u, b);
        return b;
    }
    case OFF_LCDGB: // GBUS device 3 — LCD + timebase enable (ANS only)
        if (tnt_board(cfg)->has_gbus)
            return tnt_lcd_read8(cfg, offset - OFF_LCDGB);
        break;
    case OFF_EPROM: // Ethernet address PROM + MP doorbell (ANS only)
    case OFF_BREG2: // Board Register 2 — the environmental halfword
        if (tnt_board(cfg)->has_gbus)
            return tnt_gbus_read8(cfg, offset);
        break;
    case OFF_RADACAL:
        return tnt_control_rad_read(cfg, offset - OFF_RADACAL);
    case OFF_SCSI0:
        // 53C94: sixteen byte-wide registers on $10 centres.
        return scsi_53c96_read(tnt_st(cfg)->scsi96, ((offset - OFF_SCSI0) >> 4) & 0xFu);
    case OFF_MESH:
        // Absent on the Network Servers (board delta #4): the aperture
        // decodes nothing, so it falls through to the open-bus log.
        if (tnt_board(cfg)->has_mesh)
            return tnt_mesh_read(cfg, offset - OFF_MESH);
        LOG(1, "byte read of the absent MESH aperture +$%05X", offset);
        return 0;
    default:
        break;
    }
    LOG(1, "byte read of unwired island offset +$%05X", offset);
    return 0;
}

void tnt_gc_write8(config_t *cfg, uint32_t offset, uint8_t value) {
    uint32_t block = offset & 0x1F000u;
    switch (block) {
    case OFF_VIA:
    case OFF_VIA + 0x1000:
        via_get_memory_interface(cfg->via1)->write_uint8(cfg->via1, offset - OFF_VIA, value);
        return;
    case OFF_SCCLEG:
        scc_get_memory_interface(cfg->scc)->write_uint8(cfg->scc, offset - OFF_SCCLEG, value);
        return;
    case OFF_ESCC:
        scc_get_memory_interface(cfg->scc)->write_uint8(cfg->scc, escc_pins(offset - OFF_ESCC), value);
        return;
    case OFF_NVPORT:
        // The bank-select port is ONE byte-wide cell on the $10 centre.
        // Load-bearing: the ROM's XPRam trap path selects the bank with
        // a 16-bit write of the byte-swapped bank number (ROM $FFC5831E,
        // `move.w` of bank<<8) — the bus splits it into the +$1D000 byte
        // (the bank) and a +$1D001 byte ($00) that lands on NO cell.  A
        // model that latches the off-centre byte clobbers the bank back
        // to 0 and every trap-path PRAM read serves bank 0 — the T12
        // "XPRAM $77 reads 0" wall.
        if (((offset - OFF_NVPORT) & 0xFu) == 0)
            tnt_st(cfg)->gc.nvram_bank = value;
        else
            LOG(3, "NVRAM bank-port off-centre byte +$%03X = $%02X ignored", offset - OFF_NVPORT, value);
        return;
    case OFF_NVDATA:
        nvram_write(cfg, offset - OFF_NVDATA, value);
        return;
    case OFF_RADACAL:
        tnt_control_rad_write(cfg, offset - OFF_RADACAL, value);
        return;
    case OFF_LCDGB: // GBUS device 3 — the write-only LCD ports (ANS only)
        if (tnt_board(cfg)->has_gbus) {
            tnt_lcd_write8(cfg, offset - OFF_LCDGB, value);
            return;
        }
        break;
    case OFF_EPROM:
    case OFF_BREG2:
        if (tnt_board(cfg)->has_gbus) {
            tnt_gbus_write8(cfg, offset, value);
            return;
        }
        break;
    case OFF_SCSI0:
        scsi_53c96_write(tnt_st(cfg)->scsi96, ((offset - OFF_SCSI0) >> 4) & 0xFu, value);
        return;
    case OFF_MESH:
        if (tnt_board(cfg)->has_mesh)
            tnt_mesh_write(cfg, offset - OFF_MESH, value);
        else
            LOG(1, "byte write of the absent MESH aperture +$%05X = $%02X", offset, value);
        return;
    default:
        break;
    }
    LOG(1, "byte write of unwired island offset +$%05X = $%02X", offset, value);
}

// 32-bit access: the LE register blocks.  `value` at this boundary is the
// big-endian bus view; TNT_LE32 recovers the little-endian register value
// the guest composed with stwbrx (and vice versa on reads).
uint32_t tnt_gc_read32(config_t *cfg, uint32_t offset) {
    if (offset >= OFF_INTS && offset < OFF_INTS + 0x10u)
        return TNT_LE32(int_read(cfg, offset));
    if (offset >= OFF_DBDMA && offset < OFF_DBDMA_END) {
        // DBDMA channel n at +$8000+n*$100; registers are LE longwords.
        int chan = (int)((offset - OFF_DBDMA) >> 8);
        return TNT_LE32(tnt_dbdma_reg_read(tnt_st(cfg)->dbdma, chan, offset & 0xFFu));
    }
    if ((offset & 0x1F000u) == OFF_AWACS)
        return TNT_LE32(tnt_awacs_read32(cfg, offset - OFF_AWACS));
    if ((offset & 0x1F000u) == OFF_BOXID) {
        LOG(3, "BoxID read -> $%08X", tnt_gc_boxid(cfg));
        return TNT_LE32(tnt_gc_boxid(cfg));
    }
    // The Network Server's GBUS blocks: Board Register 2 (a little-endian
    // halfword like BoxID) and the Ethernet PROM's byte cells.
    if (tnt_board(cfg)->has_gbus && ((offset & 0x1F000u) == OFF_BREG2 || (offset & 0x1F000u) == OFF_EPROM))
        return tnt_gbus_read32(cfg, offset);
    // The NVRAM data window's byte cells answering a longword cycle.  The
    // production ANS ROM reads them this way; a byte-wide cell on this
    // big-endian bus drives lane 0, which is the MOST significant byte of
    // the longword.  Gated on the board because no Macintosh TNT ROM has
    // ever been observed issuing the cycle, and widening their decode
    // without a TNT ladder run to prove it would be a blind change.
    if (tnt_board(cfg)->has_gbus && (offset & 0x1F000u) == OFF_NVDATA)
        return (uint32_t)nvram_read(cfg, offset - OFF_NVDATA) << 24;
    LOG(1, "long read of unwired island offset +$%05X", offset);
    return 0;
}

void tnt_gc_write32(config_t *cfg, uint32_t offset, uint32_t value) {
    if (offset >= OFF_INTS && offset < OFF_INTS + 0x10u) {
        int_write(cfg, offset, TNT_LE32(value));
        return;
    }
    if (offset >= OFF_DBDMA && offset < OFF_DBDMA_END) {
        int chan = (int)((offset - OFF_DBDMA) >> 8);
        tnt_dbdma_reg_write(tnt_st(cfg)->dbdma, chan, offset & 0xFFu, TNT_LE32(value));
        return;
    }
    if ((offset & 0x1F000u) == OFF_AWACS) {
        tnt_awacs_write32(cfg, offset - OFF_AWACS, TNT_LE32(value));
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
