// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gossamer.c
// The Gossamer family substrate (the beige Power Macintosh G3).  See
// gossamer.h for the platform.
//
// Memory model (Grackle address map B, MPC106UM Table 3-4; Apple, "Power
// Macintosh G3 Computers" Developer Note, 1998):
//   $00000000-RAM top   SDRAM, decoded by Grackle's bank registers once
//                       the boot program has programmed them from the
//                       DIMMs' SPD bytes (grackle.c); before that the
//                       range reads all-ones
//   $80000000-$FCFFFFFF PCI memory, 1:1 — Heathrow's BAR0 at $F3000000,
//                       the on-board ATI and slot cards wherever Open
//                       Firmware assigns them
//   $FD000000-$FDFFFFFF PCI memory 0-16 MB
//   $FE000000-$FE7FFFFF PCI I/O 0-8 MB
//   $FEC00000/$FEE00000 Grackle CONFIG_ADDR / CONFIG_DATA
//   $FEF00000           PCI interrupt acknowledge (nothing answers)
//   $FF000000-$FF7FFFFF ROM bank 1: the board register at +4
//   $FF800000-$FFFFFFFF ROM bank 0: the 4 MB image, aliased through 8 MB
//                       (the 750 fetches its reset vector at $FFF00100)

#include "gossamer.h"
#include "config_seed.h"

#include "cuda.h" // the shared behavioral Cuda model (machines/av/)
#include "davbus.h"
#include "dbdma.h"

#include "adb.h"
#include "appletalk.h"
#include "checkpoint_images.h"
#include "debug.h"
#include "floppy.h"
#include "image.h"
#include "log.h"
#include "mac_host_io.h"
#include "machine_checkpoint.h"
#include "machine_teardown.h"
#include "of_nvram.h"
#include "pci.h"
#include "ppc.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "scsi_mesh.h"
#include "slot_tables.h"
#include "swim3.h"
#include "via.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("board");

// ============================================================
// Page-table helpers (the tnt_fill_page shape)
// ============================================================

void gos_fill_page(uint32_t page_index, uint8_t *host_ptr, bool writable) {
    if (page_index >= (uint32_t)g_page_count)
        return;
    g_page_table[page_index].host_base = host_ptr;
    g_page_table[page_index].dev = NULL;
    g_page_table[page_index].dev_context = NULL;
    g_page_table[page_index].writable = writable;
    uint32_t guest_base = page_index << PAGE_SHIFT;
    uintptr_t adjusted = (uintptr_t)host_ptr - guest_base;
    // Supervisor arrays hold the eager physical identity view; the user
    // arrays belong to the PPC MMU front end and are only cleared here.
    if (g_supervisor_read)
        g_supervisor_read[page_index] = adjusted;
    if (g_supervisor_write)
        g_supervisor_write[page_index] = writable ? adjusted : 0;
    if (g_user_read)
        g_user_read[page_index] = 0;
    if (g_user_write)
        g_user_write[page_index] = 0;
    memory_logpoint_guard_page(page_index);
}

