// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// afp_meta.c
// AppleDouble sidecar metadata for the AFP server (see afp_meta.h).

#include "afp_meta.h"
#include "common.h"

#include "appledouble.h"
#include "log.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

LOG_USE_CATEGORY_NAME("appletalk");

// Ceiling on a sidecar we are willing to read whole.  The fork itself is
// streamed by afp_fork.c; this bound only guards the metadata reader.
#define AFP_META_SIDECAR_MAX (64u * 1024u * 1024u)

// --- big-endian helpers ----------------------------------------------------

// --- time conversions ------------------------------------------------------

// Host (Unix) seconds -> AFP time, clamped into the 32-bit wire field.
uint32_t afp_meta_time_from_unix(int64_t unix_secs) {
    int64_t v = unix_secs + 2082844800LL; // 1970 -> 1904
    if (v < 0)
        return 0;
    if (v > (int64_t)UINT32_MAX)
        return UINT32_MAX;
    return (uint32_t)v;
}

// AFP time -> host (Unix) seconds.  AFP_DATE_NEVER maps to 0 so a caller can
// pass it straight to utimes() without inventing a date in 2040.
int64_t afp_meta_time_to_unix(uint32_t afp_secs) {
    if (afp_secs == AFP_DATE_NEVER)
        return 0;
    return (int64_t)afp_secs - 2082844800LL;
}

// AFP time (1904 epoch) -> AppleDouble DATES entry (signed secs from 2000).
static int32_t ad_date_from_afp(uint32_t afp_secs) {
    if (afp_secs == AFP_DATE_NEVER)
        return INT32_MIN; // the AppleDouble "unknown" sentinel
    int64_t v = (int64_t)afp_secs - (int64_t)AFP_META_EPOCH_DELTA;
    if (v < INT32_MIN)
        return INT32_MIN;
    if (v > INT32_MAX)
        return INT32_MAX;
    return (int32_t)v;
}

// AppleDouble DATES entry -> AFP time.
static uint32_t afp_date_from_ad(int32_t ad_secs) {
    if (ad_secs == INT32_MIN)
        return AFP_DATE_NEVER;
    int64_t v = (int64_t)ad_secs + (int64_t)AFP_META_EPOCH_DELTA;
    if (v < 0)
        return 0;
    if (v > (int64_t)UINT32_MAX)
        return UINT32_MAX;
    return (uint32_t)v;
}

// --- sidecar path ----------------------------------------------------------

bool afp_meta_sidecar_path(const char *host_path, char *out, size_t cap) {
    if (!host_path || !out || cap == 0)
        return false;
    const char *slash = strrchr(host_path, '/');
    const char *base = slash ? slash + 1 : host_path;
    if (!*base)
        return false;
    int n = slash ? snprintf(out, cap, "%.*s._%s", (int)(slash - host_path + 1), host_path, base)
                  : snprintf(out, cap, "._%s", base);
    return n > 0 && (size_t)n < cap;
}

bool afp_meta_is_hidden(const char *name) {
    if (!name)
        return false;
    size_t n = strlen(name);
    if (n >= 2 && name[0] == '.' && name[1] == '_')
        return true;
    if (n >= 5 && strcmp(name + n - 5, ".rsrc") == 0)
        return true;
    if (strcmp(name, AFP_CONTROL_DIR) == 0)
        return true;
    return false;
}

// --- whole-file read -------------------------------------------------------

static uint32_t read_entry_table(const char *sidecar, uint8_t *hdr, size_t hdr_cap, size_t *out_got);
static bool find_entry_extent(const uint8_t *hdr, uint32_t n_entries, uint32_t id, uint32_t *out_off,
                              uint32_t *out_len);

