// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// q660av.c
// Macintosh Centris/Quadra 660AV ("Tempest", 25 MHz 68040, July 1993) — the
// pizza-box sibling of the Quadra 840AV (renamed "Quadra 660AV" late in
// life).  Same 2 MB $5BF10FD1 ROM and chipset; the deltas are pure data
// (proposal-quadra-av.md §2, the q950.c pattern):
//   * 25 MHz full 68040 (not LC)
//   * YMCA strap nibble $B (Tempest25), BoxFlag 54, Gestalt 60
//   * MUNI optional and absent by default — MUNI_Control bus-errors so the
//     ROM's TestForMUNI clears MUNIExists (ymca.md §3, muni.md)

#include "av.h"

#include "machine.h"
#include "nubus.h"

#include <stdint.h>

// Same eight-bank YMCA memory system as the 840AV (RamInfoTempest is
// byte-identical to RamInfoCyclone — ymca.md §3); the 660AV's marketing
// 68 MB limit is not encoded in hardware.
static const uint32_t q660av_ram_options_kb[] = {8192, 16384, 32768, 65536, 131072, 0};

static const struct floppy_slot q660av_floppy_slots[] = {
    {0},
};

static const struct scsi_slot q660av_scsi_slots[] = {
    {.label = "SCSI HD0", .id = 0},
    {.label = "SCSI HD1", .id = 1},
    {0},
};

static const av_board_desc_t q660av_board_desc = {
    .chipset = "YMCA+PSC",
    .rom_base = 0x40800000u,
    .rom_end = 0x40A00000u,
    .io_ranges = av_io_ranges,
    .io_mirror_mask = 0x0003FFFFu,
    .io_unmapped_read = 0xFF,
    .slots = NULL, // single slot E rides the (absent) MUNI adapter
    .bus_err_lo = 0xA0000000u,
    .bus_err_hi = 0xFEFFFFFFu,
    .strap_nibble = 0xB, // Tempest25 straps %1011 (ymca.md §2)
    .muni_present = false, // no NuBus adapter: MUNI_Control bus-errors
};

static const av_board_t q660av_board = {
    .desc = &q660av_board_desc,
    .via1_output = av_via1_output,
    .via1_shift_out = av_via1_shift_out,
    .build_devices = av_build_devices,
};

const hw_profile_t machine_q660av = {
    .name = "Macintosh Quadra 660AV",
    .id = "q660av",

    .cpu_model = 68040,
    .freq = 25000000, // 25 MHz
    .mmu_kind = MMU_68040,

    .address_bits = 32,
    .ram_default = 0x1000000, // 16 MB
    .ram_max = 0x8000000, // 128 MB (8 banks x 16 MB)
    .rom_size = 0x200000, // 2 MB ($5BF10FD1, shared with the 840AV)

    .ram_options = q660av_ram_options_kb,
    .floppy_slots = q660av_floppy_slots,
    .scsi_slots = q660av_scsi_slots,
    .has_cdrom = true,
    .cdrom_id = 3,
    .has_video_in = true, // on-board DMSD/VDC digitizer (video-in.md)

    .nubus_slots = NULL,

    .substrate = &av_substrate,
    .board = &q660av_board,
};
