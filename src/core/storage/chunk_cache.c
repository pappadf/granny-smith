// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// chunk_cache.c
// Bounded LRU chunk cache with coalescing and spill.  See chunk_cache.h.

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
// Types
// ============================================================================

#define CC_BUCKETS 1024

typedef enum { CE_FETCHING, CE_READY } ce_state_t;

// One chunk held in memory (or being fetched).
typedef struct cc_entry {
    uint64_t hash; // of (key, idx)
    char *key;
    uint64_t idx;
    uint8_t *data;
    size_t len;
    ce_state_t state;
    struct cc_entry *next_hash;
    struct cc_entry *lru_prev, *lru_next; // READY entries only
} cc_entry_t;

// Where a spilled chunk lives.
typedef struct cc_spilled {
    uint64_t hash;
    struct cc_spill_file *file;
    uint64_t idx;
    uint64_t off;
    size_t len;
    struct cc_spilled *next_hash;
} cc_spilled_t;

// One spill file per key, appended to.
typedef struct cc_spill_file {
    char *key;
    char *path;
    int fd;
    uint64_t end; // bytes written
    struct cc_spill_file *next;
} cc_spill_file_t;

struct gs_chunk_cache {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    size_t mem_budget;
    char *spill_dir; // NULL: no spill
    uint64_t spill_budget; // 0: unbounded
    cc_entry_t *buckets[CC_BUCKETS];
    cc_spilled_t *spill_buckets[CC_BUCKETS];
    cc_spill_file_t *files;
    cc_entry_t *lru_head, *lru_tail; // most recent at head
    gs_chunk_cache_stats_t st;
};

// ============================================================================
// Hashing
// ============================================================================

// FNV-1a over the key, then the index.
static uint64_t cc_hash(const char *key, uint64_t idx) {
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++)
        h = (h ^ *p) * 1099511628211ull;
    for (int i = 0; i < 8; i++)
        h = (h ^ ((idx >> (8 * i)) & 0xFF)) * 1099511628211ull;
    return h;
}

static cc_entry_t *find_entry(gs_chunk_cache_t *c, uint64_t h, const char *key, uint64_t idx) {
    for (cc_entry_t *e = c->buckets[h % CC_BUCKETS]; e; e = e->next_hash)
        if (e->hash == h && e->idx == idx && strcmp(e->key, key) == 0)
            return e;
    return NULL;
}

static cc_spilled_t *find_spilled(gs_chunk_cache_t *c, uint64_t h, const char *key, uint64_t idx) {
    for (cc_spilled_t *s = c->spill_buckets[h % CC_BUCKETS]; s; s = s->next_hash)
        if (s->hash == h && s->idx == idx && strcmp(s->file->key, key) == 0)
            return s;
    return NULL;
}

// ============================================================================
// LRU
// ============================================================================

static void lru_unlink(gs_chunk_cache_t *c, cc_entry_t *e) {
    if (e->lru_prev)
        e->lru_prev->lru_next = e->lru_next;
    else if (c->lru_head == e)
        c->lru_head = e->lru_next;
    if (e->lru_next)
        e->lru_next->lru_prev = e->lru_prev;
    else if (c->lru_tail == e)
        c->lru_tail = e->lru_prev;
    e->lru_prev = e->lru_next = NULL;
}

static void lru_push_head(gs_chunk_cache_t *c, cc_entry_t *e) {
    e->lru_prev = NULL;
    e->lru_next = c->lru_head;
    if (c->lru_head)
        c->lru_head->lru_prev = e;
    c->lru_head = e;
    if (!c->lru_tail)
        c->lru_tail = e;
}

// Remove `e` from the hash table (not the LRU) and free it.
static void entry_remove(gs_chunk_cache_t *c, cc_entry_t *e) {
    for (cc_entry_t **pp = &c->buckets[e->hash % CC_BUCKETS]; *pp; pp = &(*pp)->next_hash) {
        if (*pp == e) {
            *pp = e->next_hash;
            break;
        }
    }
    if (e->state == CE_READY)
        c->st.mem_bytes -= e->len;
    free(e->data);
    free(e->key);
    free(e);
}

// ============================================================================
// Spill
// ============================================================================

