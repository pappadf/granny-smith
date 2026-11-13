// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// codec.c
// The framework (see codec_internal.h): the codec registry, input, the
// output ring, running a decoder and draining its ring, one-shot decoding
// and encoding, and the result-code text.

#include "codec_internal.h"

#include <stdio.h>
#include <stdlib.h>

// ============================================================================
// Registry
// ============================================================================

typedef struct {
    const char *name;
    const codec_dec_ops_t *dec;
    const codec_enc_ops_t *enc;
} codec_entry_t;

static const codec_entry_t k_codecs[CODEC_COUNT] = {
    [CODEC_COPY] = {"copy",         &codec_copy_dec,       NULL            },
    [CODEC_DEFLATE] = {"deflate",      &codec_deflate_dec,    NULL            },
    [CODEC_ZLIB] = {"zlib",         &codec_zlib_dec,       &codec_zlib_enc },
    [CODEC_GZIP] = {"gzip",         &codec_gzip_dec,       NULL            },
    [CODEC_ADC] = {"adc",          &codec_adc_dec,        NULL            },
    [CODEC_BZIP2] = {"bzip2",        &codec_bzip2_dec,      NULL            },
    [CODEC_LZFSE] = {"lzfse",        &codec_lzfse_dec,      NULL            },
    [CODEC_XZ] = {"xz",           &codec_xz_dec,         &codec_xz_enc   },
    [CODEC_LZMA_ALONE] = {"lzma",         &codec_lzma_alone_dec, NULL            },
    [CODEC_RLE90] = {"rle90",        &codec_rle90_dec,      NULL            },
    [CODEC_RLE90_BINHEX] = {"rle90-binhex", &codec_rle90_dec,      NULL            },
    [CODEC_SIT2] = {"sit2",         &codec_sit2_dec,       NULL            },
    [CODEC_SIT3] = {"sit3",         &codec_sit3_dec,       NULL            },
    [CODEC_SIT13] = {"sit13",        &codec_sit13_dec,      NULL            },
    [CODEC_SIT15] = {"sit15",        &codec_sit15_dec,      NULL            },
    [CODEC_CPT_RLE] = {"cpt-rle",      &codec_cpt_rle_dec,    NULL            },
    [CODEC_CPT_LZH] = {"cpt-lzh",      &codec_cpt_lzh_dec,    NULL            },
    [CODEC_DCMP] = {"dcmp",         &codec_dcmp_dec,       NULL            },
    [CODEC_GSRLE] = {"gsrle",        &codec_gsrle_dec,      &codec_gsrle_enc},
};

const char *codec_name(codec_id_t id) {
    return (unsigned)id < CODEC_COUNT ? k_codecs[id].name : "?";
}

const char *codec_strerror(int64_t rc) {
    if (rc >= 0)
        return "ok";
    switch (rc) {
    case CODEC_ERR_DATA:
        return "malformed compressed data";
    case CODEC_ERR_TRUNCATED:
        return "truncated compressed data";
    case CODEC_ERR_OVERFLOW:
        return "decodes to more than expected";
    case CODEC_ERR_NOMEM:
        return "out of memory";
    case CODEC_ERR_UNSUPPORTED:
        return "unsupported compression feature";
    case CODEC_ERR_CHECKSUM:
        return "checksum mismatch";
    case CODEC_ERR_ARG:
        return "invalid argument";
    case CODEC_ERR_INPUT:
        return "cannot read the compressed input";
    default:
        return "decode error";
    }
}

// ============================================================================
// Input
// ============================================================================

bool codec_in_refill(codec_in_t *in) {
    if (in->pos < in->len)
        return true;
    if (in->eof || !in->pull)
        return false;
    for (;;) {
        int64_t n = in->pull(in->pull_ctx, in->buf, CODEC_IN_CHUNK);
        if (n < 0) {
            in->err = (int)n;
            in->eof = true;
            return false;
        }
        if (n == 0) {
            in->eof = true;
            return false;
        }
        in->base += in->len;
        in->p = in->buf;
        in->pos = 0;
        in->len = (size_t)n;
        return true;
    }
}

