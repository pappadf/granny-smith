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
#include "floppy.h"
#include "image.h"
#include "log.h"
#include "memory.h"
#include "mmu.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "shell.h"
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

// SE/30 Video RAM: 64 KB at logical $FEE00000 (NuBus slot E, offset $E00000)
#define SE30_VRAM_BASE 0xFEE00000UL
#define SE30_VRAM_SIZE 0x00010000UL // 64 KB

// SE/30 Video ROM: 32 KB, mapped at logical $FEFF8000 (top of slot E)
// Byte lanes = $0F (all lanes), so data is contiguous in the top 32 KB
#define SE30_VROM_BASE 0xFEFF8000UL
#define SE30_VROM_SIZE 0x00008000UL // 32 KB

// ROM's MMU page table remaps NuBus slot $E to I/O space:
// logical $FExxxxxx → physical $50Fxxxxx (I/O window for pseudoslot E)
// These are the PHYSICAL addresses used after the MMU is enabled.
#define SE30_VROM_PHYS     0xFEFF8000UL // NuBus slot $E declaration ROM physical address
#define SE30_VRAM_PHYS_ALT 0x50FE0000UL // page-table-mapped VRAM physical address
#define SE30_VROM_PHYS_ALT 0x50FF8000UL // page-table-mapped VROM physical address

// VROM selection: define SE30_FORCE_SYNTHETIC_VROM to always use the
// synthesized fallback VROM, even when the real SE30.vrom is present.
// #define SE30_FORCE_SYNTHETIC_VROM

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
    floppy_t *floppy;

    // ROM overlay state (true = ROM mapped at $00000000)
    bool rom_overlay;

    // Video RAM (64 KB, separate from main RAM)
    uint8_t *vram;

    // Video ROM (real SE/30 declaration ROM for slot E, loaded from file)
    uint8_t *vrom;

    // MMU state (NULL until se30_init creates it)
    mmu_state_t *mmu;

    // Cached device interfaces for I/O dispatch
    const memory_interface_t *via1_iface;
    const memory_interface_t *via2_iface;
    const memory_interface_t *scc_iface;
    const memory_interface_t *scsi_iface;
    const memory_interface_t *asc_iface;
    const memory_interface_t *floppy_iface;

    // Previous VIA1 port B output value for filtering ADB ST transitions
    uint8_t last_port_b;

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
// VROM loading (NuBus declaration ROM for slot E)
// ============================================================

