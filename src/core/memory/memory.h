// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// memory.h
// Public interface for memory map management.

#ifndef MEMORY_H
#define MEMORY_H

// === Includes ===
#include "checkpoint.h"
#include "common.h"
#include "machine_build_opts.h" // rom_image_t

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// === Macros ===
// Big-endian access through a host pointer.  The guest may address any of
// these at an odd byte, and the 68020+ and PowerPC cores both permit
// misaligned data operands, so the access is expressed as a memcpy rather
// than a pointer cast: casting is undefined for both alignment and strict
// aliasing, and it is the C abstract machine imposing that requirement, not
// the emulated hardware.  The compiler folds the memcpy away -- the emitted
// code is byte-identical on aarch64 (gcc), x86-64 (clang) and wasm32 (emcc),
// for every width, loads and stores; on wasm the only delta is that the
// alignment hint becomes honest (p2align=0) instead of a false claim of
// natural alignment.  For on-disk and on-wire buffers, common.h's
// RD_BE*/WR_BE* remain the byte-wise spelling.
#define LOAD_BE8(p)  (*(const uint8_t *)(p))
#define LOAD_BE16(p) (__builtin_bswap16(memory_load_host16(p)))
#define LOAD_BE32(p) (__builtin_bswap32(memory_load_host32(p)))

#define STORE_BE8(p, v)  (*(uint8_t *)(p) = (uint8_t)(v))
#define STORE_BE16(p, v) memory_store_host16((p), __builtin_bswap16((uint16_t)(v)))
#define STORE_BE32(p, v) memory_store_host32((p), __builtin_bswap32((uint32_t)(v)))

// Alignment-agnostic host-memory primitives behind the macros above.
static inline uint16_t memory_load_host16(const void *p) {
    uint16_t v;
    __builtin_memcpy(&v, p, sizeof v);
    return v;
}
static inline uint32_t memory_load_host32(const void *p) {
    uint32_t v;
    __builtin_memcpy(&v, p, sizeof v);
    return v;
}
static inline void memory_store_host16(void *p, uint16_t v) {
    __builtin_memcpy(p, &v, sizeof v);
}
static inline void memory_store_host32(void *p, uint32_t v) {
    __builtin_memcpy(p, &v, sizeof v);
}

// === Type Definitions ===
struct mmu_state;
struct lisa_mmu;

// A device's bus interface.  `read_*` is the guest's access and may have the
// hardware's read side effects (a status toggle, a RAMDAC phase step, a FIFO
// pop, a flag cleared on read).  `peek_*` is an inspection: it returns what
// the matching read would return now and changes nothing in the device.  The
// debugger's reads (memory.peek/.dump, find.*, the Mac-globals and
// disassembly views) use peek_* and fall back to read_* where it is NULL, so
// a device whose reads have any side effect MUST implement peek_* (#159).
// A missing width composes from the narrower peek, as reads do.
typedef struct memory_interface {
    uint8_t (*read_uint8)(void *device, uint32_t addr);
    uint16_t (*read_uint16)(void *device, uint32_t addr);
    uint32_t (*read_uint32)(void *device, uint32_t addr);
    void (*write_uint8)(void *device, uint32_t addr, uint8_t data);
    void (*write_uint16)(void *device, uint32_t addr, uint16_t data);
    void (*write_uint32)(void *device, uint32_t addr, uint32_t data);
    uint8_t (*peek_uint8)(void *device, uint32_t addr);
    uint16_t (*peek_uint16)(void *device, uint32_t addr);
    uint32_t (*peek_uint32)(void *device, uint32_t addr);
} memory_interface_t;

// A byte read through `m` on behalf of an I/O island that fronts the device:
// the guest's read, or, for an inspection (`peek`), the device's peek_uint8
// where it has one.  An island's own peek routes through this so the device
// behind it is inspected, not read.
static inline uint8_t memory_iface_read8(const memory_interface_t *m, void *dev, uint32_t addr, bool peek) {
    if (peek && m->peek_uint8)
        return m->peek_uint8(dev, addr);
    return m->read_uint8 ? m->read_uint8(dev, addr) : 0xFF;
}

struct memory;
typedef struct memory memory_map_t;

// The board's bus-error window: the inclusive address range where "no chip
// answered" ends in a bus error (the board's watchdog) instead of the bus
// floating to $FF.  A property of the board's bus, fixed at construction.
// lo > hi is an empty window.
typedef struct memory_bus_err_window {
    uint32_t lo;
    uint32_t hi;
} memory_bus_err_window_t;

