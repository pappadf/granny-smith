// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mac030_glue.h
// Shared GLUE-family lifecycle leaves: the IRQ→IPL routing and the teardown
// delete-chain, byte-identical across SE/30, IIcx and IIx (proposal §1.1,
// "teardown chain (~275)").  The I/O dispatcher lives in mac030_glue_io.h.

#ifndef GS_MACHINES_MAC030_GLUE_H
#define GS_MACHINES_MAC030_GLUE_H

#include "checkpoint.h"
#include "mac030_glue_io.h"
#include "memory.h" // memory_interface_t
#include "system_config.h"

#include <stdbool.h>
#include <stdint.h>

struct adb;
struct asc;
struct floppy;
struct mmu_state;
struct nubus_card;
struct via;

// Unified GLUE-family machine state — the single struct shared by SE/30,
// IIcx and IIx (collapsing the former se30_state_t and iicx_state_t; proposal
// §4.2.3 "the three _internal.h clone headers collapse into one substrate
// state struct").  It is a superset: the SE/30 uses vram/vrom/video_card and
// leaves the IIcx soft-power fields unused; the IIcx/IIx use the soft-power
// fields and leave the video pointers NULL.
typedef struct mac030_glue_state {
    struct adb *adb;
    struct asc *asc;
    struct floppy *floppy;

    bool rom_overlay; // true = ROM mapped at $00000000
    struct mmu_state *mmu; // 68030 PMMU

    mac030_glue_io_t glue_io; // device handles for the shared dispatcher

    uint8_t last_port_b; // VIA1 PB output, for ADB ST-transition filtering
    uint8_t last_via2_port_b; // IIcx soft-power detect (unused on se30/iix)
    bool soft_power_armed; // IIcx soft-power detect (unused on se30/iix)

    // SE/30 built-in video (slot $E); NULL on IIcx/IIx (they use NuBus cards).
    uint8_t *vram;
    uint8_t *vrom;
    struct nubus_card *video_card;

    memory_interface_t io_interface; // registered at the $50000000 I/O region
} mac030_glue_state_t;

// The II-family construction prefix shared by every GLUE machine: build the
// memory map, the CPU (model read FROM THE PROFILE — closing the §1.3 drift
// where every init hardcoded CPU_MODEL_68030), the scheduler, and its
// frequency/CPI.  The caller continues with any machine-specific scheduler
// event types, IRQ-state restore, and device construction.
void mac030_build_core(config_t *cfg, checkpoint_t *cp);

// Shared GLUE IRQ callbacks: route the SCC / VIA1 / VIA2 interrupt line to
// the CPU IPL via mac030_glue_update_ipl.  Identical across se30/iicx/iix.
void mac030_glue_scc_irq(void *context, bool active);
void mac030_glue_via1_irq(void *context, bool active);
void mac030_glue_via2_irq(void *context, bool active);

// Populate the AoS page entry + SoA fast-path arrays for one page.  Pure
// (touches only the global page table), identical across se30/iicx/iici/iisi.
// (iifx keeps its own variant, which additionally zeroes the write entries
// of read-only pages.)
void mac030_fill_page(uint32_t page_index, uint8_t *host_ptr, bool writable);

// Toggle the ROM/RAM overlay at $00000000.  `overlay_flag` points at the
// machine's own rom_overlay bool; `rom_start` is the machine's ROM region
// base (GLUE $40000000, MDU $40800000).
void mac030_glue_set_rom_overlay(config_t *cfg, bool *overlay_flag, uint32_t rom_start, bool on);

// Hardware RESET: re-enable the ROM overlay and disable the MMU (TC/E off,
// TLB flushed).  `overlay_flag` points at the machine's rom_overlay bool;
// `rom_start` is the ROM region base; `mmu` may be NULL.
void mac030_glue_reset(config_t *cfg, bool *overlay_flag, uint32_t rom_start, struct mmu_state *mmu);

