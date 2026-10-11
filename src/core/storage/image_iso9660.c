// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_iso9660.c
// Read-only ISO 9660 over a byte source.  See image_iso9660.h.

#include "image_iso9660.h"

#include "source.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Layout (ECMA-119)
// ============================================================================

#define ISO_SECTOR      2048u
#define ISO_VD_MAX      64 // descriptors scanned before giving up on a terminator
#define ISO_VD_PRIMARY  1
#define ISO_VD_SUPPLEM  2
#define ISO_VD_END      255
#define ISO_DR_MIN      33 // a directory record without its identifier
#define ISO_FLAG_HIDDEN 0x01 // "existence": not shown to the user
#define ISO_FLAG_DIR    0x02
#define ISO_FLAG_ASSOC  0x04 // Apple: the associated file is a resource fork
#define ISO_FLAG_MULTI  0x80 // more extents follow: not supported
#define ISO_MAX_DIR     (64u * 1024u * 1024u) // the largest directory read
#define ISO_MAX_DEPTH   64

struct iso_volume {
    source_t *src; // retained
    uint64_t off, size;
    uint32_t block; // logical block size (2048 on every disc we have seen)
    uint32_t root_extent;
    uint64_t root_size;
    bool joliet; // names are UCS-2 (the supplementary descriptor's tree)
    bool rock_ridge; // names from Rock Ridge NM entries
    uint32_t susp_skip; // SUSP: bytes to skip at the start of each system use area
    char name[64];
};

struct iso_dir_iter {
    iso_volume_t *vol;
    uint8_t *data; // the whole directory
    size_t len, pos;
    const uint8_t *assoc; // the associated-file record just passed, if any
};

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

// A run of `n` UCS-2 big-endian code units as UTF-8 (Joliet names).
static void ucs2_to_utf8(const uint8_t *p, size_t n, char *out, size_t cap) {
    size_t o = 0;
    for (size_t u = 0; u < n; u++) {
        uint32_t c = (uint32_t)p[2 * u] << 8 | p[2 * u + 1];
        char tmp[3];
        size_t k;
        if (c < 0x80) {
            tmp[0] = (char)c;
            k = 1;
        } else if (c < 0x800) {
            tmp[0] = (char)(0xC0 | c >> 6);
            tmp[1] = (char)(0x80 | (c & 0x3F));
            k = 2;
        } else {
            tmp[0] = (char)(0xE0 | c >> 12);
            tmp[1] = (char)(0x80 | ((c >> 6) & 0x3F));
            tmp[2] = (char)(0x80 | (c & 0x3F));
            k = 3;
        }
        if (o + k >= cap)
            break;
        memcpy(out + o, tmp, k);
        o += k;
    }
    out[o] = '\0';
}

// Names as the VFS shows them: a '/' (legal in a Joliet or Rock Ridge name)
// becomes ':', as it does for HFS.
static void slash_to_colon(char *s) {
    for (; *s; s++)
        if (*s == '/')
            *s = ':';
}

// ============================================================================
// Open
// ============================================================================

bool iso_probe_source(struct peel_source *src, uint64_t off, uint64_t size) {
    uint8_t vd[8];
    if (!src || size < ISO9660_VD_OFF + ISO_SECTOR || source_read_exact(src, off + ISO9660_VD_OFF, vd, sizeof(vd)) != 0)
        return false;
    return vd[0] == ISO_VD_PRIMARY && memcmp(vd + 1, ISO9660_ID, 5) == 0 && vd[6] == 1;
}

