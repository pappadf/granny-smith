// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// record.h
// The screen recorder: the emulated screen and sound to a lossless
// Matroska file (ZMBV video, FLAC audio) on the guest's clock.
//
// The recorder samples the screen at frame-unit boundaries -- the scheduler
// calls it through a hook when a frame-unit's cycle budget is used up -- and
// takes every first, second or fourth one (`rate` full, half, quarter).  A
// paused machine completes no frame-units and records nothing; a turbo one
// plays back at the guest's speed.  On the emulator thread the recorder only
// copies: the band of rows that changed (XOR against a shadow copy), the
// CLUT and the response tables when they change, and each audio push, into
// a ring.  A recorder thread (record_thread.c) rebuilds and codes the frames
// and writes the file; inline, the same coder runs on the emulator thread.
//
// The recording is process state: it spans machine swaps (a checkpoint
// load is a cut in the same file when the new machine fits, a boot is a new
// segment) and is never checkpointed.  See docs/internals/core/record/record.md.

#ifndef RECORD_H
#define RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct object;

// === Settings (fixed while recording, except block) ===

// Frame-units per output frame: 1 (full), 2 (half), 4 (quarter).
typedef enum { REC_RATE_FULL = 0, REC_RATE_HALF, REC_RATE_QUARTER } rec_rate_t;

// The settings a recording starts with.
typedef struct recorder_settings {
    rec_rate_t rate;
    bool audio; // include sound (when the machine has a stream)
    bool rgb; // store 32 bpp from the start ("rgb"), else "auto"
    uint32_t canvas_w, canvas_h; // 0: the screen's size at start ("auto")
    bool block; // wait for the recorder thread instead of dropping frames
    bool threaded; // code on a recorder thread (false: on the emulator thread)
} recorder_settings_t;

// === Lifecycle ===

// Register the `recorder` root object (core_init).  Idempotent.  The
// scheduler's frame hook and audio_out's sink are installed only while a
// recording runs, so recording off costs one NULL test in each.
void recorder_init(void);

// The platform's defaults, before any recording: whether a full ring
// blocks (headless: true -- there is no real time to protect -- and the
// browser: false), and the directory a recording without a path goes to
// (NULL: a path is required).  The core's own default is the browser's:
// no blocking, /opfs/recordings.
void recorder_set_platform_defaults(bool block, const char *default_dir);

// The current settings (what the object's attributes show).
recorder_settings_t *recorder_settings(void);

// Apply a change of `block` to the recording running now (if any).
void recorder_set_block_live(bool block);

// === Recording ===

// Begin recording to `path` (.mkv; NULL or empty: the platform's default
// name).  0, or -1 with `err` (err_len bytes) saying why.
int recorder_start(const char *path, char *err, size_t err_len);

// Finalise.  Fills `paths` with the files written, malloc'd (the caller
// frees each and the array), *n_paths their count.  0, or -1 when not
// recording (err says so).
int recorder_stop(char ***paths, size_t *n_paths, char *err, size_t err_len);

// True while a recording runs.
bool recorder_active(void);

// === Machine events (system.c) ===

// A new machine became the active one: a cut (restored, and it fits the
// recording) or a new segment (a boot, or a machine that does not fit).
void recorder_machine_attached(bool restored);

// The active machine is going away with nothing in its place (quit, shut
// down): the recording is finalised.
void recorder_machine_gone(void);

// === State (the object's read-only attributes) ===

// The recorder's state for its attributes.
typedef struct recorder_state {
    bool active;
    const char *path; // the current segment (or the last one written), "" if none
    uint64_t frames; // output frames, every segment
    uint64_t units; // frame-units completed since start
    double seconds; // the emulated time the frames cover
    uint64_t bytes; // file bytes, every segment
    uint64_t dropped; // frames replaced by a repeat because the recorder was behind
    uint32_t segments; // files written so far
} recorder_state_t;

// Fill `out` with the current state.
void recorder_get_state(recorder_state_t *out);

// The frame-unit boundary hook (installed on the scheduler; exposed for
// the unit tier).
void recorder_frame_boundary(void);

// The audio sink (installed on audio_out; exposed for the unit tier).
void recorder_audio_push(const int16_t *frames, int nframes, int channels, uint32_t rate, int vol);

#endif // RECORD_H
