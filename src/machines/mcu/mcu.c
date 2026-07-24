// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mcu.c
// The MCU/Orwell family substrate (Quadra 700/900/950) — see mcu.h.
// Implements the family lifecycle plus the pieces unique to this generation:
// the access-triggered ROM-at-zero overlay, the accept-and-log MCU register
// file, and the Q700 I/O island decode table run on the shared mac030 engine.

#include "mcu.h"

#include "mac_host_io.h" // mac_fd_*/mac_input_*
#include "mmu040.h"

#include "adb.h"
#include "asc.h"
#include "checkpoint_images.h"
#include "cpu.h"
#include "cpu_internal.h" // cpu->mmu (attach the 040 walker to the bus resolver)
#include "dafb.h"
#include "debug.h"
#include "floppy.h"
#include "image.h"
#include "log.h"
#include "memory.h"
#include "mmu.h"
#include "nubus.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "scsi_53c96.h"
#include "sonic.h"
#include "via.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("mcu");

static inline const mcu_board_t *mcu_board(config_t *cfg) {
    return (const mcu_board_t *)cfg->machine->board;
}

static inline mcu_state_t *mcu_st(config_t *cfg) {
    return (mcu_state_t *)cfg->machine_context;
}

// ============================================================
// Accept-and-log handler windows (MCU / SONIC / MAC PROM / SCSI / YANCC)
// ============================================================
// Register semantics in these blocks are [U]/[R] (reference §8/§10/§16); the
// policy is Trap 24's: writes latch and read back, every first touch is
// logged so the boot ROM's access sequence becomes an RE artifact.

// --- MCU/Orwell register file ($5000E000, ref §8.2 [U]) ---

static uint8_t mcu_reg_read(config_t *cfg, uint32_t addr) {
    mcu_state_t *st = mcu_st(cfg);
    uint32_t off = addr & 0xFFFu;
    uint32_t idx = (off >> 2) % MCU_REG_COUNT;
    uint32_t v = st->mcu_regs[idx];
    if (!(st->mcu_touched & (1ull << (idx & 63))))
        LOG(2, "MCU read  $%03X -> $%08X (pc=%08X)", off, v, cpu_get_pc(cfg->cpu));
    return (uint8_t)(v >> (8 * (3 - (off & 3))));
}

static void mcu_reg_write(config_t *cfg, uint32_t addr, uint8_t value) {
    mcu_state_t *st = mcu_st(cfg);
    uint32_t off = addr & 0xFFFu;
    uint32_t idx = (off >> 2) % MCU_REG_COUNT;
    uint32_t shift = 8 * (3 - (off & 3));
    st->mcu_regs[idx] = (st->mcu_regs[idx] & ~(0xFFu << shift)) | ((uint32_t)value << shift);
    LOG(2, "MCU write $%03X = $%08X (pc=%08X)", off, st->mcu_regs[idx], cpu_get_pc(cfg->cpu));
    st->mcu_touched |= 1ull << (idx & 63);
}

// --- Ethernet MAC-address PROM ($50008000, ref §16 [R]) ---
// The Apple presentation the SONIC driver consumes (SonicEnet.a @GetAddr):
// bytes 0-5 hold the station address with each byte BIT-REVERSED (the
// driver's NormAddr undoes it), and the XOR of all eight bytes must equal
// $FF (checksum probed before the address is trusted).  The address here is
// the locally-administered 02:00:00:09:07:01 → bit-reversed 40 00 00 90 E0
// 80; byte 6 is zero and byte 7 makes the XOR come out to $FF.
static const uint8_t mcu_mac_prom[8] = {0x40, 0x00, 0x00, 0x90, 0xE0, 0x80, 0x00, 0x4F};

static uint8_t mcu_prom_read(config_t *cfg, uint32_t addr) {
    (void)cfg;
    return mcu_mac_prom[addr & 7u];
}

static void mcu_prom_write(config_t *cfg, uint32_t addr, uint8_t value) {
    (void)cfg;
    LOG(2, "MAC PROM write $%X = $%02X ignored (read-only)", addr & 7u, value);
}

