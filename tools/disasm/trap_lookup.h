// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// trap_lookup.h
// A-trap name resolution for the standalone disasm tool.
//
// The implementation is src/core/debug/mac_traps_data.c, beside the table it
// reads -- this tool already links that file.  It used to be a third copy of
// the algorithm here in trap_lookup.c, alongside one in tools/dump/stubs.c and
// the emulator's own, each re-declaring the table with a mismatched member
// type.  See mac_traps_data.c for what that cost.

#ifndef TRAP_LOOKUP_H
#define TRAP_LOOKUP_H

#include <stdint.h>

// Returns the human-readable name for a Mac OS A-trap opcode.
const char *macos_atrap_name(uint16_t trap);

#endif // TRAP_LOOKUP_H
