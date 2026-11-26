// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// memc.h
// The MEMC family (Centris 610/650, Quadra 610/650/800, and the MEMCjr
// boards: LC 475, LC 575, Quadra 605): the 68040 + MEMC/MEMCjr + IOSB
// generation.  Chip-named like mcu/, mdu/, oss/ --
// MEMC is the memory controller that defines the generation (Apple's
// codenames are "Wombat" for the three-slot boards and "WLCD" for the
// PDS-only ones).
//
// The family is three chips plus parts reused from the Quadra 700 era:
//
//   * MEMC (this file, memc.c) -- the memory controller: up to ten DRAM
//     banks, each placed by a configuration register, the ROM-at-zero
//     overlay, and the frame-buffer controller that drives the built-in
//     video.  The video half is register-compatible with the stand-alone
//     DAFB of the Quadra 700 (Apple's driver calls it "WombatDAFB"), so the
//     mcu/ DAFB model serves it, with the Antelope CLUT/DAC in place of the
//     AC842.
//   * IOSB (iosb.h, iosb.c) -- the I/O subsystem and buffer: VIA1 and VIA2,
//     the SWIM II, the sound block, the I/O address decode and its own small
//     configuration register file.  A board-independent piece: the
//     MEMCjr-era boards (LC 475/575, Quadra 605) reuse it.
//   * Kiwi -- the NuBus controller; software-compatible with the Quadra
//     700's YANCC, so it is the same latch-and-readback register file
//     (mcu/yancc.h).
//
// Everything this generation shares with the Quadra 700 -- the 53C96,
// SONIC, EASC, SWIM, DAFB-compatible video, NuBus controller, overlay,
// /SLOTIRQ and the island plumbing -- is mcu/quadra040.h's, whose block is
// the first member of memc_state_t.
//
// Machine identity is the CPU-ID register at $5FFFFFFC, $A55A2BAD on every
// member, plus the four VIA1 port-A straps PA6/PA4/PA2/PA1, which name the
// package (PA6, PA1) and the clock (PA4, PA2).
//
// Interrupts are routed as on the Quadra 700: VIA1 -> 1, VIA2 -> 2,
// SCC -> 4, NMI -> 7 (m68k_board_update_ipl).
//
// The MEMCjr boards ("Primus" LC 475, "Optimus" LC 575, "Aladdin" Quadra
// 605) are the same generation cut down: the MEMCjr memory controller (five
// banks in 1 MB units, one SIMM socket, the same DAFB-compatible video
// half), the PrimeTime I/O chip (the IOSB's register-compatible sibling),
// a Cuda for ADB, PRAM and the clock, no Ethernet and no NuBus controller.
// The ROM tells them apart by the CPU-ID register alone ($A55A22xx, the low
// bits naming board and clock); the VIA1 straps carry nothing there.
// (Apple, Macintosh LC 475 and Macintosh Quadra 605 Developer Note (1993),
// "Custom ICs", pp. 23-25.)

#ifndef GS_MACHINES_MEMC_H
#define GS_MACHINES_MEMC_H

#include "checkpoint.h"
#include "cpuid_page.h"
#include "iosb.h"
#include "m68k_board.h"
#include "m68k_board_io.h"
#include "m68k_board_overlay.h"
#include "memory.h"
#include "quadra040.h" // the device block and plumbing shared with the MCU family
#include "system_internal.h"

#include <stdbool.h>
#include <stdint.h>

struct asc;
struct cuda;
struct dafb;
struct mmu_state;
struct scsi_53c96;
struct sonic;

// === MEMC register file (I/O island $0E000) ===============================
// Longword registers, value in the low bits:
//   $00        interleave enable (bits 0-4 bank pairs, bit 5 one-buffered
//              bus, bits 9-11 the chip's version, read-only)
//   $04..$28   DRAM bank 0..9 configuration: bits 7-0 = bank start address
//              bits 29-22 (4 MB units), bit 8 = addressing mode
//   $2C        MemTop (bits 7-0, the top of stitched RAM in 4 MB units)
//   $30        configuration (wait states, ROM speed)
//   $34        refresh period
// (Apple, Macintosh Centris 610, Centris 650, and Quadra 800 Developer Note
// (1993), "MEMC IC"; the register order is the ROM's.)
#define MEMC_REG_INTERLEAVE 0x00u
#define MEMC_REG_BANK0      0x04u
#define MEMC_REG_MEMTOP     0x2Cu
#define MEMC_REG_CONFIG     0x30u
#define MEMC_REG_REFRESH    0x34u
#define MEMC_REG_COUNT      16 // longwords the MEMC decodes ($00-$3F)
#define MEMC_MAX_BANKS      10
#define MEMC_BANK_UNIT      0x00400000u // a start field counts 4 MB
#define MEMC_BANK_WINDOW    0x04000000u // 64 MB: where sizing parks each bank
#define MEMC_BANK_START_MSK 0xFFu

