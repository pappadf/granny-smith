// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// em.h
// Emscripten platform-specific interfaces (shared header)

#ifndef EM_H
#define EM_H

// === Includes ===

#include <stdbool.h>
#include <stdint.h>

// === Forward Declarations ===

struct config;

// === Video Subsystem ===

// Initialize video subsystem
void em_video_init(void);

// Update video from emulator's framebuffer
void em_video_update(void);

// Force a redraw of the video display
void em_video_force_redraw(void);

// Get pointer to framebuffer
uint8_t *em_video_get_framebuffer(void);

// === Audio Subsystem ===

// Initialize audio subsystem
void em_audio_init(void);

// Resume audio context (if suspended)
void em_audio_resume(void);

// Play 8-bit PWM audio samples
void em_audio_play_8bit_pwm(uint8_t *samples, int num_samples, unsigned int volume);

// === Main Loop and Control ===

// Execute one tick of the emulator main loop
void em_main_tick(void);

// === Diagnostics ===

// Print host callstack for debugging
void em_print_host_callstack(void);

// === JS Bridge ===
//
// Single shared-memory region that carries every JS↔C interaction.
// JS resolves the base pointer once via `_get_js_bridge()` and reads /
// writes fields by offset through `Module.HEAP32` / `Module.HEAPU8`.
// Layout is mirrored in `app/web/js/emulator.js`; bump JS_BRIDGE_VERSION
// whenever fields are added, reordered, or resized.
//
// Request protocol — exactly two kinds, serialised by the JS-side
// `cmdInFlight` lock. Introspection and tab completion are reached
// through `gs_eval` on the synthetic `meta.*` surface
// (proposal-introspection-via-meta-attribute.md), not through their own
// bridge kinds.
//
//   pending = 1 → gs_eval(path, args)        — JSON result in `output`
//   pending = 4 → free-form shell line       — int result in `result`,
//                                              stdout/stderr to terminal
//
// JS clears `done`, fills `path` / `args`, writes `pending`, and polls
// `done`. `shell_poll()` (worker pthread, every tick) drains the slot.

#define JS_BRIDGE_VERSION     4
#define JS_BRIDGE_PATH_SIZE   1024
#define JS_BRIDGE_ARGS_SIZE   8192
#define JS_BRIDGE_OUTPUT_SIZE 16384

typedef struct {
    int32_t version; // offset 0;  must equal JS_BRIDGE_VERSION
    volatile int32_t ready; // offset 4;  1 once the worker is ready to dispatch requests
    volatile int32_t pending; // offset 8;  request kind (1..4); 0 = idle
    volatile int32_t done; // offset 12; flipped to 1 by worker on completion
    volatile int32_t result; // offset 16; integer result code
    char path[JS_BRIDGE_PATH_SIZE]; // offset 20
    char args[JS_BRIDGE_ARGS_SIZE]; // offset 1044
    char output[JS_BRIDGE_OUTPUT_SIZE]; // offset 9236
} js_bridge_t; // total: 25620 bytes

#endif // EM_H
