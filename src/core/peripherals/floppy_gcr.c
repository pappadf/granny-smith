// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// floppy_gcr.c
// GCR encoding/decoding and shared utility functions for the floppy subsystem.
// These functions are used by both IWM and SWIM code paths.

#include "floppy_internal.h"
#include "log.h"
#include "memory.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"

LOG_USE_CATEGORY_NAME("floppy");

// GCR codeword table as described in [7]
const uint8_t gcr_codewords[] = {0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6, 0xA7, 0xAB, 0xAC, 0xAD, 0xAE,
                                 0xAF, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE,
                                 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3, 0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD,
                                 0xDE, 0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xF2,
                                 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF};

// ============================================================================
// Shared Utility Functions (parameterized, used by both IWM and SWIM paths)
// ============================================================================

// Returns the approximate GCR-encoded length for a track
size_t iwm_track_length(int track) {
    // just approximative numbers
    static const size_t length[] = {9320, 8559, 7780, 6994, 6224};

    GS_ASSERT(track < 80);

    return length[track >> 4];
}

// Returns the rotational speed (RPM) for a given track
int iwm_track_rpm(int track) {
    static const int rpm[] = {394, 429, 472, 525, 590};

    GS_ASSERT(track < 80);

    return rpm[track >> 4];
}

// Returns the number of sectors for a given track (varies by zone)
int iwm_sectors_per_track(int track) {
    return 12 - (track >> 4);
}

// Calculates the byte offset into a disk image for a given track and side
size_t iwm_disk_image_offset(int track, int side, int num_sides) {
    size_t offset = 0;

    // Each track's worth of data contains num_sides sides worth of sectors
    for (int t = 0; t < track; t++)
        offset += (size_t)num_sides * iwm_sectors_per_track(t) * 512;

    // Add side offset within this track (only for double-sided disks with side=1)
    if (side && num_sides > 1)
        offset += iwm_sectors_per_track(track) * 512;

    return offset;
}

// ============================================================================
// Public geometry API (floppy_geometry.h)
// ============================================================================

int floppy_zone_sectors_per_track(int track) {
    return iwm_sectors_per_track(track);
}
int floppy_zone_rpm(int track) {
    return iwm_track_rpm(track);
}
size_t floppy_zone_track_length(int track) {
    return iwm_track_length(track);
}
size_t floppy_zone_image_offset(int track, int side, int num_sides) {
    return iwm_disk_image_offset(track, side, num_sides);
}

// Keyed on image_t::type, which classify_image() derives from the exact byte
// size -- the only classifier that was ever right (02-floppy F-17 preferred
// swim3_media's version, and this is it, promoted).
bool floppy_media_from_image(image_t *img, floppy_media_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!img)
        return false;
    out->img = img;
    switch (img->type) {
    case image_fd_hd: // 1440K MFM: 18 sectors/track, both sides, HD media
        out->mfm = true;
        out->hd = true;
        out->sides = 2;
        out->mfm_spt = 18;
        out->fmt_byte = 0x02; // MFM size code 2 = 512-byte sectors
        break;
    case image_fd_dd_mfm: // 720K MFM: 9 sectors/track on DD media
        out->mfm = true;
        out->sides = 2;
        out->mfm_spt = 9;
        out->fmt_byte = 0x02;
        break;
    case image_fd_ds: // 800K GCR: double-sided, interleave 2
        out->sides = 2;
        out->fmt_byte = 0x22;
        break;
    case image_fd_ss: // 400K GCR: single-sided
        out->sides = 1;
        out->fmt_byte = 0x02;
        break;
    default:
        out->img = NULL;
        return false;
    }
    out->valid = true;
    return true;
}

int floppy_media_spt(const floppy_media_t *m, int track) {
    if (!m || !m->valid)
        return 0;
    return m->mfm ? m->mfm_spt : floppy_zone_sectors_per_track(track);
}

// MFM tracks are uniform; GCR tracks shrink towards the spindle, so their
// offset accumulates by zone.
size_t floppy_media_sector_offset(const floppy_media_t *m, int track, int side, int sector) {
    if (!m || !m->valid)
        return 0;
    if (m->mfm)
        return ((size_t)(track * m->sides + side) * (size_t)m->mfm_spt + (size_t)sector) * FLOPPY_SECTOR_BYTES;
    return floppy_zone_image_offset(track, side, m->sides) + (size_t)sector * FLOPPY_SECTOR_BYTES;
}

