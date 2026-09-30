// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// zip.c — Zip (.zip) archives (PKWARE APPNOTE.TXT 6.3.x).
//
// Structure first: the End Of Central Directory record at the tail points at
// the central directory, which lists every member -- name, sizes, method,
// CRC and where its local header is.  Opening a zip reads the tail and the
// central directory and nothing else; the local headers and the members'
// bytes are read only when a member is opened.
//
// Stored members (method 0) are views of the archive; deflated ones
// (method 8) decode through the resumable inflater, as far as reads reach.
// Zip64 sizes and offsets, and a self-extractor stub (or anything else)
// prepended to the archive, are handled.  Encrypted members are listed but
// refuse to open.
//
// Archives made on a Mac carry each file's resource fork and Finder info in
// a parallel "__MACOSX/<dir>/._<name>" AppleDouble member.  Those are folded
// back into the file they describe -- its rsrc fork, type and creator -- and
// not listed on their own.

#include "appledouble.h"
#include "internal.h"

// ============================================================================
// Constants
// ============================================================================

#define ZIP_EOCD_SIG     0x06054b50u
#define ZIP_EOCD_LEN     22u
#define ZIP_EOCD_MAXSCAN (ZIP_EOCD_LEN + 65535u) // the record plus the longest comment
#define ZIP64_LOC_SIG    0x07064b50u
#define ZIP64_LOC_LEN    20u
#define ZIP64_EOCD_SIG   0x06064b50u
#define ZIP64_EOCD_LEN   56u
#define ZIP_CEN_SIG      0x02014b50u
#define ZIP_CEN_LEN      46u
#define ZIP_LOC_SIG      0x04034b50u
#define ZIP_LOC_LEN      30u

#define ZIP_FLAG_ENCRYPTED 0x0001u

#define ZIP_METHOD_STORED  0
#define ZIP_METHOD_DEFLATE 8

// The largest AppleDouble companion folded in at open (they are read then).
#define ZIP_MAX_APPLEDOUBLE (16u * 1024u * 1024u)

// Little-endian readers (zip is the one little-endian format here).
static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

// ============================================================================
// Types
// ============================================================================

// One central-directory record, reduced to what the reader needs.
typedef struct {
    uint16_t flags, method;
    uint32_t crc;
    uint64_t csize, usize;
    uint64_t local_off; // local header offset, SFX-corrected
} zip_member_t;

// Per peel entry: its member (-1 for a synthesised folder), and a resource
// fork folded in from an AppleDouble companion.
typedef struct {
    int member;
    uint8_t *ad_buf; // the companion's bytes (owned), or NULL
    const uint8_t *rsrc; // borrowed from ad_buf
} zip_slot_t;

typedef struct {
    zip_member_t *members;
    int n_members;
    zip_slot_t *slots; // one per peel entry
    int n_slots, cap_slots;
} zip_priv_t;

// ============================================================================
// Detection
// ============================================================================

// Position of the EOCD record in `tail` (the last `len` bytes of a source of
// `size` bytes), or -1.  The record must end exactly where its comment says.
static int64_t find_eocd(const uint8_t *tail, size_t len, uint64_t size) {
    if (len < ZIP_EOCD_LEN)
        return -1;
    size_t lo = len > ZIP_EOCD_MAXSCAN ? len - ZIP_EOCD_MAXSCAN : 0;
    for (size_t i = len - ZIP_EOCD_LEN + 1; i-- > lo;) {
        if (le32(tail + i) != ZIP_EOCD_SIG)
            continue;
        uint16_t comment = le16(tail + i + 20);
        if (i + ZIP_EOCD_LEN + comment == len)
            return (int64_t)(size - len + i);
    }
    return -1;
}

bool zip_detect(const uint8_t *src, size_t len) {
    return find_eocd(src, len, len) >= 0;
}

static bool zip_detect_probe(const peel_probe_t *p) {
    return find_eocd(p->tail, p->tail_len, p->size) >= 0;
}

// ============================================================================
// Helpers
// ============================================================================

