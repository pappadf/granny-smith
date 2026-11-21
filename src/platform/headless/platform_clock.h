// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// platform_clock.h
// The headless clock (see src/platform/platform.h): POSIX CLOCK_MONOTONIC.

#ifndef PLATFORM_CLOCK_H
#define PLATFORM_CLOCK_H

#include <stdint.h>
#include <time.h>

// Monotonic host time in milliseconds
static inline double host_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

// Monotonic host time in nanoseconds, for interval statistics
static inline uint64_t host_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#endif // PLATFORM_CLOCK_H
