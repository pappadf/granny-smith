// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// flac_enc.c
// FLAC encoder (RFC 9639) for 16-bit mono and stereo.  See codec_flac.h.
//
// A frame (§9) is a header -- sync code 0xFFF8 (fixed block size), the
// block size, sample rate, channel assignment and sample size codes, the
// frame number in UTF-8 form, the block size and sample rate where the
// codes defer to the end of the header, and a CRC-8 -- then one subframe
// per channel, zero bits to a byte boundary and a CRC-16 of the whole
// frame.  A subframe (§9.2) is a header byte (a zero bit, six bits of
// type, the wasted-bits flag) with the wasted-bit count in unary when the
// flag is set, then:
//
//   CONSTANT   one sample;
//   VERBATIM   every sample;
//   FIXED n    the first n samples, then the residual of the order-n
//              polynomial predictor, Rice-coded in 2^p partitions, each
//              with its own parameter (§9.2.7).
//
// Every channel tries all of them and keeps the cheapest; the Rice
// parameters are chosen per partition from the residual's magnitude, and
// the partition order by the total.  Stereo codes the four ways to write
// two channels (left/right, left/side, side/right, mid/side) and keeps the
// smallest pair.

#include "codec_flac.h"
#include "codec_internal.h"

#include <stdlib.h>
#include <string.h>

// ============================================================================
// Constants and types
// ============================================================================

#define FLAC_SYNC_FIXED 0xFFF8u // 14-bit sync, reserved 0, fixed block size
#define FLAC_BPS        16 // bits per sample of the stream
#define FLAC_MAX_ORDER  4 // the highest fixed predictor
#define FLAC_MAX_PORDER 8 // the finest partitioning tried (16 residuals a partition)
#define FLAC_RICE4_MAX  14 // the largest 4-bit Rice parameter (15 is the escape)
#define FLAC_RICE5_MAX  30 // the largest 5-bit Rice parameter (31 is the escape)
#define FLAC_HEADER_MAX 16 // the longest frame header

// Subframe types (the six bits after the zero bit).
#define SUB_CONSTANT 0x00
#define SUB_VERBATIM 0x01
#define SUB_FIXED    0x08 // | order

// Channel assignments (frame header bits 4..7 of byte 3).
#define CH_INDEPENDENT_MONO   0x0
#define CH_INDEPENDENT_STEREO 0x1
#define CH_LEFT_SIDE          0x8
#define CH_SIDE_RIGHT         0x9
#define CH_MID_SIDE           0xA

// How one channel of a block will be coded, and what it costs in bits.
typedef struct {
    int type; // SUB_CONSTANT, SUB_VERBATIM or SUB_FIXED
    int order; // fixed predictor order
    int wasted; // low bits that are zero in every sample
    int porder; // Rice partition order
    bool rice5; // 5-bit Rice parameters
    uint8_t k[1 << FLAC_MAX_PORDER]; // each partition's parameter
    uint64_t bits; // the subframe's size
} plan_t;

// The encoder: the stream's shape, the block being filled, the frame
// counter, and scratch for the residuals and the decorrelated channels.
struct codec_flac_enc {
    uint32_t rate;
    int channels;
    size_t pending; // sample frames in the block so far
    uint64_t taken; // sample frames taken in all
    uint64_t frame_no; // the next frame's number
    bool finished; // the short last frame has been written
    int32_t pcm[2][CODEC_FLAC_BLOCK]; // the block, per channel
    int32_t mid[CODEC_FLAC_BLOCK], side[CODEC_FLAC_BLOCK];
    int32_t shifted[CODEC_FLAC_BLOCK]; // a channel with its wasted bits removed
    uint32_t u[CODEC_FLAC_BLOCK]; // folded residuals
};

// ============================================================================
// Bit output
// ============================================================================

// MSB-first bit writer; writes past `cap` are counted, not stored, so an
// overflow is seen at the end.
typedef struct {
    uint8_t *buf;
    size_t cap, pos;
    uint64_t acc; // pending bits, the oldest highest
    int n; // pending bit count (< 8 between calls)
} bw_t;

// Append the low `bits` (<= 32) bits of `v`.
static void put(bw_t *w, uint32_t v, int bits) {
    if (bits <= 0)
        return;
    w->acc = w->acc << bits | (v & (uint32_t)((1ull << bits) - 1));
    w->n += bits;
    while (w->n >= 8) {
        w->n -= 8;
        if (w->pos < w->cap)
            w->buf[w->pos] = (uint8_t)(w->acc >> w->n);
        w->pos++;
    }
}

