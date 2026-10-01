// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The byte-source layer: views, decode-through, the chunk cache, the format
// registry.  See Makefile.

#include "chunk_cache.h"
#include "format_registry.h"
#include "source.h"
#include "test_assert.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---- A counting source -------------------------------------------------------
//
// Byte i is (i * 7 + 3) & 0xFF.  It counts its reads and the bytes they
// covered, remembers the highest offset read from the head and the lowest
// from the tail, and can claim any tier and be slow.

typedef struct {
    uint64_t size;
    gs_tier_t tier;
    int reads;
    uint64_t bytes;
    uint64_t max_end; // highest offset + len read
    uint64_t min_off; // lowest offset read at or past size/2
    unsigned delay_us;
    char key[64];
    pthread_mutex_t mu;
} count_t;

static uint8_t pattern(uint64_t i) {
    return (uint8_t)((i * 7 + 3) & 0xFF);
}

static int64_t count_read(gs_source_t *s, uint64_t off, void *buf, size_t len) {
    count_t *c = s->ctx;
    if (c->delay_us)
        usleep(c->delay_us);
    pthread_mutex_lock(&c->mu);
    if (off >= c->size) {
        pthread_mutex_unlock(&c->mu);
        return 0;
    }
    if (len > c->size - off)
        len = (size_t)(c->size - off);
    c->reads++;
    c->bytes += len;
    if (off + len > c->max_end && off < c->size / 2)
        c->max_end = off + len;
    if (off >= c->size / 2 && off < c->min_off)
        c->min_off = off;
    pthread_mutex_unlock(&c->mu);
    for (size_t i = 0; i < len; i++)
        ((uint8_t *)buf)[i] = pattern(off + i);
    return (int64_t)len;
}
static uint64_t count_size(gs_source_t *s) {
    return ((count_t *)s->ctx)->size;
}
static const char *count_key(gs_source_t *s) {
    return ((count_t *)s->ctx)->key;
}
static gs_tier_t count_tier(gs_source_t *s) {
    return ((count_t *)s->ctx)->tier;
}
static void count_close(gs_source_t *s) {
    (void)s; // the test owns the count_t
}
static const gs_source_ops_t count_ops = {count_read, count_size, count_key, count_tier, count_close};

static gs_source_t *counting(count_t *c, uint64_t size, gs_tier_t tier, const char *key) {
    memset(c, 0, sizeof(*c));
    c->size = size;
    c->tier = tier;
    c->min_off = UINT64_MAX;
    snprintf(c->key, sizeof(c->key), "%s", key);
    pthread_mutex_init(&c->mu, NULL);
    return peel_source_new(&count_ops, c, NULL);
}

// ---- Views -------------------------------------------------------------------

// A view is arithmetic: offsets shift, the length clamps to the parent, a
// read past its end is short, and a view of a view composes.
TEST(test_view_arithmetic_at_boundaries) {
    count_t c;
    gs_source_t *s = counting(&c, 1000, GS_TIER_RANDOM, "count");
    gs_source_t *v = gs_source_view(s, 100, 5000, NULL); // clamped to 900
    ASSERT_EQ_INT(900, (int)gs_source_size(v));
    ASSERT_EQ_INT(GS_TIER_RANDOM, gs_source_tier(v));
    uint8_t b[16];
    ASSERT_EQ_INT(0, gs_source_read_exact(v, 0, b, 4));
    ASSERT_EQ_INT(pattern(100), b[0]);
    ASSERT_EQ_INT(4, (int)gs_source_read(v, 896, b, 16)); // short at the end
    ASSERT_EQ_INT(pattern(996), b[0]);
    ASSERT_EQ_INT(0, (int)gs_source_read(v, 900, b, 1)); // past it
    ASSERT_EQ_INT(-EIO, gs_source_read_exact(v, 898, b, 4));
    gs_source_t *vv = gs_source_view(v, 10, 10, "inner");
    ASSERT_EQ_INT(0, gs_source_read_exact(vv, 9, b, 1));
    ASSERT_EQ_INT(pattern(119), b[0]);
    ASSERT_TRUE(strcmp(gs_source_key(vv), "inner") == 0);
    // Views retain their parents: releasing the first reference keeps them.
    gs_source_release(s);
    gs_source_release(v);
    ASSERT_EQ_INT(0, gs_source_read_exact(vv, 0, b, 1));
    ASSERT_EQ_INT(pattern(110), b[0]);
    gs_source_release(vv);
}

