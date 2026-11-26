// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// floppy_gcr.c
// GCR encoding/decoding and shared utility functions for the floppy subsystem.
// These functions are used by both IWM and SWIM code paths.

#include "floppy_internal.h"

#include "codec.h"
#include "gs_assert.h"
#include "log.h"
#include "memory.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"

LOG_USE_CATEGORY_NAME("floppy");

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
// size -- the only classifier that was ever right (this is swim3_media's
// version, promoted).
// Fills in everything that follows from a format: side count, sectors per
// track, the header's format byte, and the MFM/GCR framing flag.
void floppy_media_apply_format(floppy_media_t *m, floppy_format_t format) {
    m->format = format;
    m->mfm = floppy_format_is_mfm(format);
    switch (format) {
    case FLOPPY_FMT_MFM_1440K:
        m->sides = 2;
        m->mfm_spt = 18;
        m->fmt_byte = 0x02; // MFM size code 2 = 512-byte sectors
        break;
    case FLOPPY_FMT_MFM_720K:
        m->sides = 2;
        m->mfm_spt = 9;
        m->fmt_byte = 0x02;
        break;
    case FLOPPY_FMT_GCR_800K:
        m->sides = 2;
        m->mfm_spt = 0;
        m->fmt_byte = 0x22; // 0x22 = double-sided GCR
        break;
    case FLOPPY_FMT_GCR_400K:
    default:
        m->sides = 1;
        m->mfm_spt = 0;
        m->fmt_byte = 0x02;
        break;
    }
}

bool floppy_media_from_image(image_t *img, floppy_media_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!img)
        return false;
    out->img = img;
    switch (image_layout(img)) {
    case IMAGE_LAYOUT_MFM1440:
        out->hd = true; // the only HD class we model
        floppy_media_apply_format(out, FLOPPY_FMT_MFM_1440K);
        break;
    case IMAGE_LAYOUT_MFM720:
        floppy_media_apply_format(out, FLOPPY_FMT_MFM_720K);
        break;
    case IMAGE_LAYOUT_GCR800:
        floppy_media_apply_format(out, FLOPPY_FMT_GCR_800K);
        break;
    case IMAGE_LAYOUT_GCR400:
        floppy_media_apply_format(out, FLOPPY_FMT_GCR_400K);
        break;
    default:
        out->img = NULL;
        return false;
    }
    out->valid = true;
    return true;
}

