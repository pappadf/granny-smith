// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gz.c — gzip (.gz, RFC 1952) as a one-file wrapper.
//
// A gzip file is one or more members, each a small header, a DEFLATE stream
// and an 8-byte trailer (CRC-32, size mod 2^32).  Opening reads the header
// for the name and the tail for a size hint; the payload decodes through
// the resumable inflater as reads reach it, member after member.  Only the
// last member's trailer is at the tail (and it holds the size mod 2^32), so
// that hint is exact for a single-member file under 4 GiB and wrong for any
// other.  The entry lists it, as `gzip -l` does; the fork's source earns its
// true size with one pass instead (the EARNED tier).
//
// A BGZF file (every member a block of at most 64 KiB, each header carrying
// its compressed size in a "BC" extra subfield -- samtools/htslib, and
// dictzip's cousin) can be read at random: opening walks the block headers
// once, and a read then inflates only the block it lands in.  That is the
// INDEXED tier, honestly earned.

#include "internal.h"

// ============================================================================
// Constants
// ============================================================================

#define GZ_ID1        0x1f
#define GZ_ID2        0x8b
#define GZ_CM_DEFLATE 8

#define GZ_FHCRC     0x02
#define GZ_FEXTRA    0x04
#define GZ_FNAME     0x08
#define GZ_FCOMMENT  0x10
#define GZ_FRESERVED 0xE0

#define GZ_BGZF_MAX_BLOCK 65536u

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// ============================================================================
// Header
// ============================================================================

typedef struct {
    size_t header_len; // bytes before the deflate stream
    uint32_t mtime;
    char name[256]; // FNAME, if any
    bool bgzf;
    uint32_t bsize; // BGZF: the whole block's size
} gz_header_t;

// Parse a member header from `p` (`len` bytes available).  0 or -1.
static int gz_parse_header(const uint8_t *p, size_t len, gz_header_t *h) {
    memset(h, 0, sizeof(*h));
    if (len < 10 || p[0] != GZ_ID1 || p[1] != GZ_ID2 || p[2] != GZ_CM_DEFLATE || (p[3] & GZ_FRESERVED))
        return -1;
    uint8_t flg = p[3];
    h->mtime = le32(p + 4);
    size_t pos = 10;
    if (flg & GZ_FEXTRA) {
        if (len - pos < 2)
            return -1;
        size_t xlen = (size_t)(p[pos] | p[pos + 1] << 8);
        pos += 2;
        if (len - pos < xlen)
            return -1;
        // Subfields: SI1 SI2 LEN(2) data.  BGZF's is 'B' 'C' with 2 bytes.
        size_t x = pos, xend = pos + xlen;
        while (xend - x >= 4) {
            size_t slen = (size_t)(p[x + 2] | p[x + 3] << 8);
            if (xend - x - 4 < slen)
                break;
            if (p[x] == 'B' && p[x + 1] == 'C' && slen == 2) {
                h->bgzf = true;
                h->bsize = (uint32_t)(p[x + 4] | p[x + 5] << 8) + 1;
            }
            x += 4 + slen;
        }
        pos = xend;
    }
    if (flg & GZ_FNAME) {
        size_t start = pos;
        while (pos < len && p[pos])
            pos++;
        if (pos >= len)
            return -1;
        size_t n = pos - start < sizeof(h->name) - 1 ? pos - start : sizeof(h->name) - 1;
        memcpy(h->name, p + start, n);
        h->name[n] = '\0';
        pos++;
    }
    if (flg & GZ_FCOMMENT) {
        while (pos < len && p[pos])
            pos++;
        if (pos >= len)
            return -1;
        pos++;
    }
    if (flg & GZ_FHCRC) {
        if (len - pos < 2)
            return -1;
        pos += 2;
    }
    h->header_len = pos;
    return 0;
}

bool gz_detect(const uint8_t *src, size_t len) {
    gz_header_t h;
    return len >= 18 && gz_parse_header(src, len, &h) == 0;
}

static bool gz_detect_probe(const peel_probe_t *p) {
    return p->size >= 18 && gz_parse_header(p->head, p->head_len, &(gz_header_t){0}) == 0;
}

