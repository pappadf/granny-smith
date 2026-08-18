// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pdm.c
// The PDM family substrate (Power Macintosh 6100/7100/8100) — the first
// machines whose main CPU is the PowerPC 601 (config_t.cpu is NULL; the 68k
// world exists only as ROM bytes the 601 executes).  See pdm.h.
//
// Memory model (Developer Note Table 2-5, as exercised by the shipping
// ROM):
//   $00000000-$3FFFFFFF  DRAM bank windows, HMC-owned (hmc.c)
//   $40000000-$4FFFFFFF  the 4 MB ROM repeating every 4 MB (HWInit runs
//                        from the $40300000 alias, the OS from $40800000)
//   $50F00000-$50F4FFFF  the AMIC-decoded I/O island (amic.c + hmc port)
//   $5FFFF000            machine-ID page ($5FFFFFFC)
//   $60000000-$FEFFFFFF  undecoded: bus error → 601 machine check
//   $FF000000-$FFFFFFFF  ROM alias (601 reset vector fetches $FFF00100)

#include "pdm.h"

#include "cuda.h" // the shared behavioral Cuda model (machines/av/)

#include "adb.h"
#include "appletalk.h"
#include "debug.h"
#include "image.h"
#include "log.h"
#include "mac_host_io.h"
#include "ppc.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "scsi_53c96.h"
#include "via.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("pdm");

// ============================================================
// Page-table helpers (the mac030_fill_page shape, kept local so the PDM
// family does not pull 68K-family headers)
// ============================================================

void pdm_fill_page(uint32_t page_index, uint8_t *host_ptr, bool writable) {
    if (page_index >= (uint32_t)g_page_count)
        return;
    g_page_table[page_index].host_base = host_ptr;
    g_page_table[page_index].dev = NULL;
    g_page_table[page_index].dev_context = NULL;
    g_page_table[page_index].writable = writable;
    uint32_t guest_base = page_index << PAGE_SHIFT;
    uintptr_t adjusted = (uintptr_t)host_ptr - guest_base;
    // Supervisor arrays hold the eager physical identity view; the USER
    // arrays belong to the 601 MMU front end (logical fills, ppc_mmu.c)
    // and are only ever cleared here.  Callers that change the physical
    // map (HMC remap) additionally invalidate the MMU caches.
    if (g_supervisor_read)
        g_supervisor_read[page_index] = adjusted;
    if (g_supervisor_write)
        g_supervisor_write[page_index] = writable ? adjusted : 0;
    if (g_user_read)
        g_user_read[page_index] = 0;
    if (g_user_write)
        g_user_write[page_index] = 0;
}

void pdm_clear_page(uint32_t page_index) {
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
// I/O island dispatch ($50F00000-$50F4FFFF)
// ============================================================
// All AMIC registers are byte-wide; wider accesses decompose into bytes,
// big-endian (MSB at the lowest address) — the convention every PDM driver
// programs multi-byte quantities with.

static uint8_t pdm_io_read8(void *ctx, uint32_t offset) {
    config_t *cfg = (config_t *)ctx;
    if (offset >= 0x40000u)
        return pdm_hmc_read(cfg, offset - 0x40000u);
    return pdm_amic_read(cfg, offset);
}

static void pdm_io_write8(void *ctx, uint32_t offset, uint8_t value) {
    config_t *cfg = (config_t *)ctx;
    if (offset >= 0x40000u)
        pdm_hmc_write(cfg, offset - 0x40000u, value);
    else
        pdm_amic_write(cfg, offset, value);
}

static uint16_t pdm_io_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((pdm_io_read8(ctx, offset) << 8) | pdm_io_read8(ctx, offset + 1));
}

static uint32_t pdm_io_read32(void *ctx, uint32_t offset) {
    return ((uint32_t)pdm_io_read16(ctx, offset) << 16) | pdm_io_read16(ctx, offset + 2);
}

static void pdm_io_write16(void *ctx, uint32_t offset, uint16_t value) {
    pdm_io_write8(ctx, offset, (uint8_t)(value >> 8));
    pdm_io_write8(ctx, offset + 1, (uint8_t)value);
}

