// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// memory.c
// Memory map management and RAM/ROM access for Granny Smith.

// ============================================================================
// Includes
// ============================================================================

#include "memory.h"
#include "gs_out.h"
#include "lisa_mmu.h"
#include "memory_class.h"
#include "mmu.h"

#include "addr_format.h"
#include "checkpoint.h"
#include "common.h"
#include "cpu.h"
#include "debug.h"
#include "gs_assert.h"
#include "log.h"
#include "object.h"
#include "platform.h"
#include "rom.h"

// The category memory logpoints already use (AGENTS.md); log.set memory N.
LOG_USE_CATEGORY_NAME("memory");

// === Bus-error window ======================================================
//
// The window is a field of the memory map (bus_err), passed to memory_map_init
// by the board.  The slow path that consults it runs on the fast-path aliases
// and holds no memory_map_t, so the selected map's window is aliased here,
// with the other aliases, by memory_map_select.
// The test lives in memory.c rather than in each MMU because it is a property
// of the BUS: it applies with the MMU off (most of POST) as well as on the
// MMUs' transparent-translation paths.
static uint32_t g_bus_err_lo = 1; // lo > hi: an empty window while no map is installed
static uint32_t g_bus_err_hi = 0;

// True when an unanswered access at `addr` should fault rather than float.
bool memory_addr_faults_when_unmapped(uint32_t addr) {
    return addr >= g_bus_err_lo && addr <= g_bus_err_hi;
}

#include "shell.h"
#include "system.h"
#include "system_internal.h"
#include "value.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Constants and Macros
// ============================================================================

// Page table globals (defined here, declared extern in memory.h).
//
// One map is active at a time, by design: the fast path reads these globals
// rather than taking a memory_map_t*, because threading a context pointer
// through every guest memory access would cost the hot path for a use (two
// maps live at once) nothing needs.  Each map owns its arrays; these alias
// the selected one (memory_map_select), so building or inspecting a second
// machine means selecting its map, not sharing state.
page_entry_t *g_page_table = NULL;
uint32_t g_address_mask = 0; // set by memory_map_init; 0 here means "pre-init, do not use"
uint32_t g_page_count = 0; // number of pages in current page table

// SoA fast-path arrays (adjusted-base entries; zero = slow path)
uintptr_t *g_supervisor_read = NULL;
uintptr_t *g_supervisor_write = NULL;
uintptr_t *g_user_read = NULL;
uintptr_t *g_user_write = NULL;

// Active pointers (switched per sprint based on SR.S bit)
uintptr_t *g_active_read = NULL;
uintptr_t *g_active_write = NULL;

// Deferred bus error signal: set by slow paths on unmapped MMU accesses.
// Zeroing *g_bus_error_instr_ptr forces the decoder loop to exit early.
bool g_bus_error_pending = false;
uint32_t g_bus_error_address = 0;
bool g_bus_error_rw = false;
// FC (SSW[2:0]) of the faulting access, set with g_bus_error_pending; see
// docs/internals/core/memory/memory.md ("The faulting function code").
uint32_t g_bus_error_fc = 5;
// 1 if the fault came from a PMMU descriptor failure (invalid/perm/write-
// protect) and the handler is expected to fix-up the PTE and retry; 0 if the
// fault was a plain bus timeout (unmapped physical in a NuBus slot range)
// where the handler expects skip-instruction semantics (e.g. Mac ROM RAM
// and slot probes).  Selects Format $B vs Format $A dispatch.
bool g_bus_error_is_pmmu = false;
uint32_t *g_bus_error_instr_ptr = NULL;
// Physical page-fill hook for machines whose page table is NOT owned by a
// 68k mmu_state_t (the PowerPC families).  memory_map_host_region() routes
// card-registered host regions through it so their pages land in the
// machine's own physical view.  NULL on 68k machines, where g_mmu owns the
// host-region list and its fill walk.
void (*g_mem_host_fill)(uint32_t page_index, uint8_t *host_ptr, bool writable) = NULL;

// I/O cycle penalty state: tracks extra bus wait-state cycles for I/O accesses.
// Penalty cycles are converted to "phantom instructions" that consume sprint
// burndown slots, causing sprints with I/O to end sooner and keeping event
// timing accurate.
uint32_t g_io_penalty_remainder = 0; // sprint-time alias of the scheduler's io_penalty_remainder (x256 cycles)
uint64_t g_sprint_base_cycles = 0; // scheduler cpu_cycles at sprint start
uint32_t g_sprint_frac_x256 = 0; // sub-cycle remainder at sprint start (x256)
uint32_t g_sprint_total_slots = 0; // sprint slot budget at sprint start
uint32_t g_esync_period_x256 = 0; // E period in CPU cycles x256 (0 = unset)
uint32_t g_io_phantom_instructions = 0; // phantom instructions consumed this sprint
uint32_t g_sprint_unrun_slots = 0;
uint32_t g_io_stall_owed = 0; // stall slots owed past the sprint's end
uint32_t g_io_cpi_x256 = 0; // effective CPI for penalty conversion, x256 (0 = disabled)
uint32_t *g_sprint_burndown_ptr = NULL; // points to scheduler's sprint_burndown during sprint

// Memory logpoint support: non-zero entries force the page through the slow
// path even when the underlying page is plain RAM/ROM.  See memory.h.
// Per-page logpoint refcounts.  uint16_t, not uint8_t: the install side
// saturated at 255 while the uninstall side decremented unconditionally, so
// the counter stopped being a refcount the moment it saturated.  With 300
// logpoints on one page, installs 256..300 did not increment, removing 255 of
// them drove the count to 0, rebuild_soa_page() restored the direct mapping,
// and the 45 SURVIVING logpoints on that page silently stopped firing.
//
// An extreme configuration, but a silent wrong answer in a debugger is the
// worst failure mode a debugging tool has -- it makes you conclude the guest
// never touched the address.  One extra byte per 4 KB of address space.
uint16_t *g_mem_logpoint_page_count = NULL;
uint16_t *g_mem_logpoint_phys_page_count = NULL;
// Alias of the installed map's armed-logpoint count (memory_map_t's
// logpoints_active, install calls minus uninstall calls).  Zero lets every
// slow-path access skip logpoint_lookup with one load — the arrays above are
// always allocated, so their NULL checks never short-circuit.  Set from the
// map at memory_map_init and cleared with it, like the arrays it summarises.
static uint32_t g_mem_logpoints_active = 0;
// The map whose state the aliases above currently mirror (NULL: none).
static memory_map_t *g_installed_map = NULL;
memory_logpoint_hook_t g_mem_logpoint_hook = NULL;
bool g_user_soa_reserved = false;
void (*g_mem_map_changed)(void) = NULL;
uint32_t (*g_mem_logical_xlate)(void *ctx, uint32_t addr, bool *ok) = NULL;
void *g_mem_logical_xlate_ctx = NULL;

// Slow-path access counter (diagnostic; exposed as memory.slowpath_count)
uint64_t g_mem_slowpath_count = 0;
// 1 MB-granularity histogram of slow-path addresses (24-bit space = 16 buckets)
// Slow-path histogram bucket.  The old single `(addr >> 20) & 0xF` indexed
// address bits 20-23 only, so buckets aliased every 16 MB and $50F00000 (an
// SE/30 I/O window) shared a bucket with $FFF00000 (a ROM mirror) -- the
// histogram could not tell them apart, which is most of what you want it for.
// Split instead: the low 16 MB, where a 24-bit machine spends all its time,
// keeps bits 20-23 so the VIA, IWM and ROM-overlay windows stay separate;
// everything above is bucketed by bits 28-31 in the upper half of the table.
#define MEM_SLOWPATH_BUCKET(a) (((a) < 0x01000000u) ? (((a) >> 20) & 0xFu) : (0x10u | (((a) >> 28) & 0xFu)))
uint64_t g_mem_slowpath_hist[32] = {0};

// ============================================================================
// Type Definitions
// ============================================================================

typedef struct mapping {
    struct mapping *next;
    char *name;
    void *device;
    uint32_t addr;
    uint32_t size;
    memory_interface_t memory_interface;
} mapping_t;

// Host regions on a machine with no 68k MMU (the PowerPC families): the
// page-fill hook is the machine's own physical view, so memory_map_host_region
// fills straight into it and remembers the window here -- the alias call is
// the only thing that needs to look one up again.
#define MEM_HOST_FILL_MAX 16
typedef struct mem_host_fill_region {
    uint8_t *host;
    uint32_t phys_base;
    uint32_t size;
    bool writable;
} mem_host_fill_region_t;

typedef struct memory {

    mapping_t *map;

    uint8_t *image; // flat RAM+ROM buffer

    // The page table and fast-path arrays this map owns; the g_* fast-path
    // globals alias them while the map is selected (memory_map_select).
    page_entry_t *page_table;
    int page_count;
    uint32_t address_mask;
    uintptr_t *supervisor_read, *supervisor_write, *user_read, *user_write;
    uint16_t *logpoint_page_count, *logpoint_phys_page_count;
    // SoA population tracker: the page indices written into the SoA arrays
    // since the last invalidation, so memory_soa_invalidate zeroes those
    // rather than the whole arrays.  A list that outgrows its capacity
    // (TLB_TRACK_LIMIT) sets track_overflow, and the next invalidation
    // zeroes everything instead.
    uint32_t *track_page;
    uint32_t track_count, track_cap;
    bool track_overflow;

    // What the machine hangs on its map (aliased while selected).
    memory_cpu_hooks_t cpu_hooks;
    void (*host_fill)(uint32_t page_index, uint8_t *host_ptr, bool writable);
    struct mmu_state *pmmu;
    struct lisa_mmu *lisa_mmu;
    // Host windows filled through host_fill (memory_map_host_region)
    mem_host_fill_region_t host_fill_regions[MEM_HOST_FILL_MAX];
    int host_fill_count;

    // The board's bus-error window (aliased by g_bus_err_lo/hi while installed)
    memory_bus_err_window_t bus_err;

    // Armed memory logpoints on this map (aliased by g_mem_logpoints_active)
    uint32_t logpoints_active;

    // Machine-parameterised sizes (set by memory_map_init)
    uint32_t ram_size; // RAM region size in bytes
    uint32_t rom_size; // ROM content size in bytes

    // Path of the ROM file the map was built with (or the one a checkpoint
    // names), reported as rom.path and written back into checkpoints; NULL
    // when the ROM came from bytes with no file behind them.  The ROM's
    // identity is computed from the bytes on demand (rom.c), not stored.
    char *rom_filename;

    // Object-tree binding — lifetime tied to memory_map_init / delete.
    struct object *memory_object;
    struct object *peek_object;
    struct object *poke_object;

} memory_map_t;

// ============================================================================
// Page Table Slow Paths
// ============================================================================
// The slow path is entered when the SoA fast-path entry is zero.
// Addresses arriving here are already masked by g_address_mask.
// Dispatch order: device I/O (via page_entry_t) → MMU TLB handling via
// mmu_handle_fault(...) and deferred bus error signaling → unmapped (return $FF).