// MEMCjr: the same window, decoded to $7F.  The chip's registers are six
// bits wide on the data bus; the bits above come through the upper-bits
// latch at $7C.  Software writes the latch (the value's bits 6-11) just
// before a register, and after reading a register reads its bits 6-11 back
// from the latch -- the bank registers here and the frame-buffer
// controller's registers at $F9800000 alike.  Banks 0-4 at $04-$14 count
// 1 MB: a 10-bit start, its low six bits in the register, the high four
// from the latch.  MemTop ($2C) is the top of stitched RAM minus one, in
// 1 MB units.
#define MEMCJR_REG_COUNT      32 // longwords decoded ($00-$7F)
#define MEMCJR_REG_LATCH      0x7Cu
#define MEMCJR_MAX_BANKS      5
#define MEMCJR_BANK_UNIT      0x00100000u // 1 MB
#define MEMCJR_BANK_LO_MSK    0x3Fu // the bank register's six bits
#define MEMCJR_LATCH_HI_MSK   0x0Fu // the latch's four high start bits
#define MEMCJR_LATCH_HI_SHIFT 6
#define MEMCJR_LATCH_MSK      0x3Fu // the latch's six data bits

// The CPU-ID register at $5FFFFFFC: "high performance CISC" design centre,
// supplemental ID in VIA1, id $3AD.
#define MEMC_CPUID_VALUE 0xA55A2BADu

// VIA1 port-A identity straps (bits 6, 4, 2, 1).
//   PA6 PA1: package -- %11 "Lego" (C650/Q650), %01 "Frigidaire" (Q800),
//            %10 WLCD (C610/Q610)
//   PA4 PA2: CPU clock -- %00 20, %01 25, %10 33, %11 40 MHz
#define MEMC_VIA1_ID_MASK 0x56u

// The memory controller a board carries (memc.c's per-chip ops).
typedef enum memc_chip {
    MEMC_CHIP_MEMC = 0, // Wombat/WLCD: ten 4 MB-unit banks
    MEMC_CHIP_MEMCJR, // Primus/Optimus/Aladdin: five 1 MB-unit banks, registers through the latch
} memc_chip_t;

// Where a board's ADB (and its PRAM and clock) lives.
typedef enum memc_adb {
    MEMC_ADB_VIA = 0, // the VIA1 shifter and transceiver, the classic RTC on VIA1 PB
    MEMC_ADB_CUDA, // a Cuda on VIA1's SR and PB3-PB5 (the MEMCjr boards)
} memc_adb_t;

// The MEMC board descriptor: per-model data the shared substrate reads.
typedef struct memc_board_desc {
    m68k_board_desc_t common; // the fields the shared 68k code reads
    memc_chip_t chip; // MEMC or MEMCjr
    // Board features, independent of the memory controller.
    bool has_sonic; // the built-in SONIC Ethernet and its MAC PROM
    memc_adb_t adb; // VIA transceiver or Cuda
    bool video_latch; // the video registers are read through the memory controller's latch
    uint32_t cpuid; // the CPU-ID register's value (0: MEMC_CPUID_VALUE)
    uint8_t via1_id; // VIA1 PA6/PA4/PA2/PA1 straps (& MEMC_VIA1_ID_MASK)
    uint32_t ram_onboard; // soldered RAM: MEMC banks 0 (and 1) of 4 MB; MEMCjr bank 0
    uint8_t simm_slots; // 72-pin SIMM sockets; MEMC slot k = banks 2+2k, 3+2k; MEMCjr banks 1, 2
    uint32_t vram_size; // VRAM fitted (512 KB base, 1 MB maxed)
} memc_board_desc_t;

