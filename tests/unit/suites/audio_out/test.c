// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for audio_out.c's AC-coupled output stage.  The DC blocker: a
// step comes through and decays at the 15 Hz corner's time constant; a
// stream that starts at a large constant level primes to silence (no
// click); a constant level is removed exactly; a 1 kHz sine passes at its
// level; the output is the same however the input is split into pushes and
// matches a pinned checksum (integer arithmetic, so every host agrees);
// stereo channels are independent; a rate change keeps the state, a channel
// change primes afresh.  The path: the capture keeps the raw frames, the
// host stream and the recorder's sink get them coupled, each with its own
// state (open resets the host's, installing the sink the recorder's), and a
// sink installed raw gets the raw frames.

#include "audio_out.h"
#include "test_assert.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 22255u // the Plus's stream

// ============================================================================
// Stubs for what audio_out.c calls
// ============================================================================

// The host stream: the last push's frames, and how many were pushed in all.
static int16_t host_last[4096];
static int host_last_n;
static long host_total;

void platform_audio_open(uint32_t src_rate_hz, int channels) {
    (void)src_rate_hz;
    (void)channels;
}

void platform_audio_set_rate(uint32_t src_rate_hz) {
    (void)src_rate_hz;
}

void platform_audio_push(const int16_t *frames, int nframes, int vol_0_7) {
    (void)vol_0_7;
    memcpy(host_last, frames, (size_t)nframes * sizeof(int16_t) * (size_t)audio_out_channels());
    host_last_n = nframes;
    host_total += nframes;
}

// The object-model surface is not exercised here.
struct object *object_new(const void *cls, void *data, const char *name) {
    (void)cls;
    (void)data;
    (void)name;
    return NULL;
}
void object_set_label(struct object *o, const char *label) {
    (void)o;
    (void)label;
}
void object_attach(struct object *parent, struct object *child) {
    (void)parent;
    (void)child;
}
int out_printf(const char *fmt, ...) {
    (void)fmt;
    return 0;
}
bool path_has_ext(const char *path, const char *ext) {
    (void)path;
    (void)ext;
    return true;
}

// ============================================================================
// Helpers
// ============================================================================

// FNV-1a over int16 samples: a checksum of a filter's output.
static uint32_t fnv(const int16_t *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ (uint8_t)s[i]) * 16777619u;
        h = (h ^ (uint8_t)((uint16_t)s[i] >> 8)) * 16777619u;
    }
    return h;
}

// A deterministic test signal: a DC offset, a 440 Hz-ish integer triangle
// and a pseudo-random component (an LCG), so every path through the
// rounding is taken.
static void signal(int16_t *s, size_t n, int16_t dc) {
    uint32_t r = 12345;
    for (size_t i = 0; i < n; i++) {
        r = r * 1103515245u + 12345u;
        int32_t tri = (int32_t)(i % 50);
        tri = tri < 25 ? tri * 400 - 5000 : (50 - tri) * 400 - 5000;
        int32_t v = dc + tri + (int32_t)((r >> 16) & 0x3FF) - 512;
        s[i] = (int16_t)v;
    }
}

// ============================================================================
// The DC blocker
// ============================================================================

TEST(coefficient_is_the_15hz_corner) {
    // k = 2*pi*15/22255 = 0.0042349, Q30
    int32_t k = audio_dc_block_k(RATE);
    ASSERT_TRUE(fabs((double)k / (double)(1 << 30) - 2.0 * M_PI * 15.0 / RATE) < 1e-9);
    ASSERT_EQ_INT(0, audio_dc_block_k(0));
    ASSERT_EQ_INT(1 << 28, audio_dc_block_k(10)); // clamped far below any chip's rate
}

TEST(prime_makes_no_click) {
    // A game leaves the Plus's PWM buffer idle far from 128: -14040
    audio_dc_block_t f;
    audio_dc_block_reset(&f);
    int16_t in[2000], out[2000];
    for (int i = 0; i < 2000; i++)
        in[i] = -14040;
    audio_dc_block_run(&f, in, out, 2000, 1, RATE);
    for (int i = 0; i < 2000; i++)
        ASSERT_EQ_INT(0, out[i]);
    // A small signal on top of the offset comes through, centred on 0
    for (int i = 0; i < 2000; i++)
        in[i] = (int16_t)(-14040 + ((i / 20) & 1 ? 77 : -77));
    audio_dc_block_run(&f, in, out, 2000, 1, RATE);
    int lo = 0, hi = 0;
    for (int i = 0; i < 2000; i++) {
        lo = out[i] < lo ? out[i] : lo;
        hi = out[i] > hi ? out[i] : hi;
    }
    ASSERT_TRUE(lo >= -160 && lo <= -70 && hi >= 70 && hi <= 160);
}

