// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// colorclassic.c
// Macintosh Color Classic ("Slice"/"Columbia" board, 16 MHz 68030, 1993; also
// sold as the Performa 250): the LC II's architecture in the compact case,
// around the Spice custom IC -- a V8 with SWIM2 and the front-panel buttons,
// driving a built-in 10" 512 x 384 colour CRT from 256 KB of VRAM.  Apple,
// *Macintosh Color Classic* Developer Note (1993), chapters 1-2.  See v8.h.

#include "v8.h"

#include "machine.h"
#include "pram_defaults.h"
#include "scsi.h"
#include "slot_tables.h"

// The built-in 10" colour screen: one monitor, nothing to choose.
static bool colorclassic_monitor_at(size_t i, const char **id, const char **monitor) {
    if (i != 0)
        return false;
    *id = *monitor = "compact_10in_color";
    return true;
}

static const builtin_video_desc_t colorclassic_builtin_video = {
    .detail = "Spice, 512 \xc3\x97 384",
    .monitor_at = colorclassic_monitor_at,
    .default_monitor = "compact_10in_color",
};

// The Color Classic board: VIA1 PA6/PA4/PA2/PA1 = 0/0/0/1; 4 MB soldered;
// the CRT is cabled as the 12" RGB (sense 010) -- the 512 x 384 timing of the
// LC II's video code, which the Color Classic's ROM reuses (Developer Note
// p. 47, "Video Software").
static const v8_board_t colorclassic_board = {
    .chip = "Spice",
    .desc =
        {
               .chipset = "Spice",
               .rom_base = V8_ROM_START,
               .rom_end = V8_ROM_END,
               .io_ranges = v8_io_ranges_tbl,
               .io_mirror_mask = V8_IO_MIRROR,
               .io_unmapped_read = 0xFF,
               .bus_err_lo = 0xFE000000u,
               .bus_err_hi = 0xFEFFFFFFu,
               },
    .id_strap = 0x02,
    .pcb_ram = 4u * 1024 * 1024,
    .fb_source = V8_FB_VRAM,
    .vram_size = 256u * 1024, // soldered; the SIMM socket for 256 KB more stays empty
    .crt_width = 512,
    .crt_height = 384,
    .crt_sense = 2,
    .asc_version = 0xE8,
    .cuda = true,
};

// 4 MB soldered plus a pair of 1, 2 or 4 MB SIMMs (Developer Note Figure 1-3).
static const uint32_t colorclassic_ram_options_kb[] = {4096, 6144, 8192, 10240, 0};

const hw_profile_t machine_ccl = {
    .name = "Macintosh Color Classic",
    .id = "colorclassic",

    .cpu_model = 68030,
    .freq = 15667200, // 15.6672 MHz
    .mmu_kind = MMU_68030_PMMU,

    .address_bits = 32,
    .ram_default = 0xA00000, // 10 MB
    .ram_max = 0xA00000,
    .rom_size = 0x100000, // 1 MB

    .ram_options = colorclassic_ram_options_kb,
    .floppy_slots = mac_floppy_slots_1hd,
    .storage = mac_storage_scsi_hd_bay,
    .default_storage = mac_default_storage_hd0_cd3,
    .appletalk = true,
    .builtin_video = &colorclassic_builtin_video,
    .cdrom_drive = &scsi_cd_drive_applecd_sc_plus,

    .pram = &pram_defaults_iici,
    .substrate = &v8_substrate,
    .board = &colorclassic_board,
};