// The MEMCjr boards' CPU-ID registers: $A55A22xx, high-performance CISC
// design centre, the whole id in the register (no VIA supplement).
#define MEMCJR_CPUID_LC475 0xA55A2221u // Primus, 25 MHz
#define MEMCJR_CPUID_Q605  0xA55A2225u // Aladdin, 25 MHz
#define MEMCJR_CPUID_LC575 0xA55A222Eu // Optimus, 33 MHz

// The I/O island decode (memc.c): the IOSB's device windows plus the
// MEMC, SONIC and Kiwi register blocks that sit in the same island.
extern const board_io_range_t memc_io_ranges[];
// ...and the MEMCjr boards' (no SONIC, no PROM, no Kiwi).
extern const board_io_range_t memcjr_io_ranges[];

// The descriptor fields every MEMC board shares: the 040 Quadra windows
// (quadra040.h) with the MEMC island, the MEMC itself, the SONIC, the
// VIA ADB, and 1 MB of VRAM (modelled maxed; 512 KB base).
#define MEMC_DESC_COMMON                                                                                               \
    .common = M68K040_BOARD_COMMON("MEMC+IOSB", memc_io_ranges), .chip = MEMC_CHIP_MEMC, .has_sonic = true,            \
    .adb = MEMC_ADB_VIA, .vram_size = 0x00100000u

// ...and every MEMCjr board's: the cut-down island, the MEMCjr and its
// latched video registers, the Cuda, no Ethernet; 1 MB of VRAM (two 68-pin
// VRAM SIMMs, 512 KB or 1 MB; modelled maxed).
#define MEMCJR_DESC_COMMON                                                                                             \
    .common = M68K040_BOARD_COMMON("MEMCjr+PrimeTime", memcjr_io_ranges), .chip = MEMC_CHIP_MEMCJR,                    \
    .adb = MEMC_ADB_CUDA, .video_latch = true, .vram_size = 0x00100000u

// Unified MEMC-family machine state (cfg->machine_context).
typedef struct memc_state {
    quadra040_t q; // the shared 040 Quadra device block -- first, for quadra040_st()
    struct cuda *cuda; // the Cuda (ADB, PRAM, clock) on MEMC_ADB_CUDA boards; NULL otherwise

    cpuid_page_t cpuid; // the CPU-ID page at $5FFFF000
    memory_interface_t video_interface; // video_latch: the video registers seen through the latch
    const memory_interface_t *dafb_iface; // video_latch: the frame-buffer controller's own face

    iosb_t iosb; // IOSB configuration registers ($18000)

    // MEMC/MEMCjr register file and the bank map it drives.
    uint32_t memc_regs[MEMCJR_REG_COUNT];
    uint8_t jr_bank_hi[MEMCJR_MAX_BANKS]; // MEMCjr: the latch's high start bits each bank took
    uint32_t bank_size[MEMC_MAX_BANKS]; // installed bytes per bank (0 = empty)
    uint32_t bank_image_off[MEMC_MAX_BANKS]; // where the bank lives in the flat RAM image
    uint32_t ram_extent; // how far up the last bank mapping reached (the next remap clears below it)
} memc_state_t;

// The family's machine state.
static inline memc_state_t *memc_st(config_t *cfg) {
    return (memc_state_t *)cfg->machine_context;
}

// The one MEMC substrate; c610/c650/q610/q650/q800 and lc475/lc575/q605
// bind it.
extern const machine_substrate_t memc_substrate;

// The built-in video as a display device (the monitors the WombatDAFB
// drives: quadra040's DAFB table).
extern const builtin_video_desc_t memc_builtin_video;
// ...and the same port on the MEMCjr boards with external monitors.
extern const builtin_video_desc_t memcjr_builtin_video;

// Decompose `ram` bytes into the board's banks: the soldered 4 MB banks,
// then 72-pin SIMMs of 32/16/8/4 MB (a 32 or 8 MB SIMM is two banks, a 16 or
// 4 MB one), largest first.  False when the total is no population of this
// board.  `size` receives one entry per bank.
// On a MEMCjr board bank 0 is the soldered RAM and banks 1-2 the SIMM's
// sides (1/4/16 MB single-sided, 2/8/32 MB double-sided).
bool memc_ram_banks(const memc_board_desc_t *desc, uint32_t ram, uint32_t size[MEMC_MAX_BANKS]);

// The MEMC register window (I/O island $0E000), as board_io handler rows.
uint8_t memc_reg_read(config_t *cfg, uint32_t win_off, uint32_t addr);
void memc_reg_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value);

#endif // GS_MACHINES_MEMC_H