void gos_clear_page(uint32_t page_index) {
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
// Memory layout
// ============================================================

static void gos_memory_layout(config_t *cfg, checkpoint_t *cp) {
    // ROM: the 4 MB image at $FFC00000, and its alias at $FF800000 — "any
    // system ROM space that is not physically implemented in a bank will be
    // aliased to the physical device(s) within that bank" (MPC106UM §6.5).
    uint8_t *rom = ram_native_pointer(cfg->mem_map, cfg->ram_size);
    uint32_t rom_pages = cfg->machine->rom_size >> PAGE_SHIFT;
    for (uint32_t p = 0; p < rom_pages; p++) {
        gos_fill_page((GOS_ROM_BASE >> PAGE_SHIFT) + p, rom + (p << PAGE_SHIFT), false);
        gos_fill_page((GOS_ROM_BANK0 >> PAGE_SHIFT) + p, rom + (p << PAGE_SHIFT), false);
    }

    // PCI: the root, then the bridge (config ports, the bus, Grackle's own
    // header, the windows, the board page), then Heathrow on the bus.
    cfg->pci = pci_root_create(cfg);
    pci_init(cfg->pci, cfg->machine->pci_slots);
    gos_grackle_init(cfg, cp);
    gos_heathrow_pci_attach(cfg, cp);
}

// ============================================================
// DBDMA hooks
// ============================================================
// Bus-master DMA: RAM through the host backing store (descriptors and
// data buffers live there), anything else through the bus's slow path.
// The RAM fast path must respect the Grackle decode — RAM is contiguous
// from 0 once the ROM has sized it, and DMA never runs before that.

static void gos_dbdma_mem_read(void *ctx, uint32_t phys, uint8_t *buf, uint32_t len) {
    config_t *cfg = (config_t *)ctx;
    if (phys < cfg->ram_size && len <= cfg->ram_size - phys) {
        memcpy(buf, ram_native_pointer(cfg->mem_map, 0) + phys, len);
        return;
    }
    for (uint32_t i = 0; i < len; i++)
        buf[i] = memory_read_uint8_slow(phys + i);
}

static void gos_dbdma_mem_write(void *ctx, uint32_t phys, const uint8_t *buf, uint32_t len) {
    config_t *cfg = (config_t *)ctx;
    if (phys < cfg->ram_size && len <= cfg->ram_size - phys) {
        memcpy(ram_native_pointer(cfg->mem_map, 0) + phys, buf, len);
        return;
    }
    for (uint32_t i = 0; i < len; i++)
        memory_write_uint8_slow(phys + i, buf[i]);
}

// A channel completion is a held level on the channel's source (the
// Heathrow table, not the index) until its Clear bit is written.
static void gos_dbdma_irq(void *ctx, int chan) {
    int src = gos_dbdma_source(chan);
    if (src >= 0)
        gos_set_source((config_t *)ctx, src, true);
}

// ============================================================
// VIA / Cuda, SCC, MESH wiring
// ============================================================

static void gos_via1_output(void *context, uint8_t port, uint8_t value) {
    gossamer_state_t *st = gos_st((config_t *)context);
    av_cuda_via1_port_output(st ? st->cuda : NULL, port, value);
}

static void gos_via1_shift_out(void *context, uint8_t byte) {
    gossamer_state_t *st = gos_st((config_t *)context);
    av_cuda_via1_shift_input(st ? st->cuda : NULL, byte);
}

// VIA IRQ -> Heathrow source $12 (IPL 1 in the kind-7 classification).
static void gos_via1_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    if (gos_st(cfg))
        gos_set_source(cfg, GOS_INT_VIA, active);
}

// The SCC's one INT line -> both channel sources ($0F/$10); the guest
// discriminates via RR3 (the TNT wiring).
static void gos_scc_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    if (!gos_st(cfg))
        return;
    gos_set_source(cfg, GOS_INT_SCCA, active);
    gos_set_source(cfg, GOS_INT_SCCB, active);
}

static void gos_mesh_irq(void *ctx, bool level) {
    gos_set_source((config_t *)ctx, GOS_INT_MESH, level);
}

static void gos_mesh_dbdma_kick(void *ctx) {
    dbdma_kick(gos_st((config_t *)ctx)->dbdma, GOS_DMA_MESH);
}

// ============================================================
// SWIM3 glue: +$15000, source $13, DBDMA channel 1 (the tnt/swim3.c
// shape — the chip, the byte ring and the channel port)
// ============================================================

static uint32_t ring_count(const gos_fdring_t *r) {
    return r->tail - r->head;
}

static bool ring_push(gos_fdring_t *r, uint8_t v) {
    if (ring_count(r) >= GOS_FDRING_SIZE)
        return false;
    r->buf[r->tail % GOS_FDRING_SIZE] = v;
    r->tail++;
    return true;
}

static bool ring_pop(gos_fdring_t *r, uint8_t *v) {
    if (ring_count(r) == 0)
        return false;
    *v = r->buf[r->head % GOS_FDRING_SIZE];
    r->head++;
    return true;
}

static bool fd_dma_running(void *ctx) {
    return dbdma_active(gos_st((config_t *)ctx)->dbdma, GOS_DMA_SWIM3);
}

static bool fd_dma_put(void *ctx, uint8_t value) {
    gossamer_state_t *st = gos_st((config_t *)ctx);
    if (!dbdma_active(st->dbdma, GOS_DMA_SWIM3) || !ring_push(&st->fdring, value))
        return false;
    dbdma_kick(st->dbdma, GOS_DMA_SWIM3);
    return true;
}

