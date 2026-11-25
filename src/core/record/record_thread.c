// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// record_thread.c
// The recorder thread: the coder that turns the ring's records into a
// Matroska file (frames rebuilt and passed through the response tables,
// ZMBV, the audio timeline, FLAC, the muxer), and the worker around it --
// the ring, the thread, and the backlog policy.  See record_internal.h.
//
// Nothing here knows the machine: the coder sees records, not displays,
// so the unit tier drives it directly.

#include "record_internal.h"

#include "codec_flac.h"
#include "codec_zmbv.h"
#include "mkv_mux.h"
#include "record_audio.h"
#include "record_pixels.h"
#include "mailbox/mailbox_ring.h"
#include "mailbox/spsc_index.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ============================================================================
// Constants and types
// ============================================================================

#define VBL_PERIOD_NS 16625103ull // one frame-unit (scheduler.h MAC_VBL_PERIOD_NS)

#define CONSUMER_WAIT_MS 200 // the recorder thread re-checks the ring this often when idle

// The coder: the open segment's file and codecs, the guest's current mode
// and picture, and the counters it reports.
struct rec_coder {
    rec_stats_t st; // read by other threads with __atomic loads
    char msg[160];
    bool failed;
    uint64_t bytes_closed; // file bytes of the segments already closed
    // The open segment
    muxer_t *mux;
    uint32_t cw, ch, cls, div; // canvas, class, frame divisor
    uint64_t period_ns; // one output frame
    int bpp; // stored bytes per pixel
    codec_zmbv_enc_t *z;
    uint8_t *zbuf;
    size_t zcap;
    uint64_t k; // frames coded in this segment
    uint32_t changed_since_key;
    // Audio
    bool audio;
    uint32_t arate;
    int ach;
    rec_timeline_t tl;
    bool tl_live; // tl holds a timeline (it may outlive a segment)
    codec_flac_enc_t *flac;
    uint8_t *fbuf;
    size_t fcap;
    int16_t abuf[CODEC_FLAC_BLOCK * 2];
    // The guest's mode and the picture rebuilt from the records
    bool have_mode;
    uint32_t mw, mh, mfmt;
    size_t mrow; // raw row bytes
    uint8_t *raw; // the guest's frame, mh x mrow
    uint8_t *canvas; // the stored frame being built, ch rows of crow bytes
    size_t crow;
    uint32_t x0, y0; // where the mode sits on the canvas
    uint8_t clut[REC_TABLE_BYTES];
    uint32_t clut_len;
    bool have_dac, have_crt;
    uint8_t dac[REC_TABLE_BYTES], crt[REC_TABLE_BYTES];
    uint8_t table[3][256]; // dac then crt, composed
    uint8_t pal[CODEC_ZMBV_PALETTE]; // the palette the stored frames use
    uint8_t border; // the palette index the canvas border is filled with
};

// The worker: the ring, the recorder thread, the full-ring policy.
struct rec_worker {
    rec_coder_t *coder;
    mbx_ring_t ring; // one view for both sides: wr is the producer's, rd the consumer's
    volatile uint32_t ctrl[2]; // HEAD, TAIL
    uint8_t *buf;
    bool threaded;
    bool block; // read by the recorder thread (atomics)
    pthread_t th;
    pthread_mutex_t mu;
    pthread_cond_t cv_work, cv_room;
    bool consumer_waiting, producer_waiting; // under mu
    uint32_t last_kind; // the kind of the record reserved last (the producer's)
};

// ============================================================================
// Coder: counters and failure
// ============================================================================

// Store a counter for other threads.
static void stat_set(uint64_t *field, uint64_t v) {
    __atomic_store_n(field, v, __ATOMIC_RELEASE);
}

// The file bytes so far, published.
static void publish_bytes(rec_coder_t *c) {
    stat_set(&c->st.bytes, c->bytes_closed + (c->mux ? mux_bytes(c->mux) : 0));
}

// Record the first failure: the error code and why.
static int fail(rec_coder_t *c, int err, const char *what) {
    if (!c->failed) {
        c->failed = true;
        snprintf(c->msg, sizeof c->msg, "%s: %s", what, strerror(err < 0 ? -err : EIO));
        __atomic_store_n(&c->st.err, err < 0 ? err : -EIO, __ATOMIC_RELEASE);
    }
    return err < 0 ? err : -EIO;
}