// Forward declaration: lazy-install identity SoA for a host-backed page when
// the MMU is disabled.  Defined further down in this file.
static void rebuild_soa_page(uint32_t p);

// Returns true iff the page can take a direct identity host mapping under
// the current state (MMU disabled, host-backed, not a device, no logpoint).
// Used by the slow paths to decide whether to lazy-install the SoA entry.
static inline bool can_lazy_install(uint32_t page, const page_entry_t *pe) {
    if (g_mmu && g_mmu->enabled)
        return false;
    if (!pe->host_base || pe->dev)
        return false;
    if (g_mem_logpoint_page_count && g_mem_logpoint_page_count[page])
        return false;
    return true;
}

// Does the access at `addr` hit a memory logpoint (logical or physical-space)?
// Sets *host_out to the host pointer for the access (MMU-translated when the
// MMU is enabled), and *writable_out to whether the host page is writable.
// Returns false if no logpoint covers this page.
//
// Split in two so the nothing-armed answer costs one inlined load at the
// call site: every I/O slow-path access asks this question, and with zero
// logpoints armed even the call/return overhead was measurable (~3% of a
// PDM boot under callgrind).
static bool logpoint_lookup_armed(uint32_t addr, uint8_t **host_out, bool *writable_out) {
    uint32_t page = addr >> MEM_PAGE_SHIFT;
    bool logical_watched = g_mem_logpoint_page_count && g_mem_logpoint_page_count[page];
    bool phys_watched = false;
    uint32_t phys_addr = addr;

    if (g_mmu && g_mmu->enabled) {
        bool supervisor = (g_active_write == g_supervisor_write);
        phys_addr = mmu_translate_debug(g_mmu, addr, supervisor);
    } else if (logical_watched && g_mem_logical_xlate) {
        // A logically-watched page reaches here with its LOGICAL address
        // (ppc_dxlate_slow keeps the EA for watched pages); resolve the
        // physical backing through the CPU's current data context.
        bool ok;
        uint32_t pa = g_mem_logical_xlate(g_mem_logical_xlate_ctx, addr, &ok);
        if (ok)
            phys_addr = pa;
    }
    // The physical watch applies whichever regime produced phys_addr —
    // MMU-translated, PPC-translated, or the address itself (which IS
    // physical when a PPC slow translation already rewrote it, and equals
    // logical on MMU-less machines).
    if (g_mem_logpoint_phys_page_count && (phys_addr >> MEM_PAGE_SHIFT) < (uint32_t)g_page_count &&
        g_mem_logpoint_phys_page_count[phys_addr >> MEM_PAGE_SHIFT])
        phys_watched = true;

    if (!logical_watched && !phys_watched)
        return false;

    if (g_mmu && g_mmu->enabled) {
        // Route through the real physical address so writes land in the page
        // the guest actually sees, not the g_page_table identity alias.
        uint8_t *base = mmu_phys_to_host(g_mmu, phys_addr & ~(uint32_t)MEM_PAGE_MASK);
        *host_out = base ? base + (phys_addr & MEM_PAGE_MASK) : NULL;
        *writable_out = base ? mmu_phys_is_writable(g_mmu, phys_addr) : false;
    } else if ((phys_addr >> MEM_PAGE_SHIFT) < (uint32_t)g_page_count) {
        // No 68K MMU: phys_addr is the identity address or the PPC
        // translation from above — dispatch on ITS page entry.
        page_entry_t *pe = &g_page_table[phys_addr >> MEM_PAGE_SHIFT];
        *host_out = pe->host_base ? pe->host_base + (phys_addr & MEM_PAGE_MASK) : NULL;
        *writable_out = pe->writable;
    } else {
        *host_out = NULL;
        *writable_out = false;
    }
    return true;
}

// The nothing-armed fast half: inlined into every slow-path caller.
static inline bool logpoint_lookup(uint32_t addr, uint8_t **host_out, bool *writable_out) {
    if (__builtin_expect(g_mem_logpoints_active == 0, 1))
        return false;
    return logpoint_lookup_armed(addr, host_out, writable_out);
}

// Notify the hook for an access that a DEVICE answered.  The RAM/ROM route
// above reads through a host pointer, which a device page does not have, so
// without this a logpoint on an I/O register never fires — and reports zero
// events, which reads exactly like "the guest never touches this register".
// Device pages never sit in the SoA fast path, so the notify costs the
// nothing-armed load and nothing more.
static inline void logpoint_notify_device(uint32_t addr, unsigned size, uint32_t value, bool is_write) {
    uint8_t *host;
    bool writable;
    if (g_mem_logpoint_hook && logpoint_lookup(addr, &host, &writable))
        g_mem_logpoint_hook(addr, size, value, is_write);
}

// Device dispatch + logpoint notify, one wrapper per access width.  `addr` is
// the access address the logpoint matches against (logical or physical, as
// the caller resolved it); `off` is the device-relative offset it dispatches.
// A device that does not implement one access width is decoded a byte at a
// time, which is what a real bus does for a peripheral that only claims the
// byte lanes.  These dispatches used to call the width handler unconditionally:
// a device page whose write_uint16 is NULL then jumped to address 0 and took
// the HOST down, reachable from ordinary guest state -- a crashed 68000 guest
// pushing an exception frame with a garbage A7 lands on exactly such a page.
// Composing is both the safe answer and the hardware-shaped one.  Slow path
// only; the fast path never reaches a device page.
static inline uint8_t dev_raw8(const page_entry_t *pe, uint32_t off) {
    return pe->dev->read_uint8 ? pe->dev->read_uint8(pe->dev_context, off) : 0xFF;
}
static inline void dev_raw_w8(const page_entry_t *pe, uint32_t off, uint8_t v) {
    if (pe->dev->write_uint8)
        pe->dev->write_uint8(pe->dev_context, off, v);
}

static inline uint8_t dev_read8(const page_entry_t *pe, uint32_t addr, uint32_t off) {
    uint8_t v = dev_raw8(pe, off);
    logpoint_notify_device(addr, 1, v, false);
    return v;
}
static inline uint16_t dev_read16_raw(const page_entry_t *pe, uint32_t off) {
    return pe->dev->read_uint16 ? pe->dev->read_uint16(pe->dev_context, off)
                                : (uint16_t)((dev_raw8(pe, off) << 8) | dev_raw8(pe, off + 1));
}
static inline uint32_t dev_read32_raw(const page_entry_t *pe, uint32_t off) {
    if (pe->dev->read_uint32)
        return pe->dev->read_uint32(pe->dev_context, off);
    if (pe->dev->read_uint16)
        return ((uint32_t)pe->dev->read_uint16(pe->dev_context, off) << 16) |
               pe->dev->read_uint16(pe->dev_context, off + 2);
    return ((uint32_t)dev_raw8(pe, off) << 24) | ((uint32_t)dev_raw8(pe, off + 1) << 16) |
           ((uint32_t)dev_raw8(pe, off + 2) << 8) | dev_raw8(pe, off + 3);
}
static inline uint16_t dev_read16(const page_entry_t *pe, uint32_t addr, uint32_t off) {
    uint16_t v = dev_read16_raw(pe, off);
    logpoint_notify_device(addr, 2, v, false);
    return v;
}
static inline uint32_t dev_read32(const page_entry_t *pe, uint32_t addr, uint32_t off) {
    uint32_t v = dev_read32_raw(pe, off);
    logpoint_notify_device(addr, 4, v, false);
    return v;
}
static inline void dev_write8(const page_entry_t *pe, uint32_t addr, uint32_t off, uint8_t value) {
    dev_raw_w8(pe, off, value);
    logpoint_notify_device(addr, 1, value, true);
}
static inline void dev_write16_raw(const page_entry_t *pe, uint32_t off, uint16_t value) {
    if (pe->dev->write_uint16) {
        pe->dev->write_uint16(pe->dev_context, off, value);
        return;
    }
    dev_raw_w8(pe, off, (uint8_t)(value >> 8));
    dev_raw_w8(pe, off + 1, (uint8_t)value);
}
static inline void dev_write32_raw(const page_entry_t *pe, uint32_t off, uint32_t value) {
    if (pe->dev->write_uint32) {
        pe->dev->write_uint32(pe->dev_context, off, value);
        return;
    }
    if (pe->dev->write_uint16) {
        pe->dev->write_uint16(pe->dev_context, off, (uint16_t)(value >> 16));
        pe->dev->write_uint16(pe->dev_context, off + 2, (uint16_t)value);
        return;
    }
    dev_raw_w8(pe, off, (uint8_t)(value >> 24));
    dev_raw_w8(pe, off + 1, (uint8_t)(value >> 16));
    dev_raw_w8(pe, off + 2, (uint8_t)(value >> 8));
    dev_raw_w8(pe, off + 3, (uint8_t)value);
}
static inline void dev_write16(const page_entry_t *pe, uint32_t addr, uint32_t off, uint16_t value) {
    dev_write16_raw(pe, off, value);
    logpoint_notify_device(addr, 2, value, true);
}
static inline void dev_write32(const page_entry_t *pe, uint32_t addr, uint32_t off, uint32_t value) {
    dev_write32_raw(pe, off, value);
    logpoint_notify_device(addr, 4, value, true);
}

// Decide whether a device registered at this logical page should be
// dispatched directly, or whether the access must instead fall through to the
// MMU table walk.
//
// The motivating case is the 24-bit Memory Manager: a master pointer's high
// byte carries lock/purge/resource flags, so a relocatable block at physical
// $00xxxxxx is dereferenced as logical $40xxxxxx.  The 24-bit MMU tree
// (TC.IS=8) strips the flag byte back to $00xxxxxx (real RAM), but the static
// page-table device mapping for the $40xxxxxx ROM/I-O window would otherwise
// shadow that translation and return ROM garbage — corrupting the heap and
// hanging the OS (observed booting the IIfx 8bpp under memory pressure, where
// the Font Manager's purgeable strikes acquire the $40 purge flag).
//
// We must NOT, however, divert legitimate device I/O: in 24-bit mode the MMU
// remaps essentially every access non-identically, so a plain "is it
// identity?" test would strand the VIA/SCC/IOP registers.  Diversion is
// therefore limited to the case that actually matters: the MMU points the
// page at real host-backed RAM.  Anything still resolving to device or
// unmapped space keeps its direct dispatch, leaving 24-bit I/O untouched.
static inline bool dispatch_device_at_logical(uint32_t addr, bool supervisor) {
    if (!(g_mmu && g_mmu->enabled))
        return true; // MMU off: logical == physical
    uint32_t phys;
    // A FAILED walk (invalid PTE) must fault, never dispatch to a device — even
    // when a device is registered at this logical page.  This is not academic:
    // a user virtual address can legitimately land in a host-machine device
    // window (e.g. A/UX maps a process's image at virtual $47F00000, which sits
    // inside the IIfx ROM device window $40000000-$4FFFFFFF).  When such a page
    // is still demand-zero, copyout's touch must fault so the kernel pages it
    // in — dispatching the write to the ROM device instead silently drops it,
    // the page is never allocated, and the subsequent copy lands on physical 0
    // (the vector table).  mmu_translate_debug can't catch this on its own: a
    // failed walk returns phys==logical, indistinguishable from a true identity
    // map, so use the validity-reporting walk here.
    if (!mmu_translate_checked(g_mmu, addr, supervisor, &phys))
        return false; // translation failed → fall through to the MMU fault path
    if ((phys & ~(uint32_t)MEM_PAGE_MASK) == (addr & ~(uint32_t)MEM_PAGE_MASK))
        return true; // identity mapping — dispatch as usual
    // Non-identity: only divert to the table walk when the target is real RAM.
    return mmu_phys_to_host(g_mmu, phys & ~(uint32_t)MEM_PAGE_MASK) == NULL;
}

