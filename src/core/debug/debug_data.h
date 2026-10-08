// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// debug_data.h
// The classic Mac OS reference tables the debugger and the offline tools
// (tools/disasm, tools/dump) share: low-memory globals (mac_globals_data.c)
// and A-trap names (mac_traps_data.c).  One declaration of each table's
// type, so no translation unit re-declares its shape.

#ifndef DEBUG_DATA_H
#define DEBUG_DATA_H

#include <stddef.h>
#include <stdint.h>

// === Low-memory globals ======================================================

// One named low-memory global.  The table keeps Inside Macintosh's own
// entries, in address order, so several names can cover the same bytes
// (an alias, or a region marker spanning its fields); a size <= 0 marks
// a region with no fixed size.
typedef struct mac_global_info {
    const char *name;
    uint32_t address;
    int size; // bytes
    const char *description;
} mac_global_info_t;

extern const mac_global_info_t mac_global_vars[];
extern const size_t mac_global_vars_count;

// Find a global by name.  A name the table lists twice resolves to its first
// entry in table order.  O(log n).  NULL when unknown.
const mac_global_info_t *mac_global_find(const char *name);

// === A-traps =================================================================

// One A-trap opcode and its name.  The table is sorted by `trap`, strictly
// ascending (mac_traps_data.c looks it up by binary search), and lists most
// flag-bit variants of a trap explicitly ($A02E, $A12E, ... for _BlockMove).
typedef struct mac_trap_info {
    const char *name;
    uint16_t trap;
} mac_trap_info_t;

extern const mac_trap_info_t macos_atraps[];
extern const size_t macos_atraps_count;

#endif // DEBUG_DATA_H
