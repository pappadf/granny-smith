// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// source.c
// Byte sources (peeler.h): reference counting, memory and view sources, a
// host-file source for the CLI and tests, the heap sink, and the bounded
// detection probe.

#include "internal.h"

#include <errno.h>
#include <inttypes.h>

// ============================================================================
// Lifecycle
// ============================================================================

peel_source_t *peel_source_new(const peel_source_ops_t *ops, void *ctx, peel_source_t *parent) {
    peel_source_t *s = calloc(1, sizeof(*s));
    if (!s) {
        // The caller handed ctx over: close it the way the source would have.
        peel_source_t tmp = {.ops = ops, .ctx = ctx};
        if (ops && ops->close)
            ops->close(&tmp);
        return NULL;
    }
    s->ops = ops;
    s->ctx = ctx;
    s->parent = parent ? peel_source_retain(parent) : NULL;
    s->refs = 1;
    return s;
}

peel_source_t *peel_source_retain(peel_source_t *s) {
    if (s)
        s->refs++;
    return s;
}

void peel_source_release(peel_source_t *s) {
    // Iterative up the parent chain: a deep nest must not recurse.
    while (s && --s->refs == 0) {
        peel_source_t *parent = s->parent;
        if (s->ops && s->ops->close)
            s->ops->close(s);
        free(s);
        s = parent;
    }
}

// ============================================================================
// Op wrappers
// ============================================================================

int64_t peel_source_read(peel_source_t *s, uint64_t off, void *buf, size_t len) {
    if (!s || !s->ops || !s->ops->read)
        return -22;
    if (len == 0)
        return 0;
    return s->ops->read(s, off, buf, len);
}

uint64_t peel_source_size(peel_source_t *s) {
    return (s && s->ops && s->ops->size) ? s->ops->size(s) : 0;
}

const char *peel_source_key(peel_source_t *s) {
    const char *k = (s && s->ops && s->ops->key) ? s->ops->key(s) : NULL;
    return k ? k : "";
}

peel_tier_t peel_source_tier(peel_source_t *s) {
    return (s && s->ops && s->ops->tier) ? s->ops->tier(s) : PEEL_TIER_RANDOM;
}

int peel_source_read_exact(peel_source_t *s, uint64_t off, void *buf, size_t len) {
    uint8_t *p = buf;
    while (len > 0) {
        int64_t n = peel_source_read(s, off, p, len);
        if (n < 0)
            return (int)n;
        if (n == 0)
            return -5; // short source
        p += n;
        off += (uint64_t)n;
        len -= (size_t)n;
    }
    return 0;
}

// ============================================================================
// Memory source
// ============================================================================

typedef struct {
    const uint8_t *buf;
    size_t len;
    bool own;
    char key[64];
    char *key_heap; // an explicit key, when given
} mem_src_t;

static int64_t mem_read(peel_source_t *s, uint64_t off, void *buf, size_t len) {
    mem_src_t *m = s->ctx;
    if (off >= m->len)
        return 0;
    size_t n = m->len - (size_t)off < len ? m->len - (size_t)off : len;
    memcpy(buf, m->buf + off, n);
    return (int64_t)n;
}

static uint64_t mem_size(peel_source_t *s) {
    return ((mem_src_t *)s->ctx)->len;
}

static const char *mem_key(peel_source_t *s) {
    mem_src_t *m = s->ctx;
    return m->key_heap ? m->key_heap : m->key;
}

static peel_tier_t mem_tier(peel_source_t *s) {
    (void)s;
    return PEEL_TIER_RANDOM;
}

static void mem_close(peel_source_t *s) {
    mem_src_t *m = s->ctx;
    if (!m)
        return;
    if (m->own)
        free((void *)(uintptr_t)m->buf);
    free(m->key_heap);
    free(m);
}

static const peel_source_ops_t mem_ops = {mem_read, mem_size, mem_key, mem_tier, mem_close};

peel_source_t *peel_source_memory_keyed(const void *buf, size_t len, bool own, const char *key) {
    mem_src_t *m = calloc(1, sizeof(*m));
    if (!m) {
        if (own)
            free((void *)(uintptr_t)buf);
        return NULL;
    }
    m->buf = buf;
    m->len = buf ? len : 0;
    m->own = own;
    if (key) {
        size_t kl = strlen(key);
        m->key_heap = malloc(kl + 1);
        if (m->key_heap)
            memcpy(m->key_heap, key, kl + 1);
    }
    snprintf(m->key, sizeof(m->key), "mem:%p+%zu", buf, len);
    return peel_source_new(&mem_ops, m, NULL);
}

peel_source_t *peel_source_memory(const void *buf, size_t len, bool own) {
    return peel_source_memory_keyed(buf, len, own, NULL);
}

// ============================================================================
// View source
// ============================================================================

typedef struct {
    uint64_t off, len;
    char *key;
} view_src_t;

