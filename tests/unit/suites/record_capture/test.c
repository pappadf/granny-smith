// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The recorder's machine-free parts.  The audio timeline: an hour of a
// chip at 22,254.54 Hz into a stream declared 22,255 Hz stays inside the
// window, is corrected a held sample at a time, never dropped, and ends
// exactly on emulated time; a silent gap is held, so the sound after it
// starts at its own sample; a fast chip's surplus is dropped; the volume
// ramps across the push that changes it; another rate is resampled and
// stereo averaged into mono.  The pixel work: the changed band and its
// XOR, sub-byte indices widened, 16 bpp swapped, the response tables
// through the palette and per pixel.  The coder: records through the ring,
// inline and on a thread with blocking, give byte-identical files whose
// frames and sound decode to what went in, across a mode change onto a
// larger canvas; a full ring refuses a record when the policy is to drop.

#include "codec_flac.h"
#include "codec_zmbv.h"
#include "test_assert.h"
#include "record/mkv_read.h"
#include "record/record_audio.h"
#include "record/record_internal.h"
#include "record/record_pixels.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#ifndef TEST_OUT_DIR
#define TEST_OUT_DIR "."
#endif

#define PERIOD_NS 16625103ull // one frame-unit
#define RATE      22255u

// ============================================================================
// Helpers
// ============================================================================

// A path under the build directory.
static void out_path(char *buf, size_t n, const char *name) {
    snprintf(buf, n, "%s/record_capture_%s", TEST_OUT_DIR, name);
}

// Read every ready frame of a mono timeline into `out` (cap frames) from `at`.
static size_t drain(rec_timeline_t *t, int16_t *out, size_t at, size_t cap) {
    size_t n = rec_tl_ready(t);
    if (at + n > cap)
        n = cap - at;
    return rec_tl_read(t, out + at, n);
}

// ============================================================================
// Audio timeline
// ============================================================================

TEST(timeline_hour_slow_chip) {
    rec_timeline_t t;
    uint32_t fs = (uint32_t)rec_tl_target(1, PERIOD_NS, RATE) + 1;
    ASSERT_EQ_INT(0, rec_tl_init(&t, RATE, 1, fs));
    uint64_t units = 3600ull * 1000000000ull / PERIOD_NS; // an hour of frame-units
    uint64_t pushed = 0, read = 0, prev_held = 0;
    int16_t buf[4096];
    for (uint64_t k = 1; k <= units; k++) {
        // The chip's samples by the end of frame-unit k, at its true rate
        uint64_t due = (uint64_t)((double)k * (double)PERIOD_NS * 22254.54 / 1e9);
        size_t n = (size_t)(due - pushed);
        for (size_t i = 0; i < n; i++)
            buf[i] = (int16_t)((pushed + i) & 0x3FFF);
        ASSERT_EQ_INT(0, rec_tl_push(&t, buf, n, 1, RATE, 7));
        pushed = due;
        uint64_t target = rec_tl_target(k, PERIOD_NS, RATE);
        ASSERT_EQ_INT(0, rec_tl_boundary(&t, target));
        // Inside the window at every boundary, corrected a sample at a time
        int64_t lead = rec_tl_lead(&t, target);
        ASSERT_TRUE(lead >= -(int64_t)rec_tl_window(&t) && lead <= (int64_t)rec_tl_window(&t));
        if (k > 1)
            ASSERT_TRUE(t.held - prev_held <= 1);
        prev_held = t.held;
        size_t r;
        while ((r = rec_tl_read(&t, buf, 4096)) > 0)
            read += r;
    }
    uint64_t target = rec_tl_target(units, PERIOD_NS, RATE);
    ASSERT_EQ_INT(0, (int)t.dropped); // a slow chip is never cut
    ASSERT_EQ_INT(0, rec_tl_finish(&t, target, true));
    size_t r;
    while ((r = rec_tl_read(&t, buf, 4096)) > 0)
        read += r;
    // The stream ends exactly on emulated time: every missing sample was held
    ASSERT_TRUE(read == target);
    ASSERT_TRUE(t.held == target - pushed);
    // 0.46 Hz short for an hour: about 1,656 samples held in all
    ASSERT_TRUE(t.held > 1600 && t.held < 1720);
    fprintf(stderr, "  hour: %llu frame-units, %llu samples, %llu held, window %u\n", (unsigned long long)units,
            (unsigned long long)target, (unsigned long long)t.held, rec_tl_window(&t));
    rec_tl_free(&t);
}

TEST(timeline_gap_is_held) {
    rec_timeline_t t;
    uint32_t fs = (uint32_t)rec_tl_target(1, PERIOD_NS, RATE) + 1;
    ASSERT_EQ_INT(0, rec_tl_init(&t, RATE, 1, fs));
    static int16_t out[200000];
    size_t got = 0;
    uint64_t pushed = 0;
    int16_t buf[1024];
    uint64_t resume_at = 0;
    for (uint64_t k = 1; k <= 200; k++) {
        uint64_t target = rec_tl_target(k, PERIOD_NS, RATE);
        bool silent = k > 60 && k <= 90;
        if (!silent) {
            // A chip in step with time: what the frame-unit owes, in two pushes
            size_t n = (size_t)(target - pushed);
            int16_t v = (int16_t)(k <= 60 ? 100 : 1000);
            for (size_t i = 0; i < n; i++)
                buf[i] = v;
            if (k == 91)
                resume_at = rec_tl_target(90, PERIOD_NS, RATE);
            ASSERT_EQ_INT(0, rec_tl_push(&t, buf, n / 2, 1, RATE, 7));
            ASSERT_EQ_INT(0, rec_tl_push(&t, buf, n - n / 2, 1, RATE, 7));
            pushed = target;
        } else {
            pushed = target; // the next push owes only its own frame-unit
        }
        ASSERT_EQ_INT(0, rec_tl_boundary(&t, target));
        got += drain(&t, out, got, sizeof out / sizeof out[0]);
    }
    uint64_t end = rec_tl_target(200, PERIOD_NS, RATE);
    ASSERT_EQ_INT(0, rec_tl_finish(&t, end, true));
    got += drain(&t, out, got, sizeof out / sizeof out[0]);
    ASSERT_TRUE(got == end);
    // The gap holds the last value; the sound after it starts at its own sample
    size_t first_new = 0;
    while (first_new < got && out[first_new] != 1000)
        first_new++;
    ASSERT_TRUE(first_new == resume_at);
    for (size_t i = (size_t)rec_tl_target(60, PERIOD_NS, RATE); i < first_new; i++)
        ASSERT_EQ_INT(100, out[i]);
    ASSERT_EQ_INT(0, (int)t.dropped);
    rec_tl_free(&t);
}

