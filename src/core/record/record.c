// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// record.c
// The recorder's emulator-thread half: the frame-unit hook, the capture
// (one pass over the framebuffer against a shadow copy, the CLUT and the
// response tables against theirs), the audio sink, segments, the drop and
// resync rule, and the recorder's events.  The records it writes and the
// thread that codes them are in record_internal.h / record_thread.c.

#include "record.h"

#include "record_internal.h"
#include "record_pixels.h"

#include "audio_out.h"
#include "display.h"
#include "json_escape.h"
#include "log.h"
#include "platform_clock.h"
#include "scheduler.h"
#include "storage_util.h"
#include "system.h"
#include "event/event.h"
#include "mailbox/mailbox_ring.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

LOG_USE_CATEGORY_NAME("recorder");

// The pixel formats record_pixels.c numbers are display.h's
_Static_assert(PIXEL_1BPP_MSB == REC_PIX_1BPP && PIXEL_8BPP == REC_PIX_8BPP && PIXEL_16BPP_555 == REC_PIX_16BPP555 &&
                   PIXEL_32BPP_XRGB == REC_PIX_32BPP && PIXEL_16BPP_565 == REC_PIX_16BPP565,
               "record_pixels.h's formats follow pixel_format_t");

// ============================================================================
// Constants and state
// ============================================================================

#define RING_DEFAULT (8u << 20) // the ring: 8 MB, or twice the largest record and the audio room when that is more
#define AUDIO_ROOM   REC_RESERVE_ROOM_MAX // a frame leaves this much of the ring for audio
#define EVENT_MS     1000.0 // a progress event once a second while recording
#define MAX_PATH_LEN 1024
#define MAX_SEGMENTS 4096

// The settings, as the object's attributes show and set them.
static recorder_settings_t s_set = {
    .rate = REC_RATE_FULL,
    .audio = true,
    .rgb = false,
    .block = false,
    .threaded = true,
};

// Where a recording without a path goes (NULL: a path is required).
static char *s_default_dir = NULL;
static bool s_default_dir_set = false;

// One recording's emulator-thread state.
static struct {
    bool active;
    bool suspended; // the file is closed; resuming opens the next segment
    bool external; // the last frame's screen was presented by the GPU (half rate at most)
    rec_worker_t *w;
    uint32_t div; // frame-units per output frame
    uint64_t units; // frame-units completed since start
    uint64_t frames; // output frames, every segment
    uint64_t dropped;
    uint32_t pending; // repeat frames not yet in the ring
    uint32_t pending_flush; // write the repeats once this many are pending (a second's worth)
    double last_event_ms;
    // The shadow of the framebuffer and of what goes with it
    uint8_t *shadow;
    bool have_mode;
    uint32_t mw, mh, mfmt;
    size_t mrow;
    bool resync; // send whole rows and everything with them next
    uint8_t clut[REC_TABLE_BYTES];
    uint32_t clut_len;
    bool has_dac, has_crt;
    uint8_t dac[REC_TABLE_BYTES], crt[REC_TABLE_BYTES];
    // The open segment
    char base[MAX_PATH_LEN]; // the first file's path without ".mkv"
    uint32_t seg_index;
    uint32_t cw, ch, cls;
    uint32_t arate;
    int ach;
    char **paths;
    size_t npaths;
    // After the recording: what the attributes still report
    uint64_t final_bytes;
} R;

// The last recording's paths, kept for `path` after stop.
static char s_last_path[MAX_PATH_LEN + 32];

// ============================================================================
// Events
// ============================================================================

// Emit the recorder's state event: {"event":"recorder","state":...}.
static void rec_event(const char *state, const char *message) {
    recorder_state_t st;
    recorder_get_state(&st);
    char path[3 * MAX_PATH_LEN], msg[400] = "";
    if (json_escape(st.path, path, sizeof path) < 0)
        path[0] = '\0';
    if (message && json_escape(message, msg, sizeof msg) < 0)
        msg[0] = '\0';
    event_emitf(GS_EVENT_STATE,
                "{\"event\":\"recorder\",\"state\":\"%s\",\"path\":\"%s\",\"segment\":%u,\"frames\":%llu,"
                "\"seconds\":%.3f,\"bytes\":%llu,\"dropped\":%llu,\"external\":%s%s%s%s}",
                state, path, (unsigned)st.segments, (unsigned long long)st.frames, st.seconds,
                (unsigned long long)st.bytes, (unsigned long long)st.dropped, st.external ? "true" : "false",
                message ? ",\"message\":\"" : "", message ? msg : "", message ? "\"" : "");
}