// MS-DOS date and time to Unix seconds (taken as UTC; zip records local
// time with no zone).
static uint32_t dos_to_unix(uint16_t date, uint16_t time) {
    int year = 1980 + (date >> 9), mon = (date >> 5) & 15, day = date & 31;
    if (mon < 1 || mon > 12 || day < 1)
        return 0;
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int y = year - 1970;
    int64_t days = (int64_t)y * 365 + (y + 1) / 4 + cum[mon - 1] + day - 1;
    if (mon > 2 && (year % 4) == 0)
        days++;
    return (uint32_t)(days * 86400 + (time >> 11) * 3600 + ((time >> 5) & 63) * 60 + (time & 31) * 2);
}

// Build a sanitised entry path from a raw member name ('/'-separated).
// Returns false for a name with no components.
static bool zip_path(const uint8_t *name, size_t n, char *out, size_t cap) {
    size_t pos = 0, start = 0;
    out[0] = '\0';
    bool any = false;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && name[i] != '/' && name[i] != '\\')
            continue;
        if (i > start) {
            peel_append_segment(out, cap, &pos, name + start, i - start);
            any = true;
        }
        start = i + 1;
    }
    return any;
}

// Add a slot for a new peel entry.
static zip_slot_t *slot_add(zip_priv_t *z) {
    if (z->n_slots == z->cap_slots) {
        int ncap = z->cap_slots ? z->cap_slots * 2 : 16;
        zip_slot_t *ns = realloc(z->slots, (size_t)ncap * sizeof(*ns));
        if (!ns)
            return NULL;
        z->slots = ns;
        z->cap_slots = ncap;
    }
    zip_slot_t *s = &z->slots[z->n_slots++];
    memset(s, 0, sizeof(*s));
    s->member = -1;
    return s;
}

// Make sure every ancestor folder of `path` is an entry (zips often list
// only files).  0 or -1.
static int ensure_parents(peel_archive_t *a, zip_priv_t *z, const char *path) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *slash = strchr(buf, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        if (peel_lookup(a, buf) < 0) {
            peel_entry_t *e = peel_archive_add(a);
            zip_slot_t *s = e ? slot_add(z) : NULL;
            if (!s)
                return -1;
            snprintf(e->path, sizeof(e->path), "%s", buf);
            e->is_dir = true;
        }
        *slash = '/';
    }
    return 0;
}

// ============================================================================
// Open
// ============================================================================

// Locate the central directory: its offset in the source, size and entry
// count, Zip64 and a prepended stub included.  0 or -1 with *err.
static int zip_locate_cd(peel_archive_t *a, const peel_probe_t *p, uint64_t *cd_off, uint64_t *cd_size, uint64_t *count,
                         int64_t *delta, peel_err_t **err) {
    int64_t eocd = find_eocd(p->tail, p->tail_len, p->size);
    if (eocd < 0) {
        *err = make_err("ZIP: no end of central directory record");
        return -1;
    }
    const uint8_t *r = p->tail + (size_t)((uint64_t)eocd - (p->size - p->tail_len));
    if (le16(r + 4) != 0 || le16(r + 6) != 0) {
        *err = make_err("ZIP: multi-disk archives are not supported");
        return -1;
    }
    uint64_t n = le16(r + 10), size = le32(r + 12), off = le32(r + 16);
    uint64_t records_end = (uint64_t)eocd; // where the central directory must end
    if (n == 0xFFFF || size == 0xFFFFFFFFu || off == 0xFFFFFFFFu) {
        // Zip64: a locator just before the EOCD points at the Zip64 record.
        uint8_t loc[ZIP64_LOC_LEN], rec[ZIP64_EOCD_LEN];
        if ((uint64_t)eocd < ZIP64_LOC_LEN ||
            peel_source_read_exact(a->src, (uint64_t)eocd - ZIP64_LOC_LEN, loc, sizeof(loc)) != 0 ||
            le32(loc) != ZIP64_LOC_SIG) {
            *err = make_err("ZIP: Zip64 locator missing");
            return -1;
        }
        uint64_t rec_off = le64(loc + 8);
        // The record sits right before the locator; trust its position over
        // the stated offset when a stub was prepended.
        uint64_t rec_at = (uint64_t)eocd - ZIP64_LOC_LEN - ZIP64_EOCD_LEN;
        if (peel_source_read_exact(a->src, rec_at, rec, sizeof(rec)) != 0 || le32(rec) != ZIP64_EOCD_SIG) {
            rec_at = rec_off;
            if (peel_source_read_exact(a->src, rec_at, rec, sizeof(rec)) != 0 || le32(rec) != ZIP64_EOCD_SIG) {
                *err = make_err("ZIP: Zip64 end of central directory record missing");
                return -1;
            }
        }
        n = le64(rec + 32);
        size = le64(rec + 40);
        off = le64(rec + 48);
        records_end = rec_at;
    }
    if (size > records_end) {
        *err = make_err("ZIP: central directory larger than the archive");
        return -1;
    }
    // The central directory ends where the end records begin; anything the
    // stated offset disagrees by was prepended (a self-extractor stub).
    uint64_t actual = records_end - size;
    *delta = (int64_t)(actual - off);
    *cd_off = actual;
    *cd_size = size;
    *count = n;
    return 0;
}

