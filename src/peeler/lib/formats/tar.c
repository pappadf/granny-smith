// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// tar.c — tar archives (POSIX.1 ustar, GNU and pax extensions, and the old
// V7 layout).
//
// A tar is a run of 512-byte headers, each followed by its member's bytes
// padded to 512; two zero blocks end it.  Every member is stored, so every
// fork is a view of the archive: opening one decodes nothing.  Opening the
// archive walks the headers: a run of small members shares the read-ahead
// window, and the header after a large member is read alone (512 bytes),
// so a member's bytes are never read for the sake of the next header.
//
// Extensions honoured: a GNU long name ('L') and a pax extended header ('x':
// "path" and "size") name and size the member after them; a base-256 size
// (GNU, for members over 8 GiB); the ustar prefix field.  A hard link ('1')
// is a second name for an earlier member's bytes.  Symbolic links, devices
// and FIFOs are skipped: nothing a Mac file can be.
//
// macOS's tar keeps a file's resource fork and Finder info in a "._<name>"
// AppleDouble member beside it.  Those are folded back into the file they
// describe -- its resource fork a view of the companion's bytes -- and not
// listed on their own.

#include "appledouble.h"
#include "internal.h"

// ============================================================================
// Constants
// ============================================================================

#define TAR_BLOCK           512u
#define TAR_MAX_ENTRIES     1000000 // far above any real archive; bounds a crafted one
#define TAR_MAX_LONGNAME    4096 // a 'L' or pax path longer than this is refused
#define TAR_MAX_PAX         (64u * 1024u) // the largest pax header read
#define TAR_MAX_APPLEDOUBLE (16u * 1024u * 1024u)
// After a member this large, the next header is read alone, not with the
// read-ahead window: the window would be mostly the member's own bytes.
#define TAR_BIG_MEMBER (64u * 1024u)

// ============================================================================
// Header fields
// ============================================================================

// An octal field (spaces and NULs around it ignored), or a base-256 one
// (high bit of the first byte set).  False when it is neither.
static bool tar_number(const uint8_t *f, size_t n, uint64_t *out) {
    if (f[0] & 0x80) {
        uint64_t v = f[0] & 0x7F;
        for (size_t i = 1; i < n; i++) {
            if (v >> 56)
                return false; // over 64 bits
            v = v << 8 | f[i];
        }
        *out = v;
        return true;
    }
    size_t i = 0;
    while (i < n && f[i] == ' ')
        i++;
    uint64_t v = 0;
    for (; i < n && f[i] >= '0' && f[i] <= '7'; i++) {
        if (v >> 61)
            return false;
        v = v << 3 | (uint64_t)(f[i] - '0');
    }
    for (; i < n; i++)
        if (f[i] != ' ' && f[i] != 0)
            return false;
    *out = v; // an empty field is 0
    return true;
}

// Does `h` carry a correct header checksum?  The sum of its bytes with the
// checksum field taken as spaces, unsigned (POSIX) or signed (old tars).
static bool tar_checksum_ok(const uint8_t *h) {
    uint64_t stored;
    if (!tar_number(h + 148, 8, &stored))
        return false;
    uint32_t u = 0;
    int32_t s = 0;
    for (size_t i = 0; i < TAR_BLOCK; i++) {
        uint8_t b = (i >= 148 && i < 156) ? ' ' : h[i];
        u += b;
        s += (int8_t)b;
    }
    return stored == u || (int64_t)stored == s;
}

static bool tar_zero_block(const uint8_t *h) {
    for (size_t i = 0; i < TAR_BLOCK; i++)
        if (h[i])
            return false;
    return true;
}

// A header worth trusting: its checksum holds, and it is ustar (POSIX or
// GNU) or a plausible V7 header (a name, octal size digits).
static bool tar_header_ok(const uint8_t *h) {
    if (!tar_checksum_ok(h))
        return false;
    if (memcmp(h + 257, "ustar", 5) == 0)
        return true;
    uint64_t size;
    return h[0] != 0 && tar_number(h + 124, 12, &size);
}

static bool tar_detect_probe(const peel_probe_t *p) {
    return p->head_len >= TAR_BLOCK && !tar_zero_block(p->head) && tar_header_ok(p->head);
}