void rec_coder_stats(const rec_coder_t *c, rec_stats_t *out) {
    out->bytes = __atomic_load_n(&c->st.bytes, __ATOMIC_ACQUIRE);
    out->video_bytes = __atomic_load_n(&c->st.video_bytes, __ATOMIC_ACQUIRE);
    out->audio_bytes = __atomic_load_n(&c->st.audio_bytes, __ATOMIC_ACQUIRE);
    out->frames = __atomic_load_n(&c->st.frames, __ATOMIC_ACQUIRE);
    out->keyframes = __atomic_load_n(&c->st.keyframes, __ATOMIC_ACQUIRE);
    out->err = __atomic_load_n(&c->st.err, __ATOMIC_ACQUIRE);
}

const char *rec_coder_message(const rec_coder_t *c) {
    return c->msg;
}

// ============================================================================
// Coder: audio
// ============================================================================

// Code one FLAC frame -- the full block waiting, or (`last`) the short
// final one -- as an audio packet.
static int audio_frame(rec_coder_t *c, bool last) {
    uint64_t first = 0;
    int64_t n = last ? codec_flac_enc_flush(c->flac, c->fbuf, c->fcap, &first)
                     : codec_flac_enc_frame(c->flac, c->fbuf, c->fcap, &first);
    if (n < 0)
        return fail(c, -EIO, "audio encoder");
    if (n == 0)
        return 0;
    // The frame's samples: the encoder's count since the frame's first
    uint64_t frames = codec_flac_enc_samples(c->flac) - first;
    uint64_t pts = first * 1000000000ull / c->arate;
    uint64_t end = (first + frames) * 1000000000ull / c->arate;
    int rc = mux_packet(c->mux, MUX_TRACK_AUDIO, pts, end - pts, true, c->fbuf, (size_t)n);
    if (rc < 0)
        return fail(c, rc, "write");
    __atomic_add_fetch(&c->st.audio_bytes, (uint64_t)n, __ATOMIC_RELEASE);
    return 0;
}

// Code every whole FLAC block the timeline has made ready (and, `final`,
// the short last one) as audio packets.
static int audio_pump(rec_coder_t *c, bool final) {
    if (!c->audio || !c->flac)
        return 0;
    size_t n;
    while ((n = rec_tl_read(&c->tl, c->abuf, CODEC_FLAC_BLOCK)) > 0) {
        // The encoder takes up to the end of its block: code it and go on
        size_t off = 0;
        while (off < n) {
            off += codec_flac_enc_push(c->flac, c->abuf + off * (size_t)c->ach, n - off);
            if (codec_flac_enc_ready(c->flac)) {
                int rc = audio_frame(c, false);
                if (rc < 0)
                    return rc;
            }
        }
    }
    return final ? audio_frame(c, true) : 0;
}

// The audio target at the end of `frames` output frames of this segment.
static uint64_t audio_target(const rec_coder_t *c, uint64_t frames) {
    return rec_tl_target(frames * c->div, VBL_PERIOD_NS, c->arate);
}

// ============================================================================
// Coder: segments
// ============================================================================

// Free the open segment's codecs (the timeline is kept for the next).
static void segment_free(rec_coder_t *c) {
    codec_zmbv_enc_free(c->z);
    c->z = NULL;
    free(c->zbuf);
    c->zbuf = NULL;
    codec_flac_enc_free(c->flac);
    c->flac = NULL;
    free(c->fbuf);
    c->fbuf = NULL;
    free(c->canvas);
    c->canvas = NULL;
    c->have_mode = false;
}

// Finalise the open segment: audio to the end of its last frame (`ending`:
// the recording ends, so surplus samples go), then the file.
static int segment_close(rec_coder_t *c, bool ending) {
    if (!c->mux)
        return 0;
    int rc = 0;
    if (c->audio && !c->failed) {
        if (rec_tl_finish(&c->tl, audio_target(c, c->k), ending) < 0)
            rc = fail(c, -ENOMEM, "audio");
        else
            rc = audio_pump(c, true);
    }
    uint64_t size = mux_bytes(c->mux);
    int crc = mux_close(c->mux);
    c->mux = NULL;
    if (crc < 0 && !rc)
        rc = fail(c, crc, "close");
    c->bytes_closed += size;
    publish_bytes(c);
    segment_free(c);
    if (c->tl_live) {
        // The next segment's audio carries on from here: its clock starts at 0
        uint64_t base = c->tl.emitted;
        c->tl.emitted = 0;
        c->tl.read_limit = 0;
        c->tl.last_push_target = c->tl.last_push_target > base ? c->tl.last_push_target - base : 0;
        if (ending) {
            rec_tl_free(&c->tl);
            c->tl_live = false;
        }
    }
    return rc;
}

