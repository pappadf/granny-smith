// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iop_internal.h
// Internal interface shared between iop.c (generic PIC ASIC behaviour),
// iop_scc.c (SCC IOP firmware-behaviour model), and iop_swim.c (SWIM IOP
// firmware-behaviour model).
//
// Not exposed to the rest of the emulator — machine code only sees iop.h.

#ifndef IOP_INTERNAL_H
#define IOP_INTERNAL_H

#include "iop.h"
#include "iop_regs.h"
#include "memory.h"

#include <stdbool.h>
#include <stdint.h>

struct scheduler;

// PIC has 32 KB of shared RAM (MaxIOPRamAddr = $7FFF).  We allocate 64 KB
// to give us room for the firmware's view ($0000-$7FFF data + $F800-$FFFF
// vector mirror), as host writes via iopRamAddr can reach the $F0xx
// on-die MMIO range and the $F800-$FFFF vector mirror.
#define IOP_RAM_SIZE 0x10000u

// Forward declaration so the behaviour table can take iop_t * arguments.
typedef struct iop iop_t;

// ============================================================================
//  Behaviour table — what the 65C02 firmware would do, in C
//
// Each callback is OPTIONAL.  iop.c falls through to a generic default
// when a callback is NULL.
//
// All callbacks run on the host CPU's thread; they have full access to
// iop->ram[] and may mutate the PIC's status bits via the helpers below.
// ============================================================================

typedef struct iop_behavior {
    // Identifier used in log messages and firmware-checksum lookups.
    const char *name;

    // IOP number — SccIopNum or SwimIopNum.
    iop_kind_t kind;

    // FNV-1a 32-bit checksum of the expected 'iopc' firmware image
    // (computed over iop->ram[0..IOP_RAM_SIZE-1] at the moment of the
    // RUN-bit 0→1 transition).  If the actually-loaded image hashes to
    // something different, iop.c logs a warning but still runs.  A value
    // of 0 disables the check (use during development before we have a
    // known-good hash).
    uint32_t expected_fnv1a;

    // Called once on every RUN-bit 0→1 transition AFTER iop_init_mailbox
    // has seeded the canonical mailbox state.  Use this to model
    // the firmware's reset-entry behaviour (zeroing zero-page, setting
    // up free lists, etc.) and to register any periodic timer callbacks.
    void (*on_run_start)(iop_t *iop);

    // Called on every host write of setIopGenInt to iopStatCtl (= bit 3
    // = iopGenInterrupt set in the written byte).  Models the firmware's
    // host-kick interrupt handler ($5070 on SWIM, $0521 on SCC):
    // walks XmtMsg looking for NewMsgSent, walks RcvMsg for MsgCompleted,
    // dispatches per-channel handlers, optionally posts a reply by
    // calling iop_post_reply().
    void (*on_host_kick)(iop_t *iop);

    // Called once from iop_init so this IOP's periodic-timer callbacks
    // can register themselves with the scheduler. Required for
    // the scheduler to name pending events at save time and to resolve
    // saved events back to live callback pointers on restore. NULL if the behaviour schedules no events
    // (e.g. SCC IOP, which is purely event-driven on host kicks).
    void (*register_events)(iop_t *iop);

    // Called by iop_power_on: cancel every event this behaviour scheduled
    // with the IOP as its source.  The callbacks are static to the
    // behaviour's file, so only it can name them -- and scheduler_forget_
    // source is for destructors only (it drops the event TYPES too).  NULL
    // if the behaviour schedules nothing.
    void (*cancel_events)(iop_t *iop);

    // Called from iop_init after a checkpoint has been read into the
    // PIC's state, to re-establish what the behaviour hangs off other
    // devices (the SCC IOP's LocalTalk link).  NULL if nothing.
    void (*on_restore)(iop_t *iop);

    // Called from iop_delete before the IOP is freed, while the devices it
    // fronts still exist.  NULL if nothing.
    void (*on_delete)(iop_t *iop);

} iop_behavior_t;

// Defined in iop_scc.c.
extern const iop_behavior_t iop_scc_behavior;

// Defined in iop_swim.c.
extern const iop_behavior_t iop_swim_behavior;

// ============================================================================
//  Concrete PIC state
//
// Visible to iop_scc.c / iop_swim.c so the behaviour callbacks can read
// and mutate the shared RAM and host-visible status bits directly.
// ============================================================================

// The SCC IOP's LocalTalk driver ('iopc', downloaded by the System's IOP
// LocalTalk 'ltlk'), as iop_scc.c models it: the LLAP link -- node
// acquisition, the lapENQ/lapACK and lapRTS/lapCTS dialogs, frames to and from
// the wire -- with DDP and above left to the host's .MPP.  Plain data, so it
// rides in the PIC's checkpoint prefix.
#define IOP_LT_FRAME_MAX 603 // LLAP header + 600 data bytes
#define IOP_LT_RXQ_DEPTH 8 // frames waiting for RcvMsg[5]