// Latch a deferred bus error on behalf of a DEVICE that terminates an
// access with a transfer error — the PDM's BART windows, where an empty
// slot answers the Slot Manager's declaration-ROM probe with a recoverable
// fault rather than data.  Same delivery as the unmapped-page faults the
// slow paths raise below: the CPU seam takes it at the sprint boundary (68k
// bus error / 601 machine check).  Signalling one from a read is a read
// side effect, so such a device implements peek_* (memory_interface_t) and
// an inspection never reaches here through it.
void memory_signal_bus_error(uint32_t addr, bool write) {
    if (g_bus_error_pending)
        return;
    g_bus_error_pending = true;
    g_bus_error_address = addr;
    g_bus_error_rw = !write; // the flag reads "true = read"
    g_bus_error_fc = (g_active_read == g_supervisor_read) ? 5 : 1;
    g_bus_error_is_pmmu = false; // a plain bus timeout, not a descriptor fix-up
    memory_end_sprint(g_bus_error_instr_ptr); // force the decoder loop to exit
}

// Slow path for 8-bit reads: device I/O, MMU TLB miss, or unmapped
// One body for memory_read_uint{8,16,32}_slow.
//
// `size` is a compile-time constant at every call site, so every `size ==`
// test and the whole in-page guard fold away: the three generated functions
// come out within a few instructions of the hand-written ones they replace,
// with no new out-of-line call.  The FAST path is unaffected either way --
// it never CALLS these, it falls out to them through a tail branch.
//
// These six slow paths carried ~60-75% duplicated logic, which is why a fix
// here historically had to be made six times over.
static inline __attribute__((always_inline)) uint32_t load_be_n(const uint8_t *p, unsigned size) {
    return size == 1 ? LOAD_BE8(p) : size == 2 ? LOAD_BE16(p) : LOAD_BE32(p);
}
static inline __attribute__((always_inline)) uint32_t dev_read_n(const page_entry_t *pe, uint32_t addr, uint32_t off,
                                                                 unsigned size) {
    return size == 1 ? dev_read8(pe, addr, off) : size == 2 ? dev_read16(pe, addr, off) : dev_read32(pe, addr, off);
}

static inline __attribute__((always_inline)) uint32_t read_slow_n(uint32_t addr, unsigned size) {
    g_mem_slowpath_count++;
    g_mem_slowpath_hist[MEM_SLOWPATH_BUCKET(addr)]++;

    const bool supervisor = g_active_read == g_supervisor_read;

    // Lisa segment MMU owns translation, routing, and bus errors for Lisa/XL
    // machines (its SoA stays empty so every access reaches here).
    if (__builtin_expect(g_lisa_mmu != NULL, 0))
        return size == 1   ? lisa_mmu_read8(addr, supervisor)
               : size == 2 ? lisa_mmu_read16(addr, supervisor)
                           : lisa_mmu_read32(addr, supervisor);

    // A byte can never straddle a page, so this is constant-true for size 1.
    const bool in_page = (size == 1) || ((addr & MEM_PAGE_MASK) <= MEM_PAGE_SIZE - size);
    const uint32_t fill = size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;

    uint32_t page = addr >> MEM_PAGE_SHIFT;
    page_entry_t *pe = &g_page_table[page];

    // Memory logpoint: page is forced to the slow path but backed by RAM/ROM.
    // Read through the MMU-translated host pointer, then notify the hook.
    uint8_t *lp_host;
    bool lp_writable;
    if (in_page && logpoint_lookup(addr, &lp_host, &lp_writable) && lp_host) {
        uint32_t v = load_be_n(lp_host, size);
        if (g_mem_logpoint_hook)
            g_mem_logpoint_hook(addr, size, v, false);
        return v;
    }

    // Lazy-install identity SoA for a host-backed page when the MMU is off.
    if (in_page && can_lazy_install(page, pe)) {
        rebuild_soa_page(page);
        return load_be_n(pe->host_base + (addr & MEM_PAGE_MASK), size);
    }

    // Gate logical-device dispatch via dispatch_device_at_logical(): identity /
    // MMU-off / non-identity-to-device cases dispatch immediately; non-identity-
    // to-RAM (e.g. 24-bit Memory Manager flag-tagged master pointers $40xxxxxx
    // -> $00xxxxxx) falls through to the MMU walk below so the translated RAM
    // is read instead of returning ROM bytes from the $40000000 device window.
    if (pe->dev && in_page && dispatch_device_at_logical(addr, supervisor))
        return dev_read_n(pe, addr, addr - pe->base_addr, size);

    // With the MMU enabled, dispatch via the PHYSICAL address (after the table
    // walk) rather than the logical page-table entry -- otherwise a user-virtual
    // address whose upper byte coincides with a host MMIO range (virtual
    // $47f01000 hitting the IIfx ROM window at $40000000-$4FFFFFFF) silently
    // returns the device's value instead of faulting, and A/UX's copyin depends
    // on that fault to demand-page user pages.  A TT match means logical ==
    // physical, so the logical entry IS the right dispatch and the walk is
    // skipped -- kernel I/O is typically TT-mapped.  Cross-page accesses fall
    // through to the split below, whose halves handle the MMU correctly.
    if (g_mmu && g_mmu->enabled && in_page) {
        if (pe->dev && mmu_check_tt(g_mmu, addr, false, supervisor))
            return dev_read_n(pe, addr, addr - pe->base_addr, size);
        if (mmu_handle_fault(g_mmu, addr, false, supervisor)) {
            uintptr_t base = g_active_read[addr >> MEM_PAGE_SHIFT];
            if (base != 0)
                return load_be_n((uint8_t *)(base + addr), size);
            // SoA still 0: the physical page is a device, unmapped, or
            // logpointed.  Translate and dispatch on the PHYSICAL entry.
            uint32_t phys = mmu_translate_debug(g_mmu, addr, supervisor);
            uint32_t phys_page = phys >> MEM_PAGE_SHIFT;
            if (phys_page < g_page_count) {
                page_entry_t *phys_pe = &g_page_table[phys_page];
                if (phys_pe->dev)
                    return dev_read_n(phys_pe, addr, phys - phys_pe->base_addr, size);
            }
            // Re-check the logpoint now that mmu_handle_fault has run:
            // physical-space logpoints suppress the fill and need translation.
            if (logpoint_lookup(addr, &lp_host, &lp_writable) && lp_host) {
                uint32_t v = load_be_n(lp_host, size);
                if (g_mem_logpoint_hook)
                    g_mem_logpoint_hook(addr, size, v, false);
                return v;
            }
        } else if (!g_bus_error_pending) {
            // MMU fault: invalid descriptor, unmapped physical, permission.
            g_bus_error_pending = true;
            g_bus_error_address = addr;
            g_bus_error_rw = true; // read
            g_bus_error_fc = supervisor ? 5 : 1;
            memory_end_sprint(g_bus_error_instr_ptr); // force decoder loop exit
        }
        return fill; // unmapped physical reads $FF
    }

    // MMU disabled: logical == physical, dispatch by logical page-table entry.
    if (pe->dev && in_page)
        return dev_read_n(pe, addr, addr - pe->base_addr, size);

    if (size == 1) {
        // Nothing answered.  Inside the board's bus-error window the watchdog
        // fires; outside it the bus floats to the pull-ups and reads $FF.  The
        // window used to be consulted only on the transparent-translation path,
        // so with the MMU disabled -- most of POST -- an unpopulated slot read
        // $FF and never faulted.
        //
        // $FF on a float matches real 68k Mac hardware and is load-bearing for
        // ROM RAM sizing (write pattern, read back $FF, find the boundary) and
        // for the POST memory test.
        if (memory_addr_faults_when_unmapped(addr))
            memory_signal_bus_error(addr, false);
        return 0xFF;
    }

    // Cross-page, or host memory at a page boundary: split into two halves.
    // Real hardware splits too -- MC68030UM Table 7-6 gives a misaligned long
    // two or more bus cycles -- and each half re-enters here in-page.
    uint32_t hi = size == 2 ? memory_read_uint8(addr) : memory_read_uint16(addr);
    uint32_t lo =
        size == 2 ? memory_read_uint8((addr + 1) & g_address_mask) : memory_read_uint16((addr + 2) & g_address_mask);
    return (hi << (size * 4)) | lo;
}

uint8_t memory_read_uint8_slow(uint32_t addr) {
    return (uint8_t)read_slow_n(addr, 1);
}
uint16_t memory_read_uint16_slow(uint32_t addr) {
    return (uint16_t)read_slow_n(addr, 2);
}
uint32_t memory_read_uint32_slow(uint32_t addr) {
    return read_slow_n(addr, 4);
}

// === Side-effect-free debug reads ==========================================
//
// An inspection reads a device through its peek_* (memory_interface_t): what
// the guest's read would return, with none of the read's side effects.  A
// device without peek_* has none to avoid, so its read_* serves.  A missing
// width composes from the narrower peek, as the guest path composes reads.
static inline uint8_t dev_peek8(const page_entry_t *pe, uint32_t off) {
    return pe->dev->peek_uint8 ? pe->dev->peek_uint8(pe->dev_context, off) : dev_raw8(pe, off);
}
static inline uint16_t dev_peek16(const page_entry_t *pe, uint32_t off) {
    if (pe->dev->peek_uint16)
        return pe->dev->peek_uint16(pe->dev_context, off);
    if (pe->dev->peek_uint8)
        return (uint16_t)((dev_peek8(pe, off) << 8) | dev_peek8(pe, off + 1));
    return dev_read16_raw(pe, off);
}
static inline uint32_t dev_peek32(const page_entry_t *pe, uint32_t off) {
    if (pe->dev->peek_uint32)
        return pe->dev->peek_uint32(pe->dev_context, off);
    if (pe->dev->peek_uint16 || pe->dev->peek_uint8)
        return ((uint32_t)dev_peek16(pe, off) << 16) | dev_peek16(pe, off + 2);
    return dev_read32_raw(pe, off);
}

