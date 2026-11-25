// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iici.c
// Macintosh IIci ("Aurora", 25 MHz 68030, 1989) machine implementation.
// Architecturally the IIcx with VIA2 replaced by the RBV chip and built-in
// video reading from the slot-$B framebuffer aperture.  See
// docs/internals/machines/mdu/iici.md.
//
// Diff vs iicx.c at a glance:
//   * I/O island: VIA2 ($2000) dropped; VDAC ($24000) and RBV ($26000)
//     added.  The mirror mask widens to $3FFFF so RBV/VDAC decode
//     distinctly from the SCSI windows.
//   * ROMBase is $40800000 (MDUtable; the reset PC is $4080002A), though
//     the MDU repeats the ROM through all of $40000000-$4FFFFFFF.
//   * Interrupts: RBV's combined IFR drives IPL 2 (replacing VIA2); SCSI
//     IRQ/DRQ route through the RBV via scsi_set_irq_callback (no VIA2).
//   * Soft power-off: RBV RvPowerOff (RvDataB bit 2) instead of VIA2 PB2.
//   * Built-in video: a NuBus pseudo-card whose frame buffer is the bottom
//     of Bank A (physical 0; the ROM maps $FBB08000 there); depth follows
//     RvMonP via a mode callback.
//   * ADB / RTC use the classic VIA1 path (no Egret) — identical to IIcx.

#include "m68k_board.h"
#include "machine.h"
#include "machine_checkpoint.h"
#include "mdu.h" // mdu_substrate + mdu_board_t
#include "slot_tables.h"
#include "system_internal.h"

#include "adb.h"
#include "asc.h"
#include "builtin_rbv_video.h"
#include "checkpoint.h"
#include "cpu.h"
#include "floppy.h"
#include "iici_internal.h"
#include "image.h"
#include "log.h"
#include "memory.h"
#include "mmu.h"
#include "nubus.h"
#include "pram_defaults.h"
#include "rbv.h"
#include "rbv_board.h"
#include "rtc.h"
#include "scheduler.h"
#include "scsi.h"
#include "via.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================
// ROM overlay
// ============================================================

static void iici_set_rom_overlay(config_t *cfg, bool overlay) {
    m68k_board_overlay_set_line(&iici_st(cfg)->overlay, overlay);
}

// ============================================================
// I/O dispatcher (MDU island — IIcx map minus VIA2, plus VDAC + RBV)
// ============================================================

// The MDU I/O dispatcher is shared with the IIsi — see mdu_io.c.  This
// machine fills an mdu_io_t at init and registers it as the I/O region's
// device context.

// ============================================================
// Memory layout
// ============================================================

// Split an installed RAM total into the two physical SIMM banks.
//
// Per the Macintosh IIci Developer Note, ch. 3 "RAM Configuration": RAM is two
// banks of four SIMM sockets each, and a bank holds either nothing or four
// equal SIMMs of 256 KB / 1 MB / 4 MB / 16 MB — so a populated bank is exactly
// 1, 4, 16 or 64 MB.  Every total in iici_ram_options_kb decomposes into two
// such banks, which is why the table below is a closed set rather than a
// general partition.
//
// The note's Figure 3-1 puts the SMALLER SIMMs in Bank A ("for best
// performance with on-board video"), so the smaller half goes to Bank A here:
// a 5 MB machine is 1 MB in A plus 4 MB in B, matching the figure.  Banks are
// otherwise interchangeable, and Bank A must be populated for on-board video.
static void iici_split_ram_banks(uint32_t ram_size, uint32_t *bank_a, uint32_t *bank_b) {
    static const uint32_t legal[] = {0x00100000, 0x00400000, 0x01000000, 0x04000000}; // 1/4/16/64 MB
    const size_t n = sizeof(legal) / sizeof(legal[0]);
    // Ascending, so the first hit is the smallest legal Bank A.
    for (size_t i = 0; i < n; i++) {
        if (legal[i] > ram_size)
            break;
        uint32_t rem = ram_size - legal[i];
        if (rem == 0) {
            *bank_a = legal[i];
            *bank_b = 0;
            return;
        }
        for (size_t j = 0; j < n; j++)
            if (rem == legal[j]) {
                *bank_a = legal[i];
                *bank_b = rem;
                return;
            }
    }
    // Not a shipping SIMM combination — present it as one bank and let the
    // ROM's own sizing decide what it thinks of that.
    *bank_a = ram_size;
    *bank_b = 0;
}

