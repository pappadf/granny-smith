// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// udif_writer.c
// The streaming UDIF writer and verifier.  See udif_writer.h; the on-disk
// fields are the ones image_udif.c reads.

#include "udif_writer.h"

#include "common.h"
#include "crc32.h"
#include "deflate.h"
#include "image_udif.h"
#include "storage_util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// 'koly' trailer field offsets (big-endian), the full set a writer fills.
#define KOLY_VERSION      4
#define KOLY_FLAGS        0x0C
#define KOLY_RUNNING      0x10
#define KOLY_DATA_OFFSET  0x18
#define KOLY_DATA_LENGTH  0x20
#define KOLY_RSRC_OFFSET  0x28
#define KOLY_RSRC_LENGTH  0x30
#define KOLY_SEG_NUMBER   0x38
#define KOLY_SEG_COUNT    0x3C
#define KOLY_SEG_ID       0x40
#define KOLY_DATA_CK_TYPE 0x50
#define KOLY_DATA_CK_BITS 0x54
#define KOLY_DATA_CK      0x58
#define KOLY_XML_OFFSET   0xD8
#define KOLY_XML_LENGTH   0xE0
#define KOLY_MASTER_TYPE  0x160
#define KOLY_MASTER_BITS  0x164
#define KOLY_MASTER_CK    0x168
#define KOLY_VARIANT      0x1E8
#define KOLY_SECTORS      0x1EC

// 'mish' block table.
#define MISH_HEADER   0xCC
#define MISH_ENTRY    40
#define MISH_SECTOR   0x08
#define MISH_COUNT    0x10
#define MISH_DATA_OFF 0x18
#define MISH_BUFFERS  0x20
#define MISH_DESC     0x24
#define MISH_CK_TYPE  0x40
#define MISH_CK_BITS  0x44
#define MISH_CK       0x48
#define MISH_CHUNKS   0xC8

typedef struct {
    uint32_t type;
    uint64_t sector, count, offset, length;
} wext_t;

struct udif_writer {
    FILE *f;
    char *path;
    char *source_name;
    uint32_t chunk_sectors;
    size_t chunk_bytes;
    int level;

    uint8_t *partial; // one chunk being gathered
    size_t partial_len;
    uint8_t *zbuf; // deflate output
    size_t zcap;
    deflate_state_t *ds;

    wext_t *ext;
    size_t n_ext, cap_ext;

    uint64_t bytes_in; // decoded bytes so far
    uint64_t sectors; // sectors emitted (whole chunks)
    uint64_t data_len; // data-fork bytes written
    uint64_t zero_bytes;
    uint32_t crc_dec; // CRC-32 of the decoded bytes
    uint32_t crc_data; // CRC-32 of the data fork
    const char *table_name; // what the first chunk shows the disk to be
    int err;
};

// Name the block table for what the disk starts with, in hdiutil's
// spelling, so tools that open the payload pick the right reader (7-Zip
// treats an "Apple_HFS" table as a bare HFS volume).
static const char *table_name_for(const uint8_t *p, size_t n) {
    if (n >= 2 && p[0] == 'E' && p[1] == 'R')
        return "whole disk (Apple_partition_scheme : 0)";
    if (n >= 1026 && ((p[1024] == 'B' && p[1025] == 'D') || (p[1024] == 'H' && (p[1025] == '+' || p[1025] == 'X'))))
        return "whole disk (Apple_HFS : 0)";
    if (n >= 32774 && memcmp(p + 32769, "CD001", 5) == 0)
        return "whole disk (ISO9660 : 0)";
    return "whole disk (Unknown Partition : 0)";
}

static void put32(uint8_t *p, uint32_t v) {
    WR_BE32(p, v);
}

static void put64(uint8_t *p, uint64_t v) {
    WR_BE32(p, (uint32_t)(v >> 32));
    WR_BE32(p + 4, (uint32_t)v);
}

static void set_err(char *err, size_t cap, const char *fmt, const char *a, int e) {
    if (err && cap)
        snprintf(err, cap, fmt, a, strerror(e));
}

