// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// source_cache.c
// Sources that add a cache or a lock around another: decode-through (one
// primitive for everything expensive to re-read), the locked wrapper, and
// the scratch sink peeler's compressed forks decode into.  See source.h.

#include "source.h"

#include "chunk_cache.h"
#include "image_scratch.h"
#include "storage_util.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ============================================================================
// Decode-through
// ============================================================================

typedef struct {
    gs_source_t *src; // retained
    gs_chunk_cache_t *cache;
    char *key; // the cache key: the source's key
    uint64_t size;
    pthread_mutex_t mu; // serialises reads of src and the cursor
    uint64_t cursor; // where a sequential read of src would continue
    uint8_t *scratch; // one chunk, for the chunks a forward pass passes over
} dt_src_t;

// Read chunk `idx` of the underlying source into `buf`.  Caller holds d->mu.
static int64_t dt_read_chunk(dt_src_t *d, uint64_t idx, uint8_t *buf, size_t cap) {
    uint64_t off = idx * GS_DECODE_CHUNK;
    if (off >= d->size)
        return 0;
    size_t n = d->size - off < cap ? (size_t)(d->size - off) : cap;
    int rc = gs_source_read_exact(d->src, off, buf, n);
    if (rc != 0)
        return rc;
    d->cursor = off + n;
    return (int64_t)n;
}

// Chunk fetch for the cache.  A forward-only source reads everything
// between its cursor and the chunk asked for anyway, so each chunk passed
// on the way is stored too: a later backward read is then a hit.
static int64_t dt_fetch(void *ctx, uint64_t idx, uint8_t *buf, size_t cap) {
    dt_src_t *d = ctx;
    pthread_mutex_lock(&d->mu);
    uint64_t want = idx * GS_DECODE_CHUNK;
    if (gs_source_tier(d->src) >= GS_TIER_STREAM && d->cursor < want && d->cursor % GS_DECODE_CHUNK == 0) {
        for (uint64_t k = d->cursor / GS_DECODE_CHUNK; k < idx; k++) {
            int64_t n = dt_read_chunk(d, k, d->scratch, GS_DECODE_CHUNK);
            if (n < 0) {
                pthread_mutex_unlock(&d->mu);
                return n;
            }
            gs_chunk_cache_put(d->cache, d->key, k, d->scratch, (size_t)n);
        }
    }
    int64_t n = dt_read_chunk(d, idx, buf, cap);
    pthread_mutex_unlock(&d->mu);
    return n;
}

static int64_t dt_read(gs_source_t *s, uint64_t off, void *buf, size_t len) {
    dt_src_t *d = s->ctx;
    if (off >= d->size)
        return 0;
    if (len > d->size - off)
        len = (size_t)(d->size - off);
    size_t done = 0;
    while (done < len) {
        uint64_t at = off + done;
        uint64_t idx = at / GS_DECODE_CHUNK;
        uint64_t in = at - idx * GS_DECODE_CHUNK;
        size_t want = GS_DECODE_CHUNK - (size_t)in < len - done ? GS_DECODE_CHUNK - (size_t)in : len - done;
        int64_t n =
            gs_chunk_cache_get(d->cache, d->key, idx, GS_DECODE_CHUNK, in, (uint8_t *)buf + done, want, dt_fetch, d);
        if (n < 0)
            return done ? (int64_t)done : n;
        if (n == 0)
            break; // short source
        done += (size_t)n;
    }
    return (int64_t)done;
}

static uint64_t dt_size(gs_source_t *s) {
    return ((dt_src_t *)s->ctx)->size;
}

static const char *dt_key(gs_source_t *s) {
    return ((dt_src_t *)s->ctx)->key;
}

static gs_tier_t dt_tier(gs_source_t *s) {
    (void)s;
    return GS_TIER_EARNED; // O(1) for every chunk one pass has produced
}

static void dt_close(gs_source_t *s) {
    dt_src_t *d = s->ctx;
    if (!d)
        return;
    gs_source_release(d->src);
    pthread_mutex_destroy(&d->mu);
    free(d->scratch);
    free(d->key);
    free(d);
}

static const gs_source_ops_t dt_ops = {dt_read, dt_size, dt_key, dt_tier, dt_close};

gs_source_t *gs_source_decode_through(gs_source_t *src, gs_chunk_cache_t *cache) {
    if (!src)
        return NULL;
    dt_src_t *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->src = gs_source_retain(src);
    d->cache = cache ? cache : gs_chunk_cache_default();
    d->key = gs_strdup(gs_source_key(src));
    d->size = gs_source_size(src);
    d->scratch = malloc(GS_DECODE_CHUNK);
    pthread_mutex_init(&d->mu, NULL);
    if (!d->cache || !d->key || !d->scratch) {
        gs_source_t tmp = {.ctx = d};
        dt_close(&tmp);
        return NULL;
    }
    return peel_source_new(&dt_ops, d, NULL);
}

// ============================================================================
// Locked wrapper
// ============================================================================

