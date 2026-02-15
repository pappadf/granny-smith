// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// memory.c
// Memory map management and RAM/ROM access for Granny Smith.

// ============================================================================
// Includes
// ============================================================================

#include "memory.h"

#include "common.h"
#include "platform.h"
#include "shell.h"
#include "system.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Constants and Macros
// ============================================================================

#define RAM_SIZE (4 * 1024 * 1024)

uint8_t *g_memory_base = NULL;

// Page table globals (defined here, declared extern in memory.h)
page_entry_t *g_page_table = NULL;
uint32_t g_address_mask = 0x00FFFFFFUL; // 24-bit default

// Number of pages in current page table
static int g_page_count = 0;

#define ROM_SIZE      (128 * 1024) // 128K Mac Plus ROM
#define MAX_ROM_SIZE  (256 * 1024) // Mac Plus could in theory be upgraded to 256K ROM
#define ROM_ADDR_MASK 0x0003FFFFUL // Valid address bits when accessing the ROM

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

    memory_interface_t memory_interface;

    uint8_t *image;

    // Per-instance page table (points to g_page_table when active)
    page_entry_t *page_table;
    int page_count;

    // Path to the ROM file loaded via cmd_load_rom (if any)
    char *rom_filename;

    uint32_t checksum;

    bool checksum_valid;

    int version;

} memory_map_t;

// ============================================================================
// Memory Interface
// ============================================================================

// Read an 8-bit value from memory at the specified address
static uint8_t mem_read_uint8(void *rom, uint32_t addr) {
    addr &= 0x00FFFFFFUL;

    memory_map_t *m = (memory_map_t *)rom;

    if (addr < TOP_OF_ROM)
        return LOAD_BE8(m->image + addr);

    mapping_t *map = m->map;

    for (; map != NULL; map = map->next)
        if (addr >= map->addr && addr < (map->addr + map->size))
            return map->memory_interface.read_uint8(map->device, addr - map->addr);

    return 0;
}

// Read a 16-bit value from memory at the specified address
static uint16_t mem_read_uint16(void *rom, uint32_t addr) {
    addr &= 0x00FFFFFFUL;

    memory_map_t *m = (memory_map_t *)rom;

    if (addr < TOP_OF_ROM)
        return LOAD_BE16(m->image + addr);

    mapping_t *map = m->map;

    for (; map != NULL; map = map->next)
        if (addr >= map->addr && addr < (map->addr + map->size))
            return map->memory_interface.read_uint16(map->device, addr - map->addr);

    return 0;
}

// Read a 32-bit value from memory at the specified address
static uint32_t mem_read_uint32(void *rom, uint32_t addr) {
    addr &= 0x00FFFFFFUL;

    memory_map_t *m = (memory_map_t *)rom;

    if (addr < TOP_OF_ROM)
        return LOAD_BE32(m->image + addr);

    mapping_t *map = m->map;

    for (; map != NULL; map = map->next)
        if (addr >= map->addr && addr < (map->addr + map->size))
            return map->memory_interface.read_uint32(map->device, addr - map->addr);

    return 0;
}

// Write an 8-bit value to memory at the specified address
static void mem_write_uint8(void *rom, uint32_t addr, uint8_t value) {
    addr &= 0x00FFFFFFUL;

    memory_map_t *m = (memory_map_t *)rom;

    if (addr < TOP_OF_RAM) {
        STORE_BE8(m->image + addr, value);
        return;
    }

    mapping_t *map = m->map;

    for (; map != NULL; map = map->next)
        if (addr >= map->addr && addr < (map->addr + map->size)) {
            map->memory_interface.write_uint8(map->device, addr - map->addr, value);
            return;
        }

    assert(0);
}

// Write a 16-bit value to memory at the specified address
static void mem_write_uint16(void *rom, uint32_t addr, uint16_t value) {
    addr &= 0x00FFFFFFUL;

    memory_map_t *m = (memory_map_t *)rom;

    if (addr < TOP_OF_RAM) {
        STORE_BE16(m->image + addr, value);
        return;
    }

    mapping_t *map = m->map;

    for (; map != NULL; map = map->next)
        if (addr >= map->addr && addr < (map->addr + map->size)) {
            map->memory_interface.write_uint16(map->device, addr - map->addr, value);
            return;
        }

    assert(0);
}

