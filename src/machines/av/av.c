// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// av.c
// The Cyclone/Tempest AV family substrate (Quadra 840AV / Centris 660AV) —
// see av.h.  Implements the family lifecycle plus the pieces unique to this
// generation: the access-triggered ROM-at-zero overlay (no software overlay
// control exists — ymca.md §6), the YMCA 1-bit register file with the
// machine-ID straps, the CPU-ID register, the MUNI latches (with the 660AV
// bus-error probe behavior), and the family I/O island decode run on the
// shared mac030 engine.

#include "av.h"
#include "appletalk.h"

#include "civic.h"
#include "cuda.h"
#include "dsp.h"
#include "mace.h"
#include "new_age.h"
#include "psc.h"
#include "singer.h"
#include "vdc.h"

#include "mac_host_io.h" // mac_fd_*/mac_input_*
#include "mmu040.h"

#include "adb.h"
#include "checkpoint_images.h"
#include "cpu.h"
#include "cpu_internal.h" // cpu->mmu (attach the 040 walker to the bus resolver)
#include "debug.h"
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
#include "via.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("board");

static inline const av_board_t *av_board(config_t *cfg) {
    return (const av_board_t *)cfg->machine->board;
}

static inline av_state_t *av_st(config_t *cfg) {
    return (av_state_t *)cfg->machine_context;
}

// Raise a plain bus-timeout exception from a device window (the 660AV's
// MUNI_Control probe; same shape as the IIfx FMC probe window).
static void av_bus_error(uint32_t addr, bool read) {
    if (g_bus_error_pending)
        return;
    g_bus_error_pending = 1;
    g_bus_error_address = addr;
    g_bus_error_rw = read ? 1 : 0;
    g_bus_error_fc =
        read ? ((g_active_read == g_supervisor_read) ? 5 : 1) : ((g_active_write == g_supervisor_write) ? 5 : 1);
    g_bus_error_is_pmmu = 0;
    if (g_bus_error_instr_ptr)
        *g_bus_error_instr_ptr = 0;
}

// ============================================================
// YMCA register file ($50F30400; ymca.md §1)
// ============================================================
// Every register is one bit wide, addressed as a longword with the value in
// bit 31 — i.e. bit 7 of the big-endian MSB byte lane.  The engine
// decomposes wider accesses into bytes, so only lane 0 of each longword
// carries data.  The machine-ID straps read the board's nibble; everything
// else is a latch that reads back (speed/width semantics are not modelled —
// ymca.md §10 records that even the ROM only knows fixed patterns).

static uint8_t av_ymca_read(config_t *cfg, uint32_t addr) {
    av_state_t *st = av_st(cfg);
    uint32_t off = addr & 0x3FFu; // island offset $30400 + $000..$3FF
    if ((off & 3) != 0)
        return 0; // only the MSB byte lane carries the bit
    uint32_t idx = off >> 2;
    if (idx >= AV_YMCA_REG_COUNT)
        return 0;
    // Straps CPUID0..3 ($38/$3C/$40/$44): nibble bit n in strap register n.
    if (idx >= AV_YMCA_CPUID0 && idx < AV_YMCA_CPUID0 + 4) {
        uint8_t bit = (uint8_t)((av_board(cfg)->desc->strap_nibble >> (idx - AV_YMCA_CPUID0)) & 1);
        return (uint8_t)(bit << 7);
    }
    return (uint8_t)((st->ymca_regs[idx] & 1) << 7);
}

static void av_ymca_write(config_t *cfg, uint32_t addr, uint8_t value) {
    av_state_t *st = av_st(cfg);
    uint32_t off = addr & 0x3FFu;
    if ((off & 3) != 0)
        return;
    uint32_t idx = off >> 2;
    if (idx >= AV_YMCA_REG_COUNT)
        return;
    if (idx >= AV_YMCA_CPUID0 && idx < AV_YMCA_CPUID0 + 4)
        return; // straps are inputs
    st->ymca_regs[idx] = (uint8_t)((value >> 7) & 1);
    LOG(3, "YMCA write $%03X = %d (pc=%08X)", off, st->ymca_regs[idx], cpu_get_pc(cfg->cpu));
}

// ============================================================
// MUNI ($50F30000; muni.md)
// ============================================================
// Two latches: IntCntrl (+$00) and Control (+$08).  A 660AV without the
// NuBus adapter has no MUNI at all — reads AND writes of MUNI_Control must
// bus-error so the ROM's TestForMUNI clears MUNIExists (the speed-programming
// write in JumpIntoROM runs under a temp bus-error handler and is skipped).

static uint8_t av_muni_read(config_t *cfg, uint32_t addr) {
    av_state_t *st = av_st(cfg);
    uint32_t off = addr & 0x3FFu;
    uint32_t reg = off & ~3u;
    if (reg == AV_MUNI_CONTROL && !av_board(cfg)->desc->muni_present) {
        av_bus_error(addr, true);
        return 0xFF;
    }
    uint32_t v = (reg == AV_MUNI_CONTROL) ? st->muni_control : (reg == AV_MUNI_INTCNTRL) ? st->muni_intcntrl : 0;
    return (uint8_t)(v >> (8 * (3 - (off & 3))));
}

