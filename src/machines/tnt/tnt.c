// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// tnt.c
// The TNT family substrate (Power Macintosh 7500/8500/9500) — the second
// PowerPC family and the first PCI machine.  See tnt.h.
//
// Memory model (Apple, "Power Macintosh 7500 and 8500 Computers"
// Developer Note, 1995; the shipping ROM's Open Firmware device tree):
//   $00000000-RAM top   main DRAM, contiguous from 0 (Hammerhead-banked
//                       on hardware; Open Firmware sizes it and the tree
//                       is authoritative afterwards)
//   $80000000-$8FFFFFFF Bandit 1 PCI memory space — empty: recoverable
//                       transfer error (bandit.c)
//   $90000000-$9FFFFFFF Chaos/VCI memory space — likewise
//   $F0000000-$F1FFFFFF Chaos bridge + display-bus device space
//   $F2000000-$F2FFFFFF Bandit 1 bridge (config ports at +$800000/+$C00000)
//   $F3000000-$F3FFFFFF Bandit 1 PCI I/O window — Grand Central decodes
//                       the 128 KB at its base (grand_central.c)
//   $F4000000-$F5FFFFFF Bandit 2 (8500/9500 only)
//   $F8000000           Hammerhead register window (hammerhead.c)
//   $FFC00000-$FFFFFFFF the 4 MB ROM (601/604 reset vector $FFF00100 =
//                       image + $300100, the NanoKernel reset entry)
//
// Everything else is decoded by nobody and reads zero; the boot path is
// not expected to touch it (Open Firmware probes only what its drivers
// know, under fault catchers that the claimed windows provide).

#include "tnt.h"

#include "cuda.h" // the shared behavioral Cuda model (machines/av/)

#include "adb.h"
#include "debug.h"
#include "image.h"
#include "log.h"
#include "mac_host_io.h"
#include "ppc.h"
#include "rtc.h"
#include "scheduler.h"
#include "via.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("tnt");

// ============================================================
// Page-table helpers (the pdm_fill_page shape, kept local so the TNT
// family does not pull 68K-family headers)
// ============================================================

void tnt_fill_page(uint32_t page_index, uint8_t *host_ptr, bool writable) {
    if (page_index >= (uint32_t)g_page_count)
        return;
    g_page_table[page_index].host_base = host_ptr;
    g_page_table[page_index].dev = NULL;
    g_page_table[page_index].dev_context = NULL;
    g_page_table[page_index].writable = writable;
    uint32_t guest_base = page_index << PAGE_SHIFT;
    uintptr_t adjusted = (uintptr_t)host_ptr - guest_base;
    // Supervisor arrays hold the eager physical identity view; the USER
    // arrays belong to the PPC MMU front end (logical fills, ppc_mmu.c)
    // and are only ever cleared here.
    if (g_supervisor_read)
        g_supervisor_read[page_index] = adjusted;
    if (g_supervisor_write)
        g_supervisor_write[page_index] = writable ? adjusted : 0;
    if (g_user_read)
        g_user_read[page_index] = 0;
    if (g_user_write)
        g_user_write[page_index] = 0;
}

void tnt_clear_page(uint32_t page_index) {
    if (page_index >= (uint32_t)g_page_count)
        return;
    g_page_table[page_index].host_base = NULL;
    g_page_table[page_index].dev = NULL;
    g_page_table[page_index].dev_context = NULL;
    g_page_table[page_index].writable = false;
    if (g_supervisor_read)
        g_supervisor_read[page_index] = 0;
    if (g_supervisor_write)
        g_supervisor_write[page_index] = 0;
    if (g_user_read)
        g_user_read[page_index] = 0;
    if (g_user_write)
        g_user_write[page_index] = 0;
}

// ============================================================
// Grand Central island interface ($F3000000, 128 KB)
// ============================================================
// The island mixes byte-wide cells ($10/$200 centres) with 32-bit
// little-endian registers; grand_central.c dispatches by block.  16-bit
// access is not a natural size for anything in the chip — decompose into
// bytes, big-endian, matching what the bus would deliver.

static uint8_t gc_read8(void *ctx, uint32_t offset) {
    return tnt_gc_read8((config_t *)ctx, offset);
}

static void gc_write8(void *ctx, uint32_t offset, uint8_t value) {
    tnt_gc_write8((config_t *)ctx, offset, value);
}

static uint16_t gc_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((gc_read8(ctx, offset) << 8) | gc_read8(ctx, offset + 1));
}

static void gc_write16(void *ctx, uint32_t offset, uint16_t value) {
    gc_write8(ctx, offset, (uint8_t)(value >> 8));
    gc_write8(ctx, offset + 1, (uint8_t)value);
}