size_t codec_in_read(codec_in_t *in, uint8_t *dst, size_t n) {
    size_t got = 0;
    while (got < n) {
        if (in->pos == in->len && !codec_in_refill(in))
            break;
        size_t k = in->len - in->pos < n - got ? in->len - in->pos : n - got;
        memcpy(dst + got, in->p + in->pos, k);
        in->pos += k;
        got += k;
    }
    return got;
}

size_t lsb_read(codec_in_t *in, uint8_t *dst, size_t n) {
    size_t got = 0;
    while (got < n && in->nbits > 0) {
        int b = lsb_byte(in);
        if (b < 0)
            return got;
        dst[got++] = (uint8_t)b;
    }
    if (got < n && in->nbits == 0) {
        in->bits = 0; // the look-ahead bits are about to be read directly
        got += codec_in_read(in, dst + got, n - got);
    }
    return got;
}

// ============================================================================
// Output ring
// ============================================================================

void ring_write(codec_ring_t *r, const uint8_t *src, size_t n) {
    while (n) {
        size_t k = r->size - r->w < n ? r->size - r->w : n;
        memcpy(r->buf + r->w, src, k);
        r->w += k;
        if (r->w == r->size)
            r->w = 0;
        r->total += k;
        src += k;
        n -= k;
    }
}

void ring_fill(codec_ring_t *r, uint8_t b, size_t n) {
    while (n) {
        size_t k = r->size - r->w < n ? r->size - r->w : n;
        memset(r->buf + r->w, b, k);
        r->w += k;
        if (r->w == r->size)
            r->w = 0;
        r->total += k;
        n -= k;
    }
}

void ring_copy_zero(codec_ring_t *r, size_t dist, size_t len) {
    if (dist <= r->total) {
        ring_copy(r, dist, len);
        return;
    }
    // The first dist - total bytes come from before the start: zeros (the
    // ring may be smaller than the format's window, so its own content
    // there is not to be read).
    size_t zeros = (size_t)(dist - r->total) < len ? (size_t)(dist - r->total) : len;
    ring_fill(r, 0, zeros);
    // The rest: from the stream's first byte on, `dist` behind each.
    for (size_t i = zeros; i < len; i++)
        ring_put(r, ring_back(r, dist));
}

// ============================================================================
// Huffman trees
// ============================================================================

void codec_htree_reset(codec_htree_t *p) {
    p->used = 0;
}

int codec_htree_node(codec_htree_t *p) {
    if (p->used >= CODEC_HTREE_CAP)
        return -1;
    int idx = p->used++;
    p->node[idx].ch[0] = -1;
    p->node[idx].ch[1] = -1;
    p->node[idx].sym = CODEC_HTREE_NOSYM;
    return idx;
}

int codec_htree_insert(codec_htree_t *p, int root, uint32_t code, int len, int sym) {
    if (len < 1 || len > 31)
        return -1;
    int cur = root;
    for (int bit = len - 1; bit >= 0; bit--) {
        int b = (int)((code >> bit) & 1);
        if (p->node[cur].ch[b] < 0) {
            int n = codec_htree_node(p);
            if (n < 0)
                return -1;
            p->node[cur].ch[b] = (int16_t)n;
        }
        cur = p->node[cur].ch[b];
    }
    p->node[cur].sym = (int16_t)sym;
    return 0;
}

int codec_htree_build(codec_htree_t *p, const int8_t *lengths, int nsym, int min_len, int max_len) {
    // 0 is "absent" in every format; anything else must be in range.
    for (int s = 0; s < nsym; s++)
        if (lengths[s] != 0 && (lengths[s] < min_len || lengths[s] > max_len))
            return -1;
    int root = codec_htree_node(p);
    if (root < 0)
        return -1;
    uint32_t code = 0;
    for (int len = min_len; len <= max_len; len++, code <<= 1) {
        for (int s = 0; s < nsym; s++) {
            if (lengths[s] != len)
                continue;
            if (len > 0 && codec_htree_insert(p, root, code, len, s) < 0)
                return -1;
            code++;
        }
    }
    return root;
}

