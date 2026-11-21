// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// irq_line.h
// A device's interrupt output: the callback the machine binds it to and the
// level last driven.
//
// Every device that raises an interrupt had its own name for the same
// callback type, `void (*)(void *ctx, bool level)` (ata_irq_fn, bmac_irq_fn,
// scc_irq_fn, via_irq_fn, iop_irq_fn, sonic_irq_cb, mesh_irq_cb,
// scsi_53c96_irq_cb, dafb_irq_cb, ...); those are now aliases of irq_fn.
//
// irq_line_t adds the level and edge suppression: irq_line_set calls the
// consumer only when the level changes, which is what the devices that
// filtered edges (BMAC, SONIC, 53C96, DAFB) each wrote by hand.  The
// consumers are level-setters (an IPL resolver, a VIA CA1 input), so a
// device that called on every recompute sees no difference either -- but
// adopt it device by device, checking the consumer.
//
// fn and ctx are host pointers: an irq_line_t sits AFTER a device's
// checkpointed POD prefix.  Its level is then not in the checkpoint, so a
// device recomputes it on restore (irq_line_set from a level that starts
// false re-asserts a high one), and irq_line_force re-drives a level the
// consumer must hear even if it has not changed (binding a callback late).

#ifndef IRQ_LINE_H
#define IRQ_LINE_H

#include <stdbool.h>
#include <stddef.h>

// The callback a device drives its interrupt output through
typedef void (*irq_fn)(void *ctx, bool level);

// One interrupt output: the bound consumer and the level last driven
typedef struct irq_line {
    irq_fn fn; // consumer (NULL: unbound, levels are still tracked)
    void *ctx; // consumer's context
    bool level; // the level last driven
} irq_line_t;

// Binds the consumer; does not drive the level (see irq_line_force)
static inline void irq_line_bind(irq_line_t *l, irq_fn fn, void *ctx) {
    l->fn = fn;
    l->ctx = ctx;
}

// Drives `level`, telling the consumer only on a change
static inline void irq_line_set(irq_line_t *l, bool level) {
    if (level == l->level)
        return;
    l->level = level;
    if (l->fn)
        l->fn(l->ctx, level);
}

// Drives `level` and tells the consumer whether or not it changed
static inline void irq_line_force(irq_line_t *l, bool level) {
    l->level = level;
    if (l->fn)
        l->fn(l->ctx, level);
}

#endif // IRQ_LINE_H
