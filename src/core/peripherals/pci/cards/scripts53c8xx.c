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
#include "scheduler.h"
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
    // Interrupt-on-the-fly drives the pin too, and it has no enable bit to
    // gate it: the whole point of the instruction is to tell the driver a
    // command finished WITHOUT stopping SCRIPTS, so a model that only
    // latches INTF leaves the driver waiting on an interrupt that a
    // perfectly healthy script already sent.
    bool fly = (s->reg[SYM825_ISTAT] & SYM825_ISTAT_INTF) != 0;
    bool want = (dma || scsi || fly) && !(s->reg[SYM825_DCNTL] & SYM825_DCNTL_IRQD);
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
// Register-file helpers
// ============================================================
// Multi-byte registers are LITTLE-endian within the chip: a register's low
// byte lives at its low offset.  That is a property of the part, not of the
// host bus — the BIG_LIT/ strap governs how DATA lanes are routed on
// transfers, not how the register file is numbered.

static uint32_t reg32(sym53c8xx_t *s, uint32_t off) {
    return (uint32_t)s->reg[off] | ((uint32_t)s->reg[off + 1] << 8) | ((uint32_t)s->reg[off + 2] << 16) |
           ((uint32_t)s->reg[off + 3] << 24);
}

static void set_reg32(sym53c8xx_t *s, uint32_t off, uint32_t v) {
    s->reg[off] = (uint8_t)v;
    s->reg[off + 1] = (uint8_t)(v >> 8);
    s->reg[off + 2] = (uint8_t)(v >> 16);
    s->reg[off + 3] = (uint8_t)(v >> 24);
}

// The 24-bit DMA Byte Counter shares its dword with DCMD, which occupies
// the top byte, so it can only be written through its own mask.
static void set_dbc(sym53c8xx_t *s, uint32_t v) {
    set_reg32(s, SYM825_DBC, (reg32(s, SYM825_DBC) & 0xFF000000u) | (v & 0x00FFFFFFu));
}

// Sign-extend a 24-bit relative displacement.  Used by every relative
// addressing mode in the instruction set, all of which are 24-bit signed.
static int32_t sext24(uint32_t v) {
    return (int32_t)((v & 0x00800000u) ? (v | 0xFF000000u) : (v & 0x00FFFFFFu));
}

// One instruction dword out of host memory.  The chip fetches its program
// in the HOST's byte order, which the BIG_LIT/ strap selects — big-endian
// on the Apple Network Server, little-endian on a PC.  Data payloads need
// no such treatment: a SCSI byte stream lands lowest-address-first either
// way, so Block Move data is a straight byte copy.
static uint32_t fetch32(sym53c8xx_t *s, uint32_t phys) {
    uint8_t b[4];
    sym53c8xx_read_block(s, phys, b, 4);
    if (s->big_endian)
        return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    return ((uint32_t)b[3] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[1] << 8) | b[0];
}

// ============================================================
// The message conversation
// ============================================================
// Everything in this block exists because our shared bus/target model
// (scsi.c) answers an external initiator with COMMAND phase the moment
// selection succeeds — it has no MESSAGE OUT for the IDENTIFY, and no
// notion of a negotiation.  MESH solved the identical problem the identical
// way (mesh.c "Sync negotiation"), and doing anything else here would mean
// teaching the shared model about a conversation only two chips have.

static bool msgin_pending(const sym53c8xx_t *s) {
    return s->mi_rd < s->mi_n;
}

static void msg_session_reset(sym53c8xx_t *s) {
    s->mo_len = 0;
    s->mi_n = 0;
    s->mi_rd = 0;
    s->msgin_taken = 0;
    s->msgout_pending = 0;
}

// Queue an EXTENDED MESSAGE reply for the script to read back.
static void msgin_queue_ext(sym53c8xx_t *s, uint8_t code, uint8_t a, uint8_t b, bool two) {
    s->mi_buf[0] = 0x01u; // EXTENDED MESSAGE
    s->mi_buf[1] = two ? 0x03u : 0x02u; // length
    s->mi_buf[2] = code;
    s->mi_buf[3] = a;
    if (two)
        s->mi_buf[4] = b;
    s->mi_n = two ? 5u : 4u;
    s->mi_rd = 0;
}

// The script finished its MESSAGE OUT: parse what it said.  Both
// negotiations are ANSWERED rather than rejected, because a fast/wide
// channel is expected to negotiate and a chip that always rejected would
// be lying about the part.  The emulated bus has no timing, so what is
// modelled is the agreement, not the rate it implies.
static void msgout_complete(sym53c8xx_t *s) {
    for (uint8_t i = 0; i < s->mo_len;) {
        uint8_t b = s->mo_buf[i];
        if (b & 0x80u) { // IDENTIFY
            i++;
        } else if (b == 0x01u) { // EXTENDED MESSAGE
            if ((uint32_t)i + 2u > s->mo_len || (uint32_t)i + 2u + s->mo_buf[i + 1] > s->mo_len)
                return; // still incomplete; more bytes are coming
            uint8_t len = s->mo_buf[i + 1];
            uint8_t code = s->mo_buf[i + 2];
            if (code == 0x01u && len == 3) { // SDTR: period, offset
                s->sync_period = s->mo_buf[i + 3];
                s->sync_offset = s->mo_buf[i + 4];
                LOG(3, "ch%d: SDTR agreed, period=%u offset=%u", s->channel, s->sync_period, s->sync_offset);
                msgin_queue_ext(s, 0x01u, s->sync_period, s->sync_offset, true);
            } else if (code == 0x03u && len == 2) { // WDTR: transfer width
                s->wide = s->mo_buf[i + 3] ? 1u : 0u;
                LOG(3, "ch%d: WDTR agreed, %s transfers", s->channel, s->wide ? "16-bit" : "8-bit");
                msgin_queue_ext(s, 0x03u, s->wide, 0, false);
            }
            i = (uint8_t)(i + 2 + len);
        } else {
            i++; // a single-byte message with nothing to answer
        }
    }
    s->mo_len = 0;
    s->msgout_pending = 0;
}

