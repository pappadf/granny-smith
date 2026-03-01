// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// se30.c
// Macintosh SE/30 machine implementation.
//
// This file implements the SE/30's 32-bit physical memory map, dual-VIA
// interrupt architecture, ROM overlay, I/O address mirroring, and SCSI
// pseudo-DMA. The SE/30 shares the same Universal ROM as the IIx/IIcx
// but uses a fixed 512x342 monochrome display.
//
// I/O space ($50000000-$5FFFFFFF) is handled by a single dispatcher that
// mirrors the device block every $20000 bytes, as implemented by the GLUE
// ASIC (344S0602).

#include "machine.h"
#include "system_config.h" // full config_t definition

#include "adb.h"
#include "asc.h"
#include "cpu.h"
#include "debug.h"
#include "image.h"
#include "log.h"
#include "memory.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "shell.h"
#include "swim.h"
#include "via.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("se30");

// ============================================================
// Constants
// ============================================================

// SE/30 RAM region spans the first 1 GB of address space
#define SE30_RAM_END 0x40000000UL

// SE/30 ROM region: 256 KB mirrored across 256 MB
#define SE30_ROM_START 0x40000000UL
#define SE30_ROM_END   0x50000000UL

// SE/30 I/O region: 256 MB, mirrored every $20000
#define SE30_IO_BASE   0x50000000UL
#define SE30_IO_SIZE   0x10000000UL
#define SE30_IO_MIRROR 0x0001FFFFUL // 17-bit mask for $20000 mirroring

// I/O device offsets within the $20000 block
#define IO_VIA1           0x00000 // VIA1: ADB, RTC, ROM overlay
#define IO_VIA1_END       0x02000
#define IO_VIA2           0x02000 // VIA2: interrupt aggregation
#define IO_VIA2_END       0x04000
#define IO_SCC            0x04000 // Zilog 8530 SCC
#define IO_SCC_END        0x06000
#define IO_SCSI_DRQ       0x06000 // SCSI pseudo-DMA with DRQ handshaking
#define IO_SCSI_DRQ_END   0x08000
#define IO_SCSI_REG       0x10000 // NCR 5380 direct register access
#define IO_SCSI_REG_END   0x12000
#define IO_SCSI_BLIND     0x12000 // SCSI pseudo-DMA without DRQ
#define IO_SCSI_BLIND_END 0x14000
#define IO_ASC            0x14000 // Apple Sound Chip
#define IO_ASC_END        0x16000
#define IO_SWIM           0x16000 // SWIM floppy controller
#define IO_SWIM_END       0x18000

// Interrupt source bits for se30_update_ipl()
#define SE30_IRQ_VIA1 (1 << 0) // IPL level 1
#define SE30_IRQ_VIA2 (1 << 1) // IPL level 2
#define SE30_IRQ_SCC  (1 << 2) // IPL level 4
#define SE30_IRQ_NMI  (1 << 3) // IPL level 7

// ============================================================
// SE/30-specific state
// ============================================================

// SE/30-specific peripheral state not shared with other machines.
// Accessed through config_t.machine_context.
typedef struct se30_state {
    // SE/30-specific peripherals (not in config_t)
    adb_t *adb;
    asc_t *asc;
    swim_t *swim;

    // ROM overlay state (true = ROM mapped at $00000000)
    bool rom_overlay;

    // Cached device interfaces for I/O dispatch
    const memory_interface_t *via1_iface;
    const memory_interface_t *via2_iface;
    const memory_interface_t *scc_iface;
    const memory_interface_t *scsi_iface;
    const memory_interface_t *asc_iface;
    const memory_interface_t *swim_iface;

    // I/O dispatcher registered at $50000000
    memory_interface_t io_interface;
} se30_state_t;

// Helper: return the SE/30-specific state from a config handle
static inline se30_state_t *se30_state(config_t *cfg) {
    return (se30_state_t *)cfg->machine_context;
}

// ============================================================
// Forward declarations for SE/30 callbacks
// ============================================================

static void se30_via1_output(void *context, uint8_t port, uint8_t output);
static void se30_via1_shift_out(void *context, uint8_t byte);
static void se30_via1_irq(void *context, bool active);
static void se30_via2_output(void *context, uint8_t port, uint8_t output);
static void se30_via2_shift_out(void *context, uint8_t byte);
static void se30_via2_irq(void *context, bool active);
static void se30_scc_irq(void *context, bool active);
static void se30_update_ipl(config_t *cfg, int source, bool active);