bool floppy_media_current(struct floppy *floppy, unsigned drive, floppy_media_t *out) {
    if (!floppy_media_from_image(floppy_drive_image(floppy, drive), out))
        return false;
    // The image gives the class and a starting guess at the format; the drive
    // overrides the format once something has actually written one.
    int known = floppy_drive_format(floppy, drive);
    if (known >= 0)
        floppy_media_apply_format(out, (floppy_format_t)known);
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

bool floppy_media_read_sector(const floppy_media_t *m, int track, int side, int sector, uint8_t *data) {
    if (!m || !m->valid || sector < 0 || sector >= floppy_media_spt(m, track) || side < 0 || side >= m->sides)
        return false;
    size_t off = floppy_media_sector_offset(m, track, side, sector);
    if (off + FLOPPY_SECTOR_BYTES > disk_size(m->img))
        return false;
    return disk_read_data(m->img, off, data, FLOPPY_SECTOR_BYTES) == FLOPPY_SECTOR_BYTES;
}

bool floppy_media_write_sector(const floppy_media_t *m, int track, int side, int sector, uint8_t *data) {
    if (!m || !m->valid || sector < 0 || sector >= floppy_media_spt(m, track) || side < 0 || side >= m->sides)
        return false;
    size_t off = floppy_media_sector_offset(m, track, side, sector);
    if (off + FLOPPY_SECTOR_BYTES > disk_size(m->img) || !image_is_writable(m->img))
        return false;
    return disk_write_data(m->img, off, data, FLOPPY_SECTOR_BYTES) == FLOPPY_SECTOR_BYTES;
}

double floppy_media_rev_ns(const floppy_media_t *m, int track, int dd_mfm_rpm) {
    int rpm = floppy_zone_rpm(track);
    if (m->mfm)
        rpm = m->format == FLOPPY_FMT_MFM_720K ? dd_mfm_rpm : FLOPPY_MFM_RPM;
    return 60.0e9 / (double)rpm;
}

int floppy_media_next_header(const floppy_media_t *m, int track, double rev_ns, double now_ns, double *delay_ns) {
    int spt = floppy_media_spt(m, track);
    double slot = rev_ns / (double)spt;
    double n = floor(now_ns / slot + 1e-3) + 1.0;
    *delay_ns = n * slot - now_ns;
    return (int)fmod(n, (double)spt);
}

// Determines the number of sides based on disk image type
int iwm_image_num_sides(image_t *img) {
    if (!img)
        return 2; // Default to double-sided if unknown
    // Only the 400K disk is single-sided; 720K, 800K and 1440K are not.
    return (image_layout(img) == IMAGE_LAYOUT_GCR400) ? 1 : 2;
}

// Calculates TACH signal state (60 pulses per revolution) based on current time and motor speed
int iwm_tach_signal(struct scheduler *scheduler, floppy_drive_t *drive, double pwm_rpm, const char **reason) {
    // TACH produces 60 pulses per revolution; motor not spinning = no pulses
    // motoron is active-low: true = motor OFF, false = motor ON
    if (drive->motoron) {
        if (reason)
            *reason = "motor off";
        return 1;
    }

    double now_ns = scheduler_time_ns(scheduler);
    // A PWM-driven spindle turns at the commanded speed, a self-regulating
    // one at its track zone's
    double rpm = pwm_rpm > 0 ? pwm_rpm : (double)iwm_track_rpm(drive->track);
    double ns_per_rev = (60.0 / rpm) * 1e9;

    if (reason)
        *reason = pwm_rpm > 0 ? "pwm" : "calculated";
    return floppy_index_signal(FLOPPY_INDEX_GCR_TACH, now_ns, ns_per_rev, 60);
}

// ============================================================================
// GCR Encoding
// ============================================================================

// Lay `n` six-bit values down as their codewords.
static uint8_t *put_gcr(uint8_t *dst, const uint8_t *v6, int n) {
    for (int i = 0; i < n; i++)
        *dst++ = gcr_encode6[v6[i] & 0x3F];
    return dst;
}

// Lay `n` raw (unencoded) bytes down.
static uint8_t *put_raw(uint8_t *dst, const uint8_t *raw, int n) {
    memcpy(dst, raw, (size_t)n);
    return dst + n;
}

// Encodes a sector to GCR format with header and data fields.  `bad_data`
// lays the data field down with a checksum that does not match it: what an
// unreadable sector on a real disk gives the controller's reader (the Sony
// driver's badDCksum), so a sector the backing image cannot read fails the
// guest's read of it -- and only of it.
static uint8_t *encode_sector(uint8_t *dst, const uint8_t *tag, const uint8_t *data, int track, int sector, int side,
                              int num_sides, bool bad_data) {
    GS_ASSERT(data != NULL);

    static const uint8_t sync[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    static const uint8_t addr_mark[3] = {0xD5, 0xAA, 0x96}, data_mark[3] = {0xD5, 0xAA, 0xAD};
    static const uint8_t slip[3] = {0xDE, 0xAA, 0xFF};
    // 0x02 = single-sided GCR, 0x22 = double-sided GCR
    uint8_t format = (num_sides > 1) ? 0x22 : 0x02;

    // Header: sync, address mark, track / sector / side / format / checksum,
    // bit-slip marks.
    uint8_t addr[GCR_ADDR_VALUES];
    gcr_addr_encode(track, sector, side, format, addr);
    dst = put_raw(dst, sync, 5);
    dst = put_raw(dst, addr_mark, 3);
    dst = put_gcr(dst, addr, GCR_ADDR_VALUES);
    dst = put_raw(dst, slip, 3);

    // Data field: sync, data mark, sector number, the 703 values of tag,
    // data and checksum, bit-slip marks.
    uint8_t sec6 = (uint8_t)sector;
    uint8_t values[GCR_SECTOR_VALUES];
    gcr_sector_nibblize(tag, data, bad_data, values);
    dst = put_raw(dst, sync, 5);
    dst = put_raw(dst, data_mark, 3);
    dst = put_gcr(dst, &sec6, 1);
    dst = put_gcr(dst, values, GCR_SECTOR_VALUES);
    dst = put_raw(dst, slip, 3);

    return dst;
}

// Encodes an entire track with interleaved sectors to GCR format
// `bad` has bit n set for each sector n the image could not read; those are
// laid down with a bad data checksum (encode_sector).
static void encode_track(uint8_t *dst, size_t trk_length, int track, int side, const uint8_t *data, int num_sides,
                         image_t *img, size_t first_block, uint32_t bad) {
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
        // a 7100, which does round-trip them.
        uint8_t tag[12];
        memset(tag, 0, sizeof tag);
        if (img)
            disk_read_tag(img, first_block + (size_t)sector, tag, sizeof tag);

        dst = encode_sector(dst, tag, data + sector * 512, track, sector, side, num_sides, (bad >> sector) & 1u);
    }

    GS_ASSERT(dst < end_of_track);

    // fill out the rest of the track
    while (dst < end_of_track)
        *dst++ = 0xFF; // sync bytes
}

// Returns a pointer to the GCR data for the specified drive/image/side, encoding on demand.
// Returns NULL for HD (MFM) images — those must be read via the ISM path.
uint8_t *iwm_track_data(floppy_drive_t *drive, image_t *img, int sel, struct scheduler *scheduler) {
    (void)scheduler;
    // No media in the drive — callers pass floppy->disk[drv] unconditionally,
    // so a probe of an empty drive lands here with img == NULL.  Treat the
    // same as "not GCR" and let the caller take its no-data branch.
    if (!img)
        return NULL;
    // MFM media is not GCR — reject it so the ROM falls through to the ISM
    // (SWIM) read path.  This used to test `== image_fd_hd`, so a 720K disk
    // (which once classified as a hard disk) was GCR-encoded from
    // MFM-laid-out bytes and handed to the IWM as if it were an 800K disk.
    if (image_layout_is_mfm(image_layout(img)))
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
        size_t disk_sz = (size_t)disk_size(img); // a floppy: far under 4 GB

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
            free(track->data);
            track->data = NULL;
            return NULL;
        }
        // A track the image cannot read whole is read a sector at a time:
        // the sectors that still fail are laid down unreadable (a bad data
        // checksum), as a damaged disk presents them, and the rest read.
        uint32_t bad = 0;
        size_t read = disk_read_data(img, track_offset, sector_data, track_bytes);
        if (read != track_bytes) {
            for (size_t s = 0; s < sector_count; s++) {
                uint8_t *at = sector_data + s * 512u;
                if (disk_read_data(img, track_offset + s * 512u, at, 512) != 512) {
                    memset(at, 0, 512);
                    bad |= 1u << s;
                }
            }
            LOG(1, "floppy: track=%d side=%d: unreadable sectors mask $%03X", drive->track, sel, bad);
        }
        encode_track(track->data, track->size, drive->track, sel, sector_data, num_sides, img, track_offset / 512u,
                     bad);
        free(sector_data);
    }

    return track->data;
}