// A board with no bus-error watchdog: every unanswered access floats to $FF.
#define MEMORY_BUS_ERR_NONE ((memory_bus_err_window_t){.lo = 1u, .hi = 0u})

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

// Initialise a memory map with parameterised address space and RAM/ROM sizes.
// address_bits: 24 for Plus (16 MB), 32 for SE/30 (4 GB)
// ram_size: RAM size in bytes (e.g. 0x400000 for Plus)
// rom_size: ROM size in bytes (e.g. 0x020000 for Plus)
// bus_err: the board's bus-error window (MEMORY_BUS_ERR_NONE for none)
// rom: the ROM to build the region with (copied in; NULL or empty leaves it
//      zero), or nothing on a restore
// checkpoint: if non-NULL, restore RAM and ROM from checkpoint
extern memory_map_t *memory_map_init(int address_bits, uint32_t ram_size, uint32_t rom_size,
                                     memory_bus_err_window_t bus_err, const rom_image_t *rom, checkpoint_t *checkpoint);

void memory_map_delete(memory_map_t *mem);

// The fast-path globals (g_page_table, the SoA arrays, the logpoint counts,
// g_address_mask, the CPU hooks, g_mmu / g_lisa_mmu, the bus-error window)
// are ALIASES of one memory map's own state: the map selected here.
// memory_map_init selects the map it creates, so the machine under
// construction builds into its own map; the swap step selects the active
// machine's (NULL clears every alias).  A map that is not selected keeps its
// state, and deleting it touches no alias.
void memory_map_select(memory_map_t *mem);

// What the main CPU hangs on its map (the PowerPC front end): see
// g_user_soa_reserved, g_mem_map_changed and g_mem_logical_xlate below.
typedef struct memory_cpu_hooks {
    bool user_soa_reserved;
    void (*map_changed)(void);
    uint32_t (*logical_xlate)(void *ctx, uint32_t addr, bool *ok);
    // The map was selected: the CPU's process-wide translation caches describe
    // whichever map was selected before, so they start over.
    void (*selected)(void *ctx);
    void *ctx; // passed to logical_xlate and selected
} memory_cpu_hooks_t;
void memory_map_set_cpu_hooks(memory_map_t *mem, const memory_cpu_hooks_t *hooks);

// The machine's physical page-fill hook (g_mem_host_fill), its 68030 PMMU
// (g_mmu) and the Lisa's MMU (g_lisa_mmu): state of the map, aliased while it
// is selected.
void memory_map_set_host_fill(memory_map_t *mem, void (*fill)(uint32_t page_index, uint8_t *host_ptr, bool writable));
void memory_map_set_pmmu(memory_map_t *mem, struct mmu_state *mmu);
void memory_map_set_lisa_mmu(memory_map_t *mem, struct lisa_mmu *mmu);

void memory_map_checkpoint(memory_map_t *restrict mem, checkpoint_t *checkpoint);

// === Operations ===

extern void memory_map_add(memory_map_t *mem, uint32_t addr, uint32_t size, const char *name, memory_interface_t *iface,
                           void *device);

// Register a host-backed region on the physical bus map.  `writable`
// distinguishes RAM-shaped (VRAM, framebuffer) from ROM-shaped (declrom)
// regions.  The NuBus bus controller calls this once per card region
// during nubus_init().  Defined in memory.c: on a machine whose page table
// no 68k MMU manages (the PowerPC families) the map fills the pages itself
// through its page-fill hook and keeps the window in its own table; on a 68k
// machine the region joins the PMMU's host-region list (mmu_state_t), which
// its table walks resolve through.  No fast-path change.
void memory_map_host_region(memory_map_t *m, const char *name, uint8_t *host_ptr, uint32_t phys_base, uint32_t size,
                            bool writable);

// Mirror an already-registered host region at a second physical address.
// Used on SE/30 where the same VRAM is reachable via TT identity-map at
// $FEE00000 and via the page-table-mapped alias at $50FE0000.
void memory_map_host_region_alias(memory_map_t *m, uint32_t alias_phys_base, uint32_t original_phys_base);

