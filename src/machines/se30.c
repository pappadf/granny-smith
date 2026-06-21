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

#include "mac030_glue_io.h"
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

// Interrupt source bits for se30_update_ipl()
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

// SE/30-specific peripheral state not shared with other machines.
// Accessed through config_t.machine_context.
typedef struct se30_state {
    // SE/30-specific peripherals (not in config_t)
    adb_t *adb;
    asc_t *asc;
    floppy_t *floppy;

    // ROM overlay state (true = ROM mapped at $00000000)
    bool rom_overlay;

    // VRAM / VROM live on the slot-$E NuBus card (cards/builtin_se30_video.c).
    // These are borrowed pointers, set after nubus_init returns; the card
    // owns the storage and frees it during nubus_delete.  `video_card` is
    // the card handle used by se30_via1_output to toggle the on-screen
    // buffer when the OS writes VIA1 PA6.
    uint8_t *vram;
    uint8_t *vrom;
    nubus_card_t *video_card;

    // MMU state (NULL until se30_init creates it)
    mmu_state_t *mmu;

    // Device handles for the shared GLUE dispatcher (mac030_glue_io.c)
    mac030_glue_io_t glue_io;

    // Previous VIA1 port B output value for filtering ADB ST transitions
    uint8_t last_port_b;

    // I/O dispatcher registered at $50000000
    memory_interface_t io_interface;
} se30_state_t;

// Helper: return the SE/30-specific state from a config handle
static inline se30_state_t *se30_state(config_t *cfg) {
    return (se30_state_t *)cfg->machine_context;
}

// ============================================================
// Forward declarations for SE/30 callbacks
// ============================================================

static void se30_via1_output(void *context, uint8_t port, uint8_t output);
static void se30_via1_shift_out(void *context, uint8_t byte);
static void se30_via1_irq(void *context, bool active);
static void se30_via2_output(void *context, uint8_t port, uint8_t output);
static void se30_via2_shift_out(void *context, uint8_t byte);
static void se30_via2_irq(void *context, bool active);
static void se30_scc_irq(void *context, bool active);
static void se30_update_ipl(config_t *cfg, int source, bool active);

// ============================================================
// SoA page helper
// ============================================================

// Populate both the AoS page_entry_t and the SoA fast-path arrays for one page.
// For read-only pages (writable=false), write SoA entries stay zero (slow-path).
static void se30_fill_page(uint32_t page_index, uint8_t *host_ptr, bool writable) {
    if ((int)page_index >= g_page_count)
        return;

    // AoS cold-path entry
    g_page_table[page_index].host_base = host_ptr;
    g_page_table[page_index].dev = NULL;
    g_page_table[page_index].dev_context = NULL;
    g_page_table[page_index].writable = writable;

    // Compute adjusted base for the SoA fast-path
    uint32_t guest_base = page_index << PAGE_SHIFT;
    uintptr_t adjusted = (uintptr_t)host_ptr - guest_base;

    // All pages are readable by supervisor and user
    if (g_supervisor_read)
        g_supervisor_read[page_index] = adjusted;
    if (g_user_read)
        g_user_read[page_index] = adjusted;

    // Only writable pages get write entries
    if (writable) {
        if (g_supervisor_write)
            g_supervisor_write[page_index] = adjusted;
        if (g_user_write)
            g_user_write[page_index] = adjusted;
    }
}

// ============================================================
// ROM overlay
// ============================================================

// Toggle ROM/RAM mapping at $00000000.
// On reset, ROM is overlaid at $00000000 for the initial vector fetch.
// The ROM boot code disables the overlay by writing 0 to VIA1 PA4.
static void se30_set_rom_overlay(config_t *cfg, bool overlay) {
    se30_state_t *se30 = se30_state(cfg);
    if (se30->rom_overlay == overlay)
        return;
    se30->rom_overlay = overlay;

    uint32_t rom_size = cfg->machine->rom_size;
    uint32_t rom_pages = rom_size >> PAGE_SHIFT;
    uint32_t rom_start_page = SE30_ROM_START >> PAGE_SHIFT;

    if (overlay) {
        // Map ROM at $00000000 (copy from ROM region at $40000000)
        for (uint32_t p = 0; p < rom_pages && (int)p < g_page_count; p++) {
            uint8_t *host_ptr = g_page_table[rom_start_page + p].host_base;
            se30_fill_page(p, host_ptr, false); // read-only
        }
        LOG(1, "ROM overlay enabled: ROM at $00000000");
    } else {
        // Map RAM back at $00000000
        uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
        for (uint32_t p = 0; p < rom_pages && (int)p < g_page_count; p++) {
            se30_fill_page(p, ram_base + (p << PAGE_SHIFT), true); // writable
        }
        LOG(1, "ROM overlay disabled: RAM at $00000000");
    }
}

