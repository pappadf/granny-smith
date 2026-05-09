// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iicx.c
// Macintosh IIcx machine implementation.  Shares the GLUE-driven I/O map
// and the dual-VIA / 68030 / Universal-ROM family with the SE/30, but
// replaces the slot-$E built-in video with a real NuBus slot at $9
// (defaulting to the Apple Macintosh Display Card 8•24).  See
// proposal-machine-iicx-iix.md §3.4.
//
// Diff vs se30.c at a glance (per proposal §3.4):
//   * Slot table: $9 = VIDEO with mdc_8_24 default, $A/$B = EMPTY,
//     no slot $E.
//   * Machine-ID bits: PA6 = 1, PB3 = 1 (vs SE/30 PA6 = 1, PB3 = 0).
//   * Soft-power-off via VIA2 PB2 (`v2PowerOff`); the OS writes 0
//     and we stop the scheduler so the headless run exits cleanly.
//   * Sound-jack-detect via VIA2 PB6 (`v2SndJck`) — input only.
//   * No buffer-select handling on VIA1 PA6 (no built-in framebuffer).
//
// The bulk of this file mirrors se30.c — the proper §3.1 glue030
// extraction (which would slim iicx down to ~150 LOC) is deliberately
// deferred per the proposal's "ratchet refactor" stance: ship working
// IIcx now, factor out the duplication when iix.c lands and the third
// caller arrives.

#include "machine.h"
#include "system_config.h" // full config_t

#include "adb.h"
#include "asc.h"
#include "checkpoint_machine.h"
#include "cpu.h"
#include "cpu_internal.h" // for cpu->mmu field
#include "debug.h"
#include "floppy.h"
#include "glue030.h" // family-shared image-list checkpoint helpers
#include "iicx_internal.h" // shared IIcx/IIx internals
#include "image.h"
#include "log.h"
#include "memory.h"
#include "memory_internal.h"
#include "mmu.h"
#include "nubus.h"
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

LOG_USE_CATEGORY_NAME("iicx");

// ============================================================
// Constants
// ============================================================
//
// Most address-space and IRQ constants live in iicx_internal.h so iix.c
// reuses them.  These IO_* offsets are private to the dispatcher.

#define IICX_IO_MIRROR 0x0001FFFFUL

#define IO_VIA1           0x00000
#define IO_VIA1_END       0x02000
#define IO_VIA2           0x02000
#define IO_VIA2_END       0x04000
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

// ============================================================
// Forward declarations
// ============================================================

static void iicx_via2_output(void *context, uint8_t port, uint8_t output);
static void iicx_via2_shift_out(void *context, uint8_t byte);
static void iicx_via2_irq(void *context, bool active);

// ============================================================
// SoA page helper (mirrors the SE/30 helper — same logic)
// ============================================================

void iicx_fill_page(uint32_t page_index, uint8_t *host_ptr, bool writable) {
    if ((int)page_index >= g_page_count)
        return;
    g_page_table[page_index].host_base = host_ptr;
    g_page_table[page_index].dev = NULL;
    g_page_table[page_index].dev_context = NULL;
    g_page_table[page_index].writable = writable;
    uint32_t guest_base = page_index << PAGE_SHIFT;
    uintptr_t adjusted = (uintptr_t)host_ptr - guest_base;
    if (g_supervisor_read)
        g_supervisor_read[page_index] = adjusted;
    if (g_user_read)
        g_user_read[page_index] = adjusted;
    if (writable) {
        if (g_supervisor_write)
            g_supervisor_write[page_index] = adjusted;
        if (g_user_write)
            g_user_write[page_index] = adjusted;
    }
}

// ============================================================
// ROM overlay
// ============================================================