// ============================================================================
// Whole-buffer decode (peel_gz, and decode() for the buffer API)
// ============================================================================

// The rest of a buffer, pulled by the inflater.
typedef struct {
    const uint8_t *s;
    size_t len, pos;
} gz_mem_t;

static int64_t gz_mem_pull(void *ctx, uint8_t *buf, size_t cap) {
    gz_mem_t *m = ctx;
    size_t n = m->len - m->pos < cap ? m->len - m->pos : cap;
    memcpy(buf, m->s + m->pos, n);
    m->pos += n;
    return (int64_t)n;
}

peel_buf_t peel_gz(const uint8_t *src, size_t len, peel_err_t **err) {
    *err = NULL;
    grow_buf_t out;
    decode_ctx_t ctx;
    dctx_init(&ctx);
    if (setjmp(ctx.jmp) != 0) {
        dctx_cleanup(&ctx);
        *err = make_err("%s", ctx.errmsg);
        return (peel_buf_t){0};
    }
    grow_init(&out, len * 2, &ctx);
    size_t pos = 0;
    while (pos < len) {
        gz_header_t h;
        if (gz_parse_header(src + pos, len - pos, &h) != 0) {
            if (pos == 0)
                decode_abort(&ctx, "gzip: bad header");
            break; // trailing garbage after the last member is ignored, as gzip does
        }
        pos += h.header_len;
        // Inflate this member into the output, tracking what it consumed.
        gz_mem_t m = {src + pos, len - pos, 0};
        peel_inflater_t *z = peel_inflater_new(gz_mem_pull, &m);
        if (!z)
            decode_abort(&ctx, "gzip: out of memory");
        size_t start = out.len;
        uint32_t crc = 0;
        for (;;) {
            uint8_t chunk[65536];
            size_t n = 0;
            int rc = peel_inflater_run(z, chunk, sizeof(chunk), &n);
            if (rc < 0) {
                peel_inflater_free(z);
                decode_abort(&ctx, "gzip: corrupt deflate stream");
            }
            if (out.len + n > PEEL_MAX_FORK) {
                peel_inflater_free(z);
                decode_abort(&ctx, "gzip: decodes to over the %u MiB limit", (unsigned)(PEEL_MAX_FORK >> 20));
            }
            grow_append(&out, chunk, n, &ctx);
            crc = peel_crc32(crc, chunk, n);
            if (rc == 1)
                break;
        }
        peel_inflater_align(z);
        size_t used = (size_t)peel_inflater_consumed(z);
        peel_inflater_free(z);
        pos += used;
        if (len - pos < 8)
            decode_abort(&ctx, "gzip: truncated trailer");
        if (le32(src + pos) != crc || le32(src + pos + 4) != (uint32_t)(out.len - start))
            decode_abort(&ctx, "gzip: CRC or size mismatch");
        pos += 8;
    }
    peel_buf_t b = grow_finish(&out);
    dctx_release(&ctx, b.data);
    dctx_cleanup(&ctx);
    return b;
}

// ============================================================================
// Structure-first access
// ============================================================================

// One BGZF block: where it starts in the file and in the output.
typedef struct {
    uint64_t coff; // compressed offset of the block
    uint64_t uoff; // uncompressed offset of its first byte
    uint32_t csize; // whole block, header to trailer
    uint32_t usize;
} bgzf_block_t;

typedef struct {
    uint64_t first_member_data; // where the first deflate stream starts
    bgzf_block_t *blocks; // BGZF only
    size_t n_blocks;
} gz_priv_t;