// The SCSI phase as the INITIATOR sees it, in the chip's own 3-bit
// encoding: the two virtual overlays first, then the shared bus model.
static uint8_t chip_phase(sym53c8xx_t *s) {
    if (s->msgout_pending)
        return SYM825_PHASE_MSG_OUT;
    if (msgin_pending(s))
        return SYM825_PHASE_MSG_IN;
    if (!s->bus)
        return SYM825_PHASE_MSG_IN;
    switch (scsi_get_bus_phase(s->bus)) {
    case scsi_command:
        return SYM825_PHASE_COMMAND;
    case scsi_data_in:
        return SYM825_PHASE_DATA_IN;
    case scsi_data_out:
        return SYM825_PHASE_DATA_OUT;
    case scsi_status:
        return SYM825_PHASE_STATUS;
    case scsi_message_in:
        return SYM825_PHASE_MSG_IN;
    case scsi_message_out:
        return SYM825_PHASE_MSG_OUT;
    default:
        return SYM825_PHASE_MSG_IN;
    }
}

// Publish the live phase where a driver expects to read it: SSTAT1's low
// three bits are the latched phase lines, and SBCL mirrors them.
static void publish_phase(sym53c8xx_t *s) {
    uint8_t p = s->connected ? chip_phase(s) : 0u;
    s->phase = p;
    s->reg[SYM825_SSTAT1] = (uint8_t)((s->reg[SYM825_SSTAT1] & 0xF8u) | p);
    s->reg[SYM825_SBCL] = (uint8_t)((s->reg[SYM825_SBCL] & 0xF8u) | p);
    if (s->connected)
        s->reg[SYM825_ISTAT] |= SYM825_ISTAT_CON;
    else
        s->reg[SYM825_ISTAT] &= (uint8_t)~SYM825_ISTAT_CON;
}

static void disconnect(sym53c8xx_t *s) {
    if (s->connected && s->bus)
        scsi_external_release(s->bus);
    s->connected = false;
    msg_session_reset(s);
    s->sync_period = 0;
    s->sync_offset = 0;
    s->wide = 0;
    // A disconnect the script ASKED for is not unexpected.  The deferred
    // UDC exists for scripts that never wait (Open Firmware's does not, and
    // ends on it — see disconnect_deferred); a script whose Wait Disconnect
    // consumes the same bus-free condition must not also be told the target
    // vanished, or a driver that logs and recovers from UDC treats every
    // successful command as a failure.
    s->disconnect_pending = 0;
    publish_phase(s);
}

// The target let go of the bus on its own.  THIS is how a completed command
// ends, and the shape of it is not obvious from the instruction set at all
// — it is the ROM's own driver that says so.  Its command loop is:
//
//     begin
//       istat 2 and while/if                          \ a SCSI-type cause
//         sist@ to sist
//         sist 400 and if … true eexit then           \ selection time-out
//         sist 4 and if  dcmd@ 98 <> eexit  then      \ UNEXPECTED DISCONNECT
//         …
//       istat 1 and while/if dstat@ to dstat then     \ a DMA-type cause
//     again
//
// There is no exit on the SCRIPTS INT at all.  Every command ends on
// UNEXPECTED DISCONNECT, and success versus failure is decided by WHERE it
// landed: `dcmd@ 98 <>` is false — no error — only when the last opcode
// fetched was $98, the Transfer Control INT that ends the script.  So the
// disconnect must be reported AFTER the script has run to its INT, never at
// the moment the bus goes free; report it early and DCMD still holds the
// Clear-ACK opcode, the driver calls the command failed, and it resets the
// bus and retries forever.
static void disconnect_deferred(sym53c8xx_t *s) {
    if (s->connected && s->bus)
        scsi_external_release(s->bus);
    s->connected = false;
    msg_session_reset(s);
    publish_phase(s);
    s->disconnect_pending = 1;
}

// ============================================================
// Block Move
// ============================================================
// The instruction the whole part exists for.  In initiator mode it waits
// for an unserviced phase, compares the phase the script asked for against
// what the target is presenting, and either moves the bytes or raises a
// PHASE MISMATCH and stops.  That comparison is the single most
// load-bearing behaviour in the instruction set: a driver drives the whole
// transaction by moving one phase at a time and branching on the mismatch.

