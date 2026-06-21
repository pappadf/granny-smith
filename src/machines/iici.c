// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iici.c
// Macintosh IIci ("Aurora", 25 MHz 68030, 1989) machine implementation.
// Architecturally the IIcx with VIA2 replaced by the RBV chip and built-in
// video reading from the slot-$B framebuffer aperture.  See
// proposal-machine-iici-iisi.md and docs/iici.md.
//
// Diff vs iicx.c at a glance:
//   * I/O island: VIA2 ($2000) dropped; VDAC ($24000) and RBV ($26000)
//     added.  The mirror mask widens to $3FFFF so RBV/VDAC decode
//     distinctly from the SCSI windows.
//   * ROM base is $40800000 (MDUtable), not $40000000.
//   * Interrupts: RBV's combined IFR drives IPL 2 (replacing VIA2); SCSI
//     IRQ/DRQ route through the RBV via scsi_set_irq_callback (no VIA2).
//   * Soft power-off: RBV RvPowerOff (RvDataB bit 2) instead of VIA2 PB2.
//   * Built-in video: a NuBus pseudo-card in slot $0 whose framebuffer the
//     machine maps at $FBB00000; depth follows RvMonP via a mode callback.
//   * ADB / RTC use the classic VIA1 path (no Egret) — identical to IIcx.

#include "mac030_glue.h"
#include "machine.h"
#include "mmu_checkpoint.h"
#include "system_config.h"

#include "adb.h"
#include "asc.h"
#include "builtin_rbv_video.h"
#include "checkpoint_images.h"
#include "checkpoint_machine.h"
#include "cpu.h"
#include "cpu_internal.h" // for cpu->mmu field
#include "debug.h"
#include "floppy.h"
#include "iici_internal.h"
#include "image.h"
#include "log.h"
#include "memory.h"
#include "mmu.h"
#include "nubus.h"
#include "rbv.h"
#include "rom.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "shell.h"
#include "via.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("iici");

// ============================================================
// I/O island offsets (private to the dispatcher)
// ============================================================

#define IO_VIA1           0x00000
#define IO_VIA1_END       0x02000
#define IO_SCC            0x04000
#define IO_SCC_END        0x06000
#define IO_SCSI_DRQ       0x06000
#define IO_SCSI_DRQ_END   0x08000
#define IO_SCSI_REG       0x10000
#define IO_SCSI_REG_END   0x12000
#define IO_SCSI_BLIND     0x12000
#define IO_SCSI_BLIND_END 0x14000
#define IO_ASC            0x14000
#define IO_ASC_END        0x16000
#define IO_SWIM           0x16000
#define IO_SWIM_END       0x18000
#define IO_VDAC           0x24000
#define IO_VDAC_END       0x26000
#define IO_RBV            0x26000
#define IO_RBV_END        0x28000

// ============================================================
// SoA page helper (same logic as the SE/30 / IIcx helper)
// ============================================================

// ============================================================
// ROM overlay
// ============================================================

static void iici_set_rom_overlay(config_t *cfg, bool overlay) {
    iici_state_t *st = iici_state(cfg);
    if (st->rom_overlay == overlay)
        return;
    st->rom_overlay = overlay;
    uint32_t rom_size = cfg->machine->rom_size;
    uint32_t rom_pages = rom_size >> PAGE_SHIFT;
    uint32_t rom_start_page = IICI_ROM_START >> PAGE_SHIFT;
    if (overlay) {
        for (uint32_t p = 0; p < rom_pages && (int)p < g_page_count; p++) {
            uint8_t *host_ptr = g_page_table[rom_start_page + p].host_base;
            mac030_fill_page(p, host_ptr, false);
        }
    } else {
        uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
        for (uint32_t p = 0; p < rom_pages && (int)p < g_page_count; p++)
            mac030_fill_page(p, ram_base + (p << PAGE_SHIFT), true);
    }
}

// ============================================================
// Hardware reset
// ============================================================

static void iici_reset(config_t *cfg) {
    iici_state_t *st = iici_state(cfg);
    st->rom_overlay = false;
    iici_set_rom_overlay(cfg, true);
    if (st->mmu) {
        st->mmu->enabled = false;
        st->mmu->tc = 0;
        mmu_invalidate_tlb(st->mmu);
    }
}

