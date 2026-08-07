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
    // Level of the raw Float32 blocks as JS receives them from the worklet,
    // in int16 counts, BEFORE anything of ours touches them.  This is the
    // one measurement that splits "the browser is handing us silence" from
    // "we are losing it between the worklet and the ring" — everything else
    // in this header is downstream of the write.
    _Atomic int32_t js_rms;
    _Atomic int32_t js_peak;
    // The capture device the browser actually chose, NUL-terminated ASCII,
    // written once by JS at attach.  getUserMedia picks the system default,
    // and a default that is not the user's microphone (a monitor source, an
    // HDMI input, a muted device) yields a perfectly healthy stream of
    // near-silence — indistinguishable, from inside the ring, from a bug in
    // this file.  Naming the device is what tells those apart.
    char label[64];
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
//   3. The PLAINTALK CHANNEL: a fixed first-order darkening filter.  The
//      recognizer's acoustic models were trained through the PlainTalk
//      capsule and the Singer analog input path, not through a flat digital
//      channel — and the acceptance region measured on the real recognizer
//      is lopsided around flat input: +2..4 dB of extra brightness is total
//      failure while 12 dB of darkening still recognizes, i.e. flat sits at
//      the models' bright EDGE (debug-plaintalk/re/02-acceptance-region-
//      probe.md).  This filter moves every host's audio toward the channel
//      the models expect; it is host-independent because every host delivers
//      roughly flat digital audio.
//   4. A SLOW spectral-tilt normaliser.  Microphones differ in brightness
//      the way they differ in sensitivity, so this is the tilt sibling of
//      the gain normaliser and follows the same discipline learned from it:
//      voiced-blocks-only evidence, a settle phase then a slow drift, hold
//      in silence, and a hard clamp — so it can never ride the balance
//      WITHIN an utterance the way the first gain normaliser rode the
//      level.  It starts neutral: the fixed channel filter above carries
//      the systematic correction from the first word, and this stage only
//      trims the per-microphone residual over the following utterances.
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

// The PlainTalk channel model: one-pole darkening y = (1-a)x + a*y'.
// Calibrated against the recognizer itself (re/02-acceptance-region-probe.md):
// the value must keep the reference voice recognized while moving flat input
// away from the models' bright edge.  DC gain is unity and the loss at
// 500 Hz is ~0.1 dB, so the level normaliser's target math is unaffected.
#define GS_MIC_CHANNEL_A 0.45f

// Tilt normaliser: band balance is measured around this split, and the
// corrective high-shelf is clamped to this range.  The target balance is
// what the reference asset (the voice the recognizer demonstrably accepts)
// measures through the high-pass + channel filter above — i.e. "what a
// PlainTalk microphone would have delivered", not any particular host.
#define GS_MIC_TILT_SPLIT_HZ 1000.0f
// The corrector is DARKEN-ONLY.  From one signal a dark microphone and a
// dark voice are indistinguishable, and a deep voice measured "too dark"
// would otherwise be corrected bright — cancelling the channel filter and
// pushing the speaker toward the models' bright cliff (this happened: a
// real speaker's balance sits well below the synthetic reference's, and
// the first live capture came back brighter than the old, unconditioned
// one).  The acceptance region is asymmetric — bright is a cliff at
// +2..4 dB, dark is tolerated past 12 dB — so only the bright direction
// needs correcting, and only darkening is safe to apply.
#define GS_MIC_TILT_MAX 1.0f // never brighten
#define GS_MIC_TILT_MIN 0.5f // up to -6 dB of darkening for bright mics
#define GS_MIC_BAL_TARGET                                                                                              \
    0.42f // hi/lo voiced RMS ratio of the reference asset
          // (sr-open-the-trash.wav through HP+channel)

