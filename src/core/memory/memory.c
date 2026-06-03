// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// memory.c
// Memory map management and RAM/ROM access for Granny Smith.

// ============================================================================
// Includes
// ============================================================================

#include "memory.h"
#include "mmu.h"

#include "common.h"
#include "cpu.h"
#include "debug.h"
#include "object.h"
#include "platform.h"
#include "rom.h"
#include "shell.h"
#include "system.h"
#include "system_config.h"
#include "value.h"

// Forward declarations — class descriptors are at the bottom of the file but
// memory_map_init / memory_map_delete reference them.
extern const class_desc_t memory_class;
extern const class_desc_t mem_peek_class;
extern const class_desc_t mem_poke_class;

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Constants and Macros
// ============================================================================

// Page table globals (defined here, declared extern in memory.h)
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

// TEMP DIAG (GS_PAGE0_TRACE): fast-path writes to physical page 0.
unsigned char g_trace_page0 = 0;
uintptr_t g_phys0_host_lo = 0;
uintptr_t g_phys0_host_hi = 0;

// Out-of-line logger for fast-path writes that land on physical page 0 (the
// kernel vector table).  Gated to the corruption window (>155 M instr, past
// the legitimate early vector-table setup) and capped to avoid flooding on a
// page-sized bzero.  Prints the current PC, the logical address written, and
// the SoA adjusted base so the aliasing logical page can be identified.
// Logger for supervisor reads of the vector region (logical < $400) that
// resolve to a host page OTHER than physical page 0 — i.e. a stale/wrong SoA
// read entry for the vector table.  `host_target` is the resolved host pointer;
// (host_target - g_phys0_host_lo) >> PAGE_SHIFT names the bogus physical page.
void memory_page0_read_trace(uint32_t logical_addr, uintptr_t host_target) {
    extern uint64_t cpu_instr_count(void);
    cpu_t *cpu = system_cpu();
    if (!cpu)
        return;
    uint16_t sr = cpu_get_sr(cpu);
    if (!(sr & 0x2000))
        return; // only care about SUPERVISOR reads of low memory resolving off-page-0
    static int n = 0;
    if (n++ >= 60)
        return;
    uint32_t pc = cpu_get_pc(cpu);
    long delta = (long)((intptr_t)host_target - (intptr_t)g_phys0_host_lo);
    bool active_super = (g_active_read == g_supervisor_read);
    bool active_user = (g_active_read == g_user_read);
    fprintf(stderr, "[VECRD] instr=%llu pc=%08x sr=%04x la=%08x phys~%08lx active=%s\n",
            (unsigned long long)cpu_instr_count(), pc, sr, logical_addr, (unsigned long)delta,
            active_super ? "SUP" : (active_user ? "USR(DESYNC!)" : "?"));
}

void memory_page0_write_trace(uint32_t logical_addr, unsigned size) {
    extern uint64_t cpu_instr_count(void);
    uint64_t ic = cpu_instr_count();
    static int n = 0;
    if (n++ >= 80)
        return;
    cpu_t *cpu = system_cpu();
    uint32_t pc = cpu ? cpu_get_pc(cpu) : 0;
    uintptr_t base = g_active_write[(logical_addr & g_address_mask) >> PAGE_SHIFT];
    fprintf(stderr, "[PAGE0W] instr=%llu pc=%08x la=%08x off=%03x sz=%u super=%d soabase=%016llx\n",
            (unsigned long long)ic, pc, logical_addr, logical_addr & 0xFFF, size,
            (g_active_write == g_supervisor_write), (unsigned long long)base);
}

// TEMP DIAG: slow-path write whose RE-WALKED physical target is in the vector
// table (phys < $400).  Catches writes that bypass the fast-path SoA entirely.
void memory_slow_write_page0_check(uint32_t addr, unsigned size) {
    if (!g_trace_page0)
        return;
    extern uint64_t cpu_instr_count(void);
    uint64_t ic = cpu_instr_count();
    if (ic < 155000000ull)
        return;
    bool supervisor = (g_active_write == g_supervisor_write);
    uint32_t phys = (g_mmu && g_mmu->enabled) ? mmu_translate_debug(g_mmu, addr & g_address_mask, supervisor)
                                              : (addr & g_address_mask);
    if (phys >= 0x400)
        return;
    static int n = 0;
    if (n++ >= 60)
        return;
    cpu_t *cpu = system_cpu();
    uint32_t pc = cpu ? cpu_get_pc(cpu) : 0;
    fprintf(stderr, "[SLOW0W] instr=%llu pc=%08x la=%08x phys=%08x sz=%u super=%d\n", (unsigned long long)ic, pc,
            addr & g_address_mask, phys, size, supervisor);
}

// Deferred bus error signal: set by slow paths on unmapped MMU accesses.
// Zeroing *g_bus_error_instr_ptr forces the decoder loop to exit early.
bool g_bus_error_pending = false;
uint32_t g_bus_error_address = 0;
bool g_bus_error_rw = false;
// FC value (SSW[2:0]) of the faulting access.  Set alongside g_bus_error_pending
// so exception frame reflects the FC the access was issued with — critical for
// MOVES in a kernel that points DFC/SFC at user-data while in supervisor mode
// (A/UX's copyin/copyout/copyinstr path).  If the frame's FC bits say
// supervisor-data for what was really a user-data probe, the kernel's
// page-fault arbiter will treat it as a kernel fault and skip demand-fill.
uint32_t g_bus_error_fc = 5;
// 1 if the fault came from a PMMU descriptor failure (invalid/perm/write-
// protect) and the handler is expected to fix-up the PTE and retry; 0 if the
// fault was a plain bus timeout (unmapped physical in a NuBus slot range)
// where the handler expects skip-instruction semantics (e.g. Mac ROM RAM
// and slot probes).  Selects Format $B vs Format $A dispatch.
bool g_bus_error_is_pmmu = false;
uint32_t *g_bus_error_instr_ptr = NULL;

// I/O cycle penalty state: tracks extra bus wait-state cycles for I/O accesses.
// Penalty cycles are converted to "phantom instructions" that consume sprint
// burndown slots, causing sprints with I/O to end sooner and keeping event
// timing accurate.
uint32_t g_io_penalty_remainder = 0; // sub-CPI fraction carried across sprints
uint32_t g_io_phantom_instructions = 0; // phantom instructions consumed this sprint
uint32_t g_io_cpi = 0; // current CPI for penalty conversion (0 = disabled)
uint32_t *g_sprint_burndown_ptr = NULL; // points to scheduler's sprint_burndown during sprint

// Memory logpoint support: non-zero entries force the page through the slow
// path even when the underlying page is plain RAM/ROM.  See memory.h.
uint8_t *g_mem_logpoint_page_count = NULL;
uint8_t *g_mem_logpoint_phys_page_count = NULL;
memory_logpoint_hook_t g_mem_logpoint_hook = NULL;

// Value-trap support: catches a specific (PA, size, value) write on the fast
// path.  Disabled when g_value_trap_active == 0 (the common case).
uint32_t g_value_trap_active = 0;
uint32_t g_value_trap_pa = 0;
uint32_t g_value_trap_value = 0;
uint32_t g_value_trap_size = 0;
value_trap_hook_t g_value_trap_hook = NULL;

void value_trap_check(uint32_t logical_addr, uint32_t value, unsigned size) {
    // Compute physical address.  Translate via active SoA mode (super or user).
    uint32_t phys_addr = logical_addr;
    if (g_mmu && g_mmu->enabled) {
        bool supervisor = (g_active_write == g_supervisor_write);
        phys_addr = mmu_translate_debug(g_mmu, logical_addr, supervisor);
    }
    if (phys_addr != g_value_trap_pa)
        return;
    if (g_value_trap_hook)
        g_value_trap_hook(logical_addr, phys_addr, value, size);
}

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