static void av_muni_write(config_t *cfg, uint32_t addr, uint8_t value) {
    av_state_t *st = av_st(cfg);
    uint32_t off = addr & 0x3FFu;
    uint32_t reg = off & ~3u;
    uint32_t shift = 8 * (3 - (off & 3));
    if (reg == AV_MUNI_CONTROL) {
        if (!av_board(cfg)->desc->muni_present) {
            av_bus_error(addr, false);
            return;
        }
        st->muni_control = (st->muni_control & ~(0xFFu << shift)) | ((uint32_t)value << shift);
        LOG(2, "MUNI Control = $%08X (pc=%08X)", st->muni_control, cpu_get_pc(cfg->cpu));
    } else if (reg == AV_MUNI_INTCNTRL) {
        st->muni_intcntrl = (st->muni_intcntrl & ~(0xFFu << shift)) | ((uint32_t)value << shift);
        LOG(2, "MUNI IntCntrl = $%08X (pc=%08X)", st->muni_intcntrl, cpu_get_pc(cfg->cpu));
    }
}

// ============================================================
// CPU-ID register ($5FFFFFFC = $A55A2830, read-only; ymca.md §2)
// ============================================================
// Registered as its own page-sized device window at $5FFFF000.  The ROM's
// GetCPUIDReg validates the $A55A signature AND that the location is not
// writable — writes are simply dropped, so the write-then-readback probe
// sees the constant and concludes "not writable".

#define AV_CPUID_VALUE 0xA55A2830u

static uint8_t av_cpuid_read8(void *ctx, uint32_t offset) {
    (void)ctx;
    if (offset >= 0xFFCu)
        return (uint8_t)(AV_CPUID_VALUE >> (8 * (3 - (offset & 3))));
    return 0xFF; // nothing else decodes in this page — float high
}

static uint16_t av_cpuid_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((av_cpuid_read8(ctx, offset) << 8) | av_cpuid_read8(ctx, offset + 1));
}

static uint32_t av_cpuid_read32(void *ctx, uint32_t offset) {
    return ((uint32_t)av_cpuid_read16(ctx, offset) << 16) | av_cpuid_read16(ctx, offset + 2);
}

static void av_cpuid_write8(void *ctx, uint32_t offset, uint8_t value) {
    (void)ctx;
    LOG(2, "CPU-ID write $%X = $%02X ignored (read-only)", offset, value);
}

static void av_cpuid_write16(void *ctx, uint32_t offset, uint16_t value) {
    av_cpuid_write8(ctx, offset, (uint8_t)value);
}

static void av_cpuid_write32(void *ctx, uint32_t offset, uint32_t value) {
    av_cpuid_write8(ctx, offset, (uint8_t)value);
}

// ============================================================
// I/O island decode ($50F00000, 256 KiB, mirrored at $50F40000; ref
// docs/README.md master memory map)
// ============================================================

#define AV_VIA_IO_PENALTY 16
#define AV_SCC_IO_PENALTY 2

// 53C96 window handlers (defined with the SCSI wiring below).
static uint8_t av_scsi_read(config_t *cfg, uint32_t addr);
static void av_scsi_write(config_t *cfg, uint32_t addr, uint8_t value);
static uint8_t av_scsi_pdma_read(config_t *cfg, uint32_t addr);
static void av_scsi_pdma_write(config_t *cfg, uint32_t addr, uint8_t value);

//   base     end      device            penalty          xform            rd wr  rd_fn/wr_fn      name
const mac030_io_range_t av_io_ranges[] = {
    {0x00000, 0x02000, MAC030_DEV_VIA1, AV_VIA_IO_PENALTY, MAC030_IO_MASK_A0, 0, 0, NULL, NULL, "via1", .esync = 1},
    {0x02000, 0x04000, 0, 0, MAC030_IO_NORMAL, 0, 0, av_psc_via2_read, av_psc_via2_write, "psc_via2"},
    {0x04000, 0x08000, MAC030_DEV_SCC, AV_SCC_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, NULL, NULL, "scc"},
    {0x08000, 0x08080, 0, 0, MAC030_IO_NORMAL, 0, 0, av_mace_prom_read, av_mace_prom_write, "mac_prom"},
    {0x18000, 0x18100, 0, 0, MAC030_IO_NORMAL, 0, 0, av_scsi_read, av_scsi_write, "scsi_53c96"},
    {0x18100, 0x18200, 0, 0, MAC030_IO_NORMAL, 0, 0, av_scsi_pdma_read, av_scsi_pdma_write, "scsi_rdma"},
    {0x1C000, 0x1C200, 0, 0, MAC030_IO_NORMAL, 0, 0, av_mace_read, av_mace_write, "mace"},
    {0x2A000, 0x2A200, 0, 0, MAC030_IO_NORMAL, 0, 0, av_new_age_read, av_new_age_write, "new_age"},
    {0x2E000, 0x2E100, 0, 0, MAC030_IO_NORMAL, 0, 0, av_civic_clk_read, av_civic_clk_write, "clock"},
    {0x30000, 0x30400, 0, 0, MAC030_IO_NORMAL, 0, 0, av_muni_read, av_muni_write, "muni"},
    {0x30400, 0x30800, 0, 0, MAC030_IO_NORMAL, 0, 0, av_ymca_read, av_ymca_write, "ymca"},
    {0x30800, 0x30C00, 0, 0, MAC030_IO_NORMAL, 0, 0, av_civic_seb_read, av_civic_seb_write, "sebastian"},
    {0x31000, 0x33000, 0, 0, MAC030_IO_NORMAL, 0, 0, av_psc_reg_read, av_psc_reg_write, "psc"},
    {0x36000, 0x38000, 0, 0, MAC030_IO_NORMAL, 0, 0, av_civic_read, av_civic_write, "civic"},
    {0}, // sentinel: end == 0
};