// Open the segment a SEGMENT record describes.
static int segment_open(rec_coder_t *c, const uint8_t *p, size_t len) {
    rec_seg_hdr_t h;
    if (len < sizeof h)
        return fail(c, -EINVAL, "segment record");
    memcpy(&h, p, sizeof h);
    if (len < sizeof h + h.path_len || !h.canvas_w || !h.canvas_h || !h.div)
        return fail(c, -EINVAL, "segment record");
    char path[1024];
    if (h.path_len >= sizeof path)
        return fail(c, -ENAMETOOLONG, "segment path");
    memcpy(path, p + sizeof h, h.path_len);
    path[h.path_len] = '\0';

    c->cw = h.canvas_w;
    c->ch = h.canvas_h;
    c->cls = h.cls;
    c->div = h.div;
    c->period_ns = (uint64_t)h.div * VBL_PERIOD_NS;
    c->bpp = rec_pix_class_bytes((int)h.cls);
    c->k = 0;
    c->changed_since_key = 0;
    c->crow = (size_t)c->cw * (size_t)c->bpp;
    c->canvas = calloc((size_t)c->ch, c->crow);
    int zfmt = h.cls == REC_CLASS_INDEXED ? CODEC_ZMBV_FMT_8BPP
               : h.cls == REC_CLASS_D15   ? CODEC_ZMBV_FMT_15BPP
               : h.cls == REC_CLASS_D16   ? CODEC_ZMBV_FMT_16BPP
                                          : CODEC_ZMBV_FMT_32BPP;
    int zerr = 0;
    c->z = codec_zmbv_enc_new((int)c->cw, (int)c->ch, zfmt, 0, 0, REC_LEVEL_NORMAL, &zerr);
    if (c->z) {
        c->zcap = codec_zmbv_enc_bound(c->z);
        c->zbuf = malloc(c->zcap);
    }
    if (!c->canvas || !c->z || !c->zbuf)
        return fail(c, -ENOMEM, "video encoder");

    // The audio stream: a timeline of the same shape carries on; another starts fresh
    bool want_audio = h.audio_rate && (h.audio_channels == 1 || h.audio_channels == 2);
    if (c->tl_live && (!want_audio || c->tl.rate != h.audio_rate || c->tl.channels != (int)h.audio_channels)) {
        rec_tl_free(&c->tl);
        c->tl_live = false;
    }
    c->audio = want_audio;
    mux_audio_t ma;
    if (want_audio) {
        c->arate = h.audio_rate;
        c->ach = (int)h.audio_channels;
        if (!c->tl_live) {
            uint32_t fs = (uint32_t)rec_tl_target(1, VBL_PERIOD_NS, c->arate) + 1;
            if (rec_tl_init(&c->tl, c->arate, c->ach, fs) < 0)
                return fail(c, -ENOMEM, "audio");
            c->tl_live = true;
        }
        int ferr = 0;
        c->flac = codec_flac_enc_new(c->arate, c->ach, &ferr);
        if (c->flac) {
            c->fcap = codec_flac_enc_bound(c->flac);
            c->fbuf = malloc(c->fcap);
        }
        if (!c->flac || !c->fbuf)
            return fail(c, -ENOMEM, "audio encoder");
        memset(&ma, 0, sizeof ma);
        ma.sample_rate = c->arate;
        ma.channels = c->ach;
        ma.bits_per_sample = 16;
        codec_flac_enc_streaminfo(c->flac, ma.streaminfo);
    }
    mux_video_t mv = {
        .width = (int)c->cw,
        .height = (int)c->ch,
        .display_width = (int)h.display_w,
        .display_height = (int)h.display_h,
        .frame_period_ns = c->period_ns,
        .bits_per_pixel = c->bpp * 8,
    };
    int err = 0;
    c->mux = mkv_mux_open(path, &mv, want_audio ? &ma : NULL, &err);
    if (!c->mux)
        return fail(c, err, "open");
    publish_bytes(c);
    return 0;
}