// ============================================================================
// Settings and state
// ============================================================================

void recorder_set_platform_defaults(bool block, const char *default_dir) {
    s_set.block = block;
    free(s_default_dir);
    s_default_dir = default_dir ? gs_strdup(default_dir) : NULL;
    s_default_dir_set = true;
}

recorder_settings_t *recorder_settings(void) {
    return &s_set;
}

void recorder_set_block_live(bool block) {
    if (R.active && R.w)
        rec_worker_set_block(R.w, block);
}

bool recorder_active(void) {
    return R.active;
}

void recorder_get_state(recorder_state_t *out) {
    memset(out, 0, sizeof(*out));
    out->active = R.active;
    out->suspended = R.active && R.suspended;
    out->external = R.active && R.external;
    out->path = R.npaths ? R.paths[R.npaths - 1] : s_last_path;
    out->frames = R.frames;
    out->units = R.units;
    out->seconds = (double)R.frames * (double)(R.div ? R.div : 1) * (double)MAC_VBL_PERIOD_NS / 1e9;
    out->dropped = R.dropped;
    out->segments = (uint32_t)R.npaths;
    if (R.w) {
        rec_stats_t s;
        rec_worker_stats(R.w, &s);
        out->bytes = s.bytes;
    } else {
        out->bytes = R.final_bytes;
    }
}

// ============================================================================
// The ring
// ============================================================================

// Write the pending repeat frames as one record; false when the ring has no
// room (and `must` was not given).
static bool flush_repeats(bool must) {
    if (!R.pending)
        return true;
    uint8_t *p = rec_worker_reserve(R.w, REC_K_REPEAT, sizeof(rec_repeat_hdr_t), 0, must);
    if (!p)
        return false;
    rec_repeat_hdr_t h = {.count = R.pending};
    memcpy(p, &h, sizeof h);
    rec_worker_commit(R.w);
    R.pending = 0;
    return true;
}

// The ring size for records of up to `largest` payload bytes.
static uint32_t ring_size_for(size_t largest) {
    // As rec_worker_fit asks: the record twice over and the room a frame leaves
    uint32_t size = RING_DEFAULT;
    while ((uint64_t)MBX_PAD8(MBX_HDR_BYTES + largest) * 2 + REC_RESERVE_ROOM_MAX > size && size < (1u << 30))
        size *= 2;
    return size;
}

// The largest FRAME record a mode can produce: header, CLUT, both tables, every row.
static size_t frame_record_max(uint32_t height, size_t row_bytes) {
    return sizeof(rec_frame_hdr_t) + 3 * (size_t)REC_TABLE_BYTES + (size_t)height * row_bytes;
}

// ============================================================================
// Segments
// ============================================================================

// The path of segment `index` (1 = the base path itself).
static void segment_path(uint32_t index, char *out, size_t cap) {
    if (index <= 1)
        snprintf(out, cap, "%s.mkv", R.base);
    else
        snprintf(out, cap, "%s-%u.mkv", R.base, (unsigned)index);
}

// True when the display has a response table to apply.
static bool display_tables(const display_t *d) {
    return d && (d->crt_response || d->dac_lut);
}

