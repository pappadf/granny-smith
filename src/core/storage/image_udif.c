// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_udif.c
// UDIF 'koly' trailer + 'mish' block-map parser and chunk decoder — see
// image_udif.h.

#include "image_udif.h"
#include "common.h"
#include "udif_layout.h"

#include "codec.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A 16 MB plist is already absurd for a block map; refuse anything larger
// rather than let a corrupt trailer drive a huge allocation.
#define UDIF_XML_MAX (16u * 1024u * 1024u)

bool udif_detect(const uint8_t *trailer, size_t len) {
    if (!trailer || len < UDIF_TRAILER_SIZE)
        return false;
    if (memcmp(trailer + KOLY_OFF_SIGNATURE, KOLY_SIGNATURE_STR, 4) != 0)
        return false;
    // Version and header size are the two fields that separate a real trailer
    // from four incidental bytes of payload that happen to spell 'koly'.
    return RD_BE32(trailer + KOLY_OFF_VERSION) == KOLY_VERSION &&
           RD_BE32(trailer + KOLY_OFF_HEADER_SIZE) == UDIF_TRAILER_SIZE;
}

int udif_parse_trailer(const uint8_t *trailer, size_t len, udif_trailer_t *out) {
    if (!trailer || !out || len < UDIF_TRAILER_SIZE)
        return -EINVAL;
    if (!udif_detect(trailer, len))
        return -EINVAL;

    memset(out, 0, sizeof(*out));
    out->data_fork_offset = RD_BE64(trailer + KOLY_OFF_DATA_OFFSET);
    out->data_fork_length = RD_BE64(trailer + KOLY_OFF_DATA_LENGTH);
    out->xml_offset = RD_BE64(trailer + KOLY_OFF_XML_OFFSET);
    out->xml_length = RD_BE64(trailer + KOLY_OFF_XML_LENGTH);
    out->sectors = RD_BE64(trailer + KOLY_OFF_SECTORS);
    out->segment_count = RD_BE32(trailer + KOLY_OFF_SEGMENT_COUNT);
    out->checksum_type = RD_BE32(trailer + KOLY_OFF_CHECKSUM_TYPE);
    out->checksum = RD_BE32(trailer + KOLY_OFF_CHECKSUM);

    // A spanned image keeps later chunks in sibling .dmgpart files we have no
    // way to reach from one path; say so rather than decode a truncated disk.
    if (out->segment_count > 1)
        return -ENOTSUP;
    // The block map is mandatory; without it there is nothing to decode.
    if (out->xml_length == 0 || out->xml_length > UDIF_XML_MAX || out->sectors == 0)
        return -EINVAL;
    // The decoded image is pre-extended to sectors * 512 before anything is
    // decoded, so an unbounded count asks for an arbitrarily large scratch
    // file.  Bound it by what the storage layer can open
    // at all: 2^32 blocks.
    if (out->sectors > UINT32_MAX)
        return -EFBIG;
    return 0;
}

// Decode Base64 into a malloc'd buffer, ignoring whitespace (the plist wraps
// every blob across many lines).  Returns NULL on a malformed blob.
static uint8_t *base64_decode(const char *src, size_t src_len, size_t *out_len) {
    static const int8_t rev[256] = {
        ['A'] = 1,  ['B'] = 2,  ['C'] = 3,  ['D'] = 4,  ['E'] = 5,  ['F'] = 6,  ['G'] = 7,  ['H'] = 8,
        ['I'] = 9,  ['J'] = 10, ['K'] = 11, ['L'] = 12, ['M'] = 13, ['N'] = 14, ['O'] = 15, ['P'] = 16,
        ['Q'] = 17, ['R'] = 18, ['S'] = 19, ['T'] = 20, ['U'] = 21, ['V'] = 22, ['W'] = 23, ['X'] = 24,
        ['Y'] = 25, ['Z'] = 26, ['a'] = 27, ['b'] = 28, ['c'] = 29, ['d'] = 30, ['e'] = 31, ['f'] = 32,
        ['g'] = 33, ['h'] = 34, ['i'] = 35, ['j'] = 36, ['k'] = 37, ['l'] = 38, ['m'] = 39, ['n'] = 40,
        ['o'] = 41, ['p'] = 42, ['q'] = 43, ['r'] = 44, ['s'] = 45, ['t'] = 46, ['u'] = 47, ['v'] = 48,
        ['w'] = 49, ['x'] = 50, ['y'] = 51, ['z'] = 52, ['0'] = 53, ['1'] = 54, ['2'] = 55, ['3'] = 56,
        ['4'] = 57, ['5'] = 58, ['6'] = 59, ['7'] = 60, ['8'] = 61, ['9'] = 62, ['+'] = 63, ['/'] = 64,
    };

    uint8_t *out = (uint8_t *)malloc(src_len / 4 * 3 + 3);
    if (!out)
        return NULL;
    size_t n = 0;
    uint32_t acc = 0;
    int nbits = 0;
    for (size_t i = 0; i < src_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '=')
            break; // padding: whatever is left in `acc` is discarded
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ')
            continue;
        int v = rev[c];
        if (v == 0) { // not a Base64 alphabet character
            free(out);
            return NULL;
        }
        acc = (acc << 6) | (uint32_t)(v - 1);
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            out[n++] = (uint8_t)(acc >> nbits);
        }
    }
    *out_len = n;
    return out;
}