udif_writer_t *udif_writer_open(const char *path, const udif_writer_opts_t *opts, char *err, size_t errcap) {
    udif_writer_opts_t o = opts ? *opts : (udif_writer_opts_t){0};
    if (!o.chunk_sectors)
        o.chunk_sectors = UDIF_WRITER_CHUNK_SECTORS;
    if (o.chunk_sectors < 8 || o.chunk_sectors > 2048 || (o.chunk_sectors & (o.chunk_sectors - 1))) {
        if (err && errcap)
            snprintf(err, errcap, "chunk of %u sectors: must be a power of two in 8..2048", o.chunk_sectors);
        return NULL;
    }
    if (o.level < 0)
        o.level = 1;
    if (o.level > 9)
        o.level = 9;
    if (!path || !*path) {
        set_err(err, errcap, "no path%s (%s)", "", EINVAL);
        return NULL;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        set_err(err, errcap, "cannot create '%s': %s", path, errno);
        return NULL;
    }
    udif_writer_t *w = calloc(1, sizeof(*w));
    if (!w) {
        close(fd);
        unlink(path);
        set_err(err, errcap, "'%s': %s", path, ENOMEM);
        return NULL;
    }
    w->f = fdopen(fd, "wb");
    w->path = gs_strdup(path);
    w->source_name = o.source_name ? gs_strdup(o.source_name) : NULL;
    w->chunk_sectors = o.chunk_sectors;
    w->chunk_bytes = (size_t)o.chunk_sectors * UDIF_SECTOR_SIZE;
    w->level = o.level;
    w->partial = malloc(w->chunk_bytes);
    w->zcap = deflate_bound(w->chunk_bytes);
    w->zbuf = malloc(w->zcap);
    w->ds = o.level > 0 ? deflate_state_new(w->chunk_bytes) : NULL;
    if (!w->f || !w->path || !w->partial || !w->zbuf || (o.level > 0 && !w->ds)) {
        if (!w->f)
            close(fd);
        set_err(err, errcap, "'%s': %s", path, ENOMEM);
        udif_writer_abort(w);
        return NULL;
    }
    return w;
}

static int push_extent(udif_writer_t *w, wext_t e) {
    if (w->n_ext == w->cap_ext) {
        size_t ncap = w->cap_ext ? w->cap_ext * 2 : 256;
        wext_t *n = realloc(w->ext, ncap * sizeof(*n));
        if (!n)
            return -ENOMEM;
        w->ext = n;
        w->cap_ext = ncap;
    }
    w->ext[w->n_ext++] = e;
    return 0;
}

// Extend the trailing ZERO extent, or start one.
static int add_zero(udif_writer_t *w, uint64_t sectors) {
    if (w->sectors == 0)
        w->table_name = table_name_for(NULL, 0);
    w->zero_bytes += sectors * UDIF_SECTOR_SIZE;
    w->crc_dec = gs_crc32_zeros(w->crc_dec, sectors * UDIF_SECTOR_SIZE);
    if (w->n_ext && w->ext[w->n_ext - 1].type == UDIF_CHUNK_ZERO) {
        w->ext[w->n_ext - 1].count += sectors;
    } else {
        int rc = push_extent(w, (wext_t){UDIF_CHUNK_ZERO, w->sectors, sectors, w->data_len, 0});
        if (rc)
            return rc;
    }
    w->sectors += sectors;
    return 0;
}

static bool all_zero(const uint8_t *p, size_t n) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        if (v)
            return false;
    }
    for (; i < n; i++)
        if (p[i])
            return false;
    return true;
}

static int write_out(udif_writer_t *w, const void *p, size_t n) {
    if (n && fwrite(p, 1, n, w->f) != n)
        return errno ? -errno : -EIO;
    return 0;
}