TEST(timeline_start_silent) {
    // Nothing pushed for the first frames: the first sound is not displaced
    rec_timeline_t t;
    ASSERT_EQ_INT(0, rec_tl_init(&t, RATE, 1, 371));
    static int16_t out[50000];
    size_t got = 0;
    int16_t buf[1024];
    for (uint64_t k = 1; k <= 40; k++) {
        uint64_t target = rec_tl_target(k, PERIOD_NS, RATE);
        if (k >= 2) {
            size_t n = (size_t)(target - rec_tl_target(k - 1, PERIOD_NS, RATE));
            for (size_t i = 0; i < n; i++)
                buf[i] = 500;
            ASSERT_EQ_INT(0, rec_tl_push(&t, buf, n, 1, RATE, 7));
        }
        ASSERT_EQ_INT(0, rec_tl_boundary(&t, target));
        got += drain(&t, out, got, sizeof out / sizeof out[0]);
    }
    ASSERT_EQ_INT(0, rec_tl_finish(&t, rec_tl_target(40, PERIOD_NS, RATE), true));
    got += drain(&t, out, got, sizeof out / sizeof out[0]);
    size_t first = 0;
    while (first < got && out[first] != 500)
        first++;
    ASSERT_TRUE(first == rec_tl_target(1, PERIOD_NS, RATE));
    rec_tl_free(&t);
}

TEST(timeline_surplus_dropped) {
    rec_timeline_t t;
    ASSERT_EQ_INT(0, rec_tl_init(&t, RATE, 1, 371));
    uint64_t pushed = 0;
    int16_t buf[1024] = {0};
    uint64_t units = 60 * 60; // a minute
    for (uint64_t k = 1; k <= units; k++) {
        // A chip 0.6 % fast
        uint64_t due = (uint64_t)((double)k * (double)PERIOD_NS * 22400.0 / 1e9);
        ASSERT_EQ_INT(0, rec_tl_push(&t, buf, (size_t)(due - pushed), 1, RATE, 7));
        pushed = due;
        uint64_t target = rec_tl_target(k, PERIOD_NS, RATE);
        ASSERT_EQ_INT(0, rec_tl_boundary(&t, target));
        int64_t lead = rec_tl_lead(&t, target);
        ASSERT_TRUE(lead <= (int64_t)rec_tl_window(&t));
        while (rec_tl_read(&t, buf, 1024) > 0) {
        }
    }
    ASSERT_TRUE(t.dropped > 7000);
    ASSERT_EQ_INT(0, (int)t.held);
    rec_tl_free(&t);
}

TEST(timeline_volume_ramp) {
    rec_timeline_t t;
    ASSERT_EQ_INT(0, rec_tl_init(&t, RATE, 1, 371));
    int16_t s[8];
    for (int i = 0; i < 8; i++)
        s[i] = 7000;
    ASSERT_EQ_INT(0, rec_tl_push(&t, s, 8, 1, RATE, 7)); // full volume: the samples as they are
    ASSERT_EQ_INT(0, rec_tl_push(&t, s, 8, 1, RATE, 0)); // 7 -> 0 across this push
    ASSERT_EQ_INT(0, rec_tl_push(&t, s, 8, 1, RATE, 0)); // silent
    ASSERT_EQ_INT(0, rec_tl_push(&t, s, 8, 1, RATE, 3)); // 0 -> 3/7
    ASSERT_EQ_INT(0, rec_tl_finish(&t, 32, true));
    int16_t out[32];
    ASSERT_EQ_INT(32, (int)rec_tl_read(&t, out, 32));
    for (int i = 0; i < 8; i++)
        ASSERT_EQ_INT(7000, out[i]);
    // gain (7 (7 - i) + 0 (i + 1)) / 56 for i = 0..7: 6125 down to 0
    ASSERT_EQ_INT(6125, out[8]);
    ASSERT_EQ_INT(875, out[14]);
    ASSERT_EQ_INT(0, out[15]);
    for (int i = 16; i < 24; i++)
        ASSERT_EQ_INT(0, out[i]);
    ASSERT_EQ_INT(375, out[24]); // 3 * 1 / 56 of 7000
    ASSERT_EQ_INT(3000, out[31]); // 3 / 7 of 7000
    rec_tl_free(&t);
}