// Start a segment for a screen of w x h in `fmt` (tables: one applies) and
// the audio stream as it is now.  The first one opens the recording.
// 0, or -1 when the ring would not take it.
static int segment_begin(uint32_t w, uint32_t h, uint32_t fmt, bool tables, const display_t *d) {
    // Whatever repeats are owed belong to the file before
    flush_repeats(true);
    if (R.npaths >= MAX_SEGMENTS)
        return -1;
    R.seg_index++;
    char path[MAX_PATH_LEN + 32];
    segment_path(R.seg_index, path, sizeof path);
    // The canvas: the screen's size, or the size asked for when larger
    R.cw = s_set.canvas_w > w ? s_set.canvas_w : w;
    R.ch = s_set.canvas_h > h ? s_set.canvas_h : h;
    R.cls = (uint32_t)rec_pix_class(fmt, tables, s_set.rgb);
    // The audio stream as the machine has it now (none: no audio track)
    R.arate = s_set.audio ? audio_out_rate() : 0;
    R.ach = audio_out_channels();
    if (R.ach < 1 || R.ach > 2)
        R.arate = 0;
    rec_seg_hdr_t sh = {
        .canvas_w = R.cw,
        .canvas_h = R.ch,
        .cls = R.cls,
        .div = R.div,
        .audio_rate = R.arate,
        .audio_channels = (uint32_t)(R.arate ? R.ach : 0),
        .index = R.seg_index,
        .path_len = (uint32_t)strlen(path),
    };
    // Non-square pixels: the track's display size carries the shape
    if (d && d->par_w && d->par_h && d->par_w != d->par_h) {
        sh.display_w = R.cw * d->par_w;
        sh.display_h = R.ch * d->par_h;
    }
    if (rec_worker_fit(R.w, frame_record_max(R.ch, rec_pix_row_bytes(REC_PIX_32BPP, R.cw))) < 0)
        return -1;
    uint8_t *p = rec_worker_reserve(R.w, REC_K_SEGMENT, sizeof sh + sh.path_len, 0, true);
    if (!p)
        return -1;
    memcpy(p, &sh, sizeof sh);
    memcpy(p + sizeof sh, path, sh.path_len);
    rec_worker_commit(R.w);
    char **np = realloc(R.paths, (R.npaths + 1) * sizeof(*np));
    if (!np)
        return -1;
    R.paths = np;
    R.paths[R.npaths++] = gs_strdup(path);
    // The next frame is sent whole, with everything that goes with it
    R.resync = true;
    R.have_mode = false;
    LOG(1, "segment %u: %s, %ux%u class %u, audio %u Hz x %d", (unsigned)R.seg_index, path, (unsigned)R.cw,
        (unsigned)R.ch, (unsigned)R.cls, (unsigned)R.arate, R.arate ? R.ach : 0);
    return 0;
}

// True when a screen of w x h in `fmt` fits the open segment.
static bool segment_fits(uint32_t w, uint32_t h, uint32_t fmt, bool tables) {
    return w <= R.cw && h <= R.ch && rec_pix_class_fits((int)R.cls, fmt, tables);
}

// ============================================================================
// Stopping
// ============================================================================

// Remove the hooks and free the emulator-thread state of a finished recording.
static void teardown(void) {
    scheduler_set_frame_hook(NULL);
    audio_out_set_record_sink(NULL);
    free(R.shadow);
    R.shadow = NULL;
    R.have_mode = false;
    R.active = false;
    R.suspended = false;
}

// Finalise the recording: the STOP record, then wait for the file to close.
// Returns the coder's error (0 when the file is whole), its message in `msg`.
static int finish(char *msg, size_t msg_cap) {
    flush_repeats(true);
    if (rec_worker_reserve(R.w, REC_K_STOP, 0, 0, true))
        rec_worker_commit(R.w);
    rec_stats_t s;
    rec_worker_free(R.w, &s, msg, msg_cap);
    R.w = NULL;
    R.final_bytes = s.bytes;
    if (R.npaths)
        snprintf(s_last_path, sizeof s_last_path, "%s", R.paths[R.npaths - 1]);
    teardown();
    LOG(1, "stopped: %llu frames, %llu bytes, %llu dropped", (unsigned long long)R.frames, (unsigned long long)s.bytes,
        (unsigned long long)R.dropped);
    return s.err;
}

// Free the list of paths the recording wrote.
static void free_paths(void) {
    for (size_t i = 0; i < R.npaths; i++)
        free(R.paths[i]);
    free(R.paths);
    R.paths = NULL;
    R.npaths = 0;
}

