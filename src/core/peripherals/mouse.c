// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mouse.c
// Implements Macintosh Plus mouse quadrature signal generation for the SCC (X1/Y1) and VIA (X2/Y2).

#include "mouse.h"
#include "cpu.h"
#include "system.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

// Represents the mouse device state and scheduling tails for each axis
struct mouse {
    int x1, y1; // Current interrupt line logic levels (SCC DCD inputs)
    uint64_t tail_timestamp_x; // CPU cycle timestamp of the last scheduled X pulse (edge)
    uint64_t tail_timestamp_y; // CPU cycle timestamp of the last scheduled Y pulse (edge)

    /* Pointers last */
    struct scheduler *scheduler; // Event scheduler (CPU-cycle aligned)
    scc_t *scc; // SCC for DCD (X1/Y1) updates
    via_t *via; // VIA for quadrature + button inputs
};

// Fixed cycles per slot (uniform slot timing). Adjust to tune overall pointer speed.
#define MOUSE_CYCLES_PER_SLOT 10000
static inline uint64_t cycles_per_slot(mouse_t *restrict m, int steps_in_batch) {
    (void)m;
    (void)steps_in_batch; // Unused in fixed model
    return (uint64_t)MOUSE_CYCLES_PER_SLOT / steps_in_batch;
}

#define EVENT_DATA_HORIZONTAL 2
#define EVENT_DATA_POSITIVE   1

// Executes a scheduled single-slot edge toggle for one axis and sets the correct quadrature (X2/Y2) bit
static void mouse_event_step(void *source, uint64_t data) {
    mouse_t *m = source;

    if (data & EVENT_DATA_HORIZONTAL) {
        // Toggle X1 edge (rising or falling depending on previous state)
        m->x1 = !m->x1;
        // For right motion X2 follows X1; for left motion X2 is inverted relative to X1
        int x2 = data & EVENT_DATA_POSITIVE ? m->x1 : !m->x1;
        scc_dcd(m->scc, 0, m->x1); // Update SCC DCD A (X1)
        via_input(m->via, 1, 4, x2); // Update VIA PB4 (X2)
    } else {
        // Toggle Y1 edge
        m->y1 = !m->y1;
        // For down motion Y2 is opposite Y1; for up motion Y2 equals Y1 (see table)
        int y2 = data & EVENT_DATA_POSITIVE ? !m->y1 : m->y1;
        scc_dcd(m->scc, 1, m->y1); // Update SCC DCD B (Y1)
        via_input(m->via, 1, 5, y2); // Update VIA PB5 (Y2)
    }
}

// Internal helper: schedule all pulses for one axis for a single host delta
// Schedules all slot edges for one axis for a single host delta; each slot becomes its own event in time
static void schedule_axis(mouse_t *restrict m, int delta, bool horizontal, uint64_t now_cycles) {
    if (delta == 0)
        return; // No movement -> no pulses

    int steps = delta > 0 ? delta : -delta; // Number of slot transitions to emit
    bool positive = delta > 0; // Direction sign used for quadrature relationship
    uint64_t per_slot = cycles_per_slot(m, steps); // Constant delay between successive slots

    // Choose tail pointer for axis so new events follow any already queued pulses
    uint64_t *tail = horizontal ? &m->tail_timestamp_x : &m->tail_timestamp_y;
    if (*tail < now_cycles)
        *tail = now_cycles; // Catch up if idle (avoid time reversal)

    // Stagger Y events by half a slot relative to X to prevent simultaneous pulses.
    // When both axes fire at exactly the same timestamp during diagonal motion,
    // the CPU may not service the first interrupt before the second pulse arrives,
    // causing DCD to toggle twice and the ROM to miss the intermediate edge.
    if (!horizontal && *tail == now_cycles)
        *tail += per_slot / 2;

    const char *ev_name = horizontal ? "mouse X slot" : "mouse Y slot";

    for (int i = 0; i < steps; ++i) {
        *tail += per_slot; // Advance tail by one slot period
        uint64_t delta_cycles = *tail - now_cycles; // Relative delay from now

        // Event data encodes axis and direction in the two least significant bits
        uint64_t event_data = (horizontal ? 2 : 0) | (positive ? 1 : 0);

        scheduler_new_cpu_event(m->scheduler, &mouse_event_step, m, event_data, delta_cycles, 0);
    }
}

// Simple legacy scaling: large deltas are halved to moderate event density
static int scale(int value) {
    if (value == 0)
        return 0;
    if (value == -1 || value == 1)
        return value;
    if (value < -1 || value > 1)
        return value / 2; // Compress magnitude
    assert(0); // Should not reach here
    return 0;
}

// Public entry: accepts host-relative mouse deltas and schedules corresponding quadrature pulses
extern void mouse_update(mouse_t *restrict m, bool button, int dx, int dy) {
    // if (!scheduler_is_running(m->scheduler))
    //     return; // Ignore input while scheduler stopped

    // Button: VIA PB3, active low (0 = pressed)
    via_input(m->via, 1, 3, !button);

    // Apply simple scaling to damp extreme host motion
    dx = scale(dx);
    dy = scale(dy);

    uint64_t now_cycles = scheduler_cpu_cycles(m->scheduler);

    // Schedule independent sequences for X and Y preserving per-axis timing
    schedule_axis(m, dx, true, now_cycles);
    schedule_axis(m, dy, false, now_cycles);
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
    mouse->tail_timestamp_x = mouse->tail_timestamp_y = 0;

    // Register event type for checkpointing
    scheduler_new_event_type(scheduler, "mouse", mouse, "event_step", &mouse_event_step);

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
