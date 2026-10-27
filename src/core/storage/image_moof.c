// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_moof.c
// Applesauce MOOF floppy images: decode the bitstreams to GCR sectors, and
// lay sectors down as bitstreams.  See image_moof.h and
// docs/reference/formats/moof.md.

#include "image_moof.h"

#include "common.h"
#include "crc32.h"
#include "format_registry.h"
#include "log.h"
#include "storage_util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("image")

// ============================================================================
// Layout
// ============================================================================

static const uint8_t moof_magic[8] = {'M', 'O', 'O', 'F', 0xFF, 0x0A, 0x0D, 0x0A};

#define CHUNK_HDR    8 // id + size
#define INFO_SIZE    60 // INFO chunk data
#define TMAP_SIZE    160 // one entry per (track << 1) + side
#define TRK_COUNT    160 // TRK records in TRKS
#define TRK_SIZE     8 // startBlock, blockCount, bitCount
#define TMAP_BLANK   0xFF // no track
#define MOOF_BLOCK   512
#define FIRST_BLOCK  3 // track data starts at byte 1536
#define NUM_TRACKS   80
#define SECTOR_BYTES 512
#define SECTOR_NIBS  699 // 12 tag + 512 data bytes in 6-and-2 GCR
#define CKSUM_NIBS   4 // the 24-bit data checksum
#define GCR_TIMING   16 // optimal bit timing for GCR, in 125 ns ticks (2 µs)
#define SECTOR_RAW   (MOOF_TAG_BYTES + SECTOR_BYTES) // 524

// Fixed chunk positions an Applesauce file uses (the chunk walk finds them
// anyway; these are what a writer emits).
#define INFO_AT 12
#define TMAP_AT 80
#define TRKS_AT 248

// Sectors per track in each of the five speed zones (16 tracks each).
static int zone_sectors(int track) {
    return 12 - track / 16;
}

// ============================================================================
// 6-and-2 GCR
// ============================================================================

// The 64 disk bytes the 6-bit values 0x00-0x3F encode to.
static const uint8_t gcr_encode[64] = {0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6, 0xA7, 0xAB, 0xAC, 0xAD, 0xAE,
                                       0xAF, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE,
                                       0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3, 0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD,
                                       0xDE, 0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xF2,
                                       0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF};

// Disk byte -> 6-bit value, 0xFF for a byte that is no codeword.
static const uint8_t gcr_decode[256] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x01,
    0xFF, 0xFF, 0x02, 0x03, 0xFF, 0x04, 0x05, 0x06, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x08, 0xFF, 0xFF, 0xFF,
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0xFF, 0xFF, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0xFF, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1A, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x1B, 0xFF, 0x1C, 0x1D, 0x1E, 0xFF,
    0xFF, 0xFF, 0x1F, 0xFF, 0xFF, 0x20, 0x21, 0xFF, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0x29, 0x2A, 0x2B, 0xFF, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0xFF, 0xFF, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0xFF, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
};

// Decode 699 data nibbles and 4 checksum nibbles (disk bytes) into the 524
// bytes of a sector.  1 when the checksum matches, 0 when it does not, -1
// when a nibble is no codeword.
static int gcr_denibblize(const uint8_t *nib, uint8_t out[SECTOR_RAW]) {
    uint8_t w[SECTOR_NIBS + CKSUM_NIBS];
    for (int i = 0; i < SECTOR_NIBS + CKSUM_NIBS; i++) {
        w[i] = gcr_decode[nib[i]];
        if (w[i] == 0xFF)
            return -1;
    }
    // Each group is a nibble of high bits, then up to three low-6-bit nibbles.
    uint8_t b1[175], b2[175], b3[175];
    int k = 0;
    for (int g = 0; g < 175; g++) {
        uint8_t hi = w[k++];
        b1[g] = (uint8_t)(w[k++] | ((hi << 2) & 0xC0));
        b2[g] = (uint8_t)(w[k++] | ((hi << 4) & 0xC0));
        b3[g] = g < 174 ? (uint8_t)(w[k++] | ((hi << 6) & 0xC0)) : 0; // the last group has two bytes
    }
    // Undo the rolling three-byte checksum the bytes were XORed with.
    uint32_t c1 = 0, c2 = 0, c3 = 0;
    int n = 0;
    for (int j = 0;; j++) {
        c1 = (c1 & 0xFF) << 1; // rotate c1 left...
        if (c1 & 0x100)
            c1++; // ...bit 7 into bit 0
        uint8_t v = b1[j] ^ (uint8_t)c1;
        c3 += v;
        if (c1 & 0x100) {
            c3++; // the rotate's carry
            c1 &= 0xFF;
        }
        out[n++] = v;
        v = b2[j] ^ (uint8_t)c3;
        c2 += v;
        if (c3 > 0xFF) {
            c2++;
            c3 &= 0xFF;
        }
        out[n++] = v;
        if (n == SECTOR_RAW)
            break;
        v = b3[j] ^ (uint8_t)c2;
        c1 += v;
        if (c2 > 0xFF) {
            c1++;
            c2 &= 0xFF;
        }
        out[n++] = v;
    }
    c1 &= 0xFF, c2 &= 0xFF, c3 &= 0xFF;
    const uint8_t *ck = w + SECTOR_NIBS;
    uint8_t hi = (uint8_t)(((c1 & 0xC0) >> 6) | ((c2 & 0xC0) >> 4) | ((c3 & 0xC0) >> 2));
    return ck[0] == hi && ck[1] == (c3 & 0x3F) && ck[2] == (c2 & 0x3F) && ck[3] == (c1 & 0x3F);
}

