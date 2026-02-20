// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// debug_mac.c
// Mac-specific debugging utilities: trap names, global variable lookup, and process inspection.

#include "debug_mac.h"

#include "cpu.h"
#include "memory.h"
#include "mouse.h"
#include "scheduler.h"
#include "shell.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Global variable info (defined in mac_globals_data.c)
extern struct {
    const char *name;
    uint32_t address;
    int size;
    const char *description;
} mac_global_vars[];
extern const size_t mac_global_vars_count;

// A-trap info (defined in mac_traps_data.c)
extern struct {
    const char *name;
    uint32_t trap;
} macos_atraps[];
extern const size_t macos_atraps_count;

uint32_t debug_mac_lookup_global_address(const char *name) {
    if (!name)
        return 0;
    for (size_t i = 0; i < mac_global_vars_count; i++) {
        if (strcmp(mac_global_vars[i].name, name) == 0) {
            return mac_global_vars[i].address;
        }
    }
    return 0; // Not found
}

static const char *lookup_atrap(uint16_t trap) {
    for (int i = 0; i < macos_atraps_count; i++) {
        if (macos_atraps[i].trap == trap) {
            return macos_atraps[i].name;
        }
    }
    return NULL;
}

const char *macos_atrap_name(uint16_t trap) {

    static char buffer[32];

    const char *name = lookup_atrap(trap);

    if (trap & 0x0800) { // Toolbox trap
        if ((name = lookup_atrap(trap & 0xFBFF)))
            return name;
    } else { // OS trap

        if ((name = lookup_atrap(trap)))
            return name;
    }

    snprintf(buffer, sizeof(buffer), "_%04X", trap);
    return buffer;
}

uint8_t read_8bit_be(uint32_t addr) {
    if (!system_memory())
        return 0;
    return memory_read_uint8(addr);
}

uint16_t read_16bit_be(uint32_t addr) {
    if (!system_memory())
        return 0;
    return memory_read_uint16(addr);
}

uint32_t read_32bit_be(uint32_t addr) {
    if (!system_memory())
        return 0;
    return memory_read_uint32(addr);
}

void read_bytes(uint32_t addr, uint8_t *buffer, size_t size) {
    if (!system_memory())
        return;
    for (size_t i = 0; i < size; ++i) {
        buffer[i] = memory_read_uint8(addr + i);
    }
}

size_t read_pstring(uint32_t addr, char *buffer, size_t max_length) {
    size_t length = 0;
    while (length < max_length - 1) {
        uint8_t byte = read_8bit_be(addr + length);
        if (byte == 0) {
            break; // Null terminator found
        }
        buffer[length] = byte;
        length++;
    }
    buffer[length] = '\0'; // Null-terminate the string
    return length;
}

/**
 * @file resource_map.h
 * @brief C data structures for representing a classic Mac OS resource map.
 *
 * This file defines the C structures that correspond to the in-memory format
 * of a resource map. A resource map is the directory for all resources
 * (like CODE, MENU, ICON, etc.) within an application's resource fork.
 *
 * The structure is hierarchical:
 * 1.  A main `resource_map` contains a header and a list of resource types.
 * 2.  Each `resource_type_info` entry describes a type (e.g., 'CODE') and
 * points to a list of all resources of that type.
 * 3.  Each `resource_reference` contains the ID, name, attributes, and a
 * handle to the actual resource data for a specific resource.
 */

#pragma pack(push, 1) // Ensure structures are packed without padding

/**
 * @struct resource_reference
 * @brief Describes a single resource instance (e.g., CODE resource #1).
 *
 * This corresponds to an entry in the reference list within the resource map.
 */
typedef struct {
    int16_t resource_id; // The ID number of this resource.
    int16_t name_offset; // Offset from the beginning of the resource name list to this resource's name. -1 if no name.
    uint8_t attributes; // Resource attributes (e.g., purgeable, locked).
    uint8_t data_handle_high; // High byte of the handle to the resource data.
    uint16_t data_handle_low; // Low two bytes of the handle. (A handle is conceptually 24 bits on 68k).
    uint32_t reserved; // Reserved for future use.
} resource_reference;