void iicx_set_rom_overlay(config_t *cfg, bool overlay) {
    iicx_state_t *st = iicx_state(cfg);
    if (st->rom_overlay == overlay)
        return;
    st->rom_overlay = overlay;
    uint32_t rom_size = cfg->machine->rom_size;
    uint32_t rom_pages = rom_size >> PAGE_SHIFT;
    uint32_t rom_start_page = IICX_ROM_START >> PAGE_SHIFT;
    if (overlay) {
        for (uint32_t p = 0; p < rom_pages && (int)p < g_page_count; p++) {
            uint8_t *host_ptr = g_page_table[rom_start_page + p].host_base;
            iicx_fill_page(p, host_ptr, false);
        }
    } else {
        uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
        for (uint32_t p = 0; p < rom_pages && (int)p < g_page_count; p++)
            iicx_fill_page(p, ram_base + (p << PAGE_SHIFT), true);
    }
}

// ============================================================
// Hardware reset
// ============================================================

static void iicx_reset(config_t *cfg) {
    iicx_state_t *st = iicx_state(cfg);
    st->rom_overlay = false;
    iicx_set_rom_overlay(cfg, true);
    if (st->mmu) {
        st->mmu->enabled = false;
        st->mmu->tc = 0;
        mmu_invalidate_tlb(st->mmu);
    }
}

// ============================================================
// I/O dispatcher (same address map as SE/30 — shared GLUE chip)
// ============================================================

#define IICX_VIA_IO_PENALTY  16
#define IICX_SCC_IO_PENALTY  2
#define IICX_SCSI_IO_PENALTY 2
#define IICX_ASC_IO_PENALTY  2
#define IICX_SWIM_IO_PENALTY 2

uint8_t iicx_io_read_uint8(void *ctx, uint32_t addr) {
    config_t *cfg = (config_t *)ctx;
    iicx_state_t *st = iicx_state(cfg);
    uint32_t offset = addr & IICX_IO_MIRROR;
    if (offset < IO_VIA1_END) {
        memory_io_penalty(IICX_VIA_IO_PENALTY);
        return st->via1_iface->read_uint8(cfg->via1, (offset - IO_VIA1) & ~1u);
    }
    if (offset < IO_VIA2_END) {
        memory_io_penalty(IICX_VIA_IO_PENALTY);
        return st->via2_iface->read_uint8(cfg->via2, (offset - IO_VIA2) & ~1u);
    }
    if (offset < IO_SCC_END) {
        memory_io_penalty(IICX_SCC_IO_PENALTY);
        return st->scc_iface->read_uint8(cfg->scc, offset - IO_SCC);
    }
    if (offset < IO_SCSI_DRQ_END) {
        memory_io_penalty(IICX_SCSI_IO_PENALTY);
        return st->scsi_iface->read_uint8(cfg->scsi, 0);
    }
    if (offset >= IO_SCSI_REG && offset < IO_SCSI_REG_END) {
        memory_io_penalty(IICX_SCSI_IO_PENALTY);
        return st->scsi_iface->read_uint8(cfg->scsi, offset - IO_SCSI_REG);
    }
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        memory_io_penalty(IICX_SCSI_IO_PENALTY);
        return st->scsi_iface->read_uint8(cfg->scsi, 0);
    }
    if (offset >= IO_ASC && offset < IO_ASC_END) {
        memory_io_penalty(IICX_ASC_IO_PENALTY);
        return st->asc_iface->read_uint8(st->asc, offset - IO_ASC);
    }
    if (offset >= IO_SWIM && offset < IO_SWIM_END) {
        memory_io_penalty(IICX_SWIM_IO_PENALTY);
        return st->floppy_iface->read_uint8(st->floppy, offset - IO_SWIM);
    }
    return 0;
}

uint16_t iicx_io_read_uint16(void *ctx, uint32_t addr) {
    return ((uint16_t)iicx_io_read_uint8(ctx, addr) << 8) | iicx_io_read_uint8(ctx, addr + 1);
}

