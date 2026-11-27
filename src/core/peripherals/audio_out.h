// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// audio_out.h
// The single host-facing audio output stream shared by every machine's sound
// frontend (Plus PWM scan, ASC), plus the deterministic capture sink used by
// headless golden-WAV integration tests (the audio analog of screen.match).
//
// Producers (chip models / VBL scans) push int16 frames in *emulated* time;
// everything real-time (resampling, rate trim, concealment) lives behind the
// platform_audio_* boundary. The capture sink records the producer output at
// guest rate, ahead of any host resampling, so captures are bit-reproducible
// for a given instruction budget and identical across hosts and CI.

#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "value.h"

struct object;

// --- Stream (producer-facing wrapper over platform_audio_*) ---------------

// Writes interleaved int16 samples as a canonical PCM WAV. 0 on success,
// -1 on I/O error. Shared with the input-side capture in singer.c so both
// produce byte-identical files (one's output is fed back through the other).
int audio_wav_write(const char *path, const int16_t *samples, size_t nsamples, uint32_t rate, int channels);

// Reads a PCM int16 WAV (what audio_wav_write writes): 0 with malloc'd
// interleaved *samples (*nsamples of them), the rate and channel count; -1
// with the reason in errbuf.  Shared with the recorder's reader (record_class.c).
int audio_wav_read(const char *path, int16_t **samples, size_t *nsamples, uint32_t *rate, int *channels, char *errbuf,
                   size_t errlen);

// Opens (or re-parameterizes) the host audio stream: source sample rate in Hz
// and channel count (1 = mono, 2 = interleaved stereo).  Resets the host
// stream's DC blocker.
void audio_out_open(uint32_t src_rate_hz, int channels);

// Changes the source sample rate mid-stream (e.g. ascClockRate write).
// Host-side this is a stream restart: trim to target depth, brief gain ramp.
void audio_out_set_rate(uint32_t src_rate_hz);

// Pushes nframes interleaved int16 frames at the current source rate.
// vol_0_7 is the guest's 3-bit volume; applied host-side (not captured).
// The capture records the frames as pushed; the host stream and the
// recorder's sink get them AC-coupled (see the DC blocker below).
void audio_out_push(const int16_t *frames, int nframes, int vol_0_7);

// A second sink beside the capture: called with every push, its channel
// count, the stream's rate and the guest's volume (the screen recorder,
// record.h).  NULL removes it.  With ac_coupled the sink gets the pushes
// through a DC blocker of its own (audio_dc_block_t below), reset by this
// call and running on from then -- across machine swaps and while the
// recorder is suspended -- so one recording's segments join without a step;
// without it, the producers' raw frames, as the capture sees them.
typedef void (*audio_out_sink_t)(const int16_t *frames, int nframes, int channels, uint32_t rate, int vol_0_7);
void audio_out_set_record_sink(audio_out_sink_t sink, bool ac_coupled);

// Current stream parameters (0 / 0 before the first open).
uint32_t audio_out_rate(void);
int audio_out_channels(void);

// --- The AC-coupled output stage ---------------------------------------------
//
// A real Mac's sound output reaches the speaker and the jack through an
// output capacitor, so a constant level is silent; the producers' DACs are
// modelled without one (the Plus's PWM byte becomes (b - 128) << 8, and a
// game that leaves the buffer idle far from 128 sits at a large constant
// level).  The host stream and the recorder each pass the pushes through a
// DC blocker of their own -- a first-order high-pass,
//
//   y[n] = x[n] - x[n-1] + a * y[n-1],   a = 1 - 2*pi*fc / fs,
//
// in fixed point (integer arithmetic only, so its output is bit-identical
// on every host) with per-channel state.  The capture (machine.sound.capture
// and match) stays the raw DAC output.