// Move `count` bytes for `phase`, returning how many actually moved.  A
// short count means the target changed phase mid-transfer, which the
// caller turns into a mismatch.
static uint32_t block_move_bytes(sym53c8xx_t *s, uint8_t phase, uint32_t addr, uint32_t count, bool *sfbr_set) {
    uint8_t buf[512];
    uint32_t moved = 0;
    while (moved < count) {
        uint32_t chunk = count - moved;
        if (chunk > sizeof(buf))
            chunk = sizeof(buf);
        switch (phase) {
        case SYM825_PHASE_COMMAND:
        case SYM825_PHASE_DATA_OUT: {
            sym53c8xx_read_block(s, addr + moved, buf, chunk);
            for (uint32_t i = 0; i < chunk; i++) {
                if (scsi_get_bus_phase(s->bus) != (phase == SYM825_PHASE_COMMAND ? scsi_command : scsi_data_out))
                    return moved + i;
                scsi_push_data_out_byte(s->bus, buf[i]);
            }
            moved += chunk;
            break;
        }
        case SYM825_PHASE_MSG_OUT: {
            // Collected here rather than pushed at the bus: the target
            // side of this conversation is ours (see msgout_complete).
            sym53c8xx_read_block(s, addr + moved, buf, chunk);
            for (uint32_t i = 0; i < chunk; i++) {
                if (s->mo_len < sizeof(s->mo_buf))
                    s->mo_buf[s->mo_len++] = buf[i];
            }
            moved += chunk;
            break;
        }
        case SYM825_PHASE_DATA_IN: {
            for (uint32_t i = 0; i < chunk; i++) {
                uint8_t b;
                if (!scsi_pop_data_in_byte(s->bus, &b)) {
                    // The target ran out: a short DATA IN is a phase
                    // change, and the bus model needs telling that the
                    // phase is over before it will advance to STATUS.
                    // The bytes that DID arrive still have to land — a
                    // short transfer is not a discarded one, and the
                    // driver reads the residual count to find out how far
                    // it got.
                    if (i > 0)
                        sym53c8xx_write_block(s, addr + moved, buf, i);
                    scsi_external_data_in_complete(s->bus);
                    return moved + i;
                }
                buf[i] = b;
                if (!*sfbr_set) {
                    s->reg[SYM825_SFBR] = b;
                    *sfbr_set = true;
                }
            }
            sym53c8xx_write_block(s, addr + moved, buf, chunk);
            moved += chunk;
            break;
        }
        case SYM825_PHASE_STATUS: {
            for (uint32_t i = 0; i < chunk; i++) {
                int st = scsi_external_status_byte(s->bus);
                if (st < 0) {
                    if (i > 0)
                        sym53c8xx_write_block(s, addr + moved, buf, i);
                    return moved + i;
                }
                buf[i] = (uint8_t)st;
                if (!*sfbr_set) {
                    s->reg[SYM825_SFBR] = (uint8_t)st;
                    *sfbr_set = true;
                }
            }
            sym53c8xx_write_block(s, addr + moved, buf, chunk);
            moved += chunk;
            break;
        }
        case SYM825_PHASE_MSG_IN: {
            for (uint32_t i = 0; i < chunk; i++) {
                int msg;
                if (msgin_pending(s)) {
                    msg = s->mi_buf[s->mi_rd++];
                } else {
                    msg = scsi_external_message_byte(s->bus);
                    if (msg < 0) {
                        if (i > 0)
                            sym53c8xx_write_block(s, addr + moved, buf, i);
                        return moved + i;
                    }
                    // MESSAGE IN lingers in the bus model until it is
                    // released; once the byte is delivered the target REQs
                    // nothing more, and only a Wait Disconnect (or a Clear
                    // ACK followed by one) ends the connection.
                    s->msgin_taken = 1;
                }
                buf[i] = (uint8_t)msg;
                if (!*sfbr_set) {
                    s->reg[SYM825_SFBR] = (uint8_t)msg;
                    *sfbr_set = true;
                }
            }
            sym53c8xx_write_block(s, addr + moved, buf, chunk);
            moved += chunk;
            break;
        }
        default:
            return moved;
        }
    }
    return moved;
}

static void exec_block_move(sym53c8xx_t *s, uint32_t insn, uint32_t dsps) {
    uint8_t want = (uint8_t)((insn >> 24) & 7u);
    uint32_t count = insn & 0x00FFFFFFu;
    uint32_t addr = dsps;

    // Table indirect: both the byte count and the buffer address are
    // fetched from a structure at DSA + a 24-bit signed offset.  This is
    // what lets SCRIPTS execute an operating system's own I/O structures.
    if (insn & (1u << 28)) {
        uint32_t table = reg32(s, SYM825_DSA) + (uint32_t)sext24(dsps);
        count = fetch32(s, table) & 0x00FFFFFFu;
        addr = fetch32(s, table + 4);
    } else if (insn & (1u << 29)) {
        // Indirect: the instruction's address field points at the address.
        addr = fetch32(s, addr);
    }

    if (!s->connected || !s->bus) {
        LOG(1, "ch%d: Block Move with no connection", s->channel);
        sym53c8xx_raise_dma(s, SYM825_DSTAT_IID);
        return;
    }

    publish_phase(s);
    if (want != s->phase) {
        // PHASE MISMATCH.  The instruction does NOT execute, and DSP is
        // left pointing AT it so the driver can resume after servicing —
        // which is why DSP is rewound here rather than advanced.
        set_dbc(s, count);
        set_reg32(s, SYM825_DNAD, addr);
        set_reg32(s, SYM825_DSP, reg32(s, SYM825_DSP) - 8u);
        LOG(3, "ch%d: phase mismatch — script wants %u, target presents %u", s->channel, want, s->phase);
        sym53c8xx_raise_scsi(s, SYM825_SIST0_MA, 0);
        return;
    }

    bool sfbr_set = false;
    uint32_t moved = block_move_bytes(s, want, addr, count, &sfbr_set);
    set_dbc(s, count - moved);
    set_reg32(s, SYM825_DNAD, addr + moved);

    if (want == SYM825_PHASE_MSG_OUT && s->msgout_pending)
        msgout_complete(s);
    // A completed DATA IN has to tell the bus model the phase is over, or
    // the target never advances to STATUS.
    if (want == SYM825_PHASE_DATA_IN && moved == count && scsi_get_bus_phase(s->bus) == scsi_data_in)
        scsi_external_data_in_complete(s->bus);
    publish_phase(s);

    if (moved != count) {
        LOG(3, "ch%d: short Block Move (%u of %u bytes in phase %u)", s->channel, moved, count, want);
        sym53c8xx_raise_scsi(s, SYM825_SIST0_MA, 0);
    }
}