// Append `q` zero bits and a one: a unary count.
static void put_unary(bw_t *w, uint32_t q) {
    while (q >= 32) {
        put(w, 0, 32);
        q -= 32;
    }
    put(w, 1, (int)q + 1);
}

// Zero bits up to the next byte boundary.
static void align(bw_t *w) {
    if (w->n)
        put(w, 0, 8 - w->n);
}

// ============================================================================
// Analysis
// ============================================================================

// Fold a residual onto the non-negative integers (0, -1, 1, -2 ... -> 0, 1, 2, 3 ...).
static inline uint32_t fold(int32_t r) {
    return ((uint32_t)r << 1) ^ (uint32_t)(r >> 31);
}

// The order-`order` fixed predictor's residuals of x[order..n), folded, into u.
static void fixed_residual(const int32_t *x, size_t n, int order, uint32_t *u) {
    for (size_t i = (size_t)order; i < n; i++) {
        int32_t r;
        switch (order) {
        case 0:
            r = x[i];
            break;
        case 1:
            r = x[i] - x[i - 1];
            break;
        case 2:
            r = x[i] - 2 * x[i - 1] + x[i - 2];
            break;
        case 3:
            r = x[i] - 3 * x[i - 1] + 3 * x[i - 2] - x[i - 3];
            break;
        default:
            r = x[i] - 4 * x[i - 1] + 6 * x[i - 2] - 4 * x[i - 3] + x[i - 4];
            break;
        }
        u[i] = fold(r);
    }
}

// The Rice parameter that codes `count` folded residuals summing to `sum`
// in the fewest bits, by the usual estimate (count * (k + 1) + sum >> k),
// and that estimate.
static int best_k(uint64_t sum, uint64_t count, int kmax, uint64_t *bits) {
    int best = 0;
    uint64_t best_bits = UINT64_MAX;
    for (int k = 0; k <= kmax; k++) {
        uint64_t b = count * (uint64_t)(k + 1) + (sum >> k);
        if (b < best_bits) {
            best_bits = b;
            best = k;
        }
        // Past the minimum the estimate only grows.
        if ((sum >> k) == 0)
            break;
    }
    *bits = best_bits;
    return best;
}

// The exact bits of partitions coded with plan p's parameters.
static uint64_t rice_bits(const uint32_t *u, size_t n, int order, const plan_t *p) {
    size_t parts = (size_t)1 << p->porder, len = n >> p->porder;
    uint64_t bits = 2 + 4 + (uint64_t)parts * (p->rice5 ? 5 : 4); // method, order, parameters
    for (size_t j = 0; j < parts; j++) {
        size_t from = j == 0 ? (size_t)order : j * len, to = (j + 1) * len;
        int k = p->k[j];
        bits += (uint64_t)(to - from) * (uint64_t)(k + 1);
        for (size_t i = from; i < to; i++)
            bits += u[i] >> k;
    }
    return bits;
}

// Choose the partition order and Rice parameters for residuals u[order..n)
// into p (porder, rice5, k[]), returning the residual section's bits.
static uint64_t plan_rice(const uint32_t *u, size_t n, int order, plan_t *p) {
    // Sums over the finest partitions this block allows, merged pairwise
    // for the coarser orders.
    int pmax = 0;
    while (pmax < FLAC_MAX_PORDER && (n % ((size_t)2 << pmax)) == 0 && (n >> (pmax + 1)) > (size_t)order)
        pmax++;
    uint64_t sums[1 << FLAC_MAX_PORDER];
    size_t len = n >> pmax;
    for (size_t j = 0; j < ((size_t)1 << pmax); j++) {
        uint64_t s = 0;
        for (size_t i = j == 0 ? (size_t)order : j * len; i < (j + 1) * len; i++)
            s += u[i];
        sums[j] = s;
    }
    uint64_t best_bits = UINT64_MAX;
    plan_t trial;
    for (int po = pmax; po >= 0; po--) {
        size_t parts = (size_t)1 << po, plen = n >> po;
        uint64_t est = 0;
        int kmax_used = 0;
        for (size_t j = 0; j < parts; j++) {
            uint64_t cnt = plen - (j == 0 ? (size_t)order : 0), b;
            int k = best_k(sums[j], cnt, FLAC_RICE5_MAX, &b);
            trial.k[j] = (uint8_t)k;
            est += b;
            if (k > kmax_used)
                kmax_used = k;
        }
        trial.porder = po;
        trial.rice5 = kmax_used > FLAC_RICE4_MAX;
        est += 6 + parts * (trial.rice5 ? 5 : 4);
        if (est < best_bits) {
            best_bits = est;
            p->porder = trial.porder;
            p->rice5 = trial.rice5;
            memcpy(p->k, trial.k, parts);
        }
        // Merge pairs for the next, coarser order.
        for (size_t j = 0; j < parts / 2; j++)
            sums[j] = sums[2 * j] + sums[2 * j + 1];
    }
    return rice_bits(u, n, order, p);
}

