// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// m68k_board.c
// The board runtime every 68k II/Quadra family shares — see m68k_board.h.

#include "m68k_board.h"

#include "appletalk.h"

#include "checkpoint.h"
#include "cpu.h"
#include "cpu_internal.h" // cpu->mmu, the CPU-owned 040 register file
#include "debug.h"
#include "log.h"
#include "machine_checkpoint.h"
#include "memory.h"
#include "mmu.h"
#include "mmu040.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"

LOG_USE_CATEGORY_NAME("system");

// Create + attach the 68030 PMMU over a board's ROM window — see header.
struct mmu_state *m68k_board_build_mmu(config_t *cfg, uint32_t rom_base, uint32_t rom_end) {
    uint8_t *ram_base = ram_native_pointer(cfg->memory_map, 0);
    uint32_t ram_size = cfg->ram_size;
    uint8_t *rom_data = ram_native_pointer(cfg->memory_map, ram_size);
    uint32_t rom_size = cfg->machine->rom_size;
    mmu_state_t *mmu = mmu_init(ram_base, ram_size, cfg->machine->ram_max, rom_data, rom_size, rom_base, rom_end);
    if (!mmu) {
        LOG(0, "Error: out of memory constructing the PMMU");
        return NULL; // m68k_board_build_mmu returns the MMU, not a status
    }
    // Set both, always together: the fault hook runs on the map's PMMU and
    // the 68030 bus-error path reads cpu->mmu (asserted equal there).
    memory_map_set_pmmu(cfg->memory_map, mmu);
    cpu_attach_mmu(cfg->cpu, mmu);
    return mmu;
}

struct mmu_state *m68k_board_build_bus_mmu040(config_t *cfg, uint32_t ram_aperture, uint32_t rom_base,
                                              uint32_t rom_end) {
    uint32_t ram_size = cfg->ram_size;
    uint8_t *ram_base = ram_native_pointer(cfg->memory_map, 0);
    uint8_t *rom_data = ram_native_pointer(cfg->memory_map, ram_size); // the ROM follows RAM
    mmu_state_t *mmu = mmu_init(ram_base, ram_size, ram_aperture, rom_data, cfg->machine->rom_size, rom_base, rom_end);
    if (!mmu) {
        LOG(0, "Error: out of memory constructing the 040 bus MMU");
        return NULL;
    }
    memory_map_set_pmmu(cfg->memory_map, mmu);
    // The cpu.mmu debug object is bound by cpu_init itself (the 040-shaped
    // mmu040_class in cpu.c); this only points the walker at the bus.
    mmu_attach_mmu040(mmu, (mmu040_state_t *)cfg->cpu->mmu);
    return mmu;
}

// Build the low-speed spine every 68k family shares: the RTC, the SCC at the
// Mac's clocks, and the machine's connection to the AppleTalk network on its
// LocalTalk channel.
//
// Each of the three is a checkpoint part, registered as it is built
// (machine_parts.h).
//
// `scc_irq` is the only genuine per-family variation at this level; the VIAs
// below it differ enough (one or two, different hooks, different IRQ sinks)
// that they stay with each family.
void m68k_board_build_lowspeed(config_t *cfg, checkpoint_t *cp, void (*scc_irq)(void *, bool)) {
    machine_part_begin(cfg, cp, "rtc");
    cfg->rtc = rtc_init(cfg->scheduler, cp, true, cfg->machine->pram);
    machine_part(cfg, cp, "rtc", part_save_rtc, cfg->rtc);
    machine_part_begin(cfg, cp, "scc");
    cfg->scc = scc_init(NULL, cfg->scheduler, scc_irq ? scc_irq : m68k_board_scc_irq, cfg, cp);
    machine_part(cfg, cp, "scc", part_save_scc, cfg->scc);
    // 7.8336 MHz PCLK (C8M) / 3.6864 MHz RTxC -- the same pair on every 68k Mac.
    scc_set_clocks(cfg->scc, 7833600, 3686400);
    machine_part_begin(cfg, cp, "appletalk");
    cfg->atalk = atalk_conn_new(appletalk_network(), cfg->scheduler, cfg->scc, cp);
    machine_part(cfg, cp, "appletalk", part_save_atalk, cfg->atalk);
    machine_part_imagewriter(cfg, cp, false);
}

// Finish init: debugger, scheduler start, cold-boot IRQ/IPL reset.
void m68k_board_finish(config_t *cfg, checkpoint_t *cp, const board_io_t *io) {
    // Every family's I/O table is checked here, once the machine is fully
    // built.  This is a parameter rather than a lookup so a new family cannot
    // quietly skip it -- see board_io_validate.
    board_io_validate(io, cfg->machine->id);
    cfg->debugger = debug_init();
    machine_irq_cold_reset(cfg, cp);
}

