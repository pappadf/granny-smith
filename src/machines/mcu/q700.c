// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// q700.c
// Macintosh Quadra 700 ("Spike", 25 MHz 68040, October 1991) — the desktop
// member of the MCU/DAFB family and Granny Smith's first 68040 machine.
// Low-speed I/O is direct and IIci-like: VIA1+VIA2, classic
// RTC/PRAM, VIA-shifter ADB transceiver, direct SCC and SWIM.  No IOPs, no
// Caboose, one SCSI bus.  Shares the 420DBFF3 ROM with the Quadra 900 (model
// sense on VIA1 PA distinguishes them).

#include "machine_checkpoint.h"
#include "mcu.h"

#include "mac_host_io.h"
#include "machine.h"
#include "slot_tables.h"
#include "system_internal.h"

#include "adb.h"
#include "asc.h"
#include "checkpoint.h"
#include "checkpoint_machine.h"
#include "cpu.h"
#include "cpu_internal.h" // cpu->mmu — the CPU-owned 040 MMU register file
#include "dafb.h"
#include "floppy.h"
#include "image.h"
#include "memory.h"
#include "mmu.h"
#include "mmu040.h"
#include "nubus.h"
#include "pram_defaults.h"
#include "rom.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "scsi_53c96.h"
#include "sonic.h"
#include "via.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// The VIA wiring is the direct IIci-like one quadra040 provides: VIA1 PA5
// floppy head select, PB the classic RTC serial (bits 0-2) and the ADB ST
// lines (bits 4-5); VIA2 PB4/PB3/PB0 drive the DFAC serial control (not
// modelled).  EASC -> VIA2 CB1, 53C96 -> VIA2 CB2, SONIC -> VIA2 PA0
// through the /SLOTIRQ aggregate (A/UX level-3 remap not modeled).

// ============================================================
// Device construction (mcu_board_t.build_devices)
// ============================================================

static int q700_build_devices(config_t *cfg, checkpoint_t *cp) {
    const mcu_board_t *board = (const mcu_board_t *)cfg->machine->board;
    const mcu_board_desc_t *desc = board->desc;

    // The Q700 bit-bangs the classic RTC on VIA1.
    rtc_set_via(cfg->rtc, cfg->via1);

    // Model sense on VIA1 PA from the board descriptor ($C0: the ROM's
    // identify table matches PA & $56 == $40 for the Q700) plus the PA0
    // diagnostic strap; the VIA1/VIA2 lines idle (Q700 VIA2 PA exposes D,
    // E/PDS, video, Ethernet).
    quadra040_idle_lines(cfg, desc->via1_pa_model | 1u);

    machine_part_begin(cfg, cp, "adb");
    cfg->adb = adb_init(cfg->via1, cfg->scheduler, cp);
    machine_part(cfg, cp, "adb", part_save_adb, cfg->adb);

    machine_part_images(cfg, cp);

    quadra040_build_scsi0(cfg, cp, quadra040_scsi96_irq);

    // SONIC Ethernet (~20 MHz part on the Q700), the EASC and the SWIM.
    quadra040_build_sonic_asc_floppy(cfg, cp, quadra040_sonic_irq, quadra040_asc_irq);

    if (mcu_build_dafb(cfg, cp) != 0)
        return -1;

    // Bus-side physical resolver for the 040 walker.
    if (quadra040_build_bus_mmu(cfg) != 0)
        return -1;

    // Bind the I/O island + DAFB apertures + overlay, then arm the overlay.
    mcu_io_bind(cfg);
    mcu_memory_layout(cfg);

    mcu_private_part(cfg, cp);
    return 0;
}

// ============================================================
// Machine descriptor
// ============================================================

// Official totals 4/8/20 MB plus the accepted-in-practice 36/68 MB SIMM
// configurations (extended sizes, flagged as such).
static const uint32_t q700_ram_options_kb[] = {4096, 8192, 20480, 36864, 69632, 0};

// NuBus topology: two sockets, D and E; the PDS is mechanically
// aligned with slot E (a PDS card precludes a NuBus card there — not modeled
// as a constraint in v1).  The built-in DAFB video is pseudo-slot 9: its
// declaration ROM lives in the system ROM and its apertures are mapped
// directly by the substrate, so it is not a card on this bus.
static const nubus_slot_decl_t q700_nubus_slots[] = {
    {.slot = 0xD, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot D", .fill_order = 1},
    {.slot = 0xE, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot E", .fill_order = 2},
    {0},
};

static const mcu_board_desc_t q700_board_desc = {
    .common = MCU_BOARD_COMMON(mcu_q700_io_ranges),
    .ram_onboard_size = 0x00400000u, // 4 MB soldered = bank A; SIMM bank B follows
    .ram_bank_count = 2, // 4 MB soldered + one four-SIMM bank
    .via1_pa_model = 0xC0, // Q700 model sense
    // Modelled maxed.  Base is 512 KiB soldered plus three optional
    // 256 KiB-SIMM-pair banks.
    .dafb_vram_size = 0x00200000u,
};

static const mcu_board_t q700_board = {
    .desc = &q700_board_desc,
    .via1_output = quadra040_via1_output,
    .via1_shift_out = NULL, // ADB reads SR itself (BUG-004, glue.h)
    .via2_output = quadra040_via2_output,
    .build_devices = q700_build_devices,
};

const hw_profile_t machine_q700 = {
    .name = "Macintosh Quadra 700",
    .id = "q700",

    .cpu_model = 68040,
    .freq = 25000000, // 25 MHz
    .mmu_kind = MMU_68040,

    .address_bits = 32,
    .ram_default = 0x1400000, // 20 MB (a typical well-equipped machine)
    .ram_max = 0x4400000, // 68 MB (4 MB soldered + 4x16 MB SIMMs)
    .rom_size = 0x100000, // 1 MB

    .ram_options = q700_ram_options_kb,
    .floppy_slots = mac_floppy_slots_1hd,
    .storage = mac_storage_scsi_hd_bay,
    .default_storage = mac_default_storage_hd0_cd3,
    .appletalk = true,
    .builtin_video = &mcu_builtin_video_q700,
    .cdrom_drive = &scsi_cd_drive_applecd_sc_plus,

    .nubus_slots = q700_nubus_slots,

    .pram = &pram_defaults_iifx,
    .substrate = &mcu_substrate,
    .board = &q700_board,
};
