// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// floppy_gcr.c
// GCR encoding/decoding and shared utility functions for the floppy subsystem.
// These functions are used by both IWM and SWIM code paths.

#include "floppy_internal.h"

#include "codec.h"
#include "gs_assert.h"
#include "image_capture.h"
#include "log.h"
#include "memory.h"

#include <assert.h>
#include <errno.h>
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

bool floppy_media_block_unreadable(image_t *img, uint64_t block) {
    // A guest write makes the sector readable on the real disk, whatever it
    // wrote: the block is then in the delta.
    return image_has_capture(img) && image_sector_status(img, block) != IMAGE_SECTOR_OK &&
           !image_block_in_delta(img, block);
}

bool floppy_media_read_sector(const floppy_media_t *m, int track, int side, int sector, uint8_t *data) {
    if (!m || !m->valid || sector < 0 || sector >= floppy_media_spt(m, track) || side < 0 || side >= m->sides)
        return false;
    size_t off = floppy_media_sector_offset(m, track, side, sector);
    if (off + FLOPPY_SECTOR_BYTES > disk_size(m->img))
        return false;
    // A sector the capture could not read fails as it does on the real
    // drive, rather than reading as the zeros the sector view holds.
    if (!m->mfm && floppy_media_block_unreadable(m->img, off / FLOPPY_SECTOR_BYTES))
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

// Encodes an entire track with interleaved sectors to GCR format.  `data`
// and `tags` (NULL: zeros) are the track's sectors, 512 and 12 bytes each.
// `bad` has bit n set for each sector n that reads unreadable -- one the
// image could not read, or one the capture could not -- and those are laid
// down with a bad data checksum (encode_sector).
static void encode_track(uint8_t *dst, size_t trk_length, int track, int side, const uint8_t *data, const uint8_t *tags,
                         int num_sides, uint32_t bad) {
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
        if (tags)
            memcpy(tag, tags + (size_t)sector * sizeof tag, sizeof tag);

        dst = encode_sector(dst, tag, data + sector * 512, track, sector, side, num_sides, (bad >> sector) & 1u);
    }

    GS_ASSERT(dst < end_of_track);

    // fill out the rest of the track
    while (dst < end_of_track)
        *dst++ = 0xFF; // sync bytes
}

// ============================================================================
// Tracks: floppy_media_track and the IWM's cache over it
// ============================================================================
//
// A drive's track buffer holds the disk bytes the head passes over, one
// revolution, looping.  floppy_media_track() makes them, and owns the rule
// for what they are:
//
//   1. An image with no capture (or a track the capture's disk does not
//      have, side 1 of a 400K disk): the track is laid down from the image's
//      sectors as a Mac formats it (encode_track).
//   2. A captured track whose sectors the image still holds as the capture
//      decodes them (image_capture_track_diff): the captured revolution
//      itself, framed in steady state -- gaps, marks and fields that are no
//      sectors included.
//   3. Some sectors differ: the captured revolution with each of those
//      sectors' data fields replaced by a fresh one (track_splice), as a
//      drive rewrites a sector after finding its address field.
//   4. That is impossible (a sector with no data field in the capture, a
//      reformat, a track that would outgrow the capture by more than 2%):
//      laid down from the sectors, as 1.
//
// The rule reads only the capture and the image's current contents, so the
// same disk contents give the same track whichever way the image got there
// -- a guest write, an eject's flush, either kind of checkpoint restore, a
// reinsert.  The one exception: whether a sector the capture could not read
// has been written at all, which the image's delta bitmap answers (a guest
// writing zeros there leaves the content unchanged but the medium readable).

// Disk bytes from an address field's D5 to the end of it: D5 AA 96, five
// values, DE AA.
#define ADDR_FIELD_NIBS 10
// A data field: D5 AA AD, the sector number, 703 values, DE AA.
#define DATA_FIELD_NIBS (3 + 1 + GCR_SECTOR_VALUES + 2)

size_t floppy_track_len(const floppy_drive_t *drive, int side, int track) {
    if (!drive || track < 0 || track >= NUM_TRACKS || side < 0 || side >= NUM_SIDES)
        return iwm_track_length(0);
    const floppy_track_t *t = &drive->tracks[side][track];
    // `size` is checkpointed plain data: without a buffer it may be stale.
    return (t->data && t->size) ? t->size : iwm_track_length(track);
}

