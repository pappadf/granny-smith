// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_teardown.c
// The shared config_t-owned teardown chain -- see machine_teardown.h for the
// contract and for why the ordering is load-bearing.

#include "machine_teardown.h"

#include "appletalk.h"
#include "cpu.h"
#include "debug.h"
#include "memory.h"
#include "ppc.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "system_config.h"
#include "via.h"

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
    if (cfg->scheduler) {
        scheduler_delete(cfg->scheduler);
        cfg->scheduler = NULL;
    }
    // One of the two, never both: the 68k families build cfg->cpu, the
    // PowerPC families cfg->ppc.
    if (cfg->cpu) {
        cpu_delete(cfg->cpu);
        cfg->cpu = NULL;
    }
    if (cfg->ppc) {
        ppc_delete(cfg->ppc);
        cfg->ppc = NULL;
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