// Keys name what is inside what.
TEST(test_key_containment) {
    ASSERT_TRUE(gs_key_within("/a/disk.img@1:2", "/a/disk.img@1:2"));
    ASSERT_TRUE(gs_key_within("/a/disk.img@1:2/partition1/x", "/a/disk.img@1:2"));
    ASSERT_TRUE(gs_key_within("/a/disk.img@1:2#dc42", "/a/disk.img@1:2"));
    ASSERT_TRUE(!gs_key_within("/a/disk.img@1:23", "/a/disk.img@1:2"));
    ASSERT_TRUE(!gs_key_within("/a/disk.img", "/a/disk.img@1:2"));
    ASSERT_TRUE(!gs_key_within("x", ""));
}

// ---- Decode-through --------------------------------------------------------

// A forward-only source behind decode-through: reading forward drives it
// once; reading backward afterwards touches it not at all; and the bytes are
// the source's.
TEST(test_decode_through_backward_reads_never_redecode) {
    count_t c;
    gs_source_t *s = counting(&c, 5 * GS_DECODE_CHUNK + 123, GS_TIER_STREAM, "stream-src");
    gs_chunk_cache_t *cache = gs_chunk_cache_new(64u << 20, NULL, 0);
    gs_source_t *dt = gs_source_decode_through(s, cache);
    ASSERT_EQ_INT((int)(5 * GS_DECODE_CHUNK + 123), (int)gs_source_size(dt));

    // Straight to the last chunk: every chunk on the way is stored.
    uint8_t b[64];
    ASSERT_EQ_INT(0, gs_source_read_exact(dt, 5 * GS_DECODE_CHUNK + 100, b, 20));
    ASSERT_EQ_INT(pattern(5 * GS_DECODE_CHUNK + 100), b[0]);
    uint64_t bytes_after_forward = c.bytes;
    ASSERT_TRUE(bytes_after_forward >= 5 * GS_DECODE_CHUNK); // one pass

    // Backward, across chunk boundaries: all hits.
    for (uint64_t off = 0; off < 5 * GS_DECODE_CHUNK; off += GS_DECODE_CHUNK / 3) {
        ASSERT_EQ_INT(0, gs_source_read_exact(dt, off, b, sizeof(b)));
        for (size_t i = 0; i < sizeof(b); i++)
            ASSERT_EQ_INT(pattern(off + i), b[i]);
    }
    ASSERT_EQ_INT((int)bytes_after_forward, (int)c.bytes);
    gs_source_release(dt);
    gs_source_release(s);
    gs_chunk_cache_free(cache);
}

// ---- The chunk cache -------------------------------------------------------

typedef struct {
    gs_chunk_cache_t *cache;
    int fetches; // incremented by the fetch
    pthread_mutex_t mu;
    uint8_t got[16];
    int64_t rc;
} race_t;

static int64_t slow_fetch(void *ctx, uint64_t idx, uint8_t *buf, size_t cap) {
    race_t *r = ctx;
    pthread_mutex_lock(&r->mu);
    r->fetches++;
    pthread_mutex_unlock(&r->mu);
    usleep(50 * 1000); // long enough for every reader to arrive
    size_t n = cap < 1024 ? cap : 1024;
    memset(buf, (int)(0x40 + idx), n);
    return (int64_t)n;
}