TEST(timeline_resample_and_mix) {
    rec_timeline_t t;
    ASSERT_EQ_INT(0, rec_tl_init(&t, 22000, 1, 400));
    // Stereo at half the rate, a rising ramp on the left and its negative on the right + 2000
    int16_t s[2 * 1000];
    for (int i = 0; i < 1000; i++) {
        s[2 * i] = (int16_t)(i * 10);
        s[2 * i + 1] = (int16_t)(2000 - i * 10);
    }
    ASSERT_EQ_INT(0, rec_tl_push(&t, s, 500, 2, 11000, 7));
    ASSERT_EQ_INT(0, rec_tl_push(&t, s + 1000, 500, 2, 11000, 7));
    uint64_t n = t.count;
    // Twice the frames, give or take the interpolation's edge
    ASSERT_TRUE(n >= 1998 && n <= 2000);
    ASSERT_EQ_INT(0, rec_tl_finish(&t, n, true));
    int16_t out[2000];
    ASSERT_EQ_INT((int)n, (int)rec_tl_read(&t, out, 2000));
    // Mono is the mean of the two sides: 1000 throughout
    for (uint64_t i = 0; i < n; i++)
        ASSERT_EQ_INT(1000, out[i]);
    rec_tl_free(&t);

    // A plain ramp, mono, upsampled: monotone, and halfway values in between
    ASSERT_EQ_INT(0, rec_tl_init(&t, 22000, 1, 400));
    int16_t m[100];
    for (int i = 0; i < 100; i++)
        m[i] = (int16_t)(i * 100);
    ASSERT_EQ_INT(0, rec_tl_push(&t, m, 100, 1, 11000, 7));
    n = t.count;
    ASSERT_EQ_INT(0, rec_tl_finish(&t, n, true));
    ASSERT_EQ_INT((int)n, (int)rec_tl_read(&t, out, 2000));
    for (uint64_t i = 1; i < n; i++)
        ASSERT_TRUE(out[i] >= out[i - 1]);
    ASSERT_EQ_INT(50, out[3]); // halfway between m[1] and m[2], a frame of delay behind the source
    rec_tl_free(&t);
}

// ============================================================================
// Pixels
// ============================================================================

TEST(pixels_band) {
    enum { W = 64, H = 20, STRIDE = 80 };
    static uint8_t fb[H * STRIDE], shadow[H * W], out[H * W];
    for (int i = 0; i < H * STRIDE; i++)
        fb[i] = (uint8_t)(i * 7);
    uint32_t a, b;
    // Everything differs from a zero shadow
    ASSERT_TRUE(rec_band_scan(fb, STRIDE, shadow, W, H, &a, &b));
    ASSERT_EQ_INT(0, (int)a);
    ASSERT_EQ_INT(H, (int)b);
    rec_band_take(fb, STRIDE, shadow, W, a, b, false, out);
    ASSERT_TRUE(memcmp(out + 3 * W, fb + 3 * STRIDE, W) == 0);
    ASSERT_TRUE(!rec_band_scan(fb, STRIDE, shadow, W, H, &a, &b));
    // Bytes past the row's pixels (the stride's padding) are not looked at
    fb[2 * STRIDE + W + 1] ^= 0xFF;
    ASSERT_TRUE(!rec_band_scan(fb, STRIDE, shadow, W, H, &a, &b));
    // Two rows change: the band spans them and the XOR is the change
    fb[5 * STRIDE + 3] ^= 0x10;
    fb[9 * STRIDE + 60] ^= 0x81;
    ASSERT_TRUE(rec_band_scan(fb, STRIDE, shadow, W, H, &a, &b));
    ASSERT_EQ_INT(5, (int)a);
    ASSERT_EQ_INT(10, (int)b);
    rec_band_take(fb, STRIDE, shadow, W, a, b, true, out);
    ASSERT_EQ_HEX(0x10, out[3]);
    ASSERT_EQ_HEX(0x81, out[4 * W + 60]);
    for (int i = 0; i < 5 * W; i++)
        if (i != 3 && i != 4 * W + 60)
            ASSERT_EQ_INT(0, out[i]);
    ASSERT_TRUE(!rec_band_scan(fb, STRIDE, shadow, W, H, &a, &b));
    // A dropped frame: the shadow alone advances
    fb[0] ^= 1;
    rec_band_take(fb, STRIDE, shadow, W, 0, 1, true, NULL);
    ASSERT_TRUE(!rec_band_scan(fb, STRIDE, shadow, W, H, &a, &b));
}