// --- SONIC ($5000A000; 16-bit registers on 4-byte spacing, ref §16.2) ---
// The register value rides the LOW half of the 32-bit slot ([R] — current
// MAME maps it the same way); the engine byte-decomposes wider accesses, so
// reads serve bytes 2-3 of each slot and a write COMMITS when byte 3
// lands (the Quadra driver/tests use 32-bit accesses throughout — SonicEqu.a
// SONIC32).  Bytes 0-1 read as zero and their writes are ignored.

static uint8_t mcu_sonic_read(config_t *cfg, uint32_t addr) {
    mcu_state_t *st = mcu_st(cfg);
    uint32_t off = addr & 0xFFFu;
    uint32_t byte = off & 3u;
    if (byte < 2)
        return 0;
    uint16_t v = sonic_reg_read(st->sonic, off >> 2);
    return (byte == 2) ? (uint8_t)(v >> 8) : (uint8_t)v;
}

static void mcu_sonic_write(config_t *cfg, uint32_t addr, uint8_t value) {
    mcu_state_t *st = mcu_st(cfg);
    uint32_t off = addr & 0xFFFu;
    uint32_t byte = off & 3u;
    if (byte == 2)
        st->sonic_byte2 = value;
    else if (byte == 3)
        sonic_reg_write(st->sonic, off >> 2, (uint16_t)((st->sonic_byte2 << 8) | value));
}

// --- NCR 53C96 ($5000F000; registers on a 16-byte spacing, ref §6.4) ---

static uint8_t mcu_scsi_read(config_t *cfg, uint32_t addr) {
    return scsi_53c96_read(mcu_st(cfg)->scsi96, (addr & 0xFFu) >> 4);
}

static void mcu_scsi_write(config_t *cfg, uint32_t addr, uint8_t value) {
    scsi_53c96_write(mcu_st(cfg)->scsi96, (addr & 0xFFu) >> 4, value);
}

// --- SCSI 0 pseudo-DMA aperture ($5000F100): the TurboSCSI payload port.
// The engine byte-decomposes 16-bit accesses, so the byte hooks carry both
// widths in wire order (big-endian high byte first).

static uint8_t mcu_scsi_pdma_read(config_t *cfg, uint32_t addr) {
    (void)addr;
    return scsi_53c96_pdma_read8(mcu_st(cfg)->scsi96);
}

static void mcu_scsi_pdma_write(config_t *cfg, uint32_t addr, uint8_t value) {
    (void)addr;
    scsi_53c96_pdma_write8(mcu_st(cfg)->scsi96, value);
}

// --- YANCC ($50028000) — the system-bus/NuBus bridge register file.
// The register address is Apple-documented, its width and bits are not
// (ref §10.2 [A][U]): same latch-and-log policy as the MCU file, so the
// ROM's/driver's access sequence is recoverable as an RE artifact.  Actual
// NuBus transactions run through the memory map + nubus core directly; the
// write-buffer/error machinery this register controls is not modeled yet.

static uint8_t mcu_yancc_read(config_t *cfg, uint32_t addr) {
    mcu_state_t *st = mcu_st(cfg);
    uint32_t off = addr & 0x1FFFu;
    uint32_t idx = (off >> 2) % MCU_YANCC_REG_COUNT;
    uint32_t v = st->yancc_regs[idx];
    if (!(st->yancc_touched & (1ull << (idx & 63))))
        LOG(2, "YANCC read  $%04X -> $%08X (pc=%08X)", off, v, cpu_get_pc(cfg->cpu));
    return (uint8_t)(v >> (8 * (3 - (off & 3))));
}

static void mcu_yancc_write(config_t *cfg, uint32_t addr, uint8_t value) {
    mcu_state_t *st = mcu_st(cfg);
    uint32_t off = addr & 0x1FFFu;
    uint32_t idx = (off >> 2) % MCU_YANCC_REG_COUNT;
    uint32_t shift = 8 * (3 - (off & 3));
    st->yancc_regs[idx] = (st->yancc_regs[idx] & ~(0xFFu << shift)) | ((uint32_t)value << shift);
    LOG(2, "YANCC write $%04X = $%08X (pc=%08X)", off, st->yancc_regs[idx], cpu_get_pc(cfg->cpu));
    st->yancc_touched |= 1ull << (idx & 63);
}

