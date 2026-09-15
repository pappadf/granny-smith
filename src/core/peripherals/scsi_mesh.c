// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mesh.c
// MESH (343S1146) — Apple's fast internal SCSI cell, new with TNT.
// Sixteen byte-wide registers on $10 centres at island +$18000
// (mesh-scsi.md §2), a 16-byte FIFO for the non-data phases, DBDMA
// channel 10 for the data phases, Grand Central interrupt 13.
//
// The register core is a behavioral model over the shared bus/target
// machinery (scsi.c) through the same scsi_external_* API the 53C96
// front-end uses: SELECT lands the bus in COMMAND phase, CDB bytes are
// pushed until the target dispatches, data phases pop/push, and the
// status/message tail is stepped explicitly.  Everything the drivers
// key on is the PRESENTATION: bus_status0 carries the live phase with
// REQ asserted whenever a target is connected (the Linux/MkLinux
// driver class spin-waits on REQ before dropping ATN), and each
// completed sequence command latches INT_CMDDONE.
//
// Contract points pinned from the driver corpus (linux mesh.c,
// mesh-scsi.md §5-§8):
//   * Quick commands (ENBRESEL, DISRESEL, FLUSHFIFO, ENBPARITY,
//     DISPARITY, and a zero sequence write) complete SILENTLY — the
//     driver reads `interrupt` right after DISRESEL and treats a
//     nonzero value as an unexpected event, and DR3 issues FLUSHFIFO
//     *inside* live sequences (before SELECT, before COMMAND) where a
//     CMDDONE would post an interrupt with no command outstanding.
//     ARBITRATE, SELECT, the transfer commands and BUSFREE raise
//     INT_CMDDONE.
//   * RESETMESH raises INT_CMDDONE too — it is NOT silent, despite
//     being a quick command.  DR3's mesh_setup_chip() spins on the
//     latch forever waiting for it; see the case body for the evidence.
//   * Selection with ATN presents a virtual MSG OUT phase until the
//     driver's SEQ_MSGOUT consumes the IDENTIFY byte(s) — the shared
//     bus model has no message-out phase, so the bytes are absorbed
//     here (the 53C96 front-end discards them the same way).
//   * The count registers are a LIVE down-counter: drivers read the
//     residue back after halting a short DMA transfer.
//   * Cause bits (exception/error) latch BEFORE the interrupt summary
//     bit, and all three registers are W1C (§8).

#include "scsi_mesh.h"

#include "log.h"
#include "scheduler.h"
#include "scsi.h"
#include "system.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("mesh");

// Register indices (offset / $10) — mesh-scsi.md §2.
#define MR_COUNT_LO    0x0
#define MR_COUNT_HI    0x1
#define MR_FIFO        0x2
#define MR_SEQUENCE    0x3
#define MR_BUS_STATUS0 0x4
#define MR_BUS_STATUS1 0x5
#define MR_FIFO_COUNT  0x6
#define MR_EXCEPTION   0x7
#define MR_ERROR       0x8
#define MR_INTR_MASK   0x9
#define MR_INTERRUPT   0xA
#define MR_SOURCE_ID   0xB
#define MR_DEST_ID     0xC
#define MR_SYNC_PARAMS 0xD
#define MR_MESH_ID     0xE
#define MR_SEL_TIMEOUT 0xF

// sequence (§3.1): modifiers high nibble, command low nibble.
#define SEQ_DMA_MODE  0x80u
#define SEQ_ATN       0x20u
#define CMD_ARBITRATE 0x1
#define CMD_SELECT    0x2
#define CMD_COMMAND   0x3
#define CMD_STATUS    0x4
#define CMD_DATAOUT   0x5
#define CMD_DATAIN    0x6
#define CMD_MSGOUT    0x7
#define CMD_MSGIN     0x8
#define CMD_BUSFREE   0x9
#define CMD_ENBPARITY 0xA
#define CMD_DISPARITY 0xB
#define CMD_ENBRESEL  0xC
#define CMD_DISRESEL  0xD
#define CMD_RESETMESH 0xE
#define CMD_FLUSHFIFO 0xF

// bus_status0 (§3.2)
#define BS0_REQ 0x20u
#define BS0_ACK 0x10u
#define BS0_ATN 0x08u
// bus_status1
#define BS1_RST 0x80u
#define BS1_BSY 0x40u

// exception (§3.3) / error (§3.4) / interrupt (§3.5)
#define EXC_PHASEMM   0x02u
#define EXC_SELTO     0x01u
#define INT_ERROR     0x04u
#define INT_EXCEPTION 0x02u
#define INT_CMDDONE   0x01u

