// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// platform.h
// Minimal platform.h override for the standalone dump tool.  Force-included
// before all sources (-include) and also findable via -I for #include
// "platform.h" in core headers.  Defines the PLATFORM_H guard so the real
// emulator platform.h is skipped — same pattern as tools/disasm/ and
// tests/unit/support/.

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Opaque platform handle (unused in the dump tool)
typedef struct platform platform_t;

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#endif // PLATFORM_H
