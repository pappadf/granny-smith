// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// platform.h
// Minimal platform.h override for the standalone disasm tool.
// Force-included before all sources (-include) and also findable via
// -I for #include "platform.h" in core headers.  Defines the PLATFORM_H
// guard so the real platform.h is skipped — same pattern as
// tests/unit/support/platform.h.

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Opaque platform handle (unused in disasm tool)
typedef struct platform platform_t;

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

// Bit-reverse helper needed by MOVEM disassembly in cpu_decode.h
static inline uint16_t reverse16(uint16_t x) {
    x = (x & 0x5555) << 1 | (x & 0xAAAA) >> 1;
    x = (x & 0x3333) << 2 | (x & 0xCCCC) >> 2;
    x = (x & 0x0F0F) << 4 | (x & 0xF0F0) >> 4;
    x = (x & 0x00FF) << 8 | (x & 0xFF00) >> 8;
    return x;
}

#endif // PLATFORM_H
