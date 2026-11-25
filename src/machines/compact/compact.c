// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// compact.c
// The compact Macintosh family: the PAL boards (Macintosh 128K, 512K, 512K
// enhanced and Plus) and the BBU boards (Macintosh SE, SE FDHD and Classic).
//
// One substrate serves all seven: they are the same 7.8336 MHz MC68000 with
// RAM at zero, the ROM at $400000, the SCC, the floppy controller, the one VIA
// and the 512 x 342 screen buffer fetched from the top of RAM.  What a model
// changes is data in its compact_board_t (hw_profile_t.board): whether it has
// the 5380, which floppy chip it carries, and which generation of general
// logic decodes it.  The BBU generation (Guide to the Macintosh Family
// Hardware, 2nd ed., ch. 3 and 4) differs in four wired facts, each a branch
// here: the keyboard and mouse are ADB devices behind the VIA shift register;
// the ROM overlay is dropped by the first ROM access, not by VIA PA4; PA4
// selects between the internal floppy drives instead; and PB6 masks the SCSI
// interrupt onto /IPL0.
//
// This file owns the whole lifecycle: init, teardown, the VIA/SCC/SCSI
// interrupt callbacks, the VBL trigger, and the hw_profile_t descriptors.

#include "config_seed.h"
#include "m68k_board_overlay.h"
#include "mac_host_io.h"
#include "machine.h"
#include "machine_teardown.h"
#include "slot_tables.h"
#include "system_internal.h" // full config_t definition

#include "adb.h"
#include "appletalk.h"
#include "checkpoint.h"
#include "checkpoint_machine.h"
#include "cpu.h"
#include "debug.h"
#include "display.h"
#include "floppy.h"
#include "image.h"
#include "keyboard.h"
#include "log.h"
#include "machine_checkpoint.h"
#include "machine_irq.h"
#include "memory.h"
#include "mmu.h" // tlb_track_page
#include "mouse.h"
#include "page_fill.h"
#include "pram_defaults.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "shell.h"
#include "sound.h"
#include "via.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("board");

// One compact model's board: the data the shared substrate branches on.
typedef struct compact_board {
    // The BBU generation (SE, SE FDHD, Classic): ADB, the access-dropped
    // overlay, PA4 drive select and the PB6 SCSI-interrupt mask.  False on
    // the PAL boards (128K, 512K, 512K enhanced, Plus).
    bool bbu;
    bool has_scsi; // an NCR 5380 at $580000 (Plus, SE, SE FDHD, Classic)
    floppy_type_t floppy_chip; // FLOPPY_TYPE_IWM, or FLOPPY_TYPE_SWIM (SE FDHD, Classic)
    // BBU boards with two internal drives (SE, SE FDHD): VIA PA4 is vDriveSel,
    // steering /ENBL1 between the lower and the upper internal drive, so the
    // second drive position is the upper internal drive and the external port
    // (/ENBL2) is not modelled.  False: the second position is the external
    // drive on /ENBL2 and no upper drive is fitted; the Classic has one
    // internal drive and gives PA4 no such job.
    bool drive_select;
    // The 128K and 512K: the single-sided 400K drive has no speed control of
    // its own, so the board's disk-speed PWM (the sound buffer's low bytes)
    // sets its spindle (compact_pwm_rpm).
    bool pwm_disk_speed;
} compact_board_t;

// The model's board descriptor (hw_profile_t.board).
static inline const compact_board_t *compact_board(config_t *cfg) {
    return (const compact_board_t *)cfg->machine->board;
}

// Family peripheral state not shared with other machines.
// Accessed through config_t.machine_context.
typedef struct compact_state {
    sound_t *sound; // PWM sound (VIA-driven)
    // The display descriptor, owned directly -- no NuBus involved.
    // bits flips between the main and alternate framebuffer addresses
    // each time VIA PA6 toggles; fb_dirty is set on every change so
    // the renderer re-uploads.
    display_t display;
    // PAL boards: VIA PA4 (vOverlay) as the address decoder sees it -- true
    // while the ROM answers at $000000 (Guide 2e, ch. 3).  The line is pulled
    // up, so it is high whenever PA4 is not driven -- at power-on and after
    // /RESET.
    bool rom_overlay;
    // BBU boards: the access-triggered overlay (Guide 2e, ch. 3: "The BBU
    // switches from the ROM overlay address map to the normal address map the
    // first time an access is made in the range $40 0000 through $5F FFFF").
    m68k_board_overlay_t overlay;
    // BBU boards: the 5380's IRQ output, before the PB6 mask.
    bool scsi_irq;
} compact_state_t;

// Helper: return the family state from a config handle
static inline compact_state_t *compact_st(config_t *cfg) {
    return (compact_state_t *)cfg->machine_context;
}

// ============================================================
// Forward declarations for the family callbacks
// ============================================================

static void compact_via_output(void *context, uint8_t port, uint8_t output);
static void compact_via_shift_out(void *context, uint8_t byte);
static void compact_via_irq(void *context, bool active);
static void compact_scc_irq(void *context, bool active);
static void compact_update_ipl(config_t *cfg, int source, bool active);

// ============================================================
// Video buffer helper (family address constants)
// ============================================================

// Interrupt source bits in cfg->rt.irq, matching the M68K_BOARD_IRQ_* /
// AV_IRQ_* convention.  These are a source MASK, not an IPL level: the PALs
// (or the BBU) derive the level from the set of asserted sources
// (compact_update_ipl).  The Lisa's identically-named lisa_update_ipl()
// really does take a level, so two adjacent 68000 families used one signature
// with opposite meanings.
#define COMPACT_IRQ_VIA  (1u << 0)
#define COMPACT_IRQ_SCC  (1u << 1)
#define COMPACT_IRQ_SCSI (1u << 2) // BBU boards only: the 5380 IRQ, PB6-masked

// The routing: the SCC on /IPL1 (level 2) wins over the VIA -- and, on the
// BBU boards, the SCSI controller -- on /IPL0 (level 1).
static const machine_irq_route_t compact_irq_routes[] = {
    {COMPACT_IRQ_SCC,                    2},
    {COMPACT_IRQ_VIA | COMPACT_IRQ_SCSI, 1},
    {0,                                  0}, // sentinel
};

#define COMPACT_SCSI_BASE 0x580000UL
#define COMPACT_SCSI_SIZE 0x080000UL

// ROM start in the 24-bit address space (== the RAM ceiling of 4 MB)
#define COMPACT_ROM_START 0x400000UL
// ROM region end: the ROM decode stops where the SCSI window starts
#define COMPACT_ROM_END 0x580000UL

// The 2.5 MB SIMM configuration, and the 2 MB row it starts with
#define COMPACT_RAM_2_5MB   0x280000UL
#define COMPACT_RAM_ROW_2MB 0x200000UL

