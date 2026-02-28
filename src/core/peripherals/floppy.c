// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// floppy.c
// Implements the IWM (Integrated Woz Machine) floppy disk controller and GCR encoding/decoding for Granny Smith.

#include "floppy.h"
#include "iwm_swim.h"
#include "log.h"
#include "memory.h"
#include "platform.h"
#include "system.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"

LOG_USE_CATEGORY_NAME("floppy");

// Size of plain-data portion for checkpointing (excludes pointers at end)
#define FLOPPY_CHECKPOINT_SIZE offsetof(floppy_t, disk)

// GCR codeword table as described in [7]
const uint8_t gcr_codewords[] = {0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6, 0xA7, 0xAB, 0xAC, 0xAD, 0xAE,
                                 0xAF, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE,
                                 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3, 0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD,
                                 0xDE, 0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xF2,
                                 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF};

// Represents the IWM floppy disk controller with drives and state
struct floppy {
    // ---- Plain data (checkpointed with single call) ----
    uint8_t iwm_lines; // packed IWM state lines (CA0-Q7)
    uint8_t mode; // IWM mode register
    bool sel; // VIA-driven SEL signal (head select)
    floppy_drive_t drives[NUM_DRIVES];
    // ---- End of plain data ----

    // ---- Pointers (excluded from checkpoint) ----
    image_t *disk[NUM_DRIVES]; // disk image pointers
    struct scheduler *scheduler; // scheduler for timed events
    memory_interface_t memory_interface;
};

// ============================================================================
// Shared Utility Functions (parameterized, used by both floppy.c and swim.c)
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

// GCR encoding/decoding helper macros
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
uint8_t *encode_sector(uint8_t *dst, const uint8_t *tag, const uint8_t *data, int track, int sector, int side) {
    GS_ASSERT(data != NULL);

    uint16_t ca = 0, cb = 0, cc = 0; // checksum registers
    uint8_t ba, bb, bc; // encoded bytes
    uint8_t format = 0x22;

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
void encode_track(uint8_t *dst, size_t trk_length, int track, int side, const uint8_t *data) {
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

        dst = encode_sector(dst, tag, data + sector * 512, track, sector, side);
    }

    GS_ASSERT(dst < end_of_track);

    // fill out the rest of the track
    while (dst < end_of_track)
        *dst++ = 0xFF; // sync bytes
}

// Returns a pointer to the GCR data for the specified drive/image/side, encoding on demand
uint8_t *iwm_track_data(floppy_drive_t *drive, image_t *img, int sel, struct scheduler *scheduler) {
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
        size_t track_offset = iwm_disk_image_offset(drive->track, sel, 2);
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
        encode_track(track->data, track->size, drive->track, sel, sector_data);
        free(sector_data);
    }

    return track->data;
}

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
                    // Found a header; decode this sector to temp buffers
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

// ============================================================================
// IWM-specific Static Functions (not shared with swim.c)
// ============================================================================

// Forward declaration for motor spin-up callback (used by disk_control)
static void motor_spinup_callback(void *source, uint64_t data);

// Returns pointer to the currently selected drive based on IWM SELECT line
static floppy_drive_t *current_drive(floppy_t *floppy) {
    return &floppy->drives[IWM_SELECT(floppy) ? 1 : 0];
}

