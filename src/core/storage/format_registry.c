// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// format_registry.c
// The format table: wrappers (built in) and namespace formats (registered).
// See format_registry.h.

#include "format_registry.h"

#include "common.h"
#include "image_chunkmap.h"
#include "log.h"
#include "storage_util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("image")

// How many wrapper layers are peeled before giving up (a file that detects
// as a wrapper of itself, forever).
#define MAX_UNWRAP 8

// ============================================================================
// DiskCopy 4.2
// ============================================================================

bool dc42_parse_header(const uint8_t *hdr, size_t len, uint64_t file_size, uint32_t *data_size, uint32_t *tag_size) {
    if (len < DISKCOPY_HEADER_SIZE || file_size < DISKCOPY_HEADER_SIZE)
        return false;
    uint16_t magic = RD_BE16(hdr + 0x52);
    if (magic != 0x0100)
        return false;
    uint32_t ds = RD_BE32(hdr + 0x40), ts = RD_BE32(hdr + 0x44);
    if (ds == 0 || (ds % 512) != 0)
        return false;
    if ((uint64_t)DISKCOPY_HEADER_SIZE + ds + ts > file_size)
        return false;
    if (data_size)
        *data_size = ds;
    if (tag_size)
        *tag_size = ts;
    return true;
}

static bool dc42_detect(const gs_probe_t *p) {
    return dc42_parse_header(p->p.head, p->p.head_len, p->p.size, NULL, NULL);
}

// The payload is the data section: a view past the header.
static int dc42_unwrap(const gs_probe_t *p, gs_source_t **data, gs_source_t **rsrc) {
    uint32_t ds = 0;
    if (!dc42_parse_header(p->p.head, p->p.head_len, p->p.size, &ds, NULL))
        return -EINVAL;
    char *key = gs_str_printf("%s#dc42", gs_source_key(p->data));
    *data = gs_source_view(p->data, DISKCOPY_HEADER_SIZE, ds, key);
    free(key);
    *rsrc = NULL;
    return *data ? 0 : -ENOMEM;
}

// ============================================================================
// LisaEm ProFile images (compatibility: DiskCopy 4.2, 20-byte tags,
// logical block order; see format_registry.h)
// ============================================================================

uint32_t lisaem_profile_blocks(const uint8_t *hdr, size_t len, uint64_t file_size) {
    uint32_t ds = 0, ts = 0;
    if (!dc42_parse_header(hdr, len, file_size, &ds, &ts))
        return 0;
    uint32_t blocks = ds / 512;
    if (blocks == 0 || (blocks % LISAEM_INTERLEAVE) != 0)
        return 0;
    if ((uint64_t)ts != (uint64_t)blocks * LISAEM_TAG_BYTES)
        return 0;
    return blocks;
}

static bool lisaem_detect(const gs_probe_t *p) {
    return lisaem_profile_blocks(p->p.head, p->p.head_len, p->p.size) != 0;
}

#define LISAEM_BLOCK (512u + LISAEM_TAG_BYTES) // one ProFile block, tag first

typedef struct {
    gs_source_t *file; // the DiskCopy file (borrowed: the source's parent)
    uint32_t blocks;
    char *key;
} lisaem_src_t;

// Read `len` bytes at `off` of the ProFile-order view: each 532-byte block is
// its 20 tag bytes, then its 512 data bytes, both taken from the logical
// block the interleave puts there.
static int64_t lisaem_read(gs_source_t *s, uint64_t off, void *buf, size_t len) {
    lisaem_src_t *m = s->ctx;
    uint64_t size = (uint64_t)m->blocks * LISAEM_BLOCK;
    if (off >= size)
        return 0;
    if (len > size - off)
        len = (size_t)(size - off);
    uint64_t tags_at = DISKCOPY_HEADER_SIZE + (uint64_t)m->blocks * 512;
    size_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t block = (uint32_t)(pos / LISAEM_BLOCK);
        uint32_t within = (uint32_t)(pos % LISAEM_BLOCK);
        uint64_t logical = lisaem_logical_block(block);
        uint64_t from;
        size_t n;
        if (within < LISAEM_TAG_BYTES) {
            from = tags_at + logical * LISAEM_TAG_BYTES + within;
            n = LISAEM_TAG_BYTES - within;
        } else {
            from = DISKCOPY_HEADER_SIZE + logical * 512 + (within - LISAEM_TAG_BYTES);
            n = LISAEM_BLOCK - within;
        }
        if (n > len - done)
            n = len - done;
        int64_t got = gs_source_read(m->file, from, (uint8_t *)buf + done, n);
        if (got < 0)
            return done ? (int64_t)done : got;
        if ((size_t)got != n)
            return done ? (int64_t)done : -EIO;
        done += n;
    }
    return (int64_t)done;
}

