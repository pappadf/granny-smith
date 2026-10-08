// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mouse.c
// Implements Macintosh Plus mouse quadrature signal generation for the SCC (X1/Y1) and VIA (X2/Y2).

#include "mouse.h"
#include "checkpoint.h"
#include "cpu.h"
#include "log.h"
#include "system.h"
#include "system_internal.h"

#include <stddef.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("mouse");

// The Plus mouse is a ball turning two slotted wheels, one per axis; each
// wheel's photo-interrupters give a quadrature pair, X1/X2 and Y1/Y2, and
// every edge of X1 or Y1 is a DCD interrupt on the SCC whose handler moves
// MTemp one count (the ROM's P_SCCInt_AChng_MouseH / BChng_MouseV).  An edge
// marks a fixed distance travelled, so the edges come as fast as the mouse
// moves: a slow drag spaces them out, a fast swipe packs them close.
//
// The host reports motion in batches -- so many counts since the last batch,
// in practice once per emulated frame -- and the model plays each batch the
// way the wheels would have: its counts spread evenly over the time the
// motion took (the interval since the previous batch, at most a frame).  A
// batch is played out by the time the next arrives, so the cursor follows the
// hand a frame behind and never further, at any speed.  Each axis is one
// pulse train: the counts still to go (signed: the direction) and the gap
// between its edges.  A new batch joins what is left and re-spreads it.
struct mouse {
    bool x1, y1; // Current interrupt line logic levels (SCC DCD inputs)
    int32_t pending_x; // counts still to play on each axis (signed)
    int32_t pending_y;
    uint64_t gap_ns_x; // the time between edges of the current train
    uint64_t gap_ns_y;
    double last_batch_ns; // when the previous host batch arrived
    double window_end_ns; // when the current batch is to be played out
    int8_t scale_rem_x; // Carried host-delta remainders from the 2:1 scaling,
    int8_t scale_rem_y; // so small deltas keep their X:Y ratio across events

    /* Pointers last */
    struct scheduler *scheduler; // Event scheduler (CPU-cycle aligned)
    scc_t *scc; // SCC for DCD (X1/Y1) updates
    via_t *via; // VIA for quadrature + button inputs
};

// The closest two edges on one axis may come.  Each is an interrupt, and an
// edge that lands before the handler has read the last one toggles DCD again
// unseen: the count is lost.  The ROM's handler with the SCC's interrupt
// dispatch takes a few hundred cycles; edges 500 cycles apart were enough to
// break up MusicWorks playback, so a train keeps ~1,000 cycles (at the Plus's
// 7.83 MHz) between edges -- ~7,800 counts/s, three times a violently swung
// real mouse.  Only a batch faster than that runs past its frame.
#define MOUSE_MIN_EDGE_NS 128000ULL

#define AXIS_X 0
#define AXIS_Y 1

// VIA port B pins the Plus mouse drives (port index 1)
#define MOUSE_VIA_PORT_B 1
#define MOUSE_PB_BUTTON  3 // /SW: button, active low (0 = pressed)
#define MOUSE_PB_X2      4 // X2 quadrature
#define MOUSE_PB_Y2      5 // Y2 quadrature

// Emit one edge on an axis, in the direction `positive`, and set the
// quadrature (X2/Y2) bit that tells the handler which way it went.
static void mouse_edge(mouse_t *m, int axis, bool positive) {
    if (axis == AXIS_X) {
        // Toggle X1 edge (rising or falling depending on previous state)
        m->x1 = !m->x1;
        // For right motion X2 follows X1; for left motion X2 is inverted relative to X1
        bool x2 = positive ? m->x1 : !m->x1;
        scc_dcd(m->scc, 0, m->x1); // Update SCC DCD A (X1)
        via_input(m->via, MOUSE_VIA_PORT_B, MOUSE_PB_X2, x2);
    } else {
        // Toggle Y1 edge
        m->y1 = !m->y1;
        // For down motion Y2 is opposite Y1; for up motion Y2 equals Y1 (see table)
        bool y2 = positive ? !m->y1 : m->y1;
        scc_dcd(m->scc, 1, m->y1); // Update SCC DCD B (Y1)
        via_input(m->via, MOUSE_VIA_PORT_B, MOUSE_PB_Y2, y2);
    }
}

// The next edge of an axis's pulse train; `data` is the axis.
static void mouse_train_event(void *source, uint64_t data) {
    mouse_t *m = source;
    int axis = (int)data;
    int32_t *pending = axis == AXIS_X ? &m->pending_x : &m->pending_y;
    if (*pending == 0)
        return;
    bool positive = *pending > 0;
    mouse_edge(m, axis, positive);
    *pending += positive ? -1 : 1;
    if (*pending != 0)
        scheduler_new_cpu_event(m->scheduler, &mouse_train_event, m, (uint64_t)axis, 0,
                                axis == AXIS_X ? m->gap_ns_x : m->gap_ns_y);
}

