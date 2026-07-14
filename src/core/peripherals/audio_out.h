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
#include <stdint.h>

#include "value.h"

struct object;

// --- Stream (producer-facing wrapper over platform_audio_*) ---------------

// Opens (or re-parameterizes) the host audio stream: source sample rate in Hz
// and channel count (1 = mono, 2 = interleaved stereo).
void audio_out_open(uint32_t src_rate_hz, int channels);

// Changes the source sample rate mid-stream (e.g. ascClockRate write).
// Host-side this is a stream restart: trim to target depth, brief gain ramp.
void audio_out_set_rate(uint32_t src_rate_hz);

// Pushes nframes interleaved int16 frames at the current source rate.
// vol_0_7 is the guest's 3-bit volume; applied host-side (not captured).
void audio_out_push(const int16_t *frames, int nframes, int vol_0_7);

// Current stream parameters (0 / 0 before the first open).
uint32_t audio_out_rate(void);
int audio_out_channels(void);

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
// under the given machine sound node. One capture node exists at a time; the
// owning sound frontend must call audio_out_capture_detach() on teardown.
struct object *audio_out_capture_attach(struct object *parent);

// Detaches and deletes the capture node attached by audio_out_capture_attach.
void audio_out_capture_detach(void);

#endif // AUDIO_OUT_H