static void pdm_io_write32(void *ctx, uint32_t offset, uint32_t value) {
    pdm_io_write16(ctx, offset, (uint16_t)(value >> 16));
    pdm_io_write16(ctx, offset + 2, (uint16_t)value);
}

// Machine-ID page: byte reads deliver the ID, wider reads hide the
// signature (hmc.c); writes are ignored.
static uint16_t pdm_id_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((pdm_id_read8(ctx, offset) << 8) | pdm_id_read8(ctx, offset + 1));
}

static void pdm_id_write8(void *ctx, uint32_t offset, uint8_t value) {
    (void)ctx;
    LOG(3, "machine-ID write $%X = $%02X ignored", offset, value);
}

static void pdm_id_write16(void *ctx, uint32_t offset, uint16_t value) {
    pdm_id_write8(ctx, offset, (uint8_t)value);
}

static void pdm_id_write32(void *ctx, uint32_t offset, uint32_t value) {
    pdm_id_write8(ctx, offset, (uint8_t)value);
}

// ============================================================
// Memory layout
// ============================================================

// Map the 4 MB ROM image repeating across a window of direct read-only
// pages (offset = addr & $3FFFFF — the wrap HWInit's self-rebase, the
// $40800000 OS view, and the $FFF00100 reset fetch all rely on).
static void pdm_map_rom_window(config_t *cfg, uint32_t base, uint32_t window) {
    uint8_t *rom = ram_native_pointer(cfg->mem_map, cfg->ram_size);
    uint32_t rom_size = cfg->machine->rom_size;
    uint32_t first = base >> PAGE_SHIFT;
    uint32_t pages = window >> PAGE_SHIFT;
    for (uint32_t p = 0; p < pages; p++)
        pdm_fill_page(first + p, rom + ((p << PAGE_SHIFT) % rom_size), false);
}

static void pdm_memory_layout(config_t *cfg) {
    pdm_state_t *st = pdm_st(cfg);

    // ROM: the whole $4xxxxxxx window plus the top-of-memory alias.
    pdm_map_rom_window(cfg, 0x40000000u, 0x10000000u);
    pdm_map_rom_window(cfg, 0xFF000000u, 0x01000000u);

    // The AMIC/HMC I/O island.
    st->io_interface.read_uint8 = pdm_io_read8;
    st->io_interface.read_uint16 = pdm_io_read16;
    st->io_interface.read_uint32 = pdm_io_read32;
    st->io_interface.write_uint8 = pdm_io_write8;
    st->io_interface.write_uint16 = pdm_io_write16;
    st->io_interface.write_uint32 = pdm_io_write32;
    memory_map_add(cfg->mem_map, 0x50F00000u, 0x00050000u, "PDM I/O", &st->io_interface, cfg);

    // Machine-ID page.
    st->id_interface.read_uint8 = pdm_id_read8;
    st->id_interface.read_uint16 = pdm_id_read16;
    st->id_interface.read_uint32 = pdm_id_read32;
    st->id_interface.write_uint8 = pdm_id_write8;
    st->id_interface.write_uint16 = pdm_id_write16;
    st->id_interface.write_uint32 = pdm_id_write32;
    memory_map_add(cfg->mem_map, 0x5FFFF000u, 0x00001000u, "Machine ID", &st->id_interface, cfg);

    // Undecoded space: AMIC's bus error (40 us TEA modeled as immediate)
    // surfaces as a 601 machine check.  BART/NuBus space stays inside the
    // error range until the BART model lands (Phase H); the ROM alias at
    // $FF000000 sits above it.
    memory_set_bus_error_range(cfg->mem_map, 0x60000000u, 0xFEFFFFFFu);

    // DRAM bank windows per the HMC's power-on state.
    pdm_hmc_remap(cfg);
}

// ============================================================
// VIA1 (the AMIC pseudo-VIA) callbacks — Cuda transport
// ============================================================

