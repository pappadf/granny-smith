// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// em_audio_in.c
// Browser microphone → AV Singer codec input: the WASM overrides of the
// gs_audio_in_* seam (system.h), the shared-heap sample transport, and the
// conditioning that makes an arbitrary browser microphone behave like the
// Apple PlainTalk microphone the guest software was written for.
//
// Transport — a lock-free SPSC ring in the shared wasm heap, the audio
// analogue of em_camera.c's double-buffered frame slots.  Audio cannot use
// a slot flip: the guest pulls whole half-buffers (240 frames = 10 ms) on
// the Singer's frame cadence while the browser produces on its own, so the
// two rates must be decoupled by a queue rather than a latest-wins slot.
// The MAIN THREAD (an AudioWorklet callback) writes mono int16 and bumps
// `wr`; the WORKER-side gs_audio_in_frames reads and bumps `rd`.  Single
// producer, single consumer, no locks: each index is written by exactly one
// side and read by the other.  Static storage keeps the address stable
// under ALLOW_MEMORY_GROWTH.
//
// Why the conditioning lives here and not in JS: it is the same chain the
// offline asset generator applies (debug-plaintalk/scripts/gen-sr-asset.py),
// so the PlainTalk contract is expressed once, in one language, and the
// live path and the recorded assets arrive at the guest looking alike.
// JS is left with capture and rate conversion only.

#include "em.h"

#include "system.h"

#include <emscripten.h>
#include <emscripten/threading.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// One second at the codec's default rate: far more than the ~10 ms the
// guest consumes per frame, so a scheduling hiccup on either side costs
// latency rather than a dropout.
#define GS_MIC_RING 24576u
#define GS_MIC_RATE 24000u // the rate JS is asked to deliver (Singer default)

typedef struct gs_mic_shm {
    _Atomic int32_t connected; // main thread: a track is attached and live
    _Atomic uint32_t wr; // producer index (main thread), free-running
    _Atomic uint32_t rd; // consumer index (worker), free-running
    _Atomic int32_t rate; // the rate the producer is actually delivering
    // Diagnostics, surfaced in the UI.  Worth their four bytes each: with
    // no counters, "no audio" and "wrong audio" look identical from the
    // outside, and the guest AMPLIFIES silence into full-scale white noise
    // (its recorder gains up hard), so a dead capture path sounds exactly
    // like a corrupted one.  That cost two wrong diagnoses.
    _Atomic uint32_t underruns; // consumer found less than a frame's worth
    _Atomic uint32_t overruns; // producer outran us; backlog was dropped
    int16_t ring[GS_MIC_RING]; // mono samples; stereo is made at the seam
} gs_mic_shm_t;

static gs_mic_shm_t g_mic = {.rate = GS_MIC_RATE};

// Announce the transport to the main thread once at startup; JS keeps the
// address and writes samples directly through Module.HEAP16.
void em_audio_in_init(void) {
    // clang-format off
    MAIN_THREAD_ASYNC_EM_ASM(
        { if (typeof Module.onAudioInReady === 'function') Module.onAudioInReady($0, $1, $2); },
        (uint32_t)(uintptr_t)&g_mic, GS_MIC_RING, GS_MIC_RATE);
    // clang-format on
}

