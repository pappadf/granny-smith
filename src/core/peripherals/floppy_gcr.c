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
    size_t length[] = {9320, 8559, 7780, 6994, 6224};

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

// Determines the number of sides based on disk image type
int iwm_image_num_sides(image_t *img) {
    if (!img)
        return 2; // Default to double-sided if unknown
    // Single-sided 400KB disk has type image_fd_ss
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

// GCR encoding helper macros
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
static void encode_track(uint8_t *dst, size_t trk_length, int track, int side, const uint8_t *data, int num_sides) {
    GS_ASSERT(data != NULL);

    int i;
    int num_sectors = iwm_sectors_per_track(track);
    uint8_t *end_of_track = dst + trk_length;

#define NUM_SPEED_GROUPS 5

    // [7]: sectors are typically interleaved 2:1 because of the write recovery time
    // sector sequencing for 2:1 interleave is:
    int interleave[NUM_SPEED_GROUPS][12] = {0, 6,  1, 7,  2, 8,  3,  9,  4, 10, 5, 11, 0, 6, 1,  7,  2,  8,  3,  9,
                                            4, 10, 5, -1, 0, 5,  1,  6,  2, 7,  3, 8,  4, 9, -1, -1, 0,  5,  1,  6,
                                            2, 7,  3, 8,  4, -1, -1, -1, 0, 4,  1, 5,  2, 6, 3,  7,  -1, -1, -1, -1};

    // just assume an empty tag for now
    uint8_t tag[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    // go through all sectors in track (note: "i" is not the sector number)
    for (i = 0; i < num_sectors; i++) {

        int sector = interleave[track >> 4][i];
        GS_ASSERT(sector != -1);

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
    // HD disks use MFM encoding, not GCR — reject them so the ROM
    // falls through to the ISM (SWIM) read path
    if (img->type == image_fd_hd)
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
        encode_track(track->data, track->size, drive->track, sel, sector_data, num_sides);
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

// Converts a GCR codeword to its 6-bit value using a lookup table
uint8_t decode_gcr(uint8_t gcr_codeword) {
    static uint8_t *decode_table = NULL;

    if (decode_table == NULL) {
        decode_table = (uint8_t *)malloc(128);
        GS_ASSERT(decode_table != NULL);
        memset(decode_table, 0xFF, 128);
        for (int i = 0; i < (int)sizeof(gcr_codewords); i++)
            decode_table[gcr_codewords[i] & 0x7F] = i;
    }

    GS_ASSERT(decode_table[gcr_codeword & 0x7F] < 64);
    return decode_table[gcr_codeword & 0x7F];
}

// Decodes a GCR sector back to tag and data buffers with checksum verification
static uint8_t *decode_sector(uint8_t *tag, uint8_t *data, uint8_t *src, int *track_out, int *side_out,
                              int *sector_out) {
    // Read and verify header marks
    GS_ASSERT(*src++ == 0xD5);
    GS_ASSERT(*src++ == 0xAA);
    GS_ASSERT(*src++ == 0x96);

    // Decode header fields
    uint8_t track = decode_gcr(*src++);
    uint8_t sector = decode_gcr(*src++);
    uint8_t side = decode_gcr(*src++);
    uint8_t format = decode_gcr(*src++);
    uint8_t checksum = decode_gcr(*src++);

    GS_ASSERT(checksum == (track ^ sector ^ side ^ format));

    track = (side << 6 & 0x40) | (track & 0x3F);
    side = side >> 5 & 1;

    if (track_out)
        *track_out = (int)track;
    if (side_out)
        *side_out = (int)side;
    if (sector_out)
        *sector_out = (int)sector;

    GS_ASSERT(*src++ == 0xDE);
    GS_ASSERT(*src++ == 0xAA);

    // Find data field marks
    uint8_t *end = src + 100;
    while (src < end && (src[0] != 0xD5 || src[1] != 0xAA || src[2] != 0xAD))
        src++;
    GS_ASSERT(src < end);
    src += 3;

    GS_ASSERT(decode_gcr(*src++) == sector);

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
    GS_ASSERT((ca & 0xFF) == ba);
    GS_ASSERT((cb & 0xFF) == bb);
    GS_ASSERT((cc & 0xFF) == bc);

    return src;
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
            size_t disk_sz = disk_size(img);

            while (p + 730 < end) { // require enough space for a sector
                if (p[0] == 0xD5 && p[1] == 0xAA && p[2] == 0x96) {
                    // Validate header checksum before attempting full decode.
                    // MacTest writes test patterns that may corrupt the data
                    // field while leaving headers intact; skip those sectors.
                    uint8_t h_trk = decode_gcr(p[3]);
                    uint8_t h_sec = decode_gcr(p[4]);
                    uint8_t h_sid = decode_gcr(p[5]);
                    uint8_t h_fmt = decode_gcr(p[6]);
                    uint8_t h_chk = decode_gcr(p[7]);
                    if (h_chk != (h_trk ^ h_sec ^ h_sid ^ h_fmt)) {
                        p++;
                        continue; // header checksum bad → skip
                    }
                    // Find data field marks (D5 AA AD) within next 100 bytes
                    uint8_t *dscan = p + 8;
                    uint8_t *dlimit = dscan + 100;
                    if (dlimit > end)
                        dlimit = end;
                    while (dscan + 2 < dlimit && (dscan[0] != 0xD5 || dscan[1] != 0xAA || dscan[2] != 0xAD))
                        dscan++;
                    if (dscan + 2 >= dlimit || dscan[0] != 0xD5) {
                        p++;
                        continue; // no data field → skip
                    }
                    // Verify sector number in data field matches header
                    if (decode_gcr(dscan[3]) != h_sec) {
                        LOG(4,
                            "Drive %d: Skip corrupt sector track=%d side=%d sector=%d "
                            "(data field mismatch)",
                            drive_index, (h_sid << 6 & 0x40) | (h_trk & 0x3F), h_sid >> 5 & 1, h_sec);
                        p++;
                        continue;
                    }

                    // Found a valid header; decode this sector to temp buffers
                    uint8_t tag[12];
                    uint8_t buf[512];
                    int hdr_track = 0, hdr_side = 0, hdr_sector = 0;
                    uint8_t *next = decode_sector(tag, buf, p, &hdr_track, &hdr_side, &hdr_sector);

                    // Sanity-check header values and that they match loop indices
                    if (hdr_track >= 0 && hdr_track < NUM_TRACKS && hdr_side >= 0 && hdr_side < NUM_SIDES) {
                        // Use original 2-side calculation, but for single-sided disks
                        // map side 1 writes to side 0 if offset would exceed disk size
                        size_t off = iwm_disk_image_offset(hdr_track, hdr_side, 2) + (size_t)hdr_sector * 512u;
                        if (off + 512 > disk_sz && num_sides == 1) {
                            off = iwm_disk_image_offset(hdr_track, 0, 1) + (size_t)hdr_sector * 512u;
                        }
                        if (off + 512 <= disk_size(img)) {
                            disk_write_data(img, off, buf, 512);
                            LOG(5, "Drive %d: Write sector track=%d side=%d sector=%d", drive_index, hdr_track,
                                hdr_side, hdr_sector);
                        }
                    }

                    // Advance pointer
                    if (next > p)
                        p = next;
                    else
                        p++;
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