static inline uint32_t debug_dev_read(const page_entry_t *pe, uint32_t phys, unsigned size) {
    // Same NULL-safety as the dev_read*/dev_write* wrappers: the debugger must
    // never be the thing that takes the host down.
    uint32_t off = phys - pe->base_addr;
    return size == 1 ? dev_peek8(pe, off) : size == 2 ? dev_peek16(pe, off) : dev_peek32(pe, off);
}

// memory.poke is a real write -- it has the write's effect on the device --
// but it is not a guest bus cycle, so a transfer error the device answers
// with is not the guest's to take.  A pending fault makes
// memory_signal_bus_error a no-op, so the write runs with one "pending" and
// the guest's own state is put back after.
static inline void debug_dev_write(const page_entry_t *pe, uint32_t phys, unsigned size, uint32_t value) {
    uint32_t off = phys - pe->base_addr;
    bool pending = g_bus_error_pending;
    g_bus_error_pending = true;
    if (size == 1)
        dev_raw_w8(pe, off, (uint8_t)value);
    else if (size == 2)
        dev_write16_raw(pe, off, (uint16_t)value);
    else
        dev_write32_raw(pe, off, value);
    g_bus_error_pending = pending;
}

// Used by the shell's inspection commands (memory.peek/.dump/.read_cstring,
// find.*).  Examining guest memory MUST NOT perturb guest execution and MUST
// NOT crash on a bad address.  The normal memory_read_uint* helpers run the
// full CPU path: mmu_handle_fault populates the SoA/TLB and, on a FAILED walk,
// the caller latches g_bus_error_pending — which injects a spurious bus error
// into the next instruction the guest runs (so a stray `memory.peek` of an
// unmapped address silently corrupts execution).  These debug variants instead:
//   - translate logical->physical via the side-effect-free mmu_translate_checked,
//   - read host RAM/ROM directly, or dispatch the device read so device
//     registers stay inspectable (e.g. NuBus video regs in iicx-video-modes),
//   - return an all-ones sentinel for unmapped/invalid pages,
//   - never call mmu_handle_fault, never touch the SoA cache or g_bus_error_pending.
uint8_t memory_debug_read_uint8(uint32_t addr) {
    addr &= g_address_mask;
    if (__builtin_expect(g_lisa_mmu != NULL, 0))
        return (uint8_t)lisa_mmu_debug_read(addr, 1, g_active_read == g_supervisor_read);
    uint32_t phys = addr;
    if (g_mmu && g_mmu->enabled && !mmu_translate_checked(g_mmu, addr, g_active_read == g_supervisor_read, &phys))
        return 0xFF;
    uint32_t page = phys >> MEM_PAGE_SHIFT;
    if (page >= g_page_count)
        return 0xFF;
    page_entry_t *pe = &g_page_table[page];
    if (pe->host_base)
        return LOAD_BE8(pe->host_base + (phys & MEM_PAGE_MASK));
    if (pe->dev)
        return (uint8_t)debug_dev_read(pe, phys, 1);
    return 0xFF;
}

uint16_t memory_debug_read_uint16(uint32_t addr) {
    addr &= g_address_mask;
    if (__builtin_expect(g_lisa_mmu != NULL, 0))
        return (uint16_t)lisa_mmu_debug_read(addr, 2, g_active_read == g_supervisor_read);
    // Single-page word reads hit the device's own width handler; byte-split
    // only when the access straddles a page.
    if ((addr & MEM_PAGE_MASK) <= MEM_PAGE_SIZE - 2) {
        uint32_t phys = addr;
        if (g_mmu && g_mmu->enabled && !mmu_translate_checked(g_mmu, addr, g_active_read == g_supervisor_read, &phys))
            return 0xFFFF;
        uint32_t page = phys >> MEM_PAGE_SHIFT;
        if (page < g_page_count) {
            page_entry_t *pe = &g_page_table[page];
            if (pe->host_base)
                return LOAD_BE16(pe->host_base + (phys & MEM_PAGE_MASK));
            if (pe->dev)
                return (uint16_t)debug_dev_read(pe, phys, 2);
        }
        return 0xFFFF;
    }
    return (uint16_t)((memory_debug_read_uint8(addr) << 8) | memory_debug_read_uint8(addr + 1));
}

uint32_t memory_debug_read_uint32(uint32_t addr) {
    addr &= g_address_mask;
    if (__builtin_expect(g_lisa_mmu != NULL, 0))
        return lisa_mmu_debug_read(addr, 4, g_active_read == g_supervisor_read);
    if ((addr & MEM_PAGE_MASK) <= MEM_PAGE_SIZE - 4) {
        uint32_t phys = addr;
        if (g_mmu && g_mmu->enabled && !mmu_translate_checked(g_mmu, addr, g_active_read == g_supervisor_read, &phys))
            return 0xFFFFFFFFu;
        uint32_t page = phys >> MEM_PAGE_SHIFT;
        if (page < g_page_count) {
            page_entry_t *pe = &g_page_table[page];
            if (pe->host_base)
                return LOAD_BE32(pe->host_base + (phys & MEM_PAGE_MASK));
            if (pe->dev)
                return debug_dev_read(pe, phys, 4);
        }
        return 0xFFFFFFFFu;
    }
    return ((uint32_t)memory_debug_read_uint16(addr) << 16) | memory_debug_read_uint16(addr + 2);
}

uint32_t memory_debug_read_phys(uint32_t phys, unsigned size, bool *ok) {
    if (ok)
        *ok = false;
    if (g_lisa_mmu || (size != 1 && size != 2 && size != 4))
        return 0;
    uint32_t value = 0;
    for (unsigned i = 0; i < size; i++) { // byte-wise: an access may straddle pages
        uint32_t a = phys + i;
        uint32_t page = a >> MEM_PAGE_SHIFT;
        if (page >= g_page_count)
            return 0;
        page_entry_t *pe = &g_page_table[page];
        uint8_t b;
        if (pe->host_base)
            b = pe->host_base[a & MEM_PAGE_MASK];
        else if (pe->dev)
            b = (uint8_t)debug_dev_read(pe, a, 1);
        else
            return 0; // unmapped
        value = (value << 8) | b;
    }
    if (ok)
        *ok = true;
    return value;
}

// Bulk side-effect-free read of `len` bytes into `dst`.  Copies whole spans out
// of a page's host backing with memcpy when possible (RAM/ROM, no MMU, no Lisa,
// no device), and falls back to the per-byte path across page boundaries or for
// MMU/device/unmapped pages — so the result is byte-for-byte identical to
// calling memory_debug_read_uint8() len times, only far cheaper for the common
// contiguous-RAM case.  Never faults the guest.
void memory_debug_read_block(uint32_t addr, uint8_t *dst, uint32_t len) {
    while (len) {
        uint32_t a = addr & g_address_mask;
        uint32_t off = a & MEM_PAGE_MASK;
        uint32_t chunk = MEM_PAGE_SIZE - off; // stay within one page per iteration
        if (chunk > len)
            chunk = len;
        // Try to memcpy the whole chunk straight out of the page's host backing.
        // Translate through the MMU per page (offset preserved, so the chunk
        // stays inside one physical page) so this stays fast in 32-bit mode too.
        if (__builtin_expect(g_lisa_mmu == NULL, 1)) {
            uint32_t phys = a;
            bool ok = !(g_mmu && g_mmu->enabled) ||
                      mmu_translate_checked(g_mmu, a, g_active_read == g_supervisor_read, &phys);
            if (ok) {
                uint32_t page = phys >> MEM_PAGE_SHIFT;
                if (page < g_page_count) {
                    page_entry_t *pe = &g_page_table[page];
                    if (pe->host_base && !pe->dev) {
                        memcpy(dst, pe->host_base + (phys & MEM_PAGE_MASK), chunk);
                        dst += chunk;
                        addr += chunk;
                        len -= chunk;
                        continue;
                    }
                }
            }
        }
        // Device / unmapped / Lisa MMU / translation miss: exact per-byte path.
        for (uint32_t i = 0; i < chunk; i++)
            dst[i] = memory_debug_read_uint8(a + i);
        dst += chunk;
        addr += chunk;
        len -= chunk;
    }
}

// Side-effect-free debug writes (memory.poke) — symmetric to the debug reads:
// translate via mmu_translate_checked, write host RAM (if writable) or dispatch
// the device write, drop ROM/unmapped silently, and NEVER fault or latch
// g_bus_error_pending (a stray poke must not inject a bus error into the guest).
// Returns true if the byte landed in writable host RAM / a device.
bool memory_debug_write_uint8(uint32_t addr, uint8_t value) {
    addr &= g_address_mask;
    if (__builtin_expect(g_lisa_mmu != NULL, 0))
        return lisa_mmu_debug_write(addr, 1, g_active_write == g_supervisor_write, value);
    uint32_t phys = addr;
    if (g_mmu && g_mmu->enabled && !mmu_translate_checked(g_mmu, addr, g_active_write == g_supervisor_write, &phys))
        return false;
    uint32_t page = phys >> MEM_PAGE_SHIFT;
    if (page >= g_page_count)
        return false;
    page_entry_t *pe = &g_page_table[page];
    if (pe->host_base) {
        if (!pe->writable)
            return false; // ROM/VROM — drop silently
        STORE_BE8(pe->host_base + (phys & MEM_PAGE_MASK), value);
        return true;
    }
    if (pe->dev) {
        debug_dev_write(pe, phys, 1, value);
        return true;
    }
    return false;
}

bool memory_debug_write_uint16(uint32_t addr, uint16_t value) {
    addr &= g_address_mask;
    if (__builtin_expect(g_lisa_mmu != NULL, 0))
        return lisa_mmu_debug_write(addr, 2, g_active_write == g_supervisor_write, value);
    if ((addr & MEM_PAGE_MASK) <= MEM_PAGE_SIZE - 2) {
        uint32_t phys = addr;
        if (g_mmu && g_mmu->enabled && !mmu_translate_checked(g_mmu, addr, g_active_write == g_supervisor_write, &phys))
            return false;
        uint32_t page = phys >> MEM_PAGE_SHIFT;
        if (page < g_page_count) {
            page_entry_t *pe = &g_page_table[page];
            if (pe->host_base) {
                if (!pe->writable)
                    return false;
                STORE_BE16(pe->host_base + (phys & MEM_PAGE_MASK), value);
                return true;
            }
            if (pe->dev) {
                debug_dev_write(pe, phys, 2, value);
                return true;
            }
        }
        return false;
    }
    bool a = memory_debug_write_uint8(addr, (uint8_t)(value >> 8));
    bool b = memory_debug_write_uint8(addr + 1, (uint8_t)value);
    return a && b;
}