// Emit one chunk of `n` bytes (a whole number of sectors).
static int emit_chunk(udif_writer_t *w, const uint8_t *p, size_t n) {
    uint64_t secs = n / UDIF_SECTOR_SIZE;
    if (w->sectors == 0)
        w->table_name = table_name_for(p, n);
    if (all_zero(p, n))
        return add_zero(w, secs);
    w->crc_dec = gs_crc32(w->crc_dec, p, n);
    const uint8_t *out = p;
    size_t out_len = n;
    uint32_t type = UDIF_CHUNK_RAW;
    if (w->level > 0) {
        long z = deflate_zlib(w->ds, p, n, w->zbuf, w->zcap, w->level);
        if (z > 0 && (size_t)z < n) {
            out = w->zbuf;
            out_len = (size_t)z;
            type = UDIF_CHUNK_ZLIB;
        }
    }
    int rc = write_out(w, out, out_len);
    if (rc)
        return rc;
    w->crc_data = gs_crc32(w->crc_data, out, out_len);
    rc = push_extent(w, (wext_t){type, w->sectors, secs, w->data_len, out_len});
    if (rc)
        return rc;
    w->data_len += out_len;
    w->sectors += secs;
    return 0;
}

int udif_writer_append(udif_writer_t *w, const void *buf, size_t len) {
    if (!w)
        return -EINVAL;
    if (w->err)
        return w->err;
    const uint8_t *p = buf;
    w->bytes_in += len;
    while (len > 0) {
        if (w->partial_len == 0 && len >= w->chunk_bytes) {
            // Whole chunks straight from the caller's buffer.
            int rc = emit_chunk(w, p, w->chunk_bytes);
            if (rc)
                return w->err = rc;
            p += w->chunk_bytes;
            len -= w->chunk_bytes;
            continue;
        }
        size_t n = w->chunk_bytes - w->partial_len;
        if (n > len)
            n = len;
        memcpy(w->partial + w->partial_len, p, n);
        w->partial_len += n;
        p += n;
        len -= n;
        if (w->partial_len == w->chunk_bytes) {
            int rc = emit_chunk(w, w->partial, w->chunk_bytes);
            w->partial_len = 0;
            if (rc)
                return w->err = rc;
        }
    }
    return 0;
}

int udif_writer_append_zeros(udif_writer_t *w, uint64_t len) {
    if (!w)
        return -EINVAL;
    if (w->err)
        return w->err;
    // Top up a partial chunk byte-wise, then whole chunks as one ZERO run.
    static const uint8_t zeros[4096];
    while (len > 0 && w->partial_len > 0) {
        size_t n = len < sizeof(zeros) ? (size_t)len : sizeof(zeros);
        int rc = udif_writer_append(w, zeros, n);
        if (rc)
            return rc;
        len -= n;
    }
    uint64_t whole = len / w->chunk_bytes * w->chunk_bytes;
    if (whole) {
        int rc = add_zero(w, whole / UDIF_SECTOR_SIZE);
        if (rc)
            return w->err = rc;
        w->bytes_in += whole;
        len -= whole;
    }
    while (len > 0) {
        size_t n = len < sizeof(zeros) ? (size_t)len : sizeof(zeros);
        int rc = udif_writer_append(w, zeros, n);
        if (rc)
            return rc;
        len -= n;
    }
    return 0;
}

void udif_writer_progress(const udif_writer_t *w, udif_writer_stats_t *out) {
    if (!w || !out)
        return;
    memset(out, 0, sizeof(*out));
    out->bytes_in = w->bytes_in;
    out->sectors = w->sectors;
    out->stored_bytes = w->data_len;
    out->zero_bytes = w->zero_bytes;
    out->extents = w->n_ext;
    out->crc = w->crc_dec;
}

static void writer_free(udif_writer_t *w) {
    if (!w)
        return;
    if (w->f)
        fclose(w->f);
    free(w->path);
    free(w->source_name);
    free(w->partial);
    free(w->zbuf);
    deflate_state_free(w->ds);
    free(w->ext);
    free(w);
}

void udif_writer_abort(udif_writer_t *w) {
    if (!w)
        return;
    if (w->f) {
        fclose(w->f);
        w->f = NULL;
    }
    if (w->path)
        unlink(w->path);
    writer_free(w);
}