// ============================================================
// ROM overlay
// ============================================================

// Toggle ROM/RAM mapping at $00000000.
// On reset, ROM is overlaid at $00000000 for the initial vector fetch.
// The ROM boot code disables the overlay by writing 0 to VIA1 PA4.
static void se30_set_rom_overlay(config_t *cfg, bool overlay) {
    se30_state_t *se30 = se30_state(cfg);
    if (se30->rom_overlay == overlay)
        return;
    se30->rom_overlay = overlay;

    uint32_t rom_size = cfg->machine->rom_size;
    uint32_t rom_pages = rom_size >> PAGE_SHIFT;
    uint32_t rom_start_page = SE30_ROM_START >> PAGE_SHIFT;

    if (overlay) {
        // Map ROM at $00000000 (copy host_base from ROM region at $40000000)
        for (uint32_t p = 0; p < rom_pages && (int)p < g_page_count; p++) {
            g_page_table[p].host_base = g_page_table[rom_start_page + p].host_base;
            g_page_table[p].dev = NULL;
            g_page_table[p].dev_context = NULL;
            g_page_table[p].writable = false;
        }
        LOG(1, "ROM overlay enabled: ROM at $00000000");
    } else {
        // Map RAM back at $00000000
        uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
        for (uint32_t p = 0; p < rom_pages && (int)p < g_page_count; p++) {
            g_page_table[p].host_base = ram_base + (p << PAGE_SHIFT);
            g_page_table[p].dev = NULL;
            g_page_table[p].dev_context = NULL;
            g_page_table[p].writable = true;
        }
        LOG(1, "ROM overlay disabled: RAM at $00000000");
    }
}

// ============================================================
// I/O dispatcher
// ============================================================

// Read a byte from the SE/30 I/O space.
// Masks address with $1FFFF for GLUE mirroring, then dispatches to device.
static uint8_t se30_io_read_uint8(void *ctx, uint32_t addr) {
    config_t *cfg = (config_t *)ctx;
    se30_state_t *se30 = se30_state(cfg);

    // Mirror: take lower 17 bits of the offset from I/O base
    uint32_t offset = addr & SE30_IO_MIRROR;

    if (offset < IO_VIA1_END)
        return se30->via1_iface->read_uint8(cfg->via1, offset - IO_VIA1);
    if (offset < IO_VIA2_END)
        return se30->via2_iface->read_uint8(cfg->via2, offset - IO_VIA2);
    if (offset < IO_SCC_END)
        return se30->scc_iface->read_uint8(cfg->scc, offset - IO_SCC);
    if (offset < IO_SCSI_DRQ_END) {
        // Pseudo-DMA: 8-bit read from SCSI data register with DRQ
        return se30->scsi_iface->read_uint8(cfg->scsi, 0);
    }
    if (offset >= IO_SCSI_REG && offset < IO_SCSI_REG_END)
        return se30->scsi_iface->read_uint8(cfg->scsi, offset - IO_SCSI_REG);
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        // Blind pseudo-DMA: 8-bit read without DRQ check
        return se30->scsi_iface->read_uint8(cfg->scsi, 0);
    }
    if (offset >= IO_ASC && offset < IO_ASC_END)
        return se30->asc_iface->read_uint8(se30->asc, offset - IO_ASC);
    if (offset >= IO_SWIM && offset < IO_SWIM_END)
        return se30->swim_iface->read_uint8(se30->swim, offset - IO_SWIM);

    return 0; // unmapped I/O space
}

// Read a 16-bit word from the SE/30 I/O space
static uint16_t se30_io_read_uint16(void *ctx, uint32_t addr) {
    // Most SE/30 devices are 8-bit; split into two byte reads
    uint8_t hi = se30_io_read_uint8(ctx, addr);
    uint8_t lo = se30_io_read_uint8(ctx, addr + 1);
    return (uint16_t)((hi << 8) | lo);
}