// Returns the current disk status based on IWM CA lines and SEL signal
static int disk_status(floppy_t *floppy) {
    floppy_drive_t *drive = current_drive(floppy);
    int drv = DRIVE_INDEX(floppy);

    // Build status key from CA lines and VIA SEL signal
    int key = (IWM_CA0(floppy) ? 0x01 : 0) | (IWM_CA1(floppy) ? 0x02 : 0) | (IWM_CA2(floppy) ? 0x04 : 0) |
              (floppy->sel ? 0x08 : 0);

    int ret = 0;
    const char *desc = "unknown";

    switch (key) {
    case 0x00: // /DIRTN
        desc = "/DIRTN";
        ret = !drive->_dirtn;
        break;
    case 0x01: // /STEP
        desc = "/STEP";
        ret = 1;
        break;
    case 0x02: // /MOTORON: when low, motor is turned on
        desc = "/MOTORON";
        ret = !drive->_motoron;
        break;
    case 0x03: // EJECT
        desc = "EJECT";
        ret = 0;
        break;
    case 0x04: // RDDATA: data from side 0
        desc = "RDDATA side0";
        ret = 1;
        break;
    case 0x05: // reserved
        desc = "reserved (CA2+CA0)";
        ret = 0;
        break;
    case 0x06: // /SIDES
        desc = "/SIDES";
        ret = 1; // drive supports double-sided
        break;
    case 0x07: // /DRVIN
        desc = "/DRVIN";
        ret = 0; // drive connected
        break;
    case 0x08: // /CSTIN: zero when disk in drive
        desc = "/CSTIN";
        ret = (floppy->disk[drv] == NULL);
        break;
    case 0x09: // /WRTPRT: zero when write protected
        desc = "/WRTPRT";
        ret = (floppy->disk[drv] != NULL) ? floppy->disk[drv]->writable : 0;
        break;
    case 0x0A: // /TKO: zero when head on track 0
        desc = "/TKO";
        ret = (drive->track != 0);
        break;
    case 0x0B: // /TACH: 60 pulses per revolution
    {
        const char *tach_reason = NULL;
        ret = iwm_tach_signal(floppy->scheduler, drive, &tach_reason);
        LOG(6, "Drive %d: Reading /TACH = %d (%s, track=%d)", drv, ret, tach_reason, drive->track);
        return ret;
    }
    case 0x0C: // RDDATA: data from side 1
        desc = "RDDATA side1";
        ret = 1;
        break;
    case 0x0D: // reserved (switch to GCR mode)
        desc = "reserved (SEL+CA2+CA0)";
        ret = 0;
        break;
    case 0x0E: // /READY: zero when ready
        desc = "/READY";
        ret = drive->motor_spinning_up ? 1 : 0;
        break;
    case 0x0F: // NEWINTF
        desc = "NEWINTF";
        ret = 1; // 800K drive
        break;
    default:
        ret = 0;
        break;
    }

    LOG(6, "Drive %d: Reading %s = %d", drv, desc, ret);
    LOG(8, "  detail: key=0x%02X ca0=%d ca1=%d ca2=%d sel=%d dirtn=%d motoron=%d track=%d", key, IWM_CA0(floppy),
        IWM_CA1(floppy), IWM_CA2(floppy), floppy->sel, drive->_dirtn ? 1 : 0, drive->_motoron ? 1 : 0, drive->track);

    return ret;
}

// Processes disk control commands when LSTRB is high
static void disk_control(floppy_t *floppy) {
    floppy_drive_t *drive = current_drive(floppy);
    int drv = DRIVE_INDEX(floppy);

    // Commands only when SEL is low
    if (floppy->sel)
        return;

    if (IWM_CA0(floppy)) {
        if (IWM_CA1(floppy)) {
            // EJECT (CA0=1, CA1=1, CA2=1)
            if (IWM_CA2(floppy)) {
                LOG(1, "Drive %d: Eject requested", drv);
                iwm_flush_modified_tracks(drive, floppy->disk[drv], drv);
                memset(drive->tracks, 0, sizeof(drive->tracks));
                floppy->disk[drv] = NULL;
                LOG(1, "Drive %d: Ejected", drv);
            }
        } else {
            // STEP (CA0=1, CA1=0, CA2=0)
            if (!IWM_CA2(floppy)) {
                // _dirtn: 0=inward (higher tracks), 1=outward (lower tracks)
                if (drive->_dirtn) {
                    if (drive->track > 0)
                        drive->track--;
                } else {
                    if (drive->track < NUM_TRACKS - 1)
                        drive->track++;
                }
                drive->offset = 0;
                LOG(3, "Drive %d: Step to track %d (%s)", drv, drive->track, drive->_dirtn ? "outward" : "inward");
            }
        }
    } else {
        if (IWM_CA1(floppy)) {
            // MOTORON (CA0=0, CA1=1): CA2 sets motor state
            bool was_off = drive->_motoron;
            drive->_motoron = IWM_CA2(floppy);
            bool now_on = !drive->_motoron;

            if (was_off && now_on) {
                drive->motor_spinning_up = true;
                remove_event(floppy->scheduler, motor_spinup_callback, floppy);
                scheduler_new_cpu_event(floppy->scheduler, motor_spinup_callback, floppy, (uint64_t)drv, 0,
                                        MOTOR_SPINUP_TIME_NS);
                LOG(2, "Drive %d: Motor ON (spinning up)", drv);
            } else if (!was_off && !now_on) {
                drive->motor_spinning_up = false;
                remove_event(floppy->scheduler, motor_spinup_callback, floppy);
                LOG(2, "Drive %d: Motor OFF", drv);
            } else {
                LOG(4, "Drive %d: Motor %s (no change)", drv, drive->_motoron ? "off" : "on");
            }
        } else {
            // DIRTN (CA0=0, CA1=0): CA2 sets direction
            drive->_dirtn = IWM_CA2(floppy);
            LOG(4, "Drive %d: Direction = %s", drv, drive->_dirtn ? "outward" : "inward");
        }
    }
}