int codec_ring_alloc(codec_dec_t *d, size_t window, size_t burst) {
    size_t size = window + 2 * burst;
    if (size < 65536)
        size = 65536; // fewer, longer fills
    // A stream of known length needs no more history than that length
    // (one-shot decoding of a small chunk allocates no large window).
    if (d->out_len < size && d->out_len + burst < size)
        size = (size_t)d->out_len + burst;
    free(d->ring.buf);
    // Zero-filled: a window some formats define as starting at zeros.
    d->ring = (codec_ring_t){.buf = calloc(1, size), .size = size};
    return d->ring.buf ? 0 : CODEC_ERR_NOMEM;
}

int codec_fail(codec_dec_t *d, int rc, const char *msg) {
    if (!d->msg[0]) {
        size_t n = strlen(msg) < sizeof(d->msg) - 1 ? strlen(msg) : sizeof(d->msg) - 1;
        memcpy(d->msg, msg, n);
        d->msg[n] = '\0';
    }
    return rc;
}

int codec_set_size(codec_dec_t *d, uint64_t n) {
    if (d->out_len != CODEC_LEN_UNKNOWN && n > d->out_len)
        return codec_fail(d, CODEC_ERR_OVERFLOW, "the stream states more than the size allowed");
    d->out_len = n;
    d->exact = true;
    return 0;
}

// ============================================================================
// Decoders
// ============================================================================

codec_dec_t *codec_dec_new(codec_id_t id, const codec_dec_opts_t *o, int *err) {
    int e = 0;
    if (!err)
        err = &e;
    *err = CODEC_ERR_ARG;
    if ((unsigned)id >= CODEC_COUNT || !k_codecs[id].dec || !o || (!o->in.pull && !o->in.src && o->in.len))
        return NULL;
    const codec_dec_ops_t *ops = k_codecs[id].dec;
    bool exact = ops->sized || o->exact;
    if (exact && o->out_len == CODEC_LEN_UNKNOWN)
        return NULL;
    if (o->check != CODEC_CHECK_NONE && o->check != CODEC_CHECK_CRC32 && o->check != CODEC_CHECK_CRC16_ARC)
        return NULL;
    codec_dec_t *d = calloc(1, sizeof(*d));
    if (!d) {
        *err = CODEC_ERR_NOMEM;
        return NULL;
    }
    d->ops = ops;
    d->id = id;
    d->out_len = o->out_len;
    d->exact = exact;
    d->check = o->check;
    d->check_value = o->check_value;
    if (o->in.pull) {
        d->in.pull = o->in.pull;
        d->in.pull_ctx = o->in.ctx;
        d->in.buf = malloc(CODEC_IN_CHUNK);
        d->in.p = d->in.buf;
        if (!d->in.buf) {
            codec_dec_free(d);
            *err = CODEC_ERR_NOMEM;
            return NULL;
        }
    } else {
        d->in.p = o->in.src;
        d->in.len = o->in.len;
        d->in.eof = true;
    }
    int rc = 0;
    if (ops->state_size && !(d->st = calloc(1, ops->state_size)))
        rc = CODEC_ERR_NOMEM;
    if (rc == 0 && ops->burst)
        rc = codec_ring_alloc(d, ops->window, ops->burst);
    if (rc == 0 && ops->init)
        rc = ops->init(d);
    if (rc == 0 && !d->ring.buf)
        rc = codec_ring_alloc(d, 0, 1); // until the codec sizes its own
    if (rc != 0) {
        codec_dec_free(d);
        *err = rc;
        return NULL;
    }
    d->in_close = o->in.close; // only now: a failure leaves the input the caller's
    *err = 0;
    return d;
}