// A directory record's identifier, as a name (ISO, Joliet or Rock Ridge).
// False for "." and ".." (identifiers 0 and 1).
static bool record_name(const iso_volume_t *v, const uint8_t *r, size_t rlen, char *out, size_t cap) {
    size_t idlen = r[32];
    const uint8_t *id = r + 33;
    if (idlen == 1 && (id[0] == 0 || id[0] == 1))
        return false;
    if (v->rock_ridge) {
        // System use area: after the identifier (padded to even), less the
        // SUSP skip.  An NM entry holds the POSIX name; a continued one
        // (flag bit 0) has more NM entries after it.  A name continued into
        // a CE continuation area is not followed: the ISO name stands in.
        size_t su = 33 + idlen + (idlen % 2 == 0 ? 1 : 0) + v->susp_skip;
        size_t o = 0;
        out[0] = '\0';
        while (su + 4 <= rlen) {
            const uint8_t *e = r + su;
            size_t elen = e[2];
            if (elen < 4 || su + elen > rlen)
                break;
            if (e[0] == 'N' && e[1] == 'M' && elen >= 5 && !(e[4] & 0x06)) {
                size_t k = elen - 5;
                if (o + k >= cap)
                    k = cap - 1 - o;
                memcpy(out + o, e + 5, k);
                o += k;
                out[o] = '\0';
            }
            su += elen;
        }
        if (o) {
            slash_to_colon(out);
            return true;
        }
    }
    if (v->joliet) {
        ucs2_to_utf8(id, idlen / 2, out, cap);
    } else {
        size_t k = idlen < cap - 1 ? idlen : cap - 1;
        memcpy(out, id, k);
        out[k] = '\0';
    }
    // Drop the version (";1") and a trailing '.' of a name with no extension.
    char *semi = strrchr(out, ';');
    if (semi)
        *semi = '\0';
    size_t n = strlen(out);
    if (n > 1 && out[n - 1] == '.')
        out[n - 1] = '\0';
    slash_to_colon(out);
    return true;
}

// Apple's ISO 9660 extensions: Finder info in a directory record's system
// use area, after the identifier and its pad byte (and after the 14-byte
// CD-XA record on an XA disc).  The current form (signature "AA") is a run
// of length-prefixed entries, so it can sit among SUSP / Rock Ridge ones:
//   'A' 'A' len(14) id(2 = HFS) type[4] creator[4] flags[2]
// The original form ("BA", 1988) has no length byte and stands alone:
//   'B' 'A' id(2..4) type[4] creator[4] flags[2]
// Apple's own CD-ROM driver kept only four flag bits, in the pre-System 7
// numbering (locked 0x8000, bundle 0x2000, system 0x1000, always-switch-
// launch 0x0020).  Two of those mean something else since System 7 (0x8000
// is isAlias), so only bundle and system/name-locked survive here; the
// record's existence bit becomes the invisible bit.  False when the record
// carries neither form.
static bool record_finder_info(const uint8_t *r, size_t rlen, uint32_t *type, uint32_t *creator, uint16_t *flags) {
    size_t idlen = r[32];
    size_t su = 33 + idlen + (idlen % 2 == 0 ? 1 : 0);
    if (su + 8 <= rlen && r[su + 6] == 'X' && r[su + 7] == 'A')
        su += 14; // CD-XA system use record
    const uint8_t *fi = NULL;
    if (su + 13 <= rlen && r[su] == 'B' && r[su + 1] == 'A' && r[su + 2] >= 2 && r[su + 2] <= 4) {
        fi = r + su + 3;
    } else {
        while (su + 4 <= rlen) {
            const uint8_t *e = r + su;
            size_t elen = e[2];
            if (elen < 4 || su + elen > rlen)
                break;
            if (e[0] == 'A' && e[1] == 'A' && e[3] == 2 && elen >= 14) {
                fi = e + 4;
                break;
            }
            su += elen;
        }
    }
    if (!fi)
        return false;
    *type = (uint32_t)fi[0] << 24 | (uint32_t)fi[1] << 16 | (uint32_t)fi[2] << 8 | fi[3];
    *creator = (uint32_t)fi[4] << 24 | (uint32_t)fi[5] << 16 | (uint32_t)fi[6] << 8 | fi[7];
    *flags = (uint16_t)((fi[8] << 8 | fi[9]) & (0x2000 | 0x1000));
    if (r[25] & ISO_FLAG_HIDDEN)
        *flags |= 0x4000;
    return true;
}

