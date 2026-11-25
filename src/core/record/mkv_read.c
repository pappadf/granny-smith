// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mkv_read.c
// Reader for the Matroska (RFC 9559) subset the recorder writes.  See
// mkv_read.h.
//
// The file is walked as EBML (RFC 8794): an element is an ID (1 to 4
// bytes, its length in the leading zeros of the first), a size (1 to 8
// bytes the same way; all value bits set means "unknown") and the body.
// The EBML header must name the matroska (or webm) DocType.  Inside the
// Segment, Info and Tracks are read before the first Cluster; each Cluster
// is then read whole into memory and its Timestamp and SimpleBlocks (or
// BlockGroups' Blocks) handed out one by one; Cues are read where they
// are met; everything else is skipped.  A Segment of unknown size runs to
// the end of the file.  An element that runs past the end of the file
// ends the walk, and the file counts as cut.

#include "mkv_read.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

// ============================================================================
// Constants and macros
// ============================================================================

#define ID_EBML              0x1A45DFA3u
#define ID_DOCTYPE           0x4282u
#define ID_DOCTYPE_READ_VER  0x4285u
#define ID_SEGMENT           0x18538067u
#define ID_INFO              0x1549A966u
#define ID_TIMESTAMP_SCALE   0x2AD7B1u
#define ID_DURATION          0x4489u
#define ID_TRACKS            0x1654AE6Bu
#define ID_TRACK_ENTRY       0xAEu
#define ID_TRACK_NUMBER      0xD7u
#define ID_TRACK_TYPE        0x83u
#define ID_CODEC_ID          0x86u
#define ID_CODEC_PRIVATE     0x63A2u
#define ID_DEFAULT_DURATION  0x23E383u
#define ID_VIDEO             0xE0u
#define ID_PIXEL_WIDTH       0xB0u
#define ID_PIXEL_HEIGHT      0xBAu
#define ID_DISPLAY_WIDTH     0x54B0u
#define ID_DISPLAY_HEIGHT    0x54BAu
#define ID_AUDIO             0xE1u
#define ID_SAMPLING_FREQ     0xB5u
#define ID_CHANNELS          0x9Fu
#define ID_BIT_DEPTH         0x6264u
#define ID_CLUSTER           0x1F43B675u
#define ID_CLUSTER_TIMESTAMP 0xE7u
#define ID_SIMPLE_BLOCK      0xA3u
#define ID_BLOCK_GROUP       0xA0u
#define ID_BLOCK             0xA1u
#define ID_REFERENCE_BLOCK   0xFBu
#define ID_CUES              0x1C53BB6Bu
#define ID_CUE_POINT         0xBBu
#define ID_CUE_TIME          0xB3u
#define ID_CUE_TRACK_POS     0xB7u
#define ID_CUE_TRACK         0xF7u
#define ID_CUE_CLUSTER_POS   0xF1u

#define MKV_DEFAULT_SCALE 1000000u // TimestampScale when Info does not say
#define MKV_HEADER_MAX    (1u << 20) // the most an Info, Tracks or EBML header may hold
#define MKV_CLUSTER_MAX   (256u << 20) // the largest cluster read
#define LACING_MASK       0x06 // SimpleBlock flags: a laced block
#define SIMPLE_KEYFRAME   0x80

// ============================================================================
// Type definitions (private)
// ============================================================================

// The reader: the file, where the segment lies, the header's facts, the
// cluster in hand, and what was met along the way.
struct mkv_reader {
    FILE *f;
    uint64_t file_size, pos; // pos: the next top-level element
    uint64_t seg_data, seg_end;
    bool sized, truncated, done;
    uint64_t scale;
    double duration;
    mkv_track_t tracks[MKV_READ_MAX_TRACKS];
    int ntracks;
    uint8_t *cl; // the cluster's body
    size_t cl_len, cl_at;
    int64_t cl_ts;
    bool in_cluster;
    mkv_read_cue_t *cues;
    size_t ncues, cues_cap;
    uint64_t *clusters;
    size_t nclusters, clusters_cap;
};

// ============================================================================
// Static helpers: EBML in memory
// ============================================================================

// A variable-length integer at p[*at] (before `end`): its length from the
// first byte's leading zeros.  `keep_marker` returns it with the marker
// bit (an ID).  *unknown is set for a value with every bit one.  0, or
// -EINVAL when it is malformed or runs past `end`.
static int vint(const uint8_t *p, size_t end, size_t *at, bool keep_marker, int max_len, uint64_t *v, bool *unknown) {
    if (*at >= end || p[*at] == 0)
        return -EINVAL;
    uint8_t first = p[*at];
    int len = 1;
    while (!(first & (0x80 >> (len - 1))))
        len++;
    if (len > max_len || end - *at < (size_t)len)
        return -EINVAL;
    uint64_t x = keep_marker ? first : first & (0xFFu >> len);
    for (int i = 1; i < len; i++)
        x = x << 8 | p[*at + (size_t)i];
    if (unknown)
        *unknown = !keep_marker && x == (1ull << (7 * len)) - 1;
    *v = x;
    *at += (size_t)len;
    return 0;
}

