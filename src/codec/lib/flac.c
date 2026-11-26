// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// flac.c
// FLAC decoder (RFC 9639) for the subset flac_enc.c writes, and STREAMINFO
// parsing.  See codec_flac.h.
//
// One frame at a time from memory: the header (sync code, block size,
// sample rate, channel assignment, sample size, the UTF-8 coded frame or
// sample number, the deferred block size and rate, CRC-8), a subframe per
// channel -- CONSTANT, VERBATIM or FIXED order 0..4 with wasted bits and
// partitioned Rice residuals (4- or 5-bit parameters, escaped partitions
// too) -- the stereo decorrelation, and the CRC-16.  Frames come from
// untrusted files: every count is checked against the block and the
// input, and arithmetic on corrupt values stays in 64 bits.

#include "codec_flac.h"
#include "codec_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Constants and types
// ============================================================================

#define FLAC_SYNC_CODE 0xFFF8u // 14-bit sync and the reserved bit (the blocking bit masked off)
#define FLAC_MAX_BLOCK 65536 // a frame's largest block size
#define FLAC_MAX_CH    2

// The decoder: one block per channel and what the last header said.
struct codec_flac_dec {
    int channels;
    uint32_t rate;
    uint64_t number;
    int32_t ch[FLAC_MAX_CH][FLAC_MAX_BLOCK];
    char msg[64];
};

// MSB-first reader over one frame's bytes; reading past the end sets `over`.
typedef struct {
    const uint8_t *p;
    size_t len, pos; // bytes, and the next byte to load
    uint64_t acc; // loaded bits, the next at the top of the low `n`
    int n;
    bool over;
} br_t;

// ============================================================================
// Static helpers
// ============================================================================

// Fail the frame with `rc` and a message.
static int64_t dfail(codec_flac_dec_t *d, int rc, const char *msg) {
    snprintf(d->msg, sizeof(d->msg), "flac: %s", msg);
    return rc;
}

// The next `bits` (<= 32) bits, unsigned.
static uint32_t get(br_t *r, int bits) {
    if (bits == 0)
        return 0;
    while (r->n < bits) {
        uint8_t b = 0;
        if (r->pos < r->len)
            b = r->p[r->pos];
        else
            r->over = true;
        r->pos++;
        r->acc = r->acc << 8 | b;
        r->n += 8;
    }
    r->n -= bits;
    return (uint32_t)(r->acc >> r->n) & (uint32_t)((1ull << bits) - 1);
}

// The next `bits` (1..32) bits, two's complement.
static int32_t get_signed(br_t *r, int bits) {
    uint32_t v = get(r, bits);
    if (bits < 32 && (v >> (bits - 1)) & 1)
        v |= ~0u << bits; // extend the sign
    return (int32_t)v;
}

// A unary count (zeros before a one), or UINT32_MAX past `limit` zeros.
static uint32_t get_unary(br_t *r, uint32_t limit) {
    uint32_t q = 0;
    while (!get(r, 1)) {
        if (++q > limit || r->over)
            return UINT32_MAX;
    }
    return q;
}

// Bytes read so far (whole bytes: call at a byte boundary).
static size_t br_bytes(const br_t *r) {
    return r->pos - (size_t)(r->n / 8);
}

// The frame or sample number in its UTF-8-like form; false if malformed.
static bool get_utf8(br_t *r, uint64_t *v) {
    uint32_t lead = get(r, 8);
    if (lead < 0x80) {
        *v = lead;
        return true;
    }
    int bytes = 0;
    while (bytes < 8 && (lead << bytes & 0x80))
        bytes++;
    if (bytes < 2 || bytes > 7)
        return false; // a lone continuation byte, or 0xFF
    uint64_t x = bytes == 7 ? 0 : lead & (0x7Fu >> bytes);
    for (int i = 1; i < bytes; i++) {
        uint32_t c = get(r, 8);
        if ((c & 0xC0) != 0x80)
            return false;
        x = x << 6 | (c & 0x3F);
    }
    *v = x;
    return true;
}

// The residual of a FIXED subframe into x[order..n).  0 or a code.
static int64_t read_residual(codec_flac_dec_t *d, br_t *r, int32_t *x, size_t n, int order) {
    uint32_t method = get(r, 2);
    if (method > 1)
        return dfail(d, CODEC_ERR_DATA, "reserved residual coding method");
    int pbits = method ? 5 : 4;
    uint32_t escape = method ? 31 : 15;
    int porder = (int)get(r, 4);
    size_t parts = (size_t)1 << porder, len = n >> porder;
    if ((n & (parts - 1)) || len < (size_t)order)
        return dfail(d, CODEC_ERR_DATA, "partition order does not fit the block");
    for (size_t j = 0; j < parts; j++) {
        uint32_t k = get(r, pbits);
        size_t from = j == 0 ? (size_t)order : j * len, to = (j + 1) * len;
        if (k == escape) {
            // Escaped: the partition's residuals as raw signed values.
            int bits = (int)get(r, 5);
            for (size_t i = from; i < to; i++)
                x[i] = bits ? get_signed(r, bits) : 0;
        } else {
            for (size_t i = from; i < to; i++) {
                // Large enough to hold any 32-bit residual, small enough
                // that a corrupt run of zeros stops early.
                uint32_t q = get_unary(r, k >= 32 ? 0 : (UINT32_MAX >> k) >> 1);
                if (q == UINT32_MAX)
                    return dfail(d, r->over ? CODEC_ERR_TRUNCATED : CODEC_ERR_DATA, "Rice code out of range");
                uint32_t u = q << k | get(r, (int)k);
                // Unfold: 0, 1, 2, 3 ... -> 0, -1, 1, -2 ...
                x[i] = (int32_t)((u >> 1) ^ (0u - (u & 1)));
            }
        }
        if (r->over)
            return dfail(d, CODEC_ERR_TRUNCATED, "frame cut short in its residual");
    }
    return 0;
}