// Determines the number of sides based on disk image type
int iwm_image_num_sides(image_t *img) {
    if (!img)
        return 2; // Default to double-sided if unknown
    // Only the 400K disk is single-sided; 720K, 800K and 1440K are not.
    return (img->type == image_fd_ss) ? 1 : 2;
}

// Calculates TACH signal state (60 pulses per revolution) based on current time and motor speed
int iwm_tach_signal(struct scheduler *scheduler, floppy_drive_t *drive, const char **reason) {
    // TACH produces 60 pulses per revolution; motor not spinning = no pulses
    // _motoron is active-low: true = motor OFF, false = motor ON
    if (drive->_motoron) {
        if (reason)
            *reason = "motor off";
        return 1;
    }

    // Calculate revolution period in nanoseconds based on track RPM
    double now_ns = scheduler_time_ns(scheduler);
    int rpm = iwm_track_rpm(drive->track);
    double ns_per_rev = (60.0 / rpm) * 1e9;

    // 60 pulses per revolution = 120 state changes (high/low) per revolution
    double ns_per_half_pulse = ns_per_rev / 120.0;
    double pos_in_rev = fmod(now_ns, ns_per_rev);
    int half_pulse_index = (int)(pos_in_rev / ns_per_half_pulse);

    if (reason)
        *reason = "calculated";
    return (half_pulse_index % 2) == 0 ? 1 : 0;
}

// ============================================================================
// GCR Encoding
// ============================================================================

// GCR encoding helper macros.
// These rely on a caller-scope `uint8_t *dst` that gets advanced as bytes are
// written. Only used inside `encode_sector` / `encode_track` below — do NOT
// invoke from any function that doesn't have a `dst` in scope.
#define RAW(x) (*dst++ = (x))
#define GCR(x) (*dst++ = gcr_codewords[(x) & 0x3F])
#define GCR3(a, b, c)                                                                                                  \
    do {                                                                                                               \
        GCR(((a) >> 2 & 0x30) | ((b) >> 4 & 0x0C) | ((c) >> 6 & 0x03));                                                \
        GCR(a);                                                                                                        \
        GCR(b);                                                                                                        \
        GCR(c);                                                                                                        \
    } while (0)
#define GCR2(a, b)                                                                                                     \
    do {                                                                                                               \
        GCR(((a) >> 2 & 0x30) | ((b) >> 4 & 0x0C));                                                                    \
        GCR(a);                                                                                                        \
        GCR(b);                                                                                                        \
    } while (0)

// Encodes a triplet of bytes with checksum update and advances src pointer
#define ENCODE_TRIPLET(src, ca, cb, cc, ba, bb, bc)                                                                    \
    do {                                                                                                               \
        (cc) = ((cc) << 1) | ((cc) >> 7 & 1);                                                                          \
        (ca) &= 0xFF;                                                                                                  \
        (ca) += *(src) + ((cc) & 1);                                                                                   \
        (ba) = *(src)++ ^ (cc);                                                                                        \
        (cb) &= 0xFF;                                                                                                  \
        (cb) += *(src) + ((ca) >> 8 & 1);                                                                              \
        (bb) = *(src)++ ^ (ca);                                                                                        \
        (cc) &= 0xFF;                                                                                                  \
        (cc) += *(src) + ((cb) >> 8 & 1);                                                                              \
        (bc) = *(src)++ ^ (cb);                                                                                        \
        GCR3(ba, bb, bc);                                                                                              \
    } while (0)

// The checksum chain in function form.  floppy_gcr.c's ENCODE_TRIPLET /
// DECODE_TRIPLET macros are the whole-track variant of the same algorithm --
// they advance a source pointer and capture into a destination in place, which
// is what encode_sector/decode_sector want; these are the DMA-stream variant
// SWIM3 wants.  tests/unit/suites/floppy pins that the two agree.
void gcr_encode_triplet(const uint8_t *src, uint16_t *ca, uint16_t *cb, uint16_t *cc, uint8_t *dst) {
    *cc = (uint16_t)((*cc << 1) | ((*cc >> 7) & 1));
    *ca &= 0xFF;
    *ca = (uint16_t)(*ca + src[0] + (*cc & 1));
    uint8_t ba = (uint8_t)(src[0] ^ *cc);
    *cb &= 0xFF;
    *cb = (uint16_t)(*cb + src[1] + ((*ca >> 8) & 1));
    uint8_t bb = (uint8_t)(src[1] ^ *ca);
    *cc &= 0xFF;
    *cc = (uint16_t)(*cc + src[2] + ((*cb >> 8) & 1));
    uint8_t bc = (uint8_t)(src[2] ^ *cb);
    dst[0] = (uint8_t)(((ba >> 2) & 0x30) | ((bb >> 4) & 0x0C) | ((bc >> 6) & 0x03));
    dst[1] = (uint8_t)(ba & 0x3F);
    dst[2] = (uint8_t)(bb & 0x3F);
    dst[3] = (uint8_t)(bc & 0x3F);
}