bool memory_debug_write_uint32(uint32_t addr, uint32_t value) {
    addr &= g_address_mask;
    if (__builtin_expect(g_lisa_mmu != NULL, 0))
        return lisa_mmu_debug_write(addr, 4, g_active_write == g_supervisor_write, value);
    if ((addr & MEM_PAGE_MASK) <= MEM_PAGE_SIZE - 4) {
        uint32_t phys = addr;
        if (g_mmu && g_mmu->enabled && !mmu_translate_checked(g_mmu, addr, g_active_write == g_supervisor_write, &phys))
            return false;
        uint32_t page = phys >> MEM_PAGE_SHIFT;
        if (page < g_page_count) {
            page_entry_t *pe = &g_page_table[page];
            if (pe->host_base) {
                if (!pe->writable)
                    return false;
                STORE_BE32(pe->host_base + (phys & MEM_PAGE_MASK), value);
                return true;
            }
            if (pe->dev) {
                debug_dev_write(pe, phys, 4, value);
                return true;
            }
        }
        return false;
    }
    bool a = memory_debug_write_uint16(addr, (uint16_t)(value >> 16));
    bool b = memory_debug_write_uint16(addr + 2, (uint16_t)value);
    return a && b;
}

// Slow path for 8-bit writes: device I/O, MMU TLB miss, or unmapped
// One body for memory_write_uint{8,16,32}_slow -- the write-side twin of
// read_slow_n above, and the same reasoning applies: `size` is a compile-time
// constant, so the in-page guard and every `size ==` test fold away, and the
// fast path is untouched because it falls out to these rather than calling
// them.
static inline __attribute__((always_inline)) void store_be_n(uint8_t *p, uint32_t value, unsigned size) {
    if (size == 1)
        STORE_BE8(p, value);
    else if (size == 2)
        STORE_BE16(p, value);
    else
        STORE_BE32(p, value);
}
static inline __attribute__((always_inline)) void dev_write_n(const page_entry_t *pe, uint32_t addr, uint32_t off,
                                                              uint32_t value, unsigned size) {
    if (size == 1)
        dev_write8(pe, addr, off, (uint8_t)value);
    else if (size == 2)
        dev_write16(pe, addr, off, (uint16_t)value);
    else
        dev_write32(pe, addr, off, value);
}

static inline __attribute__((always_inline)) void write_slow_n(uint32_t addr, uint32_t value, unsigned size) {
    g_mem_slowpath_count++;
    g_mem_slowpath_hist[MEM_SLOWPATH_BUCKET(addr)]++;

    const bool supervisor = g_active_write == g_supervisor_write;

    // Lisa segment MMU owns translation, routing and bus errors for Lisa/XL.
    if (__builtin_expect(g_lisa_mmu != NULL, 0)) {
        if (size == 1)
            lisa_mmu_write8(addr, supervisor, (uint8_t)value);
        else if (size == 2)
            lisa_mmu_write16(addr, supervisor, (uint16_t)value);
        else
            lisa_mmu_write32(addr, supervisor, value);
        return;
    }

    // A byte can never straddle a page, so this is constant-true for size 1.
    const bool in_page = (size == 1) || ((addr & MEM_PAGE_MASK) <= MEM_PAGE_SIZE - size);

    uint32_t page = addr >> MEM_PAGE_SHIFT;
    page_entry_t *pe = &g_page_table[page];

    // Memory logpoint: forced slow path for a RAM write on a logged page.
    uint8_t *lp_host;
    bool lp_writable;
    if (in_page && logpoint_lookup(addr, &lp_host, &lp_writable) && lp_host && lp_writable) {
        store_be_n(lp_host, value, size);
        if (g_mem_logpoint_hook)
            g_mem_logpoint_hook(addr, size, value, true);
        return;
    }

    // Lazy-install identity SoA for a writable host-backed page when the MMU is
    // off.  Read-only pages (ROM) still drop the write via the fall-through.
    if (in_page && can_lazy_install(page, pe)) {
        rebuild_soa_page(page);
        if (pe->writable)
            store_be_n(pe->host_base + (addr & MEM_PAGE_MASK), value, size);
        return;
    }

    // Gate logical-device dispatch (24-bit Mac OS master-pointer fix -- see
    // dispatch_device_at_logical above).
    if (pe->dev && in_page && dispatch_device_at_logical(addr, supervisor)) {
        dev_write_n(pe, addr, addr - pe->base_addr, value, size);
        return;
    }

    // With the MMU enabled the device lookup must use the PHYSICAL address --
    // otherwise a user-virtual address whose upper byte coincides with a host
    // MMIO range (virtual $47f01000 hitting the IIfx ROM window at physical
    // $40000000-$4FFFFFFF) silently absorbs the write through the device's noop
    // handler and never reaches the walk.  A/UX's copyout depends on that walk
    // faulting on unmapped user pages so its handler can demand-page them in;
    // hiding the fault makes realvtop return 0 and p_blt overwrite virtual $0,
    // the kernel exception vectors.  A TT match means logical == physical, so
    // the logical pe->dev IS the right dispatch and the walk is skipped.
    if (g_mmu && g_mmu->enabled && in_page) {
        if (pe->dev && mmu_check_tt(g_mmu, addr, true, supervisor)) {
            dev_write_n(pe, addr, addr - pe->base_addr, value, size);
            return;
        }
        if (mmu_handle_fault(g_mmu, addr, true, supervisor)) {
            uintptr_t base = g_active_write[addr >> MEM_PAGE_SHIFT];
            if (base != 0) {
                store_be_n((uint8_t *)(base + addr), value, size);
                return;
            }
            // SoA still 0: the physical page is a device, unmapped, or covered
            // by a logpoint.  Translate and dispatch on the PHYSICAL entry.
            uint32_t phys = mmu_translate_debug(g_mmu, addr, supervisor);
            uint32_t phys_page = phys >> MEM_PAGE_SHIFT;
            if (phys_page < g_page_count) {
                page_entry_t *phys_pe = &g_page_table[phys_page];
                if (phys_pe->dev) {
                    dev_write_n(phys_pe, addr, phys - phys_pe->base_addr, value, size);
                    return;
                }
            }
            // Re-check the logpoint now the fault has run: physical-space
            // logpoints are only detectable after mmu_translate_debug.
            if (logpoint_lookup(addr, &lp_host, &lp_writable) && lp_host && lp_writable) {
                store_be_n(lp_host, value, size);
                if (g_mem_logpoint_hook)
                    g_mem_logpoint_hook(addr, size, value, true);
                return;
            }
            // Unmapped physical but no fault: drop the write.
        } else if (!g_bus_error_pending) {
            // MMU fault: invalid descriptor, unmapped physical, permission.
            g_bus_error_pending = true;
            g_bus_error_address = addr;
            g_bus_error_rw = false; // write
            g_bus_error_fc = supervisor ? 5 : 1;
            memory_end_sprint(g_bus_error_instr_ptr); // force decoder loop exit
        }
        return;
    }

    // MMU disabled: logical == physical, dispatch by logical page-table entry.
    if (pe->dev && in_page) {
        dev_write_n(pe, addr, addr - pe->base_addr, value, size);
        return;
    }

    if (size == 1)
        return; // nothing answered: the write is dropped

    // Cross-page, or host memory at a page boundary: split into two halves.
    if (size == 2) {
        memory_write_uint8(addr, (uint8_t)(value >> 8));
        memory_write_uint8((addr + 1) & g_address_mask, (uint8_t)value);
    } else {
        memory_write_uint16(addr, (uint16_t)(value >> 16));
        memory_write_uint16((addr + 2) & g_address_mask, (uint16_t)value);
    }
}

void memory_write_uint8_slow(uint32_t addr, uint8_t value) {
    write_slow_n(addr, value, 1);
}
void memory_write_uint16_slow(uint32_t addr, uint16_t value) {
    write_slow_n(addr, value, 2);
}
void memory_write_uint32_slow(uint32_t addr, uint32_t value) {
    write_slow_n(addr, value, 4);
}

// Read memory at the given address with specified size (1, 2, or 4 bytes)
uint32_t memory_read(unsigned int size, uint32_t addr) {
    switch (size) {
    case 1:
        return memory_read_uint8(addr);
    case 2:
        return memory_read_uint16(addr);
    case 4:
        return memory_read_uint32(addr);
    default:
        // Not a guest bus width: log it in every build, not just debug
        LOG(0, "memory_read: unsupported size %u at $%08X", size, addr);
        return 0;
    }
}

// Write memory at the given address with specified size (1, 2, or 4 bytes)
void memory_write(unsigned int size, uint32_t addr, uint32_t value) {
    switch (size) {
    case 1:
        memory_write_uint8(addr, (uint8_t)value);
        break;
    case 2:
        memory_write_uint16(addr, (uint16_t)value);
        break;
    case 4:
        memory_write_uint32(addr, (uint32_t)value);
        break;
    default:
        LOG(0, "memory_write: unsupported size %u at $%08X", size, addr);
        break;
    }
}

// ============================================================================
// Memory Logpoint Helpers
// ============================================================================

// Rebuild SoA entries for a single page from the cold-path page_entry_t.
// Called when a page's logpoint refcount returns to zero, so the fast path
// can resume direct access.  For MMU-mapped pages, the TLB fill will happen
// lazily on the next access (we simply leave the SoA entry zero).
//
// Also called from the memory_*_slow paths to lazy-install identity mappings
// for host-backed pages when the MMU is disabled — replaces the eager-fill
// loop that used to live in mmu_invalidate_tlb. Safe to call from anywhere;
// no-ops when the page can't take a direct identity mapping (MMU enabled,
// device page, unmapped page, or page covered by a memory logpoint).
static void rebuild_soa_page(uint32_t p) {
    if (p >= g_page_count)
        return;
    page_entry_t *pe = &g_page_table[p];
    // MMU-mapped pages rebuild themselves via mmu_handle_fault on next access.
    if (g_mmu && g_mmu->enabled)
        return;
    if (!pe->host_base || pe->dev)
        return; // device/unmapped: leave SoA at 0 so the slow path takes over
    if (g_mem_logpoint_page_count && g_mem_logpoint_page_count[p])
        return; // logpoint: must keep SoA = 0 to fire the hook on every access
    // Identity fill: logical page == physical page, so the physical
    // logpoint count applies directly too.
    if (g_mem_logpoint_phys_page_count && g_mem_logpoint_phys_page_count[p])
        return;
    uint32_t guest_base = p << MEM_PAGE_SHIFT;
    uintptr_t adjusted = (uintptr_t)pe->host_base - guest_base;
    if (g_supervisor_read)
        g_supervisor_read[p] = adjusted;
    if (g_user_read && !g_user_soa_reserved)
        g_user_read[p] = adjusted;
    if (pe->writable) {
        if (g_supervisor_write)
            g_supervisor_write[p] = adjusted;
        if (g_user_write && !g_user_soa_reserved)
            g_user_write[p] = adjusted;
    }
    tlb_track_page(p); // ensure the next mmu_invalidate_tlb zeroes this entry
}

// ============================================================================
// SoA population tracking (for fast invalidation)
// ============================================================================