// Bind the family device set + board tables into the shared I/O engine.
static void av_io_bind(mac030_io_t *io, config_t *cfg, const av_board_desc_t *desc) {
    for (int i = 0; i < MAC030_DEV_COUNT; i++) {
        io->handle[i] = NULL;
        io->iface[i] = NULL;
    }
    io->handle[MAC030_DEV_VIA1] = cfg->via1;
    io->iface[MAC030_DEV_VIA1] = via_get_memory_interface(cfg->via1);
    if (cfg->scc) {
        io->handle[MAC030_DEV_SCC] = cfg->scc;
        io->iface[MAC030_DEV_SCC] = scc_get_memory_interface(cfg->scc);
    }
    io->ranges = desc->io_ranges;
    io->mirror_mask = desc->io_mirror_mask;
    io->cfg = cfg;
    io->unmapped_read = desc->io_unmapped_read;
}

// ============================================================
// IRQ routing (docs/README.md interrupt table)
// ============================================================

static const mac030_irq_route_t av_irq_routes_tbl[] = {
    {AV_IRQ_NMI,  7},
    {AV_IRQ_L6,   6},
    {AV_IRQ_L5,   5},
    {AV_IRQ_L4,   4},
    {AV_IRQ_L3,   3},
    {AV_IRQ_VIA2, 2},
    {AV_IRQ_VIA1, 1},
    {0,           0},
};

const mac030_irq_route_t *av_irq_routes(void) {
    return av_irq_routes_tbl;
}

void av_update_ipl(config_t *cfg, int source, bool active) {
    if (active)
        cfg->irq |= source;
    else
        cfg->irq &= ~source;
    int new_ipl = mac030_irq_resolve_ipl(av_irq_routes_tbl, (uint32_t)cfg->irq);
    cpu_set_ipl(cfg->cpu, new_ipl);
    cpu_reschedule();
}

// VIA1 interrupt line → IPL 1.
static void av_via1_irq(void *context, bool active) {
    av_update_ipl((config_t *)context, AV_IRQ_VIA1, active);
}

// ============================================================
// SCSI: the Curio's 53C96 at island $18000 ($10 register stride)
// ============================================================
// No pseudo-DMA on this platform (pdmaAddr = 0) — data moves through PSC
// channel 0, which the SCSI HAL POLLS (it never installs a channel-0
// handler).  The chip IRQ is level-sensitive into the PSC-VIA2 window,
// bits 3 and mirror 0 (curio.md §2, IMPLEMENTATION.md §5).

static uint8_t av_scsi_read(config_t *cfg, uint32_t addr) {
    return scsi_53c96_read(av_st(cfg)->scsi96, (addr & 0xFFu) >> 4);
}

static void av_scsi_write(config_t *cfg, uint32_t addr, uint8_t value) {
    scsi_53c96_write(av_st(cfg)->scsi96, (addr & 0xFFu) >> 4, value);
}

// Curio's rDMA pseudo-DMA port at +$100: the ROM's boot-time SCSI Manager
// (SCSIMgrHWPSC.a) streams 16-bit words through it — the engine
// byte-decomposes them, and byte order equals wire order.
static uint8_t av_scsi_pdma_read(config_t *cfg, uint32_t addr) {
    (void)addr;
    return scsi_53c96_pdma_read8(av_st(cfg)->scsi96);
}

static void av_scsi_pdma_write(config_t *cfg, uint32_t addr, uint8_t value) {
    (void)addr;
    scsi_53c96_pdma_write8(av_st(cfg)->scsi96, value);
}

// The chip INT drives ONLY the CB2-position bit (3).  psc.md lists bit 0 as
// a "SCSI mirror", but driving it as a second interrupt source feeds the
// ROM's pattern-indexed level-2 dispatcher combinations it never expects
// (IFR $09/$29): the SCSI service then runs on patterns whose table entries
// mis-classify, the manager's deferred-interrupt bookkeeping is left stale,
// and the next transaction's select is never issued — verified against the
// dossier CD image, where the boot hangs in SCSIComplete's phase wait with
// the mirror driven and reaches the desktop without it.
static void av_scsi96_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    av_state_t *st = av_st(cfg);
    if (!st || !st->psc)
        return;
    av_psc_via2_source(st->psc, AV_PSC_VIA2_SCSI_CB2, active);
}