// ============================================================
// I/O dispatcher (MDU island — IIcx map minus VIA2, plus VDAC + RBV)
// ============================================================

static uint8_t iici_io_read_uint8(void *ctx, uint32_t addr) {
    config_t *cfg = (config_t *)ctx;
    iici_state_t *st = iici_state(cfg);
    uint32_t offset = addr & IICI_IO_MIRROR;
    if (offset < IO_VIA1_END) {
        memory_io_penalty(IICI_VIA_IO_PENALTY);
        return st->via1_iface->read_uint8(cfg->via1, (offset - IO_VIA1) & ~1u);
    }
    if (offset >= IO_SCC && offset < IO_SCC_END) {
        memory_io_penalty(IICI_SCC_IO_PENALTY);
        return st->scc_iface->read_uint8(cfg->scc, offset - IO_SCC);
    }
    if (offset >= IO_SCSI_DRQ && offset < IO_SCSI_DRQ_END) {
        memory_io_penalty(IICI_SCSI_IO_PENALTY);
        return st->scsi_iface->read_uint8(cfg->scsi, 0);
    }
    if (offset >= IO_SCSI_REG && offset < IO_SCSI_REG_END) {
        memory_io_penalty(IICI_SCSI_IO_PENALTY);
        return st->scsi_iface->read_uint8(cfg->scsi, offset - IO_SCSI_REG);
    }
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        memory_io_penalty(IICI_SCSI_IO_PENALTY);
        return st->scsi_iface->read_uint8(cfg->scsi, 0);
    }
    if (offset >= IO_ASC && offset < IO_ASC_END) {
        memory_io_penalty(IICI_ASC_IO_PENALTY);
        return st->asc_iface->read_uint8(st->asc, offset - IO_ASC);
    }
    if (offset >= IO_SWIM && offset < IO_SWIM_END) {
        memory_io_penalty(IICI_SWIM_IO_PENALTY);
        return st->floppy_iface->read_uint8(st->floppy, offset - IO_SWIM);
    }
    if (offset >= IO_VDAC && offset < IO_VDAC_END) {
        memory_io_penalty(IICI_VDAC_IO_PENALTY);
        return builtin_rbv_video_vdac_read(st->video_card, offset - IO_VDAC);
    }
    if (offset >= IO_RBV && offset < IO_RBV_END) {
        memory_io_penalty(IICI_RBV_IO_PENALTY);
        return st->rbv_iface->read_uint8(st->rbv, offset - IO_RBV);
    }
    return 0;
}

static uint16_t iici_io_read_uint16(void *ctx, uint32_t addr) {
    return ((uint16_t)iici_io_read_uint8(ctx, addr) << 8) | iici_io_read_uint8(ctx, addr + 1);
}

static uint32_t iici_io_read_uint32(void *ctx, uint32_t addr) {
    config_t *cfg = (config_t *)ctx;
    iici_state_t *st = iici_state(cfg);
    uint32_t offset = addr & IICI_IO_MIRROR;
    if (offset >= IO_SCSI_DRQ && offset < IO_SCSI_DRQ_END) {
        memory_io_penalty(IICI_SCSI_IO_PENALTY * 4);
        uint8_t b0 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b1 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b2 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b3 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        return ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | (uint32_t)b3;
    }
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        memory_io_penalty(IICI_SCSI_IO_PENALTY * 4);
        uint8_t b0 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b1 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b2 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b3 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        return ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | (uint32_t)b3;
    }
    return ((uint32_t)iici_io_read_uint16(ctx, addr) << 16) | iici_io_read_uint16(ctx, addr + 2);
}