void gcr_decode_triplet(const uint8_t *src, uint16_t *ca, uint16_t *cb, uint16_t *cc, uint8_t *dst) {
    uint8_t ba = (uint8_t)(((src[0] << 2) & 0xC0) | (src[1] & 0x3F));
    uint8_t bb = (uint8_t)(((src[0] << 4) & 0xC0) | (src[2] & 0x3F));
    uint8_t bc = (uint8_t)(((src[0] << 6) & 0xC0) | (src[3] & 0x3F));
    *cc = (uint16_t)((*cc << 1) | ((*cc >> 7) & 1));
    dst[0] = (uint8_t)(ba ^ *cc);
    *ca &= 0xFF;
    *ca = (uint16_t)(*ca + dst[0] + (*cc & 1));
    dst[1] = (uint8_t)(bb ^ *ca);
    *cb &= 0xFF;
    *cb = (uint16_t)(*cb + dst[1] + ((*ca >> 8) & 1));
    dst[2] = (uint8_t)(bc ^ *cb);
    *cc &= 0xFF;
    *cc = (uint16_t)(*cc + dst[2] + ((*cb >> 8) & 1));
}

// Encodes a sector to GCR format with header and data fields
static uint8_t *encode_sector(uint8_t *dst, const uint8_t *tag, const uint8_t *data, int track, int sector, int side,
                              int num_sides) {
    GS_ASSERT(data != NULL);

    uint16_t ca = 0, cb = 0, cc = 0; // checksum registers
    uint8_t ba, bb, bc; // encoded bytes
    // 0x02 = single-sided GCR, 0x22 = double-sided GCR
    uint8_t format = (num_sides > 1) ? 0x22 : 0x02;

    // Header sync field (5 bytes)
    for (int i = 0; i < 5; i++)
        RAW(0xFF);

    // Header field: address marks + metadata
    RAW(0xD5);
    RAW(0xAA);
    RAW(0x96);
    GCR(track);
    GCR(sector);
    uint8_t side_enc = (side << 5) | (track >> 6 & 0x1F);
    GCR(side_enc);
    GCR(format);
    GCR(track ^ sector ^ side_enc ^ format);
    RAW(0xDE);
    RAW(0xAA);
    RAW(0xFF);

    // Data sync field (5 bytes)
    for (int i = 0; i < 5; i++)
        RAW(0xFF);

    // Data field: marks + sector number + encoded data + checksum
    RAW(0xD5);
    RAW(0xAA);
    RAW(0xAD);
    GCR(sector);

    // Encode 12-byte tag with checksum
    for (int i = 0; i < 12; i += 3)
        ENCODE_TRIPLET(tag, ca, cb, cc, ba, bb, bc);

    // Encode 510 bytes of data (170 triplets)
    for (int i = 0; i < 510; i += 3)
        ENCODE_TRIPLET(data, ca, cb, cc, ba, bb, bc);

    // Encode final 2 bytes
    cc = (cc << 1) | (cc >> 7 & 1);
    ca &= 0xFF;
    ca += *data + (cc & 1);
    ba = *data++ ^ cc;
    cb &= 0xFF;
    cb += *data + (ca >> 8 & 1);
    bb = *data++ ^ ca;
    GCR2(ba, bb);

    // Encode 24-bit checksum
    GCR3(ca, cb, cc);

    // End markers
    RAW(0xDE);
    RAW(0xAA);
    RAW(0xFF);

    return dst;
}