// Latch a deferred bus error for a DEVICE-decoded access the hardware
// terminates with a transfer error.  The CPU seam delivers it at the sprint
// boundary (68k bus error / 601 machine check).  No-op while an inspection
// access is in flight, so `memory.peek` of such a window never perturbs the
// guest.
//
// What this models is a BUS TIMEOUT, not a decode failure: on real hardware
// no chip "returns" a bus error.  A cycle completes only if some responder
// asserts acknowledge (/DSACK on 68020/030, /TA on 68040); if none does, the
// glue's watchdog fires and asserts the error instead.  Hence the two
// distinct behaviours a machine must choose between for an address with no
// chip behind it, per the address-map legend in Guide to the Macintosh Family
// Hardware 2e p.121:
//
//   * DECODED but unpopulated ("light-shaded: decoded but might not be
//     used") — the decoder acknowledges anyway, the cycle completes, and the
//     data bus floats to the pull-ups.  Reads return $FF; NO fault.  This is
//     what mac030_board_desc_t.io_unmapped_read models.
//   * NOT DECODED ("unshaded") — nothing acknowledges, the watchdog times
//     out, bus error.  This is what bus_err_lo/bus_err_hi model for whole
//     regions, and what THIS function models for an individual device window
//     whose responder is absent on a particular model.
//
// Documented watchdogs: IIfx OSS "bus time-out logic" (DSACK asserted only
// for the shaded I/O spaces); Q900/950 Relayer 15.5 us @ 33 MHz, 20.5 us
// @ 25 MHz; AV MCA 16 us, 32 us in NuBus space; PDM AMIC 40 us; NuBus
// 25.6 us.  Callers today: the PDM's BART slot windows (empty slot), the
// IIfx FMC/RPU probe windows, and the 660AV's absent MUNI — the AV devnote
// states it outright ("if the CPU, PSC, or MUNI does not respond to a cycle
// start signal within a critical time, the MCA ... issues a bus error").
void memory_signal_bus_error(uint32_t addr, bool write);

// Physical page-fill hook for machines whose page table is not owned by a
// 68k mmu_state_t (the PowerPC families).  When set, memory_map_host_region()
// routes each page of a card-registered host region through it instead of
// the 68k MMU's host-region list.  See src/machines/pdm/pdm.c.  Alias of the
// selected map's (memory_map_set_host_fill).
extern void (*g_mem_host_fill)(uint32_t page_index, uint8_t *host_ptr, bool writable);

// True when an unanswered access at `addr` should raise a bus error rather
// than float to $FF: inside the installed map's bus-error window.  The window
// is a BUS property, so it applies with the MMU on or off -- see memory.c.
bool memory_addr_faults_when_unmapped(uint32_t addr);

extern void memory_map_remove(memory_map_t *mem, uint32_t addr, uint32_t size, const char *name,
                              memory_interface_t *iface, void *device);

extern void memory_map_print(memory_map_t *mem);

// Host pointer to guest RAM byte `addr`: exactly mem->image + addr.  Never
// NULL for a live map; no bounds check and no address folding (the caller
// keeps addr inside installed RAM).  The image is heap-aligned, so the
// pointer has addr's alignment -- an even addr may be read as uint16_t.
// The bytes are guest (big-endian) order.
uint8_t *ram_native_pointer(memory_map_t *ram, uint32_t addr);

// Installed RAM size in bytes (e.g. 0x100000 for a 1 MB Plus).
uint32_t memory_ram_size(memory_map_t *mem);

// Return the filename of the currently loaded ROM, or NULL if none.
const char *memory_rom_filename(memory_map_t *mem);

// Direct accessors for the ROM region. The returned pointer is owned by the
// memory map and valid for its lifetime: the ROM is filled at construction
// and never replaced.
const uint8_t *memory_rom_bytes(memory_map_t *mem);
uint32_t memory_rom_size(memory_map_t *mem);

uint32_t memory_read(unsigned int size, uint32_t addr);

void memory_write(unsigned int size, uint32_t addr, uint32_t value);

// Populate page table entries for RAM (writable) and ROM (read-only) regions.
// ROM pages in [rom_start_addr, rom_region_end) are populated with page-table
// mirroring: guest addresses wrap at rom_size so the ROM content repeats.
// Called from machine-specific layout callbacks (e.g. plus_memory_layout_init).
extern void memory_populate_pages(memory_map_t *mem, uint32_t rom_start_addr, uint32_t rom_region_end);

// Populate page table entries for a RAM-mirror region.  Each guest address in
// [mirror_start, mirror_end) aliases the corresponding RAM byte at
// `(guest_addr % ram_size)`, with full read+write access.  On a Mac Plus with
// less than 4 MB of RAM, the unmapped gap between top-of-RAM and the ROM
// region wraps physically through the address decoder back into installed
// RAM — the ROM's exception save area at $3FFC80 relies on this.  Call this
// from the machine layout callback for any machine that needs that
// behaviour (Plus does; explicitly-decoded machines like the SE/30 do not).
extern void memory_populate_ram_mirror(memory_map_t *mem, uint32_t mirror_start, uint32_t mirror_end);

