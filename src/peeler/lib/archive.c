// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// archive.c
// Structure-first archive access (peeler.h): the format registry, peel_open,
// the entry accessors, and peel_open_fork with its two shapes -- a stored
// fork is a view of the archive's source, a compressed one a decode-through
// source that fills the caller's sink only as far as reads reach.

#include "internal.h"

#include <inttypes.h>

// ============================================================================
// Registry
// ============================================================================

// Detection order: wrappers first, so an outer encoding is stripped before
// anything probes for archive signatures inside it.
static const peel_fmt_t *const g_fmts[] = {
    &peel_fmt_hqx, &peel_fmt_bin, &peel_fmt_gz, &peel_fmt_sit, &peel_fmt_cpt, &peel_fmt_zip,
};
#define N_FMTS ((int)(sizeof(g_fmts) / sizeof(g_fmts[0])))

static peel_format_desc_t g_descs[N_FMTS];

const peel_format_desc_t *peel_formats(int *count) {
    for (int i = 0; i < N_FMTS; i++)
        g_descs[i] = g_fmts[i]->desc;
    if (count)
        *count = N_FMTS;
    return g_descs;
}

// The vtable whose detect() accepts `p`, or NULL.
static const peel_fmt_t *fmt_for_probe(const peel_probe_t *p) {
    for (int i = 0; i < N_FMTS; i++)
        if (g_fmts[i]->desc.detect(p))
            return g_fmts[i];
    return NULL;
}

const peel_format_desc_t *peel_identify(const peel_probe_t *p) {
    const peel_fmt_t *f = fmt_for_probe(p);
    if (!f)
        return NULL;
    int n = 0;
    const peel_format_desc_t *d = peel_formats(&n);
    for (int i = 0; i < n; i++)
        if (g_fmts[i] == f)
            return &d[i];
    return NULL;
}

// ============================================================================
// Open / close
// ============================================================================

static peel_archive_t *open_with(const peel_fmt_t *fmt, peel_source_t *src, const peel_probe_t *p,
                                 const peel_sink_ops_t *sink, void *sink_ctx, peel_err_t **err) {
    peel_archive_t *a = calloc(1, sizeof(*a));
    if (!a) {
        *err = make_err("out of memory opening an archive");
        return NULL;
    }
    a->fmt = fmt;
    a->src = peel_source_retain(src);
    a->sink = sink ? sink : peel_heap_sink();
    a->sink_ctx = sink_ctx;
    a->refs = 1;
    if (fmt->open(a, p, err) != 0) {
        if (!*err)
            *err = make_err("%s: cannot parse the archive", fmt->desc.name);
        peel_close(a);
        return NULL;
    }
    return a;
}

peel_archive_t *peel_open(peel_source_t *src, const peel_sink_ops_t *sink, void *sink_ctx, peel_err_t **err) {
    *err = NULL;
    peel_probe_t p;
    if (peel_probe_init(&p, src) != 0) {
        *err = make_err("cannot read '%s'", peel_source_key(src));
        return NULL;
    }
    const peel_fmt_t *fmt = fmt_for_probe(&p);
    peel_archive_t *a = NULL;
    if (!fmt)
        *err = make_err("not a recognised archive");
    else
        a = open_with(fmt, src, &p, sink, sink_ctx, err);
    peel_probe_free(&p);
    return a;
}

peel_archive_t *peel_open_as(const char *format, peel_source_t *src, const peel_sink_ops_t *sink, void *sink_ctx,
                             peel_err_t **err) {
    *err = NULL;
    const peel_fmt_t *fmt = NULL;
    for (int i = 0; i < N_FMTS && !fmt; i++)
        if (strcmp(g_fmts[i]->desc.name, format) == 0)
            fmt = g_fmts[i];
    if (!fmt) {
        *err = make_err("unknown format '%s'", format);
        return NULL;
    }
    peel_probe_t p;
    if (peel_probe_init(&p, src) != 0) {
        *err = make_err("cannot read '%s'", peel_source_key(src));
        return NULL;
    }
    peel_archive_t *a = open_with(fmt, src, &p, sink, sink_ctx, err);
    peel_probe_free(&p);
    return a;
}

