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
//   "default" (or empty/NULL) -> 'd'   the machine's usual path -- NOT the
//                                      same operation everywhere: on a Mac,
//                                      x/y is an absolute Toolbox cursor
//                                      target; on a Lisa, x/y are deltas
//   "relative" / "hw"         -> 'h'   x/y are hardware deltas, no Toolbox
//                                      help, on EVERY machine -- the
//                                      operation a host with a relative
//                                      pointer (a pointer lock) uses
//   "global"                  -> 'g'   absolute screen position (warp)
//   "aux"                     -> 'a'   A/UX MAE routing
//
// Returns 0 for anything else, which callers report as a bad argument.
char input_mouse_mode_parse(const char *mode);

// === Delta clamping =========================================================
//
// Clamp one mouse-axis delta to a bus's report range and return what did not
// fit, so the caller can carry it into the next report.  Carrying is the
// hardware-plausible model on every bus: a real mouse's counter keeps counting
// between reports, so motion that overflows one report is not lost, it is
// merely late.
//
// The LIMITS genuinely differ and must stay parameters:
//   ADB  Register 0 packs each axis into 7 bits, so -64..+63
//        (Guide 2e Table 8-4, "ADB transceiver register 0 in the Apple
//        Standard Mouse" -- bits 14-8 Y, bits 6-0 X, two's complement)
//   COPS reports a full signed byte, clamped to -127..+127 (lisa.md §11.4)
//
// Before this existed, adb.c carried the remainder and cops.c discarded it,
// which meant a large synthetic `mouse.move` silently lost distance on the
// Lisa.
static inline int input_clamp_delta(int delta, int lo, int hi, int *remaining) {
    int clamped = delta;
    if (clamped > hi)
        clamped = hi;
    if (clamped < lo)
        clamped = lo;
    if (remaining)
        *remaining = delta - clamped;
    return clamped;
}

#endif // MOUSE_H