// ===========================================================================
// Conditioning: an arbitrary browser microphone → a PlainTalk microphone
// ===========================================================================
//
// A PlainTalk mic is a line-level electret with a built-in preamp, and the
// guest's whole input chain is calibrated for it: TIL15884 gives 100-200
// mVpp for typical voiced speech against an A/D that clips at 1.44 V RMS
// (= 4.073 Vpp) at 0 dB gain, which the driver runs at +6..+7.5 dB — so
// full scale referred to the jack is 1.7-2.0 Vpp.  Getting this wrong is
// not cosmetic: assets 6-12 dB hot made Casper's AGC wind the codec gain
// down and the recognizer never matched (sr-test-audio-assets.md §2).
//
// Browser microphones vary by tens of dB, so two things are applied here:
//
//   1. A 100 Hz high-pass.  The electret, its preamp and the codec's AC
//      coupling do not pass the proximity rumble a laptop or headset mic
//      delivers; leaving it in eats headroom in the band that matters.
//   2. A SLOW sensitivity normaliser.  This models the FIXED sensitivity of
//      the PlainTalk preamp — it exists to cancel the spread between one
//      user's microphone and another's, not to compress speech.  It tracks
//      the level of voiced frames only and moves in seconds, so it neither
//      pumps nor fights the guest's own AGC (AnalogGC → singerCtl A/D gain),
//      which adapts far faster.  Silence holds the gain rather than winding
//      it up into the noise floor.
//
// Deliberately NOT done here: adding a noise floor (singer.c already models
// the converter's own, and a live mic brings its own room tone), and peak
// normalisation (it is what flattens speech dynamics — see the §2 contract).

// Full scale referred to the mic jack, in volts peak-to-peak:
// 1.44 V RMS * 2*sqrt(2) / 10^(7.5/20).
#define GS_MIC_FS_VPP    1.7176f
#define GS_MIC_TARGET_PP 0.150f // middle of TIL15884's 100-200 mVpp window

// The target in int16 peak-to-peak counts.
#define GS_MIC_TARGET_COUNTS (GS_MIC_TARGET_PP / GS_MIC_FS_VPP * 65536.0f)

// Voiced-frame detection: a block counts toward the level estimate only if
// it is this far above the running noise floor.  Speech sits ~20 dB or more
// over room tone; anything closer is silence and must not drive the gain.
//
// The floor is SEEDED FROM THE FIRST BLOCK OBSERVED, never from a constant.
// Both constants fail, in opposite directions, and both were caught by
// micrig rather than by review:
//
//   seeded HIGH — a quiet microphone's speech never clears the threshold, so
//     no block ever counts as voiced, the normaliser never engages, and the
//     user is inaudible with nothing to indicate why;
//   seeded LOW  — a normal room's TONE clears the threshold immediately, so
//     silence is treated as speech and amplified to speech level.  That is
//     aggressive static in the guest's recorder, and it is what shipped.
//
// Seeding from observation lands in the right ballpark for either
// microphone on the first block.  The estimate then falls fast (a pause
// re-learns a lower floor within ~0.2 s) and rises slowly (a burst of speech
// never becomes "the floor"), so a mic that starts mid-word recovers as soon
// as the speaker draws breath.
#define GS_MIC_VOICED_OVER_FLOOR 6.0f

static struct {
    float hp_x1, hp_x2, hp_y1, hp_y2; // 100 Hz Butterworth state
    float gain; // the sensitivity normaliser's current gain
    float floor_est; // running noise floor (RMS counts)
    float level_est; // running voiced level (RMS counts)
    int primed;
} g_cond;

// 2nd-order Butterworth high-pass at `fc`, recomputed when the rate moves.
static struct {
    float b0, b1, b2, a1, a2;
    uint32_t rate;
} g_hp;

// Designed in double and stored in float: the coefficients want the
// precision, the running state provably does not — micrig measures the
// designed -28.00 dB at 20 Hz either way.
static void mic_hp_design(uint32_t rate, double fc) {
    double K = tan(M_PI * fc / (double)rate);
    double Q = 0.70710678118654752;
    double n = 1.0 / (1.0 + K / Q + K * K);
    g_hp.b0 = (float)n;
    g_hp.b1 = (float)(-2.0 * n);
    g_hp.b2 = (float)n;
    g_hp.a1 = (float)(2.0 * (K * K - 1.0) * n);
    g_hp.a2 = (float)((1.0 - K / Q + K * K) * n);
    g_hp.rate = rate;
}

