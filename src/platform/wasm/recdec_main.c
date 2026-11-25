// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// recdec_main.c
// The MP4 export's decoder module: a small, non-threaded Emscripten module
// (`make recdec-module` -> build/recdec.js + recdec.wasm) that reads a
// finished recording -- the Matroska subset mkv_mux.c writes, with ZMBV
// video and FLAC audio -- and hands the export worker
// (app/web2/src/recorder/export.worker.ts) one decoded frame or audio block
// at a time.  It is built from the same sources as the emulator's own reader
// and decoders (mkv_read.c, the codec library's zmbv.c and flac.c), so a
// frame it gives is the frame `recorder.extract` gives.
//
// The module never sees the file whole and has no filesystem of its own:
// the reader's stream is a cookie FILE whose reads call back into the page
// (Module.recdecRead(source, at, len) -> Uint8Array), which the worker
// serves from the OPFS file and the vitest suite from a Node file.  So a
// recording of any size is read a cluster at a time.

// ============================================================================
// Includes
// ============================================================================

#define _GNU_SOURCE
#include "codec_flac.h"
#include "codec_zmbv.h"
#include "mkv_read.h"

#include <emscripten.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

// ============================================================================
// Type definitions
// ============================================================================

// One stream over the page's file: which file (the page's number for it),
// its size, and the position the next read starts at.
typedef struct {
    int source;
    double size;
    double pos;
} recdec_stream_t;

// An open recording: the reader, its tracks, the decoders, what the last
// call decoded, and the counts the scan before decoding found.
typedef struct {
    int source;
    double size;
    mkv_reader_t *r;
    const mkv_track_t *video, *audio;
    codec_zmbv_dec_t *zd;
    codec_flac_dec_t *fd;
    int width, height;
    uint8_t *rgb; // the decoder's RGB24 picture
    uint8_t *rgba; // the same with an opaque alpha byte: what the page wants
    bool rgba_stale; // a frame was decoded since rgba was last filled
    bool changed; // the last frame differs from the one before (an empty packet does not)
    int16_t *pcm; // the last audio block, interleaved
    int pcm_frames;
    uint64_t frames_total, audio_packets;
    uint64_t frames_done;
    bool truncated;
    char message[256];
} recdec_t;

// ============================================================================
// Static state
// ============================================================================

static char last_error[256]; // why the last recdec_open failed

// ============================================================================
// The page's file, as a stream
// ============================================================================

// Read up to `len` bytes at `at` of the page's file `source` into `dst`;
// the number read (0 at the end).
EM_JS(int, recdec_js_read, (int source, double at, uint8_t *dst, int len), {
    var b = Module['recdecRead'](source, at, len);
    if (!b || !b.length)
        return 0;
    var n = Math.min(b.length, len);
    HEAPU8.set(n < b.length ? b.subarray(0, n) : b, dst);
    return n;
});

// The cookie stream's read.
static ssize_t stream_read(void *cookie, char *buf, size_t size) {
    recdec_stream_t *s = cookie;
    double left = s->size - s->pos;
    if (left <= 0 || size == 0)
        return 0;
    // Reads stay below 2 GB a call; the reader asks for a cluster at most
    size_t want = size < (size_t)left ? size : (size_t)left;
    if (want > (1u << 30))
        want = 1u << 30;
    int n = recdec_js_read(s->source, s->pos, (uint8_t *)buf, (int)want);
    if (n < 0)
        return -1;
    s->pos += n;
    return n;
}

// The cookie stream's seek.
static int stream_seek(void *cookie, off_t *off, int whence) {
    recdec_stream_t *s = cookie;
    double base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? s->pos : s->size;
    double to = base + (double)*off;
    if (to < 0) {
        errno = EINVAL;
        return -1;
    }
    s->pos = to;
    *off = (off_t)to;
    return 0;
}

// The cookie stream's close.
static int stream_close(void *cookie) {
    free(cookie);
    return 0;
}

// A FILE over the page's file `source` of `size` bytes; NULL when out of memory.
static FILE *stream_open(int source, double size) {
    recdec_stream_t *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->source = source;
    s->size = size;
    cookie_io_functions_t io = {.read = stream_read, .write = NULL, .seek = stream_seek, .close = stream_close};
    FILE *f = fopencookie(s, "rb", io);
    if (!f)
        free(s);
    return f;
}

// ============================================================================
// Static helpers
// ============================================================================