TEST(step_response_decays) {
    audio_dc_block_t f;
    audio_dc_block_reset(&f);
    enum { N = 4000 };
    static int16_t in[N], out[N];
    in[0] = 0; // primed at 0
    for (int i = 1; i < N; i++)
        in[i] = 10000;
    audio_dc_block_run(&f, in, out, N, 1, RATE);
    ASSERT_EQ_INT(0, out[0]);
    ASSERT_EQ_INT(10000, out[1]); // the step comes through whole
    // Monotonic decay, e^-1 after one time constant (236 samples at 22,255 Hz)
    for (int i = 2; i < N; i++)
        ASSERT_TRUE(out[i] <= out[i - 1] && out[i] >= 0);
    int tau = (int)(RATE / (2.0 * M_PI * 15.0) + 0.5);
    double ratio = out[1 + tau] / 10000.0;
    ASSERT_TRUE(fabs(ratio - exp(-1.0)) < 0.01);
    // Gone in under 0.12 s: exactly 0 (the rounding never leaves a residue)
    ASSERT_EQ_INT(0, out[(int)(0.12 * RATE)]);
    ASSERT_EQ_INT(0, out[N - 1]);
}

TEST(dc_removed) {
    // A negative step, a full-scale one, and a signal on a DC: no level stays
    audio_dc_block_t f;
    audio_dc_block_reset(&f);
    enum { N = 22255 };
    static int16_t in[N], out[N];
    in[0] = 32767;
    for (int i = 1; i < N; i++)
        in[i] = -32768;
    audio_dc_block_run(&f, in, out, N, 1, RATE);
    ASSERT_EQ_INT(-32768, out[1]); // saturated, the state is not
    ASSERT_EQ_INT(0, out[N - 1]);
    // A signal on a DC of -14040: its mean over the last half-second is ~0
    signal(in, N, -14040);
    audio_dc_block_reset(&f);
    audio_dc_block_run(&f, in, out, N, 1, RATE);
    double sum = 0;
    for (int i = N / 2; i < N; i++)
        sum += out[i];
    ASSERT_TRUE(fabs(sum / (N - N / 2)) < 50.0);
}

TEST(sine_passes) {
    // A 1 kHz sine at 10000 comes through at its level and phase
    audio_dc_block_t f;
    audio_dc_block_reset(&f);
    enum { N = 22255 };
    static int16_t in[N], out[N];
    for (int i = 0; i < N; i++)
        in[i] = (int16_t)lrint(10000.0 * sin(2.0 * M_PI * 1000.0 * i / RATE));
    audio_dc_block_run(&f, in, out, N, 1, RATE);
    int peak = 0, maxdiff = 0;
    for (int i = N / 2; i < N; i++) {
        peak = abs(out[i]) > peak ? abs(out[i]) : peak;
        maxdiff = abs(out[i] - in[i]) > maxdiff ? abs(out[i] - in[i]) : maxdiff;
    }
    ASSERT_TRUE(peak >= 9950 && peak <= 10050); // 0.04 dB at 1 kHz
    ASSERT_TRUE(maxdiff < 200); // a 0.86 degree phase lead (atan(15 / 1000)): 150 at this level
}

// The checksum of signal(30000, -14040) through the blocker at 22,255 Hz:
// a change to the arithmetic changes every recording's sound, so it is pinned.
#define DETERMINISM_FNV 0xE0FE8926u

TEST(deterministic) {
    // The same input split into pushes of any size gives the same output,
    // and that output is pinned: integer arithmetic, every host agrees
    enum { N = 30000 };
    static int16_t in[N], a[N], b[N];
    signal(in, N, -14040);
    audio_dc_block_t f;
    audio_dc_block_reset(&f);
    audio_dc_block_run(&f, in, a, N, 1, RATE);
    audio_dc_block_reset(&f);
    for (int i = 0, n = 1; i < N; i += n, n = n * 3 % 997 + 1)
        audio_dc_block_run(&f, in + i, b + i, i + n > N ? N - i : n, 1, RATE);
    ASSERT_TRUE(memcmp(a, b, sizeof a) == 0);
    // In place is the same as out of place
    memcpy(b, in, sizeof b);
    audio_dc_block_reset(&f);
    audio_dc_block_run(&f, b, b, N, 1, RATE);
    ASSERT_TRUE(memcmp(a, b, sizeof a) == 0);
    // NULL output advances the state as a run would
    audio_dc_block_t g;
    audio_dc_block_reset(&f);
    audio_dc_block_reset(&g);
    audio_dc_block_run(&f, in, NULL, N / 2, 1, RATE);
    audio_dc_block_run(&g, in, b, N / 2, 1, RATE);
    ASSERT_TRUE(memcmp(&f, &g, sizeof f) == 0);
    fprintf(stderr, "  checksum %08x\n", fnv(a, N));
    ASSERT_EQ_HEX(DETERMINISM_FNV, fnv(a, N));
}