// Walk every BGZF block header once: the index.  0 or -1.
static int bgzf_index(peel_archive_t *a, gz_priv_t *g, uint64_t *total) {
    uint64_t size = peel_source_size(a->src), coff = 0, uoff = 0;
    size_t cap = 0;
    while (coff < size) {
        uint8_t hdr[18];
        if (size - coff < 28 || peel_source_read_exact(a->src, coff, hdr, sizeof(hdr)) != 0)
            return -1;
        gz_header_t h;
        if (gz_parse_header(hdr, sizeof(hdr), &h) != 0 || !h.bgzf || h.bsize < 28 || h.bsize > size - coff)
            return -1;
        uint8_t isz[4];
        if (peel_source_read_exact(a->src, coff + h.bsize - 4, isz, 4) != 0)
            return -1;
        uint32_t usize = le32(isz);
        if (usize > GZ_BGZF_MAX_BLOCK)
            return -1;
        if (g->n_blocks == cap) {
            cap = cap ? cap * 2 : 256;
            bgzf_block_t *nb = realloc(g->blocks, cap * sizeof(*nb));
            if (!nb)
                return -1;
            g->blocks = nb;
        }
        g->blocks[g->n_blocks++] = (bgzf_block_t){coff, uoff, h.bsize, usize};
        coff += h.bsize;
        uoff += usize;
    }
    *total = uoff;
    return 0;
}

static int gz_open(peel_archive_t *a, const peel_probe_t *p, peel_err_t **err) {
    gz_header_t h;
    if (gz_parse_header(p->head, p->head_len, &h) != 0) {
        *err = make_err("gzip: bad header");
        return -1;
    }
    gz_priv_t *g = calloc(1, sizeof(*g));
    peel_entry_t *e = g ? peel_archive_add(a) : NULL;
    if (!e) {
        free(g);
        *err = make_err("gzip: out of memory");
        return -1;
    }
    a->priv = g;
    g->first_member_data = h.header_len;
    // The name: the one the header stores, else the file's own less ".gz".
    size_t np = 0;
    if (h.name[0]) {
        peel_append_segment(e->path, sizeof(e->path), &np, (const uint8_t *)h.name, strlen(h.name));
    } else {
        const char *key = peel_source_key(a->src);
        const char *base = strrchr(key, '/');
        base = base ? base + 1 : key;
        size_t n = strlen(base);
        char tmp[256];
        if (n > 4 && strcmp(base + n - 4, ".tgz") == 0)
            snprintf(tmp, sizeof(tmp), "%.*s.tar", (int)(n - 4), base); // x.tgz holds x.tar
        else if (n > 3 && strcmp(base + n - 3, ".gz") == 0)
            snprintf(tmp, sizeof(tmp), "%.*s", (int)(n - 3), base);
        else
            snprintf(tmp, sizeof(tmp), "data");
        peel_append_segment(e->path, sizeof(e->path), &np, (const uint8_t *)tmp, strlen(tmp));
    }
    e->mtime = h.mtime;
    e->data_method = GZ_CM_DEFLATE;
    e->data_packed = p->size;
    uint64_t total = 0;
    if (h.bgzf && bgzf_index(a, g, &total) == 0) {
        e->data_len = total;
        e->data_tier = PEEL_TIER_INDEXED;
    } else {
        free(g->blocks);
        g->blocks = NULL;
        g->n_blocks = 0;
        e->data_len = p->tail_len >= 4 ? le32(p->tail + p->tail_len - 4) : 0;
        e->data_tier = PEEL_TIER_EARNED;
    }
    return 0;
}

static peel_buf_t gz_decode(peel_archive_t *a, int i, int fork, peel_err_t **err) {
    (void)i;
    if (fork == PEEL_FORK_RSRC)
        return (peel_buf_t){0};
    peel_buf_t text = peel_source_slurp(a->src, err);
    if (*err)
        return (peel_buf_t){0};
    peel_buf_t out = peel_gz(text.data, text.size, err);
    peel_free(&text);
    return out;
}

// ---- Streaming producer: member after member ----

typedef struct {
    peel_producer_t base;
    peel_source_t *src;
    uint64_t size;
    uint64_t member; // where the current member's deflate stream starts
    uint64_t pos; // next compressed byte to pull
    peel_inflater_t *z;
    uint32_t crc, count;
    bool done;
} gz_producer_t;

static int64_t gz_pull(void *ctx, uint8_t *buf, size_t cap) {
    gz_producer_t *g = ctx;
    uint64_t left = g->size - g->pos;
    size_t n = left < cap ? (size_t)left : cap;
    if (n == 0)
        return 0;
    int rc = peel_source_read_exact(g->src, g->pos, buf, n);
    if (rc != 0)
        return rc;
    g->pos += n;
    return (int64_t)n;
}

