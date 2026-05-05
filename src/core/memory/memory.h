// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// memory.h
// Public interface for memory map management.

#ifndef MEMORY_MAP_H
#define MEMORY_MAP_H

// === Includes ===
#include "common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// === Macros ===
#define LOAD_BE8(p)  (*(const uint8_t *)(p))
#define LOAD_BE16(p) (__builtin_bswap16(*(const uint16_t *)(p)))
#define LOAD_BE32(p) (__builtin_bswap32(*(const uint32_t *)(p)))

#define STORE_BE8(p, v)  (*(uint8_t *)(p) = (uint8_t)(v))
#define STORE_BE16(p, v) (*(uint16_t *)(p) = __builtin_bswap16((uint16_t)(v)))
#define STORE_BE32(p, v) (*(uint32_t *)(p) = __builtin_bswap32((uint32_t)(v)))

// === Type Definitions ===
typedef struct memory_interface {
    uint8_t (*read_uint8)(void *device, uint32_t addr);
    uint16_t (*read_uint16)(void *device, uint32_t addr);
    uint32_t (*read_uint32)(void *device, uint32_t addr);
    void (*write_uint8)(void *device, uint32_t addr, uint8_t data);
    void (*write_uint16)(void *device, uint32_t addr, uint16_t data);
    void (*write_uint32)(void *device, uint32_t addr, uint32_t data);
} memory_interface_t;

struct memory;
typedef struct memory memory_map_t;

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

// Initialise a memory map with parameterised address space and RAM/ROM sizes.
// address_bits: 24 for Plus (16 MB), 32 for SE/30 (4 GB)
// ram_size: RAM size in bytes (e.g. 0x400000 for Plus)
// rom_size: ROM size in bytes (e.g. 0x020000 for Plus)
// checkpoint: if non-NULL, restore RAM and ROM from checkpoint
extern memory_map_t *memory_map_init(int address_bits, uint32_t ram_size, uint32_t rom_size, checkpoint_t *checkpoint);

void memory_map_delete(memory_map_t *mem);

void memory_map_checkpoint(memory_map_t *restrict mem, checkpoint_t *checkpoint);

// === Operations ===

extern void memory_map_add(memory_map_t *mem, uint32_t addr, uint32_t size, const char *name, memory_interface_t *iface,
                           void *device);

extern void memory_map_remove(memory_map_t *mem, uint32_t addr, uint32_t size, const char *name,
                              memory_interface_t *iface, void *device);

extern void memory_map_print(memory_map_t *mem);

uint8_t *ram_native_pointer(memory_map_t *ram, uint32_t addr);

// Return the filename of the currently loaded ROM, or NULL if none.
const char *memory_rom_filename(memory_map_t *mem);

// Direct accessors for the loaded ROM region. Returned pointer is owned by
// the memory map and remains valid until the next memory_install_rom() call.
const uint8_t *memory_rom_bytes(memory_map_t *mem);
uint32_t memory_rom_size(memory_map_t *mem);
uint32_t memory_rom_checksum(memory_map_t *mem);

// Copy ROM bytes into the ROM region, refresh internal checksum, and store
// the filename for checkpointing. Truncates if size > rom_size. Returns the
// number of bytes actually written.
size_t memory_install_rom(memory_map_t *mem, const uint8_t *data, size_t size, const char *filename);

uint32_t memory_read(unsigned int size, uint32_t addr);

void memory_write(unsigned int size, uint32_t addr, uint32_t value);

// Populate page table entries for RAM (writable) and ROM (read-only) regions.
// ROM pages in [rom_start_addr, rom_region_end) are populated with page-table
// mirroring: guest addresses wrap at rom_size so the ROM content repeats.
// Called from machine-specific layout callbacks (e.g. plus_memory_layout_init).
extern void memory_populate_pages(memory_map_t *mem, uint32_t rom_start_addr, uint32_t rom_region_end);

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
extern int g_page_count; // total number of pages in the current page table