static cc_spill_file_t *spill_file_for(gs_chunk_cache_t *c, const char *key) {
    for (cc_spill_file_t *f = c->files; f; f = f->next)
        if (strcmp(f->key, key) == 0)
            return f;
    if (gs_mkdir_p(c->spill_dir) != 0)
        return NULL;
    cc_spill_file_t *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    f->key = gs_strdup(key);
    f->path = gs_str_printf("%s/%016llx.spill", c->spill_dir, (unsigned long long)cc_hash(key, 0));
    f->fd = f->path ? open(f->path, O_RDWR | O_CREAT | O_TRUNC, 0600) : -1;
    if (!f->key || f->fd < 0) {
        free(f->key);
        free(f->path);
        free(f);
        return NULL;
    }
    f->next = c->files;
    c->files = f;
    return f;
}

// Write an evicted chunk to its key's spill file.  True on success.
static bool spill(gs_chunk_cache_t *c, const cc_entry_t *e) {
    if (!c->spill_dir)
        return false;
    if (c->spill_budget && c->st.spill_bytes + e->len > c->spill_budget)
        return false;
    uint64_t h = e->hash;
    if (find_spilled(c, h, e->key, e->idx))
        return true; // already there (chunks never change)
    cc_spill_file_t *f = spill_file_for(c, e->key);
    if (!f)
        return false;
    if (pwrite(f->fd, e->data, e->len, (off_t)f->end) != (ssize_t)e->len)
        return false;
    cc_spilled_t *s = calloc(1, sizeof(*s));
    if (!s)
        return false;
    s->hash = h;
    s->file = f;
    s->idx = e->idx;
    s->off = f->end;
    s->len = e->len;
    s->next_hash = c->spill_buckets[h % CC_BUCKETS];
    c->spill_buckets[h % CC_BUCKETS] = s;
    f->end += e->len;
    c->st.spill_bytes += e->len;
    c->st.spilled++;
    return true;
}

