// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// record_audio.h
// The recorder's audio timeline: the guest's pushes, placed on emulated
// time, at one sample rate and channel count fixed when a segment starts.
//
// Pushes carry no timestamp.  The timeline counts the sample frames it has
// been given and, at every output frame boundary, compares that count with
// the number emulated time says should exist by then (the target).  The
// difference e = (emitted + pending) - target is the stream's lead:
//
//   idle     no push for longer than the window (or none yet): the guest is
//            silent, so a short stream is filled up to the target exactly
//            with the last value held -- a sound that starts later starts
//            at its own time;
//   running  a chip is pushing: e stays inside +-window.  Below it, the
//            last value is held for the missing samples; above it, the
//            oldest pending samples are dropped.  A chip whose rate is a
//            fraction of a hertz off the declared one is corrected a sample
//            at a time, once its lead reaches the edge.
//
// The window is two frame-units of samples, or twice the largest push seen
// when that is more, so the jitter of a chip that pushes in chunks is never
// mistaken for drift.  Samples leave the timeline (rec_tl_read) only once
// they are a window behind the target: no correction can reach them then.
//
// A push is converted on the way in: to the segment's channel count (mono
// to both sides, stereo averaged), through the guest's volume (gain
// volume / 7, linear, ramped across the push that changes it), and, when the
// stream's rate is not the segment's, resampled by linear interpolation.
//
// Pure C over libc: the unit tier drives it with synthetic pushes.

#ifndef RECORD_AUDIO_H
#define RECORD_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One segment's audio timeline.
typedef struct rec_timeline {
    uint32_t rate; // the segment's sample rate (Hz)
    int channels; // 1 or 2
    // Pending sample frames, oldest first: a FIFO of `count` frames from
    // `head` in a buffer of `cap` frames (interleaved).
    int16_t *buf;
    size_t cap, head, count;
    uint64_t emitted; // sample frames read out (rec_tl_read) so far
    uint64_t read_limit; // frames up to this index may be read
    int16_t last[2]; // the value a hold repeats: the stream's latest frame
    uint32_t frame_samples; // samples in one frame-unit, rounded up
    uint32_t max_push; // largest push seen, in output frames
    bool ever_pushed; // a push has arrived since the timeline began
    bool pushed_since; // a push arrived since the last boundary
    uint64_t last_push_target; // the target at the last boundary that followed a push
    int vol; // the volume the last push ended at (-1: none yet)
    // The resampler, while the stream's rate differs from `rate`.
    uint32_t src_rate; // the rate the resampler was set up for (0: none)
    uint64_t pos; // 32.32 position in the source, index 0 = prev
    int16_t prev[2]; // the last source frame of the previous push
    // Counters (diagnostics and tests).
    uint64_t held, dropped;
} rec_timeline_t;

// Set up a timeline at `rate` Hz with `channels` (1 or 2); `frame_samples`
// is one frame-unit's worth of samples (the window's floor is twice it).
// 0, or -1 when out of memory or the parameters are invalid.
int rec_tl_init(rec_timeline_t *t, uint32_t rate, int channels, uint32_t frame_samples);

// Release the timeline's buffer.
void rec_tl_free(rec_timeline_t *t);

// Take one push of `n` interleaved frames of `src_channels` at `src_rate` Hz
// with the guest's volume `vol` (0..7).  0, or -1 when out of memory.
int rec_tl_push(rec_timeline_t *t, const int16_t *s, size_t n, int src_channels, uint32_t src_rate, int vol);

// An output frame boundary: emulated time says `target` sample frames
// exist.  Applies the hold and drop rules above.  0, or -1 when out of memory.
int rec_tl_boundary(rec_timeline_t *t, uint64_t target);

// The end of a segment at `target`: the stream is held up to it when
// short; when long and `drop_excess`, cut back to it (the recording ends),
// else the excess stays pending (it continues in the next segment).  After
// this every frame up to `target` can be read.  0, or -1 when out of memory.
int rec_tl_finish(rec_timeline_t *t, uint64_t target, bool drop_excess);

// Sample frames that may be read now: those a window behind the last
// boundary's target (after rec_tl_finish: all up to its target).
size_t rec_tl_ready(const rec_timeline_t *t);

// Read up to `n` ready frames into `out` (interleaved); returns the count.
size_t rec_tl_read(rec_timeline_t *t, int16_t *out, size_t n);

// The window, in sample frames.
uint32_t rec_tl_window(const rec_timeline_t *t);

// The stream's lead over `target`: (emitted + pending) - target.
int64_t rec_tl_lead(const rec_timeline_t *t, uint64_t target);

// Sample frames per segment-relative frame-unit count: round(units * period
// * rate / 1e9), exact in 128-bit arithmetic.
uint64_t rec_tl_target(uint64_t units, uint64_t period_ns, uint32_t rate);

#endif // RECORD_AUDIO_H