iso_volume_t *iso_open_source(struct peel_source *src, uint64_t off, uint64_t size) {
    if (!iso_probe_source(src, off, size))
        return NULL;
    iso_volume_t *v = calloc(1, sizeof(*v));
    if (!v)
        return NULL;
    v->src = source_retain(src);
    v->off = off;
    v->size = size;
    uint8_t vd[ISO_SECTOR];
    bool have_primary = false;
    for (int i = 0; i < ISO_VD_MAX; i++) {
        uint64_t at = off + ISO9660_VD_OFF + (uint64_t)i * ISO_SECTOR;
        if (at + ISO_SECTOR > off + size || source_read_exact(src, at, vd, ISO_SECTOR) != 0)
            break;
        if (memcmp(vd + 1, ISO9660_ID, 5) != 0 || vd[0] == ISO_VD_END)
            break;
        bool joliet = vd[0] == ISO_VD_SUPPLEM && vd[88] == '%' && vd[89] == '/' &&
                      (vd[90] == '@' || vd[90] == 'C' || vd[90] == 'E');
        if ((vd[0] == ISO_VD_PRIMARY && !have_primary) || joliet) {
            uint16_t block = le16(vd + 128);
            const uint8_t *root = vd + 156; // the root directory record
            if (block < 512 || block > ISO_SECTOR || (block & (block - 1)))
                continue;
            v->block = block;
            v->root_extent = le32(root + 2);
            v->root_size = le32(root + 10);
            if (vd[0] == ISO_VD_PRIMARY) {
                have_primary = true;
                size_t k = 32;
                memcpy(v->name, vd + 40, k);
                while (k > 0 && v->name[k - 1] == ' ')
                    k--;
                v->name[k] = '\0';
            } else {
                v->joliet = true;
                break; // Joliet is preferred: stop here
            }
        }
    }
    if (!have_primary || !v->root_size || v->root_size > ISO_MAX_DIR) {
        iso_close(v);
        return NULL;
    }
    // Rock Ridge: the root's "." record carries an "SP" entry (SUSP), and
    // then names live in NM entries.  Not with Joliet, which is preferred.
    if (!v->joliet) {
        uint8_t dot[255];
        uint64_t at = off + (uint64_t)v->root_extent * v->block;
        if (at + sizeof(dot) <= off + size && source_read_exact(src, at, dot, sizeof(dot)) == 0) {
            size_t rlen = dot[0], su = 34; // "." has a 1-byte identifier
            if (rlen >= su + 7 && dot[su] == 'S' && dot[su + 1] == 'P' && dot[su + 4] == 0xBE && dot[su + 5] == 0xEF) {
                v->rock_ridge = true;
                v->susp_skip = dot[su + 6];
            }
        }
    }
    return v;
}

void iso_close(iso_volume_t *vol) {
    if (!vol)
        return;
    source_release(vol->src);
    free(vol);
}

const char *iso_volume_name(const iso_volume_t *vol) {
    return vol->name;
}

// ============================================================================
// Directories
// ============================================================================

iso_dir_iter_t *iso_opendir(iso_volume_t *vol, uint32_t extent, uint64_t size) {
    if (size > ISO_MAX_DIR)
        return NULL;
    uint64_t at = (uint64_t)extent * vol->block;
    if (at > vol->size || size > vol->size - at)
        return NULL;
    iso_dir_iter_t *it = calloc(1, sizeof(*it));
    if (!it)
        return NULL;
    it->vol = vol;
    it->len = (size_t)size;
    it->data = malloc(it->len ? it->len : 1);
    if (!it->data || (it->len && source_read_exact(vol->src, vol->off + at, it->data, it->len) != 0)) {
        iso_closedir(it);
        return NULL;
    }
    return it;
}

void iso_closedir(iso_dir_iter_t *it) {
    if (!it)
        return;
    free(it->data);
    free(it);
}