// Fold "__MACOSX/<dir>/._<name>" companions into their files.
static void zip_fold_appledouble(peel_archive_t *a, zip_priv_t *z) {
    for (int i = 0; i < a->count; i++) {
        peel_entry_t *e = &a->entries[i];
        if (e->is_dir || strncmp(e->path, "__MACOSX/", 9) != 0)
            continue;
        const char *rest = e->path + 9;
        const char *base = strrchr(rest, '/');
        base = base ? base + 1 : rest;
        if (strncmp(base, "._", 2) != 0)
            continue;
        char target[512];
        snprintf(target, sizeof(target), "%.*s%s", (int)(base - rest), rest, base + 2);
        int t = peel_lookup(a, target);
        if (t < 0 || a->entries[t].is_dir || e->data_len > ZIP_MAX_APPLEDOUBLE)
            continue;
        peel_err_t *err = NULL;
        peel_buf_t buf = peel_read_fork(a, i, PEEL_FORK_DATA, &err);
        if (err) {
            peel_err_free(err);
            continue;
        }
        ad_file_t ad;
        if (!buf.data || !ad_detect(buf.data, buf.size) || ad_parse(buf.data, buf.size, &ad) != 0) {
            peel_free(&buf);
            continue;
        }
        peel_entry_t *te = &a->entries[t];
        zip_slot_t *ts = &z->slots[t];
        if (ad.finder && ad.finder_len >= 10) {
            te->mac_type = rd32be(ad.finder);
            te->mac_creator = rd32be(ad.finder + 4);
            te->finder_flags = rd16be(ad.finder + 8);
        }
        free(ts->ad_buf);
        ts->ad_buf = buf.data; // owned now
        ts->rsrc = ad.rsrc;
        te->rsrc_len = te->rsrc_packed = ad.rsrc ? ad.rsrc_len : 0;
        te->rsrc_off = UINT64_MAX; // in memory, not a range of the archive
        te->rsrc_tier = PEEL_TIER_RANDOM;
    }
    // Hide the companions (and the __MACOSX tree) from the listing.
    int w = 0;
    for (int i = 0; i < a->count; i++) {
        const char *p = a->entries[i].path;
        if (strcmp(p, "__MACOSX") == 0 || strncmp(p, "__MACOSX/", 9) == 0) {
            free(z->slots[i].ad_buf);
            continue;
        }
        a->entries[w] = a->entries[i];
        z->slots[w] = z->slots[i];
        w++;
    }
    a->count = w;
    z->n_slots = w;
}