// A track's sectors as the image holds them now, and what is known of each.
typedef struct {
    int nsec;
    int num_sides;
    uint64_t first_block; // the track's first sector, in image order
    uint32_t unreadable; // bit k: the image could not read sector k
    uint32_t capture_bad; // bit k: the capture could not read sector k
    uint32_t in_delta; // bit k: sector k is in the image's delta
    uint8_t data[12 * 512];
    uint8_t tags[12 * GCR_TAG_BYTES];
} track_sectors_t;

// Read (track, side)'s sectors and tags from the image.
static void track_sectors_read(image_t *img, const image_capture_t *cap, int num_sides, int track, int side,
                               track_sectors_t *ts) {
    memset(ts, 0, sizeof *ts);
    ts->nsec = iwm_sectors_per_track(track);
    ts->num_sides = num_sides;
    size_t track_bytes = (size_t)ts->nsec * 512u;
    size_t track_offset = iwm_disk_image_offset(track, side, num_sides);
    size_t disk_sz = (size_t)disk_size(img); // a floppy: far under 4 GB
    // Clamp to disk size for single-sided images
    if (track_offset + track_bytes > disk_sz) {
        LOG(3, "floppy: track=%d side=%d offset=%zu exceeds disk_size=%zu, using side 0", track, side, track_offset,
            disk_sz);
        track_offset = iwm_disk_image_offset(track, 0, num_sides);
    }
    LOG(6, "floppy: track_data track=%d side=%d offset=%zu bytes=%zu disk_size=%zu", track, side, track_offset,
        track_bytes, disk_sz);
    ts->first_block = track_offset / 512u;
    // A track the image cannot read whole is read a sector at a time: the
    // sectors that still fail are laid down unreadable (a bad data
    // checksum), as a damaged disk presents them, and the rest read.
    if (disk_read_data(img, track_offset, ts->data, track_bytes) != track_bytes) {
        for (int s = 0; s < ts->nsec; s++) {
            uint8_t *at = ts->data + (size_t)s * 512u;
            if (disk_read_data(img, track_offset + (size_t)s * 512u, at, 512) != 512) {
                memset(at, 0, 512);
                ts->unreadable |= 1u << s;
            }
        }
        LOG(1, "floppy: track=%d side=%d: unreadable sectors mask $%03X", track, side, ts->unreadable);
    }
    // The 12 GCR tag bytes carry the HFS/MFS scavenger metadata.  This path
    // once synthesised zeros, so a DiskCopy 4.2 image with tags lost them on
    // any read through the IWM/SWIM path.
    for (int s = 0; s < ts->nsec; s++) {
        disk_read_tag(img, ts->first_block + (uint64_t)s, ts->tags + (size_t)s * GCR_TAG_BYTES, GCR_TAG_BYTES);
        if (!cap)
            continue;
        if (image_capture_sector_status(cap, ts->first_block + (uint64_t)s) != IMAGE_SECTOR_OK)
            ts->capture_bad |= 1u << s;
        if (image_block_in_delta(img, ts->first_block + (uint64_t)s))
            ts->in_delta |= 1u << s;
    }
}

// Lay the track down from its sectors (rules 1 and 4), at the zone's length.
// A sector the capture could not read, and nothing has written since, is
// laid down unreadable, as the original disk has it.
static int track_synthesise(int track, int side, const track_sectors_t *ts, uint8_t **out, size_t *len) {
    size_t n = iwm_track_length(track);
    uint8_t *buf = malloc(n);
    if (!buf)
        return -ENOMEM;
    uint32_t bad = ts->unreadable | (ts->capture_bad & ~ts->in_delta);
    encode_track(buf, n, track, side, ts->data, ts->tags, ts->num_sides, bad);
    *out = buf;
    *len = n;
    return 0;
}

