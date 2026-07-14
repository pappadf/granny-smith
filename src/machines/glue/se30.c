// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// se30.c
// Macintosh SE/30 machine implementation.
//
// This file implements the SE/30's 32-bit physical memory map, dual-VIA
// interrupt architecture, ROM overlay, I/O address mirroring, and SCSI
// pseudo-DMA. The SE/30 shares the same Universal ROM as the IIx/IIcx
// but uses a fixed 512x342 monochrome display.
//
// I/O space ($50000000-$5FFFFFFF) is handled by a single dispatcher that
// mirrors the device block every $20000 bytes, as implemented by the GLUE
// ASIC (344S0602).

#include "mac030_glue.h"
#include "mac030_glue_io.h"
#include "mac_host_io.h"
#include "machine.h"
#include "mmu_checkpoint.h"
#include "system_config.h" // full config_t definition

#include "adb.h"
#include "asc.h"
#include "builtin_se30_video.h" // SE/30 built-in video as a NuBus card (slot $E)
#include "checkpoint_images.h"
#include "checkpoint_machine.h"
#include "cpu.h"
#include "cpu_internal.h" // for cpu->mmu field
#include "debug.h"
#include "floppy.h"
#include "image.h"
#include "log.h"
#include "memory.h"
#include "mmu.h"
#include "nubus.h"
#include "rom.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "shell.h"
#include "via.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("se30");

// ============================================================
// Constants
// ============================================================

// SE/30 ROM region: 256 KB mirrored across 256 MB.  RAM occupies the
// first 1 GiB (so RAM_END == ROM_START).
#define SE30_ROM_START 0x40000000UL
#define SE30_ROM_END   0x50000000UL

// SE/30 I/O region: 256 MB, mirrored every $20000
#define SE30_IO_BASE 0x50000000UL
#define SE30_IO_SIZE 0x10000000UL
// (I/O window offsets + the dispatcher are shared with IIcx/IIx — see
// mac030_glue_io.c.)

// Interrupt source bits for mac030_glue_update_ipl()
#define SE30_IRQ_VIA1 (1 << 0) // IPL level 1
#define SE30_IRQ_VIA2 (1 << 1) // IPL level 2
#define SE30_IRQ_SCC  (1 << 2) // IPL level 4
#define SE30_IRQ_NMI  (1 << 3) // IPL level 7

// SE/30 Video RAM: 64 KB at logical $FEE00000 (NuBus slot E, offset $E00000)
#define SE30_VRAM_BASE 0xFEE00000UL
#define SE30_VRAM_SIZE 0x00010000UL // 64 KB

// SE/30 Video ROM: 32 KB, mapped at logical $FEFF8000 (top of slot E)
// Byte lanes = $0F (all lanes), so data is contiguous in the top 32 KB
#define SE30_VROM_BASE 0xFEFF8000UL
#define SE30_VROM_SIZE 0x00008000UL // 32 KB

// ROM's MMU page table remaps NuBus slot $E to I/O space:
// logical $FExxxxxx → physical $50Fxxxxx (I/O window for pseudoslot E)
// These are the PHYSICAL addresses used after the MMU is enabled.
#define SE30_VROM_PHYS     0xFEFF8000UL // NuBus slot $E declaration ROM physical address
#define SE30_VRAM_PHYS_ALT 0x50FE0000UL // page-table-mapped VRAM physical address
#define SE30_VROM_PHYS_ALT 0x50FF8000UL // page-table-mapped VROM physical address

// Framebuffer offsets within the 64 KB VRAM
#define SE30_FB_PRIMARY_OFFSET   0x8040 // main screen buffer
#define SE30_FB_ALTERNATE_OFFSET 0x0040 // alternate screen buffer

// ============================================================
// SE/30-specific state
// ============================================================

// SE/30 state is the unified GLUE state struct (mac030_glue.h).  The SE/30
// uses the vram/vrom/video_card members for its slot-$E built-in video and
// leaves the IIcx soft-power members unused.
typedef mac030_glue_state_t se30_state_t;

// Helper: return the SE/30-specific state from a config handle
static inline se30_state_t *se30_state(config_t *cfg) {
    return (se30_state_t *)cfg->machine_context;
}