static struct {
    float hp_x1, hp_x2, hp_y1, hp_y2; // 100 Hz Butterworth state
    float ch_y1; // PlainTalk channel one-pole state
    float lo_y1; // tilt split one-pole state (shared measure/apply)
    float gain; // the sensitivity normaliser's current gain
    float floor_est; // running noise floor (RMS counts)
    float level_est; // running voiced level (RMS counts)
    float bal_est; // running voiced hi/lo band balance
    float shelf; // tilt corrector's current high-shelf gain (1 = neutral)
    uint32_t voiced_blocks; // voiced blocks seen since the stream opened
    uint32_t voiced_run; // CONSECUTIVE voiced blocks (reset by silence)
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
        g_cond.bal_est = 0.0f;
        g_cond.shelf = 1.0f; // neutral: the fixed channel filter carries day one
        g_cond.voiced_blocks = 0;
        g_cond.primed = 1;
    }

    // Tilt split one-pole coefficient for this rate.
    float k2 = 1.0f - (float)exp(-2.0 * M_PI * (double)GS_MIC_TILT_SPLIT_HZ / (double)rate);
    // The application pass below re-runs the split filter over the same
    // samples, so it must start from the same state this pass starts from.
    float lo_state_in = g_cond.lo_y1;

    // High-pass, apply the PlainTalk channel, and measure this block's RMS
    // and band balance from the channel-filtered signal.
    double sum2 = 0.0, losum2 = 0.0, hisum2 = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        float x = (float)s[i];
        float y = g_hp.b0 * x + g_hp.b1 * g_cond.hp_x1 + g_hp.b2 * g_cond.hp_x2 - g_hp.a1 * g_cond.hp_y1 -
                  g_hp.a2 * g_cond.hp_y2;
        g_cond.hp_x2 = g_cond.hp_x1;
        g_cond.hp_x1 = x;
        g_cond.hp_y2 = g_cond.hp_y1;
        g_cond.hp_y1 = y;
        // The fixed channel: unity at DC, first-order rolloff toward Nyquist.
        float c = (1.0f - GS_MIC_CHANNEL_A) * y + GS_MIC_CHANNEL_A * g_cond.ch_y1;
        g_cond.ch_y1 = c;
        // Band split for the tilt measurement (pre-shelf, i.e. the raw
        // microphone's balance as seen through the channel).
        g_cond.lo_y1 += k2 * (c - g_cond.lo_y1);
        float hi = c - g_cond.lo_y1;
        losum2 += (double)g_cond.lo_y1 * (double)g_cond.lo_y1;
        hisum2 += (double)hi * (double)hi;
        s[i] = (int16_t)(c < -32768.0f ? -32768.0f : (c > 32767.0f ? 32767.0f : c));
        sum2 += (double)c * (double)c;
    }
    float rms = (float)sqrt(sum2 / (double)n);
    float lo_rms = (float)sqrt(losum2 / (double)n);
    float hi_rms = (float)sqrt(hisum2 / (double)n);

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
    //
    // The estimate tracks the UPPER envelope — fast up, slow down — because
    // "this microphone's sensitivity" means the level of representative
    // voiced speech, and the first block over the threshold is not that.  It
    // is the onset: a measured trace of a real utterance opens at 56 counts
    // RMS and reaches 1970 three blocks later.  Seeding from that first
    // block sets the estimate 30x low, and the gain that follows from it
    // overshoots just as far.
    if (rms > g_cond.floor_est * GS_MIC_VOICED_OVER_FLOOR) {
        if (g_cond.level_est <= 0.0f)
            g_cond.level_est = rms;
        else if (rms > g_cond.level_est)
            g_cond.level_est += (rms - g_cond.level_est) * 0.5f; // attack
        else
            g_cond.level_est += (rms - g_cond.level_est) * 0.05f; // release
        g_cond.voiced_blocks++;
        g_cond.voiced_run++;

        // Track this microphone's spectral balance — but only from blocks
        // whose balance is PLAUSIBLE FOR SPEECH.  Voiced speech through any
        // reasonable microphone lands within a couple of octaves of the
        // reference balance; a test tone, a hum, or hiss lands decades away
        // (a 500 Hz tone over hiss measures ~0.01, a whistle ~100).  Such
        // blocks must hold the estimate, not slam the shelf to its clamp.
        // Three further guards, each caught by a rig check:
        //   - both bands clearly above the noise floor (a tone's "other
        //     band" is the room hiss AT the floor, and tone/hiss can land
        //     anywhere, including inside the balance gate);
        //   - a run of ≥3 consecutive voiced blocks (an isolated click or a
        //     burst edge is broadband for one block and looks speech-like);
        //   - the balance gate itself.
        if (g_cond.voiced_run >= 3 && lo_rms > g_cond.floor_est * 1.5f && hi_rms > g_cond.floor_est * 1.5f) {
            float bal = hi_rms / lo_rms;
            if (bal > 0.08f && bal < 5.0f) {
                if (g_cond.bal_est <= 0.0f)
                    g_cond.bal_est = bal;
                else
                    g_cond.bal_est += (bal - g_cond.bal_est) * (g_cond.voiced_blocks < 20 ? 0.3f : 0.05f);
            }
        }
    } else {
        g_cond.voiced_run = 0;
    }

    // Move the gain toward what puts the voiced level at the target.  A
    // speech RMS of about a quarter of the peak-to-peak is the usual crest
    // for ordinary speech, which is what the target window describes.
    //
    // ESTABLISH THE SENSITIVITY WITHIN THE FIRST FEW BLOCKS, then hold it.
    // This models a fixed preamp, so what it must never do is ride the
    // level WITHIN an utterance — and starting at unity and crawling at 2%
    // a block (a ~0.5 s time constant) did exactly that, because the crawl
    // and the utterance are the same length.  Measured on a live capture of
    // a known-good clip: the path was 25 dB down at speech onset, still
    // 18 dB down 300 ms in, and +6 dB by the end.  The AVERAGE level came
    // out right (voiced RMS 1001 against the asset's 1143) while the
    // dynamics were destroyed, and the casualty was the first word.  For a
    // recognizer listening for an attention word, eating the first word is
    // total failure: Casper never matched, and never even printed "Pardon
    // me?", because it never heard "Computer".
    //
    // Jumping straight to the target on the FIRST voiced block is not the
    // answer either — that block is the onset, so the jump lands ~30x too
    // high and then decays, which is the same defect with the sign flipped.
    // What works is the upper-envelope estimate above plus a fast settle
    // over the first 200 ms, which converges within a few blocks and is not
    // hostage to any single one.
    // Three voiced blocks (30 ms) before touching the gain at all: one block
    // is the onset and nothing else, and acting on it alone still threw a
    // 14x spike before the envelope pulled it back.
    if (g_cond.level_est > 0.0f && g_cond.voiced_blocks >= 3) {
        float want = (GS_MIC_TARGET_COUNTS * 0.25f) / g_cond.level_est;
        if (want > 64.0f)
            want = 64.0f; // don't chase a dead microphone into hiss
        if (want < 0.02f)
            want = 0.02f;
        g_cond.gain += (want - g_cond.gain) * (g_cond.voiced_blocks < 20 ? 0.5f : 0.02f);
    }

    // Move the shelf toward what corrects this microphone's balance to the
    // reference target.  Unlike the gain there is NO fast-settle phase: the
    // fixed channel filter already carries the systematic correction from
    // the first word, so this stage may converge gently across an utterance
    // or two.  A fast phase here would re-create the ride-within-the-
    // utterance defect the gain normaliser was cured of — in spectrum
    // instead of level.
    if (g_cond.bal_est > 0.0f && g_cond.voiced_blocks >= 3) {
        float want = GS_MIC_BAL_TARGET / g_cond.bal_est;
        if (want > GS_MIC_TILT_MAX)
            want = GS_MIC_TILT_MAX;
        if (want < GS_MIC_TILT_MIN)
            want = GS_MIC_TILT_MIN;
        // 0.004 per 10 ms block ≈ a 2.5 s time constant of VOICED audio:
        // most of one utterance passes before half the correction is in.
        g_cond.shelf += (want - g_cond.shelf) * 0.004f;
    }

    // Apply the corrective high-shelf (y = lo + shelf*hi, re-splitting from
    // the same state the measurement pass started from) and the gain.
    float lo = lo_state_in;
    for (uint32_t i = 0; i < n; i++) {
        float x = (float)s[i];
        lo += k2 * (x - lo);
        float v = (lo + g_cond.shelf * (x - lo)) * g_cond.gain;
        s[i] = (int16_t)(v < -32768.0f ? -32768.0f : (v > 32767.0f ? 32767.0f : v));
    }
}