// The next element header in p[*at..end): its ID and body size, *at moved
// to the body.  0 or -EINVAL (malformed, unknown size, or a body past end).
static int child(const uint8_t *p, size_t end, size_t *at, uint32_t *id, uint64_t *size) {
    uint64_t i;
    bool unknown;
    if (vint(p, end, at, true, 4, &i, NULL) || vint(p, end, at, false, 8, size, &unknown) || unknown ||
        *size > end - *at)
        return -EINVAL;
    *id = (uint32_t)i;
    return 0;
}

// An unsigned integer body of `n` (<= 8) bytes.
static uint64_t be_uint(const uint8_t *p, uint64_t n) {
    uint64_t v = 0;
    for (uint64_t i = 0; i < n && i < 8; i++)
        v = v << 8 | p[i];
    return v;
}

// A float body: 4 or 8 bytes (0 for any other length).
static double be_float(const uint8_t *p, uint64_t n) {
    if (n == 4) {
        uint32_t b = (uint32_t)be_uint(p, 4);
        float f;
        memcpy(&f, &b, sizeof(f));
        return f;
    }
    if (n == 8) {
        uint64_t b = be_uint(p, 8);
        double d;
        memcpy(&d, &b, sizeof(d));
        return d;
    }
    return 0;
}

// ============================================================================
// Static helpers: the file
// ============================================================================

// Read `n` bytes at file offset `at` into `dst`.  0, or -EIO.
static int read_at(mkv_reader_t *r, uint64_t at, uint8_t *dst, size_t n) {
    if (fseeko(r->f, (off_t)at, SEEK_SET) != 0 || fread(dst, 1, n, r->f) != n)
        return -EIO;
    return 0;
}

// The element header at r->pos: ID, size (or unknown), and where the body
// starts.  1, 0 at the end of the segment, -ENODATA when the header itself
// is cut off, or -EINVAL.
static int top_header(mkv_reader_t *r, uint32_t *id, uint64_t *size, bool *unknown, uint64_t *body) {
    if (r->pos >= r->seg_end)
        return 0;
    uint8_t h[12];
    size_t n = r->seg_end - r->pos < sizeof(h) ? (size_t)(r->seg_end - r->pos) : sizeof(h);
    if (read_at(r, r->pos, h, n))
        return -EIO;
    size_t at = 0;
    uint64_t i;
    if (vint(h, n, &at, true, 4, &i, NULL) || vint(h, n, &at, false, 8, size, unknown))
        return n < sizeof(h) ? -ENODATA : -EINVAL;
    *id = (uint32_t)i;
    *body = r->pos + at;
    return 1;
}

// Load an element body of `size` bytes at `body` into a fresh buffer.
static uint8_t *load(mkv_reader_t *r, uint64_t body, uint64_t size, int *err) {
    uint8_t *b = malloc(size ? (size_t)size : 1);
    if (!b) {
        *err = -ENOMEM;
        return NULL;
    }
    if (read_at(r, body, b, (size_t)size)) {
        free(b);
        *err = -EIO;
        return NULL;
    }
    return b;
}

// Append to a growing array of `elem`-byte entries.  0 or -ENOMEM.
static int push(void **arr, size_t *n, size_t *cap, size_t elem, const void *v) {
    if (*n == *cap) {
        size_t c = *cap ? *cap * 2 : 64;
        void *a = realloc(*arr, c * elem);
        if (!a)
            return -ENOMEM;
        *arr = a;
        *cap = c;
    }
    memcpy((uint8_t *)*arr + *n * elem, v, elem);
    (*n)++;
    return 0;
}

// ============================================================================
// Static helpers: the header elements
// ============================================================================

// The EBML header's body: the DocType must be one this reads.
static int parse_ebml(const uint8_t *p, size_t n) {
    bool matroska = false;
    for (size_t at = 0; at < n;) {
        uint32_t id;
        uint64_t sz;
        if (child(p, n, &at, &id, &sz))
            return -EINVAL;
        if (id == ID_DOCTYPE)
            matroska = (sz == 8 && !memcmp(p + at, "matroska", 8)) || (sz == 4 && !memcmp(p + at, "webm", 4));
        else if (id == ID_DOCTYPE_READ_VER && be_uint(p + at, sz) > 4)
            return -ENOTSUP;
        at += sz;
    }
    return matroska ? 0 : -EINVAL;
}