static bool fd_dma_get(void *ctx, uint8_t *out) {
    gossamer_state_t *st = gos_st((config_t *)ctx);
    if (ring_pop(&st->fdring, out))
        return true;
    if (!dbdma_active(st->dbdma, GOS_DMA_SWIM3))
        return false;
    dbdma_kick(st->dbdma, GOS_DMA_SWIM3);
    return ring_pop(&st->fdring, out);
}

static void fd_set_irq(void *ctx, bool level) {
    gos_set_source((config_t *)ctx, GOS_INT_SWIM3, level);
}

static int fd_port_out(void *ctx, const uint8_t *buf, int len) {
    gossamer_state_t *st = gos_st((config_t *)ctx);
    int n = 0;
    while (n < len && ring_push(&st->fdring, buf[n]))
        n++;
    return n;
}

static int fd_port_in(void *ctx, uint8_t *buf, int len) {
    gossamer_state_t *st = gos_st((config_t *)ctx);
    int n = 0;
    while (n < len && ring_pop(&st->fdring, &buf[n]))
        n++;
    return n;
}

void gos_swim3_bind(config_t *cfg) {
    const swim3_backend_t be = {
        .dma_running = fd_dma_running,
        .dma_get = fd_dma_get,
        .dma_put = fd_dma_put,
        .set_irq = fd_set_irq,
        .ctx = cfg,
    };
    swim3_bind(&gos_st(cfg)->swim3, cfg->floppy, cfg->scheduler, &be);
}

static void gos_swim3_init(config_t *cfg) {
    gossamer_state_t *st = gos_st(cfg);
    memset(&st->fdring, 0, sizeof(st->fdring));
    dbdma_port_t port = {.out = fd_port_out, .in = fd_port_in, .s_bits = NULL, .ctx = cfg};
    dbdma_set_port(st->dbdma, GOS_DMA_SWIM3, &port);
}

// ============================================================
// Substrate lifecycle
// ============================================================

// The NVRAM is non-volatile: its content survives machine.restart (the
// power switch) because a restart never destroys the machine, and a new
// machine (machine.boot) gets a new part -- the TNT rule
// and its reasons (tnt.c).  Nothing carries it across a teardown.  The new
// part holds what the board's own firmware formats (of_nvram.h): OF 2.4's
// variables and the ROM's parameter RAM defaults.  The Rev A ROM's OF
// 2.0f1 accepts it as it stands (its own format differs in one default,
// diag-device).

// Pulling the battery: the store goes back to what a new board carries.
void gos_nvram_clear(config_t *cfg) {
    gossamer_state_t *st = gos_st(cfg);
    if (st)
        of_nvram_factory(st->hr.nvram, &of_nvram_defaults_g3);
    LOG(1, "NVRAM cleared (battery removed)");
}

// Checkpoint parts of the board's own (machine_parts.h).
static void part_save_dbdma(void *obj, checkpoint_t *cp) {
    dbdma_checkpoint(obj, cp);
}

static void part_save_mesh(void *obj, checkpoint_t *cp) {
    mesh_checkpoint(obj, cp);
}

// Grackle, Heathrow, the I2C bus and the Screamer registers.
static void part_save_gossamer_board(void *obj, checkpoint_t *cp) {
    gossamer_state_t *st = obj;
    system_write_checkpoint_data(cp, &st->grackle.cfg, sizeof(st->grackle.cfg));
    system_write_checkpoint_data(cp, &st->grackle.cfg_addr, sizeof(st->grackle.cfg_addr));
    system_write_checkpoint_data(cp, &st->hr, sizeof(st->hr));
    system_write_checkpoint_data(cp, &st->i2c, sizeof(st->i2c));
    system_write_checkpoint_data(cp, &st->screamer, sizeof(st->screamer));
}

// The floppy controller (its plain-data prefix) and its DBDMA byte ring.
static void part_save_gossamer_swim3(void *obj, checkpoint_t *cp) {
    gossamer_state_t *st = obj;
    system_write_checkpoint_data(cp, &st->swim3, offsetof(swim3_t, fd));
    system_write_checkpoint_data(cp, &st->fdring, sizeof(st->fdring));
}

static void part_save_gossamer_ata(void *obj, checkpoint_t *cp) {
    gos_ata_checkpoint_save(obj, cp);
}