// ===========================================================================
// Rate conversion: browser rate → codec rate
// ===========================================================================
//
// The AudioContext runs at the browser's NATIVE rate (buildGraph explains
// why asking for 24 kHz yields silence on real devices), so almost every
// live capture arrives at 44.1 or 48 kHz and has to come down to the
// Singer's 24 kHz here.  What that filter does is audible, and the box
// filter this replaced was the only crude resampler left in the chain
// once the DSP's int32 defect was fixed (errata E16).  Measured on the
// box, 48→24:
//
//   passband  -1.25 dB at 8 kHz, -2.0 dB at 10 kHz   (dulls)
//   stopband  16 kHz folded onto 8 kHz only 6 dB down,
//             14 kHz onto 10 kHz only 4.3 dB down    (grit)
//
// The stopband is the real problem: everything a microphone picks up
// between 12 and 24 kHz — hiss, fans, sibilance, switching noise — landed
// back in the speech band essentially unattenuated.  At 44.1 kHz it was
// worse in a second way, because the box spanned 1 or 2 input samples
// alternately: a time-VARYING filter, which modulates as well as aliases.
//
// This is an ordinary polyphase windowed-sinc resampler: one Kaiser-
// windowed low-pass, sampled at GS_RS_PHASES fractional offsets, designed
// once whenever the ratio changes.  It is causal (the centre tap sits
// GS_RS_TAPS/2 input samples back) so it never needs samples the ring has
// not delivered, and it carries its tail across calls — the box filter
// restarted at every block boundary, which is a discontinuity per 10 ms
// frame on top of everything else.  Cost is ~11.5k multiply-adds per
// frame; the budget here is a whole 10 ms.
//
// Equal rates keep the 1:1 copy path: a mic already at the codec rate
// (and the Chromium fake device, which adopts whatever it is asked for)
// must pass through untouched rather than through a filter it does not
// need.

