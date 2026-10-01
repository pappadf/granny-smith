// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_chunkmap.c
// NDIF and UDIF images as chunk-mapped sources.  See image_chunkmap.h.

#include "image_chunkmap.h"

#include "chunk_cache.h"
#include "image_ndif.h"
#include "image_udif.h"
#include "log.h"
#include "resource_fork.h"
#include "storage_util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("image")

#define CM_SECTOR 512u

// ============================================================================
// The generic chunk map
// ============================================================================

typedef enum { RUN_ZERO, RUN_RAW, RUN_NDIF, RUN_UDIF } run_kind_t;

// One run of the decoded image.
typedef struct {
    uint64_t out_off, out_len; // bytes of the decoded image
    uint64_t in_off, in_len; // bytes of the data fork
    run_kind_t kind;
    uint32_t type; // the format's own chunk type (NDIF / UDIF)
} cm_run_t;

typedef struct {
    gs_source_t *data; // the data fork (retained)
    cm_run_t *runs; // sorted by out_off, non-overlapping
    size_t n_runs, cap_runs;
    uint64_t size; // decoded image size
    char *key;
    const char *what; // "NDIF" / "UDIF", for messages
} cm_src_t;

static int cm_add(cm_src_t *m, cm_run_t r) {
    if (m->n_runs == m->cap_runs) {
        size_t ncap = m->cap_runs ? m->cap_runs * 2 : 64;
        cm_run_t *nr = realloc(m->runs, ncap * sizeof(*nr));
        if (!nr)
            return -ENOMEM;
        m->runs = nr;
        m->cap_runs = ncap;
    }
    m->runs[m->n_runs++] = r;
    return 0;
}

static int cm_cmp(const void *a, const void *b) {
    const cm_run_t *x = a, *y = b;
    return x->out_off < y->out_off ? -1 : x->out_off > y->out_off;
}

// Sort the runs and refuse overlaps.  0 or -EINVAL.
static int cm_finish(cm_src_t *m) {
    qsort(m->runs, m->n_runs, sizeof(*m->runs), cm_cmp);
    for (size_t i = 1; i < m->n_runs; i++)
        if (m->runs[i - 1].out_off + m->runs[i - 1].out_len > m->runs[i].out_off)
            return -EINVAL;
    return 0;
}