static void pdm_via1_output(void *context, uint8_t port, uint8_t value) {
    config_t *cfg = (config_t *)context;
    pdm_state_t *st = pdm_st(cfg);
    // Port B carries the Cuda handshake (PB3 TREQ in, PB4 BYTEACK out,
    // PB5 TIP out — the classic Cuda bit positions).
    if (port == 1 && st && st->cuda)
        av_cuda_via1_pb_input(st->cuda, value);
}

static void pdm_via1_shift_out(void *context, uint8_t byte) {
    config_t *cfg = (config_t *)context;
    pdm_state_t *st = pdm_st(cfg);
    if (st && st->cuda)
        av_cuda_via1_shift_input(st->cuda, byte);
}

// VIA1 aggregate IRQ → ICR bit 0.
static void pdm_via1_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    if (pdm_st(cfg))
        pdm_amic_set_source(cfg, PDM_ICR_VIA1, active);
}

// 53C9x INT pins → the pseudo-VIA2 device bank (level-sensitive).
static void pdm_scsi96a_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    if (pdm_st(cfg))
        pdm_amic_set_scsi_irq(cfg, 0, active);
}

static void pdm_scsi96b_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    if (pdm_st(cfg))
        pdm_amic_set_scsi_irq(cfg, 1, active);
}

// SCC chip INT (one line for both channels) → AMIC ICR source bit 2, 68k
// level 4 (interrupt-map.md §6.1); channel discrimination is the guest's
// job via RR2B/RR3.
static void pdm_scc_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    if (pdm_st(cfg))
        pdm_amic_set_source(cfg, PDM_ICR_SCC, active);
}

// ============================================================
// Substrate lifecycle
// ============================================================

