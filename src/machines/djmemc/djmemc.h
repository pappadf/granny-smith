// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// djmemc.h
// The djMEMC family (Centris 610/650, Quadra 610/650/800): the 68040 +
// djMEMC + IOSB + Kiwi generation.  Chip-named like mcu/, mdu/, oss/ --
// djMEMC is the memory controller that defines the generation (Apple's
// codenames are "Wombat" for the three-slot boards and "WLCD" for the
// PDS-only ones).
//
// The family is three chips plus parts reused from the Quadra 700 era:
//
//   * djMEMC (this file, djmemc.c) -- the memory controller: up to ten DRAM
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
//     700's YANCC, so it is the same latch-and-readback register file.
//
// Machine identity is the CPU-ID register at $5FFFFFFC, $A55A2BAD on every
// member, plus the four VIA1 port-A straps PA6/PA4/PA2/PA1, which name the
// package (PA6, PA1) and the clock (PA4, PA2).
//
// Interrupts are routed as on the Quadra 700: VIA1 -> 1, VIA2 -> 2,
// SCC -> 4, NMI -> 7 (m68k_board_update_ipl).

#ifndef GS_MACHINES_DJMEMC_H
#define GS_MACHINES_DJMEMC_H

#include "checkpoint.h"
#include "iosb.h"
#include "m68k_board.h"
#include "m68k_board_io.h"
#include "m68k_board_overlay.h"
#include "memory.h"
#include "system_internal.h"

#include <stdbool.h>
#include <stdint.h>

struct asc;
struct dafb;
struct mmu_state;
struct scsi_53c96;
struct sonic;

// === djMEMC register file (I/O island $0E000) ===============================
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
#define DJMEMC_REG_INTERLEAVE 0x00u
#define DJMEMC_REG_BANK0      0x04u
#define DJMEMC_REG_MEMTOP     0x2Cu
#define DJMEMC_REG_CONFIG     0x30u
#define DJMEMC_REG_REFRESH    0x34u
#define DJMEMC_REG_COUNT      16 // longwords decoded ($00-$3F)
#define DJMEMC_MAX_BANKS      10
#define DJMEMC_BANK_UNIT      0x00400000u // a start field counts 4 MB
#define DJMEMC_BANK_WINDOW    0x04000000u // 64 MB: where sizing parks each bank
#define DJMEMC_BANK_START_MSK 0xFFu

// Kiwi (YANCC-compatible) register file ($28000): one bit per longword.
#define DJMEMC_KIWI_REG_COUNT 64

// The CPU-ID register at $5FFFFFFC: "high performance CISC" design centre,
// supplemental ID in VIA1, id $3AD.
#define DJMEMC_CPUID_VALUE 0xA55A2BADu

// VIA1 port-A identity straps (bits 6, 4, 2, 1).
//   PA6 PA1: package -- %11 "Lego" (C650/Q650), %01 "Frigidaire" (Q800),
//            %10 WLCD (C610/Q610)
//   PA4 PA2: CPU clock -- %00 20, %01 25, %10 33, %11 40 MHz
#define DJMEMC_VIA1_ID_MASK 0x56u

// The djMEMC board descriptor: per-model data the shared substrate reads.
typedef struct djmemc_board_desc {
    m68k_board_desc_t common; // the fields the shared 68k code reads
    uint8_t via1_id; // VIA1 PA6/PA4/PA2/PA1 straps (& DJMEMC_VIA1_ID_MASK)
    uint32_t ram_onboard; // soldered RAM: banks 0 (and 1) of 4 MB each
    uint8_t simm_slots; // 72-pin SIMM sockets; slot k = banks 2+2k, 3+2k
    uint32_t vram_size; // VRAM fitted (512 KB base, 1 MB maxed)
    bool has_sonic; // built-in Ethernet populated
} djmemc_board_desc_t;

// The I/O island decode (djmemc.c): the IOSB's device windows plus the
// djMEMC, SONIC and Kiwi register blocks that sit in the same island.
extern const board_io_range_t djmemc_io_ranges[];