typedef struct {
    race_t *r;
    uint8_t out[16];
    int64_t rc;
} reader_t;

static void *reader(void *arg) {
    reader_t *rd = arg;
    rd->rc = gs_chunk_cache_get(rd->r->cache, "k", 3, 1024, 8, rd->out, sizeof(rd->out), slow_fetch, rd->r);
    return NULL;
}

// Eight readers of one absent chunk share one fetch; each gets the bytes.
TEST(test_chunk_cache_coalesces_concurrent_readers) {
    race_t r = {.cache = gs_chunk_cache_new(1u << 20, NULL, 0)};
    pthread_mutex_init(&r.mu, NULL);
    pthread_t t[8];
    reader_t rd[8];
    for (int i = 0; i < 8; i++) {
        rd[i] = (reader_t){.r = &r};
        ASSERT_EQ_INT(0, pthread_create(&t[i], NULL, reader, &rd[i]));
    }
    for (int i = 0; i < 8; i++)
        pthread_join(t[i], NULL);
    ASSERT_EQ_INT(1, r.fetches);
    for (int i = 0; i < 8; i++) {
        ASSERT_EQ_INT(16, (int)rd[i].rc);
        ASSERT_EQ_INT(0x43, rd[i].out[0]);
    }
    gs_chunk_cache_stats_t st;
    gs_chunk_cache_stats(r.cache, &st);
    ASSERT_EQ_INT(1, (int)st.fetches);
    ASSERT_EQ_INT(7, (int)(st.coalesced + st.hits));
    gs_chunk_cache_free(r.cache);
}

static int64_t fill_fetch(void *ctx, uint64_t idx, uint8_t *buf, size_t cap) {
    int *calls = ctx;
    (*calls)++;
    memset(buf, (int)idx, cap);
    return (int64_t)cap;
}

// Past its budget the cache evicts; with a spill directory an evicted chunk
// comes back from the spill file, without fetching again.
TEST(test_chunk_cache_spills_evicted_chunks) {
    char dir[] = "/tmp/gs_chunk_spill_XXXXXX";
    ASSERT_TRUE(mkdtemp(dir) != NULL);
    gs_chunk_cache_t *c = gs_chunk_cache_new(4096, dir, 0); // room for two 2 KiB chunks
    int calls = 0;
    uint8_t b[4];
    for (uint64_t i = 0; i < 6; i++)
        ASSERT_EQ_INT(4, (int)gs_chunk_cache_get(c, "spilly", i, 2048, 0, b, 4, fill_fetch, &calls));
    ASSERT_EQ_INT(6, calls);
    gs_chunk_cache_stats_t st;
    gs_chunk_cache_stats(c, &st);
    ASSERT_TRUE(st.evictions >= 4);
    ASSERT_TRUE(st.mem_bytes <= 4096);
    // Chunk 0 was evicted; it is read back from spill, not fetched.
    ASSERT_EQ_INT(4, (int)gs_chunk_cache_get(c, "spilly", 0, 2048, 100, b, 4, fill_fetch, &calls));
    ASSERT_EQ_INT(0, b[0]);
    ASSERT_EQ_INT(6, calls);
    gs_chunk_cache_stats(c, &st);
    ASSERT_TRUE(st.spill_hits >= 1);
    // Without spill, an evicted chunk is fetched again.
    gs_chunk_cache_t *ns = gs_chunk_cache_new(4096, NULL, 0);
    calls = 0;
    for (uint64_t i = 0; i < 6; i++)
        gs_chunk_cache_get(ns, "k", i, 2048, 0, b, 4, fill_fetch, &calls);
    gs_chunk_cache_get(ns, "k", 0, 2048, 0, b, 4, fill_fetch, &calls);
    ASSERT_EQ_INT(7, calls);
    gs_chunk_cache_free(ns);
    gs_chunk_cache_drop_key(c, "spilly");
    gs_chunk_cache_free(c);
    rmdir(dir);
}

