// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// record_audio.c
// The recorder's audio timeline.  See record_audio.h for the rules.

#include "record_audio.h"

#include <stdlib.h>
#include <string.h>

// ============================================================================
// Static helpers
// ============================================================================

// The pending FIFO's frame at position i (0 = oldest).
static int16_t *frame_at(rec_timeline_t *t, size_t i) {
    return t->buf + ((t->head + i) % t->cap) * (size_t)t->channels;
}

// Make room for `more` frames beyond those pending, growing the FIFO.
static int reserve(rec_timeline_t *t, size_t more) {
    if (t->count + more <= t->cap)
        return 0;
    size_t cap = t->cap ? t->cap : 4096;
    while (cap < t->count + more)
        cap *= 2;
    int16_t *nb = malloc(cap * (size_t)t->channels * sizeof(int16_t));
    if (!nb)
        return -1;
    // Unwrap the old FIFO into the start of the new buffer
    for (size_t i = 0; i < t->count; i++)
        memcpy(nb + i * (size_t)t->channels, frame_at(t, i), (size_t)t->channels * sizeof(int16_t));
    free(t->buf);
    t->buf = nb;
    t->cap = cap;
    t->head = 0;
    return 0;
}

// Append one frame to the FIFO (room already reserved); it becomes the held value.
static void append_frame(rec_timeline_t *t, const int16_t *f) {
    int16_t *dst = frame_at(t, t->count);
    for (int c = 0; c < t->channels; c++) {
        dst[c] = f[c];
        t->last[c] = f[c];
    }
    t->count++;
}

// Append `n` copies of the held value.
static int hold(rec_timeline_t *t, uint64_t n) {
    if (!n)
        return 0;
    if (reserve(t, (size_t)n) < 0)
        return -1;
    int16_t v[2] = {t->last[0], t->last[1]};
    for (uint64_t i = 0; i < n; i++)
        append_frame(t, v);
    t->held += n;
    return 0;
}

// Drop up to `n` of the oldest pending frames that have not been read.
static void drop_head(rec_timeline_t *t, uint64_t n) {
    if (n > t->count)
        n = t->count;
    t->head = (t->head + (size_t)n) % (t->cap ? t->cap : 1);
    t->count -= (size_t)n;
    t->dropped += n;
}

// Drop up to `n` of the newest pending frames.
static void drop_tail(rec_timeline_t *t, uint64_t n) {
    if (n > t->count)
        n = t->count;
    t->count -= (size_t)n;
    t->dropped += n;
}

// A sample through a gain of num / den, rounded half away from zero.
static int16_t gain(int32_t s, int64_t num, int64_t den) {
    int64_t p = (int64_t)s * num * 2;
    int64_t q = (p + (p >= 0 ? den : -den)) / (2 * den);
    // Saturate: the gain never exceeds 1, so this only guards rounding
    return (int16_t)(q > INT16_MAX ? INT16_MAX : q < INT16_MIN ? INT16_MIN : q);
}

// One source frame converted to the timeline's channel count.
static void convert_channels(const int16_t *src, int src_channels, int channels, int16_t *out) {
    if (src_channels == channels) {
        for (int c = 0; c < channels; c++)
            out[c] = src[c];
    } else if (channels == 2) {
        // Mono to both sides
        out[0] = out[1] = src[0];
    } else {
        // Stereo (or more) to mono: the mean of the first two channels
        int32_t m = ((int32_t)src[0] + (int32_t)src[1]);
        out[0] = (int16_t)(m >= 0 ? m / 2 : -((-m) / 2));
    }
}

// Append one frame through the resampler, when one is active.  0, or -1
// when out of memory.
static int resample_frame(rec_timeline_t *t, const int16_t *f) {
    // Output frames fall at pos = k * step, measured in source frames with
    // index 0 being `prev` and index 1 this frame; emit every one before 1.
    uint64_t step = ((uint64_t)t->src_rate << 32) / t->rate;
    while (t->pos < (1ull << 32)) {
        uint32_t frac = (uint32_t)t->pos;
        int16_t o[2];
        for (int c = 0; c < t->channels; c++) {
            int64_t a = t->prev[c], b = f[c];
            o[c] = (int16_t)(a + (((b - a) * (int64_t)frac) >> 32));
        }
        // Upsampling makes several frames of one: room for each
        if (reserve(t, 1) < 0)
            return -1;
        append_frame(t, o);
        t->pos += step;
    }
    // This frame becomes index 0 for the next
    t->pos -= 1ull << 32;
    for (int c = 0; c < t->channels; c++)
        t->prev[c] = f[c];
    return 0;
}

// ============================================================================
// Operations
// ============================================================================

int rec_tl_init(rec_timeline_t *t, uint32_t rate, int channels, uint32_t frame_samples) {
    memset(t, 0, sizeof(*t));
    if (!rate || (channels != 1 && channels != 2))
        return -1;
    t->rate = rate;
    t->channels = channels;
    t->frame_samples = frame_samples ? frame_samples : 1;
    t->vol = -1;
    return reserve(t, 4096);
}