static int zip_open(peel_archive_t *a, const peel_probe_t *p, peel_err_t **err) {
    zip_priv_t *z = calloc(1, sizeof(*z));
    if (!z) {
        *err = make_err("ZIP: out of memory");
        return -1;
    }
    a->priv = z;
    uint64_t cd_off, cd_size, count;
    int64_t delta;
    if (zip_locate_cd(a, p, &cd_off, &cd_size, &count, &delta, err) != 0)
        return -1;
    if (count > cd_size / ZIP_CEN_LEN) {
        *err = make_err("ZIP: central directory too small for %llu entries", (unsigned long long)count);
        return -1;
    }
    uint8_t *cd = peel_archive_read(a, cd_off, cd_size, "ZIP central directory", err);
    if (!cd)
        return -1;
    z->members = calloc(count ? (size_t)count : 1, sizeof(zip_member_t));
    if (!z->members) {
        free(cd);
        *err = make_err("ZIP: out of memory");
        return -1;
    }
    uint64_t pos = 0;
    for (uint64_t k = 0; k < count; k++) {
        if (cd_size - pos < ZIP_CEN_LEN || le32(cd + pos) != ZIP_CEN_SIG) {
            free(cd);
            *err = make_err("ZIP: corrupt central directory record %llu", (unsigned long long)k);
            return -1;
        }
        const uint8_t *c = cd + pos;
        uint16_t nlen = le16(c + 28), xlen = le16(c + 30), clen = le16(c + 32);
        if ((uint64_t)ZIP_CEN_LEN + nlen + xlen + clen > cd_size - pos) {
            free(cd);
            *err = make_err("ZIP: central directory record %llu overruns", (unsigned long long)k);
            return -1;
        }
        zip_member_t *m = &z->members[z->n_members];
        m->flags = le16(c + 8);
        m->method = le16(c + 10);
        m->crc = le32(c + 16);
        m->csize = le32(c + 20);
        m->usize = le32(c + 24);
        uint64_t local = le32(c + 42);
        // Zip64 extra field: the 32-bit fields that saturated, in order.
        const uint8_t *x = c + ZIP_CEN_LEN + nlen, *xend = x + xlen;
        while (xend - x >= 4) {
            uint16_t id = le16(x), sz = le16(x + 2);
            const uint8_t *d = x + 4;
            if (sz > xend - d)
                break;
            if (id == 0x0001) {
                const uint8_t *q = d, *qend = d + sz;
                if (m->usize == 0xFFFFFFFFu && qend - q >= 8) {
                    m->usize = le64(q);
                    q += 8;
                }
                if (m->csize == 0xFFFFFFFFu && qend - q >= 8) {
                    m->csize = le64(q);
                    q += 8;
                }
                if (local == 0xFFFFFFFFu && qend - q >= 8)
                    local = le64(q);
            }
            x = d + sz;
        }
        m->local_off = (uint64_t)((int64_t)local + delta);

        char path[512];
        const uint8_t *name = c + ZIP_CEN_LEN;
        bool is_dir = nlen > 0 && (name[nlen - 1] == '/' || name[nlen - 1] == '\\');
        pos += (uint64_t)ZIP_CEN_LEN + nlen + xlen + clen;
        if (!zip_path(name, nlen, path, sizeof(path)))
            continue; // an empty name: nothing to list
        int member = z->n_members++;
        if (ensure_parents(a, z, path) != 0)
            goto oom;
        int existing = peel_lookup(a, path);
        if (existing >= 0 && a->entries[existing].is_dir && is_dir)
            continue; // a folder already synthesised
        peel_entry_t *e = peel_archive_add(a);
        zip_slot_t *s = e ? slot_add(z) : NULL;
        if (!s)
            goto oom;
        snprintf(e->path, sizeof(e->path), "%s", path);
        e->is_dir = is_dir;
        e->mtime = dos_to_unix(le16(c + 14), le16(c + 12));
        if (is_dir)
            continue;
        s->member = member;
        e->data_len = m->usize;
        e->data_packed = m->csize;
        e->data_method = (uint8_t)m->method;
        // The data offset needs the local header's own name and extra
        // lengths, which can differ from the central ones: it is found when
        // the member is opened, not here.
        e->data_off = UINT64_MAX;
        e->data_tier = m->method == ZIP_METHOD_STORED ? PEEL_TIER_RANDOM : PEEL_TIER_STREAM;
    }
    free(cd);
    zip_fold_appledouble(a, z);
    return 0;
oom:
    free(cd);
    *err = make_err("ZIP: out of memory");
    return -1;
}

