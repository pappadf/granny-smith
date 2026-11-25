// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// platform_clock.h
// The browser clock (see src/platform/platform.h): emscripten_get_now, in
// milliseconds.

#ifndef PLATFORM_CLOCK_H
#define PLATFORM_CLOCK_H

#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>
#endif

// Monotonic host time in milliseconds
static inline double host_time_ms(void) {
    return emscripten_get_now();
}

// Monotonic host time in nanoseconds, for interval statistics
static inline uint64_t host_time_ns(void) {
    return (uint64_t)(emscripten_get_now() * 1e6);
}

#endif // PLATFORM_CLOCK_H