// Open the reader on a fresh stream and find the first video and audio
// tracks; false with last_error set.
static bool open_reader(recdec_t *d) {
    FILE *f = stream_open(d->source, d->size);
    if (!f) {
        snprintf(last_error, sizeof last_error, "out of memory");
        return false;
    }
    int e = 0;
    d->r = mkv_read_open_file(f, &e);
    if (!d->r) {
        snprintf(last_error, sizeof last_error, "not a recording this reader knows (%s)", strerror(e < 0 ? -e : EIO));
        return false;
    }
    d->video = d->audio = NULL;
    for (int i = 0; i < mkv_read_track_count(d->r); i++) {
        const mkv_track_t *t = mkv_read_track(d->r, i);
        if (t->type == MKV_TRACK_VIDEO && !d->video)
            d->video = t;
        else if (t->type == MKV_TRACK_AUDIO && !d->audio)
            d->audio = t;
    }
    if (!d->video || !d->video->pixel_width || !d->video->pixel_height) {
        snprintf(last_error, sizeof last_error, "the file has no video track");
        return false;
    }
    if (strcmp(d->video->codec_id, "V_MS/VFW/FOURCC") != 0) {
        snprintf(last_error, sizeof last_error, "the video is %s, not ZMBV", d->video->codec_id);
        return false;
    }
    // An audio track in another codec is left out rather than refused
    if (d->audio && strcmp(d->audio->codec_id, "A_FLAC") != 0)
        d->audio = NULL;
    return true;
}

// ============================================================================
// Operations (exported)
// ============================================================================

// Open the page's file `source` (`size` bytes): read its header, count its
// frames and audio blocks in one pass, then rewind for decoding.  NULL with
// recdec_last_error saying why.
EMSCRIPTEN_KEEPALIVE recdec_t *recdec_open(int source, double size) {
    last_error[0] = 0;
    recdec_t *d = calloc(1, sizeof *d);
    if (!d) {
        snprintf(last_error, sizeof last_error, "out of memory");
        return NULL;
    }
    d->source = source;
    d->size = size;
    if (!open_reader(d))
        goto fail;
    // The scan: packets counted, nothing decoded
    mkv_packet_t p;
    int rc;
    while ((rc = mkv_read_packet(d->r, &p)) == 1) {
        if (p.track == d->video->number)
            d->frames_total++;
        else if (d->audio && p.track == d->audio->number)
            d->audio_packets++;
    }
    if (rc < 0) {
        snprintf(last_error, sizeof last_error, "the file cannot be read (%s)", strerror(-rc));
        goto fail;
    }
    d->truncated = mkv_read_truncated(d->r);
    mkv_read_close(d->r);
    d->r = NULL;
    if (!open_reader(d))
        goto fail;
    d->width = (int)d->video->pixel_width;
    d->height = (int)d->video->pixel_height;
    int e = 0;
    d->zd = codec_zmbv_dec_new(d->width, d->height, &e);
    d->fd = d->audio ? codec_flac_dec_new(&e) : NULL;
    size_t px = (size_t)d->width * (size_t)d->height;
    d->rgb = malloc(px * 3);
    d->rgba = malloc(px * 4);
    d->pcm = malloc(CODEC_FLAC_BLOCK * 2 * sizeof(int16_t));
    if (!d->zd || (d->audio && !d->fd) || !d->rgb || !d->rgba || !d->pcm) {
        snprintf(last_error, sizeof last_error, "out of memory");
        goto fail;
    }
    memset(d->rgba, 0, px * 4);
    return d;
fail:
    if (d->r)
        mkv_read_close(d->r);
    codec_zmbv_dec_free(d->zd);
    codec_flac_dec_free(d->fd);
    free(d->rgb);
    free(d->rgba);
    free(d->pcm);
    free(d);
    return NULL;
}

// Why the last recdec_open failed.
EMSCRIPTEN_KEEPALIVE const char *recdec_last_error(void) {
    return last_error;
}

// The stored picture's size, and the shape to show it at (the track's
// DisplayWidth / DisplayHeight; the stored size when it states none).
EMSCRIPTEN_KEEPALIVE int recdec_width(const recdec_t *d) {
    return d->width;
}
EMSCRIPTEN_KEEPALIVE int recdec_height(const recdec_t *d) {
    return d->height;
}
EMSCRIPTEN_KEEPALIVE int recdec_display_width(const recdec_t *d) {
    return d->video->display_width ? (int)d->video->display_width : d->width;
}
EMSCRIPTEN_KEEPALIVE int recdec_display_height(const recdec_t *d) {
    return d->video->display_height ? (int)d->video->display_height : d->height;
}

// The frame period in nanoseconds (the track's DefaultDuration; 0 when absent).
EMSCRIPTEN_KEEPALIVE double recdec_period_ns(const recdec_t *d) {
    return (double)d->video->default_duration_ns;
}