// ============================================================================
// Coder: frames
// ============================================================================

// Take the guest's mode `h`: a fresh raw copy, its place on the canvas.
static int mode_set(rec_coder_t *c, const rec_frame_hdr_t *h) {
    free(c->raw);
    c->raw = calloc(h->height ? h->height : 1, h->row_bytes ? h->row_bytes : 1);
    if (!c->raw)
        return -1;
    c->mw = h->width;
    c->mh = h->height;
    c->mfmt = h->format;
    c->mrow = h->row_bytes;
    // Centred; a mode larger than the canvas (never sent) would be cropped
    c->x0 = c->mw < c->cw ? (c->cw - c->mw) / 2 : 0;
    c->y0 = c->mh < c->ch ? (c->ch - c->mh) / 2 : 0;
    c->have_mode = true;
    return 0;
}

// True when the mode leaves canvas showing around it.
static bool has_border(const rec_coder_t *c) {
    return c->mw < c->cw || c->mh < c->ch;
}

// Fill the whole canvas with the border colour (black).
static void canvas_clear(rec_coder_t *c) {
    if (c->cls == REC_CLASS_INDEXED)
        memset(c->canvas, c->border, (size_t)c->ch * c->crow);
    else
        memset(c->canvas, 0, (size_t)c->ch * c->crow); // black in every direct format
}

// Convert raw rows [a, b) onto the canvas.
static void convert_rows(rec_coder_t *c, uint32_t a, uint32_t b) {
    uint32_t w = c->mw < c->cw ? c->mw : c->cw;
    for (uint32_t y = a; y < b && c->y0 + y < c->ch; y++) {
        uint8_t *dst = c->canvas + (size_t)(c->y0 + y) * c->crow + (size_t)c->x0 * (size_t)c->bpp;
        rec_row_convert(c->raw + (size_t)y * c->mrow, c->mfmt, w, (int)c->cls, (const uint8_t(*)[256])c->table, c->pal,
                        dst);
    }
}

// Emit the audio up to frame k's end, then code and queue frame `f`.
static int frame_out(rec_coder_t *c, const codec_zmbv_frame_t *f) {
    if (c->audio) {
        if (rec_tl_boundary(&c->tl, audio_target(c, c->k + 1)) < 0)
            return fail(c, -ENOMEM, "audio");
        int rc = audio_pump(c, false);
        if (rc < 0)
            return rc;
    }
    codec_zmbv_kind_t kind;
    int64_t n = codec_zmbv_enc_frame(c->z, f, c->zbuf, c->zcap, &kind);
    if (n < 0)
        return fail(c, -EIO, "video encoder");
    if (kind == CODEC_ZMBV_KEYFRAME) {
        c->changed_since_key = 0;
        __atomic_add_fetch(&c->st.keyframes, 1, __ATOMIC_RELEASE);
    } else if (kind == CODEC_ZMBV_INTER) {
        c->changed_since_key++;
    }
    int rc =
        mux_packet(c->mux, MUX_TRACK_VIDEO, c->k * c->period_ns, 0, kind == CODEC_ZMBV_KEYFRAME, c->zbuf, (size_t)n);
    if (rc < 0)
        return fail(c, rc, "write");
    c->k++;
    __atomic_add_fetch(&c->st.video_bytes, (uint64_t)n, __ATOMIC_RELEASE);
    __atomic_add_fetch(&c->st.frames, 1, __ATOMIC_RELEASE);
    return 0;
}

// Code `count` frames identical to the last.
static int frame_repeat(rec_coder_t *c, uint32_t count) {
    codec_zmbv_frame_t f = {0};
    for (uint32_t i = 0; i < count; i++) {
        int rc = frame_out(c, &f);
        if (rc < 0)
            return rc;
    }
    return 0;
}