// The recorder thread failed (a write, the disk full): finalise what
// exists and say so.
static void fail_stop(void) {
    char msg[200] = "";
    finish(msg, sizeof msg);
    LOG(0, "recording stopped: %s", msg);
    rec_event("error", msg[0] ? msg : "the recorder failed");
    rec_event("stopped", NULL);
    // `path` names the last file; the list itself is gone (stop has nothing to return)
    free_paths();
}

// ============================================================================
// Capture
// ============================================================================

// Compare the display's CLUT and response tables with their shadows: the
// FRAME flags for what changed (or, on a resync, for everything).
static uint32_t tables_check(const display_t *d) {
    uint32_t flags = 0;
    if (rec_pix_indexed(d->format)) {
        uint8_t clut[REC_TABLE_BYTES] = {0};
        uint32_t len = d->clut ? (d->clut_len > 256 ? 256 : d->clut_len) : 0;
        for (uint32_t i = 0; i < len; i++) {
            clut[i * 3] = d->clut[i].r;
            clut[i * 3 + 1] = d->clut[i].g;
            clut[i * 3 + 2] = d->clut[i].b;
        }
        if (R.resync || len != R.clut_len || memcmp(clut, R.clut, sizeof clut) != 0) {
            memcpy(R.clut, clut, sizeof clut);
            R.clut_len = len;
            flags |= REC_FR_CLUT;
        }
    }
    bool dac = d->dac_lut != NULL, crt = d->crt_response != NULL;
    bool changed = R.resync || dac != R.has_dac || crt != R.has_crt ||
                   (dac && memcmp(d->dac_lut, R.dac, REC_TABLE_BYTES) != 0) ||
                   (crt && memcmp(d->crt_response, R.crt, REC_TABLE_BYTES) != 0);
    if (changed) {
        R.has_dac = dac;
        R.has_crt = crt;
        if (dac)
            memcpy(R.dac, d->dac_lut, REC_TABLE_BYTES);
        if (crt)
            memcpy(R.crt, d->crt_response, REC_TABLE_BYTES);
        flags |= REC_FR_TABLES | (dac ? REC_FR_DAC : 0) | (crt ? REC_FR_CRT : 0);
    }
    return flags;
}

// Take the mode of `d` for the shadow; false when out of memory.
static bool shadow_mode(const display_t *d, size_t row) {
    free(R.shadow);
    R.shadow = calloc(d->height, row);
    if (!R.shadow)
        return false;
    R.mw = d->width;
    R.mh = d->height;
    R.mfmt = d->format;
    R.mrow = row;
    R.have_mode = true;
    R.resync = true;
    return true;
}