// The GCR track an inspection sees.  A guest read builds the track's encoding
// on first touch and caches it; doing that for a peek would read the image
// (counting toward its activity light) and store the cache, both side effects
// a peek must not have.  So a peek sees the cached encoding where the guest
// has already built one, and otherwise an all-sync ($FF) track: what the
// head passes over in a gap, and a value no sector decode will mistake for
// data.  NULL exactly where iwm_track_data returns NULL for the media (none,
// or MFM).
const uint8_t *iwm_track_data_peek(const floppy_drive_t *drive, image_t *img, int sel) {
    static uint8_t sync_track[9320]; // iwm_track_length's longest zone
    if (!img || image_layout_is_mfm(image_layout(img)) || drive->track < 0 || drive->track >= NUM_TRACKS)
        return NULL;
    const floppy_track_t *track = &drive->tracks[sel][drive->track];
    if (track->data)
        return track->data;
    if (sync_track[0] != 0xFF)
        memset(sync_track, 0xFF, sizeof sync_track);
    return sync_track;
}

// ============================================================================
// GCR Decoding
// ============================================================================

// Decodes a GCR sector back to tag and data buffers with checksum verification
// Every byte this walks is whatever the guest wrote into the track buffer
// through the IWM/SWIM data register, so NOTHING here may assert: GS_ASSERT
// prints and pauses the scheduler and then CONTINUES, so a guest that writes a
// plausible header with a bad checksum used to halt the emulator from inside a
// flush; and under GS_FAST the asserts vanish entirely and corrupt nibbles were
// written to the user's disk image as data.  A byte that is no codeword is
// refused in the data field as in the header (gcr_sector_from_disk): it can
// leave the checksum intact, so the checksum alone does not catch it.
//
// Returns NULL on any malformed field, having consumed nothing the caller
// relies on; the caller skips the sector and rescans.  `end` bounds every
// read: the old scan guard allowed 730 bytes from the mark while the worst
// case here is 817 -- 10 header bytes, a data-mark search of up to 100, then
// 4 + 16 + 680 + 3 + 4 -- so up to ~87 bytes past the end of the malloc'd
// track buffer were read on every flush.
static uint8_t *decode_sector(uint8_t *tag, uint8_t *data, uint8_t *src, const uint8_t *end, int *track_out,
                              int *side_out, int *sector_out) {
#define NEED(n)                                                                                                        \
    do {                                                                                                               \
        if (src + (n) > end)                                                                                           \
            return NULL;                                                                                               \
    } while (0)

    // Read and verify header marks
    NEED(3);
    if (src[0] != 0xD5 || src[1] != 0xAA || src[2] != 0x96)
        return NULL;
    src += 3;

    // Decode header fields: every byte a codeword, and the checksum right.
    NEED(GCR_ADDR_VALUES);
    int track, sector, side;
    if (!gcr_addr_from_disk(src, &track, &sector, &side, NULL))
        return NULL;
    src += GCR_ADDR_VALUES;

    if (track_out)
        *track_out = track;
    if (side_out)
        *side_out = side;
    if (sector_out)
        *sector_out = sector;

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

    // The data field repeats the sector number.
    NEED(1);
    if (gcr_decode6[*src++] != (uint8_t)sector)
        return NULL;

    // Everything below reads a fixed 703 values (tag, data and checksum),
    // so one bound covers the rest.
    NEED(GCR_SECTOR_VALUES);
    if (gcr_sector_from_disk(src, tag, data) != 1)
        return NULL;
    src += GCR_SECTOR_VALUES;

    return src;
#undef NEED
}