void codec_dec_free(codec_dec_t *d) {
    if (!d)
        return;
    if (d->st && d->ops->free)
        d->ops->free(d);
    free(d->st);
    free(d->ring.buf);
    free(d->in.buf);
    if (d->in_close)
        d->in_close(d->in.pull_ctx);
    free(d);
}

uint64_t codec_dec_total_out(const codec_dec_t *d) {
    return d->ring.drained;
}

uint64_t codec_dec_consumed(const codec_dec_t *d) {
    return codec_in_consumed(&d->in);
}

const char *codec_dec_message(const codec_dec_t *d) {
    if (!d)
        return "no decoder";
    if (d->msg[0] || d->status >= 0)
        return d->msg;
    return codec_strerror(d->status);
}

int codec_dec_input_error(const codec_dec_t *d) {
    return d && d->status == CODEC_ERR_INPUT ? d->in.err : 0;
}

// Copy undrained ring bytes to out[*n..cap).
static void drain(codec_dec_t *d, uint8_t *out, size_t cap, size_t *n) {
    codec_ring_t *r = &d->ring;
    while (*n < cap && r->drained < r->total) {
        const uint8_t *p;
        size_t k = ring_span(r, r->drained, &p);
        if (k > cap - *n)
            k = cap - *n;
        memcpy(out + *n, p, k);
        if (d->check == CODEC_CHECK_CRC32)
            d->check_sum = codec_crc32(d->check_sum, out + *n, k);
        else if (d->check == CODEC_CHECK_CRC16_ARC)
            d->check_sum = codec_crc16_arc((uint16_t)d->check_sum, out + *n, k);
        *n += k;
        r->drained += k;
    }
}

int codec_dec_run(codec_dec_t *d, uint8_t *out, size_t cap, size_t *n) {
    size_t got = 0;
    if (n)
        *n = 0;
    if (!d || (!out && cap))
        return CODEC_ERR_ARG;
    for (;;) {
        drain(d, out, cap, &got);
        if (d->ring.drained < d->ring.total) {
            if (n)
                *n = got;
            return 0; // the caller's buffer is full
        }
        if (d->status == CODEC_END && d->check != CODEC_CHECK_NONE) {
            // Everything is out: the stated checksum, over all of it.
            uint32_t want = d->check_value, sum = d->check_sum;
            d->check = CODEC_CHECK_NONE;
            if (sum != want) {
                snprintf(d->msg, sizeof(d->msg), "%s: the decoded bytes' checksum is 0x%X, not the stated 0x%X",
                         codec_name(d->id), (unsigned)sum, (unsigned)want);
                d->status = CODEC_ERR_CHECKSUM;
            }
        }
        if (d->status != 0)
            break; // ended or failed, and everything before it is out
        if (got == cap && cap) {
            if (n)
                *n = got;
            return 0;
        }
        uint64_t in0 = codec_in_consumed(&d->in), out0 = d->ring.total;
        // An exact stream ends at its size, whatever its codec would decode
        // next: every fill stops there (codec_room_for).
        int rc = d->exact && d->ring.total >= d->out_len ? CODEC_END : d->ops->fill(d);
        if (d->in.err) {
            // The input could not be read: that, not what the decoder made
            // of its sudden end, is the cause.  A decoder read through
            // codec_dec_pull fails in its own words and with its own code;
            // any other source is CODEC_ERR_INPUT, its code kept.
            bool inner = d->in.pull == codec_dec_pull;
            const char *why = inner ? codec_dec_message(d->in.pull_ctx) : "cannot read the compressed input";
            d->msg[0] = '\0';
            rc = codec_fail(d, inner ? d->in.err : CODEC_ERR_INPUT, why);
        }
        if (rc >= 0 && d->out_len != CODEC_LEN_UNKNOWN && d->ring.total > d->out_len) {
            // Past the stated size.  An exact stream ends there (its last
            // string or match is cut); any other has decoded too much:
            // deliver up to the size, then fail.
            size_t excess = (size_t)(d->ring.total - d->out_len); // at most one fill, so < size
            d->ring.total -= excess;
            d->ring.w = (d->ring.w + d->ring.size - excess) % d->ring.size;
            rc = d->exact ? CODEC_END
                          : codec_fail(d, CODEC_ERR_OVERFLOW, "the stream decodes to more than its stated size");
        }
        if (rc == CODEC_MORE && d->exact && d->ring.total == d->out_len)
            rc = CODEC_END; // the size reached in this fill: the stream ends with it
        if (rc == CODEC_END && d->exact && d->ring.total != d->out_len)
            rc = codec_fail(d, CODEC_ERR_TRUNCATED, "the stream ends short of its stated size");
        if (rc == CODEC_MORE && d->ring.total == out0 && codec_in_consumed(&d->in) == in0 && !codec_in_overrun(&d->in))
            rc = codec_fail(d, CODEC_ERR_DATA, "the decoder made no progress"); // a guard, not a format rule
        if (rc != CODEC_MORE)
            d->status = rc;
    }
    if (n)
        *n = got;
    return d->status;
}