// Build the shared II-family construction prefix.  Reads the CPU model from
// the profile (single source of truth), not a hardcoded constant.
void m68k_board_build_core(config_t *cfg, const struct m68k_board_desc *desc, checkpoint_t *cp) {
    // The board's NuBus bus-error window is part of the bus it builds.
    const memory_bus_err_window_t bus_err = {.lo = desc->bus_err_lo, .hi = desc->bus_err_hi};
    machine_part_begin(cfg, cp, "memory");
    cfg->memory_map = memory_map_init(cfg->machine->address_bits, cfg->ram_size, cfg->machine->rom_size, bus_err,
                                      &cfg->build_opts.rom, cp);
    machine_part(cfg, cp, "memory", part_save_memory, cfg->memory_map);
    machine_part_begin(cfg, cp, "cpu");
    cfg->cpu = cpu_init(cfg->machine->cpu_model, cp);
    machine_part(cfg, cp, "cpu", part_save_cpu, cfg->cpu);
    machine_part_begin(cfg, cp, "scheduler");
    sched_cpu_if_t cpu_if = cpu_sched_if(cfg->cpu); // the 68K main-CPU seam adapter
    cfg->scheduler = scheduler_init(&cpu_if, cp);
    machine_part(cfg, cp, "scheduler", part_save_scheduler, cfg->scheduler);
    scheduler_set_frequency(cfg->scheduler, cfg->machine->freq);
    // CPI by CPU, both from Apple's "Macintosh Family: MIPS Ratings" (Tech
    // Info Library, Oct 1996) read as MHz / MIPS, which folds in the memory
    // system's wait states as this one constant must.  The 68030 machines sit
    // at 4.0 (IIx/IIcx/SE/30 3.9 MIPS at 15.67 MHz, IIfx 10 MIPS at 40 MHz).
    // The 68040 sits at 1.14 (22 MIPS at 25 MHz, 35 at 40 MHz), and Apple's
    // 68040 description states 1.3 cycles per instruction; 1 is the nearest
    // integer.  The 68040 machines build on this core too, and used to
    // inherit the 68030's 4, which ran a Quadra at a quarter of its speed.
    scheduler_set_cpi(cfg->scheduler, cfg->machine->cpu_model == CPU_MODEL_68040 ? 1 : 4);
    g_uncached_read_cycles = 0; // a board that prices uncached reads sets it after this
}

void m68k_board_map_mirrored(uint32_t start_page, uint32_t window_pages, uint8_t *host, uint32_t size_pages,
                             m68k_board_fill_fn fill, bool writable) {
    if (size_pages == 0)
        return; // a bank smaller than one page decodes nothing
    for (uint32_t i = 0; i < window_pages && start_page + i < g_page_count; i++)
        fill(start_page + i, host + ((i % size_pages) << MEM_PAGE_SHIFT), writable);
}

// Hardware RESET: ROM overlay back on, shared devices reset.
// The GLUE/MDU half of the board's /RESET net: VIA1 goes back to power-on and
// pulls Overlay high, so the memory controller uses the ROM overlay map
// again (Guide p.256), plus the devices every board shares.
//
// The 68030 PMMU used to be cleared here.  It is INSIDE THE CPU and not on
// the net, so an external chip reset must not touch it; that moved to
// cpu_hardware_reset.  The `mmu` parameter is gone
// with it.
void m68k_board_bus_reset(config_t *cfg, m68k_board_overlay_t *overlay) {
    m68k_board_overlay_arm(overlay); // unconditionally: the pages may have changed since
    system_reset_common_devices(cfg);
}

// Shared IRQ callbacks — route a device's interrupt line to the CPU IPL.
void m68k_board_scc_irq(void *context, bool active) {
    m68k_board_update_ipl((config_t *)context, M68K_BOARD_IRQ_SCC, active);
}
void m68k_board_via1_irq(void *context, bool active) {
    m68k_board_update_ipl((config_t *)context, M68K_BOARD_IRQ_VIA1, active);
}
void m68k_board_via2_irq(void *context, bool active) {
    m68k_board_update_ipl((config_t *)context, M68K_BOARD_IRQ_VIA2, active);
}

// Set/clear an IRQ source bit and re-derive the CPU IPL.  The routing itself
// is the data-driven board_irq_routes table (m68k_board_io.c) run by the
// shared engine (machine_irq.h).
void m68k_board_update_ipl(config_t *cfg, int source, bool active) {
    int old_irq = cfg->rt.irq;
    int new_ipl = machine_irq_update(cfg, m68k_board_irq_routes(), source, active);
    LOG(2, "m68k_board_update_ipl: source=%d active=%d irq:%d->%d ipl->%d", source, active ? 1 : 0, old_irq,
        cfg->rt.irq, new_ipl);
}
