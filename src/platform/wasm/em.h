// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// em.h
// Emscripten platform-specific interfaces (shared header)

#ifndef EM_H
#define EM_H

// === Includes ===

#include <stdbool.h>
#include <stddef.h>
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

// === Audio Subsystem ===

// Initialize audio subsystem (context + worklet module; the stream itself is
// opened by the machine's sound frontend via platform_audio_open)
void em_audio_init(void);
void em_camera_init(void);
// Announce the microphone sample ring to the main thread (em_audio_in.c).
void em_audio_in_init(void);

// Resume audio context (if suspended)
void em_audio_resume(void);

// === Main Loop and Control ===

// Execute one tick of the emulator main loop
void em_main_tick(void);

// === Diagnostics ===

// Print host callstack for debugging
void em_print_host_callstack(void);

// === The mailbox ===
//
// Every JS<->C request travels through the mailbox: a control block and
// two record rings in the wasm heap (src/core/mailbox/mailbox.h; the page's
// mirror is app/web2/src/bus/mailbox.ts).  JS resolves the control block's
// address once via `_get_gs_mailbox()`, checks MAGIC and VERSION, and from
// then on writes requests and reads results through shared memory and
// Atomics.  The emulator thread drains the request ring at every tick
// (shell_poll) and, on a stopped machine, in a bounded idle wait between
// ticks.  The protocol and the result contract are described in
// docs/guide/web.md.

#include "mailbox/mailbox.h"

// The control block (32 uint32 words, 64-byte aligned, fixed address).
uint32_t *get_gs_mailbox(void);

// Drains the mailbox: serves every pending request under the drain budget
// and wakes the page when results were written.  Returns the number of
// results written.
int shell_poll(void);

#endif // EM_H