// The RAM image the PALs' overlay map places at $600000 (Inside Macintosh III)
#define COMPACT_OVLY_RAM_START 0x600000UL
#define COMPACT_OVLY_RAM_END   0x800000UL

// The IWM/SWIM window ($DFE1FF is the chip's base; A9-A12 select the register)
#define COMPACT_FLOPPY_BASE 0xD80000UL
#define COMPACT_FLOPPY_SIZE 0x080000UL

// The screen buffers, measured down from the top of RAM (ScrnBase is
// MemTop - $5900 on every compact model; the alternate buffer sits $8000
// below it).
#define COMPACT_SCREEN_FROM_TOP  0x5900u
#define COMPACT_ALT_SCREEN_BELOW 0x8000u

// Switch between main and alternate video buffer addresses.
// Main buffer is at top of RAM minus COMPACT_SCREEN_FROM_TOP; alternate is
// COMPACT_ALT_SCREEN_BELOW bytes lower.
// Both addresses scale with installed RAM -- `ScrnBase`/`ScrnAlt` are
// computed from physical RAM size by the ROM boot code, so a Plus with 1 MB
// has its framebuffer at $FA700, not $3FA700, and a 128K at $1A700.  Updates
// the descriptor's `bits` field and marks the framebuffer dirty so the
// renderer re-uploads on the next frame.
static void compact_use_video_buffer(config_t *cfg, bool main) {
    compact_state_t *st = compact_st(cfg);
    uint32_t top = cfg->ram_size;
    uint32_t addr = main ? (top - COMPACT_SCREEN_FROM_TOP) : (top - COMPACT_SCREEN_FROM_TOP - COMPACT_ALT_SCREEN_BELOW);
    st->display.bits = ram_native_pointer(cfg->memory_map, addr);
    st->display.fb_dirty = true;
}

// Initialise the display descriptor.  Called once during compact_init,
// before compact_use_video_buffer fills in `bits`.
static void compact_display_init(config_t *cfg) {
    compact_state_t *st = compact_st(cfg);
    st->display.width = 512;
    st->display.height = 342;
    st->display.stride = 512 / 8;
    st->display.format = PIXEL_1BPP_MSB;
    st->display.bits = NULL;
    st->display.clut = NULL;
    st->display.clut_len = 0;
    st->display.shape_dirty = true;
}

// hw_profile_t.display callback -- surface the framebuffer to
// system_display() on the non-NuBus path.
static display_t *compact_display(config_t *cfg) {
    compact_state_t *st = compact_st(cfg);
    return st && st->display.bits ? &st->display : NULL;
}

// ============================================================
// Memory layout
// ============================================================

// Point one page's READ side at `host_ptr`; the write side is left alone, so
// with the overlay on a write to low memory still lands in RAM (the decoder
// steers reads only, as on the II-family GLUE).
static void compact_map_read_page(uint32_t p, uint8_t *host_ptr) {
    if (p >= g_page_count)
        return;
    uintptr_t adjusted = MEM_RAM_ENTRY((uintptr_t)host_ptr - ((uintptr_t)p << MEM_PAGE_SHIFT));
    memory_page_set_host(p, host_ptr, g_page_table[p].writable);
    if (g_supervisor_read)
        g_supervisor_read[p] = adjusted;
    if (g_user_read)
        g_user_read[p] = adjusted;
    tlb_track_page(p); // on the tracker like every fast-path entry (page_fill_68k)
    memory_logpoint_guard_page(p);
}

// PAL boards: drive the ROM overlay.  On maps the ROM image over the bottom
// of the address space for reads, off puts RAM back.  The ROM drops it a few
// instructions after the reset vector (CLR of PA4 at $4000D2 in the Plus v3
// ROM), before it touches RAM; until then its reset vectors at $0/$4 are the
// ROM's own.
static void compact_set_rom_overlay(config_t *cfg, bool on) {
    compact_state_t *st = compact_st(cfg);
    if (st->rom_overlay == on)
        return;
    st->rom_overlay = on;
    uint8_t *ram = ram_native_pointer(cfg->memory_map, 0);
    uint32_t rom_pages = cfg->machine->rom_size >> MEM_PAGE_SHIFT;
    for (uint32_t p = 0; p < rom_pages; p++) {
        // RAM below the ROM's size wraps (a 128K under a 64 KB ROM does not,
        // but keep the offset inside the installed RAM regardless)
        uint32_t ram_off = (p << MEM_PAGE_SHIFT) % cfg->ram_size;
        compact_map_read_page(p, on ? ram + cfg->ram_size + (p << MEM_PAGE_SHIFT) : ram + ram_off);
    }
    // The overlay map also places RAM at $600000 ("RAM is placed starting at
    // $600000", Inside Macintosh, vol. III, ch. 2; its ovlyRAM/ovlyScreen
    // equates): the 64 KB ROM runs its memory test and draws its error screen
    // there before it clears vOverlay.  The image is RAM from offset 0 (the
    // 2 MB window never reaches a 2.5 MB board's second row), wrapping on a
    // smaller board; it goes away with the overlay, leaving the range empty.
    for (uint32_t a = COMPACT_OVLY_RAM_START; a < COMPACT_OVLY_RAM_END; a += MEM_PAGE_SIZE) {
        if (on)
            page_fill_68k(a >> MEM_PAGE_SHIFT, ram + (a - COMPACT_OVLY_RAM_START) % cfg->ram_size, true);
        else
            page_clear(a >> MEM_PAGE_SHIFT);
    }
    LOG(1, "ROM overlay %s", on ? "on: ROM at $000000" : "off: RAM at $000000");
}

// BBU boards: RAM back at zero once the overlay drops -- the pages the overlay
// covered (the ROM's size from zero), wrapped into the installed RAM.
static void compact_bbu_map_ram(config_t *cfg) {
    uint32_t rom_pages = cfg->machine->rom_size >> MEM_PAGE_SHIFT;
    uint8_t *ram = ram_native_pointer(cfg->memory_map, 0);
    for (uint32_t p = 0; p < rom_pages && p < g_page_count; p++)
        page_fill_68k(p, ram + ((p << MEM_PAGE_SHIFT) % cfg->ram_size), true);
}

// BBU drive-select boards: steer /ENBL1 to the upper internal drive or, the
// default, the lower one (/ENBL2, the external port, reaches no drive here).
static void compact_route_enbl(config_t *cfg, bool upper) {
    floppy_set_enbl_route(cfg->floppy, upper ? 1 : 0, -1);
}