static void part_save_gossamer_bmac(void *obj, checkpoint_t *cp) {
    gos_bmac_checkpoint_save(obj, cp);
}

static int gossamer_init(config_t *cfg, checkpoint_t *cp) {
    gossamer_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        LOG(0, "Error: out of memory allocating the machine state for %s", cfg->machine->name);
        return -1;
    }
    cfg->machine_context = st;
    of_nvram_factory(st->hr.nvram, &of_nvram_defaults_g3); // a checkpoint below restores over it
    const gossamer_board_desc_t *board = gos_board(cfg);

    // Core: memory map, the 750 with the board's PVR and PLL straps, the
    // scheduler on the PPC seam.
    machine_part_begin(cfg, cp, "memory");
    cfg->mem_map =
        memory_map_init(cfg->machine->address_bits, cfg->ram_size, cfg->machine->rom_size, MEMORY_BUS_ERR_NONE,
                        &cfg->build_opts.rom, cp); // no bus-error watchdog: unanswered floats to $FF
    machine_part(cfg, cp, "memory", part_save_memory, cfg->mem_map);
    memory_map_set_host_fill(cfg->mem_map, gos_fill_page);
    machine_part_begin(cfg, cp, "cpu");
    cfg->ppc = ppc_init(cp, cfg->machine->cpu_model);
    if (cfg->ppc) {
        memory_cpu_hooks_t hooks = ppc_memory_hooks(cfg->ppc);
        memory_map_set_cpu_hooks(cfg->mem_map, &hooks);
    }
    if (!cfg->ppc) {
        LOG(0, "Error: out of memory constructing the PowerPC core");
        return -1;
    }
    machine_part(cfg, cp, "cpu", part_save_ppc, cfg->ppc);
    ppc_set_identity(cfg->ppc, board->pvr, board->hid1);
    sched_cpu_if_t cpu_if = ppc_sched_if(cfg->ppc);
    machine_part_begin(cfg, cp, "scheduler");
    cfg->scheduler = scheduler_init(&cpu_if, cp);
    machine_part(cfg, cp, "scheduler", part_save_scheduler, cfg->scheduler);
    scheduler_set_frequency(cfg->scheduler, cfg->machine->freq);
    // CPI 2, the TNT rationale: a 750 running the 68k emulator sustains
    // well under one instruction per clock.
    scheduler_set_cpi(cfg->scheduler, 2);
    // Time base and decrementer at bus/4 = 16,705,000 Hz (the tree's
    // timebase-frequency $00FEE5E8; MPC750UM §2.1.1).
    ppc_bind_time(cfg->ppc, cfg->scheduler, cfg->machine->freq, board->bus_hz / 4u);

    machine_part_begin(cfg, cp, "rtc");
    cfg->rtc = rtc_init(cfg->scheduler, cp, true, cfg->machine->pram);
    machine_part(cfg, cp, "rtc", part_save_rtc, cfg->rtc);

    // The ESCC behind Heathrow's two apertures; RTxC 3.6864 MHz, the value
    // every driver assumes (Linux ZS_CLOCK 3686400; NetBSD "RTxC is 230400*16").
    machine_part_begin(cfg, cp, "scc");
    cfg->scc = scc_init(NULL, cfg->scheduler, gos_scc_irq, cfg, cp);
    machine_part(cfg, cp, "scc", part_save_scc, cfg->scc);
    scc_set_clocks(cfg->scc, 15667200, 3686400);
    machine_part_begin(cfg, cp, "appletalk");
    cfg->atalk = atalk_conn_new(appletalk_network(), cfg->scheduler, cfg->scc, cp);
    machine_part(cfg, cp, "appletalk", part_save_atalk, cfg->atalk);
    machine_part_imagewriter(cfg, cp, false);

    // VIA1: the 6522 cell at Heathrow +$16000 ($200 stride), timers at the
    // classic 783.36 kHz.
    uint8_t via_ff = via_freq_factor_for_clock(cfg->machine->freq);
    machine_part_begin(cfg, cp, "via1");
    cfg->via1 =
        via_init(NULL, cfg->scheduler, via_ff, "via1", gos_via1_output, gos_via1_shift_out, gos_via1_irq, cfg, cp);
    machine_part(cfg, cp, "via1", part_save_via, cfg->via1);
    via_set_exact_clock(cfg->via1, cfg->machine->freq);
    via_input(cfg->via1, 1, 3, 1); // PB3 = Cuda TREQ, idle high
    via_input_c(cfg->via1, 0, 0, 1);
    via_input_c(cfg->via1, 1, 0, 1);
    via_input_c(cfg->via1, 1, 1, 1);

    machine_part_begin(cfg, cp, "adb");
    cfg->adb = adb_init(NULL, cfg->scheduler, cp);
    machine_part(cfg, cp, "adb", part_save_adb, cfg->adb);

    // Cuda: the 341S0060 part with firmware 2.40, the RTC seed in the
    // Mode3Clock tick (the PDM/TNT choice), and the board's I2C bus.
    machine_part_begin(cfg, cp, "cuda");
    st->cuda = av_cuda_init(cfg->via1, cfg->rtc, cfg->adb, cfg->scheduler, cp, /*mode3_clock=*/true);
    if (!st->cuda) {
        LOG(0, "Error: out of memory constructing the Cuda");
        return -1;
    }
    machine_part(cfg, cp, "cuda", part_save_cuda, st->cuda);
    av_cuda_set_firmware_240(st->cuda);
    av_cuda_attach_i2c_bus(st->cuda, gos_i2c_read, gos_i2c_write, cfg);

    // DBDMA: Heathrow's thirteen channel blocks.
    machine_part_begin(cfg, cp, "dbdma");
    st->dbdma = dbdma_init(cp, DBDMA_CHANNELS_HEATHROW);
    if (!st->dbdma)
        return -1;
    machine_part(cfg, cp, "dbdma", part_save_dbdma, st->dbdma);
    machine_part_begin(cfg, cp, "floppy");
    cfg->floppy =
        floppy_init(FLOPPY_TYPE_SWIM3, NULL, cfg->scheduler, machine_floppy_count(cfg), cp, CONFIG_IMAGES(cfg));
    machine_part(cfg, cp, "floppy", part_save_floppy, cfg->floppy);
    gos_swim3_bind(cfg);
    gos_swim3_init(cfg);
    gos_scc_dma_init(cfg);
    static const dma_mem_port_t dbdma_port = {
        .read_block = gos_dbdma_mem_read,
        .write_block = gos_dbdma_mem_write,
    };
    dma_mem_port_t port = dbdma_port;
    port.ctx = cfg;
    dbdma_set_memory_port(st->dbdma, &port);
    dbdma_set_irq_hook(st->dbdma, gos_dbdma_irq, cfg);
    swim3_register_events(&st->swim3);
    swim3_xfer_register_events(&st->swim3);

    // The Screamer face of the DAVbus cell, output on channel 8 (the ROM's
    // boot program polls its status word for codec-ready before anything
    // else sound-related; the 68k Start Manager plays the boot beep here).
    davbus_host_t *snd = &st->screamer_host;
    snd->regs = &st->screamer;
    snd->sched = cfg->scheduler;
    snd->dbdma = st->dbdma;
    snd->out_chan = GOS_DMA_AUD_OUT;
    snd->input = true; // the record channel runs (silence): see davbus.c
    snd->in_chan = GOS_DMA_AUD_IN;
    snd->cpu_hz = cfg->machine->freq;
    snd->screamer = true;
    davbus_register_events(snd);
    davbus_init(snd);
    davbus_reset(snd);

    // Board state: the DIMMs and their SPD bytes first (the Grackle bank
    // inventory reads them), then Heathrow, then the memory map.
    gos_i2c_init(cfg);
    gos_heathrow_init(cfg);
    gos_grackle_attach_objects(cfg);
    gos_heathrow_attach_objects(cfg);
    gos_memory_layout(cfg, cp);

    // The PCI slot walk: builtins and whatever the boot document names.
    pci_seat_slots(cfg->pci, cp);

    // The board's own state.
    machine_part_begin(cfg, cp, "gossamer");
    if (cp) {
        system_read_checkpoint_data(cp, &st->grackle.cfg, sizeof(st->grackle.cfg));
        system_read_checkpoint_data(cp, &st->grackle.cfg_addr, sizeof(st->grackle.cfg_addr));
        gos_grackle_remap(cfg);
        system_read_checkpoint_data(cp, &st->hr, sizeof(st->hr));
        system_read_checkpoint_data(cp, &st->i2c, sizeof(st->i2c));
        system_read_checkpoint_data(cp, &st->screamer, sizeof(st->screamer));
    }
    machine_part(cfg, cp, "gossamer", part_save_gossamer_board, st);
    // Every PCI device read its config header with its own part; its decode
    // waits for the bus windows, which exist now.
    if (cp)
        pci_replay_decode(cfg->pci);
    if (cp) {
        via_redrive_outputs(cfg->via1);
        gos_recompute_irq(cfg);
    }

    // SCSI: the one MESH bus (internal and external connectors share it).
    machine_part_images(cfg, cp);
    machine_part_begin(cfg, cp, "scsi");
    cfg->scsi = machine_scsi_bus_init(cfg, cp, "scsi");
    machine_part(cfg, cp, "scsi", part_save_scsi, cfg->scsi);
    machine_part_begin(cfg, cp, "mesh");
    st->mesh = mesh_init(cfg->scheduler, cp);
    machine_part(cfg, cp, "mesh", part_save_mesh, st->mesh);
    mesh_attach_bus(st->mesh, cfg->scsi);
    mesh_set_irq_callback(st->mesh, gos_mesh_irq, cfg);
    mesh_set_dbdma_kick(st->mesh, gos_mesh_dbdma_kick, cfg);
    dbdma_port_t mesh_port = {
        .out = mesh_port_out,
        .in = mesh_port_in,
        .s_bits = NULL,
        .burst = MESH_DMA_BURST,
        .ctx = st->mesh,
    };
    dbdma_set_port(st->dbdma, GOS_DMA_MESH, &mesh_port);

    machine_part_begin(cfg, cp, "swim3");
    if (cp) {
        system_read_checkpoint_data(cp, &st->swim3, offsetof(swim3_t, fd));
        system_read_checkpoint_data(cp, &st->fdring, sizeof(st->fdring));
        gos_swim3_bind(cfg);
        gos_recompute_irq(cfg);
    }
    machine_part(cfg, cp, "swim3", part_save_gossamer_swim3, st);

    // The two ATA cells and their ATAPI back end.
    machine_part_begin(cfg, cp, "ata");
    gos_ata_init(cfg, cp);
    machine_part(cfg, cp, "ata", part_save_gossamer_ata, cfg);
    gos_ata_attach_objects(cfg);
    machine_part_begin(cfg, cp, "bmac");
    gos_bmac_init(cfg, cp);
    machine_part(cfg, cp, "bmac", part_save_gossamer_bmac, cfg);
    gos_bmac_attach_objects(cfg);
    if (cp)
        gos_recompute_irq(cfg);

    cfg->debugger = debug_init();
    return 0;
}

