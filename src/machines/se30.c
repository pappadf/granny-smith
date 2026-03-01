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
#include "cpu_internal.h" // for cpu->mmu field
#include "debug.h"
#include "image.h"
#include "log.h"
#include "memory.h"
#include "mmu.h"
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

// SE/30 Video RAM: 64 KB at physical $FE000000 (NuBus slot E standard space)
#define SE30_VRAM_BASE 0xFE000000UL
#define SE30_VRAM_SIZE 0x00010000UL // 64 KB

// SE/30 Video ROM: 8 KB, mapped at physical $FEFFE000 (top of slot E)
// Byte lanes = $0F (all lanes), so data is contiguous at the top 8 KB
#define SE30_VROM_BASE 0xFEFFE000UL
#define SE30_VROM_SIZE 0x00002000UL // 8 KB

// Framebuffer offsets within the 64 KB VRAM
#define SE30_FB_PRIMARY_OFFSET   0x8040 // main screen buffer
#define SE30_FB_ALTERNATE_OFFSET 0x0040 // alternate screen buffer

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

    // Video RAM (64 KB, separate from main RAM)
    uint8_t *vram;

    // Video ROM (synthesised NuBus declaration ROM for slot E)
    uint8_t vrom[SE30_VROM_SIZE];

    // MMU state (NULL until se30_init creates it)
    mmu_state_t *mmu;

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
// VROM synthesis (NuBus declaration ROM for slot E)
// ============================================================

// Build a minimal but valid NuBus declaration ROM so the Slot Manager
// discovers the built-in 512×342 monochrome display at slot E.
static void se30_build_vrom(uint8_t *rom) {
    memset(rom, 0, SE30_VROM_SIZE);

    // --- sResource Directory at 0x0000 ---
    // Board sResource (ID=1)
    rom[0x00] = 0x01;
    rom[0x03] = 0x0C; // offset → 0x000C

    // Video sResource (ID=0x80)
    rom[0x04] = 0x80;
    rom[0x07] = 0x1C; // offset = 0x1C, target = 0x0004+0x1C = 0x0020

    // End of directory
    rom[0x08] = 0xFF;

    // --- Board sResource at 0x000C ---
    // sRsrcType(1) → type at 0x0034
    rom[0x0C] = 0x01;
    rom[0x0F] = 0x28; // offset = 0x28, target = 0x000C+0x28 = 0x0034

    // sRsrcName(2) → name at 0x0044
    rom[0x10] = 0x02;
    rom[0x13] = 0x34; // offset = 0x34, target = 0x0010+0x34 = 0x0044

    // BoardId(0x20) = 9 (SE/30 board ID)
    rom[0x14] = 0x20;
    rom[0x17] = 0x09;

    // PrimaryInit(0x22) → sExec at 0x0070
    rom[0x18] = 0x22;
    rom[0x1B] = 0x58; // offset = 0x58, target = 0x0018+0x58 = 0x0070

    // End
    rom[0x1C] = 0xFF;

    // --- Video sResource at 0x0020 ---
    // sRsrcType(1) → type at 0x003C
    rom[0x20] = 0x01;
    rom[0x23] = 0x1C; // offset = 0x1C, target = 0x0020+0x1C = 0x003C

    // sRsrcName(2) → name at 0x0054
    rom[0x24] = 0x02;
    rom[0x27] = 0x30; // offset = 0x30, target = 0x0024+0x30 = 0x0054

    // minorBaseOS(0x0A) = 0 (VRAM at slot base)
    rom[0x28] = 0x0A;

    // minorLength(0x0B) = 0x010000 (64 KB)
    rom[0x2C] = 0x0B;
    rom[0x2D] = 0x01;

    // End
    rom[0x30] = 0xFF;

    // --- Board type descriptor at 0x0034 (8 bytes) ---
    // {catBoard(1), typeBoard(0), drSW(0), drHW(0)}
    rom[0x35] = 0x01; // catBoard = 1

    // --- Video type descriptor at 0x003C (8 bytes) ---
    // {catDisplay(3), typeVideo(1), drSwApple(1), drHW(0)}
    rom[0x3D] = 0x03; // catDisplay = 3
    rom[0x3F] = 0x01; // typeVideo = 1
    rom[0x41] = 0x01; // drSwApple = 1

    // --- Board name at 0x0044 ---
    memcpy(&rom[0x44], "Macintosh SE/30", 16); // includes null terminator

    // --- Video name at 0x0054 ---
    memcpy(&rom[0x54], "Display_Video_Apple_SE30", 25); // includes null

    // --- PrimaryInit sExec block at 0x0070 ---
    rom[0x70] = 0x02; // revision = 2
    rom[0x71] = 0x03; // cpu = 3 (68020/68030)
    rom[0x77] = 0x08; // code offset from sExec start = 8
    rom[0x78] = 0x4E; // RTS high byte
    rom[0x79] = 0x75; // RTS low byte

    // --- Format Header at 0x1FEC (last 20 bytes of 8 KB ROM) ---
    // fhDirOffset: directory at 0x0000, fhDirOffset at 0x1FEC
    // offset = 0x0000 - 0x1FEC = -0x1FEC = 0xFFFFE014
    rom[0x1FEC] = 0xFF;
    rom[0x1FED] = 0xFF;
    rom[0x1FEE] = 0xE0;
    rom[0x1FEF] = 0x14;

    // fhLength: 0x00002000 (8192)
    rom[0x1FF2] = 0x20;

    // fhCRC: computed below (leave as 0 for now at 0x1FF4-0x1FF7)

    // fhROMRev: 1
    rom[0x1FF8] = 0x01;

    // fhFormat: 1 (AppleFormat)
    rom[0x1FF9] = 0x01;

    // fhTstPat: 0x5A932BC7
    rom[0x1FFA] = 0x5A;
    rom[0x1FFB] = 0x93;
    rom[0x1FFC] = 0x2B;
    rom[0x1FFD] = 0xC7;

    // fhReserved: 0
    // fhByteLanes: 0x0F (all four byte lanes, contiguous data)
    rom[0x1FFF] = 0x0F;

    // Compute CRC: ROL 1 + ADD for each byte, skipping the CRC field itself
    uint32_t crc = 0;
    for (int i = 0; i < (int)SE30_VROM_SIZE; i++) {
        if (i >= 0x1FF4 && i < 0x1FF8)
            continue; // skip CRC field
        crc = ((crc << 1) | (crc >> 31)) + rom[i];
    }

    // Write CRC at 0x1FF4 (big-endian)
    rom[0x1FF4] = (uint8_t)(crc >> 24);
    rom[0x1FF5] = (uint8_t)(crc >> 16);
    rom[0x1FF6] = (uint8_t)(crc >> 8);
    rom[0x1FF7] = (uint8_t)(crc);
}