// Find `needle` in the [from, end) slice of a non-NUL-terminated buffer.
static const char *find_in(const char *from, const char *end, const char *needle) {
    size_t nlen = strlen(needle);
    if ((size_t)(end - from) < nlen)
        return NULL;
    for (const char *p = from; p <= end - nlen; p++)
        if (memcmp(p, needle, nlen) == 0)
            return p;
    return NULL;
}

bool udif_plist_value(const char *xml, size_t len, const char *key, char *dst, size_t cap) {
    if (!xml || !key || !dst || cap == 0)
        return false;
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "<key>%s</key>", key);
    const char *end = xml + len;
    const char *k = find_in(xml, end, pattern);
    if (!k)
        return false;
    // The value is the character data of the element after the key: skip to
    // the end of its opening tag.
    const char *p = memchr(k + strlen(pattern), '>', (size_t)(end - k - strlen(pattern)));
    if (!p)
        return false;
    p++;
    size_t n = 0;
    while (p < end && *p != '<' && n + 1 < cap) {
        if (*p == '&') {
            // The three entities a plist writer escapes; any other reads as '?'.
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
        // Only a human-readable label, so silent truncation is harmless.
        dst[n++] = *p++;
    }
    dst[n] = '\0';
    return true;
}

// Parse one Base64-decoded 'mish' blob into `t`.  0 / negative errno.
static int parse_mish(const uint8_t *b, size_t len, udif_table_t *t) {
    if (len < MISH_HEADER_SIZE)
        return -EINVAL;
    if (RD_BE32(b + MISH_OFF_SIGNATURE) != MISH_SIGNATURE || RD_BE32(b + MISH_OFF_VERSION) != MISH_VERSION)
        return -EINVAL;
    // Every image seen in the wild stores its chunks at absolute data-fork
    // offsets and leaves this zero; a non-zero base would silently shift every
    // read, so refuse rather than guess whether it is meant to be added.
    if (RD_BE64(b + MISH_OFF_DATA_OFFSET) != 0)
        return -ENOTSUP;

    t->base_sector = RD_BE64(b + MISH_OFF_SECTOR);
    t->sectors = RD_BE64(b + MISH_OFF_SECTOR_COUNT);
    t->checksum_type = RD_BE32(b + MISH_OFF_CHECKSUM_TYPE);
    t->checksum = RD_BE32(b + MISH_OFF_CHECKSUM);

    // NB: the u32 at 0x24 is the blkx resource ID, not a descriptor count —
    // the count lives at 0xC8, immediately before the entry array.
    uint32_t n_entries = RD_BE32(b + MISH_OFF_CHUNK_COUNT);
    if ((uint64_t)n_entries * MISH_ENTRY_SIZE > (uint64_t)(len - MISH_HEADER_SIZE))
        return -EINVAL;
    if (n_entries == 0)
        return 0; // legal but empty: nothing to decode for this partition

    udif_chunk_t *chunks = (udif_chunk_t *)calloc(n_entries, sizeof(udif_chunk_t));
    if (!chunks)
        return -ENOMEM;

    size_t nc = 0;
    for (uint32_t i = 0; i < n_entries; i++) {
        const uint8_t *e = b + MISH_HEADER_SIZE + (size_t)i * MISH_ENTRY_SIZE;
        uint32_t type = RD_BE32(e + MISH_ENTRY_OFF_TYPE);
        if (type == UDIF_CHUNK_END)
            break;
        if (type == UDIF_CHUNK_COMMENT)
            continue; // "+beg"/"+end" markers cover no sectors
        chunks[nc].type = type;
        chunks[nc].sector = RD_BE64(e + MISH_ENTRY_OFF_SECTOR);
        chunks[nc].count = RD_BE64(e + MISH_ENTRY_OFF_COUNT);
        chunks[nc].offset = RD_BE64(e + MISH_ENTRY_OFF_OFFSET);
        chunks[nc].length = RD_BE64(e + MISH_ENTRY_OFF_LENGTH);
        // A chunk must stay inside the sector run its own table declares --
        // checked without an addition that 64-bit values could wrap.
        if (chunks[nc].count > t->sectors || chunks[nc].sector > t->sectors - chunks[nc].count) {
            free(chunks);
            return -EINVAL;
        }
        nc++;
    }
    t->chunks = chunks;
    t->n_chunks = nc;
    return 0;
}

