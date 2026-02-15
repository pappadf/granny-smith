// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// platform.h
// Platform abstraction header for headless (non-WASM) builds.

#ifndef PLATFORM_H
#define PLATFORM_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Platform identification
#define PLATFORM_HEADLESS 1

// Opaque platform handle (unused in headless mode)
struct platform;
typedef struct platform platform_t;

// Activity indicators for drives (no-op in headless)
enum activity { activity_off = 0, activity_idle = 1, activity_read = 2, activity_write = 3 };

enum devices { device_floppy_0 = 0, device_floppy_1 = 1 };

// Byte swapping macros
#define BE16(x) ((((x) & 0xff00) >> 8) | (((x) & 0x00ff) << 8))
#define BE32(x)                                                                                                        \
    ((((x) & 0xff000000) >> 24) | (((x) & 0x00ff0000) >> 8) | (((x) & 0x0000ff00) << 8) | (((x) & 0x000000ff) << 24))

// Count trailing zeros in a 32-bit value
static inline int platform_ntz32(uint32_t mask) {
    if (mask == 0)
        return 32;
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctz(mask);
#else
    int n = 0;
    if ((mask & 0x0000FFFF) == 0) {
        n += 16;
        mask >>= 16;
    }
    if ((mask & 0x000000FF) == 0) {
        n += 8;
        mask >>= 8;
    }
    if ((mask & 0x0000000F) == 0) {
        n += 4;
        mask >>= 4;
    }
    if ((mask & 0x00000003) == 0) {
        n += 2;
        mask >>= 2;
    }
    if ((mask & 0x00000001) == 0) {
        n += 1;
    }
    return n;
#endif
}

// Find the highest set bit position (bit scan reverse)
static inline int platform_bsr32(uint32_t value) {
    if (value == 0)
        return -1;
#if defined(__GNUC__) || defined(__clang__)
    return 31 - __builtin_clz(value);
#else
    int pos = 0;
    if (value & 0xFFFF0000) {
        pos += 16;
        value >>= 16;
    }
    if (value & 0x0000FF00) {
        pos += 8;
        value >>= 8;
    }
    if (value & 0x000000F0) {
        pos += 4;
        value >>= 4;
    }
    if (value & 0x0000000C) {
        pos += 2;
        value >>= 2;
    }
    if (value & 0x00000002) {
        pos += 1;
    }
    return pos;
#endif
}

// Reverse bits in a 16-bit value
static inline uint16_t reverse16(uint16_t x) {
    x = (x & 0x5555) << 1 | (x & 0xAAAA) >> 1;
    x = (x & 0x3333) << 2 | (x & 0xCCCC) >> 2;
    x = (x & 0x0F0F) << 4 | (x & 0xF0F0) >> 4;
    x = (x & 0x00FF) << 8 | (x & 0xFF00) >> 8;
    return x;
}

// Count leading zeros
static inline int platform_nlz32(uint32_t mask) {
    if (mask == 0)
        return 32;
    return 31 - platform_bsr32(mask);
}

// Thread identification
static inline void *platform_current_thread(void) {
    return (void *)(uintptr_t)pthread_self();
}

// Mutex implementation using pthreads
struct host_mutex {
    pthread_mutex_t mutex;
};
typedef struct host_mutex host_mutex_t;

static inline host_mutex_t *host_mutex_new(void) {
    host_mutex_t *m = (host_mutex_t *)malloc(sizeof(host_mutex_t));
    if (m)
        pthread_mutex_init(&m->mutex, NULL);
    return m;
}

static inline void host_mutex_lock(host_mutex_t *m) {
    if (m)
        pthread_mutex_lock(&m->mutex);
}

static inline void host_mutex_unlock(host_mutex_t *m) {
    if (m)
        pthread_mutex_unlock(&m->mutex);
}

// Time functions using POSIX clock
#define PLATFORM_TICKS_PER_SEC 1000

static inline uint64_t platform_ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static inline double host_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Audio stub (no-op in headless)

// Get current time in milliseconds
static inline double host_time_ms(void) {
    return host_time() * 1000.0;
}

// Print host callstack (stub for headless)
static inline void platform_print_host_callstack(void) {
    printf("(host callstack unavailable in headless mode)\n");
}
static inline void platform_play_8bit_pwm(uint8_t *samples, int num_samples, unsigned int volume) {
    (void)samples;
    (void)num_samples;
    (void)volume;
}

static inline void platform_init_sound(void) {}

// Video stub (no-op in headless)
static inline void platform_refresh_screen(struct platform *p, unsigned char *buf) {
    (void)p;
    (void)buf;
}

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#endif // PLATFORM_H
