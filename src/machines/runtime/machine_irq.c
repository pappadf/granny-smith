// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_irq.c
// The 68K families' interrupt route engine.  See the header.

#include "machine_irq.h"

#include "cpu.h"
#include "scheduler.h"
#include "system_internal.h"

int machine_irq_update(config_t *cfg, const machine_irq_route_t *routes, int source, bool active) {
    if (active)
        cfg->rt.irq |= source;
    else
        cfg->rt.irq &= ~source;
    // Highest-priority active source wins (table ordered high→low IPL)
    int ipl = machine_irq_resolve_ipl(routes, (uint32_t)cfg->rt.irq);
    cpu_set_ipl(cfg->cpu, ipl);
    cpu_reschedule(cfg->scheduler);
    return ipl;
}

void machine_irq_cold_reset(config_t *cfg, checkpoint_t *cp) {
    // On a restore the devices and the irq part already hold the state
    if (cp)
        return;
    cfg->rt.irq = 0;
    cpu_set_ipl(cfg->cpu, 0);
}