// ============================================================
// Hardware reset (RESET line asserted)
// ============================================================

// Called when the RESET line is asserted (e.g., double bus error → HALT → GLU
// asserts RESET).  Resets peripherals to their power-on state:
//   - VIA1: ROM overlay re-enabled (ROM visible at $0 for reset vector fetch)
//   - MMU: disabled, TLB invalidated (page table may be corrupted)
// This runs BEFORE the CPU reads SSP/PC from $0/$4.
static void se30_reset(config_t *cfg) {
    se30_state_t *se30 = se30_state(cfg);

    // VIA1 reset: re-enable ROM overlay (VIA1 port A bit 4 goes high on reset)
    se30->rom_overlay = false; // force toggle
    se30_set_rom_overlay(cfg, true);

    // MMU reset: disable translation and flush TLB
    if (se30->mmu) {
        se30->mmu->enabled = false;
        se30->mmu->tc = 0;
        mmu_invalidate_tlb(se30->mmu);
    }

    LOG(1, "SE/30 RESET: ROM overlay active, MMU disabled");
}

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
        se30_fill_page(p, ram_base + ((p % ram_pages) << PAGE_SHIFT), true);

    // --- ROM pages: $40000000 - $4FFFFFFF (256 KB mirrored, read-only) ---
    uint32_t rom_pages = rom_size >> PAGE_SHIFT;
    uint32_t rom_start_page = SE30_ROM_START >> PAGE_SHIFT;
    uint32_t rom_end_page = SE30_ROM_END >> PAGE_SHIFT;

    if (rom_pages > 0) {
        for (uint32_t p = rom_start_page; p < rom_end_page && (int)p < g_page_count; p++) {
            uint32_t offset_in_rom = (p - rom_start_page) % rom_pages;
            se30_fill_page(p, rom_data + (offset_in_rom << PAGE_SHIFT), false);
        }
    }

    // --- I/O dispatcher: $50000000 - $5FFFFFFF ---
    mac030_glue_io_fill_interface(&se30->io_interface);
    memory_map_add(cfg->mem_map, SE30_IO_BASE, SE30_IO_SIZE, "SE/30 I/O", &se30->io_interface, &se30->glue_io);

    // --- VRAM: $FEE00000 - $FEE0FFFF (64 KB writable) ---
    // Mirror the 64 KB across the 1 MB decode window $FEE00000-$FEEFFFFF
    if (se30->vram) {
        uint32_t vram_pages = SE30_VRAM_SIZE >> PAGE_SHIFT; // 16 pages
        uint32_t vram_start_page = SE30_VRAM_BASE >> PAGE_SHIFT;
        uint32_t vram_mirror_end = (SE30_VRAM_BASE + 0x100000) >> PAGE_SHIFT; // 1 MB window
        for (uint32_t p = vram_start_page; p < vram_mirror_end && (int)p < g_page_count; p++) {
            uint32_t offset_in_vram = ((p - vram_start_page) % vram_pages) << PAGE_SHIFT;
            se30_fill_page(p, se30->vram + offset_in_vram, true);
        }
    }

    // --- VROM: $FEFF8000 - $FEFFFFFF (32 KB read-only) ---
    {
        uint32_t vrom_pages = SE30_VROM_SIZE >> PAGE_SHIFT; // 8 pages
        uint32_t vrom_start_page = SE30_VROM_BASE >> PAGE_SHIFT;
        for (uint32_t p = 0; p < vrom_pages && (int)(vrom_start_page + p) < g_page_count; p++)
            se30_fill_page(vrom_start_page + p, se30->vrom + (p << PAGE_SHIFT), false);
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
static void se30_update_ipl(config_t *cfg, int source, bool active) {
    int old_irq = cfg->irq;

    if (active)
        cfg->irq |= source;
    else
        cfg->irq &= ~source;

    // Determine highest active IPL
    uint32_t new_ipl;
    if (cfg->irq & SE30_IRQ_NMI)
        new_ipl = 7;
    else if (cfg->irq & SE30_IRQ_SCC)
        new_ipl = 4;
    else if (cfg->irq & SE30_IRQ_VIA2)
        new_ipl = 2;
    else if (cfg->irq & SE30_IRQ_VIA1)
        new_ipl = 1;
    else
        new_ipl = 0;

    cpu_set_ipl(cfg->cpu, new_ipl);

    LOG(2, "se30_update_ipl: source=%d active=%d irq:%d->%d ipl->%d", source, active ? 1 : 0, old_irq, cfg->irq,
        new_ipl);

    cpu_reschedule();
}

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
static void se30_via1_irq(void *context, bool active) {
    se30_update_ipl((config_t *)context, SE30_IRQ_VIA1, active);
}

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
static void se30_via2_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    se30_update_ipl(cfg, SE30_IRQ_VIA2, active);
}

