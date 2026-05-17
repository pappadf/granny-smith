// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iop.h
// Public interface for the Macintosh IIfx I/O Processor (part 343S1021;
// host-side called PIC, software-side called IOP).
//
// This header exposes the names used by the System ROM's IOP Manager
// for mailbox geometry and message states, plus the public API our
// machine code uses (iop_init / iop_delete / iop_get_memory_interface /
// iop_checkpoint).
//
// The internal PIC register surface (host-side iopStatCtl bits, on-die
// $F0xx registers) lives in iop_regs.h and is consumed by iop.c and the
// per-IOP behavioural models (iop_scc.c, iop_swim.c).

#ifndef IOP_H
#define IOP_H

#include "checkpoint.h"
#include "memory.h"

#include <stdbool.h>
#include <stdint.h>

struct scheduler;

// ============================================================================
//  IOP identifiers and mailbox geometry
//
// The IOP Manager supports up to 8 IOPs (MaxIopNum=7).  The IIfx ships
// with two: the SCC IOP fronting the Zilog Z8530 dual serial controller,
// and the SWIM IOP fronting the SWIM1 floppy controller + ADB.
// ============================================================================

typedef enum iop_kind {
    SccIopNum = 0, // Z8530 SCC dual-channel serial (modem + printer)
    SwimIopNum = 1, // SWIM1 floppy controller + ADB single-wire bus
} iop_kind_t;

// Mailbox geometry — canonical IOP Manager values.
#define MaxIOPRamAddr 0x7FFF // 32 KB shared RAM; last addressable byte
#define MaxIopMsgNum  7 // 7 transmit + 7 receive message slots
#define MaxIopMsgLen  32 // up to 32 bytes per message slot

// Shared-RAM symbol offsets used by the IOP firmware convention.
#define IOPXmtMsgBase 0x0200 // XmtMsg[0].state — host's transmit-side mailbox
#define IOPRcvMsgBase 0x0300 // RcvMsg[0].state — IOP's reply-side mailbox
#define PatchReqAddr  0x021F // host writes non-zero → request firmware restart
#define IOPAliveAddr  0x031F // firmware writes $FF here; host polls

// Per-slot state byte offset within the XmtMsg / RcvMsg banks.
// XmtMsg[0].state ($0200) holds MaxIopMsgNum; XmtMsg[1..7].state lives at
// $0201..$0207.  The IOP Manager uses `(base | slot)` to compute the
// state address.
#define IOPMsgState(slot) (slot)

// Per-slot payload offset.  Computed as `(base | slot) << 5`.
// Slot 1 message buffer = $0220, slot 2 = $0240, ..., slot 7 = $02E0.
#define IOPMsgPayload(base, slot) ((base) + (slot) * MaxIopMsgLen)

// Message-state encodings.
typedef enum iop_msg_state {
    MsgIdle = 0, // slot is unused
    NewMsgSent = 1, // posted by sender, awaiting processing
    MsgReceived = 2, // processor claimed the slot, in-flight
    MsgCompleted = 3, // reply available; receiver should drain it
} iop_msg_state_t;

// ============================================================================
//  Public API
// ============================================================================

typedef struct iop iop_t;

// Called when an IOP's `hint` line to OSS changes state.  The callback's
// job is to translate this into an OSS source-pending change.
typedef void (*iop_irq_fn)(void *context, bool active);

// Creates an IOP bridge for one of the two IIfx IOPs.
//
//   kind          - SccIopNum or SwimIopNum.  Selects the behavioural model
//                   (iop_scc.c vs iop_swim.c).
//   bypass_iface  - memory_interface_t for the front-side device, used
//                   when the firmware (or host, in bypass mode) reaches
//                   through the iopBypassBase..iopBypassEnd window.
//   bypass_device - first argument to bypass_iface->read_uint8/etc.
//   irq_cb        - called whenever the IOP raises or drops its `hint` line.
//                   Must NOT be called with the same state twice.
//   context       - first argument to irq_cb.
//   scheduler     - emulator scheduler.  Used by the per-IOP behavioural
//                   model to schedule autonomous events (e.g. the SWIM
//                   IOP's periodic ADB-poll Int1 notifications).  May
//                   be NULL for tests; periodic behaviour is then off.
//   checkpoint    - optional; restore state from a checkpoint stream.
iop_t *iop_init(iop_kind_t kind, const memory_interface_t *bypass_iface, void *bypass_device, iop_irq_fn irq_cb,
                void *context, struct scheduler *scheduler, checkpoint_t *checkpoint);

// Frees one IOP bridge.
void iop_delete(iop_t *iop);

// Saves IOP state to a checkpoint stream.
void iop_checkpoint(iop_t *iop, checkpoint_t *checkpoint);

// Returns the host-side memory-mapped I/O interface for the PIC's $2000-byte
// aperture (VIA1+5 .. VIA1+6 range on IIfx; see iifx.c for the address
// decode that routes 0x50F12000-0x50F13FFF to the SWIM IOP).
const memory_interface_t *iop_get_memory_interface(iop_t *iop);

#endif // IOP_H
