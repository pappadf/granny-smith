// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// debug_mac.c
// Mac-specific debugging utilities: trap names, global variable lookup, and process inspection.

#include "debug_mac.h"

#include "cmd_types.h"
#include "cpu.h"
#include "memory.h"
#include "mmu.h"
#include "mouse.h"
#include "rtc.h"
#include "scheduler.h"
#include "shell.h"
#include "system.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

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

void debug_mac_init(void) {
    // Phase 5c — legacy `pi` / `set-mouse` / `mouse-button` / `key` /
    // `post-event` / `mouse` / `set-time` shell command registrations
    // retired. The typed object-model surface (mouse.move, mouse.click,
    // keyboard.press) replaces them; cmd_* bodies are kept where the
    // typed wrappers still call them.
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
    char linebuf[160];
    debugger_disasm(linebuf, sizeof(linebuf), pc);
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
        debugger_disasm(linebuf, sizeof(linebuf), ret);
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

// Scans argv for an optional --global / --hw / --aux flag at any position.
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
// When true, the guard tick only re-pins MTemp when the CPU is in user
// mode at the moment of the tick.  Used by --aux under A/UX, where
// VA $0828 in supervisor mode points at A/UX kernel data and re-pinning
// in supervisor would corrupt the kernel.
static bool mouse_guard_user_only = false;

static void mouse_guard_tick(void *source, uint64_t data) {
    (void)source;
    (void)data;
    if (!mouse_guard_active)
        return;

    // Under --aux, skip the tick when the CPU is in supervisor mode —
    // VA $0828 maps to kernel data there, and re-pinning would corrupt
    // it.  We detect mode via the active SoA: if g_active_write equals
    // g_supervisor_write, the CPU was in supervisor mode at the moment
    // the scheduler fired this event.
    if (mouse_guard_user_only && g_active_write == g_supervisor_write) {
        scheduler_t *sched = system_scheduler();
        if (sched)
            scheduler_new_cpu_event(sched, &mouse_guard_tick, NULL, 0, 0, MOUSE_GUARD_INTERVAL_NS);
        return;
    }

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

static void mouse_guard_start(int16_t h, int16_t v, bool user_only) {
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
    mouse_guard_user_only = user_only;

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

// Translate `va` against the cached MAE user CRP and write `value` to the
// resolved physical address as a 16-bit big-endian word, bypassing the
// SoA fast path entirely.  Returns true on success; false if the CRP
// snapshot is empty or the page isn't mapped in MAE's address space.
static bool aux_write_uint16(uint32_t va, uint16_t value) {
    uint32_t pa = 0;
    if (!mmu_translate_with_crp(g_mmu, va, g_last_user_crp, &pa))
        return false;
    return mmu_write_physical_uint16(g_mmu, pa, value);
}

// Same as aux_write_uint16 but for a single byte (used for CrsrNew).
static bool aux_write_uint8(uint32_t va, uint8_t value) {
    uint32_t pa = 0;
    if (!mmu_translate_with_crp(g_mmu, va, g_last_user_crp, &pa))
        return false;
    return mmu_write_physical_uint8(g_mmu, pa, value);
}

// Same as aux_write_uint16 but for an 8-bit read (used to sample CrsrCouple).
// Returns false (and leaves *out untouched) if the page isn't mapped.
static bool aux_read_uint8(uint32_t va, uint8_t *out) {
    uint32_t pa = 0;
    if (!mmu_translate_with_crp(g_mmu, va, g_last_user_crp, &pa))
        return false;
    *out = mmu_read_physical_uint8(g_mmu, pa);
    return true;
}

// Set mouse position under A/UX 3.0 Mac OS compatibility (MAE).
//
// A/UX runs Mac OS apps under the Macintosh Application Environment, a
// per-process user-mode Toolbox emulator.  Each MAE process sees the
// standard Toolbox globals (MTemp $0828, RawMouse $082C, Mouse $0830,
// CrsrNew $08CE, CrsrCouple $08CF) in its own user virtual address
// space.  At the same VAs in supervisor mode A/UX has unrelated kernel
// data — so any write that rides the active SoA is correct only if the
// CPU happens to be in user mode at the instant of the write.
//
// `--global` ignores that distinction: it writes via the active SoA and
// then installs a 1ms guard tick that re-writes the same VAs forever.
// Under A/UX the guard fires while supervisor is active and corrupts the
// kernel's $0828 region.  Forbidden under A/UX.
//
// `--aux` translates each VA against the *cached MAE CRP*
// (`g_last_user_crp`, snapshotted by cpu_internal.h on every
// supervisor→user transition) and writes directly to the resolved
// physical address via `mmu_write_physical_uint16`.  Three consequences:
//
//   1. The write reaches MAE's MTemp regardless of whether the CPU is
//      currently in supervisor or user mode — the translation uses MAE's
//      page tables, not the active CPU mode.
//   2. The kernel's $0828 region is never touched; A/UX kernel data is
//      safe.
//   3. No 1ms guard tick is installed.  The Toolbox globals are written
//      exactly once per `set-mouse --aux` call, so there is no recurring
//      race against MAE's own cursor updates.
//
// If no user-mode entry has been observed yet (`g_last_user_crp == 0`),
// or the snapshot CRP doesn't map a page for one of the target VAs, the
// write is reported as failed and silently skipped — better than landing
// on the wrong page.
static void set_mouse_aux(long x, long y) {
    uint32_t addr_MTemp = debug_mac_lookup_global_address("MTemp");
    uint32_t addr_RawMouse = debug_mac_lookup_global_address("RawMouse");
    uint32_t addr_Mouse = debug_mac_lookup_global_address("Mouse");
    uint32_t addr_CrsrNew = debug_mac_lookup_global_address("CrsrNew");
    uint32_t addr_CrsrCouple = debug_mac_lookup_global_address("CrsrCouple");

    if (!addr_MTemp || !addr_RawMouse || !addr_CrsrNew) {
        printf("Error: could not resolve mouse-related globals.\n");
        return;
    }
    if (!g_mmu || !g_mmu->enabled) {
        printf("set-mouse --aux: MMU not enabled; falling back to active-SoA write.\n");
        set_mouse_global(x, y);
        return;
    }
    if (g_last_user_crp == 0) {
        printf("set-mouse --aux: no user-mode CRP observed yet; run the guest into user mode first.\n");
        return;
    }

    uint16_t v = (uint16_t)(y & 0xFFFF); // vertical word
    uint16_t h = (uint16_t)(x & 0xFFFF); // horizontal word

    // Write Toolbox position globals into MAE's address space.  Each
    // write is independent so a partial mapping reports cleanly.
    int ok = 0;
    int total = 0;
    total++;
    if (aux_write_uint16(addr_MTemp, v))
        ok++;
    total++;
    if (aux_write_uint16(addr_MTemp + 2, h))
        ok++;
    total++;
    if (aux_write_uint16(addr_RawMouse, v))
        ok++;
    total++;
    if (aux_write_uint16(addr_RawMouse + 2, h))
        ok++;
    if (addr_Mouse) {
        total++;
        if (aux_write_uint16(addr_Mouse, v))
            ok++;
        total++;
        if (aux_write_uint16(addr_Mouse + 2, h))
            ok++;
    }

    // Signal MAE's cursor VBL task: copy CrsrCouple → CrsrNew (standard
    // technique used by --global too).
    uint8_t couple = 0xFF;
    if (addr_CrsrCouple)
        (void)aux_read_uint8(addr_CrsrCouple, &couple);
    total++;
    if (aux_write_uint8(addr_CrsrNew, couple))
        ok++;

    printf("set-mouse --aux: wrote MTemp/RawMouse/Mouse = (h=%d, v=%d) via MAE CRP $%08X (%d/%d writes ok)\n", (int)x,
           (int)y, (uint32_t)(g_last_user_crp & 0xFFFFFFFF), ok, total);
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

// Set the mouse cursor position via the requested routing mode.
//   'g' = global (Mac OS Toolbox MTemp + MTemp guard)
//   'h' = hardware (raw quadrature / ADB delta)
//   'a' = aux (A/UX MAE physical-page write)
//   else = default (per-platform best route)
// Returns 0 on success, non-zero if memory is unavailable. Coordinates are
// clamped to int16 for absolute modes ('g'/'a'/default) since the Mac OS
// Point type is 16-bit signed; 'h' passes deltas through unchanged.
int debug_mac_set_mouse_mode(long x, long y, char mode) {
    if (!system_memory())
        return -1;
    if (mode != 'h') {
        if (x < -32768)
            x = -32768;
        else if (x > 32767)
            x = 32767;
        if (y < -32768)
            y = -32768;
        else if (y > 32767)
            y = 32767;
    }
    switch (mode) {
    case 'g':
        set_mouse_global(x, y);
        // Activate the MTemp guard to protect against phantom ADB deltas.
        // user_only=false: classic Mac OS — kernel addresses don't matter
        // because there is no separate Unix kernel.
        mouse_guard_start((int16_t)x, (int16_t)y, /*user_only=*/false);
        break;
    case 'h':
        mouse_guard_stop();
        set_mouse_hw(x, y);
        break;
    case 'a':
        mouse_guard_stop();
        set_mouse_aux(x, y);
        break;
    default:
        mouse_guard_stop();
        set_mouse_default(x, y);
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

// === M8 — public mouse / trace control =====================================
//
// Thin wrappers around the file-private helpers used by the typed
// `mouse.move` / `mouse.click` / `mouse.trace` root methods.

void debug_mac_set_trace_mouse(bool enabled) {
    scheduler_t *sched = system_scheduler();
    if (!sched)
        return;
    static bool registered = false;
    if (!registered) {
        scheduler_new_event_type(sched, "test", NULL, "trace_mouse", &trace_mouse_tick);
        registered = true;
    }
    if (enabled) {
        if (trace_mouse_active)
            return;
        trace_mouse_active = 1;
        trace_mouse_have_last = 0;
        remove_event(sched, &trace_mouse_tick, NULL);
        scheduler_new_cpu_event(sched, &trace_mouse_tick, NULL, 0, 0, 1000000000ULL);
    } else {
        if (!trace_mouse_active)
            return;
        trace_mouse_active = 0;
        remove_event(sched, &trace_mouse_tick, NULL);
    }
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

// Inject a mouse button up/down event via the requested routing mode.
//   'g' = global (Mac OS Toolbox MBState write)
//   else = hw / default (route through ADB/VIA PB3 hardware emulation)
void debug_mac_mouse_button_mode(bool button_down, char mode) {
    if (mode == 'g')
        mouse_button_global(button_down);
    else
        system_mouse_update(button_down, 0, 0);
}

// Resolves a key name to an ADB virtual keycode, or -1 if unknown
int debug_mac_resolve_key_name(const char *name) {
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

// Phase 5c — `post-event` and `set-time` legacy commands deleted. The
// typed surface (`network.appletalk.*`, `rtc.set_time`) replaces them.