// SCC IRQ callback: SCC drives IPL level 4
static void se30_scc_irq(void *context, bool active) {
    se30_update_ipl((config_t *)context, SE30_IRQ_SCC, active);
}

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
static void se30_init(config_t *cfg, checkpoint_t *checkpoint) {
    // Allocate SE/30-specific peripheral state
    se30_state_t *se30 = calloc(1, sizeof(se30_state_t));
    assert(se30 != NULL);
    cfg->machine_context = se30;

    // ADB bus starts in IDLE state (ST1:ST0 = 11, bits 5:4 = 0x30)
    se30->last_port_b = 0x30;

    // Initialise parameterised memory: 32-bit address space, configured RAM, 256 KB ROM
    cfg->mem_map = memory_map_init(cfg->machine->address_bits, cfg->ram_size, cfg->machine->rom_size, checkpoint);

    // Initialise CPU (68030)
    cfg->cpu = cpu_init(CPU_MODEL_68030, checkpoint);

    cfg->scheduler = scheduler_init(cfg->cpu, checkpoint);

    // Set SE/30 CPU clock frequency (15.6672 MHz = 2x Plus clock)
    scheduler_set_frequency(cfg->scheduler, cfg->machine->freq);

    // SE/30 68030: 4 cycles/instr (hw accuracy), 4 cycle/instr (fast)
    scheduler_set_cpi(cfg->scheduler, 4, 4);

    // Register VBL slot deassert event type for checkpoint save/restore
    scheduler_new_event_type(cfg->scheduler, "se30", cfg, "vbl_slot_deassert", &se30_vbl_slot_deassert);

    // Restore global interrupt state after scheduler
    if (checkpoint)
        system_read_checkpoint_data(checkpoint, &cfg->irq, sizeof(cfg->irq));

    // Initialise RTC (not yet wired to VIA — deferred until VIA2 exists)
    cfg->rtc = rtc_init(cfg->scheduler, checkpoint, true);

    // Initialise SCC (NULL map: SE/30 I/O dispatcher handles addressing)
    cfg->scc = scc_init(NULL, cfg->scheduler, se30_scc_irq, cfg, checkpoint);

    // SCC PCLK = C8M (7.8336 MHz), RTxC = 3.6864 MHz baud-rate crystal
    scc_set_clocks(cfg->scc, 7833600, 3686400);

    // Initialise VIA1 (NULL map: I/O dispatcher handles addressing)
    // VIA1: system events — VBL, ADB data (shift register), timers
    // freq_factor=20: SE/30 CPU runs at 15.6672 MHz, VIA φ2 clock is ~783 kHz (CPU/20)
    cfg->via1 = via_init(NULL, cfg->scheduler, 20, "via1", se30_via1_output, se30_via1_shift_out, se30_via1_irq, cfg,
                         checkpoint);

    // Initialise VIA2 (NULL map: I/O dispatcher handles addressing)
    // VIA2: expansion — NuBus/PDS slots, SCSI, ASC interrupts, ADB control, RTC
    cfg->via2 = via_init(NULL, cfg->scheduler, 20, "via2", se30_via2_output, se30_via2_shift_out, se30_via2_irq, cfg,
                         checkpoint);

    // Wire RTC 1-second tick to VIA1 CA2
    rtc_set_via(cfg->rtc, cfg->via1);

    // Set hardware ID bits:
    // VIA1 PA6 = 1 (SE/30 identification) — already 1 in default port A input (0xF7)
    // VIA2 PB3 = 0 (SE/30 identification) — ROM reads PA6 and PB3 to identify the board:
    //   PA6=0, PB3=0 → IIx; PA6=1, PB3=0 → SE/30; PA6=1, PB3=1 → IIcx
    via_input(cfg->via2, 1, 3, 0);
    // VIA2 PA3 = 1 — fix default port A input (0xF7 has bit 3 = 0)
    // All VIA2 port A inputs should be 1 (NuBus slot IRQs are active-low, 1 = no IRQ)
    via_input(cfg->via2, 0, 3, 1);

    // VIA2 PB6 (vSndJck) = 0: SE/30 always reports sound jack inserted
    via_input(cfg->via2, 1, 6, 0);

    // VIA2 control lines default to deasserted (HIGH) state.
    // These are active-low signals; 1 = idle / no interrupt pending.
    via_input_c(cfg->via2, 0, 0, 1); // CA1: NuBus slot IRQ (no IRQ)
    via_input_c(cfg->via2, 0, 1, 1); // CA2: SCSI DRQ (no DMA request)
    via_input_c(cfg->via2, 1, 1, 1); // CB2: SCSI IRQ (no SCSI interrupt)

    // Initialise ADB controller (uses VIA1 for shift register and port B: ST0/ST1, vADBInt)
    se30->adb = adb_init(cfg->via1, cfg->scheduler, checkpoint);
    cfg->adb = se30->adb; // expose ADB to system-level input routing

    // Restore image list from checkpoint before devices that reference them.
    // Loop body lives in mac_checkpoint_restore_images so future glue030
    // family members (IIcx, IIx) reuse the same serialisation.
    if (checkpoint)
        mac_checkpoint_restore_images(cfg, checkpoint);

    // Initialise SCSI (NULL map: I/O dispatcher handles addressing)
    cfg->scsi = scsi_init(NULL, checkpoint);
    scsi_set_via(cfg->scsi, cfg->via2);

    setup_images(cfg);

    // Initialise ASC (NULL map: I/O dispatcher handles addressing)
    se30->asc = asc_init(NULL, cfg->scheduler, checkpoint);
    asc_set_via(se30->asc, cfg->via2);

    // Initialise SWIM floppy controller (NULL map: I/O dispatcher)
    se30->floppy = floppy_init(FLOPPY_TYPE_SWIM, NULL, cfg->scheduler, checkpoint);
    cfg->floppy = se30->floppy; // expose floppy to generic floppy commands

    // Bind device handles for the shared GLUE I/O dispatcher.
    mac030_glue_io_bind(&se30->glue_io, cfg, se30->asc, se30->floppy);

    // ---- MMU + NuBus + VRAM/VROM wiring ----

    // Create the 68030 PMMU and make it globally reachable
    uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
    uint32_t ram_size = cfg->ram_size;
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, ram_size);
    uint32_t rom_size = cfg->machine->rom_size;

    se30->mmu = mmu_init(ram_base, ram_size, cfg->machine->ram_max, rom_data, rom_size, SE30_ROM_START, SE30_ROM_END);
    assert(se30->mmu != NULL);
    g_mmu = se30->mmu;
    cpu_attach_mmu(cfg->cpu, se30->mmu);

    // Bring up the NuBus.  Slot $E is BUILTIN with the SE/30 video card;
    // slots $9..$B are the PDS pseudo-slots (EMPTY in v1, no card seated).
    // The card factory allocates VRAM/VROM and loads SE30.vrom from disk;
    // we expose its buffers via borrowed pointers below for the
    // memory_map_host_region calls and the I/O dispatcher's VRAM mirror.
    static const nubus_slot_decl_t se30_slots[] = {
        {.slot = 0x9, .kind = NUBUS_SLOT_EMPTY},
        {.slot = 0xA, .kind = NUBUS_SLOT_EMPTY},
        {.slot = 0xB, .kind = NUBUS_SLOT_EMPTY},
        {.slot = 0xE, .kind = NUBUS_SLOT_BUILTIN, .builtin_card_id = "builtin_se30_video"},
        {0},
    };
    cfg->nubus = nubus_init(cfg, se30_slots, checkpoint);
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

    // Let the MMU resolve VRAM physical addresses during table walks.
    // VRAM stays identity-mapped at its logical base for MMU resolution.
    memory_map_host_region(cfg->mem_map, "se30_vram", se30->vram, SE30_VRAM_BASE, SE30_VRAM_SIZE, /*writable*/ true);

    // Let the MMU resolve VROM physical addresses during table walks.
    // TT identity-maps NuBus addresses, so physical $FEFF8000 = logical $FEFF8000.
    memory_map_host_region(cfg->mem_map, "se30_vrom", se30->vrom, SE30_VROM_PHYS, SE30_VROM_SIZE, /*writable*/ false);

    // Emulate the GLUE chip's transparent NuBus slot address decoding.
    // The SE/30 ROM never writes TT registers (confirmed by ROM binary scan);
    // real hardware routes $F0-$FF to the NuBus bus controller, bypassing the
    // PMMU entirely. Set TT1 so the MMU identity-maps NuBus slot space, which
    // lets phys_to_host() resolve $FE000000 to the VRAM buffer.
    //
    // TT1 is restricted to supervisor FCs (FC_BASE=4 FC_MASK=3 → FC=4..7) so
    // user-mode accesses in $F0000000-$FFFFFFFF fall through to the CRP walk.
    // A/UX 3.0.1 retail places USRSTACK at $FFFFFFE0 (top of 32-bit VA) and
    // maps it to real RAM via the user CRP; a user-FC-matching TT1 would
    // short-circuit that walk and silently drop stack pushes (identity phys
    // $FFFFFxxx is unmapped RAM).
    // TT1: base=$F0, mask=$0F (match $F0-$FF), E=1, FC_BASE=4 FC_MASK=3
    se30->mmu->tt1 = 0xF00F8043;

    // Register alternate physical addresses for page-table-mapped access.
    // After the ROM sets up MMU page tables, logical $FExxxxxx maps to
    // physical $50Fxxxxx. Both VRAM and VROM must be accessible at their
    // page-table-mapped physical addresses in addition to their TT addresses.
    memory_map_host_region_alias(cfg->mem_map, SE30_VRAM_PHYS_ALT, SE30_VRAM_BASE);
    memory_map_host_region_alias(cfg->mem_map, SE30_VROM_PHYS_ALT, SE30_VROM_PHYS);

    // Only NuBus expansion slots $9-$D generate bus errors on unmapped reads.
    // Slot $E is built-in video (mapped). Slot $F and $0-$8 are internal/absent
    // and return 0 without bus error, matching SE/30 GLUE chip behavior.
    memory_set_bus_error_range(cfg->mem_map, 0xF9000000, 0xFDFFFFFF);

    // Populate SE/30 memory layout (RAM, ROM, VRAM, VROM, I/O, overlay).
    // The card-owned VRAM/VROM bytes are wired into the page table here;
    // the framebuffer pointer the renderer reads lives on the card's
    // display_t and is reached via system_display() / cfg->nubus.
    se30_memory_layout_init(cfg);

    // Re-drive VIA outputs on checkpoint restore (also restores alt-buffer state)
    if (checkpoint) {
        // Restore VRAM contents
        system_read_checkpoint_data(checkpoint, se30->vram, SE30_VRAM_SIZE);

        // Restore VROM from checkpoint (content for consolidated, file ref for quick)
        checkpoint_read_file(checkpoint, se30->vrom, SE30_VROM_SIZE, NULL);

        // Restore MMU guest registers (must match save order in se30_checkpoint_save)
        mmu_checkpoint_restore(se30->mmu, checkpoint);

        // Flush TLB so page table walks use the restored CRP/SRP/TC
        mmu_invalidate_tlb(se30->mmu);

        // Re-assign MMU pointers that checkpoint_restore overwrote with stale addresses
        g_mmu = se30->mmu;
        cpu_attach_mmu(cfg->cpu, se30->mmu);

        via_redrive_outputs(cfg->via1);
        via_redrive_outputs(cfg->via2);
    }

    cfg->debugger = debug_init();

    scheduler_start(cfg->scheduler);

    // Initialise IRQ/IPL only for cold boot
    if (!checkpoint) {
        cfg->irq = 0;
        cpu_set_ipl(cfg->cpu, 0);
    }
}