static uint32_t gc_read32(void *ctx, uint32_t offset) {
    return tnt_gc_read32((config_t *)ctx, offset);
}

static void gc_write32(void *ctx, uint32_t offset, uint32_t value) {
    tnt_gc_write32((config_t *)ctx, offset, value);
}

// ============================================================
// Hammerhead window interface ($F8000000)
// ============================================================
// Byte-wide model with big-endian decomposition for wider access — the
// one block on the machine that is NOT little-endian (processor bus).

static uint8_t hh_read8(void *ctx, uint32_t offset) {
    return tnt_hh_read((config_t *)ctx, offset);
}

static void hh_write8(void *ctx, uint32_t offset, uint8_t value) {
    tnt_hh_write((config_t *)ctx, offset, value);
}

static uint16_t hh_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((hh_read8(ctx, offset) << 8) | hh_read8(ctx, offset + 1));
}

static uint32_t hh_read32(void *ctx, uint32_t offset) {
    return ((uint32_t)hh_read16(ctx, offset) << 16) | hh_read16(ctx, offset + 2);
}

static void hh_write16(void *ctx, uint32_t offset, uint16_t value) {
    hh_write8(ctx, offset, (uint8_t)(value >> 8));
    hh_write8(ctx, offset + 1, (uint8_t)value);
}

static void hh_write32(void *ctx, uint32_t offset, uint32_t value) {
    hh_write16(ctx, offset, (uint16_t)(value >> 16));
    hh_write16(ctx, offset + 2, (uint16_t)value);
}

// ============================================================
// Memory layout
// ============================================================

static void tnt_memory_layout(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);

    // RAM: contiguous at 0 (the profile's size; Open Firmware discovers
    // it and publishes /memory's reg — the tree is the contract).
    uint8_t *ram = ram_native_pointer(cfg->mem_map, 0);
    for (uint32_t p = 0; p < (cfg->ram_size >> PAGE_SHIFT); p++)
        tnt_fill_page(p, ram + (p << PAGE_SHIFT), true);

    // ROM: 4 MB at $FFC00000 (direct read-only pages).
    uint8_t *rom = ram_native_pointer(cfg->mem_map, cfg->ram_size);
    for (uint32_t p = 0; p < (cfg->machine->rom_size >> PAGE_SHIFT); p++)
        tnt_fill_page((TNT_ROM_BASE >> PAGE_SHIFT) + p, rom + (p << PAGE_SHIFT), false);

    // Grand Central: the 128 KB island.
    st->gc_interface.read_uint8 = gc_read8;
    st->gc_interface.read_uint16 = gc_read16;
    st->gc_interface.read_uint32 = gc_read32;
    st->gc_interface.write_uint8 = gc_write8;
    st->gc_interface.write_uint16 = gc_write16;
    st->gc_interface.write_uint32 = gc_write32;
    memory_map_add(cfg->mem_map, TNT_GC_BASE, 0x00020000u, "Grand Central", &st->gc_interface, cfg);

    // Hammerhead: the register window (page granularity is ours; the file
    // answers $000..$7FF and logs above it).
    st->hh_interface.read_uint8 = hh_read8;
    st->hh_interface.read_uint16 = hh_read16;
    st->hh_interface.read_uint32 = hh_read32;
    st->hh_interface.write_uint8 = hh_write8;
    st->hh_interface.write_uint16 = hh_write16;
    st->hh_interface.write_uint32 = hh_write32;
    memory_map_add(cfg->mem_map, TNT_HH_BASE, MEM_PAGE_SIZE, "Hammerhead", &st->hh_interface, cfg);

    // The bridges: config ports + the empty PCI memory fault windows.
    tnt_bandit_init(cfg);
}

// ============================================================
// VIA1 callbacks — Cuda transport (the PDM/AV pattern, third instance)
// ============================================================

static void tnt_via1_output(void *context, uint8_t port, uint8_t value) {
    config_t *cfg = (config_t *)context;
    tnt_state_t *st = tnt_st(cfg);
    // Port B carries the Cuda handshake (PB3 TREQ in, PB4 BYTEACK out,
    // PB5 TIP out — the classic Cuda bit positions).
    if (port == 1 && st && st->cuda)
        av_cuda_via1_pb_input(st->cuda, value);
}

static void tnt_via1_shift_out(void *context, uint8_t byte) {
    config_t *cfg = (config_t *)context;
    tnt_state_t *st = tnt_st(cfg);
    if (st && st->cuda)
        av_cuda_via1_shift_input(st->cuda, byte);
}