// Encodes an entire track with interleaved sectors to GCR format
static void encode_track(uint8_t *dst, size_t trk_length, int track, int side, const uint8_t *data, int num_sides,
                         image_t *img, size_t first_block) {
    GS_ASSERT(data != NULL);

    int i;
    int num_sectors = iwm_sectors_per_track(track);
    uint8_t *end_of_track = dst + trk_length;

#define NUM_SPEED_GROUPS 5

    // [7]: sectors are typically interleaved 2:1 because of the write recovery time.
    // Sector sequencing for 2:1 interleave, by speed group. -1 is "sector not
    // present at this radius" (outer tracks have more sectors than inner ones).
    static const int interleave[NUM_SPEED_GROUPS][12] = {
        // Group 0 (outermost, 12 sectors): tracks  0-15
        {0, 6, 1, 7, 2, 8, 3, 9, 4,  10, 5,  11},
        // Group 1 (11 sectors):              tracks 16-31
        {0, 6, 1, 7, 2, 8, 3, 9, 4,  10, 5,  -1},
        // Group 2 (10 sectors):              tracks 32-47
        {0, 5, 1, 6, 2, 7, 3, 8, 4,  9,  -1, -1},
        // Group 3 (9 sectors):               tracks 48-63
        {0, 5, 1, 6, 2, 7, 3, 8, 4,  -1, -1, -1},
        // Group 4 (innermost, 8 sectors):    tracks 64-79
        {0, 4, 1, 5, 2, 6, 3, 7, -1, -1, -1, -1},
    };

    // go through all sectors in track (note: "i" is not the sector number)
    for (i = 0; i < num_sectors; i++) {

        int sector = interleave[track >> 4][i];
        GS_ASSERT(sector != -1);

        // The 12 GCR tag bytes carry the HFS/MFS scavenger metadata.  This path
        // used to synthesise zeros ("just assume an empty tag for now"), so a
        // DiskCopy 4.2 image with tags lost them on any read through the
        // IWM/SWIM path -- and the same image behaved differently on a IIci and
        // a 7100, which does round-trip them (02-floppy F-14).
        uint8_t tag[12];
        memset(tag, 0, sizeof tag);
        if (img)
            disk_read_tag(img, first_block + (size_t)sector, tag, sizeof tag);

        dst = encode_sector(dst, tag, data + sector * 512, track, sector, side, num_sides);
    }

    GS_ASSERT(dst < end_of_track);

    // fill out the rest of the track
    while (dst < end_of_track)
        *dst++ = 0xFF; // sync bytes
}

// Returns a pointer to the GCR data for the specified drive/image/side, encoding on demand.
// Returns NULL for HD (MFM) images — those must be read via the ISM path.
uint8_t *iwm_track_data(floppy_drive_t *drive, image_t *img, int sel, struct scheduler *scheduler) {
    // No media in the drive — callers pass floppy->disk[drv] unconditionally,
    // so a probe of an empty drive lands here with img == NULL.  Treat the
    // same as "not GCR" and let the caller take its no-data branch.
    if (!img)
        return NULL;
    // MFM media is not GCR — reject it so the ROM falls through to the ISM
    // (SWIM) read path.  This used to test `== image_fd_hd`, so a 720K disk
    // (which classified as a hard disk before F-04) was GCR-encoded from
    // MFM-laid-out bytes and handed to the IWM as if it were an 800K disk.
    if (image_is_mfm_floppy(img->type))
        return NULL;
    GS_ASSERT(drive->track < NUM_TRACKS);

    floppy_track_t *track = &drive->tracks[sel][drive->track];

    if (track->data == NULL) {
        track->size = iwm_track_length(drive->track);
        track->data = malloc(track->size);
        if (!track->data) {
            LOG(1, "Allocation failed track=%d side=%d", drive->track, sel);
            return NULL;
        }

        GS_ASSERT(img != NULL);
        int num_sides = iwm_image_num_sides(img);
        size_t sector_count = (size_t)iwm_sectors_per_track(drive->track);
        size_t track_bytes = sector_count * 512u;
        size_t track_offset = iwm_disk_image_offset(drive->track, sel, num_sides);
        size_t disk_sz = disk_size(img);

        // Clamp to disk size for single-sided images
        if (track_offset + track_bytes > disk_sz) {
            LOG(3, "floppy: track=%d side=%d offset=%zu exceeds disk_size=%zu, using side 0", drive->track, sel,
                track_offset, disk_sz);
            track_offset = iwm_disk_image_offset(drive->track, 0, num_sides);
        }
        LOG(6, "floppy: track_data track=%d side=%d offset=%zu bytes=%zu disk_size=%zu", drive->track, sel,
            track_offset, track_bytes, disk_sz);

        uint8_t *sector_data = malloc(track_bytes);
        if (!sector_data) {
            LOG(1, "Failed to allocate sector buffer for track=%d", drive->track);
            return NULL;
        }
        size_t read = disk_read_data(img, track_offset, sector_data, track_bytes);
        if (read != track_bytes) {
            LOG(1, "disk_read_data truncated track=%d (expected=%zu got=%zu)", drive->track, track_bytes, read);
            free(sector_data);
            return NULL;
        }
        encode_track(track->data, track->size, drive->track, sel, sector_data, num_sides, img, track_offset / 512u);
        free(sector_data);
    }

    return track->data;
}

