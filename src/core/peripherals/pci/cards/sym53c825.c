// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// sym53c825.c
// The Symbios Logic 53C825A PCI-to-SCSI I/O processor — the fast/wide SCSI
// controller the Apple Network Server carries two of, in place of the
// Macintosh boards' MESH.
//
// > "The network servers implement two fast/wide SCSI channels (up to 40
// >  Mbytes/sec) using the Symbios Logi 53C825A.  These devices use SCRIPTS
// >  based DMA for high performance with low overhead."
// >  (Apple, "Network Server Hardware Developer Notes", 1996, §2.9.)
//
// This file is the CHIP: PCI identity, the 128-byte operating register
// file, the interrupt/status discipline and the SCSI front end.  The
// instruction engine that makes the part useful lives next door in
// scripts53c8xx.c, deliberately in its own translation unit with a narrow
// interface, so it is testable against a mock bus the way dbdma is.
//
// Register truth: Symbios Logic, "PCI-SCSI I/O Processors Programming
// Guide", v2.1, and LSI Logic, "LSI53C825A/825AE PCI to SCSI I/O Processor
// Technical Manual", v3.1 (2001), Chapters 4 and 5.
//
// === Three facts that are load-bearing, and easy to get wrong ==============
//
// IDENTITY IS MACHINE IDENTITY.  The ANS ROM detects these two devices,
// sets its `?esb` flag, publishes root `compatible` as `AAPL,ShinerESB` +
// `MacRISC`, drops `mesh` from the device tree and takes the Network Server
// paths.  Nothing about the machine behaves correctly until config space at
// devices 17 and 18 answers correctly — even with SCRIPTS entirely absent.
//
// THE REVISION ID GATES THAT.  The "A" is a die revision, not a separate
// device ID, so the part cannot be identified by vendor/device alone and
// the ROM carries a dedicated probe:
//
//     : 825a?  fwscsi#  8 +  config-l@   10 and  ;   \ bit 4 of Revision ID
//
// Both vendor manuals state the mechanism — "The devices are uniquely
// identified in the upper nibble of the Revision ID register" — and the LSI
// edition prints the values as a per-part bit table: the 825AE is 0x26 and
// the plain 825A, which is what this board carries, is **0x14**.  Upper
// nibble 1 = the part (bit 4 SET, so the ROM's test passes and `?esb-evt2`
// arms); lower nibble 4 = the revision level, which CTEST3's V[3:0] field
// must mirror.  An emulated 825A whose bit 4 is clear is not an ANS.
//
// ENDIANNESS IS A PIN, AND THIS BOARD DOES NOT ASSERT IT.  The chip's
// `BIG_LIT/` pin selects byte ordering, and Apple's developer note
// describes what the big-endian setting does ("the first byte of an aligned
// SCSI-to-PCI transfer routes to lane three and subsequent bytes to
// descending lanes"), which reads like a statement about this board.  The
// ROM says otherwise, three times over:
//
//   * its register accessors FLIP.  `see dsp!` at the Open Firmware prompt
//     gives `: dsp!  regs >dsp rl!-flip ;` — a byte-reversed longword
//     store, which is exactly what a big-endian host needs in order to
//     write a LITTLE-endian chip register;
//   * its byte offsets are NOT repositioned.  In big-endian mode the chip
//     moves a byte register's address to `N ^ 3`; the ROM writes SCNTL1 at
//     +$01, SCNTL3 at +$03, SCID at +$04, CTEST3 at +$1B and STIME0 at
//     +$48 — every one of them the natural register number;
//   * its SCRIPTS are stored byte-reversed in memory.  The buffer the
//     driver hands DSP holds `00000240 00000000 06000002 …`, which is
//     `40020000 00000000 02000006 …` read little-endian — a Select of
//     target 2 followed by a six-byte Command block move.
//
// So the strap is LITTLE-endian here, and it stays a construction
// parameter rather than a constant because it is a wiring fact: the
// `SYM53C825AJ` variant is little-endian ONLY (its BIG_LIT pin is a JTAG
// signal), and a socketed 53C8xx on some other board may be strapped the
// other way.
//
// One consequence lands in this file rather than the engine: the operating
// register file is byte-addressed with the register's LOW byte at its LOW
// offset, while the host bus is big-endian and delivers a 32-bit store MSB
// first.  So a wide access decomposes in BUS order and the register
// reassembles in CHIP order — which is precisely the swap the ROM's
// `-flip` words are compensating for, seen from the other side.