// One subframe of `n` samples at `bps` bits into x.  0 or a code.
static int64_t read_subframe(codec_flac_dec_t *d, br_t *r, int32_t *x, size_t n, int bps) {
    uint32_t head = get(r, 8);
    if (head & 0x80)
        return dfail(d, CODEC_ERR_DATA, "subframe padding bit set");
    uint32_t type = head >> 1 & 0x3F;
    int wasted = 0;
    if (head & 1) {
        uint32_t q = get_unary(r, (uint32_t)bps);
        if (q == UINT32_MAX || (int)q + 1 >= bps)
            return dfail(d, CODEC_ERR_DATA, "too many wasted bits");
        wasted = (int)q + 1;
    }
    int ebps = bps - wasted;
    if (type == 0) {
        int32_t v = get_signed(r, ebps);
        for (size_t i = 0; i < n; i++)
            x[i] = v;
    } else if (type == 1) {
        for (size_t i = 0; i < n; i++)
            x[i] = get_signed(r, ebps);
    } else if (type >= 8 && type <= 12) {
        int order = (int)type - 8;
        if ((size_t)order > n)
            return dfail(d, CODEC_ERR_DATA, "predictor order beyond the block");
        for (int i = 0; i < order; i++)
            x[i] = get_signed(r, ebps);
        int64_t rc = read_residual(d, r, x, n, order);
        if (rc < 0)
            return rc;
        // Undo the predictor in 64 bits, so corrupt input cannot overflow.
        for (size_t i = (size_t)order; i < n; i++) {
            int64_t p = 0;
            switch (order) {
            case 1:
                p = x[i - 1];
                break;
            case 2:
                p = 2 * (int64_t)x[i - 1] - x[i - 2];
                break;
            case 3:
                p = 3 * (int64_t)x[i - 1] - 3 * (int64_t)x[i - 2] + x[i - 3];
                break;
            case 4:
                p = 4 * (int64_t)x[i - 1] - 6 * (int64_t)x[i - 2] + 4 * (int64_t)x[i - 3] - x[i - 4];
                break;
            }
            x[i] = (int32_t)(uint32_t)(uint64_t)(p + x[i]);
        }
    } else if (type >= 32) {
        return dfail(d, CODEC_ERR_UNSUPPORTED, "LPC subframes are not decoded");
    } else {
        return dfail(d, CODEC_ERR_DATA, "reserved subframe type");
    }
    if (r->over)
        return dfail(d, CODEC_ERR_TRUNCATED, "frame cut short in a subframe");
    if (wasted)
        for (size_t i = 0; i < n; i++)
            x[i] = (int32_t)((uint32_t)x[i] << wasted);
    return 0;
}

// ============================================================================
// Lifecycle
// ============================================================================

codec_flac_dec_t *codec_flac_dec_new(int *err) {
    codec_flac_dec_t *d = calloc(1, sizeof(*d));
    if (err)
        *err = d ? 0 : CODEC_ERR_NOMEM;
    return d;
}

void codec_flac_dec_free(codec_flac_dec_t *d) {
    free(d);
}

// ============================================================================
// Operations
// ============================================================================

int codec_flac_parse_streaminfo(const uint8_t *p, size_t len, codec_flac_streaminfo_t *out) {
    if (!p || !out || len < CODEC_FLAC_STREAMINFO)
        return CODEC_ERR_DATA;
    out->min_block = codec_be16(p);
    out->max_block = codec_be16(p + 2);
    out->min_frame = (uint32_t)p[4] << 16 | (uint32_t)p[5] << 8 | p[6];
    out->max_frame = (uint32_t)p[7] << 16 | (uint32_t)p[8] << 8 | p[9];
    uint32_t v = codec_be32(p + 10);
    out->sample_rate = v >> 12;
    out->channels = (int)(v >> 9 & 7) + 1;
    out->bits = (int)(v >> 4 & 31) + 1;
    out->total_samples = (uint64_t)(v & 15) << 32 | codec_be32(p + 14);
    memcpy(out->md5, p + 18, 16);
    return 0;
}