// Read a 32-bit longword from the SE/30 I/O space.
// Handles SCSI pseudo-DMA: a 32-bit read at $50006000 coalesces
// four 8-bit reads from the NCR 5380 data register.
static uint32_t se30_io_read_uint32(void *ctx, uint32_t addr) {
    config_t *cfg = (config_t *)ctx;
    se30_state_t *se30 = se30_state(cfg);

    uint32_t offset = addr & SE30_IO_MIRROR;

    // SCSI pseudo-DMA: coalesce 4 byte reads from data register
    if (offset >= IO_SCSI_DRQ && offset < IO_SCSI_DRQ_END) {
        uint8_t b0 = se30->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b1 = se30->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b2 = se30->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b3 = se30->scsi_iface->read_uint8(cfg->scsi, 0);
        return ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | (uint32_t)b3;
    }
    // Blind pseudo-DMA: same coalescing without DRQ check
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        uint8_t b0 = se30->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b1 = se30->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b2 = se30->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b3 = se30->scsi_iface->read_uint8(cfg->scsi, 0);
        return ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | (uint32_t)b3;
    }

    // Default: split into two 16-bit reads
    uint32_t hi = se30_io_read_uint16(ctx, addr);
    uint32_t lo = se30_io_read_uint16(ctx, addr + 2);
    return (hi << 16) | lo;
}

// Write a byte to the SE/30 I/O space
static void se30_io_write_uint8(void *ctx, uint32_t addr, uint8_t value) {
    config_t *cfg = (config_t *)ctx;
    se30_state_t *se30 = se30_state(cfg);

    uint32_t offset = addr & SE30_IO_MIRROR;

    if (offset < IO_VIA1_END) {
        se30->via1_iface->write_uint8(cfg->via1, offset - IO_VIA1, value);
        return;
    }
    if (offset < IO_VIA2_END) {
        se30->via2_iface->write_uint8(cfg->via2, offset - IO_VIA2, value);
        return;
    }
    if (offset < IO_SCC_END) {
        se30->scc_iface->write_uint8(cfg->scc, offset - IO_SCC, value);
        return;
    }
    if (offset < IO_SCSI_DRQ_END) {
        // Pseudo-DMA: 8-bit write to SCSI data register (DMA mode: bit 0x200 + odd)
        se30->scsi_iface->write_uint8(cfg->scsi, 0x201, value);
        return;
    }
    if (offset >= IO_SCSI_REG && offset < IO_SCSI_REG_END) {
        se30->scsi_iface->write_uint8(cfg->scsi, offset - IO_SCSI_REG, value);
        return;
    }
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        // Blind pseudo-DMA write
        se30->scsi_iface->write_uint8(cfg->scsi, 0x201, value);
        return;
    }
    if (offset >= IO_ASC && offset < IO_ASC_END) {
        se30->asc_iface->write_uint8(se30->asc, offset - IO_ASC, value);
        return;
    }
    if (offset >= IO_SWIM && offset < IO_SWIM_END) {
        se30->swim_iface->write_uint8(se30->swim, offset - IO_SWIM, value);
        return;
    }
    // unmapped — ignore
}

// Write a 16-bit word to the SE/30 I/O space
static void se30_io_write_uint16(void *ctx, uint32_t addr, uint16_t value) {
    se30_io_write_uint8(ctx, addr, (uint8_t)(value >> 8));
    se30_io_write_uint8(ctx, addr + 1, (uint8_t)(value & 0xFF));
}

// Write a 32-bit longword to the SE/30 I/O space.
// Handles SCSI pseudo-DMA: a 32-bit write at $50006000 splits into
// four 8-bit writes to the NCR 5380 data register.
static void se30_io_write_uint32(void *ctx, uint32_t addr, uint32_t value) {
    config_t *cfg = (config_t *)ctx;
    se30_state_t *se30 = se30_state(cfg);

    uint32_t offset = addr & SE30_IO_MIRROR;

    // SCSI pseudo-DMA: split longword into 4 byte writes (DMA mode)
    if (offset >= IO_SCSI_DRQ && offset < IO_SCSI_DRQ_END) {
        se30->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 24));
        se30->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 16));
        se30->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 8));
        se30->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value));
        return;
    }
    // Blind pseudo-DMA write
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        se30->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 24));
        se30->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 16));
        se30->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 8));
        se30->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value));
        return;
    }

    // Default: split into two 16-bit writes
    se30_io_write_uint16(ctx, addr, (uint16_t)(value >> 16));
    se30_io_write_uint16(ctx, addr + 2, (uint16_t)(value & 0xFFFF));
}