static void iici_memory_layout_init(config_t *cfg) {
    iici_state_t *st = iici_st(cfg);

    uint32_t ram_size = cfg->ram_size;
    uint32_t rom_size = cfg->machine->rom_size;
    uint8_t *ram_base = ram_native_pointer(cfg->memory_map, 0);
    uint8_t *rom_data = ram_native_pointer(cfg->memory_map, ram_size);

    // Two physical RAM banks, each a socket group of four 30-pin SIMMs:
    // Bank A at physical 0, Bank B at $04000000.  Each bank mirrors its
    // installed size throughout its 64 MB window — the boot ROM sizes a bank
    // by probing down from the window top and then classifying the wrap, so
    // the mirror is load-bearing (a linear map with no wrap mis-sizes every
    // configuration the descending probe can't stumble onto).
    // Same model as the IIsi, whose Bank A is soldered 1 MB.
    uint32_t bank_a_size, bank_b_size;
    iici_split_ram_banks(ram_size, &bank_a_size, &bank_b_size);

    uint8_t *bank_b = ram_base + bank_a_size;
    m68k_board_map_mirrored(0, IICI_BANK_B_PHYS >> MEM_PAGE_SHIFT, ram_base, bank_a_size >> MEM_PAGE_SHIFT,
                            page_fill_68k, true);
    m68k_board_map_mirrored(IICI_BANK_B_PHYS >> MEM_PAGE_SHIFT, IICI_BANK_WINDOW >> MEM_PAGE_SHIFT, bank_b,
                            bank_b_size >> MEM_PAGE_SHIFT, page_fill_68k, true);

    // Teach the PMMU-side physical resolver the same two-bank layout so
    // table walks and TLB fills resolve Bank B (no-op when bank_b_size == 0
    // — but ram_a_size must still be set so window mirroring resolves).
    mmu_set_ram_bank_b(st->mmu, bank_a_size, bank_b, IICI_BANK_B_PHYS, bank_b_size, IICI_BANK_WINDOW);

    uint32_t rom_pages = rom_size >> MEM_PAGE_SHIFT;
    uint32_t rom_start_page = IICI_ROM_START >> MEM_PAGE_SHIFT;
    uint32_t rom_end_page = IICI_ROM_END >> MEM_PAGE_SHIFT;
    if (rom_pages > 0) {
        for (uint32_t p = rom_start_page; p < rom_end_page && p < g_page_count; p++) {
            uint32_t offset_in_rom = (p - rom_start_page) % rom_pages;
            page_fill_68k(p, rom_data + (offset_in_rom << MEM_PAGE_SHIFT), false);
        }
    }

    board_io_fill_interface(&st->io_interface);
    memory_map_add(cfg->memory_map, IICI_IO_BASE, IICI_IO_SIZE, "I/O", &st->io_interface, &st->mdu_io);

    // Wire any registered host regions (a socketed card's VRAM) and their
    // Mode-24 slot aliases into the page table — same machinery as the IIcx
    // VRAM/Mode-24 handling.  The built-in video needs none: its frame buffer
    // is Bank A RAM, which the guest reaches through its own PMMU tables.
    mmu_host_regions_fill_pages(st->mmu, page_fill_68k, /*mode24_alias*/ true);

    m68k_board_overlay_init_line(&st->overlay, cfg, cfg->machine->name);
    m68k_board_overlay_arm(&st->overlay);
}

// ============================================================
// VIA1 callbacks (the RBV / SCSI / sound wiring is shared: rbv_board.c, mdu_wire_rbv)
// ============================================================

// VIA1 outputs.  Identical to the IIcx: floppy head-select + ROM overlay on
// port A; ADB shift-state filtering + classic RTC on port B.
static void iici_via1_output(void *context, uint8_t port, uint8_t output) {
    config_t *cfg = (config_t *)context;

    if (port == 0) {
        if (cfg->floppy)
            floppy_set_sel_signal(cfg->floppy, (output & 0x20) != 0);
        iici_set_rom_overlay(cfg, (output & 0x10) != 0);
    } else {
        // Bits 4-5: ADB state lines (ST0/ST1) -> ADB controller.  The
        // ST-transition filter and its port-B shadow live in adb.c now.
        if (cfg->adb)
            adb_port_b_output(cfg->adb, output);
        rtc_via1_pb_output(cfg->rtc, output);
    }
}

