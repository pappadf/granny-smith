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

// Shell command handler for load-rom (registered by setup_init, used before machine exists)
uint64_t cmd_load_rom(int argc, char *argv[]);

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

// Slow-path handlers for device I/O, unmapped, or MMU TLB miss accesses
uint8_t memory_read_uint8_slow(uint32_t addr);
uint16_t memory_read_uint16_slow(uint32_t addr);
uint32_t memory_read_uint32_slow(uint32_t addr);
void memory_write_uint8_slow(uint32_t addr, uint8_t value);
void memory_write_uint16_slow(uint32_t addr, uint16_t value);
void memory_write_uint32_slow(uint32_t addr, uint32_t value);

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

#endif // MEMORY_MAP_H
