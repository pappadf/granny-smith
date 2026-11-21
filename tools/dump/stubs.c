// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// stubs.c
// Linker stubs for emulator-only symbols referenced by the core source
// files we compile into the dump tool.  The real implementations live
// in src/core/debug/debug.c and friends — none of that is meaningful in
// a standalone reverse-engineering tool, so we provide minimal no-ops.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "gs_assert.h"

// A-trap name lookup is NOT stubbed here any more: the canonical
// debug_mac_atrap_name() now lives in src/core/debug/mac_traps_data.c, beside its
// table, which this tool already links.  The three former copies each
// re-declared the table with a `uint32_t trap` member where the definition has
// `uint16_t` -- C11 6.2.7 undefined behaviour across translation units.