// The PAL boards' Phase Read area -- a STUB.  On hardware the value read tells
// the ROM whether the video and CPU clocks are in phase (Guide p.122, below);
// the emulator's clocks always are, so every read answers 0 and the phase
// adjustment loop is never exercised.  `log.set board 2` shows the reads.
static uint8_t compact_phase_read_uint8(void *dev, uint32_t addr) {
    (void)dev;
    LOG(2, "Phase Read (byte) at $%06x: not modelled, reads 0", addr);
    return 0;
}
static uint16_t compact_phase_read_uint16(void *dev, uint32_t addr) {
    (void)dev;
    LOG(2, "Phase Read (word) at $%06x: not modelled, reads 0", addr);
    return 0;
}
static uint32_t compact_phase_read_uint32(void *dev, uint32_t addr) {
    (void)dev;
    LOG(2, "Phase Read (long) at $%06x: not modelled, reads 0", addr);
    return 0;
}

// Populate the family memory layout in the page table.
// Called from compact_init() after memory_map_init() and cpu_init().
// Guide to the Macintosh Family Hardware, 2nd edition, page 122:
// At system startup, the OS reads an address in $F00000–$F7FFFF ("Phase Read")
// to verify that high-frequency timing signals are in phase (PAL boards).
static void compact_memory_layout_init(config_t *cfg) {
    const compact_board_t *board = compact_board(cfg);

    // Map RAM and ROM pages into the global page table
    memory_populate_pages(cfg->memory_map, COMPACT_ROM_START, COMPACT_ROM_END);

    // Mirror RAM into the unmapped gap [ram_size, COMPACT_ROM_START).  The
    // address decoder doesn't gate accesses in this range, so they wrap
    // physically into installed RAM ("All unused RAM space is occupied by
    // repeated RAM images", Guide 2e, Figure 3-3).  The Plus ROM's exception
    // save area at $3FFC80 (close to the 4 MB ceiling) relies on this -- on
    // a 1 MB Plus that address physically refers to RAM at $0FFC80.  Without
    // the mirror, the first interrupt/exception corrupts CPU state on any
    // sub-4-MB configuration.
    if (cfg->ram_size == COMPACT_RAM_2_5MB) {
        // Two unequal rows: the 1 MB-SIMM row's 2 MB at $000000 and the
        // 256 KB-SIMM row's 512 KB at $200000, which repeats through the
        // rest of its row's 2 MB window (A21 picks the row) -- not a wrap of
        // the 2.5 MB total, which would alias $280000 onto $000000.  The SE
        // ROM's memory sizing depends on it (Guide 2e, ch. 5, Figures 5-2
        // to 5-4: the larger row is the 2 MB one in every configuration).
        uint8_t *ram = ram_native_pointer(cfg->memory_map, 0);
        for (uint32_t a = COMPACT_RAM_2_5MB; a < COMPACT_ROM_START; a += MEM_PAGE_SIZE) {
            uint32_t off =
                COMPACT_RAM_ROW_2MB + ((a - COMPACT_RAM_ROW_2MB) % (COMPACT_RAM_2_5MB - COMPACT_RAM_ROW_2MB));
            page_fill_68k(a >> MEM_PAGE_SHIFT, ram + off, true);
        }
    } else if (cfg->ram_size < COMPACT_ROM_START)
        memory_populate_ram_mirror(cfg->memory_map, cfg->ram_size, COMPACT_ROM_START);

    // Register the Phase Read device (PAL boards: the BBU has none)
    if (!board->bbu) {
        // (The overlay map's RAM image at $600000 comes and goes with the
        // overlay: compact_set_rom_overlay.)
        memory_interface_t phase_read;
        memset(&phase_read, 0, sizeof(phase_read));
        phase_read.read_uint8 = &compact_phase_read_uint8;
        phase_read.read_uint16 = &compact_phase_read_uint16;
        phase_read.read_uint32 = &compact_phase_read_uint32;
        memory_map_add(cfg->memory_map, 0x00F00000, 0x00080000, "Phase Read", &phase_read, NULL);
    }
}

// BBU boards: register the overlay's ROM aperture as its trigger device and
// arm it.  Called once the RAM and ROM pages are in place.
static void compact_bbu_overlay_init(config_t *cfg) {
    compact_state_t *st = compact_st(cfg);
    m68k_board_overlay_init(&st->overlay, cfg, COMPACT_ROM_START, COMPACT_ROM_END, compact_bbu_map_ram,
                            cfg->machine->name);
    memory_map_add(cfg->memory_map, COMPACT_ROM_START, COMPACT_ROM_END - COMPACT_ROM_START, "ROM aperture",
                   &st->overlay.iface, &st->overlay);
    m68k_board_overlay_arm(&st->overlay);
}

// ============================================================
// Checkpoint: the BBU boards' private part
// ============================================================

// The BBU overlay is no VIA output, so a restore cannot re-derive it: it rides
// in its own part.  (The PAL boards' overlay is VIA PA4, re-driven on restore.)
static void part_save_compact_bbu(void *obj, checkpoint_t *cp) {
    compact_state_t *st = obj;
    CP_FIELD(system_write_checkpoint_data, cp, st->overlay.armed);
}

// Register (and on a restore, read) the BBU part; the layout armed the overlay,
// so a post-overlay snapshot drops it again.
static void compact_bbu_part(config_t *cfg, checkpoint_t *cp) {
    compact_state_t *st = compact_st(cfg);
    machine_part_begin(cfg, cp, "compact");
    if (cp) {
        bool armed = true;
        CP_FIELD(system_read_checkpoint_data, cp, armed);
        if (!armed)
            m68k_board_overlay_drop(&st->overlay);
    }
    machine_part(cfg, cp, "compact", part_save_compact_bbu, st);
}

// ============================================================
// SCSI interrupt (BBU boards)
// ============================================================

// True when VIA PB6 (vH4) drives low: the SCSI IRQ reaches /IPL0.  "When this
// bit is set high, interrupts on the SCSI controller's IRQ line are disabled"
// (Guide 2e, Table 4-12); an undriven pin counts as high.
static bool compact_scsi_irq_enabled(config_t *cfg) {
    return cfg->via1 && (via_port_direction(cfg->via1, 1) & 0x40) && !(via_port_output(cfg->via1, 1) & 0x40);
}

// Re-derive the SCSI source from the chip's line and the PB6 mask.
static void compact_scsi_irq_update(config_t *cfg) {
    compact_state_t *st = compact_st(cfg);
    compact_update_ipl(cfg, COMPACT_IRQ_SCSI, st->scsi_irq && compact_scsi_irq_enabled(cfg));
}

// The 5380's IRQ output changed.  DRQ raises no interrupt on these boards:
// "The DRQ interrupt signal ... does not generate an MC68000 interrupt in the
// Macintosh SE" (Guide 2e, ch. 3).
static void compact_scsi_irq(void *context, bool irq, bool drq) {
    (void)drq;
    config_t *cfg = (config_t *)context;
    compact_state_t *st = compact_st(cfg);
    if (!st)
        return;
    st->scsi_irq = irq;
    compact_scsi_irq_update(cfg);
}