// Tear down all SE/30 resources in reverse init order.
static void se30_teardown(config_t *cfg) {
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);

    se30_state_t *se30 = se30_state(cfg);

    if (se30) {
        if (se30->mmu) {
            mmu_delete(se30->mmu); // also clears g_mmu if it matches
            se30->mmu = NULL;
        }
        // VRAM, VROM, and the borrowed video_card pointer all live on the
        // slot-$E card; system_destroy calls nubus_delete before us, which
        // in turn calls the card's teardown — we just clear our borrowed
        // references here.
        se30->vram = NULL;
        se30->vrom = NULL;
        se30->video_card = NULL;
        if (se30->floppy) {
            floppy_delete(se30->floppy);
            se30->floppy = NULL;
        }
        if (se30->asc) {
            asc_delete(se30->asc);
            se30->asc = NULL;
        }
        if (se30->adb) {
            adb_delete(se30->adb);
            se30->adb = NULL;
            cfg->adb = NULL;
        }
    }

    if (cfg->scsi) {
        scsi_delete(cfg->scsi);
        cfg->scsi = NULL;
    }
    if (cfg->via2) {
        via_delete(cfg->via2);
        cfg->via2 = NULL;
    }
    if (cfg->via1) {
        via_delete(cfg->via1);
        cfg->via1 = NULL;
    }
    if (cfg->scc) {
        scc_delete(cfg->scc);
        cfg->scc = NULL;
    }
    if (cfg->rtc) {
        rtc_delete(cfg->rtc);
        cfg->rtc = NULL;
    }
    if (cfg->scheduler) {
        scheduler_delete(cfg->scheduler);
        cfg->scheduler = NULL;
    }
    if (cfg->cpu) {
        cpu_delete(cfg->cpu);
        cfg->cpu = NULL;
    }
    if (cfg->mem_map) {
        memory_map_delete(cfg->mem_map);
        cfg->mem_map = NULL;
    }
    if (cfg->debugger) {
        debug_cleanup(cfg->debugger);
        cfg->debugger = NULL;
    }

    if (se30) {
        free(se30);
        cfg->machine_context = NULL;
    }
}