// Construct the GLUE peripheral set shared by se30/iicx/iix in canonical
// order: ADB, (checkpoint image restore), SCSI (+VIA2), images, ASC (+VIA2),
// SWIM floppy, then bind the I/O dispatcher.  Stores the device handles in
// the unified state and on config_t.  Call after VIA1/VIA2 exist.
struct mac030_board_desc;
void mac030_glue_build_peripherals(config_t *cfg, checkpoint_t *cp, mac030_glue_state_t *st,
                                   const struct mac030_board_desc *desc);

// (mac030_build_mmu — the board-parameterised PMMU builder — is declared below,
// next to mac030_board_desc_t which carries the ROM window it uses.)

// Finish init: create the debugger, start the scheduler, and zero the IRQ/IPL
// on a cold boot (left intact on checkpoint restore).
void mac030_glue_finish(config_t *cfg, checkpoint_t *cp);

struct nubus_slot_decl;

// The mac030 board descriptor (proposal §4.2.2): the per-machine HARDWARE DATA,
// chipset-neutral, instantiated by EVERY II-family machine (GLUE se30/iicx/iix,
// MDU+RBV iici/iisi, OSS+FMC iifx) and consumed at init time by the shared
// helpers — mac030_build_mmu (ROM window), the family I/O bind (io_ranges +
// mirror + unmapped), nubus_init (slots) and the bus-error range.  Pure data;
// the family-specific device construction stays a code path (see each init).
typedef struct mac030_board_desc {
    const char *chipset; // "GLUE" / "MDU+RBV" / "OSS+FMC" (tracing/diagnostics)
    uint32_t rom_base, rom_end; // MMU ROM window (GLUE $40000000-$50000000; MDU/OSS differ)
    const mac030_io_range_t *io_ranges; // the shared engine's ordered window table
    uint32_t io_mirror_mask; // I/O island mirror mask (GLUE $1FFFF; MDU/OSS $3FFFF)
    uint8_t io_unmapped_read; // value returned on an unmapped read (0 GLUE/MDU; 0xFF OSS)
    const struct nubus_slot_decl *slots; // NuBus slot table
    uint32_t bus_err_lo, bus_err_hi; // unmapped-region bus-error window
} mac030_board_desc_t;

// Create the 68030 PMMU over a board's ROM window, make it the global MMU and
// attach it to the CPU.  Returns the MMU; the caller sets any TT registers.
struct mmu_state *mac030_build_mmu(config_t *cfg, uint32_t rom_base, uint32_t rom_end);

// A GLUE machine = its board descriptor (data) + a few hooks (proposal §4.2.2).
// The shared mac030_glue_init() walks this in canonical order; the per-machine
// deltas are the VIA output/shift callbacks, the machine-ID strap sequence, the
// memory layout, and (SE/30 only) the built-in-video wiring.  NULL hooks are
// skipped.
typedef struct mac030_glue_board {
    const mac030_board_desc_t *desc; // hardware data (rom window, io map, slots, bus-error)

    void (*via1_output)(void *context, uint8_t port, uint8_t value);
    void (*via1_shift_out)(void *context, uint8_t byte);
    void (*via2_output)(void *context, uint8_t port, uint8_t value);
    void (*via2_shift_out)(void *context, uint8_t byte);

    void (*setup_id)(config_t *cfg); // machine-ID strap + VIA2 idle lines

    void (*memory_layout)(config_t *cfg); // RAM/ROM/IO page-table setup + overlay

    void (*pre_devices)(config_t *cfg); // optional: before device construction (SE/30 VBL event type)
    void (*post_nubus)(config_t *cfg); // optional: after nubus_init (SE/30 VRAM/VROM wiring)
    void (*ckpt_restore_extra)(config_t *cfg, checkpoint_t *cp); // optional: extra restore (SE/30 VRAM/VROM)
    void (*ckpt_save_extra)(config_t *cfg, checkpoint_t *cp); // optional: extra save, symmetric (SE/30 VRAM/VROM)
    void (*trigger_vbl)(config_t *cfg); // optional VBL override; NULL → the default GLUE NuBus VBL
} mac030_glue_board_t;