// ============================================================
// Init / Teardown
// ============================================================

// Initialise all family subsystems.  With a checkpoint, each device restores
// its state from it as it is built, and registers its part right after.
static int compact_init(config_t *cfg, checkpoint_t *checkpoint) {
    const compact_board_t *board = compact_board(cfg);

    // Allocate the family peripheral state
    compact_state_t *st = malloc(sizeof(compact_state_t));
    if (!st) {
        LOG(0, "Error: out of memory allocating the machine state for %s", cfg->machine->name);
        return -1;
    }
    memset(st, 0, sizeof(compact_state_t));
    cfg->machine_context = st;

    // Initialise parameterised memory: 24-bit address space, configured RAM, the model's ROM
    machine_part_begin(cfg, checkpoint, "memory");
    cfg->memory_map = memory_map_init(cfg->machine->address_bits, cfg->ram_size, cfg->machine->rom_size,
                                      MEMORY_BUS_ERR_NONE, &cfg->build_opts.rom, checkpoint); // no bus-error watchdog
    machine_part(cfg, checkpoint, "memory", part_save_memory, cfg->memory_map);

    // Populate the family memory layout (RAM/ROM page table + Phase Read)
    compact_memory_layout_init(cfg);
    // Power-on: nothing drives PA4 yet, so the pull-up holds the overlay on.
    // A restore re-derives it from the VIA instead (via_redrive_outputs).
    // The BBU arms its own access-dropped overlay here, and a restore drops
    // it again from the board's part (compact_bbu_part).
    machine_part_begin(cfg, checkpoint, "cpu");
    if (board->bbu)
        compact_bbu_overlay_init(cfg);
    else if (!checkpoint)
        compact_set_rom_overlay(cfg, true);

    // The profile is the source of truth for the CPU model, as it is for the
    // clock below and as m68k_board_build_core states for the II families.
    // Every profile behind this substrate declares 68000 -- but system_create
    // derives cfg->cpu_arch from the profile unconditionally, so a profile
    // that ever disagreed with a hardcoded core here would tag the machine
    // with an arch it is not running.
    cfg->cpu = cpu_init(cfg->machine->cpu_model, checkpoint);
    machine_part(cfg, checkpoint, "cpu", part_save_cpu, cfg->cpu);

    machine_part_begin(cfg, checkpoint, "scheduler");
    sched_cpu_if_t cpu_if = cpu_sched_if(cfg->cpu); // the 68K main-CPU seam adapter
    cfg->scheduler = scheduler_init(&cpu_if, checkpoint);
    machine_part(cfg, checkpoint, "scheduler", part_save_scheduler, cfg->scheduler);
    // Average CPI for the 7.8336 MHz 68000: the Plus retires ~783k
    // instructions per emulated second (~0.78 MIPS, slightly above the
    // ~0.7 MIPS of real hardware). Calibrated against MusicWorks 0.42: its
    // VBL-locked synthesis engine writes 370 samples per two frames and
    // keeps up on real hardware; at CPI 12 it misses that budget by ~9%
    // and its own buffer content glitches (~90 transients per 8 s of
    // music), at CPI <= 11 it locks cleanly (4 transients — the app's real
    // note attacks). CPI 10 rather than the threshold-exact 11 buys safety
    // margin for CPU-hungrier real-time guests. (The pre-two-modes
    // scheduler defaulted to CPI 4 here — a ~3x overclocked Plus.)
    // The profile is the source of truth for the clock (machine_profile.h
    // §freq), and it is what catalog.profile exports to the frontend.  This
    // line was missing: the Plus ran correctly only because the scheduler's
    // own default happens to equal 7.8336 MHz exactly, so the exported value
    // and the value actually used were two independent constants that agreed
    // by coincidence.  The Lisa's comment above its own call records being
    // bitten by that default.
    scheduler_set_frequency(cfg->scheduler, cfg->machine->freq);
    scheduler_set_cpi(cfg->scheduler, 10);

    machine_part_irq(cfg, checkpoint);

    machine_part_begin(cfg, checkpoint, "rtc");
    cfg->rtc = rtc_init(cfg->scheduler, checkpoint, true, cfg->machine->pram);
    machine_part(cfg, checkpoint, "rtc", part_save_rtc, cfg->rtc);

    machine_part_begin(cfg, checkpoint, "scc");
    cfg->scc = scc_init(cfg->memory_map, cfg->scheduler, compact_scc_irq, cfg, checkpoint);
    machine_part(cfg, checkpoint, "scc", part_save_scc, cfg->scc);

    // SCC PCLK = C8M (7.8336 MHz = CPU clock), RTxC = 3.6864 MHz
    scc_set_clocks(cfg->scc, 7833600, 3686400);

    // The machine's connection to the AppleTalk network, through the SCC.
    machine_part_begin(cfg, checkpoint, "appletalk");
    cfg->atalk = atalk_conn_new(appletalk_network(), cfg->scheduler, cfg->scc, checkpoint);
    machine_part(cfg, checkpoint, "appletalk", part_save_atalk, cfg->atalk);
    machine_part_imagewriter(cfg, checkpoint, false);

    // 7.8336 MHz / 783.36 kHz = exactly 10, so this is the literal it replaces.
    // The BBU boards take no shift-out callback: adb.c reads the shift
    // register itself at each port-B ST transition (glue.h, BUG-004).
    machine_part_begin(cfg, checkpoint, "via1");
    cfg->via1 =
        via_init(cfg->memory_map, cfg->scheduler, via_freq_factor_for_clock(cfg->machine->freq), "via1",
                 compact_via_output, board->bbu ? NULL : compact_via_shift_out, compact_via_irq, cfg, checkpoint);
    machine_part(cfg, checkpoint, "via1", part_save_via, cfg->via1);
    // The RTC's data line rides VIA PB0: wired right after the VIA is built,
    // the same position as the GLUE family (glue_init).
    rtc_set_via(cfg->rtc, cfg->via1);

    machine_part_begin(cfg, checkpoint, "sound");
    st->sound = sound_init(cfg->memory_map, cfg->scheduler, checkpoint);
    machine_part(cfg, checkpoint, "sound", part_save_sound, st->sound);
    cfg->sound = st->sound; // mirror onto cfg so the object-model `sound`
                            // class can find it via cfg->sound

    // VIA PA3's input level is held LOW at boot.  (PA3 is an output here:
    // vSndPg2 on the PAL boards, vSync on the BBU; the SCC's W/REQ is PA7,
    // vSCCWrReq -- Guide 2e, Table 4-2.)  This used to be the core VIA's port-A
    // default (0xF7), which made one machine's boot condition every machine's
    // -- and on the II family the same pin is a NuBus slot /NMRQ, so each of
    // them had to raise it back up or slot $C asserted forever.
    via_input(cfg->via1, /*port A*/ 0, /*PA3*/ 3, 0);

    if (board->bbu) {
        // The ADB transceiver behind the VIA shift register and PB3-PB5, as
        // on the SE/30 (Guide 2e, ch. 8).  Keyboard and mouse are its devices.
        machine_part_begin(cfg, checkpoint, "adb");
        cfg->adb = adb_init(cfg->via1, cfg->scheduler, checkpoint);
        machine_part(cfg, checkpoint, "adb", part_save_adb, cfg->adb);
    } else {
        machine_part_begin(cfg, checkpoint, "mouse");
        cfg->mouse = mouse_init(cfg->scheduler, cfg->scc, cfg->via1, checkpoint);
        machine_part(cfg, checkpoint, "mouse", part_save_mouse, cfg->mouse);
    }

    // The image list, before any device that may reference an image.
    machine_part_images(cfg, checkpoint);

    if (board->has_scsi) {
        machine_part_begin(cfg, checkpoint, "scsi");
        cfg->scsi = machine_scsi_bus_init(cfg, checkpoint, "scsi");
        scsi_5380_attach(cfg->scsi, checkpoint);
        machine_part(cfg, checkpoint, "scsi", part_save_scsi, cfg->scsi);
        // Where the 5380 answers is this machine's decode, not the chip model's.
        //
        // Guide to the Macintosh Family Hardware, 2nd ed., ch. 3: the SCSI
        // controller's eight registers are selected by A6 through A4, while
        // "A18 through A10, A8, A7, and A3 through A1 have no significance for
        // the SCSI in the Macintosh Plus computer, so there are thousands of
        // possible addresses that will access the same register" -- which is
        // why a whole half-megabyte answers rather than the 128 bytes the
        // register file occupies.  The SE's BBU keeps the same decode (its
        // DRQ handshake paces the pseudo-DMA aperture, which the chip model
        // already completes at once).
        //
        // scsi_5380_attach() used to do this itself, from inside src/core, with
        // this address compiled into the chip model.  Every other 5380 machine
        // already took scsi_get_memory_interface() and placed it with its own
        // decode (glue_io.c, mdu_io.c, iifx.c); the compact family does the same.
        memory_map_add(cfg->memory_map, COMPACT_SCSI_BASE, COMPACT_SCSI_SIZE, "scsi",
                       (memory_interface_t *)scsi_get_memory_interface(cfg->scsi), cfg->scsi);
        // The BBU boards route the 5380's IRQ to /IPL0 behind the PB6 mask;
        // the Plus leaves it unconnected (polled, interrupt-less).
        if (board->bbu) {
            scsi_set_irq_callback(cfg->scsi, compact_scsi_irq, cfg);
            st->scsi_irq = scsi_get_irq_active(cfg->scsi);
        }
    }

    if (!board->bbu) {
        machine_part_begin(cfg, checkpoint, "keyboard");
        cfg->keyboard = keyboard_init(cfg->scheduler, cfg->scc, cfg->via1, checkpoint);
        machine_part(cfg, checkpoint, "keyboard", part_save_keyboard, cfg->keyboard);
    }

    machine_part_begin(cfg, checkpoint, "floppy");
    // board->floppy_chip is the board's controller chip; the slot table's
    // kinds are the drive in each position.  They are different facts: the
    // IWM drives 400K/800K GCR drives, the SWIM the 1.4 MB SuperDrive too.
    cfg->floppy = floppy_init(board->floppy_chip, NULL, cfg->scheduler, machine_floppy_count(cfg), checkpoint,
                              config_images(cfg));
    machine_part(cfg, checkpoint, "floppy", part_save_floppy, cfg->floppy);
    // Either chip answers in the same window, through the same A9-A12 decode
    floppy_map_stride_window(cfg->floppy, cfg->memory_map, COMPACT_FLOPPY_BASE, COMPACT_FLOPPY_SIZE);

    if (board->bbu)
        compact_bbu_part(cfg, checkpoint);

    // Initialise the display descriptor before anything that might call
    // compact_use_video_buffer().  Both the cold-boot default and the
    // checkpoint-driven via_redrive_outputs path mutate st->display.
    compact_display_init(cfg);

    // After floppy exists, if restoring from a checkpoint, re-drive VIA outputs
    // so SEL and other external signals propagate to the floppy.
    if (checkpoint) {
        via_redrive_outputs(cfg->via1);
    }

    // On cold boot, default to main video buffer.
    // When restoring from a checkpoint, VIA outputs will re-drive the selection.
    if (!checkpoint) {
        compact_use_video_buffer(cfg, true);
        // Power-on: PA4 is undriven, which selects the lower internal drive
        if (board->bbu && board->drive_select)
            compact_route_enbl(cfg, false);
        // Blank both screen buffers so a cold boot scans out black rather than
        // the white that zeroed DRAM gives at 1 bpp.  The framebuffer IS main
        // RAM, so this only touches the two visible windows, and only before
        // the guest runs — the ROM's RAM test writes and reads back its own
        // patterns over them regardless.
        size_t screen_bytes = (size_t)st->display.stride * st->display.height;
        uint8_t fill = display_black_fill(st->display.format);
        for (int main_buf = 0; main_buf <= 1; main_buf++) {
            uint32_t addr = cfg->ram_size - COMPACT_SCREEN_FROM_TOP - (main_buf ? 0 : COMPACT_ALT_SCREEN_BELOW);
            uint8_t *p = ram_native_pointer(cfg->memory_map, addr);
            if (p)
                memset(p, fill, screen_bytes);
        }
    }

    cfg->debugger = debug_init();

    // Initialise IRQ/IPL only for cold boot; on restore, devices already re-assert.
    machine_irq_cold_reset(cfg, checkpoint);
    return 0;
}