// SDTR responses (Phase E part 3).  Message-out bytes absorbed here
// are assembled and parsed; an initiator-offered SDTR (the SIM
// framework's needNegot path, or the loaded driver's inline probe that
// renegotiates with 01 03 01 00 00) gets an SDTR response presented
// through a virtual MESSAGE IN phase, with EXC_PHASEMM latched on the
// completed message-out — the phase change to MESSAGE IN is
// target-driven, and the loaded driver's post-msgout check keys on
// exception&6 to fix up the sent count and hand the message phase to
// the SIM framework.  A plain identify raises nothing: the underlying
// bus phase was COMMAND throughout, the MSG OUT the driver saw was our
// virtual overlay.  The target never INITIATES negotiation: neither
// shipping driver can accept an unsolicited extended message (the
// unsolicited-message hooks — ROM $FFEB89BC, driver $1A335C — and the
// driver's message-in state $1A2EF4 all BUSFREE on one).  Negotiation
// is NOT what arms the data phases either — the T12 wall fell to the
// SEQ_BUSFREE expect-free law (see CMD_BUSFREE) — the full forensic
// story lives in the handover §8.
// The response values mirror the framework's own offer (block+48/49):
// offset 15, period factor 25 (100 ns, fast SCSI).
#define MESH_SDTR_PERIOD 25u
#define MESH_SDTR_OFFSET 15u

// mesh_id: TWO shipping drivers gate on it (both found live).  The
// ROM's native driver (ROM $FFEBB168): < $E1 selects a 53C94-protocol
// fallback path, == $E1 sets two quirk flags, > $E1 the normal MESH
// path.  The DISK's Apple_Driver43 additionally decodes the LOW FIVE
// BITS as the cell revision: (mesh_id & $1F) <= 2 marks the chip
// quirky and installs stub data-phase handlers — its transfer-arm
// step never runs and every data phase disconnects (the 7.6 boot's
// dsBadPatch bomb).  So the shipping 7500's cell reports at least
// revision 3: we serve $E3.  NetBSD's MESH_SIGNATURE $E2 ("XXX
// wrong!") is real-hardware-plausible for an earlier cell revision.
#define MESH_ID_VALUE 0xE3u

// The GC line follows the masked interrupt summary (level semantics —
// grand_central.c's change law turns edges AND clears into latches).
static void mesh_update_irq(mesh_t *m) {
    if (m->irq_cb)
        m->irq_cb(m->irq_ctx, (m->interrupt & m->intr_mask) != 0);
}

static void raise_int(mesh_t *m, uint8_t bits) {
    m->interrupt |= bits;
    mesh_update_irq(m);
}

// Cause first, summary second (§8): the latched exception/error bit
// must be readable before (and independent of) the interrupt bit.
static void raise_exception(mesh_t *m, uint8_t cause) {
    m->exception |= cause;
    raise_int(m, INT_EXCEPTION);
}

// How long the driver asked us to wait for a target to answer.
//
// MR_SEL_TIMEOUT is programmed in units of 10 ms.  There is no MESH manual in
// gs-docs to cite for that, but two things agree on it: Mac OS programs 25,
// and 25 x 10 ms is 250 ms, which is the selection time-out ANSI X3.131-1986
// specifies and what Linux's drivers/scsi/mesh.c writes for the same reason.
//
// A zero period means the driver disabled the wait; scsi_bus_arm_select_timeout
// reports immediately in that case, which is what disabling it means.
static uint64_t mesh_select_timeout_ns(const mesh_t *m) {
    return (uint64_t)m->sel_timeout * 10ull * 1000000ull;
}

// The period elapsed and nobody answered.
//
// This used to run synchronously inside the sequence-register write that
// issued SELECT.  A Mac OS bus scan selects every target twice -- measured at
// twelve time-outs per boot on tnt-hd-boot, all of them for targets that are
// simply not fitted -- so this is the ordinary path, not the error path, and
// completing the whole select-fail-report-retry cycle inside the driver's own
// doorbell write is what docs/machines/tnt/tnt.md warns about: "the interrupt
// storm that follows never lets the clock tick, so the driver's timers never
// expire and nothing gives up".
static void mesh_select_timed_out(void *ctx) {
    mesh_t *m = (mesh_t *)ctx;
    if (!m)
        return;
    LOG(2, "select timed out after %u ms", (unsigned)m->sel_timeout * 10u);
    m->connected = 0;
    raise_exception(m, EXC_SELTO);
}

