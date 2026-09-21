// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_teardown.c
// The shared config_t-owned teardown chain -- see machine_teardown.h for the
// contract and for why the ordering is load-bearing.

#include "machine_teardown.h"

#include "appletalk.h"
#include "cpu.h"
#include "debug.h"
#include "log.h"
#include "memory.h"
#include "ppc.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "system_config.h"
#include "via.h"

LOG_USE_CATEGORY_NAME("board");

void machine_teardown_config_devices(config_t *cfg) {
    if (!cfg)
        return;

    if (cfg->scsi) {
        scsi_delete(cfg->scsi);
        cfg->scsi = NULL;
    }
    // NULL on the single-VIA machines (MDU, OSS, AV, and both PowerPC
    // families) -- they simply pass through.
    if (cfg->via2) {
        via_delete(cfg->via2);
        cfg->via2 = NULL;
    }
    if (cfg->via1) {
        via_delete(cfg->via1);
        cfg->via1 = NULL;
    }
    // The AppleTalk stack is a client of the SCC's LocalTalk channel, so it
    // goes first -- it holds the scc pointer it was given at init.
    appletalk_delete();
    if (cfg->scc) {
        scc_delete(cfg->scc);
        cfg->scc = NULL;
    }
    if (cfg->rtc) {
        rtc_delete(cfg->rtc);
        cfg->rtc = NULL;
    }
    // One of the two, never both: the 68k families build cfg->cpu, the
    // PowerPC families cfg->ppc.  BOTH GO BEFORE THE SCHEDULER: ppc_delete
    // calls scheduler_forget_source for its decrementer event, so a scheduler
    // freed first would be dereferenced through a stale pointer.  Nothing in
    // scheduler_delete needs a live CPU -- it holds the sched_cpu_if_t by
    // value and does not run -- so this order is the safe one, and it is the
    // one the invariant wants: everything that can reference the scheduler is
    // gone before the scheduler is.
    if (cfg->cpu) {
        cpu_delete(cfg->cpu);
        cfg->cpu = NULL;
    }
    if (cfg->ppc) {
        ppc_delete(cfg->ppc);
        cfg->ppc = NULL;
    }
    if (cfg->scheduler) {
        // Backstop (proposal-scheduler-source-lifetime §4): by here every
        // destructor above should have dropped what it owned, so anything
        // still queued is a destructor that missed.  Say so rather than
        // sweeping it silently -- a silent sweep would make the per-device
        // rule untestable, which is the opposite of the point.
        int left = scheduler_pending_device_events(cfg->scheduler);
        if (left > 0)
            LOG(1,
                "teardown: %d scheduler event(s) still queued; a device destructor did not call "
                "scheduler_forget_source",
                left);
        scheduler_delete(cfg->scheduler);
        cfg->scheduler = NULL;
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
