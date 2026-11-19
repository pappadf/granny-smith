// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mac030_glue.h
// Shared GLUE-family lifecycle leaves: the IRQ→IPL routing and the teardown
// delete-chain, byte-identical across SE/30, IIcx and IIx.  The I/O
// dispatcher lives in mac030_glue_io.h.

#ifndef GS_MACHINES_MAC030_GLUE_H
#define GS_MACHINES_MAC030_GLUE_H

#include "asc.h" // asc_mix_t (board speaker fold)
#include "checkpoint.h"
#include "mac030_glue_io.h"
#include "mac030_rom_overlay.h"
#include "machine_irq.h" // machine_irq_route_t
#include "memory.h" // memory_interface_t
#include "page_fill.h" // page_fill_68k / page_clear, the family's page fills
#include "system_internal.h"

#include <stdbool.h>
#include <stdint.h>

struct adb;
struct asc;
struct floppy;
struct mmu_state;
struct nubus_card;
struct via;

// Unified GLUE-family machine state — the single struct shared by SE/30,
// IIcx and IIx (collapsing the former se30_state_t and iicx_state_t).  It is
// a superset: the SE/30 uses vram/vrom/video_card and
// leaves the IIcx soft-power fields unused; the IIcx/IIx use the soft-power
// fields and leave the video pointers NULL.
typedef struct mac030_glue_state {
    struct asc *asc; // (the adb and floppy are cfg->adb / cfg->floppy)

    mac030_rom_overlay_t overlay; // ROM at $00000000, following VIA1 PA4
    struct mmu_state *mmu; // 68030 PMMU

    mac030_glue_io_t glue_io; // device handles for the shared dispatcher

    // The /SLOTIRQ aggregate this family drives onto VIA2 CA1: the OR of
    // every source, kept by the CHIPSET rather than inferred from the NuBus
    // controller's own view.  Quadra 700/900 developer notes: "The ... NuBus
    // interrupt signals ..., the built-in video interrupt signal, and the
    // Ethernet controller interrupt signal are routed through an OR gate to
    // generate a signal called /SLOTIRQ.  This signal is connected to the CA1
    // input of VIA2."  It is a LEVEL, and it can include sources the bus
    // knows nothing about -- which is why the bus's `umbrella_edge` was the
    // wrong abstraction.  Not saved: a restore derives it from the VIA2 pins
    // (slot_irq.h).
    uint8_t slot_pa_mask;

    bool soft_power_armed; // IIcx soft-power detect (unused on se30/iix)

    // SE/30 built-in video (slot $E); NULL on IIcx/IIx (they use NuBus cards).
    uint8_t *vram;
    uint8_t *vrom;
    struct nubus_card *video_card;

    memory_interface_t io_interface; // registered at the $50000000 I/O region
} mac030_glue_state_t;

// The II-family construction prefix shared by every GLUE machine: build the
// memory map, the CPU (model read FROM THE PROFILE — closing the drift
// where every init hardcoded CPU_MODEL_68030), the scheduler, and its
// frequency/CPI.  `desc` supplies the board's bus-error window, which the
// memory map takes at construction.  The caller continues with any
// machine-specific scheduler event types, IRQ-state restore, and device
// construction.
struct mac030_board_desc;
void mac030_build_core(config_t *cfg, const struct mac030_board_desc *desc, checkpoint_t *cp);

// Shared GLUE IRQ callbacks: route the SCC / VIA1 / VIA2 interrupt line to
// the CPU IPL via mac030_glue_update_ipl.  Identical across se30/iicx/iix.
void mac030_glue_scc_irq(void *context, bool active);
void mac030_glue_via1_irq(void *context, bool active);
void mac030_glue_via2_irq(void *context, bool active);

// A page-filler for the mirroring helper below (page_fill_68k, page_fill.h).
typedef void (*mac030_fill_fn)(uint32_t page_index, uint8_t *host_ptr, bool writable);

// Map `window_pages` pages from `start_page`, mirroring `size_pages` pages of
// host memory throughout — the shape every family's RAM-bank decode has: a
// bank smaller than its decode window repeats, which is how the boot ROMs size
// memory (they probe down from the window top and read where the address
// wraps).
//
// The guard is the point.  Each family computed `size >> MEM_PAGE_SHIFT` and took
// `p % that` with nothing checking it was non-zero, so a bank under 4 KB would
// divide by zero.  Unreachable through machine.boot today, which validates
// against ram_options, but iici_split_ram_banks' fallback is written to accept
// arbitrary totals ("let the ROM's own sizing decide what it thinks of that"),
// and a bank of zero pages decodes nothing rather than being undefined.
void mac030_map_mirrored(uint32_t start_page, uint32_t window_pages, uint8_t *host, uint32_t size_pages,
                         mac030_fill_fn fill, bool writable);