TEST(pixels_convert) {
    uint8_t ident[3][256], table[3][256];
    rec_table_compose(ident, NULL, NULL);
    ASSERT_EQ_INT(200, ident[1][200]);
    // A DAC table then a monitor curve
    static uint8_t dac[768], crt[768];
    for (int c = 0; c < 3; c++)
        for (int v = 0; v < 256; v++) {
            dac[c * 256 + v] = (uint8_t)(255 - v);
            crt[c * 256 + v] = (uint8_t)(c == 2 ? v / 2 : v);
        }
    rec_table_compose(table, dac, crt);
    ASSERT_EQ_INT(255, table[0][0]);
    ASSERT_EQ_INT(127, table[2][0]);

    // 1 bpp: one index per byte, white 0 / black 1 in the palette
    uint8_t raw1[2] = {0xA0, 0x01}, idx[16];
    rec_row_convert(raw1, REC_PIX_1BPP, 16, REC_CLASS_INDEXED, ident, NULL, idx);
    ASSERT_EQ_INT(1, idx[0]);
    ASSERT_EQ_INT(0, idx[1]);
    ASSERT_EQ_INT(1, idx[2]);
    ASSERT_EQ_INT(1, idx[15]);
    uint8_t pal[768];
    rec_palette_build(pal, REC_PIX_1BPP, NULL, 0, ident);
    ASSERT_EQ_INT(255, pal[0]);
    ASSERT_EQ_INT(0, pal[3]);
    ASSERT_EQ_INT(1, rec_palette_black(pal));
    // 2 and 4 bpp
    uint8_t raw2[1] = {0x1B};
    rec_row_convert(raw2, REC_PIX_2BPP, 4, REC_CLASS_INDEXED, ident, NULL, idx);
    ASSERT_EQ_INT(0, idx[0]);
    ASSERT_EQ_INT(1, idx[1]);
    ASSERT_EQ_INT(2, idx[2]);
    ASSERT_EQ_INT(3, idx[3]);
    uint8_t raw4[1] = {0x5C};
    rec_row_convert(raw4, REC_PIX_4BPP, 2, REC_CLASS_INDEXED, ident, NULL, idx);
    ASSERT_EQ_INT(5, idx[0]);
    ASSERT_EQ_INT(12, idx[1]);
    // An indexed CLUT through the tables, index modulo the CLUT's length
    uint8_t clut[768] = {10, 20, 30, 40, 50, 60};
    rec_palette_build(pal, REC_PIX_8BPP, clut, 2, table);
    ASSERT_EQ_INT(table[0][10], pal[0]);
    ASSERT_EQ_INT(table[2][60], pal[5]);
    ASSERT_EQ_INT(table[0][10], pal[6]); // entry 2 wraps to 0

    // 16 bpp without a table: the words swapped to little-endian
    uint8_t raw16[4] = {0x7C, 0x00, 0x03, 0xE0}, o16[4];
    rec_row_convert(raw16, REC_PIX_16BPP555, 2, REC_CLASS_D15, ident, NULL, o16);
    ASSERT_EQ_HEX(0x00, o16[0]);
    ASSERT_EQ_HEX(0x7C, o16[1]);
    ASSERT_EQ_HEX(0xE0, o16[2]);
    ASSERT_EQ_HEX(0x03, o16[3]);
    // 16 bpp to 32 through the table: red 31 -> 255 -> table
    uint8_t o32[8];
    rec_row_convert(raw16, REC_PIX_16BPP555, 2, REC_CLASS_D32, table, NULL, o32);
    ASSERT_EQ_INT(table[2][0], o32[0]);
    ASSERT_EQ_INT(table[1][0], o32[1]);
    ASSERT_EQ_INT(table[0][255], o32[2]);
    ASSERT_EQ_INT(0, o32[3]);
    // 32 bpp XRGB big-endian: X dropped, B G R 0 out
    uint8_t raw32[4] = {0xEE, 0x11, 0x22, 0x33};
    rec_row_convert(raw32, REC_PIX_32BPP, 1, REC_CLASS_D32, ident, NULL, o32);
    ASSERT_EQ_HEX(0x33, o32[0]);
    ASSERT_EQ_HEX(0x22, o32[1]);
    ASSERT_EQ_HEX(0x11, o32[2]);
    ASSERT_EQ_HEX(0x00, o32[3]);

    // Classes: a table pushes 16 bpp to 32; rgb takes everything to 32
    ASSERT_EQ_INT(REC_CLASS_INDEXED, rec_pix_class(REC_PIX_4BPP, true, false));
    ASSERT_EQ_INT(REC_CLASS_D15, rec_pix_class(REC_PIX_16BPP555, false, false));
    ASSERT_EQ_INT(REC_CLASS_D32, rec_pix_class(REC_PIX_16BPP555, true, false));
    ASSERT_EQ_INT(REC_CLASS_D32, rec_pix_class(REC_PIX_8BPP, false, true));
    ASSERT_TRUE(rec_pix_class_fits(REC_CLASS_INDEXED, REC_PIX_1BPP, false));
    ASSERT_TRUE(!rec_pix_class_fits(REC_CLASS_INDEXED, REC_PIX_32BPP, false));
    ASSERT_TRUE(rec_pix_class_fits(REC_CLASS_D32, REC_PIX_8BPP, false));
    ASSERT_TRUE(!rec_pix_class_fits(REC_CLASS_D15, REC_PIX_16BPP555, true));
}

// ============================================================================
// The coder through the ring
// ============================================================================

// Reserve, fill and publish one record.
static void put(rec_worker_t *w, uint32_t kind, const void *payload, size_t len) {
    uint8_t *p = rec_worker_reserve(w, kind, len, 0, true);
    ASSERT_TRUE(p != NULL);
    if (len)
        memcpy(p, payload, len);
    rec_worker_commit(w);
}

// A SEGMENT record for `path`.
static void put_segment(rec_worker_t *w, const char *path, uint32_t cw, uint32_t ch, uint32_t cls, uint32_t index) {
    uint8_t buf[512];
    rec_seg_hdr_t h = {.canvas_w = cw,
                       .canvas_h = ch,
                       .cls = cls,
                       .div = 1,
                       .audio_rate = RATE,
                       .audio_channels = 1,
                       .index = index,
                       .path_len = (uint32_t)strlen(path)};
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, path, h.path_len);
    put(w, REC_K_SEGMENT, buf, sizeof h + h.path_len);
}

// A FRAME record: header, optional CLUT and CRT table, then the band.
static void put_frame(rec_worker_t *w, uint32_t flags, uint32_t width, uint32_t height, uint32_t format,
                      uint32_t row_bytes, uint32_t first, uint32_t end, const uint8_t *clut, uint32_t clut_len,
                      const uint8_t *crt, const uint8_t *band) {
    static uint8_t buf[1 << 16];
    rec_frame_hdr_t h = {.flags = flags,
                         .width = width,
                         .height = height,
                         .format = format,
                         .row_bytes = row_bytes,
                         .row_first = first,
                         .row_end = end,
                         .clut_len = clut_len};
    size_t at = sizeof h;
    if (flags & REC_FR_CLUT) {
        memcpy(buf + at, clut, 768);
        at += 768;
    }
    if (flags & REC_FR_CRT) {
        memcpy(buf + at, crt, 768);
        at += 768;
    }
    if (end > first)
        memcpy(buf + at, band, (size_t)(end - first) * row_bytes);
    at += (size_t)(end - first) * row_bytes;
    memcpy(buf, &h, sizeof h);
    put(w, REC_K_FRAME, buf, at);
}