typedef struct memory {

    mapping_t *map;

    uint8_t *image; // flat RAM+ROM buffer

    // Per-instance page table (points to g_page_table when active)
    page_entry_t *page_table;
    int page_count;

    // Machine-parameterised sizes (set by memory_map_init)
    uint32_t ram_size; // RAM region size in bytes
    uint32_t rom_size; // ROM content size in bytes

    // Path to the ROM file loaded via cmd_rom (if any)
    char *rom_filename;

    uint32_t checksum;

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
static bool logpoint_lookup(uint32_t addr, uint8_t **host_out, bool *writable_out) {
    uint32_t page = addr >> PAGE_SHIFT;
    bool logical_watched = g_mem_logpoint_page_count && g_mem_logpoint_page_count[page];
    bool phys_watched = false;
    uint32_t phys_addr = addr;

    if (g_mmu && g_mmu->enabled) {
        bool supervisor = (g_active_write == g_supervisor_write);
        phys_addr = mmu_translate_debug(g_mmu, addr, supervisor);
        if (g_mem_logpoint_phys_page_count && g_mem_logpoint_phys_page_count[phys_addr >> PAGE_SHIFT])
            phys_watched = true;
    }

    if (!logical_watched && !phys_watched)
        return false;

    if (g_mmu && g_mmu->enabled) {
        // Route through the real physical address so writes land in the page
        // the guest actually sees, not the g_page_table identity alias.
        uint8_t *base = mmu_phys_to_host(g_mmu, phys_addr & ~(uint32_t)PAGE_MASK);
        *host_out = base ? base + (phys_addr & PAGE_MASK) : NULL;
        *writable_out = base ? mmu_phys_is_writable(g_mmu, phys_addr) : false;
    } else {
        page_entry_t *pe = &g_page_table[page];
        *host_out = pe->host_base ? pe->host_base + (addr & PAGE_MASK) : NULL;
        *writable_out = pe->writable;
    }
    return true;
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
    if ((phys & ~(uint32_t)PAGE_MASK) == (addr & ~(uint32_t)PAGE_MASK))
        return true; // identity mapping — dispatch as usual
    // Non-identity: only divert to the table walk when the target is real RAM.
    return mmu_phys_to_host(g_mmu, phys & ~(uint32_t)PAGE_MASK) == NULL;
}

// Slow path for 8-bit reads: device I/O, MMU TLB miss, or unmapped
uint8_t memory_read_uint8_slow(uint32_t addr) {
    uint32_t page = addr >> PAGE_SHIFT;
    page_entry_t *pe = &g_page_table[page];
    // Memory logpoint: page is forced to slow path but backed by RAM/ROM.
    // Read via the MMU-translated host pointer, then notify the hook.
    uint8_t *lp_host;
    bool lp_writable;
    if (logpoint_lookup(addr, &lp_host, &lp_writable) && lp_host) {
        uint8_t v = LOAD_BE8(lp_host);
        if (g_mem_logpoint_hook)
            g_mem_logpoint_hook(addr, 1, v, false);
        return v;
    }
    // Lazy-install identity SoA for a host-backed page when the MMU is off.
    if (can_lazy_install(page, pe)) {
        rebuild_soa_page(page);
        return LOAD_BE8(pe->host_base + (addr & PAGE_MASK));
    }
    // Gate logical-device dispatch via dispatch_device_at_logical(): identity /
    // MMU-off / non-identity-to-device cases dispatch immediately; non-identity-
    // to-RAM (e.g. 24-bit Memory Manager flag-tagged master pointers $40xxxxxx
    // → $00xxxxxx) falls through to the MMU walk below so the translated RAM
    // is read instead of returning ROM bytes from the $40000000 device window.
    if (pe->dev && dispatch_device_at_logical(addr, g_active_read == g_supervisor_read))
        return pe->dev->read_uint8(pe->dev_context, addr - pe->base_addr);
    // When MMU is enabled, dispatch via PHYSICAL address (after table walk),
    // not via the logical page-table entry — otherwise a user-virtual address
    // whose upper byte coincides with a host-machine MMIO range (e.g. virtual
    // $47f01000 hitting the IIfx ROM device window at $40000000-$4FFFFFFF)
    // silently returns the device's read value instead of faulting.  A/UX's
    // copyin depends on the fault to demand-page user pages.
    //
    // Fast path: TT (transparent translation) match means logical = physical,
    // so the logical-page-table dev entry IS the correct dispatch.  Skip the
    // expensive table walk for kernel I/O which is typically TT-mapped.
    if (g_mmu && g_mmu->enabled) {
        bool supervisor = g_active_read == g_supervisor_read;
        if (pe->dev && mmu_check_tt(g_mmu, addr, false, supervisor))
            return pe->dev->read_uint8(pe->dev_context, addr - pe->base_addr);
        if (mmu_handle_fault(g_mmu, addr, false, supervisor)) {
            uintptr_t base = g_active_read[addr >> PAGE_SHIFT];
            if (base != 0)
                return LOAD_BE8((uint8_t *)(base + addr));
            // SoA still 0: physical page is device, unmapped, or logpointed.
            // Translate to physical and dispatch on the PHYSICAL page-table
            // entry.
            uint32_t phys = mmu_translate_debug(g_mmu, addr, supervisor);
            uint32_t phys_page = phys >> PAGE_SHIFT;
            if ((int)phys_page < g_page_count) {
                page_entry_t *phys_pe = &g_page_table[phys_page];
                if (phys_pe->dev)
                    return phys_pe->dev->read_uint8(phys_pe->dev_context, phys - phys_pe->base_addr);
            }
            // Re-check the logpoint now that mmu_handle_fault has run
            // (physical-space logpoints suppress the fill and require
            // translation here).
            if (logpoint_lookup(addr, &lp_host, &lp_writable) && lp_host) {
                uint8_t v = LOAD_BE8(lp_host);
                if (g_mem_logpoint_hook)
                    g_mem_logpoint_hook(addr, 1, v, false);
                return v;
            }
        } else {
            // MMU fault (invalid descriptor, unmapped physical, permission, etc.)
            if (!g_bus_error_pending) {
                g_bus_error_pending = true;
                g_bus_error_address = addr;
                g_bus_error_rw = true; // read
                g_bus_error_fc = supervisor ? 5 : 1;
                if (g_bus_error_instr_ptr)
                    *g_bus_error_instr_ptr = 0; // force decoder loop exit
            }
        }
        // Unmapped physical memory returns $FF.
        return 0xFF;
    }
    // MMU disabled: logical == physical, dispatch by logical page-table entry.
    if (pe->dev)
        return pe->dev->read_uint8(pe->dev_context, addr - pe->base_addr);
    // Unmapped physical memory returns $FF (floating bus, pull-up resistors).
    // This matches real 68k Mac hardware behavior and is critical for:
    //   - ROM RAM sizing (write pattern / read-back $FF → detects boundary)
    //   - ROM POST memory test (pattern mismatch → knows address is invalid)
    return 0xFF;
}

// Slow path for 16-bit reads: cross-page or device I/O
uint16_t memory_read_uint16_slow(uint32_t addr) {
    uint32_t page = addr >> PAGE_SHIFT;
    page_entry_t *pe = &g_page_table[page];

    // Memory logpoint: forced slow path on RAM/ROM page
    uint8_t *lp_host;
    bool lp_writable;
    if ((addr & PAGE_MASK) <= MEM_PAGE_SIZE - 2 && logpoint_lookup(addr, &lp_host, &lp_writable) && lp_host) {
        uint16_t v = LOAD_BE16(lp_host);
        if (g_mem_logpoint_hook)
            g_mem_logpoint_hook(addr, 2, v, false);
        return v;
    }

    // Lazy-install identity SoA for in-page accesses to host-backed pages.
    if ((addr & PAGE_MASK) <= MEM_PAGE_SIZE - 2 && can_lazy_install(page, pe)) {
        rebuild_soa_page(page);
        return LOAD_BE16(pe->host_base + (addr & PAGE_MASK));
    }

    // Gate logical-device dispatch (24-bit Mac OS master-pointer fix — see
    // dispatch_device_at_logical above).
    if (pe->dev && (addr & PAGE_MASK) <= MEM_PAGE_SIZE - 2 &&
        dispatch_device_at_logical(addr, g_active_read == g_supervisor_read))
        return pe->dev->read_uint16(pe->dev_context, addr - pe->base_addr);

    // When MMU is enabled, dispatch via PHYSICAL address (see write_uint8_slow
    // comment for the rationale).  Cross-page accesses fall through to byte
    // reads which already handle MMU correctly.
    if (g_mmu && g_mmu->enabled && (addr & PAGE_MASK) <= MEM_PAGE_SIZE - 2) {
        bool supervisor = g_active_read == g_supervisor_read;
        if (pe->dev && mmu_check_tt(g_mmu, addr, false, supervisor))
            return pe->dev->read_uint16(pe->dev_context, addr - pe->base_addr);
        if (mmu_handle_fault(g_mmu, addr, false, supervisor)) {
            uintptr_t base = g_active_read[addr >> PAGE_SHIFT];
            if (base != 0)
                return LOAD_BE16((uint8_t *)(base + addr));
            uint32_t phys = mmu_translate_debug(g_mmu, addr, supervisor);
            uint32_t phys_page = phys >> PAGE_SHIFT;
            if ((int)phys_page < g_page_count) {
                page_entry_t *phys_pe = &g_page_table[phys_page];
                if (phys_pe->dev)
                    return phys_pe->dev->read_uint16(phys_pe->dev_context, phys - phys_pe->base_addr);
            }
            if (logpoint_lookup(addr, &lp_host, &lp_writable) && lp_host) {
                uint16_t v = LOAD_BE16(lp_host);
                if (g_mem_logpoint_hook)
                    g_mem_logpoint_hook(addr, 2, v, false);
                return v;
            }
        } else {
            if (!g_bus_error_pending) {
                g_bus_error_pending = true;
                g_bus_error_address = addr;
                g_bus_error_rw = true;
                g_bus_error_fc = supervisor ? 5 : 1;
                if (g_bus_error_instr_ptr)
                    *g_bus_error_instr_ptr = 0;
            }
        }
        return 0xFFFF;
    }

    // MMU-off fallback: dispatch device on logical page-table entry.
    if (pe->dev && (addr & PAGE_MASK) <= MEM_PAGE_SIZE - 2)
        return pe->dev->read_uint16(pe->dev_context, addr - pe->base_addr);

    // Cross-page or host memory at page boundary: split into two byte reads
    uint16_t hi = memory_read_uint8(addr);
    uint16_t lo = memory_read_uint8((addr + 1) & g_address_mask);
    return (hi << 8) | lo;
}

// Slow path for 32-bit reads: cross-page or device I/O
uint32_t memory_read_uint32_slow(uint32_t addr) {
    // TEMP DIAG (GS_PAGE0_TRACE): slow-path read of the vector region.
    if (__builtin_expect(g_trace_page0, 0) && (addr & g_address_mask) < 0x400) {
        extern uint64_t cpu_instr_count(void);
        uint64_t ic = cpu_instr_count();
        if (ic > 155000000ull) {
            static int n = 0;
            if (n++ < 60) {
                bool supervisor = (g_active_read == g_supervisor_read);
                uint32_t la = addr & g_address_mask;
                uint32_t phys = (g_mmu && g_mmu->enabled) ? mmu_translate_debug(g_mmu, la, supervisor) : la;
                cpu_t *c = system_cpu();
                fprintf(stderr, "[VECSLOW] instr=%llu pc=%08x la=%08x phys=%08x super=%d activeU=%d\n",
                        (unsigned long long)ic, c ? cpu_get_pc(c) : 0, la, phys, supervisor,
                        (g_active_read == g_user_read));
            }
        }
    }
    uint32_t page = addr >> PAGE_SHIFT;
    page_entry_t *pe = &g_page_table[page];

    // Memory logpoint: forced slow path on RAM/ROM page
    uint8_t *lp_host;
    bool lp_writable;
    if ((addr & PAGE_MASK) <= MEM_PAGE_SIZE - 4 && logpoint_lookup(addr, &lp_host, &lp_writable) && lp_host) {
        uint32_t v = LOAD_BE32(lp_host);
        if (g_mem_logpoint_hook)
            g_mem_logpoint_hook(addr, 4, v, false);
        return v;
    }

    // Lazy-install identity SoA for in-page accesses to host-backed pages.
    if ((addr & PAGE_MASK) <= MEM_PAGE_SIZE - 4 && can_lazy_install(page, pe)) {
        rebuild_soa_page(page);
        return LOAD_BE32(pe->host_base + (addr & PAGE_MASK));
    }

    // Gate logical-device dispatch (24-bit Mac OS master-pointer fix).
    if (pe->dev && (addr & PAGE_MASK) <= MEM_PAGE_SIZE - 4 &&
        dispatch_device_at_logical(addr, g_active_read == g_supervisor_read)) {
        uint32_t v = pe->dev->read_uint32(pe->dev_context, addr - pe->base_addr);
        return v;
    }

    // When MMU is enabled, dispatch via PHYSICAL address (see write_uint8_slow
    // comment).  Cross-page accesses fall through to 16-bit reads.
    if (g_mmu && g_mmu->enabled && (addr & PAGE_MASK) <= MEM_PAGE_SIZE - 4) {
        bool supervisor = g_active_read == g_supervisor_read;
        if (pe->dev && mmu_check_tt(g_mmu, addr, false, supervisor))
            return pe->dev->read_uint32(pe->dev_context, addr - pe->base_addr);
        if (mmu_handle_fault(g_mmu, addr, false, supervisor)) {
            uintptr_t base = g_active_read[addr >> PAGE_SHIFT];
            if (base != 0)
                return LOAD_BE32((uint8_t *)(base + addr));
            uint32_t phys = mmu_translate_debug(g_mmu, addr, supervisor);
            uint32_t phys_page = phys >> PAGE_SHIFT;
            if ((int)phys_page < g_page_count) {
                page_entry_t *phys_pe = &g_page_table[phys_page];
                if (phys_pe->dev)
                    return phys_pe->dev->read_uint32(phys_pe->dev_context, phys - phys_pe->base_addr);
            }
            if (logpoint_lookup(addr, &lp_host, &lp_writable) && lp_host) {
                uint32_t v = LOAD_BE32(lp_host);
                if (g_mem_logpoint_hook)
                    g_mem_logpoint_hook(addr, 4, v, false);
                return v;
            }
        } else {
            if (!g_bus_error_pending) {
                g_bus_error_pending = true;
                g_bus_error_address = addr;
                g_bus_error_rw = true;
                g_bus_error_fc = supervisor ? 5 : 1;
                if (g_bus_error_instr_ptr)
                    *g_bus_error_instr_ptr = 0;
            }
        }
        return 0xFFFFFFFFu;
    }

    // MMU-off fallback: dispatch device on logical page-table entry.
    if (pe->dev && (addr & PAGE_MASK) <= MEM_PAGE_SIZE - 4) {
        uint32_t v = pe->dev->read_uint32(pe->dev_context, addr - pe->base_addr);
        return v;
    }

    // Cross-page: split into two 16-bit reads
    uint32_t hi = memory_read_uint16(addr);
    uint32_t lo = memory_read_uint16(addr + 2);
    return (hi << 16) | lo;
}

// === Side-effect-free debug reads ==========================================
//
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
    uint32_t phys = addr;
    if (g_mmu && g_mmu->enabled && !mmu_translate_checked(g_mmu, addr, g_active_read == g_supervisor_read, &phys))
        return 0xFF;
    uint32_t page = phys >> PAGE_SHIFT;
    if ((int)page >= g_page_count)
        return 0xFF;
    page_entry_t *pe = &g_page_table[page];
    if (pe->host_base)
        return LOAD_BE8(pe->host_base + (phys & PAGE_MASK));
    if (pe->dev)
        return pe->dev->read_uint8(pe->dev_context, phys - pe->base_addr);
    return 0xFF;
}