// The one GLUE-family substrate (proposal §4.2.2): SE/30, IIcx and IIx all bind
// this; their per-machine deltas live entirely in their mac030_glue_board_t
// (named via hw_profile_t.board) and profile.  The lifecycle methods read the
// board from cfg->machine->board.
extern const machine_substrate_t glue_substrate;

// The shared GLUE init: allocates the unified state, builds the II-family
// core + RTC/SCC/VIA1/VIA2 + peripherals + PMMU + NuBus in canonical order,
// applies the board's deltas via its hooks, and finishes.  A GLUE machine's
// substrate.init is a one-liner that calls this with its board.
void mac030_glue_init(config_t *cfg, checkpoint_t *cp, const mac030_glue_board_t *board);

// IRQ source bits driven into cfg->irq.  GLUE routes them to fixed IPLs:
// VIA1→1, VIA2→2, SCC→4, NMI→7.  The per-machine SE30_IRQ_* / IICX_IRQ_*
// aliases carry the same values and remain valid `source` arguments.
#define MAC030_GLUE_IRQ_VIA1 (1 << 0)
#define MAC030_GLUE_IRQ_VIA2 (1 << 1)
#define MAC030_GLUE_IRQ_SCC  (1 << 2)
#define MAC030_GLUE_IRQ_NMI  (1 << 3)

// One IRQ source → CPU-IPL routing rule (proposal §4.2.2).  A family's routing
// is an ordered table of these, highest IPL first; the resolver returns the
// IPL of the highest-priority currently-active source.  GLUE routes the
// level-2 source through VIA2, MDU through RBV, OSS through the OSS controller
// — three different tables, one resolver.
typedef struct mac030_irq_route {
    int source; // IRQ source bit (MAC030_GLUE_IRQ_*)
    int ipl; // CPU IPL it raises
} mac030_irq_route_t;

// Resolve a set of active IRQ source bits to a CPU IPL by walking `routes`
// (ordered highest-IPL-first, sentinel source == 0) and returning the first
// match — i.e. the highest-priority active source.  0 when none active.  Pure;
// exposed for the IRQ-routing unit test (§6.1).
int mac030_irq_resolve_ipl(const mac030_irq_route_t *routes, uint32_t irq);

// The GLUE family's IRQ routing table (sentinel-terminated).  Exposed for the
// IRQ-routing unit test.
const mac030_irq_route_t *mac030_glue_irq_routes(void);

// Set/clear an IRQ source bit and re-derive the CPU IPL (highest active wins).
void mac030_glue_update_ipl(config_t *cfg, int source, bool active);

// substrate.nubus_slot_irq for the GLUE family: each slot's /NMRQ is a VIA2
// port-A bit (active-low; slot $9→PA0 .. $E→PA5), and the umbrella OR-line edge
// pulses CA1.  (se30/iicx/iix.)
void mac030_glue_nubus_slot_irq(config_t *cfg, int slot, bool active, bool umbrella_edge);

// substrate.nubus_slot_irq for chipsets whose own IRQ controller aggregates the
// slots (MDU's RBV, OSS): route the slot source through the substrate's own
// update_ipl.  umbrella_edge is irrelevant (the controller aggregates
// internally).  (iici/iisi/iifx.)
void mac030_nubus_slot_irq_via_ipl(config_t *cfg, int slot, bool active, bool umbrella_edge);

// Family-shared teardown delete-chain: scheduler_stop → mmu → floppy → asc →
// adb → scsi → via2 → via1 → scc → rtc → scheduler → cpu → mem_map → debugger.
// The machine-owned devices (which live in its private state, not config_t)
// are passed in; the caller frees its own state struct afterwards.  Any NULL
// handle is skipped.
void mac030_glue_teardown(config_t *cfg, struct adb *adb, struct asc *asc, struct floppy *floppy,
                          struct mmu_state *mmu);

#endif // GS_MACHINES_MAC030_GLUE_H
