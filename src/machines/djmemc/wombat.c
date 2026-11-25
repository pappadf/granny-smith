// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// wombat.c
// The five djMEMC machines (February-October 1993): the three-slot "Wombat"
// boards -- Centris 650, Quadra 650, Quadra 800 -- and the PDS-only "WLCD"
// boards -- Centris 610, Quadra 610.  One substrate (djmemc.c); the models
// differ only in data: clock, VIA1 identity straps, soldered RAM, SIMM
// sockets, NuBus sockets and the FPU.  Every board is modelled with its
// Ethernet populated: the ROM resets the SONIC unconditionally on all of
// them (a board without one would wait on it forever).
//
// The ROM tells them apart by the VIA1 port-A straps PA6/PA4/PA2/PA1
// (package and clock) under the shared CPU-ID $A55A2BAD; the BoxFlag it
// derives is Centris 650 = 24, Quadra 800 = 29, Quadra 650 = 30,
// Centris 610 = 46, Quadra 610 = 47.

#include "djmemc.h"

#include "machine.h"
#include "nubus.h"
#include "pram_defaults.h"
#include "slot_tables.h"

#include <stdint.h>

// --- Topology ----------------------------------------------------------------

// Wombat: three NuBus sockets C, D, E; the PDS shares slot E's address.
// The built-in video is pseudo-slot 9 in the system ROM, not a card.
static const nubus_slot_decl_t wombat_nubus_slots[] = {
    {.slot = 0xC, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot C", .fill_order = 1},
    {.slot = 0xD, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot D", .fill_order = 2},
    {.slot = 0xE, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot E", .fill_order = 3},
    {0},
};

// WLCD: the single expansion connector takes an adapter for one NuBus card,
// addressed as slot $E (Developer Note, "Address Maps").
static const nubus_slot_decl_t wlcd_nubus_slots[] = {
    {.slot = 0xE, .kind = NUBUS_SLOT_SOCKET, .label = "Expansion slot E (NuBus adapter)", .fill_order = 1},
    {0},
};

// RAM: 8 MB soldered + four SIMM sockets (to 136 MB) on the Wombat boards,
// 4 MB soldered + two sockets (to 68 MB) on the WLCD.  Each total is a
// population djmemc_ram_banks decomposes.
static const uint32_t wombat_ram_options_kb[] = {8192, 16384, 24576, 40960, 73728, 139264, 0};
static const uint32_t wlcd_ram_options_kb[] = {4096, 8192, 12288, 20480, 36864, 69632, 0};

#define WOMBAT_RAM_MAX 0x08800000u // 136 MB
#define WLCD_RAM_MAX   0x04400000u // 68 MB

// --- Boards --------------------------------------------------------------------

static const djmemc_board_desc_t c610_board = {
    .common = DJMEMC_BOARD_COMMON,
    .via1_id = 0x40, // WLCD, 20 MHz
    .ram_onboard = 0x00400000u,
    .simm_slots = 2,
    .vram_size = 0x00100000u, // modelled maxed (512 KB base)
};

static const djmemc_board_desc_t q610_board = {
    .common = DJMEMC_BOARD_COMMON,
    .via1_id = 0x44, // WLCD, 25 MHz
    .ram_onboard = 0x00400000u,
    .simm_slots = 2,
    .vram_size = 0x00100000u,
};

static const djmemc_board_desc_t c650_board = {
    .common = DJMEMC_BOARD_COMMON,
    .via1_id = 0x46, // Lego package, 25 MHz
    .ram_onboard = 0x00800000u,
    .simm_slots = 4,
    .vram_size = 0x00100000u,
};

static const djmemc_board_desc_t q650_board = {
    .common = DJMEMC_BOARD_COMMON,
    .via1_id = 0x52, // Lego package, 33 MHz
    .ram_onboard = 0x00800000u,
    .simm_slots = 4,
    .vram_size = 0x00100000u,
};

static const djmemc_board_desc_t q800_board = {
    .common = DJMEMC_BOARD_COMMON,
    .via1_id = 0x12, // Frigidaire package, 33 MHz
    .ram_onboard = 0x00800000u,
    .simm_slots = 4,
    .vram_size = 0x00100000u,
};

// --- Profiles --------------------------------------------------------------------

// The fields every djMEMC profile shares.
#define DJMEMC_PROFILE_COMMON                                                                                          \
    .cpu_model = 68040, .mmu_kind = MMU_68040, .address_bits = 32, .rom_size = 0x100000,                               \
    .floppy_slots = mac_floppy_slots_1hd, .storage = mac_storage_scsi_cd_bay,                                          \
    .default_storage = mac_default_storage_hd0_cd3, .appletalk = true, .builtin_video = &djmemc_builtin_video,         \
    .cdrom_drive = &scsi_cd_drive_applecd_300i, .pram = &pram_defaults_iifx, .substrate = &djmemc_substrate

const hw_profile_t machine_c610 = {
    .name = "Macintosh Centris 610",
    .id = "c610",
    DJMEMC_PROFILE_COMMON,
    .freq = 20000000,
    .fpu_absent = true, // 68LC040
    .ram_default = 0x00800000u,
    .ram_max = WLCD_RAM_MAX,
    .ram_options = wlcd_ram_options_kb,
    .nubus_slots = wlcd_nubus_slots,
    .board = &c610_board,
};

const hw_profile_t machine_q610 = {
    .name = "Macintosh Quadra 610",
    .id = "q610",
    DJMEMC_PROFILE_COMMON,
    .freq = 25000000,
    .ram_default = 0x00800000u,
    .ram_max = WLCD_RAM_MAX,
    .ram_options = wlcd_ram_options_kb,
    .nubus_slots = wlcd_nubus_slots,
    .board = &q610_board,
};

const hw_profile_t machine_c650 = {
    .name = "Macintosh Centris 650",
    .id = "c650",
    DJMEMC_PROFILE_COMMON,
    .freq = 25000000,
    .ram_default = 0x01000000u,
    .ram_max = WOMBAT_RAM_MAX,
    .ram_options = wombat_ram_options_kb,
    .nubus_slots = wombat_nubus_slots,
    .board = &c650_board,
};

const hw_profile_t machine_q650 = {
    .name = "Macintosh Quadra 650",
    .id = "q650",
    DJMEMC_PROFILE_COMMON,
    .freq = 33333333,
    .ram_default = 0x01000000u,
    .ram_max = WOMBAT_RAM_MAX,
    .ram_options = wombat_ram_options_kb,
    .nubus_slots = wombat_nubus_slots,
    .board = &q650_board,
};

const hw_profile_t machine_q800 = {
    .name = "Macintosh Quadra 800",
    .id = "q800",
    DJMEMC_PROFILE_COMMON,
    .freq = 33333333,
    .ram_default = 0x01000000u,
    .ram_max = WOMBAT_RAM_MAX,
    .ram_options = wombat_ram_options_kb,
    .nubus_slots = wombat_nubus_slots,
    .board = &q800_board,
};