// Condition `n` mono samples in place.
static void mic_condition(int16_t *s, uint32_t n, uint32_t rate) {
    if (!n)
        return;
    if (g_hp.rate != rate)
        mic_hp_design(rate, 100.0);
    if (!g_cond.primed) {
        g_cond.gain = 1.0f;
        g_cond.floor_est = -1.0f; // seeded from the first block's RMS below
        g_cond.level_est = 0.0f;
        g_cond.primed = 1;
    }

    // High-pass, and measure this block's RMS from the filtered signal.
    double sum2 = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        float x = (float)s[i];
        float y = g_hp.b0 * x + g_hp.b1 * g_cond.hp_x1 + g_hp.b2 * g_cond.hp_x2 - g_hp.a1 * g_cond.hp_y1 -
                  g_hp.a2 * g_cond.hp_y2;
        g_cond.hp_x2 = g_cond.hp_x1;
        g_cond.hp_x1 = x;
        g_cond.hp_y2 = g_cond.hp_y1;
        g_cond.hp_y1 = y;
        s[i] = (int16_t)(y < -32768.0f ? -32768.0f : (y > 32767.0f ? 32767.0f : y));
        sum2 += (double)y * (double)y;
    }
    float rms = (float)sqrt(sum2 / (double)n);

    // Track the noise floor downward quickly and upward slowly, so a quiet
    // room is learned fast but a burst of speech never becomes "the floor".
    if (g_cond.floor_est < 0.0f)
        g_cond.floor_est = rms; // first block: seed from what is actually there
    else if (rms < g_cond.floor_est)
        g_cond.floor_est += (rms - g_cond.floor_est) * 0.05f;
    else
        g_cond.floor_est += (rms - g_cond.floor_est) * 0.002f;
    if (g_cond.floor_est < 4.0f)
        g_cond.floor_est = 4.0f;

    // Only voiced blocks move the level estimate; silence holds it.
    if (rms > g_cond.floor_est * GS_MIC_VOICED_OVER_FLOOR) {
        if (g_cond.level_est <= 0.0f)
            g_cond.level_est = rms;
        else
            g_cond.level_est += (rms - g_cond.level_est) * 0.05f;
    }

    // Move the gain toward what puts the voiced level at the target.  A
    // speech RMS of about a quarter of the peak-to-peak is the usual crest
    // for ordinary speech, which is what the target window describes.
    if (g_cond.level_est > 0.0f) {
        float want = (GS_MIC_TARGET_COUNTS * 0.25f) / g_cond.level_est;
        if (want > 64.0f)
            want = 64.0f; // don't chase a dead microphone into hiss
        if (want < 0.02f)
            want = 0.02f;
        g_cond.gain += (want - g_cond.gain) * 0.02f; // seconds, not frames
    }

    for (uint32_t i = 0; i < n; i++) {
        float v = (float)s[i] * g_cond.gain;
        s[i] = (int16_t)(v < -32768.0f ? -32768.0f : (v > 32767.0f ? 32767.0f : v));
    }
}

// ===========================================================================
// gs_audio_in_* seam overrides (weak defaults in core/system.c)
// ===========================================================================

bool gs_audio_in_connected(void) {
    return atomic_load_explicit(&g_mic.connected, memory_order_relaxed) != 0;
}