// STIME0's low nibble selects the selection/reselection time-out from a
// fixed table: 0 disables it, and 1..15 double from 100 microseconds to
// 1.6 seconds (LSI53C825A TM v3.1, STIME0).  AIX programs $0C — 204.8 ms.
static uint64_t select_timeout_ns(const sym53c8xx_t *s) {
    unsigned sel = s->reg[SYM825_STIME0] & 0x0Fu;
    if (sel == 0)
        return 0;
    return 100000ull << (sel - 1u);
}

// The time-out lands: the cause latches, the pin follows it, and the engine
// HALTS where it stands.  The alternate address is not this instruction's
// error exit; it belongs to the one other thing that can end an arbitration,
// and taking it here runs a handler written for a different accident.
static void select_timeout_event(void *source, uint64_t data) {
    (void)data;
    sym53c8xx_t *s = (sym53c8xx_t *)source;
    if (!s->select_timeout_armed)
        return;
    s->select_timeout_armed = false;
    LOG(3, "ch%d: select timed out (target %u)", s->channel, s->reg[SYM825_SDID] & 0x0Fu);
    s->connected = false;
    publish_phase(s);
    s->running = false;
    // TWO causes, and the ORDER is what a driver's recovery hangs off.  The
    // arbitration ends with the bus going free, which the part reports
    // FIRST as an unexpected disconnect; the time-out itself is stacked
    // behind it and surfaces once the first level has been read clear.
    s->sist0 |= SYM825_SIST0_UDC;
    s->sist1_stacked |= SYM825_SIST1_STO;
    sym53c8xx_update_irq(s);
}

// Start the wait.  With no scheduler underneath (the unit suite drives the
// engine directly) there is no time to pass, so the time-out is immediate.
static void sym53c8xx_arm_select_timeout(sym53c8xx_t *s) {
    uint64_t ns = select_timeout_ns(s);
    struct scheduler *sched = s->cfg ? s->cfg->scheduler : NULL;
    if (!sched || ns == 0) {
        select_timeout_event(s, 0);
        return;
    }
    scheduler_new_cpu_event(sched, select_timeout_event, s, 0, 0, ns);
}

// ============================================================
// I/O instructions
// ============================================================