static uint64_t lisaem_size(gs_source_t *s) {
    return (uint64_t)((lisaem_src_t *)s->ctx)->blocks * LISAEM_BLOCK;
}

static const char *lisaem_key(gs_source_t *s) {
    return ((lisaem_src_t *)s->ctx)->key;
}

static gs_tier_t lisaem_tier(gs_source_t *s) {
    return gs_source_tier(((lisaem_src_t *)s->ctx)->file); // pure arithmetic over the file
}

static void lisaem_close(gs_source_t *s) {
    lisaem_src_t *m = s->ctx;
    if (m) {
        free(m->key);
        free(m);
    }
}

static const gs_source_ops_t lisaem_ops = {lisaem_read, lisaem_size, lisaem_key, lisaem_tier, lisaem_close, NULL};

static int lisaem_unwrap(const gs_probe_t *p, gs_source_t **data, gs_source_t **rsrc) {
    *data = *rsrc = NULL;
    uint32_t blocks = lisaem_profile_blocks(p->p.head, p->p.head_len, p->p.size);
    if (!blocks)
        return -EINVAL;
    lisaem_src_t *m = calloc(1, sizeof(*m));
    if (!m)
        return -ENOMEM;
    m->file = p->data;
    m->blocks = blocks;
    m->key = gs_str_printf("%s#lisaem", gs_source_key(p->data));
    if (!m->key) {
        free(m);
        return -ENOMEM;
    }
    // The file is the parent: retained for the view's life, and its poll
    // serves a remote file's "not yet".
    *data = peel_source_new(&lisaem_ops, m, p->data);
    return *data ? 0 : -ENOMEM;
}

// ============================================================================
// UDIF and NDIF
// ============================================================================

static bool udif_detect_probe(const gs_probe_t *p) {
    return udif_source_detect(p->p.tail, p->p.tail_len);
}

static int udif_unwrap(const gs_probe_t *p, gs_source_t **data, gs_source_t **rsrc) {
    int err = 0;
    *data = udif_source_open(p->data, &err);
    *rsrc = NULL;
    return *data ? 0 : (err ? err : -EINVAL);
}

static bool ndif_detect_probe(const gs_probe_t *p) {
    return p->rsrc && ndif_source_detect(p->rsrc);
}

static int ndif_unwrap(const gs_probe_t *p, gs_source_t **data, gs_source_t **rsrc) {
    int err = 0;
    *data = ndif_source_open(p->data, p->rsrc, &err);
    *rsrc = NULL; // the decoded disk has no forks of its own
    return *data ? 0 : (err ? err : -EINVAL);
}

// ============================================================================
// Peeler's wrappers (BinHex, MacBinary, gzip)
// ============================================================================

// A peeler wrapper's detect, by name.
static bool peeler_detect(const gs_probe_t *p, const char *name) {
    int n = 0;
    const peel_format_desc_t *d = peel_formats(&n);
    for (int i = 0; i < n; i++)
        if (strcmp(d[i].name, name) == 0)
            return d[i].detect(&p->p);
    return false;
}

static bool hqx_detect_probe(const gs_probe_t *p) {
    return peeler_detect(p, "hqx");
}
static bool bin_detect_probe(const gs_probe_t *p) {
    return peeler_detect(p, "bin");
}
static bool gz_detect_probe(const gs_probe_t *p) {
    return peeler_detect(p, "gz");
}

// The one file's forks.  Decoding ones fill the scratch sink; each is
// wrapped in a lock, since peeler's sources are not thread-safe.
static int peeler_unwrap(const gs_probe_t *p, const char *name, gs_source_t **data, gs_source_t **rsrc) {
    *data = *rsrc = NULL;
    peel_err_t *err = NULL;
    peel_archive_t *a = peel_open_as(name, p->data, gs_scratch_sink(), NULL, &err);
    if (!a) {
        LOG(2, "%s wrapper '%s' did not open: %s", name, gs_source_key(p->data), peel_err_msg(err));
        peel_err_free(err);
        return -EINVAL;
    }
    const peel_entry_t *e = peel_entry(a, 0);
    int rc = -EINVAL;
    if (e && !e->is_dir) {
        gs_source_t *d = peel_open_fork(a, 0, PEEL_FORK_DATA, &err);
        gs_source_t *r = (d && e->rsrc_len) ? peel_open_fork(a, 0, PEEL_FORK_RSRC, &err) : NULL;
        if (d) {
            *data = gs_source_locked(d);
            *rsrc = r ? gs_source_locked(r) : NULL;
            rc = *data ? 0 : -ENOMEM;
        }
        gs_source_release(d);
        gs_source_release(r);
    }
    peel_err_free(err);
    peel_close(a);
    return rc;
}

