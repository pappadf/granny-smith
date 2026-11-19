// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_build.c
// The PowerPC families' shared construction prefix -- see ppc_build.h.

#include "ppc_build.h"

#include "cuda.h" // the shared behavioral Cuda model (machines/av/)

#include "adb.h"
#include "appletalk.h"
#include "log.h"
#include "machine_checkpoint.h"
#include "memory.h"
#include "ppc.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "system_internal.h"
#include "via.h"

LOG_USE_CATEGORY_NAME("board");

// ============================================================================
// Constants and Macros
// ============================================================================

#define PPC_SCC_PCLK_HZ 15667200u // the ESCC's PCLK on every PowerPC board

// ============================================================================
// Operations
// ============================================================================

int ppc_build_core(config_t *cfg, checkpoint_t *cp, const ppc_core_opts_t *o) {
    machine_part_begin(cfg, cp, "memory");
    cfg->memory_map =
        memory_map_init(cfg->machine->address_bits, cfg->ram_size, cfg->machine->rom_size, MEMORY_BUS_ERR_NONE,
                        &cfg->build_opts.rom, cp); // no bus-error watchdog: unanswered floats to $FF
    machine_part(cfg, cp, "memory", part_save_memory, cfg->memory_map);
    // No 68k MMU owns this machine's page table: host-backed regions are
    // filled through the family's own page filler
    memory_map_set_host_fill(cfg->memory_map, o->fill_page);
    machine_part_begin(cfg, cp, "cpu");
    cfg->ppc = ppc_init(cp, cfg->machine->cpu_model);
    if (cfg->ppc) {
        memory_cpu_hooks_t hooks = ppc_memory_hooks(cfg->ppc);
        memory_map_set_cpu_hooks(cfg->memory_map, &hooks);
    }
    if (!cfg->ppc) {
        LOG(0, "Error: out of memory constructing the PowerPC core");
        return -1;
    }
    machine_part(cfg, cp, "cpu", part_save_ppc, cfg->ppc);
    // The board's straps rewrite what a restore just loaded with the same values
    if (o->set_identity)
        ppc_set_identity(cfg->ppc, o->pvr, o->hid1);
    sched_cpu_if_t cpu_if = ppc_sched_if(cfg->ppc);
    machine_part_begin(cfg, cp, "scheduler");
    cfg->scheduler = scheduler_init(&cpu_if, cp);
    machine_part(cfg, cp, "scheduler", part_save_scheduler, cfg->scheduler);
    scheduler_set_frequency(cfg->scheduler, cfg->machine->freq);
    scheduler_set_cpi(cfg->scheduler, o->cpi);
    ppc_bind_time(cfg->ppc, cfg->scheduler, cfg->machine->freq, o->tb_hz);
    return 0;
}

void ppc_build_lowspeed(config_t *cfg, checkpoint_t *cp, const ppc_lowspeed_opts_t *o) {
    machine_part_begin(cfg, cp, "rtc");
    cfg->rtc = rtc_init(cfg->scheduler, cp, true, cfg->machine->pram);
    machine_part(cfg, cp, "rtc", part_save_rtc, cfg->rtc);

    machine_part_begin(cfg, cp, "scc");
    cfg->scc = scc_init(NULL, cfg->scheduler, o->scc_irq, cfg, cp);
    machine_part(cfg, cp, "scc", part_save_scc, cfg->scc);
    scc_set_clocks(cfg->scc, PPC_SCC_PCLK_HZ, o->scc_rtxc_hz);

    // The AppleTalk connection rides the SCC's LocalTalk channel
    machine_part_begin(cfg, cp, "appletalk");
    cfg->atalk = atalk_conn_new(appletalk_network(), cfg->scheduler, cfg->scc, cp);
    machine_part(cfg, cp, "appletalk", part_save_atalk, cfg->atalk);
    machine_part_imagewriter(cfg, cp, false);

    // VIA1: timers at the classic 783.36 kHz; no PowerPC CPU clock divides
    // integrally by that, so via_set_exact_clock installs the reduced
    // rational and guest-measured timer rates stay exact
    uint8_t via_ff = via_freq_factor_for_clock(cfg->machine->freq);
    machine_part_begin(cfg, cp, "via1");
    cfg->via1 = via_init(NULL, cfg->scheduler, via_ff, "via1", o->via1_output, o->via1_shift_out, o->via1_irq, cfg, cp);
    machine_part(cfg, cp, "via1", part_save_via, cfg->via1);
    via_set_exact_clock(cfg->via1, cfg->machine->freq);

    // VIA1 idle input levels: PB3 is Cuda TREQ (active-low, idle high);
    // CA1 and the Cuda CB1/CB2 lines idle high
    via_input(cfg->via1, 1, 3, 1);
    via_input_c(cfg->via1, 0, 0, 1);
    via_input_c(cfg->via1, 1, 0, 1);
    via_input_c(cfg->via1, 1, 1, 1);
}

struct av_cuda *ppc_build_cuda(config_t *cfg, checkpoint_t *cp) {
    // ADB device state, serviced through Cuda packets
    machine_part_begin(cfg, cp, "adb");
    cfg->adb = adb_init(NULL, cfg->scheduler, cp);
    machine_part(cfg, cp, "adb", part_save_adb, cfg->adb);

    // The Mode3Clock tick is on: the guest clock lives behind Cuda RdTime/PRAM
    machine_part_begin(cfg, cp, "cuda");
    av_cuda_t *cuda = av_cuda_init(cfg->via1, cfg->rtc, cfg->adb, cfg->scheduler, cp, /*mode3_clock=*/true);
    if (!cuda) {
        LOG(0, "Error: out of memory constructing the Cuda");
        return NULL;
    }
    machine_part(cfg, cp, "cuda", part_save_cuda, cuda);
    return cuda;
}