// Tear down all family resources in reverse init order.
static void compact_teardown(config_t *cfg) {
    // Machine-owned devices first, then the shared delete-chain.  This is the
    // shape every family follows; the Plus and the Lisa were the last two that
    // hand-rolled the whole thing.
    //
    // ORDER IS LOAD-BEARING and this is why: the floppy used to be deleted
    // AFTER scheduler_delete here.  That was harmless while floppy_delete only
    // freed memory, but it now calls scheduler_forget_source(floppy->scheduler,
    // ...) -- a read through a pointer to the freed scheduler.  Everything that
    // holds a scheduler handle must go before the scheduler does, which is
    // exactly what machine_teardown_config_devices guarantees by deleting the
    // scheduler itself, last.
    compact_state_t *st = compact_st(cfg);
    if (cfg->keyboard) {
        keyboard_delete(cfg->keyboard);
        cfg->keyboard = NULL;
    }
    if (cfg->mouse) {
        mouse_delete(cfg->mouse);
        cfg->mouse = NULL;
    }
    if (st && st->sound) {
        sound_delete(st->sound);
        st->sound = NULL;
        cfg->sound = NULL;
    }

    // floppy, adb, scsi, appletalk, scc, rtc, via1, the scheduler, the CPU, the
    // memory map and the debugger -- in the one order that is documented once.
    machine_teardown_config_devices(cfg);
}

