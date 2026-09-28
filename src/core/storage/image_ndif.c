// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_ndif.c
// NDIF 'bcem' block-map parser + chunk decoder — see image_ndif.h.

#include "image_ndif.h"
#include "common.h"

#include "adc.h"
#include "resource_fork.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

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

    uint32_t sectors = RD_BE32(b + BCEM_OFF_SECTORS);
    uint32_t data_offset = RD_BE32(b + BCEM_OFF_DATAOFFSET);
    uint32_t crc = RD_BE32(b + BCEM_OFF_CRC);
    uint32_t n_entries = RD_BE32(b + BCEM_OFF_CHUNKS);

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
        uint32_t word = RD_BE32(e);
        uint8_t type = (uint8_t)(word & 0xFF);
        if (type == NDIF_CHUNK_END)
            break;
        uint32_t sector = word >> 8;
        uint32_t next_sector = sectors;
        if (i + 1 < n_entries) {
            uint32_t nword = RD_BE32(arr + (size_t)(i + 1) * BCEM_ENTRY_SIZE);
            next_sector = nword >> 8;
        }
        chunks[nc].sector = sector;
        chunks[nc].count = (next_sector > sector) ? (next_sector - sector) : 0;
        chunks[nc].type = type;
        chunks[nc].offset = RD_BE32(e + 4) + data_offset;
        chunks[nc].length = RD_BE32(e + 8);
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

// ---- Materialising a whole image ------------------------------------------

// Uncompressed chunks are copied through in slices of this size, so they
// are never buffered whole however many sectors they cover.
#define NDIF_COPY_SLICE (64u * 1024u)

// Seek to `sector` of `out` in off_t: a long is 32 bits on wasm32, and a
// sector past 4 Mi (2 GiB) would overflow it.
static int seek_sector(FILE *out, uint64_t sector) {
    return fseeko(out, (off_t)sector * NDIF_SECTOR_SIZE, SEEK_SET) == 0 ? 0 : -EIO;
}

static int copy_chunk(const ndif_chunk_t *c, ndif_read_fn read, void *ctx, FILE *out) {
    uint64_t remaining = (uint64_t)c->count * NDIF_SECTOR_SIZE;
    if (c->length < remaining)
        return -EINVAL; // the map promises more sectors than the fork holds
    if (seek_sector(out, c->sector) != 0)
        return -EIO;
    uint8_t buf[NDIF_COPY_SLICE];
    uint64_t src = c->offset;
    while (remaining) {
        size_t n = remaining < sizeof(buf) ? (size_t)remaining : sizeof(buf);
        int rc = read(ctx, src, buf, n);
        if (rc != 0)
            return rc;
        if (fwrite(buf, 1, n, out) != n)
            return -EIO;
        src += n;
        remaining -= n;
    }
    return 0;
}

static int decode_chunk_to(const ndif_chunk_t *c, ndif_read_fn read, void *ctx, FILE *out) {
    uint64_t need = (uint64_t)c->count * NDIF_SECTOR_SIZE;
    if (need > NDIF_MAX_CHUNK_BYTES || c->length > NDIF_MAX_CHUNK_BYTES)
        return -EFBIG;
    uint8_t *cbuf = malloc(c->length ? c->length : 1);
    uint8_t *dbuf = malloc((size_t)need);
    int rc = 0;
    if (!cbuf || !dbuf)
        rc = -ENOMEM;
    else if (c->length && (rc = read(ctx, c->offset, cbuf, c->length)) != 0)
        ;
    else if (ndif_decode_chunk(c, cbuf, c->length, dbuf, (size_t)need) != 0)
        rc = -EINVAL;
    else if (seek_sector(out, c->sector) != 0 || fwrite(dbuf, 1, (size_t)need, out) != need)
        rc = -EIO;
    free(cbuf);
    free(dbuf);
    return rc;
}

int ndif_materialize(const ndif_map_t *map, ndif_read_fn read, void *ctx, FILE *out) {
    if (!map || !read || !out)
        return -EINVAL;
    // Pre-extend: zero-fill chunks, and any sectors no chunk covers, then
    // need no writes.  sectors is 32 bits, so this is at most 2 TiB.
    if (ftruncate(fileno(out), (off_t)map->sectors * NDIF_SECTOR_SIZE) != 0)
        return -EIO;
    for (size_t i = 0; i < map->n_chunks; i++) {
        const ndif_chunk_t *c = &map->chunks[i];
        if (c->type == NDIF_CHUNK_ZERO || c->count == 0)
            continue;
        // Every chunk stays inside the image the header declares; checked
        // without an addition that could wrap.
        if (c->count > map->sectors || c->sector > map->sectors - c->count)
            return -EINVAL;
        int rc = (c->type == NDIF_CHUNK_COPY) ? copy_chunk(c, read, ctx, out) : decode_chunk_to(c, read, ctx, out);
        if (rc != 0)
            return rc;
    }
    return 0;
}