// The board's reset net: every chip back to power-on (NVRAM survives).
// A power cycle's power-on-only half (machine_profile.h): Cuda stays
// powered, but the host side of its VIA1 handshake went down under it.
static void gossamer_power_on(config_t *cfg) {
    av_cuda_host_power_cycle(gos_st(cfg)->cuda);
}

static void gossamer_bus_reset(config_t *cfg) {
    gossamer_state_t *st = gos_st(cfg);
    gos_grackle_reset(cfg);
    gos_heathrow_init(cfg);
    dbdma_reset(st->dbdma);
    davbus_reset(&st->screamer_host);
    swim3_reset(&st->swim3);
    mesh_reset(st->mesh);
    gos_ata_reset(cfg);
    gos_bmac_reset(cfg);
    scc_reset(cfg->scc);
    system_reset_common_devices(cfg);
    gos_recompute_irq(cfg);
}

static void gossamer_teardown(config_t *cfg) {
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);
    gossamer_state_t *st = gos_st(cfg);
    if (st) {
        gos_bmac_detach_objects(cfg);
        gos_ata_detach_objects(cfg);
        gos_heathrow_detach_objects(cfg);
        gos_grackle_detach_objects(cfg);
        davbus_teardown(&st->screamer_host);
        if (st->mesh) {
            mesh_delete(st->mesh);
            st->mesh = NULL;
        }
        gos_ata_teardown(cfg);
        gos_bmac_teardown(cfg);
    }
    if (cfg->floppy) {
        floppy_delete(cfg->floppy);
        cfg->floppy = NULL;
    }
    if (st && st->dbdma) {
        dbdma_delete(st->dbdma);
        st->dbdma = NULL;
    }
    if (st && st->cuda) {
        av_cuda_delete(st->cuda);
        st->cuda = NULL;
    }
    if (cfg->adb) {
        adb_delete(cfg->adb);
        cfg->adb = NULL;
    }
    machine_teardown_config_devices(cfg);
    if (st) {
        free(st);
        cfg->machine_context = NULL;
    }
}