static void iici_io_write_uint8(void *ctx, uint32_t addr, uint8_t value) {
    config_t *cfg = (config_t *)ctx;
    iici_state_t *st = iici_state(cfg);
    uint32_t offset = addr & IICI_IO_MIRROR;
    if (offset < IO_VIA1_END) {
        memory_io_penalty(IICI_VIA_IO_PENALTY);
        st->via1_iface->write_uint8(cfg->via1, (offset - IO_VIA1) & ~1u, value);
        return;
    }
    if (offset >= IO_SCC && offset < IO_SCC_END) {
        memory_io_penalty(IICI_SCC_IO_PENALTY);
        st->scc_iface->write_uint8(cfg->scc, offset - IO_SCC, value);
        return;
    }
    if (offset >= IO_SCSI_DRQ && offset < IO_SCSI_DRQ_END) {
        memory_io_penalty(IICI_SCSI_IO_PENALTY);
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, value);
        return;
    }
    if (offset >= IO_SCSI_REG && offset < IO_SCSI_REG_END) {
        memory_io_penalty(IICI_SCSI_IO_PENALTY);
        st->scsi_iface->write_uint8(cfg->scsi, offset - IO_SCSI_REG, value);
        return;
    }
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        memory_io_penalty(IICI_SCSI_IO_PENALTY);
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, value);
        return;
    }
    if (offset >= IO_ASC && offset < IO_ASC_END) {
        memory_io_penalty(IICI_ASC_IO_PENALTY);
        st->asc_iface->write_uint8(st->asc, offset - IO_ASC, value);
        return;
    }
    if (offset >= IO_SWIM && offset < IO_SWIM_END) {
        memory_io_penalty(IICI_SWIM_IO_PENALTY);
        st->floppy_iface->write_uint8(st->floppy, offset - IO_SWIM, value);
        return;
    }
    if (offset >= IO_VDAC && offset < IO_VDAC_END) {
        memory_io_penalty(IICI_VDAC_IO_PENALTY);
        builtin_rbv_video_vdac_write(st->video_card, offset - IO_VDAC, value);
        return;
    }
    if (offset >= IO_RBV && offset < IO_RBV_END) {
        memory_io_penalty(IICI_RBV_IO_PENALTY);
        st->rbv_iface->write_uint8(st->rbv, offset - IO_RBV, value);
        return;
    }
}

static void iici_io_write_uint16(void *ctx, uint32_t addr, uint16_t value) {
    iici_io_write_uint8(ctx, addr, (uint8_t)(value >> 8));
    iici_io_write_uint8(ctx, addr + 1, (uint8_t)(value & 0xFF));
}

static void iici_io_write_uint32(void *ctx, uint32_t addr, uint32_t value) {
    config_t *cfg = (config_t *)ctx;
    iici_state_t *st = iici_state(cfg);
    uint32_t offset = addr & IICI_IO_MIRROR;
    if (offset >= IO_SCSI_DRQ && offset < IO_SCSI_DRQ_END) {
        memory_io_penalty(IICI_SCSI_IO_PENALTY * 4);
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 24));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 16));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 8));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value));
        return;
    }
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        memory_io_penalty(IICI_SCSI_IO_PENALTY * 4);
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 24));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 16));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 8));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value));
        return;
    }
    iici_io_write_uint16(ctx, addr, (uint16_t)(value >> 16));
    iici_io_write_uint16(ctx, addr + 2, (uint16_t)(value & 0xFFFF));
}

// ============================================================
// Memory layout
// ============================================================

