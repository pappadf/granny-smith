// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mac030_glue.c
// Shared GLUE-family lifecycle leaves — see mac030_glue.h.

#include "mac030_glue.h"

#include "adb.h"
#include "asc.h"
#include "cpu.h"
#include "debug.h"
#include "floppy.h"
#include "log.h"
#include "memory.h"
#include "mmu.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "via.h"

LOG_USE_CATEGORY_NAME("setup");

// Set/clear an IRQ source bit and re-derive the CPU IPL.  Verbatim from the
// (identical) se30_update_ipl / iicx_update_ipl.
void mac030_glue_update_ipl(config_t *cfg, int source, bool active) {
    int old_irq = cfg->irq;
    if (active)
        cfg->irq |= source;
    else
        cfg->irq &= ~source;

    // Highest active source wins.
    uint32_t new_ipl;
    if (cfg->irq & MAC030_GLUE_IRQ_NMI)
        new_ipl = 7;
    else if (cfg->irq & MAC030_GLUE_IRQ_SCC)
        new_ipl = 4;
    else if (cfg->irq & MAC030_GLUE_IRQ_VIA2)
        new_ipl = 2;
    else if (cfg->irq & MAC030_GLUE_IRQ_VIA1)
        new_ipl = 1;
    else
        new_ipl = 0;

    cpu_set_ipl(cfg->cpu, new_ipl);
    LOG(2, "mac030_glue_update_ipl: source=%d active=%d irq:%d->%d ipl->%d", source, active ? 1 : 0, old_irq, cfg->irq,
        new_ipl);
    cpu_reschedule();
}

// Family-shared teardown delete-chain.  Order matches the (identical)
// per-machine teardowns; NuBus cards are already gone (system_destroy calls
// nubus_delete before machine teardown — §6.2 ownership invariant).
void mac030_glue_teardown(config_t *cfg, struct adb *adb, struct asc *asc, struct floppy *floppy,
                          struct mmu_state *mmu) {
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);

    // Machine-owned devices (in the machine's private state).
    if (mmu)
        mmu_delete(mmu); // also clears g_mmu if it matches
    if (floppy) {
        floppy_delete(floppy);
        cfg->floppy = NULL;
    }
    if (asc)
        asc_delete(asc);
    if (adb) {
        adb_delete(adb);
        cfg->adb = NULL;
    }

    // config_t-owned devices.
    if (cfg->scsi) {
        scsi_delete(cfg->scsi);
        cfg->scsi = NULL;
    }
    if (cfg->via2) {
        via_delete(cfg->via2);
        cfg->via2 = NULL;
    }
    if (cfg->via1) {
        via_delete(cfg->via1);
        cfg->via1 = NULL;
    }
    if (cfg->scc) {
        scc_delete(cfg->scc);
        cfg->scc = NULL;
    }
    if (cfg->rtc) {
        rtc_delete(cfg->rtc);
        cfg->rtc = NULL;
    }
    if (cfg->scheduler) {
        scheduler_delete(cfg->scheduler);
        cfg->scheduler = NULL;
    }
    if (cfg->cpu) {
        cpu_delete(cfg->cpu);
        cfg->cpu = NULL;
    }
    if (cfg->mem_map) {
        memory_map_delete(cfg->mem_map);
        cfg->mem_map = NULL;
    }
    if (cfg->debugger) {
        debug_cleanup(cfg->debugger);
        cfg->debugger = NULL;
    }
}