// Code one FRAME record.
static int frame_record(rec_coder_t *c, const uint8_t *p, size_t len) {
    rec_frame_hdr_t h;
    if (len < sizeof h)
        return fail(c, -EINVAL, "frame record");
    memcpy(&h, p, sizeof h);
    size_t at = sizeof h;
    bool tables_changed = false, clut_changed = false;
    // The CLUT and the tables, when they came with the frame
    if (h.flags & REC_FR_CLUT) {
        if (len < at + REC_TABLE_BYTES)
            return fail(c, -EINVAL, "frame record");
        memcpy(c->clut, p + at, REC_TABLE_BYTES);
        c->clut_len = h.clut_len > 256 ? 256 : h.clut_len;
        at += REC_TABLE_BYTES;
        clut_changed = true;
    }
    if (h.flags & REC_FR_TABLES) {
        c->have_dac = (h.flags & REC_FR_DAC) != 0;
        c->have_crt = (h.flags & REC_FR_CRT) != 0;
        if (len < at + (c->have_dac + c->have_crt) * (size_t)REC_TABLE_BYTES)
            return fail(c, -EINVAL, "frame record");
        if (c->have_dac) {
            memcpy(c->dac, p + at, REC_TABLE_BYTES);
            at += REC_TABLE_BYTES;
        }
        if (c->have_crt) {
            memcpy(c->crt, p + at, REC_TABLE_BYTES);
            at += REC_TABLE_BYTES;
        }
        rec_table_compose(c->table, c->have_dac ? c->dac : NULL, c->have_crt ? c->crt : NULL);
        tables_changed = true;
    }
    size_t band_rows = h.row_end > h.row_first ? h.row_end - h.row_first : 0;
    if (h.row_end > h.height || len < at + band_rows * h.row_bytes)
        return fail(c, -EINVAL, "frame record");
    const uint8_t *band = p + at;

    bool full = false; // the whole canvas is rebuilt from the raw frame
    bool key = (h.flags & REC_FR_WHOLE) != 0;
    if ((h.flags & REC_FR_MODE) || !c->have_mode || h.width != c->mw || h.height != c->mh || h.format != c->mfmt ||
        h.row_bytes != c->mrow) {
        if (mode_set(c, &h) < 0)
            return fail(c, -ENOMEM, "frame");
        full = key = true;
    }
    // The palette the stored frames carry (indexed), or the per-pixel table
    const uint8_t *pal_arg = NULL;
    if (rec_pix_indexed(c->mfmt) && (clut_changed || tables_changed || full)) {
        uint8_t pal[CODEC_ZMBV_PALETTE];
        rec_palette_build(pal, c->mfmt, c->clut, c->clut_len, (const uint8_t(*)[256])c->table);
        if (memcmp(pal, c->pal, sizeof pal) != 0 || full) {
            memcpy(c->pal, pal, sizeof pal);
            if (c->cls == REC_CLASS_INDEXED) {
                pal_arg = c->pal;
                // The border follows black's index
                uint8_t b = rec_palette_black(c->pal);
                if (has_border(c) && b != c->border)
                    full = true;
                c->border = b;
            } else {
                full = true; // an RGB recording looks every pixel up again
            }
        }
    }
    if (tables_changed && !rec_pix_indexed(c->mfmt))
        full = key = true; // direct colour: every pixel goes through the new table
    if (pal_arg == NULL && c->cls == REC_CLASS_INDEXED && full)
        pal_arg = c->pal;

    // The raw frame takes the band
    for (size_t r = 0; r < band_rows; r++) {
        uint8_t *dst = c->raw + (size_t)(h.row_first + r) * c->mrow;
        const uint8_t *src = band + r * h.row_bytes;
        if (h.flags & REC_FR_WHOLE) {
            memcpy(dst, src, c->mrow);
        } else {
            for (size_t i = 0; i < c->mrow; i++)
                dst[i] ^= src[i];
        }
    }

    codec_zmbv_frame_t f = {0};
    f.palette = pal_arg;
    if (full) {
        canvas_clear(c);
        convert_rows(c, 0, c->mh);
        f.rows = c->canvas;
        f.stride = c->crow;
        f.row_first = 0;
        f.row_end = (int)c->ch;
    } else if (band_rows && c->cls == REC_CLASS_INDEXED && c->mfmt == REC_PIX_8BPP && !has_border(c) &&
               !(h.flags & REC_FR_WHOLE)) {
        // 8 bpp filling the canvas: the band's XOR is ZMBV's delta as it is
        f.rows = band;
        f.stride = h.row_bytes;
        f.is_xor = true;
        f.row_first = (int)h.row_first;
        f.row_end = (int)h.row_end;
    } else if (band_rows) {
        convert_rows(c, h.row_first, h.row_end);
        uint32_t a = c->y0 + h.row_first, b = c->y0 + h.row_end;
        if (b > c->ch)
            b = c->ch;
        f.rows = c->canvas + (size_t)a * c->crow;
        f.stride = c->crow;
        f.row_first = (int)a;
        f.row_end = (int)b;
    }
    // A keyframe now and then, but only for a frame that changed
    bool changed = f.row_end > f.row_first || f.palette;
    f.keyframe = key || (changed && c->changed_since_key >= REC_KEY_INTERVAL);
    return frame_out(c, &f);
}