// The cutoff: 15 Hz.  The time constant is 1 / (2*pi*15) = 10.6 ms, so a
// level the guest leaves behind is gone (under half an LSB, even from full
// scale) in about 0.12 s, while the response is down only 0.55 dB at 41 Hz
// (a bass's low E) and 0.15 dB at 80 Hz -- the corner sits below anything these machines'
// speakers, or most music they play, reach.  Lower (10 Hz) would let a
// thump linger longer; higher (20 Hz and up) starts to bend the bottom
// octave and tilts low square waves.
#define AUDIO_DC_CUTOFF_HZ 15u

// One DC blocker's state: a coefficient for the rate it last ran at and,
// per channel, the previous input and output.
typedef struct audio_dc_block {
    uint32_t rate; // the rate k was computed for (0: none yet)
    int32_t k; // 1 - a, Q30
    int channels; // the channel count primed for (0: not primed)
    int32_t x1[2]; // previous input sample, per channel
    int64_t y1[2]; // previous output, Q16, per channel (unsaturated)
} audio_dc_block_t;

// Back to reset: the next frame primes the filter (x1 = that frame, y1 = 0),
// so the output starts at 0 whatever level the stream is at -- no step.
void audio_dc_block_reset(audio_dc_block_t *f);

// Run nframes interleaved frames of `channels` (1 or 2) at `rate` Hz from
// `in` to `out` (may be the same buffer; NULL advances the state only).  A
// rate change recomputes the coefficient and keeps the state; a channel
// count change primes afresh.
void audio_dc_block_run(audio_dc_block_t *f, const int16_t *in, int16_t *out, int nframes, int channels, uint32_t rate);

// The Q30 coefficient k = 1 - a for `rate` Hz at AUDIO_DC_CUTOFF_HZ.
int32_t audio_dc_block_k(uint32_t rate);

// --- Producer helpers --------------------------------------------------------

// Re-parameterizes the stream only when the engine's rate moved: each
// engine checks before every push, and a set_rate is a host stream restart.
static inline void audio_out_ensure_rate(uint32_t src_rate_hz) {
    if (audio_out_rate() != src_rate_hz)
        audio_out_set_rate(src_rate_hz);
}

// Saturates a 32-bit mix or gain result to an int16 sample.
static inline int16_t audio_sat16(int32_t v) {
    return (int16_t)(v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v);
}

// The same for a float sample on the int16 scale (truncated toward zero).
static inline int16_t audio_sat16f(float v) {
    return (int16_t)(v < -32768.0f ? -32768.0f : (v > 32767.0f ? 32767.0f : v));
}

// The 1.5 dB-per-step D/A attenuation ladder as x65536 gains (0 = loudest,
// 15 = -22.5 dB): the ASCO 2300 / AWACS codec's and the AV Singer's, the
// same sixteen values.
extern const uint32_t audio_atten_1p5db_x65536[16];

// --- Capture sink (deterministic, guest-rate, pre-volume) ------------------

// Starts recording pushed frames. Returns false if already recording.
bool audio_out_capture_start(void);

// Stops recording; optionally writes the capture as PCM int16 WAV to
// wav_path (NULL = keep in memory only, for a following match). Returns the
// number of frames captured, or -1 on error (not recording / write failed).
int64_t audio_out_capture_stop(const char *wav_path);

// True while a capture is recording.
bool audio_out_capture_active(void);

// Frames accumulated in the current or last capture.
uint64_t audio_out_capture_frames(void);

// Compares the last (stopped) capture against a golden PCM int16 WAV —
// sample-exact, zero tolerance; channels and rate must match too. On
// mismatch, reports the first divergent sample (index + timestamp) and
// writes the capture next to the golden as "<golden>.actual.wav".
// Returns val_bool(true) on match, val_err(...) otherwise, so the headless
// script runner fails the test on mismatch (same contract as screen.match).
value_t audio_out_match_value(const char *golden_wav);

// --- Object-model surface ---------------------------------------------------

// Attaches a `capture` child node (start/stop methods, active/frames attrs)
// under the given machine sound node; it is deleted with that node's tree.
// Each machine's sound node has its own, so a machine built beside the
// running one gets its own capture node.
struct object *audio_out_capture_attach(struct object *parent);

#endif // AUDIO_OUT_H
