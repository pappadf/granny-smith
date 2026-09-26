// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// em_audio.c
// Audio subsystem for Emscripten platform — implements WebAudio streaming via
// AudioWorklet fed from a SharedArrayBuffer ring (perf proposal P6). One
// parameterized stream serves every machine: interleaved int16 frames, mono
// or stereo, at a runtime-settable source rate (22,255 / 22,257 / 22,050 /
// 44,100 Hz).
//
// Real-time machinery lives here on the consumer side (per the sound-emulation
// strategy): linear-interpolation resampling with PI rate trim (±2000 ppm),
// a start gate at the target depth, silence-aware depth trimming, per-quantum
// volume ramping, and a one-pole LPF. Producers stay deterministic.
//
// With PROXY_TO_PTHREAD, the emulator runs on a worker thread.  AudioContext
// and AudioWorklet are main-thread-only APIs, so the *control-plane* calls
// (init / open / rate change / resume) are proxied to the main thread with
// emscripten_sync_run_in_main_runtime_thread().  The *data plane* is not:
// with -pthread the wasm heap is a SharedArrayBuffer whose static addresses
// never move (shared WebAssembly.Memory grows in place, it never replaces
// the buffer), so the worklet consumes frames directly from a C-side ring
// via Atomics.  platform_audio_push used to be a synchronous worker→main
// round trip (~348/s during playback) doing a heap copy, a silence scan and
// a transfer postMessage — recurring stalls of the emulation thread whose
// jitter fed the governor's audio back-off.  It is now a local memcpy plus
// two atomic stores, and the governor's ring-fill query is an atomic load.

// ============================================================================
// Includes
// ============================================================================

#include "em.h"
#include "em_shm_layout.h"

#include <emscripten.h>
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

#include <stdatomic.h>
#include <stddef.h>

// ============================================================================
// SharedArrayBuffer audio ring (emulator worker → AudioWorklet)
// ============================================================================

// Ring geometry: power-of-two frame count, ~2.9 s at 22 kHz.  The struct
// lives in static storage, so its address is fixed for the process lifetime
// and stays valid across wasm memory growth (shared memory grows in place).
#define GS_ARING_FRAMES 65536
#define GS_ARING_MAX_CH 2

// The control block (em_shm_layout.h), then the frames.  One writer per
// index (T4, F-35): the producer owns write_idx, the worklet owns read_idx,
// both free-running.  A full ring is the consumer's to notice -- the producer
// just keeps writing, and the worklet resyncs when it finds more than a
// ring's worth outstanding.  A new stream is REQUESTED (reset_gen) and the
// worklet carries it out.  read_idx used to have five writers: the worklet's
// publish, its start-gate re-arm and depth trim, a rate-change message, and
// the producer's overflow CAS and stream reset.
typedef struct gs_audio_ring {
    uint32_t magic, version;
    uint32_t data_off, frames;
    _Atomic uint32_t write_idx;
    _Atomic uint32_t read_idx;
    _Atomic int32_t vol;
    _Atomic int32_t silent_pushes;
    _Atomic int32_t fill_pm;
    _Atomic uint32_t push_seq;
    _Atomic uint32_t reset_gen;
    _Atomic uint32_t owner; // written by the page: the worklet allowed to consume
    uint32_t reserved[4]; // pad the block to GS_ARING_HDR_BYTES
    int16_t data[GS_ARING_FRAMES * GS_ARING_MAX_CH];
} gs_audio_ring_t;

_Static_assert(offsetof(gs_audio_ring_t, data) == GS_ARING_HDR_BYTES, "audio ring layout");
_Static_assert(offsetof(gs_audio_ring_t, data_off) == GS_ARING_W_DATA_OFF * 4, "audio ring layout");
_Static_assert(offsetof(gs_audio_ring_t, write_idx) == GS_ARING_W_WRITE * 4, "audio ring layout");
_Static_assert(offsetof(gs_audio_ring_t, read_idx) == GS_ARING_W_READ * 4, "audio ring layout");
_Static_assert(offsetof(gs_audio_ring_t, fill_pm) == GS_ARING_W_FILL_PM * 4, "audio ring layout");
_Static_assert(offsetof(gs_audio_ring_t, push_seq) == GS_ARING_W_PUSH_SEQ * 4, "audio ring layout");
_Static_assert(offsetof(gs_audio_ring_t, reset_gen) == GS_ARING_W_RESET_GEN * 4, "audio ring layout");
_Static_assert(offsetof(gs_audio_ring_t, owner) == GS_ARING_W_OWNER * 4, "audio ring layout");

