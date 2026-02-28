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

#endif // ADB_H