// ============================================================
// VIA / SCC callbacks
// ============================================================

// Family interrupt routing: set/clear one COMPACT_IRQ_* source bit and
// re-derive the CPU IPL -- the same (cfg, source, active) shape as
// m68k_board_update_ipl.
static void compact_update_ipl(config_t *cfg, int source, bool active) {
    int old_irq = cfg->rt.irq;
    int old_ipl = cfg->cpu ? cpu_get_ipl(cfg->cpu) : 0;

    // Guide to the Macintosh Family Hardware, chapter 3:
    // The interrupt request line from the VIA goes to the PALs,
    // which can assert an interrupt to the processor on line /IPL0.
    // The PALs also monitor interrupt line /IPL1,
    // and deassert /IPL0 whenever /IPL1 is asserted.
    // (The BBU does the same, with the SCSI IRQ joining the VIA on /IPL0.)

    // The SCC on /IPL1 wins over the VIA on /IPL0, exactly as the quote
    // above describes (compact_irq_routes).
    int new_ipl = machine_irq_update(cfg, compact_irq_routes, source, active);

    LOG(2, "compact_update_ipl: source=%d active=%d irq:%d->%d ipl:%d->%d", source, active ? 1 : 0, old_irq,
        cfg->rt.irq, old_ipl, new_ipl);
}

// VIA output callback: routes port changes to the floppy, video, sound, RTC
// and, by board generation, the overlay or drive select, ADB and the SCSI
// mask (Guide 2e, Tables 4-2, 4-3 and 4-12).
static void compact_via_output(void *context, uint8_t port, uint8_t output) {
    config_t *cfg = (config_t *)context;
    compact_state_t *st = compact_st(cfg);
    bool bbu = compact_board(cfg)->bbu;

    // via_init re-drives this callback while it restores a checkpoint, and
    // the VIA is now built before the sound chip (see compact_init), so the
    // half-built case is real and the sound_* entry points do not guard
    // against NULL.  Same shape as av.c's "scc_init fires this before the
    // PSC is built" guard.  cfg->via1 is still NULL then too;
    // via_redrive_outputs runs again once it is set.
    sound_t *snd = st ? st->sound : NULL;

    if (port == 0) {
        // PA6 vPage2: 0 = alternate screen buffer, 1 = main
        if (st)
            compact_use_video_buffer(cfg, (output >> 6) & 1);
        // PA5 vHeadSel: the drives' SEL line
        floppy_set_sel_signal(cfg->floppy, (output & 0x20) != 0);
        if (st && cfg->via1) {
            // `output` is `ORA & DDRA`, so an undriven pin reads 0 here
            bool pa4_driven = (via_port_direction(cfg->via1, 0) & 0x10) != 0;
            if (!bbu)
                // PAL: PA4 is vOverlay, pulled up, so undriven means overlay ON
                compact_set_rom_overlay(cfg, !pa4_driven || (output & 0x10) != 0);
            else if (compact_board(cfg)->drive_select)
                // BBU: PA4 vDriveSel picks the lower (1) or the upper (0)
                // internal drive for /ENBL1; undriven counts as lower.
                compact_route_enbl(cfg, pa4_driven && !(output & 0x10));
        }
        if (snd) {
            // PA3: vSndPg2 on the PAL boards (0 = the alternate sound
            // buffer); vSync on the BBU (not modelled), which has no
            // alternate sound buffer, so the main one always plays.
            if (!bbu)
                sound_use_buffer(snd, (output >> 3) & 1);
            // PA0-PA2: the sound volume
            sound_volume(snd, output & 7);
        }
    } else {
        // BBU PB4/PB5: the ADB state lines ST0/ST1 -> the transceiver.  (PB3,
        // vFDBInt, is its interrupt input.)
        if (bbu && cfg->adb)
            adb_port_b_output(cfg->adb, output);
        // PB0-PB2: RTC enable, clock and data
        rtc_via1_pb_output(cfg->rtc, output);
        // BBU PB6 vH4: the SCSI interrupt mask
        if (bbu && st && cfg->via1)
            compact_scsi_irq_update(cfg);
        // PB7 vSndEnb: 0 = sound enabled
        if (snd)
            sound_enable(snd, (output & 0x80) == 0);
    }
}

// The board's /RESET net: "MC68000, VIA, SWIM, SCC, SCSI, BBU" (Guide 2e,
// Table 14-2; the PAL boards have the IWM where that list says SWIM).  The
// common set covers the VIA, SCC, floppy chip and the 5380.  The VIA comes
// back with every pin an input, so on a PAL board PA4 floats high and the ROM
// overlay is on again; the BBU re-arms its own -- which is what lets
// machine.reset fetch the reset vectors from $0/$4.
static void compact_bus_reset(config_t *cfg) {
    system_reset_common_devices(cfg);
    if (compact_board(cfg)->bbu) {
        m68k_board_overlay_arm(&compact_st(cfg)->overlay);
        // PA4 (vDriveSel) floats too, which selects the lower drive: the VIA's
        // reset drives no output callback, so put the /ENBL1 steering back
        // here rather than wait for the ROM to rewrite the pin.
        if (compact_board(cfg)->drive_select)
            compact_route_enbl(cfg, false);
    } else
        compact_set_rom_overlay(cfg, true);
}

// PAL-board VIA shift-out callback: routes keyboard data to keyboard device
static void compact_via_shift_out(void *context, uint8_t byte) {
    config_t *cfg = (config_t *)context;
    keyboard_input(cfg->keyboard, byte);
}

// VIA IRQ callback: the VIA drives cfg->rt.irq bit 0
static void compact_via_irq(void *context, bool active) {
    compact_update_ipl((config_t *)context, COMPACT_IRQ_VIA, active);
}

// SCC IRQ callback: the SCC drives cfg->rt.irq bit 1
static void compact_scc_irq(void *context, bool active) {
    compact_update_ipl((config_t *)context, COMPACT_IRQ_SCC, active);
}

// ============================================================
// Disk-speed PWM (the 128K and 512K)
// ============================================================