// Callback invoked when motor spin-up delay completes
static void motor_spinup_callback(void *source, uint64_t data) {
    floppy_t *floppy = (floppy_t *)source;
    int drive_index = (int)data;

    if (drive_index < 0 || drive_index >= NUM_DRIVES) {
        LOG(1, "Drive %d: Invalid drive in spin-up callback", drive_index);
        return;
    }

    floppy->drives[drive_index].motor_spinning_up = false;
    LOG(3, "Drive %d: Motor spin-up complete, now ready", drive_index);
}

// Updates IWM state lines based on address offset (even=clear, odd=set)
static void update_iwm_state_lines(floppy_t *floppy, int offset) {
    // Map offset pair to bit mask: offset/2 gives line index (0-7)
    static const uint8_t line_masks[] = {IWM_LINE_CA0,    IWM_LINE_CA1,    IWM_LINE_CA2, IWM_LINE_LSTRB,
                                         IWM_LINE_ENABLE, IWM_LINE_SELECT, IWM_LINE_Q6,  IWM_LINE_Q7};
    static const char *offset_names[16] = {"CA0 low",    "CA0 high",  "CA1 low",         "CA1 high",
                                           "CA2 low",    "CA2 high",  "LSTRB low",       "LSTRB high",
                                           "ENABLE off", "ENABLE on", "SELECT internal", "SELECT external",
                                           "Q6 low",     "Q6 high",   "Q7 low",          "Q7 high"};

    uint8_t mask = line_masks[offset >> 1];
    if (offset & 1)
        floppy->iwm_lines |= mask; // odd offset = set line
    else
        floppy->iwm_lines &= ~mask; // even offset = clear line

    const char *name = (offset >= 0 && offset < 16) ? offset_names[offset] : "unknown";
    LOG(6, "Drive %d: %s", IWM_SELECT(floppy), name);

    if (IWM_LSTRB(floppy))
        disk_control(floppy);
}

