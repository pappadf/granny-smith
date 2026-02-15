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

// Interrupt the emulator (stop execution)
void em_main_interrupt(void);

// === Diagnostics ===

// Print host callstack for debugging
void em_print_host_callstack(void);

#endif // EM_H
