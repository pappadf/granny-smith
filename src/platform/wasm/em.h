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

// === JS Bridge ===
//
// Single shared-memory region that carries every JS↔C interaction.
// JS resolves the base pointer once via `_get_js_bridge()` and reads /
// writes fields by offset through `Module.HEAP32` / `Module.HEAPU8`.
// Layout is mirrored in `app/web2/src/bus/emulator.ts` (the OFF_* constants;
// the _Static_asserts below pin every offset it hardcodes); bump
// JS_BRIDGE_VERSION whenever fields are added, reordered, or resized.  The
// protocol and the result contract are described in docs/guide/web.md.
//
// The int32 words are shared with JS's Atomics.* and are therefore accessed
// only through __atomic_* on this side — never plain loads or stores, which
// carry no ordering with JS's writes (A4, F-25).  `version` is
// the exception: a static initialiser, fixed before JS can see the struct.
//
// Request protocol — exactly one kind, serialised by the JS-side
// `cmdInFlight` lock. Introspection rides on `<path>.meta.*`
// (proposal-introspection-via-meta-attribute.md); free-form shell
// lines and tab completion ride on the `Shell` class's `run` and
// `complete` methods (proposal-shell-as-object-model-citizen.md). The
// `pending` field is kept as a 32-bit slot so future call kinds can be
// added without a layout change, but only kind 1 is currently used.
//
//   pending = 1 → gs_eval(path, args)        — JSON result in `output`
//
// JS clears `done`, fills `path` / `args`, writes `pending`, and polls
// `done`. `shell_poll()` (worker pthread, every tick) drains the slot.

#define JS_BRIDGE_VERSION   7
#define JS_BRIDGE_PATH_SIZE 1024
#define JS_BRIDGE_ARGS_SIZE 8192
// 256 KB: a vfs.list of a large in-image directory is returned as one JSON
// document; 16 KB silently truncated at ~250 entries (the JS side then saw
// unparseable JSON and rendered the directory as empty). gs_eval now also
// reports truncation as an explicit error instead of returning garbage.
#define JS_BRIDGE_OUTPUT_SIZE 262144

typedef struct {
    int32_t version; // offset 0;  must equal JS_BRIDGE_VERSION
    int32_t ready; // offset 4;  1 once the worker is ready to dispatch requests
    int32_t pending; // offset 8;  1 = a gs_eval request is waiting; 0 = idle
    int32_t done; // offset 12; flipped to 1 by worker on completion
    int32_t reserved; // offset 16; unused (was a result code JS never read); kept so offsets hold
    char path[JS_BRIDGE_PATH_SIZE]; // offset 20
    char args[JS_BRIDGE_ARGS_SIZE]; // offset 1044
    char output[JS_BRIDGE_OUTPUT_SIZE]; // offset 9236
    // offset 271380: 1 once the page has a WebGPU device for the Voodoo2
    // takeover (em_gpu.c); written by JS before any machine boots.
    int32_t gpu_available;
} js_bridge_t; // total: 271384 bytes

// The offsets app/web2/src/bus/emulator.ts hardcodes (OFF_*): a field change
// that forgets the TS side now fails the build here, not the page at runtime.
_Static_assert(offsetof(js_bridge_t, version) == 0, "OFF_VERSION");
_Static_assert(offsetof(js_bridge_t, ready) == 4, "OFF_READY");
_Static_assert(offsetof(js_bridge_t, pending) == 8, "OFF_PENDING");
_Static_assert(offsetof(js_bridge_t, done) == 12, "OFF_DONE");
_Static_assert(offsetof(js_bridge_t, path) == 20, "OFF_PATH");
_Static_assert(offsetof(js_bridge_t, args) == 1044, "OFF_ARGS");
_Static_assert(offsetof(js_bridge_t, output) == 9236, "OFF_OUTPUT");
_Static_assert(offsetof(js_bridge_t, gpu_available) == 271380, "OFF_GPU_AVAILABLE");

#endif // EM_H
