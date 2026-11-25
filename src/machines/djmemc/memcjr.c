// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// memcjr.c
// The three MEMCjr machines (1993-1994), all on the October 1993 ROM
// $FF7439EE: the LC 475 ("Primus", LC-style low-profile case), the Quadra
// 605 ("Aladdin", the same board in its own low-profile case) and the
// LC 575 ("Optimus", the all-in-one case with a built-in 14" colour display
// and a communication slot).  One substrate with the djMEMC boards
// (djmemc.c); the models differ in data: CPU-ID, clock, FPU and display.
//
// The ROM tells them apart by the CPU-ID register alone: $A55A2221 (LC 475),
// $A55A2225 (Quadra 605), $A55A222E (LC 575); the low two bits name the clock
// (1 = 25 MHz, 2 = 33 MHz), which the ROM uses to pick the MEMCjr and
// PrimeTime timing.  The BoxFlag it derives is LC 475 = 83, LC 575 = 86,
// Quadra 605 = 88.
//
// The LC PDS (and the LC 575's communication slot) is not modelled: no
// card is offered for it, as on the LC III.

#include "djmemc.h"

#include "machine.h"
#include "nubus.h"
#include "pram_defaults.h"
#include "slot_tables.h"

#include <stdint.h>
#include <string.h>

// RAM: 4 MB soldered + one 72-pin SIMM socket of 1 to 32 MB (to 36 MB).
// (Apple, Macintosh LC 475 and Macintosh Quadra 605 Developer Note (1993),
// "RAM SIMMs", p. 9.)
static const uint32_t memcjr_ram_options_kb[] = {4096, 5120, 6144, 8192, 12288, 20480, 36864, 0};

#define MEMCJR_RAM_MAX 0x02400000u // 36 MB

// --- Boards --------------------------------------------------------------------

// VRAM: two 68-pin VRAM SIMMs, 512 KB or 1 MB; modelled maxed.
static const djmemc_board_desc_t lc475_board = {
    .common = MEMCJR_BOARD_COMMON,
    .chip = DJMEMC_CHIP_MEMCJR,
    .cpuid = MEMCJR_CPUID_LC475,
    .ram_onboard = 0x00400000u,
    .simm_slots = 1,
    .vram_size = 0x00100000u,
};

static const djmemc_board_desc_t q605_board = {
    .common = MEMCJR_BOARD_COMMON,
    .chip = DJMEMC_CHIP_MEMCJR,
    .cpuid = MEMCJR_CPUID_Q605,
    .ram_onboard = 0x00400000u,
    .simm_slots = 1,
    .vram_size = 0x00100000u,
};

static const djmemc_board_desc_t lc575_board = {
    .common = MEMCJR_BOARD_COMMON,
    .chip = DJMEMC_CHIP_MEMCJR,
    .cpuid = MEMCJR_CPUID_LC575,
    .ram_onboard = 0x00400000u,
    .simm_slots = 1,
    .vram_size = 0x00100000u,
};

// --- The LC 575's built-in display ---------------------------------------------
// The 14" colour CRT is cabled with the 640 x 480 RGB sense code (6).

#define LC575_DISPLAY       "builtin_14in_color"
#define LC575_DISPLAY_SENSE 0x6u

static bool lc575_monitor_at(size_t i, const char **id, const char **monitor) {
    if (i != 0)
        return false;
    *id = *monitor = LC575_DISPLAY;
    return true;
}

static bool lc575_monitor_sense(const char *id, uint8_t *out) {
    if (strcmp(id, LC575_DISPLAY) != 0)
        return false;
    *out = LC575_DISPLAY_SENSE;
    return true;
}

static const builtin_video_desc_t lc575_builtin_video = {
    .detail = "MEMCjr (DAFB-compatible) + Antelope, 640 \xc3\x97 480",
    .monitor_at = lc575_monitor_at,
    .monitor_sense = lc575_monitor_sense,
    .default_monitor = LC575_DISPLAY,
};

// --- Profiles --------------------------------------------------------------------

// The fields every MEMCjr profile shares.
#define MEMCJR_PROFILE_COMMON                                                                                          \
    .cpu_model = 68040, .mmu_kind = MMU_68040, .address_bits = 32, .rom_size = 0x100000,                               \
    .floppy_slots = mac_floppy_slots_1hd, .default_storage = mac_default_storage_hd0_cd3, .appletalk = true,           \
    .ram_default = 0x00800000u, .ram_max = MEMCJR_RAM_MAX, .ram_options = memcjr_ram_options_kb,                       \
    .cdrom_drive = &scsi_cd_drive_applecd_300i, .pram = &pram_defaults_iifx, .substrate = &djmemc_substrate

const hw_profile_t machine_lc475 = {
    .name = "Macintosh LC 475",
    .id = "lc475",
    MEMCJR_PROFILE_COMMON,
    .freq = 25000000,
    .fpu_absent = true, // 68LC040
    .storage = mac_storage_scsi_hd_bay,
    .builtin_video = &memcjr_builtin_video,
    .board = &lc475_board,
};

const hw_profile_t machine_q605 = {
    .name = "Macintosh Quadra 605",
    .id = "q605",
    MEMCJR_PROFILE_COMMON,
    .freq = 25000000,
    .fpu_absent = true, // 68LC040
    .storage = mac_storage_scsi_hd_bay,
    .builtin_video = &memcjr_builtin_video,
    .board = &q605_board,
};

const hw_profile_t machine_lc575 = {
    .name = "Macintosh LC 575",
    .id = "lc575",
    MEMCJR_PROFILE_COMMON,
    .freq = 33333333,
    .storage = mac_storage_scsi_cd_bay, // the internal CD-ROM bay
    .builtin_video = &lc575_builtin_video,
    .board = &lc575_board,
};
