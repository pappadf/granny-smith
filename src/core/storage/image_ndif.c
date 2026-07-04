// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_ndif.c
// NDIF 'bcem' block-map parser + chunk decoder — see image_ndif.h.

#include "image_ndif.h"

#include "adc.h"
#include "resource_fork.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define NDIF_SECTOR_SIZE 512u
// 'bcem' resource, ID 128 (Aaru: NDIF_RESOURCE / NDIF_RESOURCEID).
#define BCEM_RESOURCE_ID 128
// Header size preceding the 12-byte chunk-descriptor array.
#define BCEM_HEADER_SIZE 0x80
#define BCEM_ENTRY_SIZE  12

// Header field offsets (big-endian), verified against real images.
#define BCEM_OFF_NAME       0x04 // Str63 (length byte + up to 63 chars)
#define BCEM_OFF_SECTORS    0x44
#define BCEM_OFF_DATAOFFSET 0x4C
#define BCEM_OFF_CRC        0x50
#define BCEM_OFF_CHUNKS     0x7C

static const uint8_t BCEM_TYPE[4] = {'b', 'c', 'e', 'm'};

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Locate the 'bcem' resource bytes in a parsed fork.  Prefers ID 128, then
// falls back to the first 'bcem' present.  On success returns 0 and sets
// *bytes (pointing into the caller's `rfork` buffer, which must outlive use)
// and *size; also returns the parsed rfork_t via *rf_out (caller frees).
static int locate_bcem(const uint8_t *rfork, size_t rfork_len, const uint8_t **bytes, size_t *size, rfork_t **rf_out) {
    const char *errmsg = NULL;
    rfork_t *rf = rfork_parse(rfork, rfork_len, &errmsg);
    if (!rf)
        return -EINVAL;
    const uint8_t *b = NULL;
    size_t sz = 0;
    if (rfork_lookup(rf, BCEM_TYPE, BCEM_RESOURCE_ID, &b, &sz, NULL, NULL) < 0) {
        if (rfork_num_resources(rf, BCEM_TYPE) == 0) {
            rfork_free(rf);
            return -ENOENT;
        }
        int16_t id = rfork_id_at(rf, BCEM_TYPE, 0);
        if (rfork_lookup(rf, BCEM_TYPE, id, &b, &sz, NULL, NULL) < 0) {
            rfork_free(rf);
            return -ENOENT;
        }
    }
    *bytes = b;
    *size = sz;
    *rf_out = rf;
    return 0;
}

bool ndif_detect(const uint8_t *rfork, size_t rfork_len) {
    if (!rfork || rfork_len < 16)
        return false;
    const uint8_t *b = NULL;
    size_t sz = 0;
    rfork_t *rf = NULL;
    if (locate_bcem(rfork, rfork_len, &b, &sz, &rf) != 0)
        return false;
    bool ok = (sz >= BCEM_HEADER_SIZE);
    rfork_free(rf);
    return ok;
}

int ndif_parse(const uint8_t *rfork, size_t rfork_len, ndif_map_t **out) {
    if (!rfork || !out)
        return -EINVAL;
    *out = NULL;

    const uint8_t *b = NULL;
    size_t sz = 0;
    rfork_t *rf = NULL;
    int rc = locate_bcem(rfork, rfork_len, &b, &sz, &rf);
    if (rc)
        return rc;
    if (sz < BCEM_HEADER_SIZE) {
        rfork_free(rf);
        return -EINVAL;
    }

    uint32_t sectors = rd32(b + BCEM_OFF_SECTORS);
    uint32_t data_offset = rd32(b + BCEM_OFF_DATAOFFSET);
    uint32_t crc = rd32(b + BCEM_OFF_CRC);
    uint32_t n_entries = rd32(b + BCEM_OFF_CHUNKS);

    // Bounds: the descriptor array must fit within the resource.
    if (n_entries == 0 || (uint64_t)n_entries * BCEM_ENTRY_SIZE > (uint64_t)(sz - BCEM_HEADER_SIZE)) {
        rfork_free(rf);
        return -EINVAL;
    }

    ndif_map_t *m = calloc(1, sizeof(*m));
    ndif_chunk_t *chunks = calloc(n_entries, sizeof(ndif_chunk_t));
    if (!m || !chunks) {
        free(m);
        free(chunks);
        rfork_free(rf);
        return -ENOMEM;
    }

    m->sectors = sectors;
    m->crc = crc;
    uint8_t nlen = b[BCEM_OFF_NAME];
    if (nlen > 63)
        nlen = 63;
    memcpy(m->volume_name, b + BCEM_OFF_NAME + 1, nlen);
    m->volume_name[nlen] = '\0';

    // Read raw descriptors first; the per-chunk sector count derives from the
    // next descriptor's starting sector (or the image total for the last one).
    const uint8_t *arr = b + BCEM_HEADER_SIZE;
    size_t nc = 0;
    for (uint32_t i = 0; i < n_entries; i++) {
        const uint8_t *e = arr + (size_t)i * BCEM_ENTRY_SIZE;
        uint32_t word = rd32(e);
        uint8_t type = (uint8_t)(word & 0xFF);
        if (type == NDIF_CHUNK_END)
            break;
        uint32_t sector = word >> 8;
        uint32_t next_sector = sectors;
        if (i + 1 < n_entries) {
            uint32_t nword = rd32(arr + (size_t)(i + 1) * BCEM_ENTRY_SIZE);
            next_sector = nword >> 8;
        }
        chunks[nc].sector = sector;
        chunks[nc].count = (next_sector > sector) ? (next_sector - sector) : 0;
        chunks[nc].type = type;
        chunks[nc].offset = rd32(e + 4) + data_offset;
        chunks[nc].length = rd32(e + 8);
        nc++;
    }

    m->chunks = chunks;
    m->n_chunks = nc;
    rfork_free(rf);
    *out = m;
    return 0;
}

void ndif_map_free(ndif_map_t *m) {
    if (!m)
        return;
    free(m->chunks);
    free(m);
}

int ndif_decode_chunk(const ndif_chunk_t *chunk, const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len) {
    if (!chunk || !dst)
        return -EINVAL;
    size_t need = (size_t)chunk->count * NDIF_SECTOR_SIZE;
    if (dst_len < need)
        return -EINVAL;

    switch (chunk->type) {
    case NDIF_CHUNK_ZERO:
        memset(dst, 0, need);
        return 0;
    case NDIF_CHUNK_COPY:
        if (!src || src_len < need)
            return -EINVAL;
        memcpy(dst, src, need);
        return 0;
    case NDIF_CHUNK_ADC: {
        if (!src)
            return -EINVAL;
        long got = adc_decompress(src, src_len, dst, need);
        if (got < 0)
            return -EINVAL;
        // A well-formed chunk decodes to exactly `need` bytes; zero-pad any
        // short tail defensively so callers always get a full sector run.
        if ((size_t)got < need)
            memset(dst + got, 0, need - (size_t)got);
        return 0;
    }
    default:
        return -EINVAL; // KenCode / RLE / LZH / StuffIt not implemented
    }
}