// ============================================================
// Q700 I/O island decode ($50000000, 256 KiB, mirror $3FFFF; ref §6)
// ============================================================
// Direct low-speed I/O (no IOPs): VIA1/VIA2, direct SCC, direct SWIM, EASC,
// plus the handler windows above.  Penalties follow the MDU values until the
// JDB/Relayer per-device wait-state classes are measured (ref §9.3 Tier 1).

#define MCU_VIA_IO_PENALTY  16
#define MCU_SCC_IO_PENALTY  2
#define MCU_ASC_IO_PENALTY  2
#define MCU_SWIM_IO_PENALTY 5 // current MAME charges 5 CPU cycles [R]

//   base     end      device            penalty          xform            rd wr  rd_fn/wr_fn      name
const mac030_io_range_t mcu_q700_io_ranges[] = {
    {0x00000, 0x02000, MAC030_DEV_VIA1, MCU_VIA_IO_PENALTY, MAC030_IO_MASK_A0, 0, 0, NULL, NULL, "via1", .esync = 1},
    {0x02000, 0x04000, MAC030_DEV_VIA2, MCU_VIA_IO_PENALTY, MAC030_IO_MASK_A0, 0, 0, NULL, NULL, "via2", .esync = 1},
    {0x08000, 0x08008, 0, 0, MAC030_IO_NORMAL, 0, 0, mcu_prom_read, mcu_prom_write, "mac_prom"},
    {0x0A000, 0x0B100, 0, 0, MAC030_IO_NORMAL, 0, 0, mcu_sonic_read, mcu_sonic_write, "sonic"},
    {0x0C000, 0x0E000, MAC030_DEV_SCC, MCU_SCC_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, NULL, NULL, "scc"},
    {0x0E000, 0x0F000, 0, 0, MAC030_IO_NORMAL, 0, 0, mcu_reg_read, mcu_reg_write, "mcu"},
    {0x0F000, 0x0F100, 0, 0, MAC030_IO_NORMAL, 0, 0, mcu_scsi_read, mcu_scsi_write, "scsi_53c96"},
    {0x0F100, 0x0F102, 0, 0, MAC030_IO_NORMAL, 0, 0, mcu_scsi_pdma_read, mcu_scsi_pdma_write, "scsi_pdma"},
    {0x14000, 0x16000, MAC030_DEV_ASC, MCU_ASC_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, NULL, NULL, "easc"},
    {0x1E000, 0x20000, MAC030_DEV_FLOPPY, MCU_SWIM_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, NULL, NULL, "swim"},
    {0x28000, 0x2A000, 0, 0, MAC030_IO_NORMAL, 0, 0, mcu_yancc_read, mcu_yancc_write, "yancc"},
    {0}, // sentinel: end == 0
};

void mcu_io_bind(mac030_io_t *io, config_t *cfg, const mcu_board_desc_t *desc, void *asc, void *floppy) {
    for (int i = 0; i < MAC030_DEV_COUNT; i++) {
        io->handle[i] = NULL;
        io->iface[i] = NULL;
    }
    io->handle[MAC030_DEV_VIA1] = cfg->via1;
    io->handle[MAC030_DEV_VIA2] = cfg->via2;
    io->handle[MAC030_DEV_SCC] = cfg->scc;
    io->handle[MAC030_DEV_ASC] = asc;
    io->handle[MAC030_DEV_FLOPPY] = floppy;

    io->iface[MAC030_DEV_VIA1] = via_get_memory_interface(cfg->via1);
    io->iface[MAC030_DEV_VIA2] = via_get_memory_interface(cfg->via2);
    io->iface[MAC030_DEV_SCC] = scc_get_memory_interface(cfg->scc);
    io->iface[MAC030_DEV_ASC] = asc_get_memory_interface((asc_t *)asc);
    io->iface[MAC030_DEV_FLOPPY] = floppy_get_memory_interface((floppy_t *)floppy);

    io->ranges = desc->io_ranges;
    io->mirror_mask = desc->io_mirror_mask;
    io->cfg = cfg;
    io->unmapped_read = desc->io_unmapped_read;
}

// ============================================================
// /SLOTIRQ aggregation (ref §13.3)
// ============================================================
// The slot/video/Ethernet request lines land on VIA2 PA0-PA6 (active-low)
// and OR into the active-low /SLOTIRQ on VIA2 CA1.  Every source funnels
// through here so the aggregate stays consistent regardless of origin
// (DAFB video = PA6, SONIC = PA0, NuBus slots A-E = PA1-PA5).

