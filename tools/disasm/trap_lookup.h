// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// trap_lookup.h
// Interface for A-trap name resolution in the standalone disasm tool.

#ifndef TRAP_LOOKUP_H
#define TRAP_LOOKUP_H

#include <stdint.h>

// Returns the human-readable name for a Mac OS A-trap opcode
const char *macos_atrap_name(uint16_t trap);

#endif // TRAP_LOOKUP_H