uint32_t iicx_io_read_uint32(void *ctx, uint32_t addr) {
    config_t *cfg = (config_t *)ctx;
    iicx_state_t *st = iicx_state(cfg);
    uint32_t offset = addr & IICX_IO_MIRROR;
    if (offset >= IO_SCSI_DRQ && offset < IO_SCSI_DRQ_END) {
        memory_io_penalty(IICX_SCSI_IO_PENALTY * 4);
        uint8_t b0 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b1 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b2 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b3 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        return ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | (uint32_t)b3;
    }
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        memory_io_penalty(IICX_SCSI_IO_PENALTY * 4);
        uint8_t b0 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b1 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b2 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b3 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        return ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | (uint32_t)b3;
    }
    return ((uint32_t)iicx_io_read_uint16(ctx, addr) << 16) | iicx_io_read_uint16(ctx, addr + 2);
}

void iicx_io_write_uint8(void *ctx, uint32_t addr, uint8_t value) {
    config_t *cfg = (config_t *)ctx;
    iicx_state_t *st = iicx_state(cfg);
    uint32_t offset = addr & IICX_IO_MIRROR;
    if (offset < IO_VIA1_END) {
        memory_io_penalty(IICX_VIA_IO_PENALTY);
        st->via1_iface->write_uint8(cfg->via1, (offset - IO_VIA1) & ~1u, value);
        return;
    }
    if (offset < IO_VIA2_END) {
        memory_io_penalty(IICX_VIA_IO_PENALTY);
        st->via2_iface->write_uint8(cfg->via2, (offset - IO_VIA2) & ~1u, value);
        return;
    }
    if (offset < IO_SCC_END) {
        memory_io_penalty(IICX_SCC_IO_PENALTY);
        st->scc_iface->write_uint8(cfg->scc, offset - IO_SCC, value);
        return;
    }
    if (offset < IO_SCSI_DRQ_END) {
        memory_io_penalty(IICX_SCSI_IO_PENALTY);
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, value);
        return;
    }
    if (offset >= IO_SCSI_REG && offset < IO_SCSI_REG_END) {
        memory_io_penalty(IICX_SCSI_IO_PENALTY);
        st->scsi_iface->write_uint8(cfg->scsi, offset - IO_SCSI_REG, value);
        return;
    }
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        memory_io_penalty(IICX_SCSI_IO_PENALTY);
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, value);
        return;
    }
    if (offset >= IO_ASC && offset < IO_ASC_END) {
        memory_io_penalty(IICX_ASC_IO_PENALTY);
        st->asc_iface->write_uint8(st->asc, offset - IO_ASC, value);
        return;
    }
    if (offset >= IO_SWIM && offset < IO_SWIM_END) {
        memory_io_penalty(IICX_SWIM_IO_PENALTY);
        st->floppy_iface->write_uint8(st->floppy, offset - IO_SWIM, value);
        return;
    }
}

void iicx_io_write_uint16(void *ctx, uint32_t addr, uint16_t value) {
    iicx_io_write_uint8(ctx, addr, (uint8_t)(value >> 8));
    iicx_io_write_uint8(ctx, addr + 1, (uint8_t)(value & 0xFF));
}

void iicx_io_write_uint32(void *ctx, uint32_t addr, uint32_t value) {
    config_t *cfg = (config_t *)ctx;
    iicx_state_t *st = iicx_state(cfg);
    uint32_t offset = addr & IICX_IO_MIRROR;
    if (offset >= IO_SCSI_DRQ && offset < IO_SCSI_DRQ_END) {
        memory_io_penalty(IICX_SCSI_IO_PENALTY * 4);
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 24));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 16));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 8));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value));
        return;
    }
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        memory_io_penalty(IICX_SCSI_IO_PENALTY * 4);
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 24));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 16));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 8));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value));
        return;
    }
    iicx_io_write_uint16(ctx, addr, (uint16_t)(value >> 16));
    iicx_io_write_uint16(ctx, addr + 2, (uint16_t)(value & 0xFFFF));
}