/**
 * @struct resource_type_info
 * @brief Describes a single type of resource (e.g., 'CODE').
 *
 * This corresponds to an entry in the type list within the resource map.
 */
typedef struct {
    uint32_t type_tag; // The four-character code for the type (e.g., 'CODE', 'MENU').
    uint16_t num_resources; // Number of resources of this type minus one.
    uint16_t ref_list_offset; // Offset from the beginning of the type list to this type's reference list.
    resource_reference *reference_list; // A dynamically allocated array to hold the parsed references.
} resource_type_info;

/**
 * @struct resource_map_header
 * @brief The header at the very beginning of the resource map data.
 *
 * It contains offsets to the other key parts of the map.
 */
typedef struct {
    uint8_t reserved_header_copy[16]; // A copy of the resource fork header.
    uint32_t next_map_handle; // Handle to the next resource map in the chain (for searching multiple files).
    uint16_t file_ref_num; // The file reference number for this resource file.
    uint16_t attributes; // Attributes of the resource file itself (e.g., read-only).
    uint16_t type_list_offset; // Offset from the beginning of the header to the type list.
    uint16_t name_list_offset; // Offset from the beginning of the header to the resource name list.
} resource_map_header;

/**
 * @struct resource_map
 * @brief The top-level structure to hold the entire parsed resource map.
 */
typedef struct {
    resource_map_header header; // The parsed header of the map.
    uint16_t num_types; // Number of resource types minus one.
    resource_type_info *type_list; // A dynamically allocated array to hold the parsed types.
    char *name_list_data; // A buffer holding all resource names.
} resource_map;

#pragma pack(pop) // Restore default packing

/**
 * @brief Frees the memory allocated for a resource_map structure.
 *
 * @param map A pointer to the resource_map structure to be freed.
 */
void free_resource_map(resource_map *map) {
    if (!map) {
        return;
    }

    if (map->type_list) {
        for (int i = 0; i < map->num_types; ++i) {
            if (map->type_list[i].reference_list) {
                free(map->type_list[i].reference_list);
            }
        }
        free(map->type_list);
    }

    free(map);
}

/**
 * @brief Reads the current application's resource map from memory.
 *
 * This function locates the resource map using the TopMapHndl global,
 * allocates memory for a local copy, and parses it into the C structures
 * defined in resource_map.h.
 *
 * @return A pointer to a newly allocated resource_map structure, or NULL on failure.
 * The caller is responsible for freeing this structure using free_resource_map().
 */