// Write a 32-bit value to memory at the specified address
static void mem_write_uint32(void *rom, uint32_t addr, uint32_t value) {
    addr &= 0x00FFFFFFUL;

    memory_map_t *m = (memory_map_t *)rom;

    if (addr < TOP_OF_RAM) {
        STORE_BE32(m->image + addr, value);
        return;
    }

    mapping_t *map = m->map;

    for (; map != NULL; map = map->next)
        if (addr >= map->addr && addr < (map->addr + map->size)) {
            map->memory_interface.write_uint32(map->device, addr - map->addr, value);
            return;
        }

    GS_ASSERT(0);
}

// Read memory at the given address with specified size (1, 2, or 4 bytes)
uint32_t memory_read(unsigned int size, uint32_t addr) {
    switch (size) {
    case 1:
        return mem_read_uint8(system_memory(), addr);
    case 2:
        return mem_read_uint16(system_memory(), addr);
    case 4:
        return mem_read_uint32(system_memory(), addr);
    default:
        assert(0);
        return 0;
    }
}

// Write memory at the given address with specified size (1, 2, or 4 bytes)
void memory_write(unsigned int size, uint32_t addr, uint32_t value) {
    switch (size) {
    case 1:
        mem_write_uint8(system_memory(), addr, (uint8_t)value);
        break;
    case 2:
        mem_write_uint16(system_memory(), addr, (uint16_t)value);
        break;
    case 4:
        mem_write_uint32(system_memory(), addr, (uint32_t)value);
        break;
    default:
        assert(0);
    }
}

// ============================================================================
// Page Table Slow Paths
// ============================================================================

// Slow path for 16-bit reads: cross-page or device I/O
uint16_t memory_read_uint16_slow(uint32_t addr) {
    page_entry_t *pe = &g_page_table[addr >> PAGE_SHIFT];

    // Device I/O (single page, not crossing boundary)
    if (pe->dev && (addr & PAGE_MASK) <= MEM_PAGE_SIZE - 2)
        return pe->dev->read_uint16(pe->dev_context, addr - pe->base_addr);

    // Cross-page or host memory at page boundary: split into two byte reads
    uint16_t hi = memory_read_uint8(addr);
    uint16_t lo = memory_read_uint8(addr + 1);
    return (hi << 8) | lo;
}

// Slow path for 32-bit reads: cross-page or device I/O
uint32_t memory_read_uint32_slow(uint32_t addr) {
    page_entry_t *pe = &g_page_table[addr >> PAGE_SHIFT];

    // Device I/O (single page, not crossing boundary)
    if (pe->dev && (addr & PAGE_MASK) <= MEM_PAGE_SIZE - 4)
        return pe->dev->read_uint32(pe->dev_context, addr - pe->base_addr);

    // Cross-page: split into two 16-bit reads
    uint32_t hi = memory_read_uint16(addr);
    uint32_t lo = memory_read_uint16(addr + 2);
    return (hi << 16) | lo;
}

// Slow path for 16-bit writes: cross-page or device I/O
void memory_write_uint16_slow(uint32_t addr, uint16_t value) {
    page_entry_t *pe = &g_page_table[addr >> PAGE_SHIFT];

    // Device I/O (single page, not crossing boundary)
    if (pe->dev && (addr & PAGE_MASK) <= MEM_PAGE_SIZE - 2) {
        pe->dev->write_uint16(pe->dev_context, addr - pe->base_addr, value);
        return;
    }

    // Cross-page: split into two byte writes
    memory_write_uint8(addr, (uint8_t)(value >> 8));
    memory_write_uint8(addr + 1, (uint8_t)(value & 0xFF));
}