// The ring is byte_fifo.h's; only the ends are MESH's.  Unlike the 53C96 --
// whose overflow and empty-read are both spelled out in the NCR manual -- what
// real MESH silicon does at either end is documented NOWHERE.  There is no
// Apple datasheet, and none of the three driver corpora (Linux, NetBSD,
// MkLinux) exercises it, because a driver that respects `fifo_count` never
// reaches either end.  Both choices below are ours, and defensive: measured
// across a 7.6 boot, 64,053 pushes and 64,053 pops hit neither.
static void fifo_clear(mesh_t *m) {
    byte_fifo_clear(&m->fifo);
}

static void fifo_push(mesh_t *m, uint8_t v) {
    if (!byte_fifo_push(&m->fifo, v))
        LOG(1, "FIFO overflow (byte $%02X dropped)", v);
}

static uint8_t fifo_pop(mesh_t *m) {
    uint8_t v;
    if (!byte_fifo_pop(&m->fifo, &v))
        return 0; // our choice: an empty FIFO reads as zero
    return v;
}

// End the active transfer command, completing with INT_CMDDONE.
static void finish_command(mesh_t *m) {
    m->active = 0;
    m->active_dma = 0;
    raise_int(m, INT_CMDDONE);
}

// ============================================================
// SDTR message engine (see the "Sync negotiation" header block)
// ============================================================

// MESH can express a four-bit synchronous offset (sync_params 0xD0:
// `offset = value >> 4`) and its transfer period is (x + 2) * 40 ns with
// x == 0 meaning 100 ns, so 100 ns -- SDTR period 25 -- is the fastest it
// runs.  Narrow part: it answers WDTR with 8-bit rather than agreeing.
// [Linux/NetBSD/MkLinux mesh.h; there is no Apple datasheet]
static const scsi_msg_caps_t MESH_MSG_CAPS = {.min_period = 25, .max_offset = 15, .wide = false};

// A virtual MESSAGE IN is pending while queued bytes remain unread.
static bool msgin_pending(mesh_t *m) {
    return scsi_msg_pending(&m->msg);
}

// Per-session message state: assembled message-out, virtual message-in, and
// the delivered flag all die with the connection.
static void msg_session_reset(mesh_t *m) {
    scsi_msg_reset(&m->msg);
    m->msgin_taken = 0;
}

// A message-out sequence completed: parse what the initiator said.
// Called after finish_command so the CMDDONE presentation is normal;
// queueing a message-in flips the visible phase for the NEXT command.
static void msgout_complete(mesh_t *m) {
    scsi_msg_result_t r;
    scsi_msg_complete(&m->msg, &MESH_MSG_CAPS, &r);
    if (r.incomplete)
        return; // more bytes still to come; nothing consumed
    int target = m->dest_id & 7;
    if (r.sdtr)
        LOG(2, "SDTR offer answered (period=%u offset=%u): target %d", r.period, r.offset, target);
    if (r.wdtr)
        LOG(2, "WDTR offer answered %s: target %d", r.wide ? "16-bit" : "8-bit", target);
    if (r.rejected)
        LOG(2, "MESSAGE REJECT from initiator: target %d", target);
}

// ============================================================
// Transfer pumps (synchronous against the shared bus model)
// ============================================================

// Feed FIFO bytes to the bus for the out-going non-DMA commands
// (COMMAND / MSGOUT / DATAOUT).  MSGOUT bytes are the IDENTIFY family —
// absorbed here, exactly as the 53C96 front-end discards them.
static void pump_out(mesh_t *m) {
    if (!m->connected)
        return; // no target: bytes stay in the FIFO
    while (m->remaining > 0 && byte_fifo_count(&m->fifo) > 0) {
        uint8_t b = fifo_pop(m);
        if (m->active == CMD_MSGOUT) {
            LOG(3, "msgout byte $%02X absorbed", b);
            if (!scsi_msg_collect(&m->msg, b))
                LOG(1, "message-out overflow (byte $%02X dropped)", b);
        } else if (m->bus) {
            scsi_push_data_out_byte(m->bus, b);
        }
        m->remaining--;
    }
    if (m->remaining == 0 && m->active != 0) {
        uint8_t done = m->active;
        if (done == CMD_MSGOUT)
            m->msgout_pending = 0; // message sent: present the real phase
        finish_command(m);
        if (done == CMD_MSGOUT) {
            msgout_complete(m);
            // If the target now has a message to deliver, its phase
            // change to MESSAGE IN lands while the sequencer still
            // holds MSG OUT: the phase-mismatch exception latches on
            // the completed command.  (A plain identify raises nothing:
            // the underlying bus phase was COMMAND throughout — the
            // MSG OUT the driver saw was our virtual overlay, so no
            // target-driven change occurred.)
            if (msgin_pending(m))
                raise_exception(m, EXC_PHASEMM);
        }
        // NOTE (T12 forensics): raising EXC_PHASEMM on a COMMAND
        // completion whose target has already changed phase was tried
        // and REVERTED: it routes Apple_Driver43 into its interrupt&6
        // exception states, which drain the data phase byte-wise into
        // a scratch buffer (proven: the wire bytes never reach the
        // client's scsiDataPtr) — and it breaks the ROM engine's
        // media scan outright.  See the handover §8 rewrite.
    }
}