// ============================================================
// Forward declarations for SE/30 callbacks
// ============================================================

static void se30_via1_output(void *context, uint8_t port, uint8_t output);
static void se30_via1_shift_out(void *context, uint8_t byte);
static void se30_via2_output(void *context, uint8_t port, uint8_t output);
static void se30_via2_shift_out(void *context, uint8_t byte);

// ============================================================
// SoA page helper
// ============================================================

// Populate both the AoS page_entry_t and the SoA fast-path arrays for one page.
// For read-only pages (writable=false), write SoA entries stay zero (slow-path).

// ============================================================
// ROM overlay
// ============================================================

// Toggle ROM/RAM mapping at $00000000.
// On reset, ROM is overlaid at $00000000 for the initial vector fetch.
// The ROM boot code disables the overlay by writing 0 to VIA1 PA4.
static void se30_set_rom_overlay(config_t *cfg, bool overlay) {
    mac030_glue_set_rom_overlay(cfg, &se30_state(cfg)->rom_overlay, SE30_ROM_START, overlay);
}

// ============================================================
// Hardware reset (RESET line asserted)
// ============================================================

// Called when the RESET line is asserted (e.g., double bus error → HALT → GLU
// ============================================================
// I/O dispatcher
// ============================================================
// Shared with IIcx/IIx — see mac030_glue_io.c.  se30_init fills a
// mac030_glue_io_t and registers it as the I/O region's context.

// ============================================================
// Memory layout
// ============================================================