// Rule 3: the captured revolution `nib` (n bytes) with the data field of
// every sector in `diff` re-encoded from the image.  Each replaced region
// runs from the end of the sector's address field through the DE AA that
// closes its data field: the gap after the address field becomes sync of
// the same length (what the drive writes ahead of the mark, so a signature
// left there by the duplicator goes as on a real rewrite), then a standard
// data field.  Everything else -- other sectors, gaps, the address field
// itself -- stays as captured.  False when a sector has no data field to
// replace, when a region would take an address field with it (its own
// sector's data field is not where the scan put its end), when two regions
// overlap, or when the result would outgrow the capture by more than 2%: a
// track no drive could hold.
static bool track_splice(const uint8_t *nib, size_t n, const moof_sector_t *sec, uint32_t diff, int track, int side,
                         const track_sectors_t *ts, uint8_t **out, size_t *len) {
    struct {
        size_t start, end, gap;
        int sector;
    } r[12];
    int nr = 0;
    size_t out_len = n;
    for (int k = 0; k < ts->nsec; k++) {
        if (!((diff >> k) & 1))
            continue;
        const moof_sector_t *f = &sec[k];
        if (!f->addr || f->data_at == MOOF_NO_FIELD || f->data_end == MOOF_NO_FIELD)
            return false; // nothing a drive would find to rewrite
        size_t start = (size_t)f->addr_at + ADDR_FIELD_NIBS, data_at = f->data_at, end = f->data_end;
        if (data_at < start || end <= data_at || end - start > n)
            return false;
        if (start >= n) { // the address field itself straddles the index
            start -= n;
            data_at -= n;
            end -= n;
        }
        for (size_t p = start; p + 3 <= end; p++)
            if (nib[p % n] == 0xD5 && nib[(p + 1) % n] == 0xAA && nib[(p + 2) % n] == 0x96)
                return false; // another sector's address field

        // Keep the regions sorted by where they start.
        int at = nr++;
        while (at > 0 && r[at - 1].start > start) {
            r[at] = r[at - 1];
            at--;
        }
        r[at].start = start;
        r[at].end = end;
        r[at].gap = data_at - start;
        r[at].sector = k;
        out_len += r[at].gap + DATA_FIELD_NIBS; // the new field and its sync...
        out_len -= end - start; // ...in place of the old
    }
    if (!nr || out_len * 50 > n * 51)
        return false; // nothing to splice, or the track outgrows the capture by more than 2%
    // Regions must not overlap, the last one included across the index.
    for (int i = 0; i < nr; i++) {
        size_t next = i + 1 < nr ? r[i + 1].start : r[0].start + n;
        if (r[i].end > next)
            return false;
    }
    // A region that runs across the index moves the seam to its end.
    size_t origin = r[nr - 1].end > n ? r[nr - 1].end - n : 0;
    uint8_t *buf = malloc(out_len);
    if (!buf)
        return false;
    size_t o = 0, pos = origin;
    uint8_t values[GCR_SECTOR_VALUES];
    for (int i = 0; i < nr; i++) {
        while (pos < r[i].start)
            buf[o++] = nib[pos++ % n];
        // Sync over the gap, then a data field laid down as encode_sector does.
        memset(buf + o, 0xFF, r[i].gap);
        o += r[i].gap;
        int k = r[i].sector;
        buf[o++] = 0xD5, buf[o++] = 0xAA, buf[o++] = 0xAD;
        buf[o++] = gcr_encode6[k & 0x3F];
        gcr_sector_nibblize(ts->tags + (size_t)k * GCR_TAG_BYTES, ts->data + (size_t)k * 512u,
                            (ts->unreadable >> k) & 1, values);
        for (int v = 0; v < GCR_SECTOR_VALUES; v++)
            buf[o++] = gcr_encode6[values[v] & 0x3F];
        buf[o++] = 0xDE, buf[o++] = 0xAA;
        pos = r[i].end;
    }
    while (pos < origin + n)
        buf[o++] = nib[pos++ % n];
    GS_ASSERT(o == out_len);
    LOG(3, "floppy: track=%d side=%d: %d changed sector(s) spliced into the capture (%zu -> %zu bytes)", track, side,
        nr, n, out_len);
    *out = buf;
    *len = out_len;
    return true;
}