void mcu_slot_irq_source(config_t *cfg, int pa_bit, bool active) {
    mcu_state_t *st = mcu_st(cfg);
    if (pa_bit < 0 || pa_bit > 6)
        return;
    uint8_t bit = (uint8_t)(1u << pa_bit);
    st->slot_pa_mask = active ? (st->slot_pa_mask | bit) : (st->slot_pa_mask & (uint8_t)~bit);
    via_input(cfg->via2, /*port A*/ 0, pa_bit, active ? 0 : 1); // active-low line
    via_input_c(cfg->via2, /*CA1*/ 0, 0, st->slot_pa_mask ? 0 : 1); // /SLOTIRQ = OR of sources
}

// substrate.nubus_slot_irq: a NuBus card's /NMRQ maps to VIA2 PA(slot-9)
// (slot $A→PA1 .. $E→PA5; ref §13.3).  Slot 9 is the built-in video and
// never arrives here — DAFB drives PA6 directly through the aggregate.
static void mcu_nubus_slot_irq(config_t *cfg, int slot, bool active, bool umbrella_edge) {
    (void)umbrella_edge; // CA1 derives from the family aggregate, not the bus's own OR
    int pa_bit = slot - 0x9;
    if (pa_bit < 1 || pa_bit > 5)
        return;
    mcu_slot_irq_source(cfg, pa_bit, active);
}

// ============================================================
// ROM-at-zero overlay (access-triggered; ref §4.3 [A])
// ============================================================
// While armed, the ROM aperture ($40000000-$4FFFFFFF) is registered as a
// device window: the first access drops the overlay — RAM appears at zero,
// the aperture pages become direct ROM mirrors — and the triggering access
// itself returns ROM data.  This matches the MCU's documented behavior
// without trapping every access after the drop.

// Fill the whole ROM aperture with direct 1 MiB mirrors of the ROM image.
static void mcu_fill_rom_aperture(config_t *cfg) {
    const mcu_board_desc_t *desc = mcu_board(cfg)->desc;
    uint32_t rom_size = cfg->machine->rom_size;
    uint32_t rom_pages = rom_size >> PAGE_SHIFT;
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, cfg->ram_size);
    uint32_t start_page = desc->rom_base >> PAGE_SHIFT;
    uint32_t end_page = desc->rom_end >> PAGE_SHIFT;
    for (uint32_t p = start_page; p < end_page && (int)p < g_page_count; p++)
        mac030_fill_page(p, rom_data + (((p - start_page) % rom_pages) << PAGE_SHIFT), false);
}

// Drop the overlay: RAM at zero, aperture direct.  Idempotent.
static void mcu_overlay_drop(config_t *cfg) {
    mcu_state_t *st = mcu_st(cfg);
    if (!st->rom_overlay)
        return;
    st->rom_overlay = false;
    LOG(1, "MCU overlay drop: RAM at $00000000, ROM direct in aperture (pc=%08X)", cpu_get_pc(cfg->cpu));

    // RAM appears at zero (flat model, ref §8.5's sanctioned compromise).
    uint32_t ram_pages = cfg->ram_size >> PAGE_SHIFT;
    uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
    for (uint32_t p = 0; p < ram_pages && (int)p < g_page_count; p++)
        mac030_fill_page(p, ram_base + (p << PAGE_SHIFT), true);

    // The aperture switches from the trigger device to direct ROM mirrors
    // (mac030_fill_page overwrites the device page entries).
    mcu_fill_rom_aperture(cfg);
}