// The sound buffer: 370 words ending $300 below the top of RAM (Guide 2e,
// ch. 2); the low byte of each word is the disk-speed sample.
#define COMPACT_SOUND_FROM_TOP 0x300u
#define COMPACT_SOUND_WORDS    370
// The pulse-width generator counts its 6-bit sample through a 63-state
// shift-register sequence, so a sample's duty is its distance along that
// sequence, not its binary value.  The sequence (next = x >> 1, with bit 5
// fed from bit 0 XOR bit 1) and its zero point $0B (the sample the 64 KB ROM
// writes for its highest speed value) are read from the ROM's own encoder.
#define COMPACT_PWM_ORIGIN 0x0B
#define COMPACT_PWM_STATES 63
// The single-sided drive's speed law: duty position 0 spins it at 700 rpm and
// each of the 40 positions below that takes 10 rpm off.  Not a documented
// figure -- the drive's response is in no Apple document in evidence -- but a
// linear law spanning the five 394-590 rpm zones, which is all the ROM's
// calibration (it measures /TACH at two settings and interpolates) assumes.
#define COMPACT_PWM_RPM_TOP  700.0
#define COMPACT_PWM_RPM_STEP 10.0

// Steps from COMPACT_PWM_ORIGIN to each 6-bit sample (-1: not in the sequence)
static int compact_pwm_position(uint8_t sample) {
    static int8_t pos[64];
    static bool built = false;
    if (!built) {
        memset(pos, -1, sizeof pos);
        uint8_t x = COMPACT_PWM_ORIGIN;
        for (int i = 0; i < COMPACT_PWM_STATES; i++) {
            if (pos[x] < 0)
                pos[x] = (int8_t)i;
            x = (uint8_t)((x >> 1) | (((x ^ (x >> 1)) & 1) << 5)); // the shift-register step
        }
        built = true;
    }
    return pos[sample & 0x3F];
}

// The spindle speed the disk-speed samples in the sound buffer command: the
// mean duty position over the buffer, through the drive's speed law.
static double compact_pwm_rpm(config_t *cfg) {
    uint32_t base = cfg->ram_size - COMPACT_SOUND_FROM_TOP;
    const uint8_t *buf = ram_native_pointer(cfg->memory_map, base);
    if (!buf)
        return 0;
    int sum = 0, n = 0;
    for (int i = 0; i < COMPACT_SOUND_WORDS; i++) {
        int pos = compact_pwm_position(buf[2 * i + 1]); // the word's low byte
        if (pos >= 0) {
            sum += pos;
            n++;
        }
    }
    if (n == 0)
        return 0;
    return COMPACT_PWM_RPM_TOP - COMPACT_PWM_RPM_STEP * ((double)sum / n);
}

// ============================================================
// VBL trigger
// ============================================================

// Trigger the vertical blanking interval.
static void compact_trigger_vbl(config_t *cfg) {
    compact_state_t *st = compact_st(cfg);

    // Assert then deassert VIA CA1 to signal VBL to the CPU
    via_pulse_c(cfg->via1, 0, 0); // VBL: CA1 low then high

    // Advance the sound DMA phase
    if (st && st->sound) {
        sound_vbl(st->sound);
    }

    // A 400K single-sided drive has no speed control of its own: the board's
    // disk-speed PWM sets its spindle, re-read once a frame
    if (compact_board(cfg)->pwm_disk_speed)
        floppy_set_pwm_rpm(cfg->floppy, compact_pwm_rpm(cfg));

    image_tick_all(cfg);
}

// ============================================================
// Machine descriptors
// ============================================================

// The family substrate: every compact model binds it
static const machine_substrate_t compact_substrate = {
    .init = compact_init,
    .bus_reset = compact_bus_reset,
    .teardown = compact_teardown,
    .seed = mac_seed_rtc_pram,
    .trigger_vbl = compact_trigger_vbl,
    MAC_SUBSTRATE_HOST_IO,
    MAC_SUBSTRATE_MEDIA_STD,
    .display = compact_display,
};

// The built-in 9" screen: one monitor, nothing to choose.
static bool compact_monitor_at(size_t i, const char **id, const char **monitor) {
    if (i != 0)
        return false;
    *id = "compact_9in";
    *monitor = "compact_9in";
    return true;
}

static const builtin_video_desc_t compact_builtin_video = {
    .detail = "512 \xc3\x97 342, 1-bit",
    .monitor_at = compact_monitor_at,
    .default_monitor = "compact_9in",
};

// --- Board descriptors -----------------------------------------------------

// 128K, 512K: PALs, IWM, no SCSI, the 400K drive on the disk-speed PWM
static const compact_board_t compact_board_pal = {
    .bbu = false, .has_scsi = false, .floppy_chip = FLOPPY_TYPE_IWM, .pwm_disk_speed = true};
// 512K enhanced: the same board with the Plus's 800K drive, which regulates
// its own speed
static const compact_board_t compact_board_512ke = {.bbu = false, .has_scsi = false, .floppy_chip = FLOPPY_TYPE_IWM};
// Plus: PALs, IWM, the 5380
static const compact_board_t compact_board_plus = {.bbu = false, .has_scsi = true, .floppy_chip = FLOPPY_TYPE_IWM};
// SE: BBU, IWM, the 5380, two internal drives
static const compact_board_t compact_board_se = {
    .bbu = true, .has_scsi = true, .floppy_chip = FLOPPY_TYPE_IWM, .drive_select = true};
// SE FDHD: BBU, SWIM, the 5380, two internal drives
static const compact_board_t compact_board_sefdhd = {
    .bbu = true, .has_scsi = true, .floppy_chip = FLOPPY_TYPE_SWIM, .drive_select = true};
// Classic: BBU, SWIM, the 5380, one internal drive and the external port
static const compact_board_t compact_board_classic = {.bbu = true, .has_scsi = true, .floppy_chip = FLOPPY_TYPE_SWIM};

// --- Shared configuration tables ---------------------------------------------

// The SIMM machines (Plus, SE) and the Classic's on-board 1 MB plus its
// expansion card: 1, 2, 2.5 and 4 MB -- 2.5 MB historically valid on all.
static const uint32_t compact_ram_options_simm_kb[] = {1024, 2048, 2560, 4096, 0};

// The compact SCSI port: no internal bay on the Plus (its one SCSI bus is the
// DB-25 on the back, so every device is "External"), an internal bay on the
// SE and Classic.  The Plus ROM keeps no startup-device record in parameter
// memory, so its bus cannot hold the startup device (no "Start up from"); the
// SE and Classic ROMs do (GetDefaultStartup, Inside Macintosh, vol. V,
// ch. 19), and their bus is mac_storage_scsi_hd_bay's.
static const storage_bus_decl_t plus_storage[] = {
    MAC_SCSI_BUS_STARTUP("scsi", "SCSI", MEDIA_BUS_SCSI, NULL, true, false),
    {0},
};

