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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Constants and Macros
// ============================================================================

// (Plus-specific PLUS_ROM_SIZE and rom_version/rom_file_version removed in M6;
//  ROM identification is now handled by rom.c)

// Page table globals (defined here, declared extern in memory.h)
page_entry_t *g_page_table = NULL;
uint32_t g_address_mask = 0x00FFFFFFUL; // 24-bit default
int g_page_count = 0; // number of pages in current page table

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
uint32_t g_bus_error_pending = 0;
uint32_t g_bus_error_address = 0;
uint32_t g_bus_error_rw = 0;
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
uint32_t g_bus_error_is_pmmu = 0;
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

struct mapping;
typedef struct mapping mapping_t;

struct mapping {

    mapping_t *next;

    char *name;

    void *device;

    uint32_t addr;
    uint32_t size;

    memory_interface_t memory_interface;
};

typedef struct memory {

    int i;

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

    bool checksum_valid;

    int version;

    // Object-tree binding — lifetime tied to memory_map_init / delete.
    struct object *memory_object;
    struct object *peek_object;

} memory_map_t;

// ============================================================================
// Page Table Slow Paths
// ============================================================================
// The slow path is entered when the SoA fast-path entry is zero.
// Addresses arriving here are already masked by g_address_mask.
// Dispatch order: device I/O (via page_entry_t) → MMU TLB handling via
// mmu_handle_fault(...) and deferred bus error signaling → unmapped (return $FF).

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
    if (pe->dev)
        return pe->dev->read_uint8(pe->dev_context, addr - pe->base_addr);
    // MMU TLB miss: try to resolve via table walk and retry
    if (g_mmu && g_mmu->enabled) {
        if (mmu_handle_fault(g_mmu, addr, false, g_active_read == g_supervisor_read)) {
            uintptr_t base = g_active_read[addr >> PAGE_SHIFT];
            if (base != 0)
                return LOAD_BE8((uint8_t *)(base + addr));
            // SoA still 0: either unmapped physical (no fault, e.g. TT-matched
            // pseudo-slot) or a logpoint page where fill was suppressed.  Re-
            // check the logpoint now that mmu_handle_fault has run (physical-
            // space logpoints suppress the fill and require translation here).
            if (logpoint_lookup(addr, &lp_host, &lp_writable) && lp_host) {
                uint8_t v = LOAD_BE8(lp_host);
                if (g_mem_logpoint_hook)
                    g_mem_logpoint_hook(addr, 1, v, false);
                return v;
            }
        } else {
            // MMU fault (invalid descriptor, unmapped physical, permission, etc.)
            if (!g_bus_error_pending) {
                g_bus_error_pending = 1;
                g_bus_error_address = addr;
                g_bus_error_rw = 1; // read
                g_bus_error_fc = (g_active_read == g_supervisor_read) ? 5 : 1;
                if (g_bus_error_instr_ptr)
                    *g_bus_error_instr_ptr = 0; // force decoder loop exit
            }
        }
    }
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

    // Device I/O (single page, not crossing boundary)
    if (pe->dev && (addr & PAGE_MASK) <= MEM_PAGE_SIZE - 2)
        return pe->dev->read_uint16(pe->dev_context, addr - pe->base_addr);

    // Cross-page or host memory at page boundary: split into two byte reads
    uint16_t hi = memory_read_uint8(addr);
    uint16_t lo = memory_read_uint8((addr + 1) & g_address_mask);
    return (hi << 8) | lo;
}