// ============================================================================
// Members
// ============================================================================

// Where member `m`'s data starts, from its local header.  0 or -1 + *err.
static int zip_data_start(peel_archive_t *a, const zip_member_t *m, uint64_t *out, peel_err_t **err) {
    uint8_t h[ZIP_LOC_LEN];
    if (peel_source_read_exact(a->src, m->local_off, h, sizeof(h)) != 0 || le32(h) != ZIP_LOC_SIG) {
        *err = make_err("ZIP: bad local header at offset %llu", (unsigned long long)m->local_off);
        return -1;
    }
    uint64_t start = m->local_off + ZIP_LOC_LEN + le16(h + 26) + le16(h + 28);
    uint64_t size = peel_source_size(a->src);
    if (start > size || m->csize > size - start) {
        *err = make_err("ZIP: member data extends past archive end");
        return -1;
    }
    *out = start;
    return 0;
}

// The member behind entry `i`, checked openable.  NULL + *err.
static const zip_member_t *zip_member(peel_archive_t *a, int i, peel_err_t **err) {
    zip_priv_t *z = a->priv;
    int mi = z->slots[i].member;
    if (mi < 0) {
        *err = make_err("ZIP: not a file");
        return NULL;
    }
    const zip_member_t *m = &z->members[mi];
    if (m->flags & ZIP_FLAG_ENCRYPTED) {
        *err = make_err("ZIP: '%s' is encrypted (unsupported)", a->entries[i].path);
        return NULL;
    }
    if (m->method != ZIP_METHOD_STORED && m->method != ZIP_METHOD_DEFLATE) {
        *err = make_err("ZIP: '%s' uses compression method %u (unsupported)", a->entries[i].path, m->method);
        return NULL;
    }
    if (m->usize > PEEL_MAX_FORK) {
        *err = make_err("ZIP: member declares %llu bytes, over the %u MiB limit", (unsigned long long)m->usize,
                        (unsigned)(PEEL_MAX_FORK >> 20));
        return NULL;
    }
    return m;
}

// The folded-in resource fork of entry `i`, copied.
static peel_buf_t zip_rsrc(peel_archive_t *a, int i, peel_err_t **err) {
    zip_priv_t *z = a->priv;
    const peel_entry_t *e = &a->entries[i];
    peel_buf_t b = peel_buf_copy(z->slots[i].rsrc, (size_t)e->rsrc_len, err);
    return b;
}

static peel_buf_t zip_decode(peel_archive_t *a, int i, int fork, peel_err_t **err) {
    if (fork == PEEL_FORK_RSRC)
        return zip_rsrc(a, i, err);
    const zip_member_t *m = zip_member(a, i, err);
    uint64_t start;
    if (!m || zip_data_start(a, m, &start, err) != 0)
        return (peel_buf_t){0};
    uint8_t *packed = peel_archive_read(a, start, m->csize, "ZIP member", err);
    if (!packed)
        return (peel_buf_t){0};
    uint8_t *out = malloc(m->usize ? (size_t)m->usize : 1);
    if (!out) {
        free(packed);
        *err = make_err("ZIP: out of memory");
        return (peel_buf_t){0};
    }
    int64_t n;
    if (m->method == ZIP_METHOD_STORED) {
        n = m->csize >= m->usize ? (int64_t)m->usize : -1;
        if (n >= 0)
            memcpy(out, packed, (size_t)m->usize);
    } else {
        n = peel_inflate(packed, (size_t)m->csize, out, (size_t)m->usize);
    }
    free(packed);
    if (n != (int64_t)m->usize) {
        free(out);
        *err = make_err("ZIP: '%s' does not decode to its stated size", a->entries[i].path);
        return (peel_buf_t){0};
    }
    if (peel_crc32(0, out, (size_t)m->usize) != m->crc) {
        free(out);
        *err = make_err("ZIP: '%s' CRC mismatch", a->entries[i].path);
        return (peel_buf_t){0};
    }
    return (peel_buf_t){.data = out, .size = (size_t)m->usize, .owned = true};
}