resource_map *read_resource_map(void) {
    // 1. Find the resource map handle from low-memory globals
    uint32_t map_handle = read_32bit_be(0x0A50);
    if (map_handle == 0) {
        // No resource map found
        return NULL;
    }

    // 2. Dereference the handle to get the map's address in the heap
    // A handle points to a master pointer, which points to the data.
    uint32_t master_ptr_addr = map_handle;
    uint32_t map_base_addr = read_32bit_be(master_ptr_addr);
    if (map_base_addr == 0) {
        return NULL;
    }

    // 3. Allocate the top-level structure for our parsed map
    resource_map *parsed_map = (resource_map *)malloc(sizeof(resource_map));
    if (!parsed_map) {
        return NULL;
    }
    memset(parsed_map, 0, sizeof(resource_map));

    // 4. Read the resource map header
    read_bytes(map_base_addr, (uint8_t *)&parsed_map->header, sizeof(resource_map_header));

    // The number of types immediately follows the header in the map data
    parsed_map->num_types = read_16bit_be(map_base_addr + parsed_map->header.type_list_offset - 2) + 1;

    // 5. Parse the Type List
    uint32_t type_list_addr = map_base_addr + parsed_map->header.type_list_offset;
    parsed_map->type_list = (resource_type_info *)malloc(sizeof(resource_type_info) * parsed_map->num_types);
    if (!parsed_map->type_list) {
        free_resource_map(parsed_map);
        return NULL;
    }

    for (int i = 0; i < parsed_map->num_types; ++i) {
        uint32_t current_type_addr = type_list_addr + (i * 8); // Each type entry is 8 bytes
        resource_type_info *current_type_info = &parsed_map->type_list[i];

        current_type_info->type_tag = read_32bit_be(current_type_addr);
        current_type_info->num_resources = read_16bit_be(current_type_addr + 4) + 1;
        current_type_info->ref_list_offset = read_16bit_be(current_type_addr + 6);

        // 6. Parse the Reference List for this type
        uint32_t ref_list_addr =
            map_base_addr + parsed_map->header.type_list_offset + current_type_info->ref_list_offset;
        current_type_info->reference_list =
            (resource_reference *)malloc(sizeof(resource_reference) * current_type_info->num_resources);
        if (!current_type_info->reference_list) {
            free_resource_map(parsed_map);
            return NULL;
        }
        read_bytes(ref_list_addr, (uint8_t *)current_type_info->reference_list,
                   sizeof(resource_reference) * current_type_info->num_resources);
    }

    // 7. Store a pointer to the name list data (optional, could be parsed further)
    // For simplicity, we just point to the raw data block.
    // A full implementation would need to know the total size of the name list.
    parsed_map->name_list_data = (char *)(map_base_addr + parsed_map->header.name_list_offset);

    return parsed_map;
}

/**
 * @brief Prints information about the currently running application.
 *
 * This function reads various low-memory globals to display the application's
 * name, memory layout (heap and stack), and other relevant details.
 */
uint64_t cmd_process_info(int argc, char *argv[]) {
    printf("--- Current Application Info ---\n");

    // 1. Get the Application Name
    char app_name[64];
    read_pstring(0x0910, app_name, sizeof(app_name));
    printf("Name: %s\n", app_name);

    // 2. Get the Application's Memory Map
    printf("\n--- Memory Map ---\n");

    uint32_t heap_start_ptr = read_32bit_be(0x02AA);
    uint32_t heap_limit_ptr = read_32bit_be(0x0130);
    uint32_t a5_world_ptr = read_32bit_be(0x0904);

    uint32_t partition_size_bytes = heap_limit_ptr - heap_start_ptr;

    printf("Application Partition Start: %#010x\n", heap_start_ptr);
    printf("Application Partition Limit: %#010x\n", heap_limit_ptr);
    printf("  Heap Start:                %#010x\n", heap_start_ptr);
    printf("  Stack Base (Top of A5):    %#010x (grows downwards)\n", a5_world_ptr);
    printf("Total Partition Size:        %u KB\n", partition_size_bytes / 1024);

    return 0;
}

// Forward declarations for mouse automation commands
uint64_t cmd_set_mouse(int argc, char *argv[]);
uint64_t cmd_trace_mouse(int argc, char *argv[]);
uint64_t cmd_mouse_button(int argc, char *argv[]);

void debug_mac_init(void) {
    register_cmd("pi", "Debugger", "Print process information", &cmd_process_info);

    // Mouse automation commands for E2E testing
    register_cmd("set-mouse", "Testing", "set-mouse x y  – set mouse position (h=x, v=y)", cmd_set_mouse);
    register_cmd("trace-mouse", "Testing", "trace-mouse start|stop  – log mouse position once per second",
                 cmd_trace_mouse);
    register_cmd("mouse-button", "Testing", "mouse-button [up|down]  – inject mouse button up/down event",
                 cmd_mouse_button);
}

// Helper to print process info programmatically (used by assertion handler)
void debug_mac_print_process_info(void) {
    (void)cmd_process_info(0, NULL);
}

// ────────────────────────────────────────────────────────────────────────────
// Target (68K) backtrace and diagnostic functions
// ────────────────────────────────────────────────────────────────────────────