// Reads from the IWM register at the specified offset
uint8_t iwm_read(floppy_t *floppy, uint32_t offset) {
    GS_ASSERT(offset < 16);
    update_iwm_state_lines(floppy, offset);

    int drv = DRIVE_INDEX(floppy);

    // Mode register is WRITE ONLY
    if (IWM_Q6(floppy) && IWM_Q7(floppy))
        GS_ASSERT(0);

    // Read status register: Q6=1, Q7=0
    if (IWM_Q6(floppy) && !IWM_Q7(floppy)) {
        uint8_t status = floppy->mode & IWM_STATUS_MODE;
        if (IWM_ENABLE(floppy))
            status |= IWM_STATUS_ENABLE;
        if (disk_status(floppy))
            status |= IWM_STATUS_SENSE;
        LOG(8, "  status=0x%02X (enable=%d mode=0x%02X)", status, IWM_ENABLE(floppy) ? 1 : 0,
            floppy->mode & IWM_STATUS_MODE);
        return status;
    }

    // Read handshake register: Q6=0, Q7=1
    if (!IWM_Q6(floppy) && IWM_Q7(floppy)) {
        uint8_t hdshk = IWM_HDSHK_RES | IWM_HDSHK_WRITE | IWM_HDSHK_WB_EMTPY;
        LOG(6, "Drive %d: Reading handshake = 0x%02X", drv, hdshk);
        return hdshk;
    }

    // Read data register: Q6=0, Q7=0
    if (!IWM_Q6(floppy) && !IWM_Q7(floppy)) {
        if (!(floppy->mode & IWM_MODE_ASYNC)) {
            LOG(6, "Drive %d: Sync read mode not implemented = 0x00", drv);
            return 0;
        }

        if (!IWM_ENABLE(floppy)) {
            LOG(6, "Drive %d: Reading data (disabled) = 0xFF", drv);
            return 0xFF;
        }

        if (floppy->disk[drv] == NULL) {
            LOG(6, "Drive %d: Reading data (no disk) = 0x00", drv);
            return 0x00;
        }

        floppy_drive_t *drive = current_drive(floppy);
        uint8_t *data = iwm_track_data(drive, floppy->disk[drv], floppy->sel, floppy->scheduler);
        if (!data) {
            LOG(1, "Drive %d: Read failed - no track data", drv);
            return 0xFF;
        }

        size_t trk_len = iwm_track_length(drive->track);

        // IWM latch mode: only bytes with MSB=1 are latched (valid GCR bytes)
        if (floppy->mode & IWM_MODE_LATCH) {
            for (size_t i = 0; i < trk_len; i++) {
                uint8_t byte = data[drive->offset++];
                if (drive->offset >= (int)trk_len)
                    drive->offset = 0;
                if (byte & 0x80) {
                    LOG(8, "Drive %d: Reading data track=%d side=%d offset=%d = 0x%02X", drv, drive->track, floppy->sel,
                        drive->offset, byte);
                    return byte;
                }
                LOG(9, "Drive %d: Skipping MSB=0 byte 0x%02X at offset %d", drv, byte, drive->offset - 1);
            }
            LOG(6, "Drive %d: No valid GCR byte found on track", drv);
            return 0x00;
        }

        // Non-latch mode: return raw bytes
        uint8_t ret = data[drive->offset++];
        if (drive->offset >= (int)trk_len)
            drive->offset = 0;
        LOG(8, "Drive %d: Reading data track=%d side=%d offset=%d = 0x%02X", drv, drive->track, floppy->sel,
            drive->offset, ret);
        return ret;
    }

    GS_ASSERT(0);
    return 0;
}

// Writes a byte to the IWM register at the specified offset
static void iwm_write(floppy_t *floppy, uint32_t offset, uint8_t byte) {
    update_iwm_state_lines(floppy, offset);

    if (!IWM_Q6(floppy) || !IWM_Q7(floppy))
        return;

    int drv = DRIVE_INDEX(floppy);

    if (IWM_ENABLE(floppy)) {
        // Write data to disk
        floppy_drive_t *drive = current_drive(floppy);

        uint8_t *data = iwm_track_data(drive, floppy->disk[drv], floppy->sel, floppy->scheduler);
        if (!data) {
            LOG(1, "Drive %d: Write failed - no track data", drv);
            return;
        }

        data[drive->offset++] = byte;
        LOG(8, "Drive %d: Writing data track=%d side=%d offset=%d = 0x%02X", drv, drive->track, floppy->sel,
            drive->offset - 1, byte);

        floppy_track_t *trk = &drive->tracks[floppy->sel][drive->track];
        if (!trk->modified) {
            trk->modified = true;
            LOG(5, "Drive %d: Track %d side %d marked dirty", drv, drive->track, floppy->sel);
        }

        if (drive->offset == (int)iwm_track_length(drive->track))
            drive->offset = 0;
    } else {
        // Write mode register
        floppy->mode = byte;
        LOG(4, "IWM: Mode register = 0x%02X", byte);
    }
}

// ============================================================================
// Memory Interface (Mac Plus address decoding)
// ============================================================================