// Evict from the LRU tail until the budget holds.  `keep` is never evicted.
static void evict(gs_chunk_cache_t *c, const cc_entry_t *keep) {
    cc_entry_t *e = c->lru_tail;
    while (c->st.mem_bytes > c->mem_budget && e) {
        cc_entry_t *prev = e->lru_prev;
        if (e != keep) {
            spill(c, e);
            lru_unlink(c, e);
            entry_remove(c, e);
            c->st.evictions++;
        }
        e = prev;
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

gs_chunk_cache_t *gs_chunk_cache_new(size_t mem_budget, const char *spill_dir, uint64_t spill_budget) {
    gs_chunk_cache_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);
    c->mem_budget = mem_budget;
    c->spill_dir = spill_dir ? gs_strdup(spill_dir) : NULL;
    c->spill_budget = spill_budget;
    return c;
}

// Drop spill files of `key` (NULL: all).  Caller holds the lock.
static void drop_spill(gs_chunk_cache_t *c, const char *key) {
    for (int b = 0; b < CC_BUCKETS; b++) {
        for (cc_spilled_t **pp = &c->spill_buckets[b]; *pp;) {
            cc_spilled_t *s = *pp;
            if (!key || strcmp(s->file->key, key) == 0) {
                *pp = s->next_hash;
                c->st.spill_bytes -= s->len;
                free(s);
            } else {
                pp = &s->next_hash;
            }
        }
    }
    for (cc_spill_file_t **pp = &c->files; *pp;) {
        cc_spill_file_t *f = *pp;
        if (!key || strcmp(f->key, key) == 0) {
            *pp = f->next;
            close(f->fd);
            unlink(f->path);
            free(f->path);
            free(f->key);
            free(f);
        } else {
            pp = &f->next;
        }
    }
}

void gs_chunk_cache_free(gs_chunk_cache_t *c) {
    if (!c)
        return;
    for (int b = 0; b < CC_BUCKETS; b++) {
        cc_entry_t *e = c->buckets[b];
        while (e) {
            cc_entry_t *n = e->next_hash;
            free(e->data);
            free(e->key);
            free(e);
            e = n;
        }
    }
    drop_spill(c, NULL);
    free(c->spill_dir);
    pthread_mutex_destroy(&c->mu);
    pthread_cond_destroy(&c->cv);
    free(c);
}

void gs_chunk_cache_set_budgets(gs_chunk_cache_t *c, size_t mem_budget, uint64_t spill_budget) {
    if (!c)
        return;
    pthread_mutex_lock(&c->mu);
    c->spill_budget = spill_budget;
    if (spill_budget && c->st.spill_bytes > spill_budget)
        drop_spill(c, NULL);
    c->mem_budget = mem_budget;
    evict(c, NULL);
    pthread_mutex_unlock(&c->mu);
}

void gs_chunk_cache_budgets(gs_chunk_cache_t *c, size_t *mem_budget, uint64_t *spill_budget) {
    if (!c)
        return;
    pthread_mutex_lock(&c->mu);
    if (mem_budget)
        *mem_budget = c->mem_budget;
    if (spill_budget)
        *spill_budget = c->spill_budget;
    pthread_mutex_unlock(&c->mu);
}

// A size in MiB from the environment, or `dflt`.
static uint64_t env_mib(const char *name, uint64_t dflt) {
    const char *v = getenv(name);
    if (!v || !*v)
        return dflt;
    char *end = NULL;
    unsigned long long n = strtoull(v, &end, 10);
    return (end && *end == '\0') ? (uint64_t)n << 20 : dflt;
}

static gs_chunk_cache_t *g_default;
static pthread_once_t g_default_once = PTHREAD_ONCE_INIT;

static void default_init(void) {
#ifdef __EMSCRIPTEN__
    uint64_t spill_default = 512ull << 20;
#else
    uint64_t spill_default = 0; // unbounded
#endif
    char *dir = gs_str_printf("%s/chunks", image_scratch_dir());
    g_default = gs_chunk_cache_new((size_t)env_mib("GS_CHUNK_CACHE_MB", 64ull << 20), dir,
                                   env_mib("GS_CHUNK_SPILL_MB", spill_default));
    free(dir);
}

gs_chunk_cache_t *gs_chunk_cache_default(void) {
    pthread_once(&g_default_once, default_init);
    return g_default;
}

// ============================================================================
// Access
// ============================================================================

// Insert READY data for (key, idx).  Caller holds the lock; takes `data`.
static cc_entry_t *insert_ready(gs_chunk_cache_t *c, uint64_t h, const char *key, uint64_t idx, uint8_t *data,
                                size_t len) {
    cc_entry_t *e = calloc(1, sizeof(*e));
    char *k = gs_strdup(key);
    if (!e || !k) {
        free(e);
        free(k);
        free(data);
        return NULL;
    }
    e->hash = h;
    e->key = k;
    e->idx = idx;
    e->data = data;
    e->len = len;
    e->state = CE_READY;
    e->next_hash = c->buckets[h % CC_BUCKETS];
    c->buckets[h % CC_BUCKETS] = e;
    lru_push_head(c, e);
    c->st.mem_bytes += len;
    evict(c, e);
    return e;
}

// Bring a spilled chunk back into memory.  Caller holds the lock.
static cc_entry_t *unspill(gs_chunk_cache_t *c, cc_spilled_t *s, const char *key) {
    uint8_t *buf = malloc(s->len ? s->len : 1);
    if (!buf)
        return NULL;
    if (pread(s->file->fd, buf, s->len, (off_t)s->off) != (ssize_t)s->len) {
        free(buf);
        return NULL;
    }
    c->st.spill_hits++;
    return insert_ready(c, s->hash, key, s->idx, buf, s->len);
}

// Copy out [in_off, in_off + n) of a chunk of `len` bytes: the count copied.
static int64_t copy_range(const uint8_t *data, size_t len, uint64_t in_off, void *out, size_t n) {
    if (in_off >= len)
        return 0;
    size_t k = len - (size_t)in_off < n ? len - (size_t)in_off : n;
    memcpy(out, data + in_off, k);
    return (int64_t)k;
}

int64_t gs_chunk_cache_get(gs_chunk_cache_t *c, const char *key, uint64_t idx, size_t chunk_cap, uint64_t in_off,
                           void *out, size_t n, gs_chunk_fetch_fn fetch, void *ctx) {
    size_t cap = chunk_cap;
    uint64_t h = cc_hash(key, idx);
    pthread_mutex_lock(&c->mu);
    bool waited = false;
    for (;;) {
        cc_entry_t *e = find_entry(c, h, key, idx);
        if (e && e->state == CE_READY) {
            if (!waited)
                c->st.hits++;
            lru_unlink(c, e);
            lru_push_head(c, e);
            int64_t k = copy_range(e->data, e->len, in_off, out, n);
            pthread_mutex_unlock(&c->mu);
            return k;
        }
        if (e) {
            // Someone is fetching it: wait for that, do not fetch again.
            if (!waited)
                c->st.coalesced++;
            waited = true;
            pthread_cond_wait(&c->cv, &c->mu);
            continue;
        }
        cc_spilled_t *s = find_spilled(c, h, key, idx);
        if (s && (e = unspill(c, s, key)) != NULL) {
            int64_t k = copy_range(e->data, e->len, in_off, out, n);
            pthread_mutex_unlock(&c->mu);
            return k;
        }
        break;
    }
    // Absent: fetch it, with a placeholder so others wait on us.
    c->st.misses++;
    c->st.fetches++;
    cc_entry_t *ph = calloc(1, sizeof(*ph));
    char *k = gs_strdup(key);
    if (!ph || !k) {
        free(ph);
        free(k);
        pthread_mutex_unlock(&c->mu);
        return -ENOMEM;
    }
    ph->hash = h;
    ph->key = k;
    ph->idx = idx;
    ph->state = CE_FETCHING;
    ph->next_hash = c->buckets[h % CC_BUCKETS];
    c->buckets[h % CC_BUCKETS] = ph;
    pthread_mutex_unlock(&c->mu);

    uint8_t *buf = malloc(cap ? cap : 1);
    int64_t got = buf ? fetch(ctx, idx, buf, cap) : -ENOMEM;
    int64_t result = got;

    pthread_mutex_lock(&c->mu);
    if (got < 0) {
        entry_remove(c, ph); // waiters retry (and fetch themselves)
        free(buf);
    } else {
        result = copy_range(buf, (size_t)got, in_off, out, n);
        uint8_t *shrunk = realloc(buf, got ? (size_t)got : 1);
        ph->data = shrunk ? shrunk : buf;
        ph->len = (size_t)got;
        ph->state = CE_READY;
        lru_push_head(c, ph);
        c->st.mem_bytes += ph->len;
        evict(c, ph);
    }
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
    return result;
}

int gs_chunk_cache_put(gs_chunk_cache_t *c, const char *key, uint64_t idx, const void *data, size_t len) {
    uint64_t h = cc_hash(key, idx);
    uint8_t *copy = malloc(len ? len : 1);
    if (!copy)
        return -ENOMEM;
    memcpy(copy, data, len);
    pthread_mutex_lock(&c->mu);
    if (find_entry(c, h, key, idx)) {
        pthread_mutex_unlock(&c->mu);
        free(copy); // held already (or being fetched): chunks never change
        return 0;
    }
    cc_entry_t *e = insert_ready(c, h, key, idx, copy, len);
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
    return e ? 0 : -ENOMEM;
}

bool gs_chunk_cache_peek(gs_chunk_cache_t *c, const char *key, uint64_t idx, void *out, size_t cap, size_t *len) {
    uint64_t h = cc_hash(key, idx);
    bool found = false;
    pthread_mutex_lock(&c->mu);
    cc_entry_t *e = find_entry(c, h, key, idx);
    if (e && e->state == CE_READY) {
        found = true;
        if (len)
            *len = e->len;
        if (out)
            memcpy(out, e->data, e->len < cap ? e->len : cap);
    } else if (!e) {
        cc_spilled_t *s = find_spilled(c, h, key, idx);
        if (s) {
            found = true;
            if (len)
                *len = s->len;
            if (out && pread(s->file->fd, out, s->len < cap ? s->len : cap, (off_t)s->off) < 0)
                found = false;
        }
    }
    pthread_mutex_unlock(&c->mu);
    return found;
}

void gs_chunk_cache_drop_key(gs_chunk_cache_t *c, const char *key) {
    pthread_mutex_lock(&c->mu);
    for (int b = 0; b < CC_BUCKETS; b++) {
        for (cc_entry_t *e = c->buckets[b]; e;) {
            cc_entry_t *n = e->next_hash;
            if (e->state == CE_READY && strcmp(e->key, key) == 0) {
                lru_unlink(c, e);
                entry_remove(c, e);
            }
            e = n;
        }
    }
    drop_spill(c, key);
    pthread_mutex_unlock(&c->mu);
}

void gs_chunk_cache_stats(gs_chunk_cache_t *c, gs_chunk_cache_stats_t *out) {
    pthread_mutex_lock(&c->mu);
    *out = c->st;
    pthread_mutex_unlock(&c->mu);
}