static void pdm_init(config_t *cfg, checkpoint_t *cp) {
    pdm_state_t *st = calloc(1, sizeof(*st));
    assert(st != NULL);
    cfg->machine_context = st;

    // Core: memory map, the 601, the scheduler on the PPC seam.  CPI is
    // 1.0 — the 601 is near-1-CPI on HWInit's measurement loop, and 1.0
    // makes the measured clock land exactly on the snap-table value
    // (proposal §5.2).
    cfg->mem_map = memory_map_init(cfg->machine->address_bits, cfg->ram_size, cfg->machine->rom_size, cp);
    cfg->ppc = ppc_init(cp);
    assert(cfg->ppc != NULL);
    sched_cpu_if_t cpu_if = ppc_sched_if(cfg->ppc);
    cfg->scheduler = scheduler_init(&cpu_if, cp);
    scheduler_set_frequency(cfg->scheduler, cfg->machine->freq);
    scheduler_set_cpi(cfg->scheduler, 1);
    ppc_bind_time(cfg->ppc, cfg->scheduler, cfg->machine->freq);

    cfg->rtc = rtc_init(cfg->scheduler, cp, true);

    // The ESCC cell in Curio behind the AMIC island decode (escc-serial.md
    // §2: single base $50F04000, +0 bCtl / +2 aCtl / +4 bData / +6 aData;
    // PCLK 15.6672 MHz, RTxC 3.672 MHz synthesized by AMIC).
    cfg->scc = scc_init(NULL, cfg->scheduler, pdm_scc_irq, cfg, cp);
    scc_set_clocks(cfg->scc, 15667200, 3672000);

    // AppleTalk rides the SCC's LocalTalk channel, so it is built as soon as
    // the SCC exists — and, because the checkpoint stream is positional, in
    // the same relative place the save writes it (right after scc_checkpoint).
    appletalk_init(cfg->scheduler, cfg->scc, cp);

    // The AMIC pseudo-VIA1 is a real 6522 core instance behind the island
    // decode.  Its timers run at 783.36 kHz on every model, and no PDM CPU
    // clock divides integrally by that — the rounded divisor is display-only
    // and via_set_exact_clock installs the reduced 783360/freq rational so
    // guest-measured timer rates are exactly φ2-equivalent (the dossier's
    // hard constraint, owed by rung L17).
    uint8_t via_ff = via_freq_factor_for_clock(cfg->machine->freq);
    cfg->via1 =
        via_init(NULL, cfg->scheduler, via_ff, "via1", pdm_via1_output, pdm_via1_shift_out, pdm_via1_irq, cfg, cp);
    via_set_exact_clock(cfg->via1, cfg->machine->freq);

    // VIA1 idle input levels: PB3 is Cuda TREQ (active-low, idle high);
    // CA1 (tick) and the Cuda CB1/CB2 lines idle high.
    via_input(cfg->via1, 1, 3, 1);
    via_input_c(cfg->via1, 0, 0, 1);
    via_input_c(cfg->via1, 1, 0, 1);
    via_input_c(cfg->via1, 1, 1, 1);

    // ADB device state, serviced through Cuda packets (the AV pattern).
    cfg->adb = adb_init(NULL, cfg->scheduler, cp);

    // The behavioral Cuda (firmware 2.37 — the same 341S0788 part as the
    // AV machines) on the pseudo-VIA1 shift register + PB3/4/5.
    st->cuda = av_cuda_init(cfg->via1, cfg->rtc, cfg->adb, cfg->scheduler, cp);
    assert(st->cuda != NULL);

    // SCSI: the shared bus/target model on the Curio 53C94 cell (20 MHz
    // SCSI clock — the divided "Ethernet" oscillator).  The 8100 adds the
    // discrete 53CF96 on its fast internal bus (40 MHz), instantiated with
    // no bus attached: every select times out, the empty-bus presentation.
    // hd=/cd= media land on cfg->scsi, i.e. the Curio bus, on all models.
    cfg->scsi = scsi_init(NULL, cp);
    st->scsi96[0] = scsi_53c96_init(cfg->scheduler, 20000000, cp);
    scsi_53c96_set_irq_callback(st->scsi96[0], pdm_scsi96a_irq, cfg);
    scsi_53c96_attach_bus(st->scsi96[0], cfg->scsi);
    if (pdm_board(cfg)->has_fast_scsi) {
        st->scsi96[1] = scsi_53c96_init(cfg->scheduler, 40000000, cp);
        scsi_53c96_set_irq_callback(st->scsi96[1], pdm_scsi96b_irq, cfg);
    }

    // Board state + memory map.
    pdm_hmc_init(cfg);
    pdm_amic_init(cfg);
    pdm_amic_register_events(cfg);
    pdm_awacs_register_events(cfg);
    pdm_memory_layout(cfg);

    // Substrate-private checkpoint tail: the HMC config and AMIC register
    // file are plain data; derived mappings are rebuilt below.
    if (cp) {
        system_read_checkpoint_data(cp, &st->hmc, sizeof(st->hmc));
        system_read_checkpoint_data(cp, &st->amic, sizeof(st->amic));
        system_read_checkpoint_data(cp, &st->icr_sources, sizeof(st->icr_sources));
        pdm_hmc_remap(cfg);
        via_redrive_outputs(cfg->via1);
        pdm_amic_recompute(cfg);
    }

    // Presentation state, derived from the (possibly restored) register
    // file: the scanout descriptor over physical DRAM 0 and the AWACS
    // staging buffer + machine.sound node.
    pdm_video_init(cfg);
    pdm_awacs_init(cfg);

    // Finish: debugger + scheduler start (the mac030_glue_finish shape).
    cfg->debugger = debug_init();
    scheduler_start(cfg->scheduler);

    // Fresh boot: start the free-running VBL raster (a restore rebinds
    // the checkpointed pending event through the registered type).
    if (!cp)
        pdm_amic_start_vbl(cfg);
}

static void pdm_reset(config_t *cfg) {
    pdm_state_t *st = pdm_st(cfg);
    // Power-on reset: the 601 back to the reset vector, AMIC and HMC to
    // their power-on state.  (The 68k-RESET warm path re-enters HWInit
    // with MSR[IR] on and AMIC state SURVIVING — that path is guest-driven
    // and becomes a first-class test row in Phase D.)
    ppc_reset(cfg->ppc);
    pdm_amic_init(cfg);
    scc_reset(cfg->scc);
    for (int i = 0; i < 2; i++)
        if (st->scsi96[i])
            scsi_53c96_reset(st->scsi96[i]);
    st->hmc.cfg_lo = 0;
    st->hmc.cfg_hi = 0;
    st->hmc.bit_ptr = 0;
    st->hmc.wait_state = false;
    st->icr_sources = 0;
    pdm_hmc_remap(cfg);
    pdm_video_update(cfg); // blanked power-on raster follows the reset regs
}

