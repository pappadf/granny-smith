// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// platform_clock.h
// The browser clock (see src/platform/platform.h): emscripten_get_now, in
// milliseconds.

#ifndef PLATFORM_CLOCK_H
#define PLATFORM_CLOCK_H

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>
#endif

// Monotonic host time in milliseconds
static inline double host_time_ms(void) {
    return emscripten_get_now();
}

#endif // PLATFORM_CLOCK_H