static void dec_close(void *ctx) {
    codec_dec_free(ctx);
}

codec_dec_t *codec_dec_stack(codec_dec_t *inner, codec_id_t id, const codec_dec_opts_t *opts, int *err) {
    int e = 0;
    if (!err)
        err = &e;
    if (!inner || !opts) {
        codec_dec_free(inner);
        *err = CODEC_ERR_ARG;
        return NULL;
    }
    codec_dec_opts_t o = *opts;
    o.in = (codec_input_t){.pull = codec_dec_pull, .ctx = inner, .close = dec_close};
    codec_dec_t *d = codec_dec_new(id, &o, err);
    if (!d)
        codec_dec_free(inner); // still ours: a failed codec_dec_new closes nothing
    return d;
}

int64_t codec_dec_pull(void *ctx, uint8_t *buf, size_t cap) {
    size_t n = 0;
    int rc = codec_dec_run((codec_dec_t *)ctx, buf, cap, &n);
    if (rc < 0 && n == 0)
        return rc;
    return (int64_t)n;
}

// Past `got` bytes, a stream that has not ended must end now: a one-byte
// probe that finds more is an overflow.  The run's result.
static int expect_end(codec_dec_t *d) {
    uint8_t probe;
    size_t extra = 0;
    int rc = codec_dec_run(d, &probe, 1, &extra);
    return extra ? CODEC_ERR_OVERFLOW : rc;
}

int64_t codec_dec_read(codec_dec_t *d, uint8_t *out, size_t cap) {
    if (!d || (!out && cap))
        return CODEC_ERR_ARG;
    size_t n = 0;
    int rc = codec_dec_run(d, out, cap, &n);
    if (rc == 0)
        rc = expect_end(d); // output full: the stream must end right here
    return rc == CODEC_END ? (int64_t)n : rc;
}

uint8_t *codec_dec_read_alloc(codec_dec_t *d, size_t max, size_t *out_n, int *err) {
    int e = 0;
    if (!err)
        err = &e;
    if (!d || !out_n) {
        *err = CODEC_ERR_ARG;
        return NULL;
    }
    size_t cap = max < 65536 ? max : 65536, n = 0;
    uint8_t *buf = malloc(cap ? cap : 1);
    int rc = buf ? 0 : CODEC_ERR_NOMEM;
    while (rc == 0) {
        if (n == cap) {
            if (cap >= max) {
                rc = expect_end(d); // full at the limit: the stream must end here
                break;
            }
            size_t ncap = cap > max / 2 ? max : cap * 2;
            uint8_t *nb = realloc(buf, ncap);
            if (!nb) {
                rc = CODEC_ERR_NOMEM;
                break;
            }
            buf = nb;
            cap = ncap;
        }
        size_t k = 0;
        rc = codec_dec_run(d, buf + n, cap - n, &k);
        n += k;
    }
    if (rc != CODEC_END) {
        free(buf);
        *err = rc < 0 ? rc : CODEC_ERR_DATA;
        return NULL;
    }
    *out_n = n;
    *err = 0;
    return buf;
}