// Info: the timestamp scale and the duration.
static int parse_info(mkv_reader_t *r, const uint8_t *p, size_t n) {
    for (size_t at = 0; at < n;) {
        uint32_t id;
        uint64_t sz;
        if (child(p, n, &at, &id, &sz))
            return -EINVAL;
        if (id == ID_TIMESTAMP_SCALE)
            r->scale = be_uint(p + at, sz);
        else if (id == ID_DURATION)
            r->duration = be_float(p + at, sz);
        at += sz;
    }
    return r->scale ? 0 : -EINVAL;
}

// A Video or Audio element's settings into t.
static int parse_track_settings(mkv_track_t *t, const uint8_t *p, size_t n) {
    for (size_t at = 0; at < n;) {
        uint32_t id;
        uint64_t sz;
        if (child(p, n, &at, &id, &sz))
            return -EINVAL;
        const uint8_t *b = p + at;
        switch (id) {
        case ID_PIXEL_WIDTH:
            t->pixel_width = be_uint(b, sz);
            break;
        case ID_PIXEL_HEIGHT:
            t->pixel_height = be_uint(b, sz);
            break;
        case ID_DISPLAY_WIDTH:
            t->display_width = be_uint(b, sz);
            break;
        case ID_DISPLAY_HEIGHT:
            t->display_height = be_uint(b, sz);
            break;
        case ID_SAMPLING_FREQ:
            t->sampling_frequency = be_float(b, sz);
            break;
        case ID_CHANNELS:
            t->channels = be_uint(b, sz);
            break;
        case ID_BIT_DEPTH:
            t->bit_depth = be_uint(b, sz);
            break;
        }
        at += sz;
    }
    return 0;
}

// Tracks: each TrackEntry.
static int parse_tracks(mkv_reader_t *r, const uint8_t *p, size_t n) {
    for (size_t at = 0; at < n;) {
        uint32_t id;
        uint64_t sz;
        if (child(p, n, &at, &id, &sz))
            return -EINVAL;
        if (id != ID_TRACK_ENTRY) {
            at += sz;
            continue;
        }
        if (r->ntracks == MKV_READ_MAX_TRACKS)
            return -ENOTSUP;
        mkv_track_t *t = &r->tracks[r->ntracks++];
        size_t end = at + sz;
        while (at < end) {
            uint32_t cid;
            uint64_t csz;
            if (child(p, end, &at, &cid, &csz))
                return -EINVAL;
            const uint8_t *b = p + at;
            switch (cid) {
            case ID_TRACK_NUMBER:
                t->number = be_uint(b, csz);
                break;
            case ID_TRACK_TYPE:
                t->type = (int)be_uint(b, csz);
                break;
            case ID_CODEC_ID: {
                size_t k = csz < sizeof(t->codec_id) - 1 ? (size_t)csz : sizeof(t->codec_id) - 1;
                memcpy(t->codec_id, b, k);
                t->codec_id[k] = '\0';
                break;
            }
            case ID_CODEC_PRIVATE:
                free(t->codec_private);
                t->codec_private = malloc(csz ? (size_t)csz : 1);
                if (!t->codec_private)
                    return -ENOMEM;
                memcpy(t->codec_private, b, (size_t)csz);
                t->codec_private_len = (size_t)csz;
                break;
            case ID_DEFAULT_DURATION:
                t->default_duration_ns = be_uint(b, csz);
                break;
            case ID_VIDEO:
            case ID_AUDIO:
                if (parse_track_settings(t, b, (size_t)csz))
                    return -EINVAL;
                break;
            }
            at += csz;
        }
    }
    return 0;
}

// Cues: every CuePoint's time and first track position.
static int parse_cues(mkv_reader_t *r, const uint8_t *p, size_t n) {
    for (size_t at = 0; at < n;) {
        uint32_t id;
        uint64_t sz;
        if (child(p, n, &at, &id, &sz))
            return -EINVAL;
        if (id == ID_CUE_POINT) {
            mkv_read_cue_t c = {0};
            for (size_t a = at, end = at + sz; a < end;) {
                uint32_t cid;
                uint64_t csz;
                if (child(p, end, &a, &cid, &csz))
                    return -EINVAL;
                if (cid == ID_CUE_TIME)
                    c.time = be_uint(p + a, csz);
                else if (cid == ID_CUE_TRACK_POS) {
                    for (size_t b = a, bend = a + csz; b < bend;) {
                        uint32_t tid;
                        uint64_t tsz;
                        if (child(p, bend, &b, &tid, &tsz))
                            return -EINVAL;
                        if (tid == ID_CUE_TRACK)
                            c.track = be_uint(p + b, tsz);
                        else if (tid == ID_CUE_CLUSTER_POS)
                            c.cluster_pos = be_uint(p + b, tsz);
                        b += tsz;
                    }
                }
                a += csz;
            }
            if (push((void **)&r->cues, &r->ncues, &r->cues_cap, sizeof(c), &c))
                return -ENOMEM;
        }
        at += sz;
    }
    return 0;
}