// ============================================================
// SoA page helper
// ============================================================

// Populate both the AoS page_entry_t and the SoA fast-path arrays for one page.
// For read-only pages (writable=false), write SoA entries stay zero (slow-path).
static void se30_fill_page(uint32_t page_index, uint8_t *host_ptr, bool writable) {
    if ((int)page_index >= g_page_count)
        return;

    // AoS cold-path entry
    g_page_table[page_index].host_base = host_ptr;
    g_page_table[page_index].dev = NULL;
    g_page_table[page_index].dev_context = NULL;
    g_page_table[page_index].writable = writable;

    // Compute adjusted base for the SoA fast-path
    uint32_t guest_base = page_index << PAGE_SHIFT;
    uintptr_t adjusted = (uintptr_t)host_ptr - guest_base;

    // All pages are readable by supervisor and user
    if (g_supervisor_read)
        g_supervisor_read[page_index] = adjusted;
    if (g_user_read)
        g_user_read[page_index] = adjusted;

    // Only writable pages get write entries
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
        // Map ROM at $00000000 (copy from ROM region at $40000000)
        for (uint32_t p = 0; p < rom_pages && (int)p < g_page_count; p++) {
            uint8_t *host_ptr = g_page_table[rom_start_page + p].host_base;
            se30_fill_page(p, host_ptr, false); // read-only
        }
        LOG(1, "ROM overlay enabled: ROM at $00000000");
    } else {
        // Map RAM back at $00000000
        uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
        for (uint32_t p = 0; p < rom_pages && (int)p < g_page_count; p++) {
            se30_fill_page(p, ram_base + (p << PAGE_SHIFT), true); // writable
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

    // VIA chips are 8-bit devices on even byte lanes only;
    // odd-address reads return bus float ($FF)
    if (offset < IO_VIA1_END)
        return (offset & 1) ? 0xFF : se30->via1_iface->read_uint8(cfg->via1, offset - IO_VIA1);
    if (offset < IO_VIA2_END)
        return (offset & 1) ? 0xFF : se30->via2_iface->read_uint8(cfg->via2, offset - IO_VIA2);
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

    // VIA chips are 8-bit devices on even byte lanes only;
    // odd-address writes are ignored (no device on that lane)
    if (offset < IO_VIA1_END) {
        if (!(offset & 1))
            se30->via1_iface->write_uint8(cfg->via1, offset - IO_VIA1, value);
        return;
    }
    if (offset < IO_VIA2_END) {
        if (!(offset & 1))
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
// VRAM: $FE000000-$FE00FFFF (64 KB, writable)
// VROM: $FEFFE000-$FEFFFFFF (8 KB, read-only, synthesised declaration ROM)
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
    for (uint32_t p = 0; p < ram_pages && (int)p < g_page_count; p++)
        se30_fill_page(p, ram_base + (p << PAGE_SHIFT), true);

    // --- ROM pages: $40000000 - $4FFFFFFF (256 KB mirrored, read-only) ---
    uint32_t rom_pages = rom_size >> PAGE_SHIFT;
    uint32_t rom_start_page = SE30_ROM_START >> PAGE_SHIFT;
    uint32_t rom_end_page = SE30_ROM_END >> PAGE_SHIFT;

    if (rom_pages > 0) {
        for (uint32_t p = rom_start_page; p < rom_end_page && (int)p < g_page_count; p++) {
            uint32_t offset_in_rom = (p - rom_start_page) % rom_pages;
            se30_fill_page(p, rom_data + (offset_in_rom << PAGE_SHIFT), false);
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

    // --- VRAM: $FE000000 - $FE00FFFF (64 KB writable) ---
    // Mirror the 64 KB across the 1 MB decode window $FE000000-$FE0FFFFF
    if (se30->vram) {
        uint32_t vram_pages = SE30_VRAM_SIZE >> PAGE_SHIFT; // 16 pages
        uint32_t vram_start_page = SE30_VRAM_BASE >> PAGE_SHIFT;
        uint32_t vram_mirror_end = (SE30_VRAM_BASE + 0x100000) >> PAGE_SHIFT; // 1 MB window
        for (uint32_t p = vram_start_page; p < vram_mirror_end && (int)p < g_page_count; p++) {
            uint32_t offset_in_vram = ((p - vram_start_page) % vram_pages) << PAGE_SHIFT;
            se30_fill_page(p, se30->vram + offset_in_vram, true);
        }
    }

    // --- VROM: $FEFFE000 - $FEFFFFFF (8 KB read-only) ---
    {
        uint32_t vrom_pages = SE30_VROM_SIZE >> PAGE_SHIFT; // 2 pages
        uint32_t vrom_start_page = SE30_VROM_BASE >> PAGE_SHIFT;
        for (uint32_t p = 0; p < vrom_pages && (int)(vrom_start_page + p) < g_page_count; p++)
            se30_fill_page(vrom_start_page + p, se30->vrom + (p << PAGE_SHIFT), false);
    }

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
        // Bit 6: alternate screen buffer (1 = alternate at VRAM+$0040, 0 = primary at VRAM+$8040)
        bool alt_buf = (output & 0x40) != 0;
        cfg->ram_vbuf = se30->vram + (alt_buf ? SE30_FB_ALTERNATE_OFFSET : SE30_FB_PRIMARY_OFFSET);
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

    // ---- VRAM / VROM / MMU wiring ----

    // Allocate 64 KB VRAM (framebuffer lives here, not in main RAM)
    se30->vram = calloc(1, SE30_VRAM_SIZE);
    assert(se30->vram != NULL);

    // Build the synthesised NuBus declaration ROM for built-in video
    se30_build_vrom(se30->vrom);

    // Create the 68030 PMMU and make it globally reachable
    uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
    uint32_t ram_size = cfg->machine->ram_size_default;
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, ram_size);
    uint32_t rom_size = cfg->machine->rom_size;

    se30->mmu = mmu_init(ram_base, ram_size, rom_data, rom_size, SE30_ROM_START, SE30_ROM_END);
    assert(se30->mmu != NULL);
    g_mmu = se30->mmu;
    cfg->cpu->mmu = se30->mmu;

    // Let the MMU resolve VRAM physical addresses during table walks
    mmu_register_vram(se30->mmu, se30->vram, SE30_VRAM_BASE, SE30_VRAM_SIZE);

    // Point the video subsystem at the primary framebuffer in VRAM
    cfg->ram_vbuf = se30->vram + SE30_FB_PRIMARY_OFFSET;

    // Populate SE/30 memory layout (RAM, ROM, VRAM, VROM, I/O, overlay)
    se30_memory_layout_init(cfg);

    // Re-drive VIA outputs on checkpoint restore (also restores alt-buffer state)
    if (checkpoint) {
        // Restore VRAM contents
        system_read_checkpoint_data(checkpoint, se30->vram, SE30_VRAM_SIZE);

        // Re-assign MMU pointers that checkpoint_restore overwrote with stale addresses
        g_mmu = se30->mmu;
        cfg->cpu->mmu = se30->mmu;

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
        if (se30->mmu) {
            mmu_delete(se30->mmu); // also clears g_mmu if it matches
            se30->mmu = NULL;
        }
        if (se30->vram) {
            free(se30->vram);
            se30->vram = NULL;
        }
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

    // Save VRAM contents (must match restore order in se30_init)
    system_write_checkpoint_data(cp, se30->vram, SE30_VRAM_SIZE);
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