static int64_t view_read(peel_source_t *s, uint64_t off, void *buf, size_t len) {
    view_src_t *v = s->ctx;
    if (off >= v->len)
        return 0;
    if (len > v->len - off)
        len = (size_t)(v->len - off);
    return peel_source_read(s->parent, v->off + off, buf, len);
}

static uint64_t view_size(peel_source_t *s) {
    return ((view_src_t *)s->ctx)->len;
}

static const char *view_key(peel_source_t *s) {
    return ((view_src_t *)s->ctx)->key;
}

static peel_tier_t view_tier(peel_source_t *s) {
    return peel_source_tier(s->parent); // a view costs what its parent costs
}

static void view_close(peel_source_t *s) {
    view_src_t *v = s->ctx;
    if (v)
        free(v->key);
    free(v);
}

static const peel_source_ops_t view_ops = {view_read, view_size, view_key, view_tier, view_close};

peel_source_t *peel_source_view_keyed(peel_source_t *parent, uint64_t off, uint64_t len, const char *key) {
    if (!parent)
        return NULL;
    uint64_t psize = peel_source_size(parent);
    if (off > psize)
        off = psize;
    if (len > psize - off)
        len = psize - off;
    view_src_t *v = calloc(1, sizeof(*v));
    if (!v)
        return NULL;
    v->off = off;
    v->len = len;
    char tmp[64];
    const char *pk = peel_source_key(parent);
    if (!key) {
        snprintf(tmp, sizeof(tmp), "@%" PRIu64 "+%" PRIu64, off, len);
    }
    size_t need = key ? strlen(key) + 1 : strlen(pk) + strlen(tmp) + 1;
    v->key = malloc(need);
    if (!v->key) {
        free(v);
        return NULL;
    }
    if (key)
        memcpy(v->key, key, need);
    else
        snprintf(v->key, need, "%s%s", pk, tmp);
    return peel_source_new(&view_ops, v, parent);
}

peel_source_t *peel_source_view(peel_source_t *parent, uint64_t off, uint64_t len) {
    return peel_source_view_keyed(parent, off, len, NULL);
}

// ============================================================================
// Host file source (CLI and tests; embedders supply their own)
// ============================================================================

typedef struct {
    FILE *fp;
    uint64_t size;
    char *path;
} file_src_t;

static int64_t file_read(peel_source_t *s, uint64_t off, void *buf, size_t len) {
    file_src_t *f = s->ctx;
    if (off >= f->size)
        return 0;
    if (len > f->size - off)
        len = (size_t)(f->size - off);
    // C99 fseek takes a long: a file source past 2 GiB on a 32-bit long is
    // refused at open, so this cannot wrap.
    if (fseek(f->fp, (long)off, SEEK_SET) != 0)
        return -5;
    size_t n = fread(buf, 1, len, f->fp);
    if (n == 0 && ferror(f->fp))
        return -5;
    return (int64_t)n;
}

static uint64_t file_size(peel_source_t *s) {
    return ((file_src_t *)s->ctx)->size;
}

static const char *file_key(peel_source_t *s) {
    return ((file_src_t *)s->ctx)->path;
}

static peel_tier_t file_tier(peel_source_t *s) {
    (void)s;
    return PEEL_TIER_RANDOM;
}

static void file_close(peel_source_t *s) {
    file_src_t *f = s->ctx;
    if (!f)
        return;
    if (f->fp)
        fclose(f->fp);
    free(f->path);
    free(f);
}

static const peel_source_ops_t file_ops = {file_read, file_size, file_key, file_tier, file_close};

peel_source_t *peel_source_file(const char *path, peel_err_t **err) {
    *err = NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        *err = make_err("cannot open '%s': %s", path, strerror(errno));
        return NULL;
    }
    long sz = -1;
    if (fseek(fp, 0, SEEK_END) == 0)
        sz = ftell(fp);
    if (sz < 0) {
        *err = make_err("cannot determine size of '%s': %s", path, strerror(errno));
        fclose(fp);
        return NULL;
    }
    file_src_t *f = calloc(1, sizeof(*f));
    size_t pl = strlen(path);
    char *p = malloc(pl + 1);
    if (!f || !p) {
        free(f);
        free(p);
        fclose(fp);
        *err = make_err("out of memory opening '%s'", path);
        return NULL;
    }
    memcpy(p, path, pl + 1);
    f->fp = fp;
    f->size = (uint64_t)sz;
    f->path = p;
    peel_source_t *s = peel_source_new(&file_ops, f, NULL);
    if (!s)
        *err = make_err("out of memory opening '%s'", path);
    return s;
}

// ============================================================================
// Whole-source read
// ============================================================================