// Build a minimal fallback NuBus declaration ROM (8 KB at the top of the
// 32 KB buffer) when the real VROM binary is not available.
static void se30_build_vrom_fallback(uint8_t *rom) {
    memset(rom, 0, SE30_VROM_SIZE);
    // The fallback VROM lives in the top 8 KB of the 32 KB buffer
    // (file offsets $6000-$7FFF = NuBus $FEFFE000-$FEFFFFFF).
    uint8_t *top = rom + 0x6000;

    // ---- sResource Directory at 0x0000 ----
    top[0x00] = 0x01;
    top[0x03] = 0x14; // Board sResource (ID=1) at +0x14
    top[0x04] = 0x80;
    top[0x07] = 0x3C; // Video sResource (ID=0x80) at +0x3C
    top[0x08] = 0xFF; // end

    // ---- Board sResource at 0x0014 ----
    top[0x14] = 0x01;
    top[0x17] = 0xAC; // sRsrcType → 0x00C0
    top[0x18] = 0x02;
    top[0x1B] = 0xB8; // sRsrcName → 0x00D0
    top[0x1C] = 0x20;
    top[0x1F] = 0x0C; // BoardId = $0C
    top[0x20] = 0x22;
    top[0x22] = 0x02;
    top[0x23] = 0xE0; // PrimaryInit → 0x0300
    top[0x24] = 0xFF;

    // ---- Video sResource at 0x0040 ----
    top[0x40] = 0x01;
    top[0x43] = 0xA0; // sRsrcType → 0x00E0
    top[0x44] = 0x02;
    top[0x47] = 0xAC; // sRsrcName → 0x00F0
    top[0x48] = 0x04;
    top[0x4A] = 0x01;
    top[0x4B] = 0xB8; // sRsrcDrvrDir → 0x0200
    top[0x4C] = 0x08;
    top[0x4F] = 0x01; // sRsrcHWDevId = 1
    top[0x50] = 0x0A;
    top[0x52] = 0x01;
    top[0x53] = 0x2A; // minorBaseOS → 0x017A
    top[0x54] = 0x0B;
    top[0x56] = 0x01;
    top[0x57] = 0x2A; // minorLength → 0x017E
    top[0x58] = 0x80;
    top[0x5B] = 0xE8; // OneBitMode → 0x0140
    top[0x5C] = 0xFF;

    // ---- Type & name blocks ----
    top[0xC1] = 0x01; // catBoard
    memcpy(&top[0xD0], "Macintosh SE/30", 16);
    top[0xE1] = 0x03;
    top[0xE3] = 0x01;
    top[0xE5] = 0x01; // drSwApple = 1 (standard driver in declaration ROM)
    top[0xE7] = 0x09;
    memcpy(&top[0xF0], "Built-in Video", 15);

    // ---- PrimaryInit sBlock at 0x0300 ----
    // Modeled on the real SE/30 VROM PrimaryInit: sets spResult=1 (video
    // initialized), writes XPRAM default-video (slot $E, spID $80),
    // configures VIA PA6/PB6 bits, fills both video buffers with gray.
    top[0x303] = 0x80; // sBlock size 128
    top[0x304] = 0x02;
    top[0x305] = 0x02; // rev=2, cpu=68020
    top[0x30B] = 0x04; // code offset
    static const uint8_t primaryinit[] = {
        0x31, 0x7C, 0x00, 0x01, 0x00, 0x02, // MOVE.W #1,spResult(A0)
        0x41, 0xFA, 0x00, 0x6A, // LEA data(PC),A0
        0x20, 0x3C, 0x00, 0x02, 0x00, 0x80, // MOVE.L #$00020080,D0 (2 bytes at XPRAM $80)
        0xA0, 0x52, // _WriteXPRam
        0x20, 0x78, 0x01, 0xD4, // MOVEA.L $01D4.W,A0 (VIA1 base)
        0x08, 0xE8, 0x00, 0x06, 0x06, 0x00, // BSET #6,$600(A0) (DDRA)
        0x08, 0xE8, 0x00, 0x06, 0x04, 0x00, // BSET #6,$400(A0) (DDRB)
        0x08, 0xE8, 0x00, 0x06, 0x1E, 0x00, // BSET #6,$1E00(A0) (ORA, PA6=1)
        0x08, 0xD0, 0x00, 0x06, // BSET #6,(A0) (ORB, PB6=1)
        0x22, 0x7C, 0xFE, 0xE0, 0x00, 0x00, // MOVEA.L #$FEE00000,A1 (VRAM)
        0x24, 0x49, // MOVEA.L A1,A2
        0x2A, 0x3C, 0xAA, 0xAA, 0xAA, 0xAA, // MOVE.L #$AAAAAAAA,D5
        0xD3, 0xFC, 0x00, 0x00, 0x80, 0x40, // ADDA.L #$8040,A1 (primary buf)
        0x36, 0x3C, 0x01, 0x55, // MOVE.W #$155,D3 (342 rows)
        0x34, 0x3C, 0x00, 0x0F, // MOVE.W #$F,D2 (16 longs/row)
        0x22, 0xC5, // MOVE.L D5,(A1)+
        0x51, 0xCA, 0xFF, 0xFC, // DBF D2,*-2
        0x46, 0x85, // NOT.L D5
        0x51, 0xCB, 0xFF, 0xF2, // DBF D3,*-14
        0x22, 0x4A, // MOVEA.L A2,A1
        0xD2, 0xFC, 0x00, 0x40, // ADDA.W #$40,A1 (alt buf)
        0x36, 0x3C, 0x01, 0x55, // MOVE.W #$155,D3
        0x34, 0x3C, 0x00, 0x0F, // MOVE.W #$F,D2
        0x22, 0xC5, // MOVE.L D5,(A1)+
        0x51, 0xCA, 0xFF, 0xFC, // DBF D2,*-2
        0x46, 0x85, // NOT.L D5
        0x51, 0xCB, 0xFF, 0xF2, // DBF D3,*-14
        0x70, 0x00, // MOVEQ #0,D0
        0x4E, 0x75, // RTS
        0x0E, 0x80 // data: slot $E, spID $80
    };
    memcpy(&top[0x30C], primaryinit, sizeof(primaryinit));

    // ---- Mode sResource at 0x0140 ----
    top[0x140] = 0x01;
    top[0x143] = 0x10; // mVidParams → 0x0150
    top[0x144] = 0x03;
    top[0x147] = 0x02; // mPageCnt = 2
    top[0x148] = 0x04; // mDevType = 0
    top[0x14C] = 0xFF;

    // ---- VPBlock sBlock at 0x0150 ----
    // sBlock: 4-byte size header followed by VPBlock data
    // VPBlock fields: vpBaseOffset(4), vpRowBytes(2), vpBounds(8),
    //   vpVersion(2), vpPackType(2), vpPackSize(4), vpHRes(4), vpVRes(4),
    //   vpPixelType(2), vpPixelSize(2), vpCmpCount(2), vpCmpSize(2),
    //   vpPlaneBytes(4) = 42 bytes total
    top[0x153] = 0x2E; // sBlock size = 46 ($2E), matches real VROM
    // VPBlock data at top+0x154: offsets relative to 0x154
    top[0x156] = 0x80;
    top[0x157] = 0x40; // +0: vpBaseOffset = $00008040
    top[0x159] = 0x40; // +4: vpRowBytes = 64
    // +6: vpBounds = {0, 0, 342, 512}
    top[0x15E] = 0x01;
    top[0x15F] = 0x56; // bottom = 342
    top[0x160] = 0x02; // right high byte (512 = $0200)
    // +22: vpHRes = 72.0 ($00480000)
    top[0x16A] = 0x00;
    top[0x16B] = 0x48;
    // +26: vpVRes = 72.0
    top[0x16E] = 0x00;
    top[0x16F] = 0x48;
    // +30: vpPixelType = 0 (indexed) — default
    // +32: vpPixelSize = 1
    top[0x175] = 0x01;
    // +34: vpCmpCount = 1
    top[0x177] = 0x01;
    // +36: vpCmpSize = 1
    top[0x179] = 0x01;

    // ---- minorLength data at 0x017E = $0000D5C0 ----
    // Match real VROM: video framebuffer region size
    top[0x0180] = 0xD5;
    top[0x0181] = 0xC0;

    // ---- Driver directory at 0x0200 ----
    // With drSw=1, the ROM's video init expects a standard DRVR resource
    // via SReadDrvrName (Slot Manager trap $16). The driver directory must
    // contain ID=2 pointing to an sBlock with a valid DRVR.
    top[0x200] = 0x02;
    top[0x203] = 0x20; // ID=2 DRVR → 0x0220
    top[0x204] = 0xFF;

    // ---- DRVR sBlock at 0x0220 ----
    // Minimal video driver: all routines return noErr.
    // The ROM opens this driver to install a DCE for the video slot.
    // DRVR layout (offsets relative to DRVR start, after 4-byte sBlock size):
    //   +$00: drvrFlags    +$08: drvrOpen   +$0C: drvrCtl
    //   +$0E: drvrStatus   +$10: drvrClose  +$12: drvrName (Pascal)
    //   Name ".Display_Video_Apple_SE30" = 25 chars → +$12..+$2B (26 bytes)
    //   Code starts at +$2C (even boundary after name)
    static const uint8_t drvr_sblock[] = {
        // sBlock size (4 bytes): total size of data after this field
        0x00, 0x00, 0x00, 0x38, // 56 bytes ($38)

        // DRVR header (18 bytes: +$00..+$11)
        0x4F, 0x00, // +$00 drvrFlags: $4F00
        0x00, 0x01, // +$02 drvrDelay: 1 tick
        0x00, 0x00, // +$04 drvrEMask: 0
        0x00, 0x00, // +$06 drvrMenu: 0
        0x00, 0x2C, // +$08 drvrOpen:   offset $2C
        0x00, 0x00, // +$0A drvrPrime:  not used
        0x00, 0x30, // +$0C drvrCtl:    offset $30
        0x00, 0x34, // +$0E drvrStatus: offset $34
        0x00, 0x2C, // +$10 drvrClose:  offset $2C (same as Open)

        // drvrName: Pascal string ".Display_Video_Apple_SE30" (26 bytes: +$12..+$2B)
        0x19, // length = 25
        0x2E, 0x44, 0x69, 0x73, 0x70, 0x6C, 0x61, 0x79, // .Display
        0x5F, 0x56, 0x69, 0x64, 0x65, 0x6F, 0x5F, // _Video_
        0x41, 0x70, 0x70, 0x6C, 0x65, 0x5F, 0x53, 0x45, // Apple_SE
        0x33, 0x30, // 30

        // ---- Open/Close at +$2C: return noErr ----
        0x70, 0x00, // MOVEQ #0,D0
        0x4E, 0x75, // RTS

        // ---- Control at +$30: return noErr for all csCodes ----
        0x70, 0x00, // MOVEQ #0,D0
        0x4E, 0x75, // RTS

        // ---- Status at +$34: return noErr for all csCodes ----
        0x70, 0x00, // MOVEQ #0,D0
        0x4E, 0x75, // RTS
    };
    memcpy(&top[0x220], drvr_sblock, sizeof(drvr_sblock));

    // ---- Format Header at top+0x1FEC (last 20 bytes) ----
    top[0x1FEC] = 0x00;
    top[0x1FED] = 0xFF;
    top[0x1FEE] = 0xE0;
    top[0x1FEF] = 0x14;
    top[0x1FF2] = 0x20; // fhLength = 0x2000
    top[0x1FF8] = 0x01; // fhROMRev
    top[0x1FF9] = 0x01; // fhFormat (Apple)
    top[0x1FFA] = 0x5A;
    top[0x1FFB] = 0x93;
    top[0x1FFC] = 0x2B;
    top[0x1FFD] = 0xC7;
    top[0x1FFF] = 0x0F; // fhByteLanes

    // CRC over the top 8 KB
    uint32_t crc = 0;
    for (int i = 0; i < 0x2000; i++) {
        crc = ((crc << 1) | (crc >> 31));
        if (i < 0x1FF4 || i >= 0x1FF8)
            crc += top[i];
    }
    top[0x1FF4] = (uint8_t)(crc >> 24);
    top[0x1FF5] = (uint8_t)(crc >> 16);
    top[0x1FF6] = (uint8_t)(crc >> 8);
    top[0x1FF7] = (uint8_t)(crc);
}