// Fill `frames` interleaved stereo pairs at `rate` Hz.  Returns false with
// the buffer untouched when nothing is attached, so the caller keeps its
// silence (and singer.c's converter noise floor) rather than a click.
bool gs_audio_in_frames(int16_t *lr, uint32_t frames, uint32_t rate) {
    if (!gs_audio_in_connected() || !frames)
        return false;

    uint32_t producer_rate = (uint32_t)atomic_load_explicit(&g_mic.rate, memory_order_relaxed);
    if (!producer_rate)
        producer_rate = GS_MIC_RATE;

    uint32_t rd = atomic_load_explicit(&g_mic.rd, memory_order_relaxed);
    uint32_t wr = atomic_load_explicit(&g_mic.wr, memory_order_acquire);
    uint32_t avail = wr - rd;

    // How many producer samples this request consumes.  The browser is
    // asked for the codec rate, so this is normally 1:1; the ratio covers a
    // guest that reprograms pSndRate (32/48 kHz) without renegotiating the
    // capture graph.
    uint64_t need64 = ((uint64_t)frames * producer_rate + rate / 2) / rate;
    uint32_t need = (uint32_t)(need64 ? need64 : 1);
    if (need > GS_MIC_RING)
        need = GS_MIC_RING;

    // The browser captures on WALL time and the guest consumes on EMULATED
    // time, so the two clocks drift by definition.  When the producer gets
    // ahead, drop the backlog and take the freshest samples: the alternative
    // is unbounded latency between speaking and the guest hearing it.  The
    // CONSUMER does this, never the producer — `rd` has exactly one writer,
    // which is what makes the lock-free claim true.
    if (avail > GS_MIC_RING / 2) {
        rd = wr - need;
        avail = need;
        atomic_fetch_add_explicit(&g_mic.overruns, 1, memory_order_relaxed);
    }

    // Underrun: the producer has not caught up (a tab throttled in the
    // background, or the very first frames after attach).  Report "no
    // source" rather than emitting a partial buffer of garbage — the caller
    // then presents silence for this frame and tries again on the next.
    if (avail < need) {
        atomic_fetch_add_explicit(&g_mic.underruns, 1, memory_order_relaxed);
        return false;
    }

    // Pull into a scratch block, condition it, then fan mono out to both
    // channels.  The PlainTalk plug's middle contact drives the left AND
    // right inputs with the same blended signal, so identical channels is
    // what the hardware actually presents — not a simplification.
    static int16_t block[GS_MIC_RING];
    for (uint32_t i = 0; i < need; i++)
        block[i] = g_mic.ring[(rd + i) % GS_MIC_RING];
    atomic_store_explicit(&g_mic.rd, rd + need, memory_order_release);

    mic_condition(block, need, producer_rate);

    if (need == frames) {
        for (uint32_t i = 0; i < frames; i++) {
            lr[i * 2] = block[i];
            lr[i * 2 + 1] = block[i];
        }
    } else {
        // Nearest-neighbour rate match for the off-nominal case above.
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t j = (uint32_t)((uint64_t)i * need / frames);
            if (j >= need)
                j = need - 1;
            lr[i * 2] = block[j];
            lr[i * 2 + 1] = block[j];
        }
    }
    return true;
}

// Report the capture side's own state for machine.audioin's level meter:
// whether the browser is delivering at all, how the ring is tracking, and
// where the conditioner's normaliser has settled.  The guest-side level says
// only that audio is missing; this says which side lost it.
bool gs_audio_in_debug(char *buf, size_t buflen) {
    uint32_t wr = atomic_load_explicit(&g_mic.wr, memory_order_relaxed);
    uint32_t rd = atomic_load_explicit(&g_mic.rd, memory_order_relaxed);
    snprintf(buf, buflen, "| host: conn %d in %u out %u lag %u under %u over %u @%dHz gain %.2f floor %.0f",
             atomic_load_explicit(&g_mic.connected, memory_order_relaxed) != 0, wr, rd, wr - rd,
             atomic_load_explicit(&g_mic.underruns, memory_order_relaxed),
             atomic_load_explicit(&g_mic.overruns, memory_order_relaxed),
             (int)atomic_load_explicit(&g_mic.rate, memory_order_relaxed), (double)g_cond.gain,
             (double)g_cond.floor_est);
    return true;
}

// The guest gated sound input (pSndInEn): let JS attach or release the
// microphone track, so the browser's recording indicator is lit only while
// the guest is genuinely listening — the same privacy discipline the camera
// path follows.
void gs_audio_in_state(bool active) {
    // clang-format off
    MAIN_THREAD_ASYNC_EM_ASM(
        { if (typeof Module.onAudioInState === 'function') Module.onAudioInState(!!$0); },
        active ? 1 : 0);
    // clang-format on
}