// Hardware RESET (the board's /RESET net): re-enable the ROM overlay and reset
// the devices every board shares.  The 68030 PMMU is inside the CPU, not on the
// net, so it is left to cpu_hardware_reset.  `overlay` is the machine's
// line-following ROM overlay.
void mac030_glue_bus_reset(config_t *cfg, mac030_rom_overlay_t *overlay);

// Construct the GLUE peripheral set shared by se30/iicx/iix in canonical
// order: ADB, (checkpoint image restore), SCSI (+VIA2), images, ASC (+VIA2),
// SWIM floppy, then bind the I/O dispatcher.  Stores the device handles in
// the unified state and on config_t.  Call after VIA1/VIA2 exist.
struct mac030_board_desc;
int mac030_glue_build_peripherals(config_t *cfg, checkpoint_t *cp, mac030_glue_state_t *st,
                                  const struct mac030_board_desc *desc);

// (mac030_build_mmu — the board-parameterised PMMU builder — is declared below,
// next to mac030_board_desc_t which carries the ROM window it uses.)

// Finish init: validate the family's I/O table against the devices it bound,
// create the debugger, start the scheduler, and zero the IRQ/IPL on a cold
// boot (left intact on checkpoint restore).  `io` is the family's engine (NULL
// only for a family that has none).
void mac030_glue_finish(config_t *cfg, checkpoint_t *cp, const mac030_io_t *io);

struct nubus_slot_decl;

// The mac030 board descriptor: the per-machine HARDWARE DATA,
// chipset-neutral, instantiated by EVERY II-family machine (GLUE se30/iicx/iix,
// MDU+RBV iici/iisi, OSS+FMC iifx) and consumed at init time by the shared
// helpers — mac030_build_mmu (ROM window), the family I/O bind (io_ranges +
// mirror + unmapped), nubus_init (slots) and the bus-error range.  Pure data;
// the family-specific device construction stays a code path (see each init).
// The field set every 68k board descriptor shares, and that the shared code
// reads: mac030_io_* takes io_ranges / io_mirror_mask / io_unmapped_read,
// mac030_build_mmu takes rom_base / rom_end, and mac030_build_core passes the
// bus-error pair to memory_map_init.  The MCU and AV descriptors EMBED this rather
// than redeclaring the fields -- which is what lets
// mcu_io_bind and av_io_bind hand &desc->common to mac030_glue_io_bind
// instead of each carrying its own copy of the same four assignments.
//
// Keeping it in one struct also keeps its documentation in one place.  The
// io_unmapped_read note below is load-bearing and was previously written out
// here while the other two descriptors said only "unmapped-read value inside
// the island", so a reader of mcu.h or av.h had no way to know the field
// mattered.
typedef struct mac030_board_desc {
    const char *chipset; // "GLUE" / "MDU+RBV" / "OSS+FMC" (tracing/diagnostics)
    uint32_t rom_base, rom_end; // MMU ROM window (GLUE $40000000-$50000000; MDU/OSS differ)
    const mac030_io_range_t *io_ranges; // the shared engine's ordered window table
    uint32_t io_mirror_mask; // I/O island mirror mask (GLUE $1FFFF; MDU/OSS $3FFFF)
    // Value an undecoded address inside the I/O island reads back.  On real
    // hardware nothing drives the bus there, so pull-ups float it high — the
    // same reasoning memory.c applies to unmapped physical space ("floating
    // bus, pull-up resistors").  0xFF for every machine; a machine that
    // genuinely drives a decoded 0 should model that window explicitly rather
    // than lowering this.
    //
    // This is load-bearing, not cosmetic.  MacTest 2.11's sound test reads the
    // VIA2 IFR at island $3A00 unconditionally — the IIci has no VIA2 there, so
    // the read floats — and checks bit 4.  With a 0 fill the bit read clear and
    // the machine was condemned as a bad logic board.
    uint8_t io_unmapped_read;
    uint32_t bus_err_lo, bus_err_hi; // unmapped-region bus-error window
    // A bus-error window INSIDE the 256 MB I/O window, as offsets from its
    // base (desc->rom_end), tested before the island mirror is applied.  An
    // empty window (lo > hi, or both 0) means the island mirrors over the
    // whole I/O window.  The IIci uses it for its cache-card connector
    // ($5200_0000-$52FF_FFFF): the card decodes that range itself and the
    // board provides no select, so with no card fitted nothing answers and
    // the access times out.  Mirroring the island there made VIA1 answer
    // the cache tag/data probes and MacTest Pro reported a failed cache card.
    uint32_t io_berr_lo, io_berr_hi;
    asc_mix_t asc_mix; // speaker fold of the ASC stereo pair (SE/30 sums; IIx/IIcx take A)
} mac030_board_desc_t;