// Try to load the real SE/30 VROM from a file.
// Returns true if a real VROM was loaded, false if not found.
static bool se30_load_vrom(config_t *cfg, uint8_t *vrom_buf) {
    // Search well-known paths for the real 32 KB VROM binary
    static const char *search_paths[] = {"tests/data/roms/SE30.vrom", "SE30.vrom", NULL};

    for (const char **p = search_paths; *p; p++) {
        FILE *f = fopen(*p, "rb");
        if (f) {
            size_t n = fread(vrom_buf, 1, SE30_VROM_SIZE, f);
            fclose(f);
            if (n == SE30_VROM_SIZE) {
                LOG(1, "Loaded real VROM from %s (%zu bytes)", *p, n);
                return true;
            }
        }
    }

    // Also search in the same directory as the ROM file.
    // Use pending_rom_path since rom_filename isn't set yet during init.
    const char *rom_path = memory_pending_rom_path();
    if (!rom_path)
        rom_path = memory_rom_filename(cfg->mem_map);
    if (rom_path) {
        const char *slash = strrchr(rom_path, '/');
        if (slash) {
            size_t dir_len = (size_t)(slash - rom_path + 1);
            char vrom_path[512];
            if (dir_len + sizeof("SE30.vrom") <= sizeof(vrom_path)) {
                memcpy(vrom_path, rom_path, dir_len);
                memcpy(vrom_path + dir_len, "SE30.vrom", sizeof("SE30.vrom"));
                FILE *f = fopen(vrom_path, "rb");
                if (f) {
                    size_t n = fread(vrom_buf, 1, SE30_VROM_SIZE, f);
                    fclose(f);
                    if (n == SE30_VROM_SIZE) {
                        LOG(1, "Loaded real VROM from %s (%zu bytes)", vrom_path, n);
                        return true;
                    }
                }
            }
        }
    }

    LOG(0, "FATAL: Real VROM (SE30.vrom) not found. "
           "The SE/30 requires a real Video ROM for proper VBL interrupt setup. "
           "Place SE30.vrom next to the ROM file or in tests/data/roms/.");
    return false;
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

// I/O bus cycle penalties: extra CPU clocks per byte beyond the CPI baseline.
// The GLUE ASIC holds DSACK during I/O accesses, stalling the 68030.
// VIA penalty dominates due to E-clock synchronization (~19-23 avg vs 4 for RAM).
// Set to 0 to disable penalties (preserves existing timing behaviour).
// See local/gs-docs/notes/SE30-timing.md for derivation.
#define SE30_VIA_IO_PENALTY  16 // VIA E-clock sync: ~19-23 avg, minus ~4 baseline
#define SE30_SCC_IO_PENALTY  2 // SCC with own 3.672 MHz PCLK: ~6 total, minus ~4
#define SE30_SCSI_IO_PENALTY 2 // NCR 5380: ~6 total, minus ~4
#define SE30_ASC_IO_PENALTY  2 // Apple Sound Chip: ~6 total, minus ~4
#define SE30_SWIM_IO_PENALTY 2 // Floppy controller: ~6 total, minus ~4

// Read a byte from the SE/30 I/O space.
// Masks address with $1FFFF for GLUE mirroring, then dispatches to device.
static uint8_t se30_io_read_uint8(void *ctx, uint32_t addr) {
    config_t *cfg = (config_t *)ctx;
    se30_state_t *se30 = se30_state(cfg);

    // Mirror: take lower 17 bits of the offset from I/O base
    uint32_t offset = addr & SE30_IO_MIRROR;

    // VIA chips are 8-bit devices on even byte lanes only;
    // odd-address reads return bus float ($FF)
    if (offset < IO_VIA1_END) {
        memory_io_penalty(SE30_VIA_IO_PENALTY);
        return (offset & 1) ? 0xFF : se30->via1_iface->read_uint8(cfg->via1, offset - IO_VIA1);
    }
    if (offset < IO_VIA2_END) {
        memory_io_penalty(SE30_VIA_IO_PENALTY);
        return (offset & 1) ? 0xFF : se30->via2_iface->read_uint8(cfg->via2, offset - IO_VIA2);
    }
    if (offset < IO_SCC_END) {
        memory_io_penalty(SE30_SCC_IO_PENALTY);
        return se30->scc_iface->read_uint8(cfg->scc, offset - IO_SCC);
    }
    if (offset < IO_SCSI_DRQ_END) {
        // Pseudo-DMA: 8-bit read from SCSI data register with DRQ
        memory_io_penalty(SE30_SCSI_IO_PENALTY);
        return se30->scsi_iface->read_uint8(cfg->scsi, 0);
    }
    if (offset >= IO_SCSI_REG && offset < IO_SCSI_REG_END) {
        memory_io_penalty(SE30_SCSI_IO_PENALTY);
        return se30->scsi_iface->read_uint8(cfg->scsi, offset - IO_SCSI_REG);
    }
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        // Blind pseudo-DMA: 8-bit read without DRQ check
        memory_io_penalty(SE30_SCSI_IO_PENALTY);
        return se30->scsi_iface->read_uint8(cfg->scsi, 0);
    }
    if (offset >= IO_ASC && offset < IO_ASC_END) {
        memory_io_penalty(SE30_ASC_IO_PENALTY);
        return se30->asc_iface->read_uint8(se30->asc, offset - IO_ASC);
    }
    if (offset >= IO_SWIM && offset < IO_SWIM_END) {
        memory_io_penalty(SE30_SWIM_IO_PENALTY);
        return se30->floppy_iface->read_uint8(se30->floppy, offset - IO_SWIM);
    }

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
        memory_io_penalty(SE30_SCSI_IO_PENALTY * 4); // 4 sequential 8-bit bus cycles
        uint8_t b0 = se30->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b1 = se30->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b2 = se30->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b3 = se30->scsi_iface->read_uint8(cfg->scsi, 0);
        return ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | (uint32_t)b3;
    }
    // Blind pseudo-DMA: same coalescing without DRQ check
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        memory_io_penalty(SE30_SCSI_IO_PENALTY * 4); // 4 sequential 8-bit bus cycles
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
        memory_io_penalty(SE30_VIA_IO_PENALTY);
        if (!(offset & 1))
            se30->via1_iface->write_uint8(cfg->via1, offset - IO_VIA1, value);
        return;
    }
    if (offset < IO_VIA2_END) {
        memory_io_penalty(SE30_VIA_IO_PENALTY);
        if (!(offset & 1))
            se30->via2_iface->write_uint8(cfg->via2, offset - IO_VIA2, value);
        return;
    }
    if (offset < IO_SCC_END) {
        memory_io_penalty(SE30_SCC_IO_PENALTY);
        se30->scc_iface->write_uint8(cfg->scc, offset - IO_SCC, value);
        return;
    }
    if (offset < IO_SCSI_DRQ_END) {
        // Pseudo-DMA: 8-bit write to SCSI data register (DMA mode: bit 0x200 + odd)
        memory_io_penalty(SE30_SCSI_IO_PENALTY);
        se30->scsi_iface->write_uint8(cfg->scsi, 0x201, value);
        return;
    }
    if (offset >= IO_SCSI_REG && offset < IO_SCSI_REG_END) {
        memory_io_penalty(SE30_SCSI_IO_PENALTY);
        se30->scsi_iface->write_uint8(cfg->scsi, offset - IO_SCSI_REG, value);
        return;
    }
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        // Blind pseudo-DMA write
        memory_io_penalty(SE30_SCSI_IO_PENALTY);
        se30->scsi_iface->write_uint8(cfg->scsi, 0x201, value);
        return;
    }
    if (offset >= IO_ASC && offset < IO_ASC_END) {
        memory_io_penalty(SE30_ASC_IO_PENALTY);
        se30->asc_iface->write_uint8(se30->asc, offset - IO_ASC, value);
        return;
    }
    if (offset >= IO_SWIM && offset < IO_SWIM_END) {
        memory_io_penalty(SE30_SWIM_IO_PENALTY);
        se30->floppy_iface->write_uint8(se30->floppy, offset - IO_SWIM, value);
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
        memory_io_penalty(SE30_SCSI_IO_PENALTY * 4); // 4 sequential 8-bit bus cycles
        se30->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 24));
        se30->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 16));
        se30->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 8));
        se30->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value));
        return;
    }
    // Blind pseudo-DMA write
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        memory_io_penalty(SE30_SCSI_IO_PENALTY * 4); // 4 sequential 8-bit bus cycles
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

    // --- VRAM: $FEE00000 - $FEE0FFFF (64 KB writable) ---
    // Mirror the 64 KB across the 1 MB decode window $FEE00000-$FEEFFFFF
    if (se30->vram) {
        uint32_t vram_pages = SE30_VRAM_SIZE >> PAGE_SHIFT; // 16 pages
        uint32_t vram_start_page = SE30_VRAM_BASE >> PAGE_SHIFT;
        uint32_t vram_mirror_end = (SE30_VRAM_BASE + 0x100000) >> PAGE_SHIFT; // 1 MB window
        for (uint32_t p = vram_start_page; p < vram_mirror_end && (int)p < g_page_count; p++) {
            uint32_t offset_in_vram = ((p - vram_start_page) % vram_pages) << PAGE_SHIFT;
            se30_fill_page(p, se30->vram + offset_in_vram, true);
        }
    }

    // --- VROM: $FEFF8000 - $FEFFFFFF (32 KB read-only) ---
    {
        uint32_t vrom_pages = SE30_VROM_SIZE >> PAGE_SHIFT; // 8 pages
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
        // Bit 6: screen buffer select (1 = primary at VRAM+$8040, 0 = alternate at VRAM+$0040)
        bool main_buf = (output & 0x40) != 0;
        cfg->ram_vbuf = se30->vram + (main_buf ? SE30_FB_PRIMARY_OFFSET : SE30_FB_ALTERNATE_OFFSET);
        // Bit 5: floppy head select → SWIM
        if (se30->floppy)
            floppy_set_sel_signal(se30->floppy, (output & 0x20) != 0);
        // Bit 4: ROM overlay control (1 = ROM at $00000000, 0 = RAM)
        se30_set_rom_overlay(cfg, (output & 0x10) != 0);
        // Bit 3: alternate sound buffer (legacy, ignored for ASC)
        // Bits 0-2: sound volume (legacy, ignored for ASC)
    } else {
        // Port B outputs:
        // Bits 4-5: ADB state lines (ST0/ST1) → ADB controller
        // Only notify ADB when ST bits actually transition.  The ROM
        // bit-bangs the RTC via port B bits 0-2 without intending to
        // change ST; on real hardware the ADB transceiver ignores writes
        // where the ST lines don't change electrically (BUG-004).
        if (se30->adb) {
            uint8_t st_mask = 0x30; // bits 5:4 = ST1:ST0
            uint8_t old_st = se30->last_port_b & st_mask;
            uint8_t new_st = output & st_mask;
            if (new_st != old_st) {
                adb_port_b_output(se30->adb, output);
            }
        }
        se30->last_port_b = output;
        // Bit 3: vADBInt (input — read-only, driven by ADB module)
        // Bits 0-2: RTC chip select/clock/data
        if (cfg->rtc)
            rtc_input(cfg->rtc, (output >> 2) & 1, (output >> 1) & 1, output & 1);
    }
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
        // Bit 3: SE/30 ID bit (input — not driven here)
        (void)se30;
        (void)output;
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
    config_t *cfg = (config_t *)context;
    se30_update_ipl(cfg, SE30_IRQ_VIA2, active);
}