typedef struct {
    gs_source_t *src;
    pthread_mutex_t mu;
} lock_src_t;

static int64_t lock_read(gs_source_t *s, uint64_t off, void *buf, size_t len) {
    lock_src_t *l = s->ctx;
    pthread_mutex_lock(&l->mu);
    int64_t n = gs_source_read(l->src, off, buf, len);
    pthread_mutex_unlock(&l->mu);
    return n;
}

static uint64_t lock_size(gs_source_t *s) {
    return gs_source_size(((lock_src_t *)s->ctx)->src);
}

static const char *lock_key(gs_source_t *s) {
    return gs_source_key(((lock_src_t *)s->ctx)->src);
}

static gs_tier_t lock_tier(gs_source_t *s) {
    lock_src_t *l = s->ctx;
    pthread_mutex_lock(&l->mu);
    gs_tier_t t = gs_source_tier(l->src);
    pthread_mutex_unlock(&l->mu);
    return t;
}

static void lock_close(gs_source_t *s) {
    lock_src_t *l = s->ctx;
    if (!l)
        return;
    gs_source_release(l->src);
    pthread_mutex_destroy(&l->mu);
    free(l);
}

static const gs_source_ops_t lock_ops = {lock_read, lock_size, lock_key, lock_tier, lock_close};

gs_source_t *gs_source_locked(gs_source_t *src) {
    if (!src)
        return NULL;
    lock_src_t *l = calloc(1, sizeof(*l));
    if (!l)
        return NULL;
    l->src = gs_source_retain(src);
    pthread_mutex_init(&l->mu, NULL);
    return peel_source_new(&lock_ops, l, NULL);
}

// ============================================================================
// Scratch sink
// ============================================================================

// Forks up to this size decode into memory; larger ones into a scratch file.
#define SINK_MEMORY_MAX (8u * 1024u * 1024u)

typedef struct {
    int fd;
    char *path; // removed at close
    char *key;
    uint64_t expected, written;
} file_sink_t;

static int64_t fsink_read(gs_source_t *s, uint64_t off, void *buf, size_t len) {
    file_sink_t *f = s->ctx;
    if (off >= f->written)
        return 0;
    if (len > f->written - off)
        len = (size_t)(f->written - off);
    ssize_t n = pread(f->fd, buf, len, (off_t)off);
    return n < 0 ? -errno : (int64_t)n;
}

static uint64_t fsink_size(gs_source_t *s) {
    file_sink_t *f = s->ctx;
    return f->expected > f->written ? f->expected : f->written;
}

static const char *fsink_key(gs_source_t *s) {
    return ((file_sink_t *)s->ctx)->key;
}

static gs_tier_t fsink_tier(gs_source_t *s) {
    (void)s;
    return GS_TIER_RANDOM;
}

static void fsink_close(gs_source_t *s) {
    file_sink_t *f = s->ctx;
    if (!f)
        return;
    if (f->fd >= 0)
        close(f->fd);
    if (f->path)
        unlink(f->path);
    free(f->path);
    free(f->key);
    free(f);
}

static const gs_source_ops_t fsink_ops = {fsink_read, fsink_size, fsink_key, fsink_tier, fsink_close};

static gs_source_t *sink_create(void *ctx, const char *key, uint64_t expected_len) {
    (void)ctx;
    if (expected_len <= SINK_MEMORY_MAX)
        return peel_heap_sink()->create(NULL, key, expected_len);
    file_sink_t *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    f->fd = -1;
    f->key = gs_strdup(key);
    gs_mkdir_p(image_scratch_dir());
    // A fresh name each time: two opens of one fork decode independently.
    static unsigned counter;
    f->path = gs_str_printf("%s/sink-%ld-%u.bin", image_scratch_dir(), (long)getpid(), ++counter);
    if (f->key && f->path)
        f->fd = open(f->path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (f->fd < 0) {
        gs_source_t tmp = {.ctx = f};
        fsink_close(&tmp);
        return NULL;
    }
    f->expected = expected_len;
    return peel_source_new(&fsink_ops, f, NULL);
}

static int64_t sink_write(gs_source_t *sink, uint64_t off, const void *buf, size_t len) {
    if (sink->ops != &fsink_ops)
        return peel_heap_sink()->write(sink, off, buf, len);
    file_sink_t *f = sink->ctx;
    size_t done = 0;
    while (done < len) {
        ssize_t n = pwrite(f->fd, (const uint8_t *)buf + done, len - done, (off_t)(off + done));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        done += (size_t)n;
    }
    if (off + len > f->written)
        f->written = off + len;
    return (int64_t)len;
}

static void sink_commit(gs_source_t *sink) {
    if (sink->ops != &fsink_ops)
        peel_heap_sink()->commit(sink);
}

static const peel_sink_ops_t g_scratch_sink = {sink_create, sink_write, sink_commit};

const peel_sink_ops_t *gs_scratch_sink(void) {
    return &g_scratch_sink;
}