// The video frames in the file, and whether it ended inside a cluster.
EMSCRIPTEN_KEEPALIVE double recdec_frames_total(const recdec_t *d) {
    return (double)d->frames_total;
}
EMSCRIPTEN_KEEPALIVE int recdec_truncated(const recdec_t *d) {
    return d->truncated;
}

// The audio track: sample rate and channels (0, 0 with none), and its blocks.
EMSCRIPTEN_KEEPALIVE int recdec_audio_rate(const recdec_t *d) {
    return d->audio ? (int)d->audio->sampling_frequency : 0;
}
EMSCRIPTEN_KEEPALIVE int recdec_audio_channels(const recdec_t *d) {
    return d->audio ? (int)(d->audio->channels ? d->audio->channels : 1) : 0;
}
EMSCRIPTEN_KEEPALIVE double recdec_audio_packets(const recdec_t *d) {
    return (double)d->audio_packets;
}

// Decode the next packet in file order: 1 a video frame (recdec_rgba), 2 an
// audio block (recdec_pcm, recdec_pcm_frames), 0 at the end, -1 on a
// failure (recdec_message).
EMSCRIPTEN_KEEPALIVE int recdec_next(recdec_t *d) {
    mkv_packet_t p;
    int rc;
    while ((rc = mkv_read_packet(d->r, &p)) == 1) {
        if (p.track == d->video->number) {
            if (codec_zmbv_dec_frame(d->zd, p.data, p.len) != 0) {
                snprintf(d->message, sizeof d->message, "frame %llu: %s", (unsigned long long)d->frames_done,
                         codec_zmbv_dec_message(d->zd));
                return -1;
            }
            d->frames_done++;
            // An empty packet repeats the frame before it
            d->changed = p.len > 0 || d->frames_done == 1;
            d->rgba_stale |= d->changed;
            return 1;
        }
        if (d->audio && p.track == d->audio->number) {
            int64_t n = codec_flac_dec_frame(d->fd, p.data, p.len, d->pcm, CODEC_FLAC_BLOCK, NULL);
            if (n < 0) {
                snprintf(d->message, sizeof d->message, "audio: %s", codec_flac_dec_message(d->fd));
                return -1;
            }
            d->pcm_frames = (int)n;
            return 2;
        }
    }
    if (rc < 0) {
        snprintf(d->message, sizeof d->message, "read: %s", strerror(-rc));
        return -1;
    }
    return 0;
}

// Whether the last frame differs from the one before it (false for a
// repeat, so the page can reuse what it made of the last one).
EMSCRIPTEN_KEEPALIVE int recdec_frame_changed(const recdec_t *d) {
    return d->changed;
}

// The last frame as RGBA, width x height x 4 bytes, the palette applied,
// alpha 255.  NULL before any frame.
EMSCRIPTEN_KEEPALIVE const uint8_t *recdec_rgba(recdec_t *d) {
    if (!d->frames_done)
        return NULL;
    if (d->rgba_stale) {
        if (codec_zmbv_dec_rgb24(d->zd, d->rgb) != 0)
            return NULL;
        size_t px = (size_t)d->width * (size_t)d->height;
        // RGB24 widened to RGBA, one pixel at a time
        for (size_t i = 0; i < px; i++) {
            d->rgba[i * 4] = d->rgb[i * 3];
            d->rgba[i * 4 + 1] = d->rgb[i * 3 + 1];
            d->rgba[i * 4 + 2] = d->rgb[i * 3 + 2];
            d->rgba[i * 4 + 3] = 0xFF;
        }
        d->rgba_stale = false;
    }
    return d->rgba;
}

// The last audio block: interleaved samples, and their sample frames.
EMSCRIPTEN_KEEPALIVE const int16_t *recdec_pcm(const recdec_t *d) {
    return d->pcm;
}
EMSCRIPTEN_KEEPALIVE int recdec_pcm_frames(const recdec_t *d) {
    return d->pcm_frames;
}

// Why the last recdec_next failed.
EMSCRIPTEN_KEEPALIVE const char *recdec_message(const recdec_t *d) {
    return d->message;
}

// Close the recording and free everything.
EMSCRIPTEN_KEEPALIVE void recdec_close(recdec_t *d) {
    if (!d)
        return;
    mkv_read_close(d->r);
    codec_zmbv_dec_free(d->zd);
    codec_flac_dec_free(d->fd);
    free(d->rgb);
    free(d->rgba);
    free(d->pcm);
    free(d);
}