// ============================================================
// Memory layout
// ============================================================

// Populate the SE/30 memory layout in the page table.
// RAM: $00000000-$3FFFFFFF (actual size from profile, mirrored)
// ROM: $40000000-$4FFFFFFF (256 KB mirrored across 256 MB)
// I/O: $50000000-$5FFFFFFF (dispatcher with $20000 mirroring)
// ROM overlay at $00000000 is active on reset.
static void se30_memory_layout_init(config_t *cfg) {
    se30_state_t *se30 = se30_state(cfg);

    uint32_t ram_size = cfg->machine->ram_size_default;
    uint32_t rom_size = cfg->machine->rom_size;
    uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
    // ROM data is stored immediately after RAM in the flat buffer
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, ram_size);

    // --- RAM pages: $00000000 - ram_size (writable, direct access) ---
    uint32_t ram_pages = ram_size >> PAGE_SHIFT;
    for (uint32_t p = 0; p < ram_pages && (int)p < g_page_count; p++) {
        g_page_table[p].host_base = ram_base + (p << PAGE_SHIFT);
        g_page_table[p].dev = NULL;
        g_page_table[p].dev_context = NULL;
        g_page_table[p].writable = true;
    }

    // --- ROM pages: $40000000 - $4FFFFFFF (256 KB mirrored, read-only) ---
    uint32_t rom_pages = rom_size >> PAGE_SHIFT;
    uint32_t rom_start_page = SE30_ROM_START >> PAGE_SHIFT;
    uint32_t rom_end_page = SE30_ROM_END >> PAGE_SHIFT;

    if (rom_pages > 0) {
        for (uint32_t p = rom_start_page; p < rom_end_page && (int)p < g_page_count; p++) {
            uint32_t offset_in_rom = (p - rom_start_page) % rom_pages;
            g_page_table[p].host_base = rom_data + (offset_in_rom << PAGE_SHIFT);
            g_page_table[p].dev = NULL;
            g_page_table[p].dev_context = NULL;
            g_page_table[p].writable = false;
        }
    }

    // --- I/O dispatcher: $50000000 - $5FFFFFFF ---
    se30->io_interface.read_uint8 = se30_io_read_uint8;
    se30->io_interface.read_uint16 = se30_io_read_uint16;
    se30->io_interface.read_uint32 = se30_io_read_uint32;
    se30->io_interface.write_uint8 = se30_io_write_uint8;
    se30->io_interface.write_uint16 = se30_io_write_uint16;
    se30->io_interface.write_uint32 = se30_io_write_uint32;

    memory_map_add(cfg->mem_map, SE30_IO_BASE, SE30_IO_SIZE, "SE/30 I/O", &se30->io_interface, cfg);

    // --- ROM overlay: map ROM at $00000000 on reset ---
    se30->rom_overlay = false; // se30_set_rom_overlay will toggle it on
    se30_set_rom_overlay(cfg, true);
}

// ============================================================
// Interrupt routing
// ============================================================

// SE/30 dual-VIA interrupt routing: combines active sources and drives CPU IPL.
// VIA1 → IPL 1, VIA2 → IPL 2, SCC → IPL 4, NMI → IPL 7.
static void se30_update_ipl(config_t *cfg, int source, bool active) {
    int old_irq = cfg->irq;

    if (active)
        cfg->irq |= source;
    else
        cfg->irq &= ~source;

    // Determine highest active IPL
    uint32_t new_ipl;
    if (cfg->irq & SE30_IRQ_NMI)
        new_ipl = 7;
    else if (cfg->irq & SE30_IRQ_SCC)
        new_ipl = 4;
    else if (cfg->irq & SE30_IRQ_VIA2)
        new_ipl = 2;
    else if (cfg->irq & SE30_IRQ_VIA1)
        new_ipl = 1;
    else
        new_ipl = 0;

    cpu_set_ipl(cfg->cpu, new_ipl);

    LOG(2, "se30_update_ipl: source=%d active=%d irq:%d->%d ipl->%d", source, active ? 1 : 0, old_irq, cfg->irq,
        new_ipl);

    cpu_reschedule();
}

// ============================================================
// VIA callbacks
// ============================================================