// ============================================================================
// GCR Decoding
// ============================================================================

// GCR decoding helpers
#define READ_GCR3(src, a, b, c)                                                                                        \
    do {                                                                                                               \
        uint8_t msb = decode_gcr(*(src)++);                                                                            \
        (a) = decode_gcr(*(src)++) | ((msb << 2) & 0xC0);                                                              \
        (b) = decode_gcr(*(src)++) | ((msb << 4) & 0xC0);                                                              \
        (c) = decode_gcr(*(src)++) | ((msb << 6) & 0xC0);                                                              \
    } while (0)

#define READ_GCR2(src, a, b)                                                                                           \
    do {                                                                                                               \
        uint8_t msb = decode_gcr(*(src)++);                                                                            \
        (a) = decode_gcr(*(src)++) | ((msb << 2) & 0xC0);                                                              \
        (b) = decode_gcr(*(src)++) | ((msb << 4) & 0xC0);                                                              \
    } while (0)

// Decode triplet with checksum update (writes to dst pointer)
#define DECODE_TRIPLET(src, dst, ca, cb, cc, ba, bb, bc)                                                               \
    do {                                                                                                               \
        (cc) = ((cc) << 1 & 0xFE) | ((cc) >> 7 & 1);                                                                   \
        READ_GCR3(src, ba, bb, bc);                                                                                    \
        (ca) &= 0xFF;                                                                                                  \
        *(dst)++ = (ba) = (ba) ^ (cc);                                                                                 \
        (ca) += (ba) + ((cc) & 1);                                                                                     \
        (cb) &= 0xFF;                                                                                                  \
        *(dst)++ = (bb) = (bb) ^ (ca);                                                                                 \
        (cb) += (bb) + ((ca) >> 8 & 1);                                                                                \
        (cc) &= 0xFF;                                                                                                  \
        *(dst)++ = (bc) = (bc) ^ (cb);                                                                                 \
        (cc) += (bc) + ((cb) >> 8 & 1);                                                                                \
    } while (0)

// Converts a GCR codeword to its 6-bit value, or GCR_BAD_CODEWORD.
//
// The table is 256 entries keyed on the WHOLE byte.  It used to be a lazily
// malloc'd 128 keyed on `codeword & 0x7F` -- and since every legal codeword has
// bit 7 set ($96..$FF), that aliased 64 illegal bytes onto legal values ($16 ->
// $96, $1F -> $9F, ...), defeating the very check meant to catch corrupt
// nibbles (02-floppy F-33).  The old table was also never freed and made the
// function non-reentrant.
uint8_t decode_gcr(uint8_t gcr_codeword) {
    static uint8_t decode_table[256];
    static bool built = false;
    if (!built) {
        memset(decode_table, GCR_BAD_CODEWORD, sizeof decode_table);
        for (int i = 0; i < (int)sizeof(gcr_codewords); i++)
            decode_table[gcr_codewords[i]] = (uint8_t)i;
        built = true;
    }
    return decode_table[gcr_codeword];
}