// Encode the 524 bytes of a sector as 699 data nibbles and 4 checksum
// nibbles (disk bytes): gcr_denibblize's inverse.
static void gcr_nibblize(const uint8_t in[SECTOR_RAW], uint8_t nib[SECTOR_NIBS + CKSUM_NIBS]) {
    uint8_t b1[175], b2[175], b3[175];
    uint32_t c1 = 0, c2 = 0, c3 = 0;
    int i = 0;
    for (int j = 0;; j++) {
        c1 = (c1 & 0xFF) << 1;
        if (c1 & 0x100)
            c1++;
        uint8_t v = in[i++];
        c3 += v;
        if (c1 & 0x100) {
            c3++;
            c1 &= 0xFF;
        }
        b1[j] = v ^ (uint8_t)c1;
        v = in[i++];
        c2 += v;
        if (c3 > 0xFF) {
            c2++;
            c3 &= 0xFF;
        }
        b2[j] = v ^ (uint8_t)c3;
        if (i == SECTOR_RAW) {
            b3[j] = 0;
            break;
        }
        v = in[i++];
        c1 += v;
        if (c2 > 0xFF) {
            c1++;
            c2 &= 0xFF;
        }
        b3[j] = v ^ (uint8_t)c2;
    }
    c1 &= 0xFF, c2 &= 0xFF, c3 &= 0xFF;
    int k = 0;
    for (int g = 0; g < 175; g++) {
        nib[k++] = gcr_encode[((b1[g] >> 2) & 0x30) | ((b2[g] >> 4) & 0x0C) | ((b3[g] >> 6) & 0x03)];
        nib[k++] = gcr_encode[b1[g] & 0x3F];
        nib[k++] = gcr_encode[b2[g] & 0x3F];
        if (g < 174)
            nib[k++] = gcr_encode[b3[g] & 0x3F];
    }
    nib[k++] = gcr_encode[((c1 & 0xC0) >> 6) | ((c2 & 0xC0) >> 4) | ((c3 & 0xC0) >> 2)];
    nib[k++] = gcr_encode[c3 & 0x3F];
    nib[k++] = gcr_encode[c2 & 0x3F];
    nib[k++] = gcr_encode[c1 & 0x3F];
}

// ============================================================================
// Reader
// ============================================================================

bool moof_detect(const uint8_t *head, size_t len) {
    return head && len >= sizeof moof_magic && memcmp(head, moof_magic, sizeof moof_magic) == 0;
}

// Where the chunks the reader needs are: data offset and size, 0 when absent.
typedef struct {
    size_t info, tmap, trks, flux;
    size_t trks_len;
} moof_chunks_t;