// Spread an axis's remaining counts evenly over the time left in the window:
// the first edge one gap from now (half a gap for Y, so the axes' edges do
// not coincide), the last at the window's end.
static void mouse_train_start(mouse_t *m, int axis, double now_ns) {
    int32_t pending = axis == AXIS_X ? m->pending_x : m->pending_y;
    remove_event_by_data(m->scheduler, &mouse_train_event, m, (uint64_t)axis);
    if (pending == 0)
        return;
    uint32_t steps = (uint32_t)(pending < 0 ? -pending : pending);
    double window = m->window_end_ns - now_ns;
    uint64_t gap = window > 0 ? (uint64_t)(window / steps) : 0;
    if (gap < MOUSE_MIN_EDGE_NS)
        gap = MOUSE_MIN_EDGE_NS;
    if (axis == AXIS_X)
        m->gap_ns_x = gap;
    else
        m->gap_ns_y = gap;
    uint64_t first = axis == AXIS_X ? gap : gap / 2;
    LOG(3, "train %s: %+d count(s), %llu ns apart", axis == AXIS_X ? "X" : "Y", pending, (unsigned long long)gap);
    scheduler_new_cpu_event(m->scheduler, &mouse_train_event, m, (uint64_t)axis, 0, first);
}

// 2:1 host-pixel-to-count scaling with a carried remainder. Halving each
// event independently with integer division collapsed small deltas: at high
// pointer-event rates (1 kHz mice, uncoalesced trackpads) per-event deltas
// are +-1..3 on both axes, and 2 and 3 both floored to 1 — nearly every
// event contributed (+-1,+-1) and the cursor walked perfect 45-degree
// diagonals. Carrying the remainder preserves the true X:Y ratio across
// events (and stops odd deltas from silently losing half a count).
static int scale(int value, int8_t *rem) {
    if (value == -1 || value == 1)
        return value; // Preserve single-pixel nudges exactly
    int total = value + *rem;
    int out = total / 2; // Truncates toward zero
    *rem = (int8_t)(total - out * 2);
    LOG(3, "scale: %+d + rem %+d -> %+d, rem %+d", value, (int)(total - value), out, (int)*rem);
    return out;
}

// A host batch: dx/dy host pixels moved since the last one.  Scaled 2:1 to
// counts, joined to whatever the trains still have to play, and re-spread
// over the time the motion took -- the interval since the previous batch,
// at most a frame.  Batches that arrive together (host events between two
// emulated frames all land at the same instant) share one window.
static void mouse_motion(mouse_t *restrict m, int dx, int dy) {
    dx = scale(dx, &m->scale_rem_x);
    dy = scale(dy, &m->scale_rem_y);
    if (dx == 0 && dy == 0)
        return;

    double now = scheduler_time_ns(m->scheduler);
    double since = now - m->last_batch_ns;
    if (since > 0) {
        double window = since < (double)MAC_VBL_PERIOD_NS ? since : (double)MAC_VBL_PERIOD_NS;
        m->window_end_ns = now + window;
        m->last_batch_ns = now;
    }

    if (dx != 0) {
        m->pending_x += dx;
        mouse_train_start(m, AXIS_X, now);
    }
    if (dy != 0) {
        m->pending_y += dy;
        mouse_train_start(m, AXIS_Y, now);
    }
}

// Host motion and the button (VIA PB3, active low: 0 = pressed).
void mouse_update(mouse_t *restrict m, bool button, int dx, int dy) {
    via_input(m->via, MOUSE_VIA_PORT_B, MOUSE_PB_BUTTON, !button);
    mouse_motion(m, dx, dy);
}

// Host motion without a button change.
void mouse_move(mouse_t *restrict m, int dx, int dy) {
    mouse_motion(m, dx, dy);
}

// Allocates and initializes a mouse instance with default timing state
mouse_t *mouse_init(struct scheduler *scheduler, scc_t *scc, via_t *restrict via, checkpoint_t *checkpoint) {
    mouse_t *mouse = (mouse_t *)malloc(sizeof(mouse_t));
    if (!mouse)
        return NULL;
    memset(mouse, 0, sizeof(mouse_t));
    mouse->scheduler = scheduler;
    mouse->scc = scc;
    mouse->via = via;

    // Register event type for checkpointing
    scheduler_new_event_type(scheduler, "mouse", mouse, "train", &mouse_train_event);

    // Load from checkpoint if provided
    if (checkpoint) {
        size_t data_size = offsetof(mouse_t, scheduler);
        system_read_checkpoint_data(checkpoint, mouse, data_size);
    }

    return mouse;
}

// Free resources associated with a mouse instance
void mouse_delete(mouse_t *mouse) {
    if (!mouse)
        return;
    // Drop everything the scheduler still holds for this object before any
    // of it is torn down.
    scheduler_forget_source(mouse->scheduler, mouse);
    free(mouse);
}

// Save mouse state to a checkpoint
void mouse_checkpoint(mouse_t *restrict mouse, checkpoint_t *checkpoint) {

    // Save mouse state
    if (!mouse || !checkpoint)
        return;
    size_t data_size = offsetof(mouse_t, scheduler);
    system_write_checkpoint_data(checkpoint, mouse, data_size);
}