// VIA1 output callback: routes port A/B changes to peripherals
static void se30_via1_output(void *context, uint8_t port, uint8_t output) {
    config_t *cfg = (config_t *)context;
    se30_state_t *se30 = se30_state(cfg);

    if (port == 0) {
        // Port A outputs:
        // Bit 6: alternate screen buffer
        // Bit 5: floppy head select → SWIM
        if (se30->swim)
            swim_set_sel_signal(se30->swim, (output & 0x20) != 0);
        // Bit 4: ROM overlay control (1 = ROM at $00000000, 0 = RAM)
        se30_set_rom_overlay(cfg, (output & 0x10) != 0);
        // Bit 3: alternate sound buffer (legacy, ignored for ASC)
        // Bits 0-2: sound volume (legacy, ignored for ASC)
    }
    // Port B: no outputs on SE/30 VIA1 port B
}

// VIA1 shift-out callback: ADB byte data transfer
static void se30_via1_shift_out(void *context, uint8_t byte) {
    config_t *cfg = (config_t *)context;
    se30_state_t *se30 = se30_state(cfg);
    if (se30->adb)
        adb_shift_byte(se30->adb, byte);
}

// VIA1 IRQ callback: VIA1 drives IPL level 1
static void se30_via1_irq(void *context, bool active) {
    se30_update_ipl((config_t *)context, SE30_IRQ_VIA1, active);
}

// VIA2 output callback: routes port B changes to ADB and RTC
static void se30_via2_output(void *context, uint8_t port, uint8_t output) {
    config_t *cfg = (config_t *)context;
    se30_state_t *se30 = se30_state(cfg);

    if (port == 1) {
        // Port B outputs:
        // Bit 7: sound enable (master mute)
        // Bit 6: VSync IRQ enable
        // Bits 4-5: ADB state (ST0/ST1) → ADB controller
        if (se30->adb)
            adb_port_b_output(se30->adb, output);
        // Bit 3: ADB interrupt (input — read-only, not driven here)
        // Bit 2: RTC chip select, Bit 1: RTC clock, Bit 0: RTC data
        if (cfg->rtc)
            rtc_input(cfg->rtc, (output >> 2) & 1, (output >> 1) & 1, output & 1);
    }
    // Port A: no outputs on SE/30 VIA2 port A (slot IRQ inputs)
}

// VIA2 shift-out callback: not used on SE/30
static void se30_via2_shift_out(void *context, uint8_t byte) {
    (void)context;
    (void)byte;
}

// VIA2 IRQ callback: VIA2 drives IPL level 2
static void se30_via2_irq(void *context, bool active) {
    se30_update_ipl((config_t *)context, SE30_IRQ_VIA2, active);
}

// SCC IRQ callback: SCC drives IPL level 4
static void se30_scc_irq(void *context, bool active) {
    se30_update_ipl((config_t *)context, SE30_IRQ_SCC, active);
}

// ============================================================
// VBL trigger
// ============================================================

// Trigger vertical blanking interval for the SE/30.
// VBL is signalled via VIA1 CA1 (same edge polarity as Plus).
static void se30_trigger_vbl(config_t *cfg) {
    // Assert then deassert VIA1 CA1 to signal VBL
    via_input_c(cfg->via1, 0, 0, 0);
    via_input_c(cfg->via1, 0, 0, 1);

    image_tick_all(cfg);
}

// ============================================================
// Init / Teardown
// ============================================================