// Walk the chunk list from byte 12, skipping unknown chunks.
static void moof_walk(const uint8_t *f, size_t len, moof_chunks_t *c) {
    memset(c, 0, sizeof *c);
    size_t off = MOOF_HEADER_SIZE;
    while (off + CHUNK_HDR <= len) {
        uint32_t id = RD_BE32(f + off), size = RD_LE32(f + off + 4);
        size_t data = off + CHUNK_HDR;
        if (size > len - data)
            size = (uint32_t)(len - data); // a truncated last chunk: what is there
        if (id == 0x494E464F && !c->info && size >= INFO_SIZE) // 'INFO'
            c->info = data;
        else if (id == 0x544D4150 && !c->tmap && size >= TMAP_SIZE) // 'TMAP'
            c->tmap = data;
        else if (id == 0x54524B53 && !c->trks && size >= TRK_COUNT * TRK_SIZE) { // 'TRKS'
            c->trks = data;
            c->trks_len = size;
        } else if (id == 0x464C5558 && !c->flux && size >= TMAP_SIZE) // 'FLUX'
            c->flux = data;
        off = data + size;
    }
}

// Convert a flux track (A2R-style tick deltas, 125 ns per tick, 255 =
// keep counting) to a bitstream at the GCR cell time.  The bits (MSB first)
// go into a malloc'd *out; the bit count is returned, 0 on failure.
static uint32_t flux_to_bits(const uint8_t *flux, size_t n, uint8_t timing, uint8_t **out) {
    if (!timing)
        timing = GCR_TIMING;
    // At most 255 cells per flux byte: size the buffer from the tick total.
    uint64_t ticks = 0;
    for (size_t i = 0; i < n; i++)
        ticks += flux[i];
    uint64_t max_bits = ticks / timing + n + 8;
    if (max_bits > 8u * 1024u * 1024u)
        return 0;
    uint8_t *bits = calloc(1, (size_t)(max_bits + 7) / 8);
    if (!bits)
        return 0;
    uint32_t pos = 0, acc = 0;
    for (size_t i = 0; i < n; i++) {
        acc += flux[i];
        if (flux[i] == 255)
            continue; // the interval goes on
        uint32_t cells = (acc + timing / 2) / timing; // nearest whole cell count
        if (cells == 0)
            cells = 1;
        pos += cells - 1; // the zeros
        bits[pos >> 3] |= (uint8_t)(0x80 >> (pos & 7)); // the transition
        pos++;
        acc = 0;
    }
    *out = bits;
    return pos;
}

// Frame a looped bitstream into disk bytes the way the IWM does: shift bits
// in MSB first, and a byte is complete once its top bit is set.  Two
// revolutions, so a sector straddling the index is caught whole.  Returns
// the byte count (at most `cap`).
static size_t frame_nibbles(const uint8_t *bits, uint32_t nbits, uint8_t *out, size_t cap) {
    uint32_t reg = 0, k = 0;
    size_t n = 0;
    for (uint64_t i = 0; i < 2ull * nbits && n < cap; i++) {
        reg = (reg << 1) | ((bits[k >> 3] >> (7 - (k & 7))) & 1);
        if (++k == nbits)
            k = 0; // the track loops
        if (reg & 0x80) {
            out[n++] = (uint8_t)reg;
            reg = 0;
        }
    }
    return n;
}

// Per-sector decode state while a track is scanned.
typedef struct {
    bool found; // a sector with this number was decoded
    bool good; // and its checksum matched
} sector_seen_t;