static void iici_memory_layout_init(config_t *cfg) {
    iici_state_t *st = iici_state(cfg);

    uint32_t ram_size = cfg->ram_size;
    uint32_t rom_size = cfg->machine->rom_size;
    uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, ram_size);

    uint32_t ram_pages = ram_size >> PAGE_SHIFT;
    bool standard_bank = (ram_size == 1 * 1024 * 1024 || ram_size == 4 * 1024 * 1024 || ram_size == 16 * 1024 * 1024);
    uint32_t map_end_page = standard_bank ? ram_pages : (ram_pages * 2);
    for (uint32_t p = 0; p < map_end_page && (int)p < g_page_count; p++)
        mac030_fill_page(p, ram_base + ((p % ram_pages) << PAGE_SHIFT), true);

    uint32_t rom_pages = rom_size >> PAGE_SHIFT;
    uint32_t rom_start_page = IICI_ROM_START >> PAGE_SHIFT;
    uint32_t rom_end_page = IICI_ROM_END >> PAGE_SHIFT;
    if (rom_pages > 0) {
        for (uint32_t p = rom_start_page; p < rom_end_page && (int)p < g_page_count; p++) {
            uint32_t offset_in_rom = (p - rom_start_page) % rom_pages;
            mac030_fill_page(p, rom_data + (offset_in_rom << PAGE_SHIFT), false);
        }
    }

    st->io_interface.read_uint8 = iici_io_read_uint8;
    st->io_interface.read_uint16 = iici_io_read_uint16;
    st->io_interface.read_uint32 = iici_io_read_uint32;
    st->io_interface.write_uint8 = iici_io_write_uint8;
    st->io_interface.write_uint16 = iici_io_write_uint16;
    st->io_interface.write_uint32 = iici_io_write_uint32;
    memory_map_add(cfg->mem_map, IICI_IO_BASE, IICI_IO_SIZE, "IIci I/O", &st->io_interface, cfg);

    // Wire the built-in framebuffer (registered via mmu_register_vram) and
    // its Mode-24 slot-$B alias into the page table — same machinery as the
    // IIcx VRAM/Mode-24 handling so QuickDraw's 32-bit ($FBB08000) and
    // 24-bit ($00B08000) screen-base writes both land in the card buffer.
    if (st->mmu) {
        if (st->mmu->physical_vram && st->mmu->physical_vram_size > 0) {
            uint32_t pages = st->mmu->physical_vram_size >> PAGE_SHIFT;
            uint32_t start = st->mmu->vram_phys_base >> PAGE_SHIFT;
            for (uint32_t i = 0; i < pages && (int)(start + i) < g_page_count; i++)
                mac030_fill_page(start + i, st->mmu->physical_vram + (i << PAGE_SHIFT), /*writable*/ true);

            uint32_t base32 = st->mmu->vram_phys_base;
            uint32_t high = base32 & 0xFF000000u;
            if (high >= 0xF9000000u && high <= 0xFE000000u) {
                int slot = (int)((base32 >> 24) & 0xFu);
                uint32_t mode24_base = (uint32_t)slot << 20; // $00s00000
                uint32_t alias_bytes = st->mmu->physical_vram_size;
                if (alias_bytes > 0x100000u)
                    alias_bytes = 0x100000u; // 1 MB Mode-24 slot window
                uint32_t alias_pages = alias_bytes >> PAGE_SHIFT;
                uint32_t start24 = mode24_base >> PAGE_SHIFT;
                for (uint32_t i = 0; i < alias_pages && (int)(start24 + i) < g_page_count; i++)
                    mac030_fill_page(start24 + i, st->mmu->physical_vram + (i << PAGE_SHIFT), /*writable*/ true);
            }
        }
    }

    st->rom_overlay = false;
    iici_set_rom_overlay(cfg, true);
}

// ============================================================
// Interrupt routing
// ============================================================

static void iici_update_ipl(config_t *cfg, int source, bool active) {
    if (active)
        cfg->irq |= source;
    else
        cfg->irq &= ~source;
    uint32_t new_ipl;
    if (cfg->irq & IICI_IRQ_NMI)
        new_ipl = 7;
    else if (cfg->irq & IICI_IRQ_SCC)
        new_ipl = 4;
    else if (cfg->irq & IICI_IRQ_RBV)
        new_ipl = 2;
    else if (cfg->irq & IICI_IRQ_VIA1)
        new_ipl = 1;
    else
        new_ipl = 0;
    cpu_set_ipl(cfg->cpu, new_ipl);
    cpu_reschedule();
}

// ============================================================
// RBV / SCC / SCSI callbacks
// ============================================================

// RBV combined interrupt → 68030 IPL 2 (replaces the IIcx VIA2 source).
static void iici_rbv_irq(void *context, bool active) {
    iici_update_ipl((config_t *)context, IICI_IRQ_RBV, active);
}

// RBV soft power-off (RvPowerOff falling edge) — stop the scheduler so the
// headless run exits cleanly, matching the IIcx VIA2 PB2 behaviour.
static void iici_power_off(void *context) {
    config_t *cfg = (config_t *)context;
    LOG(1, "IIci soft power-off (RBV RvPowerOff)");
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);
}

// RBV depth change (RvMonP bits 0-2) → reshape the built-in video display.
static void iici_rbv_mode(void *context, int depth_code) {
    config_t *cfg = (config_t *)context;
    iici_state_t *st = iici_state(cfg);
    builtin_rbv_video_set_depth(st->video_card, depth_code);
}

// SCSI IRQ/DRQ → RBV flag bits (RvSCSIRQ / RvSCSIDRQ).  The IIci routes
// SCSI through the RBV, not a VIA2 — so we use scsi_set_irq_callback like
// the IIfx rather than scsi_set_via.
static void iici_scsi_irq(void *context, bool irq, bool drq) {
    config_t *cfg = (config_t *)context;
    iici_state_t *st = iici_state(cfg);
    if (!st->rbv)
        return;
    rbv_set_scsi_irq(st->rbv, irq);
    rbv_set_scsi_drq(st->rbv, drq);
}

