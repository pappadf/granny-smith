// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_irq.h
// The 68K families' interrupt-source to CPU-IPL route engine.  A family keeps
// its active sources in cfg->rt.irq (one bit each, saved by
// machine_part_irq) and an ordered table of routes; an edge on any source
// sets or clears its bit and re-derives the IPL from the highest-priority
// active one.  GLUE, MDU and MCU route through one table, the AV through its
// own, the Plus and the Lisa through theirs -- one engine.  (The IIfx asks its
// OSS, and the PowerPC families use irq_controller.h; both are outside this.)

#ifndef GS_MACHINES_RUNTIME_MACHINE_IRQ_H
#define GS_MACHINES_RUNTIME_MACHINE_IRQ_H

#include "checkpoint.h"

#include <stdbool.h>
#include <stdint.h>

struct config;

// One IRQ source → CPU-IPL routing rule.  A family's routing is an ordered
// table of these, highest IPL first, ending in a {0, 0} sentinel.
typedef struct machine_irq_route {
    int source; // IRQ source bit(s) in cfg->rt.irq
    int ipl; // CPU IPL it raises
} machine_irq_route_t;

// The IPL of the highest-priority active source in `irq`: the first route
// whose source bit is set, 0 when none is.  Pure; unit-tested.
static inline int machine_irq_resolve_ipl(const machine_irq_route_t *routes, uint32_t irq) {
    for (; routes->source; routes++)
        if (irq & (uint32_t)routes->source)
            return routes->ipl;
    return 0;
}

// Set or clear `source` in cfg->rt.irq, drive the CPU IPL from `routes` and
// reschedule; returns the new IPL.
int machine_irq_update(struct config *cfg, const machine_irq_route_t *routes, int source, bool active);

// A cold boot starts with no source active and the IPL at 0; a restore keeps
// the checkpoint's (machine_part_irq), so this does nothing then.
void machine_irq_cold_reset(struct config *cfg, checkpoint_t *cp);

#endif // GS_MACHINES_RUNTIME_MACHINE_IRQ_H