// Plan the cheapest subframe for channel x[0..n) at `bps` bits per sample.
// e->shifted ends up holding x without its wasted bits.
static void plan_channel(codec_flac_enc_t *e, const int32_t *x, size_t n, int bps, plan_t *p) {
    memset(p, 0, sizeof(*p));
    // CONSTANT: one value throughout (silence, or a held level).
    bool constant = true;
    uint32_t any = 0;
    for (size_t i = 0; i < n; i++) {
        constant &= x[i] == x[0];
        any |= (uint32_t)x[i];
    }
    if (constant) {
        p->type = SUB_CONSTANT;
        p->bits = 8 + (uint64_t)bps;
        return;
    }
    // Wasted bits: the low bits no sample uses (8-bit audio in int16 has eight).
    int w = 0;
    while (!(any & 1u)) {
        any >>= 1;
        w++;
    }
    p->wasted = w;
    int ebps = bps - w;
    for (size_t i = 0; i < n; i++)
        e->shifted[i] = x[i] >> w;
    uint64_t head = 8 + (uint64_t)(w ? w : 0); // header byte and the unary count
    p->type = SUB_VERBATIM;
    p->bits = head + (uint64_t)n * (uint64_t)ebps;
    // FIXED of every order the block is long enough for.
    for (int order = 0; order <= FLAC_MAX_ORDER && (size_t)order < n; order++) {
        plan_t t = *p;
        fixed_residual(e->shifted, n, order, e->u);
        uint64_t bits = head + (uint64_t)order * (uint64_t)ebps + plan_rice(e->u, n, order, &t);
        if (bits < p->bits) {
            t.type = SUB_FIXED;
            t.order = order;
            t.bits = bits;
            *p = t;
        }
    }
}

// ============================================================================
// Writing
// ============================================================================

// Write channel x[0..n) at `bps` bits per sample as planned.
static void write_channel(codec_flac_enc_t *e, bw_t *w, const int32_t *x, size_t n, int bps, const plan_t *p) {
    if (p->type == SUB_CONSTANT) {
        put(w, SUB_CONSTANT << 1, 8);
        put(w, (uint32_t)x[0], bps);
        return;
    }
    int type = p->type == SUB_FIXED ? SUB_FIXED | p->order : SUB_VERBATIM;
    put(w, (uint32_t)type << 1 | (p->wasted ? 1u : 0u), 8);
    if (p->wasted)
        put_unary(w, (uint32_t)p->wasted - 1);
    int ebps = bps - p->wasted;
    for (size_t i = 0; i < n; i++)
        e->shifted[i] = x[i] >> p->wasted;
    if (p->type == SUB_VERBATIM) {
        for (size_t i = 0; i < n; i++)
            put(w, (uint32_t)e->shifted[i], ebps);
        return;
    }
    // Warm-up samples, then the partitioned residual.
    for (int i = 0; i < p->order; i++)
        put(w, (uint32_t)e->shifted[i], ebps);
    fixed_residual(e->shifted, n, p->order, e->u);
    put(w, p->rice5 ? 1 : 0, 2);
    put(w, (uint32_t)p->porder, 4);
    size_t parts = (size_t)1 << p->porder, len = n >> p->porder;
    for (size_t j = 0; j < parts; j++) {
        int k = p->k[j];
        put(w, (uint32_t)k, p->rice5 ? 5 : 4);
        for (size_t i = j == 0 ? (size_t)p->order : j * len; i < (j + 1) * len; i++) {
            put_unary(w, e->u[i] >> k);
            put(w, e->u[i], k);
        }
    }
}