// Find and decode every sector of track `track`, side `side` in the framed
// disk bytes `nib`, storing them at `data` / `tags` (the track's first
// sector).  Updates the format byte and the bad-checksum count.
static void decode_track(const uint8_t *nib, size_t n, int track, int side, uint8_t *data, uint8_t *tags,
                         sector_seen_t *seen, moof_disk_t *d) {
    int nsec = zone_sectors(track);
    uint8_t raw[SECTOR_RAW];
    for (size_t i = 0; i + 8 <= n; i++) {
        if (nib[i] != 0xD5 || nib[i + 1] != 0xAA || nib[i + 2] != 0x96)
            continue;
        // Address field: track, sector, side (+ track bit 6), format, checksum.
        uint8_t h[5];
        bool ok = true;
        for (int j = 0; j < 5; j++) {
            h[j] = gcr_decode[nib[i + 3 + j]];
            ok = ok && h[j] != 0xFF;
        }
        if (!ok || ((h[0] ^ h[1] ^ h[2] ^ h[3]) & 0x3F) != h[4])
            continue;
        int trk = h[0] | ((h[2] & 0x01) << 6), sec = h[1], sid = (h[2] & 0x20) ? 1 : 0;
        if (trk != track || sid != side || sec >= nsec)
            continue; // not a sector of this track
        // The data field follows within a few dozen bytes.
        size_t j = i + 8, end = i + 8 + 64;
        while (j + 3 <= n && j < end && !(nib[j] == 0xD5 && nib[j + 1] == 0xAA && nib[j + 2] == 0xAD))
            j++;
        if (j >= end || j + 4 + SECTOR_NIBS + CKSUM_NIBS > n)
            continue;
        if (seen[sec].good)
            continue; // the second revolution's copy
        int rc = gcr_denibblize(nib + j + 4, raw);
        if (rc < 0 || (seen[sec].found && rc == 0))
            continue; // undecodable, or no better than the copy we have
        memcpy(tags + (size_t)sec * MOOF_TAG_BYTES, raw, MOOF_TAG_BYTES);
        memcpy(data + (size_t)sec * SECTOR_BYTES, raw + MOOF_TAG_BYTES, SECTOR_BYTES);
        if (!d->format_byte)
            d->format_byte = h[3];
        seen[sec].found = true;
        seen[sec].good = rc == 1;
        i = j + SECTOR_NIBS; // past the data field
    }
}

int moof_decode(const uint8_t *f, size_t len, moof_disk_t *out) {
    memset(out, 0, sizeof *out);
    if (!moof_detect(f, len) || len < MOOF_HEADER_SIZE + CHUNK_HDR)
        return -EINVAL;
    uint32_t want_crc = RD_LE32(f + 8);
    if (want_crc && gs_crc32(0, f + MOOF_HEADER_SIZE, len - MOOF_HEADER_SIZE) != want_crc)
        out->crc_mismatch = true; // the spec says warn, not refuse
    moof_chunks_t c;
    moof_walk(f, len, &c);
    if (!c.info || !c.tmap || !c.trks)
        return -EINVAL;
    const uint8_t *info = f + c.info;
    if (info[0] < 1)
        return -EINVAL; // INFO version
    out->disk_type = info[1];
    out->write_protected = info[2] == 1;
    if (out->disk_type != MOOF_DISK_SSDD_GCR && out->disk_type != MOOF_DISK_DSDD_GCR)
        return -ENOTSUP;
    uint8_t timing = info[4] ? info[4] : GCR_TIMING;
    // A FLUX map, when INFO says there is one (both flux fields set).
    const uint8_t *flux_map = NULL;
    if (RD_LE16(info + 40) && RD_LE16(info + 42) && c.flux)
        flux_map = f + c.flux;
    int sides = out->disk_type == MOOF_DISK_DSDD_GCR ? 2 : 1;
    uint32_t sectors = 0;
    for (int t = 0; t < NUM_TRACKS; t++)
        sectors += (uint32_t)(zone_sectors(t) * sides);
    out->sectors = sectors;
    out->data = calloc(sectors, SECTOR_BYTES);
    out->tags = calloc(sectors, MOOF_TAG_BYTES);
    // Twice the longest track's bytes: two revolutions of at most one disk
    // byte per 8 bits.
    size_t nib_cap = 0;
    uint8_t *nib = NULL;
    if (!out->data || !out->tags) {
        moof_disk_free(out);
        return -ENOMEM;
    }
    const uint8_t *tmap = f + c.tmap;
    const uint8_t *trk = f + c.trks;
    uint32_t sector = 0, decoded = 0;
    int rc = 0;
    for (int t = 0; t < NUM_TRACKS && rc == 0; t++) {
        int nsec = zone_sectors(t);
        for (int s = 0; s < sides; s++, sector += (uint32_t)nsec) {
            sector_seen_t seen[12] = {0};
            int slot = t * 2 + s;
            // FLUX wins over TMAP when both name a track.
            bool is_flux = flux_map && flux_map[slot] != TMAP_BLANK;
            uint8_t idx = is_flux ? flux_map[slot] : tmap[slot];
            if (idx != TMAP_BLANK && idx < TRK_COUNT) {
                const uint8_t *rec = trk + (size_t)idx * TRK_SIZE;
                uint64_t start = (uint64_t)RD_LE16(rec) * MOOF_BLOCK;
                uint32_t count = RD_LE32(rec + 4); // bits, or flux bytes
                uint64_t bytes = is_flux ? count : ((uint64_t)count + 7) / 8;
                const uint8_t *bits = NULL;
                uint8_t *owned = NULL;
                uint32_t nbits = 0;
                if (count && start >= FIRST_BLOCK * MOOF_BLOCK && start + bytes <= len) {
                    if (is_flux) {
                        nbits = flux_to_bits(f + start, (size_t)bytes, timing, &owned);
                        bits = owned;
                        out->flux_tracks++;
                    } else {
                        bits = f + start;
                        nbits = count;
                    }
                }
                if (bits && nbits) {
                    size_t need = (size_t)nbits / 4 + 16; // two revolutions of bytes
                    if (need > nib_cap) {
                        uint8_t *nn = realloc(nib, need);
                        if (!nn) {
                            free(owned);
                            rc = -ENOMEM;
                            break;
                        }
                        nib = nn;
                        nib_cap = need;
                    }
                    size_t n = frame_nibbles(bits, nbits, nib, nib_cap);
                    decode_track(nib, n, t, s, out->data + (size_t)sector * SECTOR_BYTES,
                                 out->tags + (size_t)sector * MOOF_TAG_BYTES, seen, out);
                }
                free(owned);
            }
            for (int k = 0; k < nsec; k++) {
                if (!seen[k].found)
                    out->missing++;
                else {
                    decoded++;
                    if (!seen[k].good)
                        out->bad_checksum++;
                }
            }
        }
    }
    free(nib);
    if (rc == 0 && decoded == 0)
        rc = -EINVAL; // nothing on it reads as a Mac GCR disk
    if (rc != 0) {
        moof_disk_free(out);
        return rc;
    }
    return 0;
}