int64_t codec_decode(codec_id_t id, const uint8_t *src, size_t len, uint8_t *dst, size_t cap) {
    if ((!src && len) || (!dst && cap))
        return CODEC_ERR_ARG;
    codec_dec_opts_t o = {
        .in = {.src = src, .len = len},
          .out_len = cap
    };
    int err = 0;
    codec_dec_t *d = codec_dec_new(id, &o, &err);
    if (!d)
        return err;
    int64_t rc = codec_dec_read(d, dst, cap);
    codec_dec_free(d);
    return rc;
}

uint8_t *codec_decode_alloc(codec_id_t id, const uint8_t *src, size_t len, size_t max, size_t *out_len, int *err) {
    int e = 0;
    if (!err)
        err = &e;
    if (!out_len || (!src && len)) {
        *err = CODEC_ERR_ARG;
        return NULL;
    }
    codec_dec_opts_t o = {
        .in = {.src = src, .len = len},
          .out_len = max
    };
    codec_dec_t *d = codec_dec_new(id, &o, err);
    if (!d)
        return NULL;
    uint8_t *buf = codec_dec_read_alloc(d, max, out_len, err);
    codec_dec_free(d);
    return buf;
}

// ============================================================================
// Encoders
// ============================================================================

codec_enc_t *codec_enc_new(codec_id_t id, size_t max_len, int level, int *err) {
    int e = 0;
    if (!err)
        err = &e;
    *err = CODEC_ERR_ARG;
    if ((unsigned)id >= CODEC_COUNT || !k_codecs[id].enc)
        return NULL;
    codec_enc_t *enc = calloc(1, sizeof(*enc));
    if (!enc) {
        *err = CODEC_ERR_NOMEM;
        return NULL;
    }
    enc->ops = k_codecs[id].enc;
    enc->max_len = max_len;
    enc->level = level < 0 ? enc->ops->default_level : level > 9 ? 9 : level;
    int rc = enc->ops->init(enc, max_len);
    if (rc != 0) {
        codec_enc_free(enc);
        *err = rc;
        return NULL;
    }
    *err = 0;
    return enc;
}

size_t codec_enc_bound(codec_id_t id, size_t len) {
    if ((unsigned)id >= CODEC_COUNT || !k_codecs[id].enc)
        return 0;
    return k_codecs[id].enc->bound(len);
}

int64_t codec_enc_run(codec_enc_t *e, const uint8_t *in, size_t len, uint8_t *out, size_t cap) {
    if (!e || (!in && len) || !out || len > e->max_len)
        return CODEC_ERR_ARG;
    return e->ops->run(e, in, len, out, cap);
}

void codec_enc_free(codec_enc_t *e) {
    if (!e)
        return;
    if (e->st)
        e->ops->free(e);
    free(e);
}

int64_t codec_encode(codec_id_t id, int level, const uint8_t *in, size_t len, uint8_t *out, size_t cap) {
    int err = 0;
    codec_enc_t *e = codec_enc_new(id, len, level, &err);
    if (!e)
        return err;
    int64_t rc = codec_enc_run(e, in, len, out, cap);
    codec_enc_free(e);
    return rc;
}

// ============================================================================
// CODEC_COPY: the bytes as they are
// ============================================================================

static int copy_fill(codec_dec_t *d) {
    codec_ring_t *r = &d->ring;
    while (codec_room_for(d, 1)) {
        if (d->in.pos == d->in.len && !codec_in_refill(&d->in))
            return CODEC_END;
        size_t k = d->in.len - d->in.pos;
        size_t room = ring_room(r);
        if (k > room)
            k = room;
        ring_write(r, d->in.p + d->in.pos, k);
        d->in.pos += k;
    }
    return CODEC_MORE;
}

const codec_dec_ops_t codec_copy_dec = {.burst = 65536, .fill = copy_fill};