static gs_audio_ring_t g_aring = {
    .magic = GS_ARING_MAGIC,
    .version = GS_ARING_VERSION,
    .data_off = offsetof(gs_audio_ring_t, data),
    .frames = GS_ARING_FRAMES,
    .fill_pm = -1,
}; // the one shared ring
static int g_aring_channels = 1; // set by platform_audio_open
static double g_last_push_time = -1.0; // producer-side freshness for ring_fill

// ============================================================================
// WebAudio (AudioWorklet) Implementation
// ============================================================================

// Initialize the WebAudio subsystem: create the context and register the
// worklet processor. The worklet *node* is created lazily by gs_audio_open_js
// once the machine's sound frontend declares its stream parameters.
// clang-format off
EM_JS(void, gs_audio_init_js, (), {
    // Check if already initialized
    if (Module.gsAudio && Module.gsAudio.initialized)
        return;

    // Initialize audio subsystem object
    var ga = Module.gsAudio = (Module.gsAudio || {});
    ga.initialized = true;
    ga.targetLatency = 0.083; // fixed latency target (~5 Plus VBLs)
    ga.srcRate = 0;           // set by gs_audio_open_js
    ga.channels = 0;
    ga.node = null;
    ga.pend = null;           // stream params requested before the module loaded
    ga.modReady = false;
    ga.ctx = ga.ctx || new (self.AudioContext || self.webkitAudioContext)();

    // Autoplay policy: a suspended AudioContext may only resume from a user
    // gesture.  The data path no longer touches the main thread (SAB ring),
    // so the old per-push resume nudge is gone — hook input gestures once
    // instead (resume() on a running context is a no-op).
    var resumeOnGesture = function() {
        try {
            if (ga.ctx.state === 'suspended')
                ga.ctx.resume();
        } catch (e) {
        }
    };
    self.addEventListener('pointerdown', resumeOnGesture, true);
    self.addEventListener('keydown', resumeOnGesture, true);

    // The AudioWorklet processor is app/web2's gsAudio.worklet.ts (its ring
    // logic, audioRing.ts, is unit-tested there), bundled by the page and
    // handed over as Module.gsAudioWorkletUrl.  It consumes int16 frames
    // straight from the ring in the wasm heap (a SharedArrayBuffer), reading
    // the layout from the ring's control block.  It used to be a JS string
    // in this file, where nothing could test it.
    ga.nextOwner = 0;

    // (Re)creates the worklet node for the pending stream parameters
    ga.makeNode = function() {
        var p = ga.pend;
        if (!p)
            return;
        ga.pend = null;
        if (ga.node) {
            // disconnect() alone does not stop an AudioWorkletProcessor: a
            // source node whose process() returns true stays "actively
            // processing" even with no connections, so the old processor
            // would keep consuming the shared ring forever, racing the new
            // node for read_idx — audible as a chopped-up stream after any
            // channel-count change (e.g. mono ASC -> stereo Singer).
            try {
                ga.node.port.postMessage({stop: 1});
            } catch (e) {
            }
            try {
                ga.node.disconnect();
            } catch (e) {
            }
            ga.node = null;
        }
        ga.srcRate = p.rate;
        ga.channels = p.ch;
        try {
            // Hand the worklet the wasm heap (a SharedArrayBuffer under
            // -pthread) plus the ring's fixed address; it consumes frames
            // and publishes fill reports through it with Atomics.  Only the
            // worklet whose id is in the owner word consumes: a retired one
            // that has not yet seen its {stop} cannot touch read_idx.
            var heap = (typeof HEAPU8 !== 'undefined') ? HEAPU8 : Module.HEAPU8;
            var owner = ++ga.nextOwner;
            Atomics.store(new Int32Array(heap.buffer, p.ringPtr, 16), 11, owner); // GS_ARING_W_OWNER
            ga.node = new AudioWorkletNode(ga.ctx, 'gs-audio-worklet', {
                numberOfInputs: 0,
                numberOfOutputs: 1,
                outputChannelCount: [p.ch],
                processorOptions: {
                    srcRate: p.rate,
                    channels: p.ch,
                    targetLatency: ga.targetLatency,
                    sab: heap.buffer,
                    ringPtr: p.ringPtr,
                    owner: owner
                }
            });
            ga.node.connect(ga.ctx.destination);
            // Opportunistic resume: covers a context suspended before the
            // node existed when a qualifying gesture already happened.
            try {
                if (ga.ctx.state === 'suspended')
                    ga.ctx.resume();
            } catch (e) {
            }
            console.log('[audio] worklet open rate=' + p.rate + 'Hz ch=' + p.ch +
                        ' targetLatency=' + (ga.targetLatency * 1000).toFixed(1) + 'ms (SAB ring)');
        } catch (e) {
            console.error('[audio] worklet node creation failed', e);
        }
    };

    // Load the worklet module the page bundled, then honor any pending open
    if (!Module.gsAudioWorkletUrl) {
        console.error('[audio] no worklet module (Module.gsAudioWorkletUrl unset): audio is off');
        return;
    }
    ga.ctx.audioWorklet.addModule(Module.gsAudioWorkletUrl)
        .then(function() {
            ga.modReady = true;
            ga.makeNode();
        })
        .catch(function(e) { console.error('[audio] addModule failed', e); });
});