void moof_disk_free(moof_disk_t *d) {
    if (!d)
        return;
    free(d->data);
    free(d->tags);
    memset(d, 0, sizeof *d);
}

// ============================================================================
// Writer
// ============================================================================

// A growing bitstream, MSB first.
typedef struct {
    uint8_t *buf;
    uint32_t bits;
    uint32_t cap_bits;
} bitbuf_t;

// Append the low `n` bits of `v`, most significant first.
static void bits_put(bitbuf_t *b, uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--) {
        if (b->bits < b->cap_bits && ((v >> i) & 1))
            b->buf[b->bits >> 3] |= (uint8_t)(0x80 >> (b->bits & 7));
        b->bits++;
    }
}

// A disk byte: eight bit cells.
static void bits_byte(bitbuf_t *b, uint8_t v) {
    bits_put(b, v, 8);
}

// A self-sync byte: $FF followed by two zero cells, ten in all.
static void bits_sync(bitbuf_t *b, int count) {
    for (int i = 0; i < count; i++)
        bits_put(b, 0x3FC, 10);
}

// Revolution length in 2 µs cells for a zone: the Mac drive's speed for the
// zone (394-590 rpm, outer to inner).
static uint32_t zone_track_bits(int track) {
    static const uint32_t rpm[5] = {394, 429, 472, 525, 590};
    return (uint32_t)(60000000ull / (rpm[track / 16] * 2ull));
}

// Bits one sector costs beyond its leading sync: address field and slip
// bytes (11 bytes), five sync, data field (3 + 1 + 703 + 2) and a pad.
#define SECTOR_FIXED_BITS (11 * 8 + 5 * 10 + (3 + 1 + SECTOR_NIBS + CKSUM_NIBS + 2 + 1) * 8)
#define MIN_GAP_SYNC      5 // fewest self-sync bytes ahead of an address field

