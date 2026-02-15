// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// platform.h
// Platform-specific definitions and abstractions for Emscripten/WebAssembly

#ifndef PLATFORM_H
#define PLATFORM_H

// === Includes ===

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>
#endif

// === Type Definitions ===

struct platform;
typedef struct platform platform_t;

// Screen refresh callback
extern void platform_refresh_screen(struct platform *platform, unsigned char *buf);

// Activity indicators for device status
enum activity { activity_off = 0, activity_idle = 1, activity_read = 2, activity_write = 3 };

// Device identifiers
enum devices { device_floppy_0 = 0, device_floppy_1 = 1 };

// === Macros ===

// Portable byte swapping implementations
#define BE16(x) ((((x) & 0xff00) >> 8) | (((x) & 0x00ff) << 8))
#define BE32(x)                                                                                                        \
    ((((x) & 0xff000000) >> 24) | (((x) & 0x00ff0000) >> 8) | (((x) & 0x0000ff00) << 8) | (((x) & 0x000000ff) << 24))

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

// === Bit Manipulation Functions ===

// Count trailing zeros in a 32-bit value
static int platform_ntz32(uint32_t mask) {
    if (mask == 0)
        return 32;

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
}

// Find bit scan reverse (highest set bit position) in a 32-bit value
static int platform_bsr32(uint32_t value) {
    if (value == 0)
        return -1;

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
}

// Reverse bits in a 16-bit value
static uint16_t reverse16(uint16_t x) {
    x = (x & 0x5555) << 1 | (x & 0xAAAA) >> 1;
    x = (x & 0x3333) << 2 | (x & 0xCCCC) >> 2;
    x = (x & 0x0F0F) << 4 | (x & 0xF0F0) >> 4;
    x = (x & 0x00FF) << 8 | (x & 0xFF00) >> 8;
    return x;
}

// Count leading zeros in a 32-bit value
static int platform_nlz32(uint32_t mask) {
    if (mask == 0)
        return 32;

    return 31 - platform_bsr32(mask);
}

// === Threading Support ===

// Get current thread identifier
static void *platform_current_thread(void) {
#ifdef __EMSCRIPTEN_PTHREADS__
    // Replace deprecated function with the recommended alternative
    return (void *)(uintptr_t)emscripten_main_runtime_thread_id();
#else
    return (void *)(uintptr_t)1; // Single thread
#endif
}

// Mutex structure for thread synchronization
struct host_mutex {
#ifdef __EMSCRIPTEN_PTHREADS__
    pthread_mutex_t mutex;
#else
    int dummy; // No-op for single-threaded
#endif
};

typedef struct host_mutex host_mutex_t;

// Allocate and initialize a new mutex
static host_mutex_t *host_mutex_new(void) {
    host_mutex_t *mutex = malloc(sizeof(host_mutex_t));

#ifdef __EMSCRIPTEN_PTHREADS__
    pthread_mutex_init(&mutex->mutex, NULL);
#endif

    return mutex;
}

// Acquire a mutex lock
static void host_mutex_lock(host_mutex_t *mutex) {
#ifdef __EMSCRIPTEN_PTHREADS__
    pthread_mutex_lock(&mutex->mutex);
#endif
}

// Release a mutex lock
static void host_mutex_unlock(host_mutex_t *mutex) {
#ifdef __EMSCRIPTEN_PTHREADS__
    pthread_mutex_unlock(&mutex->mutex);
#endif
}

// === Audio Platform Interface ===

// Initialize sound subsystem
void platform_init_sound(void);

// Play 8-bit PWM audio samples
extern void platform_play_8bit_pwm(uint8_t *samples, int num_samples, unsigned int volume);

// === Timing Functions ===

#define PLATFORM_TICKS_PER_SEC 1000

// Get current time in platform ticks (milliseconds for Emscripten)
static uint64_t platform_ticks(void) {
    return (uint64_t)emscripten_get_now();
}

// Get current time in seconds (with fractional part)
static inline double host_time(void) {
    return emscripten_get_now() / 1000.0; // Convert milliseconds to seconds
}

// Get current time in milliseconds
static inline double host_time_ms(void) {
    return emscripten_get_now();
}

// === Diagnostics ===

// Print host callstack for debugging (implemented in em_main.c)
void platform_print_host_callstack(void);

#endif // PLATFORM_H