// Populate the SE/30 memory layout in the page table.
// RAM: $00000000-$3FFFFFFF (actual size from profile, mirrored)
// ROM: $40000000-$4FFFFFFF (256 KB mirrored across 256 MB)
// I/O: $50000000-$5FFFFFFF (dispatcher with $20000 mirroring)
// VRAM: $FE000000-$FE00FFFF (64 KB, writable)
// VROM: $FEFFE000-$FEFFFFFF (8 KB, read-only, synthesised declaration ROM)
// ROM overlay at $00000000 is active on reset.
static void se30_memory_layout_init(config_t *cfg) {
    se30_state_t *se30 = se30_state(cfg);

    uint32_t ram_size = cfg->ram_size;
    uint32_t rom_size = cfg->machine->rom_size;
    uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
    // ROM data is stored immediately after RAM in the flat buffer
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, ram_size);

    // --- RAM pages: $00000000 - ram_size (writable, with SIMM aliasing) ---
    //
    // Physical RAM is mapped directly at $0.  An additional mirror of the full
    // RAM image is placed immediately above, at ram_size .. 2*ram_size-1.
    // This emulates the real SE/30 SIMM address-line wrapping: SIMMs ignore
    // address bits above their capacity, so the byte at <ram_size> is the same
    // physical cell as the byte at 0.  The ROM's ram_address_test writes to
    // the top-of-RAM address and checks whether the pattern appears at a lower
    // alias; without this mirror, the write falls into unmapped space and the
    // test fails with a spurious address-bus error.
    //
    // The ROM's address test table uses BMI rows (alias=$FFFFFFFF) for 1, 4,
    // and 16 MB — these expect NO aliasing at the boundary.  All other sizes
    // (2, 5, 8, 32, 64 … MB) use non-BMI rows that expect the top-of-RAM
    // write to alias back to a lower address.  We map one extra mirror for
    // non-BMI sizes so the alias check succeeds.
    uint32_t ram_pages = ram_size >> PAGE_SHIFT;

    // Determine whether SIMM aliasing is needed.  The ROM's ram_address_test
    // table has two kinds of entries: "BMI" rows (alias = $FFFFFFFF) that
    // expect NO aliasing, and "non-BMI" rows (alias = an address) that
    // expect the top-of-RAM write to alias back.  BMI rows correspond to
    // the GLUE's standard bank sizes (1, 4, 16, 64 MB); non-BMI rows cover
    // intermediate totals (2, 5, 8, 32 … MB).  We only need a mirror for
    // sizes whose top-of-RAM entry is non-BMI.
    // BMI rows in the ROM table: 1 MB ($100000), 4 MB ($400000), 16 MB ($1000000).
    // All other sizes (including 64 MB) use non-BMI rows that expect aliasing.
    bool standard_bank = (ram_size == 1 * 1024 * 1024 || ram_size == 4 * 1024 * 1024 || ram_size == 16 * 1024 * 1024);
    uint32_t map_end_page = standard_bank ? ram_pages : (ram_pages * 2);

    for (uint32_t p = 0; p < map_end_page && (int)p < g_page_count; p++)
        mac030_fill_page(p, ram_base + ((p % ram_pages) << PAGE_SHIFT), true);

    // --- ROM pages: $40000000 - $4FFFFFFF (256 KB mirrored, read-only) ---
    uint32_t rom_pages = rom_size >> PAGE_SHIFT;
    uint32_t rom_start_page = SE30_ROM_START >> PAGE_SHIFT;
    uint32_t rom_end_page = SE30_ROM_END >> PAGE_SHIFT;

    if (rom_pages > 0) {
        for (uint32_t p = rom_start_page; p < rom_end_page && (int)p < g_page_count; p++) {
            uint32_t offset_in_rom = (p - rom_start_page) % rom_pages;
            mac030_fill_page(p, rom_data + (offset_in_rom << PAGE_SHIFT), false);
        }
    }

    // --- I/O dispatcher: $50000000 - $5FFFFFFF ---
    mac030_io_fill_interface(&se30->io_interface);
    memory_map_add(cfg->mem_map, SE30_IO_BASE, SE30_IO_SIZE, "SE/30 I/O", &se30->io_interface, &se30->glue_io);

    // --- VRAM: $FEE00000 - $FEE0FFFF (64 KB writable) ---
    // Mirror the 64 KB across the 1 MB decode window $FEE00000-$FEEFFFFF
    if (se30->vram) {
        uint32_t vram_pages = SE30_VRAM_SIZE >> PAGE_SHIFT; // 16 pages
        uint32_t vram_start_page = SE30_VRAM_BASE >> PAGE_SHIFT;
        uint32_t vram_mirror_end = (SE30_VRAM_BASE + 0x100000) >> PAGE_SHIFT; // 1 MB window
        for (uint32_t p = vram_start_page; p < vram_mirror_end && (int)p < g_page_count; p++) {
            uint32_t offset_in_vram = ((p - vram_start_page) % vram_pages) << PAGE_SHIFT;
            mac030_fill_page(p, se30->vram + offset_in_vram, true);
        }
    }

    // --- VROM: $FEFF8000 - $FEFFFFFF (32 KB read-only) ---
    {
        uint32_t vrom_pages = SE30_VROM_SIZE >> PAGE_SHIFT; // 8 pages
        uint32_t vrom_start_page = SE30_VROM_BASE >> PAGE_SHIFT;
        for (uint32_t p = 0; p < vrom_pages && (int)(vrom_start_page + p) < g_page_count; p++)
            mac030_fill_page(vrom_start_page + p, se30->vrom + (p << PAGE_SHIFT), false);
    }

    // --- ROM overlay: map ROM at $00000000 on reset ---
    se30->rom_overlay = false; // se30_set_rom_overlay will toggle it on
    se30_set_rom_overlay(cfg, true);
}

// ============================================================
// Interrupt routing
// ============================================================

// SE/30 dual-VIA interrupt routing: combines active sources and drives CPU IPL.
// VIA1 → IPL 1, VIA2 → IPL 2, SCC → IPL 4, NMI → IPL 7.

// ============================================================
// VIA callbacks
// ============================================================