#define GS_RS_TAPS   48 // taps per output sample (even)
#define GS_RS_PHASES 128 // fractional positions the bank is sampled at
// Cutoff as a fraction of the lower Nyquist.  0.90 puts 48→24's corner at
// 10.8 kHz and the stopband inside 12 kHz, which is where it has to be:
// the guest's own converter is already 3.4 dB down at 9 kHz, so passband
// spent above ~11 kHz buys nothing and costs stopband.
#define GS_RS_KEEP 0.90

static struct {
    float h[GS_RS_PHASES][GS_RS_TAPS];
    uint32_t src, dst; // the ratio the bank was designed for
    int16_t hist[GS_RS_TAPS]; // input tail from the previous block
} g_rs;

// Modified Bessel function of the first kind, order 0 — the Kaiser window's
// only transcendental.  The series converges in a handful of terms for the
// beta we use.
static double rs_i0(double x) {
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 40; k++) {
        double q = x / (2.0 * k);
        t *= q * q;
        s += t;
        if (t < 1e-13 * s)
            break;
    }
    return s;
}

static void rs_design(uint32_t src, uint32_t dst) {
    // beta 7.0 is about 72 dB of stopband, which puts every fold-back
    // under the guest's own 8-bit quantisation floor.
    const double beta = 7.0;
    const int m = GS_RS_TAPS / 2;
    double fc = 0.5 * (dst < src ? (double)dst / (double)src : 1.0) * GS_RS_KEEP;
    double i0b = rs_i0(beta);

    for (int p = 0; p < GS_RS_PHASES; p++) {
        double frac = (double)p / (double)GS_RS_PHASES;
        double sum = 0.0;
        for (int k = 0; k < GS_RS_TAPS; k++) {
            // Distance, in input samples, from tap k to the point being
            // reconstructed (which sits m samples back — see the header).
            double d = (double)(k - m) + frac;
            double x = 2.0 * fc * d;
            double sinc = (fabs(x) < 1e-9) ? 1.0 : sin(M_PI * x) / (M_PI * x);
            double r = d / ((double)m + 0.5); // |r| < 1 over every tap
            double w = rs_i0(beta * sqrt(1.0 - r * r)) / i0b;
            double v = sinc * w;
            g_rs.h[p][k] = (float)v;
            sum += v;
        }
        // Normalise each phase to unity DC gain.  Without this the phases
        // differ by a fraction of a dB and the difference is a periodic
        // amplitude ripple at the beat between the two rates.
        float inv = (float)(sum != 0.0 ? 1.0 / sum : 1.0);
        for (int k = 0; k < GS_RS_TAPS; k++)
            g_rs.h[p][k] *= inv;
    }
    g_rs.src = src;
    g_rs.dst = dst;
    memset(g_rs.hist, 0, sizeof g_rs.hist);
}