// Instead of zeroing all 1M+ page entries on every TLB invalidation (32 MB of
// memset on a 32-bit address space), the selected map lists which pages have
// been populated and only those are zeroed.  Typical working sets are
// ~2000-3000 pages (8-12 MB of RAM + ROM + VRAM), reducing invalidation cost
// from O(address_space) to O(working_set).  The list grows on demand up to
// TLB_TRACK_LIMIT entries, so only a working set past that (512 MB of touched
// pages) falls back to the full memset.
#define TLB_TRACK_INITIAL 8192
#define TLB_TRACK_LIMIT   (1u << 17)

// Zero all four SoA arrays of the selected map.
static void soa_zero_all(void) {
    size_t sz = (size_t)g_page_count * sizeof(uintptr_t);
    if (g_supervisor_read)
        memset(g_supervisor_read, 0, sz);
    if (g_supervisor_write)
        memset(g_supervisor_write, 0, sz);
    if (g_user_read)
        memset(g_user_read, 0, sz);
    if (g_user_write)
        memset(g_user_write, 0, sz);
}

// Start a fresh (empty, exact) list: every SoA entry is zero right now.
static void tlb_track_reset(memory_map_t *m) {
    m->track_count = 0;
    m->track_overflow = false;
}

void tlb_track_page(uint32_t page_index) {
    memory_map_t *m = g_installed_map;
    if (!m || m->track_overflow)
        return; // no map, or already in zero-everything mode
    if (m->track_count >= m->track_cap) {
        uint32_t cap = m->track_cap ? m->track_cap * 2 : TLB_TRACK_INITIAL;
        uint32_t *grown = cap <= TLB_TRACK_LIMIT ? realloc(m->track_page, (size_t)cap * sizeof(uint32_t)) : NULL;
        if (!grown) {
            m->track_overflow = true; // past the limit (or out of memory): zero everything next time
            return;
        }
        m->track_page = grown;
        m->track_cap = cap;
    }
    m->track_page[m->track_count++] = page_index;
}

void memory_soa_invalidate(void) {
    memory_map_t *m = g_installed_map;
    if (!m || m->track_overflow) {
        soa_zero_all();
    } else {
        for (uint32_t i = 0; i < m->track_count; i++) {
            uint32_t p = m->track_page[i];
            if (p >= g_page_count)
                continue;
            if (g_supervisor_read)
                g_supervisor_read[p] = 0;
            if (g_supervisor_write)
                g_supervisor_write[p] = 0;
            if (g_user_read)
                g_user_read[p] = 0;
            if (g_user_write)
                g_user_write[p] = 0;
        }
    }
    if (m)
        tlb_track_reset(m);
}

// Count one logpoint armed (+1) or disarmed (-1) on the installed map and
// refresh the slow path's alias.  A disarm with nothing armed is a no-op.
static void logpoints_active_adjust(int delta) {
    memory_map_t *m = g_installed_map;
    if (!m || (delta < 0 && m->logpoints_active == 0))
        return;
    m->logpoints_active += (uint32_t)delta;
    g_mem_logpoints_active = m->logpoints_active;
}

void memory_logpoint_install(uint32_t start_page, uint32_t end_page) {
    if (!g_mem_logpoint_page_count)
        return;
    logpoints_active_adjust(+1);
    for (uint32_t p = start_page; p <= end_page && p < g_page_count; p++) {
        if (g_mem_logpoint_page_count[p] < 0xFFFF)
            g_mem_logpoint_page_count[p]++;
        else
            GS_ASSERTF(false, "logpoint refcount saturated on page %u", p);
        // Zero the SoA entries to force slow path for this page
        if (g_supervisor_read)
            g_supervisor_read[p] = 0;
        if (g_supervisor_write)
            g_supervisor_write[p] = 0;
        if (g_user_read)
            g_user_read[p] = 0;
        if (g_user_write)
            g_user_write[p] = 0;
    }
    if (g_mem_map_changed)
        g_mem_map_changed(); // CPU-side caches must drop bypassing entries
}

void memory_logpoint_uninstall(uint32_t start_page, uint32_t end_page) {
    if (!g_mem_logpoint_page_count)
        return;
    logpoints_active_adjust(-1);
    for (uint32_t p = start_page; p <= end_page && p < g_page_count; p++) {
        if (g_mem_logpoint_page_count[p])
            g_mem_logpoint_page_count[p]--;
        if (g_mem_logpoint_page_count[p] == 0)
            rebuild_soa_page(p);
    }
    if (g_mem_map_changed)
        g_mem_map_changed(); // CPU-side caches must drop bypassing entries
}

void memory_logpoint_install_phys(uint32_t start_page, uint32_t end_page) {
    if (!g_mem_logpoint_phys_page_count)
        return;
    logpoints_active_adjust(+1);
    for (uint32_t p = start_page; p <= end_page && p < g_page_count; p++) {
        if (g_mem_logpoint_phys_page_count[p] < 0xFFFF)
            g_mem_logpoint_phys_page_count[p]++;
        else
            GS_ASSERTF(false, "logpoint phys refcount saturated on page %u", p);
    }
    // We can't cheaply enumerate which logical pages currently alias the
    // watched physical pages, so conservatively invalidate the entire SoA
    // arrays.  All logical pages re-walk on next access, and the fill path
    // (mmu_fill_soa_entry) suppresses any alias hitting the watched physical.
    // One-time cost at install; fast-path unaffected once entries repopulate.
    soa_zero_all();
    // Every entry is zero now, so the population list starts over exact.
    if (g_installed_map)
        tlb_track_reset(g_installed_map);
    if (g_mem_map_changed)
        g_mem_map_changed(); // CPU-side caches must drop bypassing entries
}

void memory_logpoint_uninstall_phys(uint32_t start_page, uint32_t end_page) {
    if (!g_mem_logpoint_phys_page_count)
        return;
    logpoints_active_adjust(-1);
    for (uint32_t p = start_page; p <= end_page && p < g_page_count; p++) {
        if (g_mem_logpoint_phys_page_count[p])
            g_mem_logpoint_phys_page_count[p]--;
    }
    // No need to rebuild SoA entries; they refill lazily on next access.
    if (g_mem_map_changed)
        g_mem_map_changed(); // CPU-side caches must drop bypassing entries
}

// ============================================================================
// Operations
// ============================================================================

// Add a memory-mapped device to the memory map (linked list + page table)
void memory_map_add(memory_map_t *mem, uint32_t addr, uint32_t size, const char *name, memory_interface_t *iface,
                    void *device) {
    // Add to linked list (for memory_map_print / memory_map_remove)
    mapping_t *map = (mapping_t *)calloc(1, sizeof(mapping_t));
    GS_ASSERTF(map != NULL, "memory_map_add: out of memory allocating mapping for '%s'", name ? name : "?");

    map->name = strdup(name ? name : "");
    GS_ASSERTF(map->name != NULL, "memory_map_add: out of memory duplicating name '%s'", name ? name : "?");
    map->device = device;
    map->addr = addr;
    map->size = size;
    map->memory_interface = *iface;

    map->next = mem->map;
    mem->map = map;

    // Three registration mistakes that used to be silent.  All three are
    // init-only, so the cost is nil, and each one produced a mapping that
    // LOOKED registered -- it is in the linked list above and memory_map_print
    // shows it -- while claiming the wrong pages or none at all.
    //
    // 1. WRAP.  end_page is computed from `addr + size - 1` masked to the
    //    address space.  If that overflows 32 bits, or exceeds the 24-bit
    //    mask on a Plus or Lisa, end_page comes out BELOW start_page, the
    //    loop body never runs, and the region claims nothing.
    //    bart_claim_empty(cfg, 0xD0000000, 0x10000000, ...) sits one slot
    //    away from this.
    // 2. SUB-PAGE.  A region smaller than a page claims the whole page, so
    //    two sub-page devices sharing one page silently collide -- the
    //    second wins for the entire page, including the first one's bytes.
    // 3. OVERLAP.  A later registration overwrites an earlier one's page
    //    entries with no diagnostic, while the earlier mapping stays in the
    //    list.  That layering-by-call-order is load-bearing and documented
    //    only in prose (bart.c:262-264, "Called from the family memory
    //    layout, BEFORE nubus_init"), and bart.c:295-303 records a real bug
    //    caused by getting it wrong.
    if (size == 0)
        LOG(0, "memory_map_add('%s'): zero size at $%08X claims no pages", name ? name : "?", addr);
    if (addr + size - 1 < addr)
        LOG(0, "memory_map_add('%s'): $%08X + $%08X wraps the address space; the region will claim no pages",
            name ? name : "?", addr, size);
    // Measured across every ROM in tests/data: exactly one hit, the IIfx's
    // JMFB registering a 1 KB register window ('JMFB regs' at $F9200000+$400).
    // It claims the whole 4 KB page and nothing else is in that page today,
    // so it is a risk rather than a fault -- which is what level 2 is for.
    if ((addr & (MEM_PAGE_SIZE - 1)) != 0 || (size & (MEM_PAGE_SIZE - 1)) != 0)
        LOG(2,
            "memory_map_add('%s'): $%08X+$%08X is not page-aligned; it claims whole pages and can collide with a "
            "neighbour in the same page",
            name ? name : "?", addr, size);

    // Populate page table entries for the device's address range
    if (g_page_table) {
        uint32_t start_page = (addr & g_address_mask) >> MEM_PAGE_SHIFT;
        uint32_t end_page = ((addr + size - 1) & g_address_mask) >> MEM_PAGE_SHIFT;
        assert(start_page < g_page_count && "device start address exceeds page table bounds");
        if (end_page < start_page)
            LOG(0, "memory_map_add('%s'): page range $%X..$%X is inverted; the region claims no pages",
                name ? name : "?", start_page, end_page);
        for (uint32_t p = start_page; p <= end_page && p < g_page_count; p++) {
            // Overlap: another device already owns this page.  Layering by
            // call order is intentional in places, so this is a log and not
            // a refusal -- but it must be visible.
            if (g_page_table[p].dev && g_page_table[p].dev != &map->memory_interface)
                LOG(2, "memory_map_add('%s'): page $%X was already claimed by a device at $%08X; replacing it",
                    name ? name : "?", p, g_page_table[p].base_addr);
            // AoS cold-path: register device handler
            g_page_table[p].host_base = NULL;
            g_page_table[p].dev = &map->memory_interface;
            g_page_table[p].dev_context = device;
            g_page_table[p].base_addr = addr;
            g_page_table[p].writable = false;

            // SoA fast-path: zero entries force slow path for device I/O
            if (g_supervisor_read)
                g_supervisor_read[p] = 0;
            if (g_supervisor_write)
                g_supervisor_write[p] = 0;
            if (g_user_read)
                g_user_read[p] = 0;
            if (g_user_write)
                g_user_write[p] = 0;
        }
    }
    if (g_mem_map_changed)
        g_mem_map_changed(); // fetch caches hold host pointers the SoA cannot evict
}