#include "card.h"
#include "log.h"
#include "pci.h"
#include "sym53c8xx.h"
#include "system_config.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("53c825");

// === PCI identity ===========================================================
// Vendor/device are attested three ways: Apple's Software Developer Notes
// print `pci1000,3 for the 825A SCSI adapter` as the Open Firmware node
// name; AIX 4.1.5 ships the driver fileset `devices.pci.pci1000+3`
// ("High Performance 53c825 SCSI Software"); and stock AIX 4.1.3's own
// signature table defines P825_SIGNATURE as 0x00031000.
#define SYM825_VENDOR_ID 0x1000u // Symbios Logic
#define SYM825_DEVICE_ID 0x0003u // 53C825 / 53C825A (see the Revision ID note)
#define SYM825_REVISION  0x14u // the plain 53C825A (LSI TM v3.1, register 0x08)
#define SYM825_CLASS     0x010000u // mass storage / SCSI / no programming interface

// BAR geometry (LSI TM v3.1, Table 4.1).  Base Address Zero maps the
// operating registers into I/O space and Base Address One maps the same
// registers into memory space; Base Address Two is the 4 KB SCRIPTS RAM,
// which "powers up enabled and can be disabled by pull-down resistors on
// the MAD5 pin".  The expansion-ROM BAR exists only "if expansion memory is
// enabled through pull-down resistors on the MAD[7:0] bus" — this board
// fits no BIOS ROM (Open Firmware builds the node itself), so it reads
// zero and Open Firmware never tries to size it.
#define SYM825_BAR_IO   0
#define SYM825_BAR_MEM  1
#define SYM825_BAR_RAM  2
#define SYM825_REG_SPAN 0x100u // 256 bytes decoded; 128 registers implemented

static const pci_config_decl_t sym825_decl = {
    .vendor_id = SYM825_VENDOR_ID,
    .device_id = SYM825_DEVICE_ID,
    .revision = SYM825_REVISION,
    .class_code = SYM825_CLASS,
    .header_type = 0x00u,
    .interrupt_pin = 1u, // INTA; the board straps it to Grand Central EXT2/EXT6
    .command_writable = PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE | PCI_CMD_MASTER,
    .bar =
        {
              [SYM825_BAR_IO] = {.size = SYM825_REG_SPAN, .kind = PCI_BAR_IO},
              [SYM825_BAR_MEM] = {.size = SYM825_REG_SPAN, .kind = PCI_BAR_MEM},
              [SYM825_BAR_RAM] = {.size = SYM825_SCRIPTS_RAM, .kind = PCI_BAR_MEM},
              },
};

// ============================================================
// The operating register file
// ============================================================
// 128 byte-wide registers, most of them plain store-and-readback.  What is
// NOT plain is called out here; everything else falls through to the array.
//
// The registers a driver spins on are the ones worth getting exactly right,
// because the failure mode is a silent hang rather than a wrong answer.
// ISTAT is the interrupt summary a polling driver reads; DSTAT and SIST0/1
// are its two cause registers and they are READ-TO-CLEAR, which means a
// model that leaves the bits standing re-interrupts forever, and one that
// clears them too eagerly loses the cause.

static uint8_t sym825_reg_read(sym53c8xx_t *s, uint32_t reg) {
    switch (reg) {
    case SYM825_ISTAT:
        // The summary register: DIP and SIP are live views of whether the
        // DMA and SCSI cause registers hold anything, never stored state.
        return (uint8_t)((s->reg[SYM825_ISTAT] & ~(SYM825_ISTAT_DIP | SYM825_ISTAT_SIP)) |
                         (s->dstat ? SYM825_ISTAT_DIP : 0u) | ((s->sist0 | s->sist1) ? SYM825_ISTAT_SIP : 0u));
    case SYM825_DSTAT: {
        // Read-to-clear.  DFE (DMA FIFO empty) is a live condition and is
        // not part of the latched cause, so it survives the read.
        uint8_t v = (uint8_t)(s->dstat | SYM825_DSTAT_DFE);
        s->dstat = 0;
        sym53c8xx_update_irq(s);
        return v;
    }
    case SYM825_SIST0: {
        uint8_t v = s->sist0;
        s->sist0 = 0;
        sym53c8xx_update_irq(s);
        return v;
    }
    case SYM825_SIST1: {
        uint8_t v = s->sist1;
        s->sist1 = 0;
        sym53c8xx_update_irq(s);
        return v;
    }
    case SYM825_GPREG:
        // GPIO[3:0] are INPUT pins at power-up; what a read returns is what
        // the board drives onto them, not what software last wrote.  Only
        // the bits GPCNTL declares as outputs read back the latch.
        return (uint8_t)((s->reg[SYM825_GPREG] & s->reg[SYM825_GPCNTL]) |
                         (s->gpio_strap & (uint8_t)~s->reg[SYM825_GPCNTL]));
    case SYM825_CTEST3:
        // V[3:0], the chip revision level, "should have the same value as
        // the lower nibble of the PCI Revision ID register" — 4 here.
        return (uint8_t)((SYM825_REVISION & 0x0Fu) << 4 | (s->reg[SYM825_CTEST3] & 0x0Fu));
    default:
        return s->reg[reg];
    }
}