// Memory interface handler for 8-bit reads from IWM address space
static uint8_t read_uint8(void *floppy, uint32_t addr) {
    floppy_t *s = (floppy_t *)floppy;

    // [3]: The IWM is on the lower byte of the data bus, so use odd-addressed byte accesses only
    GS_ASSERT(addr & 1);

    // [5]: A1-A4 of the IWM are connected to A9-A12 of the CPU bus
    addr >>= 9;

    return iwm_read(s, addr & 0x0F);
}

// Memory interface handler for 16-bit reads (not supported)
static uint16_t read_uint16(void *floppy, uint32_t addr) {
    GS_ASSERT(0);

    return 0;
}

// Memory interface handler for 32-bit reads (not supported)
static uint32_t read_uint32(void *floppy, uint32_t addr) {
    GS_ASSERT(0);

    return 0;
}

// Memory interface handler for 8-bit writes to IWM address space
static void write_uint8(void *floppy, uint32_t addr, uint8_t value) {
    floppy_t *s = (floppy_t *)floppy;

    // [3]: The IWM is on the lower byte of the data bus, so use odd-addressed byte accesses only
    GS_ASSERT(addr & 1);

    // [5]: A1-A4 of the IWM are connected to A9-A12 of the CPU bus
    addr >>= 9;

    iwm_write(s, addr & 0x0F, value);
}

// Memory interface handler for 16-bit writes (not supported)
static void write_uint16(void *floppy, uint32_t addr, uint16_t value) {
    GS_ASSERT(0);
}

// Memory interface handler for 32-bit writes (not supported)
static void write_uint32(void *floppy, uint32_t addr, uint32_t value) {
    GS_ASSERT(0);
}

// ============================================================================
// Public API
// ============================================================================

// Sets the VIA-driven SEL signal for head selection
void floppy_set_sel_signal(floppy_t *floppy, bool sel) {
    if (!floppy) {
        LOG(4, "SEL signal -> %s (deferred, no floppy)", sel ? "high" : "low");
        return;
    }
    LOG(6, "SEL signal: %s -> %s", floppy->sel ? "high" : "low", sel ? "high" : "low");
    floppy->sel = sel;
}

// Inserts a disk image into the specified drive
int floppy_insert(floppy_t *floppy, int drive, image_t *disk) {
    GS_ASSERT(drive < NUM_DRIVES);

    if (floppy->disk[drive] != NULL) {
        LOG(2, "Drive %d: Insert failed - disk already present", drive);
        return -1;
    }

    floppy->disk[drive] = disk;

    current_drive(floppy)->offset = 0;
    const char *name = disk ? image_get_filename(disk) : NULL;
    LOG(1, "Drive %d: Inserted disk '%s' (writable=%d)", drive, name ? name : "<unnamed>", disk ? disk->writable : 0);

    return 0;
}

// Returns whether a disk is currently inserted in the specified drive
bool floppy_is_inserted(floppy_t *floppy, int drive) {
    GS_ASSERT(drive < NUM_DRIVES);

    return floppy->disk[drive] != NULL;
}

// ============================================================================
// Lifecycle (Init / Delete / Checkpoint)
// ============================================================================