static void exec_io(sym53c8xx_t *s, uint32_t insn, uint32_t dsps) {
    unsigned opc = (insn >> 27) & 7u;
    uint32_t alt = dsps;
    if (insn & (1u << 26)) // relative alternate address
        alt = reg32(s, SYM825_DSP) + (uint32_t)sext24(dsps);

    switch (opc) {
    case 0: { // Select (initiator mode)
        int target = (int)((insn >> 16) & 0x0Fu);
        // Table indirect: the SCNTL3 configuration, the destination ID and
        // the synchronous offset/period all come from a four-byte entry at
        // DSA + a signed offset, ordered `Config | ID | Offset/period | 00`.
        if (insn & (1u << 25)) {
            uint32_t e = fetch32(s, reg32(s, SYM825_DSA) + (uint32_t)sext24(insn));
            s->reg[SYM825_SCNTL3] = (uint8_t)(e >> 24);
            target = (int)((e >> 16) & 0x0Fu);
            s->reg[SYM825_SXFER] = (uint8_t)(e >> 8);
        }
        s->reg[SYM825_SDID] = (uint8_t)target;
        if (s->connected)
            disconnect(s);
        msg_session_reset(s);
        if (!s->bus || !scsi_external_select(s->bus, target)) {
            // Nobody home.  The chip WAITS — STIME0's programmed period, a
            // fifth of a second as this driver sets it — and only then
            // reports, halting where it stands.
            //
            // It does NOT go to the alternate address.  That field is the
            // script's handler for the other way an arbitration can end:
            // another target — possibly the very one being selected —
            // reselecting the chip first, which leaves a command that was
            // never issued and must be re-queued.  A time-out leaves a
            // command that WAS issued to a target that is not there, and a
            // driver tells the two apart by which interrupt arrives.  Send a
            // time-out to that handler and the driver is told a device it
            // has never seen reselected the bus.
            //
            // The wait is what keeps the retry honest.  Reported instantly,
            // the whole select-fail-report-retry cycle completes INSIDE the
            // driver's own doorbell write — its interrupt handler sees the
            // result of a command it has not finished issuing — and the
            // storm of interrupts that follows never lets the clock tick,
            // so the driver's own timers never expire and nothing ever
            // gives up.  A quarter of a million retries per second is not a
            // slow machine; it is a machine that has stopped.
            LOG(3, "ch%d: select %d — arbitrating, no answer yet", s->channel, target);
            s->select_timeout_armed = true;
            s->running = false;
            sym53c8xx_arm_select_timeout(s);
            return;
        }
        s->connected = true;
        s->target = (uint8_t)target;
        // Select WITH ATN/: the target enters MESSAGE OUT to collect the
        // IDENTIFY.  The shared bus model is already in COMMAND, so the
        // chip presents a virtual MESSAGE OUT until the script delivers it.
        s->msgout_pending = (insn & (1u << 24)) ? 1u : 0u;
        publish_phase(s);
        LOG(3, "ch%d: selected target %d%s", s->channel, target, s->msgout_pending ? " with ATN" : "");
        return;
    }
    case 1: // Wait Disconnect
        disconnect(s);
        return;
    case 2: // Wait Reselect
        // The driver's doorbell decides this instruction: "if the SIGP bit
        // in the ISTAT register is set, then the SCRIPTS processor will
        // fetch the next instruction from the alternate address" — and
        // SIGP is cleared by the jump.
        if (s->reg[SYM825_ISTAT] & SYM825_ISTAT_SIGP) {
            s->reg[SYM825_ISTAT] &= (uint8_t)~SYM825_ISTAT_SIGP;
            set_reg32(s, SYM825_DSP, alt);
            return;
        }
        // With SIGP clear the part WAITS, and on this bus it waits
        // forever: there are no disconnecting targets to reselect it.  So
        // the engine parks exactly as the chip does — DSP rewound to point
        // AT the instruction, no interrupt, nothing running — and the next
        // write of SIGP re-executes it.
        //
        // Parking is not a detail.  A driver's idle script is a short ring
        // that ends in Wait Reselect and jumps back to its own start; an
        // engine that takes the alternate address unconditionally runs that
        // ring at the speed of the host until the watchdog stops it, and the
        // driver waiting on the interrupt never gets one.
        LOG(4, "ch%d: Wait Reselect — parked (no SIGP)", s->channel);
        set_reg32(s, SYM825_DSP, reg32(s, SYM825_DSP) - 8u);
        s->waiting_reselect = true;
        s->running = false;
        return;
    case 3: // Set
    case 4: { // Clear
        bool set = (opc == 3);
        if (insn & (1u << 10)) // carry
            s->reg[SYM825_SCNTL1] = (uint8_t)(set ? (s->reg[SYM825_SCNTL1] | 0x04u) : (s->reg[SYM825_SCNTL1] & ~0x04u));
        if (insn & (1u << 9)) // target mode
            s->reg[SYM825_SCNTL0] = (uint8_t)(set ? (s->reg[SYM825_SCNTL0] | SYM825_SCNTL0_TRG)
                                                  : (s->reg[SYM825_SCNTL0] & ~SYM825_SCNTL0_TRG));
        if (insn & (1u << 6)) { // SACK/
            s->reg[SYM825_SOCL] = (uint8_t)(set ? (s->reg[SYM825_SOCL] | 0x40u) : (s->reg[SYM825_SOCL] & ~0x40u));
            // Clearing ACK after the last MESSAGE IN byte is what releases
            // the target: the chip deliberately holds ACK asserted on that
            // final handshake, and the script must clear it explicitly.
            if (!set && s->msgin_taken && !msgin_pending(s))
                disconnect_deferred(s);
        }
        if (insn & (1u << 3)) // SATN/
            s->reg[SYM825_SOCL] = (uint8_t)(set ? (s->reg[SYM825_SOCL] | 0x08u) : (s->reg[SYM825_SOCL] & ~0x08u));
        return;
    }
    default:
        LOG(1, "ch%d: unimplemented I/O opcode %u", s->channel, opc);
        sym53c8xx_raise_dma(s, SYM825_DSTAT_IID);
        return;
    }
}

// ============================================================
// Read/Write — the register ALU
// ============================================================

static void exec_read_write(sym53c8xx_t *s, uint32_t insn) {
    unsigned opc = (insn >> 27) & 7u; // 5 = move from SFBR, 6 = to SFBR, 7 = RMW
    unsigned op = (insn >> 24) & 7u;
    bool use_sfbr = (insn & (1u << 23)) != 0;
    uint32_t ra = (insn >> 16) & 0x7Fu;
    uint8_t imm = (uint8_t)((insn >> 8) & 0xFFu);
    if (ra >= SYM825_REGS) {
        sym53c8xx_raise_dma(s, SYM825_DSTAT_IID);
        return;
    }
    // The two operands: for opcode 101 the accumulator is SFBR, otherwise
    // it is the addressed register; the second operand is the immediate,
    // or SFBR when the D8 bit says so (which is how two registers are
    // combined without a temporary).
    uint8_t acc = (opc == 5) ? s->reg[SYM825_SFBR] : s->reg[ra];
    uint8_t data = use_sfbr ? s->reg[SYM825_SFBR] : imm;
    bool carry_in = (s->reg[SYM825_SCNTL1] & 0x04u) != 0;
    uint8_t result = acc;
    bool carry_out = carry_in;
    switch (op) {
    case 0: // move data
        result = data;
        break;
    case 1: // shift left through carry
        result = (uint8_t)((acc << 1) | (carry_in ? 1u : 0u));
        carry_out = (acc & 0x80u) != 0;
        break;
    case 2:
        result = (uint8_t)(acc | data);
        break;
    case 3:
        result = (uint8_t)(acc ^ data);
        break;
    case 4:
        result = (uint8_t)(acc & data);
        break;
    case 5: // shift right through carry
        result = (uint8_t)((acc >> 1) | (carry_in ? 0x80u : 0u));
        carry_out = (acc & 0x01u) != 0;
        break;
    case 6: { // add without carry
        unsigned sum = (unsigned)acc + data;
        result = (uint8_t)sum;
        carry_out = sum > 0xFFu;
        break;
    }
    case 7: { // add with carry
        unsigned sum = (unsigned)acc + data + (carry_in ? 1u : 0u);
        result = (uint8_t)sum;
        carry_out = sum > 0xFFu;
        break;
    }
    default:
        break;
    }
    s->reg[SYM825_SCNTL1] = (uint8_t)(carry_out ? (s->reg[SYM825_SCNTL1] | 0x04u) : (s->reg[SYM825_SCNTL1] & ~0x04u));
    // Where the result lands: opcode 110 writes SFBR, 101 and 111 write
    // the addressed register.  SFBR is not writable by the host, but it IS
    // writable from here — which is the documented way to load it.
    if (opc == 6)
        s->reg[SYM825_SFBR] = result;
    else
        s->reg[ra] = result;
}