// The channel-0 pump: the hardware's DREQ/DACK engine, functionally — while
// the chip has payload to move and the channel's active set is armed,
// shuttle bytes between the 53C96 FIFO and memory.  The SCSI HAL polls CIRQ
// in the CmdStat for completion and never installs a channel-0 handler.
//
// The phase gate matters: `scsi_53c96_dreq()` only reports "a DMA-mode
// transfer is armed", which stays true across a phase change, so without it
// the pump can drain bytes the CPU is about to read out of the FIFO during a
// STATUS or MESSAGE phase.
#define AV_SCSI_PUMP_NS  10000.0 // 10 us cadence
#define AV_SCSI_PUMP_MAX 2048 // bytes per firing (a CD sector burst)

// True while the bus is in a phase whose payload the DMA engine carries.
// DATA IN/OUT are the payload phases, and COMMAND is included because a
// DMA-mode select streams the CDB out of the same engine.  STATUS and the
// MESSAGE phases are always the CPU's through the FIFO, so pumping there
// would steal bytes the driver is about to read.
static bool av_scsi_data_phase(config_t *cfg) {
    int ph = scsi_get_bus_phase(cfg->scsi);
    return ph == scsi_data_in || ph == scsi_data_out || ph == scsi_command;
}

static void av_scsi_pump_event(void *source, uint64_t data) {
    (void)data;
    config_t *cfg = (config_t *)source;
    av_state_t *st = av_st(cfg);
    if (st && st->psc && st->scsi96 && cfg->scsi) {
        int dir = av_psc_dma_dir(st->psc, AV_PSC_DMA_SCSI);
        int moved = 0;
        while (dir >= 0 && moved < AV_SCSI_PUMP_MAX && av_scsi_data_phase(cfg) && scsi_53c96_dreq(st->scsi96)) {
            uint8_t byte;
            if (dir == 1) { // device → memory
                byte = scsi_53c96_pdma_read8(st->scsi96);
                if (av_psc_dma_device_in(st->psc, AV_PSC_DMA_SCSI, &byte, 1) != 1)
                    break;
            } else { // memory → device
                if (av_psc_dma_device_out(st->psc, AV_PSC_DMA_SCSI, &byte, 1) != 1)
                    break;
                scsi_53c96_pdma_write8(st->scsi96, byte);
            }
            moved++;
            dir = av_psc_dma_dir(st->psc, AV_PSC_DMA_SCSI);
        }
    }
    scheduler_new_cpu_event(cfg->scheduler, &av_scsi_pump_event, cfg, 0, 0, (uint64_t)AV_SCSI_PUMP_NS);
}

// PSC bus-master DMA: guest-physical accesses through the bus resolver
// (the sonic memory-hook pattern — the CPU MMU is deliberately not in the
// path).
static uint32_t av_psc_mem_read(void *context, uint32_t phys, unsigned width) {
    (void)context;
    if (width == 1)
        return mmu_read_physical_uint8(g_mmu, phys);
    if (width == 2)
        return mmu_read_physical_uint16(g_mmu, phys);
    return mmu_read_physical_uint32(g_mmu, phys);
}

static void av_psc_mem_write(void *context, uint32_t phys, uint32_t value, unsigned width) {
    (void)context;
    if (width == 1)
        mmu_write_physical_uint8(g_mmu, phys, (uint8_t)value);
    else if (width == 2)
        mmu_write_physical_uint16(g_mmu, phys, (uint16_t)value);
    else
        mmu_write_physical_uint32(g_mmu, phys, value);
}

// SCC chip INT → PSC level-4 SCCA/SCCB bits.  The chip has one INT line;
// the ROM's SccDecode handler reads SCC RR3 to find the channel, so both
// bits track the line.  (Guarded: scc_init fires this before the PSC is
// built.)
static void av_scc_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    av_state_t *st = av_st(cfg);
    if (!st || !st->psc)
        return;
    av_psc_level_source(st->psc, AV_PSC_L4, 1, active);
    av_psc_level_source(st->psc, AV_PSC_L4, 2, active);
}

// ============================================================
// RAM mapping
// ============================================================
// A flat map of the installed RAM at physical 0 is the CORRECT model for
// this platform, not a shortcut: the eight YMCA banks decode at a fixed
// 16 MB spacing ($00000000/$01000000/…, ymca.md §3 RamInfo), so any
// population of full banks is contiguous by construction, and a partial
// last bank simply ends early (probes above installed RAM read floating
// $FF, which is how the ROM's SizeMemory finds each bank's size — verified
// for 8/16/32 MB against the real sizing + Mod3Test).  The per-bank
// boundary/size registers still latch and read back (av_ymca_write); the
// ROM's merge pass re-programs them to the same contiguous layout they
// power up with, so they never change the decode.

static void av_map_ram(config_t *cfg) {
    uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
    uint32_t pages = cfg->ram_size >> PAGE_SHIFT;
    for (uint32_t p = 0; p < pages && (int)p < g_page_count; p++)
        mac030_fill_page(p, ram_base + (p << PAGE_SHIFT), true);
}