// Frame tick: the 60.15 Hz reference into VIA1 CA1 (the Cuda driver waits
// for a CA1 edge in its init — the TNT/PDM/AV wiring), media polling and
// the PCI cards' VBL.
static void gossamer_trigger_vbl(config_t *cfg) {
    via_input_c(cfg->via1, 0, 0, 0);
    via_input_c(cfg->via1, 0, 0, 1);
    image_tick_all(cfg);
    pci_tick_vbl(cfg->pci);
}

// A PCI slot's INTA-D line reaches the Heathrow source its declaration
// names (A1/B1/C1 -> $17/$18/$19, the ATI -> $16).
static void gossamer_pci_slot_irq(config_t *cfg, int slot, bool active) {
    const pci_slot_decl_t *d = pci_slot_decl_get(cfg->pci, slot);
    if (!d || d->int_line <= 0) {
        LOG(1, "PCI slot %d asserted an interrupt but declares no line", slot);
        return;
    }
    gos_set_source(cfg, d->int_line, active);
}

static int gossamer_fd_insert(config_t *cfg, int drive, struct image *disk) {
    if (!cfg->floppy || drive != 0)
        return -1;
    return floppy_insert(cfg->floppy, drive, disk);
}

static bool gossamer_fd_present(config_t *cfg, int drive) {
    if (!cfg->floppy || drive != 0)
        return false;
    return floppy_is_inserted(cfg->floppy, drive);
}

