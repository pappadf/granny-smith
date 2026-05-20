// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// resource_fork.c
// Read-only parser for the classic-Mac resource-fork on-disk format.  See
// resource_fork.h for the format reference.  Two well-documented traps
// the parser handles explicitly:
//   - the on-disk num_types and count fields are stored as "minus one"
//     (so a fork with 0 types stores 0xFFFF, and a single resource of a
//     type stores 0 for that type's count).
//   - the data-offset field in each ref-list entry is a packed 24-bit
//     integer in the low three bytes of a u32; the high byte is attrs.
//
// All field reads are range-checked against the fork-length / map-length
// bounds.  Malformed forks return NULL from rfork_parse with *errmsg set
// to a static string, never abort.

#include "resource_fork.h"

#include "macroman.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Big-endian field readers — the fork buffer is the on-disk layout verbatim.
static uint16_t be_u16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static uint32_t be_u32(const uint8_t *p) {
    return (uint32_t)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}
static int16_t be_i16(const uint8_t *p) {
    return (int16_t)be_u16(p);
}

// One parsed type entry: 4-CC plus a contiguous run of ref-list slots.
typedef struct rfork_type {
    uint8_t cc[4];
    size_t num_resources;
    // Each resource carries: id, data slice, name slice, attrs.  Names are
    // resolved to UTF-8 once at parse time so callers don't repeat the
    // transcoding work per lookup.
    struct {
        int16_t id;
        const uint8_t *bytes;
        size_t size;
        uint8_t attrs;
        char name_utf8[64]; // empty string when name_off == -1
    } *resources;
} rfork_type_t;

struct rfork {
    uint16_t fork_attrs;
    size_t num_types;
    rfork_type_t *types;
};

// Read a Pascal-format name out of the name list and transcode to UTF-8.
// `name_off` is the offset relative to the name-list start; -1 means "no
// name".  Returns true on success.
static bool read_name(const uint8_t *map, size_t map_len, size_t name_list_off, int16_t name_off, char *out,
                      size_t cap) {
    out[0] = '\0';
    if (name_off < 0)
        return true;
    size_t abs = name_list_off + (size_t)(uint16_t)name_off;
    if (abs >= map_len)
        return false;
    uint8_t plen = map[abs];
    if (abs + 1 + plen > map_len)
        return false;
    macroman_to_utf8(map + abs + 1, plen, out, cap);
    return true;
}