// ============================================================
// Transfer Control
// ============================================================

static void exec_transfer(sym53c8xx_t *s, uint32_t insn, uint32_t dsps) {
    unsigned opc = (insn >> 27) & 7u; // 0 Jump, 1 Call, 2 Return, 3 Interrupt
    uint8_t want = (uint8_t)((insn >> 24) & 7u);
    bool relative = (insn & (1u << 23)) != 0;
    bool carry_test = (insn & (1u << 21)) != 0;
    bool on_the_fly = (insn & (1u << 20)) != 0;
    bool jump_if_true = (insn & (1u << 19)) != 0;
    bool cmp_data = (insn & (1u << 18)) != 0;
    bool cmp_phase = (insn & (1u << 17)) != 0;
    uint8_t mask = (uint8_t)((insn >> 8) & 0xFFu);
    uint8_t value = (uint8_t)(insn & 0xFFu);

    // "If both the Phase Compare and Data Compare bits are set, then both
    // compares must be true to branch on a true condition."  With neither
    // set and no carry test the transfer is unconditional.
    bool cond = true;
    if (carry_test) {
        cond = (s->reg[SYM825_SCNTL1] & 0x04u) != 0;
    } else {
        if (cmp_phase) {
            publish_phase(s);
            cond = cond && (s->phase == want);
        }
        if (cmp_data)
            cond = cond && (((s->reg[SYM825_SFBR] ^ value) & (uint8_t)~mask) == 0);
    }
    bool take = (cmp_phase || cmp_data || carry_test) ? (cond == jump_if_true) : true;

    if (opc == 3) { // Interrupt
        if (!take)
            return;
        set_reg32(s, SYM825_DSPS, dsps);
        if (on_the_fly) {
            // Interrupt-on-the-fly does NOT halt the processor; it sets
            // its own ISTAT bit and execution continues.
            s->reg[SYM825_ISTAT] |= SYM825_ISTAT_INTF;
            sym53c8xx_update_irq(s);
            LOG(3, "ch%d: INTFLY vector $%08X", s->channel, dsps);
            return;
        }
        LOG(3, "ch%d: INT vector $%08X", s->channel, dsps);
        sym53c8xx_raise_dma(s, SYM825_DSTAT_SIR);
        return;
    }
    if (opc == 2) { // Return
        if (take)
            set_reg32(s, SYM825_DSP, reg32(s, SYM825_TEMP));
        return;
    }
    if (!take)
        return;
    uint32_t dest = relative ? reg32(s, SYM825_DSP) + (uint32_t)sext24(dsps) : dsps;
    if (opc == 1) // Call: TEMP is the link register, and it is not a stack
        set_reg32(s, SYM825_TEMP, reg32(s, SYM825_DSP));
    set_reg32(s, SYM825_DSP, dest);
}

// ============================================================
// Memory Move, and Load/Store
// ============================================================

static void exec_memory_move(sym53c8xx_t *s, uint32_t insn, uint32_t src, uint32_t dst) {
    uint32_t count = insn & 0x00FFFFFFu;
    // "Both the source and destination addresses must start with the same
    // address alignment A[1:0].  If the source and destination are not
    // aligned, then an illegal instruction interrupt occurs."
    if ((src & 3u) != (dst & 3u)) {
        LOG(1, "ch%d: Memory Move alignment mismatch ($%08X -> $%08X)", s->channel, src, dst);
        sym53c8xx_raise_dma(s, SYM825_DSTAT_IID);
        return;
    }
    uint8_t buf[512];
    while (count) {
        uint32_t chunk = count > sizeof(buf) ? (uint32_t)sizeof(buf) : count;
        sym53c8xx_read_block(s, src, buf, chunk);
        sym53c8xx_write_block(s, dst, buf, chunk);
        src += chunk;
        dst += chunk;
        count -= chunk;
    }
}