void rec_tl_free(rec_timeline_t *t) {
    free(t->buf);
    t->buf = NULL;
    t->cap = t->count = t->head = 0;
}

uint32_t rec_tl_window(const rec_timeline_t *t) {
    uint32_t w = 2 * t->frame_samples;
    return 2 * t->max_push > w ? 2 * t->max_push : w;
}

uint64_t rec_tl_target(uint64_t units, uint64_t period_ns, uint32_t rate) {
    unsigned __int128 n = (unsigned __int128)units * period_ns * rate;
    return (uint64_t)((n + 500000000u) / 1000000000u);
}

int64_t rec_tl_lead(const rec_timeline_t *t, uint64_t target) {
    return (int64_t)(t->emitted + t->count) - (int64_t)target;
}

int rec_tl_push(rec_timeline_t *t, const int16_t *s, size_t n, int src_channels, uint32_t src_rate, int vol) {
    if (!n || !s || src_channels < 1)
        return 0;
    if (src_channels > 2)
        src_channels = 2; // nothing pushes more; read the first two
    if (vol < 0)
        vol = 0;
    if (vol > 7)
        vol = 7;
    // The volume ramps from where the last push left it to this push's
    int v0 = t->vol < 0 ? vol : t->vol, v1 = vol;
    t->vol = vol;
    bool resample = src_rate && src_rate != t->rate;
    if (resample && t->src_rate != src_rate) {
        // A new source rate: start the interpolation at this push
        t->src_rate = src_rate;
        t->pos = 0;
        convert_channels(s, src_channels, t->channels, t->prev);
    } else if (!resample) {
        t->src_rate = 0;
    }
    // Room for the worst case: every source frame and the resampler's surplus
    size_t want = resample ? (size_t)((uint64_t)n * t->rate / src_rate + 2) : n;
    if (reserve(t, want) < 0)
        return -1;
    for (size_t i = 0; i < n; i++) {
        int16_t f[2];
        convert_channels(s + i * (size_t)src_channels, src_channels, t->channels, f);
        if (v0 != 7 || v1 != 7) {
            // gain = v / 7, interpolated across the push: v0 before it, v1 at its end
            int64_t num = (int64_t)v0 * (int64_t)(n - 1 - i) + (int64_t)v1 * (int64_t)(i + 1);
            int64_t den = 7 * (int64_t)n;
            for (int c = 0; c < t->channels; c++)
                f[c] = gain(f[c], num, den);
        }
        if (resample) {
            if (resample_frame(t, f) < 0)
                return -1;
        } else {
            append_frame(t, f);
        }
    }
    size_t out = resample ? (size_t)((uint64_t)n * t->rate / src_rate) : n;
    if (out > t->max_push)
        t->max_push = (uint32_t)out;
    t->ever_pushed = true;
    t->pushed_since = true;
    return 0;
}

int rec_tl_boundary(rec_timeline_t *t, uint64_t target) {
    uint64_t w = rec_tl_window(t);
    if (t->pushed_since)
        t->last_push_target = target;
    t->pushed_since = false;
    int64_t e = rec_tl_lead(t, target);
    bool idle = !t->ever_pushed || target - t->last_push_target > w;
    if (idle) {
        // Silence: the stream follows emulated time exactly
        if (e < 0 && hold(t, (uint64_t)-e) < 0)
            return -1;
    } else if (e < -(int64_t)w) {
        // Running short: hold just enough to stay inside the window
        if (hold(t, (uint64_t)(-(int64_t)w - e)) < 0)
            return -1;
    } else if (e > (int64_t)w) {
        // Running long: drop the oldest surplus
        drop_head(t, (uint64_t)(e - (int64_t)w));
    }
    // What a window behind the target is final
    uint64_t limit = target > w ? target - w : 0;
    if (limit > t->read_limit)
        t->read_limit = limit;
    return 0;
}

int rec_tl_finish(rec_timeline_t *t, uint64_t target, bool drop_excess) {
    int64_t e = rec_tl_lead(t, target);
    if (e < 0 && hold(t, (uint64_t)-e) < 0)
        return -1;
    if (e > 0 && drop_excess)
        drop_tail(t, (uint64_t)e);
    t->read_limit = target;
    t->pushed_since = false;
    return 0;
}

size_t rec_tl_ready(const rec_timeline_t *t) {
    if (t->read_limit <= t->emitted)
        return 0;
    uint64_t n = t->read_limit - t->emitted;
    return n < t->count ? (size_t)n : t->count;
}

size_t rec_tl_read(rec_timeline_t *t, int16_t *out, size_t n) {
    size_t r = rec_tl_ready(t);
    if (n > r)
        n = r;
    for (size_t i = 0; i < n; i++)
        memcpy(out + i * (size_t)t->channels, frame_at(t, i), (size_t)t->channels * sizeof(int16_t));
    t->head = (t->head + n) % t->cap;
    t->count -= n;
    t->emitted += n;
    return n;
}