// ---- Streaming deflate producer ----

typedef struct {
    peel_producer_t base;
    peel_source_t *src; // retained archive source
    uint64_t pos, end; // compressed range still to pull
    peel_inflater_t *z;
    uint32_t crc, want_crc;
} zip_producer_t;

// peel_pull_fn over the member's compressed range.
static int64_t zip_pull(void *ctx, uint8_t *buf, size_t cap) {
    zip_producer_t *zp = ctx;
    uint64_t left = zp->end - zp->pos;
    size_t n = left < cap ? (size_t)left : cap;
    if (n == 0)
        return 0;
    int rc = peel_source_read_exact(zp->src, zp->pos, buf, n);
    if (rc != 0)
        return rc;
    zp->pos += n;
    return (int64_t)n;
}

static int zip_run(peel_producer_t *p, uint8_t *out, size_t cap, size_t *n) {
    zip_producer_t *zp = (zip_producer_t *)p;
    int rc = peel_inflater_run(zp->z, out, cap, n);
    if (rc < 0) {
        snprintf(p->err, sizeof(p->err), "ZIP: corrupt deflate stream");
        return -5;
    }
    zp->crc = peel_crc32(zp->crc, out, *n);
    if (rc == 1 && zp->crc != zp->want_crc) {
        snprintf(p->err, sizeof(p->err), "ZIP: CRC mismatch");
        return -5;
    }
    return rc;
}

static void zip_producer_free(peel_producer_t *p) {
    zip_producer_t *zp = (zip_producer_t *)p;
    peel_inflater_free(zp->z);
    peel_source_release(zp->src);
    free(zp);
}

static peel_producer_t *zip_producer(peel_archive_t *a, int i, int fork, peel_err_t **err) {
    if (fork == PEEL_FORK_RSRC)
        return NULL; // folded forks are in memory: decoded whole (copied)
    const zip_member_t *m = zip_member(a, i, err);
    uint64_t start;
    if (!m || zip_data_start(a, m, &start, err) != 0)
        return NULL;
    zip_producer_t *zp = calloc(1, sizeof(*zp));
    if (!zp) {
        *err = make_err("ZIP: out of memory");
        return NULL;
    }
    zp->base.run = zip_run;
    zp->base.free = zip_producer_free;
    zp->src = peel_source_retain(a->src);
    zp->pos = start;
    zp->end = start + m->csize;
    zp->want_crc = m->crc;
    zp->z = peel_inflater_new(zip_pull, zp);
    if (!zp->z) {
        zip_producer_free(&zp->base);
        *err = make_err("ZIP: out of memory");
        return NULL;
    }
    return &zp->base;
}

// A stored member is a view -- but its data offset is only known from the
// local header, so it is filled in here, on first open, before the generic
// view path runs.
static int zip_prepare(peel_archive_t *a, int i, int fork, peel_err_t **err) {
    peel_entry_t *e = &a->entries[i];
    if (fork != PEEL_FORK_DATA)
        return 0;
    if (e->data_off != UINT64_MAX || e->data_tier != PEEL_TIER_RANDOM)
        return 0;
    const zip_member_t *m = zip_member(a, i, err);
    uint64_t start;
    if (!m || zip_data_start(a, m, &start, err) != 0)
        return -1;
    if (m->csize < m->usize) {
        *err = make_err("ZIP: stored member '%s' is shorter than its size", e->path);
        return -1;
    }
    e->data_off = start;
    return 0;
}

static void zip_close(peel_archive_t *a) {
    zip_priv_t *z = a->priv;
    if (!z)
        return;
    for (int i = 0; i < z->n_slots; i++)
        free(z->slots[i].ad_buf);
    free(z->slots);
    free(z->members);
    free(z);
}

const peel_fmt_t peel_fmt_zip = {
    .desc = {"zip", false, zip_detect_probe},
    .open = zip_open,
    .decode = zip_decode,
    .producer = zip_producer,
    .prepare = zip_prepare,
    .close = zip_close,
};