bool tar_detect(const uint8_t *src, size_t len) {
    return len >= TAR_BLOCK && !tar_zero_block(src) && tar_header_ok(src);
}

// ============================================================================
// Open
// ============================================================================

// Copy a NUL-or-length-terminated field.
static size_t tar_field(const uint8_t *f, size_t n, char *out, size_t cap) {
    size_t k = 0;
    while (k < n && f[k])
        k++;
    if (k >= cap)
        k = cap - 1;
    memcpy(out, f, k);
    out[k] = '\0';
    return k;
}

// A member's path, sanitised one component at a time (peel_append_segment);
// "." components are dropped.  False for a path with no components.
static bool tar_path(const char *raw, char *out, size_t cap) {
    size_t pos = 0, start = 0, n = strlen(raw);
    out[0] = '\0';
    bool any = false;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && raw[i] != '/')
            continue;
        size_t len = i - start;
        if (len && !(len == 1 && raw[start] == '.')) {
            peel_append_segment(out, cap, &pos, (const uint8_t *)raw + start, len);
            any = true;
        }
        start = i + 1;
    }
    return any;
}

// Read `len` bytes at `off` into a fresh NUL-terminated buffer.
static char *tar_read_text(peel_archive_t *a, uint64_t off, uint64_t len) {
    char *s = malloc((size_t)len + 1);
    if (!s)
        return NULL;
    if (len && peel_source_read_exact(a->src, off, s, (size_t)len) != 0) {
        free(s);
        return NULL;
    }
    s[len] = '\0';
    return s;
}

// Apply a pax extended header's records ("<len> <key>=<value>\n") that we
// honour: path and size.
static void tar_pax(const char *rec, size_t len, char *path, size_t path_cap, uint64_t *size, bool *have_size) {
    size_t p = 0;
    while (p < len) {
        char *end = NULL;
        unsigned long rl = strtoul(rec + p, &end, 10);
        if (!end || *end != ' ' || rl == 0 || rl > len - p)
            return;
        const char *kv = end + 1;
        const char *stop = rec + p + rl - 1; // the record's '\n'
        const char *eq = memchr(kv, '=', (size_t)(stop - kv));
        if (eq) {
            size_t klen = (size_t)(eq - kv), vlen = (size_t)(stop - eq - 1);
            if (klen == 4 && memcmp(kv, "path", 4) == 0 && vlen < path_cap) {
                memcpy(path, eq + 1, vlen);
                path[vlen] = '\0';
            } else if (klen == 4 && memcmp(kv, "size", 4) == 0) {
                char num[32];
                if (vlen < sizeof(num)) {
                    memcpy(num, eq + 1, vlen);
                    num[vlen] = '\0';
                    *size = strtoull(num, NULL, 10);
                    *have_size = true;
                }
            }
        }
        p += rl;
    }
}

// Fold "._<name>" AppleDouble companions into their files: the resource
// fork as a view of the companion's bytes, the Finder info into the entry.
static void tar_fold_appledouble(peel_archive_t *a) {
    for (int i = 0; i < a->count; i++) {
        peel_entry_t *e = &a->entries[i];
        const char *base = strrchr(e->path, '/');
        base = base ? base + 1 : e->path;
        if (e->is_dir || strncmp(base, "._", 2) != 0 || !base[2] || e->data_len > TAR_MAX_APPLEDOUBLE)
            continue;
        char target[512];
        snprintf(target, sizeof(target), "%.*s%s", (int)(base - e->path), e->path, base + 2);
        int t = peel_lookup(a, target);
        if (t < 0 || a->entries[t].is_dir)
            continue;
        peel_err_t *err = NULL;
        uint8_t *buf = peel_archive_read(a, e->data_off, e->data_len, "tar AppleDouble member", &err);
        if (!buf) {
            peel_err_free(err);
            continue;
        }
        ad_file_t ad;
        if (ad_detect(buf, (size_t)e->data_len) && ad_parse(buf, (size_t)e->data_len, &ad) == 0) {
            peel_entry_t *te = &a->entries[t];
            if (ad.finder && ad.finder_len >= 10) {
                te->mac_type = rd32be(ad.finder);
                te->mac_creator = rd32be(ad.finder + 4);
                te->finder_flags = rd16be(ad.finder + 8);
            }
            if (ad.rsrc && ad.rsrc_len) {
                te->rsrc_off = e->data_off + (uint64_t)(ad.rsrc - buf);
                te->rsrc_len = te->rsrc_packed = ad.rsrc_len;
                te->rsrc_tier = PEEL_TIER_RANDOM;
            }
            e->is_dir = true; // marked for removal below
            e->path[0] = '\0';
        }
        free(buf);
    }
    // Drop the folded companions.
    int w = 0;
    for (int i = 0; i < a->count; i++)
        if (a->entries[i].path[0])
            a->entries[w++] = a->entries[i];
    a->count = w;
}