rfork_t *rfork_parse(const uint8_t *fork_bytes, size_t fork_len, const char **errmsg_out) {
    const char *err = NULL;
#define FAIL(msg)                                                                                                      \
    do {                                                                                                               \
        err = (msg);                                                                                                   \
        goto fail;                                                                                                     \
    } while (0)

    rfork_t *rf = NULL;

    if (!fork_bytes || fork_len < 16)
        FAIL("fork too small for header");
    uint32_t data_off = be_u32(fork_bytes + 0);
    uint32_t map_off = be_u32(fork_bytes + 4);
    uint32_t data_len = be_u32(fork_bytes + 8);
    uint32_t map_len = be_u32(fork_bytes + 12);
    if ((uint64_t)data_off + data_len > fork_len)
        FAIL("data area out of range");
    if ((uint64_t)map_off + map_len > fork_len)
        FAIL("map area out of range");
    if (map_len < 30)
        FAIL("map area too small");

    const uint8_t *map = fork_bytes + map_off;
    const uint8_t *data = fork_bytes + data_off;
    uint16_t fork_attrs = be_u16(map + 22);
    uint16_t type_list_off = be_u16(map + 24);
    uint16_t name_list_off = be_u16(map + 26);
    if ((uint32_t)type_list_off + 2 > map_len)
        FAIL("type-list offset out of range");
    // name_list_off may equal map_len exactly when no names are stored.
    if ((uint32_t)name_list_off > map_len)
        FAIL("name-list offset out of range");

    rf = calloc(1, sizeof(*rf));
    if (!rf)
        FAIL("out of memory");
    rf->fork_attrs = fork_attrs;

    uint16_t num_types_m1 = be_u16(map + type_list_off);
    // The off-by-one encoding means an empty fork stores 0xFFFF for the
    // num-types word; detect that explicitly so we don't allocate 64K
    // empty type slots.
    if (num_types_m1 == 0xFFFF) {
        rf->num_types = 0;
        if (errmsg_out)
            *errmsg_out = NULL;
        return rf;
    }
    rf->num_types = (size_t)num_types_m1 + 1;

    // Each type entry is 8 bytes; verify the table fits inside the map.
    size_t type_table_end = (size_t)type_list_off + 2 + 8 * rf->num_types;
    if (type_table_end > map_len)
        FAIL("type table out of range");

    rf->types = calloc(rf->num_types, sizeof(rfork_type_t));
    if (!rf->types)
        FAIL("out of memory");

    for (size_t t = 0; t < rf->num_types; t++) {
        const uint8_t *te = map + type_list_off + 2 + 8 * t;
        memcpy(rf->types[t].cc, te + 0, 4);
        uint16_t count_m1 = be_u16(te + 4);
        uint16_t ref_off = be_u16(te + 6);
        size_t num_res = (size_t)count_m1 + 1;
        rf->types[t].num_resources = num_res;

        // Each ref-list entry is 12 bytes; the offset is relative to the
        // start of the type list (i.e., type_list_off, not the entry that
        // owns the ref).
        size_t ref_start = (size_t)type_list_off + ref_off;
        size_t ref_end = ref_start + 12 * num_res;
        if (ref_end > map_len)
            FAIL("ref list out of range");

        rf->types[t].resources = calloc(num_res, sizeof(*rf->types[t].resources));
        if (!rf->types[t].resources)
            FAIL("out of memory");

        for (size_t r = 0; r < num_res; r++) {
            const uint8_t *re = map + ref_start + 12 * r;
            int16_t id = be_i16(re + 0);
            int16_t name_off = be_i16(re + 2);
            uint8_t attrs = re[4];
            // 24-bit data offset in the low 3 bytes of the next u32.
            uint32_t do24 = ((uint32_t)re[5] << 16) | ((uint32_t)re[6] << 8) | (uint32_t)re[7];
            if ((size_t)do24 + 4 > data_len)
                FAIL("resource data offset out of range");
            uint32_t rlen = be_u32(data + do24);
            if ((size_t)do24 + 4 + rlen > data_len)
                FAIL("resource data length out of range");

            rf->types[t].resources[r].id = id;
            rf->types[t].resources[r].attrs = attrs;
            rf->types[t].resources[r].bytes = data + do24 + 4;
            rf->types[t].resources[r].size = rlen;

            if (!read_name(map, map_len, name_list_off, name_off, rf->types[t].resources[r].name_utf8,
                           sizeof(rf->types[t].resources[r].name_utf8))) {
                FAIL("name list out of range");
            }
        }
    }

    if (errmsg_out)
        *errmsg_out = NULL;
    return rf;

fail:
    if (errmsg_out)
        *errmsg_out = err ? err : "corrupt resource fork";
    rfork_free(rf);
    return NULL;
#undef FAIL
}

void rfork_free(rfork_t *rf) {
    if (!rf)
        return;
    if (rf->types) {
        for (size_t t = 0; t < rf->num_types; t++)
            free(rf->types[t].resources);
        free(rf->types);
    }
    free(rf);
}

uint16_t rfork_attrs(const rfork_t *rf) {
    return rf ? rf->fork_attrs : 0;
}

size_t rfork_num_types(const rfork_t *rf) {
    return rf ? rf->num_types : 0;
}

const uint8_t *rfork_type_at(const rfork_t *rf, size_t idx) {
    if (!rf || idx >= rf->num_types)
        return NULL;
    return rf->types[idx].cc;
}

