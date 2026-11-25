// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// classicii.c
// Macintosh Classic II ("Apollo", 16 MHz 68030, 1991; also sold as the
// Performa 200): the LC's architecture in the compact case, around the Eagle
// gate array -- a V8 without VRAM, scanning a built-in 512 x 342 1-bit CRT
// out of main RAM at $9F9A80.  Apple, *Macintosh Classic II* Developer Note
// (1991), pp. 5-9.  See v8.h.

#include "v8.h"

#include "machine.h"
#include "pram_defaults.h"
#include "scsi.h"
#include "slot_tables.h"

// The built-in 9" screen: one monitor, nothing to choose.
static bool classicii_monitor_at(size_t i, const char **id, const char **monitor) {
    if (i != 0)
        return false;
    *id = *monitor = "compact_9in";
    return true;
}

static const builtin_video_desc_t classicii_builtin_video = {
    .detail = "512 \xc3\x97 342, 1-bit",
    .monitor_at = classicii_monitor_at,
    .default_monitor = "compact_9in",
};

// The Classic II board: VIA1 PA6/PA4/PA2/PA1 = 0/1/0/1; 2 MB soldered; the
// screen is the 21,888 bytes below the sound buffer at the top of the RAM
// space ($9F9A80-$9FEFFF, Developer Note Table 1-1).
static const v8_board_t classicii_board = {
    .chip = "Eagle",
    .desc =
        {
               .chipset = "Eagle",
               .rom_base = V8_ROM_START,
               .rom_end = V8_ROM_END,
               .io_ranges = v8_io_ranges_tbl,
               .io_mirror_mask = V8_IO_MIRROR,
               .io_unmapped_read = 0xFF,
               .bus_err_lo = 0xFE000000u,
               .bus_err_hi = 0xFEFFFFFFu,
               },
    .id_strap = 0x12,
    .pcb_ram = 2u * 1024 * 1024,
    .fb_source = V8_FB_DRAM,
    .dram_fb = 0x009F9A80u,
    .crt_width = 512,
    .crt_height = 342,
    .crt_sense = 7,
    .crt_mono = true,
    .asc_version = 0xE8,
};

// 2 MB soldered plus a pair of 1, 2 or 4 MB SIMMs (Developer Note p. 7).
static const uint32_t classicii_ram_options_kb[] = {2048, 4096, 6144, 10240, 0};

const hw_profile_t machine_clii = {
    .name = "Macintosh Classic II",
    .id = "classicii",

    .cpu_model = 68030,
    .freq = 15667200, // 15.6672 MHz
    .mmu_kind = MMU_68030_PMMU,

    .address_bits = 32,
    .ram_default = 0xA00000, // 10 MB
    .ram_max = 0xA00000,
    .rom_size = 0x80000, // 512 KB

    .ram_options = classicii_ram_options_kb,
    .floppy_slots = mac_floppy_slots_ext,
    .storage = mac_storage_scsi_hd_bay,
    .default_storage = mac_default_storage_hd0_cd3,
    .appletalk = true,
    .builtin_video = &classicii_builtin_video,
    .cdrom_drive = &scsi_cd_drive_applecd_sc_plus,

    .pram = &pram_defaults_iici,
    .substrate = &v8_substrate,
    .board = &classicii_board,
};