// Resample `need` conditioned mono samples in `in` to `frames` output
// samples, fanned out to both channels of `lr`.
static void rs_run(const int16_t *in, uint32_t need, int16_t *lr, uint32_t frames, uint32_t src, uint32_t dst) {
    if (g_rs.src != src || g_rs.dst != dst)
        rs_design(src, dst);

    // Position is computed from `i` rather than accumulated: output g of the
    // whole stream sits at input g*need/frames, and since consecutive blocks
    // are contiguous in both, evaluating i*need/frames per block is exactly
    // that mapping with no drift.  A `pos += step` accumulator truncates
    // once per output instead, which walks ~0.0015 samples off across a
    // block and puts a discontinuity at every block boundary.
    for (uint32_t i = 0; i < frames; i++) {
        uint64_t pos = ((uint64_t)i * need << 16) / frames;
        uint32_t j = (uint32_t)(pos >> 16);
        const float *h = g_rs.h[(pos >> 9) & (GS_RS_PHASES - 1)];
        float acc = 0.0f;
        for (uint32_t k = 0; k < GS_RS_TAPS; k++) {
            int32_t n = (int32_t)j - (int32_t)k;
            acc += h[k] * (float)(n >= 0 ? in[n] : g_rs.hist[GS_RS_TAPS + n]);
        }
        int16_t v = (int16_t)(acc < -32768.0f ? -32768.0f : (acc > 32767.0f ? 32767.0f : acc));
        lr[i * 2] = v;
        lr[i * 2 + 1] = v;
    }

    // Carry the tail so the next block's first taps see real history.
    if (need >= GS_RS_TAPS) {
        memcpy(g_rs.hist, in + need - GS_RS_TAPS, sizeof g_rs.hist);
    } else {
        memmove(g_rs.hist, g_rs.hist + need, (GS_RS_TAPS - need) * sizeof(int16_t));
        memcpy(g_rs.hist + GS_RS_TAPS - need, in, need * sizeof(int16_t));
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
        rs_run(block, need, lr, frames, producer_rate, rate);
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
    snprintf(buf, buflen,
             "| host: conn %d in %u out %u lag %u under %u over %u @%dHz gain %.2f floor %.0f "
             "shelf %.2f bal %.2f | JS rms %d peak %d src \"%.48s\"",
             atomic_load_explicit(&g_mic.connected, memory_order_relaxed) != 0, wr, rd, wr - rd,
             atomic_load_explicit(&g_mic.underruns, memory_order_relaxed),
             atomic_load_explicit(&g_mic.overruns, memory_order_relaxed),
             (int)atomic_load_explicit(&g_mic.rate, memory_order_relaxed), (double)g_cond.gain,
             (double)g_cond.floor_est, (double)g_cond.shelf, (double)g_cond.bal_est,
             (int)atomic_load_explicit(&g_mic.js_rms, memory_order_relaxed),
             (int)atomic_load_explicit(&g_mic.js_peak, memory_order_relaxed), g_mic.label[0] ? g_mic.label : "?");
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