// --- The property list -------------------------------------------------------

typedef struct {
    char *buf;
    size_t len, cap;
    bool oom;
} sbuf_t;

static void sb_put(sbuf_t *s, const char *p, size_t n) {
    if (s->oom)
        return;
    if (s->len + n + 1 > s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 4096;
        while (ncap < s->len + n + 1)
            ncap *= 2;
        char *nb = realloc(s->buf, ncap);
        if (!nb) {
            s->oom = true;
            return;
        }
        s->buf = nb;
        s->cap = ncap;
    }
    memcpy(s->buf + s->len, p, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

static void sb_str(sbuf_t *s, const char *p) {
    sb_put(s, p, strlen(p));
}

// Text as XML character data.
static void sb_xml(sbuf_t *s, const char *p) {
    for (; *p; p++) {
        if (*p == '<')
            sb_str(s, "&lt;");
        else if (*p == '>')
            sb_str(s, "&gt;");
        else if (*p == '&')
            sb_str(s, "&amp;");
        else if ((unsigned char)*p >= 0x20 || *p == '\t')
            sb_put(s, p, 1);
    }
}

// Base64, in lines of 52 characters with the plist's tab indent, as
// hdiutil wraps them.
static void sb_base64(sbuf_t *s, const uint8_t *d, size_t n) {
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t col = 0;
    sb_str(s, "\t\t\t\t");
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16;
        if (i + 1 < n)
            v |= (uint32_t)d[i + 1] << 8;
        if (i + 2 < n)
            v |= d[i + 2];
        char q[4] = {a[(v >> 18) & 63], a[(v >> 12) & 63], i + 1 < n ? a[(v >> 6) & 63] : '=',
                     i + 2 < n ? a[v & 63] : '='};
        sb_put(s, q, 4);
        col += 4;
        if (col >= 52 && i + 3 < n) {
            sb_str(s, "\n\t\t\t\t");
            col = 0;
        }
    }
    sb_str(s, "\n");
}

// The 'mish' block table for the extents written.
static uint8_t *build_mish(const udif_writer_t *w, size_t *out_len) {
    size_t n = MISH_HEADER + (w->n_ext + 1) * MISH_ENTRY;
    uint8_t *b = calloc(1, n);
    if (!b)
        return NULL;
    put32(b, 0x6D697368u); // 'mish'
    put32(b + 4, 1);
    put64(b + MISH_SECTOR, 0);
    put64(b + MISH_COUNT, w->sectors);
    put64(b + MISH_DATA_OFF, 0);
    put32(b + MISH_BUFFERS, w->chunk_sectors + 8);
    put32(b + MISH_DESC, 0);
    put32(b + MISH_CK_TYPE, UDIF_CHECKSUM_CRC32);
    put32(b + MISH_CK_BITS, 32);
    put32(b + MISH_CK, w->crc_dec);
    put32(b + MISH_CHUNKS, (uint32_t)(w->n_ext + 1));
    for (size_t i = 0; i <= w->n_ext; i++) {
        uint8_t *e = b + MISH_HEADER + i * MISH_ENTRY;
        if (i == w->n_ext) {
            put32(e, UDIF_CHUNK_END);
            put64(e + 8, w->sectors);
            put64(e + 24, w->data_len);
            continue;
        }
        const wext_t *x = &w->ext[i];
        put32(e, x->type);
        put64(e + 8, x->sector);
        put64(e + 16, x->count);
        put64(e + 24, x->offset);
        put64(e + 32, x->length);
    }
    *out_len = n;
    return b;
}

static char *build_plist(const udif_writer_t *w, size_t *out_len) {
    size_t mlen = 0;
    uint8_t *mish = build_mish(w, &mlen);
    if (!mish)
        return NULL;
    sbuf_t s = {0};
    sb_str(&s, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
               "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
               "<plist version=\"1.0\">\n<dict>\n"
               "\t<key>resource-fork</key>\n\t<dict>\n\t\t<key>blkx</key>\n\t\t<array>\n\t\t\t<dict>\n"
               "\t\t\t\t<key>Attributes</key>\n\t\t\t\t<string>0x0050</string>\n"
               "\t\t\t\t<key>CFName</key>\n\t\t\t\t<string>");
    sb_str(&s, w->table_name ? w->table_name : table_name_for(NULL, 0));
    sb_str(&s, "</string>\n"
               "\t\t\t\t<key>Data</key>\n\t\t\t\t<data>\n");
    sb_base64(&s, mish, mlen);
    free(mish);
    sb_str(&s, "\t\t\t\t</data>\n"
               "\t\t\t\t<key>ID</key>\n\t\t\t\t<string>0</string>\n"
               "\t\t\t\t<key>Name</key>\n\t\t\t\t<string>");
    sb_str(&s, w->table_name ? w->table_name : table_name_for(NULL, 0));
    sb_str(&s, "</string>\n"
               "\t\t\t</dict>\n\t\t</array>\n\t</dict>\n"
               "\t<key>gs-profile</key>\n\t<integer>1</integer>\n");
    if (w->bytes_in != w->sectors * UDIF_SECTOR_SIZE) {
        char num[32];
        snprintf(num, sizeof(num), "%llu", (unsigned long long)w->bytes_in);
        sb_str(&s, "\t<key>gs-byte-length</key>\n\t<integer>");
        sb_str(&s, num);
        sb_str(&s, "</integer>\n");
    }
    if (w->source_name && *w->source_name) {
        sb_str(&s, "\t<key>gs-source</key>\n\t<string>");
        sb_xml(&s, w->source_name);
        sb_str(&s, "</string>\n");
    }
    sb_str(&s, "</dict>\n</plist>\n");
    if (s.oom) {
        free(s.buf);
        return NULL;
    }
    *out_len = s.len;
    return s.buf;
}

// 16 random bytes for the segment ID (a v4 UUID).
static void random_uuid(uint8_t out[16]) {
    bool got = false;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        got = read(fd, out, 16) == 16;
        close(fd);
    }
    if (!got) {
        uint64_t x = (uint64_t)time(NULL) ^ ((uint64_t)(uintptr_t)out << 17) ^ (uint64_t)getpid();
        for (int i = 0; i < 16; i++) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            out[i] = (uint8_t)x;
        }
    }
    out[6] = (uint8_t)((out[6] & 0x0F) | 0x40);
    out[8] = (uint8_t)((out[8] & 0x3F) | 0x80);
}