// The next record at or after it->pos, or NULL at the end.  Records never
// cross a sector boundary: a zero length byte means "the next sector".
static const uint8_t *next_record(iso_dir_iter_t *it, size_t *rlen) {
    while (it->pos < it->len) {
        size_t len = it->data[it->pos];
        if (len == 0) {
            it->pos = (it->pos / ISO_SECTOR + 1) * ISO_SECTOR;
            continue;
        }
        if (len < ISO_DR_MIN || it->pos + len > it->len || ISO_DR_MIN + (size_t)it->data[it->pos + 32] > len)
            return NULL; // corrupt: stop
        const uint8_t *r = it->data + it->pos;
        it->pos += len;
        *rlen = len;
        return r;
    }
    return NULL;
}

int iso_readdir_next(iso_dir_iter_t *it, iso_dirent_t *out) {
    size_t rlen;
    const uint8_t *r;
    while ((r = next_record(it, &rlen)) != NULL) {
        uint8_t flags = r[25];
        if (flags & ISO_FLAG_ASSOC) {
            it->assoc = r; // recorded just before its file, under its name
            continue;
        }
        const uint8_t *assoc = it->assoc;
        it->assoc = NULL;
        memset(out, 0, sizeof(*out));
        if (!record_name(it->vol, r, rlen, out->name, sizeof(out->name)))
            continue; // "." or ".."
        if ((flags & ISO_FLAG_MULTI) || r[26] || r[27])
            return -EIO; // multi-extent or interleaved: not supported
        out->is_dir = (flags & ISO_FLAG_DIR) != 0;
        out->extent = le32(r + 2);
        out->size = le32(r + 10);
        bool paired = assoc && assoc[32] == r[32] && memcmp(assoc + 33, r + 33, r[32]) == 0;
        if (paired) {
            out->rsrc_extent = le32(assoc + 2);
            out->rsrc_size = le32(assoc + 10);
        }
        // Finder info from the file's own record, else from its associated
        // file's (some mastering tools put it only there).
        out->has_finder_info = record_finder_info(r, rlen, &out->type, &out->creator, &out->finder_flags);
        if (!out->has_finder_info && paired)
            out->has_finder_info = record_finder_info(assoc, assoc[0], &out->type, &out->creator, &out->finder_flags);
        return 1;
    }
    return it->pos >= it->len ? 0 : -EIO;
}

// Names compare case-insensitively over ASCII.
static bool iso_name_eq(const char *a, const char *b) {
    for (;; a++, b++) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'a' && ca <= 'z')
            ca -= 32;
        if (cb >= 'a' && cb <= 'z')
            cb -= 32;
        if (ca != cb)
            return false;
        if (!ca)
            return true;
    }
}

int iso_lookup(iso_volume_t *vol, const char *const *comp, size_t nc, iso_dirent_t *out) {
    memset(out, 0, sizeof(*out));
    out->is_dir = true;
    out->extent = vol->root_extent;
    out->size = vol->root_size;
    if (nc > ISO_MAX_DEPTH)
        return -ENAMETOOLONG;
    for (size_t i = 0; i < nc; i++) {
        if (!out->is_dir)
            return -ENOTDIR;
        iso_dir_iter_t *it = iso_opendir(vol, out->extent, out->size);
        if (!it)
            return -EIO;
        iso_dirent_t e;
        int rc;
        bool found = false;
        while ((rc = iso_readdir_next(it, &e)) > 0) {
            if (iso_name_eq(e.name, comp[i])) {
                found = true;
                break;
            }
        }
        iso_closedir(it);
        if (rc < 0)
            return rc;
        if (!found)
            return -ENOENT;
        *out = e;
    }
    return 0;
}

// ============================================================================
// Files
// ============================================================================

int iso_read(iso_volume_t *vol, uint32_t extent, uint64_t size, uint64_t off, void *buf, size_t n, size_t *nread) {
    *nread = 0;
    if (off >= size || n == 0)
        return 0;
    if (n > size - off)
        n = (size_t)(size - off);
    uint64_t at = (uint64_t)extent * vol->block + off;
    if (at > vol->size || n > vol->size - at)
        return -EIO; // an extent past the volume's end
    int rc = source_read_exact(vol->src, vol->off + at, buf, n);
    if (rc != 0)
        return rc;
    *nread = n;
    return 0;
}
