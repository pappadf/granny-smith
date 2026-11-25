// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// m68k_board.h
// The board runtime every 68k II/Quadra family shares (GLUE, MDU, OSS, MCU,
// AV): the board descriptor, the core and low-speed builders, the PMMU and
// 040 bus-MMU builders, RAM-bank mirroring, the IRQ→IPL routing, finish and
// bus reset.  The descriptor face of the I/O engine is m68k_board_io.h; the
// ROM overlay is m68k_board_overlay.h; the GLUE family itself is glue/glue.h.

#ifndef GS_MACHINES_RUNTIME_M68K_BOARD_H
#define GS_MACHINES_RUNTIME_M68K_BOARD_H

#include "asc.h" // asc_mix_t (board speaker fold)
#include "checkpoint.h"
#include "m68k_board_io.h"
#include "m68k_board_overlay.h"
#include "machine_irq.h" // machine_irq_route_t
#include "memory.h" // memory_interface_t
#include "page_fill.h" // page_fill_68k / page_clear, the family's page fills
#include "system_internal.h"

#include <stdbool.h>
#include <stdint.h>

struct mmu_state;

// The II-family construction prefix shared by every 68k board: build the
// memory map, the CPU (model read FROM THE PROFILE — closing the drift
// where every init hardcoded CPU_MODEL_68030), the scheduler, and its
// frequency/CPI.  `desc` supplies the board's bus-error window, which the
// memory map takes at construction.  The caller continues with any
// machine-specific scheduler event types, IRQ-state restore, and device
// construction.
struct m68k_board_desc;
void m68k_board_build_core(config_t *cfg, const struct m68k_board_desc *desc, checkpoint_t *cp);

// Shared IRQ callbacks: route the SCC / VIA1 / VIA2 interrupt line to the
// CPU IPL via m68k_board_update_ipl.  Identical across se30/iicx/iix, and
// taken by the MDU and MCU for the lines they route the same way.
void m68k_board_scc_irq(void *context, bool active);
void m68k_board_via1_irq(void *context, bool active);
void m68k_board_via2_irq(void *context, bool active);

// A page-filler for the mirroring helper below (page_fill_68k, page_fill.h).
typedef void (*m68k_board_fill_fn)(uint32_t page_index, uint8_t *host_ptr, bool writable);

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
void m68k_board_map_mirrored(uint32_t start_page, uint32_t window_pages, uint8_t *host, uint32_t size_pages,
                             m68k_board_fill_fn fill, bool writable);

// One DRAM bank as a memory controller places it.
typedef struct m68k_board_bank {
    uint32_t start; // the physical address the controller decodes it from
    uint32_t size; // installed bytes (0 = an empty bank)
    uint32_t image_off; // where its bytes live in the flat RAM image
} m68k_board_bank_t;

// Apply a memory controller's bank placement to the page table.  A populated
// bank decodes from its start up to whichever populated bank starts next
// above it, or `window` bytes if none does, and mirrors its size through
// that span -- the boot ROMs size a bank by parking it on a window boundary
// and finding where its image repeats.  `span` (0: none) caps every bank's
// decode at the controller's reach.
//
// `extent` (NULL: keep whatever is mapped) is the caller's tracked reach of
// the previous mapping: the pages below it are cleared first, so a bank that
// moves (the ROMs park the banks, size them, then stitch them contiguous)
// leaves no stale image, and it is updated to this mapping's reach.  Seed it
// with the controller's whole reach whenever anything else may have filled
// low pages (construction, the overlay arming).  `bus_mmu` (NULL: none) has
// its TLB flushed afterwards.
void m68k_board_map_banks(config_t *cfg, const m68k_board_bank_t *banks, int count, uint32_t window, uint32_t span,
                          uint32_t *extent, struct mmu_state *bus_mmu);

// Hardware RESET (the board's /RESET net): re-enable the ROM overlay and reset
// the devices every board shares.  The 68030 PMMU is inside the CPU, not on the
// net, so it is left to cpu_hardware_reset.  `overlay` is the machine's
// line-following ROM overlay.
void m68k_board_bus_reset(config_t *cfg, m68k_board_overlay_t *overlay);

// (m68k_board_build_mmu — the board-parameterised PMMU builder — is declared below,
// next to m68k_board_desc_t which carries the ROM window it uses.)

// Finish init: validate the family's I/O table against the devices it bound,
// create the debugger, start the scheduler, and zero the IRQ/IPL on a cold
// boot (left intact on checkpoint restore).  `io` is the family's engine (NULL
// only for a family that has none).
void m68k_board_finish(config_t *cfg, checkpoint_t *cp, const board_io_t *io);

struct nubus_slot_decl;