int floppy_media_track(const floppy_media_t *m, int track, int side, uint8_t **out, size_t *len) {
    *out = NULL;
    *len = 0;
    if (!m || !m->valid || m->mfm || !m->img || track < 0 || track >= NUM_TRACKS || side < 0 || side >= NUM_SIDES)
        return -EINVAL;
    image_capture_t *cap = image_capture(m->img);
    track_sectors_t ts;
    track_sectors_read(m->img, cap, m->sides, track, side, &ts);
    if (cap && side < image_capture_sides(cap)) {
        moof_sector_t sec[MOOF_MAX_SECTORS];
        uint8_t *nib = NULL;
        size_t n = image_capture_track(cap, track, side, &nib, sec);
        uint32_t diff = image_capture_track_diff(sec, ts.nsec, ts.data, ts.tags, ts.in_delta);
        uint32_t all = (1u << ts.nsec) - 1;
        if (diff == 0) {
            // Rule 2.  A track the capture holds no bits for is a gap all
            // round: sync, at the zone's length.
            if (!n) {
                n = iwm_track_length(track);
                nib = malloc(n);
                if (!nib)
                    return -ENOMEM;
                memset(nib, 0xFF, n);
            }
            LOG(4, "floppy: track=%d side=%d: served from the capture (%zu bytes)", track, side, n);
            *out = nib;
            *len = n;
            return 0;
        }
        // Rule 3, unless every sector changed (a reformat).
        bool spliced = diff != all && n && track_splice(nib, n, sec, diff, track, side, &ts, out, len);
        free(nib);
        if (spliced)
            return 0;
        LOG(3, "floppy: track=%d side=%d: sectors $%03X differ from the capture; laid down from the image", track, side,
            diff);
    }
    return track_synthesise(track, side, &ts, out, len);
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
        floppy_media_t m;
        uint8_t *buf = NULL;
        size_t len = 0;
        if (!floppy_media_from_image(img, &m) || floppy_media_track(&m, drive->track, sel, &buf, &len) != 0) {
            LOG(1, "Track build failed track=%d side=%d", drive->track, sel);
            return NULL;
        }
        track->data = buf;
        track->size = len;
        track->modified = false;
    }

    // The head's position is the drive's and the length the track's: tracks
    // differ in length (a capture's are its own), so a position carried from
    // another track or side, or restored from a checkpoint, is brought inside
    // this one before anything indexes with it.
    if (drive->offset < 0)
        drive->offset = 0;
    if ((size_t)drive->offset >= track->size)
        drive->offset = (int)((size_t)drive->offset % track->size);
    if (drive->write_hdr_start >= (int)track->size)
        drive->write_hdr_start = -1;

    return track->data;
}