// Memory read helpers
static uint8_t read8(uint32_t addr) {
    if (!system_memory())
        return 0;
    return memory_read_uint8(addr);
}
static uint16_t read16(uint32_t addr) {
    if (!system_memory())
        return 0;
    return memory_read_uint16(addr);
}
static uint32_t read32(uint32_t addr) {
    if (!system_memory())
        return 0;
    return memory_read_uint32(addr);
}

// Print target 68K backtrace by walking stack frames
void debug_mac_print_target_backtrace(void) {
    printf("\n=== Target 68K backtrace ===\n");
    cpu_t *cpu = system_cpu();
    if (!cpu) {
        printf("(CPU not initialized)\n");
        return;
    }

    // Frame-walk using A6 as frame pointer, printing return addresses
    uint32_t pc = cpu_get_pc(cpu);
    char linebuf[128];
    debugger_disasm(linebuf, pc);
    printf("#0  %s\n", linebuf);

    uint32_t a6 = cpu_get_an(cpu, 6);
    for (int depth = 1; depth <= 16; depth++) {
        if (a6 == 0)
            break; // end of chain
        // Standard 68K frame: [0]: previous A6, [4]: return address
        uint32_t prev_a6 = 0, ret = 0;
        // Guard against invalid memory reads (use address mask to bound the check)
        if (a6 < 0x100 || a6 > g_address_mask)
            break;
        prev_a6 = read32(a6 + 0);
        ret = read32(a6 + 4);
        if (ret == 0 || ret > g_address_mask)
            break;
        debugger_disasm(linebuf, ret);
        printf("#%d  %s\n", depth, linebuf);
        if (prev_a6 == a6)
            break; // prevent loops
        a6 = prev_a6;
    }
}

// Print current Mac application process info (wrapper for diagnostic output)
void debug_mac_print_process_info_header(void) {
    printf("\n=== Current Mac application ===\n");
    debug_mac_print_process_info();
}

// ────────────────────────────────────────────────────────────────────────────
// Mouse automation commands for E2E testing
// (Moved from test.c - these interact with Mac OS low-memory globals)
// ────────────────────────────────────────────────────────────────────────────

// Command implementation placed after init to keep file order simple
uint64_t cmd_set_mouse(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: set-mouse x y\n");
        return 0;
    }

    // Parse x
    char *endp = NULL;
    long x = strtol(argv[1], &endp, 0);
    if (*endp != '\0') {
        printf("Invalid x coordinate: %s\n", argv[1]);
        return 0;
    }

    // Parse y
    endp = NULL;
    long y = strtol(argv[2], &endp, 0);
    if (*endp != '\0') {
        printf("Invalid y coordinate: %s\n", argv[2]);
        return 0;
    }

    // Clamp to 16-bit signed range like classic Mac Point (short)
    if (x < -32768)
        x = -32768;
    else if (x > 32767)
        x = 32767;

    if (y < -32768)
        y = -32768;
    else if (y > 32767)
        y = 32767;

    if (!system_memory()) {
        printf("Error: memory system not initialized.\n");
        return 0;
    }

    // Low memory globals (Points are (v, h) = (y, x), big-endian shorts)
    uint32_t addr_MTemp = debug_mac_lookup_global_address("MTemp");
    uint32_t addr_RawMouse = debug_mac_lookup_global_address("RawMouse");
    uint32_t addr_CrsrNew = debug_mac_lookup_global_address("CrsrNew");

    // Basic sanity check in case lookup failed (optional, but nice to have)
    if (!addr_MTemp || !addr_RawMouse || !addr_CrsrNew) {
        printf("Error: could not resolve one or more mouse-related globals.\n");
        return 0;
    }

    uint16_t v = (uint16_t)(y & 0xFFFF);
    uint16_t h = (uint16_t)(x & 0xFFFF);

    // Write new absolute position to MTemp (latest mouse value)
    memory_write_uint16(addr_MTemp, v); // v
    memory_write_uint16(addr_MTemp + 2, h); // h

    // And to RawMouse (raw/unaccelerated mouse)
    memory_write_uint16(addr_RawMouse, v); // v
    memory_write_uint16(addr_RawMouse + 2, h); // h

    // Signal to the ROM / cursor code that the cursor has moved.
    // Many examples treat this as a 16-bit "-1" (0xFFFF).
    memory_write_uint16(addr_CrsrNew, 0xFFFF);

    return 0;
}