// Resume audio context if suspended
EM_JS(void, gs_audio_resume_js, (), {
    try {
        const ga = Module.gsAudio;
        if (ga && ga.ctx && ga.ctx.state === 'suspended')
            ga.ctx.resume();
    } catch (e) {
    }
});

// Open (or re-parameterize) the stream: same channel count = in-place rate
// change (worklet restart), different channel count = node re-creation.
// ring_ptr / ring_frames locate the shared ring inside the wasm heap.
EM_JS(void, gs_audio_open_js, (int rate, int channels, uint32_t ring_ptr), {
    gs_audio_init_js();
    var ga = Module.gsAudio;
    if (ga.node && ga.channels === channels) {
        if (ga.srcRate !== rate) {
            ga.srcRate = rate;
            ga.node.port.postMessage({rate: rate});
        }
        return;
    }
    ga.pend = {rate: rate, ch: channels, ringPtr: ring_ptr};
    if (ga.modReady)
        ga.makeNode();
});

// Change the source sample rate mid-stream (stream restart in the worklet)
EM_JS(void, gs_audio_set_rate_js, (int rate), {
    var ga = Module.gsAudio;
    if (!ga)
        return;
    if (ga.pend) {
        ga.pend.rate = rate;
        return;
    }
    if (ga.srcRate === rate)
        return;
    ga.srcRate = rate;
    if (ga.node)
        ga.node.port.postMessage({rate: rate});
});

// clang-format on

// ============================================================================
// Main-thread Proxy Wrappers
// ============================================================================

// These wrappers ensure audio EM_JS functions execute on the main thread
// (where AudioContext and AudioWorklet are available), even though the
// emulator runs on a worker thread via PROXY_TO_PTHREAD.

static void gs_audio_init(void) {
    if (emscripten_is_main_browser_thread()) {
        gs_audio_init_js();
    } else {
        emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_V, gs_audio_init_js);
    }
}

static void gs_audio_resume(void) {
    if (emscripten_is_main_browser_thread()) {
        gs_audio_resume_js();
    } else {
        emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_V, gs_audio_resume_js);
    }
}

static void gs_audio_open(int rate, int channels) {
    uint32_t ring_ptr = (uint32_t)(uintptr_t)&g_aring;
    if (emscripten_is_main_browser_thread()) {
        gs_audio_open_js(rate, channels, ring_ptr);
    } else {
        emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_VIII, gs_audio_open_js, rate, channels, ring_ptr);
    }
}

static void gs_audio_set_rate(int rate) {
    if (emscripten_is_main_browser_thread()) {
        gs_audio_set_rate_js(rate);
    } else {
        emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_VI, gs_audio_set_rate_js, rate);
    }
}