int udif_parse_blkx(const uint8_t *xml, size_t xml_len, udif_map_t **out) {
    if (!xml || !out || xml_len == 0)
        return -EINVAL;
    *out = NULL;

    const char *text = (const char *)xml;
    const char *end = text + xml_len;

    // Locate the blkx array.  Its entries are flat dicts of strings and one
    // <data> blob — no nested arrays — so the first </array> closes it.  Every
    // blob is separately validated for the 'mish' magic below, so a mis-scan
    // fails loudly instead of yielding a plausible-looking map.
    const char *key = find_in(text, end, "<key>blkx</key>");
    if (!key)
        return -ENOENT;
    const char *arr = find_in(key, end, "<array>");
    if (!arr)
        return -EINVAL;
    const char *arr_end = find_in(arr, end, "</array>");
    if (!arr_end)
        return -EINVAL;

    udif_map_t *m = (udif_map_t *)calloc(1, sizeof(*m));
    if (!m)
        return -ENOMEM;

    int rc = 0;
    for (const char *p = arr; p < arr_end;) {
        const char *dict = find_in(p, arr_end, "<dict>");
        if (!dict)
            break;
        const char *dict_end = find_in(dict, arr_end, "</dict>");
        if (!dict_end) {
            rc = -EINVAL;
            break;
        }
        p = dict_end + strlen("</dict>");

        const char *data = find_in(dict, dict_end, "<data>");
        if (!data)
            continue; // an entry without a blob is not a block table
        data += strlen("<data>");
        const char *data_end = find_in(data, dict_end, "</data>");
        if (!data_end) {
            rc = -EINVAL;
            break;
        }

        size_t blob_len = 0;
        uint8_t *blob = base64_decode(data, (size_t)(data_end - data), &blob_len);
        if (!blob) {
            rc = -EINVAL;
            break;
        }

        udif_table_t *grown = (udif_table_t *)realloc(m->tables, (m->n_tables + 1) * sizeof(udif_table_t));
        if (!grown) {
            free(blob);
            rc = -ENOMEM;
            break;
        }
        m->tables = grown;
        udif_table_t *t = &m->tables[m->n_tables];
        memset(t, 0, sizeof(*t));
        // CFName carries the readable partition label; older writers set only
        // Name.  Either way this is diagnostics, never decode input.
        // Entry dicts list Attributes / CFName / Data / ID / Name in that
        // order, so the key is matched, not the first <string> taken.
        size_t dict_len = (size_t)(dict_end - dict);
        if (!udif_plist_value(dict, dict_len, "CFName", t->name, sizeof(t->name)))
            udif_plist_value(dict, dict_len, "Name", t->name, sizeof(t->name));

        rc = parse_mish(blob, blob_len, t);
        free(blob);
        if (rc != 0)
            break;
        m->n_tables++;

        // The decoded image spans up to the far edge of the last table.
        uint64_t top = t->base_sector + t->sectors;
        if (top > m->sectors)
            m->sectors = top;
    }

    if (rc == 0 && m->n_tables == 0)
        rc = -ENOENT; // a blkx array with no usable block table
    if (rc != 0) {
        udif_map_free(m);
        return rc;
    }
    *out = m;
    return 0;
}

void udif_map_free(udif_map_t *m) {
    if (!m)
        return;
    for (size_t i = 0; i < m->n_tables; i++)
        free(m->tables[i].chunks);
    free(m->tables);
    free(m);
}

int udif_chunk_codec(uint32_t type) {
    switch (type) {
    case UDIF_CHUNK_ADC:
        return CODEC_ADC;
    case UDIF_CHUNK_ZLIB:
        return CODEC_ZLIB;
    case UDIF_CHUNK_BZ2:
        return CODEC_BZIP2;
    case UDIF_CHUNK_LZFSE:
        return CODEC_LZFSE;
    case UDIF_CHUNK_LZMA:
        return CODEC_XZ;
    default:
        return -1;
    }
}

int udif_decode_chunk(const udif_chunk_t *chunk, const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len) {
    if (!chunk || !dst)
        return -EINVAL;
    size_t need = (size_t)chunk->count * UDIF_SECTOR_SIZE;
    if (dst_len < need)
        return -EINVAL;

    switch (chunk->type) {
    case UDIF_CHUNK_ZERO:
    case UDIF_CHUNK_IGNORE:
        memset(dst, 0, need);
        return 0;
    case UDIF_CHUNK_RAW:
        if (!src || src_len < need)
            return -EINVAL;
        memcpy(dst, src, need);
        return 0;
    default:
        break;
    }
    int codec = udif_chunk_codec(chunk->type);
    if (codec < 0 || !src)
        return -EINVAL;
    int64_t got = codec_decode((codec_id_t)codec, src, src_len, dst, need);
    if (got == CODEC_ERR_UNSUPPORTED)
        return -ENOTSUP;
    if (got == CODEC_ERR_NOMEM)
        return -ENOMEM;
    // The chunk's sector count states the exact decoded size, so a short or
    // long result means the stream and the map disagree.
    return got == (int64_t)need ? 0 : -EINVAL;
}