// Lay one track down as a Mac formats it: 2:1 interleave, each sector a
// sync gap, an address field and a data field.  `data` / `tags` are the
// track's first sector.
static void encode_track(bitbuf_t *b, int track, int side, uint8_t format_byte, const uint8_t *data,
                         const uint8_t *tags) {
    int nsec = zone_sectors(track);
    // 2:1 interleave: each sector two slots after the last, sliding past a
    // taken slot (0 6 1 7 2 8 ... on a twelve-sector track).
    int order[12];
    bool used[12] = {false};
    for (int s = 0, slot = 0; s < nsec; s++) {
        while (used[slot])
            slot = (slot + 1) % nsec;
        order[slot] = s;
        used[slot] = true;
        slot = (slot + 2) % nsec;
    }
    // The revolution's spare cells go to the gaps, evenly.
    uint32_t want = zone_track_bits(track);
    uint32_t fixed = (uint32_t)nsec * SECTOR_FIXED_BITS;
    int gap = want > fixed ? (int)((want - fixed) / 10 / (uint32_t)nsec) : 0;
    if (gap < MIN_GAP_SYNC)
        gap = MIN_GAP_SYNC;
    uint8_t raw[SECTOR_RAW];
    uint8_t nib[SECTOR_NIBS + CKSUM_NIBS];
    for (int p = 0; p < nsec; p++) {
        int sec = order[p];
        uint8_t trk = (uint8_t)(track & 0x3F);
        uint8_t sid = (uint8_t)((side ? 0x20 : 0) | (track >> 6)); // side, and track bit 6
        bits_sync(b, gap);
        bits_byte(b, 0xD5), bits_byte(b, 0xAA), bits_byte(b, 0x96);
        bits_byte(b, gcr_encode[trk]);
        bits_byte(b, gcr_encode[sec]);
        bits_byte(b, gcr_encode[sid]);
        bits_byte(b, gcr_encode[format_byte & 0x3F]);
        bits_byte(b, gcr_encode[(trk ^ sec ^ sid ^ format_byte) & 0x3F]);
        bits_byte(b, 0xDE), bits_byte(b, 0xAA), bits_byte(b, 0xFF);
        bits_sync(b, 5);
        bits_byte(b, 0xD5), bits_byte(b, 0xAA), bits_byte(b, 0xAD);
        bits_byte(b, gcr_encode[sec]);
        if (tags)
            memcpy(raw, tags + (size_t)sec * MOOF_TAG_BYTES, MOOF_TAG_BYTES);
        else
            memset(raw, 0, MOOF_TAG_BYTES);
        memcpy(raw + MOOF_TAG_BYTES, data + (size_t)sec * SECTOR_BYTES, SECTOR_BYTES);
        gcr_nibblize(raw, nib);
        for (int k = 0; k < SECTOR_NIBS + CKSUM_NIBS; k++)
            bits_byte(b, nib[k]);
        bits_byte(b, 0xDE), bits_byte(b, 0xAA), bits_byte(b, 0xFF);
    }
}

int moof_encode(const uint8_t *data, uint32_t sectors, const uint8_t *tags, uint8_t format_byte, uint8_t **out,
                size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    int sides = sectors == 1600 ? 2 : sectors == 800 ? 1 : 0;
    if (!data || !sides)
        return -EINVAL;
    if (!format_byte)
        format_byte = sides == 2 ? 0x22 : 0x02;
    // Every track fits in 20 blocks (the outer zone's ~76,000 bits + slack).
    const uint32_t track_blocks_max = 20;
    int ntracks = NUM_TRACKS * sides;
    size_t cap = (size_t)FIRST_BLOCK * MOOF_BLOCK + (size_t)ntracks * track_blocks_max * MOOF_BLOCK;
    uint8_t *f = calloc(1, cap);
    if (!f)
        return -ENOMEM;
    // Header (CRC filled in last), INFO, TMAP and TRKS at their fixed places.
    memcpy(f, moof_magic, sizeof moof_magic);
    memcpy(f + INFO_AT, "INFO", 4);
    WR_LE32(f + INFO_AT + 4, INFO_SIZE);
    uint8_t *info = f + INFO_AT + CHUNK_HDR;
    info[0] = 1; // INFO version
    info[1] = sides == 2 ? MOOF_DISK_DSDD_GCR : MOOF_DISK_SSDD_GCR;
    info[2] = 0; // not write protected
    info[3] = 0; // no cross-track sync
    info[4] = GCR_TIMING;
    memset(info + 5, ' ', 32);
    memcpy(info + 5, "Granny Smith", 12); // creator, space padded
    memcpy(f + TMAP_AT, "TMAP", 4);
    WR_LE32(f + TMAP_AT + 4, TMAP_SIZE);
    uint8_t *tmap = f + TMAP_AT + CHUNK_HDR;
    memset(tmap, TMAP_BLANK, TMAP_SIZE);
    memcpy(f + TRKS_AT, "TRKS", 4);
    uint8_t *trk = f + TRKS_AT + CHUNK_HDR;
    uint32_t block = FIRST_BLOCK, largest = 0, sector = 0;
    int idx = 0;
    for (int t = 0; t < NUM_TRACKS; t++) {
        for (int s = 0; s < sides; s++, idx++) {
            bitbuf_t b = {f + (size_t)block * MOOF_BLOCK, 0, track_blocks_max * MOOF_BLOCK * 8};
            encode_track(&b, t, s, format_byte, data + (size_t)sector * SECTOR_BYTES,
                         tags ? tags + (size_t)sector * MOOF_TAG_BYTES : NULL);
            sector += (uint32_t)zone_sectors(t);
            if (b.bits > b.cap_bits) {
                free(f);
                return -EOVERFLOW; // cannot happen with the zone lengths above
            }
            uint32_t blocks = (b.bits + MOOF_BLOCK * 8 - 1) / (MOOF_BLOCK * 8);
            tmap[t * 2 + s] = (uint8_t)idx;
            WR_LE16(trk + idx * TRK_SIZE, block);
            WR_LE16(trk + idx * TRK_SIZE + 2, blocks);
            WR_LE32(trk + idx * TRK_SIZE + 4, b.bits);
            block += blocks;
            if (blocks > largest)
                largest = blocks;
        }
    }
    WR_LE16(info + 38, largest); // largest track, in blocks
    size_t len = (size_t)block * MOOF_BLOCK;
    WR_LE32(f + TRKS_AT + 4, (uint32_t)(len - (TRKS_AT + CHUNK_HDR)));
    WR_LE32(f + 8, gs_crc32(0, f + MOOF_HEADER_SIZE, len - MOOF_HEADER_SIZE));
    *out = f;
    *out_len = len;
    return 0;
}