// ============================================================================
// Operations (Public API)
// ============================================================================

// Initialize audio subsystem (context + worklet module; node comes with open)
void em_audio_init(void) {
    gs_audio_init();
}

// Resume audio context
void em_audio_resume(void) {
    gs_audio_resume();
}

// ============================================================================
// Platform Interface Implementation
// ============================================================================

// Open (or re-parameterize) the host audio stream
void platform_audio_open(uint32_t src_rate_hz, int channels) {
    if (channels < 1)
        channels = 1;
    if (channels > GS_ARING_MAX_CH)
        channels = GS_ARING_MAX_CH;
    g_aring_channels = channels;
    // Fresh stream: ask the worklet to drop what is queued, so stale frames
    // from a previous machine or stream shape can't play into the new one.
    // (Asked, not done: read_idx is the worklet's alone.)
    atomic_fetch_add_explicit(&g_aring.reset_gen, 1, memory_order_release);
    atomic_store_explicit(&g_aring.silent_pushes, 0, memory_order_relaxed);
    atomic_store_explicit(&g_aring.fill_pm, -1, memory_order_relaxed);
    gs_audio_open((int)src_rate_hz, channels);
}

// Push interleaved int16 frames to the stream: a local copy into the shared
// ring plus atomic index/volume updates.  No main-thread round trip — this
// runs entirely on the emulation thread (perf proposal P6).
void platform_audio_push(const int16_t *frames, int nframes, int vol_0_7) {
    if (!frames || nframes <= 0)
        return;
    gs_audio_ring_t *ring = &g_aring;
    int ch = g_aring_channels;
    uint32_t n = (uint32_t)nframes;
    uint32_t mask = GS_ARING_FRAMES - 1;
    if (n > mask)
        return; // nonsensical push (larger than the ring)

    // Silence detection (all samples equal — any DC level counts); this scan
    // used to run on the main thread inside the push proxy.
    int total = nframes * ch;
    bool silent = true;
    for (int k = 1; k < total; k++) {
        if (frames[k] != frames[0]) {
            silent = false;
            break;
        }
    }

    // Free-running: the slot is w & mask.  A full ring is not the
    // producer's business: it overwrites the oldest frames, and the worklet,
    // finding more than a ring's worth outstanding, resyncs to the newest.
    uint32_t w_idx = atomic_load_explicit(&ring->write_idx, memory_order_relaxed);
    uint32_t w = w_idx & mask;

    // Copy the frames in at most two contiguous spans.
    uint32_t first = GS_ARING_FRAMES - w;
    if (first > n)
        first = n;
    memcpy(&ring->data[w * ch], frames, (size_t)first * ch * sizeof(int16_t));
    if (n > first)
        memcpy(&ring->data[0], frames + first * ch, (size_t)(n - first) * ch * sizeof(int16_t));

    // Publish: data first, then the release-store of write_idx the consumer
    // acquires — the worklet never reads frames it can't see completely.
    atomic_store_explicit(&ring->write_idx, w_idx + n, memory_order_release);
    atomic_store_explicit(&ring->vol, vol_0_7 & 7, memory_order_relaxed);
    if (silent)
        atomic_fetch_add_explicit(&ring->silent_pushes, 1, memory_order_relaxed);
    else
        atomic_store_explicit(&ring->silent_pushes, 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&ring->push_seq, 1, memory_order_relaxed);
    g_last_push_time = host_time();
}

// Change the source sample rate mid-stream
void platform_audio_set_rate(uint32_t src_rate_hz) {
    gs_audio_set_rate((int)src_rate_hz);
}

// Ring fill fraction against target depth for the accelerated-mode governor:
// an atomic read of the worklet's periodic report in the ring header.
// Meaningful only while the producer is actively pushing — reads as "no
// signal" within half a second of the stream idling, matching the governor's
// contract from the proxy-based implementation.
double platform_audio_ring_fill(void) {
    if (g_last_push_time < 0.0 || host_time() - g_last_push_time > 0.5)
        return -1.0;
    int32_t pm = atomic_load_explicit(&g_aring.fill_pm, memory_order_relaxed);
    if (pm < 0)
        return -1.0;
    return (double)pm / 1000.0;
}