// A Block or SimpleBlock body into *p (timestamp relative to the cluster).
static int parse_block(mkv_reader_t *r, const uint8_t *b, size_t n, bool simple, mkv_packet_t *p) {
    size_t at = 0;
    uint64_t track;
    if (vint(b, n, &at, false, 8, &track, NULL) || n - at < 3)
        return -EINVAL;
    int16_t rel = (int16_t)(b[at] << 8 | b[at + 1]);
    uint8_t flags = b[at + 2];
    at += 3;
    if (flags & LACING_MASK)
        return -ENOTSUP;
    p->track = track;
    // In unsigned arithmetic: a corrupt cluster timestamp near the top wraps, defined
    p->timestamp = (int64_t)((uint64_t)r->cl_ts + (uint64_t)(int64_t)rel);
    p->keyframe = simple && (flags & SIMPLE_KEYFRAME);
    p->data = b + at;
    p->len = n - at;
    return 0;
}

// ============================================================================
// Lifecycle
// ============================================================================

mkv_reader_t *mkv_read_open(const char *path, int *err) {
    int e0 = 0;
    if (!err)
        err = &e0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        *err = errno ? -errno : -EIO;
        return NULL;
    }
    return mkv_read_open_file(f, err);
}

mkv_reader_t *mkv_read_open_file(FILE *f, int *err) {
    int e0 = 0;
    if (!err)
        err = &e0;
    mkv_reader_t *r = calloc(1, sizeof(*r));
    if (!r) {
        fclose(f);
        *err = -ENOMEM;
        return NULL;
    }
    r->scale = MKV_DEFAULT_SCALE;
    r->f = f;
    int rc = -EINVAL;
    if (fseeko(r->f, 0, SEEK_END) == 0)
        r->file_size = (uint64_t)ftello(r->f);
    // The EBML header, then the Segment.
    r->seg_end = r->file_size;
    uint32_t id;
    uint64_t size, body;
    bool unknown;
    if (top_header(r, &id, &size, &unknown, &body) != 1 || id != ID_EBML || unknown || size > MKV_HEADER_MAX ||
        body + size > r->file_size)
        goto fail;
    uint8_t *b = load(r, body, size, &rc);
    if (!b)
        goto fail;
    rc = parse_ebml(b, (size_t)size);
    free(b);
    if (rc)
        goto fail;
    rc = -EINVAL;
    r->pos = body + size;
    if (top_header(r, &id, &size, &unknown, &body) != 1 || id != ID_SEGMENT)
        goto fail;
    r->seg_data = body;
    r->sized = !unknown;
    r->seg_end = unknown || size > r->file_size - body ? r->file_size : body + size;
    r->truncated = !unknown && size > r->file_size - body;
    r->pos = body;
    // Everything before the first cluster.
    for (;;) {
        int h = top_header(r, &id, &size, &unknown, &body);
        if (h == 0 || h == -ENODATA) {
            r->truncated |= h == -ENODATA;
            break;
        }
        if (h < 0) {
            rc = h;
            goto fail;
        }
        if (id == ID_CLUSTER)
            break;
        if (unknown) {
            rc = -ENOTSUP;
            goto fail;
        }
        if (size > r->seg_end - body) {
            r->truncated = true;
            r->pos = r->seg_end;
            break;
        }
        if (id == ID_INFO || id == ID_TRACKS) {
            if (size > MKV_HEADER_MAX)
                goto fail;
            b = load(r, body, size, &rc);
            if (!b)
                goto fail;
            rc = id == ID_INFO ? parse_info(r, b, (size_t)size) : parse_tracks(r, b, (size_t)size);
            free(b);
            if (rc)
                goto fail;
        }
        r->pos = body + size;
    }
    if (r->ntracks == 0) {
        rc = -EINVAL;
        goto fail;
    }
    *err = 0;
    return r;
fail:
    *err = rc ? rc : -EINVAL;
    mkv_read_close(r);
    return NULL;
}