int udif_writer_finish(udif_writer_t *w, udif_writer_stats_t *stats) {
    if (!w)
        return -EINVAL;
    int rc = w->err;
    // The tail: a partial chunk, padded to a whole sector.
    if (!rc && w->partial_len) {
        size_t pad = (UDIF_SECTOR_SIZE - w->partial_len % UDIF_SECTOR_SIZE) % UDIF_SECTOR_SIZE;
        memset(w->partial + w->partial_len, 0, pad);
        rc = emit_chunk(w, w->partial, w->partial_len + pad);
        w->partial_len = 0;
    }
    if (!rc && w->sectors == 0)
        rc = -EINVAL;
    if (!rc && w->sectors > UINT32_MAX)
        rc = -EFBIG; // more than any reader of ours (or storage) can open
    size_t xlen = 0;
    char *xml = NULL;
    if (!rc) {
        xml = build_plist(w, &xlen);
        if (!xml)
            rc = -ENOMEM;
    }
    if (!rc)
        rc = write_out(w, xml, xlen);
    free(xml);
    if (!rc) {
        uint8_t k[UDIF_TRAILER_SIZE] = {0};
        memcpy(k, "koly", 4);
        put32(k + 4, KOLY_VERSION);
        put32(k + 8, UDIF_TRAILER_SIZE);
        put32(k + KOLY_FLAGS, 1); // flattened
        put64(k + KOLY_RUNNING, 0);
        put64(k + KOLY_DATA_OFFSET, 0);
        put64(k + KOLY_DATA_LENGTH, w->data_len);
        put64(k + KOLY_RSRC_OFFSET, 0);
        put64(k + KOLY_RSRC_LENGTH, 0);
        put32(k + KOLY_SEG_NUMBER, 1);
        put32(k + KOLY_SEG_COUNT, 1);
        random_uuid(k + KOLY_SEG_ID);
        put32(k + KOLY_DATA_CK_TYPE, UDIF_CHECKSUM_CRC32);
        put32(k + KOLY_DATA_CK_BITS, 32);
        put32(k + KOLY_DATA_CK, w->crc_data);
        put64(k + KOLY_XML_OFFSET, w->data_len);
        put64(k + KOLY_XML_LENGTH, xlen);
        // The master checksum: CRC-32 over the block tables' checksums,
        // each as its big-endian 4 bytes.
        uint8_t ck[4];
        put32(ck, w->crc_dec);
        put32(k + KOLY_MASTER_TYPE, UDIF_CHECKSUM_CRC32);
        put32(k + KOLY_MASTER_BITS, 32);
        put32(k + KOLY_MASTER_CK, gs_crc32(0, ck, 4));
        put32(k + KOLY_VARIANT, 1); // device image
        put64(k + KOLY_SECTORS, w->sectors);
        rc = write_out(w, k, sizeof(k));
    }
    if (!rc && fflush(w->f) != 0)
        rc = errno ? -errno : -EIO;
    if (!rc) {
        fsync(fileno(w->f));
        if (stats) {
            udif_writer_progress(w, stats);
            stats->stored_bytes = w->data_len + xlen + UDIF_TRAILER_SIZE;
        }
        if (fclose(w->f) != 0)
            rc = errno ? -errno : -EIO;
        w->f = NULL;
    }
    if (rc) {
        udif_writer_abort(w);
        return rc;
    }
    writer_free(w);
    return 0;
}