// Fill the FIFO from the bus for non-DMA DATAIN.
static void pump_in(mesh_t *m) {
    while (m->remaining > 0 && !byte_fifo_full(&m->fifo)) {
        uint8_t b;
        if (!m->bus || !scsi_pop_data_in_byte(m->bus, &b)) {
            // Target has no more data: it leaves DATA IN — a short
            // transfer is a phase mismatch to the initiator.
            if (m->bus)
                scsi_external_data_in_complete(m->bus);
            m->active = 0;
            m->active_dma = 0;
            raise_exception(m, EXC_PHASEMM);
            return;
        }
        fifo_push(m, b);
        m->remaining--;
    }
    if (m->remaining == 0 && m->active != 0) {
        if (m->bus)
            scsi_external_data_in_complete(m->bus);
        finish_command(m);
    }
}

// ============================================================
// DBDMA channel-10 device port (the data phases)
// ============================================================

int mesh_port_in(void *ctx, uint8_t *buf, int len) {
    mesh_t *m = (mesh_t *)ctx;
    if (m->active != CMD_DATAIN || !m->active_dma || !m->bus)
        return 0; // no transfer armed: honest stall
    int n = 0;
    while (n < len && m->remaining > 0) {
        uint8_t b;
        if (!scsi_pop_data_in_byte(m->bus, &b)) {
            scsi_external_data_in_complete(m->bus);
            m->active = 0;
            m->active_dma = 0;
            raise_exception(m, EXC_PHASEMM); // short transfer
            break;
        }
        buf[n++] = b;
        m->remaining--;
    }
    if (m->remaining == 0 && m->active != 0) {
        scsi_external_data_in_complete(m->bus);
        finish_command(m);
    }
    return n;
}

int mesh_port_out(void *ctx, const uint8_t *buf, int len) {
    mesh_t *m = (mesh_t *)ctx;
    if (m->active != CMD_DATAOUT || !m->active_dma || !m->bus)
        return 0;
    int n = 0;
    while (n < len && m->remaining > 0 && scsi_get_bus_phase(m->bus) == scsi_data_out) {
        scsi_push_data_out_byte(m->bus, buf[n++]);
        m->remaining--;
    }
    if (m->remaining > 0 && n < len && scsi_get_bus_phase(m->bus) != scsi_data_out) {
        m->active = 0;
        m->active_dma = 0;
        raise_exception(m, EXC_PHASEMM); // target left DATA OUT early
    } else if (m->remaining == 0 && m->active != 0) {
        finish_command(m);
    }
    return n;
}

// ============================================================
// The sequence command dispatch
// ============================================================