static void exec_load_store(sym53c8xx_t *s, uint32_t insn, uint32_t dsps) {
    bool load = (insn & (1u << 24)) != 0;
    uint32_t ra = (insn >> 16) & 0x7Fu;
    uint32_t n = insn & 7u;
    uint32_t addr = (insn & (1u << 28)) ? reg32(s, SYM825_DSA) + (uint32_t)sext24(dsps) : dsps;
    // "A maximum of 4 bytes may be moved… the register address and memory
    // address must have the same byte alignment, and the count set such
    // that it does not cross Dword boundaries."
    if (n == 0 || n > 4 || ra + n > SYM825_REGS || (ra & 3u) != (addr & 3u)) {
        LOG(1, "ch%d: illegal Load/Store (reg $%02X, addr $%08X, %u bytes)", s->channel, ra, addr, n);
        sym53c8xx_raise_dma(s, SYM825_DSTAT_IID);
        return;
    }
    if (load)
        sym53c8xx_read_block(s, addr, &s->reg[ra], n);
    else
        sym53c8xx_write_block(s, addr, &s->reg[ra], n);
}

// ============================================================
// The instruction engine
// ============================================================

// Fetch and execute one instruction.  Returns false when the engine
// stopped (an interrupt, a mismatch, an illegal instruction).
static bool step(sym53c8xx_t *s) {
    uint32_t pc = reg32(s, SYM825_DSP);
    uint32_t insn = fetch32(s, pc);
    uint32_t dsps = fetch32(s, pc + 4);
    unsigned type3 = (insn >> 29) & 7u;
    unsigned type2 = (insn >> 30) & 3u;

    // DCMD and DBC hold the instruction's own first dword while it runs —
    // a driver reading them after a halt sees what stopped it.
    set_reg32(s, SYM825_DBC, insn);
    set_reg32(s, SYM825_DSPS, dsps);
    // Memory Move is the one three-dword instruction.
    uint32_t third = (type3 == 6) ? fetch32(s, pc + 8) : 0;
    set_reg32(s, SYM825_DSP, pc + ((type3 == 6) ? 12u : 8u));
    s->insn_count++;
    LOG(5, "ch%d: $%08X: %08X %08X", s->channel, pc, insn, dsps);

    switch (type2) {
    case 0:
        exec_block_move(s, insn, dsps);
        break;
    case 1:
        if (((insn >> 27) & 7u) <= 4u)
            exec_io(s, insn, dsps);
        else
            exec_read_write(s, insn);
        break;
    case 2:
        exec_transfer(s, insn, dsps);
        break;
    default:
        if (type3 == 6) {
            // TEMP is NOT touched here.  It is the Call/Return link
            // register, and the Network Server's own AIX driver calls a
            // subroutine that patches its own instruction stream with a
            // four-byte Memory Move and then returns: writing the
            // destination address into TEMP sends that Return into the
            // middle of the instruction the move had just patched.  The
            // three-Dword form carries both addresses in the instruction,
            // so it has no need of the register.
            exec_memory_move(s, insn, dsps, third);
        } else {
            exec_load_store(s, insn, dsps);
        }
        break;
    }

    // Single-step mode: one instruction per START, with its own cause.
    if (s->running && (s->reg[SYM825_DCNTL] & SYM825_DCNTL_SSM)) {
        s->running = false;
        sym53c8xx_raise_dma(s, SYM825_DSTAT_SSI);
        return false;
    }
    return s->running;
}

// Run the engine until the script stops it.  Called from the scheduler a
// short time after the driver asks for it — never inside the store that
// asked (see sym53c8xx_start).
static void sym53c8xx_run(sym53c8xx_t *s) {
    if (!s)
        return;
    s->running = true;
    s->waiting_reselect = false;
    uint32_t budget = SYM825_INSN_BUDGET;
    while (s->running && budget--) {
        if (!step(s))
            break;
    }
    // The target's disconnect, owed since the script cleared ACK on the last
    // MESSAGE IN byte and reported now that DCMD holds the opcode the
    // script stopped on (see disconnect_deferred).
    if (s->disconnect_pending) {
        s->disconnect_pending = 0;
        sym53c8xx_raise_scsi(s, SYM825_SIST0_UDC, 0);
    }
    if (s->running) {
        // A program that never stopped.  The chip's own watchdog cause is
        // the honest report, and it halts the engine — far better than
        // taking the emulator down with the guest.
        LOG(0, "ch%d: SCRIPTS ran %u instructions without halting; stopping (DSP $%08X)", s->channel,
            SYM825_INSN_BUDGET, reg32(s, SYM825_DSP));
        s->running = false;
        sym53c8xx_raise_dma(s, SYM825_DSTAT_WTD);
    }
}

// The engine start event: the latency between the driver asking and the
// chip having done it.
static void script_start_event(void *source, uint64_t data) {
    (void)data;
    sym53c8xx_t *s = (sym53c8xx_t *)source;
    s->start_pending = false;
    sym53c8xx_run(s);
}