// The 4-bit sample-rate code and, for codes 12..14, the value that follows
// the header (*extra_bits of it, 0 for none).
static int rate_code(uint32_t rate, uint32_t *extra, int *extra_bits) {
    static const uint32_t named[] = {0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
    *extra_bits = 0;
    for (int c = 1; c < (int)(sizeof(named) / sizeof(named[0])); c++)
        if (named[c] == rate)
            return c;
    if (rate % 1000 == 0 && rate / 1000 <= 255) {
        *extra = rate / 1000;
        *extra_bits = 8;
        return 12;
    }
    if (rate <= 65535) {
        *extra = rate;
        *extra_bits = 16;
        return 13;
    }
    if (rate % 10 == 0 && rate / 10 <= 65535) {
        *extra = rate / 10;
        *extra_bits = 16;
        return 14;
    }
    return 0; // only in STREAMINFO
}

// The frame number in FLAC's UTF-8-like form (up to 36 bits, 7 bytes).
static void put_utf8(bw_t *w, uint64_t v) {
    if (v < 0x80) {
        put(w, (uint32_t)v, 8);
        return;
    }
    int bytes = v < 0x800 ? 2 : v < 0x10000 ? 3 : v < 0x200000 ? 4 : v < 0x4000000 ? 5 : v < 0x80000000u ? 6 : 7;
    // The lead byte: `bytes` ones, a zero, then the value's top bits.
    int cont = bytes - 1, lead_bits = bytes == 7 ? 0 : 7 - bytes;
    uint32_t lead = (uint32_t)(0xFF00u >> bytes) & 0xFFu;
    put(w, lead | ((uint32_t)(v >> (6 * cont)) & ((1u << lead_bits) - 1)), 8);
    for (int i = cont - 1; i >= 0; i--)
        put(w, 0x80u | ((uint32_t)(v >> (6 * i)) & 0x3Fu), 8);
}

// Code the first `n` samples of the block as one frame.
static int64_t code_frame(codec_flac_enc_t *e, size_t n, uint8_t *out, size_t cap) {
    plan_t pa, pb;
    memset(&pb, 0, sizeof(pb)); // unused for mono
    int assign, bps_a = FLAC_BPS, bps_b = FLAC_BPS;
    const int32_t *a = e->pcm[0], *b = e->pcm[1];
    if (e->channels == 1) {
        assign = CH_INDEPENDENT_MONO;
        plan_channel(e, a, n, FLAC_BPS, &pa);
    } else {
        // The four ways to write a pair: plan each signal once, keep the
        // cheapest pair.
        for (size_t i = 0; i < n; i++) {
            e->side[i] = e->pcm[0][i] - e->pcm[1][i];
            e->mid[i] = (e->pcm[0][i] + e->pcm[1][i]) >> 1;
        }
        plan_t pl, pr, ps, pm;
        plan_channel(e, e->pcm[0], n, FLAC_BPS, &pl);
        plan_channel(e, e->pcm[1], n, FLAC_BPS, &pr);
        plan_channel(e, e->side, n, FLAC_BPS + 1, &ps);
        plan_channel(e, e->mid, n, FLAC_BPS, &pm);
        uint64_t lr = pl.bits + pr.bits, ls = pl.bits + ps.bits, sr = ps.bits + pr.bits, ms = pm.bits + ps.bits;
        assign = CH_INDEPENDENT_STEREO;
        pa = pl;
        pb = pr;
        uint64_t best = lr;
        if (ls < best) {
            best = ls;
            assign = CH_LEFT_SIDE;
            pb = ps;
            b = e->side;
            bps_b = FLAC_BPS + 1;
        }
        if (sr < best) {
            best = sr;
            assign = CH_SIDE_RIGHT;
            pa = ps;
            a = e->side;
            bps_a = FLAC_BPS + 1;
            pb = pr;
            b = e->pcm[1];
            bps_b = FLAC_BPS;
        }
        if (ms < best) {
            assign = CH_MID_SIDE;
            pa = pm;
            a = e->mid;
            bps_a = FLAC_BPS;
            pb = ps;
            b = e->side;
            bps_b = FLAC_BPS + 1;
        }
    }
    bw_t w = {out, cap, 0, 0, 0};
    // Header: the block size code (4096 has one; others follow the header).
    int bs_code = n == CODEC_FLAC_BLOCK ? 12 : n <= 256 ? 6 : 7;
    uint32_t rate_extra = 0;
    int rate_bits;
    int rc = rate_code(e->rate, &rate_extra, &rate_bits);
    put(&w, FLAC_SYNC_FIXED, 16);
    put(&w, (uint32_t)(bs_code << 4 | rc), 8);
    put(&w, (uint32_t)(assign << 4 | 0x4 << 1), 8); // sample size code 4: 16 bits; reserved 0
    put_utf8(&w, e->frame_no);
    if (bs_code == 6)
        put(&w, (uint32_t)(n - 1), 8);
    else if (bs_code == 7)
        put(&w, (uint32_t)(n - 1), 16);
    put(&w, rate_extra, rate_bits);
    if (w.pos > cap)
        return CODEC_ERR_OVERFLOW;
    put(&w, codec_crc8_smbus(0, out, w.pos), 8);
    write_channel(e, &w, a, n, bps_a, &pa);
    if (e->channels == 2)
        write_channel(e, &w, b, n, bps_b, &pb);
    align(&w);
    if (w.pos + 2 > cap)
        return CODEC_ERR_OVERFLOW;
    uint16_t crc = codec_crc16_umts(0, out, w.pos);
    put(&w, crc, 16);
    return (int64_t)w.pos;
}

// ============================================================================
// Lifecycle
// ============================================================================

codec_flac_enc_t *codec_flac_enc_new(uint32_t sample_rate, int channels, int *err) {
    int e0 = 0;
    if (!err)
        err = &e0;
    if (sample_rate == 0 || sample_rate > CODEC_FLAC_MAX_RATE || (channels != 1 && channels != 2)) {
        *err = CODEC_ERR_ARG;
        return NULL;
    }
    codec_flac_enc_t *e = calloc(1, sizeof(*e));
    if (!e) {
        *err = CODEC_ERR_NOMEM;
        return NULL;
    }
    e->rate = sample_rate;
    e->channels = channels;
    *err = 0;
    return e;
}

void codec_flac_enc_free(codec_flac_enc_t *e) {
    free(e);
}

// ============================================================================
// Operations
// ============================================================================

void codec_flac_enc_streaminfo(const codec_flac_enc_t *e, uint8_t *out) {
    memset(out, 0, CODEC_FLAC_STREAMINFO);
    // Minimum and maximum block size; the frame sizes (bytes 4..9) unknown.
    codec_wr_be16(out, CODEC_FLAC_BLOCK);
    codec_wr_be16(out + 2, CODEC_FLAC_BLOCK);
    // 20 bits of rate, 3 of channels - 1, 5 of bits - 1, then 36 bits of
    // total samples (0: unknown), and a zero MD5 (not computed).
    uint32_t v = e->rate << 12 | (uint32_t)(e->channels - 1) << 9 | (uint32_t)(FLAC_BPS - 1) << 4;
    codec_wr_be32(out + 10, v);
}

size_t codec_flac_enc_bound(const codec_flac_enc_t *e) {
    // Never more than VERBATIM at 17 bits a sample per channel, plus headers.
    return FLAC_HEADER_MAX + 2 + (size_t)e->channels * (2 + 2 + (size_t)CODEC_FLAC_BLOCK * (FLAC_BPS + 1) / 8);
}

size_t codec_flac_enc_push(codec_flac_enc_t *e, const int16_t *samples, size_t n) {
    if (!e || e->finished || !samples)
        return 0;
    size_t room = CODEC_FLAC_BLOCK - e->pending;
    size_t k = n < room ? n : room;
    for (size_t i = 0; i < k; i++)
        for (int c = 0; c < e->channels; c++)
            e->pcm[c][e->pending + i] = samples[i * (size_t)e->channels + (size_t)c];
    e->pending += k;
    e->taken += k;
    return k;
}

bool codec_flac_enc_ready(const codec_flac_enc_t *e) {
    return e && e->pending == CODEC_FLAC_BLOCK;
}

int64_t codec_flac_enc_frame(codec_flac_enc_t *e, uint8_t *out, size_t cap, uint64_t *first_sample) {
    if (!e || !out)
        return CODEC_ERR_ARG;
    if (e->pending < CODEC_FLAC_BLOCK)
        return 0;
    int64_t n = code_frame(e, CODEC_FLAC_BLOCK, out, cap);
    if (n < 0)
        return n;
    if (first_sample)
        *first_sample = e->frame_no * CODEC_FLAC_BLOCK;
    e->frame_no++;
    e->pending = 0;
    return n;
}

int64_t codec_flac_enc_flush(codec_flac_enc_t *e, uint8_t *out, size_t cap, uint64_t *first_sample) {
    if (!e || !out)
        return CODEC_ERR_ARG;
    // A full block is an ordinary frame; a partial one is the last.
    if (e->pending == CODEC_FLAC_BLOCK)
        return codec_flac_enc_frame(e, out, cap, first_sample);
    if (e->pending == 0 || e->finished)
        return 0;
    int64_t n = code_frame(e, e->pending, out, cap);
    if (n < 0)
        return n;
    if (first_sample)
        *first_sample = e->frame_no * CODEC_FLAC_BLOCK;
    e->frame_no++;
    e->pending = 0;
    e->finished = true;
    return n;
}

uint64_t codec_flac_enc_samples(const codec_flac_enc_t *e) {
    return e ? e->taken : 0;
}