// Linear-scan the type table for the requested 4-CC.  Type counts in
// practice are < 200, so a linear scan is faster than building a hash.
static const rfork_type_t *find_type(const rfork_t *rf, const uint8_t type[4]) {
    if (!rf)
        return NULL;
    for (size_t t = 0; t < rf->num_types; t++) {
        if (memcmp(rf->types[t].cc, type, 4) == 0)
            return &rf->types[t];
    }
    return NULL;
}

size_t rfork_num_resources(const rfork_t *rf, const uint8_t type[4]) {
    const rfork_type_t *t = find_type(rf, type);
    return t ? t->num_resources : 0;
}

int16_t rfork_id_at(const rfork_t *rf, const uint8_t type[4], size_t idx) {
    const rfork_type_t *t = find_type(rf, type);
    if (!t || idx >= t->num_resources)
        return 0;
    return t->resources[idx].id;
}

int rfork_lookup(const rfork_t *rf, const uint8_t type[4], int16_t id, const uint8_t **bytes_out, size_t *size_out,
                 const char **name_out, uint8_t *attrs_out) {
    const rfork_type_t *t = find_type(rf, type);
    if (!t)
        return -ENOENT;
    for (size_t r = 0; r < t->num_resources; r++) {
        if (t->resources[r].id == id) {
            if (bytes_out)
                *bytes_out = t->resources[r].bytes;
            if (size_out)
                *size_out = t->resources[r].size;
            if (name_out)
                *name_out = t->resources[r].name_utf8;
            if (attrs_out)
                *attrs_out = t->resources[r].attrs;
            return 0;
        }
    }
    return -ENOENT;
}

// === Path-component helpers =================================================

void rfork_type_to_path(const uint8_t type[4], char *out, size_t cap) {
    macroman_to_utf8(type, 4, out, cap);
}

// Inverse of rfork_type_to_path: walk the UTF-8 component, decode each
// codepoint, look it up in the MacRoman table (low byte = ASCII, high
// byte = scan).  Reject if the decode produces != 4 MacRoman bytes.
// Mirror of the MacRoman high-byte table from macroman.c, used by the
// inverse transcoder below.  Duplicating the 128 entries here is cheaper
// than exposing the internal table; both stay in sync as a single
// source-of-truth update.
static const uint16_t macroman_hi_lookup[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5,
    0x00E7, 0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4,
    0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC, 0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6,
    0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8, 0x221E, 0x00B1, 0x2264, 0x2265,
    0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8, 0x00BF,
    0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5,
    0x0152, 0x0153, 0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044,
    0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02, 0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
    0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4, 0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9,
    0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

int rfork_type_from_path(const char *path_component, uint8_t out[4]) {
    if (!path_component)
        return -EINVAL;
    const uint8_t *p = (const uint8_t *)path_component;
    size_t out_n = 0;
    while (*p && out_n < 4) {
        uint32_t cp;
        if (p[0] < 0x80) {
            cp = p[0];
            p++;
        } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else {
            return -EINVAL;
        }
        if (cp < 0x80) {
            out[out_n++] = (uint8_t)cp;
            continue;
        }
        // Scan the MacRoman high-byte table for a match.  ~128 entries,
        // amortised on 4 chars per type, is negligible.
        bool found = false;
        for (size_t i = 0; i < 128; i++) {
            if (macroman_hi_lookup[i] == cp) {
                out[out_n++] = (uint8_t)(0x80 + i);
                found = true;
                break;
            }
        }
        if (!found)
            return -EINVAL;
    }
    if (out_n != 4 || *p != '\0')
        return -EINVAL;
    return 0;
}

void rfork_id_to_path(int16_t id, char *out, size_t cap) {
    if (cap == 0)
        return;
    snprintf(out, cap, "%d", (int)id);
}

int rfork_id_from_path(const char *path_component, int16_t *out) {
    if (!path_component || !*path_component)
        return -EINVAL;
    char *end = NULL;
    errno = 0;
    long v = strtol(path_component, &end, 10);
    if (errno != 0 || !end || *end != '\0')
        return -EINVAL;
    if (v < -32768 || v > 32767)
        return -EINVAL;
    if (out)
        *out = (int16_t)v;
    return 0;
}

// Map an attrs byte to its zero-or-more flag names.  Order matters: callers
// inspect the JSON output by name, not bit position.
static const char *attr_name(uint8_t bit) {
    switch (bit) {
    case RFORK_ATTR_SYSHEAP:
        return "sysheap";
    case RFORK_ATTR_PURGEABLE:
        return "purgeable";
    case RFORK_ATTR_LOCKED:
        return "locked";
    case RFORK_ATTR_PROTECTED:
        return "protected";
    case RFORK_ATTR_PRELOAD:
        return "preload";
    case RFORK_ATTR_CHANGED:
        return "changed";
    case RFORK_ATTR_COMPRESSED:
        return "compressed";
    default:
        return NULL;
    }
}

// JSON-escape `src` into `dst`.  Replicates the small subset required for
// resource names: backslash, quote, control chars escaped to \uXXXX.
static int json_escape(const char *src, char *dst, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; src[i]; i++) {
        unsigned char c = (unsigned char)src[i];
        char buf[8];
        size_t need = 0;
        if (c == '"' || c == '\\') {
            need = 2;
            buf[0] = '\\';
            buf[1] = (char)c;
        } else if (c == '\n') {
            need = 2;
            buf[0] = '\\';
            buf[1] = 'n';
        } else if (c == '\r') {
            need = 2;
            buf[0] = '\\';
            buf[1] = 'r';
        } else if (c == '\t') {
            need = 2;
            buf[0] = '\\';
            buf[1] = 't';
        } else if (c < 0x20) {
            need = 6;
            snprintf(buf, sizeof(buf), "\\u%04x", c);
        } else {
            need = 1;
            buf[0] = (char)c;
        }
        if (o + need >= cap)
            return -EINVAL;
        memcpy(dst + o, buf, need);
        o += need;
    }
    if (o >= cap)
        return -EINVAL;
    dst[o] = '\0';
    return (int)o;
}

