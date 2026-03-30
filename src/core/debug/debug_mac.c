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
#include "system.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
uint64_t cmd_key(int argc, char *argv[]);
uint64_t cmd_post_event(int argc, char *argv[]);

void debug_mac_init(void) {
    register_cmd("pi", "Debugger", "Print process information", &cmd_process_info);

    // Mouse automation commands for E2E testing
    register_cmd("set-mouse", "Testing",
                 "set-mouse [--global|--hw] x y  – set/move mouse position\n"
                 "  (default): absolute coords, best method per platform\n"
                 "  --global:  absolute coords via low-memory globals\n"
                 "  --hw:      relative deltas via hardware (ADB/quadrature)",
                 cmd_set_mouse);
    register_cmd("trace-mouse", "Testing", "trace-mouse start|stop  – log mouse position once per second",
                 cmd_trace_mouse);
    register_cmd("mouse-button", "Testing",
                 "mouse-button [--global|--hw] up|down  – inject mouse button event\n"
                 "  --global: writes MBState directly (no event posting)\n"
                 "  --hw (default): routes through hardware (ADB/VIA)",
                 cmd_mouse_button);
    register_cmd("key", "Testing",
                 "key <name|0xNN>  – inject a key press+release via ADB/keyboard\n"
                 "  Named keys: return, space, escape, tab, delete, up, down, left, right\n"
                 "  Or hex ADB keycode: key 0x24",
                 cmd_key);
    register_cmd("post-event", "Testing",
                 "post-event <what> <message>  – post a Mac OS event\n"
                 "  what:    event code (1=mouseDown, 7=diskEvt, etc.)\n"
                 "  message: event message (for diskEvt: drive number 1 or 2)",
                 cmd_post_event);
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

// Scans argv for an optional --global or --hw flag at any position.
// Removes the flag from argv (shifts subsequent args down) and adjusts argc.
// Sets *mode to 'g' for --global, 'h' for --hw, 'd' for default (no flag).
static void parse_mode_flag(int *argc, char *argv[], char *mode) {
    *mode = 'd'; // default
    for (int i = 1; i < *argc; i++) {
        if (strcmp(argv[i], "--global") == 0) {
            *mode = 'g';
        } else if (strcmp(argv[i], "--hw") == 0) {
            *mode = 'h';
        } else {
            continue;
        }
        // shift remaining args down to remove the flag
        for (int j = i; j < *argc - 1; j++)
            argv[j] = argv[j + 1];
        (*argc)--;
        return;
    }
}

// ---- MTemp guard ----
// The SE/30 ROM's ADB mouse handler reads deltas from a shared buffer at
// ADBBase+$164/$165.  Stale data from keyboard auto-poll can contaminate this
// buffer, causing the handler to apply phantom deltas to MTemp even when no
// real mouse movement occurred.  This corrupts the position set by --global
// mode and causes clicks to miss their target (TrackControl reads the drifted
// Mouse position during the button-hold tracking loop).
//
// The guard is a periodic scheduler event that restores MTemp (and RawMouse,
// Mouse) to the target position whenever drift is detected.  It runs at ~1 kHz
// (every 1 ms of emulated time), which is fast enough to correct MTemp before
// the next VBL (~16 ms) copies it to Mouse.  Activated by set-mouse --global,
// deactivated by the next set-mouse call or mouse-button up.

#define MOUSE_GUARD_INTERVAL_NS (1 * 1000 * 1000) // 1 ms

static bool mouse_guard_active = false;
static int16_t mouse_guard_h = 0; // target horizontal (x)
static int16_t mouse_guard_v = 0; // target vertical (y)
static bool mouse_guard_registered = false;

static void mouse_guard_tick(void *source, uint64_t data) {
    (void)source;
    (void)data;
    if (!mouse_guard_active)
        return;

    // Check if MTemp has drifted from the target position
    int16_t cur_v = (int16_t)memory_read_uint16(0x0828);
    int16_t cur_h = (int16_t)memory_read_uint16(0x082A);

    if (cur_v != mouse_guard_v || cur_h != mouse_guard_h) {
        // Restore all position globals to the target
        memory_write_uint16(0x0828, (uint16_t)mouse_guard_v); // MTemp.v
        memory_write_uint16(0x082A, (uint16_t)mouse_guard_h); // MTemp.h
        memory_write_uint16(0x082C, (uint16_t)mouse_guard_v); // RawMouse.v
        memory_write_uint16(0x082E, (uint16_t)mouse_guard_h); // RawMouse.h
        memory_write_uint16(0x0830, (uint16_t)mouse_guard_v); // Mouse.v
        memory_write_uint16(0x0832, (uint16_t)mouse_guard_h); // Mouse.h
    }

    // Reschedule
    scheduler_t *sched = system_scheduler();
    if (sched)
        scheduler_new_cpu_event(sched, &mouse_guard_tick, NULL, 0, 0, MOUSE_GUARD_INTERVAL_NS);
}

static void mouse_guard_start(int16_t h, int16_t v) {
    scheduler_t *sched = system_scheduler();
    if (!sched)
        return;

    if (!mouse_guard_registered) {
        scheduler_new_event_type(sched, "test", NULL, "mouse_guard", &mouse_guard_tick);
        mouse_guard_registered = true;
    }

    mouse_guard_h = h;
    mouse_guard_v = v;
    mouse_guard_active = true;

    // Remove any existing guard event and schedule a fresh one
    remove_event(sched, &mouse_guard_tick, NULL);
    scheduler_new_cpu_event(sched, &mouse_guard_tick, NULL, 0, 0, MOUSE_GUARD_INTERVAL_NS);
}

static void mouse_guard_stop(void) {
    mouse_guard_active = false;
    scheduler_t *sched = system_scheduler();
    if (sched)
        remove_event(sched, &mouse_guard_tick, NULL);
}

// Writes absolute cursor position to Mac low-memory globals (MTemp, RawMouse, Mouse, CrsrNew).
// This is the classic technique used by ChromiVNC/MiniVNC and Basilisk II.
// Note: on SE/30 (and other NuBus-capable Macs), this updates the position globals
// but may not redraw the cursor image on screen until the next slot VBL fires.
static void set_mouse_global(long x, long y) {
    uint32_t addr_MTemp = debug_mac_lookup_global_address("MTemp");
    uint32_t addr_RawMouse = debug_mac_lookup_global_address("RawMouse");
    uint32_t addr_Mouse = debug_mac_lookup_global_address("Mouse");
    uint32_t addr_CrsrNew = debug_mac_lookup_global_address("CrsrNew");
    uint32_t addr_CrsrCouple = debug_mac_lookup_global_address("CrsrCouple");

    if (!addr_MTemp || !addr_RawMouse || !addr_CrsrNew) {
        printf("Error: could not resolve mouse-related globals.\n");
        return;
    }

    uint16_t v = (uint16_t)(y & 0xFFFF); // vertical in high word
    uint16_t h = (uint16_t)(x & 0xFFFF); // horizontal in low word

    // Write new position to MTemp and RawMouse (the interrupt-level inputs)
    memory_write_uint16(addr_MTemp, v);
    memory_write_uint16(addr_MTemp + 2, h);
    memory_write_uint16(addr_RawMouse, v);
    memory_write_uint16(addr_RawMouse + 2, h);

    // Also write Mouse directly so GetMouse returns the correct value immediately
    if (addr_Mouse) {
        memory_write_uint16(addr_Mouse, v);
        memory_write_uint16(addr_Mouse + 2, h);
    }

    // Signal the cursor VBL task: copy CrsrCouple → CrsrNew (standard technique)
    if (addr_CrsrCouple) {
        uint8_t couple = memory_read_uint8(addr_CrsrCouple);
        memory_write_uint8(addr_CrsrNew, couple);
    } else {
        memory_write_uint8(addr_CrsrNew, 0xFF);
    }
}

// Injects relative mouse movement through the hardware path (ADB or quadrature).
// Preserves the current button state on both ADB and non-ADB machines.
static void set_mouse_hw(long dx, long dy) {
    bool injected = system_mouse_move((int)dx, (int)dy);
    if (!injected)
        printf("Error: no mouse device available for hardware injection.\n");
}

// Default set-mouse: absolute coordinates, platform-dependent strategy.
// ADB (SE/30): computes deltas from current position and injects via ADB hardware.
// Non-ADB (Plus): writes globals directly.
static void set_mouse_default(long x, long y) {
    uint32_t addr_MTemp = debug_mac_lookup_global_address("MTemp");
    if (!addr_MTemp) {
        printf("Error: could not resolve MTemp.\n");
        return;
    }

    // Read current cursor position from MTemp
    int16_t cur_v = (int16_t)memory_read_uint16(addr_MTemp);
    int16_t cur_h = (int16_t)memory_read_uint16(addr_MTemp + 2);
    int dx = (int)x - (int)cur_h;
    int dy = (int)y - (int)cur_v;

    // On ADB machines, inject deltas through ADB so the ROM ISR naturally
    // updates the cursor position (including the screen cursor image).
    // Non-ADB machines (Plus) fall through to global writes.
    bool injected = system_mouse_move_adb(dx, dy);
    if (!injected) {
        set_mouse_global(x, y);
    }
}

uint64_t cmd_set_mouse(int argc, char *argv[]) {
    char mode;
    parse_mode_flag(&argc, argv, &mode);

    if (argc < 3) {
        printf("Usage: set-mouse [--global|--hw] x y\n"
               "  (default)  absolute coords, best method per platform\n"
               "  --global   absolute coords via low-memory globals\n"
               "  --hw       relative deltas via hardware (ADB/quadrature)\n");
        return 0;
    }

    // Parse first coordinate
    char *endp = NULL;
    long a = strtol(argv[1], &endp, 0);
    if (*endp != '\0') {
        printf("Invalid coordinate: %s\n", argv[1]);
        return 0;
    }

    // Parse second coordinate
    endp = NULL;
    long b = strtol(argv[2], &endp, 0);
    if (*endp != '\0') {
        printf("Invalid coordinate: %s\n", argv[2]);
        return 0;
    }

    if (!system_memory()) {
        printf("Error: memory system not initialized.\n");
        return 0;
    }

    // Clamp to 16-bit signed range (Mac Point) for absolute modes
    if (mode != 'h') {
        if (a < -32768)
            a = -32768;
        else if (a > 32767)
            a = 32767;
        if (b < -32768)
            b = -32768;
        else if (b > 32767)
            b = 32767;
    }

    switch (mode) {
    case 'g':
        set_mouse_global(a, b);
        // Activate the MTemp guard to protect against phantom ADB deltas.
        // The guard keeps MTemp pinned to (a, b) until the next set-mouse
        // or mouse-button up, preventing stale ADB buffer data from
        // shifting the cursor during button-hold tracking loops.
        mouse_guard_start((int16_t)a, (int16_t)b);
        break;
    case 'h':
        mouse_guard_stop();
        set_mouse_hw(a, b);
        break;
    default:
        mouse_guard_stop();
        set_mouse_default(a, b);
        break;
    }

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
// Injects a mouse button state change.
// --hw (default): routes through hardware emulation (ADB or VIA PB3), which causes
//   the ROM's device handler to write MBState and post mouseDown/mouseUp events.
// --global: writes MBState directly.  On Mac Plus, sets MBTicks to a future value
//   to prevent the VIA interrupt from overwriting MBState (the debounce hack).
//   No event is posted, so event-driven code won't see the click — use --hw for that.

// Writes button state directly to MBState, with MBTicks hack for Mac Plus safety.
static void mouse_button_global(bool button_down) {
    uint32_t addr_MBState = debug_mac_lookup_global_address("MBState");
    uint32_t addr_MBTicks = debug_mac_lookup_global_address("MBTicks");
    uint32_t addr_Ticks = debug_mac_lookup_global_address("Ticks");

    if (!addr_MBState) {
        printf("Error: could not resolve MBState.\n");
        return;
    }

    // MBState bit 7: 0 = button down, 0x80 = button up
    memory_write_uint8(addr_MBState, button_down ? 0x00 : 0x80);

    // MBTicks hack: set MBTicks to a far-future value to prevent the VIA
    // interrupt from overwriting MBState.  Required on Mac Plus where the
    // VIA ISR continuously polls the physical button.  Safe on ADB machines
    // too (the field is unused there).
    if (addr_MBTicks && addr_Ticks) {
        uint32_t ticks = memory_read_uint32(addr_Ticks);
        memory_write_uint32(addr_MBTicks, ticks + 100);
    }
}

uint64_t cmd_mouse_button(int argc, char *argv[]) {
    char mode;
    parse_mode_flag(&argc, argv, &mode);

    if (argc < 2) {
        printf("Usage: mouse-button [--global|--hw] up|down\n");
        return 0;
    }

    bool button_down;
    if (strcmp(argv[1], "down") == 0) {
        button_down = true;
    } else if (strcmp(argv[1], "up") == 0) {
        button_down = false;
    } else {
        printf("Usage: mouse-button [--global|--hw] up|down\n");
        return 0;
    }

    if (mode == 'g') {
        // --global: write MBState directly
        mouse_button_global(button_down);
    } else {
        // --hw or default: route through hardware emulation (ADB or VIA PB3)
        system_mouse_update(button_down, 0, 0);
    }

    printf("mouse-button: %s (%s)\n", button_down ? "down" : "up", mode == 'g' ? "global" : "hw");
    return 0;
}

// Resolves a key name to an ADB virtual keycode, or -1 if unknown
static int resolve_key_name(const char *name) {
    // Named keys (case-insensitive comparison via manual lowering)
    if (!strcasecmp(name, "return") || !strcasecmp(name, "enter"))
        return 0x24;
    if (!strcasecmp(name, "space"))
        return 0x31;
    if (!strcasecmp(name, "escape") || !strcasecmp(name, "esc"))
        return 0x35;
    if (!strcasecmp(name, "tab"))
        return 0x30;
    if (!strcasecmp(name, "delete") || !strcasecmp(name, "backspace"))
        return 0x33;
    if (!strcasecmp(name, "up"))
        return 0x7E;
    if (!strcasecmp(name, "down"))
        return 0x7D;
    if (!strcasecmp(name, "left"))
        return 0x7B;
    if (!strcasecmp(name, "right"))
        return 0x7C;
    if (!strcasecmp(name, "command") || !strcasecmp(name, "cmd"))
        return 0x37;
    if (!strcasecmp(name, "shift"))
        return 0x38;
    if (!strcasecmp(name, "option") || !strcasecmp(name, "alt"))
        return 0x3A;
    if (!strcasecmp(name, "control") || !strcasecmp(name, "ctrl"))
        return 0x36;

    // Hex keycode (e.g., 0x24)
    if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        char *endp = NULL;
        long val = strtol(name, &endp, 16);
        if (*endp == '\0' && val >= 0 && val <= 0x7F)
            return (int)val;
    }

    return -1;
}

// Injects a key press followed by a key release via the keyboard subsystem
uint64_t cmd_key(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: key <name|0xNN>\n"
               "  Named: return, space, escape, tab, delete, up, down, left, right\n"
               "  Hex:   key 0x24  (ADB virtual keycode)\n");
        return 0;
    }

    int keycode = resolve_key_name(argv[1]);
    if (keycode < 0) {
        printf("Unknown key: %s\n", argv[1]);
        return 0;
    }

    // Inject key-down then key-up through the keyboard subsystem
    system_keyboard_update(key_down, keycode);
    system_keyboard_update(key_up, keycode);
    printf("key: 0x%02X (%s)\n", keycode, argv[1]);
    return 0;
}