// Add any folder above `path` the archive did not list.  0 or -1.
static int tar_parents(peel_archive_t *a, const char *path) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *slash = strchr(buf, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        if (peel_lookup(a, buf) < 0) {
            peel_entry_t *d = peel_archive_add(a);
            if (!d)
                return -1;
            snprintf(d->path, sizeof(d->path), "%s", buf);
            d->is_dir = true;
        }
        *slash = '/';
    }
    return 0;
}

static int tar_open(peel_archive_t *a, const peel_probe_t *p, peel_err_t **err) {
    (void)p;
    peel_reader_t rd;
    if (peel_reader_init(&rd, a->src) != 0) {
        *err = make_err("TAR: out of memory");
        return -1;
    }
    uint64_t size = peel_source_size(a->src), off = 0;
    char longname[TAR_MAX_LONGNAME + 1] = {0};
    uint64_t pax_size = 0;
    bool have_pax_size = false;
    int members = 0;
    int rc = 0;
    uint64_t prev_len = TAR_BIG_MEMBER; // the first header is read alone too
    uint8_t alone[TAR_BLOCK];
    while (off + TAR_BLOCK <= size) {
        const uint8_t *h;
        if (prev_len >= TAR_BIG_MEMBER)
            h = peel_source_read_exact(a->src, off, alone, TAR_BLOCK) == 0 ? alone : NULL;
        else
            h = peel_reader_at(&rd, off, TAR_BLOCK);
        if (!h) {
            *err = make_err("TAR: cannot read the header at %llu", (unsigned long long)off);
            rc = -1;
            break;
        }
        if (tar_zero_block(h))
            break; // the end of the archive
        if (!tar_header_ok(h)) {
            if (members == 0) {
                *err = make_err("TAR: bad header checksum");
                rc = -1;
            }
            break; // trailing garbage after members is ignored, as tar does
        }
        uint64_t len;
        if (!tar_number(h + 124, 12, &len)) {
            *err = make_err("TAR: bad size field at %llu", (unsigned long long)off);
            rc = -1;
            break;
        }
        uint8_t type = h[156];
        if (have_pax_size && type != 'x' && type != 'g' && type != 'L' && type != 'K') {
            len = pax_size;
            have_pax_size = false;
        }
        uint64_t data = off + TAR_BLOCK;
        if (len > size - data && type != '1' && type != '2' && type != '5') {
            *err = make_err("TAR: member at %llu extends past the archive", (unsigned long long)off);
            rc = -1;
            break;
        }
        uint64_t next =
            data + ((type == '1' || type == '2' || type == '5') ? 0 : (len + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK);
        prev_len = next - data;
        members++;
        if (type == 'L' || type == 'x') {
            // A long name or pax header for the member that follows.
            if (len > (type == 'L' ? TAR_MAX_LONGNAME : TAR_MAX_PAX)) {
                *err = make_err("TAR: extended header too long");
                rc = -1;
                break;
            }
            char *text = tar_read_text(a, data, len);
            if (!text) {
                *err = make_err("TAR: cannot read an extended header");
                rc = -1;
                break;
            }
            if (type == 'L')
                snprintf(longname, sizeof(longname), "%s", text);
            else
                tar_pax(text, (size_t)len, longname, sizeof(longname), &pax_size, &have_pax_size);
            free(text);
            off = next;
            continue;
        }
        if (type == 'g' || type == 'K') {
            off = next; // global pax header, long link name: not needed
            continue;
        }
        char raw[TAR_MAX_LONGNAME + 1];
        if (longname[0]) {
            snprintf(raw, sizeof(raw), "%s", longname);
            longname[0] = '\0';
        } else {
            char name[101], prefix[156];
            tar_field(h, 100, name, sizeof(name));
            size_t pl = memcmp(h + 257, "ustar\0", 6) == 0 ? tar_field(h + 345, 155, prefix, sizeof(prefix)) : 0;
            if (pl)
                snprintf(raw, sizeof(raw), "%s/%s", prefix, name);
            else
                snprintf(raw, sizeof(raw), "%s", name);
        }
        bool is_dir = type == '5' || (raw[0] && raw[strlen(raw) - 1] == '/');
        bool regular = type == '0' || type == 0 || type == '7';
        char path[512];
        if ((!regular && !is_dir && type != '1') || !tar_path(raw, path, sizeof(path))) {
            off = next; // a symlink, a device, a FIFO, or a name with nothing in it
            continue;
        }
        if (a->count >= TAR_MAX_ENTRIES) {
            *err = make_err("TAR: too many members");
            rc = -1;
            break;
        }
        if (tar_parents(a, path) != 0) {
            *err = make_err("TAR: out of memory");
            rc = -1;
            break;
        }
        // A hard link is a second name for an earlier member's bytes; one
        // whose target is not here is dropped.
        int link_to = -1;
        if (!is_dir && type == '1') {
            char link[101], lpath[512];
            tar_field(h + 157, 100, link, sizeof(link));
            link_to = tar_path(link, lpath, sizeof(lpath)) ? peel_lookup(a, lpath) : -1;
            if (link_to < 0 || a->entries[link_to].is_dir) {
                off = next;
                continue;
            }
        }
        int existing = peel_lookup(a, path);
        if (existing >= 0 && a->entries[existing].is_dir && is_dir) {
            off = next; // a folder already listed (or synthesised)
            continue;
        }
        if (existing < 0) {
            if (!peel_archive_add(a)) {
                *err = make_err("TAR: out of memory");
                rc = -1;
                break;
            }
            existing = a->count - 1;
        }
        peel_entry_t *e = &a->entries[existing]; // a later copy of a name wins
        memset(e, 0, sizeof(*e));
        e->data_off = e->rsrc_off = UINT64_MAX;
        snprintf(e->path, sizeof(e->path), "%s", path);
        e->is_dir = is_dir;
        uint64_t mtime = 0;
        if (tar_number(h + 136, 12, &mtime) && mtime <= UINT32_MAX)
            e->mtime = (uint32_t)mtime;
        if (link_to >= 0) {
            e->data_off = a->entries[link_to].data_off;
            e->data_len = e->data_packed = a->entries[link_to].data_len;
            e->data_tier = PEEL_TIER_RANDOM;
        } else if (!is_dir) {
            e->data_off = data;
            e->data_len = e->data_packed = len;
            e->data_tier = PEEL_TIER_RANDOM; // stored: a view
        }
        off = next;
    }
    peel_reader_free(&rd);
    if (rc == 0)
        tar_fold_appledouble(a);
    return rc;
}

static peel_buf_t tar_decode(peel_archive_t *a, int i, int fork, peel_err_t **err) {
    const peel_entry_t *e = &a->entries[i];
    uint64_t off = fork == PEEL_FORK_RSRC ? e->rsrc_off : e->data_off;
    uint64_t len = fork == PEEL_FORK_RSRC ? e->rsrc_len : e->data_len;
    if (len == 0 || off == UINT64_MAX)
        return (peel_buf_t){0};
    uint8_t *buf = peel_archive_read(a, off, len, "TAR member", err);
    return buf ? (peel_buf_t){.data = buf, .size = (size_t)len, .owned = true} : (peel_buf_t){0};
}

const peel_fmt_t peel_fmt_tar = {
    .desc = {"tar", false, tar_detect_probe},
    .open = tar_open,
    .decode = tar_decode,
};