int rfork_info_format(const char *name, uint8_t attrs, size_t size, char *out, size_t cap) {
    char esc[256];
    if (json_escape(name ? name : "", esc, sizeof(esc)) < 0)
        return -EINVAL;

    int n = snprintf(out, cap, "{\"name\":\"%s\",\"attrs\":[", esc);
    if (n < 0 || (size_t)n >= cap)
        return -EINVAL;
    size_t o = (size_t)n;

    bool first = true;
    static const uint8_t bits[] = {
        RFORK_ATTR_SYSHEAP, RFORK_ATTR_PURGEABLE, RFORK_ATTR_LOCKED,     RFORK_ATTR_PROTECTED,
        RFORK_ATTR_PRELOAD, RFORK_ATTR_CHANGED,   RFORK_ATTR_COMPRESSED,
    };
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        if (attrs & bits[i]) {
            const char *nm = attr_name(bits[i]);
            int w = snprintf(out + o, cap - o, "%s\"%s\"", first ? "" : ",", nm);
            if (w < 0 || (size_t)w >= cap - o)
                return -EINVAL;
            o += (size_t)w;
            first = false;
        }
    }
    // Surface any unknown reserved bits as "reserved_0xNN" entries so callers
    // can still see them without us silently dropping the information.
    uint8_t unknown = attrs & ~0x7F;
    if (unknown) {
        int w = snprintf(out + o, cap - o, "%s\"reserved_0x%02x\"", first ? "" : ",", unknown);
        if (w < 0 || (size_t)w >= cap - o)
            return -EINVAL;
        o += (size_t)w;
    }
    int w = snprintf(out + o, cap - o, "],\"size\":%zu}\n", size);
    if (w < 0 || (size_t)w >= cap - o)
        return -EINVAL;
    o += (size_t)w;
    return (int)o;
}
