// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// platform.h
// The one platform header: what core code may assume of any host.
//
// Every target -- the browser (wasm), headless, the tools and the unit tier
// -- includes this, and each supplies only its clock, as `platform_clock.h`
// on its include path.  There used to be a copy per target, and they had
// drifted: the wasm copy hand-rolled the bit scans the headless one got from
// the compiler, both carried a mutex, a thread id, drive-activity enums and a
// tick counter nothing used, and the unit tier and the tools each kept a
// third and fourth.

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

// Count trailing zeros in a 32-bit value (32 for zero).
static inline int platform_ntz32(uint32_t mask) {
    return mask ? __builtin_ctz(mask) : 32;
}

// The highest set bit's position (bit scan reverse); -1 for zero.
static inline int platform_bsr32(uint32_t value) {
    return value ? 31 - __builtin_clz(value) : -1;
}

// The host seams the core calls (audio, redraw, callstack, card-ROM offers,
// camera, GPU, wake/wait, ...): one contract, each with a weak default.
#include "../core/platform_hooks.h"

// The target's clock: host_time_ms().
#include "platform_clock.h"

#endif // PLATFORM_H
