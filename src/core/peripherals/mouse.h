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

// Injects movement deltas without changing the current button state
void mouse_move(mouse_t *restrict mouse, int dx, int dy);

// === Routing mode ===========================================================
//
// The one parser for the mouse.move / mouse.click `mode` argument, shared
// across the substrate boundary: the object methods in mouse.c validate with
// it, and the machine-side hooks (mac_host_io.c) decode with it.  It used to
// exist twice, and the copies had already drifted apart in what they accepted.
//
//   "default" (or empty/NULL) -> 'd'   Toolbox cursor, the usual path
//   "global"                  -> 'g'   absolute screen position (warp)
//   "hw"                      -> 'h'   raw hardware deltas, no Toolbox help
//   "aux"                     -> 'a'   A/UX MAE routing
//
// Returns 0 for anything else, which callers report as a bad argument.
char input_mouse_mode_parse(const char *mode);

#endif // MOUSE_H