// ---- trace-mouse implementation ----
// Schedules an event every second that reads the classic Mac low-memory MTemp (Point {v,h})
// and prints it. Uses the scheduler's ns-based API for 1 Hz cadence.
static int trace_mouse_active = 0;
static int trace_mouse_have_last = 0; // whether we have a previous sample
static int16_t trace_mouse_last_h = 0;
static int16_t trace_mouse_last_v = 0;
static void trace_mouse_tick(void *source, uint64_t data) {
    (void)data;
    if (!trace_mouse_active)
        return; // Do not reschedule if stopped during callback

    scheduler_t *sched = system_scheduler();
    if (!system_memory() || !sched)
        return;
    uint32_t addr_Mouse = debug_mac_lookup_global_address("Mouse");
    uint16_t v_be = memory_read_uint16(addr_Mouse);
    uint16_t h_be = memory_read_uint16(addr_Mouse + 2);
    int16_t v = (int16_t)v_be;
    int16_t h = (int16_t)h_be;
    if (!trace_mouse_have_last || h != trace_mouse_last_h || v != trace_mouse_last_v) {
        printf("[trace-mouse] h=%d v=%d\n", h, v);
        trace_mouse_last_h = h;
        trace_mouse_last_v = v;
        trace_mouse_have_last = 1;
    }

    // Reschedule next tick in 1 second
    scheduler_new_cpu_event(sched, &trace_mouse_tick, NULL, 0, 0, 1000000000ULL);
}

uint64_t cmd_trace_mouse(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: trace-mouse start|stop\n");
        return 0;
    }

    scheduler_t *sched = system_scheduler();
    if (!sched) {
        printf("Error: scheduler not initialized.\n");
        return 0;
    }

    // Register event type for checkpointing
    static bool registered = false;
    if (!registered) {
        scheduler_new_event_type(sched, "test", NULL, "trace_mouse", &trace_mouse_tick);
        registered = true;
    }

    if (strcmp(argv[1], "start") == 0) {
        if (!trace_mouse_active) {
            trace_mouse_active = 1;
            trace_mouse_have_last = 0; // force first sample to print
            // Remove any stray events just in case
            remove_event(sched, &trace_mouse_tick, NULL);
            scheduler_new_cpu_event(sched, &trace_mouse_tick, NULL, 0, 0, 1000000000ULL);
            printf("trace-mouse: started\n");
        } else {
            printf("trace-mouse: already running\n");
        }
    } else if (strcmp(argv[1], "stop") == 0) {
        if (trace_mouse_active) {
            trace_mouse_active = 0;
            remove_event(sched, &trace_mouse_tick, NULL);
            printf("trace-mouse: stopped\n");
        } else {
            printf("trace-mouse: not running\n");
        }
    } else {
        printf("Usage: trace-mouse start|stop\n");
    }
    return 0;
}

// ---- mouse-button implementation ----
// Usage: mouse-button [up|down]
// Injects a mouse button state change into the emulated hardware by calling system_mouse_update
// with zero movement. The VIA PB3 line is active-low (0 = pressed) inside the mouse implementation.
uint64_t cmd_mouse_button(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: mouse-button [up|down]\n");
        return 0;
    }

    bool button_down;
    if (strcmp(argv[1], "down") == 0) {
        button_down = true;
    } else if (strcmp(argv[1], "up") == 0) {
        button_down = false;
    } else {
        printf("Usage: mouse-button [up|down]\n");
        return 0;
    }

    // Apply the button change with no movement deltas
    system_mouse_update(button_down, 0, 0);
    printf("mouse-button: %s\n", button_down ? "down" : "up");
    return 0;
}