// VIA1 aggregate IRQ -> Grand Central interrupt 18 (level-sensitive; the
// NanoKernel classifies it to 68k IPL 1).
static void tnt_via1_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    if (tnt_st(cfg))
        tnt_gc_set_source(cfg, TNT_INT_VIA1, active);
}

// ============================================================
// Substrate lifecycle
// ============================================================

static void tnt_init(config_t *cfg, checkpoint_t *cp) {
    tnt_state_t *st = calloc(1, sizeof(*st));
    assert(st != NULL);
    cfg->machine_context = st;

    // Core: memory map, the 601/604 per profile, the scheduler on the PPC
    // seam.  CPI 1.0 — the same determinism-and-measurement rationale as
    // PDM; whether any TNT guest code times itself against the TB and
    // cares is a ladder observable.
    cfg->mem_map = memory_map_init(cfg->machine->address_bits, cfg->ram_size, cfg->machine->rom_size, cp);
    // No 68k MMU owns this machine's page table; host-backed regions that
    // core code registers on the bus map are filled through our filler.
    g_mem_host_fill = tnt_fill_page;
    cfg->ppc = ppc_init(cp, cfg->machine->cpu_model);
    assert(cfg->ppc != NULL);
    sched_cpu_if_t cpu_if = ppc_sched_if(cfg->ppc);
    cfg->scheduler = scheduler_init(&cpu_if, cp);
    scheduler_set_frequency(cfg->scheduler, cfg->machine->freq);
    scheduler_set_cpi(cfg->scheduler, 1);
    // Time: the 601's RTC input keeps the PDM 7.8336 MHz assumption until
    // ladder rung T2 proves otherwise; the 604's timebase/DEC tick at a
    // quarter of the bus clock (Motorola, MPC604UM/AD, §1.3.2.2).
    uint32_t tick_hz = (cfg->machine->cpu_model == CPU_MODEL_PPC601) ? 7833600u : tnt_board(cfg)->bus_hz / 4u;
    ppc_bind_time(cfg->ppc, cfg->scheduler, cfg->machine->freq, tick_hz);

    cfg->rtc = rtc_init(cfg->scheduler, cp, true);

    // VIA1: one real 6522 behind the Grand Central decode, byte-wide on
    // $200 centres.  Timer clock: 783.36 kHz is the classic rate and the
    // starting assumption — the actual TNT VIA input clock is pinned at
    // the ladder's tick-rate rung (T8).
    uint8_t via_ff = via_freq_factor_for_clock(cfg->machine->freq);
    cfg->via1 =
        via_init(NULL, cfg->scheduler, via_ff, "via1", tnt_via1_output, tnt_via1_shift_out, tnt_via1_irq, cfg, cp);
    via_set_exact_clock(cfg->via1, cfg->machine->freq);

    // VIA1 idle input levels: PB3 is Cuda TREQ (active-low, idle high);
    // CA1 and the Cuda CB1/CB2 lines idle high.
    via_input(cfg->via1, 1, 3, 1);
    via_input_c(cfg->via1, 0, 0, 1);
    via_input_c(cfg->via1, 1, 0, 1);
    via_input_c(cfg->via1, 1, 1, 1);

    // ADB device state, serviced through Cuda packets (the AV pattern).
    cfg->adb = adb_init(NULL, cfg->scheduler, cp);

    // The behavioral Cuda (firmware 2.37 — the same 341S0788 part as the
    // AV and PDM machines) on the VIA1 shift register + PB3/4/5.  The
    // Mode3Clock tick is on, as on PDM: the guest clock lives behind
    // Cuda RdTime/PRAM here too and needs the real seed.
    st->cuda = av_cuda_init(cfg->via1, cfg->rtc, cfg->adb, cfg->scheduler, cp, /*mode3_clock=*/true);
    assert(st->cuda != NULL);

    // Board state + memory map.
    tnt_hh_init(cfg);
    tnt_gc_init(cfg);
    tnt_memory_layout(cfg);

    // Substrate-private checkpoint tail: register files + NVRAM are plain
    // data; the CPU line is recomputed below.
    if (cp) {
        system_read_checkpoint_data(cp, &st->hh, sizeof(st->hh));
        system_read_checkpoint_data(cp, &st->gc, sizeof(st->gc));
        for (int i = 0; i < st->bridge_count; i++) {
            system_read_checkpoint_data(cp, &st->bridge[i].cfg_addr, sizeof(st->bridge[i].cfg_addr));
            system_read_checkpoint_data(cp, &st->bridge[i].mode_select, sizeof(st->bridge[i].mode_select));
        }
        via_redrive_outputs(cfg->via1);
        tnt_gc_recompute(cfg);
    }

    // Finish: debugger + scheduler start.
    cfg->debugger = debug_init();
    scheduler_start(cfg->scheduler);
}