// The I/O window sits directly above the ROM window and is 256 MB on every
// GLUE machine ($50000000-$5FFFFFFF), so its base is desc->rom_end.
#define MAC030_GLUE_IO_SIZE 0x10000000UL

// The GLUE family's shared memory layout: RAM (with the SIMM address wrap the
// ROM's ram_address_test needs), the ROM window from desc->rom_base/rom_end,
// and the I/O dispatcher.  mac030_glue_init calls this, then the board's
// memory_layout_tail.  One motherboard design, one function.
void mac030_glue_memory_layout(config_t *cfg, const mac030_board_desc_t *desc);

// Build the low-speed spine every 68k family shares: RTC, the SCC at the Mac's
// clocks (3.6864 MHz PCLK / 7.8336 MHz RTxC), and the machine's connection to
// the AppleTalk network on its LocalTalk channel.  Pass NULL for `scc_irq` to take the family
// default (mac030_glue_scc_irq).
//
// Each is a checkpoint part, registered as it is built (machine_parts.h).
//
// The VIAs are deliberately NOT here: one machine or two, different port
// hooks, different IRQ sinks, and the GLUE machines' exact 20:1 clock ratio
// versus the derived factor everyone else needs.  That variation is real, and
// a parameter list long enough to absorb it would be longer than the code.
void mac030_build_lowspeed(config_t *cfg, checkpoint_t *cp, void (*scc_irq)(void *, bool));

// Create the 68030 PMMU over a board's ROM window, make it the global MMU and
// attach it to the CPU.  Returns the MMU; the caller sets any TT registers.
struct mmu_state *mac030_build_mmu(config_t *cfg, uint32_t rom_base, uint32_t rom_end);

// The 68040 boards' (MCU, AV) bus-side physical resolver: RAM at 0 decoded up
// to `ram_aperture` (probes above installed memory read $FF rather than
// bus-erroring), the ROM mirrored through [rom_base, rom_end).  Makes it the
// map's PMMU and attaches the CPU-owned 040 register file, so translation
// dispatches to the mmu040 walker.  NULL (logged) when out of memory.
struct mmu_state *mac030_build_bus_mmu040(config_t *cfg, uint32_t ram_aperture, uint32_t rom_base, uint32_t rom_end);

// A GLUE machine = its board descriptor (data) + a few hooks.
// The shared mac030_glue_init() walks this in canonical order; the per-machine
// deltas are the VIA output/shift callbacks, the machine-ID strap sequence, the
// memory layout, and (SE/30 only) the built-in-video wiring.  NULL hooks are
// skipped.
typedef struct mac030_glue_board {
    const mac030_board_desc_t *desc; // hardware data (rom window, io map, slots, bus-error)

    void (*via1_output)(void *context, uint8_t port, uint8_t value);
    // VIA1 shift-out routing, NULL on every ADB board (here, the MDU's and the
    // MCU's): adb.c reads the VIA's shift register directly at each port-B ST
    // transition, because in mode 7 the ADB transceiver clocks the shift, not
    // the VIA's internal timer, and the ROM's SR writes during interrupt
    // handling fire the callback spuriously (BUG-004).  via.c tolerates NULL.
    void (*via1_shift_out)(void *context, uint8_t byte);
    void (*via2_output)(void *context, uint8_t port, uint8_t value);
    void (*via2_shift_out)(void *context, uint8_t byte);

    void (*setup_id)(config_t *cfg); // machine-ID strap + VIA2 idle lines

    // Per-board remainder of the memory layout, run right after the shared
    // mac030_glue_memory_layout() has done RAM, ROM and the I/O dispatcher:
    // the SE/30 maps its built-in video's VRAM/VROM, the IIcx and IIx fill
    // page entries for NuBus cards' host-backed regions, and both then arm
    // the ROM overlay.  Optional, like the other tail hooks.
    void (*memory_layout_tail)(config_t *cfg);

    void (*pre_devices)(config_t *cfg); // optional: before device construction (SE/30 VBL event type)
    void (*post_nubus)(config_t *cfg); // optional: after nubus_init (SE/30 VRAM/VROM wiring)
    void (*trigger_vbl)(config_t *cfg); // optional VBL override; NULL → the default GLUE NuBus VBL
} mac030_glue_board_t;