// === Index / tachometer (floppy_geometry.h) =================================

int floppy_index_signal(floppy_index_mode_t mode, double now_ns, double rev_ns, int pulses_per_rev) {
    if (rev_ns <= 0.0)
        return 0;
    double pos_in_rev = fmod(now_ns, rev_ns);

    switch (mode) {
    case FLOPPY_INDEX_ISM: {
        // A short HIGH spike from the inductive sensor detecting the hub mark,
        // then a long LOW phase.  The asymmetric duty cycle is load-bearing:
        // MacTest's MEASURE_SPEED counts VIA T2 overflows during the LOW phase.
        double ns_per_cycle = rev_ns / (double)(pulses_per_rev > 0 ? pulses_per_rev : 1);
        double pos_in_cycle = fmod(pos_in_rev, ns_per_cycle);
        return (pos_in_cycle < 2.0e6) ? 1 : 0; // 2 ms HIGH
    }
    case FLOPPY_INDEX_SWIM3_MFM:
        return (pos_in_rev < rev_ns / 50.0) ? 1 : 0; // one short mark per rev
    case FLOPPY_INDEX_GCR_TACH:
    default: {
        // 60 pulses per revolution = 120 half-pulses, a square wave.
        double ns_per_half_pulse = rev_ns / 120.0;
        int half_pulse_index = (int)(pos_in_rev / ns_per_half_pulse);
        return (half_pulse_index % 2) == 0 ? 1 : 0;
    }
    }
}

// === MFM sector layout (floppy_geometry.h) ==================================

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
        // One $A1 after the seed: the ISM resets its CRC on each mark byte,
        // so only the last of the three counts (floppy_swim.c)
        const uint8_t id[6] = {0xA1, 0xFE, hdr[0], hdr[1], hdr[2], hdr[3]};
        uint16_t crc = codec_crc16_xmodem(FLOPPY_MFM_CRC_INIT, id, sizeof id);
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
        static const uint8_t mark[2] = {0xA1, 0xFB};
        uint16_t crc = codec_crc16_xmodem(FLOPPY_MFM_CRC_INIT, mark, sizeof mark);
        crc = codec_crc16_xmodem(crc, data, 512);
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
    if (!img || !image_is_writable(img))
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

    // hdr_side is checked against the medium, not NUM_SIDES: a single-sided
    // image has no side-1 block, and iwm_disk_image_offset ignores the side
    // there, so a side-1 header would overwrite side 0 of the same track.
    int num_sides = iwm_image_num_sides(img);
    if (hdr_track < 0 || hdr_track >= NUM_TRACKS || hdr_side < 0 || hdr_side >= num_sides || hdr_sector < 0 ||
        hdr_sector >= iwm_sectors_per_track(hdr_track))
        return;
    size_t off = iwm_disk_image_offset(hdr_track, hdr_side, num_sides) + (size_t)hdr_sector * 512u;
    if (off + 512 > disk_size(img))
        return;

    disk_write_data(img, off, buf, 512);
    disk_write_tag(img, off / 512u, tag, sizeof tag);
    t->modified = false; // this sector is in the image now (a later nibble write re-marks it)
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
            if (!image_is_writable(img)) {
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
            // short of the decoder's 817-byte worst case.
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
                    // from one track write.
                    // hdr_side likewise: see iwm_write_through.
                    if (hdr_track >= 0 && hdr_track < NUM_TRACKS && hdr_side >= 0 && hdr_side < num_sides &&
                        hdr_sector >= 0 && hdr_sector < iwm_sectors_per_track(hdr_track)) {
                        size_t off = iwm_disk_image_offset(hdr_track, hdr_side, num_sides) + (size_t)hdr_sector * 512u;
                        if (off + 512 <= disk_size(img)) {
                            disk_write_data(img, off, buf, 512);
                            // The 12 GCR tag bytes carry HFS/MFS scavenger
                            // metadata; SWIM3 round-trips them and this path
                            // used to decode them into a local and throw them
                            // away.
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