peel_buf_t peel_source_slurp(peel_source_t *s, peel_err_t **err) {
    *err = NULL;
    uint64_t size = peel_source_size(s);
    if (size > PEEL_MAX_INPUT) {
        *err = make_err("'%s' is %" PRIu64 " bytes, over the %u MiB limit", peel_source_key(s), size,
                        (unsigned)(PEEL_MAX_INPUT >> 20));
        return (peel_buf_t){0};
    }
    if (size == 0)
        return (peel_buf_t){0};
    uint8_t *data = malloc((size_t)size);
    if (!data) {
        *err = make_err("out of memory reading '%s' (%" PRIu64 " bytes)", peel_source_key(s), size);
        return (peel_buf_t){0};
    }
    int rc = peel_source_read_exact(s, 0, data, (size_t)size);
    if (rc != 0) {
        free(data);
        *err = make_err("short read on '%s'", peel_source_key(s));
        return (peel_buf_t){0};
    }
    return (peel_buf_t){.data = data, .size = (size_t)size, .owned = true};
}

// ============================================================================
// Heap sink
// ============================================================================

typedef struct {
    uint8_t *buf;
    size_t len, cap;
    uint64_t expected;
    char *key;
} heap_sink_t;

static int64_t heap_read(peel_source_t *s, uint64_t off, void *buf, size_t len) {
    heap_sink_t *h = s->ctx;
    if (off >= h->len)
        return 0;
    size_t n = h->len - (size_t)off < len ? h->len - (size_t)off : len;
    memcpy(buf, h->buf + off, n);
    return (int64_t)n;
}

static uint64_t heap_size(peel_source_t *s) {
    heap_sink_t *h = s->ctx;
    return h->expected > h->len ? h->expected : h->len;
}

static const char *heap_key(peel_source_t *s) {
    return ((heap_sink_t *)s->ctx)->key;
}

static void heap_close(peel_source_t *s) {
    heap_sink_t *h = s->ctx;
    if (!h)
        return;
    free(h->buf);
    free(h->key);
    free(h);
}

static const peel_source_ops_t heap_ops = {heap_read, heap_size, heap_key, mem_tier, heap_close};

static peel_source_t *heap_create(void *ctx, const char *key, uint64_t expected_len) {
    (void)ctx;
    heap_sink_t *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->expected = expected_len;
    size_t kl = strlen(key);
    h->key = malloc(kl + 1);
    if (!h->key) {
        free(h);
        return NULL;
    }
    memcpy(h->key, key, kl + 1);
    return peel_source_new(&heap_ops, h, NULL);
}

static int64_t heap_write(peel_source_t *sink, uint64_t off, const void *buf, size_t len) {
    heap_sink_t *h = sink->ctx;
    if (off > h->len || off + len > PEEL_MAX_FORK)
        return -22; // written in order, never past the fork cap
    size_t need = (size_t)off + len;
    if (need > h->cap) {
        size_t ncap = h->cap ? h->cap : 4096;
        while (ncap < need)
            ncap *= 2;
        if (h->expected && ncap > h->expected && need <= h->expected)
            ncap = (size_t)h->expected; // exact when the size is known
        uint8_t *nb = realloc(h->buf, ncap);
        if (!nb)
            return -12;
        h->buf = nb;
        h->cap = ncap;
    }
    memcpy(h->buf + off, buf, len);
    if (need > h->len)
        h->len = need;
    return (int64_t)len;
}

static void heap_commit(peel_source_t *sink) {
    (void)sink;
}

static const peel_sink_ops_t heap_sink_ops = {heap_create, heap_write, heap_commit};

const peel_sink_ops_t *peel_heap_sink(void) {
    return &heap_sink_ops;
}

// ============================================================================
// Detection probe
// ============================================================================

int peel_probe_init(peel_probe_t *p, peel_source_t *src) {
    memset(p, 0, sizeof(*p));
    uint64_t size = peel_source_size(src);
    p->size = size;
    size_t head = size < PEEL_DETECT_BUDGET ? (size_t)size : PEEL_DETECT_BUDGET;
    // The tail overlaps the head for a small source: then it is the head's
    // last bytes and costs no second read.
    size_t tail = head;
    bool separate = size > PEEL_DETECT_BUDGET;
    p->owned = malloc(head + (separate ? tail : 0) + 1);
    if (!p->owned)
        return -12;
    int rc = head ? peel_source_read_exact(src, 0, p->owned, head) : 0;
    if (rc != 0) {
        peel_probe_free(p);
        return rc;
    }
    p->head = p->owned;
    p->head_len = head;
    if (separate) {
        uint8_t *t = p->owned + head;
        rc = peel_source_read_exact(src, size - tail, t, tail);
        if (rc != 0) {
            peel_probe_free(p);
            return rc;
        }
        p->tail = t;
    } else {
        p->tail = p->owned;
    }
    p->tail_len = tail;
    return 0;
}

void peel_probe_free(peel_probe_t *p) {
    if (!p)
        return;
    free(p->owned);
    memset(p, 0, sizeof(*p));
}
