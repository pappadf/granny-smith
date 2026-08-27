// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scripts53c8xx.c
// The SCRIPTS instruction engine of the Symbios/LSI 53C8xx I/O processors,
// and the chip lifecycle and interrupt discipline it shares with the
// register file next door.  See sym53c8xx.h for why this is a separate
// translation unit and sym53c825.c for the chip itself.
//
// Reference: Symbios Logic, "PCI-SCSI I/O Processors Programming Guide",
// v2.1, Chapter 6 ("Instruction Set of the I/O Processor").
//
// THE INTERRUPT DISCIPLINE, which is the part a driver spins on.  Three
// registers, and each has a different law:
//
//   ISTAT  is a SUMMARY.  Its DIP and SIP bits are live views of "does
//          DSTAT hold anything" and "does SIST0/SIST1 hold anything", never
//          stored state.  It is the only register a driver may touch while
//          SCRIPTS run, which is exactly why polled drivers read it.
//   DSTAT / SIST0 / SIST1 are CAUSES, and they are READ TO CLEAR.  Leave a
//          bit standing and the driver re-interrupts forever; clear it a
//          moment too early and the cause is lost.
//   IRQ/   follows (causes AND enables).  A MASKED fatal condition still
//          halts SCRIPTS and still sets its status bit — "the SCRIPTS still
//          stop … but the IRQ/ pin is not asserted."  Masking an interrupt
//          on this part does not mean ignoring the event.
//
// And the rule that the DBDMA work paid for once already: STATUS MUST
// CHANGE SYNCHRONOUSLY WITH THE CONTROL WRITE.  A driver writes DSP (or
// DCNTL's START DMA) and immediately polls; if the engine's effect on
// ISTAT/DSTAT lands later, the driver's first `while (running)` loop spins
// forever with nothing to diagnose.

#include "sym53c8xx.h"

#include "card.h"
#include "log.h"
#include "memory.h"
#include "pci.h"
#include "scsi.h"
#include "system.h"
#include "system_config.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("scripts");

// ============================================================
// Host-memory access as a bus master
// ============================================================
// The engine fetches instructions and moves data through the host's
// physical address space.  RAM goes through the backing store directly;
// anything else (a table-indirect descriptor pointing at a device register,
// say) takes the bus's slow path.  The CPU MMU is deliberately NOT in the
// path — the same rule DBDMA follows.

void sym53c8xx_read_block(sym53c8xx_t *s, uint32_t phys, uint8_t *buf, uint32_t len) {
    config_t *cfg = s->cfg;
    if (cfg && cfg->mem_map && phys < cfg->ram_size && len <= cfg->ram_size - phys) {
        memcpy(buf, ram_native_pointer(cfg->mem_map, 0) + phys, len);
        return;
    }
    for (uint32_t i = 0; i < len; i++)
        buf[i] = memory_read_uint8_slow(phys + i);
}

void sym53c8xx_write_block(sym53c8xx_t *s, uint32_t phys, const uint8_t *buf, uint32_t len) {
    config_t *cfg = s->cfg;
    if (cfg && cfg->mem_map && phys < cfg->ram_size && len <= cfg->ram_size - phys) {
        memcpy(ram_native_pointer(cfg->mem_map, 0) + phys, buf, len);
        return;
    }
    for (uint32_t i = 0; i < len; i++)
        memory_write_uint8_slow(phys + i, buf[i]);
}