// Slow path for 32-bit writes: cross-page or device I/O
void memory_write_uint32_slow(uint32_t addr, uint32_t value) {
    page_entry_t *pe = &g_page_table[addr >> PAGE_SHIFT];

    // Device I/O (single page, not crossing boundary)
    if (pe->dev && (addr & PAGE_MASK) <= MEM_PAGE_SIZE - 4) {
        pe->dev->write_uint32(pe->dev_context, addr - pe->base_addr, value);
        return;
    }

    // Cross-page: split into two 16-bit writes
    memory_write_uint16(addr, (uint16_t)(value >> 16));
    memory_write_uint16(addr + 2, (uint16_t)(value & 0xFFFF));
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
        for (uint32_t p = start_page; p <= end_page && (int)p < g_page_count; p++) {
            g_page_table[p].host_base = NULL;
            // Point to the copied interface in the linked list node (stable pointer)
            g_page_table[p].dev = &map->memory_interface;
            g_page_table[p].dev_context = device;
            g_page_table[p].base_addr = addr;
            g_page_table[p].writable = false;
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

// Calculate and validate ROM checksum and identify ROM version
static void calculate_checksum(memory_map_t *rom) {
    int i;

    uint16_t *image16 = (uint16_t *)(rom->image + TOP_OF_RAM);
    uint32_t *image32 = (uint32_t *)(rom->image + TOP_OF_RAM);

    rom->checksum = 0;

    for (i = 2; i < ROM_SIZE / 2; i++)
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

// Determine ROM version from raw ROM data by checking the checksum
uint32_t rom_version(const uint8_t *data) {
    uint32_t checksum = 0;

    uint16_t *image16 = (uint16_t *)(data);
    uint32_t *image32 = (uint32_t *)(data);

    for (int i = 2; i < ROM_SIZE / 2; i++)
        checksum += BE16(image16[i]);

    if (checksum != BE32(image32[0]))
        return 0;

    switch (BE32(image32[0])) {
    case 0x4D1EEEE1: // version 1 (Lonely Hearts)
        return 1;
    case 0x4D1EEAE1: // version 2 (Lonely Heifers)
        return 2;
    case 0x4D1F8172: // version 3 (Loud Harmonicas)
        return 3;
    default:
        return 0;
    }
}

// Determine ROM version from a file path
int rom_file_version(const char *path) {
    uint8_t data[ROM_SIZE];

    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    size_t n = fread(data, 1, ROM_SIZE, f);

    fclose(f);

    if (n != ROM_SIZE)
        return false;

    return rom_version(data);
}

// ============================================================================
// Shell Commands
// ============================================================================

// Shell command to load a ROM file into memory
uint64_t cmd_load_rom(int argc, char *argv[]) {
    // Check for --probe option
    bool probe_mode = false;
    int filename_arg = 1;

    if (argc >= 2 && strcmp(argv[1], "--probe") == 0) {
        probe_mode = true;
        filename_arg = 2;
    }

    if (argc < filename_arg + 1) {
        if (probe_mode) {
            // No filename given with --probe: check if a ROM is currently loaded
            memory_map_t *mem = system_memory();
            return (mem && mem->rom_filename) ? 0 : 1;
        }
        printf("Usage: load-rom [--probe] <filename>\n");
        return 0;
    }

    const char *filename = argv[filename_arg];

    // In probe mode, just check if file is a valid ROM
    if (probe_mode) {
        int version = rom_file_version(filename);
        if (version > 0) {
            return 0; // Valid ROM
        } else {
            return 1; // Not a valid ROM
        }
    }

    // Normal load mode
    FILE *f = fopen(filename, "rb");
    if (!f) {
        printf("Failed to open ROM file: %s\n", filename);
        return -1;
    }

    size_t n = fread(system_memory()->image + TOP_OF_RAM, 1, ROM_SIZE, f);
    fclose(f);

    if (n != ROM_SIZE) {
        printf("Failed to read ROM file: %s\n", filename);
        return -1;
    }

    calculate_checksum(system_memory());

    for (uint32_t addr = TOP_OF_RAM + 2 * ROM_SIZE; addr < TOP_OF_ROM; addr += 2 * ROM_SIZE) {
        printf("copying ROM from %08x to %08x\n", TOP_OF_RAM, addr);
        memcpy(system_memory()->image + addr, system_memory()->image + TOP_OF_RAM, ROM_SIZE);
    }

    printf("ROM loaded successfully from %s\n", filename);

    // Remember ROM filename for checkpointing
    memory_map_t *mem = system_memory();
    if (mem->rom_filename) {
        free(mem->rom_filename);
        mem->rom_filename = NULL;
    }
    mem->rom_filename = strdup(filename);

    return 0;
}

// ============================================================================
// Lifecycle: Constructor
// ============================================================================

// Populate page table entries for RAM and ROM from the flat image buffer
static void populate_ram_rom_pages(uint8_t *image) {
    if (!g_page_table || !image)
        return;

    // RAM pages: 0x000000 – TOP_OF_RAM (writable, direct access)
    uint32_t ram_pages = TOP_OF_RAM >> PAGE_SHIFT;
    for (uint32_t p = 0; p < ram_pages && (int)p < g_page_count; p++) {
        g_page_table[p].host_base = image + (p << PAGE_SHIFT);
        g_page_table[p].dev = NULL;
        g_page_table[p].dev_context = NULL;
        g_page_table[p].writable = true;
    }

    // ROM pages: TOP_OF_RAM – TOP_OF_ROM (read-only, direct access)
    uint32_t rom_start_page = TOP_OF_RAM >> PAGE_SHIFT;
    uint32_t rom_end_page = TOP_OF_ROM >> PAGE_SHIFT;
    for (uint32_t p = rom_start_page; p < rom_end_page && (int)p < g_page_count; p++) {
        g_page_table[p].host_base = image + (p << PAGE_SHIFT);
        g_page_table[p].dev = NULL;
        g_page_table[p].dev_context = NULL;
        g_page_table[p].writable = false;
    }
}

memory_map_t *memory_map_init(checkpoint_t *checkpoint) {
    memory_map_t *mem = (memory_map_t *)malloc(sizeof(memory_map_t));

    memset(mem, 0, sizeof(memory_map_t));

    mem->memory_interface.read_uint8 = &mem_read_uint8;
    mem->memory_interface.read_uint16 = &mem_read_uint16;
    mem->memory_interface.read_uint32 = &mem_read_uint32;

    mem->memory_interface.write_uint8 = &mem_write_uint8;
    mem->memory_interface.write_uint16 = &mem_write_uint16;
    mem->memory_interface.write_uint32 = &mem_write_uint32;

    // Allocate the flat RAM+ROM image
    g_memory_base = mem->image = malloc(TOP_OF_ROM);
    memset(mem->image, 0, TOP_OF_ROM);

    // Allocate page table for 24-bit address space
    g_address_mask = 0x00FFFFFFUL;
    g_page_count = 1 << (24 - PAGE_SHIFT); // 4096 pages
    g_page_table = (page_entry_t *)calloc(g_page_count, sizeof(page_entry_t));
    mem->page_table = g_page_table;
    mem->page_count = g_page_count;

    // Populate RAM and ROM pages
    populate_ram_rom_pages(mem->image);

    // Guide to the Macintosh Family Hardware, 2nd edition, page 122:
    // At system startup, the operating system reads an address in the range $F0 0000 through $F7F FFF
    // labeled "Phase Read" to determine whether the computer's high-frequency timing signals are correctly in phase

    memory_interface_t phase_read;
    memset(&phase_read, 0, sizeof(phase_read));

    phase_read.read_uint8 = &phase_read_uint8;
    phase_read.read_uint16 = &phase_read_uint16;
    phase_read.read_uint32 = &phase_read_uint32;

    memory_map_add(mem, 0x00F00000, 0x00080000, "Phase Read", &phase_read, NULL);

    register_cmd("load-rom", "ROM", "load-rom [--probe [filename]] – load or probe ROM", (void *)&cmd_load_rom);

    // Load from checkpoint if provided
    if (checkpoint) {
        // Restore RAM
        system_read_checkpoint_data(checkpoint, mem->image, RAM_SIZE);
        // Restore ROM (content or reference)
        char *restored_path = NULL;
        size_t got = checkpoint_read_file(checkpoint, mem->image + TOP_OF_RAM, ROM_SIZE, &restored_path);
        if (restored_path) {
            if (mem->rom_filename)
                free(mem->rom_filename);
            mem->rom_filename = restored_path;
        }
        if (got > 0) {
            // Mirror ROM across the ROM region like cmd_load_rom
            calculate_checksum(mem);
            for (uint32_t addr = TOP_OF_RAM + 2 * ROM_SIZE; addr < TOP_OF_ROM; addr += 2 * ROM_SIZE) {
                memcpy(mem->image + addr, mem->image + TOP_OF_RAM, ROM_SIZE);
            }
        }
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
        }
        free(mem->page_table);
        mem->page_table = NULL;
    }
    // Free RAM/ROM image buffer
    if (mem->image) {
        free(mem->image);
        mem->image = NULL;
        g_memory_base = NULL;
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
    system_write_checkpoint_data(checkpoint, mem->image, RAM_SIZE);

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

memory_interface_t *memory_map_interface(memory_map_t *restrict mem) {
    return &mem->memory_interface;
}