// ============================================================================
// Coder: lifecycle and dispatch
// ============================================================================

rec_coder_t *rec_coder_new(void) {
    rec_coder_t *c = calloc(1, sizeof(*c));
    if (c)
        rec_table_compose(c->table, NULL, NULL);
    return c;
}

int rec_coder_record(rec_coder_t *c, uint32_t kind, const uint8_t *p, size_t len, int level) {
    // After a failure the file is already finalised: nothing more is written
    if (c->failed)
        return c->st.err;
    int rc = 0;
    switch (kind) {
    case REC_K_SEGMENT:
        rc = segment_close(c, false);
        if (rc >= 0)
            rc = segment_open(c, p, len);
        break;
    case REC_K_FRAME:
        if (!c->mux)
            return fail(c, -EINVAL, "frame before segment");
        codec_zmbv_enc_set_level(c->z, level);
        rc = frame_record(c, p, len);
        break;
    case REC_K_REPEAT: {
        rec_repeat_hdr_t h;
        if (!c->mux || len < sizeof h)
            return fail(c, -EINVAL, "repeat record");
        memcpy(&h, p, sizeof h);
        rc = frame_repeat(c, h.count);
        break;
    }
    case REC_K_AUDIO: {
        rec_audio_hdr_t h;
        if (len < sizeof h)
            return fail(c, -EINVAL, "audio record");
        memcpy(&h, p, sizeof h);
        if (len < sizeof h + (size_t)h.frames * h.channels * sizeof(int16_t))
            return fail(c, -EINVAL, "audio record");
        if (c->audio && c->tl_live) {
            int16_t *s = (int16_t *)(uintptr_t)(p + sizeof h); // 8-aligned in the ring
            if (rec_tl_push(&c->tl, s, h.frames, h.channels, h.rate, h.vol) < 0)
                rc = fail(c, -ENOMEM, "audio");
        }
        break;
    }
    case REC_K_STOP:
        rc = segment_close(c, true);
        break;
    case REC_K_CLOSE:
        // The file ends here; the timeline carries into the next SEGMENT
        rc = segment_close(c, false);
        break;
    default:
        break;
    }
    // A failure finalises what exists: the file plays up to it
    if (rc < 0 && c->mux) {
        uint64_t size = mux_bytes(c->mux);
        mux_close(c->mux);
        c->mux = NULL;
        c->bytes_closed += size;
        segment_free(c);
    }
    publish_bytes(c);
    return rc;
}

void rec_coder_free(rec_coder_t *c) {
    if (!c)
        return;
    if (c->mux && !c->failed)
        segment_close(c, true);
    if (c->mux)
        mux_close(c->mux);
    c->mux = NULL;
    segment_free(c);
    if (c->tl_live)
        rec_tl_free(&c->tl);
    free(c->raw);
    free(c);
}

// ============================================================================
// Worker: the ring
// ============================================================================

// The DEFLATE effort for the backlog: `used` bytes of the ring waiting.
static int level_for(const rec_worker_t *w, uint32_t used) {
    if (__atomic_load_n(&w->block, __ATOMIC_ACQUIRE))
        return REC_LEVEL_NORMAL; // blocking keeps the output a function of the records
    if (used > w->ring.size / 4 * 3)
        return REC_LEVEL_STORED;
    if (used > w->ring.size / 2)
        return REC_LEVEL_FAST;
    return REC_LEVEL_NORMAL;
}