// VIA1 output callback: routes port A/B changes to peripherals
static void se30_via1_output(void *context, uint8_t port, uint8_t output) {
    config_t *cfg = (config_t *)context;
    se30_state_t *se30 = se30_state(cfg);

    if (port == 0) {
        // Port A outputs:
        // Bit 6: screen buffer select (1 = primary at VRAM+$8040, 0 = alternate at VRAM+$0040)
        bool main_buf = (output & 0x40) != 0;
        builtin_se30_video_select_buffer(se30->video_card, main_buf);
        // Bit 5: floppy head select → SWIM
        if (se30->floppy)
            floppy_set_sel_signal(se30->floppy, (output & 0x20) != 0);
        // Bit 4: ROM overlay control (1 = ROM at $00000000, 0 = RAM)
        se30_set_rom_overlay(cfg, (output & 0x10) != 0);
        // Bit 3: alternate sound buffer (legacy, ignored for ASC)
        // Bits 0-2: sound volume (legacy, ignored for ASC)
    } else {
        // Port B outputs:
        // Bits 4-5: ADB state lines (ST0/ST1) → ADB controller
        // Only notify ADB when ST bits actually transition.  The ROM
        // bit-bangs the RTC via port B bits 0-2 without intending to
        // change ST; on real hardware the ADB transceiver ignores writes
        // where the ST lines don't change electrically (BUG-004).
        if (se30->adb) {
            uint8_t st_mask = 0x30; // bits 5:4 = ST1:ST0
            uint8_t old_st = se30->last_port_b & st_mask;
            uint8_t new_st = output & st_mask;
            if (new_st != old_st) {
                adb_port_b_output(se30->adb, output);
            }
        }
        se30->last_port_b = output;
        // Bit 3: vADBInt (input — read-only, driven by ADB module)
        // Bits 0-2: RTC chip select/clock/data
        if (cfg->rtc)
            rtc_input(cfg->rtc, (output >> 2) & 1, (output >> 1) & 1, output & 1);
    }
}

// VIA1 shift-out callback: ADB byte data transfer
static void se30_via1_shift_out(void *context, uint8_t byte) {
    config_t *cfg = (config_t *)context;
    se30_state_t *se30 = se30_state(cfg);
    if (se30->adb)
        adb_shift_byte(se30->adb, byte);
}

// VIA1 IRQ callback: VIA1 drives IPL level 1

// VIA2 output callback: no SE/30 port-B output is observed here today
// (sound-enable bit 7, VSync-IRQ-enable bit 6, ID bit 3 — none gated).
// Port A is slot-IRQ inputs only.  Kept for the via_init callback shape.
static void se30_via2_output(void *context, uint8_t port, uint8_t output) {
    (void)context;
    (void)port;
    (void)output;
}

// VIA2 shift-out callback: not used on SE/30
static void se30_via2_shift_out(void *context, uint8_t byte) {
    (void)context;
    (void)byte;
}

// VIA2 IRQ callback: VIA2 drives IPL level 2

// SCC IRQ callback: SCC drives IPL level 4

// ============================================================
// VBL trigger
// ============================================================

// Trigger vertical blanking interval for the SE/30.
// On real hardware, the GLUE chip (344S0602) drives both VIA1 CA1
// and VIA2 CA1 simultaneously from the same vertical blanking signal.
// It also asserts slot $E on VIA2 port A bit 5 (active-low) so the
// level-2 handler can identify the interrupt source. The CPU's
// interrupt priority logic services level 2 (VIA2) before level 1
// (VIA1). VIA IER masking and the CPU SR interrupt mask naturally
// prevent servicing before the ROM's Slot Manager has initialised.
static void se30_vbl_slot_deassert(void *context, uint64_t data);

static void se30_trigger_vbl(config_t *cfg) {
    // Assert slot $E on VIA2 port A bit 5 (active-low)
    via_input(cfg->via2, 0, 5, 0);

    // Pulse both VIA CA1 lines simultaneously, as the GLUE chip does
    via_input_c(cfg->via1, 0, 0, 0);
    via_input_c(cfg->via2, 0, 0, 0);
    via_input_c(cfg->via1, 0, 0, 1);
    via_input_c(cfg->via2, 0, 0, 1);

    // Deassert slot $E after the vertical blanking interval ends.
    // 15700 cycles ≈ 1 ms at 15.6672 MHz — matches the SE/30 video
    // blanking duration. With deassert=50000 the slot stayed asserted
    // long enough for a second CA1 pulse to deliver into Mac OS's
    // empty-queue panic path during the MAE→A/UX kernel handoff window
    // for some RTC values.
    scheduler_new_cpu_event(cfg->scheduler, &se30_vbl_slot_deassert, cfg, 0, 0, 15700);

    image_tick_all(cfg);
}

// Deferred callback: deasserts slot $E when the blanking interval ends.
static void se30_vbl_slot_deassert(void *context, uint64_t data) {
    (void)data;
    config_t *cfg = (config_t *)context;
    via_input(cfg->via2, 0, 5, 1);
}