// Decodes a GCR sector back to tag and data buffers with checksum verification
// Every byte this walks is whatever the guest wrote into the track buffer
// through the IWM/SWIM data register, so NOTHING here may assert: GS_ASSERT
// prints and pauses the scheduler and then CONTINUES, so a guest that writes a
// plausible header with a bad checksum used to halt the emulator from inside a
// flush; and under GS_FAST the asserts vanish entirely and corrupt nibbles were
// written to the user's disk image as data (02-floppy F-07).
//
// Returns NULL on any malformed field, having consumed nothing the caller
// relies on; the caller skips the sector and rescans.  `end` bounds every read
// (02-floppy F-06): the old scan guard allowed 730 bytes from the mark while
// the worst case here is 817 -- 10 header bytes, a data-mark search of up to
// 100, then 4 + 16 + 680 + 3 + 4 -- so up to ~87 bytes past the end of the
// malloc'd track buffer were read on every flush.
static uint8_t *decode_sector(uint8_t *tag, uint8_t *data, uint8_t *src, const uint8_t *end, int *track_out,
                              int *side_out, int *sector_out) {
#define NEED(n)                                                                                                        \
    do {                                                                                                               \
        if (src + (n) > end)                                                                                           \
            return NULL;                                                                                               \
    } while (0)
#define GCR_OR_FAIL(dst)                                                                                               \
    do {                                                                                                               \
        NEED(1);                                                                                                       \
        uint8_t v_ = decode_gcr(*src++);                                                                               \
        if (v_ == GCR_BAD_CODEWORD)                                                                                    \
            return NULL;                                                                                               \
        (dst) = v_;                                                                                                    \
    } while (0)

    // Read and verify header marks
    NEED(3);
    if (src[0] != 0xD5 || src[1] != 0xAA || src[2] != 0x96)
        return NULL;
    src += 3;

    // Decode header fields
    uint8_t track, sector, side, format, checksum;
    GCR_OR_FAIL(track);
    GCR_OR_FAIL(sector);
    GCR_OR_FAIL(side);
    GCR_OR_FAIL(format);
    GCR_OR_FAIL(checksum);

    if (checksum != (track ^ sector ^ side ^ format))
        return NULL;

    track = (side << 6 & 0x40) | (track & 0x3F);
    side = side >> 5 & 1;

    if (track_out)
        *track_out = (int)track;
    if (side_out)
        *side_out = (int)side;
    if (sector_out)
        *sector_out = (int)sector;

    NEED(2);
    if (src[0] != 0xDE || src[1] != 0xAA)
        return NULL;
    src += 2;

    // Find the data field's mark, within the gap the encoder leaves (6 bytes
    // nominally) and never past the end of the track buffer.
    const uint8_t *search_end = src + 100;
    if (search_end > end)
        search_end = end;
    while (src + 3 <= search_end && (src[0] != 0xD5 || src[1] != 0xAA || src[2] != 0xAD))
        src++;
    if (src + 3 > search_end)
        return NULL;
    src += 3;

    uint8_t data_sector;
    GCR_OR_FAIL(data_sector);
    if (data_sector != sector)
        return NULL;

    // Everything below reads a fixed 703 six-bit values plus the 3-byte
    // checksum, so one bound covers the rest.
    NEED(16 + 680 + 3 + 4);

    // Decode data with checksum verification
    uint16_t ca = 0, cb = 0, cc = 0;
    uint8_t ba, bb, bc;

    // Decode 12-byte tag
    for (int i = 0; i < 12; i += 3)
        DECODE_TRIPLET(src, tag, ca, cb, cc, ba, bb, bc);

    // Decode 510 bytes of data
    for (int i = 0; i < 510; i += 3)
        DECODE_TRIPLET(src, data, ca, cb, cc, ba, bb, bc);

    // Decode final 2 bytes
    cc = (cc << 1 & 0xFE) | (cc >> 7 & 1);
    READ_GCR2(src, ba, bb);
    ca &= 0xFF;
    *data++ = ba = ba ^ cc;
    ca += ba + (cc & 1);
    cb &= 0xFF;
    *data++ = bb = bb ^ ca;
    cb += bb + (ca >> 8 & 1);

    // Verify checksum
    READ_GCR3(src, ba, bb, bc);
    if ((ca & 0xFF) != ba || (cb & 0xFF) != bb || (cc & 0xFF) != bc)
        return NULL;

    return src;
#undef NEED
#undef GCR_OR_FAIL
}

// === MFM sector layout (floppy_geometry.h) ==================================

#define MFM_CRC_INIT 0xFFFFu

static uint16_t mfm_crc_byte(uint16_t crc, uint8_t byte) {
    crc ^= (uint16_t)byte << 8;
    for (int i = 0; i < 8; i++)
        crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    return crc;
}

