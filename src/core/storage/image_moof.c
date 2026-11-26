// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_moof.c
// Applesauce MOOF floppy images: decode the bitstreams to GCR sectors, and
// lay sectors down as bitstreams.  See image_moof.h and
// docs/reference/formats/moof.md.

#include "image_moof.h"

#include "codec.h"
#include "common.h"
#include "format_registry.h"
#include "gcr.h"
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
#define SECTOR_NIBS  699 // 12 tag + 512 data bytes in 6-and-2 GCR (then 4 checksum values)
#define GCR_TIMING   16 // optimal bit timing for GCR, in 125 ns ticks (2 µs)

_Static_assert(MOOF_TAG_BYTES == GCR_TAG_BYTES && SECTOR_BYTES == GCR_DATA_BYTES, "a MOOF sector is a GCR sector");

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
// Reader
// ============================================================================

bool moof_detect(const uint8_t *head, size_t len) {
    return head && len >= sizeof moof_magic && memcmp(head, moof_magic, sizeof moof_magic) == 0;
}

// Walk the chunk list from byte 12, skipping unknown chunks.
static void moof_walk(const uint8_t *f, size_t len, moof_index_t *ix) {
    size_t off = MOOF_HEADER_SIZE;
    while (off + CHUNK_HDR <= len) {
        uint32_t id = RD_BE32(f + off), size = RD_LE32(f + off + 4);
        size_t data = off + CHUNK_HDR;
        if (size > len - data)
            size = (uint32_t)(len - data); // a truncated last chunk: what is there
        if (id == 0x494E464F && !ix->info && size >= INFO_SIZE) // 'INFO'
            ix->info = f + data;
        else if (id == 0x544D4150 && !ix->tmap && size >= TMAP_SIZE) // 'TMAP'
            ix->tmap = f + data;
        else if (id == 0x54524B53 && !ix->trks && size >= TRK_COUNT * TRK_SIZE) // 'TRKS'
            ix->trks = f + data;
        else if (id == 0x464C5558 && !ix->flux_map && size >= TMAP_SIZE) // 'FLUX'
            ix->flux_map = f + data;
        else if (id == 0x4D455441 && !ix->meta) { // 'META'
            ix->meta = f + data;
            ix->meta_len = size;
        }
        off = data + size;
    }
}