// VIA1 outputs.  Identical to the IIcx: floppy head-select + ROM overlay on
// port A; ADB shift-state filtering + classic RTC on port B.
static void iici_via1_output(void *context, uint8_t port, uint8_t output) {
    config_t *cfg = (config_t *)context;
    iici_state_t *st = iici_state(cfg);

    if (port == 0) {
        if (st->floppy)
            floppy_set_sel_signal(st->floppy, (output & 0x20) != 0);
        iici_set_rom_overlay(cfg, (output & 0x10) != 0);
    } else {
        if (st->adb) {
            uint8_t st_mask = 0x30;
            uint8_t old_st = st->last_port_b & st_mask;
            uint8_t new_st = output & st_mask;
            if (new_st != old_st)
                adb_port_b_output(st->adb, output);
        }
        st->last_port_b = output;
        if (cfg->rtc)
            rtc_input(cfg->rtc, (output >> 2) & 1, (output >> 1) & 1, output & 1);
    }
}

static void iici_via1_shift_out(void *context, uint8_t byte) {
    config_t *cfg = (config_t *)context;
    iici_state_t *st = iici_state(cfg);
    if (st->adb)
        adb_shift_byte(st->adb, byte);
}

static void iici_via1_irq(void *context, bool active) {
    iici_update_ipl((config_t *)context, IICI_IRQ_VIA1, active);
}

static void iici_scc_irq(void *context, bool active) {
    iici_update_ipl((config_t *)context, IICI_IRQ_SCC, active);
}

// ============================================================
// VBL trigger
// ============================================================

// No VIA2: pulse only the VIA1 CA1 heartbeat, then fan out to the NuBus
// cards (the built-in video card marks its framebuffer dirty on_vbl).
static void iici_trigger_vbl(config_t *cfg) {
    via_input_c(cfg->via1, 0, 0, 0);
    via_input_c(cfg->via1, 0, 0, 1);
    nubus_tick_vbl(cfg->nubus);
    image_tick_all(cfg);
}

// ============================================================
// Slot table
// ============================================================
//
// Built-in RBV video is "slot $0" to the Slot Manager, but the slot table
// is sentinel-terminated by slot == 0, so we seat the card at our internal
// NuBus slot $B — the slot whose address space actually holds the
// framebuffer ($FBxxxxxx / VideoInfoMDU's PRAM slot $0B).  The slot index
// is an implementation detail (the boot ROM drives built-in video from the
// baked-in VideoInfoMDU, not by scanning this bus; the RBV video VBL uses
// logical slot 0 = RvIRQ0 independently).  Slots $C/$D/$E are the
// user-visible NuBus expansion slots (empty in v1).
static const nubus_slot_decl_t iici_slots[] = {
    {.slot = 0xB, .kind = NUBUS_SLOT_BUILTIN, .builtin_card_id = "builtin_rbv_video"},
    {.slot = 0xC, .kind = NUBUS_SLOT_EMPTY},
    {.slot = 0xD, .kind = NUBUS_SLOT_EMPTY},
    {.slot = 0xE, .kind = NUBUS_SLOT_EMPTY},
    {0},
};

// ============================================================
// Init / Teardown
// ============================================================