// The .common block every djMEMC board shares: the 256 MB ROM window at
// $40000000 (the 1 MB ROM mirrors through it; it is linked at $40800000),
// the 256 KiB I/O island, slots $1-$8 and the NuBus standard slot space
// below the video slot bus-erroring.
#define DJMEMC_BOARD_COMMON                                                                                            \
    {                                                                                                                  \
        .chipset = "djMEMC+IOSB",                                                                                      \
        .rom_base = 0x40000000u,                                                                                       \
        .rom_end = 0x50000000u,                                                                                        \
        .io_ranges = djmemc_io_ranges,                                                                                 \
        .io_mirror_mask = 0x0003FFFFu,                                                                                 \
        .io_unmapped_read = 0xFF,                                                                                      \
        .bus_err_lo = 0xF1000000u,                                                                                     \
        .bus_err_hi = NUBUS_BERR_HI,                                                                                   \
    }

// Unified djMEMC-family machine state (cfg->machine_context).
typedef struct djmemc_state {
    struct asc *asc; // the IOSB sound block (EASC-compatible)
    struct dafb *dafb; // the djMEMC frame-buffer controller + Antelope
    struct scsi_53c96 *scsi96; // NCR 53C96
    struct sonic *sonic; // DP83932 SONIC Ethernet (no wire)
    uint8_t sonic_byte2; // high byte latched from an in-flight register write

    m68k_board_overlay_t overlay; // ROM-at-zero until the aperture is touched
    struct mmu_state *bus_mmu; // bus-side resolver for the 040 table walker

    board_io_t io; // device handles for the shared I/O engine
    memory_interface_t io_interface; // registered at the $50000000 island
    memory_interface_t cpuid_interface; // the CPU-ID page at $5FFFF000

    iosb_t iosb; // IOSB configuration registers ($18000)

    // djMEMC register file and the bank map it drives.
    uint32_t memc_regs[DJMEMC_REG_COUNT];
    uint32_t bank_size[DJMEMC_MAX_BANKS]; // installed bytes per bank (0 = empty)
    uint32_t bank_image_off[DJMEMC_MAX_BANKS]; // where the bank lives in the flat RAM image

    // Kiwi NuBus controller register file.
    uint32_t kiwi_regs[DJMEMC_KIWI_REG_COUNT];
    uint64_t kiwi_touched; // log-once bitmap

    // /SLOTIRQ aggregate: bit n = VIA2 PA n source asserting (Ethernet 0,
    // slots $A-$E 1-5, built-in video 6); CA1 follows the OR.
    uint8_t slot_pa_mask;
} djmemc_state_t;

// The family's machine state.
static inline djmemc_state_t *djmemc_st(config_t *cfg) {
    return (djmemc_state_t *)cfg->machine_context;
}

// The one djMEMC substrate; c610/c650/q610/q650/q800 bind it.
extern const machine_substrate_t djmemc_substrate;

// The built-in video as a display device (the monitors the WombatDAFB
// drives).
extern const builtin_video_desc_t djmemc_builtin_video;

// Decompose `ram` bytes into the board's banks: the soldered 4 MB banks,
// then 72-pin SIMMs of 32/16/8/4 MB (a 32 or 8 MB SIMM is two banks, a 16 or
// 4 MB one), largest first.  False when the total is no population of this
// board.  `size` receives one entry per bank.
bool djmemc_ram_banks(const djmemc_board_desc_t *desc, uint32_t ram, uint32_t size[DJMEMC_MAX_BANKS]);

// Drive one /SLOTIRQ source (VIA2 PA bit 0-6, `active` in source polarity).
void djmemc_slot_irq_source(config_t *cfg, int pa_bit, bool active);

// The djMEMC register window (I/O island $0E000), as board_io handler rows.
uint8_t djmemc_reg_read(config_t *cfg, uint32_t win_off, uint32_t addr);
void djmemc_reg_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value);

// The Kiwi NuBus controller window ($28000), latch-and-readback.
uint8_t djmemc_kiwi_read(config_t *cfg, uint32_t win_off, uint32_t addr);
uint8_t djmemc_kiwi_peek(config_t *cfg, uint32_t win_off, uint32_t addr);
void djmemc_kiwi_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value);

// The SONIC and its MAC-address PROM ($0A000, $08000).
uint8_t djmemc_sonic_read(config_t *cfg, uint32_t win_off, uint32_t addr);
void djmemc_sonic_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value);
uint8_t djmemc_prom_read(config_t *cfg, uint32_t win_off, uint32_t addr);
void djmemc_prom_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value);

#endif // GS_MACHINES_DJMEMC_H