// SoA (Struct-of-Arrays) fast-path page tables.
// Each entry stores an adjusted host address: (uintptr_t)host_base - page_guest_base
// so that (uint8_t *)(entry + masked_addr) yields the correct host pointer directly.
// Zero entry = slow path (device I/O, unmapped, or MMU miss).
extern uintptr_t *g_supervisor_read; // supervisor-mode read mapping
extern uintptr_t *g_supervisor_write; // supervisor-mode write mapping (RAM only)
extern uintptr_t *g_user_read; // user-mode read mapping
extern uintptr_t *g_user_write; // user-mode write mapping

// Active pointers switched per sprint based on SR.S bit
extern uintptr_t *g_active_read;
extern uintptr_t *g_active_write;

// Deferred bus error: slow paths signal unmapped MMU accesses by setting
// the pending flag AND zeroing *g_bus_error_instr_ptr, which forces the
// CPU decoder loop to exit after the current instruction completes.
// The bus error exception is then processed outside the hot loop.
extern uint32_t g_bus_error_pending; // 0=none, 1=pending
extern uint32_t g_bus_error_address; // faulting logical address
extern uint32_t g_bus_error_rw; // 1=read, 0=write
extern uint32_t g_bus_error_fc; // FC of the faulting access (1=user-data, 5=super-data)
extern uint32_t g_bus_error_is_pmmu; // 1=PMMU descriptor fault (retry), 0=bus timeout (skip)
extern uint32_t *g_bus_error_instr_ptr; // points to decoder's instruction counter

// I/O cycle penalty: tracks extra bus wait-state cycles for I/O accesses.
// Penalty cycles are converted to phantom instructions that burn sprint burndown,
// causing I/O-heavy sprints to end sooner and keeping event timing accurate.
// g_io_cpi == 0 disables the mechanism entirely (e.g. max_speed mode).
extern uint32_t g_io_penalty_remainder; // sub-CPI fraction carried across sprints
extern uint32_t g_io_phantom_instructions; // phantom instructions consumed this sprint
extern uint32_t g_io_cpi; // current CPI for conversion (0 = disabled)
extern uint32_t *g_sprint_burndown_ptr; // points to sprint_burndown during sprint

// Apply an I/O bus cycle penalty (extra_cycles beyond the CPI baseline).
// Called from machine I/O dispatchers (e.g. SE/30) in the slow path only.
static inline void memory_io_penalty(uint32_t extra_cycles) {
    if (__builtin_expect(g_io_cpi == 0, 0))
        return; // penalties disabled
    g_io_penalty_remainder += extra_cycles;
    uint32_t burn = g_io_penalty_remainder / g_io_cpi;
    if (__builtin_expect(burn > 0, 1)) {
        g_io_penalty_remainder -= burn * g_io_cpi;
        g_io_phantom_instructions += burn;
        uint32_t *bp = g_sprint_burndown_ptr;
        if (bp)
            *bp = (*bp > burn) ? (*bp - burn) : 0;
    }
}

// Slow-path handlers for device I/O, unmapped, or MMU TLB miss accesses
uint8_t memory_read_uint8_slow(uint32_t addr);
uint16_t memory_read_uint16_slow(uint32_t addr);
uint32_t memory_read_uint32_slow(uint32_t addr);
void memory_write_uint8_slow(uint32_t addr, uint8_t value);
void memory_write_uint16_slow(uint32_t addr, uint16_t value);
void memory_write_uint32_slow(uint32_t addr, uint32_t value);

// === Memory Logpoint Support ===
// When a memory logpoint covers a page, its SoA entries are forced to zero so
// every access routes through the slow path.  The slow path then consults the
// logpoint hook (installed by debug.c) and emits a log line.  The fast path
// is unchanged — no comparisons or branches added — so this feature has zero
// cost when no memory logpoints are set.