// Clear page-table entries for [addr, addr+size) that point at `iface_ptr`.
// Called by memory_map_remove before freeing the mapping so the page table
// stops dereferencing a freed memory_interface.
static void clear_page_table_for_mapping(uint32_t addr, uint32_t size, const memory_interface_t *iface_ptr) {
    if (!g_page_table || size == 0)
        return;
    uint32_t start_page = (addr & g_address_mask) >> MEM_PAGE_SHIFT;
    uint32_t end_page = ((addr + size - 1) & g_address_mask) >> MEM_PAGE_SHIFT;
    for (uint32_t p = start_page; p <= end_page && p < g_page_count; p++) {
        if (g_page_table[p].dev == iface_ptr) {
            g_page_table[p].host_base = NULL;
            g_page_table[p].dev = NULL;
            g_page_table[p].dev_context = NULL;
            g_page_table[p].base_addr = 0;
            g_page_table[p].writable = false;
        }
    }
    if (g_mem_map_changed)
        g_mem_map_changed(); // ditto: a freed window may still be cached by PC
}

// Remove a memory-mapped device from the memory map: the mapping is found by
// device and address, and its own size is used
void memory_map_remove(memory_map_t *memory_map, uint32_t addr, void *device) {
    if (!memory_map || !memory_map->map)
        return; // empty list — nothing to remove
    mapping_t *map = memory_map->map;

    if (map->device == device && map->addr == addr) {
        clear_page_table_for_mapping(map->addr, map->size, &map->memory_interface);
        free(map->name);
        memory_map->map = map->next;
        free(map);
    } else
        while (map->next != NULL) {
            if (map->next->device == device && map->next->addr == addr) {
                mapping_t *tmp = map->next;
                clear_page_table_for_mapping(tmp->addr, tmp->size, &tmp->memory_interface);
                free(map->next->name);
                map->next = map->next->next;
                free(tmp);
                break;
            } else
                map = map->next;
        }
}

uint8_t *ram_native_pointer(memory_map_t *mem, uint32_t addr) {
    return mem->image + addr;
}

uint32_t memory_ram_size(memory_map_t *mem) {
    return mem ? mem->ram_size : 0;
}

const char *memory_rom_filename(memory_map_t *mem) {
    return mem ? mem->rom_filename : NULL;
}

// Direct read access to the ROM region (read-only). Returns NULL if the
// memory map has no ROM bytes loaded yet.
const uint8_t *memory_rom_bytes(memory_map_t *mem) {
    return (mem && mem->image) ? mem->image + mem->ram_size : NULL;
}

uint32_t memory_rom_size(memory_map_t *mem) {
    return mem ? mem->rom_size : 0;
}

// ============================================================================
// Lifecycle: Page Table Population
// ============================================================================

// Populate page table entries for RAM (writable) and ROM (read-only) regions.
// RAM pages cover [0, ram_size); ROM pages cover [rom_start_addr, rom_region_end)
// with mirroring: guest ROM addresses wrap at rom_size within the host buffer.
// Called from machine-specific layout callbacks (e.g. plus_memory_layout_init).
void memory_populate_pages(memory_map_t *mem, uint32_t rom_start_addr, uint32_t rom_region_end) {
    if (!g_page_table || !mem->image)
        return;

    uint32_t ram_size = mem->ram_size;
    uint32_t rom_size = mem->rom_size;

    // RAM pages: 0x000000 – ram_size (writable, direct access)
    uint32_t ram_pages = ram_size >> MEM_PAGE_SHIFT;
    for (uint32_t p = 0; p < ram_pages && p < g_page_count; p++) {
        assert((p << MEM_PAGE_SHIFT) < ram_size && "RAM page index out of bounds");
        uint8_t *host_ptr = mem->image + (p << MEM_PAGE_SHIFT);
        uint32_t guest_base = p << MEM_PAGE_SHIFT;
        uintptr_t adjusted = (uintptr_t)host_ptr - guest_base;

        // AoS cold-path entry (device dispatch)
        g_page_table[p].host_base = host_ptr;
        g_page_table[p].dev = NULL;
        g_page_table[p].dev_context = NULL;
        g_page_table[p].writable = true;

        // SoA fast-path entries: RAM is readable and writable by all
        if (g_supervisor_read)
            g_supervisor_read[p] = adjusted;
        if (g_supervisor_write)
            g_supervisor_write[p] = adjusted;
        if (g_user_read)
            g_user_read[p] = adjusted;
        if (g_user_write)
            g_user_write[p] = adjusted;
        memory_logpoint_guard_page(p);
    }

    // ROM pages: rom_start_addr – rom_region_end (read-only, mirrored)
    // On Mac hardware, address line A17 (relative to ROM base) distinguishes ROM from I/O:
    //   offset % (2*rom_size) in [0, rom_size)       → ROM content
    //   offset % (2*rom_size) in [rom_size, 2*rom_size) → I/O / undefined (left unmapped)
    // This produces ROM mirrors at every 2*rom_size (256 KB for the 128 KB Plus ROM),
    // matching the old flat-buffer copy loop: addr += 2 * ROM_SIZE.
    uint32_t rom_start_page = (rom_start_addr & g_address_mask) >> MEM_PAGE_SHIFT;
    uint32_t rom_end_page = (rom_region_end & g_address_mask) >> MEM_PAGE_SHIFT;
    uint32_t rom_pages = rom_size >> MEM_PAGE_SHIFT; // content pages per ROM copy
    uint32_t mirror_stride = rom_pages * 2; // mirror cycle in pages (A17 stride)

    // Protect against a zero-length ROM (degenerate case)
    if (rom_pages == 0)
        return;

    for (uint32_t p = rom_start_page; p < rom_end_page && p < g_page_count; p++) {
        uint32_t offset_in_cycle = (p - rom_start_page) % mirror_stride;
        if (offset_in_cycle >= rom_pages)
            continue; // interleaved I/O / undefined range — leave page unmapped (returns 0)
        uint8_t *host_ptr = mem->image + ram_size + (offset_in_cycle << MEM_PAGE_SHIFT);
        uint32_t guest_base = p << MEM_PAGE_SHIFT;
        uintptr_t adjusted = (uintptr_t)host_ptr - guest_base;

        // AoS cold-path entry
        g_page_table[p].host_base = host_ptr;
        g_page_table[p].dev = NULL;
        g_page_table[p].dev_context = NULL;
        g_page_table[p].writable = false;

        // SoA fast-path entries: ROM is read-only (write entries stay 0 → slow path)
        if (g_supervisor_read)
            g_supervisor_read[p] = adjusted;
        if (g_user_read)
            g_user_read[p] = adjusted;
        memory_logpoint_guard_page(p);
    }
}

// Populate page-table entries for a RAM-mirror region.  Each page in
// [mirror_start, mirror_end) is set up as a full read+write alias of RAM at
// (guest_page << MEM_PAGE_SHIFT) % ram_size.  This models the physical
// address-bus wraparound on a Macintosh Plus: high address bits beyond the
// installed RAM range are undecoded, so accesses in the unmapped gap fold
// down into actual RAM.  The Plus ROM relies on this for its exception save
// area at $3FFC80, which physically points into installed RAM on any Plus
// with less than 4 MB.
void memory_populate_ram_mirror(memory_map_t *mem, uint32_t mirror_start, uint32_t mirror_end) {
    if (!g_page_table || !mem->image || mem->ram_size == 0)
        return;
    if (mirror_end <= mirror_start)
        return;

    uint32_t ram_size = mem->ram_size;
    uint32_t start_page = (mirror_start & g_address_mask) >> MEM_PAGE_SHIFT;
    uint32_t end_page = (mirror_end & g_address_mask) >> MEM_PAGE_SHIFT;

    for (uint32_t p = start_page; p < end_page && p < g_page_count; p++) {
        uint32_t guest_base = p << MEM_PAGE_SHIFT;
        uint32_t ram_off = guest_base % ram_size;
        uint8_t *host_ptr = mem->image + ram_off;
        uintptr_t adjusted = (uintptr_t)host_ptr - guest_base;

        // AoS cold-path entry: same RAM image, just aliased at a different
        // guest base.  Marked writable so the slow path treats it like RAM.
        g_page_table[p].host_base = host_ptr;
        g_page_table[p].dev = NULL;
        g_page_table[p].dev_context = NULL;
        g_page_table[p].writable = true;

        // SoA fast-path: full read+write on both supervisor and user sides.
        if (g_supervisor_read)
            g_supervisor_read[p] = adjusted;
        if (g_supervisor_write)
            g_supervisor_write[p] = adjusted;
        if (g_user_read)
            g_user_read[p] = adjusted;
        if (g_user_write)
            g_user_write[p] = adjusted;
        memory_logpoint_guard_page(p);
    }
}

// ============================================================================
// Lifecycle: Constructor
// ============================================================================
// Selection: the fast-path aliases
// ============================================================================

void memory_map_select(memory_map_t *mem) {
    g_installed_map = mem;
    g_page_table = mem ? mem->page_table : NULL;
    g_page_count = mem ? (uint32_t)mem->page_count : 0;
    g_address_mask = mem ? mem->address_mask : 0;
    g_supervisor_read = mem ? mem->supervisor_read : NULL;
    g_supervisor_write = mem ? mem->supervisor_write : NULL;
    g_user_read = mem ? mem->user_read : NULL;
    g_user_write = mem ? mem->user_write : NULL;
    // Supervisor until the CPU picks the active pair: a 68K at its next
    // sprint, the PowerPC through its selected hook below.
    g_active_read = g_supervisor_read;
    g_active_write = g_supervisor_write;
    g_mem_logpoint_page_count = mem ? mem->logpoint_page_count : NULL;
    g_mem_logpoint_phys_page_count = mem ? mem->logpoint_phys_page_count : NULL;
    g_mem_logpoints_active = mem ? mem->logpoints_active : 0;
    g_bus_err_lo = mem ? mem->bus_err.lo : MEMORY_BUS_ERR_NONE.lo;
    g_bus_err_hi = mem ? mem->bus_err.hi : MEMORY_BUS_ERR_NONE.hi;
    g_user_soa_reserved = mem ? mem->cpu_hooks.user_soa_reserved : false;
    g_mem_map_changed = mem ? mem->cpu_hooks.map_changed : NULL;
    g_mem_logical_xlate = mem ? mem->cpu_hooks.logical_xlate : NULL;
    g_mem_logical_xlate_ctx = mem ? mem->cpu_hooks.ctx : NULL;
    g_mem_host_fill = mem ? mem->host_fill : NULL;
    g_mmu = mem ? mem->pmmu : NULL;
    g_lisa_mmu = mem ? mem->lisa_mmu : NULL;
    if (mem && mem->cpu_hooks.selected)
        mem->cpu_hooks.selected(mem->cpu_hooks.ctx);
}

void memory_map_set_cpu_hooks(memory_map_t *mem, const memory_cpu_hooks_t *hooks) {
    mem->cpu_hooks = *hooks;
    if (g_installed_map == mem)
        memory_map_select(mem);
}