// The GCR track an inspection sees.  A guest read builds the track's encoding
// on first touch and caches it; doing that for a peek would store the cache,
// a side effect a peek must not have.  So a peek sees the cached encoding
// where the guest has already built one.  Otherwise, on a disk with a
// capture, it sees the track floppy_media_track() would build, made into a
// scratch buffer of its own (the capture's tracks are not the sync a gap
// reads as); this reads the image, which counts toward its activity light.
// On any other disk it sees an all-sync ($FF) track: what the head passes
// over in a gap, and a value no sector decode will mistake for data.  NULL
// exactly where iwm_track_data returns NULL for the media (none, or MFM).
const uint8_t *iwm_track_data_peek(const floppy_drive_t *drive, image_t *img, int sel, size_t *len) {
    static uint8_t sync_track[9320]; // iwm_track_length's longest zone
    // The last track a peek built, and what it was built from.
    static struct {
        const image_t *img;
        uint32_t crc;
        int track, side;
        uint64_t writes;
        uint8_t *buf;
        size_t len;
    } scratch;
    if (!img || image_layout_is_mfm(image_layout(img)) || drive->track < 0 || drive->track >= NUM_TRACKS)
        return NULL;
    const floppy_track_t *track = &drive->tracks[sel][drive->track];
    if (track->data) {
        *len = track->size;
        return track->data;
    }
    image_capture_t *cap = image_has_capture(img) ? image_capture(img) : NULL;
    if (cap) {
        uint32_t crc = image_capture_crc(cap);
        uint64_t writes = image_get_writes(img);
        if (scratch.buf && scratch.img == img && scratch.crc == crc && scratch.track == drive->track &&
            scratch.side == sel && scratch.writes == writes) {
            *len = scratch.len;
            return scratch.buf;
        }
        floppy_media_t m;
        uint8_t *buf = NULL;
        size_t n = 0;
        if (floppy_media_from_image(img, &m) && floppy_media_track(&m, drive->track, sel, &buf, &n) == 0) {
            free(scratch.buf);
            scratch.img = img;
            scratch.crc = crc;
            scratch.track = drive->track;
            scratch.side = sel;
            scratch.writes = writes;
            scratch.buf = buf;
            scratch.len = n;
            *len = n;
            return buf;
        }
    }
    if (sync_track[0] != 0xFF)
        memset(sync_track, 0xFF, sizeof sync_track);
    *len = iwm_track_length(drive->track);
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
// track buffer were read on every flush.  (The write-through is its one
// caller now; the eject flush reads a track with moof_scan_track.)
#define SECTOR_SPAN_MAX 820 // the most it reads from an address mark: 817

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
    if (!t->data || t->size < 3)
        return;

    // The buffer is one revolution, looping: a sector may start before the
    // index and end after it, so every position here is taken modulo its
    // length.
    size_t n = t->size;
    int pos = drive->offset; // one past the byte just written
    if (pos < 1 || (size_t)pos > n)
        return;
    const uint8_t *d = t->data;
    size_t p = (size_t)pos;
    uint8_t b3 = d[(p + n - 3) % n], b2 = d[(p + n - 2) % n], b1 = d[p - 1];

    // An address mark just passed: remember where this sector starts.
    if (b3 == 0xD5 && b2 == 0xAA && b1 == 0x96) {
        drive->write_hdr_start = (int)((p + n - 3) % n);
        return;
    }

    // A field just ended.  If it is the data field of the sector we are
    // tracking, it is now complete.
    if (!(b2 == 0xDE && b1 == 0xAA))
        return;
    int start = drive->write_hdr_start;
    if (start < 0 || (size_t)start >= n)
        return;
    // From the address mark to here, laid out in a line: past the index
    // when the head crossed it in between.
    size_t span = (p + n - (size_t)start) % n;
    if (span == 0 || span > SECTOR_SPAN_MAX)
        return;
    uint8_t line[SECTOR_SPAN_MAX];
    for (size_t k = 0; k < span; k++)
        line[k] = d[((size_t)start + k) % n];

    uint8_t tag[12];
    uint8_t buf[512];
    int hdr_track = 0, hdr_side = 0, hdr_sector = 0;
    if (!decode_sector(tag, buf, line, line + span, &hdr_track, &hdr_side, &hdr_sector))
        return; // header field only, or a partial/corrupt sector: nothing to do

    drive->write_hdr_start = -1;

    // A sector is written where it lies: on this track and side.  A header
    // naming another (a duplicator's, a protection's) would otherwise put
    // this track's bytes into that one's sector.  hdr_side is also checked
    // against the medium: a single-sided image has no side-1 block, and
    // iwm_disk_image_offset ignores the side there, so a side-1 header would
    // overwrite side 0 of the same track.
    int num_sides = iwm_image_num_sides(img);
    if (hdr_track != drive->track || hdr_side != side || hdr_side >= num_sides || hdr_sector < 0 ||
        hdr_sector >= iwm_sectors_per_track(hdr_track)) {
        LOG(4, "Drive %d: sector written on track %d side %d names track %d side %d sector %d: not written through",
            drive_index, drive->track, side, hdr_track, hdr_side, hdr_sector);
        return;
    }
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

            // A side the medium does not have holds no sector to write back
            // (a 400K disk's side 1: its blocks are side 0's).
            int num_sides = iwm_image_num_sides(img);
            if (side >= num_sides) {
                LOG(4, "Drive %d: Skip flush track %d side %d - the disk has no side %d", drive_index, tr, side, side);
                t->modified = false;
                continue;
            }

            // The track's sectors as the one sector scan finds them
            // (moof_scan_track): the buffer loops, so a sector running across
            // the index -- every captured track may have one -- is found like
            // any other; only an address field naming this track and side
            // counts, and of two naming the same sector, the first that
            // reads: the copy a capture of this track gives the image.  The
            // scan bounds every read and refuses a field with a byte that is
            // no codeword or a failing checksum (gcr_sector_from_disk), so
            // guest-written bytes that do not form a sector -- a formatter in
            // progress, a protection pattern -- are skipped.
            moof_sector_t sec[MOOF_MAX_SECTORS];
            moof_scan_track(t->data, t->size, tr, side, sec, NULL);
            size_t first = iwm_disk_image_offset(tr, side, num_sides);
            for (int k = 0; k < iwm_sectors_per_track(tr); k++) {
                // The data field repeats the sector number.
                if (sec[k].state != MOOF_SECTOR_OK || gcr_decode6[t->data[(sec[k].data_at + 3) % t->size]] != k) {
                    LOG(4, "Drive %d: track %d side %d sector %d does not read; not written", drive_index, tr, side, k);
                    continue;
                }
                size_t off = first + (size_t)k * 512u;
                if (off + 512 > disk_size(img))
                    continue;
                disk_write_data(img, off, sec[k].data, 512);
                // The 12 GCR tag bytes carry HFS/MFS scavenger metadata; SWIM3
                // round-trips them and this path used to decode them into a
                // local and throw them away.
                disk_write_tag(img, off / 512u, sec[k].tag, sizeof sec[k].tag);
                LOG(5, "Drive %d: Write sector track=%d side=%d sector=%d", drive_index, tr, side, k);
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