// SCC IRQ callback: SCC drives IPL level 4
static void se30_scc_irq(void *context, bool active) {
    se30_update_ipl((config_t *)context, SE30_IRQ_SCC, active);
}

// ============================================================
// VBL trigger
// ============================================================

// Trigger vertical blanking interval for the SE/30.
// On real hardware, the GLUE chip (344S0602) drives both VIA1 CA1
// and VIA2 CA1 simultaneously from the same vertical blanking signal.
// It also asserts slot $E on VIA2 port A bit 5 (active-low) so the
// level-2 handler can identify the interrupt source. The CPU's
// interrupt priority logic services level 2 (VIA2) before level 1
// (VIA1). VIA IER masking and the CPU SR interrupt mask naturally
// prevent servicing before the ROM's Slot Manager has initialised.
static void se30_vbl_slot_deassert(void *context, uint64_t data);

static void se30_trigger_vbl(config_t *cfg) {
    // Assert slot $E on VIA2 port A bit 5 (active-low)
    via_input(cfg->via2, 0, 5, 0);

    // Pulse both VIA CA1 lines simultaneously, as the GLUE chip does
    via_input_c(cfg->via1, 0, 0, 0);
    via_input_c(cfg->via2, 0, 0, 0);
    via_input_c(cfg->via1, 0, 0, 1);
    via_input_c(cfg->via2, 0, 0, 1);

    // Deassert slot $E after the blanking interval ends
    scheduler_new_cpu_event(cfg->scheduler, &se30_vbl_slot_deassert, cfg, 0, 0, 50000);

    image_tick_all(cfg);
}