// Bytes published and not yet consumed.
static uint32_t ring_used(rec_worker_t *w) {
    return spsc_avail(mbx_load(w->ctrl, 0), w->ring.rd);
}

// Code every record waiting (the inline worker's drain, on the caller's thread).
static void drain_inline(rec_worker_t *w) {
    mbx_rec_t rec;
    while (mbx_next(&w->ring, mbx_load(w->ctrl, 0), &rec) == 1) {
        if (rec.kind != MBX_R_PAD)
            rec_coder_record(w->coder, rec.kind, mbx_rec_payload(&w->ring, &rec), rec.len - MBX_HDR_BYTES,
                             REC_LEVEL_NORMAL);
        mbx_consume(&w->ring, &rec);
    }
}

// The recorder thread: take each record in turn, code it, give its bytes back.
static void *worker_main(void *arg) {
    rec_worker_t *w = arg;
    for (;;) {
        mbx_rec_t rec;
        pthread_mutex_lock(&w->mu);
        int got;
        while ((got = mbx_next(&w->ring, mbx_load(w->ctrl, 0), &rec)) == 0) {
            // Idle: wait for a record, or the timeout
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += CONSUMER_WAIT_MS * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            w->consumer_waiting = true;
            pthread_cond_timedwait(&w->cv_work, &w->mu, &ts);
            w->consumer_waiting = false;
        }
        uint32_t used = ring_used(w);
        pthread_mutex_unlock(&w->mu);
        if (got < 0)
            break; // corrupt framing: the producer is broken, nothing more is trusted
        uint32_t kind = rec.kind;
        if (kind != MBX_R_PAD)
            rec_coder_record(w->coder, kind, mbx_rec_payload(&w->ring, &rec), rec.len - MBX_HDR_BYTES,
                             level_for(w, used));
        pthread_mutex_lock(&w->mu);
        mbx_consume(&w->ring, &rec);
        if (w->producer_waiting)
            pthread_cond_signal(&w->cv_room);
        pthread_mutex_unlock(&w->mu);
        if (kind == REC_K_STOP)
            break;
    }
    return NULL;
}

rec_worker_t *rec_worker_new(rec_coder_t *coder, uint32_t ring_bytes, bool threaded, bool block) {
    if (!coder || !spsc_size_ok(ring_bytes))
        return NULL;
    rec_worker_t *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->buf = malloc(ring_bytes);
    if (!w->buf) {
        free(w);
        return NULL;
    }
    mbx_ring_init(&w->ring, w->ctrl, 0, 1, w->buf, ring_bytes);
    w->coder = coder;
    w->threaded = threaded;
    w->block = block;
    pthread_mutex_init(&w->mu, NULL);
    pthread_cond_init(&w->cv_work, NULL);
    pthread_cond_init(&w->cv_room, NULL);
    if (threaded && pthread_create(&w->th, NULL, worker_main, w) != 0) {
        pthread_mutex_destroy(&w->mu);
        pthread_cond_destroy(&w->cv_work);
        pthread_cond_destroy(&w->cv_room);
        free(w->buf);
        free(w);
        return NULL;
    }
    return w;
}

void rec_worker_set_block(rec_worker_t *w, bool block) {
    __atomic_store_n(&w->block, block, __ATOMIC_RELEASE);
}

// True when a record of `total` ring bytes (header included) plus `extra`
// free bytes fit now.
static bool room_for(rec_worker_t *w, uint32_t total, size_t extra) {
    uint32_t room = spsc_room(w->ring.size, w->ring.wr, mbx_load(w->ctrl, 1));
    // A PAD before the record can cost up to its own size again
    return (uint64_t)room >= (uint64_t)total * 2 + extra;
}