// One audio push of `n` mono frames of value `v`.
static void put_audio(rec_worker_t *w, size_t n, int16_t v) {
    static uint8_t buf[sizeof(rec_audio_hdr_t) + 2048 * 2];
    rec_audio_hdr_t h = {.rate = RATE, .channels = 1, .vol = 7, .frames = (uint32_t)n};
    memcpy(buf, &h, sizeof h);
    int16_t *s = (int16_t *)(buf + sizeof h);
    for (size_t i = 0; i < n; i++)
        s[i] = v;
    put(w, REC_K_AUDIO, buf, sizeof h + n * 2);
}

// The session the coder tests record: a 64x48 1 bpp screen that changes,
// repeats, then an 8 bpp 32x24 mode centred on the same canvas with a CLUT,
// then a monitor curve; audio throughout.  Returns the frames sent.
static int record_session(rec_worker_t *w, const char *path, uint8_t final_crt[768]) {
    put_segment(w, path, 64, 48, REC_CLASS_INDEXED, 1);
    static uint8_t fb1[48 * 8];
    for (int i = 0; i < 48 * 8; i++)
        fb1[i] = (uint8_t)(i * 37);
    int frames = 0;
    uint64_t pushed = 0;
    // Audio in step with time before each frame
#define AUDIO_TO(k)                                                                                                    \
    do {                                                                                                               \
        uint64_t due_ = rec_tl_target((k), PERIOD_NS, RATE);                                                           \
        put_audio(w, (size_t)(due_ - pushed), (int16_t)(100 * (k)));                                                   \
        pushed = due_;                                                                                                 \
    } while (0)
    AUDIO_TO(1);
    put_frame(w, REC_FR_WHOLE | REC_FR_MODE | REC_FR_TABLES, 64, 48, REC_PIX_1BPP, 8, 0, 48, NULL, 0, NULL, fb1);
    frames++;
    // Rows 10..12 change: their XOR
    uint8_t x[3 * 8] = {0};
    x[0] = 0x80;
    x[2 * 8 + 7] = 0x01;
    fb1[10 * 8] ^= 0x80;
    fb1[12 * 8 + 7] ^= 0x01;
    AUDIO_TO(2);
    put_frame(w, 0, 64, 48, REC_PIX_1BPP, 8, 10, 13, NULL, 0, NULL, x);
    frames++;
    // Five unchanged frames: each one's repeat goes out before the next
    // frame-unit's audio, as the capture side orders them
    rec_repeat_hdr_t r = {.count = 1};
    for (int k = 3; k <= 7; k++) {
        AUDIO_TO(k);
        put(w, REC_K_REPEAT, &r, sizeof r);
    }
    frames += 5;
    // 8 bpp, smaller: centred on black
    static uint8_t fb8[24 * 32], clut[768];
    for (int i = 0; i < 24 * 32; i++)
        fb8[i] = (uint8_t)(i % 7 + 1);
    for (int i = 0; i < 256; i++) {
        clut[i * 3] = (uint8_t)(i * 30);
        clut[i * 3 + 1] = (uint8_t)(i * 20);
        clut[i * 3 + 2] = (uint8_t)(i * 10);
    }
    AUDIO_TO(8);
    put_frame(w, REC_FR_WHOLE | REC_FR_MODE | REC_FR_CLUT | REC_FR_TABLES, 32, 24, REC_PIX_8BPP, 32, 0, 24, clut, 256,
              NULL, fb8);
    frames++;
    // The monitor's curve arrives: a palette record, no pixel change
    for (int c = 0; c < 3; c++)
        for (int v = 0; v < 256; v++)
            final_crt[c * 256 + v] = (uint8_t)(c == 2 ? v * 3 / 4 : v);
    AUDIO_TO(9);
    put_frame(w, REC_FR_TABLES | REC_FR_CRT, 32, 24, REC_PIX_8BPP, 32, 0, 0, NULL, 0, final_crt, NULL);
    frames++;
    // Silence (no pushes) for a hundred frames: held, then the stop
    r.count = 100;
    put(w, REC_K_REPEAT, &r, sizeof r);
    frames += 100;
    put(w, REC_K_STOP, NULL, 0);
#undef AUDIO_TO
    return frames;
}