// The 68k board descriptor: the per-machine HARDWARE DATA,
// chipset-neutral, instantiated by EVERY II-family machine (GLUE se30/iicx/iix,
// MDU+RBV iici/iisi, OSS+FMC iifx) and consumed at init time by the shared
// helpers — m68k_board_build_mmu (ROM window), the family I/O bind (io_ranges +
// mirror + unmapped), nubus_init (slots) and the bus-error range.  Pure data;
// the family-specific device construction stays a code path (see each init).
// The field set every 68k board descriptor shares, and that the shared code
// reads: m68k_board_io_install takes io_ranges / io_mirror_mask / io_unmapped_read,
// m68k_board_build_mmu takes rom_base / rom_end, and m68k_board_build_core passes the
// bus-error pair to memory_map_init.  The MCU and AV descriptors EMBED this rather
// than redeclaring the fields -- which is what lets
// mcu_io_bind and av_io_bind hand &desc->common to m68k_board_io_install
// instead of each carrying its own copy of the same four assignments.
//
// Keeping it in one struct also keeps its documentation in one place.  The
// io_unmapped_read note below is load-bearing and was previously written out
// here while the other two descriptors said only "unmapped-read value inside
// the island", so a reader of mcu.h or av.h had no way to know the field
// mattered.
typedef struct m68k_board_desc {
    const char *chipset; // "GLUE" / "MDU+RBV" / "OSS+FMC" (tracing/diagnostics)
    uint32_t rom_base, rom_end; // MMU ROM window (GLUE $40000000-$50000000; MDU/OSS differ)
    const board_io_range_t *io_ranges; // the shared engine's ordered window table
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
} m68k_board_desc_t;

// Build the low-speed spine every 68k family shares: RTC, the SCC at the Mac's
// clocks (7.8336 MHz PCLK / 3.6864 MHz RTxC), and the machine's connection to
// the AppleTalk network on its LocalTalk channel.  Pass NULL for `scc_irq` to take the family
// default (m68k_board_scc_irq).
//
// Each is a checkpoint part, registered as it is built (machine_parts.h).
//
// The VIAs are deliberately NOT here: one machine or two, different port
// hooks, different IRQ sinks, and the GLUE machines' exact 20:1 clock ratio
// versus the derived factor everyone else needs.  That variation is real, and
// a parameter list long enough to absorb it would be longer than the code.
void m68k_board_build_lowspeed(config_t *cfg, checkpoint_t *cp, void (*scc_irq)(void *, bool));

// Create the 68030 PMMU over a board's ROM window, make it the global MMU and
// attach it to the CPU.  Returns the MMU; the caller sets any TT registers.
struct mmu_state *m68k_board_build_mmu(config_t *cfg, uint32_t rom_base, uint32_t rom_end);

// The 68040 boards' (MCU, AV) bus-side physical resolver: RAM at 0 decoded up
// to `ram_aperture` (probes above installed memory read $FF rather than
// bus-erroring), the ROM mirrored through [rom_base, rom_end).  Makes it the
// map's PMMU and attaches the CPU-owned 040 register file, so translation
// dispatches to the mmu040 walker.  NULL (logged) when out of memory.
struct mmu_state *m68k_board_build_bus_mmu040(config_t *cfg, uint32_t ram_aperture, uint32_t rom_base,
                                              uint32_t rom_end);

// IRQ source bits driven into cfg->rt.irq.  GLUE routes them to fixed IPLs:
// VIA1→1, VIA2→2, SCC→4, NMI→7.  The one set for the whole family (the
// per-machine SE30_IRQ_* / IICX_IRQ_* aliases are gone).
#define M68K_BOARD_IRQ_VIA1 (1 << 0)
#define M68K_BOARD_IRQ_VIA2 (1 << 1)
#define M68K_BOARD_IRQ_SCC  (1 << 2)
#define M68K_BOARD_IRQ_NMI  (1 << 3)

// The GLUE family's IRQ routing table (machine_irq.h; sentinel-terminated),
// shared by the MDU and MCU, whose level-2 source is the RBV / VIA2.
// Exposed for the IRQ-routing unit test.
const machine_irq_route_t *m68k_board_irq_routes(void);

// Set/clear an IRQ source bit and re-derive the CPU IPL (highest active wins).
void m68k_board_update_ipl(config_t *cfg, int source, bool active);

// TT1 as the 68030 boards set it (GLUE, the IIsi, the IIfx): a supervisor-
// only transparent translation of $F0000000-$FFFFFFFF (NuBus slot space), so
// slot accesses bypass the page tables.  The ROMs never write TT1
// themselves; the chipset routes $F0-$FF to NuBus and this identity map lets
// phys_to_host resolve card memory.
#define M68K_BOARD_TT1_NUBUS_SUPER 0xF00F8043u

#endif // GS_MACHINES_RUNTIME_M68K_BOARD_H