// SCNTL1's RST bit drives the SCSI RST/ line.  The driver pulses it — set,
// wait, clear — and every device on the bus goes back to its power-on
// state, which is how a driver recovers a bus it has lost track of.  The
// chip sees its own RST/ like any other initiator would and reports it, so
// the assertion raises SIST0[RST]; AIX's driver has a handler named for
// exactly that (`bsc_scsi_reset_received`).
#define SYM825_SCNTL1_RST 0x08u

static void sym825_scntl1_write(sym53c8xx_t *s, uint8_t value) {
    bool was = (s->reg[SYM825_SCNTL1] & SYM825_SCNTL1_RST) != 0;
    bool now = (value & SYM825_SCNTL1_RST) != 0;
    s->reg[SYM825_SCNTL1] = value;
    if (now == was)
        return;
    if (!now) {
        LOG(2, "ch%d: SCSI RST/ released", s->channel);
        return;
    }
    LOG(2, "ch%d: SCSI RST/ asserted — resetting the bus", s->channel);
    sym53c8xx_bus_reset(s);
}

static void sym825_reg_write(sym53c8xx_t *s, uint32_t reg, uint8_t value) {
    switch (reg) {
    case SYM825_SCNTL1:
        sym825_scntl1_write(s, value);
        return;
    case SYM825_ISTAT: {
        // INTF is write-ONE-to-clear, and it is the one bit here a driver
        // acknowledges rather than sets: a plain store of the value it just
        // read would re-arm the very interrupt it is dismissing.
        uint8_t intf = (uint8_t)(s->reg[SYM825_ISTAT] & SYM825_ISTAT_INTF);
        if (value & SYM825_ISTAT_INTF)
            intf = 0;
        s->reg[reg] = (uint8_t)((value & (uint8_t)~SYM825_ISTAT_INTF) | intf);
        // A software reset clears the chip but not the PCI header (that is
        // RST#'s job) — SRST is self-clearing.
        if (value & SYM825_ISTAT_SRST) {
            LOG(2, "software reset (ISTAT SRST)");
            sym53c8xx_chip_reset(s);
            return;
        }
        // ABRT is the driver's escape from an operation that is not going
        // to finish on its own — the one thing it can do to a chip that is
        // arbitrating for a target which will never answer.  The operation
        // is abandoned and the cause reported; without it the driver's
        // recovery has nothing to act on, and whatever the chip was doing
        // lands later, against a command the driver has already given up
        // on and freed.
        if (value & SYM825_ISTAT_ABRT) {
            LOG(2, "ch%d: ABRT — the driver is abandoning the current operation", s->channel);
            sym53c8xx_abort(s);
            return;
        }
        sym53c8xx_update_irq(s);
        // SIGP is the driver's doorbell on a script parked at Wait
        // Reselect: setting it is how the CPU tells an idle engine it has
        // work.  The instruction is re-executed, sees the bit, clears it
        // and takes its alternate address — the whole point of parking.
        if ((value & SYM825_ISTAT_SIGP) && s->waiting_reselect)
            sym53c8xx_start(s);
        return;
    }
    case SYM825_DSTAT:
    case SYM825_SIST0:
    case SYM825_SIST1:
        LOG(3, "write to read-only status register $%02X = $%02X ignored", reg, value);
        return;
    case SYM825_DCNTL:
        s->reg[reg] = value;
        // START DMA: the driver has written DSP and wants the engine to
        // fetch.  The status MUST change synchronously with this write —
        // the DBDMA lesson, transplanted: a driver's first `while (running)`
        // loop hangs with no diagnostic otherwise.
        if (value & SYM825_DCNTL_STD)
            sym53c8xx_start(s);
        return;
    case SYM825_DSP + 3:
        // Writing the HIGH byte of DSP begins execution (LSI TM v3.1: "When
        // writing this register eight bits at a time, writing the upper
        // eight bits begins execution of SCSI SCRIPTS").  The register file
        // is little-endian within the chip, so byte 3 is the top.
        s->reg[reg] = value;
        sym53c8xx_start(s);
        return;
    default:
        s->reg[reg] = value;
        return;
    }
}