// Drop one reference; the last frees everything.
static void archive_unref(peel_archive_t *a) {
    if (!a || --a->refs > 0)
        return;
    if (a->fmt && a->fmt->close)
        a->fmt->close(a);
    peel_source_release(a->src);
    free(a->entries);
    free(a);
}

void peel_close(peel_archive_t *a) {
    archive_unref(a);
}

// ============================================================================
// Accessors
// ============================================================================

const char *peel_format(const peel_archive_t *a) {
    return a ? a->fmt->desc.name : NULL;
}

bool peel_is_wrapper(const peel_archive_t *a) {
    return a && a->fmt->desc.is_wrapper;
}

int peel_count(const peel_archive_t *a) {
    return a ? a->count : 0;
}

const peel_entry_t *peel_entry(const peel_archive_t *a, int i) {
    return (a && i >= 0 && i < a->count) ? &a->entries[i] : NULL;
}

int peel_lookup(const peel_archive_t *a, const char *path) {
    if (!a || !path)
        return -1;
    while (*path == '/')
        path++;
    size_t pl = strlen(path);
    while (pl > 0 && path[pl - 1] == '/')
        pl--;
    for (int i = 0; i < a->count; i++)
        if (strlen(a->entries[i].path) == pl && strncmp(a->entries[i].path, path, pl) == 0)
            return i;
    return -1;
}

peel_entry_t *peel_archive_add(peel_archive_t *a) {
    if (a->count == a->cap) {
        int ncap = a->cap ? a->cap * 2 : 16;
        peel_entry_t *ne = realloc(a->entries, (size_t)ncap * sizeof(*ne));
        if (!ne)
            return NULL;
        a->entries = ne;
        a->cap = ncap;
    }
    peel_entry_t *e = &a->entries[a->count++];
    memset(e, 0, sizeof(*e));
    e->data_off = e->rsrc_off = UINT64_MAX;
    return e;
}

uint8_t *peel_archive_read(peel_archive_t *a, uint64_t off, uint64_t len, const char *what, peel_err_t **err) {
    uint64_t size = peel_source_size(a->src);
    if (off > size || len > size - off) {
        *err = make_err("%s extends past archive end", what);
        return NULL;
    }
    if (len > PEEL_MAX_INPUT) {
        *err = make_err("%s is %" PRIu64 " bytes, over the %u MiB limit", what, len, (unsigned)(PEEL_MAX_INPUT >> 20));
        return NULL;
    }
    uint8_t *buf = malloc(len ? (size_t)len : 1);
    if (!buf) {
        *err = make_err("out of memory reading %s", what);
        return NULL;
    }
    if (len && peel_source_read_exact(a->src, off, buf, (size_t)len) != 0) {
        free(buf);
        *err = make_err("cannot read %s", what);
        return NULL;
    }
    return buf;
}

void peel_fork_key(peel_archive_t *a, int i, int fork, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s%s", peel_source_key(a->src), a->entries[i].path, fork == PEEL_FORK_RSRC ? "/rsrc" : "");
}

// ============================================================================
// Whole-fork producer: decode() once, then emit
// ============================================================================

typedef struct {
    peel_producer_t base;
    peel_archive_t *a;
    int entry, fork;
    peel_buf_t buf;
    size_t pos;
    bool decoded;
} whole_producer_t;

static int whole_run(peel_producer_t *pp, uint8_t *out, size_t cap, size_t *n) {
    whole_producer_t *w = (whole_producer_t *)pp;
    *n = 0;
    if (!w->decoded) {
        peel_err_t *err = NULL;
        w->buf = w->a->fmt->decode(w->a, w->entry, w->fork, &err);
        if (err) {
            snprintf(pp->err, sizeof(pp->err), "%s", peel_err_msg(err));
            peel_err_free(err);
            return -5;
        }
        w->decoded = true;
    }
    size_t left = w->buf.size - w->pos;
    size_t k = left < cap ? left : cap;
    if (k)
        memcpy(out, w->buf.data + w->pos, k);
    w->pos += k;
    *n = k;
    if (w->pos == w->buf.size) {
        peel_free(&w->buf); // everything handed to the sink
        return 1;
    }
    return 0;
}