// Initializes the floppy controller and maps it to memory
floppy_t *floppy_init(memory_map_t *map, struct scheduler *scheduler, checkpoint_t *checkpoint) {
    floppy_t *floppy = malloc(sizeof(floppy_t));
    if (!floppy) {
        LOG(1, "Floppy: Allocation failed");
        return NULL;
    }

    memset(floppy, 0, sizeof(floppy_t));
    LOG(2, "Floppy: Controller created");

    floppy->scheduler = scheduler;
    scheduler_new_event_type(scheduler, "floppy", floppy, "motor_spinup", &motor_spinup_callback);

    floppy->memory_interface.read_uint8 = &read_uint8;
    floppy->memory_interface.read_uint16 = &read_uint16;
    floppy->memory_interface.read_uint32 = &read_uint32;
    floppy->memory_interface.write_uint8 = &write_uint8;
    floppy->memory_interface.write_uint16 = &write_uint16;
    floppy->memory_interface.write_uint32 = &write_uint32;

    memory_map_add(map, 0x00d80000, 0x00080000, "floppy", &floppy->memory_interface, floppy);

    if (checkpoint) {
        LOG(3, "Floppy: Restoring from checkpoint");

        // Clear track data pointers before restoring (they will be overwritten)
        for (int d = 0; d < NUM_DRIVES; d++)
            for (int s = 0; s < NUM_SIDES; s++)
                for (int t = 0; t < NUM_TRACKS; t++)
                    floppy->drives[d].tracks[s][t].data = NULL;

        // Read plain-data portion
        system_read_checkpoint_data(checkpoint, floppy, FLOPPY_CHECKPOINT_SIZE);

        // Restore disk images by filename
        for (int i = 0; i < NUM_DRIVES; i++) {
            floppy->disk[i] = NULL;
            uint32_t len = 0;
            system_read_checkpoint_data(checkpoint, &len, sizeof(len));
            if (len > 0) {
                char *name = malloc(len);
                if (name) {
                    system_read_checkpoint_data(checkpoint, name, len);
                    floppy->disk[i] = setup_get_image_by_filename(name);
                    free(name);
                } else {
                    // Consume bytes to keep stream aligned
                    for (uint32_t k = 0; k < len; k++) {
                        char tmp;
                        system_read_checkpoint_data(checkpoint, &tmp, 1);
                    }
                }
            }
        }

        // Restore GCR track buffers
        for (int d = 0; d < NUM_DRIVES; d++) {
            for (int s = 0; s < NUM_SIDES; s++) {
                for (int t = 0; t < NUM_TRACKS; t++) {
                    floppy_track_t *trk = &floppy->drives[d].tracks[s][t];
                    uint8_t has_data = 0;
                    system_read_checkpoint_data(checkpoint, &has_data, 1);
                    if (has_data && trk->size > 0) {
                        trk->data = malloc(trk->size);
                        if (trk->data)
                            system_read_checkpoint_data(checkpoint, trk->data, trk->size);
                        else {
                            // Consume bytes to keep stream aligned
                            for (size_t k = 0; k < trk->size; k++) {
                                uint8_t tmp;
                                system_read_checkpoint_data(checkpoint, &tmp, 1);
                            }
                            trk->size = 0;
                            trk->modified = false;
                        }
                    }
                }
            }
        }
    }

    return floppy;
}

// Frees all resources associated with the floppy controller
void floppy_delete(floppy_t *floppy) {
    if (!floppy)
        return;
    LOG(2, "Floppy: Deleting controller");
    // Free all track data for both drives
    for (int d = 0; d < NUM_DRIVES; d++) {
        for (int s = 0; s < NUM_SIDES; s++) {
            for (int t = 0; t < NUM_TRACKS; t++) {
                free(floppy->drives[d].tracks[s][t].data);
            }
        }
    }
    free(floppy);
}

// Saves the floppy controller state to a checkpoint
void floppy_checkpoint(floppy_t *restrict floppy, checkpoint_t *checkpoint) {
    if (!floppy || !checkpoint)
        return;
    LOG(13, "Floppy: Checkpointing controller");

    // Write plain-data portion (iwm_lines, mode, sel, drives)
    system_write_checkpoint_data(checkpoint, floppy, FLOPPY_CHECKPOINT_SIZE);

    // Write disk filenames for each drive
    for (int i = 0; i < NUM_DRIVES; i++) {
        const char *name = floppy->disk[i] ? image_get_filename(floppy->disk[i]) : NULL;
        uint32_t len = (name && *name) ? (uint32_t)strlen(name) + 1 : 0;
        system_write_checkpoint_data(checkpoint, &len, sizeof(len));
        if (len)
            system_write_checkpoint_data(checkpoint, name, len);
    }

    // Save GCR track buffers (preserves in-flight write data)
    for (int d = 0; d < NUM_DRIVES; d++) {
        for (int s = 0; s < NUM_SIDES; s++) {
            for (int t = 0; t < NUM_TRACKS; t++) {
                floppy_track_t *trk = &floppy->drives[d].tracks[s][t];
                uint8_t has_data = (trk->data != NULL);
                system_write_checkpoint_data(checkpoint, &has_data, 1);
                if (has_data)
                    system_write_checkpoint_data(checkpoint, trk->data, trk->size);
            }
        }
    }
}
