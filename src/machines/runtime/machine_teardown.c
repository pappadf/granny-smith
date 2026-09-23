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
    appletalk_delete(cfg->scc);
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
        // Events sourced on the MACHINE rather than on a device are dropped
        // here, because this is their owner's destructor.
        //
        // Several chipset modules arm with `cfg` as the source -- amic's vbl
        // and sndout, the AWACS tick on both pdm and tnt, control's VBL,
        // se30's slot de-assert.  None of them can call
        // scheduler_forget_source themselves: the source they share is the
        // machine, so the first module to tear down would drop every other
        // module's events too, and the next arm from a still-live device
        // would trip scheduler_new_cpu_event's "event type not registered"
        // assert.  One call, at the point the machine itself goes away.
        //
        // Measured before adding it: a pm7100 teardown left one event queued
        // (amic), which the backstop below reported but nothing cleaned.
        // These are not the use-after-free half of F-27 -- `cfg` outlives
        // them -- but they are the half that made the backstop's count noisy,
        // and a count that is never zero cannot detect the dangerous kind.
        scheduler_forget_source(cfg->scheduler, cfg);

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