int udif_create_empty(const char *path, uint64_t size) {
    char err[160];
    udif_writer_opts_t o = {.level = 0};
    udif_writer_t *w = udif_writer_open(path, &o, err, sizeof(err));
    if (!w)
        return access(path, F_OK) == 0 ? -EEXIST : -EIO;
    int rc = udif_writer_append_zeros(w, size);
    if (rc) {
        udif_writer_abort(w);
        return rc;
    }
    return udif_writer_finish(w, NULL);
}

// --- Verification -------------------------------------------------------------

int udif_verify(gs_source_t *data, udif_writer_stats_t *stats, char *err, size_t errcap) {
#define FAIL(rc_, ...)                                                                                                 \
    do {                                                                                                               \
        if (err && errcap)                                                                                             \
            snprintf(err, errcap, __VA_ARGS__);                                                                        \
        rc = (rc_);                                                                                                    \
        goto out;                                                                                                      \
    } while (0)
    int rc = 0;
    uint8_t *xml = NULL, *in = NULL, *outb = NULL;
    udif_map_t *map = NULL;
    udif_writer_stats_t st = {0};
    size_t in_cap = 0, out_cap = 0;
    uint64_t size = data ? gs_source_size(data) : 0;
    uint8_t tb[UDIF_TRAILER_SIZE];
    udif_trailer_t tr;
    if (size < UDIF_TRAILER_SIZE || gs_source_read_exact(data, size - UDIF_TRAILER_SIZE, tb, sizeof(tb)) != 0 ||
        udif_parse_trailer(tb, sizeof(tb), &tr) != 0)
        FAIL(-EINVAL, "not a UDIF image (no usable 'koly' trailer)");
    if (tr.xml_offset > size || tr.xml_length > size - tr.xml_offset)
        FAIL(-EINVAL, "the block map lies outside the file");
    xml = malloc((size_t)tr.xml_length);
    if (!xml)
        FAIL(-ENOMEM, "out of memory");
    if (gs_source_read_exact(data, tr.xml_offset, xml, (size_t)tr.xml_length) != 0)
        FAIL(-EIO, "cannot read the block map");
    if (udif_parse_blkx(xml, (size_t)tr.xml_length, &map) != 0)
        FAIL(-EINVAL, "the block map does not parse");
    if (tr.data_fork_offset > size || tr.data_fork_length > size - tr.data_fork_offset)
        FAIL(-EINVAL, "the data fork lies outside the file");

    // The data fork's CRC, streamed.
    if (tr.checksum_type == UDIF_CHECKSUM_CRC32) {
        uint8_t buf[65536];
        uint32_t crc = 0;
        for (uint64_t at = 0; at < tr.data_fork_length;) {
            size_t n = tr.data_fork_length - at < sizeof(buf) ? (size_t)(tr.data_fork_length - at) : sizeof(buf);
            if (gs_source_read_exact(data, tr.data_fork_offset + at, buf, n) != 0)
                FAIL(-EIO, "cannot read the data fork at %llu", (unsigned long long)at);
            crc = gs_crc32(crc, buf, n);
            at += n;
        }
        if (crc != tr.checksum)
            FAIL(-EILSEQ, "data fork checksum %08x, the trailer says %08x", crc, tr.checksum);
    }

    for (size_t t = 0; t < map->n_tables; t++) {
        const udif_table_t *tbl = &map->tables[t];
        uint32_t crc = 0;
        for (size_t i = 0; i < tbl->n_chunks; i++) {
            const udif_chunk_t *c = &tbl->chunks[i];
            uint64_t bytes = c->count * UDIF_SECTOR_SIZE;
            st.extents++;
            if (c->type == UDIF_CHUNK_ZERO || c->type == UDIF_CHUNK_IGNORE) {
                st.zero_bytes += bytes;
                if (c->type == UDIF_CHUNK_ZERO)
                    crc = gs_crc32_zeros(crc, bytes);
                continue;
            }
            if (bytes > (256u << 20) || c->length > (256u << 20))
                FAIL(-EFBIG, "chunk at sector %llu is too large to verify",
                     (unsigned long long)(tbl->base_sector + c->sector));
            if (c->offset > tr.data_fork_length || c->length > tr.data_fork_length - c->offset)
                FAIL(-EINVAL, "chunk at sector %llu lies outside the data fork",
                     (unsigned long long)(tbl->base_sector + c->sector));
            if (c->length > in_cap) {
                free(in);
                in_cap = (size_t)c->length;
                in = malloc(in_cap ? in_cap : 1);
            }
            if (bytes > out_cap) {
                free(outb);
                out_cap = (size_t)bytes;
                outb = malloc(out_cap ? out_cap : 1);
            }
            if (!in || !outb)
                FAIL(-ENOMEM, "out of memory");
            if (gs_source_read_exact(data, tr.data_fork_offset + c->offset, in, (size_t)c->length) != 0)
                FAIL(-EIO, "cannot read the chunk at sector %llu", (unsigned long long)(tbl->base_sector + c->sector));
            int drc = udif_decode_chunk(c, in, (size_t)c->length, outb, (size_t)bytes);
            if (drc != 0)
                FAIL(drc == -ENOTSUP ? -ENOTSUP : -EILSEQ, "chunk at sector %llu (type %#x) does not decode",
                     (unsigned long long)(tbl->base_sector + c->sector), c->type);
            crc = gs_crc32(crc, outb, (size_t)bytes);
        }
        if (tbl->checksum_type == UDIF_CHECKSUM_CRC32 && crc != tbl->checksum)
            FAIL(-EILSEQ, "block table '%s': checksum %08x, the table says %08x", tbl->name, crc, tbl->checksum);
        if (map->n_tables == 1)
            st.crc = crc;
    }
    st.sectors = tr.sectors;
    st.bytes_in = tr.sectors * UDIF_SECTOR_SIZE;
    st.stored_bytes = size;
out:
    if (stats)
        *stats = st;
    udif_map_free(map);
    free(xml);
    free(in);
    free(outb);
    return rc;
#undef FAIL
}