static void iici_init(config_t *cfg, checkpoint_t *checkpoint) {
    iici_state_t *st = calloc(1, sizeof(*st));
    assert(st != NULL);
    cfg->machine_context = st;
    st->last_port_b = 0x30; // ADB ST1:ST0 idle = 11

    // Build the shared II-family core (mem_map, cpu-from-profile, scheduler).
    mac030_build_core(cfg, checkpoint);
    if (checkpoint)
        system_read_checkpoint_data(checkpoint, &cfg->irq, sizeof(cfg->irq));

    cfg->rtc = rtc_init(cfg->scheduler, checkpoint, true);
    cfg->scc = scc_init(NULL, cfg->scheduler, iici_scc_irq, cfg, checkpoint);
    scc_set_clocks(cfg->scc, 7833600, 3686400);

    cfg->via1 = via_init(NULL, cfg->scheduler, 20, "via1", iici_via1_output, iici_via1_shift_out, iici_via1_irq, cfg,
                         checkpoint);
    rtc_set_via(cfg->rtc, cfg->via1);

    // Machine-ID readback on VIA1 port A: PA6/PA4/PA2/PA1 = 1/0/1/1 for the
    // production (no Parity Generator Card) IIci — mask $56, value $46 per
    // InfoMacIIci.  (Verified at the assembler level during bring-up.)
    via_input(cfg->via1, 0, 6, 1);
    via_input(cfg->via1, 0, 4, 0);
    via_input(cfg->via1, 0, 2, 1);
    via_input(cfg->via1, 0, 1, 1);
    // CA1 / CB1 idle high (VBL heartbeat + ADB clock reference edges).
    via_input_c(cfg->via1, 0, 0, 1);
    via_input_c(cfg->via1, 1, 0, 1);

    st->adb = adb_init(cfg->via1, cfg->scheduler, checkpoint);
    cfg->adb = st->adb;

    if (checkpoint)
        mac_checkpoint_restore_images(cfg, checkpoint);

    cfg->scsi = scsi_init(NULL, checkpoint);
    scsi_set_irq_callback(cfg->scsi, iici_scsi_irq, cfg);
    setup_images(cfg);

    st->asc = asc_init(NULL, cfg->scheduler, checkpoint); // no asc_set_via (RBV path; sound IRQ unwired, IIfx-parity)
    st->floppy = floppy_init(FLOPPY_TYPE_SWIM, NULL, cfg->scheduler, checkpoint);
    cfg->floppy = st->floppy;

    // RBV chip (VIA2 replacement + video control).  Default monitor sense 6
    // = 13" RGB.  IRQ → IPL 2; RvPowerOff → scheduler stop.
    st->rbv = rbv_init(RBV_VARIANT_IICI, checkpoint);
    assert(st->rbv != NULL);
    rbv_set_irq_callback(st->rbv, iici_rbv_irq, cfg);
    rbv_set_power_off_callback(st->rbv, iici_power_off, cfg);
    rbv_set_mode_callback(st->rbv, iici_rbv_mode, cfg);
    rbv_set_monitor_sense(st->rbv, 6);

    st->via1_iface = via_get_memory_interface(cfg->via1);
    st->scc_iface = scc_get_memory_interface(cfg->scc);
    st->scsi_iface = scsi_get_memory_interface(cfg->scsi);
    st->asc_iface = asc_get_memory_interface(st->asc);
    st->floppy_iface = floppy_get_memory_interface(st->floppy);
    st->rbv_iface = rbv_get_memory_interface(st->rbv);

    uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
    uint32_t ram_size = cfg->ram_size;
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, ram_size);
    uint32_t rom_size = cfg->machine->rom_size;
    st->mmu = mmu_init(ram_base, ram_size, cfg->machine->ram_max, rom_data, rom_size, IICI_ROM_START, IICI_ROM_END);
    assert(st->mmu != NULL);
    g_mmu = st->mmu;
    cpu_attach_mmu(cfg->cpu, st->mmu);
    // TT1 identity-maps NuBus space $F0-$FF for supervisor FCs (same as SE/30).
    st->mmu->tt1 = 0xF00F8043;

    cfg->nubus = nubus_init(cfg, iici_slots, checkpoint);
    st->video_card = nubus_card(cfg->nubus, 0xB);
    assert(st->video_card != NULL);
    builtin_rbv_video_set_rbv(st->video_card, st->rbv);

    // Register the built-in framebuffer at the slot-$B aperture so the boot
    // ROM's VideoInfoMDU screen base ($FBB08000) and its Mode-24 alias land
    // in the card buffer.  Mirrors se30_init's VRAM registration.
    uint8_t *fb = builtin_rbv_video_framebuffer(st->video_card);
    assert(fb != NULL);
    memory_map_host_region(cfg->mem_map, "iici_vram", fb, BUILTIN_RBV_VRAM_BASE, BUILTIN_RBV_VRAM_SIZE,
                           /*writable*/ true);

    // NuBus expansion slots $9..$E bus-error on unmapped reads; the mapped
    // built-in video aperture at $FBxxxxxx resolves ahead of this range.
    memory_set_bus_error_range(cfg->mem_map, 0xF9000000, 0xFEFFFFFF);

    iici_memory_layout_init(cfg);

    if (checkpoint) {
        mmu_checkpoint_restore(st->mmu, checkpoint);
        mmu_invalidate_tlb(st->mmu);
        g_mmu = st->mmu;
        cpu_attach_mmu(cfg->cpu, st->mmu);
        via_redrive_outputs(cfg->via1);
    }

    cfg->debugger = debug_init();
    scheduler_start(cfg->scheduler);
    if (!checkpoint) {
        cfg->irq = 0;
        cpu_set_ipl(cfg->cpu, 0);
    }
}