// Arm the overlay: ROM readable at zero, aperture pages routed to the
// trigger device.  Used at cold boot and by hardware RESET.
static void mcu_overlay_arm(config_t *cfg) {
    mcu_state_t *st = mcu_st(cfg);
    const mcu_board_desc_t *desc = mcu_board(cfg)->desc;
    uint32_t rom_size = cfg->machine->rom_size;
    uint32_t rom_pages = rom_size >> PAGE_SHIFT;
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, cfg->ram_size);

    st->rom_overlay = true;

    // ROM mapped read-only at zero (1 MiB; aliasing above the image is [U] —
    // unmapped reads return $FF, the conservative choice per ref §4.3).
    for (uint32_t p = 0; p < rom_pages && (int)p < g_page_count; p++)
        mac030_fill_page(p, rom_data + (p << PAGE_SHIFT), false);

    // Route the aperture through the trigger device: reuse memory_map_add's
    // page plumbing once, then re-point the pages manually on later arms.
    uint32_t start_page = desc->rom_base >> PAGE_SHIFT;
    uint32_t end_page = desc->rom_end >> PAGE_SHIFT;
    for (uint32_t p = start_page; p < end_page && (int)p < g_page_count; p++) {
        g_page_table[p].host_base = NULL;
        g_page_table[p].dev = &st->overlay_interface;
        g_page_table[p].dev_context = cfg;
        g_page_table[p].base_addr = desc->rom_base;
        g_page_table[p].writable = false;
        if (g_supervisor_read)
            g_supervisor_read[p] = 0;
        if (g_supervisor_write)
            g_supervisor_write[p] = 0;
        if (g_user_read)
            g_user_read[p] = 0;
        if (g_user_write)
            g_user_write[p] = 0;
    }
}

// Trigger-device handlers: any access drops the overlay and completes from
// the ROM image.  `offset` is relative to the aperture base; the image
// repeats every 1 MiB.
static inline uint8_t *mcu_rom_ptr(config_t *cfg, uint32_t offset) {
    uint32_t rom_size = cfg->machine->rom_size;
    return ram_native_pointer(cfg->mem_map, cfg->ram_size) + (offset % rom_size);
}

static uint8_t mcu_overlay_read8(void *ctx, uint32_t offset) {
    config_t *cfg = (config_t *)ctx;
    mcu_overlay_drop(cfg);
    return mcu_rom_ptr(cfg, offset)[0];
}