static void tnt_reset(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    // Power-on reset: the CPU back to $FFF00100, chipset registers to
    // their power-on state.  NVRAM survives — it is non-volatile, and
    // POST's log plus the Open Firmware environment must persist across
    // restarts (warm-restart semantics proper are observed at the ladder).
    ppc_reset(cfg->ppc);
    tnt_hh_init(cfg);
    tnt_gc_init(cfg);
    for (int i = 0; i < st->bridge_count; i++) {
        st->bridge[i].cfg_addr = 0;
        st->bridge[i].mode_select = 0;
    }
    tnt_gc_recompute(cfg);
}

static void tnt_teardown(config_t *cfg) {
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);
    tnt_state_t *st = tnt_st(cfg);
    if (st && st->cuda) {
        av_cuda_delete(st->cuda);
        st->cuda = NULL;
    }
    if (cfg->adb) {
        adb_delete(cfg->adb);
        cfg->adb = NULL;
    }
    if (cfg->via1) {
        via_delete(cfg->via1);
        cfg->via1 = NULL;
    }
    if (cfg->rtc) {
        rtc_delete(cfg->rtc);
        cfg->rtc = NULL;
    }
    if (cfg->scheduler) {
        scheduler_delete(cfg->scheduler);
        cfg->scheduler = NULL;
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
    if (st) {
        free(st);
        cfg->machine_context = NULL;
    }
}

static void tnt_checkpoint_save(config_t *cfg, checkpoint_t *cp) {
    tnt_state_t *st = tnt_st(cfg);
    // Same relative order as the tnt_init construction sequence (the
    // checkpoint stream is positional).
    memory_map_checkpoint(cfg->mem_map, cp);
    ppc_checkpoint(cfg->ppc, cp);
    scheduler_checkpoint(cfg->scheduler, cp);
    rtc_checkpoint(cfg->rtc, cp);
    via_checkpoint(cfg->via1, cp);
    adb_checkpoint(cfg->adb, cp);
    av_cuda_checkpoint(st->cuda, cp);
    // Substrate-private tail (mirrored by the restore block in tnt_init).
    system_write_checkpoint_data(cp, &st->hh, sizeof(st->hh));
    system_write_checkpoint_data(cp, &st->gc, sizeof(st->gc));
    for (int i = 0; i < st->bridge_count; i++) {
        system_write_checkpoint_data(cp, &st->bridge[i].cfg_addr, sizeof(st->bridge[i].cfg_addr));
        system_write_checkpoint_data(cp, &st->bridge[i].mode_select, sizeof(st->bridge[i].mode_select));
    }
}

// Host-frame tick: media insertion polling only.  The guest's own 60 Hz
// tick comes from VIA1 timer 1, not from an external VBL line, and video
// VBL (Grand Central interrupt 30) arrives with the Control model in a
// later phase.
static void tnt_trigger_vbl(config_t *cfg) {
    image_tick_all(cfg);
}

// Chipset IRQ spine.  Nothing routes through it: every on-board source is
// a Grand Central interrupt number (tnt_gc_set_source).
static void tnt_update_ipl(config_t *cfg, int source, bool active) {
    (void)cfg;
    LOG(1, "update_ipl source=%d active=%d (TNT sources drive Grand Central directly)", source, active);
}

// Floppy: the internal SuperDrive arrives with the SWIM3/DBDMA datapath
// (Phase F); until then the bay refuses media and reports itself occupied.
static int tnt_fd_insert(config_t *cfg, int drive, struct image *disk) {
    (void)cfg;
    (void)drive;
    (void)disk;
    return -1;
}

static bool tnt_fd_present(config_t *cfg, int drive) {
    (void)cfg;
    (void)drive;
    return true; // no usable bay yet: report occupied so nothing targets it
}

const machine_substrate_t tnt_substrate = {
    .init = tnt_init,
    .reset = tnt_reset,
    .teardown = tnt_teardown,
    .checkpoint_save = tnt_checkpoint_save,
    .update_ipl = tnt_update_ipl,
    .trigger_vbl = tnt_trigger_vbl,
    .fd_insert = tnt_fd_insert,
    .fd_present = tnt_fd_present,
    .input_key = mac_input_key,
    .input_mouse_move = mac_input_mouse_move,
    .input_mouse_button = mac_input_mouse_button,
    .media_detach = system_media_detach_std,
    .media_attach = system_media_attach_std,
};
