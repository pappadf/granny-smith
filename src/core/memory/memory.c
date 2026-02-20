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

// Plus ROM size used only for identification functions (rom_version / rom_file_version).
// These functions are Plus-specific and will be replaced in M6 by the ROM identification module.
#define PLUS_ROM_SIZE (128 * 1024)

// Page table globals (defined here, declared extern in memory.h)
page_entry_t *g_page_table = NULL;
uint32_t g_address_mask = 0x00FFFFFFUL; // 24-bit default
int g_page_count = 0; // number of pages in current page table

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

    // Path to the ROM file loaded via cmd_load_rom (if any)
    char *rom_filename;

    uint32_t checksum;

    bool checksum_valid;

    int version;

} memory_map_t;

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

// Determine ROM version from raw ROM data by checking the checksum (Plus-specific)
uint32_t rom_version(const uint8_t *data) {
    uint32_t checksum = 0;

    uint16_t *image16 = (uint16_t *)(data);
    uint32_t *image32 = (uint32_t *)(data);

    for (int i = 2; i < PLUS_ROM_SIZE / 2; i++)
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

// Determine ROM version from a file path (Plus-specific)
int rom_file_version(const char *path) {
    uint8_t data[PLUS_ROM_SIZE];

    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    size_t n = fread(data, 1, PLUS_ROM_SIZE, f);

    fclose(f);

    if (n != PLUS_ROM_SIZE)
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
    memory_map_t *mem = system_memory();
    if (!mem) {
        printf("Failed to load ROM: memory not initialized\n");
        return -1;
    }

    FILE *f = fopen(filename, "rb");
    if (!f) {
        printf("Failed to open ROM file: %s\n", filename);
        return -1;
    }

    // ROM content goes at the ram_size offset in the flat buffer
    size_t n = fread(mem->image + mem->ram_size, 1, mem->rom_size, f);
    fclose(f);

    if (n != mem->rom_size) {
        printf("Failed to read ROM file: %s\n", filename);
        return -1;
    }

    calculate_checksum(mem);

    printf("ROM loaded successfully from %s\n", filename);

    // Remember ROM filename for checkpointing
    if (mem->rom_filename) {
        free(mem->rom_filename);
        mem->rom_filename = NULL;
    }
    mem->rom_filename = strdup(filename);

    return 0;
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
        g_page_table[p].host_base = mem->image + (p << PAGE_SHIFT);
        g_page_table[p].dev = NULL;
        g_page_table[p].dev_context = NULL;
        g_page_table[p].writable = true;
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
        g_page_table[p].host_base = mem->image + ram_size + (offset_in_cycle << PAGE_SHIFT);
        g_page_table[p].dev = NULL;
        g_page_table[p].dev_context = NULL;
        g_page_table[p].writable = false;
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

    // Store parameterised sizes for later use by layout, checkpoint, and cmd_load_rom
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
    g_page_table = (page_entry_t *)calloc(g_page_count, sizeof(page_entry_t));
    assert(g_page_table != NULL && "failed to allocate page table");
    mem->page_table = g_page_table;
    mem->page_count = g_page_count;

    register_cmd("load-rom", "ROM", "load-rom [--probe [filename]] – load or probe ROM", (void *)&cmd_load_rom);

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