static void whole_free(peel_producer_t *pp) {
    whole_producer_t *w = (whole_producer_t *)pp;
    peel_free(&w->buf);
    archive_unref(w->a);
    free(w);
}

// ============================================================================
// Decode-through source
// ============================================================================

typedef struct {
    peel_archive_t *a; // for the sink ops; retained
    peel_producer_t *prod; // NULL once finished
    peel_source_t *sink; // created on first read
    char *key;
    uint64_t len; // declared unpacked length (a hint until committed when !exact)
    uint64_t produced; // bytes written to the sink so far
    bool exact; // len is the fork's true length
    peel_tier_t tier; // until committed
    bool committed;
    bool failed;
} decode_src_t;

// Drive the producer until the sink holds `want` bytes or the fork ends.
static int decode_fill(decode_src_t *d, uint64_t want) {
    if (d->failed)
        return -5;
    if (!d->sink) {
        d->sink = d->a->sink->create(d->a->sink_ctx, d->key, d->exact ? d->len : PEEL_SIZE_UNKNOWN);
        if (!d->sink) {
            d->failed = true;
            return -12;
        }
    }
    uint8_t chunk[65536];
    while (!d->committed && d->produced < want) {
        size_t n = 0;
        int rc = d->prod->run(d->prod, chunk, sizeof(chunk), &n);
        if (rc < 0 || (d->exact && d->produced + n > d->len)) {
            d->failed = true; // a corrupt fork, or one longer than it declared
            return -5;
        }
        if (n && d->a->sink->write(d->sink, d->produced, chunk, n) != (int64_t)n) {
            d->failed = true;
            return -5;
        }
        d->produced += n;
        if (rc == 1) {
            if (!d->exact)
                d->len = d->produced; // the size, earned
            else if (d->produced != d->len) {
                d->failed = true; // the fork ran dry before its declared length
                return -5;
            }
            d->a->sink->commit(d->sink);
            d->committed = true;
            d->prod->free(d->prod);
            d->prod = NULL;
        }
    }
    return 0;
}

static int64_t decode_read(peel_source_t *s, uint64_t off, void *buf, size_t len) {
    decode_src_t *d = s->ctx;
    if (!d->exact && !d->committed) {
        // The length is not known yet: decode as far as the read reaches.
        uint64_t want = len > UINT64_MAX - off ? UINT64_MAX : off + len;
        int rc = decode_fill(d, want);
        if (rc != 0)
            return rc;
        if (!d->committed) {
            if (off >= d->produced)
                return 0;
            if (len > d->produced - off)
                len = (size_t)(d->produced - off);
            return peel_source_read(d->sink, off, buf, len);
        }
    }
    if (off >= d->len)
        return 0;
    if (len > d->len - off)
        len = (size_t)(d->len - off);
    int rc = decode_fill(d, off + len);
    if (rc != 0)
        return rc;
    return peel_source_read(d->sink, off, buf, len);
}

static uint64_t decode_size(peel_source_t *s) {
    decode_src_t *d = s->ctx;
    // An unsized fork earns its length with one full pass; a failed one
    // reports what it produced, and its reads fail.
    if (!d->exact && !d->committed && decode_fill(d, UINT64_MAX) != 0)
        return d->produced;
    return d->len;
}

static const char *decode_key(peel_source_t *s) {
    return ((decode_src_t *)s->ctx)->key;
}

static peel_tier_t decode_tier(peel_source_t *s) {
    decode_src_t *d = s->ctx;
    return d->committed ? PEEL_TIER_RANDOM : d->tier;
}

static void decode_close(peel_source_t *s) {
    decode_src_t *d = s->ctx;
    if (!d)
        return;
    if (d->prod)
        d->prod->free(d->prod);
    peel_source_release(d->sink);
    archive_unref(d->a);
    free(d->key);
    free(d);
}