// A driver asks the engine to run by writing DSP's high byte, by strobing
// DCNTL's START bit, or by ringing SIGP at a parked script.  The chip then
// arbitrates, selects, moves the command out, moves the data, takes status
// and interrupts — all of which takes time, and NONE of which happens
// inside the store that asked for it.
//
// Modelling that as instantaneous is not a harmless simplification, and
// AIX is where it stops being one.  Its driver queues a command under a
// lock and writes DSP; run the whole transaction inside that store and the
// completion interrupt arrives while the lock is still held, so the
// interrupt handler tries to take a lock its own caller owns.  The kernel
// checks for exactly that — `twi 4,r4,0` against the held bit — and the
// machine panics with LED 888 / 102 / 700, a program interrupt.  Open
// Firmware never noticed because it polls with interrupts masked and holds
// no locks.
//
// So the start is a scheduler event a few microseconds out: long enough
// that the caller has left its critical section, short enough to be
// invisible to a polled driver, which simply spins one more time.
void sym53c8xx_start(sym53c8xx_t *s) {
    if (!s)
        return;
    // Clear the START bit: it is a strobe, not a mode.
    s->reg[SYM825_DCNTL] &= (uint8_t)~SYM825_DCNTL_STD;
    struct scheduler *sched = s->cfg ? s->cfg->scheduler : NULL;
    // A chip that is arbitrating is BUSY, and it stays busy for the whole
    // of STIME0's period whatever the driver does.  A start arriving in
    // that window is not a second command running alongside the first —
    // it is ignored, and the driver hears about the selection when the
    // time-out lands.  SIGP is a level and stays set, so a script that
    // parks on Wait Reselect afterwards still sees the doorbell.
    //
    // Running it would be worse than useless: the command in flight and
    // the command just started would share one chip, and the time-out
    // would eventually be reported against whichever NEXUS the driver had
    // most recently written down.
    if (s->select_timeout_armed)
        return;
    if (!sched) {
        sym53c8xx_run(s); // no time to pass (the unit suite drives it directly)
        return;
    }
    if (s->start_pending)
        return;
    s->start_pending = true;
    scheduler_new_cpu_event(sched, script_start_event, s, 0, 0, SYM825_START_LATENCY_NS);
}

// ============================================================
// Lifecycle
// ============================================================

// ISTAT's ABRT bit: the driver has abandoned whatever the chip is doing.
// Any selection still arbitrating is dropped, the engine stops wherever it
// is, and the cause is reported so the driver's recovery has something to
// act on.  The bus is left alone — an abort is not a reset.
void sym53c8xx_abort(sym53c8xx_t *s) {
    if (!s)
        return;
    s->select_timeout_armed = false;
    s->start_pending = false;
    s->waiting_reselect = false;
    s->running = false;
    s->sist0_stacked = 0;
    s->sist1_stacked = 0;
    if (s->cfg && s->cfg->scheduler) {
        remove_event(s->cfg->scheduler, select_timeout_event, s);
        remove_event(s->cfg->scheduler, script_start_event, s);
    }
    sym53c8xx_raise_dma(s, SYM825_DSTAT_ABRT);
}

// The driver drove RST/.  Everything in flight on this channel is over:
// the connection, the message conversation, the negotiated transfer
// agreement (a reset target comes back asynchronous and narrow), and any
// selection the chip was still arbitrating for.  The devices on the bus go
// back to their power-on state, and the chip reports what it saw.
void sym53c8xx_bus_reset(sym53c8xx_t *s) {
    if (!s)
        return;
    if (s->bus) {
        if (s->connected)
            scsi_external_release(s->bus);
        scsi_reset_pin(s->bus);
    }
    s->connected = false;
    s->disconnect_pending = 0;
    s->sync_period = 0;
    s->sync_offset = 0;
    s->wide = 0;
    msg_session_reset(s);
    s->select_timeout_armed = false;
    s->start_pending = false;
    s->running = false;
    s->waiting_reselect = false;
    s->sist0_stacked = 0;
    s->sist1_stacked = 0;
    if (s->cfg && s->cfg->scheduler) {
        remove_event(s->cfg->scheduler, select_timeout_event, s);
        remove_event(s->cfg->scheduler, script_start_event, s);
    }
    sym53c8xx_raise_scsi(s, SYM825_SIST0_RST, 0);
}

void sym53c8xx_chip_reset(sym53c8xx_t *s) {
    if (!s)
        return;
    // Power-on / SRST.  The SCRIPTS RAM is host memory and survives, as it
    // does on the part; everything else returns to its reset value.
    memset(s->reg, 0, sizeof(s->reg));
    s->dstat = 0;
    s->sist0 = 0;
    s->sist1 = 0;
    s->sist0_stacked = 0;
    s->sist1_stacked = 0;
    s->running = false;
    s->waiting_reselect = false;
    s->connected = false;
    s->target = 0;
    s->phase = SYM825_PHASE_MSG_OUT;
    s->insn_count = 0;
    // A reset abandons an arbitration in progress; the time-out it was
    // waiting for must not land on the reset chip.
    s->select_timeout_armed = false;
    s->start_pending = false;
    if (s->cfg && s->cfg->scheduler) {
        remove_event(s->cfg->scheduler, select_timeout_event, s);
        remove_event(s->cfg->scheduler, script_start_event, s);
    }
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
    // The Apple Network Server does NOT assert BIG_LIT/ — see the endianness
    // note in sym53c825.c, which the ROM settles three separate ways.
    s->big_endian = false;
    // GPIO0 pulled high: the Network Server's presence strap for a fitted
    // fast/wide channel (see sym53c8xx.h).  Discovered by decompiling the
    // ROM's own `check-disabled` at the Open Firmware prompt, after the
    // node came up `status "disabled"` with the chip otherwise perfect.
    s->gpio_strap = 0x01u;
    // The selection time-out is a scheduled event, so the checkpoint has to
    // know its name (one per channel, since both chips post their own).
    if (cfg && cfg->scheduler) {
        const char *who = channel ? "53c825-1" : "53c825-0";
        scheduler_new_event_type(cfg->scheduler, who, s, "select-timeout", select_timeout_event);
        scheduler_new_event_type(cfg->scheduler, who, s, "script-start", script_start_event);
    }
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