void mkv_read_close(mkv_reader_t *r) {
    if (!r)
        return;
    for (int i = 0; i < r->ntracks; i++)
        free(r->tracks[i].codec_private);
    if (r->f)
        fclose(r->f);
    free(r->cl);
    free(r->cues);
    free(r->clusters);
    free(r);
}

// ============================================================================
// Operations
// ============================================================================

int mkv_read_track_count(const mkv_reader_t *r) {
    return r ? r->ntracks : 0;
}

const mkv_track_t *mkv_read_track(const mkv_reader_t *r, int i) {
    return r && i >= 0 && i < r->ntracks ? &r->tracks[i] : NULL;
}

uint64_t mkv_read_timestamp_scale(const mkv_reader_t *r) {
    return r ? r->scale : 0;
}

double mkv_read_duration(const mkv_reader_t *r) {
    return r ? r->duration : 0;
}

int mkv_read_packet(mkv_reader_t *r, mkv_packet_t *p) {
    if (!r || !p)
        return -EINVAL;
    for (;;) {
        // The cluster in hand: its next block.
        while (r->in_cluster && r->cl_at < r->cl_len) {
            uint32_t id;
            uint64_t sz;
            if (child(r->cl, r->cl_len, &r->cl_at, &id, &sz))
                return -EINVAL;
            const uint8_t *b = r->cl + r->cl_at;
            r->cl_at += (size_t)sz;
            if (id == ID_CLUSTER_TIMESTAMP) {
                r->cl_ts = (int64_t)be_uint(b, sz);
            } else if (id == ID_SIMPLE_BLOCK) {
                return parse_block(r, b, (size_t)sz, true, p) ?: 1;
            } else if (id == ID_BLOCK_GROUP) {
                // A Block, a keyframe unless it references another.
                const uint8_t *blk = NULL;
                uint64_t blen = 0;
                bool refs = false;
                for (size_t at = 0; at < sz;) {
                    uint32_t gid;
                    uint64_t gsz;
                    if (child(b, (size_t)sz, &at, &gid, &gsz))
                        return -EINVAL;
                    if (gid == ID_BLOCK) {
                        blk = b + at;
                        blen = gsz;
                    } else if (gid == ID_REFERENCE_BLOCK) {
                        refs = true;
                    }
                    at += gsz;
                }
                if (blk) {
                    int rc = parse_block(r, blk, (size_t)blen, false, p);
                    if (rc)
                        return rc;
                    p->keyframe = !refs;
                    return 1;
                }
            }
        }
        r->in_cluster = false;
        if (r->done)
            return 0;
        // The next top-level element.
        uint32_t id;
        uint64_t size, body;
        bool unknown;
        int h = top_header(r, &id, &size, &unknown, &body);
        if (h <= 0) {
            r->done = true;
            r->truncated |= h == -ENODATA;
            return h == -ENODATA ? 0 : h;
        }
        if (unknown)
            return -ENOTSUP;
        if (size > r->seg_end - body) {
            // Cut off inside this element: the walk ends before it.
            r->done = true;
            r->truncated = true;
            return 0;
        }
        uint64_t at = r->pos;
        r->pos = body + size;
        if (id == ID_CLUSTER) {
            if (size > MKV_CLUSTER_MAX)
                return -ENOTSUP;
            int rc = 0;
            free(r->cl);
            r->cl = load(r, body, size, &rc);
            if (!r->cl)
                return rc;
            r->cl_len = (size_t)size;
            r->cl_at = 0;
            r->cl_ts = 0;
            r->in_cluster = true;
            uint64_t pos = at - r->seg_data;
            if (push((void **)&r->clusters, &r->nclusters, &r->clusters_cap, sizeof(pos), &pos))
                return -ENOMEM;
        } else if (id == ID_CUES) {
            int rc = 0;
            uint8_t *b = size <= MKV_CLUSTER_MAX ? load(r, body, size, &rc) : NULL;
            if (!b)
                return rc ? rc : -ENOTSUP;
            rc = parse_cues(r, b, (size_t)size);
            free(b);
            if (rc)
                return rc;
        }
    }
}

bool mkv_read_truncated(const mkv_reader_t *r) {
    return r && r->truncated;
}

bool mkv_read_sized(const mkv_reader_t *r) {
    return r && r->sized;
}

size_t mkv_read_cues(const mkv_reader_t *r, const mkv_read_cue_t **cues) {
    if (cues)
        *cues = r ? r->cues : NULL;
    return r ? r->ncues : 0;
}

size_t mkv_read_clusters(const mkv_reader_t *r, const uint64_t **positions) {
    if (positions)
        *positions = r ? r->clusters : NULL;
    return r ? r->nclusters : 0;
}