void floppy_mfm_emit_sector(floppy_mfm_emit_fn emit, void *ctx, int track, int side, int sector, const uint8_t *data,
                            int gap3_len, bool emit_crc) {
    if (!emit)
        return;

    // Address field: 12 x $00 sync, 3 x $A1 (marked), $FE, C/H/S/N, CRC.
    for (int i = 0; i < 12; i++)
        emit(ctx, 0x00, false);
    for (int i = 0; i < 3; i++)
        emit(ctx, 0xA1, true);
    emit(ctx, 0xFE, false);
    const uint8_t hdr[4] = {(uint8_t)track, (uint8_t)side, (uint8_t)sector, 0x02}; // 0x02 = 512-byte sectors
    for (int i = 0; i < 4; i++)
        emit(ctx, hdr[i], false);
    if (emit_crc) {
        uint16_t crc = MFM_CRC_INIT;
        crc = mfm_crc_byte(crc, 0xA1);
        crc = mfm_crc_byte(crc, 0xFE);
        for (int i = 0; i < 4; i++)
            crc = mfm_crc_byte(crc, hdr[i]);
        emit(ctx, (uint8_t)(crc >> 8), false);
        emit(ctx, (uint8_t)(crc & 0xFF), false);
    }

    // Gap 2, then the data field: sync, 3 x $A1 (marked), $FB, 512 bytes, CRC.
    for (int i = 0; i < 22; i++)
        emit(ctx, 0x4E, false);
    for (int i = 0; i < 12; i++)
        emit(ctx, 0x00, false);
    for (int i = 0; i < 3; i++)
        emit(ctx, 0xA1, true);
    emit(ctx, 0xFB, false);
    for (int i = 0; i < 512; i++)
        emit(ctx, data[i], false);
    if (emit_crc) {
        uint16_t crc = MFM_CRC_INIT;
        crc = mfm_crc_byte(crc, 0xA1);
        crc = mfm_crc_byte(crc, 0xFB);
        for (int i = 0; i < 512; i++)
            crc = mfm_crc_byte(crc, data[i]);
        emit(ctx, (uint8_t)(crc >> 8), false);
        emit(ctx, (uint8_t)(crc & 0xFF), false);
    }

    // Gap 3 (inter-sector).
    for (int i = 0; i < gap3_len; i++)
        emit(ctx, 0x4E, false);
}

// Per-sector write-through for the GCR path.
//
// The ISM path (ism_write_capture_flush) and SWIM3 (swim3_write_sector) both
// write a sector to the image the moment it completes.  The GCR path did not:
// guest writes accumulated in the heap track buffer and reached the image only
// on EJECT or machine teardown (iwm_flush_modified_tracks).  Same subsystem,
// three write paths, two policies, and no recorded rationale -- the deferral
// dates to the original SE/30 commit and was never revisited.
//
// What it cost: a crash, a kill or a closed browser tab lost every floppy write
// since insertion, while the identical operation on a SCSI disk survived (both
// sit on the same delta-file storage engine and are checkpointed the same way);
// and the image was stale while the disk was mounted, which matters because the
// IIfx/Q900 IOP reads the same image directly.
//
// The legitimate part of the old argument is that you cannot write through per
// BYTE -- the guest supplies GCR nibbles one at a time and nothing is decodable
// until a whole sector's data field has arrived.  That justifies buffering to
// SECTOR granularity, which is what this does, and is exactly what the ISM path
// already does.
//
// Detection is cheap because the on-disk form is self-delimiting: a sector is
// D5 AA 96, five header nibbles, DE AA, a gap, D5 AA AD, 704 six-bit values,
// DE AA.  We remember where the last address mark passed under the head, and on
// each DE AA try to decode from there.  decode_sector already validates every
// field and bounds every read, so a partial or malformed sector simply fails
// and nothing is written -- matching the ISM path's refusal to flush an
// incomplete sector.
//
// The track buffer still exists and is still checkpointed: nibbles that never
// complete a sector (a format in progress, copy-protection patterns that
// deliberately do not form valid sectors) have no representation in the image
// at all, which is why "just flush at checkpoint time" is not a substitute.
void iwm_write_through(floppy_drive_t *drive, image_t *img, int drive_index, int side) {
    if (!img || !img->writable)
        return;
    floppy_track_t *t = &drive->tracks[side][drive->track];
    if (!t->data || t->size == 0)
        return;

    int pos = drive->offset; // one past the byte just written
    if (pos < 3 || (size_t)pos > t->size)
        return;

    const uint8_t *d = t->data;

    // An address mark just passed: remember where this sector starts.
    if (d[pos - 3] == 0xD5 && d[pos - 2] == 0xAA && d[pos - 1] == 0x96) {
        drive->write_hdr_start = pos - 3;
        return;
    }

    // A field just ended.  If it is the data field of the sector we are
    // tracking, it is now complete.
    if (!(d[pos - 2] == 0xDE && d[pos - 1] == 0xAA))
        return;
    int start = drive->write_hdr_start;
    if (start < 0 || start >= pos)
        return;

    uint8_t tag[12];
    uint8_t buf[512];
    int hdr_track = 0, hdr_side = 0, hdr_sector = 0;
    if (!decode_sector(tag, buf, t->data + start, t->data + pos, &hdr_track, &hdr_side, &hdr_sector))
        return; // header field only, or a partial/corrupt sector: nothing to do

    drive->write_hdr_start = -1;

    int num_sides = iwm_image_num_sides(img);
    if (hdr_track < 0 || hdr_track >= NUM_TRACKS || hdr_side < 0 || hdr_side >= NUM_SIDES || hdr_sector < 0 ||
        hdr_sector >= iwm_sectors_per_track(hdr_track))
        return;
    size_t off = iwm_disk_image_offset(hdr_track, hdr_side, num_sides) + (size_t)hdr_sector * 512u;
    if (off + 512 > disk_size(img))
        return;

    disk_write_data(img, off, buf, 512);
    disk_write_tag(img, off / 512u, tag, sizeof tag);
    t->modified = false; // this sector is in the image now
    LOG(5, "Drive %d: Wrote through track=%d side=%d sector=%d", drive_index, hdr_track, hdr_side, hdr_sector);
}