// ============================================================
// Memory layout
// ============================================================

void iicx_memory_layout_init(config_t *cfg) {
    iicx_state_t *st = iicx_state(cfg);

    uint32_t ram_size = cfg->ram_size;
    uint32_t rom_size = cfg->machine->rom_size;
    uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, ram_size);

    uint32_t ram_pages = ram_size >> PAGE_SHIFT;
    bool standard_bank = (ram_size == 1 * 1024 * 1024 || ram_size == 4 * 1024 * 1024 || ram_size == 16 * 1024 * 1024);
    uint32_t map_end_page = standard_bank ? ram_pages : (ram_pages * 2);
    for (uint32_t p = 0; p < map_end_page && (int)p < g_page_count; p++)
        iicx_fill_page(p, ram_base + ((p % ram_pages) << PAGE_SHIFT), true);

    uint32_t rom_pages = rom_size >> PAGE_SHIFT;
    uint32_t rom_start_page = IICX_ROM_START >> PAGE_SHIFT;
    uint32_t rom_end_page = IICX_ROM_END >> PAGE_SHIFT;
    if (rom_pages > 0) {
        for (uint32_t p = rom_start_page; p < rom_end_page && (int)p < g_page_count; p++) {
            uint32_t offset_in_rom = (p - rom_start_page) % rom_pages;
            iicx_fill_page(p, rom_data + (offset_in_rom << PAGE_SHIFT), false);
        }
    }

    st->io_interface.read_uint8 = iicx_io_read_uint8;
    st->io_interface.read_uint16 = iicx_io_read_uint16;
    st->io_interface.read_uint32 = iicx_io_read_uint32;
    st->io_interface.write_uint8 = iicx_io_write_uint8;
    st->io_interface.write_uint16 = iicx_io_write_uint16;
    st->io_interface.write_uint32 = iicx_io_write_uint32;
    memory_map_add(cfg->mem_map, IICX_IO_BASE, IICX_IO_SIZE, "IIcx I/O", &st->io_interface, cfg);

    // Populate page-table entries for any host-backed slot regions
    // registered by NuBus cards (matches the SE/30 pattern of explicit
    // se30_fill_page calls).  Without this, slot-space reads fall to the
    // unmapped-$FF path even when phys_to_host would resolve, because
    // memory_read's slow path doesn't probe phys_to_host directly.
    if (st->mmu) {
        if (st->mmu->physical_vram && st->mmu->physical_vram_size > 0) {
            uint32_t pages = st->mmu->physical_vram_size >> PAGE_SHIFT;
            uint32_t start = st->mmu->vram_phys_base >> PAGE_SHIFT;
            for (uint32_t i = 0; i < pages && (int)(start + i) < g_page_count; i++)
                iicx_fill_page(start + i, st->mmu->physical_vram + (i << PAGE_SHIFT), /*writable*/ true);
        }
        if (st->mmu->physical_vrom && st->mmu->physical_vrom_size > 0) {
            uint32_t pages = st->mmu->physical_vrom_size >> PAGE_SHIFT;
            uint32_t start = st->mmu->vrom_phys_base >> PAGE_SHIFT;
            for (uint32_t i = 0; i < pages && (int)(start + i) < g_page_count; i++)
                iicx_fill_page(start + i, st->mmu->physical_vrom + (i << PAGE_SHIFT), /*writable*/ false);
        }

        // Mode-24 (24-bit Memory Manager) NuBus slot windows: each slot s
        // ($9..$E) has a 1 MB region at $00s00000 that mirrors the start of
        // its 32-bit slot space at $Fs000000. On real Mac II family hardware
        // the GLUE / BBU chip decodes both ranges to the same NuBus slot;
        // the IIcx/IIx/SE/30 ROM is 24-bit-only, so the OS routinely uses
        // these low aliases. Without this mirror, a card's framebuffer that
        // QuickDraw reaches via a Mode-24 ScrnBase ($00900080) lands in
        // unmapped low memory.
        if (st->mmu->physical_vram) {
            uint32_t base32 = st->mmu->vram_phys_base;
            uint32_t high = base32 & 0xFF000000u;
            if (high >= 0xF9000000u && high <= 0xFE000000u) {
                int slot = (int)((base32 >> 24) & 0xFu);
                uint32_t mode24_base = (uint32_t)slot << 20; // $00s00000
                uint32_t alias_bytes = 0x100000u; // 1 MB Mode-24 slot window
                if (alias_bytes > st->mmu->physical_vram_size)
                    alias_bytes = st->mmu->physical_vram_size;
                uint32_t alias_pages = alias_bytes >> PAGE_SHIFT;
                uint32_t start = mode24_base >> PAGE_SHIFT;
                for (uint32_t i = 0; i < alias_pages && (int)(start + i) < g_page_count; i++)
                    iicx_fill_page(start + i, st->mmu->physical_vram + (i << PAGE_SHIFT), /*writable*/ true);
            }
        }
    }

    st->rom_overlay = false;
    iicx_set_rom_overlay(cfg, true);
}

