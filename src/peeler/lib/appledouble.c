// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appledouble.c — see appledouble.h.

#include "appledouble.h"
#include "internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

// Fixed header geometry.
#define AD_HDR_FIXED    26u // magic + version + 16 filler + numEntries
#define AD_DESC_SIZE    12u // entryID + offset + length
#define AD_FILLER_OFF   8u // 16-byte filler starts here
#define AD_NENTRIES_OFF 24u

static bool magic_ok(uint32_t magic) {
    return magic == APPLEDOUBLE_MAGIC || magic == APPLESINGLE_MAGIC;
}

// Validate the header + descriptor table without recording entries.  Shared by
// ad_detect and (as a precondition) ad_parse.
static bool header_valid(const uint8_t *buf, size_t len, uint16_t *n_out) {
    if (!buf || len < AD_HDR_FIXED)
        return false;
    if (!magic_ok(rd32be(buf)))
        return false;
    if (rd32be(buf + 4) != APPLE_FORK_VERSION)
        return false;
    uint16_t n = rd16be(buf + AD_NENTRIES_OFF);
    if (n == 0 || n > AD_MAX_ENTRIES)
        return false;
    // Descriptor table must fit.
    if (len < (size_t)AD_HDR_FIXED + (size_t)n * AD_DESC_SIZE)
        return false;
    // Every descriptor's payload window must fall inside the buffer, and each
    // entry id must be non-zero (id 0 is invalid per the spec).
    for (uint16_t i = 0; i < n; i++) {
        const uint8_t *d = buf + AD_HDR_FIXED + (size_t)i * AD_DESC_SIZE;
        uint32_t id = rd32be(d);
        uint32_t off = rd32be(d + 4);
        uint32_t elen = rd32be(d + 8);
        if (id == 0)
            return false;
        if ((uint64_t)off + elen > (uint64_t)len)
            return false;
    }
    if (n_out)
        *n_out = n;
    return true;
}

bool ad_detect(const uint8_t *buf, size_t len) {
    return header_valid(buf, len, NULL);
}

int ad_parse(const uint8_t *buf, size_t len, ad_file_t *out) {
    uint16_t n = 0;
    if (!out || !header_valid(buf, len, &n))
        return -EINVAL;

    memset(out, 0, sizeof(*out));
    out->magic = rd32be(buf);
    out->version = rd32be(buf + 4);
    out->n_entries = n;
    for (uint16_t i = 0; i < n; i++) {
        const uint8_t *d = buf + AD_HDR_FIXED + (size_t)i * AD_DESC_SIZE;
        uint32_t id = rd32be(d);
        uint32_t off = rd32be(d + 4);
        uint32_t elen = rd32be(d + 8);
        const uint8_t *bytes = elen ? buf + off : NULL;
        out->entries[i].id = id;
        out->entries[i].bytes = bytes;
        out->entries[i].len = elen;
        switch (id) {
        case AD_ENTRY_DATA:
            out->data = bytes;
            out->data_len = elen;
            break;
        case AD_ENTRY_RSRC:
            out->rsrc = bytes;
            out->rsrc_len = elen;
            break;
        case AD_ENTRY_FINDER:
            out->finder = bytes;
            out->finder_len = elen;
            break;
        default:
            break;
        }
    }
    return 0;
}

// Arguments both builders check: a non-empty table of real ids, with no data
// fork in an AppleDouble header.
static int entries_valid(bool applesingle, const ad_entry_t *entries, size_t n_entries) {
    if (!entries || n_entries == 0 || n_entries > AD_MAX_ENTRIES)
        return -EINVAL;
    for (size_t i = 0; i < n_entries; i++) {
        if (entries[i].id == 0)
            return -EINVAL;
        if (!applesingle && entries[i].id == AD_ENTRY_DATA)
            return -EINVAL; // AppleDouble header never holds the data fork
    }
    return 0;
}

long ad_build_header(bool applesingle, const ad_entry_t *entries, size_t n_entries, uint8_t *out, size_t cap) {
    if (!out || entries_valid(applesingle, entries, n_entries) != 0)
        return -EINVAL;
    size_t hdr = (size_t)AD_HDR_FIXED + n_entries * AD_DESC_SIZE;
    if (hdr > cap)
        return -EINVAL;
    memset(out, 0, hdr); // including the 16-byte filler at AD_FILLER_OFF
    wr32be(out, applesingle ? APPLESINGLE_MAGIC : APPLEDOUBLE_MAGIC);
    wr32be(out + 4, APPLE_FORK_VERSION);
    wr16be(out + AD_NENTRIES_OFF, (uint16_t)n_entries);
    size_t payload = hdr;
    for (size_t i = 0; i < n_entries; i++) {
        uint8_t *d = out + AD_HDR_FIXED + i * AD_DESC_SIZE;
        wr32be(d, entries[i].id);
        wr32be(d + 4, (uint32_t)payload);
        wr32be(d + 8, (uint32_t)entries[i].len);
        payload += entries[i].len;
    }
    return (long)hdr;
}

int ad_build(bool applesingle, const ad_entry_t *entries, size_t n_entries, uint8_t **out, size_t *out_len) {
    if (!out || !out_len || entries_valid(applesingle, entries, n_entries) != 0)
        return -EINVAL;
    size_t total = (size_t)AD_HDR_FIXED + n_entries * AD_DESC_SIZE;
    for (size_t i = 0; i < n_entries; i++) {
        if (entries[i].len && !entries[i].bytes)
            return -EINVAL;
        total += entries[i].len;
    }
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf)
        return -ENOMEM;
    long hdr = ad_build_header(applesingle, entries, n_entries, buf, total);
    if (hdr < 0) {
        free(buf);
        return (int)hdr;
    }
    size_t payload = (size_t)hdr;
    for (size_t i = 0; i < n_entries; i++) {
        if (entries[i].len)
            memcpy(buf + payload, entries[i].bytes, entries[i].len);
        payload += entries[i].len;
    }
    *out = buf;
    *out_len = total;
    return 0;
}

int ad_build_sidecar(const uint8_t *rsrc, size_t rsrc_len, const uint8_t *finder, uint8_t **out, size_t *out_len) {
    ad_entry_t entries[2];
    size_t n = 0;
    // Metadata first, resource fork last (per the note's ordering guidance).
    if (finder) {
        entries[n].id = AD_ENTRY_FINDER;
        entries[n].bytes = finder;
        entries[n].len = AD_FINDER_INFO_SIZE;
        n++;
    }
    if (rsrc_len) {
        entries[n].id = AD_ENTRY_RSRC;
        entries[n].bytes = rsrc;
        entries[n].len = rsrc_len;
        n++;
    }
    if (n == 0)
        return -EINVAL;
    return ad_build(false, entries, n, out, out_len);
}