static void pdm_teardown(config_t *cfg) {
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);
    pdm_state_t *st = pdm_st(cfg);
    if (st) {
        pdm_awacs_teardown(cfg);
        pdm_video_teardown(cfg);
        for (int i = 0; i < 2; i++) {
            if (st->scsi96[i]) {
                scsi_53c96_delete(st->scsi96[i]);
                st->scsi96[i] = NULL;
            }
        }
    }
    if (cfg->scsi) {
        scsi_delete(cfg->scsi);
        cfg->scsi = NULL;
    }
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
    // The AppleTalk stack is a client of the SCC's LocalTalk channel, so it
    // goes first — it holds the scc pointer it was given at init.
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

static void pdm_checkpoint_save(config_t *cfg, checkpoint_t *cp) {
    pdm_state_t *st = pdm_st(cfg);
    memory_map_checkpoint(cfg->mem_map, cp);
    ppc_checkpoint(cfg->ppc, cp);
    scheduler_checkpoint(cfg->scheduler, cp);
    rtc_checkpoint(cfg->rtc, cp);
    scc_checkpoint(cfg->scc, cp);
    appletalk_checkpoint(cp);
    via_checkpoint(cfg->via1, cp);
    adb_checkpoint(cfg->adb, cp);
    av_cuda_checkpoint(st->cuda, cp);
    // Same relative order as the pdm_init construction sequence (the
    // checkpoint stream is positional).
    scsi_checkpoint(cfg->scsi, cp);
    scsi_53c96_checkpoint(st->scsi96[0], cp);
    if (st->scsi96[1])
        scsi_53c96_checkpoint(st->scsi96[1], cp);
    // Substrate-private tail (mirrored by the restore block in pdm_init).
    system_write_checkpoint_data(cp, &st->hmc, sizeof(st->hmc));
    system_write_checkpoint_data(cp, &st->amic, sizeof(st->amic));
    system_write_checkpoint_data(cp, &st->icr_sources, sizeof(st->icr_sources));
}

// The AMIC-internal 60.15 Hz tick: VIA1 CA1 pulse.
static void pdm_trigger_vbl(config_t *cfg) {
    via_input_c(cfg->via1, 0, 0, 0);
    via_input_c(cfg->via1, 0, 0, 1);
    image_tick_all(cfg);
}

// NuBus routing spine — no BART/NuBus until Phase H.
static void pdm_update_ipl(config_t *cfg, int source, bool active) {
    (void)cfg;
    LOG(1, "update_ipl source=%d active=%d (no NuBus on PDM yet)", source, active);
}

// Primary display: the Ariel scanout over physical DRAM 0 (ariel.c).
static struct display *pdm_display(config_t *cfg) {
    return pdm_video_display(cfg);
}

// Floppy: no SWIM3 until Phase H — refuse politely.
static int pdm_fd_insert(config_t *cfg, int drive, struct image *disk) {
    (void)cfg;
    (void)drive;
    (void)disk;
    return -1;
}

static bool pdm_fd_present(config_t *cfg, int drive) {
    (void)cfg;
    (void)drive;
    return false;
}

const machine_substrate_t pdm_substrate = {
    .init = pdm_init,
    .reset = pdm_reset,
    .teardown = pdm_teardown,
    .checkpoint_save = pdm_checkpoint_save,
    .update_ipl = pdm_update_ipl,
    .trigger_vbl = pdm_trigger_vbl,
    .fd_insert = pdm_fd_insert,
    .fd_present = pdm_fd_present,
    .input_key = mac_input_key,
    .input_mouse_move = mac_input_mouse_move,
    .input_mouse_button = mac_input_mouse_button,
    .media_detach = system_media_detach_std,
    .media_attach = system_media_attach_std,
    .display = pdm_display,
};