static void do_sequence(mesh_t *m, uint8_t value, uint32_t count) {
    uint8_t cmd = value & 0x0Fu;
    bool dma = (value & SEQ_DMA_MODE) != 0;
    m->sequence = value;
    LOG(3, "sequence $%02X (count=%u fifo=%u conn=%d)", value, count, byte_fifo_count(&m->fifo), m->connected);

    // Starting a sequence command clears the PREVIOUS command's cause
    // latches: the ROM's native driver reads exception unconditionally
    // in its completion path and never W1Cs it — after a selection
    // timeout it moves straight to the next target, and a stale SELTO
    // surviving into that target's successful select reads as failure.
    // (Refines mesh-scsi.md §8's "hold until W1C" — that holds only
    // within one command's lifetime.)
    m->exception = 0;
    m->error = 0;

    switch (cmd) {
    case 0: // "no command" — the driver parks the sequencer; silent
        m->active = 0;
        m->active_dma = 0;
        return;

    case CMD_ARBITRATE:
        // Arbitration implies a free bus: a stale connection from an
        // abandoned transaction (see CMD_BUSFREE) is gone by the time
        // the initiator re-arbitrates.
        if (m->connected) {
            if (m->bus)
                scsi_external_release(m->bus);
            m->connected = 0;
            msg_session_reset(m);
        }
        // The shared bus model has no competing initiators: arbitration
        // is always won, immediately.
        m->active = 0;
        raise_int(m, INT_CMDDONE);
        return;

    case CMD_SELECT: {
        int target = m->dest_id & 7u;
        if (!m->bus || !scsi_external_select(m->bus, target)) {
            // Nobody home.  Report it after the period the driver programmed,
            // NOT inside this register write -- see mesh_select_timed_out.
            LOG(2, "select %d: nobody answered, waiting out the period", target);
            m->connected = 0;
            scsi_bus_arm_select_timeout(m->bus, mesh_select_timeout_ns(m), mesh_select_timed_out, m);
            return;
        }
        // A target answered, so any wait from a previous attempt is moot.
        scsi_bus_cancel_select_timeout(m->bus);
        LOG(2, "select %d: connected", target);
        m->connected = 1;
        m->active = 0; // a parked transfer command is superseded
        m->active_dma = 0;
        msg_session_reset(m); // a fresh connection, a fresh conversation
        // With ATN the target enters MSG OUT for the IDENTIFY; the bus
        // model is already in COMMAND, so present a virtual MSG OUT
        // phase until the driver's SEQ_MSGOUT delivers the message.
        m->msgout_pending = (value & SEQ_ATN) ? 1 : 0;
        raise_int(m, INT_CMDDONE);
        return;
    }

    case CMD_COMMAND:
    case CMD_MSGOUT:
    case CMD_DATAOUT:
        if (cmd != CMD_MSGOUT && m->connected && msgin_pending(m)) {
            // The target is presenting MESSAGE IN (an SDTR to deliver):
            // a mismatched transfer command trips the phase-mismatch
            // exception — the driver's message-in state picks it up.
            // MSGOUT stays legal: asserting ATN overrides the target.
            raise_exception(m, EXC_PHASEMM);
            return;
        }
        m->active = cmd;
        m->active_dma = dma && cmd == CMD_DATAOUT;
        m->remaining = count;
        if (!m->connected)
            return; // no target: the chip waits for REQ that never comes
        if (m->active_dma) {
            // Data flows through the channel-10 port; wake a program
            // that stalled waiting for the device to arm.
            if (m->dbdma_kick)
                m->dbdma_kick(m->dbdma_ctx);
        } else {
            pump_out(m); // consume whatever is already in the FIFO
        }
        return;

    case CMD_DATAIN:
        if (m->connected && msgin_pending(m)) {
            raise_exception(m, EXC_PHASEMM); // MESSAGE IN pending (see above)
            return;
        }
        m->active = cmd;
        m->active_dma = dma;
        m->remaining = count;
        if (!m->connected)
            return; // parked (see above)
        if (dma) {
            // Data flows through the channel-10 port; the machine owns the
            // engine, so ask it to run.
            if (m->dbdma_kick)
                m->dbdma_kick(m->dbdma_ctx);
        } else {
            pump_in(m);
        }
        LOG(3, "datain first bytes: %02X %02X %02X %02X (fifo_n=%u remaining=%u)", byte_fifo_peek(&m->fifo, 0),
            byte_fifo_peek(&m->fifo, 1), byte_fifo_peek(&m->fifo, 2), byte_fifo_peek(&m->fifo, 3),
            byte_fifo_count(&m->fifo), m->remaining);
        return;

    case CMD_STATUS: {
        if (!m->connected) {
            m->active = cmd; // parked: no REQ without a target
            return;
        }
        if (msgin_pending(m)) {
            raise_exception(m, EXC_PHASEMM); // MESSAGE IN pending (see above)
            return;
        }
        int st = m->bus ? scsi_external_status_byte(m->bus) : -1;
        if (st < 0) {
            LOG(2, "STATUS sequence outside status phase");
            raise_exception(m, EXC_PHASEMM);
            return;
        }
        fifo_push(m, (uint8_t)st);
        m->active = 0;
        raise_int(m, INT_CMDDONE);
        return;
    }

    case CMD_MSGIN: {
        if (!m->connected) {
            m->active = cmd; // parked
            return;
        }
        if (msgin_pending(m)) {
            // Serve the virtual message (the SDTR conversation) first;
            // the phase reverts to the live bus once it drains.
            uint32_t n = count;
            uint8_t mb;
            while (n-- > 0 && scsi_msg_next(&m->msg, &mb))
                fifo_push(m, mb);
            if (!msgin_pending(m)) {
                scsi_msg_reset(&m->msg);
            }
            m->active = 0;
            raise_int(m, INT_CMDDONE);
            return;
        }
        int msg = m->bus ? scsi_external_message_byte(m->bus) : -1;
        if (msg < 0) {
            LOG(2, "MSGIN sequence outside message phase");
            raise_exception(m, EXC_PHASEMM);
            return;
        }
        fifo_push(m, (uint8_t)msg);
        m->msgin_taken = 1; // delivered: the target REQs nothing more
        m->active = 0;
        raise_int(m, INT_CMDDONE);
        return;
    }

    case CMD_BUSFREE: {
        // SEQ_BUSFREE means "expect the bus to go free" — it is NOT a
        // forced release.  Issued while the target still drives a
        // phase (REQ pending — e.g. right after a no-data command's
        // CDB, with the target presenting STATUS), the sequencer sees
        // REQ where it expects bus-free and raises the phase-mismatch
        // exception, keeping the connection.  The ROM SIM DEPENDS on
        // this distinction: its no-data request path issues BUSFREE
        // straight after the CDB, and its completion treats a PURE
        // CMDDONE here as "the bus really went free" — phase 8 —
        // which it then fails as scsiUnexpectedBusFree (-7917, stamp
        // at ROM $FFEB6A14).  Every TEST UNIT READY through the
        // request path died on that; and Apple_Driver43's data phases
        // hang off the same law — its unarmed-sync path ([114]==4)
        // issues BUSFREE as an expect-free PROBE, and the mismatch
        // exception is what tells it the target still has data, so it
        // continues the transaction.  This one semantic was the whole
        // T12/T13 "data-phase arming wall": with it pinned, the 7.6
        // disk boots to the Finder.  A stale connection left behind
        // by an abandoned transaction is swept by the next ARBITRATE.
        bool req_pending = false;
        if (m->connected && m->bus) {
            switch (scsi_get_bus_phase(m->bus)) {
            case scsi_command:
            case scsi_data_in:
            case scsi_data_out:
            case scsi_status:
                req_pending = true;
                break;
            case scsi_message_in:
                // The bus model holds MESSAGE IN until released, but
                // once the single message byte has been DELIVERED the
                // target REQs nothing more — this busfree IS the
                // release step of a normal tail.
                req_pending = !m->msgin_taken;
                break;
            default:
                break;
            }
        }
        if (req_pending || (m->connected && (m->msgout_pending || msgin_pending(m)))) {
            raise_exception(m, EXC_PHASEMM);
            return;
        }
        if (m->bus)
            scsi_external_release(m->bus);
        m->connected = 0;
        m->msgout_pending = 0;
        m->active = 0;
        msg_session_reset(m);
        raise_int(m, INT_CMDDONE);
        return;
    }

    // Quick commands: no completion interrupt (see header comment).
    // RESETMESH is the documented exception and is handled below.
    case CMD_ENBRESEL:
        m->resel_enabled = 1;
        return;
    case CMD_DISRESEL:
        m->resel_enabled = 0;
        return;
    case CMD_ENBPARITY:
        m->parity_enabled = 1;
        return;
    case CMD_DISPARITY:
        m->parity_enabled = 0;
        return;
    case CMD_FLUSHFIFO:
        fifo_clear(m);
        return;
    case CMD_RESETMESH:
        // Reset the cell: FIFO, latches, sequencer.  The bus connection
        // is dropped too (drivers reset only from error/recovery paths
        // or with the bus free).
        //
        // ...and then COMPLETE, like any other sequence command.  This
        // one is the exception to the quick-command rule above, and it
        // is not a guess: MkLinux DR3's mesh_setup_chip() spins on the
        // latch with no ceiling and no escape --
        //
        //     regs->r_cmd = MESH_CMD_RESET_MESH; eieio();
        //     delay(1);
        //     do { eieio(); } while (regs->r_interrupt == 0);
        //     regs->r_interrupt = 0xff; eieio();
        //
        // -- so a silent reset hangs the machine outright.  Linux's
        // mesh_init() agrees from the other side: it does not wait, but
        // it does write `interrupt <- 0xFF` immediately afterwards,
        // which is only meaningful if the reset left a bit set.  And
        // mesh-scsi.md §8 states the rule without a carve-out:
        // "Sequence command completes -> INT_CMDDONE".
        //
        // The carve-out the other quick commands need is real and stays:
        // DR3 issues FLUSHFIFO *inside* live sequences (immediately
        // before SELECT and before COMMAND), and ENBRESEL while waiting
        // for a reselection, so those raising CMDDONE would post an
        // interrupt with no command outstanding.  RESETMESH is only ever
        // issued from init and recovery paths, with the bus quiet.
        //
        // Note the latch is set with `intr_mask` already zeroed by the
        // driver ("No interrupts") -- raise_int() latches unconditionally
        // and only the GC line is gated by the mask, which is exactly the
        // behaviour the polling loop above depends on.
        if (m->connected && m->bus)
            scsi_external_release(m->bus);
        fifo_clear(m);
        m->exception = 0;
        m->error = 0;
        m->interrupt = 0;
        m->active = 0;
        m->active_dma = 0;
        m->remaining = 0;
        m->connected = 0;
        m->msgout_pending = 0;
        m->bus0_atn = 0;
        msg_session_reset(m);
        raise_int(m, INT_CMDDONE);
        return;

    default:
        LOG(1, "unknown sequence command $%02X", value);
        return;
    }
}

