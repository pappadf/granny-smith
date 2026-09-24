// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// macroman.c
// MacRoman -> UTF-8 transcoder. See macroman.h.

#include "macroman.h"

#include <errno.h>
#include <string.h>

// MacRoman codepoints for 0x80..0xFF, from Apple's legacy encoding table.
// Stored as Unicode code points; converted on demand to UTF-8.
static const uint16_t macroman_hi[128] = {
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

void macroman_to_utf8(const uint8_t *src, size_t src_len, char *dst, size_t dst_cap) {
    if (dst_cap == 0)
        return;
    size_t o = 0;
    for (size_t i = 0; i < src_len && o + 1 < dst_cap; i++) {
        uint8_t c = src[i];
        uint32_t cp = (c < 0x80) ? c : macroman_hi[c - 0x80];
        if (cp < 0x80) {
            dst[o++] = (char)cp;
        } else if (cp < 0x800) {
            if (o + 2 >= dst_cap)
                break;
            dst[o++] = (char)(0xC0 | (cp >> 6));
            dst[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if (o + 3 >= dst_cap)
                break;
            dst[o++] = (char)(0xE0 | (cp >> 12));
            dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    dst[o] = '\0';
}

// The MacRoman byte for Unicode code point `cp`, or -1.  A scan of the one
// table: names are short, and a second, inverted table would be a second
// thing to keep in step with the first.
static int macroman_byte(uint32_t cp) {
    if (cp < 0x80)
        return (int)cp;
    for (size_t i = 0; i < 128; i++)
        if (macroman_hi[i] == cp)
            return (int)(0x80 + i);
    return -1;
}

int macroman_from_utf8(const char *utf8, uint8_t *dst, size_t dst_cap) {
    if (!utf8 || !dst)
        return -EINVAL;
    const uint8_t *p = (const uint8_t *)utf8;
    size_t n = 0;
    while (*p) {
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
        int b = macroman_byte(cp);
        if (b < 0 || n == dst_cap)
            return -EINVAL;
        dst[n++] = (uint8_t)b;
    }
    return (int)n;
}

// === Mac file names on a host filesystem (macroman.h) ========================

bool macroman_name_to_host(const uint8_t *mac, size_t len, char *dst, size_t dst_cap) {
    uint8_t swapped[256];
    if (len > sizeof(swapped) || dst_cap == 0)
        return false;
    for (size_t i = 0; i < len; i++) {
        if (mac[i] == 0 || mac[i] == ':')
            return false;
        swapped[i] = (mac[i] == '/') ? ':' : mac[i];
    }
    // Worst case three UTF-8 bytes per MacRoman byte.
    if (len * 3 + 1 > dst_cap) {
        char tmp[256 * 3 + 1];
        macroman_to_utf8(swapped, len, tmp, sizeof(tmp));
        size_t n = strlen(tmp);
        if (n + 1 > dst_cap)
            return false;
        memcpy(dst, tmp, n + 1);
        return true;
    }
    macroman_to_utf8(swapped, len, dst, dst_cap);
    return true;
}

// One code point from UTF-8 at *p, advancing it; -1 on a malformed sequence.
static int32_t utf8_next(const uint8_t **p) {
    const uint8_t *s = *p;
    if (s[0] < 0x80) {
        *p = s + 1;
        return s[0];
    }
    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80 && s[0] >= 0xC2) {
        *p = s + 2;
        return ((int32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
    }
    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        int32_t cp = ((int32_t)(s[0] & 0x0F) << 12) | ((int32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))
            return -1;
        *p = s + 3;
        return cp;
    }
    if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        int32_t cp = ((int32_t)(s[0] & 0x07) << 18) | ((int32_t)(s[1] & 0x3F) << 12) | ((int32_t)(s[2] & 0x3F) << 6) |
                     (s[3] & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF)
            return -1;
        *p = s + 4;
        return cp;
    }
    return -1;
}

static bool utf8_valid(const char *s) {
    const uint8_t *p = (const uint8_t *)s;
    while (*p)
        if (utf8_next(&p) < 0)
            return false;
    return true;
}

// Base letter + combining mark -> the precomposed letter, for every accented
// letter MacRoman has.  Enough to read the names macOS writes decomposed.
static const struct {
    uint8_t base;
    uint16_t mark;
    uint16_t composed;
} k_compose[] = {
    {'A', 0x0300, 0x00C0},
    {'A', 0x0301, 0x00C1},
    {'A', 0x0302, 0x00C2},
    {'A', 0x0303, 0x00C3},
    {'A', 0x0308, 0x00C4},
    {'A', 0x030A, 0x00C5},
    {'C', 0x0327, 0x00C7},
    {'E', 0x0300, 0x00C8},
    {'E', 0x0301, 0x00C9},
    {'E', 0x0302, 0x00CA},
    {'E', 0x0308, 0x00CB},
    {'I', 0x0300, 0x00CC},
    {'I', 0x0301, 0x00CD},
    {'I', 0x0302, 0x00CE},
    {'I', 0x0308, 0x00CF},
    {'N', 0x0303, 0x00D1},
    {'O', 0x0300, 0x00D2},
    {'O', 0x0301, 0x00D3},
    {'O', 0x0302, 0x00D4},
    {'O', 0x0303, 0x00D5},
    {'O', 0x0308, 0x00D6},
    {'U', 0x0300, 0x00D9},
    {'U', 0x0301, 0x00DA},
    {'U', 0x0302, 0x00DB},
    {'U', 0x0308, 0x00DC},
    {'Y', 0x0308, 0x0178},
    {'a', 0x0300, 0x00E0},
    {'a', 0x0301, 0x00E1},
    {'a', 0x0302, 0x00E2},
    {'a', 0x0303, 0x00E3},
    {'a', 0x0308, 0x00E4},
    {'a', 0x030A, 0x00E5},
    {'c', 0x0327, 0x00E7},
    {'e', 0x0300, 0x00E8},
    {'e', 0x0301, 0x00E9},
    {'e', 0x0302, 0x00EA},
    {'e', 0x0308, 0x00EB},
    {'i', 0x0300, 0x00EC},
    {'i', 0x0301, 0x00ED},
    {'i', 0x0302, 0x00EE},
    {'i', 0x0308, 0x00EF},
    {'n', 0x0303, 0x00F1},
    {'o', 0x0300, 0x00F2},
    {'o', 0x0301, 0x00F3},
    {'o', 0x0302, 0x00F4},
    {'o', 0x0303, 0x00F5},
    {'o', 0x0308, 0x00F6},
    {'u', 0x0300, 0x00F9},
    {'u', 0x0301, 0x00FA},
    {'u', 0x0302, 0x00FB},
    {'u', 0x0308, 0x00FC},
    {'y', 0x0308, 0x00FF},
};

static int32_t compose(int32_t base, int32_t mark) {
    for (size_t i = 0; i < sizeof(k_compose) / sizeof(k_compose[0]); i++)
        if (k_compose[i].base == base && k_compose[i].mark == mark)
            return k_compose[i].composed;
    return -1;
}

int macroman_name_from_host(const char *host, uint8_t *dst, size_t dst_cap) {
    if (!host || !dst)
        return -1;
    size_t n = 0;
    if (!utf8_valid(host)) {
        // Raw MacRoman, as the server wrote it before it transcoded.
        for (const uint8_t *p = (const uint8_t *)host; *p; p++) {
            if (n == dst_cap)
                return -1;
            dst[n++] = (*p == ':') ? '/' : *p;
        }
        return (int)n;
    }
    const uint8_t *p = (const uint8_t *)host;
    while (*p) {
        int32_t cp = utf8_next(&p);
        if (*p) {
            const uint8_t *q = p;
            int32_t mark = utf8_next(&q);
            int32_t c = compose(cp, mark);
            if (c >= 0) {
                cp = c;
                p = q;
            }
        }
        if (cp == ':')
            cp = '/';
        int b = (cp >= 0 && cp <= 0xFFFF) ? macroman_byte((uint32_t)cp) : -1;
        if (b <= 0 || n == dst_cap)
            return -1;
        dst[n++] = (uint8_t)b;
    }
    return (int)n;
}