int moof_index(const uint8_t *f, size_t len, moof_index_t *ix) {
    memset(ix, 0, sizeof *ix);
    if (!moof_detect(f, len) || len < MOOF_HEADER_SIZE + CHUNK_HDR)
        return -EINVAL;
    ix->file = f;
    ix->len = len;
    uint32_t want_crc = RD_LE32(f + 8);
    if (want_crc && codec_crc32(0, f + MOOF_HEADER_SIZE, len - MOOF_HEADER_SIZE) != want_crc)
        ix->crc_mismatch = true; // the spec says warn, not refuse
    moof_walk(f, len, ix);
    if (!ix->info || !ix->tmap || !ix->trks)
        return -EINVAL;
    if (ix->info[0] < 1)
        return -EINVAL; // INFO version
    ix->disk_type = ix->info[1];
    ix->write_protected = ix->info[2] == 1;
    if (ix->disk_type != MOOF_DISK_SSDD_GCR && ix->disk_type != MOOF_DISK_DSDD_GCR)
        return -ENOTSUP;
    ix->timing = ix->info[4] ? ix->info[4] : GCR_TIMING;
    ix->largest_track_blocks = RD_LE16(ix->info + 38);
    // A FLUX map counts only when INFO says there is one (both flux fields set).
    if (!(RD_LE16(ix->info + 40) && RD_LE16(ix->info + 42)))
        ix->flux_map = NULL;
    ix->sides = ix->disk_type == MOOF_DISK_DSDD_GCR ? 2 : 1;
    return 0;
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

bool moof_track_is_flux(const moof_index_t *ix, int track, int side) {
    if (!ix || !ix->flux_map || track < 0 || track >= NUM_TRACKS || side < 0 || side > 1)
        return false;
    return ix->flux_map[track * 2 + side] != TMAP_BLANK;
}

uint32_t moof_track_bits(const moof_index_t *ix, int track, int side, const uint8_t **bits, uint8_t **owned) {
    *bits = NULL;
    *owned = NULL;
    if (!ix || !ix->file || track < 0 || track >= NUM_TRACKS || side < 0 || side >= ix->sides)
        return 0;
    int slot = track * 2 + side;
    // FLUX wins over TMAP when both name a track.
    bool is_flux = moof_track_is_flux(ix, track, side);
    uint8_t idx = is_flux ? ix->flux_map[slot] : ix->tmap[slot];
    if (idx == TMAP_BLANK || idx >= TRK_COUNT)
        return 0;
    const uint8_t *rec = ix->trks + (size_t)idx * TRK_SIZE;
    uint64_t start = (uint64_t)RD_LE16(rec) * MOOF_BLOCK;
    uint32_t count = RD_LE32(rec + 4); // bits, or flux bytes
    uint64_t bytes = is_flux ? count : ((uint64_t)count + 7) / 8;
    if (!count || start < FIRST_BLOCK * MOOF_BLOCK || start + bytes > ix->len)
        return 0;
    if (!is_flux) {
        *bits = ix->file + start;
        return count;
    }
    uint32_t n = flux_to_bits(ix->file + start, (size_t)bytes, ix->timing, owned);
    *bits = *owned;
    return n;
}

size_t moof_frame_track(const uint8_t *bits, uint32_t nbits, uint8_t **out) {
    *out = NULL;
    if (!bits || !nbits)
        return 0;
    size_t cap = (size_t)nbits / 8 + 2; // at most one disk byte per 8 cells, and one straddling the index
    uint8_t *nib = malloc(cap);
    if (!nib)
        return 0;
    size_t n = gcr_frame_revolution(bits, nbits, nib, cap);
    if (!n) {
        free(nib);
        return 0;
    }
    *out = nib;
    return n;
}

// The nibble `i` places past `at` on a looping revolution of `n` bytes.
static inline uint8_t nib_at(const uint8_t *rev, size_t n, size_t at) {
    return rev[at % n];
}

// True when the three nibbles at `at` are `a b c`.
static inline bool mark_at(const uint8_t *rev, size_t n, size_t at, uint8_t a, uint8_t b, uint8_t c) {
    return nib_at(rev, n, at) == a && nib_at(rev, n, at + 1) == b && nib_at(rev, n, at + 2) == c;
}

// How far past a data mark its field may run before the scan stops looking
// for the DE AA that closes it: a standard field is 709 nibbles.
#define DATA_FIELD_SEARCH 1200

// Where the data field whose mark is at `j` ends: past the DE AA that
// closes it -- right after the values on a standard field, wherever it falls
// on another (AA is no codeword, so it cannot occur inside a field that
// decodes).  Never past the next field's mark: a field with no DE AA of its
// own (a bit-slip field, a garbled field) would otherwise run on to the
// one closing the next address field, and a rewrite of the sector would
// take that address field with it.  Such a field ends where a standard one
// does, or, if the next mark comes sooner, at the sync in front of it.
static size_t data_field_end(const uint8_t *rev, size_t n, size_t j) {
    size_t e = j + 4, standard = j + 4 + GCR_SECTOR_VALUES + 2;
    for (; e < j + DATA_FIELD_SEARCH; e++) {
        uint8_t a = nib_at(rev, n, e), b = nib_at(rev, n, e + 1);
        if (a == 0xDE && b == 0xAA)
            return e + 2;
        if (a == 0xD5 && b == 0xAA)
            break; // the next field's mark
    }
    if (e >= standard)
        return standard;
    while (e > j + 4 && nib_at(rev, n, e - 1) == 0xFF)
        e--; // keep the next field's sync
    return e;
}

void moof_scan_track(const uint8_t *rev, size_t n, int track, int side, moof_sector_t sec[MOOF_MAX_SECTORS],
                     uint8_t *format_byte) {
    int nsec = zone_sectors(track);
    memset(sec, 0, sizeof(moof_sector_t) * MOOF_MAX_SECTORS);
    for (int k = 0; k < MOOF_MAX_SECTORS; k++) {
        sec[k].state = MOOF_SECTOR_MISSING;
        sec[k].data_at = sec[k].data_end = MOOF_NO_FIELD;
    }
    if (!rev || n < 16)
        return;
    uint8_t field[GCR_SECTOR_VALUES], addr[GCR_ADDR_VALUES];
    uint8_t tag[MOOF_TAG_BYTES], data[SECTOR_BYTES];
    for (size_t i = 0; i < n; i++) {
        if (!mark_at(rev, n, i, 0xD5, 0xAA, 0x96))
            continue;
        // Address field: track, sector, side (+ track bit 6), format, checksum.
        for (int k = 0; k < GCR_ADDR_VALUES; k++)
            addr[k] = nib_at(rev, n, i + 3 + (size_t)k);
        int trk, s, sid;
        uint8_t fmt;
        if (!gcr_addr_from_disk(addr, &trk, &s, &sid, &fmt))
            continue;
        if (trk != track || sid != side || s >= nsec)
            continue; // not a sector of this track
        moof_sector_t *o = &sec[s];
        if (o->state == MOOF_SECTOR_OK)
            continue; // a second address field for a sector already read
        // The data field follows within a few dozen bytes.
        size_t j = i + 8, end = i + 8 + 64, data_at = MOOF_NO_FIELD, data_end = MOOF_NO_FIELD;
        while (j < end && !mark_at(rev, n, j, 0xD5, 0xAA, 0xAD))
            j++;
        int rc = -1;
        if (j < end) {
            data_at = j;
            // Its 703 values follow the sector nibble.
            for (int k = 0; k < GCR_SECTOR_VALUES; k++)
                field[k] = nib_at(rev, n, j + 4 + (size_t)k);
            rc = gcr_sector_from_disk(field, tag, data);
            data_end = data_field_end(rev, n, j);
        }
        // Keep the first copy that reads, else the first with a bad
        // checksum, else the first address field seen.
        bool take = rc == 1 || (rc == 0 && o->state != MOOF_SECTOR_BAD) || (!o->addr);
        if (!take)
            continue;
        o->addr = true;
        o->addr_at = (uint32_t)i;
        o->data_at = (uint32_t)data_at;
        o->data_end = (uint32_t)data_end;
        if (rc >= 0) {
            memcpy(o->tag, tag, MOOF_TAG_BYTES);
            memcpy(o->data, data, SECTOR_BYTES);
            o->state = rc == 1 ? MOOF_SECTOR_OK : MOOF_SECTOR_BAD;
            if (format_byte && !*format_byte)
                *format_byte = fmt;
        }
    }
}

int moof_decode(const uint8_t *f, size_t len, moof_disk_t *out) {
    memset(out, 0, sizeof *out);
    moof_index_t ix;
    int rc = moof_index(f, len, &ix);
    if (rc == -ENOTSUP) {
        out->disk_type = ix.disk_type;
        return rc;
    }
    if (rc)
        return rc;
    out->crc_mismatch = ix.crc_mismatch;
    out->disk_type = ix.disk_type;
    out->write_protected = ix.write_protected;
    uint32_t sectors = 0;
    for (int t = 0; t < NUM_TRACKS; t++)
        sectors += (uint32_t)(zone_sectors(t) * ix.sides);
    out->sectors = sectors;
    out->data = calloc(sectors, SECTOR_BYTES);
    out->tags = calloc(sectors, MOOF_TAG_BYTES);
    if (!out->data || !out->tags) {
        moof_disk_free(out);
        return -ENOMEM;
    }
    moof_sector_t *sec = malloc(sizeof(moof_sector_t) * MOOF_MAX_SECTORS);
    if (!sec) {
        moof_disk_free(out);
        return -ENOMEM;
    }
    uint32_t sector = 0, decoded = 0;
    for (int t = 0; t < NUM_TRACKS; t++) {
        int nsec = zone_sectors(t);
        for (int s = 0; s < ix.sides; s++, sector += (uint32_t)nsec) {
            const uint8_t *bits = NULL;
            uint8_t *owned = NULL, *nib = NULL;
            uint32_t nbits = moof_track_bits(&ix, t, s, &bits, &owned);
            if (moof_track_is_flux(&ix, t, s) && nbits)
                out->flux_tracks++;
            // The track as the head sees it in steady state, scanned looping.
            size_t n = moof_frame_track(bits, nbits, &nib);
            moof_scan_track(nib, n, t, s, sec, &out->format_byte);
            free(nib);
            free(owned);
            for (int k = 0; k < nsec; k++) {
                if (sec[k].state == MOOF_SECTOR_MISSING) {
                    out->missing++;
                    continue;
                }
                memcpy(out->tags + (size_t)(sector + (uint32_t)k) * MOOF_TAG_BYTES, sec[k].tag, MOOF_TAG_BYTES);
                memcpy(out->data + (size_t)(sector + (uint32_t)k) * SECTOR_BYTES, sec[k].data, SECTOR_BYTES);
                decoded++;
                if (sec[k].state == MOOF_SECTOR_BAD)
                    out->bad_checksum++;
            }
        }
    }
    free(sec);
    if (decoded == 0) {
        moof_disk_free(out);
        return -EINVAL; // nothing on it reads as a Mac GCR disk
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
#define SECTOR_FIXED_BITS (11 * 8 + 5 * 10 + (3 + 1 + GCR_SECTOR_VALUES + 2 + 1) * 8)
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
    static const uint8_t no_tag[MOOF_TAG_BYTES];
    uint8_t addr[GCR_ADDR_VALUES];
    uint8_t values[GCR_SECTOR_VALUES];
    for (int p = 0; p < nsec; p++) {
        int sec = order[p];
        bits_sync(b, gap);
        bits_byte(b, 0xD5), bits_byte(b, 0xAA), bits_byte(b, 0x96);
        // Track, sector, side (+ track bit 6), format, checksum.
        gcr_addr_encode(track, sec, side, format_byte, addr);
        for (int k = 0; k < GCR_ADDR_VALUES; k++)
            bits_byte(b, gcr_encode6[addr[k]]);
        bits_byte(b, 0xDE), bits_byte(b, 0xAA), bits_byte(b, 0xFF);
        bits_sync(b, 5);
        bits_byte(b, 0xD5), bits_byte(b, 0xAA), bits_byte(b, 0xAD);
        bits_byte(b, gcr_encode6[sec]);
        const uint8_t *tag = tags ? tags + (size_t)sec * MOOF_TAG_BYTES : no_tag;
        gcr_sector_nibblize(tag, data + (size_t)sec * SECTOR_BYTES, false, values);
        for (int k = 0; k < GCR_SECTOR_VALUES; k++)
            bits_byte(b, gcr_encode6[values[k]]);
        bits_byte(b, 0xDE), bits_byte(b, 0xAA), bits_byte(b, 0xFF);
    }
}

int moof_encode(const uint8_t *data, uint32_t sectors, const uint8_t *tags, uint8_t format_byte, uint8_t **out,
                size_t *out_len) {
    return moof_encode_with(data, sectors, tags, format_byte, NULL, NULL, out, out_len);
}

// True when (track, side)'s bit is set in a keep mask.
static bool keep_bit(const uint8_t *keep, int track, int side) {
    int slot = track * 2 + side;
    return keep && (keep[slot >> 3] >> (slot & 7)) & 1;
}

int moof_encode_with(const uint8_t *data, uint32_t sectors, const uint8_t *tags, uint8_t format_byte,
                     const moof_index_t *src, const uint8_t keep[MOOF_TRACK_SLOTS / 8], uint8_t **out,
                     size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    int sides = sectors == 1600 ? 2 : sectors == 800 ? 1 : 0;
    if (!data || !sides || (src && src->sides != sides))
        return -EINVAL;
    if (!src)
        keep = NULL; // nothing to keep tracks from
    if (!format_byte)
        format_byte = sides == 2 ? 0x22 : 0x02;
    // Every encoded track fits in 20 blocks (the outer zone's ~76,000 bits +
    // slack); a kept one takes what its bits take.
    const uint32_t track_blocks_max = 20;
    size_t cap = (size_t)FIRST_BLOCK * MOOF_BLOCK;
    for (int t = 0; t < NUM_TRACKS; t++)
        for (int s = 0; s < sides; s++) {
            size_t blocks = track_blocks_max;
            if (keep_bit(keep, t, s)) {
                const uint8_t *bits = NULL;
                uint8_t *owned = NULL;
                uint32_t nbits = moof_track_bits(src, t, s, &bits, &owned);
                free(owned);
                size_t need = ((size_t)nbits + MOOF_BLOCK * 8 - 1) / (MOOF_BLOCK * 8);
                if (need > blocks)
                    blocks = need;
            }
            cap += blocks * MOOF_BLOCK;
        }
    size_t meta_len = src && src->meta ? src->meta_len : 0;
    if (meta_len)
        cap += CHUNK_HDR + meta_len;
    uint8_t *f = calloc(1, cap);
    if (!f)
        return -ENOMEM;
    // Header (CRC filled in last), INFO, TMAP and TRKS at their fixed places.
    memcpy(f, moof_magic, sizeof moof_magic);
    memcpy(f + INFO_AT, "INFO", 4);
    WR_LE32(f + INFO_AT + 4, INFO_SIZE);
    uint8_t *info = f + INFO_AT + CHUNK_HDR;
    if (src) {
        memcpy(info, src->info, INFO_SIZE); // the capture's description of the disk
        WR_LE16(info + 40, 0); // no FLUX chunk: flux tracks are kept as bitstreams
        WR_LE16(info + 42, 0);
    } else {
        info[0] = 1; // INFO version
        info[1] = sides == 2 ? MOOF_DISK_DSDD_GCR : MOOF_DISK_SSDD_GCR;
        info[2] = 0; // not write protected
        info[3] = 0; // no cross-track sync
        info[4] = GCR_TIMING;
    }
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
        for (int s = 0; s < sides; s++) {
            uint8_t *at = f + (size_t)block * MOOF_BLOCK;
            uint32_t nbits = 0;
            if (keep_bit(keep, t, s)) {
                // The captured track, bit for bit.
                const uint8_t *bits = NULL;
                uint8_t *owned = NULL;
                nbits = moof_track_bits(src, t, s, &bits, &owned);
                if (nbits)
                    memcpy(at, bits, ((size_t)nbits + 7) / 8);
                free(owned);
            } else {
                bitbuf_t b = {at, 0, track_blocks_max * MOOF_BLOCK * 8};
                encode_track(&b, t, s, format_byte, data + (size_t)sector * SECTOR_BYTES,
                             tags ? tags + (size_t)sector * MOOF_TAG_BYTES : NULL);
                if (b.bits > b.cap_bits) {
                    free(f);
                    return -EOVERFLOW; // cannot happen with the zone lengths above
                }
                nbits = b.bits;
            }
            sector += (uint32_t)zone_sectors(t);
            if (!nbits)
                continue; // a blank track kept blank: no record
            uint32_t blocks = (nbits + MOOF_BLOCK * 8 - 1) / (MOOF_BLOCK * 8);
            tmap[t * 2 + s] = (uint8_t)idx;
            WR_LE16(trk + idx * TRK_SIZE, block);
            WR_LE16(trk + idx * TRK_SIZE + 2, blocks);
            WR_LE32(trk + idx * TRK_SIZE + 4, nbits);
            idx++;
            block += blocks;
            if (blocks > largest)
                largest = blocks;
        }
    }
    WR_LE16(info + 38, largest); // largest track, in blocks
    size_t len = (size_t)block * MOOF_BLOCK;
    WR_LE32(f + TRKS_AT + 4, (uint32_t)(len - (TRKS_AT + CHUNK_HDR)));
    if (meta_len) {
        // The capture's metadata, last.
        memcpy(f + len, "META", 4);
        WR_LE32(f + len + 4, (uint32_t)meta_len);
        memcpy(f + len + CHUNK_HDR, src->meta, meta_len);
        len += CHUNK_HDR + meta_len;
    }
    WR_LE32(f + 8, codec_crc32(0, f + MOOF_HEADER_SIZE, len - MOOF_HEADER_SIZE));
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

bool moof_source_write_protected(source_t *file) {
    uint8_t head[INFO_AT + CHUNK_HDR + INFO_SIZE];
    if (!file || source_read_exact(file, 0, head, sizeof head) != 0 || !moof_detect(head, sizeof head))
        return false;
    // INFO is the first chunk, at its fixed place.
    if (RD_BE32(head + INFO_AT) != 0x494E464F || RD_LE32(head + INFO_AT + 4) < INFO_SIZE)
        return false;
    return head[INFO_AT + CHUNK_HDR + 2] == 1;
}