// ============================================================
// Init / Teardown
// ============================================================

// Initialise all SE/30 subsystems.
// If checkpoint is non-NULL, each device restores state from it.
// SE/30 registers its built-in-video VBL slot-deassert event type before
// device construction (keeps checkpoint event-type ordering stable).
static void se30_pre_devices(config_t *cfg) {
    scheduler_new_event_type(cfg->scheduler, "se30", cfg, "vbl_slot_deassert", &se30_vbl_slot_deassert);
}

// Machine-ID straps: PA6 = 1 / PB3 = 0 (SE/30 signature); VIA2 PA3 high; PB6
// reports the sound jack inserted; control lines idle high.
static void se30_setup_id(config_t *cfg) {
    via_input(cfg->via2, 1, 3, 0);
    via_input(cfg->via2, 0, 3, 1);
    via_input(cfg->via2, 1, 6, 0);
    via_input_c(cfg->via2, 0, 0, 1); // CA1: NuBus slot IRQ
    via_input_c(cfg->via2, 0, 1, 1); // CA2: SCSI DRQ
    via_input_c(cfg->via2, 1, 1, 1); // CB2: SCSI IRQ
}

// SE/30 slot table: slot $E is the built-in video card; $9..$B are empty PDS
// pseudo-slots.
static const nubus_slot_decl_t se30_slots[] = {
    {.slot = 0x9, .kind = NUBUS_SLOT_EMPTY},
    {.slot = 0xA, .kind = NUBUS_SLOT_EMPTY},
    {.slot = 0xB, .kind = NUBUS_SLOT_EMPTY},
    {.slot = 0xE, .kind = NUBUS_SLOT_BUILTIN, .builtin_card_id = "builtin_se30_video"},
    {0},
};

// After nubus_init: borrow the slot-$E card's VRAM/VROM and wire them into the
// memory map at both their TT-identity bases and their page-table-mapped
// physical aliases, so the PMMU and the I/O dispatcher resolve the apertures.
// TT1 itself is set by mac030_glue_init right after the PMMU is built (the SE/30
// ROM never writes TT registers; the GLUE chip routes $F0-$FF to NuBus, and TT1
// identity-maps that for supervisor FCs so phys_to_host resolves the VRAM).
static void se30_post_nubus(config_t *cfg) {
    se30_state_t *se30 = se30_state(cfg);
    se30->video_card = nubus_card(cfg->nubus, 0xE);
    assert(se30->video_card != NULL);
    se30->vram = builtin_se30_video_vram(se30->video_card);
    se30->vrom = builtin_se30_video_vrom(se30->video_card);
    if (!se30->vram || !se30->vrom) {
        fprintf(stderr, "Error: SE/30 Video ROM (SE30.vrom) not found.\n"
                        "The SE/30 emulator requires a real VROM file for proper operation.\n"
                        "Place SE30.vrom next to the ROM file or in tests/data/roms/.\n");
        exit(1);
    }
    memory_map_host_region(cfg->mem_map, "se30_vram", se30->vram, SE30_VRAM_BASE, SE30_VRAM_SIZE, /*writable*/ true);
    memory_map_host_region(cfg->mem_map, "se30_vrom", se30->vrom, SE30_VROM_PHYS, SE30_VROM_SIZE, /*writable*/ false);
    memory_map_host_region_alias(cfg->mem_map, SE30_VRAM_PHYS_ALT, SE30_VRAM_BASE);
    memory_map_host_region_alias(cfg->mem_map, SE30_VROM_PHYS_ALT, SE30_VROM_PHYS);
}

// Restore the card-owned VRAM/VROM bytes from a checkpoint (before the shared
// MMU-register restore that mac030_glue_init performs).
static void se30_ckpt_restore_extra(config_t *cfg, checkpoint_t *cp) {
    se30_state_t *se30 = se30_state(cfg);
    system_read_checkpoint_data(cp, se30->vram, SE30_VRAM_SIZE);
    checkpoint_read_file(cp, se30->vrom, SE30_VROM_SIZE, NULL);
}