static void iici_teardown(config_t *cfg) {
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);
    iici_state_t *st = iici_state(cfg);
    if (st) {
        if (st->rbv) {
            rbv_delete(st->rbv);
            st->rbv = NULL;
        }
        if (st->mmu) {
            mmu_delete(st->mmu);
            st->mmu = NULL;
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
    // cfg->nubus is freed by system_destroy (which calls nubus_delete before
    // the machine teardown), matching the SE/30 and IIcx lifecycle.
    if (cfg->scsi) {
        scsi_delete(cfg->scsi);
        cfg->scsi = NULL;
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

// ============================================================
// Checkpoint
// ============================================================

static void iici_checkpoint_save(config_t *cfg, checkpoint_t *cp) {
    iici_state_t *st = iici_state(cfg);
    memory_map_checkpoint(cfg->mem_map, cp);
    cpu_checkpoint(cfg->cpu, cp);
    scheduler_checkpoint(cfg->scheduler, cp);
    system_write_checkpoint_data(cp, &cfg->irq, sizeof(cfg->irq));
    rtc_checkpoint(cfg->rtc, cp);
    scc_checkpoint(cfg->scc, cp);
    via_checkpoint(cfg->via1, cp);
    adb_checkpoint(st->adb, cp);
    mac_checkpoint_save_images(cfg, cp);
    scsi_checkpoint(cfg->scsi, cp);
    asc_checkpoint(st->asc, cp);
    floppy_checkpoint(st->floppy, cp);
    rbv_checkpoint(st->rbv, cp);
    mmu_checkpoint_save(st->mmu, cp);
}

// ============================================================
// Machine descriptor
// ============================================================

static const uint32_t iici_ram_options_kb[] = {1024, 2048, 4096, 5120, 8192, 16384, 32768, 65536, 131072, 0};

static const struct floppy_slot iici_floppy_slots[] = {
    {.label = "Internal FD0", .kind = FLOPPY_HD},
    {.label = "External FD1", .kind = FLOPPY_HD},
    {0},
};

static const struct scsi_slot iici_scsi_slots[] = {
    {.label = "SCSI HD0", .id = 0},
    {.label = "SCSI HD1", .id = 1},
    {0},
};

const hw_profile_t machine_iici = {
    .name = "Macintosh IIci",
    .id = "iici",

    .cpu_model = 68030,
    .freq = 25000000, // 25 MHz
    .mmu_present = true,
    .fpu_present = true,
    .mmu_kind = MMU_68030_PMMU,

    .address_bits = 32,
    .ram_default = 0x800000, // 8 MB
    .ram_max = 0x8000000, // 128 MB
    .rom_size = 0x80000, // 512 KB

    .ram_options = iici_ram_options_kb,
    .floppy_slots = iici_floppy_slots,
    .scsi_slots = iici_scsi_slots,
    .has_cdrom = true,
    .cdrom_id = 3,
    // Built-in RBV video has no separate declaration ROM — the boot ROM
    // drives it from the hard-coded VideoInfoMDU record.
    .needs_vrom = false,

    .via_count = 1, // VIA2 is replaced by the RBV
    .has_adb = true,
    .has_nubus = true,
    .nubus_slot_count = 3, // $C, $D, $E user-visible
    .nubus_slots = iici_slots,

    .init = iici_init,
    .reset = iici_reset,
    .teardown = iici_teardown,
    .checkpoint_save = iici_checkpoint_save,
    .checkpoint_restore = NULL,
    .memory_layout_init = iici_memory_layout_init,
    .update_ipl = iici_update_ipl,
    .trigger_vbl = iici_trigger_vbl,
    .display = NULL, // primary display sourced from cfg->nubus
};