static int hqx_unwrap(const gs_probe_t *p, gs_source_t **d, gs_source_t **r) {
    return peeler_unwrap(p, "hqx", d, r);
}
static int bin_unwrap(const gs_probe_t *p, gs_source_t **d, gs_source_t **r) {
    return peeler_unwrap(p, "bin", d, r);
}
static int gz_unwrap(const gs_probe_t *p, gs_source_t **d, gs_source_t **r) {
    return peeler_unwrap(p, "gz", d, r);
}

// ============================================================================
// The table
// ============================================================================

// Order: the trailer-identified container first (a UDIF's payload may
// begin with anything), then the fork-identified one, then header formats
// -- the LisaEm ProFile image before the plain DiskCopy 4.2 it refines.
static const gs_format_t g_wrappers[] = {
    {"udif",   GS_FMT_WRAPPER, "UDIF (.dmg) disk image",        udif_detect_probe, udif_unwrap,   NULL},
    {"ndif",   GS_FMT_WRAPPER, "Disk Copy 6 (NDIF) disk image", ndif_detect_probe, ndif_unwrap,   NULL},
    {"lisaem", GS_FMT_WRAPPER, "LisaEm ProFile disk image",     lisaem_detect,     lisaem_unwrap, NULL},
    {"dc42",   GS_FMT_WRAPPER, "Disk Copy 4.2 disk image",      dc42_detect,       dc42_unwrap,   NULL},
    {"hqx",    GS_FMT_WRAPPER, "BinHex 4.0",                    hqx_detect_probe,  hqx_unwrap,    NULL},
    {"bin",    GS_FMT_WRAPPER, "MacBinary",                     bin_detect_probe,  bin_unwrap,    NULL},
    {"gz",     GS_FMT_WRAPPER, "gzip",                          gz_detect_probe,   gz_unwrap,     NULL},
};
#define N_WRAPPERS (sizeof(g_wrappers) / sizeof(g_wrappers[0]))

#define MAX_FORMATS 32
static const gs_format_t *g_all[MAX_FORMATS];
static int g_n_all;

static void table_init(void) {
    if (g_n_all)
        return;
    for (size_t i = 0; i < N_WRAPPERS; i++)
        g_all[g_n_all++] = &g_wrappers[i];
}

void gs_format_register(const gs_format_t *f) {
    table_init();
    for (int i = 0; i < g_n_all; i++)
        if (strcmp(g_all[i]->name, f->name) == 0)
            return;
    if (g_n_all < MAX_FORMATS)
        g_all[g_n_all++] = f;
}

const gs_format_t *const *gs_formats(int *count) {
    table_init();
    if (count)
        *count = g_n_all;
    return g_all;
}

static struct gs_namespace *(*g_wrapper_ns)(gs_source_t *src, const char *format);

void gs_format_set_wrapper_namespace(struct gs_namespace *(*open)(gs_source_t *src, const char *format)) {
    g_wrapper_ns = open;
}

// ============================================================================
// Detection and unwrapping
// ============================================================================

int gs_probe_init(gs_probe_t *p, gs_source_t *data, gs_source_t *rsrc) {
    memset(p, 0, sizeof(*p));
    int rc = peel_probe_init(&p->p, data);
    if (rc != 0)
        return rc == -5 ? -EIO : rc;
    p->data = data;
    p->rsrc = rsrc;
    return 0;
}

void gs_probe_free(gs_probe_t *p) {
    peel_probe_free(&p->p);
}

const gs_format_t *gs_format_detect(const gs_probe_t *p, gs_fmt_kind_t kind) {
    table_init();
    for (int i = 0; i < g_n_all; i++)
        if (g_all[i]->kind == kind && g_all[i]->detect(p))
            return g_all[i];
    return NULL;
}