// Initialise all SE/30 subsystems.
// If checkpoint is non-NULL, each device restores state from it.
static void se30_init(config_t *cfg, checkpoint_t *checkpoint) {
    // Allocate SE/30-specific peripheral state
    se30_state_t *se30 = calloc(1, sizeof(se30_state_t));
    assert(se30 != NULL);
    cfg->machine_context = se30;

    // Initialise parameterised memory: 32-bit address space, default RAM, 256 KB ROM
    cfg->mem_map =
        memory_map_init(cfg->machine->address_bits, cfg->machine->ram_size_default, cfg->machine->rom_size, checkpoint);

    // Initialise CPU (68030)
    cfg->cpu = cpu_init(CPU_MODEL_68030, checkpoint);

    cfg->scheduler = scheduler_init(cfg->cpu, checkpoint);

    // Set SE/30 CPU clock frequency (15.6672 MHz = 2x Plus clock)
    scheduler_set_frequency(cfg->scheduler, cfg->machine->cpu_clock_hz);

    // Restore global interrupt state after scheduler
    if (checkpoint)
        system_read_checkpoint_data(checkpoint, &cfg->irq, sizeof(cfg->irq));

    // Initialise RTC (not yet wired to VIA — deferred until VIA2 exists)
    cfg->rtc = rtc_init(cfg->scheduler, checkpoint);

    // Initialise SCC (NULL map: SE/30 I/O dispatcher handles addressing)
    cfg->scc = scc_init(NULL, cfg->scheduler, se30_scc_irq, cfg, checkpoint);

    // Initialise VIA1 (NULL map: I/O dispatcher handles addressing)
    // VIA1: system events — VBL, ADB data (shift register), timers
    cfg->via1 = via_init(NULL, cfg->scheduler, se30_via1_output, se30_via1_shift_out, se30_via1_irq, cfg, checkpoint);

    // Initialise VIA2 (NULL map: I/O dispatcher handles addressing)
    // VIA2: expansion — NuBus/PDS slots, SCSI, ASC interrupts, ADB control, RTC
    cfg->via2 = via_init(NULL, cfg->scheduler, se30_via2_output, se30_via2_shift_out, se30_via2_irq, cfg, checkpoint);

    // Wire RTC 1-second tick to VIA1 CA2
    rtc_set_via(cfg->rtc, cfg->via1);

    // Set hardware ID bits:
    // VIA1 PA6 = 1 (SE/30 identification) — already 1 in default port A input (0xF7)
    // VIA2 PB3 = 0 (SE/30 identification) — set explicitly
    via_input(cfg->via2, 1, 3, 0);
    // VIA2 PA3 = 1 — fix default port A input (0xF7 has bit 3 = 0)
    // All VIA2 port A inputs should be 1 (NuBus slot IRQs are active-low, 1 = no IRQ)
    via_input(cfg->via2, 0, 3, 1);

    // Initialise ADB controller (uses VIA2 for port B control: ST0/ST1, vADBInt)
    se30->adb = adb_init(cfg->via2, cfg->scheduler, checkpoint);

    // Restore image list from checkpoint before devices that reference them
    if (checkpoint) {
        uint32_t count = 0;
        system_read_checkpoint_data(checkpoint, &count, sizeof(count));
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t len = 0;
            system_read_checkpoint_data(checkpoint, &len, sizeof(len));
            char *name = NULL;
            if (len > 0) {
                name = (char *)malloc(len);
                if (!name) {
                    char tmp;
                    for (uint32_t k = 0; k < len; ++k)
                        system_read_checkpoint_data(checkpoint, &tmp, 1);
                } else {
                    system_read_checkpoint_data(checkpoint, name, len);
                }
            }
            char writable = 0;
            system_read_checkpoint_data(checkpoint, &writable, sizeof(writable));
            uint64_t raw_size = 0;
            system_read_checkpoint_data(checkpoint, &raw_size, sizeof(raw_size));
            image_t *img = NULL;
            if (name) {
                if (raw_size > 0 && checkpoint_get_kind(checkpoint) == CHECKPOINT_KIND_CONSOLIDATED)
                    image_create_empty(name, (size_t)raw_size);
                img = image_open(name, writable != 0);
                if (!img) {
                    printf("Error: image_open failed for %s while restoring checkpoint\n", name);
                    checkpoint_set_error(checkpoint);
                }
            }
            if (storage_restore_from_checkpoint(img ? img->storage : NULL, checkpoint) != GS_SUCCESS) {
                printf("Error: storage_restore_from_checkpoint failed for %s\n", name ? name : "<unnamed>");
                checkpoint_set_error(checkpoint);
            }
            if (img)
                add_image(cfg, img);
            if (name)
                free(name);
        }
    }

    // Initialise SCSI (NULL map: I/O dispatcher handles addressing)
    cfg->scsi = scsi_init(NULL, checkpoint);

    setup_images(cfg);

    // Initialise ASC (NULL map: I/O dispatcher handles addressing)
    se30->asc = asc_init(NULL, cfg->scheduler, checkpoint);
    asc_set_via(se30->asc, cfg->via2);

    // Initialise SWIM floppy controller (NULL map: I/O dispatcher)
    se30->swim = swim_init(NULL, cfg->scheduler, checkpoint);

    // Cache device memory interfaces for the I/O dispatcher
    se30->via1_iface = via_get_memory_interface(cfg->via1);
    se30->via2_iface = via_get_memory_interface(cfg->via2);
    se30->scc_iface = scc_get_memory_interface(cfg->scc);
    se30->scsi_iface = scsi_get_memory_interface(cfg->scsi);
    se30->asc_iface = asc_get_memory_interface(se30->asc);
    se30->swim_iface = swim_get_memory_interface(se30->swim);

    // Populate SE/30 memory layout (RAM, ROM, I/O dispatcher, ROM overlay)
    se30_memory_layout_init(cfg);

    // Re-drive VIA outputs on checkpoint restore
    if (checkpoint) {
        via_redrive_outputs(cfg->via1);
        via_redrive_outputs(cfg->via2);
    }

    cfg->debugger = debug_init();

    scheduler_start(cfg->scheduler);

    // Initialise IRQ/IPL only for cold boot
    if (!checkpoint) {
        cfg->irq = 0;
        cpu_set_ipl(cfg->cpu, 0);
    }
}