static uint16_t mcu_overlay_read16(void *ctx, uint32_t offset) {
    config_t *cfg = (config_t *)ctx;
    mcu_overlay_drop(cfg);
    uint8_t *p = mcu_rom_ptr(cfg, offset);
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t mcu_overlay_read32(void *ctx, uint32_t offset) {
    config_t *cfg = (config_t *)ctx;
    mcu_overlay_drop(cfg);
    uint8_t *p = mcu_rom_ptr(cfg, offset);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void mcu_overlay_write8(void *ctx, uint32_t offset, uint8_t value) {
    (void)value;
    mcu_overlay_drop((config_t *)ctx); // a write access also triggers the switch
    LOG(2, "ROM aperture write $%X ignored", offset);
}

static void mcu_overlay_write16(void *ctx, uint32_t offset, uint16_t value) {
    mcu_overlay_write8(ctx, offset, (uint8_t)value);
}

static void mcu_overlay_write32(void *ctx, uint32_t offset, uint32_t value) {
    mcu_overlay_write8(ctx, offset, (uint8_t)value);
}

// ============================================================
// Memory layout
// ============================================================

static void mcu_memory_layout_init(config_t *cfg) {
    mcu_state_t *st = mcu_st(cfg);
    const mcu_board_desc_t *desc = mcu_board(cfg)->desc;

    // I/O island at $50000000 (256 KiB block; the published map mirrors it
    // through $53FFFFFF, current RE through $50FFFFFF — we register the
    // Apple-documented extent and let the mirror mask fold accesses; ref §6).
    mac030_io_fill_interface(&st->io_interface);
    memory_map_add(cfg->mem_map, 0x50000000u, 0x04000000u, "MCU I/O", &st->io_interface, &st->io);

    // DAFB registers at $F9800000; VRAM pages direct at $F9000000.
    memory_map_add(cfg->mem_map, DAFB_REG_BASE, DAFB_REG_APERTURE, "DAFB regs",
                   (memory_interface_t *)dafb_reg_interface(st->dafb), st->dafb);
    uint8_t *vram = dafb_vram(st->dafb);
    uint32_t vram_pages = dafb_vram_size(st->dafb) >> PAGE_SHIFT;
    uint32_t vram_start = DAFB_VRAM_BASE >> PAGE_SHIFT;
    for (uint32_t i = 0; i < vram_pages && (int)(vram_start + i) < g_page_count; i++)
        mac030_fill_page(vram_start + i, vram + (i << PAGE_SHIFT), true);
    // Register VRAM with the bus resolver so 040 table walks / TT matches
    // reaching physical $F9xxxxxx resolve to the buffer.
    memory_map_host_region(cfg->mem_map, "dafb_vram", vram, DAFB_VRAM_BASE, dafb_vram_size(st->dafb),
                           /*writable*/ true);

    // The overlay-trigger device for the ROM aperture is registered once;
    // arming/dropping only re-points page entries.
    st->overlay_interface.read_uint8 = mcu_overlay_read8;
    st->overlay_interface.read_uint16 = mcu_overlay_read16;
    st->overlay_interface.read_uint32 = mcu_overlay_read32;
    st->overlay_interface.write_uint8 = mcu_overlay_write8;
    st->overlay_interface.write_uint16 = mcu_overlay_write16;
    st->overlay_interface.write_uint32 = mcu_overlay_write32;
    memory_map_add(cfg->mem_map, desc->rom_base, desc->rom_end - desc->rom_base, "ROM aperture", &st->overlay_interface,
                   cfg);

    mcu_overlay_arm(cfg);
}

// ============================================================
// Substrate lifecycle
// ============================================================

static void mcu_init(config_t *cfg, checkpoint_t *cp) {
    const mcu_board_t *board = mcu_board(cfg);
    mcu_state_t *st = calloc(1, sizeof(*st));
    assert(st != NULL);
    cfg->machine_context = st;
    st->last_port_b = 0x30; // ADB ST1:ST0 idle = 11

    // Shared core (mem_map, 68040 CPU from the profile, scheduler) + RTC +
    // SCC + the two VIAs.
    mac030_build_core(cfg, cp);
    if (cp)
        system_read_checkpoint_data(cp, &cfg->irq, sizeof(cfg->irq));

    cfg->rtc = rtc_init(cfg->scheduler, cp, true);
    cfg->scc = scc_init(NULL, cfg->scheduler, mac030_glue_scc_irq, cfg, cp);
    scc_set_clocks(cfg->scc, 7833600, 3686400);

    cfg->via1 = via_init(NULL, cfg->scheduler, 20, "via1", board->via1_output, board->via1_shift_out,
                         mac030_glue_via1_irq, cfg, cp);
    cfg->via2 = via_init(NULL, cfg->scheduler, 21, "via2", board->via2_output, NULL, mac030_glue_via2_irq, cfg, cp);

    // Machine-specific tail: straps, ADB, EASC, SWIM, DAFB, bus resolver,
    // memory layout, checkpoint restore.
    board->build_devices(cfg, cp);

    // NuBus (Phase F): seat the declared slot cards; their windows layer
    // over the bus-error range, and slot IRQs route through the substrate's
    // nubus_slot_irq into the VIA2 PA aggregate.
    cfg->nubus = nubus_init(cfg, board->desc->slots, cp);
    // Project the cards' host regions (VRAM, declaration ROMs) into the
    // page table so CPU accesses resolve with the MMU off; the bus
    // resolver serves the 040 walker when it's on.  No Mode-24 aliases —
    // this family's ROM and System are 32-bit clean, and a low alias would
    // shadow RAM at $00s00000 on large-memory configurations.
    mmu_host_regions_fill_pages(st->bus_mmu, mac030_fill_page, /*mode24_alias*/ false);

    mac030_glue_finish(cfg, cp);
}

static void mcu_reset(config_t *cfg) {
    mcu_state_t *st = mcu_st(cfg);
    // Hardware RESET: overlay re-arms; the CPU-owned 040 MMU state is reset
    // by cpu_hardware_reset_040; DAFB registers clear.
    mcu_overlay_arm(cfg);
    if (st->dafb)
        dafb_reset(st->dafb);
    if (st->bus_mmu) {
        st->bus_mmu->enabled = false;
        mmu_invalidate_tlb(st->bus_mmu);
    }
}

static void mcu_teardown(config_t *cfg) {
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);
    mcu_state_t *st = mcu_st(cfg);
    if (st) {
        if (st->sonic) {
            sonic_delete(st->sonic);
            st->sonic = NULL;
        }
        if (st->scsi96) {
            scsi_53c96_delete(st->scsi96);
            st->scsi96 = NULL;
        }
        if (st->dafb) {
            dafb_delete(st->dafb);
            st->dafb = NULL;
        }
        if (st->bus_mmu) {
            mmu_delete(st->bus_mmu);
            st->bus_mmu = NULL;
        }
        if (st->floppy) {
            floppy_delete(st->floppy);
            st->floppy = NULL;
            cfg->floppy = NULL;
        }
        if (st->asc) {
            asc_delete(st->asc);
            st->asc = NULL;
        }
        if (st->adb) {
            adb_delete(st->adb);
            st->adb = NULL;
            cfg->adb = NULL;
        }
    }
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
    if (st) {
        free(st);
        cfg->machine_context = NULL;
    }
}

static void mcu_checkpoint_save(config_t *cfg, checkpoint_t *cp) {
    mcu_state_t *st = mcu_st(cfg);
    memory_map_checkpoint(cfg->mem_map, cp);
    cpu_checkpoint(cfg->cpu, cp); // includes the 040 MMU register file
    scheduler_checkpoint(cfg->scheduler, cp);
    system_write_checkpoint_data(cp, &cfg->irq, sizeof(cfg->irq));
    rtc_checkpoint(cfg->rtc, cp);
    scc_checkpoint(cfg->scc, cp);
    via_checkpoint(cfg->via1, cp);
    via_checkpoint(cfg->via2, cp);
    adb_checkpoint(st->adb, cp);
    mac_checkpoint_save_images(cfg, cp);
    if (cfg->scsi)
        scsi_checkpoint(cfg->scsi, cp);
    asc_checkpoint(st->asc, cp);
    floppy_checkpoint(st->floppy, cp);
    scsi_53c96_checkpoint(st->scsi96, cp);
    sonic_checkpoint(st->sonic, cp);
    dafb_checkpoint(st->dafb, cp);
    // Substrate-private state: overlay flag + MCU/YANCC register files +
    // the /SLOTIRQ aggregate mask.
    system_write_checkpoint_data(cp, &st->rom_overlay, sizeof(st->rom_overlay));
    system_write_checkpoint_data(cp, st->mcu_regs, sizeof(st->mcu_regs));
    system_write_checkpoint_data(cp, st->yancc_regs, sizeof(st->yancc_regs));
    system_write_checkpoint_data(cp, &st->slot_pa_mask, sizeof(st->slot_pa_mask));
}

// VBL tick: VIA1 CA1 pulse (the 60.15 Hz VIA2-PB7 chain, functionally;
// video interrupts come from the Swatch's programmed timing in dafb.c —
// Traps 9/10 apply to the Swatch IRQs, not this line).
static void mcu_trigger_vbl(config_t *cfg) {
    via_input_c(cfg->via1, 0, 0, 0);
    via_input_c(cfg->via1, 0, 0, 1);
    nubus_tick_vbl(cfg->nubus);
    image_tick_all(cfg);
}

// Primary display: the DAFB scanout (substrate .display hook).
static struct display *mcu_display(config_t *cfg) {
    mcu_state_t *st = mcu_st(cfg);
    return (st && st->dafb) ? dafb_display(st->dafb) : NULL;
}

const machine_substrate_t mcu_substrate = {
    .init = mcu_init,
    .reset = mcu_reset,
    .teardown = mcu_teardown,
    .checkpoint_save = mcu_checkpoint_save,
    .update_ipl = mac030_glue_update_ipl, // VIA1→1, VIA2→2, SCC→4, NMI→7 (ref §13)
    .trigger_vbl = mcu_trigger_vbl,
    .nubus_slot_irq = mcu_nubus_slot_irq, // slots → VIA2 PA1-PA5 + /SLOTIRQ aggregate
    .fd_insert = mac_fd_insert,
    .fd_present = mac_fd_present,
    .input_key = mac_input_key,
    .input_mouse_move = mac_input_mouse_move,
    .input_mouse_button = mac_input_mouse_button,
    .display = mcu_display,
};

// Shared layout entry for the machine files (called from build_devices once
// the DAFB and bus resolver exist).
void mcu_memory_layout(config_t *cfg) {
    mcu_memory_layout_init(cfg);
}

// Public overlay control for checkpoint restore: layout leaves the overlay
// armed; a restore of a post-overlay state drops it again.
void mcu_set_overlay(config_t *cfg, bool on) {
    if (on)
        mcu_overlay_arm(cfg);
    else
        mcu_overlay_drop(cfg);
}