// Deferred callback: deasserts slot $E when the blanking interval ends.
static void se30_vbl_slot_deassert(void *context, uint64_t data) {
    (void)data;
    config_t *cfg = (config_t *)context;
    via_input(cfg->via2, 0, 5, 1);
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

    // ADB bus starts in IDLE state (ST1:ST0 = 11, bits 5:4 = 0x30)
    se30->last_port_b = 0x30;

    // Initialise parameterised memory: 32-bit address space, default RAM, 256 KB ROM
    cfg->mem_map =
        memory_map_init(cfg->machine->address_bits, cfg->machine->ram_size_default, cfg->machine->rom_size, checkpoint);

    // Initialise CPU (68030)
    cfg->cpu = cpu_init(CPU_MODEL_68030, checkpoint);

    cfg->scheduler = scheduler_init(cfg->cpu, checkpoint);

    // Set SE/30 CPU clock frequency (15.6672 MHz = 2x Plus clock)
    scheduler_set_frequency(cfg->scheduler, cfg->machine->cpu_clock_hz);

    // SE/30 68030: 4 cycles/instr (hw accuracy), 4 cycle/instr (fast)
    scheduler_set_cpi(cfg->scheduler, 4, 4);

    // Register VBL slot deassert event type for checkpoint save/restore
    scheduler_new_event_type(cfg->scheduler, "se30", cfg, "vbl_slot_deassert", &se30_vbl_slot_deassert);

    // Restore global interrupt state after scheduler
    if (checkpoint)
        system_read_checkpoint_data(checkpoint, &cfg->irq, sizeof(cfg->irq));

    // Initialise RTC (not yet wired to VIA — deferred until VIA2 exists)
    cfg->rtc = rtc_init(cfg->scheduler, checkpoint);

    // Initialise SCC (NULL map: SE/30 I/O dispatcher handles addressing)
    cfg->scc = scc_init(NULL, cfg->scheduler, se30_scc_irq, cfg, checkpoint);

    // SCC PCLK = C8M (7.8336 MHz), RTxC = 3.6864 MHz baud-rate crystal
    scc_set_clocks(cfg->scc, 7833600, 3686400);

    // Initialise VIA1 (NULL map: I/O dispatcher handles addressing)
    // VIA1: system events — VBL, ADB data (shift register), timers
    // freq_factor=20: SE/30 CPU runs at 15.6672 MHz, VIA φ2 clock is ~783 kHz (CPU/20)
    cfg->via1 =
        via_init(NULL, cfg->scheduler, 20, se30_via1_output, se30_via1_shift_out, se30_via1_irq, cfg, checkpoint);
    via_set_instance_name(cfg->via1, "via1");

    // Initialise VIA2 (NULL map: I/O dispatcher handles addressing)
    // VIA2: expansion — NuBus/PDS slots, SCSI, ASC interrupts, ADB control, RTC
    cfg->via2 =
        via_init(NULL, cfg->scheduler, 20, se30_via2_output, se30_via2_shift_out, se30_via2_irq, cfg, checkpoint);
    via_set_instance_name(cfg->via2, "via2");

    // Wire RTC 1-second tick to VIA1 CA2
    rtc_set_via(cfg->rtc, cfg->via1);

    // Set hardware ID bits:
    // VIA1 PA6 = 1 (SE/30 identification) — already 1 in default port A input (0xF7)
    // VIA2 PB3 = 0 (SE/30 identification) — ROM reads PA6 and PB3 to identify the board:
    //   PA6=0, PB3=0 → IIx; PA6=1, PB3=0 → SE/30; PA6=1, PB3=1 → IIcx
    via_input(cfg->via2, 1, 3, 0);
    // VIA2 PA3 = 1 — fix default port A input (0xF7 has bit 3 = 0)
    // All VIA2 port A inputs should be 1 (NuBus slot IRQs are active-low, 1 = no IRQ)
    via_input(cfg->via2, 0, 3, 1);

    // VIA2 PB6 (vSndJck) = 0: SE/30 always reports sound jack inserted
    via_input(cfg->via2, 1, 6, 0);

    // VIA2 control lines default to deasserted (HIGH) state.
    // These are active-low signals; 1 = idle / no interrupt pending.
    via_input_c(cfg->via2, 0, 0, 1); // CA1: NuBus slot IRQ (no IRQ)
    via_input_c(cfg->via2, 0, 1, 1); // CA2: SCSI DRQ (no DMA request)
    via_input_c(cfg->via2, 1, 1, 1); // CB2: SCSI IRQ (no SCSI interrupt)

    // Initialise ADB controller (uses VIA1 for shift register and port B: ST0/ST1, vADBInt)
    se30->adb = adb_init(cfg->via1, cfg->scheduler, checkpoint);
    cfg->adb = se30->adb; // expose ADB to system-level input routing

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
    scsi_set_via(cfg->scsi, cfg->via2);

    setup_images(cfg);

    // Initialise ASC (NULL map: I/O dispatcher handles addressing)
    se30->asc = asc_init(NULL, cfg->scheduler, checkpoint);
    asc_set_via(se30->asc, cfg->via2);

    // Initialise SWIM floppy controller (NULL map: I/O dispatcher)
    se30->floppy = floppy_init(FLOPPY_TYPE_SWIM, NULL, cfg->scheduler, checkpoint);
    cfg->floppy = se30->floppy; // expose floppy to generic floppy commands

    // Cache device memory interfaces for the I/O dispatcher
    se30->via1_iface = via_get_memory_interface(cfg->via1);
    se30->via2_iface = via_get_memory_interface(cfg->via2);
    se30->scc_iface = scc_get_memory_interface(cfg->scc);
    se30->scsi_iface = scsi_get_memory_interface(cfg->scsi);
    se30->asc_iface = asc_get_memory_interface(se30->asc);
    se30->floppy_iface = floppy_get_memory_interface(se30->floppy);

    // ---- VRAM / VROM / MMU wiring ----

    // Allocate 64 KB VRAM (framebuffer lives here, not in main RAM)
    se30->vram = calloc(1, SE30_VRAM_SIZE);
    assert(se30->vram != NULL);

    // Load the real SE/30 video declaration ROM from disk
    se30->vrom = calloc(1, SE30_VROM_SIZE);
    assert(se30->vrom != NULL);
    if (!se30_load_vrom(cfg, se30->vrom)) {
        fprintf(stderr, "Error: SE/30 Video ROM (SE30.vrom) not found.\n"
                        "The SE/30 emulator requires a real VROM file for proper operation.\n"
                        "Place SE30.vrom next to the ROM file or in tests/data/roms/.\n");
        exit(1);
    }

    // Create the 68030 PMMU and make it globally reachable
    uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
    uint32_t ram_size = cfg->machine->ram_size_default;
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, ram_size);
    uint32_t rom_size = cfg->machine->rom_size;

    se30->mmu = mmu_init(ram_base, ram_size, rom_data, rom_size, SE30_ROM_START, SE30_ROM_END);
    assert(se30->mmu != NULL);
    g_mmu = se30->mmu;
    cfg->cpu->mmu = se30->mmu;

    // Let the MMU resolve VRAM physical addresses during table walks.
    // VRAM stays identity-mapped at its logical base for MMU resolution.
    mmu_register_vram(se30->mmu, se30->vram, SE30_VRAM_BASE, SE30_VRAM_SIZE);

    // Let the MMU resolve VROM physical addresses during table walks.
    // TT identity-maps NuBus addresses, so physical $FEFF8000 = logical $FEFF8000.
    mmu_register_vrom(se30->mmu, se30->vrom, SE30_VROM_PHYS, SE30_VROM_SIZE);

    // Emulate the GLUE chip's transparent NuBus slot address decoding.
    // The SE/30 ROM never writes TT registers (confirmed by ROM binary scan);
    // real hardware routes $F0-$FF to the NuBus bus controller, bypassing the
    // PMMU entirely. Set TT1 so the MMU identity-maps NuBus slot space, which
    // lets phys_to_host() resolve $FE000000 to the VRAM buffer.
    // TT1: base=$F0, mask=$0F (match $F0-$FF), E=1, FC_MASK=7 (match all)
    se30->mmu->tt1 = 0xF00F8007;

    // Register alternate physical addresses for page-table-mapped access.
    // After the ROM sets up MMU page tables, logical $FExxxxxx maps to
    // physical $50Fxxxxx. Both VRAM and VROM must be accessible at their
    // page-table-mapped physical addresses in addition to their TT addresses.
    se30->mmu->vram_phys_alt = SE30_VRAM_PHYS_ALT;
    se30->mmu->vrom_phys_alt = SE30_VROM_PHYS_ALT;

    // Only NuBus expansion slots $9-$D generate bus errors on unmapped reads.
    // Slot $E is built-in video (mapped). Slot $F and $0-$8 are internal/absent
    // and return 0 without bus error, matching SE/30 GLUE chip behavior.
    se30->mmu->nubus_berr_start = 0xF9000000;
    se30->mmu->nubus_berr_end = 0xFDFFFFFF;

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
        if (se30->vrom) {
            free(se30->vrom);
            se30->vrom = NULL;
        }
        if (se30->floppy) {
            floppy_delete(se30->floppy);
            se30->floppy = NULL;
        }
        if (se30->asc) {
            asc_delete(se30->asc);
            se30->asc = NULL;
        }
        if (se30->adb) {
            adb_delete(se30->adb);
            se30->adb = NULL;
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
    floppy_checkpoint(se30->floppy, cp);

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
    .fpu_present = true, // 68882 FPU standard on SE/30

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