uint16_t memory_debug_read_uint16(uint32_t addr) {
    addr &= g_address_mask;
    // Single-page word reads hit the device's own width handler; byte-split
    // only when the access straddles a page.
    if ((addr & PAGE_MASK) <= MEM_PAGE_SIZE - 2) {
        uint32_t phys = addr;
        if (g_mmu && g_mmu->enabled && !mmu_translate_checked(g_mmu, addr, g_active_read == g_supervisor_read, &phys))
            return 0xFFFF;
        uint32_t page = phys >> PAGE_SHIFT;
        if ((int)page < g_page_count) {
            page_entry_t *pe = &g_page_table[page];
            if (pe->host_base)
                return LOAD_BE16(pe->host_base + (phys & PAGE_MASK));
            if (pe->dev)
                return pe->dev->read_uint16(pe->dev_context, phys - pe->base_addr);
        }
        return 0xFFFF;
    }
    return (uint16_t)((memory_debug_read_uint8(addr) << 8) | memory_debug_read_uint8(addr + 1));
}

uint32_t memory_debug_read_uint32(uint32_t addr) {
    addr &= g_address_mask;
    if ((addr & PAGE_MASK) <= MEM_PAGE_SIZE - 4) {
        uint32_t phys = addr;
        if (g_mmu && g_mmu->enabled && !mmu_translate_checked(g_mmu, addr, g_active_read == g_supervisor_read, &phys))
            return 0xFFFFFFFFu;
        uint32_t page = phys >> PAGE_SHIFT;
        if ((int)page < g_page_count) {
            page_entry_t *pe = &g_page_table[page];
            if (pe->host_base)
                return LOAD_BE32(pe->host_base + (phys & PAGE_MASK));
            if (pe->dev)
                return pe->dev->read_uint32(pe->dev_context, phys - pe->base_addr);
        }
        return 0xFFFFFFFFu;
    }
    return ((uint32_t)memory_debug_read_uint16(addr) << 16) | memory_debug_read_uint16(addr + 2);
}

