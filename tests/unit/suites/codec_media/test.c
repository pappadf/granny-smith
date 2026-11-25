// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Media codecs of the codec library (src/codec), each encoder against its
// decoder: the streaming zlib encoder (chunks with a sync flush) against
// the in-tree chunked inflater and, natively, the system zlib; ZMBV at
// every format, block size and awkward frame size, with palette changes,
// forced keyframes and unchanged frames, from whole frames and from XOR
// bands.

#include "codec.h"
#include "codec_zmbv.h"
#include "test_assert.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

// ---- data ----------------------------------------------------------------

// A small deterministic generator (xorshift32), so every run tests the
// same data.
static uint32_t s_rng = 0x2545F491u;

static uint32_t rnd(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

// A value in [0, n).
static uint32_t rnd_below(uint32_t n) {
    return n ? rnd() % n : 0;
}

// `n` bytes of a kind: 0 zeros, 1 noise, 2 text-like (a small alphabet
// with repeats), 3 runs of random lengths.
static void fill_kind(uint8_t *p, size_t n, int kind) {
    static const char words[] = "the quick brown fox jumps over the lazy dog; ";
    for (size_t i = 0; i < n; i++) {
        switch (kind) {
        case 0:
            p[i] = 0;
            break;
        case 1:
            p[i] = (uint8_t)rnd();
            break;
        case 2:
            p[i] = (uint8_t)words[(i * 7 + (i >> 9)) % (sizeof(words) - 1)];
            break;
        default: {
            uint8_t v = (uint8_t)rnd();
            size_t run = 1 + rnd_below(300);
            for (size_t k = 0; k < run && i < n; k++, i++)
                p[i] = v;
            i--;
            break;
        }
        }
    }
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    ASSERT_TRUE(p != NULL);
    return p;
}

// ---- streaming zlib ------------------------------------------------------

#define ZS_TOTAL  300000
#define ZS_CHUNKS 23

// N chunks, each ended by a sync flush, inflate chunk by chunk to the
// chunks themselves: through the in-tree decoder and through zlib.
TEST(zstream_chunks_inflate) {
    static const int levels[] = {CODEC_LEVEL_STORED, 1, 3, 9};
    uint8_t *data = xmalloc(ZS_TOTAL);
    size_t cut[ZS_CHUNKS + 1];
    for (int kind = 0; kind < 4; kind++) {
        fill_kind(data, ZS_TOTAL, kind);
        // Chunk boundaries: some empty chunks, some tiny, some large.
        cut[0] = 0;
        for (int i = 1; i < ZS_CHUNKS; i++)
            cut[i] = i % 7 == 3 ? cut[i - 1] : cut[i - 1] + rnd_below(2 * ZS_TOTAL / ZS_CHUNKS);
        for (int i = 1; i < ZS_CHUNKS; i++)
            if (cut[i] > ZS_TOTAL)
                cut[i] = ZS_TOTAL;
        cut[ZS_CHUNKS] = ZS_TOTAL;
        size_t max_chunk = 0;
        for (int i = 0; i < ZS_CHUNKS; i++)
            if (cut[i + 1] - cut[i] > max_chunk)
                max_chunk = cut[i + 1] - cut[i];
        for (size_t li = 0; li < sizeof(levels) / sizeof(levels[0]); li++) {
            int err = 0;
            codec_zstream_enc_t *enc = codec_zstream_enc_new(max_chunk, levels[li], &err);
            ASSERT_TRUE(enc != NULL);
            codec_zstream_dec_t *dec = codec_zstream_dec_new(&err);
            ASSERT_TRUE(dec != NULL);
            size_t cap = codec_zstream_enc_bound(max_chunk);
            uint8_t *out = xmalloc(cap), *back = xmalloc(max_chunk + 1);
#ifdef HAVE_ZLIB
            z_stream zs;
            memset(&zs, 0, sizeof(zs));
            ASSERT_EQ_INT(Z_OK, inflateInit(&zs));
#endif
            size_t total = 0;
            for (int i = 0; i < ZS_CHUNKS; i++) {
                size_t len = cut[i + 1] - cut[i];
                int64_t n = codec_zstream_enc_chunk(enc, data + cut[i], len, out, cap);
                ASSERT_TRUE(n >= 5);
                total += (size_t)n;
                // The chunk ends with the sync flush's empty stored block.
                ASSERT_TRUE(memcmp(out + n - 4, "\x00\x00\xFF\xFF", 4) == 0);
                // Only the first chunk carries the zlib header.
                if (i == 0)
                    ASSERT_TRUE(out[0] == 0x78 && out[1] == 0x9C);
                int64_t m = codec_zstream_dec_chunk(dec, out, (size_t)n, back, max_chunk + 1);
                if (m != (int64_t)len)
                    fprintf(stderr, "       kind %d level %d chunk %d: %s\n", kind, levels[li], i,
                            codec_zstream_dec_message(dec));
                ASSERT_EQ_INT((int)len, (int)m);
                ASSERT_TRUE(memcmp(back, data + cut[i], len) == 0);
#ifdef HAVE_ZLIB
                // zlib's inflate stops at the flush with every byte out.
                zs.next_in = out;
                zs.avail_in = (uInt)n;
                zs.next_out = back;
                zs.avail_out = (uInt)(max_chunk + 1);
                int zr = inflate(&zs, Z_SYNC_FLUSH);
                ASSERT_TRUE(zr == Z_OK || (zr == Z_BUF_ERROR && len == 0));
                ASSERT_EQ_INT(0, (int)zs.avail_in);
                ASSERT_EQ_INT((int)len, (int)(max_chunk + 1 - zs.avail_out));
                ASSERT_TRUE(memcmp(back, data + cut[i], len) == 0);
#endif
            }
            // Coded levels compress the compressible kinds.
            if (levels[li] > 0 && kind != 1)
                ASSERT_TRUE(total < ZS_TOTAL / 2);
#ifdef HAVE_ZLIB
            inflateEnd(&zs);
#endif
            free(out);
            free(back);
            codec_zstream_dec_free(dec);
            codec_zstream_enc_free(enc);
        }
    }
    free(data);
}

// A chunk that does not fit its buffer changes nothing; a reset starts a
// new stream with its own header; a chunk cut inside a block is an error
// that sticks until the decoder is reset.
TEST(zstream_overflow_reset_and_truncation) {
    enum { LEN = 5000 };
    uint8_t data[LEN], out[LEN * 2], back[LEN];
    fill_kind(data, LEN, 2);
    int err = 0;
    codec_zstream_enc_t *enc = codec_zstream_enc_new(LEN, 3, &err);
    codec_zstream_dec_t *dec = codec_zstream_dec_new(&err);
    ASSERT_TRUE(enc && dec);
    int64_t full = codec_zstream_enc_chunk(enc, data, LEN, out, sizeof(out));
    ASSERT_TRUE(full > 0);
    // Again into one byte too few: refused, and the header is still due.
    codec_zstream_enc_reset(enc);
    ASSERT_EQ_INT(CODEC_ERR_OVERFLOW, (int)codec_zstream_enc_chunk(enc, data, LEN, out, (size_t)full - 1));
    int64_t again = codec_zstream_enc_chunk(enc, data, LEN, out, sizeof(out));
    ASSERT_EQ_INT((int)full, (int)again);
    ASSERT_TRUE(out[0] == 0x78);
    ASSERT_EQ_INT(LEN, (int)codec_zstream_dec_chunk(dec, out, (size_t)again, back, LEN));
    // A second chunk continues without a header.
    int64_t second = codec_zstream_enc_chunk(enc, data, 100, out, sizeof(out));
    ASSERT_TRUE(second > 0 && out[0] != 0x78);
    // Decoded into a buffer one byte short: an overflow.
    ASSERT_EQ_INT(CODEC_ERR_OVERFLOW, (int)codec_zstream_dec_chunk(dec, out, (size_t)second, back, 99));
    // Sticks until reset.
    ASSERT_TRUE(codec_zstream_dec_chunk(dec, out, (size_t)second, back, LEN) < 0);
    // Reset both: a new stream, header first.
    codec_zstream_enc_reset(enc);
    codec_zstream_dec_reset(dec);
    int64_t n = codec_zstream_enc_chunk(enc, data, LEN, out, sizeof(out));
    ASSERT_TRUE(n > 6 && out[0] == 0x78);
    // A chunk cut short.
    ASSERT_EQ_INT(CODEC_ERR_TRUNCATED, (int)codec_zstream_dec_chunk(dec, out, (size_t)n - 6, back, LEN));
    codec_zstream_dec_reset(dec);
    ASSERT_EQ_INT(LEN, (int)codec_zstream_dec_chunk(dec, out, (size_t)n, back, LEN));
    // Exactly the right buffer size is not an overflow.
    int64_t k = codec_zstream_enc_chunk(enc, data, 777, out, sizeof(out));
    ASSERT_EQ_INT(777, (int)codec_zstream_dec_chunk(dec, out, (size_t)k, back, 777));
    ASSERT_TRUE(memcmp(back, data, 777) == 0);
    codec_zstream_dec_free(dec);
    codec_zstream_enc_free(enc);
}

// The one-shot encoder is untouched by the streaming entry points: a
// stream from codec_encode still decodes as a whole zlib stream.
TEST(zlib_one_shot_unchanged) {
    enum { LEN = 70000 };
    uint8_t *data = xmalloc(LEN), *back = xmalloc(LEN);
    fill_kind(data, LEN, 3);
    for (int level = 0; level <= 9; level += 3) {
        size_t cap = codec_enc_bound(CODEC_ZLIB, LEN);
        uint8_t *out = xmalloc(cap);
        int64_t n = codec_encode(CODEC_ZLIB, level, data, LEN, out, cap);
        ASSERT_TRUE(n > 0);
        ASSERT_EQ_INT(LEN, (int)codec_decode(CODEC_ZLIB, out, (size_t)n, back, LEN));
        ASSERT_TRUE(memcmp(back, data, LEN) == 0);
        free(out);
    }
    free(data);
    free(back);
}

// ---- ZMBV ------------------------------------------------------------------

// A test stream's shape.
typedef struct {
    int w, h, format, bw, bh, level;
} zshape_t;

static int fmt_bpp(int format) {
    return format == CODEC_ZMBV_FMT_8BPP ? 1 : format == CODEC_ZMBV_FMT_32BPP ? 4 : 2;
}

// Paint a random rectangle (or, rarely, the whole frame) into `frame`,
// widening [*lo, *hi) to the rows it touched.
static void paint(uint8_t *frame, const zshape_t *s, int *lo, int *hi) {
    int bpp = fmt_bpp(s->format);
    bool whole = rnd_below(10) == 0;
    int x0 = whole ? 0 : (int)rnd_below((uint32_t)s->w), y0 = whole ? 0 : (int)rnd_below((uint32_t)s->h);
    int x1 = whole ? s->w : x0 + 1 + (int)rnd_below((uint32_t)(s->w - x0));
    int y1 = whole ? s->h : y0 + 1 + (int)rnd_below((uint32_t)(s->h - y0 < 40 ? s->h - y0 : 40));
    uint8_t v[4] = {(uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd(), 0};
    bool noisy = rnd_below(3) == 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            for (int k = 0; k < bpp; k++) {
                uint8_t b = noisy ? (uint8_t)rnd() : v[k];
                // 32 bpp's top byte is unused; keep it zero, as a capture would.
                frame[((size_t)y * s->w + x) * bpp + k] = (bpp == 4 && k == 3) ? 0 : b;
            }
    if (y0 < *lo)
        *lo = y0;
    if (y1 > *hi)
        *hi = y1;
}

// Encode a run of frames in one shape and decode each, checking the
// decoded picture and palette are the encoded ones.  `xor_mode` feeds XOR
// bands instead of frames; `loose` passes the whole frame as the band.
static void zmbv_run(const zshape_t *s, int frames, bool xor_mode, bool loose) {
    int bpp = fmt_bpp(s->format);
    size_t row = (size_t)s->w * bpp, size = row * s->h;
    int err = 0;
    codec_zmbv_enc_t *enc = codec_zmbv_enc_new(s->w, s->h, s->format, s->bw, s->bh, s->level, &err);
    ASSERT_TRUE(enc != NULL);
    codec_zmbv_dec_t *dec = codec_zmbv_dec_new(s->w, s->h, &err);
    ASSERT_TRUE(dec != NULL);
    uint8_t *cur = calloc(1, size), *old = calloc(1, size), *xr = calloc(1, size);
    size_t cap = codec_zmbv_enc_bound(enc);
    uint8_t *pkt = xmalloc(cap);
    uint8_t pal[CODEC_ZMBV_PALETTE], want_pal[CODEC_ZMBV_PALETTE];
    ASSERT_TRUE(cur && old && xr);
    for (int i = 0; i < CODEC_ZMBV_PALETTE; i++)
        pal[i] = (uint8_t)rnd();
    memcpy(want_pal, pal, sizeof(pal));
    int counts[3] = {0};
    for (int f = 0; f < frames; f++) {
        int lo = s->h, hi = 0;
        uint32_t what = rnd_below(8);
        // Half the frames unchanged, the rest one to three rectangles.
        if (what >= 4)
            for (int k = 0, n = 1 + (int)rnd_below(3); k < n; k++)
                paint(cur, s, &lo, &hi);
        bool pal_change = s->format == CODEC_ZMBV_FMT_8BPP && rnd_below(5) == 0;
        if (pal_change) {
            int at = (int)rnd_below(CODEC_ZMBV_PALETTE - 30);
            for (int k = 0; k < 30; k++)
                pal[at + k] ^= (uint8_t)(1 + rnd_below(255));
        }
        if (lo >= hi)
            lo = hi = 0;
        if (loose) {
            lo = 0;
            hi = s->h;
        }
        codec_zmbv_frame_t fr = {.stride = row, .is_xor = xor_mode, .row_first = lo, .row_end = hi, .palette = NULL};
        // The palette with every change and the first frame, and now and then unchanged.
        if (pal_change || f == 0 || rnd_below(3) == 0)
            fr.palette = pal;
        // Every 17th frame forced through the flag, every 23rd through the call.
        fr.keyframe = f % 17 == 16;
        if (f % 23 == 22)
            codec_zmbv_enc_force_keyframe(enc);
        if (xor_mode) {
            for (size_t i = row * lo; i < row * hi; i++)
                xr[i] = cur[i] ^ old[i];
            fr.rows = xr + row * lo;
        } else {
            fr.rows = cur + row * lo;
        }
        codec_zmbv_kind_t kind;
        int64_t n = codec_zmbv_enc_frame(enc, &fr, pkt, cap, &kind);
        ASSERT_TRUE(n > 0);
        counts[kind]++;
        bool key_expected = f == 0 || f % 17 == 16 || f % 23 == 22;
        ASSERT_EQ_INT(key_expected ? CODEC_ZMBV_FLAG_KEY : 0, pkt[0] & CODEC_ZMBV_FLAG_KEY);
        ASSERT_EQ_INT(key_expected, kind == CODEC_ZMBV_KEYFRAME);
        if (key_expected)
            ASSERT_TRUE(pkt[1] == 0 && pkt[2] == 1 && pkt[3] == 1 && pkt[4] == s->format && pkt[5] == s->bw &&
                        pkt[6] == s->bh);
        bool same = memcmp(cur, old, size) == 0 && !pal_change;
        if (!key_expected && same) {
            ASSERT_EQ_INT(CODEC_ZMBV_UNCHANGED, kind);
            // A dozen bytes or so when coded, whatever the frame size.
            if (s->level > 0 && s->w * s->h <= 640 * 480 && s->bw >= 32)
                ASSERT_TRUE(n <= 24);
        }
        int rc = codec_zmbv_dec_frame(dec, pkt, (size_t)n);
        if (rc)
            fprintf(stderr, "       %dx%d fmt %d frame %d: %s\n", s->w, s->h, s->format, f,
                    codec_zmbv_dec_message(dec));
        ASSERT_EQ_INT(0, rc);
        ASSERT_EQ_INT(s->format, codec_zmbv_dec_format(dec));
        ASSERT_TRUE(memcmp(codec_zmbv_dec_pixels(dec), cur, size) == 0);
        ASSERT_TRUE(memcmp(codec_zmbv_enc_pixels(enc), cur, size) == 0);
        if (s->format == CODEC_ZMBV_FMT_8BPP) {
            memcpy(want_pal, pal, sizeof(pal));
            ASSERT_TRUE(memcmp(codec_zmbv_dec_palette(dec), want_pal, sizeof(pal)) == 0);
        }
        memcpy(old, cur, size);
    }
    // Every kind turned up.
    ASSERT_TRUE(counts[CODEC_ZMBV_KEYFRAME] > 0 && counts[CODEC_ZMBV_INTER] > 0 && counts[CODEC_ZMBV_UNCHANGED] > 0);
    free(cur);
    free(old);
    free(xr);
    free(pkt);
    codec_zmbv_dec_free(dec);
    codec_zmbv_enc_free(enc);
}

// Every format at sizes the 64x64 blocks do and do not divide, odd block
// sizes, every DEFLATE effort, from frames and from XOR bands.
TEST(zmbv_round_trips) {
    static const int formats[] = {CODEC_ZMBV_FMT_8BPP, CODEC_ZMBV_FMT_15BPP, CODEC_ZMBV_FMT_16BPP,
                                  CODEC_ZMBV_FMT_32BPP};
    static const zshape_t sizes[] = {
        {512, 342, 0, 64, 64, 3                 },
        {640, 480, 0, 64, 64, 1                 },
        {100, 70,  0, 64, 64, 3                 },
        {100, 70,  0, 7,  5,  CODEC_LEVEL_STORED},
        {1,   1,   0, 64, 64, 3                 },
        {33,  17,  0, 16, 16, 9                 },
    };
    for (size_t f = 0; f < sizeof(formats) / sizeof(formats[0]); f++)
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            zshape_t s = sizes[i];
            s.format = formats[f];
            int frames = s.w * s.h > 100000 ? 40 : 80;
            zmbv_run(&s, frames, false, false);
            zmbv_run(&s, frames, true, false);
            zmbv_run(&s, frames / 2, false, true);
        }
}

// A 640x480 frame with nothing changed costs about a dozen bytes, and a
// change in one block codes that block only.
TEST(zmbv_unchanged_is_tiny) {
    int err = 0;
    codec_zmbv_enc_t *e = codec_zmbv_enc_new(640, 480, CODEC_ZMBV_FMT_8BPP, 0, 0, 3, &err);
    ASSERT_TRUE(e != NULL);
    uint8_t *frame = calloc(1, 640 * 480), pkt[4096];
    uint8_t *big = xmalloc(codec_zmbv_enc_bound(e));
    ASSERT_TRUE(frame != NULL);
    codec_zmbv_frame_t f = {.rows = frame, .row_first = 0, .row_end = 480};
    ASSERT_TRUE(codec_zmbv_enc_frame(e, &f, big, codec_zmbv_enc_bound(e), NULL) > 0);
    codec_zmbv_frame_t same = {.row_first = 0, .row_end = 0};
    codec_zmbv_kind_t kind;
    int64_t n = codec_zmbv_enc_frame(e, &same, pkt, sizeof(pkt), &kind);
    fprintf(stderr, "       unchanged 640x480 frame: %d bytes\n", (int)n);
    ASSERT_EQ_INT(CODEC_ZMBV_UNCHANGED, kind);
    ASSERT_TRUE(n > 0 && n <= 16);
    // A band whose rows did not actually change is still an unchanged frame.
    codec_zmbv_frame_t band = {.rows = frame + 640 * 100, .row_first = 100, .row_end = 200};
    n = codec_zmbv_enc_frame(e, &band, pkt, sizeof(pkt), &kind);
    ASSERT_EQ_INT(CODEC_ZMBV_UNCHANGED, kind);
    // One pixel: one block of deltas.
    frame[640 * 130 + 70] = 9;
    n = codec_zmbv_enc_frame(e, &band, pkt, sizeof(pkt), &kind);
    ASSERT_EQ_INT(CODEC_ZMBV_INTER, kind);
    ASSERT_TRUE(n < 64);
    // Too small a buffer: refused, then the same frame fits.
    codec_zmbv_enc_force_keyframe(e);
    ASSERT_EQ_INT(CODEC_ERR_OVERFLOW, (int)codec_zmbv_enc_frame(e, &f, pkt, 8, &kind));
    n = codec_zmbv_enc_frame(e, &f, big, codec_zmbv_enc_bound(e), &kind);
    ASSERT_EQ_INT(CODEC_ZMBV_KEYFRAME, kind);
    free(frame);
    free(big);
    codec_zmbv_enc_free(e);
}

// The decoder survives every prefix and many corruptions of a short
// stream (the sanitizers catch an overrun), and refuses an inter frame
// before any keyframe.
TEST(zmbv_decoder_bad_input) {
    enum { W = 70, H = 50, FRAMES = 6 };
    int err = 0;
    codec_zmbv_enc_t *e = codec_zmbv_enc_new(W, H, CODEC_ZMBV_FMT_16BPP, 16, 16, 3, &err);
    codec_zmbv_dec_t *d = codec_zmbv_dec_new(W, H, &err);
    ASSERT_TRUE(e && d);
    size_t cap = codec_zmbv_enc_bound(e);
    uint8_t *pkts[FRAMES];
    int64_t lens[FRAMES];
    uint8_t frame[W * H * 2];
    memset(frame, 0, sizeof(frame));
    for (int i = 0; i < FRAMES; i++) {
        int lo = H, hi = 0;
        zshape_t s = {W, H, CODEC_ZMBV_FMT_16BPP, 16, 16, 3};
        paint(frame, &s, &lo, &hi);
        codec_zmbv_frame_t f = {.rows = frame, .row_first = 0, .row_end = H};
        pkts[i] = xmalloc(cap);
        lens[i] = codec_zmbv_enc_frame(e, &f, pkts[i], cap, NULL);
        ASSERT_TRUE(lens[i] > 0);
    }
    ASSERT_EQ_INT(CODEC_ERR_DATA, codec_zmbv_dec_frame(d, pkts[1], (size_t)lens[1]));
    // The pictures the whole frames decode to.
    static uint8_t want[FRAMES][W * H * 2];
    for (int i = 0; i < FRAMES; i++) {
        ASSERT_EQ_INT(0, codec_zmbv_dec_frame(d, pkts[i], (size_t)lens[i]));
        memcpy(want[i], codec_zmbv_dec_pixels(d), sizeof(want[i]));
    }
    for (int i = 0; i < FRAMES; i++)
        for (int64_t cut = 1; cut < lens[i]; cut++) {
            for (int k = 0; k < i; k++)
                ASSERT_EQ_INT(0, codec_zmbv_dec_frame(d, pkts[k], (size_t)lens[k]));
            int rc = codec_zmbv_dec_frame(d, pkts[i], (size_t)cut);
            // A prefix fails, unless all it lost is the sync flush's tail.
            ASSERT_TRUE(rc < 0 ||
                        (cut >= lens[i] - 5 && memcmp(codec_zmbv_dec_pixels(d), want[i], sizeof(want[i])) == 0));
        }
    for (int i = 0; i < FRAMES; i++)
        for (int64_t at = 0; at < lens[i]; at++) {
            for (int k = 0; k < i; k++)
                codec_zmbv_dec_frame(d, pkts[k], (size_t)lens[k]);
            pkts[i][at] ^= 0x5A;
            codec_zmbv_dec_frame(d, pkts[i], (size_t)lens[i]); // any result but a crash
            pkts[i][at] ^= 0x5A;
        }
    for (int i = 0; i < FRAMES; i++)
        free(pkts[i]);
    codec_zmbv_dec_free(d);
    codec_zmbv_enc_free(e);
}

int main(void) {
    RUN(zstream_chunks_inflate);
    RUN(zstream_overflow_reset_and_truncation);
    RUN(zlib_one_shot_unchanged);
    RUN(zmbv_round_trips);
    RUN(zmbv_unchanged_is_tiny);
    RUN(zmbv_decoder_bad_input);
    fprintf(stderr, "codec_media: all tests passed\n");
    return 0;
}