// ============================================================
// BAR windows
// ============================================================
// The same 128 registers answer through the I/O BAR and the memory BAR;
// which one a driver uses is its business.  The register file is BYTE
// addressed and byte-wide, and wider accesses decompose — little-endian
// within the chip, because that is what the chip is, regardless of how the
// host bus is strapped (the BIG_LIT strap governs DATA lanes on transfers,
// not the register file's own numbering).

static uint8_t regs_read8(void *ctx, uint32_t offset) {
    sym53c8xx_t *s = (sym53c8xx_t *)ctx;
    if (offset >= SYM825_REGS) {
        LOG(3, "read above the implemented register file +$%02X -> 0", offset);
        return 0;
    }
    uint8_t v = sym825_reg_read(s, offset);
    LOG(5, "ch%d read +$%02X -> $%02X", s->channel, offset, v);
    return v;
}

static void regs_write8(void *ctx, uint32_t offset, uint8_t value) {
    sym53c8xx_t *s = (sym53c8xx_t *)ctx;
    if (offset >= SYM825_REGS) {
        LOG(3, "write above the implemented register file +$%02X = $%02X ignored", offset, value);
        return;
    }
    LOG(5, "ch%d write +$%02X = $%02X", s->channel, offset, value);
    sym825_reg_write(s, offset, value);
}

// Wider accesses decompose in BUS order — big-endian, MSB at the lowest
// address — because that is what the processor bus delivers.  The register
// file then reassembles them in CHIP order (low byte at the low offset),
// which is the byte swap the ROM's `rl!-flip` performs from its side.
static uint16_t regs_read16(void *ctx, uint32_t offset) {
    return (uint16_t)(((uint16_t)regs_read8(ctx, offset) << 8) | regs_read8(ctx, offset + 1));
}

static void regs_write16(void *ctx, uint32_t offset, uint16_t value) {
    regs_write8(ctx, offset, (uint8_t)(value >> 8));
    regs_write8(ctx, offset + 1, (uint8_t)value);
}

static uint32_t regs_read32(void *ctx, uint32_t offset) {
    return ((uint32_t)regs_read16(ctx, offset) << 16) | regs_read16(ctx, offset + 2);
}

static void regs_write32(void *ctx, uint32_t offset, uint32_t value) {
    regs_write16(ctx, offset, (uint16_t)(value >> 16));
    regs_write16(ctx, offset + 2, (uint16_t)value);
}

// The 4 KB SCRIPTS RAM: ordinary host-visible memory the engine can also
// fetch from, "byte accessible from the PCI bus and visible to any bus
// mastering device on the bus".
static uint8_t ram_read8(void *ctx, uint32_t offset) {
    sym53c8xx_t *s = (sym53c8xx_t *)ctx;
    return s->script_ram[offset & (SYM825_SCRIPTS_RAM - 1u)];
}

static void ram_write8(void *ctx, uint32_t offset, uint8_t value) {
    sym53c8xx_t *s = (sym53c8xx_t *)ctx;
    s->script_ram[offset & (SYM825_SCRIPTS_RAM - 1u)] = value;
}

static uint16_t ram_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((ram_read8(ctx, offset) << 8) | ram_read8(ctx, offset + 1));
}

static void ram_write16(void *ctx, uint32_t offset, uint16_t value) {
    ram_write8(ctx, offset, (uint8_t)(value >> 8));
    ram_write8(ctx, offset + 1, (uint8_t)value);
}

static uint32_t ram_read32(void *ctx, uint32_t offset) {
    return ((uint32_t)ram_read16(ctx, offset) << 16) | ram_read16(ctx, offset + 2);
}

static void ram_write32(void *ctx, uint32_t offset, uint32_t value) {
    ram_write16(ctx, offset, (uint16_t)(value >> 16));
    ram_write16(ctx, offset + 2, (uint16_t)value);
}

// ============================================================
// Device lifecycle
// ============================================================

static const char *sym825_name(const pci_device_t *dev) {
    sym53c8xx_t *s = (sym53c8xx_t *)dev->priv;
    return s && s->channel ? "53C825A #1" : "53C825A #0";
}

static void sym825_pci_reset(pci_device_t *dev, config_t *cfg) {
    (void)cfg;
    sym53c8xx_chip_reset((sym53c8xx_t *)dev->priv);
}