// Load a file's metadata without ever reading its resource fork.  The four
// metadata entries are small and written ahead of the fork, so each is read
// by seeking to its own extent — an enumeration of a directory full of
// multi-megabyte forks costs a few hundred bytes per file, not megabytes.
bool afp_meta_load(const char *host_path, afp_meta_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    char sc[PATH_MAX];
    if (!host_path || !afp_meta_sidecar_path(host_path, sc, sizeof(sc)))
        return false;
    uint8_t hdr[26 + AD_MAX_ENTRIES * 12];
    uint32_t n = read_entry_table(sc, hdr, sizeof(hdr), NULL);
    if (!n)
        return false;
    FILE *f = fopen(sc, "rb");
    if (!f)
        return false;

    uint32_t off = 0, len = 0;
    uint8_t buf[AFP_META_COMMENT_MAX + 1];

    if (find_entry_extent(hdr, n, AD_ENTRY_DATES, &off, &len) && len >= 16 && fseek(f, (long)off, SEEK_SET) == 0 &&
        fread(buf, 1, 16, f) == 16) {
        out->create_date = afp_date_from_ad((int32_t)RD_BE32(buf + 0));
        out->modify_date = afp_date_from_ad((int32_t)RD_BE32(buf + 4));
        out->backup_date = afp_date_from_ad((int32_t)RD_BE32(buf + 8));
        out->access_date = afp_date_from_ad((int32_t)RD_BE32(buf + 12));
        out->has_dates = true;
    }
    if (find_entry_extent(hdr, n, AD_ENTRY_FINDER, &off, &len) && len >= AFP_META_FINDER_SIZE &&
        fseek(f, (long)off, SEEK_SET) == 0 && fread(out->finder, 1, AFP_META_FINDER_SIZE, f) == AFP_META_FINDER_SIZE) {
        out->has_finder = true;
    }
    if (find_entry_extent(hdr, n, AD_ENTRY_MACINFO, &off, &len) && len >= 4 && fseek(f, (long)off, SEEK_SET) == 0 &&
        fread(buf, 1, 4, f) == 4) {
        // Entry 10's leading two bytes carry our AFP attribute word; the
        // trailing byte keeps the classic "protected" flag for foreign readers.
        out->attrs = (uint16_t)((buf[0] << 8) | buf[1]);
        out->has_attrs = true;
    }
    if (find_entry_extent(hdr, n, AD_ENTRY_COMMENT, &off, &len) && fseek(f, (long)off, SEEK_SET) == 0) {
        size_t want = len > AFP_META_COMMENT_MAX ? AFP_META_COMMENT_MAX : len;
        size_t got = want ? fread(out->comment, 1, want, f) : 0;
        out->comment[got] = '\0';
        out->comment_len = (uint8_t)got;
        out->has_comment = true;
    }
    fclose(f);
    return true;
}

void afp_meta_load_rsrc(const char *host_path, uint8_t **rsrc, size_t *rsrc_len) {
    if (rsrc)
        *rsrc = NULL;
    if (rsrc_len)
        *rsrc_len = 0;
    if (!rsrc || !rsrc_len)
        return;
    size_t len = afp_meta_rsrc_len(host_path);
    if (!len || len > AFP_META_SIDECAR_MAX)
        return;
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf)
        return;
    FILE *tmp = tmpfile();
    if (!tmp) {
        free(buf);
        return;
    }
    size_t copied = afp_meta_copy_rsrc(host_path, tmp);
    rewind(tmp);
    if (copied == len && fread(buf, 1, len, tmp) == len) {
        *rsrc = buf;
        *rsrc_len = len;
    } else {
        free(buf);
    }
    fclose(tmp);
}

// Read a sidecar's fixed header plus its entry table, without touching the
// payloads.  ad_detect() cannot be used here: it validates that every entry
// lies inside the buffer, which is false by construction for a header-only
// read of a sidecar carrying a multi-megabyte fork.  Returns the entry count,
// or 0 when the file is not a usable AppleDouble header.
static uint32_t read_entry_table(const char *sidecar, uint8_t *hdr, size_t hdr_cap, size_t *out_got) {
    FILE *f = fopen(sidecar, "rb");
    if (!f)
        return 0;
    size_t got = fread(hdr, 1, hdr_cap, f);
    fclose(f);
    if (out_got)
        *out_got = got;
    if (got < 26)
        return 0;
    if (RD_BE32(hdr) != APPLEDOUBLE_MAGIC && RD_BE32(hdr) != APPLESINGLE_MAGIC)
        return 0;
    if (RD_BE32(hdr + 4) != APPLE_FORK_VERSION)
        return 0;
    uint32_t n = (uint32_t)((hdr[24] << 8) | hdr[25]);
    if (n > AD_MAX_ENTRIES)
        return 0;
    if (26 + (size_t)n * 12 > got)
        return 0;
    return n;
}

// Offset and length of one entry, or false when it is absent.
static bool find_entry_extent(const uint8_t *hdr, uint32_t n_entries, uint32_t id, uint32_t *out_off,
                              uint32_t *out_len) {
    for (uint32_t i = 0; i < n_entries; i++) {
        const uint8_t *d = hdr + 26 + (size_t)i * 12;
        if (RD_BE32(d) != id)
            continue;
        if (out_off)
            *out_off = RD_BE32(d + 4);
        if (out_len)
            *out_len = RD_BE32(d + 8);
        return true;
    }
    return false;
}