int64_t codec_flac_dec_frame(codec_flac_dec_t *d, const uint8_t *frame, size_t len, int16_t *out, size_t cap,
                             size_t *used) {
    static const uint32_t rates[12] = {0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
    if (!d || (!frame && len) || (!out && cap))
        return CODEC_ERR_ARG;
    d->msg[0] = '\0';
    br_t r = {.p = frame, .len = len};
    if ((get(&r, 16) & 0xFFFE) != FLAC_SYNC_CODE)
        return r.over ? dfail(d, CODEC_ERR_TRUNCATED, "frame cut short") : dfail(d, CODEC_ERR_DATA, "no frame sync");
    uint32_t bs_code = get(&r, 4), rate_code = get(&r, 4), assign = get(&r, 4), ss = get(&r, 3);
    if (get(&r, 1))
        return dfail(d, CODEC_ERR_DATA, "reserved header bit set");
    if (ss != 0 && ss != 4)
        return dfail(d, CODEC_ERR_UNSUPPORTED, "sample size other than 16 bits");
    if (assign > 10)
        return dfail(d, CODEC_ERR_DATA, "reserved channel assignment");
    int channels = assign < 8 ? (int)assign + 1 : 2;
    if (channels > FLAC_MAX_CH)
        return dfail(d, CODEC_ERR_UNSUPPORTED, "more than two channels");
    uint64_t number;
    if (!get_utf8(&r, &number))
        return dfail(d, CODEC_ERR_DATA, "bad frame number");
    // The block size: by code, or after the header.
    size_t n;
    if (bs_code == 0)
        return dfail(d, CODEC_ERR_DATA, "reserved block size");
    else if (bs_code == 1)
        n = 192;
    else if (bs_code <= 5)
        n = (size_t)576 << (bs_code - 2);
    else if (bs_code == 6)
        n = get(&r, 8) + 1;
    else if (bs_code == 7)
        n = get(&r, 16) + 1;
    else
        n = (size_t)256 << (bs_code - 8);
    uint32_t rate = 0;
    if (rate_code == 15)
        return dfail(d, CODEC_ERR_DATA, "invalid sample rate code");
    else if (rate_code < 12)
        rate = rates[rate_code];
    else if (rate_code == 12)
        rate = get(&r, 8) * 1000;
    else if (rate_code == 13)
        rate = get(&r, 16);
    else
        rate = get(&r, 16) * 10;
    size_t hlen = br_bytes(&r);
    uint32_t crc8 = get(&r, 8);
    if (r.over)
        return dfail(d, CODEC_ERR_TRUNCATED, "frame header cut short");
    if (crc8 != codec_crc8_smbus(0, frame, hlen))
        return dfail(d, CODEC_ERR_CHECKSUM, "header CRC-8 mismatch");
    if (n > FLAC_MAX_BLOCK)
        return dfail(d, CODEC_ERR_DATA, "block too large");
    if (n > cap)
        return dfail(d, CODEC_ERR_OVERFLOW, "frame larger than the buffer");
    // Subframes: the side channel has one more bit.
    for (int c = 0; c < channels; c++) {
        int bps = 16 + ((assign == 8 && c == 1) || (assign == 9 && c == 0) || (assign == 10 && c == 1));
        int64_t rc = read_subframe(d, &r, d->ch[c], n, bps);
        if (rc < 0)
            return rc;
    }
    // Footer: zero padding, then the CRC-16 of everything before it.
    get(&r, r.n % 8);
    size_t flen = br_bytes(&r);
    uint32_t crc16 = get(&r, 16);
    if (r.over)
        return dfail(d, CODEC_ERR_TRUNCATED, "frame cut short");
    if (crc16 != codec_crc16_umts(0, frame, flen))
        return dfail(d, CODEC_ERR_CHECKSUM, "frame CRC-16 mismatch");
    // Undo the decorrelation, interleaving as it goes.
    for (size_t i = 0; i < n; i++) {
        int64_t a = d->ch[0][i], b = channels == 2 ? d->ch[1][i] : 0;
        int64_t left = a, right = b;
        if (assign == 8) {
            right = a - b; // left, side
        } else if (assign == 9) {
            left = a + b; // side, right
        } else if (assign == 10) {
            int64_t mid = a * 2 + (b & 1); // mid, side
            left = (mid + b) >> 1;
            right = (mid - b) >> 1;
        }
        out[i * (size_t)channels] = (int16_t)left;
        if (channels == 2)
            out[i * 2 + 1] = (int16_t)right;
    }
    d->channels = channels;
    d->rate = rate;
    d->number = number;
    if (used)
        *used = flen + 2;
    return (int64_t)n;
}

int codec_flac_dec_channels(const codec_flac_dec_t *d) {
    return d ? d->channels : 0;
}

uint32_t codec_flac_dec_sample_rate(const codec_flac_dec_t *d) {
    return d ? d->rate : 0;
}

uint64_t codec_flac_dec_frame_number(const codec_flac_dec_t *d) {
    return d ? d->number : 0;
}

const char *codec_flac_dec_message(const codec_flac_dec_t *d) {
    return d ? d->msg : "no decoder";
}