// ---- post-event implementation ----
// Posts a Mac OS event by writing directly into the Event Manager queue.
//
// The Event Manager queue (EvQHdr at $014A) is a linked list of EvQEl elements.
// Each EvQEl is 22 bytes:
//   +0: qLink       (long)  — next element pointer (or 0 for tail)
//   +4: qType       (word)  — element type (5 = OS event)
//   +6: evtQWhat    (word)  — event code
//   +8: evtQMessage (long)  — event message
//   +12: evtQWhen   (long)  — Ticks at event time
//   +16: evtQWhere  (long)  — Point (v<<16 | h) from Mouse
//   +20: evtQModifiers (word) — modifier key state
//
// The event buffer is a pre-allocated pool starting after the system globals.
// We scan the pool for a free element (qType == 0) and link it at the tail.

uint64_t cmd_post_event(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: post-event <what> <message>\n"
               "  Common event codes: 1=mouseDown, 2=mouseUp, 7=diskEvt\n"
               "  For diskEvt, message = drive number (1=internal, 2=external)\n");
        return 0;
    }

    if (!system_memory()) {
        printf("Error: memory system not initialized.\n");
        return 0;
    }

    char *endp = NULL;
    long what = strtol(argv[1], &endp, 0);
    if (*endp != '\0') {
        printf("Invalid event code: %s\n", argv[1]);
        return 0;
    }

    endp = NULL;
    long message = strtol(argv[2], &endp, 0);
    if (*endp != '\0') {
        printf("Invalid message: %s\n", argv[2]);
        return 0;
    }

    // Read current queue state
    // EvQHdr at $014A: QFlags(2) + QHead(4) + QTail(4)
    uint32_t q_head = memory_read_uint32(0x014C);
    uint32_t q_tail = memory_read_uint32(0x0150);
    uint32_t ticks = memory_read_uint32(0x016A);
    uint16_t mouse_v = memory_read_uint16(0x0830);
    uint16_t mouse_h = memory_read_uint16(0x0832);

    // Find a free EvQEl by scanning from the start of the event buffer.
    // The event buffer typically starts right after the system globals.
    // We scan a reasonable range for a free element (qType == 0).
    // Each EvQEl is 22 bytes.  The pool is typically 20 elements.
    uint32_t pool_start = q_head ? q_head : q_tail;
    if (!pool_start) {
        // Empty queue — try a common event buffer base
        pool_start = 0x0160;
    }

    // Walk back from the first known element to find the pool start.
    // Actually, we can just scan a range near the known elements.
    // The event pool is usually in the range $0160-$0300.
    uint32_t free_el = 0;
    for (uint32_t addr = 0x015C; addr < 0x0400; addr += 22) {
        uint16_t qtype = memory_read_uint16(addr + 4);
        if (qtype == 0) {
            free_el = addr;
            break;
        }
    }

    if (free_el == 0) {
        printf("post-event: no free event buffer element found.\n");
        return 0;
    }

    // Fill in the event record
    memory_write_uint32(free_el + 0, 0); // qLink = nil
    memory_write_uint16(free_el + 4, 5); // qType = 5 (OS event)
    memory_write_uint16(free_el + 6, (uint16_t)(what & 0xFFFF)); // evtQWhat
    memory_write_uint32(free_el + 8, (uint32_t)message); // evtQMessage
    memory_write_uint32(free_el + 12, ticks); // evtQWhen
    memory_write_uint32(free_el + 16, ((uint32_t)mouse_v << 16) | mouse_h); // evtQWhere
    memory_write_uint16(free_el + 20, 0); // evtQModifiers

    // Link into the queue at the tail
    if (q_tail != 0) {
        memory_write_uint32(q_tail, free_el); // old tail's qLink → new element
    } else {
        memory_write_uint32(0x014C, free_el); // QHead = new element (was empty)
    }
    memory_write_uint32(0x0150, free_el); // QTail = new element

    printf("post-event: what=%ld message=$%lX at el=$%08X\n", what, message, free_el);
    return 0;
}