// Tear down all SE/30 resources in reverse init order.
static void se30_teardown(config_t *cfg) {
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);

    se30_state_t *se30 = se30_state(cfg);

    if (se30) {
        if (se30->swim) {
            swim_delete(se30->swim);
            se30->swim = NULL;
        }
        if (se30->asc) {
            asc_delete(se30->asc);
            se30->asc = NULL;
        }
        if (se30->adb) {
            adb_delete(se30->adb);
            se30->adb = NULL;
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

    if (se30) {
        free(se30);
        cfg->machine_context = NULL;
    }
}

// ============================================================
// Checkpoint
// ============================================================

// Save complete SE/30 machine state to a checkpoint stream.
// Order must match the restore path in se30_init().
static void se30_checkpoint_save(config_t *cfg, checkpoint_t *cp) {
    se30_state_t *se30 = se30_state(cfg);

    memory_map_checkpoint(cfg->mem_map, cp);
    cpu_checkpoint(cfg->cpu, cp);
    scheduler_checkpoint(cfg->scheduler, cp);

    system_write_checkpoint_data(cp, &cfg->irq, sizeof(cfg->irq));

    rtc_checkpoint(cfg->rtc, cp);
    scc_checkpoint(cfg->scc, cp);
    via_checkpoint(cfg->via1, cp);
    via_checkpoint(cfg->via2, cp);

    adb_checkpoint(se30->adb, cp);

    // Checkpoint list of images before devices that reference them
    {
        uint32_t count = (uint32_t)cfg->n_images;
        system_write_checkpoint_data(cp, &count, sizeof(count));
        for (uint32_t i = 0; i < count; ++i)
            image_checkpoint(cfg->images[i], cp);
    }

    scsi_checkpoint(cfg->scsi, cp);
    asc_checkpoint(se30->asc, cp);
    swim_checkpoint(se30->swim, cp);
}

// ============================================================
// Machine descriptor
// ============================================================

// Macintosh SE/30 hardware profile descriptor
const hw_profile_t machine_se30 = {
    .model_name = "Macintosh SE/30",
    .model_id = "se30",

    // 68030 at 15.6672 MHz
    .cpu_model = 68030,
    .cpu_clock_hz = 15667200,
    .mmu_present = true,
    .fpu_present = false, // optional 68882, not emulated yet

    // 32-bit address space
    .address_bits = 32,
    .ram_size_default = 0x400000, // 4 MB default
    .ram_size_max = 0x8000000, // 128 MB max
    .rom_size = 0x040000, // 256 KB

    // Two VIAs, ADB present, one PDS slot (no NuBus)
    .via_count = 2,
    .has_adb = true,
    .has_nubus = false,
    .nubus_slot_count = 0,

    // Lifecycle callbacks
    .init = se30_init,
    .teardown = se30_teardown,
    .checkpoint_save = se30_checkpoint_save,
    .checkpoint_restore = NULL, // restore handled by se30_init when checkpoint != NULL
    .memory_layout_init = se30_memory_layout_init,
    .update_ipl = se30_update_ipl,
    .trigger_vbl = se30_trigger_vbl,
};