// Read a whole file into memory.
static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    ASSERT_TRUE(f != NULL);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    ASSERT_TRUE(fread(b, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *len = (size_t)n;
    return b;
}

TEST(coder_inline_and_threaded) {
    char p1[512], p2[512];
    out_path(p1, sizeof p1, "inline.mkv");
    out_path(p2, sizeof p2, "threaded.mkv");
    uint8_t crt[768];
    rec_worker_t *w = rec_worker_new(rec_coder_new(), 1u << 20, false, true);
    ASSERT_TRUE(w != NULL);
    int frames = record_session(w, p1, crt);
    rec_stats_t st;
    rec_worker_free(w, &st, NULL, 0);
    ASSERT_EQ_INT(0, st.err);
    ASSERT_EQ_INT(frames, (int)st.frames);
    w = rec_worker_new(rec_coder_new(), 1u << 20, true, true);
    ASSERT_TRUE(w != NULL);
    record_session(w, p2, crt);
    rec_worker_free(w, &st, NULL, 0);
    ASSERT_EQ_INT(0, st.err);
    size_t n1, n2;
    uint8_t *a = slurp(p1, &n1), *b = slurp(p2, &n2);
    ASSERT_TRUE(n1 == n2 && memcmp(a, b, n1) == 0);
    free(a);
    free(b);

    // Read it back: every frame decodes, the pictures are what went in
    int err = 0;
    mkv_reader_t *r = mkv_read_open(p1, &err);
    ASSERT_TRUE(r != NULL);
    codec_zmbv_dec_t *zd = codec_zmbv_dec_new(64, 48, &err);
    codec_flac_dec_t *fd = codec_flac_dec_new(&err);
    static int16_t pcm[1 << 20];
    size_t samples = 0;
    int k = 0, keys = 0;
    mkv_packet_t p;
    while (mkv_read_packet(r, &p) == 1) {
        if (p.track == 1) {
            ASSERT_EQ_INT(0, codec_zmbv_dec_frame(zd, p.data, p.len));
            keys += p.keyframe;
            const uint8_t *px = codec_zmbv_dec_pixels(zd);
            const uint8_t *pal = codec_zmbv_dec_palette(zd);
            if (k == 0) {
                // 1 bpp as indices, white and black
                ASSERT_EQ_INT((0x00 * 37 >> 7) & 1, px[0]);
                ASSERT_EQ_INT(255, pal[0]);
                ASSERT_EQ_INT(0, pal[3]);
            }
            if (k == 1)
                ASSERT_EQ_INT(((uint8_t)(80 * 37) ^ 0x80) >> 7, px[10 * 64]);
            if (k == 7) {
                // The 8 bpp mode centred: border index is black's
                ASSERT_EQ_INT(0, px[0]); // index 0 is black in this CLUT
                ASSERT_EQ_INT(1, px[12 * 64 + 16]);
                ASSERT_EQ_INT(30, pal[3]);
            }
            if (k == 8) {
                // The curve went into the palette
                ASSERT_EQ_INT(30, pal[3]);
                ASSERT_EQ_INT(crt[512 + 10], pal[5]);
            }
            k++;
        } else {
            int64_t n = codec_flac_dec_frame(fd, p.data, p.len, pcm + samples, 4096, NULL);
            ASSERT_TRUE(n > 0);
            samples += (size_t)n;
        }
    }
    ASSERT_EQ_INT(frames, k);
    ASSERT_EQ_INT(2, keys); // the first frame, and the mode change
    // The sound covers the frames exactly, and the silence held the last value
    ASSERT_TRUE(samples == rec_tl_target((uint64_t)frames, PERIOD_NS, RATE));
    ASSERT_EQ_INT(900, pcm[samples - 1]);
    ASSERT_EQ_INT(100, pcm[0]);
    codec_flac_dec_free(fd);
    codec_zmbv_dec_free(zd);
    mkv_read_close(r);
}

TEST(coder_segments) {
    // A second SEGMENT closes the first file and opens the next
    char p1[512], p2[512];
    out_path(p1, sizeof p1, "seg-1.mkv");
    out_path(p2, sizeof p2, "seg-2.mkv");
    rec_worker_t *w = rec_worker_new(rec_coder_new(), 1u << 20, true, true);
    static uint8_t fb[32 * 32];
    put_segment(w, p1, 32, 32, REC_CLASS_INDEXED, 1);
    put_frame(w, REC_FR_WHOLE | REC_FR_MODE | REC_FR_TABLES, 32, 32, REC_PIX_8BPP, 32, 0, 32, NULL, 0, NULL, fb);
    put_segment(w, p2, 64, 64, REC_CLASS_INDEXED, 2);
    static uint8_t fb2[64 * 64];
    put_frame(w, REC_FR_WHOLE | REC_FR_MODE | REC_FR_TABLES, 64, 64, REC_PIX_8BPP, 64, 0, 64, NULL, 0, NULL, fb2);
    rec_repeat_hdr_t r = {.count = 2};
    put(w, REC_K_REPEAT, &r, sizeof r);
    put(w, REC_K_STOP, NULL, 0);
    rec_stats_t st;
    rec_worker_free(w, &st, NULL, 0);
    ASSERT_EQ_INT(0, st.err);
    int err = 0;
    const char *paths[2] = {p1, p2};
    int want[2] = {1, 3}, size[2] = {32, 64};
    for (int i = 0; i < 2; i++) {
        mkv_reader_t *rd = mkv_read_open(paths[i], &err);
        ASSERT_TRUE(rd != NULL);
        ASSERT_EQ_INT(size[i], (int)mkv_read_track(rd, 0)->pixel_width);
        mkv_packet_t p;
        int n = 0;
        while (mkv_read_packet(rd, &p) == 1)
            n += p.track == 1;
        ASSERT_EQ_INT(want[i], n);
        mkv_read_close(rd);
    }
}

TEST(coder_close_then_sync) {
    // CLOSE finalises the file and the recording carries on: once
    // rec_worker_sync returns the first file is whole, and a later SEGMENT
    // opens the next, on a keyframe
    char p1[512], p2[512];
    out_path(p1, sizeof p1, "close-1.mkv");
    out_path(p2, sizeof p2, "close-2.mkv");
    rec_worker_t *w = rec_worker_new(rec_coder_new(), 1u << 20, true, true);
    static uint8_t fb[32 * 32];
    put_segment(w, p1, 32, 32, REC_CLASS_INDEXED, 1);
    put_frame(w, REC_FR_WHOLE | REC_FR_MODE | REC_FR_TABLES, 32, 32, REC_PIX_8BPP, 32, 0, 32, NULL, 0, NULL, fb);
    put_audio(w, 600, 100);
    rec_repeat_hdr_t r = {.count = 3};
    put(w, REC_K_REPEAT, &r, sizeof r);
    put(w, REC_K_CLOSE, NULL, 0);
    rec_worker_sync(w);
    rec_stats_t st;
    rec_worker_stats(w, &st);
    ASSERT_EQ_INT(0, st.err);
    int err = 0;
    mkv_reader_t *rd = mkv_read_open(p1, &err);
    ASSERT_TRUE(rd != NULL);
    mkv_packet_t p;
    int n = 0;
    while (mkv_read_packet(rd, &p) == 1)
        n += p.track == 1;
    ASSERT_EQ_INT(4, n);
    ASSERT_TRUE(!mkv_read_truncated(rd));
    ASSERT_TRUE(mkv_read_duration(rd) > 0);
    mkv_read_close(rd);
    // The next segment, after a gap the recording did not see
    put_segment(w, p2, 32, 32, REC_CLASS_INDEXED, 2);
    put_frame(w, REC_FR_WHOLE | REC_FR_MODE | REC_FR_TABLES, 32, 32, REC_PIX_8BPP, 32, 0, 32, NULL, 0, NULL, fb);
    put(w, REC_K_REPEAT, &r, sizeof r);
    put(w, REC_K_STOP, NULL, 0);
    rec_worker_free(w, &st, NULL, 0);
    ASSERT_EQ_INT(0, st.err);
    rd = mkv_read_open(p2, &err);
    ASSERT_TRUE(rd != NULL);
    n = 0;
    int keys = 0;
    while (mkv_read_packet(rd, &p) == 1) {
        n += p.track == 1;
        keys += p.track == 1 && p.keyframe;
    }
    ASSERT_EQ_INT(4, n);
    ASSERT_EQ_INT(1, keys);
    mkv_read_close(rd);
}

TEST(coder_direct_colour_tables) {
    // 32 bpp through a monitor curve, then the curve changes: a keyframe
    char path[512];
    out_path(path, sizeof path, "d32.mkv");
    rec_worker_t *w = rec_worker_new(rec_coder_new(), 1u << 20, false, true);
    uint8_t buf[512];
    rec_seg_hdr_t h = {.canvas_w = 8, .canvas_h = 4, .cls = REC_CLASS_D32, .div = 2, .index = 1};
    h.path_len = (uint32_t)strlen(path);
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, path, h.path_len);
    put(w, REC_K_SEGMENT, buf, sizeof h + h.path_len);
    static uint8_t fb[4 * 32], crt[768];
    for (int i = 0; i < 4 * 8; i++) {
        fb[i * 4] = 0xFF;
        fb[i * 4 + 1] = (uint8_t)i;
        fb[i * 4 + 2] = (uint8_t)(i * 2);
        fb[i * 4 + 3] = (uint8_t)(i * 3);
    }
    for (int c = 0; c < 3; c++)
        for (int v = 0; v < 256; v++)
            crt[c * 256 + v] = (uint8_t)(255 - v);
    put_frame(w, REC_FR_WHOLE | REC_FR_MODE | REC_FR_TABLES | REC_FR_CRT, 8, 4, REC_PIX_32BPP, 32, 0, 4, NULL, 0, crt,
              fb);
    put_frame(w, REC_FR_TABLES, 8, 4, REC_PIX_32BPP, 32, 0, 0, NULL, 0, NULL, NULL);
    put(w, REC_K_STOP, NULL, 0);
    rec_stats_t st;
    rec_worker_free(w, &st, NULL, 0);
    ASSERT_EQ_INT(0, st.err);
    ASSERT_EQ_INT(2, (int)st.keyframes);
    int err = 0;
    mkv_reader_t *r = mkv_read_open(path, &err);
    ASSERT_TRUE(r != NULL);
    ASSERT_TRUE(mkv_read_track(r, 0)->default_duration_ns == 2 * PERIOD_NS);
    ASSERT_EQ_INT(1, mkv_read_track_count(r)); // no audio stream: no audio track
    codec_zmbv_dec_t *zd = codec_zmbv_dec_new(8, 4, &err);
    uint8_t rgb[8 * 4 * 3];
    mkv_packet_t p;
    int k = 0;
    while (mkv_read_packet(r, &p) == 1) {
        ASSERT_EQ_INT(0, codec_zmbv_dec_frame(zd, p.data, p.len));
        ASSERT_EQ_INT(0, codec_zmbv_dec_rgb24(zd, rgb));
        // Frame 0 through the curve; frame 1 without one
        int i = 5;
        ASSERT_EQ_INT(k == 0 ? 255 - i : i, rgb[i * 3]);
        ASSERT_EQ_INT(k == 0 ? 255 - i * 3 : i * 3, rgb[i * 3 + 2]);
        k++;
    }
    ASSERT_EQ_INT(2, k);
    codec_zmbv_dec_free(zd);
    mkv_read_close(r);
}

