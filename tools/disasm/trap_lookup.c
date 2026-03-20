// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// trap_lookup.c
// A-trap name lookup using data from mac_traps_data.c.

#include "trap_lookup.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// A-trap info (defined in mac_traps_data.c)
extern struct {
    const char *name;
    uint32_t trap;
} macos_atraps[];
extern const size_t macos_atraps_count;

// Look up a trap by its raw 16-bit value
static const char *lookup_atrap(uint16_t trap) {
    for (size_t i = 0; i < macos_atraps_count; i++) {
        if (macos_atraps[i].trap == trap) {
            return macos_atraps[i].name;
        }
    }
    return NULL;
}

// Returns the trap name, handling toolbox vs OS trap bit masking
const char *macos_atrap_name(uint16_t trap) {
    static char buffer[32];
    const char *name;

    if (trap & 0x0800) { // toolbox trap
        if ((name = lookup_atrap(trap & 0xFBFF)))
            return name;
    } else { // OS trap
        if ((name = lookup_atrap(trap)))
            return name;
    }

    // fallback: hex representation
    snprintf(buffer, sizeof(buffer), "_%04X", trap);
    return buffer;
}