static const peel_source_ops_t decode_ops = {decode_read, decode_size, decode_key, decode_tier, decode_close};

// Both constructors.
static peel_source_t *decode_source_new(peel_archive_t *a, const char *key, uint64_t len, bool exact, peel_tier_t tier,
                                        peel_producer_t *p) {
    decode_src_t *d = calloc(1, sizeof(*d));
    size_t kl = strlen(key);
    char *k = malloc(kl + 1);
    if (!d || !k) {
        free(d);
        free(k);
        p->free(p);
        return NULL;
    }
    memcpy(k, key, kl + 1);
    a->refs++;
    d->a = a;
    d->prod = p;
    d->key = k;
    d->len = len;
    d->exact = exact;
    d->tier = tier;
    return peel_source_new(&decode_ops, d, NULL);
}

peel_source_t *peel_decode_source(peel_archive_t *a, const char *key, uint64_t len, peel_tier_t tier,
                                  peel_producer_t *p) {
    return decode_source_new(a, key, len, true, tier, p);
}

peel_source_t *peel_decode_source_unsized(peel_archive_t *a, const char *key, uint64_t hint, peel_tier_t tier,
                                          peel_producer_t *p) {
    return decode_source_new(a, key, hint, false, tier, p);
}

// ============================================================================
// Forks
// ============================================================================

peel_source_t *peel_open_fork(peel_archive_t *a, int i, int fork, peel_err_t **err) {
    *err = NULL;
    const peel_entry_t *e = peel_entry(a, i);
    if (!e || e->is_dir) {
        *err = make_err("no such file in the archive");
        return NULL;
    }
    if (a->fmt->prepare && a->fmt->prepare(a, i, fork, err) != 0)
        return NULL;
    char key[1024];
    peel_fork_key(a, i, fork, key, sizeof(key));
    uint64_t len = fork == PEEL_FORK_RSRC ? e->rsrc_len : e->data_len;
    uint64_t off = fork == PEEL_FORK_RSRC ? e->rsrc_off : e->data_off;
    peel_tier_t tier = fork == PEEL_FORK_RSRC ? e->rsrc_tier : e->data_tier;
    bool exact = !a->fmt->len_exact || a->fmt->len_exact(a, i, fork);
    if (len == 0 && exact)
        return peel_source_memory_keyed(NULL, 0, false, key);
    if (a->fmt->open_fork) {
        peel_source_t *own = a->fmt->open_fork(a, i, fork, err);
        if (own || *err)
            return own;
    }
    // A stored fork is a window of the archive: nothing to decode.
    if (tier == PEEL_TIER_RANDOM && off != UINT64_MAX)
        return peel_source_view_keyed(a->src, off, len, key);
    peel_producer_t *p = NULL;
    if (a->fmt->producer) {
        p = a->fmt->producer(a, i, fork, err);
        if (!p && *err)
            return NULL;
    }
    if (!p) {
        whole_producer_t *w = calloc(1, sizeof(*w));
        if (!w) {
            *err = make_err("out of memory opening a fork");
            return NULL;
        }
        w->base.run = whole_run;
        w->base.free = whole_free;
        a->refs++;
        w->a = a;
        w->entry = i;
        w->fork = fork;
        p = &w->base;
    }
    peel_source_t *s = decode_source_new(a, key, len, exact, tier, p);
    if (!s)
        *err = make_err("out of memory opening a fork");
    return s;
}

peel_buf_t peel_read_fork(peel_archive_t *a, int i, int fork, peel_err_t **err) {
    *err = NULL;
    const peel_entry_t *e = peel_entry(a, i);
    if (!e || e->is_dir) {
        *err = make_err("no such file in the archive");
        return (peel_buf_t){0};
    }
    if ((fork == PEEL_FORK_RSRC ? e->rsrc_len : e->data_len) == 0)
        return (peel_buf_t){0};
    return a->fmt->decode(a, i, fork, err);
}