// --- Info -----------------------------------------------------------------------

// The text of <key>key</key><tag>VALUE</tag> in `xml`, unescaped, into `dst`.
static bool plist_value(const char *xml, size_t len, const char *key, char *dst, size_t cap) {
    char k[64];
    snprintf(k, sizeof(k), "<key>%s</key>", key);
    size_t kl = strlen(k);
    for (size_t i = 0; i + kl <= len; i++) {
        if (memcmp(xml + i, k, kl) != 0)
            continue;
        const char *p = memchr(xml + i + kl, '>', len - i - kl);
        if (!p)
            return false;
        p++;
        const char *end = xml + len;
        size_t n = 0;
        while (p < end && *p != '<' && n + 1 < cap) {
            if (*p == '&') {
                const char *semi = memchr(p, ';', (size_t)(end - p));
                if (semi) {
                    size_t el = (size_t)(semi - p);
                    char c = el == 3 && !memcmp(p, "&lt", 3)    ? '<'
                             : el == 3 && !memcmp(p, "&gt", 3)  ? '>'
                             : el == 4 && !memcmp(p, "&amp", 4) ? '&'
                                                                : '?';
                    dst[n++] = c;
                    p = semi + 1;
                    continue;
                }
            }
            dst[n++] = *p++;
        }
        dst[n] = '\0';
        return true;
    }
    return false;
}