// ============================================================
// Register file
// ============================================================

// Map the live bus phase into the bus_status0 MSG/CD/IO bits (§3.2).
static uint8_t phase_bits(mesh_t *m) {
    if (m->msgout_pending)
        return 0x06u; // MSG OUT — the virtual post-select-with-ATN phase
    if (msgin_pending(m))
        return 0x07u; // MSG IN — the target has an SDTR to deliver
    // Below the overlays it is just the wire, in the same MSG/C-D/I-O order.
    // The copy this replaced had no MESSAGE OUT case at all, so that phase read
    // back as 0x00 -- DATA OUT.  Unreachable today (only the 5380 drives the
    // bus into MESSAGE OUT), but wrong, and wrong in the one direction a
    // transfer-phase encoding must never be.
    return scsi_phase_wire_bits(scsi_get_bus_phase(m->bus));
}

static uint8_t mesh_read_inner(mesh_t *m, uint32_t offset);

uint8_t mesh_read(mesh_t *m, uint32_t offset) {
    uint8_t v = mesh_read_inner(m, offset);
    LOG(4, "read reg %u -> $%02X", (offset >> 4) & 0xFu, v);
    return v;
}

static uint8_t mesh_read_inner(mesh_t *m, uint32_t offset) {
    uint32_t idx = (offset >> 4) & 0xFu;
    switch (idx) {
    case MR_COUNT_LO:
        return (uint8_t)(m->remaining & 0xFFu);
    case MR_COUNT_HI:
        return (uint8_t)((m->remaining >> 8) & 0xFFu);
    case MR_FIFO: {
        uint8_t v = fifo_pop(m);
        // A draining non-DMA DATAIN refills as the driver reads.
        if (m->active == CMD_DATAIN && !m->active_dma)
            pump_in(m);
        return v;
    }
    case MR_SEQUENCE:
        return m->sequence;
    case MR_BUS_STATUS0: {
        // REQ presents whenever a target is connected: the driver class
        // spin-waits on REQ between phases before dropping ATN.
        uint8_t v = phase_bits(m);
        if (m->connected)
            v |= BS0_REQ;
        if (m->bus0_atn || m->msgout_pending)
            v |= BS0_ATN;
        return v;
    }
    case MR_BUS_STATUS1:
        return m->connected ? BS1_BSY : 0;
    case MR_FIFO_COUNT:
        return byte_fifo_count(&m->fifo);
    case MR_EXCEPTION:
        return m->exception;
    case MR_ERROR:
        return m->error;
    case MR_INTR_MASK:
        return m->intr_mask;
    case MR_INTERRUPT:
        return m->interrupt;
    case MR_SOURCE_ID:
        return m->source_id;
    case MR_DEST_ID:
        return m->dest_id;
    case MR_SYNC_PARAMS:
        return m->sync_params;
    case MR_MESH_ID:
        return MESH_ID_VALUE;
    case MR_SEL_TIMEOUT:
        return m->sel_timeout;
    }
    return 0;
}