// One output frame: the band that changed, and the tables with it, into
// the ring -- or a repeat when nothing changed or the ring is full.
static void capture(void) {
    display_t *d = system_display();
    size_t row = d ? rec_pix_row_bytes(d->format, d->width) : 0;
    if (!d || !d->bits || !d->width || !d->height || !row || d->stride < row) {
        R.pending++; // no picture to take: the last one stands
        return;
    }
    // A screen the GPU presents (a WebGPU takeover) costs a readback per
    // frame taken: at full rate only every other frame-unit is taken, the
    // ones between are repeats (half and quarter rate are already so)
    R.external = d->presented_externally;
    if (R.external && R.div == 1 && (R.units & 1)) {
        R.pending++;
        return;
    }
    // A card that builds its frame on demand brings `bits` current first
    display_sync_pixels(d);
    bool tables = display_tables(d);
    uint32_t flags = 0;
    if (!segment_fits(d->width, d->height, d->format, tables)) {
        // A larger mode, or a pixel class the file cannot hold: the next file
        if (segment_begin(d->width, d->height, d->format, tables, d) < 0) {
            rec_event("error", "could not start the next segment");
            R.pending++;
            return;
        }
        rec_event("segment", NULL);
    }
    if (!R.have_mode || d->width != R.mw || d->height != R.mh || d->format != R.mfmt || row != R.mrow) {
        if (!shadow_mode(d, row)) {
            R.pending++;
            return;
        }
        flags |= REC_FR_MODE;
    }
    flags |= tables_check(d);
    uint32_t first = 0, end = 0;
    if (R.resync) {
        end = d->height;
        flags |= REC_FR_WHOLE;
    } else {
        rec_band_scan(d->bits, d->stride, R.shadow, row, d->height, &first, &end);
    }
    if (first == end && !(flags & (REC_FR_CLUT | REC_FR_TABLES))) {
        R.pending++; // unchanged: a dozen bytes in the file, nothing in the ring yet
        return;
    }
    size_t extra = ((flags & REC_FR_CLUT) ? REC_TABLE_BYTES : 0) + ((flags & REC_FR_DAC) ? REC_TABLE_BYTES : 0) +
                   ((flags & REC_FR_CRT) ? REC_TABLE_BYTES : 0);
    size_t len = sizeof(rec_frame_hdr_t) + extra + (size_t)(end - first) * row;
    uint8_t *p = NULL;
    if (flush_repeats(false) && rec_worker_fit(R.w, len) == 0)
        p = rec_worker_reserve(R.w, REC_K_FRAME, len, AUDIO_ROOM, false);
    if (!p) {
        // The recorder is behind: a repeat instead, the shadow still advances,
        // and the next frame that fits goes whole as a keyframe
        rec_band_take(d->bits, d->stride, R.shadow, row, first, end, false, NULL);
        R.resync = true;
        R.dropped++;
        R.pending++;
        return;
    }
    rec_frame_hdr_t h = {
        .flags = flags,
        .width = d->width,
        .height = d->height,
        .format = d->format,
        .row_bytes = (uint32_t)row,
        .row_first = first,
        .row_end = end,
        .clut_len = R.clut_len,
    };
    memcpy(p, &h, sizeof h);
    uint8_t *q = p + sizeof h;
    if (flags & REC_FR_CLUT) {
        memcpy(q, R.clut, REC_TABLE_BYTES);
        q += REC_TABLE_BYTES;
    }
    if (flags & REC_FR_DAC) {
        memcpy(q, R.dac, REC_TABLE_BYTES);
        q += REC_TABLE_BYTES;
    }
    if (flags & REC_FR_CRT) {
        memcpy(q, R.crt, REC_TABLE_BYTES);
        q += REC_TABLE_BYTES;
    }
    // The one pass: XOR (or whole rows after a resync) into the ring, shadow updated
    rec_band_take(d->bits, d->stride, R.shadow, row, first, end, !(flags & REC_FR_WHOLE), q);
    rec_worker_commit(R.w);
    R.resync = false;
}

// ============================================================================
// Hooks
// ============================================================================

void recorder_frame_boundary(void) {
    // Suspended: the file is closed and nothing is recorded until resumed
    if (!R.active || R.suspended)
        return;
    R.units++;
    // The recorder thread failed: finalise what exists
    rec_stats_t s;
    rec_worker_stats(R.w, &s);
    if (s.err) {
        fail_stop();
        return;
    }
    if (R.units % R.div)
        return;
    capture();
    R.frames++;
    // Repeats go out at least once a second, so the file keeps moving
    if (R.pending >= R.pending_flush)
        flush_repeats(false);
    double now = host_time_ms();
    if (now - R.last_event_ms >= EVENT_MS) {
        R.last_event_ms = now;
        rec_event("recording", NULL);
    }
}

void recorder_audio_push(const int16_t *frames, int nframes, int channels, uint32_t rate, int vol) {
    if (!R.active || R.suspended || !R.arate || nframes <= 0 || channels < 1)
        return;
    // Repeats owed come first, so the record order is the time order; with
    // no room for either, the timeline fills the gap
    if (!flush_repeats(false))
        return;
    size_t bytes = (size_t)nframes * (size_t)channels * sizeof(int16_t);
    uint8_t *p = rec_worker_reserve(R.w, REC_K_AUDIO, sizeof(rec_audio_hdr_t) + bytes, 0, false);
    if (!p)
        return;
    rec_audio_hdr_t h = {
        .rate = rate, .channels = (uint16_t)channels, .vol = (uint16_t)(vol & 7), .frames = (uint32_t)nframes};
    memcpy(p, &h, sizeof h);
    memcpy(p + sizeof h, frames, bytes);
    rec_worker_commit(R.w);
}