static void sym825_teardown(pci_device_t *dev, config_t *cfg) {
    (void)cfg;
    sym53c8xx_delete((sym53c8xx_t *)dev->priv);
    dev->priv = NULL;
}

static void sym825_checkpoint_save(pci_device_t *dev, checkpoint_t *cp) {
    sym53c8xx_checkpoint_save((sym53c8xx_t *)dev->priv, cp);
}

static void sym825_checkpoint_restore(pci_device_t *dev, checkpoint_t *cp) {
    sym53c8xx_checkpoint_restore((sym53c8xx_t *)dev->priv, cp);
}

static const pci_device_ops_t sym825_ops = {
    .reset = sym825_pci_reset,
    .teardown = sym825_teardown,
    .checkpoint_save = sym825_checkpoint_save,
    .checkpoint_restore = sym825_checkpoint_restore,
    .name = sym825_name,
};

// Identify one of ours among the machine's seated devices: the config
// declaration is the card's identity and no other device shares it.
sym53c8xx_t *sym53c8xx_from_device(pci_device_t *dev) {
    return (dev && dev->decl == &sym825_decl) ? (sym53c8xx_t *)dev->priv : NULL;
}

// Which channel a seated instance is.  The machine's slot table names the
// same card kind twice, at IDSEL 17 and 18, so the factory derives the
// channel from the slot it was asked for rather than from a global counter
// — restarts and checkpoint restores then reproduce the same assignment.
static pci_device_t *sym825_factory_for(int slot_index, config_t *cfg, checkpoint_t *cp, int channel) {
    (void)cp;
    pci_device_t *dev = (pci_device_t *)calloc(1, sizeof(*dev));
    sym53c8xx_t *s = sym53c8xx_new(cfg, channel);
    if (!dev || !s) {
        free(dev);
        sym53c8xx_delete(s);
        return NULL;
    }
    dev->ops = &sym825_ops;
    dev->decl = &sym825_decl;
    dev->priv = s;
    pci_cfg_reset(dev);
    s->dev = dev;

    s->regs_if.read_uint8 = regs_read8;
    s->regs_if.read_uint16 = regs_read16;
    s->regs_if.read_uint32 = regs_read32;
    s->regs_if.write_uint8 = regs_write8;
    s->regs_if.write_uint16 = regs_write16;
    s->regs_if.write_uint32 = regs_write32;
    s->ram_if.read_uint8 = ram_read8;
    s->ram_if.read_uint16 = ram_read16;
    s->ram_if.read_uint32 = ram_read32;
    s->ram_if.write_uint8 = ram_write8;
    s->ram_if.write_uint16 = ram_write16;
    s->ram_if.write_uint32 = ram_write32;

    pci_bar_backing_iface(dev, SYM825_BAR_IO, &s->regs_if, s);
    pci_bar_backing_iface(dev, SYM825_BAR_MEM, &s->regs_if, s);
    pci_bar_backing_iface(dev, SYM825_BAR_RAM, &s->ram_if, s);

    LOG(1, "seated in slot %d as fast/wide channel %d (revision $%02X, %s-endian strapping)", slot_index, channel,
        SYM825_REVISION, s->big_endian ? "big" : "little");
    return dev;
}

static pci_device_t *sym825_ch0_factory(int slot_index, config_t *cfg, checkpoint_t *cp) {
    return sym825_factory_for(slot_index, cfg, cp, 0);
}

static pci_device_t *sym825_ch1_factory(int slot_index, config_t *cfg, checkpoint_t *cp) {
    return sym825_factory_for(slot_index, cfg, cp, 1);
}

// Two kinds rather than one, because a card kind's factory takes no channel
// argument and the two controllers are genuinely distinct devices on the
// board — different IDSELs, different Grand Central lines, different drive
// bays.  Both are BUILTIN: soldered down, instantiable only where a
// machine's slot table names them, never offered on a socket.
const pci_card_kind_t sym53c825_ch0_kind = {
    .id = "sym53c825_0",
    .display_name = "Symbios 53C825A fast/wide SCSI (channel 0)",
    .attach = PCI_ATTACH_BUILTIN,
    .card_class = "scsi",
    .factory = sym825_ch0_factory,
};

const pci_card_kind_t sym53c825_ch1_kind = {
    .id = "sym53c825_1",
    .display_name = "Symbios 53C825A fast/wide SCSI (channel 1)",
    .attach = PCI_ATTACH_BUILTIN,
    .card_class = "scsi",
    .factory = sym825_ch1_factory,
};