void mesh_write(mesh_t *m, uint32_t offset, uint8_t value) {
    uint32_t idx = (offset >> 4) & 0xFu;
    if (idx != MR_SEQUENCE)
        LOG(4, "write reg %u = $%02X", idx, value);
    switch (idx) {
    case MR_COUNT_LO:
        m->remaining = (m->remaining & 0xFF00u) | value;
        break;
    case MR_COUNT_HI:
        m->remaining = (m->remaining & 0x00FFu) | ((uint32_t)value << 8);
        break;
    case MR_FIFO:
        fifo_push(m, value);
        // A waiting out-going transfer consumes bytes as they arrive
        // (the driver issues the sequence first, then fills the FIFO).
        if (m->active == CMD_COMMAND || m->active == CMD_MSGOUT || (m->active == CMD_DATAOUT && !m->active_dma))
            pump_out(m);
        break;
    case MR_SEQUENCE: {
        // A transfer command uses the current count; 0 arms the full
        // 65536 of the 16-bit down-counter.
        uint32_t count = m->remaining ? m->remaining : 65536u;
        do_sequence(m, value, count);
        break;
    }
    case MR_BUS_STATUS0:
        // Explicit ATN drive/release (the driver's last-message-byte
        // timing dance); other bits are not driven this way.
        m->bus0_atn = (value & BS0_ATN) ? 1 : 0;
        break;
    case MR_BUS_STATUS1:
        if (value & BS1_RST) {
            // SCSI bus reset: everything back to bus-free.
            LOG(2, "bus reset via bus_status1");
            if (m->bus) {
                scsi_external_release(m->bus);
                scsi_bus_reset(m->bus); // also abandons any armed select time-out
            }
            m->connected = 0;
            m->msgout_pending = 0;
            m->active = 0;
            m->active_dma = 0;
            fifo_clear(m);
            msg_session_reset(m);
        }
        break;
    case MR_EXCEPTION:
        m->exception &= (uint8_t)~value; // W1C
        break;
    case MR_ERROR:
        m->error &= (uint8_t)~value; // W1C
        break;
    case MR_INTR_MASK:
        m->intr_mask = value & 0x07u;
        mesh_update_irq(m);
        break;
    case MR_INTERRUPT:
        m->interrupt &= (uint8_t)~value; // W1C
        mesh_update_irq(m);
        break;
    case MR_SOURCE_ID:
        m->source_id = value & 7u;
        break;
    case MR_DEST_ID:
        m->dest_id = value & 7u;
        break;
    case MR_SYNC_PARAMS: {
        // The period field clamps to the async minimum of 2 (100 ns is
        // period 0's special case, but the sequencer's smallest DIVIDER
        // is 2 — mesh.h's "(x+2)*40ns" law).  Load-bearing: the ROM's
        // native driver writes 1 as its unnegotiated default and reads
        // the register back at data-phase setup — a readback still
        // exactly 1 makes it refuse the transfer and disconnect (ROM
        // $FFEB75F4), so real silicon must not store the raw 1.
        uint8_t period = value & 0x0Fu;
        if (period < 2)
            period = 2;
        m->sync_params = (uint8_t)((value & 0xF0u) | period);
        break;
    }
    case MR_SEL_TIMEOUT:
        m->sel_timeout = value;
        break;
    default:
        LOG(1, "write to read-only register %u = $%02X", idx, value);
        break;
    }
}