// === Page Table ===

#define PAGE_SHIFT    12
#define MEM_PAGE_SIZE (1 << PAGE_SHIFT) // 4096
#define PAGE_MASK     (MEM_PAGE_SIZE - 1) // 0xFFF

// Each page maps to either a direct host pointer or a device handler
typedef struct page_entry {
    uint8_t *host_base; // non-NULL: direct access (RAM/ROM)
    const memory_interface_t *dev; // non-NULL: device-mapped I/O
    void *dev_context; // opaque device context for dev callbacks
    uint32_t base_addr; // base address of device mapping (subtracted before calling dev)
    bool writable; // true for RAM pages, false for ROM/I/O
} page_entry_t;

// Global page table and address mask (set by memory_map_init)
extern page_entry_t *g_page_table;
extern uint32_t g_address_mask; // 0x00FFFFFF for 24-bit, 0xFFFFFFFF for 32-bit
extern uint32_t g_page_count; // total number of pages in the current page table

// SoA (Struct-of-Arrays) fast-path page tables.
// Each entry stores an adjusted host address: (uintptr_t)host_base - page_guest_base
// so that (uint8_t *)(entry + masked_addr) yields the correct host pointer directly.
// Zero entry = slow path (device I/O, unmapped, or MMU miss).
extern uintptr_t *g_supervisor_read; // supervisor-mode read mapping
extern uintptr_t *g_supervisor_write; // supervisor-mode write mapping (RAM only)
extern uintptr_t *g_user_read; // user-mode read mapping
extern uintptr_t *g_user_write; // user-mode write mapping

// SoA population tracking: the selected map lists the page indices written
// into the SoA arrays since the last invalidation.  Every writer of a
// fast-path entry outside a plain layout fill calls tlb_track_page (after the
// store); memory_soa_invalidate -- the PMMU's TLB flush -- then zeroes just
// those entries, or all four arrays when the list outgrew its capacity.
void tlb_track_page(uint32_t page_index);
void memory_soa_invalidate(void);

// Active pointers switched per sprint based on SR.S bit.  The pair is the
// function code of the access in flight, not just the CPU's mode: MOVES
// points it at the SFC/DFC's arrays for the duration of its one access
// (cpu_ops.h).  That is why the slow paths take "supervisor" from which pair
// is active rather than from SR.S -- it is the access's own FC.
extern uintptr_t *g_active_read;
extern uintptr_t *g_active_write;

// Deferred bus error: slow paths signal unmapped MMU accesses by setting
// the pending flag AND zeroing *g_bus_error_instr_ptr, which forces the
// CPU decoder loop to exit after the current instruction completes.
// The bus error exception is then processed outside the hot loop.
extern bool g_bus_error_pending; // false=none, true=pending
extern uint32_t g_bus_error_address; // faulting logical address
extern bool g_bus_error_rw; // true=read, false=write
extern uint32_t g_bus_error_fc; // FC of the faulting access (1=user-data, 5=super-data)
extern bool g_bus_error_is_pmmu; // true=PMMU descriptor fault (retry), false=bus timeout (skip)
extern uint32_t *g_bus_error_instr_ptr; // points to decoder's instruction counter

// I/O cycle penalty: tracks extra bus wait-state cycles for I/O accesses.
// Penalty cycles are converted to phantom instructions that burn sprint burndown,
// causing I/O-heavy sprints to end sooner and keeping event timing accurate.
// The CPI is the scheduler's *effective* CPI in x256 fixed point (cpi << 8
// unless accelerated mode lowered it), so penalties convert at the same rate
// the sprint accounts cycles. g_io_cpi_x256 == 0 disables the mechanism.
extern uint32_t g_io_penalty_remainder; // sprint-time alias of the scheduler's io_penalty_remainder (x256 cycles)
extern uint32_t g_io_phantom_instructions; // phantom instructions consumed this sprint
extern uint32_t g_io_stall_owed; // sprint-time alias of the scheduler's io_stall_slots
extern uint32_t g_io_cpi_x256; // effective CPI for conversion, x256 (0 = disabled)
extern uint32_t *g_sprint_burndown_ptr; // points to sprint_burndown during sprint
// Slots the running sprint planned but will not spend: something ended it
// at this instruction boundary (an exception, a STOP, a trace step).  The
// scheduler takes them off the sprint, so neither the clock nor the
// instruction count advances for them -- time the CPU did not run.
extern uint32_t g_sprint_unrun_slots;

