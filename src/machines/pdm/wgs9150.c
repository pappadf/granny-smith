// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// wgs9150.c
// Apple Workgroup Server 9150/80 (April 1994) and 9150/120 (April 1995) —
// the Power Macintosh 8100's board in a tower server case.  What differs
// from the 8100 (Apple, "Workgroup Server 9150" Developer Note, 1995):
//
//   * its own CPU identification: the 3 low bits of the machine-ID
//     register at $5FFFFFFC read %100 (the 8100's read %011), so the
//     register is $A55A3014 — and both 9150 ROMs carry a ProductInfo
//     record for $3014 that the 6100/7100/8100 ROM lacks.  Gestalt
//     reports $27 (9150/80) or $39 (9150/120) (ibid., Table 1);
//   * a fourth NuBus slot at slot address $A, its /NMRQ on the FAT AMIC's
//     pseudo-VIA2 slot bank bit 1 ("SLT4"; ibid., Figure 1);
//   * 80 MHz on a 40 MHz bus, or 120 MHz on a 40 MHz bus (3:1; Table 2);
//   * BART 4 (9150/80) or BART 21 (9150/120) — register compatible.
//
// Each speed has its own ROM ($9C7C98F7 / $9B037F6F), so each is its own
// profile.

#include "pdm.h"
#include "slot_tables.h"

#include "nubus.h"
#include "pram_defaults.h"

// The 8100's three NuBus connectors plus the 9150's fourth at slot $A.
static const struct nubus_slot_decl wgs9150_nubus_slots[] = {
    {.slot = 0xA, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot A", .fill_order = 4},
    {.slot = 0xC, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot B", .fill_order = 1},
    {.slot = 0xD, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot C", .fill_order = 2},
    {.slot = 0xE, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot D", .fill_order = 3},
    {0},
};

// The 9150/80: 80 MHz 601 on a 40 MHz bus, as the 8100/80.
static const pdm_board_desc_t wgs9150_80_board = {
    PDM_8100_BOARD_COMMON, // 2:1 bus; the 8100's discrete 53CF96 internal bus
    .machine_id = 0x3014, // the 9150's own CPU identification (%100)
    .wait_state_penalty = 2, // the 8100's (pdm-rom-ladder L7)
    .first_nubus_slot = 0xA, // the fourth NuBus connector, slot $A
};

// The 9150/120: 120 MHz 601 on a 40 MHz bus (3:1).
static const pdm_board_desc_t wgs9150_120_board = {
    PDM_8100_BOARD_COMMON,
    .machine_id = 0x3014,
    // Sized so HWInit's bus-ratio measurement lands on 3:1 (a 40 MHz bus;
    // 2 measured 60 MHz, the 2:1 boards' answer).
    .wait_state_penalty = 3,
    .first_nubus_slot = 0xA,
};

// The two speeds differ in name, clock and board; the rest is the 8100's
// (pdm.h) with the fourth connector.
#define WGS9150_PROFILE_COMMON PDM_8100_PROFILE_COMMON, .nubus_slots = wgs9150_nubus_slots

const hw_profile_t machine_wgs9150 = {
    .name = "Workgroup Server 9150/80",
    .id = "wgs9150",
    WGS9150_PROFILE_COMMON, // the ROM: $9C7C98F7
    .freq = 80000000, // 80 MHz
    .board = &wgs9150_80_board,
};

const hw_profile_t machine_wgs9150_120 = {
    .name = "Workgroup Server 9150/120",
    .id = "wgs9150_120",
    WGS9150_PROFILE_COMMON, // the ROM: $9B037F6F
    .freq = 120000000, // 120 MHz
    .board = &wgs9150_120_board,
};
