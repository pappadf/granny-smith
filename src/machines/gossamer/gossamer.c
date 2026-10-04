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

#include "cuda.h" // the shared behavioral Cuda model (machines/av/)
#include "davbus.h"
#include "dbdma.h"

#include "adb.h"
#include "appletalk.h"
#include "checkpoint_images.h"
#include "debug.h"
#include "debug_mac.h"
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

static void gos_memory_layout(config_t *cfg) {
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
    gos_grackle_init(cfg);
    gos_heathrow_pci_attach(cfg);
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
// machine (machine.boot, machine.rebuild) gets a new part -- the TNT rule
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
    cfg->mem_map =
        memory_map_init(cfg->machine->address_bits, cfg->ram_size, cfg->machine->rom_size, MEMORY_BUS_ERR_NONE,
                        &cfg->build_opts.rom, cp); // no bus-error watchdog: unanswered floats to $FF
    g_mem_host_fill = gos_fill_page;
    cfg->ppc = ppc_init(cp, cfg->machine->cpu_model);
    if (!cfg->ppc) {
        LOG(0, "Error: out of memory constructing the PowerPC core");
        return -1;
    }
    ppc_set_identity(cfg->ppc, board->pvr, board->hid1);
    sched_cpu_if_t cpu_if = ppc_sched_if(cfg->ppc);
    cfg->scheduler = scheduler_init(&cpu_if, cp);
    debug_mac_register_scheduler_events(cfg->scheduler);
    scheduler_set_frequency(cfg->scheduler, cfg->machine->freq);
    // CPI 2, the TNT rationale: a 750 running the 68k emulator sustains
    // well under one instruction per clock.
    scheduler_set_cpi(cfg->scheduler, 2);
    // Time base and decrementer at bus/4 = 16,705,000 Hz (the tree's
    // timebase-frequency $00FEE5E8; MPC750UM §2.1.1).
    ppc_bind_time(cfg->ppc, cfg->scheduler, cfg->machine->freq, board->bus_hz / 4u);

    cfg->rtc = rtc_init(cfg->scheduler, cp, true, cfg->machine->pram);

    // The ESCC behind Heathrow's two apertures; RTxC 3.6864 MHz, the value
    // every driver assumes (Linux ZS_CLOCK 3686400; NetBSD "RTxC is 230400*16").
    cfg->scc = scc_init(NULL, cfg->scheduler, gos_scc_irq, cfg, cp);
    scc_set_clocks(cfg->scc, 15667200, 3686400);
    appletalk_init(cfg->scheduler, cfg->scc, cp);

    // VIA1: the 6522 cell at Heathrow +$16000 ($200 stride), timers at the
    // classic 783.36 kHz.
    uint8_t via_ff = via_freq_factor_for_clock(cfg->machine->freq);
    cfg->via1 =
        via_init(NULL, cfg->scheduler, via_ff, "via1", gos_via1_output, gos_via1_shift_out, gos_via1_irq, cfg, cp);
    via_set_exact_clock(cfg->via1, cfg->machine->freq);
    via_input(cfg->via1, 1, 3, 1); // PB3 = Cuda TREQ, idle high
    via_input_c(cfg->via1, 0, 0, 1);
    via_input_c(cfg->via1, 1, 0, 1);
    via_input_c(cfg->via1, 1, 1, 1);

    cfg->adb = adb_init(NULL, cfg->scheduler, cp);

    // Cuda: the 341S0060 part with firmware 2.40, the RTC seed in the
    // Mode3Clock tick (the PDM/TNT choice), and the board's I2C bus.
    st->cuda = av_cuda_init(cfg->via1, cfg->rtc, cfg->adb, cfg->scheduler, cp, /*mode3_clock=*/true);
    if (!st->cuda) {
        LOG(0, "Error: out of memory constructing the Cuda");
        return -1;
    }
    av_cuda_set_firmware_240(st->cuda);
    av_cuda_attach_i2c_bus(st->cuda, gos_i2c_read, gos_i2c_write, cfg);

    // DBDMA: Heathrow's thirteen channel blocks.
    st->dbdma = dbdma_init(cp, DBDMA_CHANNELS_HEATHROW);
    if (!st->dbdma)
        return -1;
    cfg->floppy = floppy_init(FLOPPY_TYPE_SWIM3, NULL, cfg->scheduler, profile_floppy_count(cfg->machine), cp);
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
    gos_memory_layout(cfg);

    // The PCI slot walk: builtins and whatever the user staged.
    pci_seat_slots(cfg->pci, cp);

    // Substrate tail (mirrored by gossamer_checkpoint_save).
    if (cp) {
        system_read_checkpoint_data(cp, &st->grackle.cfg, sizeof(st->grackle.cfg));
        system_read_checkpoint_data(cp, &st->grackle.cfg_addr, sizeof(st->grackle.cfg_addr));
        gos_grackle_remap(cfg);
        system_read_checkpoint_data(cp, &st->hr, sizeof(st->hr));
        system_read_checkpoint_data(cp, &st->i2c, sizeof(st->i2c));
        system_read_checkpoint_data(cp, &st->screamer, sizeof(st->screamer));
        pci_checkpoint_restore(cfg->pci, cp);
        via_redrive_outputs(cfg->via1);
        gos_recompute_irq(cfg);
    }

    // SCSI: the one MESH bus (internal and external connectors share it).
    if (cp)
        mac_checkpoint_restore_images(cfg, cp);
    cfg->scsi = profile_scsi_init(cfg->machine, cp);
    st->mesh = mesh_init(cfg->scheduler, cp);
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

    if (cp) {
        system_read_checkpoint_data(cp, &st->swim3, offsetof(swim3_t, fd));
        system_read_checkpoint_data(cp, &st->fdring, sizeof(st->fdring));
        gos_swim3_bind(cfg);
        gos_recompute_irq(cfg);
    }

    // The two ATA cells and their ATAPI back end (restored last, as saved).
    gos_ata_init(cfg, cp);
    gos_ata_attach_objects(cfg);
    // BMAC, after the ATA cells in the stream too.
    gos_bmac_init(cfg, cp);
    gos_bmac_attach_objects(cfg);
    if (cp)
        gos_recompute_irq(cfg);

    cfg->debugger = debug_init();
    scheduler_start(cfg->scheduler);
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

static void gossamer_checkpoint_save(config_t *cfg, checkpoint_t *cp) {
    gossamer_state_t *st = gos_st(cfg);
    // Same relative order as the gossamer_init construction sequence.
    machine_checkpoint_save_core(cfg, cp);
    adb_checkpoint(cfg->adb, cp);
    av_cuda_checkpoint(st->cuda, cp);
    dbdma_checkpoint(st->dbdma, cp);
    floppy_checkpoint(cfg->floppy, cp);
    system_write_checkpoint_data(cp, &st->grackle.cfg, sizeof(st->grackle.cfg));
    system_write_checkpoint_data(cp, &st->grackle.cfg_addr, sizeof(st->grackle.cfg_addr));
    system_write_checkpoint_data(cp, &st->hr, sizeof(st->hr));
    system_write_checkpoint_data(cp, &st->i2c, sizeof(st->i2c));
    system_write_checkpoint_data(cp, &st->screamer, sizeof(st->screamer));
    pci_checkpoint_save(cfg->pci, cp);
    mac_checkpoint_save_images(cfg, cp);
    scsi_checkpoint(cfg->scsi, cp);
    mesh_checkpoint(st->mesh, cp);
    system_write_checkpoint_data(cp, &st->swim3, offsetof(swim3_t, fd));
    system_write_checkpoint_data(cp, &st->fdring, sizeof(st->fdring));
    gos_ata_checkpoint_save(cfg, cp);
    gos_bmac_checkpoint_save(cfg, cp);
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

static struct display *gossamer_display(config_t *cfg) {
    return pci_primary_display(cfg->pci);
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

// The MESH bus: internal bays and the external connector share it.
const struct scsi_slot gossamer_scsi_slots[] = {
    {.label = "SCSI HD0", .id = 0},
    {.label = "SCSI HD1", .id = 1},
    {0},
};

// The PCI topology: the three expansion sockets at devices $0D/$0E/$0F
// (the Grackle node's `slot-names 0000e000 "A1" "B1" "C1"`) with their
// strapped lines $17/$18/$19 (the ROM's device -> source pairs).  Grackle
// and Heathrow are the family's own devices, attached by grackle.c /
// heathrow.c, not slot entries.
const pci_slot_decl_t gossamer_pci_slots[] = {
    {.slot = 1,
     .kind = PCI_SLOT_SOCKET,
     .label = "A1",
     .bus = GOS_PCI_BUS,
     .device = GOS_DEV_SLOT_A1,
     .int_line = GOS_INT_SLOT_A1},
    {.slot = 2,
     .kind = PCI_SLOT_SOCKET,
     .label = "B1",
     .bus = GOS_PCI_BUS,
     .device = GOS_DEV_SLOT_B1,
     .int_line = GOS_INT_SLOT_B1},
    {.slot = 3,
     .kind = PCI_SLOT_SOCKET,
     .label = "C1",
     .bus = GOS_PCI_BUS,
     .device = GOS_DEV_SLOT_C1,
     .int_line = GOS_INT_SLOT_C1},
    // The on-board ATI Rage Pro, "slot" F1 in the firmware's slot names.
    // Declared LAST so a display card in a real slot, when one is seated,
    // is the primary display (pci_primary_display takes the first).
    {.slot = 4,
     .kind = PCI_SLOT_BUILTIN,
     .label = "F1",
     .bus = GOS_PCI_BUS,
     .device = GOS_DEV_ATI,
     .int_line = GOS_INT_ATI,
     .builtin_card_id = "ati_rage_pro"},
    {0},
};

const machine_substrate_t gossamer_substrate = {
    .init = gossamer_init,
    .bus_reset = gossamer_bus_reset,
    .power_on = gossamer_power_on,
    .teardown = gossamer_teardown,
    .checkpoint_save = gossamer_checkpoint_save,
    .pci_slot_irq = gossamer_pci_slot_irq,
    .trigger_vbl = gossamer_trigger_vbl,
    .fd_insert = gossamer_fd_insert,
    .fd_present = gossamer_fd_present,
    .input_key = mac_input_key,
    .input_mouse_move = mac_input_mouse_move,
    .input_mouse_button = mac_input_mouse_button,
    .display = gossamer_display,
    .media_detach = gos_media_detach,
    .media_attach = gos_media_attach,
    .media_present = gos_media_present,
    .media_eject = gos_media_eject,
};