// Finish a member: check its trailer; start the next one if there is one.
static int gz_next_member(gz_producer_t *g) {
    peel_inflater_align(g->z);
    uint64_t end = g->member + peel_inflater_consumed(g->z);
    peel_inflater_free(g->z);
    g->z = NULL;
    uint8_t tr[8];
    if (g->size - end < 8 || peel_source_read_exact(g->src, end, tr, 8) != 0)
        return -1;
    if (le32(tr) != g->crc || le32(tr + 4) != g->count)
        return -1;
    end += 8;
    uint8_t hdr[1024];
    size_t n = g->size - end < sizeof(hdr) ? (size_t)(g->size - end) : sizeof(hdr);
    gz_header_t h;
    if (n < 18 || peel_source_read_exact(g->src, end, hdr, n) != 0 || gz_parse_header(hdr, n, &h) != 0) {
        g->done = true; // the last member (trailing garbage is ignored)
        return 0;
    }
    g->member = g->pos = end + h.header_len;
    g->crc = g->count = 0;
    g->z = peel_inflater_new(gz_pull, g);
    return g->z ? 0 : -1;
}

static int gz_run(peel_producer_t *p, uint8_t *out, size_t cap, size_t *n) {
    gz_producer_t *g = (gz_producer_t *)p;
    *n = 0;
    while (!g->done && *n < cap) {
        size_t k = 0;
        int rc = peel_inflater_run(g->z, out + *n, cap - *n, &k);
        if (rc < 0) {
            snprintf(p->err, sizeof(p->err), "gzip: corrupt deflate stream");
            return -5;
        }
        g->crc = peel_crc32(g->crc, out + *n, k);
        g->count += (uint32_t)k;
        *n += k;
        if (rc == 1 && gz_next_member(g) != 0) {
            snprintf(p->err, sizeof(p->err), "gzip: CRC or size mismatch");
            return -5;
        }
    }
    return g->done ? 1 : 0;
}

static void gz_producer_free(peel_producer_t *p) {
    gz_producer_t *g = (gz_producer_t *)p;
    peel_inflater_free(g->z);
    peel_source_release(g->src);
    free(g);
}

static peel_producer_t *gz_producer(peel_archive_t *a, int i, int fork, peel_err_t **err) {
    (void)i;
    gz_priv_t *gp = a->priv;
    if (fork == PEEL_FORK_RSRC || gp->blocks)
        return NULL;
    gz_producer_t *g = calloc(1, sizeof(*g));
    if (!g) {
        *err = make_err("gzip: out of memory");
        return NULL;
    }
    g->base.run = gz_run;
    g->base.free = gz_producer_free;
    g->src = peel_source_retain(a->src);
    g->size = peel_source_size(a->src);
    g->member = g->pos = gp->first_member_data;
    g->z = peel_inflater_new(gz_pull, g);
    if (!g->z) {
        gz_producer_free(&g->base);
        *err = make_err("gzip: out of memory");
        return NULL;
    }
    return &g->base;
}

// ---- BGZF: random access by block ----

typedef struct {
    peel_source_t *src; // retained
    bgzf_block_t *blocks; // a copy of the index
    size_t n_blocks;
    uint64_t total;
    char *key;
    size_t cur; // block held in `buf`, or SIZE_MAX
    uint8_t buf[GZ_BGZF_MAX_BLOCK];
    uint8_t cbuf[GZ_BGZF_MAX_BLOCK];
} bgzf_src_t;

// Inflate block `b` into s->buf.  0 or -5.
static int bgzf_load(bgzf_src_t *s, size_t b) {
    if (s->cur == b)
        return 0;
    const bgzf_block_t *blk = &s->blocks[b];
    if (blk->csize > sizeof(s->cbuf) || peel_source_read_exact(s->src, blk->coff, s->cbuf, blk->csize) != 0)
        return -5;
    gz_header_t h;
    if (gz_parse_header(s->cbuf, blk->csize, &h) != 0 || h.header_len + 8 > blk->csize)
        return -5;
    int64_t n = peel_inflate(s->cbuf + h.header_len, blk->csize - h.header_len - 8, s->buf, sizeof(s->buf));
    if (n != (int64_t)blk->usize || peel_crc32(0, s->buf, blk->usize) != le32(s->cbuf + blk->csize - 8))
        return -5;
    s->cur = b;
    return 0;
}