TEST(worker_full_ring_refuses) {
    // With the drop policy a record that does not fit is refused, not waited for
    rec_worker_t *w = rec_worker_new(rec_coder_new(), 1u << 16, true, false);
    ASSERT_TRUE(w != NULL);
    uint8_t *a = rec_worker_reserve(w, REC_K_AUDIO, 25000, 0, false);
    ASSERT_TRUE(a != NULL);
    // Reserved, not published: the recorder thread cannot have freed it
    ASSERT_TRUE(rec_worker_reserve(w, REC_K_AUDIO, 25000, 0, false) == NULL);
    // A record larger than half the ring needs the ring to grow first
    ASSERT_TRUE(rec_worker_reserve(w, REC_K_AUDIO, 40000, 0, false) == NULL);
    rec_audio_hdr_t h = {.rate = RATE, .channels = 1, .vol = 7, .frames = 0};
    memcpy(a, &h, sizeof h);
    rec_worker_commit(w);
    uint8_t *s = rec_worker_reserve(w, REC_K_STOP, 0, 0, true);
    ASSERT_TRUE(s != NULL);
    rec_worker_commit(w);
    rec_worker_free(w, NULL, NULL, 0);
}

TEST(worker_fit_then_fits) {
    // A record rec_worker_fit says the ring can hold must then fit an empty
    // ring, with the room a frame leaves for audio besides: one that did
    // not was refused for ever (every frame dropped) or, blocking, waited
    // for ever
    rec_worker_t *w = rec_worker_new(rec_coder_new(), 1u << 16, true, false);
    ASSERT_TRUE(w != NULL);
    size_t sizes[] = {(1u << 15) - 8, (1u << 15) - 4096, 40000, 100000};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        size_t len = sizes[i];
        ASSERT_EQ_INT(0, rec_worker_fit(w, len));
        uint8_t *p = rec_worker_reserve(w, REC_K_AUDIO, len, REC_RESERVE_ROOM_MAX, false);
        ASSERT_TRUE(p != NULL);
        rec_audio_hdr_t h = {.rate = RATE, .channels = 1, .vol = 7, .frames = 0};
        memcpy(p, &h, sizeof h);
        rec_worker_commit(w);
        rec_worker_sync(w);
    }
    uint8_t *s = rec_worker_reserve(w, REC_K_STOP, 0, 0, true);
    ASSERT_TRUE(s != NULL);
    rec_worker_commit(w);
    rec_worker_free(w, NULL, NULL, 0);
}