// Writes any modified GCR tracks back to the underlying disk image
void iwm_flush_modified_tracks(floppy_drive_t *drive, image_t *img, int drive_index) {
    if (!img) {
        LOG(12, "Drive %d: Flush skipped - no disk present", drive_index);
        return;
    }

    bool any_flushed = false;

    for (int side = 0; side < NUM_SIDES; ++side) {
        for (int tr = 0; tr < NUM_TRACKS; ++tr) {
            floppy_track_t *t = &drive->tracks[side][tr];
            if (!t->data || !t->modified)
                continue; // nothing to flush
            if (!any_flushed) {
                LOG(3, "Drive %d: Flushing modified tracks", drive_index);
                any_flushed = true;
            }
            LOG(4, "Drive %d: Flush track %d side %d (size=%zu)", drive_index, tr, side, t->size);

            // Respect write-protect: do not modify underlying image
            if (!img->writable) {
                LOG(2, "Drive %d: Skip flush track %d side %d - write-protected", drive_index, tr, side);
                t->modified = false;
                continue;
            }

            uint8_t *p = t->data;
            uint8_t *end = t->data + t->size;
            int num_sides = iwm_image_num_sides(img);

            // decode_sector bounds every read against `end` and returns
            // NULL on anything malformed, so the scan only has to find a
            // plausible mark.  The old guard was `p + 730 < end`, ~87 bytes
            // short of the decoder's 817-byte worst case (02-floppy F-06).
            while (p + 3 <= end) {
                if (p[0] == 0xD5 && p[1] == 0xAA && p[2] == 0x96) {
                    uint8_t tag[12];
                    uint8_t buf[512];
                    int hdr_track = 0, hdr_side = 0, hdr_sector = 0;
                    uint8_t *next = decode_sector(tag, buf, p, end, &hdr_track, &hdr_side, &hdr_sector);
                    if (!next) {
                        // Guest-written bytes that do not form a sector: skip
                        // and rescan.  Level 4 because a formatter in progress
                        // produces these legitimately.
                        LOG(4, "Drive %d: Skip undecodable sector at +%zu", drive_index, (size_t)(p - t->data));
                        p++;
                        continue;
                    }

                    // hdr_sector comes from a 6-bit GCR nibble the guest wrote,
                    // so it is 0..63 while a track holds at most 12 sectors.
                    // Unchecked, a header claiming sector 40 wrote 20 KB past
                    // the start of its own track, into neighbouring tracks'
                    // data -- arbitrary corruption of a mounted writable image
                    // from one track write (02-floppy F-13).
                    if (hdr_track >= 0 && hdr_track < NUM_TRACKS && hdr_side >= 0 && hdr_side < NUM_SIDES &&
                        hdr_sector >= 0 && hdr_sector < iwm_sectors_per_track(hdr_track)) {
                        size_t off = iwm_disk_image_offset(hdr_track, hdr_side, num_sides) + (size_t)hdr_sector * 512u;
                        if (off + 512 <= disk_size(img)) {
                            disk_write_data(img, off, buf, 512);
                            // The 12 GCR tag bytes carry HFS/MFS scavenger
                            // metadata; SWIM3 round-trips them and this path
                            // used to decode them into a local and throw them
                            // away (02-floppy F-14).
                            disk_write_tag(img, off / 512u, tag, sizeof tag);
                            LOG(5, "Drive %d: Write sector track=%d side=%d sector=%d", drive_index, hdr_track,
                                hdr_side, hdr_sector);
                        }
                    }

                    p = (next > p) ? next : p + 1;
                } else {
                    p++;
                }
            }

            LOG(4, "Drive %d: Track %d side %d flush complete", drive_index, tr, side);
            // Track flushed
            t->modified = false;
        }
    }

    if (any_flushed) {
        LOG(3, "Drive %d: Flush complete", drive_index);
    }
}