// End the running sprint at the current instruction boundary.  Always this,
// never a bare `*instructions = 0`: the bare store left the slots in the
// sprint's plan, so the scheduler advanced the clock and counted instructions
// all the way to the next event while the CPU did nothing -- a stall as long
// as the gap to whatever event came next.
static inline void memory_end_sprint(uint32_t *instructions) {
    if (instructions) {
        g_sprint_unrun_slots += *instructions;
        *instructions = 0;
    }
}

// --- VIA E-clock synchronization --------------------------------------------
//
// 6522 VIA accesses are synchronized to the 783.360 kHz E clock on real
// hardware: an access completes at the next E boundary, so back-to-back
// polling loops (`TST.B (VIA); DBF`) lock to one access per E period
// (1.2766 us) independent of CPU speed — the ROMs' pre-calibration timebase
// (1-E "pipelined" model, settled against a real IIsi chime recording).
// Ranges flagged `esync` charge this phase-accurate penalty instead of a
// fixed cycle count.

extern uint64_t g_sprint_base_cycles; // scheduler cpu_cycles at sprint start
extern uint32_t g_sprint_frac_x256; // sub-cycle remainder at sprint start (x256)
extern uint32_t g_sprint_total_slots; // sprint slot budget at sprint start
extern uint32_t g_esync_period_x256; // E period in CPU cycles x256 (0 = unset)

// Pure math: cycles from `now_cycles` to the next E boundary, in (0, E].
// Fixed-point x256 grid keeps the boundary sequence exact for non-integer
// cycles-per-E machines (25 MHz -> 31.91 cycles). Rounds UP to whole cycles
// so an access always lands on/just past its boundary — never before it
// (round-to-nearest could yield 0 and let two accesses share a boundary);
// the overshoot self-corrects because every access re-aims at the absolute
// grid, keeping the long-run rate exact. Deterministic.
static inline uint32_t memory_esync_penalty_cycles(uint64_t now_cycles, uint32_t period_x256) {
    uint32_t phase_x256 = (uint32_t)((now_cycles << 8) % period_x256);
    uint32_t pen_x256 = period_x256 - phase_x256; // (0, period]
    return (pen_x256 + 255) >> 8; // ceil to whole cycles
}

// Largest penalty one access may charge, in cycles (see memory_io_penalty).
#define MEMORY_IO_PENALTY_MAX ((UINT32_MAX >> 8) - 0x10000u)

// Apply an I/O bus cycle penalty (extra_cycles beyond the CPI baseline).
// Called from machine I/O dispatchers (e.g. SE/30) in the slow path only.
// The remainder accumulates x256 cycles so fractional effective CPIs
// (accelerated mode) convert without losing sub-slot penalty time.
static inline void memory_io_penalty(uint32_t extra_cycles) {
    if (__builtin_expect(g_io_cpi_x256 == 0, 0))
        return; // penalties disabled
    if (__builtin_expect(g_sprint_burndown_ptr == NULL, 0))
        return; // outside a sprint (an inspection access): never touches guest timing
    // Saturate so the x256 shift and the add stay inside 32 bits (the
    // remainder is already under one CPI, < 2^16).  Real penalties are tens
    // of cycles; the cap is ~16M.
    if (__builtin_expect(extra_cycles > MEMORY_IO_PENALTY_MAX, 0))
        extra_cycles = MEMORY_IO_PENALTY_MAX;
    g_io_penalty_remainder += extra_cycles << 8; // whole cycles onto the x256 grid
    uint32_t burn = g_io_penalty_remainder / g_io_cpi_x256;
    if (__builtin_expect(burn > 0, 1)) {
        g_io_penalty_remainder -= burn * g_io_cpi_x256;
        uint32_t *bp = g_sprint_burndown_ptr;
        // A stall longer than what is left of the sprint runs on past the
        // sprint's end (the event there fires on time, the CPU is still
        // stalled): the slots past it are owed to the next sprint, not lost.
        uint32_t take = (*bp > burn) ? burn : *bp;
        g_io_phantom_instructions += take;
        g_io_stall_owed += burn - take;
        *bp -= take;
    }
}