TEST(stereo_channels_independent) {
    // Left: an offset and a step; right: a sine -- each as it would be alone
    enum { N = 8000 };
    static int16_t l[N], r[N], st[2 * N], lo[N], ro[N];
    signal(l, N, 9000);
    for (int i = 0; i < N; i++) {
        r[i] = (int16_t)lrint(-3000 + 8000.0 * sin(2.0 * M_PI * 440.0 * i / 44100.0));
        st[2 * i] = l[i];
        st[2 * i + 1] = r[i];
    }
    audio_dc_block_t f;
    audio_dc_block_reset(&f);
    audio_dc_block_run(&f, st, st, N, 2, 44100);
    audio_dc_block_reset(&f);
    audio_dc_block_run(&f, l, lo, N, 1, 44100);
    audio_dc_block_reset(&f);
    audio_dc_block_run(&f, r, ro, N, 1, 44100);
    for (int i = 0; i < N; i++) {
        ASSERT_EQ_INT(lo[i], st[2 * i]);
        ASSERT_EQ_INT(ro[i], st[2 * i + 1]);
    }
    ASSERT_EQ_INT(0, st[0]); // both sides primed: no click
    ASSERT_EQ_INT(0, st[1]);
}

TEST(rate_change_keeps_state_channels_reprime) {
    audio_dc_block_t f;
    audio_dc_block_reset(&f);
    int16_t in[400], out[400];
    for (int i = 0; i < 400; i++)
        in[i] = (int16_t)(i < 10 ? 0 : 8000);
    audio_dc_block_run(&f, in, out, 400, 1, 22255);
    int16_t before = out[399];
    // The same level at another rate: the decay carries on, no step
    audio_dc_block_run(&f, in + 200, out, 200, 1, 44100);
    ASSERT_TRUE(out[0] < before && out[0] > before - 200);
    ASSERT_EQ_INT(audio_dc_block_k(44100), f.k);
    // Stereo now: primed on its first frame
    int16_t st[4] = {5000, -5000, 5000, -5000};
    audio_dc_block_run(&f, st, st, 2, 2, 44100);
    ASSERT_TRUE(st[0] == 0 && st[1] == 0 && st[2] == 0 && st[3] == 0);
}

// ============================================================================
// Where it applies
// ============================================================================

// The recorder's sink: the last push it got.
static int16_t sink_last[4096];
static int sink_n;
static void sink(const int16_t *frames, int nframes, int channels, uint32_t rate, int vol) {
    (void)rate;
    (void)vol;
    memcpy(sink_last, frames, (size_t)nframes * (size_t)channels * sizeof(int16_t));
    sink_n = nframes;
}

TEST(path_capture_raw_host_and_recorder_coupled) {
    int16_t idle[370];
    for (int i = 0; i < 370; i++)
        idle[i] = -14040;
    audio_out_open(RATE, 1);
    audio_out_set_record_sink(sink, true);
    ASSERT_TRUE(audio_out_capture_start());
    audio_out_push(idle, 370, 7);
    // Host and recorder: coupled, primed on the stream's first frame -- silence
    ASSERT_EQ_INT(370, host_last_n);
    ASSERT_EQ_INT(370, sink_n);
    for (int i = 0; i < 370; i++) {
        ASSERT_EQ_INT(0, host_last[i]);
        ASSERT_EQ_INT(0, sink_last[i]);
    }
    // The capture: the raw DAC output
    ASSERT_EQ_INT(370, (int)audio_out_capture_frames());
    ASSERT_TRUE(audio_out_capture_stop(NULL) == 370);

    // The guest goes quiet at 0 then back to its offset: both see the step
    // and its decay, the same values while their states agree
    int16_t zero[370] = {0};
    audio_out_push(zero, 370, 7);
    ASSERT_EQ_INT(14040, host_last[0]);
    ASSERT_TRUE(memcmp(host_last, sink_last, sizeof zero) == 0);

    // A re-open resets the host's state only: the recorder runs on
    audio_out_open(RATE, 1);
    audio_out_push(zero, 370, 7);
    ASSERT_EQ_INT(0, host_last[0]); // primed at 0
    ASSERT_TRUE(sink_last[0] > 0 && sink_last[0] < 14040); // still decaying
    ASSERT_TRUE(sink_last[369] < sink_last[0]);

    // Installing the sink again (a new recording) resets the recorder's
    audio_out_set_record_sink(sink, true);
    audio_out_push(idle, 370, 7);
    ASSERT_EQ_INT(0, sink_last[0]);
    ASSERT_EQ_INT(0, sink_last[369]);

    // Installed raw: the sink gets the DAC's frames; the host stays coupled
    audio_out_set_record_sink(sink, false);
    audio_out_push(idle, 370, 7);
    ASSERT_EQ_INT(-14040, sink_last[0]);
    ASSERT_TRUE(host_last[0] < 0 && host_last[0] > -14040);
    audio_out_set_record_sink(NULL, false);
}

int main(void) {
    RUN(coefficient_is_the_15hz_corner);
    RUN(prime_makes_no_click);
    RUN(step_response_decays);
    RUN(dc_removed);
    RUN(sine_passes);
    RUN(deterministic);
    RUN(stereo_channels_independent);
    RUN(rate_change_keeps_state_channels_reprime);
    RUN(path_capture_raw_host_and_recorder_coupled);
    return 0;
}