// Slow path for 32-bit reads: cross-page or device I/O
uint32_t memory_read_uint32_slow(uint32_t addr) {
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

    // Device I/O (single page, not crossing boundary)
    if (pe->dev && (addr & PAGE_MASK) <= MEM_PAGE_SIZE - 4) {
        uint32_t v = pe->dev->read_uint32(pe->dev_context, addr - pe->base_addr);
        if (g_value_trap_active && addr == 0x50006000 && (v == 0x4244E607 || v == 0x004244E6)) {
            fprintf(stderr, "[mem.read_uint32_slow MATCH] addr=$%08X dev_returned=$%08X\n", addr, v);
        }
        return v;
    }

    // Cross-page: split into two 16-bit reads
    uint32_t hi = memory_read_uint16(addr);
    uint32_t lo = memory_read_uint16(addr + 2);
    if (g_value_trap_active && addr == 0x50006000) {
        static int t = 0;
        if (t < 8) {
            fprintf(stderr, "[mem.read_uint32_slow CROSS-PAGE] addr=$%08X hi=$%04X lo=$%04X\n", addr, hi, lo);
            t++;
        }
    }
    return (hi << 16) | lo;
}

// Slow path for 8-bit writes: device I/O, MMU TLB miss, or unmapped
void memory_write_uint8_slow(uint32_t addr, uint8_t value) {
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
    if (pe->dev) {
        pe->dev->write_uint8(pe->dev_context, addr - pe->base_addr, value);
        return;
    }
    // MMU TLB miss: try to resolve via table walk and retry
    if (g_mmu && g_mmu->enabled) {
        if (mmu_handle_fault(g_mmu, addr, true, g_active_write == g_supervisor_write)) {
            uintptr_t base = g_active_write[addr >> PAGE_SHIFT];
            if (base != 0) {
                STORE_BE8((uint8_t *)(base + addr), value);
                return;
            }
            // SoA still 0: either unmapped or suppressed for logpoint.  Re-
            // check now that the fault has run — physical-space logpoints are
            // only detectable after mmu_translate_debug can reach the page.
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
                g_bus_error_pending = 1;
                g_bus_error_address = addr;
                g_bus_error_rw = 0; // write
                g_bus_error_fc = (g_active_write == g_supervisor_write) ? 5 : 1;
                if (g_bus_error_instr_ptr)
                    *g_bus_error_instr_ptr = 0; // force decoder loop exit
            }
        }
    }
}