// Charge an E-synchronized access: stall until the next E boundary.
// "Now" is reconstructed from sprint progress (slots consumed x effective CPI
// plus the sprint's carried sub-cycle fraction, on top of the base cycles —
// mirroring the scheduler's current_cpu_cycles); earlier penalties in the
// same sprint have already burned slots, so consecutive E-synced accesses see
// advancing time and lock to the boundary grid. Sub-CPI phase jitter (< one
// CPI slot) is inherent to the slot quantization and irrelevant against the
// >=20-cycle E period; the long-run rate is exact.
static inline void memory_io_esync_penalty(void) {
    if (__builtin_expect(g_io_cpi_x256 == 0, 0) || g_esync_period_x256 == 0)
        return; // penalties disabled / E grid not configured
    uint64_t now = g_sprint_base_cycles;
    uint32_t *bp = g_sprint_burndown_ptr;
    if (bp)
        now += ((uint64_t)(g_sprint_total_slots - *bp) * g_io_cpi_x256 + g_sprint_frac_x256) >> 8;
    memory_io_penalty(memory_esync_penalty_cycles(now, g_esync_period_x256));
}

// Slow-path diagnostics (memory.slowpath_count / .slowpath_hist): accesses
// taken through the slow path since process start, and a histogram of them by
// address bucket (memory.c's MEM_SLOWPATH_BUCKET).
extern uint64_t g_mem_slowpath_count;
extern uint64_t g_mem_slowpath_hist[32];

// Slow-path handlers for device I/O, unmapped, or MMU TLB miss accesses
uint8_t memory_read_uint8_slow(uint32_t addr);
uint16_t memory_read_uint16_slow(uint32_t addr);
uint32_t memory_read_uint32_slow(uint32_t addr);
void memory_write_uint8_slow(uint32_t addr, uint8_t value);
void memory_write_uint16_slow(uint32_t addr, uint16_t value);
void memory_write_uint32_slow(uint32_t addr, uint32_t value);

// Side-effect-free reads for debug/inspection commands (memory.peek/.dump,
// find.*): translate via mmu_translate_checked, read host RAM/ROM or dispatch
// the device read, return all-ones for unmapped/invalid, and NEVER fault,
// touch the SoA cache, or latch g_bus_error_pending.  See memory.c.
uint8_t memory_debug_read_uint8(uint32_t addr);
uint16_t memory_debug_read_uint16(uint32_t addr);
uint32_t memory_debug_read_uint32(uint32_t addr);
// Side-effect-free read of `size` (1, 2 or 4) bytes at a PHYSICAL address,
// straight from the physical page table (host RAM/ROM or a device read).
// *ok is false for unmapped addresses, and on the Lisa, whose three physical
// spaces (RAM, I/O, boot ROM) are not in the page table.  Big-endian.
uint32_t memory_debug_read_phys(uint32_t phys, unsigned size, bool *ok);
// Bulk equivalent of len consecutive memory_debug_read_uint8 calls, but copies
// contiguous host-backed spans with memcpy.  Same result, far cheaper for RAM.
void memory_debug_read_block(uint32_t addr, uint8_t *dst, uint32_t len);

// Side-effect-free writes for memory.poke: write host RAM (if writable) or
// dispatch the device write; drop ROM/unmapped silently; never fault or latch
// g_bus_error_pending.  Returns true if the value landed.
bool memory_debug_write_uint8(uint32_t addr, uint8_t value);
bool memory_debug_write_uint16(uint32_t addr, uint16_t value);
bool memory_debug_write_uint32(uint32_t addr, uint32_t value);

// === Memory Logpoint Support ===
// When a memory logpoint covers a page, its SoA entries are forced to zero so
// every access routes through the slow path.  The slow path then consults the
// logpoint hook (installed by debug.c) and emits a log line.  The fast path
// is unchanged — no comparisons or branches added — so this feature has zero
// cost when no memory logpoints are set.

// Per-page memory-logpoint reference count.  Non-zero entries indicate pages
// whose SoA fast-path must stay at 0 (force slow path). Allocated alongside the
// page tables in memory_map_init.
extern uint16_t *g_mem_logpoint_page_count;

// Per-physical-page memory-logpoint reference count.  Non-zero entries mean
// "any logical alias mapping to this physical page must stay on the slow
// path so the logpoint fires regardless of which alias the CPU uses."
// Indexed by physical page number.  mmu_fill_soa_entry consults both arrays.
extern uint16_t *g_mem_logpoint_phys_page_count;