// The one GLUE-family substrate: SE/30, IIcx and IIx all bind
// this; their per-machine deltas live entirely in their mac030_glue_board_t
// (named via hw_profile_t.board) and profile.  The lifecycle methods read the
// board from cfg->machine->board.
extern const machine_substrate_t glue_substrate;

// The shared GLUE init: allocates the unified state, builds the II-family
// core + RTC/SCC/VIA1/VIA2 + peripherals + PMMU + NuBus in canonical order,
// applies the board's deltas via its hooks, and finishes.  A GLUE machine's
// substrate.init is a one-liner that calls this with its board.
int mac030_glue_init(config_t *cfg, checkpoint_t *cp, const mac030_glue_board_t *board);

// IRQ source bits driven into cfg->rt.irq.  GLUE routes them to fixed IPLs:
// VIA1→1, VIA2→2, SCC→4, NMI→7.  The one set for the whole family (the
// per-machine SE30_IRQ_* / IICX_IRQ_* aliases are gone).
#define MAC030_GLUE_IRQ_VIA1 (1 << 0)
#define MAC030_GLUE_IRQ_VIA2 (1 << 1)
#define MAC030_GLUE_IRQ_SCC  (1 << 2)
#define MAC030_GLUE_IRQ_NMI  (1 << 3)

// The GLUE family's IRQ routing table (machine_irq.h; sentinel-terminated),
// shared by the MDU and MCU, whose level-2 source is the RBV / VIA2.
// Exposed for the IRQ-routing unit test.
const machine_irq_route_t *mac030_glue_irq_routes(void);

// Set/clear an IRQ source bit and re-derive the CPU IPL (highest active wins).
void mac030_glue_update_ipl(config_t *cfg, int source, bool active);

// The GLUE machines' RAM range: 8 MB stock, 128 MB the most the 68030 boards
// here take (SE/30, IIcx, IIx profiles).
#define MAC030_GLUE_RAM_DEFAULT 0x800000u // 8 MB
#define MAC030_GLUE_RAM_MAX     0x8000000u // 128 MB

// TT1 as the 68030 boards set it (GLUE, the IIsi, the IIfx): a supervisor-
// only transparent translation of $F0000000-$FFFFFFFF (NuBus slot space), so
// slot accesses bypass the page tables.  The ROMs never write TT1
// themselves; the chipset routes $F0-$FF to NuBus and this identity map lets
// phys_to_host resolve card memory.
#define MAC030_TT1_NUBUS_SUPER 0xF00F8043u

// VIA callbacks for a pin set the board does not observe: VIA2 output on
// the SE/30 and IIx, VIA2 shift-out on all three (via_init takes a function,
// and a NULL shift-out changes mode-7 timing -- see via.c).
void mac030_glue_via_output_ignored(void *context, uint8_t port, uint8_t output);
void mac030_glue_via_shift_out_ignored(void *context, uint8_t byte);

// substrate.nubus_slot_irq for the GLUE family: each slot's /NMRQ is a VIA2
// port-A bit (active-low; slot $9→PA0 .. $E→PA5), and the umbrella OR-line edge
// pulses CA1.  (se30/iicx/iix.)
// One source of the family /SLOTIRQ aggregate on VIA2 port A changes state.
// pa_bit 0-5 are NuBus slots $9-$E; 6 is available for a built-in source the
// bus knows nothing about (the SE/30's video, the MCU's DAFB).
// One 60.15 Hz VBL pulse on a VIA's CA1 line.
struct via;
void mac_vbl_pulse(struct via *via);

void mac030_glue_slot_irq_source(config_t *cfg, int pa_bit, bool active);

void mac030_glue_nubus_slot_irq(config_t *cfg, int slot, bool active);

#endif // GS_MACHINES_MAC030_GLUE_H