// What a channel's downloaded driver is, decided at Initialize Driver
enum {
    IOP_DRIVER_NONE = 0, // not initialised
    IOP_DRIVER_SERIAL, // the asynchronous serial driver
    IOP_DRIVER_LOCALTALK, // the LocalTalk driver (channel B only)
};

typedef struct iop_lt_frame {
    uint16_t len;
    uint8_t buf[IOP_LT_FRAME_MAX];
} iop_lt_frame_t;

typedef struct iop_scc_state {
    // IOP Kernel driver table: 0 = free, else the owner's ClientID.
    uint8_t driver_client[2];
    uint8_t driver_kind[2]; // IOP_DRIVER_*: what Initialize Driver found

    // The serial drivers ('SERD'), per channel (Serial IOP Driver ERS)
    bool ser_open[2];
    bool ser_read_waiting[2]; // a read is held until bytes arrive
    uint16_t ser_wbuf[2]; // IOP RAM addresses handed to the host
    uint16_t ser_stats[2];
    // Kernel bypass: the ClientID that owns it, 0 when off.  The kernel
    // enters bypass at reset, owned by $FF until the IOP Manager leaves it.
    uint8_t bypass_client;

    // LocalTalk driver B (boxes 5-7, SCC channel B)
    bool lt_open;
    uint8_t lt_node; // the node the IOP answers for and stamps as source
    uint8_t lt_nodes[32]; // receive filter: bitmap of the nodes it answers for
    uint16_t lt_wbuf; // IOP RAM addresses handed to the host
    uint16_t lt_rbuf;
    uint16_t lt_stats;
    bool lt_rx_posted; // RcvMsg[5] holds a frame the host has not released
    uint8_t lt_rxq_head;
    uint8_t lt_rxq_count;
    iop_lt_frame_t lt_rxq[IOP_LT_RXQ_DEPTH];
} iop_scc_state_t;

struct iop {
    uint8_t ram[IOP_RAM_SIZE];
    uint16_t ram_addr; // host-side iopRamAddr (16-bit pointer into iop->ram)
    uint8_t stat_ctl; // host-visible iopStatCtl (bits per iop_regs.h)
    bool host_irq; // current state of the `hint` line to OSS

    // Extent of the host's RAM writes outside the mailboxes since the
    // behaviour last reset it: where a downloaded driver image landed.
    uint16_t dl_lo, dl_hi;

    iop_scc_state_t scc; // SCC IOP only (iop_scc.c)

    const iop_behavior_t *behavior;

    const memory_interface_t *bypass_iface;
    void *bypass_device;
    iop_irq_fn irq_cb;
    void *cb_context;

    // Scheduler for autonomous events (firmware-replacement timers).
    // Used by iop_swim.c to model the firmware's periodic ADB / drive-
    // poll state machine via scheduler_new_cpu_event.  May be NULL.
    struct scheduler *scheduler;

    memory_interface_t memory_interface;
};

// ============================================================================
//  Helpers for behaviour callbacks
//
// Each models the firmware-side effect of a specific event.  All update
// iop->stat_ctl and call iop->irq_cb as needed (so the OSS source-6/7
// pending state stays consistent).
// ============================================================================

// Asserts iopInt0Active in iopStatCtl (= firmware wrote $04 to HostIntReq).
// On SCC IOP this is "TX/RX completion or driver event" (cf. $0521 logic).
void iop_raise_int0(iop_t *iop);

// Asserts iopInt1Active in iopStatCtl (= firmware wrote $08 to HostIntReq).
// On SWIM IOP this is the SendMsgToHost path at $5125 — the boot-time
// "I have something for you" signal that wakes the host's level-1 IRQ.
void iop_raise_int1(iop_t *iop);

// Posts an outgoing reply from the IOP to the host:
//   - Copies `payload` (up to MaxIopMsgLen bytes) into the RcvMsg slot
//     at IOPRcvMsgBase + $20 + slot * MaxIopMsgLen.
//   - Sets RcvMsg[slot].state = NewMsgSent.
//   - Raises Int1 (per SWIM firmware $5125) or Int0 (SCC convention).
//
// Returns false if slot is out of range.  This is the C-level equivalent
// of the firmware's SendMsgToHost routine.
bool iop_post_reply(iop_t *iop, int slot, const uint8_t *payload, unsigned len, bool use_int1);

// Marks an incoming XmtMsg slot as MsgCompleted and raises Int0.  This
// is what the firmware does after processing a host-posted command —
// the host's IRQ handler will then read whatever the firmware wrote
// back into the slot's payload area.
void iop_complete_xmt(iop_t *iop, int slot);

#endif // IOP_INTERNAL_H