// The board's storage: the MESH SCSI bus (the internal 50-pin cable and the
// external DB-25 on one bus, with no named bay) and Heathrow's two ATA
// buses.  A stock machine has its hard disk and CD-ROM drive on ATA; here
// they stay on SCSI until the ROM's expected ATA placement is checked, and
// an ATA bus takes hard disks (an ATAPI CD-ROM drive there is not yet a
// construction input).  The startup record names a SCSI ID, so only the SCSI
// bus can hold the startup device.
const storage_bus_decl_t gossamer_storage[] = {
    {.id = "scsi",
     .label = "SCSI",
     .detail = "MESH",
     .kind = STORAGE_KIND_SCSI,
     .media_bus = MEDIA_BUS_SCSI,
     .units = MAC_SCSI_UNITS,
     .reserved = MAC_SCSI_RESERVED,
     .external_connector = true,
     .accepts = STORAGE_DEV_HD | STORAGE_DEV_CD,
     .startup_ok = true},
    {.id = "ata0",
     .label = "Primary ATA bus",
     .kind = STORAGE_KIND_ATA,
     .media_bus = MEDIA_BUS_ATA,
     .media_unit_base = 0,
     .units = 0x3u,
     .accepts = STORAGE_DEV_HD},
    {.id = "ata1",
     .label = "Secondary ATA bus",
     .kind = STORAGE_KIND_ATA,
     .media_bus = MEDIA_BUS_ATA,
     .media_unit_base = 2,
     .units = 0x3u,
     .accepts = STORAGE_DEV_HD},
    {0},
};

// The SCSI path the install CD boots through: a hard disk at ID 0 and the
// CD-ROM drive at the era's ID 3.
const storage_device_decl_t gossamer_default_storage[] = {
    {.bus = "scsi", .unit = 0, .type = STORAGE_DEV_HD},
    {.bus = "scsi", .unit = 3, .type = STORAGE_DEV_CD},
    {0},
};

// The ATI Rage Pro on the board, the machine's built-in video.  Its port
// offers the 13" RGB (Apple sense 6) or nothing at all: with no cable every
// sense line floats high (MACHINE_SENSE_NONE, 7), which the chip's FCode
// reads as "nothing attached" — how a G3 runs with its display on a PCI
// card instead.  The chip's other monitors wait on a monitor option for it.
static const struct {
    const char *id, *monitor;
    uint8_t sense;
} gossamer_monitors[] = {
    {"13in_rgb", "13in_rgb", 6u                },
    {"none",     "none",     MACHINE_SENSE_NONE},
};