// SE/30 board: GLUE family with built-in slot-$E video; bus-error window covers
// only slots $9..$D (slot $E is the mapped built-in video).
static const mac030_board_desc_t se30_desc = {
    .chipset = "GLUE",
    .rom_base = 0x40000000UL,
    .rom_end = 0x50000000UL,
    .io_ranges = glue_io_ranges,
    .io_mirror_mask = MAC030_GLUE_IO_MIRROR,
    .io_unmapped_read = 0,
    .slots = se30_slots,
    .bus_err_lo = 0xF9000000,
    .bus_err_hi = 0xFDFFFFFF,
    .asc_mix = ASC_MIX_SUM, // SE/30 board sums both channels to the speaker
};

// VRAM + VROM save (symmetric with se30_ckpt_restore_extra); defined below.
static void se30_ckpt_save_extra(config_t *cfg, checkpoint_t *cp);

static const mac030_glue_board_t se30_board = {
    .desc = &se30_desc,
    .via1_output = se30_via1_output,
    .via1_shift_out = se30_via1_shift_out,
    .via2_output = se30_via2_output,
    .via2_shift_out = se30_via2_shift_out,
    .setup_id = se30_setup_id,
    .memory_layout = se30_memory_layout_init,
    .pre_devices = se30_pre_devices,
    .post_nubus = se30_post_nubus,
    .ckpt_restore_extra = se30_ckpt_restore_extra,
    .ckpt_save_extra = se30_ckpt_save_extra, // built-in slot-$E video VRAM + VROM
    .trigger_vbl = se30_trigger_vbl, // built-in video VBL (slot-$E assert)
};

// ============================================================
// Checkpoint
// ============================================================

// SE/30 board ckpt_save_extra hook: the built-in slot-$E video's VRAM + VROM,
// written immediately before the MMU block — symmetric with the restore order
// in se30_ckpt_restore_extra (the shared glue_checkpoint_save handles the rest
// of the machine state).
static void se30_ckpt_save_extra(config_t *cfg, checkpoint_t *cp) {
    se30_state_t *se30 = se30_state(cfg);

    // Save VRAM contents.  The bytes are owned by the slot-$E card; se30->vram
    // is a borrowed pointer into that buffer, so the memcpy hits the right
    // backing store.
    system_write_checkpoint_data(cp, se30->vram, SE30_VRAM_SIZE);

    // Save VROM (content embedded in consolidated checkpoints, path reference in
    // quick).  The path lives on the card too.
    const char *vrom_path = builtin_se30_video_vrom_path(se30->video_card);
    checkpoint_write_file(cp, vrom_path ? vrom_path : "");
}

// ============================================================
// Machine descriptor
// ============================================================

// SE/30 configuration-dialog metadata.
static const uint32_t se30_ram_options_kb[] = {1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 0};

static const struct floppy_slot se30_floppy_slots[] = {
    {.label = "Internal FD0", .kind = FLOPPY_HD},
    {.label = "External FD1", .kind = FLOPPY_HD},
    {0},
};

static const struct scsi_slot se30_scsi_slots[] = {
    {.label = "SCSI HD0", .id = 0},
    {.label = "SCSI HD1", .id = 1},
    {0},
};

const hw_profile_t machine_se30 = {
    .name = "Macintosh SE/30",
    .id = "se30",

    // 68030 at 15.6672 MHz
    .cpu_model = 68030,
    .freq = 15667200,
    .mmu_kind = MMU_68030_PMMU,

    // 32-bit address space
    .address_bits = 32,
    .ram_default = 0x800000, // 8 MB default (matches dialog RAM default)
    .ram_max = 0x8000000, // 128 MB max
    .rom_size = 0x040000, // 256 KB

    // Configuration-dialog shape
    .ram_options = se30_ram_options_kb,
    .floppy_slots = se30_floppy_slots,
    .scsi_slots = se30_scsi_slots,
    .has_cdrom = true,
    .cdrom_id = 3,

    // Built-in slot-$E video card.  Exposed in the profile so the config
    // dialog reads the VROM requirement from the card (it needs SE30.vrom);
    // this is the same table the substrate passes to nubus_init().
    .nubus_slots = se30_slots,

    .substrate = &glue_substrate, // shared GLUE-family substrate
    .board = &se30_board,
};