// One big-endian longword of host memory.  SCRIPTS instructions are stored
// in the host's byte order, and this host is big-endian.
uint32_t sym53c8xx_read32(sym53c8xx_t *s, uint32_t phys) {
    uint8_t b[4];
    sym53c8xx_read_block(s, phys, b, 4);
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

// ============================================================
// Interrupts
// ============================================================

void sym53c8xx_update_irq(sym53c8xx_t *s) {
    if (!s)
        return;
    // Enables gate only the PIN, never the latch (see the header comment).
    bool dma = (s->dstat & s->reg[SYM825_DIEN]) != 0;
    bool scsi = ((s->sist0 & s->reg[SYM825_SIEN0]) | (s->sist1 & s->reg[SYM825_SIEN1])) != 0;
    bool want = (dma || scsi) && !(s->reg[SYM825_DCNTL] & SYM825_DCNTL_IRQD);
    if (want == s->irq)
        return;
    s->irq = want;
    if (!s->dev)
        return;
    if (want)
        pci_assert_irq(s->dev);
    else
        pci_deassert_irq(s->dev);
}

void sym53c8xx_raise_dma(sym53c8xx_t *s, uint8_t dstat_bits) {
    s->dstat |= dstat_bits;
    // Every DSTAT cause except the single-step marker halts the engine.
    if (dstat_bits & ~SYM825_DSTAT_SSI)
        s->running = false;
    sym53c8xx_update_irq(s);
}

void sym53c8xx_raise_scsi(sym53c8xx_t *s, uint8_t sist0_bits, uint8_t sist1_bits) {
    s->sist0 |= sist0_bits;
    s->sist1 |= sist1_bits;
    // "When the LSI53C825A is operating in Initiator mode, only the Function
    // Complete (CMP), Selected (SEL), Reselected (RSL), General Purpose
    // Timer Expired (GEN), and Handshake-to-Handshake Timer Expired (HTH)
    // interrupts are nonfatal."  Everything else stops SCRIPTS.
    uint8_t nonfatal0 = SYM825_SIST0_CMP | SYM825_SIST0_SEL | SYM825_SIST0_RSL;
    uint8_t nonfatal1 = SYM825_SIST1_GEN | SYM825_SIST1_HTH;
    if ((sist0_bits & ~nonfatal0) || (sist1_bits & ~nonfatal1))
        s->running = false;
    sym53c8xx_update_irq(s);
}

// ============================================================
// The instruction engine
// ============================================================
// Not yet built: this lands in the SCRIPTS phase, together with its
// mock-bus unit suite covering all five instruction classes (Block Move,
// I/O, Transfer Control, Memory Move, Load and Store), phase mismatch and
// every interrupt condition, including a negative case per class.
//
// Until then the engine reports an ILLEGAL INSTRUCTION rather than
// pretending to run, which is the honest answer AND the safe one: a driver
// gets a defined error with a cause it can read, instead of the spin loop
// that a silently-idle engine would produce.  Nothing on the boot path
// reaches here before Open Firmware's `probe-scsi1` — the machine's whole
// identity, device tree and interrupt map are proven with SCRIPTS absent.
void sym53c8xx_start(sym53c8xx_t *s) {
    if (!s)
        return;
    uint32_t dsp = (uint32_t)s->reg[SYM825_DSP] | ((uint32_t)s->reg[SYM825_DSP + 1] << 8) |
                   ((uint32_t)s->reg[SYM825_DSP + 2] << 16) | ((uint32_t)s->reg[SYM825_DSP + 3] << 24);
    LOG(1, "channel %d: START at DSP $%08X — the SCRIPTS engine is not built yet", s->channel, dsp);
    s->running = false;
    sym53c8xx_raise_dma(s, SYM825_DSTAT_IID);
}

// ============================================================
// Lifecycle
// ============================================================

void sym53c8xx_chip_reset(sym53c8xx_t *s) {
    if (!s)
        return;
    // Power-on / SRST.  The SCRIPTS RAM is host memory and survives, as it
    // does on the part; everything else returns to its reset value.
    memset(s->reg, 0, sizeof(s->reg));
    s->dstat = 0;
    s->sist0 = 0;
    s->sist1 = 0;
    s->running = false;
    s->connected = false;
    s->target = 0;
    s->phase = SYM825_PHASE_MSG_OUT;
    s->insn_count = 0;
    // The chip's own SCSI ID.  7 is the initiator convention on every SCSI
    // bus this repository models, and on this board it is what leaves ids
    // 0-6 for the drive bays the backplane wires.
    s->reg[SYM825_SCID] = 7u;
    s->irq = true; // force update_irq to drive the line down
    sym53c8xx_update_irq(s);
}

sym53c8xx_t *sym53c8xx_new(config_t *cfg, int channel) {
    sym53c8xx_t *s = (sym53c8xx_t *)calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->cfg = cfg;
    s->channel = channel;
    // The Apple Network Server straps BIG_LIT/ big-endian (Apple, Network
    // Server Hardware Developer Notes, 1996, §2.9).  A socketed 53C8xx card
    // on some other machine would pass false here.
    s->big_endian = true;
    sym53c8xx_chip_reset(s);
    return s;
}

void sym53c8xx_delete(sym53c8xx_t *s) {
    free(s);
}

void sym53c8xx_attach_bus(sym53c8xx_t *s, struct scsi *bus) {
    if (s)
        s->bus = bus;
}

void sym53c8xx_checkpoint_save(sym53c8xx_t *s, checkpoint_t *cp) {
    if (!s || !cp)
        return;
    system_write_checkpoint_data(cp, s->reg, sizeof(s->reg));
    system_write_checkpoint_data(cp, &s->dstat, sizeof(s->dstat));
    system_write_checkpoint_data(cp, &s->sist0, sizeof(s->sist0));
    system_write_checkpoint_data(cp, &s->sist1, sizeof(s->sist1));
    system_write_checkpoint_data(cp, s->script_ram, sizeof(s->script_ram));
    system_write_checkpoint_data(cp, &s->running, sizeof(s->running));
    system_write_checkpoint_data(cp, &s->connected, sizeof(s->connected));
    system_write_checkpoint_data(cp, &s->target, sizeof(s->target));
    system_write_checkpoint_data(cp, &s->phase, sizeof(s->phase));
}

void sym53c8xx_checkpoint_restore(sym53c8xx_t *s, checkpoint_t *cp) {
    if (!s || !cp)
        return;
    system_read_checkpoint_data(cp, s->reg, sizeof(s->reg));
    system_read_checkpoint_data(cp, &s->dstat, sizeof(s->dstat));
    system_read_checkpoint_data(cp, &s->sist0, sizeof(s->sist0));
    system_read_checkpoint_data(cp, &s->sist1, sizeof(s->sist1));
    system_read_checkpoint_data(cp, s->script_ram, sizeof(s->script_ram));
    system_read_checkpoint_data(cp, &s->running, sizeof(s->running));
    system_read_checkpoint_data(cp, &s->connected, sizeof(s->connected));
    system_read_checkpoint_data(cp, &s->target, sizeof(s->target));
    system_read_checkpoint_data(cp, &s->phase, sizeof(s->phase));
    s->irq = !s->irq; // force the pin to be re-derived
    sym53c8xx_update_irq(s);
}
