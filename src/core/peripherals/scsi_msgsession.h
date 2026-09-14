// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scsi_msgsession.h
// The initiator-side SCSI message conversation: IDENTIFY, SDTR, WDTR.
//
// Our shared bus model (scsi_bus.c) answers an external initiator with COMMAND
// phase the moment selection succeeds.  It has no MESSAGE OUT for the IDENTIFY
// and no notion of a negotiation, because only a real initiator chip needs one.
// So the two chips that DO need one -- Apple's MESH and the Symbios 53C8xx
// SCRIPTS engine -- each synthesised the whole conversation: collect the bytes
// the driver sends, parse them, and present a virtual MESSAGE IN carrying the
// reply.  They wrote the same parse loop twice, and it drifted.
//
// What is shared is the CONVERSATION.  What each chip keeps is its own phase
// overlay (`msgout_pending`, `msgin_taken`), because those feed register and
// REQ logic that has nothing to do with message parsing.
//
// Capability limits are per chip and come from the parts, not from each other:
//
//   MESH     min_period 25, max_offset 15, narrow.  Its sync_params register
//            (0xD0) packs the offset into FOUR bits -- `offset = value >> 4` --
//            so 15 is the deepest it can express, and its transfer period is
//            (x + 2) * 40 ns with x == 0 meaning 100 ns, so 100 ns (SDTR
//            period 25) is the fastest it can run.
//            [Linux/NetBSD/MkLinux mesh.h; no Apple datasheet exists]
//
//   53C825   min_period 25, max_offset 16, wide.  Its SXFER register's
//            MO4-MO0 table (data manual, register 05) tops out at exactly 16,
//            and the part does Fast SCSI, so 100 ns is its floor too.
//
// The 53C825 used to echo whatever the initiator offered, unclamped.  AIX asks
// it for period 25 / offset 16, which is precisely the part's maximum, so the
// echo was right by luck; an initiator asking for more would have been told
// yes.  Clamping is the same rule for both chips now, with different numbers.

#ifndef SCSI_MSGSESSION_H
#define SCSI_MSGSESSION_H

#include <stdbool.h>
#include <stdint.h>

// The longest MESSAGE OUT we accumulate before parsing.  An IDENTIFY plus a
// three-byte extended message is 7; 16 leaves room for a driver that batches
// several, and is what the 53C8xx already used.
#define SCSI_MSG_OUT_MAX 16
// The longest reply we present: EXTENDED + len + code + two parameters.
#define SCSI_MSG_IN_MAX 8

// What a chip can actually agree to.  Both numbers are in the units SDTR uses:
// period in 4 ns steps, offset in bytes.
typedef struct {
    uint8_t min_period; // fastest period this part can run; slower offers stand
    uint8_t max_offset; // deepest offset it can express
    bool wide; // can it agree to 16-bit transfers?
} scsi_msg_caps_t;

// Plain data, no pointers: this lives inside the chip structs whose POD prefix
// a checkpoint writes in one block, so it must be safe to memcpy.
typedef struct {
    uint8_t out[SCSI_MSG_OUT_MAX]; // what the initiator has said so far
    uint8_t out_len;
    uint8_t in[SCSI_MSG_IN_MAX]; // what we are saying back
    uint8_t in_n, in_rd;
} scsi_msgsession_t;

// What one completed MESSAGE OUT amounted to.  The caller logs it and stores
// whatever its registers expose; the session itself keeps no agreement state,
// because the two chips report it in different registers.
typedef struct {
    bool identify; // an IDENTIFY (or family member) was present
    bool sdtr; // a synchronous agreement was reached
    uint8_t period; // ...at these values, AFTER clamping to the caps
    uint8_t offset; //
    bool wdtr; // a width agreement was reached
    bool wide; // ...16-bit if true, 8-bit if the chip is narrow
    bool rejected; // the initiator sent MESSAGE REJECT
    bool incomplete; // an extended message is still arriving; nothing consumed
} scsi_msg_result_t;

// Start a fresh conversation.  Called when a connection begins or ends: none of
// this survives a disconnect.
void scsi_msg_reset(scsi_msgsession_t *s);

// Add one byte the initiator sent.  Returns false if the buffer is full, in
// which case the byte is DROPPED and the caller should say so -- silently
// losing a message byte is how a negotiation hangs.
bool scsi_msg_collect(scsi_msgsession_t *s, uint8_t byte);

// The MESSAGE OUT ended: parse it under `caps` and queue any reply.
//
// If an extended message is still arriving the result says `incomplete` and
// the collected bytes are kept for the next call.  Otherwise the out buffer is
// consumed.  `result` may not be NULL.
void scsi_msg_complete(scsi_msgsession_t *s, const scsi_msg_caps_t *caps, scsi_msg_result_t *result);

// Is a virtual MESSAGE IN owed to the initiator?
bool scsi_msg_pending(const scsi_msgsession_t *s);

// Take the next reply byte.  False when there is none.
bool scsi_msg_next(scsi_msgsession_t *s, uint8_t *out);

#endif // SCSI_MSGSESSION_H
