// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// platform_clock.h
// The headless clock (see src/platform/platform.h): POSIX CLOCK_MONOTONIC.

#ifndef PLATFORM_CLOCK_H
#define PLATFORM_CLOCK_H

#include <time.h>

// Monotonic host time in milliseconds
static inline double host_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

#endif // PLATFORM_CLOCK_H