uint32_t afp_meta_rsrc_len(const char *host_path) {
    char sc[PATH_MAX];
    if (!host_path || !afp_meta_sidecar_path(host_path, sc, sizeof(sc)))
        return 0;
    uint8_t hdr[26 + AD_MAX_ENTRIES * 12];
    uint32_t n = read_entry_table(sc, hdr, sizeof(hdr), NULL);
    uint32_t len = 0;
    if (n && find_entry_extent(hdr, n, AD_ENTRY_RSRC, NULL, &len))
        return len;
    return 0;
}

// --- store -----------------------------------------------------------------

// True when `meta` holds nothing worth persisting.
static bool meta_is_empty(const afp_meta_t *m) {
    if (!m)
        return true;
    if (m->has_dates || m->has_comment)
        return false;
    if (m->has_attrs && (m->attrs & AFP_ATTR_PERSISTED))
        return false;
    if (m->has_finder) {
        for (int i = 0; i < AFP_META_FINDER_SIZE; i++)
            if (m->finder[i])
                return false;
    }
    return true;
}

// Assemble the metadata entry table (everything except the resource fork)
// shared by the buffered and streaming store paths.  Returns the entry count.
static size_t build_meta_entries(const afp_meta_t *meta, ad_entry_t *entries, size_t cap, uint8_t dates[16],
                                 uint8_t macinfo[4]) {
    size_t n = 0;
    if (!meta)
        return 0;
    // Metadata first, forks last, per the AppleSingle/AppleDouble note.
    if (meta->has_comment && meta->comment_len && n < cap) {
        entries[n].id = AD_ENTRY_COMMENT;
        entries[n].bytes = (const uint8_t *)meta->comment;
        entries[n].len = meta->comment_len;
        n++;
    }
    if (meta->has_dates && n < cap) {
        WR_BE32(dates + 0, (uint32_t)ad_date_from_afp(meta->create_date));
        WR_BE32(dates + 4, (uint32_t)ad_date_from_afp(meta->modify_date));
        WR_BE32(dates + 8, (uint32_t)ad_date_from_afp(meta->backup_date));
        WR_BE32(dates + 12, (uint32_t)ad_date_from_afp(meta->access_date));
        entries[n].id = AD_ENTRY_DATES;
        entries[n].bytes = dates;
        entries[n].len = 16;
        n++;
    }
    if (meta->has_finder && n < cap) {
        entries[n].id = AD_ENTRY_FINDER;
        entries[n].bytes = meta->finder;
        entries[n].len = AFP_META_FINDER_SIZE;
        n++;
    }
    if (meta->has_attrs && (meta->attrs & AFP_ATTR_PERSISTED) && n < cap) {
        macinfo[0] = (uint8_t)(meta->attrs >> 8);
        macinfo[1] = (uint8_t)meta->attrs;
        macinfo[2] = 0;
        // Low byte bit 1 is the classic "protected" flag foreign readers use.
        macinfo[3] = (meta->attrs & AFP_ATTR_WRITEINHIBIT) ? 0x02 : 0x00;
        entries[n].id = AD_ENTRY_MACINFO;
        entries[n].bytes = macinfo;
        entries[n].len = 4;
        n++;
    }
    return n;
}

int afp_meta_store(const char *host_path, const afp_meta_t *meta, const uint8_t *rsrc, size_t rsrc_len) {
    char sc[PATH_MAX];
    if (!host_path || !afp_meta_sidecar_path(host_path, sc, sizeof(sc)))
        return -EINVAL;
    if (rsrc_len == 0 && meta_is_empty(meta)) {
        remove(sc); // keep metadata-free files a clean stream
        return 0;
    }

    ad_entry_t entries[5];
    uint8_t dates[16];
    uint8_t macinfo[4];
    size_t n = build_meta_entries(meta, entries, 4, dates, macinfo);
    if (rsrc_len) {
        entries[n].id = AD_ENTRY_RSRC;
        entries[n].bytes = rsrc;
        entries[n].len = rsrc_len;
        n++;
    }

    uint8_t *buf = NULL;
    size_t buf_len = 0;
    int rc = ad_build(false, entries, n, &buf, &buf_len);
    if (rc < 0)
        return rc;
    FILE *f = fopen(sc, "wb");
    if (!f) {
        int e = errno;
        free(buf);
        return e ? -e : -EIO;
    }
    size_t w = fwrite(buf, 1, buf_len, f);
    int cr = fclose(f);
    free(buf);
    return (w == buf_len && cr == 0) ? 0 : -EIO;
}