// ============================================================
// ROM-at-zero overlay (access-triggered; ymca.md §6)
// ============================================================
// While armed, the ROM aperture ($40800000-$40A00000) is registered as a
// device window: the first access drops the overlay — RAM appears at zero,
// the aperture pages become direct ROM pages — and the triggering access
// itself returns ROM data.  The reset PC ($0000002A from ROM offset 4)
// executes `JMP $40800074` as its very first instruction, so the drop
// happens before any RAM is touched.

// Fill the ROM aperture with direct pages of the 2 MB image.
static void av_fill_rom_aperture(config_t *cfg) {
    const av_board_desc_t *desc = av_board(cfg)->desc;
    uint32_t rom_size = cfg->machine->rom_size;
    uint32_t rom_pages = rom_size >> PAGE_SHIFT;
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, cfg->ram_size);
    uint32_t start_page = desc->rom_base >> PAGE_SHIFT;
    uint32_t end_page = desc->rom_end >> PAGE_SHIFT;
    for (uint32_t p = start_page; p < end_page && (int)p < g_page_count; p++)
        mac030_fill_page(p, rom_data + (((p - start_page) % rom_pages) << PAGE_SHIFT), false);
}

// Drop the overlay: RAM at zero, aperture direct.  Idempotent.
static void av_overlay_drop(config_t *cfg) {
    av_state_t *st = av_st(cfg);
    if (!st->rom_overlay)
        return;
    st->rom_overlay = false;
    LOG(1, "AV overlay drop: RAM at $00000000, ROM direct in aperture (pc=%08X)", cpu_get_pc(cfg->cpu));
    av_map_ram(cfg);
    av_fill_rom_aperture(cfg);
}

// Arm the overlay: ROM readable at zero, aperture pages routed to the
// trigger device.  Used at cold boot and by hardware RESET.
static void av_overlay_arm(config_t *cfg) {
    av_state_t *st = av_st(cfg);
    const av_board_desc_t *desc = av_board(cfg)->desc;
    uint32_t rom_size = cfg->machine->rom_size;
    uint32_t rom_pages = rom_size >> PAGE_SHIFT;
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, cfg->ram_size);

    st->rom_overlay = true;

    // ROM mapped read-only at zero (2 MB).
    for (uint32_t p = 0; p < rom_pages && (int)p < g_page_count; p++)
        mac030_fill_page(p, rom_data + (p << PAGE_SHIFT), false);

    // Route the aperture through the trigger device (page plumbing was done
    // once by memory_map_add; later arms re-point the pages manually).
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
// the ROM image.  `offset` is relative to the aperture base.
static inline uint8_t *av_rom_ptr(config_t *cfg, uint32_t offset) {
    uint32_t rom_size = cfg->machine->rom_size;
    return ram_native_pointer(cfg->mem_map, cfg->ram_size) + (offset % rom_size);
}

static uint8_t av_overlay_read8(void *ctx, uint32_t offset) {
    config_t *cfg = (config_t *)ctx;
    av_overlay_drop(cfg);
    return av_rom_ptr(cfg, offset)[0];
}

