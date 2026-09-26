// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// em_camera.c
// Browser webcam → AV video-in frame path: the WASM overrides of the
// gs_video_in_* seam (system.h) plus the shared-heap frame transport
// (proposal-av-video-in.md §2.3).
//
// Transport — a static double-buffered frame slot pair behind a control
// block (em_shm_layout.h) in the shared wasm heap.  Static storage keeps the
// address stable under ALLOW_MEMORY_GROWTH.  The MAIN THREAD writes each
// decoded camera frame into the non-active slot through Module.HEAPU8, flips
// `active`, then bumps `seq`; the WORKER-side gs_video_in_frame copies out of
// the active slot under a seqlock on `seq`.  The writer never touches the
// active slot, but that alone does not rule out a tear: a reader that
// latched slot A can still be copying when the writer completes B, flips,
// and starts on A.  The seqlock catches exactly that (the writer must bump
// `seq` in between), and the copy is retried.  No locks cross the thread
// boundary.
//
// Lifecycle — the camera runs only while the guest captures: VDCClk
// transitions surface through gs_video_in_state → Module.onVideoInState,
// and JS attaches/stops the MediaStreamTrack on those events under the
// user's master camera toggle (app/web2 DisplayToolbar).

#include "em.h"
#include "em_shm_layout.h"

#include "system.h"

#include <emscripten.h>
#include <emscripten/threading.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define GS_CAM_W     640
#define GS_CAM_H     480
#define GS_CAM_BYTES (GS_CAM_W * GS_CAM_H * 4)

// The shared frame transport: the control block (em_shm_layout.h), then the
// two slots.
typedef struct gs_camera_shm {
    uint32_t magic, version;
    uint32_t slot_off, slot_bytes;
    uint32_t width, height;
    _Atomic int32_t connected; // main thread: camera attached + delivering
    _Atomic int32_t active; // slot holding the newest complete frame
    _Atomic uint32_t seq; // bumped after each completed frame write
    uint32_t reserved[7]; // pad the block to 64 bytes
    uint8_t slot[2][GS_CAM_BYTES];
} gs_camera_shm_t;

_Static_assert(offsetof(gs_camera_shm_t, slot_off) == GS_CAM_W_SLOT_OFF * 4, "camera layout");
_Static_assert(offsetof(gs_camera_shm_t, width) == GS_CAM_W_WIDTH * 4, "camera layout");
_Static_assert(offsetof(gs_camera_shm_t, connected) == GS_CAM_W_CONNECTED * 4, "camera layout");
_Static_assert(offsetof(gs_camera_shm_t, active) == GS_CAM_W_ACTIVE * 4, "camera layout");
_Static_assert(offsetof(gs_camera_shm_t, seq) == GS_CAM_W_SEQ * 4, "camera layout");

static gs_camera_shm_t g_camera = {
    .magic = GS_CAM_MAGIC,
    .version = GS_CAM_VERSION,
    .slot_off = offsetof(gs_camera_shm_t, slot),
    .slot_bytes = GS_CAM_BYTES,
    .width = GS_CAM_W,
    .height = GS_CAM_H,
    .active = -1,
};

// Announce the transport to the main thread once at startup: JS reads the
// layout from the control block and writes frames through Module.HEAPU8.
void em_camera_init(void) {
    // clang-format off
    MAIN_THREAD_ASYNC_EM_ASM(
        { if (typeof Module.onVideoInReady === 'function') Module.onVideoInReady($0); },
        (uint32_t)(uintptr_t)&g_camera);
    // clang-format on
}

// === gs_video_in_* seam overrides (weak defaults in core/system.c) ==========

// True while the browser camera is attached and delivering frames — the
// guest-visible "signal present" (DMSD HLCK) answer.
bool gs_video_in_connected(void) {
    return atomic_load_explicit(&g_camera.connected, memory_order_relaxed) != 0;
}

// Copy the newest complete camera frame into the digitizer's staging
// buffer.  Runs on the emulator worker at field cadence.  A seqlock on `seq`
// (T3, F-29): if a frame completed while we copied, the slot we latched may
// be the one the writer is now filling, so copy again.  A frame period is
// ~33 ms against a ~0.2 ms copy, so a retry all but always succeeds; after
// three the copy stands -- the caller would otherwise show black, which is
// worse than the tear it avoids, and only a writer finishing frames faster
// than a memcpy could get there.
int gs_video_in_frame(uint8_t *rgba) {
    if (!gs_video_in_connected())
        return -1;
    for (int attempt = 0; attempt < 3; attempt++) {
        uint32_t seq = atomic_load_explicit(&g_camera.seq, memory_order_acquire);
        int32_t active = atomic_load_explicit(&g_camera.active, memory_order_acquire);
        if (active < 0 || active > 1)
            return -1;
        memcpy(rgba, g_camera.slot[active], GS_CAM_BYTES);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&g_camera.seq, memory_order_relaxed) == seq)
            break;
    }
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