int udif_info(gs_source_t *data, udif_info_t *out) {
    if (!data || !out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    uint64_t size = gs_source_size(data);
    uint8_t tb[UDIF_TRAILER_SIZE];
    udif_trailer_t tr;
    if (size < UDIF_TRAILER_SIZE || gs_source_read_exact(data, size - UDIF_TRAILER_SIZE, tb, sizeof(tb)) != 0)
        return -EINVAL;
    int rc = udif_parse_trailer(tb, sizeof(tb), &tr);
    if (rc)
        return rc;
    if (tr.xml_offset > size || tr.xml_length > size - tr.xml_offset)
        return -EINVAL;
    char *xml = malloc((size_t)tr.xml_length + 1);
    if (!xml)
        return -ENOMEM;
    udif_map_t *map = NULL;
    rc = gs_source_read_exact(data, tr.xml_offset, xml, (size_t)tr.xml_length);
    if (!rc)
        rc = udif_parse_blkx((const uint8_t *)xml, (size_t)tr.xml_length, &map);
    if (!rc) {
        size_t xl = (size_t)tr.xml_length;
        xml[xl] = '\0';
        out->sectors = tr.sectors;
        out->byte_length = tr.sectors * UDIF_SECTOR_SIZE;
        out->data_fork_bytes = tr.data_fork_length;
        out->tables = (uint32_t)map->n_tables;
        char v[64];
        out->gs_profile = plist_value(xml, xl, "gs-profile", v, sizeof(v));
        if (plist_value(xml, xl, "gs-byte-length", v, sizeof(v))) {
            uint64_t n = strtoull(v, NULL, 10);
            if (n && n <= out->byte_length)
                out->byte_length = n;
        }
        plist_value(xml, xl, "gs-source", out->source_name, sizeof(out->source_name));
        for (size_t t = 0; t < map->n_tables; t++) {
            for (size_t i = 0; i < map->tables[t].n_chunks; i++) {
                const udif_chunk_t *c = &map->tables[t].chunks[i];
                out->extents++;
                uint64_t bytes = c->count * UDIF_SECTOR_SIZE;
                if (c->type == UDIF_CHUNK_ZERO || c->type == UDIF_CHUNK_IGNORE)
                    out->zero_bytes += bytes;
                else if (c->type != UDIF_CHUNK_RAW && bytes > out->max_chunk_bytes)
                    out->max_chunk_bytes = bytes;
            }
        }
        if (map->n_tables == 1)
            out->crc = map->tables[0].checksum;
    }
    udif_map_free(map);
    free(xml);
    return rc;
}