static uint16_t av_overlay_read16(void *ctx, uint32_t offset) {
    config_t *cfg = (config_t *)ctx;
    av_overlay_drop(cfg);
    uint8_t *p = av_rom_ptr(cfg, offset);
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t av_overlay_read32(void *ctx, uint32_t offset) {
    config_t *cfg = (config_t *)ctx;
    av_overlay_drop(cfg);
    uint8_t *p = av_rom_ptr(cfg, offset);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void av_overlay_write8(void *ctx, uint32_t offset, uint8_t value) {
    (void)value;
    av_overlay_drop((config_t *)ctx); // a write access also triggers the switch
    LOG(2, "ROM aperture write $%X ignored", offset);
}

static void av_overlay_write16(void *ctx, uint32_t offset, uint16_t value) {
    av_overlay_write8(ctx, offset, (uint8_t)value);
}

static void av_overlay_write32(void *ctx, uint32_t offset, uint32_t value) {
    av_overlay_write8(ctx, offset, (uint8_t)value);
}

// ============================================================
// Memory layout
// ============================================================

static void av_memory_layout(config_t *cfg) {
    av_state_t *st = av_st(cfg);
    const av_board_desc_t *desc = av_board(cfg)->desc;

    // I/O island: the serialized window at $50F00000 plus its non-serialized
    // alias at $50F40000, folded by the $3FFFF mirror mask.
    mac030_io_fill_interface(&st->io_interface);
    memory_map_add(cfg->mem_map, 0x50F00000u, 0x00080000u, "AV I/O", &st->io_interface, &st->io);

    // CPU-ID register page at $5FFFF000 (the register itself is $5FFFFFFC).
    st->cpuid_interface.read_uint8 = av_cpuid_read8;
    st->cpuid_interface.read_uint16 = av_cpuid_read16;
    st->cpuid_interface.read_uint32 = av_cpuid_read32;
    st->cpuid_interface.write_uint8 = av_cpuid_write8;
    st->cpuid_interface.write_uint16 = av_cpuid_write16;
    st->cpuid_interface.write_uint32 = av_cpuid_write32;
    memory_map_add(cfg->mem_map, 0x5FFFF000u, 0x00001000u, "CPU-ID", &st->cpuid_interface, cfg);

    // The overlay-trigger device for the ROM aperture is registered once;
    // arming/dropping only re-points page entries.
    st->overlay_interface.read_uint8 = av_overlay_read8;
    st->overlay_interface.read_uint16 = av_overlay_read16;
    st->overlay_interface.read_uint32 = av_overlay_read32;
    st->overlay_interface.write_uint8 = av_overlay_write8;
    st->overlay_interface.write_uint16 = av_overlay_write16;
    st->overlay_interface.write_uint32 = av_overlay_write32;
    memory_map_add(cfg->mem_map, desc->rom_base, desc->rom_end - desc->rom_base, "ROM aperture", &st->overlay_interface,
                   cfg);

    av_overlay_arm(cfg);
}

// ============================================================
// VIA1 callbacks (shared by both leaves)
// ============================================================
// Port B carries the Cuda handshake (PB3 TREQ in, PB4 BYTEACK out, PB5 TIP
// out — via1-cuda.md §2); the SR shift-out is a Cuda command byte.

void av_via1_output(void *context, uint8_t port, uint8_t value) {
    config_t *cfg = (config_t *)context;
    av_state_t *st = av_st(cfg);
    if (port == 1 && st && st->cuda)
        av_cuda_via1_pb_input(st->cuda, value);
}

void av_via1_shift_out(void *context, uint8_t byte) {
    config_t *cfg = (config_t *)context;
    av_state_t *st = av_st(cfg);
    if (st && st->cuda)
        av_cuda_via1_shift_input(st->cuda, byte);
}

// ============================================================
// Device construction (shared by both leaves)
// ============================================================

void av_build_devices(config_t *cfg, checkpoint_t *cp) {
    av_state_t *st = av_st(cfg);
    const av_board_desc_t *desc = av_board(cfg)->desc;

    // VIA1 idle input levels (via1-cuda.md §2).  Port A: PA0/PA1 are the
    // POST CheckLoopBack burn-in probe — held at differing levels so no
    // jumper is detected; PA7 vSCCWrReq idles high (no SCC request).
    via_input(cfg->via1, 0, 0, 1);
    via_input(cfg->via1, 0, 1, 0);
    via_input(cfg->via1, 0, 2, 0);
    via_input(cfg->via1, 0, 5, 0);
    via_input(cfg->via1, 0, 7, 1);
    // Port B: PB3 is Cuda TREQ (active-low, idle high).
    via_input(cfg->via1, 1, 3, 1);
    // CA1 (60 Hz) and the Cuda CB1/CB2 lines idle high.
    via_input_c(cfg->via1, 0, 0, 1);
    via_input_c(cfg->via1, 1, 0, 1);
    via_input_c(cfg->via1, 1, 1, 1);

    // The PSC interrupt controller + DMA engine (VIA2 window, L3-L6,
    // sndPhase, the 7 channels).
    st->psc = av_psc_init(cfg, cp);
    assert(st->psc != NULL);
    av_psc_set_memory_hooks(st->psc, av_psc_mem_read, av_psc_mem_write, cfg);

    // The DSP3210 aux core on the PSC's dspOverRun reset latch, and the
    // Singer sound frame engine that feeds it EXT1 ticks.
    st->dsp = av_dsp_init(cfg, cp);
    assert(st->dsp != NULL);
    av_psc_set_dsp_hook(st->psc, av_dsp_overrun_hook, st->dsp);
    st->singer = av_singer_init(cfg, cp);
    assert(st->singer != NULL);

    // ADB device state, serviced through Cuda packets (adb_iop_transact),
    // not the VIA shifter — pass NULL for the VIA (the IIsi/Egret pattern).
    st->adb = adb_init(NULL, cfg->scheduler, cp);
    cfg->adb = st->adb;

    // The behavioral Cuda on VIA1's shift register + PB3/PB4/PB5.
    st->cuda = av_cuda_init(cfg->via1, cfg->rtc, st->adb, cfg->scheduler, cp, /*mode3_clock=*/false);
    assert(st->cuda != NULL);

    // New Age FDC stub ("no drive" — ST3 = $FF).
    st->fdc = av_new_age_init(cfg, cp);
    assert(st->fdc != NULL);

    // MACE Ethernet register stub + address PROM (no wire).
    st->mace = av_mace_init(cfg, cp);
    assert(st->mace != NULL);

    // CIVIC + Sebastian video (Hi-Res 640x480 monitor, 2 MB VRAM).
    st->civic = av_civic_init(cfg, cp);
    assert(st->civic != NULL);

    // The video digitizer (DMSD + VDC + frame engine), reached through
    // Cuda pseudo-command $22 and CIVIC's video-in gates.
    st->vdc = av_vdc_init(cfg, cp);
    assert(st->vdc != NULL);
    av_cuda_attach_vdc(st->cuda, st->vdc);

    if (cp)
        mac_checkpoint_restore_images(cfg, cp);

    // SCSI: the bus/target model carries the disks and CD; the 53C96 chip
    // model fronts it through the external-initiator API.
    cfg->scsi = scsi_init(NULL, cp);
    st->scsi96 = scsi_53c96_init(cfg->scheduler, 25000000, cp);
    scsi_53c96_set_irq_callback(st->scsi96, av_scsi96_irq, cfg);
    scsi_53c96_attach_bus(st->scsi96, cfg->scsi);
    // The HAL polls PSC-VIA2 IFR bit 0 for the chip's DREQ (see psc.c).
    av_psc_set_dreq_query(st->psc, (av_psc_dreq_fn)scsi_53c96_dreq, st->scsi96);

    // The PSC channel-0 pump (the hardware's DREQ/DACK engine).
    scheduler_new_event_type(cfg->scheduler, "av", cfg, "scsi_pump", &av_scsi_pump_event);
    scheduler_new_cpu_event(cfg->scheduler, &av_scsi_pump_event, cfg, 0, 0, (uint64_t)AV_SCSI_PUMP_NS);

    // Bus-side physical resolver for the 040 walker: RAM decoded up to the
    // ROM base, the 2 MB ROM at $40800000.  ram aperture max = $40800000 so
    // RAM-sizing probes above installed memory read $FF, not bus-error.
    uint32_t ram_size = cfg->ram_size;
    uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, ram_size);
    st->bus_mmu =
        mmu_init(ram_base, ram_size, desc->rom_base, rom_data, cfg->machine->rom_size, desc->rom_base, desc->rom_end);
    assert(st->bus_mmu != NULL);
    g_mmu = st->bus_mmu;
    mmu_attach_mmu040(st->bus_mmu, (mmu040_state_t *)cfg->cpu->mmu);

    setup_images(cfg);

    // Bind the I/O island + CPU-ID + ROM aperture, then arm the overlay.
    av_io_bind(&st->io, cfg, desc);
    av_memory_layout(cfg);

    // VRAM pages + the $50036000 CIVIC alias layer over the flat map.
    av_civic_install_memory(cfg, st->civic);

    // NuBus super-slot and slot space bus-errors on probes (the ROM's slot
    // scan expects it even with no cards).
    memory_set_bus_error_range(cfg->mem_map, desc->bus_err_lo, desc->bus_err_hi);
}

// ============================================================
// Substrate lifecycle
// ============================================================

static void av_init(config_t *cfg, checkpoint_t *cp) {
    const av_board_t *board = av_board(cfg);
    av_state_t *st = calloc(1, sizeof(*st));
    assert(st != NULL);
    cfg->machine_context = st;

    // Shared core (mem_map, 68040 CPU from the profile, scheduler) + RTC +
    // SCC + the single VIA (there is no VIA2 chip on this platform).
    mac030_build_core(cfg, cp);
    if (cp)
        system_read_checkpoint_data(cp, &cfg->irq, sizeof(cfg->irq));

    cfg->rtc = rtc_init(cfg->scheduler, cp, true);
    cfg->scc = scc_init(NULL, cfg->scheduler, av_scc_irq, cfg, cp);
    scc_set_clocks(cfg->scc, 7833600, 3686400);

    // AppleTalk rides the SCC's LocalTalk channel, so it is built as soon as
    // the SCC exists — and, because the checkpoint stream is positional, in
    // the same relative place the save writes it (right after scc_checkpoint).
    appletalk_init(cfg->scheduler, cfg->scc, cp);

    uint8_t via_ff = via_freq_factor_for_clock(cfg->machine->freq);
    cfg->via1 =
        via_init(NULL, cfg->scheduler, via_ff, "via1", board->via1_output, board->via1_shift_out, av_via1_irq, cfg, cp);

    // Machine-specific tail (shared for both AV leaves).
    board->build_devices(cfg, cp);

    cfg->nubus = nubus_init(cfg, board->desc->slots, cp);

    // Substrate-private checkpoint tail.
    if (cp) {
        bool overlay = true;
        system_read_checkpoint_data(cp, &overlay, sizeof(overlay));
        system_read_checkpoint_data(cp, st->ymca_regs, sizeof(st->ymca_regs));
        system_read_checkpoint_data(cp, &st->muni_intcntrl, sizeof(st->muni_intcntrl));
        system_read_checkpoint_data(cp, &st->muni_control, sizeof(st->muni_control));
        nubus_checkpoint_restore(cfg->nubus, cp); // matches av_checkpoint_save
        if (!overlay)
            av_set_overlay(cfg, false);
        mmu_invalidate_tlb(st->bus_mmu);
        via_redrive_outputs(cfg->via1);
    }

    mac030_glue_finish(cfg, cp);
}

static void av_reset(config_t *cfg) {
    av_state_t *st = av_st(cfg);
    // Hardware RESET: overlay re-arms; the CPU-owned 040 MMU state is reset
    // by cpu_hardware_reset_040.
    av_overlay_arm(cfg);
    if (st->bus_mmu) {
        st->bus_mmu->enabled = false;
        mmu_invalidate_tlb(st->bus_mmu);
    }
}

static void av_teardown(config_t *cfg) {
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);
    av_state_t *st = av_st(cfg);
    if (st) {
        if (st->vdc) {
            av_vdc_delete(st->vdc);
            st->vdc = NULL;
        }
        if (st->civic) {
            av_civic_delete(st->civic);
            st->civic = NULL;
        }
        if (st->mace) {
            av_mace_delete(st->mace);
            st->mace = NULL;
        }
        if (st->scsi96) {
            scsi_53c96_delete(st->scsi96);
            st->scsi96 = NULL;
        }
        if (st->fdc) {
            av_new_age_delete(st->fdc);
            st->fdc = NULL;
        }
        if (st->cuda) {
            av_cuda_delete(st->cuda);
            st->cuda = NULL;
        }
        if (st->singer) {
            av_singer_delete(st->singer);
            st->singer = NULL;
        }
        if (st->dsp) {
            av_dsp_delete(st->dsp);
            st->dsp = NULL;
        }
        if (st->psc) {
            av_psc_delete(st->psc);
            st->psc = NULL;
        }
        if (st->adb) {
            adb_delete(st->adb);
            st->adb = NULL;
            cfg->adb = NULL;
        }
        if (st->bus_mmu) {
            mmu_delete(st->bus_mmu);
            st->bus_mmu = NULL;
        }
    }
    if (cfg->scsi) {
        scsi_delete(cfg->scsi);
        cfg->scsi = NULL;
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

static void av_checkpoint_save(config_t *cfg, checkpoint_t *cp) {
    av_state_t *st = av_st(cfg);
    memory_map_checkpoint(cfg->mem_map, cp);
    cpu_checkpoint(cfg->cpu, cp); // includes the 040 MMU register file
    scheduler_checkpoint(cfg->scheduler, cp);
    system_write_checkpoint_data(cp, &cfg->irq, sizeof(cfg->irq));
    rtc_checkpoint(cfg->rtc, cp);
    scc_checkpoint(cfg->scc, cp);
    appletalk_checkpoint(cp);
    via_checkpoint(cfg->via1, cp);
    // Device order mirrors the checkpoint READS in av_build_devices — the
    // stream is sequential, so save and restore must walk it identically.
    av_psc_checkpoint(st->psc, cp);
    av_dsp_checkpoint(st->dsp, cp);
    av_singer_checkpoint(st->singer, cp);
    adb_checkpoint(st->adb, cp);
    av_cuda_checkpoint(st->cuda, cp);
    av_new_age_checkpoint(st->fdc, cp);
    av_mace_checkpoint(st->mace, cp);
    av_civic_checkpoint(st->civic, cp);
    av_vdc_checkpoint(st->vdc, cp);
    mac_checkpoint_save_images(cfg, cp);
    if (cfg->scsi)
        scsi_checkpoint(cfg->scsi, cp);
    scsi_53c96_checkpoint(st->scsi96, cp);
    // Substrate-private tail (mirrored by the restore block in av_init).
    system_write_checkpoint_data(cp, &st->rom_overlay, sizeof(st->rom_overlay));
    system_write_checkpoint_data(cp, st->ymca_regs, sizeof(st->ymca_regs));
    system_write_checkpoint_data(cp, &st->muni_intcntrl, sizeof(st->muni_intcntrl));
    system_write_checkpoint_data(cp, &st->muni_control, sizeof(st->muni_control));
    // Card-side display state (VRAM, palette, active mode) — last before the
    // block below, so a machine that restores with fewer cards than it saved
    // short-reads here without shifting anything that follows (mdu.c:190,
    // pdm.c:537 use the same position).
    nubus_checkpoint_save(cfg->nubus, cp);
}

// VBL tick: VIA1 CA1 pulse (60 Hz reference) + the PSC's own 60.15 Hz
// level-6 source.
static void av_trigger_vbl(config_t *cfg) {
    av_state_t *st = av_st(cfg);
    via_input_c(cfg->via1, 0, 0, 0);
    via_input_c(cfg->via1, 0, 0, 1);
    if (st && st->psc)
        av_psc_tick60(st->psc);
    if (cfg->nubus)
        nubus_tick_vbl(cfg->nubus);
    image_tick_all(cfg);
}

// Primary display: the CIVIC scanout (substrate .display hook).
static struct display *av_display(config_t *cfg) {
    av_state_t *st = av_st(cfg);
    return (st && st->civic) ? av_civic_display(st->civic) : NULL;
}

const machine_substrate_t av_substrate = {
    .init = av_init,
    .reset = av_reset,
    .teardown = av_teardown,
    .checkpoint_save = av_checkpoint_save,
    .update_ipl = av_update_ipl, // VIA1→1, VIA2→2, L3-L6→3-6, NMI→7
    .trigger_vbl = av_trigger_vbl,
    .fd_insert = mac_fd_insert,
    .fd_present = mac_fd_present,
    .input_key = mac_input_key,
    .input_mouse_move = mac_input_mouse_move,
    .input_mouse_button = mac_input_mouse_button,
    .media_detach = system_media_detach_std,
    .media_attach = system_media_attach_std,
    .display = av_display,
};

// Public overlay control for checkpoint restore / tests.
void av_set_overlay(config_t *cfg, bool on) {
    if (on)
        av_overlay_arm(cfg);
    else
        av_overlay_drop(cfg);
}