uint8_t *rec_worker_reserve(rec_worker_t *w, uint32_t kind, size_t len, size_t reserve_room, bool must) {
    uint32_t total = MBX_PAD8(MBX_HDR_BYTES + len);
    if ((uint64_t)MBX_HDR_BYTES + len > w->ring.size / 2)
        return NULL; // rec_worker_fit first
    for (;;) {
        if (room_for(w, total, reserve_room)) {
            uint32_t at = mbx_reserve(&w->ring, kind, (uint32_t)(MBX_HDR_BYTES + len));
            if (at != UINT32_MAX) {
                w->last_kind = kind;
                return mbx_payload(&w->ring, at);
            }
        }
        bool wait = must || __atomic_load_n(&w->block, __ATOMIC_ACQUIRE);
        if (!wait || !w->threaded)
            return NULL;
        // Blocked: wake the recorder thread, then wait for it to give bytes back
        pthread_mutex_lock(&w->mu);
        if (w->consumer_waiting)
            pthread_cond_signal(&w->cv_work);
        while (!room_for(w, total, reserve_room)) {
            w->producer_waiting = true;
            pthread_cond_wait(&w->cv_room, &w->mu);
            w->producer_waiting = false;
        }
        pthread_mutex_unlock(&w->mu);
    }
}

void rec_worker_commit(rec_worker_t *w) {
    mbx_publish(&w->ring);
    if (!w->threaded) {
        drain_inline(w);
        return;
    }
    // The small records (audio, repeats) wait for the next frame, a filling
    // ring or the recorder thread's own timer: a wake-up per push would cost
    // the emulator thread more than the copy
    bool urgent = w->last_kind == REC_K_FRAME || w->last_kind == REC_K_SEGMENT || w->last_kind == REC_K_STOP ||
                  w->last_kind == REC_K_CLOSE || spsc_avail(w->ring.wr, mbx_load(w->ctrl, 1)) >= w->ring.size / 16;
    if (!urgent)
        return;
    pthread_mutex_lock(&w->mu);
    if (w->consumer_waiting)
        pthread_cond_signal(&w->cv_work);
    pthread_mutex_unlock(&w->mu);
}

int rec_worker_fit(rec_worker_t *w, size_t len) {
    if ((uint64_t)MBX_HDR_BYTES + len <= w->ring.size / 2)
        return 0;
    uint32_t size = w->ring.size;
    while ((uint64_t)MBX_HDR_BYTES + len > size / 2) {
        if (size >= (1u << 30))
            return -1;
        size *= 2;
    }
    uint8_t *nb = malloc(size);
    if (!nb)
        return -1;
    // The ring changes only while it is empty: the recorder thread is then
    // idle, waiting under the lock it takes before reading a record
    pthread_mutex_lock(&w->mu);
    while (ring_used(w) != 0) {
        w->producer_waiting = true;
        pthread_cond_wait(&w->cv_room, &w->mu);
        w->producer_waiting = false;
    }
    free(w->buf);
    w->buf = nb;
    w->ring.buf = nb;
    w->ring.size = size;
    pthread_mutex_unlock(&w->mu);
    return 0;
}

void rec_worker_sync(rec_worker_t *w) {
    if (!w->threaded) {
        drain_inline(w);
        return;
    }
    // The recorder thread gives each record back only once it is coded, so
    // an empty ring means everything published has been
    pthread_mutex_lock(&w->mu);
    if (w->consumer_waiting)
        pthread_cond_signal(&w->cv_work);
    while (ring_used(w) != 0) {
        w->producer_waiting = true;
        pthread_cond_wait(&w->cv_room, &w->mu);
        w->producer_waiting = false;
    }
    pthread_mutex_unlock(&w->mu);
}

void rec_worker_stats(rec_worker_t *w, rec_stats_t *out) {
    rec_coder_stats(w->coder, out);
}

const char *rec_worker_message(rec_worker_t *w) {
    return rec_coder_message(w->coder);
}

void rec_worker_free(rec_worker_t *w, rec_stats_t *final, char *msg, size_t msg_cap) {
    if (!w)
        return;
    if (w->threaded)
        pthread_join(w->th, NULL);
    else
        drain_inline(w);
    // Freeing finalises a file no STOP reached; the counters are read after
    rec_coder_t *c = w->coder;
    if (c->mux && !c->failed)
        segment_close(c, true);
    if (final)
        rec_coder_stats(c, final);
    if (msg && msg_cap)
        snprintf(msg, msg_cap, "%s", c->msg);
    rec_coder_free(c);
    pthread_mutex_destroy(&w->mu);
    pthread_cond_destroy(&w->cv_work);
    pthread_cond_destroy(&w->cv_room);
    free(w->buf);
    free(w);
}
