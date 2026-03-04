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

// SE/30 Video RAM: 64 KB at logical $FE000000 (NuBus slot E standard space)
#define SE30_VRAM_BASE 0xFE000000UL
#define SE30_VRAM_SIZE 0x00010000UL // 64 KB

// SE/30 Video ROM: 8 KB, mapped at logical $FEFFE000 (top of slot E)
// Byte lanes = $0F (all lanes), so data is contiguous at the top 8 KB
#define SE30_VROM_BASE 0xFEFFE000UL
#define SE30_VROM_SIZE 0x00002000UL // 8 KB

// ROM's MMU page table remaps NuBus slot $E to I/O space:
// logical $FExxxxxx → physical $50Fxxxxx (I/O window for pseudoslot E)
// These are the PHYSICAL addresses used after the MMU is enabled.
#define SE30_VROM_PHYS     0xFEFFE000UL // NuBus slot $E declaration ROM physical address
#define SE30_VRAM_PHYS_ALT 0x50F00000UL // page-table-mapped VRAM physical address
#define SE30_VROM_PHYS_ALT 0x50FFE000UL // page-table-mapped VROM physical address

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
// VROM synthesis (NuBus declaration ROM for slot E)
// ============================================================

// Build a minimal but valid NuBus declaration ROM so the Slot Manager
// discovers the built-in 512×342 monochrome display at slot E.
static void se30_build_vrom(uint8_t *rom) {
    memset(rom, 0, SE30_VROM_SIZE);

    // ---- sResource Directory at 0x0000 ----
    // Board sResource (ID=1) at 0x0014
    rom[0x00] = 0x01;
    rom[0x03] = 0x14; // offset → 0x0000+0x14 = 0x0014

    // Video sResource (ID=0x80) at 0x0040
    rom[0x04] = 0x80;
    rom[0x07] = 0x3C; // offset → 0x0004+0x3C = 0x0040

    // End of directory
    rom[0x08] = 0xFF;

    // ---- Board sResource at 0x0014 ----
    // sRsrcType(1) → type at 0x00C0
    rom[0x14] = 0x01;
    rom[0x17] = 0xAC; // offset → 0x0014+0xAC = 0x00C0

    // sRsrcName(2) → name at 0x00D0
    rom[0x18] = 0x02;
    rom[0x1B] = 0xB8; // offset → 0x0018+0xB8 = 0x00D0

    // BoardId(0x20) = 0x0C (SE/30 VROM board ID, matches real hardware)
    rom[0x1C] = 0x20;
    rom[0x1F] = 0x0C;

    // PrimaryInit(0x22) → sExec at 0x0100
    rom[0x20] = 0x22;
    rom[0x23] = 0xE0; // offset → 0x0020+0xE0 = 0x0100

    // End
    rom[0x24] = 0xFF;

    // ---- Video sResource at 0x0040 ----
    // Entries MUST be in ascending ID order per NuBus declaration ROM spec.
    // sRsrcType(1) → type at 0x00E0
    rom[0x40] = 0x01;
    rom[0x43] = 0xA0; // offset → 0x0040+0xA0 = 0x00E0

    // sRsrcName(2) → name at 0x00F0
    rom[0x44] = 0x02;
    rom[0x47] = 0xAC; // offset → 0x0044+0xAC = 0x00F0

    // sRsrcDrvrDir(4) → driver directory at 0x0200
    rom[0x48] = 0x04;
    rom[0x4A] = 0x01;
    rom[0x4B] = 0xB8; // offset → 0x0048+0x1B8 = 0x0200

    // sRsrcHWDevId(8) = 1 (immediate, matches real VROM)
    rom[0x4C] = 0x08;
    rom[0x4F] = 0x01;

    // minorBaseOS(0x0A) → data at 0x017A (4-byte value = 0)
    // NuBus format: 3-byte field is self-relative offset to a 4-byte data block
    rom[0x50] = 0x0A;
    rom[0x52] = 0x01;
    rom[0x53] = 0x2A; // offset → 0x0050+0x012A = 0x017A

    // minorLength(0x0B) → data at 0x017E (4-byte value = $10000)
    rom[0x54] = 0x0B;
    rom[0x56] = 0x01;
    rom[0x57] = 0x2A; // offset → 0x0054+0x012A = 0x017E

    // OneBitMode(0x80) → mode sResource at 0x0140
    rom[0x58] = 0x80;
    rom[0x5B] = 0xE8; // offset → 0x0058+0xE8 = 0x0140

    // End
    rom[0x5C] = 0xFF;

    // ---- Board type descriptor at 0x00C0 (8 bytes) ----
    // {catBoard(1), typeBoard(0), drSW(0), drHW(0)}
    rom[0xC1] = 0x01; // catBoard = 1

    // ---- Board name at 0x00D0 ----
    memcpy(&rom[0xD0], "Macintosh SE/30", 16); // includes null terminator

    // ---- Video type descriptor at 0x00E0 (8 bytes) ----
    // {catDisplay(3), typeVideo(1), drSwApple(1), drHW(9)}
    rom[0xE1] = 0x03; // catDisplay = 3
    rom[0xE3] = 0x01; // typeVideo = 1
    rom[0xE5] = 0x01; // drSwApple = 1
    rom[0xE7] = 0x09; // drHW = 9 (SE/30 built-in video, matches real VROM)

    // ---- Video name at 0x00F0 (max 16 bytes to avoid overlapping sExec at 0x100) ----
    memcpy(&rom[0xF0], "Built-in Video", 15); // 14 chars + null = 15 bytes

    // ---- PrimaryInit sBlock at 0x0100 ----
    // sBlock-wrapped sExec: the Slot Manager copies the sBlock to RAM before
    // executing.  A raw sExec (no sBlock) is silently rejected by the Mac ROM.
    //
    // PrimaryInit writes default video monitor to extended PRAM[0x80-0x81].
    //
    // sBlock layout (30 bytes = $1E):
    //   +0  sBlock size (long, includes this field)
    //   +4  sExec revision (byte) = 2
    //   +5  sExec cpu      (byte) = 2 (68020)
    //   +6  reserved       (word) = 0
    //   +8  codeOffset     (long) = 4 (bytes from this field to code)
    //   +12 code (18 bytes)
    rom[0x100] = 0x00;
    rom[0x101] = 0x00; // sBlock size high
    rom[0x102] = 0x00;
    rom[0x103] = 0x1E; // sBlock size = 30
    rom[0x104] = 0x02; // revision = 2
    rom[0x105] = 0x02; // cpu = 2 (68020)
    // bytes 0x106-0x107 reserved = 0
    rom[0x10B] = 0x04; // code offset = 4
    // 68K code at 0x010C:
    //   LEA     data(PC),A0     ; A0 → inline data
    //   MOVE.L  #$00800002,D0   ; offset=$80, count=2
    //   _WriteXPRam             ; A052
    //   MOVEQ   #0,D0           ; return noErr
    //   RTS
    //   data: DC.B $0E, $80     ; slot $E, spID $80
    rom[0x10C] = 0x41;
    rom[0x10D] = 0xFA; // LEA d16(PC),A0
    rom[0x10E] = 0x00;
    rom[0x10F] = 0x0E; // d16 = $000E → data at $011C
    rom[0x110] = 0x20;
    rom[0x111] = 0x3C; // MOVE.L #imm,D0
    rom[0x112] = 0x00;
    rom[0x113] = 0x80; // high word = $0080 (offset)
    rom[0x114] = 0x00;
    rom[0x115] = 0x02; // low word = $0002 (count)
    rom[0x116] = 0xA0;
    rom[0x117] = 0x52; // _WriteXPRam
    rom[0x118] = 0x70;
    rom[0x119] = 0x00; // MOVEQ #0,D0
    rom[0x11A] = 0x4E;
    rom[0x11B] = 0x75; // RTS
    rom[0x11C] = 0x0E; // data: slot $E
    rom[0x11D] = 0x80; // data: spID $80

    // ---- Mode sResource at 0x0140 (one-bit mode) ----
    // mVidParams(1) → VPBlock at 0x0150
    rom[0x140] = 0x01;
    rom[0x143] = 0x10; // offset → 0x0140+0x10 = 0x0150

    // mPageCnt(3) = 2 (two video pages, immediate value)
    rom[0x144] = 0x03;
    rom[0x147] = 0x02;

    // mDevType(4) = 0 (bitmap device, immediate value)
    rom[0x148] = 0x04;
    // rom[0x14B] = 0x00; // already zero from memset

    // End
    rom[0x14C] = 0xFF;

    // ---- VPBlock at 0x0150 (42 bytes) ----
    // vpBaseOffset (4 bytes): framebuffer at $FE008040, slot-relative = 0x8040
    rom[0x150] = 0x00;
    rom[0x151] = 0x00;
    rom[0x152] = 0x80;
    rom[0x153] = 0x40;

    // vpRowBytes (2 bytes): 64 bytes per scan line
    rom[0x155] = 0x40;

    // vpBounds: {top=0, left=0, bottom=342, right=512}
    // top (2 bytes) = 0 at 0x156
    // left (2 bytes) = 0 at 0x158
    // bottom (2 bytes) = 342 = 0x0156
    rom[0x15A] = 0x01;
    rom[0x15B] = 0x56;
    // right (2 bytes) = 512 = 0x0200
    rom[0x15C] = 0x02;

    // vpVersion (2 bytes) = 0 at 0x15E
    // vpPackType (2 bytes) = 0 at 0x160
    // vpPackSize (4 bytes) = 0 at 0x162

    // vpHRes (4 bytes) = 72.0 dpi = 0x00480000
    rom[0x166] = 0x00;
    rom[0x167] = 0x48;

    // vpVRes (4 bytes) = 72.0 dpi = 0x00480000
    rom[0x16A] = 0x00;
    rom[0x16B] = 0x48;

    // vpPixelType (2 bytes) = 0 (chunky) at 0x16E
    // vpPixelSize (2 bytes) = 1 (1 bit per pixel)
    rom[0x171] = 0x01;
    // vpCmpCount (2 bytes) = 1
    rom[0x173] = 0x01;
    // vpCmpSize (2 bytes) = 1
    rom[0x175] = 0x01;
    // vpPlaneBytes (4 bytes) = 0 at 0x176

    // ---- minorBaseOS data at 0x017A (4 bytes) ----
    // value = 0 (VRAM starts at slot base offset 0) — already zero from memset

    // ---- minorLength data at 0x017E (4 bytes) ----
    // value = $00010000 (64 KB framebuffer)
    rom[0x017F] = 0x01;

    // ---- Driver directory at 0x0200 ----
    // sMacOS68020(2) → sBlock at 0x0220
    rom[0x200] = 0x02;
    rom[0x203] = 0x20; // offset → 0x0200+0x20 = 0x0220
    // End
    rom[0x204] = 0xFF;

    // ---- sBlock-wrapped DRVR at 0x0220 ----
    // sBlock size (4 bytes): DRVR is 106 bytes = 0x6A
    rom[0x222] = 0x00;
    rom[0x223] = 0x6A;

    // DRVR header at 0x0224 (18 bytes)
    // drvrFlags = $4C00 (dWritEnable | dNeedGoodBye | dNeedTime)
    rom[0x224] = 0x4C;
    // drvrDelay, drvrEMask, drvrMenu = 0 (already zero)
    // drvrOpen → DRVR+$2C (noErr stub)
    rom[0x22D] = 0x2C;
    // drvrPrime = 0 (unused)
    // drvrControl → DRVR+$30 (control handler with cscGrayPage)
    rom[0x231] = 0x30;
    // drvrStatus → DRVR+$2C (noErr stub)
    rom[0x233] = 0x2C;
    // drvrClose → DRVR+$2C (noErr stub)
    rom[0x235] = 0x2C;

    // Driver name (Pascal string): ".Display_Video_Apple_SE30" (25 chars)
    rom[0x236] = 25;
    memcpy(&rom[0x237], ".Display_Video_Apple_SE30", 25);

    // ---- Open/Close/Status stub at DRVR+$2C = VROM 0x0250 ----
    //   MOVEQ   #0,D0
    //   RTS
    rom[0x250] = 0x70;
    rom[0x251] = 0x00; // MOVEQ #0,D0
    rom[0x252] = 0x4E;
    rom[0x253] = 0x75; // RTS

    // ---- Control handler at DRVR+$30 = VROM 0x0254 ----
    // Dispatches cscGrayPage (csCode=5) to fill VRAM with gray pattern.
    // All other Control calls return noErr.
    //
    //   MOVE.W  $1A(A0),D0          ; read csCode from param block
    //   CMPI.W  #5,D0               ; cscGrayPage?
    //   BEQ.S   gray                ; → gray fill
    //   MOVEQ   #0,D0               ; noErr
    //   RTS
    // gray:
    //   MOVEM.L D2-D5/A1,-(SP)
    //   MOVEA.L #$FE008040,A1       ; VRAM primary framebuffer
    //   MOVE.L  #$AAAAAAAA,D5       ; gray pattern
    //   MOVE.W  #$0155,D3           ; 342 rows - 1
    // .row:
    //   MOVE.W  #$000F,D2           ; 16 longs/row - 1
    // .col:
    //   MOVE.L  D5,(A1)+
    //   DBRA    D2,.col
    //   NOT.L   D5                   ; alternate pattern
    //   DBRA    D3,.row
    //   MOVEM.L (SP)+,D2-D5/A1
    //   MOVEQ   #0,D0
    //   RTS
    static const uint8_t ctl_code[] = {
        0x30, 0x28, 0x00, 0x1A, // MOVE.W $1A(A0),D0
        0x0C, 0x40, 0x00, 0x05, // CMPI.W #5,D0
        0x67, 0x04, // BEQ.S gray (+4)
        0x70, 0x00, // MOVEQ #0,D0
        0x4E, 0x75, // RTS
        // gray:
        0x48, 0xE7, 0x3C, 0x40, // MOVEM.L D2-D5/A1,-(SP)
        0x22, 0x7C, 0xFE, 0x00, 0x80, 0x40, // MOVEA.L #$FE008040,A1
        0x2A, 0x3C, 0xAA, 0xAA, 0xAA, 0xAA, // MOVE.L #$AAAAAAAA,D5
        0x36, 0x3C, 0x01, 0x55, // MOVE.W #$0155,D3
        // .row:
        0x34, 0x3C, 0x00, 0x0F, // MOVE.W #$000F,D2
        // .col:
        0x22, 0xC5, // MOVE.L D5,(A1)+
        0x51, 0xCA, 0xFF, 0xFC, // DBRA D2,.col
        0x46, 0x85, // NOT.L D5
        0x51, 0xCB, 0xFF, 0xF2, // DBRA D3,.row
        0x4C, 0xDF, 0x02, 0x3C, // MOVEM.L (SP)+,D2-D5/A1
        0x70, 0x00, // MOVEQ #0,D0
        0x4E, 0x75 // RTS
    };
    memcpy(&rom[0x254], ctl_code, sizeof(ctl_code));

    // ---- Format Header at 0x1FEC (last 20 bytes of 8 KB ROM) ----
    // fhDirOffset: 24-bit signed backward offset from this field to directory.
    // Directory at $FEFFE000, field at $FEFFFFEC → offset = -$1FEC = $FFE014 (24-bit).
    // Upper byte must be zero; the Slot Manager sign-extends from 24 bits.
    rom[0x1FEC] = 0x00;
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

    // Compute CRC: ROL 1 for every byte, ADD for non-CRC bytes.
    // The Slot Manager always rotates, but only adds bytes outside the CRC field.
    uint32_t crc = 0;
    for (int i = 0; i < (int)SE30_VROM_SIZE; i++) {
        crc = ((crc << 1) | (crc >> 31)); // ROL.L #1 (always)
        if (i < 0x1FF4 || i >= 0x1FF8)
            crc += rom[i]; // ADD only for non-CRC bytes
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
    } else {
        // Port B outputs:
        // Bits 4-5: ADB state lines (ST0/ST1) → ADB controller
        // Only notify ADB when ST bits actually transition.  The ROM
        // bit-bangs the RTC via port B bits 0-2 without intending to
        // change ST; on real hardware the ADB transceiver ignores writes
        // where the ST lines don't change electrically (BUG-004).
        if (se30->adb) {
            uint8_t st_mask = 0x30; // bits 5:4 = ST1:ST0
            if ((output & st_mask) != (se30->last_port_b & st_mask))
                adb_port_b_output(se30->adb, output);
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

    // Pre-initialise PRAM video default so _GetVideoDefault returns slot $E.
    // PRAM offset $80 = spSlot (0x0E = slot E), $81 = spID (0x80 = video sResource).
    // Lock these addresses so the ROM's sInitSlotPRAM cannot overwrite them.
    if (!checkpoint) {
        rtc_write_pram(cfg->rtc, 0x80, 0x0E);
        rtc_write_pram(cfg->rtc, 0x81, 0x80);
        rtc_lock_pram(cfg->rtc, 0x80);
        rtc_lock_pram(cfg->rtc, 0x81);
    }

    // Set hardware ID bits:
    // VIA1 PA6 = 1 (SE/30 identification) — already 1 in default port A input (0xF7)
    // VIA2 PB3 = 0 (SE/30 identification) — set explicitly
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

    // Let the MMU resolve VRAM physical addresses during table walks.
    // VRAM stays identity-mapped at its logical base for MMU resolution.
    mmu_register_vram(se30->mmu, se30->vram, SE30_VRAM_BASE, SE30_VRAM_SIZE);

    // Let the MMU resolve VROM physical addresses during table walks.
    // TT identity-maps NuBus addresses, so physical $FEFFE000 = logical $FEFFE000.
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