static int64_t bgzf_read(peel_source_t *src, uint64_t off, void *buf, size_t len) {
    bgzf_src_t *s = src->ctx;
    if (off >= s->total)
        return 0;
    // Binary search for the block holding `off`.
    size_t lo = 0, hi = s->n_blocks;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (s->blocks[mid].uoff <= off)
            lo = mid;
        else
            hi = mid;
    }
    // Skip empty blocks (the EOF marker block has usize 0).
    while (lo < s->n_blocks && s->blocks[lo].uoff + s->blocks[lo].usize <= off)
        lo++;
    if (lo == s->n_blocks)
        return 0;
    int rc = bgzf_load(s, lo);
    if (rc != 0)
        return rc;
    uint64_t in = off - s->blocks[lo].uoff;
    size_t n = s->blocks[lo].usize - (size_t)in < len ? s->blocks[lo].usize - (size_t)in : len;
    memcpy(buf, s->buf + in, n);
    return (int64_t)n;
}

static uint64_t bgzf_size(peel_source_t *src) {
    return ((bgzf_src_t *)src->ctx)->total;
}

static const char *bgzf_key(peel_source_t *src) {
    return ((bgzf_src_t *)src->ctx)->key;
}

static peel_tier_t bgzf_tier(peel_source_t *src) {
    (void)src;
    return PEEL_TIER_INDEXED;
}

static void bgzf_close(peel_source_t *src) {
    bgzf_src_t *s = src->ctx;
    if (!s)
        return;
    peel_source_release(s->src);
    free(s->blocks);
    free(s->key);
    free(s);
}

static const peel_source_ops_t bgzf_ops = {bgzf_read, bgzf_size, bgzf_key, bgzf_tier, bgzf_close, NULL};

static peel_source_t *gz_open_fork(peel_archive_t *a, int i, int fork, peel_err_t **err) {
    gz_priv_t *gp = a->priv;
    if (fork != PEEL_FORK_DATA || !gp->blocks)
        return NULL; // the generic paths
    bgzf_src_t *s = calloc(1, sizeof(*s));
    char key[1024];
    peel_fork_key(a, i, fork, key, sizeof(key));
    size_t kl = strlen(key);
    if (s) {
        s->blocks = malloc(gp->n_blocks * sizeof(*s->blocks));
        s->key = malloc(kl + 1);
    }
    if (!s || !s->blocks || !s->key) {
        if (s) {
            free(s->blocks);
            free(s->key);
        }
        free(s);
        *err = make_err("gzip: out of memory");
        return NULL;
    }
    memcpy(s->blocks, gp->blocks, gp->n_blocks * sizeof(*s->blocks));
    memcpy(s->key, key, kl + 1);
    s->n_blocks = gp->n_blocks;
    s->total = a->entries[i].data_len;
    s->cur = (size_t)-1;
    s->src = peel_source_retain(a->src);
    peel_source_t *out = peel_source_new(&bgzf_ops, s, NULL);
    if (!out)
        *err = make_err("gzip: out of memory");
    return out;
}

// Only a BGZF index states the payload's size; the tail ISIZE is a hint.
static bool gz_len_exact(peel_archive_t *a, int i, int fork) {
    (void)i;
    (void)fork;
    return ((gz_priv_t *)a->priv)->blocks != NULL;
}

static void gz_close(peel_archive_t *a) {
    gz_priv_t *g = a->priv;
    if (!g)
        return;
    free(g->blocks);
    free(g);
}

const peel_fmt_t peel_fmt_gz = {
    .desc = {"gz", true, gz_detect_probe},
    .open = gz_open,
    .decode = gz_decode,
    .producer = gz_producer,
    .open_fork = gz_open_fork,
    .len_exact = gz_len_exact,
    .close = gz_close,
};