void memory_map_set_host_fill(memory_map_t *mem, void (*fill)(uint32_t page_index, uint8_t *host_ptr, bool writable)) {
    mem->host_fill = fill;
    if (g_installed_map == mem)
        g_mem_host_fill = fill;
}

void memory_map_set_pmmu(memory_map_t *mem, struct mmu_state *mmu) {
    mem->pmmu = mmu;
    if (g_installed_map == mem)
        g_mmu = mmu;
}

void memory_map_set_lisa_mmu(memory_map_t *mem, struct lisa_mmu *mmu) {
    mem->lisa_mmu = mmu;
    if (g_installed_map == mem)
        g_lisa_mmu = mmu;
}

// === Host-backed regions on the bus map ======================================

// Fill one host window through the hook, page by page.
static void host_fill_region(uint8_t *host_ptr, uint32_t phys_base, uint32_t size, bool writable) {
    for (uint32_t off = 0; off < size; off += MEM_PAGE_SIZE)
        g_mem_host_fill((phys_base + off) >> MEM_PAGE_SHIFT, host_ptr + off, writable);
}

// Runs only at machine build, on the map under construction -- the selected
// one, whose aliases (g_mem_host_fill, g_mmu) this reads.
void memory_map_host_region(memory_map_t *m, const char *name, uint8_t *host_ptr, uint32_t phys_base, uint32_t size,
                            bool writable) {
    (void)m; // the selected map: the one under construction
    (void)name; // regions are matched by physical window, not name
    // A machine that installed a page-fill hook owns its own physical view
    // (the PowerPC families, whose page table no mmu_state_t manages).  The
    // hook -- not the absence of g_mmu -- is the discriminator.
    memory_map_t *sel = g_installed_map;
    if (g_mem_host_fill && sel) {
        if (!host_ptr || size == 0)
            return;
        host_fill_region(host_ptr, phys_base, size, writable);
        // Re-registration of the same window replaces its record (same rule
        // as mmu_register_host_region).
        for (int i = 0; i < sel->host_fill_count; i++) {
            mem_host_fill_region_t *r = &sel->host_fill_regions[i];
            if (r->phys_base == phys_base && r->size == size) {
                r->host = host_ptr;
                r->writable = writable;
                if (g_mem_map_changed)
                    g_mem_map_changed();
                return;
            }
        }
        if (sel->host_fill_count >= MEM_HOST_FILL_MAX) {
            LOG(0, "memory_map_host_region: fill list full (%d); region $%08X+$%X not recorded", MEM_HOST_FILL_MAX,
                phys_base, size);
            return;
        }
        sel->host_fill_regions[sel->host_fill_count++] =
            (mem_host_fill_region_t){.host = host_ptr, .phys_base = phys_base, .size = size, .writable = writable};
        if (g_mem_map_changed)
            g_mem_map_changed();
        return;
    }
    // 68k: the PMMU's host-region list, which its table walks resolve through
    mmu_register_host_region(g_mmu, host_ptr, phys_base, size, writable);
    if (g_mem_map_changed)
        g_mem_map_changed(); // fetch caches hold host pointers the SoA cannot evict
}

void memory_map_host_region_alias(memory_map_t *m, uint32_t alias_phys_base, uint32_t original_phys_base) {
    (void)m; // the selected map, as above
    memory_map_t *sel = g_installed_map;
    if (g_mem_host_fill && sel) {
        // Machine-owned physical view: the alias is a second page fill of the
        // same host bytes.  Card register windows are registered AFTER their
        // aliases (display_card_24ac.c), so a device page still wins its page.
        for (int i = 0; i < sel->host_fill_count; i++) {
            const mem_host_fill_region_t *r = &sel->host_fill_regions[i];
            if (r->phys_base == original_phys_base) {
                host_fill_region(r->host, alias_phys_base, r->size, r->writable);
                return;
            }
        }
    } else if (mmu_register_host_region_alias(g_mmu, alias_phys_base, original_phys_base)) {
        return;
    }
    LOG(1, "memory_map_host_region_alias: no host region at phys $%08X; alias $%08X dropped", original_phys_base,
        alias_phys_base);
}

// ============================================================================

// Allocate and initialise a memory map for the given address space and RAM/ROM sizes.
// The page table is allocated dynamically based on address_bits.
// Machine-specific memory layout (page table population) is done by the machine's
// memory_layout_init callback, not here.
memory_map_t *memory_map_init(int address_bits, uint32_t ram_size, uint32_t rom_size, memory_bus_err_window_t bus_err,
                              const rom_image_t *rom, checkpoint_t *checkpoint) {
    // Validate address_bits before deciding the page-table shape so a 28 or 0
    // doesn't silently default to the 24-bit layout.
    GS_ASSERTF(address_bits == 24 || address_bits == 32, "memory_map_init: address_bits must be 24 or 32 (got %d)",
               address_bits);
    memory_map_t *mem = (memory_map_t *)calloc(1, sizeof(memory_map_t));
    GS_ASSERTF(mem != NULL, "memory_map_init: out of memory allocating memory_map_t");

    // Store parameterised sizes for later use by layout, checkpoint, and cmd_rom
    mem->ram_size = ram_size;
    mem->rom_size = rom_size;

    // The board's bus-error window (the slow path's alias while selected).
    mem->bus_err = bus_err;

    // The layout fills (memory_populate_pages, memory_populate_ram_mirror and
    // the machines' own page fills) write SoA entries before anything
    // tracks them, so the tracker starts overflowed: the first real
    // invalidation zeroes the whole arrays, and the list is exact from then
    // on.  On a machine whose MMU never invalidates (no PMMU) it simply
    // stays that way, unused.
    mem->track_overflow = true;

    // Allocate the flat RAM+ROM image (ram_size + rom_size bytes)
    size_t image_size = (size_t)ram_size + (size_t)rom_size;
    mem->image = calloc(1, image_size);
    GS_ASSERTF(mem->image != NULL, "memory_map_init: out of memory allocating %zu-byte image", image_size);

    // Allocate page table sized for the given address space
    if (address_bits == 32) {
        mem->address_mask = 0xFFFFFFFFUL; // full 32-bit
        mem->page_count = 1 << (32 - MEM_PAGE_SHIFT); // 1,048,576 pages
    } else {
        // 24-bit (Macintosh Plus / SE)
        mem->address_mask = 0x00FFFFFFUL;
        mem->page_count = 1 << (24 - MEM_PAGE_SHIFT); // 4,096 pages
    }
    size_t pages = (size_t)mem->page_count;

    // AoS cold-path page table (device dispatch)
    mem->page_table = (page_entry_t *)calloc(pages, sizeof(page_entry_t));
    assert(mem->page_table != NULL && "failed to allocate page table");

    // SoA fast-path arrays (calloc → zero = slow path for all pages initially)
    mem->supervisor_read = (uintptr_t *)calloc(pages, sizeof(uintptr_t));
    mem->supervisor_write = (uintptr_t *)calloc(pages, sizeof(uintptr_t));
    mem->user_read = (uintptr_t *)calloc(pages, sizeof(uintptr_t));
    mem->user_write = (uintptr_t *)calloc(pages, sizeof(uintptr_t));
    assert(mem->supervisor_read && mem->supervisor_write && mem->user_read && mem->user_write);

    // Memory logpoint reference-count array (zero = no logpoint on that page),
    // and the physical-page one, sized the same way so any physical page the
    // guest can reach is coverable.
    mem->logpoint_page_count = (uint16_t *)calloc(pages, sizeof(uint16_t));
    mem->logpoint_phys_page_count = (uint16_t *)calloc(pages, sizeof(uint16_t));
    assert(mem->logpoint_page_count && mem->logpoint_phys_page_count);

    // The machine under construction builds into its own map: select it.
    memory_map_select(mem);

    // The ROM is on the board from power-on: the region is created filled.
    if (rom && rom->data && rom->size) {
        memcpy(mem->image + ram_size, rom->data, rom->size < rom_size ? rom->size : rom_size);
        if (rom->path)
            mem->rom_filename = strdup(rom->path);
    }

    // Load from checkpoint if provided
    if (checkpoint) {
        // Restore RAM
        system_read_checkpoint_data(checkpoint, mem->image, ram_size);
        // Restore ROM (content or reference)
        char *restored_path = NULL;
        checkpoint_read_file(checkpoint, mem->image + ram_size, rom_size, &restored_path);
        if (restored_path) {
            if (mem->rom_filename)
                free(mem->rom_filename);
            mem->rom_filename = restored_path;
        }
    }

    // Object-tree binding — instance_data is the memory_map_t itself,
    // memory.peek's accessors call into the global memory_read_* helpers
    // directly so its instance_data is unused.
    mem->memory_object = memory_object_new(mem, &mem->peek_object, &mem->poke_object);

    return mem;
}

// ============================================================================
// Lifecycle: Destructor
// ============================================================================

// Free resources associated with a memory map instance
void memory_map_delete(memory_map_t *mem) {
    if (!mem)
        return;
    memory_object_delete(mem->memory_object, mem->peek_object, mem->poke_object);
    mem->memory_object = mem->peek_object = mem->poke_object = NULL;
    // Free mappings
    mapping_t *m = mem->map;
    while (m) {
        mapping_t *next = m->next;
        if (m->name)
            free(m->name);
        free(m);
        m = next;
    }
    // Deselect it if it is the selected map, then free what it owns.
    if (g_installed_map == mem)
        memory_map_select(NULL);
    free(mem->page_table);
    free(mem->track_page);
    free(mem->supervisor_read);
    free(mem->supervisor_write);
    free(mem->user_read);
    free(mem->user_write);
    free(mem->logpoint_page_count);
    free(mem->logpoint_phys_page_count);
    // Free RAM/ROM image buffer
    if (mem->image) {
        free(mem->image);
        mem->image = NULL;
    }
    // Free ROM filename if present
    if (mem->rom_filename) {
        free(mem->rom_filename);
        mem->rom_filename = NULL;
    }
    free(mem);
}

// ============================================================================
// Lifecycle: Checkpointing
// ============================================================================

// Save memory state to a checkpoint
void memory_map_checkpoint(memory_map_t *restrict mem, checkpoint_t *checkpoint) {
    if (!mem || !checkpoint)
        return;

    // Write RAM contents
    system_write_checkpoint_data(checkpoint, mem->image, mem->ram_size);

    // Write ROM: either inline contents (default) or by filename reference depending on save mode
    checkpoint_write_file(checkpoint, mem->rom_filename ? mem->rom_filename : "");
}

// ============================================================================
// Operations
// ============================================================================

// Print the current memory map to stdout
void memory_map_print(memory_map_t *restrict mem) {
    mapping_t *map = mem->map;

    while (map != NULL) {

        gs_outf("0x%08x - 0x%08x: %s\n", map->addr, map->addr + map->size - 1, map->name);
        map = map->next;
    }
}
