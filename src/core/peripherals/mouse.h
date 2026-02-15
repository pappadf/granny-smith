// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mouse.h
// Public interface for Mac Plus mouse emulation.

#ifndef MOUSE_H
#define MOUSE_H

// === Includes ===
#include "common.h"
#include "scc.h"
#include "via.h"

#include <stdbool.h>

// === Forward Declarations ===
struct scheduler;

// === Type Definitions ===
struct mouse;
typedef struct mouse mouse_t;

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

mouse_t *mouse_init(struct scheduler *scheduler, scc_t *scc, via_t *restrict via, checkpoint_t *checkpoint);

void mouse_delete(mouse_t *mouse);

void mouse_checkpoint(mouse_t *restrict mouse, checkpoint_t *checkpoint);

// === Operations ===

void mouse_update(mouse_t *restrict mouse, bool button, int dx, int dy);

#endif // MOUSE_H
