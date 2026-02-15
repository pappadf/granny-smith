// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// keyboard.h
// Public interface for Mac Plus keyboard emulation.

#ifndef KEYBOARD_H
#define KEYBOARD_H

// === Includes ===

#include "common.h"
#include "scc.h"
#include "scheduler.h"
#include "via.h"

#include <stdbool.h>

// === Type Definitions ===

// Represents a key press or release event
typedef enum { key_up, key_down } key_event_t;

struct keyboard;
typedef struct keyboard keyboard_t;

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

// Creates and initializes a keyboard instance
keyboard_t *keyboard_init(struct scheduler *scheduler, scc_t *scc, via_t *via, checkpoint_t *checkpoint);

// Frees resources associated with a keyboard instance
void keyboard_delete(keyboard_t *keyboard);

// Saves keyboard state to a checkpoint
void keyboard_checkpoint(keyboard_t *restrict keyboard, checkpoint_t *checkpoint);

// === Operations ===

// Receives a command byte from the Mac via VIA shift register
void keyboard_input(keyboard_t *keyboard, unsigned char val);

// Processes a key event from the host system
extern void keyboard_update(keyboard_t *keyboard, key_event_t event, int key);

#endif // KEYBOARD_H
