// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// em_camera.c
// Browser webcam → AV video-in frame path: the WASM overrides of the
// gs_video_in_* seam (system.h) plus the shared-heap frame transport
// (proposal-av-video-in.md §2.3).
//
// Transport — the audio ring pattern inverted (em_audio.c): a static
// double-buffered frame slot pair + atomic header in the shared wasm heap.
// Static storage keeps the address stable under ALLOW_MEMORY_GROWTH.  The
// MAIN THREAD writes each decoded camera frame into the non-active slot
// through Module.HEAPU8 and then flips `active`; the WORKER-side
// gs_video_in_frame copies out of the active slot.  The writer never
// touches the active slot, so tearing is impossible and staleness is at
// most one frame.  No locks cross the thread boundary.
//
// Lifecycle — the camera runs only while the guest captures: VDCClk
// transitions surface through gs_video_in_state → Module.onVideoInState,
// and JS attaches/stops the MediaStreamTrack on those events under the
// user's master camera toggle (app/web2 DisplayToolbar).

#include "em.h"

#include "system.h"

#include <emscripten.h>
#include <emscripten/threading.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define GS_CAM_W     640
#define GS_CAM_H     480
#define GS_CAM_BYTES (GS_CAM_W * GS_CAM_H * 4)

// The shared frame transport.  Header layout (Int32 indices, for the JS
// side): [0] connected, [1] active slot (-1 = none yet), [2] frame seq.
typedef struct gs_camera_shm {
    _Atomic int32_t connected; // main thread: camera attached + delivering
    _Atomic int32_t active; // slot holding the newest complete frame
    _Atomic uint32_t seq; // bumped after each completed frame write
    _Atomic uint32_t reserved; // pad the header to 16 bytes
    uint8_t slot[2][GS_CAM_BYTES];
} gs_camera_shm_t;

static gs_camera_shm_t g_camera = {.active = -1};

// Announce the transport to the main thread once at startup: JS keeps the
// address and writes frames directly through Module.HEAPU8.
void em_camera_init(void) {
    // clang-format off
    MAIN_THREAD_ASYNC_EM_ASM(
        { if (typeof Module.onVideoInReady === 'function') Module.onVideoInReady($0, $1, $2); },
        (uint32_t)(uintptr_t)&g_camera, GS_CAM_W, GS_CAM_H);
    // clang-format on
}

// === gs_video_in_* seam overrides (weak defaults in core/system.c) ==========

// True while the browser camera is attached and delivering frames — the
// guest-visible "signal present" (DMSD HLCK) answer.
bool gs_video_in_connected(void) {
    return atomic_load_explicit(&g_camera.connected, memory_order_relaxed) != 0;
}

// Copy the newest complete camera frame into the digitizer's staging
// buffer.  Runs on the emulator worker at field cadence.
int gs_video_in_frame(uint8_t *rgba) {
    if (!gs_video_in_connected())
        return -1;
    int32_t active = atomic_load_explicit(&g_camera.active, memory_order_acquire);
    if (active < 0 || active > 1)
        return -1;
    memcpy(rgba, g_camera.slot[active], GS_CAM_BYTES);
    return 0;
}

// The guest gated the capture clock: let JS attach/stop the camera track
// (same push pattern as Module.onFloppyChange — em_main.c).
void gs_video_in_state(bool active) {
    // clang-format off
    MAIN_THREAD_ASYNC_EM_ASM(
        { if (typeof Module.onVideoInState === 'function') Module.onVideoInState(!!$0); },
        active ? 1 : 0);
    // clang-format on
}