// The scheduler's hook (a plain void(void)).
static void frame_hook(void) {
    recorder_frame_boundary();
}

// audio_out's sink.
static void audio_hook(const int16_t *frames, int nframes, int channels, uint32_t rate, int vol) {
    recorder_audio_push(frames, nframes, channels, rate, vol);
}

// ============================================================================
// Start and stop
// ============================================================================

// Write `fmt` into err (err_len bytes) and return -1.
static int start_error(char *err, size_t err_len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int start_error(char *err, size_t err_len, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (err && err_len)
        vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
    return -1;
}

// The default file for a recording with no path: <dir>/<machine>-<timestamp>.mkv.
static int default_path(char *out, size_t cap) {
    if (!s_default_dir)
        return -1;
    const char *model = system_machine_model_id();
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    char leaf[128];
    snprintf(leaf, sizeof leaf, "%s-%s.mkv", model && *model ? model : "machine", stamp);
    return path_join(out, cap, s_default_dir, leaf) ? 0 : -1;
}

int recorder_start(const char *path, char *err, size_t err_len) {
    if (R.active)
        return start_error(err, err_len, "already recording to '%s'", R.paths[R.npaths - 1]);
    if (!system_is_initialized())
        return start_error(err, err_len, "no machine");
    display_t *d = system_display();
    if (!d || !d->bits || !d->width || !d->height || !rec_pix_bpp(d->format))
        return start_error(err, err_len, "the machine has no screen");
    char file[MAX_PATH_LEN];
    if (!path || !*path) {
        if (!s_default_dir_set && !s_default_dir)
            s_default_dir = gs_strdup("/opfs/recordings"); // the core's default is the browser's
        if (default_path(file, sizeof file) < 0)
            return start_error(err, err_len, "a path is required (none is the default on this platform)");
    } else {
        if (strlen(path) >= sizeof file - 16)
            return start_error(err, err_len, "path too long");
        snprintf(file, sizeof file, "%s", path);
    }
    if (!path_has_ext(file, ".mkv"))
        return start_error(err, err_len, "path must end in .mkv (got '%s')", file);
    int mrc = mkdir_parents(file);
    if (mrc < 0)
        return start_error(err, err_len, "cannot create the directory for '%s': %s", file, strerror(-mrc));

    // A fresh recording
    free_paths();
    memset(&R, 0, sizeof R);
    snprintf(R.base, sizeof R.base, "%.*s", (int)(strlen(file) - 4), file);
    R.div = s_set.rate == REC_RATE_QUARTER ? 4 : s_set.rate == REC_RATE_HALF ? 2 : 1;
    R.pending_flush = 60 / R.div; // about a second of output frames
    size_t row = rec_pix_row_bytes(d->format, d->width);
    uint32_t cw = s_set.canvas_w > d->width ? s_set.canvas_w : d->width;
    uint32_t ch = s_set.canvas_h > d->height ? s_set.canvas_h : d->height;
    size_t largest = frame_record_max(ch, rec_pix_row_bytes(REC_PIX_32BPP, cw));
    if (frame_record_max(d->height, row) > largest)
        largest = frame_record_max(d->height, row);
    rec_coder_t *coder = rec_coder_new();
    R.w = coder ? rec_worker_new(coder, ring_size_for(largest), s_set.threaded, s_set.block) : NULL;
    if (!R.w && coder && s_set.threaded) {
        // No thread to be had (a single-threaded build): code inline, the same bytes
        LOG(1, "no recorder thread: coding on the emulator thread");
        R.w = rec_worker_new(coder, ring_size_for(largest), false, s_set.block);
    }
    if (!R.w) {
        if (coder)
            rec_coder_free(coder);
        return start_error(err, err_len, "out of memory, or no thread for the recorder");
    }
    R.active = true;
    if (segment_begin(d->width, d->height, d->format, display_tables(d), d) < 0) {
        char msg[200];
        finish(msg, sizeof msg);
        free_paths();
        return start_error(err, err_len, "could not open '%s'", file);
    }
    // The coder opens the file as it takes the first record: inline that is
    // now; on the thread, a failure arrives with the first frame
    rec_stats_t s;
    rec_worker_stats(R.w, &s);
    if (s.err) {
        char msg[200] = "";
        finish(msg, sizeof msg);
        free_paths();
        return start_error(err, err_len, "%s", msg[0] ? msg : "cannot write the file");
    }
    R.last_event_ms = host_time_ms();
    scheduler_set_frame_hook(frame_hook);
    audio_out_set_record_sink(audio_hook);
    LOG(1, "recording to %s (rate 1/%u, %s, %s)", file, (unsigned)R.div, s_set.threaded ? "threaded" : "inline",
        s_set.block ? "blocking" : "dropping");
    rec_event("started", NULL);
    return 0;
}

int recorder_stop(char ***paths, size_t *n_paths, char *err, size_t err_len) {
    if (paths)
        *paths = NULL;
    if (n_paths)
        *n_paths = 0;
    if (!R.active)
        return start_error(err, err_len, "not recording");
    char msg[200] = "";
    int rc = finish(msg, sizeof msg);
    if (rc)
        rec_event("error", msg[0] ? msg : "the recorder failed");
    rec_event("stopped", NULL);
    // Hand the list over; `path` keeps naming the last file
    if (paths && n_paths) {
        *paths = R.paths;
        *n_paths = R.npaths;
        R.paths = NULL;
        R.npaths = 0;
    } else {
        free_paths();
    }
    return 0;
}

// ============================================================================
// Suspend and resume
// ============================================================================

// The next segment, for the display `d` as it is now (none: the last canvas).
static int segment_next(const display_t *d) {
    uint32_t w = d && d->width ? d->width : R.cw, h = d && d->height ? d->height : R.ch;
    uint32_t fmt = d ? d->format : REC_PIX_8BPP;
    return segment_begin(w, h, fmt, display_tables(d), d);
}

bool recorder_suspended(void) {
    return R.active && R.suspended;
}

int recorder_suspend(char *err, size_t err_len) {
    if (!R.active)
        return start_error(err, err_len, "not recording");
    if (R.suspended)
        return 0;
    // The repeats owed belong to this file; then close it and wait until it is
    flush_repeats(true);
    if (rec_worker_reserve(R.w, REC_K_CLOSE, 0, 0, true))
        rec_worker_commit(R.w);
    rec_worker_sync(R.w);
    rec_stats_t s;
    rec_worker_stats(R.w, &s);
    if (s.err) {
        fail_stop();
        return start_error(err, err_len, "the recorder failed");
    }
    R.suspended = true;
    R.have_mode = false;
    LOG(1, "suspended: %s closed after %llu frames", R.npaths ? R.paths[R.npaths - 1] : "",
        (unsigned long long)R.frames);
    rec_event("suspended", NULL);
    return 0;
}

int recorder_resume(char *err, size_t err_len) {
    if (!R.active)
        return start_error(err, err_len, "not recording");
    if (!R.suspended)
        return 0;
    if (segment_next(system_display()) < 0) {
        fail_stop();
        return start_error(err, err_len, "could not start the next segment");
    }
    R.suspended = false;
    rec_event("segment", NULL);
    return 0;
}

// ============================================================================
// Machine events
// ============================================================================

void recorder_machine_attached(bool restored) {
    // Suspended: the resume opens a segment for whatever machine is there then
    if (!R.active || R.suspended)
        return;
    display_t *d = system_display();
    bool audio_same = !s_set.audio || (audio_out_rate() == R.arate && (!R.arate || audio_out_channels() == R.ach));
    if (restored && d && segment_fits(d->width, d->height, d->format, display_tables(d)) && audio_same) {
        // A checkpoint load that fits: a cut in the same file, sent whole as a keyframe
        R.resync = true;
        R.have_mode = false;
        return;
    }
    // A boot (a different machine is a different recording), or one that does not fit
    if (segment_next(d) < 0) {
        fail_stop();
        return;
    }
    rec_event("segment", NULL);
}

void recorder_machine_gone(void) {
    if (!R.active)
        return;
    char msg[200] = "";
    int rc = finish(msg, sizeof msg);
    if (rc)
        rec_event("error", msg[0] ? msg : "the recorder failed");
    rec_event("stopped", NULL);
}