// ============================================================
// Checkpoint
// ============================================================

// Save complete SE/30 machine state to a checkpoint stream.
// Order must match the restore path in se30_init().
static void se30_checkpoint_save(config_t *cfg, checkpoint_t *cp) {
    se30_state_t *se30 = se30_state(cfg);

    memory_map_checkpoint(cfg->mem_map, cp);
    cpu_checkpoint(cfg->cpu, cp);
    scheduler_checkpoint(cfg->scheduler, cp);

    system_write_checkpoint_data(cp, &cfg->irq, sizeof(cfg->irq));

    rtc_checkpoint(cfg->rtc, cp);
    scc_checkpoint(cfg->scc, cp);
    via_checkpoint(cfg->via1, cp);
    via_checkpoint(cfg->via2, cp);

    adb_checkpoint(se30->adb, cp);

    // Checkpoint list of images before devices that reference them.
    // Family-shared serialisation lives in mac_checkpoint_save_images.
    mac_checkpoint_save_images(cfg, cp);

    scsi_checkpoint(cfg->scsi, cp);
    asc_checkpoint(se30->asc, cp);
    floppy_checkpoint(se30->floppy, cp);

    // Save VRAM contents (must match restore order in se30_init).  The
    // bytes are owned by the slot-$E card; se30->vram is a borrowed
    // pointer into that buffer, so the existing memcpy still hits the
    // right backing store.
    system_write_checkpoint_data(cp, se30->vram, SE30_VRAM_SIZE);

    // Save VROM (content embedded in consolidated checkpoints, path
    // reference in quick).  The path lives on the card too.
    const char *vrom_path = builtin_se30_video_vrom_path(se30->video_card);
    checkpoint_write_file(cp, vrom_path ? vrom_path : "");

    // Save MMU guest registers (TC, CRP, SRP, TT0, TT1, MMUSR, enabled flag)
    mmu_checkpoint_save(se30->mmu, cp);
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

// Macintosh SE/30 hardware profile descriptor
const hw_profile_t machine_se30 = {
    .name = "Macintosh SE/30",
    .id = "se30",

    // 68030 at 15.6672 MHz
    .cpu_model = 68030,
    .freq = 15667200,
    .mmu_present = true,
    .fpu_present = true, // 68882 FPU standard on SE/30
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
    .needs_vrom = true,

    // Two VIAs, ADB present, one PDS slot (no NuBus)
    .via_count = 2,
    .has_adb = true,
    .has_nubus = false,
    .nubus_slot_count = 0,

    // Lifecycle callbacks
    .init = se30_init,
    .reset = se30_reset,
    .teardown = se30_teardown,
    .checkpoint_save = se30_checkpoint_save,
    .checkpoint_restore = NULL, // restore handled by se30_init when checkpoint != NULL
    .memory_layout_init = se30_memory_layout_init,
    .update_ipl = se30_update_ipl,
    .trigger_vbl = se30_trigger_vbl,
};