int afp_meta_store_stream(const char *host_path, const afp_meta_t *meta, FILE *rsrc_src, size_t rsrc_len) {
    char sc[PATH_MAX];
    if (!host_path || !afp_meta_sidecar_path(host_path, sc, sizeof(sc)))
        return -EINVAL;
    if ((rsrc_len == 0 || !rsrc_src) && meta_is_empty(meta)) {
        remove(sc);
        return 0;
    }
    if (!rsrc_src)
        rsrc_len = 0;

    ad_entry_t entries[5];
    uint8_t dates[16];
    uint8_t macinfo[4];
    size_t n_meta = build_meta_entries(meta, entries, 4, dates, macinfo);
    size_t n = n_meta + (rsrc_len ? 1 : 0);

    // Header layout per appledouble.h: magic, version, 16-byte filler, entry
    // count, then one 12-byte descriptor per entry; payloads follow in order.
    size_t hdr_len = 26 + n * 12;
    size_t off = hdr_len;
    uint8_t hdr[26 + 5 * 12];
    memset(hdr, 0, sizeof(hdr));
    WR_BE32(hdr + 0, APPLEDOUBLE_MAGIC);
    WR_BE32(hdr + 4, APPLE_FORK_VERSION);
    hdr[24] = (uint8_t)(n >> 8);
    hdr[25] = (uint8_t)n;
    for (size_t i = 0; i < n_meta; i++) {
        uint8_t *d = hdr + 26 + i * 12;
        WR_BE32(d + 0, entries[i].id);
        WR_BE32(d + 4, (uint32_t)off);
        WR_BE32(d + 8, (uint32_t)entries[i].len);
        off += entries[i].len;
    }
    if (rsrc_len) {
        uint8_t *d = hdr + 26 + n_meta * 12;
        WR_BE32(d + 0, AD_ENTRY_RSRC);
        WR_BE32(d + 4, (uint32_t)off);
        WR_BE32(d + 8, (uint32_t)rsrc_len);
    }

    char tmp[PATH_MAX];
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.gstmp", sc) >= sizeof(tmp))
        return -ENAMETOOLONG;
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return errno ? -errno : -EIO;
    bool ok = fwrite(hdr, 1, hdr_len, f) == hdr_len;
    for (size_t i = 0; ok && i < n_meta; i++)
        ok = entries[i].len == 0 || fwrite(entries[i].bytes, 1, entries[i].len, f) == entries[i].len;
    // Stream the fork in fixed-size chunks so its size never bounds memory.
    uint8_t chunk[64 * 1024];
    size_t left = rsrc_len;
    while (ok && left) {
        size_t want = left < sizeof(chunk) ? left : sizeof(chunk);
        size_t got = fread(chunk, 1, want, rsrc_src);
        if (got == 0)
            break;
        ok = fwrite(chunk, 1, got, f) == got;
        left -= got;
    }
    if (left)
        ok = false; // the source ran short of the declared length
    if (fclose(f) != 0)
        ok = false;
    if (!ok) {
        remove(tmp);
        return -EIO;
    }
    if (rename(tmp, sc) != 0) {
        int e = errno;
        remove(tmp);
        return e ? -e : -EIO;
    }
    return 0;
}

size_t afp_meta_copy_rsrc(const char *host_path, FILE *dst) {
    char sc[PATH_MAX];
    if (!host_path || !dst || !afp_meta_sidecar_path(host_path, sc, sizeof(sc)))
        return 0;
    uint8_t hdr[26 + AD_MAX_ENTRIES * 12];
    uint32_t n = read_entry_table(sc, hdr, sizeof(hdr), NULL);
    uint32_t off = 0, len = 0;
    if (!n || !find_entry_extent(hdr, n, AD_ENTRY_RSRC, &off, &len) || len == 0)
        return 0;
    FILE *f = fopen(sc, "rb");
    if (!f)
        return 0;
    size_t copied = 0;
    if (fseek(f, (long)off, SEEK_SET) == 0) {
        uint8_t chunk[64 * 1024];
        size_t left = len;
        while (left) {
            size_t want = left < sizeof(chunk) ? left : sizeof(chunk);
            size_t got = fread(chunk, 1, want, f);
            if (!got)
                break;
            if (fwrite(chunk, 1, got, dst) != got)
                break;
            copied += got;
            left -= got;
        }
    }
    fclose(f);
    return copied;
}

int afp_meta_update(const char *host_path, const afp_meta_t *meta) {
    uint32_t rsrc_len = afp_meta_rsrc_len(host_path);
    if (rsrc_len == 0)
        return afp_meta_store(host_path, meta, NULL, 0);
    // Park the existing fork in a temp file and stream it back, so a metadata
    // edit never has to hold a multi-megabyte fork in memory.
    FILE *tmp = tmpfile();
    if (!tmp)
        return -EIO;
    size_t copied = afp_meta_copy_rsrc(host_path, tmp);
    rewind(tmp);
    int rc = afp_meta_store_stream(host_path, meta, tmp, copied);
    fclose(tmp);
    return rc;
}