// Append `name` to the chain.
static void chain_add(char *chain, size_t cap, const char *name) {
    size_t n = strlen(chain);
    snprintf(chain + n, cap - n, "%s%s", n ? "+" : "", name);
}

static bool is_peeler_wrapper(const gs_format_t *f) {
    return strcmp(f->name, "hqx") == 0 || strcmp(f->name, "bin") == 0 || strcmp(f->name, "gz") == 0;
}

int gs_format_unwrap(gs_source_t *data, gs_source_t *rsrc, gs_unwrapped_t *out) {
    memset(out, 0, sizeof(*out));
    out->data = gs_source_retain(data);
    out->rsrc = gs_source_retain(rsrc);
    for (int depth = 0; depth < MAX_UNWRAP; depth++) {
        gs_probe_t p;
        if (gs_probe_init(&p, out->data, out->rsrc) != 0)
            break;
        const gs_format_t *f = gs_format_detect(&p, GS_FMT_WRAPPER);
        gs_source_t *nd = NULL, *nr = NULL;
        int rc = f ? f->unwrap(&p, &nd, &nr) : -ENOENT;
        gs_probe_free(&p);
        if (rc != 0) {
            if (f) {
                out->failed_format = f->name;
                out->failed_rc = rc;
            }
            break;
        }
        chain_add(out->chain, sizeof(out->chain), f->name);
        if (strcmp(f->name, "dc42") == 0) {
            gs_source_release(out->dc42);
            out->dc42 = gs_source_retain(out->data);
        }
        if (is_peeler_wrapper(f)) {
            gs_source_release(out->peeler_outer);
            out->peeler_outer = gs_source_retain(out->data);
            out->peeler_format = f->name;
        }
        gs_source_release(out->data);
        gs_source_release(out->rsrc);
        out->data = nd;
        out->rsrc = nr;
    }
    return 0;
}

void gs_unwrapped_free(gs_unwrapped_t *u) {
    if (!u)
        return;
    gs_source_release(u->data);
    gs_source_release(u->rsrc);
    gs_source_release(u->dc42);
    gs_source_release(u->peeler_outer);
    memset(u, 0, sizeof(*u));
}

struct gs_namespace *gs_format_open_namespace(gs_source_t *data, gs_source_t *rsrc, const char **format, int *err) {
    int e = 0;
    if (!err)
        err = &e;
    *err = -ENOTDIR;
    if (format)
        *format = NULL;
    gs_unwrapped_t u;
    gs_format_unwrap(data, rsrc, &u);
    struct gs_namespace *ns = NULL;
    gs_probe_t p;
    if (gs_probe_init(&p, u.data, u.rsrc) == 0) {
        const gs_format_t *f = gs_format_detect(&p, GS_FMT_NAMESPACE);
        gs_probe_free(&p);
        if (f) {
            ns = f->open_namespace(u.data, u.rsrc);
            if (ns && format)
                *format = f->name;
        }
    }
    // A wrapper around a file that is no tree: the wrapper is the tree, its
    // one file inside (an application in a .bin, a document in a .hqx).
    if (!ns && u.peeler_outer && g_wrapper_ns) {
        ns = g_wrapper_ns(u.peeler_outer, u.peeler_format);
        if (ns && format)
            *format = u.peeler_format;
    }
    gs_unwrapped_free(&u);
    if (ns)
        *err = 0;
    return ns;
}

bool gs_format_is_namespace(gs_source_t *data, gs_source_t *rsrc) {
    gs_probe_t p;
    if (gs_probe_init(&p, data, rsrc) != 0)
        return false;
    // A peeler wrapper always opens as at least its one file; a disk
    // wrapper (dc42, udif, ndif) is a tree when its disk is.  Checking the
    // outermost layer keeps this cheap: no wrapper is decoded to answer.
    const gs_format_t *w = gs_format_detect(&p, GS_FMT_WRAPPER);
    bool yes = false;
    if (w)
        yes = is_peeler_wrapper(w) ? g_wrapper_ns != NULL : true;
    else
        yes = gs_format_detect(&p, GS_FMT_NAMESPACE) != NULL;
    gs_probe_free(&p);
    return yes;
}

const gs_format_t *gs_format_contents(gs_source_t *data, gs_source_t *rsrc) {
    gs_probe_t p;
    if (!data || gs_probe_init(&p, data, rsrc) != 0)
        return NULL;
    const gs_format_t *f = gs_format_detect(&p, GS_FMT_NAMESPACE);
    gs_probe_free(&p);
    return f;
}