// ---- The format registry ------------------------------------------------------

// Detection reads a bounded probe: no more than the budget from the head
// and from the tail, however large the source.
TEST(test_detection_reads_within_the_budget) {
    count_t c;
    gs_source_t *s = counting(&c, 50u << 20, GS_TIER_RANDOM, "big");
    gs_probe_t p;
    ASSERT_EQ_INT(0, gs_probe_init(&p, s, NULL));
    ASSERT_TRUE(gs_format_detect(&p, GS_FMT_WRAPPER) == NULL);
    gs_probe_free(&p);
    ASSERT_TRUE(c.max_end <= PEEL_DETECT_BUDGET);
    ASSERT_TRUE(c.min_off >= (50u << 20) - PEEL_DETECT_BUDGET);
    ASSERT_TRUE(c.bytes <= 2 * PEEL_DETECT_BUDGET);
    gs_unwrapped_t u;
    ASSERT_EQ_INT(0, gs_format_unwrap(s, NULL, &u));
    ASSERT_TRUE(u.chain[0] == '\0');
    ASSERT_TRUE(u.data == s);
    gs_unwrapped_free(&u);
    gs_source_release(s);
}

// A DiskCopy 4.2 file unwraps to a view of its data section; a gzip around
// it unwraps first.
TEST(test_unwrap_diskcopy_inside_gzip) {
    // An 84-byte DiskCopy 4.2 header + 1024 bytes of data + no tags.
    static uint8_t dc[0x54 + 1024];
    memset(dc, 0, sizeof(dc));
    dc[0] = 4;
    memcpy(dc + 1, "Test", 4);
    dc[0x40] = 0;
    dc[0x41] = 0;
    dc[0x42] = 0x04;
    dc[0x43] = 0x00; // data size 1024
    dc[0x52] = 0x01;
    dc[0x53] = 0x00; // magic 0x0100
    for (int i = 0; i < 1024; i++)
        dc[0x54 + i] = (uint8_t)i;
    gs_source_t *s = gs_source_memory(dc, sizeof(dc), false, "dc42");
    gs_unwrapped_t u;
    ASSERT_EQ_INT(0, gs_format_unwrap(s, NULL, &u));
    ASSERT_TRUE(strcmp(u.chain, "dc42") == 0);
    ASSERT_EQ_INT(1024, (int)gs_source_size(u.data));
    ASSERT_TRUE(u.dc42 != NULL);
    uint8_t b[4];
    ASSERT_EQ_INT(0, gs_source_read_exact(u.data, 1020, b, 4));
    ASSERT_EQ_INT(1020 & 0xFF, b[0]);
    gs_unwrapped_free(&u);

    // The same bytes gzipped (a stored deflate block, so the test needs no
    // compressor): 10-byte header, then BFINAL stored blocks, then CRC/size.
    size_t n = sizeof(dc);
    uint8_t *gz = malloc(10 + 5 + n + 8);
    const uint8_t hdr[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
    memcpy(gz, hdr, 10);
    gz[10] = 1; // BFINAL, stored
    gz[11] = (uint8_t)n;
    gz[12] = (uint8_t)(n >> 8);
    gz[13] = (uint8_t)~n;
    gz[14] = (uint8_t)(~n >> 8);
    memcpy(gz + 15, dc, n);
    // CRC-32 of dc.
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= dc[i];
        for (int k = 0; k < 8; k++)
            crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
    }
    crc = ~crc;
    uint8_t *t = gz + 15 + n;
    for (int k = 0; k < 4; k++) {
        t[k] = (uint8_t)(crc >> (8 * k));
        t[4 + k] = (uint8_t)(n >> (8 * k));
    }
    gs_source_t *g = gs_source_memory(gz, 10 + 5 + n + 8, true, "/x/disk.dc42.gz");
    ASSERT_EQ_INT(0, gs_format_unwrap(g, NULL, &u));
    ASSERT_TRUE(strcmp(u.chain, "gz+dc42") == 0);
    ASSERT_EQ_INT(1024, (int)gs_source_size(u.data));
    ASSERT_EQ_INT(0, gs_source_read_exact(u.data, 0, b, 4));
    ASSERT_EQ_INT(0, b[0]);
    ASSERT_EQ_INT(3, b[3]);
    gs_unwrapped_free(&u);
    gs_source_release(g);
    gs_source_release(s);
}