// Side-effect-free debug writes (memory.poke) — symmetric to the debug reads:
// translate via mmu_translate_checked, write host RAM (if writable) or dispatch
// the device write, drop ROM/unmapped silently, and NEVER fault or latch
// g_bus_error_pending (a stray poke must not inject a bus error into the guest).
// Returns true if the byte landed in writable host RAM / a device.
bool memory_debug_write_uint8(uint32_t addr, uint8_t value) {
    addr &= g_address_mask;
    uint32_t phys = addr;
    if (g_mmu && g_mmu->enabled && !mmu_translate_checked(g_mmu, addr, g_active_write == g_supervisor_write, &phys))
        return false;
    uint32_t page = phys >> PAGE_SHIFT;
    if ((int)page >= g_page_count)
        return false;
    page_entry_t *pe = &g_page_table[page];
    if (pe->host_base) {
        if (!pe->writable)
            return false; // ROM/VROM — drop silently
        STORE_BE8(pe->host_base + (phys & PAGE_MASK), value);
        return true;
    }
    if (pe->dev) {
        pe->dev->write_uint8(pe->dev_context, phys - pe->base_addr, value);
        return true;
    }
    return false;
}

bool memory_debug_write_uint16(uint32_t addr, uint16_t value) {
    addr &= g_address_mask;
    if ((addr & PAGE_MASK) <= MEM_PAGE_SIZE - 2) {
        uint32_t phys = addr;
        if (g_mmu && g_mmu->enabled && !mmu_translate_checked(g_mmu, addr, g_active_write == g_supervisor_write, &phys))
            return false;
        uint32_t page = phys >> PAGE_SHIFT;
        if ((int)page < g_page_count) {
            page_entry_t *pe = &g_page_table[page];
            if (pe->host_base) {
                if (!pe->writable)
                    return false;
                STORE_BE16(pe->host_base + (phys & PAGE_MASK), value);
                return true;
            }
            if (pe->dev) {
                pe->dev->write_uint16(pe->dev_context, phys - pe->base_addr, value);
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
    if ((addr & PAGE_MASK) <= MEM_PAGE_SIZE - 4) {
        uint32_t phys = addr;
        if (g_mmu && g_mmu->enabled && !mmu_translate_checked(g_mmu, addr, g_active_write == g_supervisor_write, &phys))
            return false;
        uint32_t page = phys >> PAGE_SHIFT;
        if ((int)page < g_page_count) {
            page_entry_t *pe = &g_page_table[page];
            if (pe->host_base) {
                if (!pe->writable)
                    return false;
                STORE_BE32(pe->host_base + (phys & PAGE_MASK), value);
                return true;
            }
            if (pe->dev) {
                pe->dev->write_uint32(pe->dev_context, phys - pe->base_addr, value);
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
void memory_write_uint8_slow(uint32_t addr, uint8_t value) {
    if (__builtin_expect(g_trace_page0, 0))
        memory_slow_write_page0_check(addr, 1);
    uint32_t page = addr >> PAGE_SHIFT;
    page_entry_t *pe = &g_page_table[page];
    // Memory logpoint: forced slow path for RAM write on logged page
    uint8_t *lp_host;
    bool lp_writable;
    if (logpoint_lookup(addr, &lp_host, &lp_writable) && lp_host && lp_writable) {
        STORE_BE8(lp_host, value);
        if (g_mem_logpoint_hook)
            g_mem_logpoint_hook(addr, 1, value, true);
        return;
    }
    // Lazy-install identity SoA for a writable host-backed page when MMU off.
    // Read-only pages (ROM) still drop the write silently via the fall-through.
    if (can_lazy_install(page, pe)) {
        rebuild_soa_page(page);
        if (pe->writable)
            STORE_BE8(pe->host_base + (addr & PAGE_MASK), value);
        return;
    }
    // Gate logical-device dispatch (24-bit Mac OS master-pointer fix — see
    // dispatch_device_at_logical above).
    if (pe->dev && dispatch_device_at_logical(addr, g_active_write == g_supervisor_write)) {
        pe->dev->write_uint8(pe->dev_context, addr - pe->base_addr, value);
        return;
    }
    // When MMU is enabled, the page-table device lookup must use the PHYSICAL
    // address — otherwise a user-virtual address whose upper byte coincides
    // with a host-machine MMIO range (e.g. virtual $47f01000 hitting the IIfx
    // ROM device window at physical $40000000-$4FFFFFFF) silently absorbs the
    // write via the device's noop write handler, never reaching the MMU walk.
    // A/UX's copyout depends on that walk faulting on unmapped user pages so
    // its fault handler can demand-page them in.  Hide-the-fault → realvtop
    // returns 0 → p_blt overwrites virtual $0 (the kernel exception vectors).
    if (g_mmu && g_mmu->enabled) {
        bool supervisor = g_active_write == g_supervisor_write;
        // Fast path: TT match means logical = physical, so the logical pe->dev
        // IS the correct dispatch — skip the table walk.
        if (pe->dev && mmu_check_tt(g_mmu, addr, true, supervisor)) {
            pe->dev->write_uint8(pe->dev_context, addr - pe->base_addr, value);
            return;
        }
        if (mmu_handle_fault(g_mmu, addr, true, supervisor)) {
            uintptr_t base = g_active_write[addr >> PAGE_SHIFT];
            if (base != 0) {
                STORE_BE8((uint8_t *)(base + addr), value);
                return;
            }
            // SoA still 0: physical page is a device, unmapped, or covered by
            // a logpoint.  Translate to physical and dispatch on the PHYSICAL
            // page-table entry (mirrors the MMU-disabled path below).
            uint32_t phys = mmu_translate_debug(g_mmu, addr, supervisor);
            uint32_t phys_page = phys >> PAGE_SHIFT;
            if ((int)phys_page < g_page_count) {
                page_entry_t *phys_pe = &g_page_table[phys_page];
                if (phys_pe->dev) {
                    phys_pe->dev->write_uint8(phys_pe->dev_context, phys - phys_pe->base_addr, value);
                    return;
                }
            }
            // Re-check logpoint now that the fault has run — physical-space
            // logpoints are only detectable after mmu_translate_debug.
            if (logpoint_lookup(addr, &lp_host, &lp_writable) && lp_host && lp_writable) {
                STORE_BE8(lp_host, value);
                if (g_mem_logpoint_hook)
                    g_mem_logpoint_hook(addr, 1, value, true);
                return;
            }
            // Unmapped physical but no fault — drop write
        } else {
            // MMU fault (invalid descriptor, unmapped physical, permission, etc.)
            if (!g_bus_error_pending) {
                g_bus_error_pending = true;
                g_bus_error_address = addr;
                g_bus_error_rw = false; // write
                g_bus_error_fc = supervisor ? 5 : 1;
                if (g_bus_error_instr_ptr)
                    *g_bus_error_instr_ptr = 0; // force decoder loop exit
            }
        }
        return;
    }
    // MMU disabled: logical == physical, dispatch by logical page-table entry.
    if (pe->dev) {
        pe->dev->write_uint8(pe->dev_context, addr - pe->base_addr, value);
        return;
    }
}

// Slow path for 16-bit writes: cross-page or device I/O
void memory_write_uint16_slow(uint32_t addr, uint16_t value) {
    if (__builtin_expect(g_trace_page0, 0))
        memory_slow_write_page0_check(addr, 2);
    uint32_t page = addr >> PAGE_SHIFT;
    page_entry_t *pe = &g_page_table[page];

    // Memory logpoint: forced slow path for RAM write on logged page
    uint8_t *lp_host;
    bool lp_writable;
    if ((addr & PAGE_MASK) <= MEM_PAGE_SIZE - 2 && logpoint_lookup(addr, &lp_host, &lp_writable) && lp_host &&
        lp_writable) {
        STORE_BE16(lp_host, value);
        if (g_mem_logpoint_hook)
            g_mem_logpoint_hook(addr, 2, value, true);
        return;
    }

    // Lazy-install identity SoA for in-page writes to host-backed pages.
    if ((addr & PAGE_MASK) <= MEM_PAGE_SIZE - 2 && can_lazy_install(page, pe)) {
        rebuild_soa_page(page);
        if (pe->writable)
            STORE_BE16(pe->host_base + (addr & PAGE_MASK), value);
        return;
    }

    // Gate logical-device dispatch (24-bit Mac OS master-pointer fix).
    if (pe->dev && (addr & PAGE_MASK) <= MEM_PAGE_SIZE - 2 &&
        dispatch_device_at_logical(addr, g_active_write == g_supervisor_write)) {
        pe->dev->write_uint16(pe->dev_context, addr - pe->base_addr, value);
        return;
    }

    // When MMU is enabled, dispatch via PHYSICAL address (see write_uint8_slow
    // comment).  Cross-page accesses fall through to byte writes.
    if (g_mmu && g_mmu->enabled && (addr & PAGE_MASK) <= MEM_PAGE_SIZE - 2) {
        bool supervisor = g_active_write == g_supervisor_write;
        if (pe->dev && mmu_check_tt(g_mmu, addr, true, supervisor)) {
            pe->dev->write_uint16(pe->dev_context, addr - pe->base_addr, value);
            return;
        }
        if (mmu_handle_fault(g_mmu, addr, true, supervisor)) {
            uintptr_t base = g_active_write[addr >> PAGE_SHIFT];
            if (base != 0) {
                STORE_BE16((uint8_t *)(base + addr), value);
                return;
            }
            uint32_t phys = mmu_translate_debug(g_mmu, addr, supervisor);
            uint32_t phys_page = phys >> PAGE_SHIFT;
            if ((int)phys_page < g_page_count) {
                page_entry_t *phys_pe = &g_page_table[phys_page];
                if (phys_pe->dev) {
                    phys_pe->dev->write_uint16(phys_pe->dev_context, phys - phys_pe->base_addr, value);
                    return;
                }
            }
            if (logpoint_lookup(addr, &lp_host, &lp_writable) && lp_host && lp_writable) {
                STORE_BE16(lp_host, value);
                if (g_mem_logpoint_hook)
                    g_mem_logpoint_hook(addr, 2, value, true);
                return;
            }
        } else {
            if (!g_bus_error_pending) {
                g_bus_error_pending = true;
                g_bus_error_address = addr;
                g_bus_error_rw = false;
                g_bus_error_fc = supervisor ? 5 : 1;
                if (g_bus_error_instr_ptr)
                    *g_bus_error_instr_ptr = 0;
            }
        }
        return;
    }

    // MMU-off fallback: dispatch device on logical page-table entry.
    if (pe->dev && (addr & PAGE_MASK) <= MEM_PAGE_SIZE - 2) {
        pe->dev->write_uint16(pe->dev_context, addr - pe->base_addr, value);
        return;
    }

    // Cross-page: split into two byte writes
    memory_write_uint8(addr, (uint8_t)(value >> 8));
    memory_write_uint8((addr + 1) & g_address_mask, (uint8_t)(value & 0xFF));
}

// Slow path for 32-bit writes: cross-page or device I/O
void memory_write_uint32_slow(uint32_t addr, uint32_t value) {
    if (__builtin_expect(g_trace_page0, 0))
        memory_slow_write_page0_check(addr, 4);
    uint32_t page = addr >> PAGE_SHIFT;
    page_entry_t *pe = &g_page_table[page];

    // Memory logpoint: forced slow path for RAM write on logged page
    uint8_t *lp_host;
    bool lp_writable;
    if ((addr & PAGE_MASK) <= MEM_PAGE_SIZE - 4 && logpoint_lookup(addr, &lp_host, &lp_writable) && lp_host &&
        lp_writable) {
        STORE_BE32(lp_host, value);
        if (g_mem_logpoint_hook)
            g_mem_logpoint_hook(addr, 4, value, true);
        return;
    }

    // Lazy-install identity SoA for in-page writes to host-backed pages.
    if ((addr & PAGE_MASK) <= MEM_PAGE_SIZE - 4 && can_lazy_install(page, pe)) {
        rebuild_soa_page(page);
        if (pe->writable)
            STORE_BE32(pe->host_base + (addr & PAGE_MASK), value);
        return;
    }

    // Gate logical-device dispatch (24-bit Mac OS master-pointer fix).
    if (pe->dev && (addr & PAGE_MASK) <= MEM_PAGE_SIZE - 4 &&
        dispatch_device_at_logical(addr, g_active_write == g_supervisor_write)) {
        pe->dev->write_uint32(pe->dev_context, addr - pe->base_addr, value);
        return;
    }

    // When MMU is enabled, dispatch via PHYSICAL address (see write_uint8_slow
    // comment).  Cross-page accesses fall through to 16-bit writes.
    if (g_mmu && g_mmu->enabled && (addr & PAGE_MASK) <= MEM_PAGE_SIZE - 4) {
        bool supervisor = g_active_write == g_supervisor_write;
        if (pe->dev && mmu_check_tt(g_mmu, addr, true, supervisor)) {
            pe->dev->write_uint32(pe->dev_context, addr - pe->base_addr, value);
            return;
        }
        if (mmu_handle_fault(g_mmu, addr, true, supervisor)) {
            uintptr_t base = g_active_write[addr >> PAGE_SHIFT];
            if (base != 0) {
                STORE_BE32((uint8_t *)(base + addr), value);
                return;
            }
            uint32_t phys = mmu_translate_debug(g_mmu, addr, supervisor);
            uint32_t phys_page = phys >> PAGE_SHIFT;
            if ((int)phys_page < g_page_count) {
                page_entry_t *phys_pe = &g_page_table[phys_page];
                if (phys_pe->dev) {
                    phys_pe->dev->write_uint32(phys_pe->dev_context, phys - phys_pe->base_addr, value);
                    return;
                }
            }
            if (logpoint_lookup(addr, &lp_host, &lp_writable) && lp_host && lp_writable) {
                STORE_BE32(lp_host, value);
                if (g_mem_logpoint_hook)
                    g_mem_logpoint_hook(addr, 4, value, true);
                return;
            }
        } else {
            if (!g_bus_error_pending) {
                g_bus_error_pending = true;
                g_bus_error_address = addr;
                g_bus_error_rw = false;
                g_bus_error_fc = supervisor ? 5 : 1;
                if (g_bus_error_instr_ptr)
                    *g_bus_error_instr_ptr = 0;
            }
        }
        return;
    }

    // MMU-off fallback: dispatch device on logical page-table entry.
    if (pe->dev && (addr & PAGE_MASK) <= MEM_PAGE_SIZE - 4) {
        pe->dev->write_uint32(pe->dev_context, addr - pe->base_addr, value);
        return;
    }

    // Cross-page: split into two 16-bit writes
    memory_write_uint16(addr, (uint16_t)(value >> 16));
    memory_write_uint16(addr + 2, (uint16_t)(value & 0xFFFF));
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
        assert(0);
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
        assert(0);
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
    uint32_t guest_base = p << PAGE_SHIFT;
    uintptr_t adjusted = (uintptr_t)pe->host_base - guest_base;
    tlb_track_page(p); // ensure the next mmu_invalidate_tlb zeroes this entry
    if (g_supervisor_read)
        g_supervisor_read[p] = adjusted;
    if (g_user_read)
        g_user_read[p] = adjusted;
    if (pe->writable) {
        if (g_supervisor_write)
            g_supervisor_write[p] = adjusted;
        if (g_user_write)
            g_user_write[p] = adjusted;
    }
}

void memory_logpoint_install(uint32_t start_page, uint32_t end_page) {
    if (!g_mem_logpoint_page_count)
        return;
    for (uint32_t p = start_page; p <= end_page && p < g_page_count; p++) {
        if (g_mem_logpoint_page_count[p] < 0xFF)
            g_mem_logpoint_page_count[p]++;
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
}

void memory_logpoint_uninstall(uint32_t start_page, uint32_t end_page) {
    if (!g_mem_logpoint_page_count)
        return;
    for (uint32_t p = start_page; p <= end_page && p < g_page_count; p++) {
        if (g_mem_logpoint_page_count[p])
            g_mem_logpoint_page_count[p]--;
        if (g_mem_logpoint_page_count[p] == 0)
            rebuild_soa_page(p);
    }
}

void memory_logpoint_install_phys(uint32_t start_page, uint32_t end_page) {
    if (!g_mem_logpoint_phys_page_count)
        return;
    for (uint32_t p = start_page; p <= end_page && p < g_page_count; p++) {
        if (g_mem_logpoint_phys_page_count[p] < 0xFF)
            g_mem_logpoint_phys_page_count[p]++;
    }
    // We can't cheaply enumerate which logical pages currently alias the
    // watched physical pages, so conservatively invalidate the entire SoA
    // arrays.  All logical pages re-walk on next access, and the fill path
    // (mmu_fill_soa_entry) suppresses any alias hitting the watched physical.
    // One-time cost at install; fast-path unaffected once entries repopulate.
    if (g_supervisor_read)
        memset(g_supervisor_read, 0, (size_t)g_page_count * sizeof(uintptr_t));
    if (g_supervisor_write)
        memset(g_supervisor_write, 0, (size_t)g_page_count * sizeof(uintptr_t));
    if (g_user_read)
        memset(g_user_read, 0, (size_t)g_page_count * sizeof(uintptr_t));
    if (g_user_write)
        memset(g_user_write, 0, (size_t)g_page_count * sizeof(uintptr_t));
}

void memory_logpoint_uninstall_phys(uint32_t start_page, uint32_t end_page) {
    if (!g_mem_logpoint_phys_page_count)
        return;
    for (uint32_t p = start_page; p <= end_page && p < g_page_count; p++) {
        if (g_mem_logpoint_phys_page_count[p])
            g_mem_logpoint_phys_page_count[p]--;
    }
    // No need to rebuild SoA entries; they refill lazily on next access.
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

    // Populate page table entries for the device's address range
    if (g_page_table) {
        uint32_t start_page = (addr & g_address_mask) >> PAGE_SHIFT;
        uint32_t end_page = ((addr + size - 1) & g_address_mask) >> PAGE_SHIFT;
        assert(start_page < g_page_count && "device start address exceeds page table bounds");
        for (uint32_t p = start_page; p <= end_page && p < g_page_count; p++) {
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
}

// Clear page-table entries for [addr, addr+size) that point at `iface_ptr`.
// Called by memory_map_remove before freeing the mapping so the page table
// stops dereferencing a freed memory_interface.
static void clear_page_table_for_mapping(uint32_t addr, uint32_t size, const memory_interface_t *iface_ptr) {
    if (!g_page_table || size == 0)
        return;
    uint32_t start_page = (addr & g_address_mask) >> PAGE_SHIFT;
    uint32_t end_page = ((addr + size - 1) & g_address_mask) >> PAGE_SHIFT;
    for (uint32_t p = start_page; p <= end_page && p < g_page_count; p++) {
        if (g_page_table[p].dev == iface_ptr) {
            g_page_table[p].host_base = NULL;
            g_page_table[p].dev = NULL;
            g_page_table[p].dev_context = NULL;
            g_page_table[p].base_addr = 0;
            g_page_table[p].writable = false;
        }
    }
}

// Remove a memory-mapped device from the memory map
void memory_map_remove(memory_map_t *memory_map, uint32_t addr, uint32_t size, const char *name,
                       memory_interface_t *iface, void *device) {
    (void)name;
    (void)iface;
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

const char *memory_rom_filename(memory_map_t *mem) {
    return mem ? mem->rom_filename : NULL;
}

// Recompute the ROM checksum field. Reads the ROM region byte-by-byte so it
// works regardless of host alignment / endianness.
static void calculate_checksum(memory_map_t *rom) {
    if (!rom || !rom->image || rom->rom_size < 8)
        return;
    const uint8_t *p = rom->image + rom->ram_size;
    uint32_t sum = 0;
    // Skip the first 4 bytes (the stored checksum word) and iterate the rest
    // as big-endian 16-bit words. Matches the layout the Mac ROM's own
    // self-check uses.
    for (uint32_t i = 4; i + 1 < rom->rom_size; i += 2) {
        sum += ((uint32_t)p[i] << 8) | p[i + 1];
    }
    rom->checksum = sum;
}

// Copy ROM bytes into the rom region (immediately after RAM) and refresh the
// internal checksum. Truncates if size > mem->rom_size, drops nothing if
// size < mem->rom_size (the trailing bytes keep whatever they had — for
// freshly-allocated memory that's zero).
size_t memory_install_rom(memory_map_t *mem, const uint8_t *data, size_t size, const char *filename) {
    if (!mem || !mem->image || !data || size == 0)
        return 0;
    size_t copy_size = size < mem->rom_size ? size : mem->rom_size;
    memcpy(mem->image + mem->ram_size, data, copy_size);
    calculate_checksum(mem);
    if (mem->rom_filename) {
        free(mem->rom_filename);
        mem->rom_filename = NULL;
    }
    if (filename)
        mem->rom_filename = strdup(filename);
    return copy_size;
}

// Direct read access to the ROM region (read-only). Returns NULL if the
// memory map has no ROM bytes loaded yet.
const uint8_t *memory_rom_bytes(memory_map_t *mem) {
    return (mem && mem->image) ? mem->image + mem->ram_size : NULL;
}

uint32_t memory_rom_size(memory_map_t *mem) {
    return mem ? mem->rom_size : 0;
}

uint32_t memory_rom_checksum(memory_map_t *mem) {
    return mem ? mem->checksum : 0;
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
    uint32_t ram_pages = ram_size >> PAGE_SHIFT;
    for (uint32_t p = 0; p < ram_pages && p < g_page_count; p++) {
        assert((p << PAGE_SHIFT) < ram_size && "RAM page index out of bounds");
        uint8_t *host_ptr = mem->image + (p << PAGE_SHIFT);
        uint32_t guest_base = p << PAGE_SHIFT;
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
    }

    // ROM pages: rom_start_addr – rom_region_end (read-only, mirrored)
    // On Mac hardware, address line A17 (relative to ROM base) distinguishes ROM from I/O:
    //   offset % (2*rom_size) in [0, rom_size)       → ROM content
    //   offset % (2*rom_size) in [rom_size, 2*rom_size) → I/O / undefined (left unmapped)
    // This produces ROM mirrors at every 2*rom_size (256 KB for the 128 KB Plus ROM),
    // matching the old flat-buffer copy loop: addr += 2 * ROM_SIZE.
    uint32_t rom_start_page = (rom_start_addr & g_address_mask) >> PAGE_SHIFT;
    uint32_t rom_end_page = (rom_region_end & g_address_mask) >> PAGE_SHIFT;
    uint32_t rom_pages = rom_size >> PAGE_SHIFT; // content pages per ROM copy
    uint32_t mirror_stride = rom_pages * 2; // mirror cycle in pages (A17 stride)

    // Protect against a zero-length ROM (degenerate case)
    if (rom_pages == 0)
        return;

    for (uint32_t p = rom_start_page; p < rom_end_page && p < g_page_count; p++) {
        uint32_t offset_in_cycle = (p - rom_start_page) % mirror_stride;
        if (offset_in_cycle >= rom_pages)
            continue; // interleaved I/O / undefined range — leave page unmapped (returns 0)
        uint8_t *host_ptr = mem->image + ram_size + (offset_in_cycle << PAGE_SHIFT);
        uint32_t guest_base = p << PAGE_SHIFT;
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
    }
}

// Populate page-table entries for a RAM-mirror region.  Each page in
// [mirror_start, mirror_end) is set up as a full read+write alias of RAM at
// (guest_page << PAGE_SHIFT) % ram_size.  This models the physical
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
    uint32_t start_page = (mirror_start & g_address_mask) >> PAGE_SHIFT;
    uint32_t end_page = (mirror_end & g_address_mask) >> PAGE_SHIFT;

    for (uint32_t p = start_page; p < end_page && p < g_page_count; p++) {
        uint32_t guest_base = p << PAGE_SHIFT;
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
    }
}

// ============================================================================
// Lifecycle: Constructor
// ============================================================================

// Allocate and initialise a memory map for the given address space and RAM/ROM sizes.
// The page table is allocated dynamically based on address_bits.
// Machine-specific memory layout (page table population) is done by the machine's
// memory_layout_init callback, not here.
memory_map_t *memory_map_init(int address_bits, uint32_t ram_size, uint32_t rom_size, checkpoint_t *checkpoint) {
    // Validate address_bits before deciding the page-table shape so a 28 or 0
    // doesn't silently default to the 24-bit layout.
    GS_ASSERTF(address_bits == 24 || address_bits == 32, "memory_map_init: address_bits must be 24 or 32 (got %d)",
               address_bits);
    // A double-init without an intervening memory_map_delete would leak the
    // previous tables. The current architecture only creates one map per
    // machine, so flag the inconsistency.
    GS_ASSERTF(g_page_table == NULL, "memory_map_init: previous memory map not torn down before re-init");

    memory_map_t *mem = (memory_map_t *)calloc(1, sizeof(memory_map_t));
    GS_ASSERTF(mem != NULL, "memory_map_init: out of memory allocating memory_map_t");

    // Store parameterised sizes for later use by layout, checkpoint, and cmd_rom
    mem->ram_size = ram_size;
    mem->rom_size = rom_size;

    // Allocate the flat RAM+ROM image (ram_size + rom_size bytes)
    size_t image_size = (size_t)ram_size + (size_t)rom_size;
    mem->image = calloc(1, image_size);
    GS_ASSERTF(mem->image != NULL, "memory_map_init: out of memory allocating %zu-byte image", image_size);

    // TEMP DIAG (GS_PAGE0_TRACE): physical page 0 = first MEM_PAGE_SIZE of RAM.
    g_phys0_host_lo = (uintptr_t)mem->image;
    g_phys0_host_hi = (uintptr_t)mem->image + MEM_PAGE_SIZE;
    g_trace_page0 = getenv("GS_PAGE0_TRACE") ? 1 : 0;
    if (g_trace_page0)
        fprintf(stderr, "[PAGE0CFG] g_trace_page0=%d phys0_host=[%016llx,%016llx)\n", g_trace_page0,
                (unsigned long long)g_phys0_host_lo, (unsigned long long)g_phys0_host_hi);

    // Allocate page table sized for the given address space
    if (address_bits == 32) {
        g_address_mask = 0xFFFFFFFFUL; // full 32-bit
        g_page_count = 1 << (32 - PAGE_SHIFT); // 1,048,576 pages
    } else {
        // 24-bit (Macintosh Plus / SE)
        g_address_mask = 0x00FFFFFFUL;
        g_page_count = 1 << (24 - PAGE_SHIFT); // 4,096 pages
    }

    // AoS cold-path page table (device dispatch)
    g_page_table = (page_entry_t *)calloc(g_page_count, sizeof(page_entry_t));
    assert(g_page_table != NULL && "failed to allocate page table");
    mem->page_table = g_page_table;
    mem->page_count = g_page_count;

    // SoA fast-path arrays (calloc → zero = slow path for all pages initially)
    g_supervisor_read = (uintptr_t *)calloc(g_page_count, sizeof(uintptr_t));
    g_supervisor_write = (uintptr_t *)calloc(g_page_count, sizeof(uintptr_t));
    g_user_read = (uintptr_t *)calloc(g_page_count, sizeof(uintptr_t));
    g_user_write = (uintptr_t *)calloc(g_page_count, sizeof(uintptr_t));
    assert(g_supervisor_read && g_supervisor_write && g_user_read && g_user_write);

    // Memory logpoint reference-count array (zero = no logpoint on that page)
    g_mem_logpoint_page_count = (uint8_t *)calloc(g_page_count, sizeof(uint8_t));
    assert(g_mem_logpoint_page_count);

    // Physical-page logpoint reference count.  Sized the same way as the
    // logical array so any physical page the guest can reach is coverable.
    g_mem_logpoint_phys_page_count = (uint8_t *)calloc(g_page_count, sizeof(uint8_t));
    assert(g_mem_logpoint_phys_page_count);

    // Default active pointers: supervisor mode
    g_active_read = g_supervisor_read;
    g_active_write = g_supervisor_write;

    // Note: rom command is registered once from setup_init() so it's
    // available before any machine is created (deferred boot).

    // Load from checkpoint if provided
    if (checkpoint) {
        // Restore RAM
        system_read_checkpoint_data(checkpoint, mem->image, ram_size);
        // Restore ROM (content or reference)
        char *restored_path = NULL;
        size_t got = checkpoint_read_file(checkpoint, mem->image + ram_size, rom_size, &restored_path);
        if (restored_path) {
            if (mem->rom_filename)
                free(mem->rom_filename);
            mem->rom_filename = restored_path;
        }
        if (got > 0) {
            calculate_checksum(mem);
        }
    }

    // Object-tree binding — instance_data is the memory_map_t itself,
    // memory.peek's accessors call into the global memory_read_* helpers
    // directly so its instance_data is unused.
    mem->memory_object = object_new(&memory_class, mem, "memory");
    if (mem->memory_object) {
        object_attach(object_root(), mem->memory_object);
        mem->peek_object = object_new(&mem_peek_class, NULL, "peek");
        if (mem->peek_object)
            object_attach(mem->memory_object, mem->peek_object);
        mem->poke_object = object_new(&mem_poke_class, NULL, "poke");
        if (mem->poke_object)
            object_attach(mem->memory_object, mem->poke_object);
    }

    return mem;
}

// ============================================================================
// Lifecycle: Destructor
// ============================================================================

// Free resources associated with a memory map instance
void memory_map_delete(memory_map_t *mem) {
    if (!mem)
        return;
    if (mem->peek_object) {
        object_detach(mem->peek_object);
        object_delete(mem->peek_object);
        mem->peek_object = NULL;
    }
    if (mem->poke_object) {
        object_detach(mem->poke_object);
        object_delete(mem->poke_object);
        mem->poke_object = NULL;
    }
    if (mem->memory_object) {
        object_detach(mem->memory_object);
        object_delete(mem->memory_object);
        mem->memory_object = NULL;
    }
    // Free mappings
    mapping_t *m = mem->map;
    while (m) {
        mapping_t *next = m->next;
        if (m->name)
            free(m->name);
        free(m);
        m = next;
    }
    // Free this instance's page table
    if (mem->page_table) {
        // Only clear globals if this instance owns the active page table
        if (g_page_table == mem->page_table) {
            g_page_table = NULL;
            g_page_count = 0;

            // Free SoA fast-path arrays
            free(g_supervisor_read);
            g_supervisor_read = NULL;
            free(g_supervisor_write);
            g_supervisor_write = NULL;
            free(g_user_read);
            g_user_read = NULL;
            free(g_user_write);
            g_user_write = NULL;
            g_active_read = NULL;
            g_active_write = NULL;

            // Free logpoint page-count arrays
            free(g_mem_logpoint_page_count);
            g_mem_logpoint_page_count = NULL;
            free(g_mem_logpoint_phys_page_count);
            g_mem_logpoint_phys_page_count = NULL;
        }
        free(mem->page_table);
        mem->page_table = NULL;
    }
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

        printf("0x%08x - 0x%08x: %s\n", map->addr, map->addr + map->size - 1, map->name);
        map = map->next;
    }
}

// === Object-model class descriptor =========================================
//
// instance_data on the memory node is the memory_map_t* itself.
// Lifetime is tied to memory_map_init / memory_map_delete.

static value_t attr_mem_ram_size(struct object *self, const member_t *m) {
    (void)m;
    memory_map_t *mem = (memory_map_t *)object_data(self);
    return val_uint(4, mem ? mem->ram_size : 0u);
}

static value_t attr_mem_rom_size(struct object *self, const member_t *m) {
    (void)m;
    memory_map_t *mem = (memory_map_t *)object_data(self);
    return val_uint(4, mem ? mem->rom_size : 0u);
}

// === memory.read_cstring ====================================================
//
// Read a NUL-terminated string from guest memory at addr, escaping
// non-printable bytes. Used to migrate the legacy `$str.<src>`
// vocabulary onto the unified ${...} interpolator.

static value_t method_mem_read_cstring(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    uint32_t addr = (uint32_t)argv[0].u;
    int max_chars = 96;
    if (argc >= 2) {
        int64_t mc = argv[1].i;
        if (mc > 0 && mc <= 4096)
            max_chars = (int)mc;
    }
    char buf[8192];
    size_t out = 0;
    buf[out++] = '"';
    // Reserve 2 bytes for the closing quote and the NUL terminator so the
    // body never lands the buffer in a state where the closing quote drops.
    for (int i = 0; i < max_chars && out + 4 + 2 <= sizeof(buf); i++) {
        uint8_t b = memory_debug_read_uint8(addr + (uint32_t)i);
        if (b == 0)
            break;
        if (b >= 0x20 && b <= 0x7E) {
            buf[out++] = (char)b;
        } else {
            int n = snprintf(buf + out, sizeof(buf) - out, "\\x%02X", b);
            if (n < 0)
                break;
            out += (size_t)n;
        }
    }
    buf[out++] = '"';
    buf[out] = '\0';
    return val_str(buf);
}

static const arg_decl_t mem_read_cstring_args[] = {
    {.name = "addr",      .kind = V_UINT, .presentation_flags = VAL_HEX,        .doc = "guest memory address"          },
    {.name = "max_chars", .kind = V_INT,  .validation_flags = OBJ_ARG_OPTIONAL, .doc = "max chars to read (default 96)"},
};

// `memory.dump(addr, [count])` — hex-dump `count` bytes from `addr`.
// Replaces the legacy gdb-style `x` / `examine` command. `addr` accepts an
// integer or a string (alias / register name / expression resolved by the
// rich-parser). Output goes to stdout in the legacy `x` layout; the method
// returns true on dispatch success.
static value_t method_mem_dump(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    // addr is V_NONE-kind: body discriminates integer vs string.
    bool addr_ok = false;
    uint64_t addr_u = val_as_u64(&argv[0], &addr_ok);
    bool addr_is_str = (argv[0].kind == V_STRING);
    if (!addr_ok && !addr_is_str)
        return val_err("memory.dump: addr must be integer or string");
    int64_t count = 0;
    bool have_count = false;
    if (argc >= 2) {
        count = argv[1].i;
        have_count = true;
    }
    char line[128];
    int n;
    if (have_count) {
        if (addr_ok)
            n = snprintf(line, sizeof(line), "x 0x%llx %lld", (unsigned long long)addr_u, (long long)count);
        else
            n = snprintf(line, sizeof(line), "x %s %lld", argv[0].s ? argv[0].s : "", (long long)count);
    } else {
        if (addr_ok)
            n = snprintf(line, sizeof(line), "x 0x%llx", (unsigned long long)addr_u);
        else
            n = snprintf(line, sizeof(line), "x %s", argv[0].s ? argv[0].s : "");
    }
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("memory.dump: argument too long");
    char *targv[32];
    int targc = tokenize(line, targv, 32);
    if (targc <= 0)
        return val_err("memory.dump: tokenisation failed");
    return val_bool(shell_examine_argv(targc, targv) == 0);
}

static const arg_decl_t mem_dump_args[] = {
    {.name = "addr", .kind = V_NONE, .doc = "guest memory address (integer or alias/expression)"},
    {.name = "count", .kind = V_INT, .validation_flags = OBJ_ARG_OPTIONAL, .doc = "byte count (default 16)"},
};

static const member_t memory_members[] = {
    {.kind = M_ATTR,
     .name = "ram_size",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = attr_mem_ram_size, .set = NULL}                                         },
    {.kind = M_ATTR,
     .name = "rom_size",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = attr_mem_rom_size, .set = NULL}                                         },
    {.kind = M_METHOD,
     .name = "read_cstring",
     .doc = "Read a quoted, escape-encoded C string at addr",
     .method = {.args = mem_read_cstring_args, .nargs = 2, .result = V_STRING, .fn = method_mem_read_cstring}},
    {.kind = M_METHOD,
     .name = "dump",
     .doc = "Hex-dump count bytes at addr (replaces the legacy `x` / examine)",
     .method = {.args = mem_dump_args, .nargs = 2, .result = V_BOOL, .fn = method_mem_dump}                  },
};

const class_desc_t memory_class = {
    .name = "memory",
    .members = memory_members,
    .n_members = sizeof(memory_members) / sizeof(memory_members[0]),
};

// === memory.peek child class ================================================
//
// Three methods (b/w/l) that read sized values from guest memory at a
// caller-supplied address. Used by ${...} interpolation in logpoint
// messages (proposal §5.3) and any expression that needs a peek.

static value_t method_mem_peek_b(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    value_t v = val_uint(1, memory_debug_read_uint8((uint32_t)argv[0].u));
    v.flags |= VAL_HEX;
    return v;
}
static value_t method_mem_peek_w(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    value_t v = val_uint(2, memory_debug_read_uint16((uint32_t)argv[0].u));
    v.flags |= VAL_HEX;
    return v;
}
static value_t method_mem_peek_l(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    value_t v = val_uint(4, memory_debug_read_uint32((uint32_t)argv[0].u));
    v.flags |= VAL_HEX;
    return v;
}

// `memory.peek.bytes(addr, count)` — bulk byte read. Returns a
// V_BYTES blob, capped to 4 KB so the bridge output slot can hold the
// JSON-encoded payload. Replaces the per-byte fan-out the debug UI's
// memory pane used to do (128 separate gsEval calls → 128 bridge
// round-trips → noticeable lag while stepping). One call now suffices.
static value_t method_mem_peek_bytes(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    uint32_t addr = (uint32_t)argv[0].u;
    uint64_t count = argv[1].u;
    if (count == 0)
        return val_bytes(NULL, 0);
    // Cap at 4 KB. The bridge serialises V_BYTES as a base64-ish JSON
    // string; 4 KB × 4/3 ≈ 5.5 KB, well under JS_BRIDGE_OUTPUT_SIZE.
    if (count > 4096)
        count = 4096;
    uint8_t *buf = (uint8_t *)malloc(count);
    if (!buf)
        return val_err("memory.peek.bytes: out of memory");
    for (uint64_t i = 0; i < count; i++)
        buf[i] = memory_debug_read_uint8((uint32_t)(addr + i));
    value_t v = val_bytes(buf, (size_t)count);
    free(buf);
    return v;
}

static const arg_decl_t mem_peek_args[] = {
    {.name = "addr", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "guest memory address"},
};

static const arg_decl_t mem_peek_bytes_args[] = {
    {.name = "addr", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "guest memory address"},
    {.name = "count", .kind = V_UINT, .doc = "byte count (max 4096)"},
};

static const member_t mem_peek_members[] = {
    {.kind = M_METHOD,
     .name = "b",
     .doc = "Read 1 byte at addr",
     .method = {.args = mem_peek_args, .nargs = 1, .result = V_UINT, .fn = method_mem_peek_b}           },
    {.kind = M_METHOD,
     .name = "w",
     .doc = "Read 2 bytes (big-endian word) at addr",
     .method = {.args = mem_peek_args, .nargs = 1, .result = V_UINT, .fn = method_mem_peek_w}           },
    {.kind = M_METHOD,
     .name = "l",
     .doc = "Read 4 bytes (big-endian long) at addr",
     .method = {.args = mem_peek_args, .nargs = 1, .result = V_UINT, .fn = method_mem_peek_l}           },
    {.kind = M_METHOD,
     .name = "bytes",
     .doc = "Read `count` bytes at addr (bulk; max 4096 bytes per call)",
     .method = {.args = mem_peek_bytes_args, .nargs = 2, .result = V_BYTES, .fn = method_mem_peek_bytes}},
};

const class_desc_t mem_peek_class = {
    .name = "peek",
    .members = mem_peek_members,
    .n_members = sizeof(mem_peek_members) / sizeof(mem_peek_members[0]),
};

// === memory.poke child class ================================================
//
// Three methods (b/w/l) that write sized values to guest memory at a
// caller-supplied address. Pairs with memory.peek, replacing the legacy
// `set <addr>.<size> <value>` shell form.

static value_t method_mem_poke_b(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    memory_debug_write_uint8((uint32_t)argv[0].u, (uint8_t)argv[1].u);
    return val_none();
}
static value_t method_mem_poke_w(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    memory_debug_write_uint16((uint32_t)argv[0].u, (uint16_t)argv[1].u);
    return val_none();
}
static value_t method_mem_poke_l(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    memory_debug_write_uint32((uint32_t)argv[0].u, (uint32_t)argv[1].u);
    return val_none();
}

static const arg_decl_t mem_poke_args[] = {
    {.name = "addr",  .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "guest memory address"},
    {.name = "value", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "value to write"      },
};

static const member_t mem_poke_members[] = {
    {.kind = M_METHOD,
     .name = "b",
     .doc = "Write 1 byte at addr",
     .method = {.args = mem_poke_args, .nargs = 2, .result = V_NONE, .fn = method_mem_poke_b}},
    {.kind = M_METHOD,
     .name = "w",
     .doc = "Write 2 bytes (big-endian word) at addr",
     .method = {.args = mem_poke_args, .nargs = 2, .result = V_NONE, .fn = method_mem_poke_w}},
    {.kind = M_METHOD,
     .name = "l",
     .doc = "Write 4 bytes (big-endian long) at addr",
     .method = {.args = mem_poke_args, .nargs = 2, .result = V_NONE, .fn = method_mem_poke_l}},
};

const class_desc_t mem_poke_class = {
    .name = "poke",
    .members = mem_poke_members,
    .n_members = sizeof(mem_poke_members) / sizeof(mem_poke_members[0]),
};