// ============================================================
// Interrupt routing
// ============================================================

void iicx_update_ipl(config_t *cfg, int source, bool active) {
    if (active)
        cfg->irq |= source;
    else
        cfg->irq &= ~source;
    uint32_t new_ipl;
    if (cfg->irq & IICX_IRQ_NMI)
        new_ipl = 7;
    else if (cfg->irq & IICX_IRQ_SCC)
        new_ipl = 4;
    else if (cfg->irq & IICX_IRQ_VIA2)
        new_ipl = 2;
    else if (cfg->irq & IICX_IRQ_VIA1)
        new_ipl = 1;
    else
        new_ipl = 0;
    cpu_set_ipl(cfg->cpu, new_ipl);
    cpu_reschedule();
}

// ============================================================
// VIA / SCC callbacks
// ============================================================

// VIA1 outputs.  Mostly identical to SE/30 except for PA6 — IIcx has no
// built-in framebuffer, so PA6 is a free port pin we ignore.
void iicx_via1_output(void *context, uint8_t port, uint8_t output) {
    config_t *cfg = (config_t *)context;
    iicx_state_t *st = iicx_state(cfg);

    if (port == 0) {
        if (st->floppy)
            floppy_set_sel_signal(st->floppy, (output & 0x20) != 0);
        iicx_set_rom_overlay(cfg, (output & 0x10) != 0);
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

void iicx_via1_shift_out(void *context, uint8_t byte) {
    config_t *cfg = (config_t *)context;
    iicx_state_t *st = iicx_state(cfg);
    if (st->adb)
        adb_shift_byte(st->adb, byte);
}

void iicx_via1_irq(void *context, bool active) {
    iicx_update_ipl((config_t *)context, IICX_IRQ_VIA1, active);
}

// VIA2 outputs.  Adds the IIcx-specific soft-power-off on PB2.  When the
// OS pulls v2PowerOff low the PSU shuts down — v1 emulator equivalent:
// stop the scheduler so the headless run exits cleanly.
static void iicx_via2_output(void *context, uint8_t port, uint8_t output) {
    config_t *cfg = (config_t *)context;
    iicx_state_t *st = iicx_state(cfg);
    if (port != 1)
        return;

    bool new_pb2 = (output & 0x04) != 0;
    if (!st->soft_power_armed && new_pb2) {
        st->soft_power_armed = true;
    } else if (st->soft_power_armed && !new_pb2) {
        LOG(0, "IIcx soft power-off (VIA2 PB2 = 0)");
        if (cfg->scheduler)
            scheduler_stop(cfg->scheduler);
    }
    st->last_via2_port_b = output;
}

static void iicx_via2_shift_out(void *context, uint8_t byte) {
    (void)context;
    (void)byte;
}

static void iicx_via2_irq(void *context, bool active) {
    iicx_update_ipl((config_t *)context, IICX_IRQ_VIA2, active);
}

void iicx_scc_irq(void *context, bool active) {
    iicx_update_ipl((config_t *)context, IICX_IRQ_SCC, active);
}

// ============================================================
// VBL trigger
// ============================================================

// Unlike SE/30 we do not directly poke a slot-IRQ bit on VIA2 PA — the
// per-card on_vbl handler does that via nubus_assert_irq.  The family
// CA1 heartbeat still fires on every VBL.
static void iicx_trigger_vbl(config_t *cfg) {
    via_input_c(cfg->via1, 0, 0, 0);
    via_input_c(cfg->via2, 0, 0, 0);
    via_input_c(cfg->via1, 0, 0, 1);
    via_input_c(cfg->via2, 0, 0, 1);
    nubus_tick_vbl(cfg->nubus);
    image_tick_all(cfg);
}

// ============================================================
// Slot table
// ============================================================

static const char *const iicx_video_cards[] = {"mdc_8_24", NULL};

static const nubus_slot_decl_t iicx_slots[] = {
    {.slot = 0x9, .kind = NUBUS_SLOT_VIDEO, .available_cards = iicx_video_cards, .default_card = "mdc_8_24"},
    {.slot = 0xA, .kind = NUBUS_SLOT_EMPTY},
    {.slot = 0xB, .kind = NUBUS_SLOT_EMPTY},
    {0},
};

// ============================================================
// Init / Teardown
// ============================================================

static void iicx_init(config_t *cfg, checkpoint_t *checkpoint) {
    iicx_state_t *st = calloc(1, sizeof(*st));
    assert(st != NULL);
    cfg->machine_context = st;
    st->last_port_b = 0x30; // ADB ST1:ST0 idle = 11
    st->last_via2_port_b = 0xFF; // PB2 starts high (no power-off)

    cfg->mem_map = memory_map_init(cfg->machine->address_bits, cfg->ram_size, cfg->machine->rom_size, checkpoint);
    cfg->cpu = cpu_init(CPU_MODEL_68030, checkpoint);
    cfg->scheduler = scheduler_init(cfg->cpu, checkpoint);
    scheduler_set_frequency(cfg->scheduler, cfg->machine->freq);
    scheduler_set_cpi(cfg->scheduler, 4, 4);
    if (checkpoint)
        system_read_checkpoint_data(checkpoint, &cfg->irq, sizeof(cfg->irq));

    cfg->rtc = rtc_init(cfg->scheduler, checkpoint);
    cfg->scc = scc_init(NULL, cfg->scheduler, iicx_scc_irq, cfg, checkpoint);
    scc_set_clocks(cfg->scc, 7833600, 3686400);

    cfg->via1 = via_init(NULL, cfg->scheduler, 20, "via1", iicx_via1_output, iicx_via1_shift_out, iicx_via1_irq, cfg,
                         checkpoint);
    cfg->via2 = via_init(NULL, cfg->scheduler, 20, "via2", iicx_via2_output, iicx_via2_shift_out, iicx_via2_irq, cfg,
                         checkpoint);
    rtc_set_via(cfg->rtc, cfg->via1);

    // Machine ID: PA6 = 1 (default high), PB3 = 1 (IIcx).
    via_input(cfg->via2, 1, 3, 1);
    // VIA2 PA bits (slot IRQs) idle high (no IRQ).  The bus controller
    // drives them per-slot once a card asserts.
    via_input(cfg->via2, 0, 0, 1);
    via_input(cfg->via2, 0, 1, 1);
    via_input(cfg->via2, 0, 2, 1);
    via_input(cfg->via2, 0, 3, 1);
    via_input(cfg->via2, 0, 4, 1);
    via_input(cfg->via2, 0, 5, 1);
    // PB6 — sound jack inserted (active-low).
    via_input(cfg->via2, 1, 6, 0);
    via_input_c(cfg->via2, 0, 0, 1);
    via_input_c(cfg->via2, 0, 1, 1);
    via_input_c(cfg->via2, 1, 1, 1);

    st->adb = adb_init(cfg->via1, cfg->scheduler, checkpoint);
    cfg->adb = st->adb;

    if (checkpoint)
        glue030_checkpoint_restore_images(cfg, checkpoint);

    cfg->scsi = scsi_init(NULL, checkpoint);
    scsi_set_via(cfg->scsi, cfg->via2);
    setup_images(cfg);

    st->asc = asc_init(NULL, cfg->scheduler, checkpoint);
    asc_set_via(st->asc, cfg->via2);
    st->floppy = floppy_init(FLOPPY_TYPE_SWIM, NULL, cfg->scheduler, checkpoint);
    cfg->floppy = st->floppy;

    st->via1_iface = via_get_memory_interface(cfg->via1);
    st->via2_iface = via_get_memory_interface(cfg->via2);
    st->scc_iface = scc_get_memory_interface(cfg->scc);
    st->scsi_iface = scsi_get_memory_interface(cfg->scsi);
    st->asc_iface = asc_get_memory_interface(st->asc);
    st->floppy_iface = floppy_get_memory_interface(st->floppy);

    uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
    uint32_t ram_size = cfg->ram_size;
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, ram_size);
    uint32_t rom_size = cfg->machine->rom_size;
    st->mmu = mmu_init(ram_base, ram_size, cfg->machine->ram_max, rom_data, rom_size, IICX_ROM_START, IICX_ROM_END);
    assert(st->mmu != NULL);
    g_mmu = st->mmu;
    cfg->cpu->mmu = st->mmu;
    // Same TT1 as SE/30 — supervisor-only identity for $F0..$FF.
    st->mmu->tt1 = 0xF00F8043;

    cfg->nubus = nubus_init(cfg, iicx_slots, checkpoint);

    // Bus error window covers all six possible NuBus slots ($9..$E).
    memory_set_bus_error_range(cfg->mem_map, 0xF9000000, 0xFEFFFFFF);

    iicx_memory_layout_init(cfg);

    if (checkpoint) {
        system_read_checkpoint_data(checkpoint, &st->mmu->tc, sizeof(st->mmu->tc));
        system_read_checkpoint_data(checkpoint, &st->mmu->crp, sizeof(st->mmu->crp));
        system_read_checkpoint_data(checkpoint, &st->mmu->srp, sizeof(st->mmu->srp));
        system_read_checkpoint_data(checkpoint, &st->mmu->tt0, sizeof(st->mmu->tt0));
        system_read_checkpoint_data(checkpoint, &st->mmu->tt1, sizeof(st->mmu->tt1));
        system_read_checkpoint_data(checkpoint, &st->mmu->mmusr, sizeof(st->mmu->mmusr));
        system_read_checkpoint_data(checkpoint, &st->mmu->enabled, sizeof(st->mmu->enabled));
        mmu_invalidate_tlb(st->mmu);
        g_mmu = st->mmu;
        cfg->cpu->mmu = st->mmu;
        via_redrive_outputs(cfg->via1);
        via_redrive_outputs(cfg->via2);
    }

    cfg->debugger = debug_init();
    scheduler_register_vbl_type(cfg->scheduler, cfg);
    scheduler_start(cfg->scheduler);
    if (!checkpoint) {
        cfg->irq = 0;
        cpu_set_ipl(cfg->cpu, 0);
    }
}

static void iicx_teardown(config_t *cfg) {
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);
    iicx_state_t *st = iicx_state(cfg);
    if (st) {
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

// ============================================================
// Checkpoint
// ============================================================

static void iicx_checkpoint_save(config_t *cfg, checkpoint_t *cp) {
    iicx_state_t *st = iicx_state(cfg);
    memory_map_checkpoint(cfg->mem_map, cp);
    cpu_checkpoint(cfg->cpu, cp);
    scheduler_checkpoint(cfg->scheduler, cp);
    system_write_checkpoint_data(cp, &cfg->irq, sizeof(cfg->irq));
    rtc_checkpoint(cfg->rtc, cp);
    scc_checkpoint(cfg->scc, cp);
    via_checkpoint(cfg->via1, cp);
    via_checkpoint(cfg->via2, cp);
    adb_checkpoint(st->adb, cp);
    glue030_checkpoint_save_images(cfg, cp);
    scsi_checkpoint(cfg->scsi, cp);
    asc_checkpoint(st->asc, cp);
    floppy_checkpoint(st->floppy, cp);
    system_write_checkpoint_data(cp, &st->mmu->tc, sizeof(st->mmu->tc));
    system_write_checkpoint_data(cp, &st->mmu->crp, sizeof(st->mmu->crp));
    system_write_checkpoint_data(cp, &st->mmu->srp, sizeof(st->mmu->srp));
    system_write_checkpoint_data(cp, &st->mmu->tt0, sizeof(st->mmu->tt0));
    system_write_checkpoint_data(cp, &st->mmu->tt1, sizeof(st->mmu->tt1));
    system_write_checkpoint_data(cp, &st->mmu->mmusr, sizeof(st->mmu->mmusr));
    system_write_checkpoint_data(cp, &st->mmu->enabled, sizeof(st->mmu->enabled));
}

// ============================================================
// Machine descriptor
// ============================================================

static const uint32_t iicx_ram_options_kb[] = {1024, 2048, 4096, 5120, 8192, 16384, 32768, 65536, 131072, 0};

static const struct floppy_slot iicx_floppy_slots[] = {
    {.label = "Internal FD0", .kind = FLOPPY_HD},
    {.label = "External FD1", .kind = FLOPPY_HD},
    {0},
};

static const struct scsi_slot iicx_scsi_slots[] = {
    {.label = "SCSI HD0", .id = 0},
    {.label = "SCSI HD1", .id = 1},
    {0},
};

const hw_profile_t machine_iicx = {
    .name = "Macintosh IIcx",
    .id = "iicx",

    .cpu_model = 68030,
    .freq = 15667200,
    .mmu_present = true,
    .fpu_present = true,

    .address_bits = 32,
    .ram_default = 0x800000, // 8 MB
    .ram_max = 0x8000000, // 128 MB
    .rom_size = 0x040000, // 256 KB

    .ram_options = iicx_ram_options_kb,
    .floppy_slots = iicx_floppy_slots,
    .scsi_slots = iicx_scsi_slots,
    .has_cdrom = true,
    .cdrom_id = 3,
    // The IIcx has no built-in video — its primary display comes from
    // a NuBus video card seated in slot $9 (Apple Display Card 8•24 by
    // default).  That card needs Apple-341-0868.vrom to declare itself
    // to the Slot Manager (without it, slot scan finds an empty slot
    // and the boot ROM can't bring up a framebuffer).  We mark
    // needs_vrom = true so the config dialog asks the user for a VROM
    // file; the JMFB card driver picks it up from /opfs/images/vrom/
    // by its canonical name during card init.
    .needs_vrom = true,

    .via_count = 2,
    .has_adb = true,
    .has_nubus = true,
    .nubus_slot_count = 3,

    .init = iicx_init,
    .reset = iicx_reset,
    .teardown = iicx_teardown,
    .checkpoint_save = iicx_checkpoint_save,
    .checkpoint_restore = NULL,
    .memory_layout_init = iicx_memory_layout_init,
    .update_ipl = iicx_update_ipl,
    .trigger_vbl = iicx_trigger_vbl,
    .display = NULL, // primary display sourced from cfg->nubus
};