// The rule above for every writer of a direct (identity) fast-path entry:
// called after the writer fills page `p`, it puts the page's SoA entries
// back to 0 when a memory logpoint watches it -- logically or, the fill
// being an identity one, physically -- so the hook keeps firing after a
// machine re-maps the page (ROM overlay, reset, a memory controller's bank
// set-up).  Two loads when nothing is armed.
static inline void memory_logpoint_guard_page(uint32_t p) {
    if (!((g_mem_logpoint_page_count && g_mem_logpoint_page_count[p]) ||
          (g_mem_logpoint_phys_page_count && g_mem_logpoint_phys_page_count[p])))
        return;
    if (g_supervisor_read)
        g_supervisor_read[p] = 0;
    if (g_supervisor_write)
        g_supervisor_write[p] = 0;
    if (g_user_read)
        g_user_read[p] = 0;
    if (g_user_write)
        g_user_write[p] = 0;
}

// Hook invoked by the slow path on logpoint pages.  is_write=true on writes.
// Installed by debug.c.  NULL means no hook (skip check).
typedef void (*memory_logpoint_hook_t)(uint32_t addr, unsigned size, uint32_t value, bool is_write);
extern memory_logpoint_hook_t g_mem_logpoint_hook;

// When true, the user SoA arrays are owned by an architecture MMU front
// end that fills them with LOGICAL (translated) mappings — the generic
// identity-restore paths (rebuild_soa_page, logpoint uninstall) must not
// plant identity entries in them.  Set by machines whose main CPU keeps
// translated fills in the user arrays (the PPC 601); 68K machines leave
// it false and keep the classic all-four-arrays behavior.
extern bool g_user_soa_reserved;

// Optional notification that the SoA fast-path shape changed outside the
// owning CPU's control (memory logpoint install/uninstall): CPU-side
// translation caches (fetch windows, TLBs) must drop entries that could
// bypass the slow path.  NULL when no CPU registered one.
// Fired whenever the PHYSICAL MAP changes shape: a device window claimed or
// released, a host region registered, or a logpoint installed or removed.
// CPU-side fetch caches (g_ftlb, g_ppc_fetch) hold raw HOST POINTERS that
// bypass the SoA arrays, so zeroing an SoA entry is not enough to evict them.
// It used to be named g_mem_fastpath_changed and was fired only from the four
// logpoint sites, which left those caches holding pointers into a window that
// had since been remapped.
extern void (*g_mem_map_changed)(void);

// Current-context logical→physical translation for machines whose data
// translation lives outside g_mmu (the PPC 601 front end).  Used by the
// logpoint slow path to resolve the backing of a logically-watched page
// that arrives with its LOGICAL address (ppc_dxlate_slow keeps the EA for
// watched plain-RAM pages instead of rewriting it to physical).  Must be
// side-effect-free; *ok=false → treat as identity.  NULL on 68K machines.
// Called with g_mem_logical_xlate_ctx.  Alias of the selected map's hooks.
extern uint32_t (*g_mem_logical_xlate)(void *ctx, uint32_t addr, bool *ok);
extern void *g_mem_logical_xlate_ctx;

// Force/unforce the slow path for a page range (caller in debug.c).
// Each page in [start_page, end_page] (inclusive) has its reference count
// adjusted; if the count becomes non-zero the SoA entries are zeroed, and if
// it returns to zero they are restored from the page table.
void memory_logpoint_install(uint32_t start_page, uint32_t end_page);
void memory_logpoint_uninstall(uint32_t start_page, uint32_t end_page);

// Same as install/uninstall above, but for physical pages.  On install, every
// currently-populated SoA entry is invalidated so that new accesses re-walk
// the MMU and get suppressed by mmu_fill_soa_entry's physical-page check.
// On uninstall, the SoA stays empty and will refill lazily on next access.
void memory_logpoint_install_phys(uint32_t start_page, uint32_t end_page);
void memory_logpoint_uninstall_phys(uint32_t start_page, uint32_t end_page);

// === Inline Accessors (SoA fast-path with adjusted-base trick) ===
// Non-zero entry in g_active_read/write = adjusted host address.
// Zero entry = slow path (device I/O, unmapped, or MMU TLB miss).

static inline uint8_t memory_read_uint8(uint32_t addr) {
    uint32_t masked = addr & g_address_mask;
    uintptr_t base = g_active_read[masked >> PAGE_SHIFT];
    if (__builtin_expect(base != 0, 1))
        return LOAD_BE8((uint8_t *)(base + masked));
    return memory_read_uint8_slow(masked);
}