// Slow path for 16-bit writes: cross-page or device I/O
void memory_write_uint16_slow(uint32_t addr, uint16_t value) {
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

    // Device I/O (single page, not crossing boundary)
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

    // Device I/O (single page, not crossing boundary)
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
static void rebuild_soa_page(uint32_t p) {
    if ((int)p >= g_page_count)
        return;
    page_entry_t *pe = &g_page_table[p];
    // MMU-mapped pages rebuild themselves via mmu_handle_fault on next access.
    // Only plain RAM/ROM pages (host_base != NULL, no MMU) rebuild here when
    // the MMU is disabled — otherwise the MMU owns the SoA entry.
    if (g_mmu && g_mmu->enabled)
        return; // let the TLB re-fill via the slow path
    if (!pe->host_base)
        return; // device/unmapped page: leave SoA at 0
    uint32_t guest_base = p << PAGE_SHIFT;
    uintptr_t adjusted = (uintptr_t)pe->host_base - guest_base;
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
    for (uint32_t p = start_page; p <= end_page && (int)p < g_page_count; p++) {
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
    for (uint32_t p = start_page; p <= end_page && (int)p < g_page_count; p++) {
        if (g_mem_logpoint_page_count[p])
            g_mem_logpoint_page_count[p]--;
        if (g_mem_logpoint_page_count[p] == 0)
            rebuild_soa_page(p);
    }
}

void memory_logpoint_install_phys(uint32_t start_page, uint32_t end_page) {
    if (!g_mem_logpoint_phys_page_count)
        return;
    for (uint32_t p = start_page; p <= end_page && (int)p < g_page_count; p++) {
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
    for (uint32_t p = start_page; p <= end_page && (int)p < g_page_count; p++) {
        if (g_mem_logpoint_phys_page_count[p])
            g_mem_logpoint_phys_page_count[p]--;
    }
    // No need to rebuild SoA entries; they refill lazily on next access.
}

// ============================================================================
// Static Helpers
// ============================================================================

// Placeholder read handler for unmapped memory (8-bit)
static uint8_t phase_read_uint8(void *dev, uint32_t addr) {
    return 0;
}

// Placeholder read handler for unmapped memory (16-bit)
static uint16_t phase_read_uint16(void *dev, uint32_t addr) {
    return 0;
}

// Placeholder read handler for unmapped memory (32-bit)
static uint32_t phase_read_uint32(void *dev, uint32_t addr) {
    return 0;
}

// ============================================================================
// Operations
// ============================================================================

// Add a memory-mapped device to the memory map (linked list + page table)
void memory_map_add(memory_map_t *mem, uint32_t addr, uint32_t size, const char *name, memory_interface_t *iface,
                    void *device) {
    // Add to linked list (for memory_map_print / memory_map_remove)
    mapping_t *map = (mapping_t *)malloc(sizeof(mapping_t));

    memset(map, 0, sizeof(mapping_t));

    map->name = strdup(name);
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
        assert((int)start_page < g_page_count && "device start address exceeds page table bounds");
        for (uint32_t p = start_page; p <= end_page && (int)p < g_page_count; p++) {
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

// Remove a memory-mapped device from the memory map
void memory_map_remove(memory_map_t *memory_map, uint32_t addr, uint32_t size, const char *name,
                       memory_interface_t *iface, void *device) {
    mapping_t *map = memory_map->map;

    if (map->device == device && map->addr == addr) {
        free(map->name);
        memory_map->map = map->next;
        free(map);
    } else
        while (map->next != NULL) {
            if (map->next->device == device && map->next->addr == addr) {
                mapping_t *tmp = map->next;
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

// Calculate and validate ROM checksum and identify ROM version
static void calculate_checksum(memory_map_t *rom) {
    int i;

    // ROM content begins at ram_size offset in the flat buffer
    uint16_t *image16 = (uint16_t *)(rom->image + rom->ram_size);
    uint32_t *image32 = (uint32_t *)(rom->image + rom->ram_size);

    rom->checksum = 0;

    for (i = 2; i < (int)(rom->rom_size / 2); i++)
        rom->checksum += BE16(image16[i]);

    rom->checksum_valid = (rom->checksum == BE32(image32[0]));

    switch (BE32(image32[0])) {

    case 0x4D1EEEE1: // version 1 (Lonely Hearts)
        rom->version = 1;
        break;
    case 0x4D1EEAE1: // version 2 (Lonely Heifers)
        rom->version = 2;
        break;
    case 0x4D1F8172: // version 3 (Loud Harmonicas)
        rom->version = 3;
        break;
    default:
        rom->version = -1;
        break;
    }
}

// ============================================================================
// Shell Commands
// ============================================================================

// Pending ROM path — set before machine init so VROM loading can find it.
static char *s_pending_rom_path = NULL;

const char *memory_pending_rom_path(void) {
    return s_pending_rom_path;
}

// Pending VROM path — set via "rom load-vrom" before machine init.
static char *s_pending_vrom_path = NULL;

const char *memory_pending_vrom_path(void) {
    return s_pending_vrom_path;
}

// Helper: read a ROM file into a buffer, returning size via out_size.
// Returns NULL on failure; caller must free the returned buffer.
static uint8_t *read_rom_file(const char *filename, size_t *out_size, bool quiet) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        if (!quiet)
            printf("Failed to open ROM file: %s\n", filename);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size <= 0) {
        fclose(f);
        if (!quiet)
            printf("Failed to read ROM file: %s\n", filename);
        return NULL;
    }
    uint8_t *rom_data = malloc((size_t)file_size);
    if (!rom_data) {
        fclose(f);
        if (!quiet)
            printf("Failed to allocate memory for ROM\n");
        return NULL;
    }
    size_t n = fread(rom_data, 1, (size_t)file_size, f);
    fclose(f);
    if ((long)n != file_size) {
        free(rom_data);
        if (!quiet)
            printf("Failed to read ROM file: %s\n", filename);
        return NULL;
    }
    *out_size = (size_t)file_size;
    return rom_data;
}

// rom load <path>: Load a ROM file, identify the machine, and reset CPU.
uint64_t cmd_rom_load(const char *filename) {
    size_t file_size = 0;
    uint8_t *rom_data = read_rom_file(filename, &file_size, false);
    if (!rom_data)
        return (uint64_t)-1;

    uint32_t checksum = 0;
    const rom_info_t *info = rom_identify_data(rom_data, file_size, &checksum);

    if (info) {
        printf("ROM: %s (checksum %08X)\n", info->model_name, checksum);
    } else {
        printf("ROM: unknown (checksum %08X, size %zu bytes)\n", checksum, file_size);
        free(rom_data);
        return (uint64_t)-1;
    }

    // Store pending ROM path so machine init code (e.g. SE/30 VROM loader)
    // can find related files next to the ROM.
    free(s_pending_rom_path);
    s_pending_rom_path = strdup(filename);

    // Ensure the correct machine is active (creates or switches as needed)
    if (system_ensure_machine(info->model_id) != 0) {
        free(rom_data);
        return (uint64_t)-1;
    }

    // Load ROM data into machine memory
    memory_map_t *mem = system_memory();
    if (!mem) {
        printf("rom: memory not initialized\n");
        free(rom_data);
        return (uint64_t)-1;
    }

    // Copy ROM data into the flat buffer at ram_size offset
    size_t copy_size = file_size < mem->rom_size ? file_size : mem->rom_size;
    memcpy(mem->image + mem->ram_size, rom_data, copy_size);
    free(rom_data);

    // Update memory-level checksum state
    calculate_checksum(mem);

    // Remember ROM filename for checkpointing
    if (mem->rom_filename)
        free(mem->rom_filename);
    mem->rom_filename = strdup(filename);

    // Reset CPU from ROM reset vectors (SSP at ROM offset 0, PC at ROM offset 4).
    // Read directly from the ROM buffer since address 0 may map to RAM, not ROM
    // (e.g. Plus has ROM at 0x400000, not overlaid at address 0).
    cpu_t *cpu = system_cpu();
    if (cpu && copy_size >= 8) {
        uint8_t *rom_base = mem->image + mem->ram_size;
        uint32_t initial_ssp = LOAD_BE32(rom_base);
        uint32_t initial_pc = LOAD_BE32(rom_base + 4);
        cpu_set_an(cpu, 7, initial_ssp);
        cpu_set_pc(cpu, initial_pc);
        printf("CPU reset: PC=%08X SSP=%08X\n", initial_pc, initial_ssp);
    }

    printf("ROM loaded successfully from %s\n", filename);
    return 0;
}

// rom checksum <path>: Validate and print checksum, or print "0" if invalid.
static uint64_t cmd_rom_checksum(const char *filename) {
    size_t file_size = 0;
    uint8_t *rom_data = read_rom_file(filename, &file_size, true);
    if (!rom_data) {
        printf("0\n");
        return 1;
    }

    uint32_t checksum = 0;
    const rom_info_t *info = rom_identify_data(rom_data, file_size, &checksum);
    free(rom_data);

    if (info) {
        printf("%08X\n", checksum);
        return 0;
    }
    printf("0\n");
    return 1;
}

// rom probe [<path>]: Check if a ROM file or current ROM is valid.
uint64_t cmd_rom_probe(int argc, char *argv[], int filename_arg) {
    if (argc < filename_arg + 1) {
        // No filename: check if a ROM is currently loaded
        memory_map_t *mem = system_memory();
        return (mem && mem->rom_filename) ? 0 : 1;
    }
    const char *filename = argv[filename_arg];
    size_t file_size = 0;
    uint8_t *rom_data = read_rom_file(filename, &file_size, true);
    if (!rom_data)
        return 1;
    uint32_t checksum = 0;
    const rom_info_t *info = rom_identify_data(rom_data, file_size, &checksum);
    free(rom_data);
    return info ? 0 : 1;
}

// Shell command: ROM operations.
//   rom load <path>       Load and identify ROM, create machine, reset CPU
//   rom checksum <path>   Print checksum hex if valid, "0" if not
//   rom probe [<path>]    Return 0 if valid ROM, 1 if not (no output)
uint64_t cmd_rom(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: rom load <path> | --checksum <path> | --probe [<path>]\n");
        return 0;
    }

    const char *action = argv[1];

    if (strcmp(action, "load") == 0) {
        if (argc < 3) {
            printf("Usage: rom load <path>\n");
            return (uint64_t)-1;
        }
        return cmd_rom_load(argv[2]);
    }

    if (strcmp(action, "checksum") == 0) {
        if (argc < 3) {
            printf("Usage: rom checksum <path>\n");
            return (uint64_t)-1;
        }
        return cmd_rom_checksum(argv[2]);
    }

    if (strcmp(action, "probe") == 0) {
        return cmd_rom_probe(argc, argv, 2);
    }

    printf("Usage: rom load <path> | --checksum <path> | --probe [<path>]\n");
    return (uint64_t)-1;
}

// Shell command: Video ROM operations.
//   vrom load <path>      Set VROM path for next machine init (SE/30)
//   vrom checksum <path>  Validate VROM file size (must be 32 KB)
//   vrom probe [<path>]   Return 0 if valid VROM, 1 if not
uint64_t cmd_vrom(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: vrom load <path> | --checksum <path> | --probe [<path>]\n");
        return 0;
    }

    const char *action = argv[1];

    if (strcmp(action, "load") == 0) {
        if (argc < 3) {
            printf("Usage: vrom load <path>\n");
            return (uint64_t)-1;
        }
        free(s_pending_vrom_path);
        s_pending_vrom_path = strdup(argv[2]);
        printf("VROM path set: %s\n", s_pending_vrom_path);
        return 0;
    }

    if (strcmp(action, "checksum") == 0 || strcmp(action, "probe") == 0) {
        bool probe = strcmp(action, "probe") == 0;
        if (argc < 3) {
            if (probe) {
                // No path: check if a VROM path is set
                return s_pending_vrom_path ? 0 : 1;
            }
            printf("Usage: vrom checksum <path>\n");
            return (uint64_t)-1;
        }
        // Validate: must be exactly 32 KB
        FILE *f = fopen(argv[2], "rb");
        if (!f) {
            if (!probe)
                printf("0\n");
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fclose(f);
        if (size == 32 * 1024) {
            if (!probe)
                printf("VROM OK (%ld bytes)\n", size);
            return 0;
        }
        if (!probe)
            printf("0\n");
        return 1;
    }

    printf("Usage: vrom load <path> | --checksum <path> | --probe [<path>]\n");
    return (uint64_t)-1;
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
    for (uint32_t p = 0; p < ram_pages && (int)p < g_page_count; p++) {
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

    for (uint32_t p = rom_start_page; p < rom_end_page && (int)p < g_page_count; p++) {
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

// ============================================================================
// Lifecycle: Constructor
// ============================================================================

// Allocate and initialise a memory map for the given address space and RAM/ROM sizes.
// The page table is allocated dynamically based on address_bits.
// Machine-specific memory layout (page table population) is done by the machine's
// memory_layout_init callback, not here.
memory_map_t *memory_map_init(int address_bits, uint32_t ram_size, uint32_t rom_size, checkpoint_t *checkpoint) {
    memory_map_t *mem = (memory_map_t *)malloc(sizeof(memory_map_t));

    memset(mem, 0, sizeof(memory_map_t));

    // Store parameterised sizes for later use by layout, checkpoint, and cmd_rom
    mem->ram_size = ram_size;
    mem->rom_size = rom_size;

    // Allocate the flat RAM+ROM image (ram_size + rom_size bytes)
    size_t image_size = (size_t)ram_size + (size_t)rom_size;
    mem->image = malloc(image_size);
    memset(mem->image, 0, image_size);

    // Allocate page table sized for the given address space
    if (address_bits == 32) {
        g_address_mask = 0xFFFFFFFFUL; // full 32-bit
        g_page_count = 1 << (32 - PAGE_SHIFT); // 1,048,576 pages
    } else {
        // Default to 24-bit (Macintosh Plus)
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
    if (argc < 1)
        return val_err("memory.read_cstring: expected addr");
    bool ok = false;
    uint64_t addr_u = val_as_u64(&argv[0], &ok);
    if (!ok)
        return val_err("memory.read_cstring: addr must be numeric");
    uint32_t addr = (uint32_t)addr_u;
    int max_chars = 96;
    if (argc >= 2) {
        bool ok2 = false;
        int64_t mc = val_as_i64(&argv[1], &ok2);
        if (ok2 && mc > 0 && mc <= 4096)
            max_chars = (int)mc;
    }
    char buf[8192];
    size_t out = 0;
    if (out < sizeof(buf))
        buf[out++] = '"';
    for (int i = 0; i < max_chars && out + 4 < sizeof(buf); i++) {
        uint8_t b = memory_read_uint8(addr + (uint32_t)i);
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
    if (out + 1 < sizeof(buf))
        buf[out++] = '"';
    buf[out] = '\0';
    return val_str(buf);
}

static const arg_decl_t mem_read_cstring_args[] = {
    {.name = "addr",      .kind = V_UINT, .flags = VAL_HEX,          .doc = "guest memory address"          },
    {.name = "max_chars", .kind = V_INT,  .flags = OBJ_ARG_OPTIONAL, .doc = "max chars to read (default 96)"},
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
    if (argc < 1)
        return val_err("memory.peek.b: expected addr");
    bool ok = false;
    uint64_t a = val_as_u64(&argv[0], &ok);
    if (!ok)
        return val_err("memory.peek.b: addr must be numeric");
    value_t v = val_uint(1, memory_read_uint8((uint32_t)a));
    v.flags |= VAL_HEX;
    return v;
}
static value_t method_mem_peek_w(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1)
        return val_err("memory.peek.w: expected addr");
    bool ok = false;
    uint64_t a = val_as_u64(&argv[0], &ok);
    if (!ok)
        return val_err("memory.peek.w: addr must be numeric");
    value_t v = val_uint(2, memory_read_uint16((uint32_t)a));
    v.flags |= VAL_HEX;
    return v;
}
static value_t method_mem_peek_l(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1)
        return val_err("memory.peek.l: expected addr");
    bool ok = false;
    uint64_t a = val_as_u64(&argv[0], &ok);
    if (!ok)
        return val_err("memory.peek.l: addr must be numeric");
    value_t v = val_uint(4, memory_read_uint32((uint32_t)a));
    v.flags |= VAL_HEX;
    return v;
}

static const arg_decl_t mem_peek_args[] = {
    {.name = "addr", .kind = V_UINT, .flags = VAL_HEX, .doc = "guest memory address"},
};

static const member_t mem_peek_members[] = {
    {.kind = M_METHOD,
     .name = "b",
     .doc = "Read 1 byte at addr",
     .method = {.args = mem_peek_args, .nargs = 1, .result = V_UINT, .fn = method_mem_peek_b}},
    {.kind = M_METHOD,
     .name = "w",
     .doc = "Read 2 bytes (big-endian word) at addr",
     .method = {.args = mem_peek_args, .nargs = 1, .result = V_UINT, .fn = method_mem_peek_w}},
    {.kind = M_METHOD,
     .name = "l",
     .doc = "Read 4 bytes (big-endian long) at addr",
     .method = {.args = mem_peek_args, .nargs = 1, .result = V_UINT, .fn = method_mem_peek_l}},
};

const class_desc_t mem_peek_class = {
    .name = "peek",
    .members = mem_peek_members,
    .n_members = sizeof(mem_peek_members) / sizeof(mem_peek_members[0]),
};