// ============================================================
// Lifecycle
// ============================================================

void mesh_reset(mesh_t *m) {
    if (!m)
        return;
    if (m->connected && m->bus)
        scsi_external_release(m->bus);
    // The PLAIN-DATA region only -- the same bound the checkpoint uses.
    //
    // This was sizeof(*m) while the struct had no pointers and lived inside the
    // machine's own state.  Now that it owns its bus and its callbacks, a full
    // memset erases the wiring: the chip comes back with no bus, no interrupt
    // line and no DBDMA kick, and the Power Macintosh never finds a boot drive.
    memset(m, 0, offsetof(mesh_t, bus));
    m->sync_params = 2; // ASYNC_PARAMS power-on default
    if (m->irq_cb)
        m->irq_cb(m->irq_ctx, false);
}

// ============================================================================
// Lifecycle
// ============================================================================
// The 53C96's shape, because they are peers: an opaque handle the machine
// holds, wiring supplied by callback, and a checkpoint bounded by the first
// pointer.

mesh_t *mesh_init(struct scheduler *sched, checkpoint_t *cp) {
    mesh_t *m = (mesh_t *)calloc(1, sizeof(mesh_t));
    if (!m)
        return NULL;
    m->sched = sched;
    if (cp) {
        // The plain-data block only.  The pointers below it stay NULL and are
        // re-bound by the machine (mesh_attach_bus, mesh_set_irq_callback,
        // mesh_set_dbdma_kick).
        system_read_checkpoint_data(cp, m, offsetof(mesh_t, bus));
        m->sched = sched;
    }
    return m;
}

void mesh_delete(mesh_t *m) {
    if (!m)
        return;
    // Any selection time-out this controller armed is queued on the BUS, so it
    // must be cancelled through the bus -- an event outliving its source is
    // what F-11/F-12 were about.
    scsi_bus_cancel_select_timeout(m->bus);
    free(m);
}

void mesh_checkpoint(mesh_t *m, checkpoint_t *cp) {
    if (!m || !cp)
        return;
    system_write_checkpoint_data(cp, m, offsetof(mesh_t, bus));
}

void mesh_attach_bus(mesh_t *m, struct scsi *bus) {
    if (m)
        m->bus = bus;
}

void mesh_set_irq_callback(mesh_t *m, mesh_irq_cb cb, void *ctx) {
    if (!m)
        return;
    m->irq_cb = cb;
    m->irq_ctx = ctx;
    // Re-drive the current level so a restore re-establishes the machine's
    // input, the same thing scsi_53c96_set_irq_callback does.
    if (cb)
        cb(ctx, (m->interrupt & m->intr_mask) != 0);
}

void mesh_set_dbdma_kick(mesh_t *m, mesh_dbdma_kick_cb cb, void *ctx) {
    if (!m)
        return;
    m->dbdma_kick = cb;
    m->dbdma_ctx = ctx;
}