// The budgets can change at run time (files.cache): a smaller memory budget
// evicts at once, and a spill area over its new budget is emptied -- the
// chunks are fetched again, correctly, when next read.
TEST(test_chunk_cache_budgets_change_at_run_time) {
    char dir[] = "/tmp/gs_chunk_budget_XXXXXX";
    ASSERT_TRUE(mkdtemp(dir) != NULL);
    gs_chunk_cache_t *c = gs_chunk_cache_new(16384, dir, 0);
    int calls = 0;
    uint8_t b[4];
    for (uint64_t i = 0; i < 8; i++)
        ASSERT_EQ_INT(4, (int)gs_chunk_cache_get(c, "bud", i, 2048, 0, b, 4, fill_fetch, &calls));
    gs_chunk_cache_stats_t st;
    gs_chunk_cache_stats(c, &st);
    ASSERT_TRUE(st.mem_bytes == 8 * 2048 && st.spill_bytes == 0);
    size_t mem;
    uint64_t spill;
    gs_chunk_cache_set_budgets(c, 4096, 0);
    gs_chunk_cache_budgets(c, &mem, &spill);
    ASSERT_TRUE(mem == 4096 && spill == 0);
    gs_chunk_cache_stats(c, &st);
    ASSERT_TRUE(st.mem_bytes <= 4096);
    ASSERT_TRUE(st.spill_bytes >= 4 * 2048); // evicted to spill
    gs_chunk_cache_set_budgets(c, 4096, 2048); // the spill area is over: emptied
    gs_chunk_cache_stats(c, &st);
    ASSERT_EQ_INT(0, (int)st.spill_bytes);
    int before = calls;
    ASSERT_EQ_INT(4, (int)gs_chunk_cache_get(c, "bud", 0, 2048, 8, b, 4, fill_fetch, &calls));
    ASSERT_EQ_INT(before + 1, calls); // fetched again
    ASSERT_EQ_INT(0, b[0]);
    gs_chunk_cache_free(c);
    rmdir(dir);
}

// Keys made at different times name the same bytes when they are equal --
// or, ignoring host time stamps (the browser's OPFS gives files their load
// time), when only those differ.  A path or a size that differs never
// matches, nor does a member of another archive.
TEST(test_key_same_ignores_only_host_times) {
    const char *a = "/opfs/fd/sys.dsk@819200:1790881846";
    const char *b = "/opfs/fd/sys.dsk@819200:1790881849";
    ASSERT_TRUE(gs_key_same(a, a, false));
    ASSERT_TRUE(!gs_key_same(a, b, false));
    ASSERT_TRUE(gs_key_same(a, b, true));
    ASSERT_TRUE(!gs_key_same(a, "/opfs/fd/sys.dsk@819201:1790881846", true)); // size
    ASSERT_TRUE(!gs_key_same(a, "/opfs/fd/sys2.dsk@819200:1790881846", true)); // path
    // Nested: the host segment inside a member key, and a wrapper layer.
    ASSERT_TRUE(gs_key_same("/x/a.zip@100:5/disk.img#dc42", "/x/a.zip@100:9/disk.img#dc42", true));
    ASSERT_TRUE(!gs_key_same("/x/a.zip@100:5/disk.img#dc42", "/x/a.zip@100:9/disk.img#dc42", false));
    ASSERT_TRUE(!gs_key_same("/x/a.zip@100:5/disk.img", "/x/a.zip@100:5/other.img", true));
    // A ':' that is no time stamp is compared as it stands.
    ASSERT_TRUE(!gs_key_same("/x/a:1/b@1:2", "/x/a:9/b@1:2", true));
    ASSERT_TRUE(!gs_key_same(NULL, a, true));
}