TEST(coder_write_failure) {
    // The disk fills mid-recording (a file-size limit stands in): the coder
    // fails once with the write's errno, skips every later record without
    // stalling a blocking producer, and the file is finalised as far as it got
    char path[512];
    out_path(path, sizeof path, "efbig.mkv");
    remove(path);
    struct rlimit old;
    ASSERT_EQ_INT(0, getrlimit(RLIMIT_FSIZE, &old));
    void (*old_sig)(int) = signal(SIGXFSZ, SIG_IGN); // write() returns EFBIG instead
    struct rlimit lim = {.rlim_cur = 6000, .rlim_max = old.rlim_max};
    ASSERT_EQ_INT(0, setrlimit(RLIMIT_FSIZE, &lim));
    rec_worker_t *w = rec_worker_new(rec_coder_new(), 1u << 16, true, true);
    ASSERT_TRUE(w != NULL);
    put_segment(w, path, 64, 64, REC_CLASS_INDEXED, 1);
    static uint8_t fb[64 * 64];
    uint32_t x = 1;
    for (int k = 0; k < 400; k++) {
        // Noise compresses to nothing: every frame a few KB, a keyframe each
        for (size_t i = 0; i < sizeof fb; i++) {
            x = x * 1103515245u + 12345u;
            fb[i] = (uint8_t)(x >> 24);
        }
        put_frame(w, REC_FR_WHOLE | REC_FR_MODE | REC_FR_TABLES, 64, 64, REC_PIX_8BPP, 64, 0, 64, NULL, 0, NULL, fb);
        put_audio(w, 367, (int16_t)k);
    }
    put(w, REC_K_STOP, NULL, 0);
    rec_stats_t st;
    char msg[160] = "";
    rec_worker_free(w, &st, msg, sizeof msg);
    ASSERT_EQ_INT(0, setrlimit(RLIMIT_FSIZE, &old));
    signal(SIGXFSZ, old_sig);
    ASSERT_EQ_INT(-EFBIG, st.err);
    ASSERT_TRUE(strncmp(msg, "write", 5) == 0);
    ASSERT_TRUE(st.frames < 400);
    // What reached the disk still reads, up to where it was cut
    int err = 0;
    mkv_reader_t *r = mkv_read_open(path, &err);
    ASSERT_TRUE(r != NULL);
    mkv_packet_t p;
    while (mkv_read_packet(r, &p) == 1)
        ;
    mkv_read_close(r);

    // A file that cannot take even its header: the open fails, and the
    // records after it are skipped
    w = rec_worker_new(rec_coder_new(), 1u << 16, false, true);
    put_segment(w, "/dev/full", 64, 64, REC_CLASS_INDEXED, 1);
    put_frame(w, REC_FR_WHOLE | REC_FR_MODE | REC_FR_TABLES, 64, 64, REC_PIX_8BPP, 64, 0, 64, NULL, 0, NULL, fb);
    put(w, REC_K_STOP, NULL, 0);
    rec_worker_free(w, &st, msg, sizeof msg);
    ASSERT_EQ_INT(-ENOSPC, st.err);
    ASSERT_TRUE(strncmp(msg, "open", 4) == 0);
    ASSERT_EQ_INT(0, (int)st.frames);
}

int main(void) {
    RUN(timeline_hour_slow_chip);
    RUN(timeline_gap_is_held);
    RUN(timeline_start_silent);
    RUN(timeline_surplus_dropped);
    RUN(timeline_volume_ramp);
    RUN(timeline_resample_and_mix);
    RUN(pixels_band);
    RUN(pixels_convert);
    RUN(coder_inline_and_threaded);
    RUN(coder_segments);
    RUN(coder_close_then_sync);
    RUN(coder_direct_colour_tables);
    RUN(worker_full_ring_refuses);
    RUN(worker_fit_then_fits);
    RUN(coder_write_failure);
    fprintf(stderr, "record_capture: all tests passed\n");
    return 0;
}