static bool gossamer_monitor_at(size_t i, const char **id, const char **monitor) {
    if (i >= sizeof(gossamer_monitors) / sizeof(gossamer_monitors[0]))
        return false;
    *id = gossamer_monitors[i].id;
    *monitor = gossamer_monitors[i].monitor;
    return true;
}

static bool gossamer_monitor_sense(const char *id, uint8_t *out_sense) {
    for (size_t i = 0; i < sizeof(gossamer_monitors) / sizeof(gossamer_monitors[0]); i++)
        if (strcmp(gossamer_monitors[i].id, id) == 0) {
            *out_sense = gossamer_monitors[i].sense;
            return true;
        }
    return false;
}

const builtin_video_desc_t gossamer_builtin_video = {
    .detail = "ATI Rage Pro",
    .monitor_at = gossamer_monitor_at,
    .monitor_sense = gossamer_monitor_sense,
    .default_monitor = "13in_rgb",
};

// The PCI topology: the three expansion sockets at devices $0D/$0E/$0F
// (the Grackle node's `slot-names 0000e000 "A1" "B1" "C1"`) with their
// strapped lines $17/$18/$19 (the ROM's device -> source pairs).  Grackle
// and Heathrow are the family's own devices, attached by grackle.c /
// heathrow.c, not slot entries.
const pci_slot_decl_t gossamer_pci_slots[] = {
    {.slot = 1,
     .kind = PCI_SLOT_SOCKET,
     .label = "PCI slot A1",
     .detail = "A1",
     .fill_order = 1,
     .bus = GOS_PCI_BUS,
     .device = GOS_DEV_SLOT_A1,
     .int_line = GOS_INT_SLOT_A1},
    {.slot = 2,
     .kind = PCI_SLOT_SOCKET,
     .label = "PCI slot B1",
     .detail = "B1",
     .fill_order = 2,
     .bus = GOS_PCI_BUS,
     .device = GOS_DEV_SLOT_B1,
     .int_line = GOS_INT_SLOT_B1},
    {.slot = 3,
     .kind = PCI_SLOT_SOCKET,
     .label = "PCI slot C1",
     .detail = "C1",
     .fill_order = 3,
     .bus = GOS_PCI_BUS,
     .device = GOS_DEV_SLOT_C1,
     .int_line = GOS_INT_SLOT_C1},
    // The on-board ATI Rage Pro, "slot" F1 in the firmware's slot names.
    // Its display is the screen when the configuration connects the monitor
    // to it (the built-in video's slot entry).
    {.slot = 4,
     .kind = PCI_SLOT_BUILTIN,
     .label = "Built-in video",
     .detail = "F1",
     .bus = GOS_PCI_BUS,
     .device = GOS_DEV_ATI,
     .int_line = GOS_INT_ATI,
     .builtin_card_id = "ati_rage_pro"},
    {0},
};

// The seeding step: Mac OS keeps its PRAM in the NVRAM's XPRAM partition.
static void gossamer_seed(config_t *cfg) {
    gossamer_state_t *st = gos_st(cfg);
    if (!st)
        return;
    mac_seed_xpram_appletalk(st->hr.nvram + OF_NVRAM_XPRAM, cfg, of_nvram_defaults_g3.pram);
    int id = mac_seed_startup_scsi_id(cfg, "scsi");
    if (id != -2)
        of_nvram_set_startup_scsi(st->hr.nvram, id, &of_nvram_defaults_g3);
}

const machine_substrate_t gossamer_substrate = {
    .init = gossamer_init,
    .bus_reset = gossamer_bus_reset,
    .power_on = gossamer_power_on,
    .teardown = gossamer_teardown,
    .seed = gossamer_seed,
    .pci_slot_irq = gossamer_pci_slot_irq,
    .trigger_vbl = gossamer_trigger_vbl,
    .fd_insert = gossamer_fd_insert,
    .fd_present = gossamer_fd_present,
    .input_key = mac_input_key,
    .input_mouse_move = mac_input_mouse_move,
    .input_mouse_button = mac_input_mouse_button,
    .media_attach = gos_media_attach,
    .media_present = gos_media_present,
    .media_eject = gos_media_eject,
};
