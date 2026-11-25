// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// colorclassicii.c
// Macintosh Color Classic II ("Slice33", 33 MHz 68030, 1993; Performa 275):
// the Color Classic's compact case and 10" 512 x 384 colour CRT on a
// Sonora-decoder board -- the LC 550's logic with the Color Classic's display.
// It runs the 1 MB ROM $EDE66CBD, which finds it by the CPU ID register
// ($A55A0101, the LC 550's) and the CRT's sense code (010, the 12" RGB): the
// sense is what separates the "Slice" boards from the "Hook" ones.  See v8.h
// and sonora.c.

#include "v8.h"

#include "machine.h"
#include "pram_defaults.h"
#include "scsi.h"
#include "slot_tables.h"

// The built-in 10" colour screen: one monitor, nothing to choose.
static bool colorclassicii_monitor_at(size_t i, const char **id, const char **monitor) {
    if (i != 0)
        return false;
    *id = *monitor = "compact_10in_color";
    return true;
}

static const builtin_video_desc_t colorclassicii_builtin_video = {
    .detail = "Sonora, 512 \xc3\x97 384",
    .monitor_at = colorclassicii_monitor_at,
    .default_monitor = "compact_10in_color",
};

// The CRT is cabled as the 12" RGB (sense 010), Cuda on VIA1's shift register.
static const v8_board_t colorclassicii_board = {
    .chip = "Sonora",
    .kind = V8_KIND_SONORA,
    .desc =
        {
               .chipset = "Sonora",
               .rom_base = V8_ROM_START,
               .rom_end = V8_ROM_END,
               .io_ranges = sonora_io_ranges_tbl,
               .io_mirror_mask = V8_IO_MIRROR,
               .io_unmapped_read = 0xFF,
               .bus_err_lo = 0xFE000000u,
               .bus_err_hi = 0xFEFFFFFFu,
               },
    .pcb_ram = 4u * 1024 * 1024,
    .fb_source = V8_FB_VRAM,
    .vram_size = 512u * 1024,
    .crt_width = 512,
    .crt_height = 384,
    .crt_sense = 2,
    .asc_version = 0xE8,
    .cuda = true,
    .cpu_id = 0xA55A0101u,
};

// 4 MB soldered plus one 72-pin SIMM of 1 to 32 MB.
static const uint32_t colorclassicii_ram_options_kb[] = {4096, 5120, 6144, 8192, 12288, 20480, 36864, 0};

const hw_profile_t machine_colorclassicii = {
    .name = "Macintosh Color Classic II",
    .id = "colorclassicii",

    .cpu_model = 68030,
    .freq = 33000000,
    .mmu_kind = MMU_68030_PMMU,

    .address_bits = 32,
    .ram_default = 0x800000, // 8 MB
    .ram_max = 0x2400000, // 36 MB
    .rom_size = 0x100000, // 1 MB

    .ram_options = colorclassicii_ram_options_kb,
    .floppy_slots = mac_floppy_slots_1hd,
    .storage = mac_storage_scsi_hd_bay,
    .default_storage = mac_default_storage_hd0_cd3,
    .appletalk = true,
    .builtin_video = &colorclassicii_builtin_video,
    .cdrom_drive = &scsi_cd_drive_applecd_sc_plus,

    .pram = &pram_defaults_iici,
    .substrate = &v8_substrate,
    .board = &colorclassicii_board,
};
