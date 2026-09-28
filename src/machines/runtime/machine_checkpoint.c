// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_checkpoint.c — the shared checkpoint prefix.  See the header for
// the ordering contract and for why the Lisa is not on it.

#include "machine_checkpoint.h"

#include "appletalk.h"
#include "cpu.h"
#include "memory.h"
#include "ppc.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "system.h"
#include "system_config.h"
#include "via.h"

void machine_checkpoint_save_core(struct config *cfg, struct checkpoint *cp) {
    memory_map_checkpoint(cfg->mem_map, cp);
    // One or the other, never both: which CPU this machine has is the same
    // fact the teardown chain reads (machine_teardown.h).
    if (cfg->ppc)
        ppc_checkpoint(cfg->ppc, cp); // the PowerPC families
    else
        cpu_checkpoint(cfg->cpu, cp); // on the 040 families this carries the MMU register file too
    scheduler_checkpoint(cfg->scheduler, cp);
    // cfg->irq is the 68k families' aggregated source bitmap.  A PowerPC
    // machine has one external-interrupt pin driven by its own controller
    // (the AMIC's ICR, Grand Central's latch) and never reads cfg->irq, so
    // it carries nothing here -- which is also exactly what PDM and TNT's
    // hand-written prefixes already did.
    if (!cfg->ppc)
        system_write_checkpoint_data(cp, &cfg->irq, sizeof(cfg->irq));
    rtc_checkpoint(cfg->rtc, cp);
    scc_checkpoint(cfg->scc, cp);
    appletalk_checkpoint(cp);
    via_checkpoint(cfg->via1, cp);
    via_checkpoint(cfg->via2, cp); // no-op on a one-VIA machine
}