// ============================================================
// Slot table
// ============================================================
//
// Built-in RBV video is "slot $0" to the Slot Manager, but the slot table
// is sentinel-terminated by slot == 0, so we seat the card at our internal
// NuBus slot $B — the slot whose address space actually holds the
// framebuffer ($FBxxxxxx / VideoInfoMDU's PRAM slot $0B).  The slot index
// is an implementation detail (the boot ROM drives built-in video from the
// baked-in VideoInfoMDU, not by scanning this bus; the RBV video VBL uses
// logical slot 0 = RvIRQ0 independently).  Slots $C/$D/$E are the
// user-visible NuBus expansion slots (empty in v1).
static const nubus_slot_decl_t iici_slots[] = {
    {.slot = 0xB, .kind = NUBUS_SLOT_BUILTIN, .builtin_card_id = "builtin_rbv_video", .fb_in_ram = true},
    // The three physical sockets ship empty (no default_card): the RBV
    // built-in video is the factory display; a socketed card is an add-on.
    {.slot = 0xC, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot 4", .fill_order = 4},
    {.slot = 0xD, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot 5", .fill_order = 5},
    {.slot = 0xE, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot 6", .fill_order = 6},
    {0},
};

// The IIci board descriptor: MDU+RBV hardware data, consumed
// at init by the shared helpers.  ROM repeating through $4xxxxxxx; the 18-bit $40000 I/O
// mirror; the shared MDU window table.
static const m68k_board_desc_t iici_board_desc = {
    .chipset = "MDU+RBV",
    .rom_base = IICI_ROM_START,
    .rom_end = IICI_ROM_END,
    .io_ranges = mdu_io_ranges_tbl,
    .io_mirror_mask = 0x0003FFFFUL,
    .io_unmapped_read = 0xFF, // undecoded island reads float high (see m68k_board.h)
    // NuBus expansion slots $9..$E bus-error on unmapped reads.  The built-in
    // video's $FBxxxxxx screen base is logical only: the guest's tables send
    // it to Bank A before this physical range is consulted.
    .bus_err_lo = NUBUS_BERR_LO,
    .bus_err_hi = NUBUS_BERR_HI,
    // Cache-card connector, $5200_0000-$52FF_FFFF (data, then tags): decoded
    // only by a fitted card, which this model does not have (Macintosh IIci
    // Developer Note, memory map).
    .io_berr_lo = 0x02000000UL,
    .io_berr_hi = 0x02FFFFFFUL,
};

// ============================================================
// Init / Teardown
// ============================================================

// IIci device construction (mdu_board_t.build_devices): everything after
// the shared core/RTC/SCC/VIA1 prefix and before m68k_board_finish.
static int iici_build_devices(config_t *cfg, checkpoint_t *checkpoint) {
    iici_state_t *st = iici_st(cfg);
    // The IIci bit-bangs the RTC on VIA1 (classic transceiver path).
    rtc_set_via(cfg->rtc, cfg->via1);

    // Machine-ID readback on VIA1 port A: PA6/PA4/PA2/PA1 = 1/0/1/1 for the
    // production (no Parity Generator Card) IIci — mask $56, value $46 per
    // InfoMacIIci.  (Verified at the assembler level during bring-up.)
    via_input(cfg->via1, 0, 6, 1);
    via_input(cfg->via1, 0, 4, 0);
    via_input(cfg->via1, 0, 2, 1);
    via_input(cfg->via1, 0, 1, 1);
    // CA1 / CB1 idle high (VBL heartbeat + ADB clock reference edges).
    via_input_c(cfg->via1, 0, 0, 1);
    via_input_c(cfg->via1, 1, 0, 1);

    // ADB on the VIA1 transceiver, the images, the 5380 (through the RBV,
    // not a VIA2), the ASC and SWIM: no companion.
    static const rbv_board_spec_t spec = {.companion = RBV_BOARD_NO_COMPANION};
    const rbv_board_slots_t slots = {.rbv = &st->rbv, .asc = &st->asc};
    if (rbv_board_build_devices(cfg, checkpoint, &spec, &slots) != 0)
        return -1;

    // RBV chip (VIA2 replacement + video control), the built-in port's
    // monitor strapped on its sense lines (7, nothing connected, turns
    // built-in video off).  IRQ → IPL 2; RvPowerOff → scheduler stop.
    if (rbv_board_build_rbv(cfg, checkpoint, RBV_VARIANT_IICI, cfg->build_opts.builtin_sense, &slots) != 0)
        return -1;
    mdu_wire_rbv(cfg);

    st->mmu = m68k_board_build_mmu(cfg, iici_board_desc.rom_base, iici_board_desc.rom_end);
    if (!st->mmu)
        return -1; // m68k_board_build_mmu reported the reason
    // No TT1 preset (the GLUE machines identity-map NuBus $F0-$FF with one).
    // Reset leaves TT0/TT1 disabled and this ROM never loads them, so its
    // tables govern slot space -- and they map the built-in video's screen
    // base $FBB08000 to Bank A, which an identity TT1 would bypass.

    cfg->nubus = nubus_init(cfg, cfg->machine->nubus_slots, checkpoint);
    st->video_card = nubus_card(cfg->nubus, 0xB);
    assert(st->video_card != NULL);
    builtin_rbv_video_set_rbv(st->video_card, st->rbv);
    // The RBV scans its frame buffer out of the BOTTOM of Bank A -- physical 0,
    // the 320 KB the ROM keeps out of the logical RAM map (its level-A
    // descriptor $00050019 starts logical 0 at physical $50000).  The ROM's own
    // tables map the screen base $FBB08000 (and $00B08000 in 24-bit mode) to
    // physical 0; A/UX builds its tables the same way.  The card points at Bank
    // A when it is built (the slot's fb_in_ram), so every path to the screen --
    // ROM tables, A/UX tables, a physical access -- lands where the renderer
    // reads, as on the IIsi.

    // Bind device handles + the board's I/O window table for the shared engine.
    mdu_io_bind(&st->mdu_io, cfg, &iici_board_desc, st->asc, cfg->floppy, st->rbv, st->video_card);

    iici_memory_layout_init(cfg);

    // VIA1 is re-driven before the PMMU restore: the guest's tables start
    // logical 0 at physical $50000, past the frame buffer (machine_part_pmmu)
    machine_part_pmmu(cfg, checkpoint, st->mmu, cfg->via1, NULL);
    return 0;
}

// ============================================================
// Machine descriptor
// ============================================================

// Two banks of four SIMMs (1, 4, 16 or 64 MB per bank): every total the
// pair makes, which iici_split_ram_banks decomposes.
static const uint32_t iici_ram_options_kb[] = {1024,  2048,  4096,  5120,  8192,  16384,  17408, 20480,
                                               32768, 65536, 66560, 69632, 81920, 131072, 0};

// IIci board: the shared mdu_substrate reads its data descriptor + VIA1 hooks
// + the device-construction body.
static const mdu_board_t iici_board = {
    .desc = &iici_board_desc,
    .via1_output = iici_via1_output,
    .via1_shift_out = NULL, // ADB reads SR itself (BUG-004, glue.h)
    .build_devices = iici_build_devices,
};

const hw_profile_t machine_iici = {
    .name = "Macintosh IIci",
    .id = "iici",

    .cpu_model = 68030,
    .freq = 25000000, // 25 MHz
    .mmu_kind = MMU_68030_PMMU,

    .address_bits = 32,
    .ram_default = 0x800000, // 8 MB
    .ram_max = 0x8000000, // 128 MB
    .rom_size = 0x80000, // 512 KB

    .ram_options = iici_ram_options_kb,
    .floppy_slots = mac_floppy_slots_ext,
    .storage = mac_storage_scsi_hd_bay,
    .default_storage = mac_default_storage_hd0_cd3,
    .appletalk = true,
    .builtin_video = &mdu_builtin_video_iici,
    .cdrom_drive = &scsi_cd_drive_applecd_sc_plus,
    // Built-in RBV video has no separate declaration ROM — the boot ROM
    // drives it from the hard-coded VideoInfoMDU record.

    .nubus_slots = iici_slots,

    .pram = &pram_defaults_iici,
    .substrate = &mdu_substrate, // shared MDU+RBV-family substrate
    .board = &iici_board,
};