// Per-page memory-logpoint reference count.  Non-zero entries indicate pages
// whose SoA fast-path must stay at 0 (force slow path).  Allocated with the
// page tables in memory_map_init.
extern uint8_t *g_mem_logpoint_page_count;

// Per-physical-page memory-logpoint reference count.  Non-zero entries mean
// "any logical alias mapping to this physical page must stay on the slow
// path so the logpoint fires regardless of which alias the CPU uses."
// Indexed by physical page number.  mmu_fill_soa_entry consults both arrays.
extern uint8_t *g_mem_logpoint_phys_page_count;

// Hook invoked by the slow path on logpoint pages.  is_write=true on writes.
// Installed by debug.c.  NULL means no hook (skip check).
typedef void (*memory_logpoint_hook_t)(uint32_t addr, unsigned size, uint32_t value, bool is_write);
extern memory_logpoint_hook_t g_mem_logpoint_hook;

// === Value Trap (fast-path needle search) ===
// Catches writes of a specific (PA, size, value) combination without forcing
// the page to slow path.  Controlled by `value-trap` shell command.  When
// disabled (value_trap_active=0), the inline check is one cmovne and one
// branch-not-taken — sub-1% overhead on the fast path.  When fired, the hook
// is called with the access details; the hook can then disarm itself or stop.
// Multiple writes across an instruction may all match; the hook is called for
// each.  Use to find rare needles in haystacks (e.g. "the write that placed
// these 4 specific bytes at this physical address").
extern uint32_t g_value_trap_active; // 0 = disabled, nonzero = enabled
extern uint32_t g_value_trap_pa; // physical address to match
extern uint32_t g_value_trap_value; // value to match
extern uint32_t g_value_trap_size; // 1, 2, or 4 (byte/word/long)
typedef void (*value_trap_hook_t)(uint32_t logical_addr, uint32_t phys_addr, uint32_t value, unsigned size);
extern value_trap_hook_t g_value_trap_hook;
// Called by the fast path when the value matches; resolves PA and invokes hook.
void value_trap_check(uint32_t logical_addr, uint32_t value, unsigned size);

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
    if (__builtin_expect(base != 0 && (masked & PAGE_MASK) <= MEM_PAGE_SIZE - 4, 1))
        return LOAD_BE32((uint8_t *)(base + masked));
    return memory_read_uint32_slow(masked);
}

static inline void memory_write_uint8(uint32_t addr, uint8_t value) {
    uint32_t masked = addr & g_address_mask;
    uintptr_t base = g_active_write[masked >> PAGE_SHIFT];
    if (__builtin_expect(g_value_trap_active && g_value_trap_size == 1 && (uint8_t)value == (uint8_t)g_value_trap_value,
                         0))
        value_trap_check(masked, value, 1);
    if (__builtin_expect(base != 0, 1)) {
        STORE_BE8((uint8_t *)(base + masked), value);
        return;
    }
    memory_write_uint8_slow(masked, value);
}

static inline void memory_write_uint16(uint32_t addr, uint16_t value) {
    uint32_t masked = addr & g_address_mask;
    uintptr_t base = g_active_write[masked >> PAGE_SHIFT];
    if (__builtin_expect(
            g_value_trap_active && g_value_trap_size == 2 && (uint16_t)value == (uint16_t)g_value_trap_value, 0))
        value_trap_check(masked, value, 2);
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
    if (__builtin_expect(g_value_trap_active && g_value_trap_size == 4 && value == g_value_trap_value, 0))
        value_trap_check(masked, value, 4);
    // Fast path: non-zero entry and access doesn't cross page boundary
    if (__builtin_expect(base != 0 && (masked & PAGE_MASK) <= MEM_PAGE_SIZE - 4, 1)) {
        STORE_BE32((uint8_t *)(base + masked), value);
        return;
    }
    memory_write_uint32_slow(masked, value);
}

#endif // MEMORY_MAP_H
