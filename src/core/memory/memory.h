// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// memory.h
// Public interface for memory map management.

#ifndef MEMORY_MAP_H
#define MEMORY_MAP_H

// === Includes ===
#include "common.h"

#include <stdbool.h>
#include <stdint.h>

// === Constants ===
#define TOP_OF_RAM 0x400000
#define TOP_OF_ROM 0x580000

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

extern memory_map_t *memory_map_init(checkpoint_t *checkpoint);

void memory_map_delete(memory_map_t *mem);

void memory_map_checkpoint(memory_map_t *restrict mem, checkpoint_t *checkpoint);

// === Operations ===

extern void memory_map_add(memory_map_t *mem, uint32_t addr, uint32_t size, const char *name, memory_interface_t *iface,
                           void *device);

extern void memory_map_remove(memory_map_t *mem, uint32_t addr, uint32_t size, const char *name,
                              memory_interface_t *iface, void *device);

extern void memory_map_print(memory_map_t *mem);

extern memory_interface_t *memory_map_interface(memory_map_t *restrict mem);

uint8_t *ram_native_pointer(memory_map_t *ram, uint32_t addr);

uint32_t memory_read(unsigned int size, uint32_t addr);

void memory_write(unsigned int size, uint32_t addr, uint32_t value);

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

// Slow-path handlers for cross-page or device I/O accesses
uint16_t memory_read_uint16_slow(uint32_t addr);
uint32_t memory_read_uint32_slow(uint32_t addr);
void memory_write_uint16_slow(uint32_t addr, uint16_t value);
void memory_write_uint32_slow(uint32_t addr, uint32_t value);

// === Inline Accessors (page-table based) ===

// Backward compatibility: g_memory_base is still set for test stubs
extern uint8_t *g_memory_base;

static inline uint8_t memory_read_uint8(uint32_t addr) {
    addr &= g_address_mask;
    page_entry_t *pe = &g_page_table[addr >> PAGE_SHIFT];
    if (__builtin_expect(pe->host_base != NULL, 1))
        return LOAD_BE8(pe->host_base + (addr & PAGE_MASK));
    if (pe->dev)
        return pe->dev->read_uint8(pe->dev_context, addr - pe->base_addr);
    return 0;
}

static inline uint16_t memory_read_uint16(uint32_t addr) {
    addr &= g_address_mask;
    page_entry_t *pe = &g_page_table[addr >> PAGE_SHIFT];
    // Fast path: same page and direct memory
    if (__builtin_expect(pe->host_base != NULL && (addr & PAGE_MASK) <= MEM_PAGE_SIZE - 2, 1))
        return LOAD_BE16(pe->host_base + (addr & PAGE_MASK));
    return memory_read_uint16_slow(addr);
}

static inline uint32_t memory_read_uint32(uint32_t addr) {
    addr &= g_address_mask;
    page_entry_t *pe = &g_page_table[addr >> PAGE_SHIFT];
    // Fast path: same page and direct memory
    if (__builtin_expect(pe->host_base != NULL && (addr & PAGE_MASK) <= MEM_PAGE_SIZE - 4, 1))
        return LOAD_BE32(pe->host_base + (addr & PAGE_MASK));
    return memory_read_uint32_slow(addr);
}

static inline void memory_write_uint8(uint32_t addr, uint8_t value) {
    addr &= g_address_mask;
    page_entry_t *pe = &g_page_table[addr >> PAGE_SHIFT];
    if (__builtin_expect(pe->host_base != NULL && pe->writable, 1)) {
        STORE_BE8(pe->host_base + (addr & PAGE_MASK), value);
        return;
    }
    if (pe->dev)
        pe->dev->write_uint8(pe->dev_context, addr - pe->base_addr, value);
}

static inline void memory_write_uint16(uint32_t addr, uint16_t value) {
    addr &= g_address_mask;
    page_entry_t *pe = &g_page_table[addr >> PAGE_SHIFT];
    // Fast path: same page and writable
    if (__builtin_expect(pe->host_base != NULL && pe->writable && (addr & PAGE_MASK) <= MEM_PAGE_SIZE - 2, 1)) {
        STORE_BE16(pe->host_base + (addr & PAGE_MASK), value);
        return;
    }
    memory_write_uint16_slow(addr, value);
}

static inline void memory_write_uint32(uint32_t addr, uint32_t value) {
    addr &= g_address_mask;
    page_entry_t *pe = &g_page_table[addr >> PAGE_SHIFT];
    // Fast path: same page and writable
    if (__builtin_expect(pe->host_base != NULL && pe->writable && (addr & PAGE_MASK) <= MEM_PAGE_SIZE - 4, 1)) {
        STORE_BE32(pe->host_base + (addr & PAGE_MASK), value);
        return;
    }
    memory_write_uint32_slow(addr, value);
}

#endif // MEMORY_MAP_H