// --- Profile fields every compact model shares --------------------------------

// The 7.8336 MHz MC68000 (the Plus's actual clock), no MMU, a 24-bit address
// space, the SCC's AppleTalk port, the built-in 9" screen, the Plus's PRAM
// defaults and this substrate.  Only fields no model overrides belong here
// (an override would trip -Woverride-init).
#define COMPACT_PROFILE_COMMON                                                                                         \
    .cpu_model = 68000, .freq = 7833600, .mmu_kind = MMU_NONE, .address_bits = 24, .appletalk = true,                  \
    .builtin_video = &compact_builtin_video, .pram = &pram_defaults_plus, .substrate = &compact_substrate

// The SCSI models' default devices: a hard disk at ID 0 and an external
// CD-ROM drive at ID 3 -- Apple's CD-ROM driver runs on the Plus.  The bus
// declaration itself (.storage) differs between the Plus and the BBU boards.
#define COMPACT_SCSI_COMMON                                                                                            \
    .default_storage = mac_default_storage_hd0_cd3, .cdrom_drive = &scsi_cd_drive_applecd_sc_plus

// --- Macintosh Plus ----------------------------------------------------------

// The external drive was an option, not factory equipment; a stock Plus here
// keeps one, as it always has, and the user may remove it.
static const floppy_slot_t plus_floppy_slots[] = MAC_FLOPPY_PAIR(FLOPPY_800K, "External floppy drive", false);

const hw_profile_t machine_plus = {
    .name = "Macintosh Plus",
    .id = "plus",
    COMPACT_PROFILE_COMMON,
    .ram_default = 0x400000, // 4 MB
    .ram_max = 0x400000, // 4 MB max
    .rom_size = 0x020000, // 128 KB
    .ram_options = compact_ram_options_simm_kb,
    .floppy_slots = plus_floppy_slots,
    .storage = plus_storage,
    COMPACT_SCSI_COMMON,
    .board = &compact_board_plus, // single VIA, no ADB, no NuBus
};

// --- Macintosh 128K and 512K -------------------------------------------------

// Soldered DRAM: one size each (Guide 2e, ch. 5).
static const uint32_t mac128k_ram_options_kb[] = {128, 0};
static const uint32_t mac512k_ram_options_kb[] = {512, 0};

// The single-sided 400K drive inside, and the optional external one.
static const floppy_slot_t mac400k_floppy_slots[] = MAC_FLOPPY_PAIR(FLOPPY_400K, "External floppy drive", true);

// No SCSI on the 128K, 512K and 512K enhanced: no storage bus, no CD-ROM drive
const hw_profile_t machine_mac128k = {
    .name = "Macintosh 128K",
    .id = "mac128k",
    COMPACT_PROFILE_COMMON,
    .ram_default = 0x020000, // 128 KB
    .ram_max = 0x020000,
    .rom_size = 0x010000, // 64 KB
    .ram_options = mac128k_ram_options_kb,
    .floppy_slots = mac400k_floppy_slots,
    .board = &compact_board_pal,
};

const hw_profile_t machine_mac512k = {
    .name = "Macintosh 512K",
    .id = "mac512k",
    COMPACT_PROFILE_COMMON,
    .ram_default = 0x080000, // 512 KB
    .ram_max = 0x080000,
    .rom_size = 0x010000, // 64 KB
    .ram_options = mac512k_ram_options_kb,
    .floppy_slots = mac400k_floppy_slots,
    .board = &compact_board_pal,
};

// --- Macintosh 512K enhanced -------------------------------------------------

// "The ROM and disk drive in the Macintosh 512K enhanced computer are the same
// as those used in the Macintosh Plus" (Guide 2e, ch. 1): the 800K drive.
// Unlike the Plus's, the external position ships empty.
static const floppy_slot_t mac512ke_floppy_slots[] = MAC_FLOPPY_PAIR(FLOPPY_800K, "External floppy drive", true);

const hw_profile_t machine_mac512ke = {
    .name = "Macintosh 512K enhanced",
    .id = "mac512ke",
    COMPACT_PROFILE_COMMON,
    .ram_default = 0x080000, // 512 KB
    .ram_max = 0x080000,
    .rom_size = 0x020000, // 128 KB, the Plus ROM
    .ram_options = mac512k_ram_options_kb,
    .floppy_slots = mac512ke_floppy_slots,
    .board = &compact_board_512ke,
};

// --- Macintosh SE, SE FDHD and Classic --------------------------------------

// The SE's two internal 800K drives, lower and upper (the upper one optional:
// the other factory configuration put a hard disk there).
static const floppy_slot_t se_floppy_slots[] = MAC_FLOPPY_PAIR(FLOPPY_800K, "Second internal floppy drive", false);

// The SE FDHD's: the same pair of SuperDrives.  (mac_floppy_slots_2int is
// this table with the second bay shipping empty, as on the IIx and IIfx.)
static const floppy_slot_t sefdhd_floppy_slots[] = MAC_FLOPPY_PAIR(FLOPPY_HD, "Second internal floppy drive", false);

const hw_profile_t machine_se = {
    .name = "Macintosh SE",
    .id = "se",
    COMPACT_PROFILE_COMMON,
    .ram_default = 0x400000, // 4 MB
    .ram_max = 0x400000,
    .rom_size = 0x040000, // 256 KB
    .ram_options = compact_ram_options_simm_kb,
    .floppy_slots = se_floppy_slots,
    .storage = mac_storage_scsi_hd_bay,
    COMPACT_SCSI_COMMON,
    .board = &compact_board_se,
};

const hw_profile_t machine_sefdhd = {
    .name = "Macintosh SE FDHD",
    .id = "sefdhd",
    COMPACT_PROFILE_COMMON,
    .ram_default = 0x400000, // 4 MB
    .ram_max = 0x400000,
    .rom_size = 0x040000, // 256 KB
    .ram_options = compact_ram_options_simm_kb,
    .floppy_slots = sefdhd_floppy_slots,
    .storage = mac_storage_scsi_hd_bay,
    COMPACT_SCSI_COMMON,
    .board = &compact_board_sefdhd,
};

const hw_profile_t machine_classic = {
    .name = "Macintosh Classic",
    .id = "classic",
    COMPACT_PROFILE_COMMON,
    .ram_default = 0x400000, // 4 MB
    .ram_max = 0x400000,
    .rom_size = 0x080000, // 512 KB, the ROM disk in its upper half
    .ram_options = compact_ram_options_simm_kb,
    .floppy_slots = mac_floppy_slots_ext,
    .storage = mac_storage_scsi_hd_bay,
    COMPACT_SCSI_COMMON,
    .board = &compact_board_classic,
};
