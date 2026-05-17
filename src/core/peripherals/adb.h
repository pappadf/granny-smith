// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// adb.h
// Public interface for Apple Desktop Bus (ADB) transceiver emulation (SE/30).

#ifndef ADB_H
#define ADB_H

// === Includes ===

#include "common.h"
#include "keyboard.h"
#include "scheduler.h"
#include "via.h"

#include <stdbool.h>
#include <stdint.h>

// === Forward Declarations ===

struct adb;

// === Type Definitions ===

// Opaque handle for the ADB transceiver + device state machine
typedef struct adb adb_t;

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

// Creates and initialises an ADB controller instance, wiring it to VIA1
adb_t *adb_init(via_t *via, struct scheduler *scheduler, checkpoint_t *checkpoint);

// Frees all resources associated with an ADB controller instance
void adb_delete(adb_t *adb);

// Saves ADB controller state to a checkpoint
void adb_checkpoint(adb_t *restrict adb, checkpoint_t *checkpoint);

// === VIA Callback Hooks ===

// Called by the machine's VIA shift-out callback when a byte is fully shifted out
void adb_shift_byte(adb_t *adb, uint8_t byte);

// Called by the machine's VIA port-B output callback when ST0/ST1 state lines change
void adb_port_b_output(adb_t *adb, uint8_t value);

// === Operations ===

// Enqueues a host keyboard event in ADB Register 0 format
void adb_keyboard_event(adb_t *adb, key_event_t event, int key);

// Updates mouse movement and button state; accumulates deltas until next Talk R0
void adb_mouse_event(adb_t *adb, bool button, int dx, int dy);

// Injects mouse movement deltas without changing the current button state
void adb_mouse_move(adb_t *adb, int dx, int dy);

// === IOP-based ADB transaction (Macintosh IIfx and friends) ================
//
// On VIA-shift machines (SE/30, IIcx, IIx) the host writes the ADB command
// byte into VIA1's shift register and clocks bytes back via SR interrupts
// (handled by adb_shift_byte / adb_port_b_output above).  On the Macintosh
// IIfx, the SWIM IOP firmware bit-bangs the ADB bus itself — the host
// just posts an ADBMsg on XmtMsg[3] and reads the reply from RcvMsg[3].
//
// adb_iop_transact bridges that protocol to this module's existing device
// state machine: given an ADB command byte plus any Listen-side payload,
// it runs the same dispatch (Talk / Listen / Reset / Flush) and returns
// the Talk reply (if any).  Output buffer must hold up to 8 bytes.
//
// Returns true if a device responded with `*out_data_len` reply bytes;
// false for "no device at this address" (= NoReply, the firmware sets
// ADBMSG_FLAG_NOREPLY on the reply).
bool adb_iop_transact(adb_t *adb, uint8_t cmd, const uint8_t *in_data, int in_data_len, uint8_t *out_data,
                      int *out_data_len);

#endif // ADB_H