static inline uint16_t memory_read_uint16(uint32_t addr) {
    uint32_t masked = addr & g_address_mask;
    uintptr_t base = g_active_read[masked >> PAGE_SHIFT];
    // Fast path: non-zero entry and access doesn't cross page boundary
    if (__builtin_expect(base != 0 && (masked & PAGE_MASK) <= MEM_PAGE_SIZE - 2, 1))
        return LOAD_BE16((uint8_t *)(base + masked));
    return memory_read_uint16_slow(masked);
}

static inline uint32_t memory_read_uint32(uint32_t addr) {
    uint32_t masked = addr & g_address_mask;
    uintptr_t base = g_active_read[masked >> PAGE_SHIFT];
    // Fast path: non-zero entry and access doesn't cross page boundary
    if (__builtin_expect(base != 0 && (masked & PAGE_MASK) <= MEM_PAGE_SIZE - 4, 1)) {
        return LOAD_BE32((uint8_t *)(base + masked));
    }
    return memory_read_uint32_slow(masked);
}

// Instruction prefetch: the opcode word, plus the following word when it is
// free to take.
//
// The decoders fetch 32 bits at the PC so the opcode and its first extension
// word arrive together.  A plain memory_read_uint32 at the last word of a page
// reads FORWARD into the next page, which can fault or perform a phantom
// device read on a page the instruction never touches -- and real hardware
// does the opposite: MC68030UM 7.2 says the processor "always prefetches
// instructions by reading a long word from a long-word address (A1:A0 = 00),
// regardless of port size or alignment", i.e. it aligns DOWN and stays inside
// the page.  A fault on a prefetched word that is never used must also not be
// delivered (MC68030UM 8.2; MC68040UM: "the processor does not take the
// exception until it attempts to use the instruction").
//
// So: keep the 32-bit read whenever it is in-page -- byte-for-byte the fast
// path above, which is why this costs nothing in the decoder loop -- and when
// it is not, return just the opcode word in the high half.  Consumers of the
// low half (only the MOVES direction bit, cpu_decode.h) re-read it from the PC
// when they need it, by which point the access is deliberate rather than
// speculative.  Narrowing the fast path itself to a 16-bit read instead was
// measured at +6.7% on the SE/30 row: the ldrh/rev16 pair is a wash in the
// loop header, but losing the 32-bit load perturbs register allocation across
// the whole decoder body.
static inline uint32_t memory_read_prefetch32(uint32_t addr) {
    uint32_t masked = addr & g_address_mask;
    uintptr_t base = g_active_read[masked >> PAGE_SHIFT];
    if (__builtin_expect(base != 0 && (masked & PAGE_MASK) <= MEM_PAGE_SIZE - 4, 1)) {
        return LOAD_BE32((uint8_t *)(base + masked));
    }
    // Out of page (or no SoA entry): take the opcode word only.
    return (uint32_t)memory_read_uint16_slow(masked) << 16;
}

static inline void memory_write_uint8(uint32_t addr, uint8_t value) {
    uint32_t masked = addr & g_address_mask;
    uintptr_t base = g_active_write[masked >> PAGE_SHIFT];
    if (__builtin_expect(base != 0, 1)) {
        STORE_BE8((uint8_t *)(base + masked), value);
        return;
    }
    memory_write_uint8_slow(masked, value);
}

static inline void memory_write_uint16(uint32_t addr, uint16_t value) {
    uint32_t masked = addr & g_address_mask;
    uintptr_t base = g_active_write[masked >> PAGE_SHIFT];
    // Fast path: non-zero entry and access doesn't cross page boundary
    if (__builtin_expect(base != 0 && (masked & PAGE_MASK) <= MEM_PAGE_SIZE - 2, 1)) {
        STORE_BE16((uint8_t *)(base + masked), value);
        return;
    }
    memory_write_uint16_slow(masked, value);
}

static inline void memory_write_uint32(uint32_t addr, uint32_t value) {
    uint32_t masked = addr & g_address_mask;
    uintptr_t base = g_active_write[masked >> PAGE_SHIFT];
    // Fast path: non-zero entry and access doesn't cross page boundary
    if (__builtin_expect(base != 0 && (masked & PAGE_MASK) <= MEM_PAGE_SIZE - 4, 1)) {
        STORE_BE32((uint8_t *)(base + masked), value);
        return;
    }
    memory_write_uint32_slow(masked, value);
}

#endif // MEMORY_H