// ============================================================================
// Format-registry adapter
// ============================================================================

source_t *moof_source_open(source_t *file, int *err) {
    int e = 0;
    if (!err)
        err = &e;
    uint8_t *buf = NULL;
    size_t len = 0;
    *err = source_read_all(file, MOOF_MAX_FILE, &buf, &len);
    if (*err)
        return NULL;
    moof_disk_t d;
    *err = moof_decode(buf, len, &d);
    free(buf);
    if (*err) {
        LOG(1, "MOOF '%s' does not decode: %s", source_key(file),
            *err == -ENOTSUP ? "not a 400K/800K GCR disk" : strerror(-*err));
        return NULL;
    }
    if (d.crc_mismatch)
        LOG(0, "MOOF '%s': the header CRC does not match its contents; reading it anyway", source_key(file));
    if (d.missing || d.bad_checksum)
        LOG(0, "MOOF '%s': %u of %u sectors missing (read as zeros), %u with a bad data checksum", source_key(file),
            d.missing, d.sectors, d.bad_checksum);
    // A DiskCopy 4.2 image: header, data section, tag section.
    size_t data_len = (size_t)d.sectors * SECTOR_BYTES, tag_len = (size_t)d.sectors * MOOF_TAG_BYTES;
    size_t out_len = DISKCOPY_HEADER_SIZE + data_len + tag_len;
    uint8_t *img = malloc(out_len);
    if (!img) {
        moof_disk_free(&d);
        *err = -ENOMEM;
        return NULL;
    }
    memcpy(img + DISKCOPY_HEADER_SIZE, d.data, data_len);
    memcpy(img + DISKCOPY_HEADER_SIZE + data_len, d.tags, tag_len);
    uint8_t name[DC42_NAME_FIELD] = {0}; // no name: an export names it after the file
    uint32_t data_sum = dc42_checksum(0, d.data, data_len);
    uint32_t tag_sum =
        dc42_checksum(0, d.tags + MOOF_TAG_BYTES, tag_len - MOOF_TAG_BYTES); // the first tag is not summed
    uint8_t disk_format = d.disk_type == MOOF_DISK_DSDD_GCR ? 1 : 0;
    uint8_t format_byte = d.format_byte ? d.format_byte : (disk_format ? 0x22 : 0x02);
    dc42_build_header(img, name, (uint32_t)data_len, (uint32_t)tag_len, data_sum, tag_sum, disk_format, format_byte);
    moof_disk_free(&d);
    char *key = str_printf("%s#moof", source_key(file));
    source_t *s = source_memory(img, out_len, true, key);
    free(key);
    if (!s) {
        free(img);
        *err = -ENOMEM;
    }
    return s;
}