// ---- "Not yet" -------------------------------------------------------------

// A source whose bytes arrive later, as a remote one's do: a read says
// GS_EAGAIN while `not_yet` is positive, and each poll brings it one closer.
typedef struct {
    int not_yet; // reads still to refuse
    int polls;
    int reads;
} late_t;

static int64_t late_read(gs_source_t *s, uint64_t off, void *buf, size_t len) {
    late_t *l = s->ctx;
    l->reads++;
    if (l->not_yet > 0)
        return GS_EAGAIN;
    if (off >= 4096)
        return 0;
    if (len > 4096 - off)
        len = (size_t)(4096 - off);
    for (size_t i = 0; i < len; i++)
        ((uint8_t *)buf)[i] = pattern(off + i);
    return (int64_t)len;
}
static uint64_t late_size(gs_source_t *s) {
    (void)s;
    return 4096;
}
static const char *late_key(gs_source_t *s) {
    (void)s;
    return "late";
}
static gs_tier_t late_tier(gs_source_t *s) {
    (void)s;
    return GS_TIER_RANDOM;
}
static void late_close(gs_source_t *s) {
    (void)s;
}
static int late_poll(gs_source_t *s, int timeout_ms) {
    late_t *l = s->ctx;
    (void)timeout_ms;
    l->polls++;
    l->not_yet--;
    return 0;
}
static const gs_source_ops_t late_ops = {late_read, late_size, late_key, late_tier, late_close, late_poll};

// GS_EAGAIN is waited out: a raw read reports it; read_exact polls and reads
// again until the bytes are there; a view (and a locked wrapper) polls the
// source it reads.
TEST(test_not_yet_is_waited_out_with_poll) {
    late_t l = {.not_yet = 3};
    gs_source_t *s = peel_source_new(&late_ops, &l, NULL);
    uint8_t b[16];
    ASSERT_EQ_INT(GS_EAGAIN, (int)gs_source_read(s, 0, b, sizeof(b)));
    ASSERT_EQ_INT(0, gs_source_poll(s, 0)); // one step: two refusals left
    ASSERT_EQ_INT(1, l.polls);

    gs_source_t *v = gs_source_view(s, 100, 1000, NULL);
    gs_source_t *lk = gs_source_locked(v);
    ASSERT_EQ_INT(0, gs_source_read_exact(lk, 10, b, sizeof(b)));
    for (int i = 0; i < 16; i++)
        ASSERT_EQ_INT(pattern(110 + (uint64_t)i), b[i]);
    ASSERT_EQ_INT(3, l.polls); // two more polls, through the lock and the view
    ASSERT_EQ_INT(0, l.not_yet);

    // A source that never makes progress is given up on, not spun on forever.
    l.not_yet = 1 << 30;
    ASSERT_EQ_INT(GS_EAGAIN, gs_source_read_exact(lk, 0, b, sizeof(b)));
    gs_source_release(lk);
    gs_source_release(v);
    gs_source_release(s);
}

int main(void) {
    RUN(test_view_arithmetic_at_boundaries);
    RUN(test_key_containment);
    RUN(test_decode_through_backward_reads_never_redecode);
    RUN(test_chunk_cache_coalesces_concurrent_readers);
    RUN(test_chunk_cache_spills_evicted_chunks);
    RUN(test_detection_reads_within_the_budget);
    RUN(test_unwrap_diskcopy_inside_gzip);
    RUN(test_not_yet_is_waited_out_with_poll);
    RUN(test_chunk_cache_budgets_change_at_run_time);
    RUN(test_key_same_ignores_only_host_times);
    fprintf(stderr, "All source tests passed\n");
    return 0;
}