// The index of the run holding `off`, or the first run after it.
static size_t cm_find(const cm_src_t *m, uint64_t off) {
    size_t lo = 0, hi = m->n_runs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (m->runs[mid].out_off + m->runs[mid].out_len <= off)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

typedef struct {
    cm_src_t *m;
    const cm_run_t *run;
} cm_fetch_t;

// Decode one compressed run for the chunk cache.
static int64_t cm_fetch(void *ctx, uint64_t idx, uint8_t *buf, size_t cap) {
    (void)idx;
    cm_fetch_t *f = ctx;
    const cm_run_t *r = f->run;
    uint8_t *in = malloc(r->in_len ? (size_t)r->in_len : 1);
    if (!in)
        return -ENOMEM;
    int rc = r->in_len ? gs_source_read_exact(f->m->data, r->in_off, in, (size_t)r->in_len) : 0;
    if (rc == 0) {
        if (r->kind == RUN_NDIF) {
            ndif_chunk_t c = {.sector = 0,
                              .count = (uint32_t)(r->out_len / CM_SECTOR),
                              .type = (uint8_t)r->type,
                              .offset = 0,
                              .length = (uint32_t)r->in_len};
            rc = ndif_decode_chunk(&c, in, (size_t)r->in_len, buf, cap);
        } else {
            udif_chunk_t c = {
                .type = r->type, .sector = 0, .count = r->out_len / CM_SECTOR, .offset = 0, .length = r->in_len};
            rc = udif_decode_chunk(&c, in, (size_t)r->in_len, buf, cap);
        }
        if (rc != 0)
            LOG(1, "%s chunk at %llu (type %#x) failed to decode: %d", f->m->what, (unsigned long long)r->out_off,
                r->type, rc);
    }
    free(in);
    return rc != 0 ? (rc < 0 ? rc : -EIO) : (int64_t)r->out_len;
}

static int64_t cm_read(gs_source_t *s, uint64_t off, void *buf, size_t len) {
    cm_src_t *m = s->ctx;
    if (off >= m->size)
        return 0;
    if (len > m->size - off)
        len = (size_t)(m->size - off);
    uint8_t *out = buf;
    size_t done = 0;
    size_t i = cm_find(m, off);
    while (done < len) {
        uint64_t at = off + done;
        const cm_run_t *r = i < m->n_runs ? &m->runs[i] : NULL;
        if (!r || at < r->out_off) {
            // A gap no run covers reads as zeros, up to the next run.
            uint64_t gap_end = r ? r->out_off : m->size;
            size_t n = gap_end - at < len - done ? (size_t)(gap_end - at) : len - done;
            memset(out + done, 0, n);
            done += n;
            continue;
        }
        uint64_t in = at - r->out_off;
        size_t n = r->out_len - in < len - done ? (size_t)(r->out_len - in) : len - done;
        if (r->kind == RUN_ZERO) {
            memset(out + done, 0, n);
        } else if (r->kind == RUN_RAW) {
            int rc = gs_source_read_exact(m->data, r->in_off + in, out + done, n);
            if (rc != 0)
                return done ? (int64_t)done : rc;
        } else {
            cm_fetch_t f = {m, r};
            int64_t got = gs_chunk_cache_get(gs_chunk_cache_default(), m->key, i, (size_t)r->out_len, in, out + done, n,
                                             cm_fetch, &f);
            if (got < 0)
                return done ? (int64_t)done : got;
            if ((size_t)got != n)
                return done ? (int64_t)done : -EIO;
        }
        done += n;
        i++;
    }
    return (int64_t)done;
}

static uint64_t cm_size(gs_source_t *s) {
    return ((cm_src_t *)s->ctx)->size;
}

static const char *cm_key(gs_source_t *s) {
    return ((cm_src_t *)s->ctx)->key;
}

static gs_tier_t cm_tier(gs_source_t *s) {
    (void)s;
    return GS_TIER_INDEXED; // the chunk table is the index
}

static void cm_free(cm_src_t *m) {
    if (!m)
        return;
    gs_source_release(m->data);
    free(m->runs);
    free(m->key);
    free(m);
}

static void cm_close(gs_source_t *s) {
    cm_free(s->ctx);
}

static const gs_source_ops_t cm_ops = {cm_read, cm_size, cm_key, cm_tier, cm_close};

static cm_src_t *cm_new(gs_source_t *data, const char *what) {
    cm_src_t *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->data = gs_source_retain(data);
    m->what = what;
    m->key = gs_str_printf("%s#%s", gs_source_key(data), what);
    if (!m->key) {
        cm_free(m);
        return NULL;
    }
    return m;
}

// ============================================================================
// NDIF
// ============================================================================

bool ndif_source_detect(gs_source_t *rsrc) {
    if (!rsrc)
        return false;
    uint8_t *buf = NULL;
    size_t len = 0;
    if (gs_source_read_all(rsrc, RFORK_MAX_FORK_LEN, &buf, &len) != 0)
        return false;
    bool yes = ndif_detect(buf, len);
    free(buf);
    return yes;
}

gs_source_t *ndif_source_open(gs_source_t *data, gs_source_t *rsrc, int *err) {
    int e = 0;
    if (!err)
        err = &e;
    *err = -EINVAL;
    if (!data || !rsrc)
        return NULL;
    uint8_t *rbuf = NULL;
    size_t rlen = 0;
    if (gs_source_read_all(rsrc, RFORK_MAX_FORK_LEN, &rbuf, &rlen) != 0)
        return NULL;
    ndif_map_t *map = NULL;
    int rc = ndif_detect(rbuf, rlen) ? ndif_parse(rbuf, rlen, &map) : -EINVAL;
    free(rbuf);
    if (rc != 0) {
        *err = rc;
        return NULL;
    }
    cm_src_t *m = cm_new(data, "ndif");
    if (!m) {
        ndif_map_free(map);
        *err = -ENOMEM;
        return NULL;
    }
    m->size = (uint64_t)map->sectors * CM_SECTOR;
    uint64_t dsize = gs_source_size(data);
    rc = 0;
    for (size_t i = 0; i < map->n_chunks && rc == 0; i++) {
        const ndif_chunk_t *c = &map->chunks[i];
        if (c->count == 0)
            continue;
        // Every chunk stays inside the image the header declares, and its
        // stored bytes inside the data fork; checked without a wrapping add.
        if (c->count > map->sectors || c->sector > map->sectors - c->count) {
            rc = -EINVAL;
            break;
        }
        cm_run_t r = {.out_off = (uint64_t)c->sector * CM_SECTOR,
                      .out_len = (uint64_t)c->count * CM_SECTOR,
                      .in_off = c->offset,
                      .in_len = c->length,
                      .type = c->type};
        if (c->type == NDIF_CHUNK_ZERO) {
            r.kind = RUN_ZERO;
        } else if (c->type == NDIF_CHUNK_COPY) {
            r.kind = RUN_RAW;
            if (c->length < r.out_len)
                rc = -EINVAL; // the map promises more sectors than the fork holds
        } else if (c->type == NDIF_CHUNK_ADC) {
            r.kind = RUN_NDIF;
            if (r.out_len > NDIF_MAX_CHUNK_BYTES || c->length > NDIF_MAX_CHUNK_BYTES)
                rc = -EFBIG;
        } else {
            rc = -ENOTSUP; // KenCode / RLE / LZH / StuffIt chunks
        }
        if (rc == 0 && r.kind != RUN_ZERO && (r.in_off > dsize || r.in_len > dsize - r.in_off))
            rc = -EINVAL;
        if (rc == 0)
            rc = cm_add(m, r);
    }
    ndif_map_free(map);
    if (rc == 0)
        rc = cm_finish(m);
    if (rc != 0) {
        LOG(1, "NDIF '%s': unusable block map (%d)", gs_source_key(data), rc);
        cm_free(m);
        *err = rc;
        return NULL;
    }
    gs_source_t *s = peel_source_new(&cm_ops, m, NULL);
    *err = s ? 0 : -ENOMEM;
    return s;
}

// ============================================================================
// UDIF
// ============================================================================

bool udif_source_detect(const uint8_t *tail, size_t len) {
    return len >= UDIF_TRAILER_SIZE && udif_detect(tail + len - UDIF_TRAILER_SIZE, UDIF_TRAILER_SIZE);
}

// Largest XML block map read.  Real ones are a few hundred KB.
#define UDIF_MAX_XML (64u * 1024u * 1024u)

gs_source_t *udif_source_open(gs_source_t *data, int *err) {
    int e = 0;
    if (!err)
        err = &e;
    *err = -EINVAL;
    uint64_t dsize = gs_source_size(data);
    uint8_t trailer[UDIF_TRAILER_SIZE];
    if (dsize < UDIF_TRAILER_SIZE || gs_source_read_exact(data, dsize - UDIF_TRAILER_SIZE, trailer, sizeof(trailer)))
        return NULL;
    if (!udif_detect(trailer, sizeof(trailer)))
        return NULL;
    udif_trailer_t tr;
    int rc = udif_parse_trailer(trailer, sizeof(trailer), &tr);
    if (rc != 0) {
        LOG(1, "unsupported UDIF trailer in '%s' (%d)", gs_source_key(data), rc);
        *err = rc;
        return NULL;
    }
    if (tr.xml_offset > dsize || tr.xml_length > dsize - tr.xml_offset || tr.xml_length > UDIF_MAX_XML) {
        *err = -EINVAL;
        return NULL;
    }
    uint8_t *xml = malloc(tr.xml_length ? (size_t)tr.xml_length : 1);
    if (!xml) {
        *err = -ENOMEM;
        return NULL;
    }
    udif_map_t *map = NULL;
    rc = gs_source_read_exact(data, tr.xml_offset, xml, (size_t)tr.xml_length);
    if (rc == 0)
        rc = udif_parse_blkx(xml, (size_t)tr.xml_length, &map);
    free(xml);
    if (rc != 0) {
        LOG(1, "UDIF '%s': block map unreadable (%d)", gs_source_key(data), rc);
        *err = rc;
        return NULL;
    }
    cm_src_t *m = cm_new(data, "udif");
    if (!m) {
        udif_map_free(map);
        *err = -ENOMEM;
        return NULL;
    }
    m->size = tr.sectors * CM_SECTOR;
    rc = 0;
    for (size_t t = 0; t < map->n_tables && rc == 0; t++) {
        const udif_table_t *tb = &map->tables[t];
        for (size_t j = 0; j < tb->n_chunks && rc == 0; j++) {
            const udif_chunk_t *c = &tb->chunks[j];
            if (c->type == UDIF_CHUNK_COMMENT || c->count == 0)
                continue;
            // Absolute position is the table's base plus the chunk's own
            // sector, which restarts at 0 in every table; checked wrap-safe.
            if (c->count > tr.sectors || tb->base_sector > tr.sectors - c->count ||
                c->sector > tr.sectors - c->count - tb->base_sector) {
                rc = -EINVAL;
                break;
            }
            cm_run_t r = {.out_off = (tb->base_sector + c->sector) * CM_SECTOR,
                          .out_len = c->count * CM_SECTOR,
                          .in_off = c->offset,
                          .in_len = c->length,
                          .type = c->type};
            if (c->type == UDIF_CHUNK_ZERO || c->type == UDIF_CHUNK_IGNORE) {
                r.kind = RUN_ZERO;
            } else if (c->type == UDIF_CHUNK_RAW) {
                r.kind = RUN_RAW;
                if (c->length < r.out_len)
                    rc = -EINVAL;
            } else if (c->type == UDIF_CHUNK_ADC || c->type == UDIF_CHUNK_ZLIB) {
                r.kind = RUN_UDIF;
                if (r.out_len > NDIF_MAX_CHUNK_BYTES || c->length > NDIF_MAX_CHUNK_BYTES)
                    rc = -EFBIG;
            } else {
                LOG(1, "UDIF '%s': chunk codec %#x not supported", gs_source_key(data), c->type);
                rc = -ENOTSUP; // bzip2 / LZFSE / LZMA
            }
            if (rc == 0 && r.kind != RUN_ZERO && (r.in_off > dsize || r.in_len > dsize - r.in_off))
                rc = -EINVAL;
            if (rc == 0)
                rc = cm_add(m, r);
        }
    }
    udif_map_free(map);
    if (rc == 0)
        rc = cm_finish(m);
    if (rc != 0) {
        LOG(1, "UDIF '%s': unusable block map (%d)", gs_source_key(data), rc);
        cm_free(m);
        *err = rc;
        return NULL;
    }
    gs_source_t *s = peel_source_new(&cm_ops, m, NULL);
    *err = s ? 0 : -ENOMEM;
    return s;
}
