// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// swim3_xfer.c
// The SWIM3 media transfer engine: address-header hunt, sector read and
// write, whole-track format, raw (copy-protect) capture, and the GCR
// nibble codec.  swim3.c is the register file and the Sony drive protocol;
// this file is what happens between GO going up and doneIntNum coming
// back.  The DMA channel it streams through is the board's, behind the
// swim3_backend_t movers (AMIC on the PDM, DBDMA channel 1 behind Grand
// Central); the engine sees only "one byte, yes or no".
//
// Level of the model — sector, not flux.  SWIM3 already does header
// parsing, sector matching, CRC and the GCR 6<->8 conversion IN HARDWARE,
// and the only thing guest software ever observes is the DMA byte stream,
// whose contents the SWIM3 ERS specifies byte for byte: an MFM read
// deposits exactly the 512 decoded data bytes, a GCR read deposits one
// sector byte followed by 703 six-bit values (12 tag + 512 data as 699
// nibbles + 4 checksum nibbles).  Synthesising flux would only mean
// decoding it again one layer down.  The cost of the choice, stated
// plainly: copy-protected media that check flux timing or non-standard
// sector layouts do not work.  (Same trade the IIfx's iop_swim.c and the
// SE/30's floppy_swim.c already make.)
//
// Rotation IS modelled, because two guest behaviours observe it: the
// driver's per-sector and per-track timeouts, and the GCR format routine,
// which self-tunes its intersector sync count by measuring rotational wrap
// with _GetMicroSeconds and fails with fmt1Err if the disk appears to spin
// implausibly fast.  The head therefore sees one sector's worth of track
// per (revolution / sectors-per-track), at 300 rpm in MFM mode and at the
// zone's rpm in GCR.
//
// Sources: Apple SWIM3 ERS v1.2 (transfer sizes, escape command table, the
// GCR converter rule, FirstSector/SectorsToXfer semantics); Apple, "Guide
// to the Macintosh Family Hardware", 2nd ed. (GCR speed zones, sector
// nibblisation, the 3-byte checksum); and the byte streams the ROM's own
// .Sony driver puts on the DMA channel, observed on this emulator.

#include "floppy.h"
#include "swim3.h"

#include "floppy.h"
#include "image.h"
#include "log.h"
#include "scheduler.h"

#include <math.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("swim3");

// The internal drive is always drive 1; PDM has no second drive.
#define FD 0u

// Bytes per sector of user data, and the GCR tag bytes that ride with it.
#define SECTOR_BYTES 512
#define TAG_BYTES    12

// The GCR data field's DMA form: one sector byte then 703 six-bit values.
#define GCR_STREAM_BYTES 704

// Hardware paces step pulses 80 us apart (SWIM3 ERS).
#define STEP_PULSE_NS 80000.0

// A whole-track operation (format, raw capture) covers one revolution.
#define TRACK_OP_REVS 1.0

// === Media geometry =========================================================

// What the disk in the drive looks like to the engine.  Everything here is
// derived from the image's size: the four Macintosh floppy capacities each
// pin an encoding, a side count and a sector layout.
typedef struct swim3_media {
    image_t *img;
    bool mfm; // MFM-formatted media (720K / 1440K); otherwise GCR
    bool hd; // 2 MB (HD) media — what sense address 15 reports
    int sides;
    int mfm_spt; // MFM sectors per track (GCR varies by zone)
    uint8_t fmt_byte; // the address field's 4th byte: MFM size code / GCR format
} swim3_media_t;

// GCR speed zones: 12 sectors on the outermost 16 tracks, one fewer per
// zone inward, with the spindle speeding up to keep the bit rate constant.
static int gcr_sectors_per_track(int track) {
    return 12 - (track >> 4);
}

static int gcr_rpm(int track) {
    static const int rpm[5] = {394, 429, 472, 525, 590};
    return rpm[(track >> 4) & 7];
}

// Fill *m from the disk currently in the drive; false when the drive is
// empty or holds something that is not a floppy geometry we can present.
static bool swim3_media(swim3_t *sw, swim3_media_t *m) {
    memset(m, 0, sizeof(*m));
    m->img = sw->fd ? floppy_drive_image(sw->fd, FD) : NULL;
    if (!m->img)
        return false;
    switch (disk_size(m->img)) {
    case 1440u * 1024u: // MFM 1.44 MB: 18 sectors/track, both sides, HD media
        m->mfm = true;
        m->hd = true;
        m->sides = 2;
        m->mfm_spt = 18;
        m->fmt_byte = 0x02; // MFM size code 2 = 512-byte sectors
        break;
    case 720u * 1024u: // MFM 720K: 9 sectors/track on DD media
        m->mfm = true;
        m->sides = 2;
        m->mfm_spt = 9;
        m->fmt_byte = 0x02;
        break;
    case 800u * 1024u: // GCR 800K: double-sided, interleave 2
        m->sides = 2;
        m->fmt_byte = 0x22;
        break;
    case 400u * 1024u: // GCR 400K: single-sided
        m->sides = 1;
        m->fmt_byte = 0x02;
        break;
    default:
        return false;
    }
    return true;
}

static int swim3_spt(const swim3_media_t *m, int track) {
    return m->mfm ? m->mfm_spt : gcr_sectors_per_track(track);
}

// Byte offset of one sector inside the image.  MFM tracks are uniform;
// GCR tracks shrink towards the spindle, so their offset accumulates.
static size_t swim3_sector_offset(const swim3_media_t *m, int track, int side, int sector) {
    if (m->mfm)
        return ((size_t)(track * m->sides + side) * (size_t)m->mfm_spt + (size_t)sector) * SECTOR_BYTES;
    size_t off = 0;
    for (int t = 0; t < track; t++)
        off += (size_t)m->sides * (size_t)gcr_sectors_per_track(t) * SECTOR_BYTES;
    if (side && m->sides > 1)
        off += (size_t)gcr_sectors_per_track(track) * SECTOR_BYTES;
    return off + (size_t)sector * SECTOR_BYTES;
}

bool swim3_media_is_hd(swim3_t *sw) {
    swim3_media_t m;
    return swim3_media(sw, &m) && m.hd;
}

// === Rotation ===============================================================

// How long one sector's worth of track takes to pass the head.
static double swim3_sector_ns(swim3_t *sw, const swim3_media_t *m, int track) {
    (void)sw;
    int rpm = m->mfm ? 300 : gcr_rpm(track);
    double rev_ns = 60.0e9 / (double)rpm;
    return rev_ns / (double)swim3_spt(m, track);
}

static double swim3_rev_ns(const swim3_media_t *m, int track) {
    return 60.0e9 / (double)(m->mfm ? 300 : gcr_rpm(track));
}

// The next address header to pass under the head: its index around the
// track, and how long until it arrives.
static int swim3_next_header(swim3_t *sw, const swim3_media_t *m, int track, double *delay_ns) {
    double now = scheduler_time_ns(sw->sched);
    double sec_ns = swim3_sector_ns(sw, m, track);
    // A slot lands ON a header boundary (swim3_arm rounds up to it); the
    // division can still come out a hair under the integer, and floor()
    // would then name the header just delivered a second time — which a
    // continuous transfer (xfer_any) would hand to the driver as the next
    // sector.  Nudge by a thousandth of a sector before flooring.
    double n = floor(now / sec_ns + 1e-3) + 1.0;
    *delay_ns = n * sec_ns - now;
    return (int)fmod(n, (double)swim3_spt(m, track));
}

// Sense address 11: the index pulse (MFM, one per revolution) or the tach
// (GCR, 60 pulses per revolution).  Both are derived from emulated time so
// software that spins on them makes progress.
int swim3_index_pulse(swim3_t *sw) {
    swim3_media_t m;
    if (!swim3_media(sw, &m) || !sw->fd || !floppy_drive_motor_on(sw->fd, FD))
        return 0;
    int track = floppy_drive_track(sw->fd, FD);
    double now = scheduler_time_ns(sw->sched);
    double rev_ns = swim3_rev_ns(&m, track);
    if (m.mfm)
        return fmod(now, rev_ns) < rev_ns / 50.0 ? 1 : 0; // a short 1/rev mark
    return ((uint64_t)(now / (rev_ns / 120.0)) & 1u) ? 1 : 0; // 60 pulses/rev
}

// === GCR nibble codec =======================================================
//
// The 6-bit values SWIM3's converter would have produced from (or consumed
// for) a sector's 12 tag + 512 data bytes: 4+170 encoded triplets, a final
// pair, and the three checksum registers — 699 + 4 = 703 values.  The
// checksum is Apple's rotate-add-xor chain; hardware verifies it and the
// driver verifies it again in software, so it must be right.

// Encode three bytes into four six-bit values, advancing the checksum.
static void gcr_encode_triplet(const uint8_t *src, uint16_t *ca, uint16_t *cb, uint16_t *cc, uint8_t *dst) {
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

// tag[12] + data[512] -> 703 six-bit values.
static void gcr_nibblize(const uint8_t *tag, const uint8_t *data, uint8_t *out) {
    uint16_t ca = 0, cb = 0, cc = 0;
    uint8_t *dst = out;
    for (int i = 0; i < TAG_BYTES; i += 3, dst += 4)
        gcr_encode_triplet(tag + i, &ca, &cb, &cc, dst);
    for (int i = 0; i < 510; i += 3, dst += 4)
        gcr_encode_triplet(data + i, &ca, &cb, &cc, dst);
    // The last two data bytes make a pair, not a triplet.
    cc = (uint16_t)((cc << 1) | ((cc >> 7) & 1));
    ca &= 0xFF;
    ca = (uint16_t)(ca + data[510] + (cc & 1));
    uint8_t ba = (uint8_t)(data[510] ^ cc);
    cb &= 0xFF;
    cb = (uint16_t)(cb + data[511] + ((ca >> 8) & 1));
    uint8_t bb = (uint8_t)(data[511] ^ ca);
    *dst++ = (uint8_t)(((ba >> 2) & 0x30) | ((bb >> 4) & 0x0C));
    *dst++ = (uint8_t)(ba & 0x3F);
    *dst++ = (uint8_t)(bb & 0x3F);
    // ...then the three checksum registers, themselves encoded as a triplet.
    uint8_t c[3] = {(uint8_t)ca, (uint8_t)cb, (uint8_t)cc};
    *dst++ = (uint8_t)(((c[0] >> 2) & 0x30) | ((c[1] >> 4) & 0x0C) | ((c[2] >> 6) & 0x03));
    *dst++ = (uint8_t)(c[0] & 0x3F);
    *dst++ = (uint8_t)(c[1] & 0x3F);
    *dst++ = (uint8_t)(c[2] & 0x3F);
}

// Decode four six-bit values back into three bytes, advancing the checksum.
static void gcr_decode_triplet(const uint8_t *src, uint16_t *ca, uint16_t *cb, uint16_t *cc, uint8_t *dst) {
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

// 703 six-bit values -> tag[12] + data[512].  Returns false when the
// three-byte checksum the stream carries disagrees with the recomputed one.
static bool gcr_denibblize(const uint8_t *in, uint8_t *tag, uint8_t *data) {
    uint16_t ca = 0, cb = 0, cc = 0;
    const uint8_t *src = in;
    for (int i = 0; i < TAG_BYTES; i += 3, src += 4)
        gcr_decode_triplet(src, &ca, &cb, &cc, tag + i);
    for (int i = 0; i < 510; i += 3, src += 4)
        gcr_decode_triplet(src, &ca, &cb, &cc, data + i);
    uint8_t ba = (uint8_t)(((src[0] << 2) & 0xC0) | (src[1] & 0x3F));
    uint8_t bb = (uint8_t)(((src[0] << 4) & 0xC0) | (src[2] & 0x3F));
    cc = (uint16_t)((cc << 1) | ((cc >> 7) & 1));
    data[510] = (uint8_t)(ba ^ cc);
    ca &= 0xFF;
    ca = (uint16_t)(ca + data[510] + (cc & 1));
    data[511] = (uint8_t)(bb ^ ca);
    cb &= 0xFF;
    cb = (uint16_t)(cb + data[511] + ((ca >> 8) & 1));
    src += 3;
    uint8_t got[3] = {(uint8_t)(((src[0] << 2) & 0xC0) | (src[1] & 0x3F)),
                      (uint8_t)(((src[0] << 4) & 0xC0) | (src[2] & 0x3F)),
                      (uint8_t)(((src[0] << 6) & 0xC0) | (src[3] & 0x3F))};
    return got[0] == (uint8_t)ca && got[1] == (uint8_t)cb && got[2] == (uint8_t)cc;
}

// === Image access ===========================================================

// Read one sector's user data (and its DiskCopy tags, when the image
// carries them) out of the image.
static bool swim3_read_sector(const swim3_media_t *m, int track, int side, int sector, uint8_t *data, uint8_t *tag) {
    size_t off = swim3_sector_offset(m, track, side, sector);
    if (off + SECTOR_BYTES > disk_size(m->img))
        return false;
    if (disk_read_data(m->img, off, data, SECTOR_BYTES) != SECTOR_BYTES)
        return false;
    if (tag) {
        memset(tag, 0, TAG_BYTES);
        disk_read_tag(m->img, off / SECTOR_BYTES, tag, TAG_BYTES);
    }
    return true;
}

static bool swim3_write_sector(const swim3_media_t *m, int track, int side, int sector, uint8_t *data,
                               const uint8_t *tag) {
    size_t off = swim3_sector_offset(m, track, side, sector);
    if (off + SECTOR_BYTES > disk_size(m->img) || !m->img->writable)
        return false;
    if (disk_write_data(m->img, off, data, SECTOR_BYTES) != SECTOR_BYTES)
        return false;
    if (tag)
        disk_write_tag(m->img, off / SECTOR_BYTES, tag, TAG_BYTES);
    return true;
}

// === DMA helpers ============================================================

// Push one byte to the DMA channel; false stops the transfer (the channel
// was closed, or its window points outside RAM — an underrun on hardware).
static bool dma_put(swim3_t *sw, uint8_t v) {
    return sw->be.dma_put(sw->be.ctx, v);
}

static bool dma_get(swim3_t *sw, uint8_t *v) {
    return sw->be.dma_get(sw->be.ctx, v);
}

// === Sector read ============================================================

// Stream one sector's data field into the DMA window in the form the ERS
// specifies for the current encoding, then the Gap register's worth of pad
// requests (the driver writes 0, so normally none).
static bool swim3_stream_read(swim3_t *sw, const swim3_media_t *m, int track, int side, int sector) {
    uint8_t data[SECTOR_BYTES], tag[TAG_BYTES];
    if (!swim3_read_sector(m, track, side, sector, data, tag)) {
        sw->error |= SWIM3_E_CRC_DATA; // unreadable sector reads as a bad CRC
        return false;
    }

    if (m->mfm) {
        for (int i = 0; i < SECTOR_BYTES; i++)
            if (!dma_put(sw, data[i])) {
                sw->error |= SWIM3_E_OVERRUN;
                return false;
            }
    } else {
        uint8_t nib[GCR_STREAM_BYTES - 1];
        gcr_nibblize(tag, data, nib);
        if (!dma_put(sw, (uint8_t)sector)) { // the sector byte leads the field
            sw->error |= SWIM3_E_OVERRUN;
            return false;
        }
        for (int i = 0; i < GCR_STREAM_BYTES - 1; i++)
            if (!dma_put(sw, nib[i])) {
                sw->error |= SWIM3_E_OVERRUN;
                return false;
            }
    }

    for (unsigned i = 0; i < sw->gap; i++) // pad requests over the gap
        dma_put(sw, 0);
    return true;
}

// === Write / format stream parser ===========================================
//
// One parser serves both, because a write and a format put the same thing
// on the DMA channel: a byte stream in which $99 introduces a command
// ($0F "pass 512 bytes literally", $04 "write the CRC", $08 "end data",
// $A1/$C2/$FB/$FE the missing-clock mark bytes) and, in GCR mode, values
// below $40 go through the hardware encode table while high-bit values are
// literal patterns.  A write parses one data field at the sector whose
// header matched; a format parses a whole track image and takes each data
// field's sector number from the address field that preceded it.

// How many stream bytes to consume before giving up on finding "99 08".
// A 1.44 MB track image is ~12.5 KB; the driver's own buffer is 44 KB.
#define STREAM_LIMIT (48u * 1024u)

typedef struct swim3_parse {
    const swim3_media_t *m;
    int track, side;
    int sector; // for a sector write: the matched header's sector
    bool format; // whole-track parse: take sectors from the stream
    int sectors_written;
    // Diagnostics, printed under `debug.log swim3 6` when the parse ends.
    // The marks the driver feeds the converter are the one part of these
    // streams no document pins byte-exactly, so make them readable rather
    // than something to guess at from a silent failure.
    unsigned bytes; // stream bytes consumed
    char head[3 * 24 + 1]; // hex dump of the first 24 of them
} swim3_parse_t;

// Record one stream byte for the trace above.
static void stream_note(swim3_parse_t *p, uint8_t b) {
    static const char hex[] = "0123456789ABCDEF";
    unsigned n = p->bytes++;
    if (n >= 24)
        return;
    p->head[n * 3 + 0] = hex[b >> 4];
    p->head[n * 3 + 1] = hex[b & 15];
    p->head[n * 3 + 2] = ' ';
    p->head[n * 3 + 3] = 0;
}

// Deposit one parsed data field.
static void swim3_deposit(swim3_t *sw, swim3_parse_t *p, int sector, uint8_t *data, const uint8_t *tag) {
    if (sector < 0 || sector >= swim3_spt(p->m, p->track)) {
        LOG(2, "stream: data field for out-of-range sector %d on track %d", sector, p->track);
        return;
    }
    if (!swim3_write_sector(p->m, p->track, p->side, sector, data, tag)) {
        sw->error |= SWIM3_E_UNDERRUN; // write-protected or off the end
        LOG(2, "stream: sector %d/%d side %d not written", p->track, sector, p->side);
        return;
    }
    p->sectors_written++;
}

// MFM stream: gap and sync bytes are noise, "99 0F" opens the 512-byte
// data field, and "99 08" ends the transfer.
static bool swim3_parse_mfm(swim3_t *sw, swim3_parse_t *p) {
    uint8_t data[SECTOR_BYTES];
    int hdr_sector = p->sector;
    bool in_header = false;
    unsigned hdr_pos = 0;
    uint8_t hdr[4];

    for (unsigned n = 0; n < STREAM_LIMIT; n++) {
        uint8_t b;
        if (!dma_get(sw, &b))
            return false;
        stream_note(p, b);
        if (in_header) {
            // The four bytes after the address mark are C H S N; they are
            // ordinary stream bytes, not escapes.
            hdr[hdr_pos++] = b;
            if (hdr_pos == 4) {
                in_header = false;
                hdr_sector = hdr[2] - 1; // MFM headers number sectors from 1
            }
            continue;
        }
        if (b != 0x99)
            continue; // gap ($4E) and sync ($00) filler
        if (!dma_get(sw, &b))
            return false;
        switch (b) {
        case 0x0F: // pass the next 512 bytes literally: the data field
            for (int i = 0; i < SECTOR_BYTES; i++)
                if (!dma_get(sw, &data[i]))
                    return false;
            swim3_deposit(sw, p, hdr_sector, data, NULL);
            break;
        case 0xFE: // address mark: C H S N follow
            if (p->format) {
                in_header = true;
                hdr_pos = 0;
            }
            break;
        case 0x08: // end data — terminate the transfer
            return true;
        default: // $04 CRC (we store decoded sectors), $A1/$C2/$FB marks, $99
            break;
        }
    }
    LOG(2, "stream: no end-of-data command within %u bytes", STREAM_LIMIT);
    return false;
}

// GCR stream: the marks arrive as literal high-bit patterns ($D5 $AA) and
// the third mark byte is the encoded value that distinguishes an address
// field ($00 -> $96) from a data field ($0B -> $AD).
static bool swim3_parse_gcr(swim3_t *sw, swim3_parse_t *p) {
    uint8_t nib[GCR_STREAM_BYTES - 1], data[SECTOR_BYTES], tag[TAG_BYTES];
    int hdr_sector = p->sector;
    uint8_t prev = 0, prev2 = 0;

    for (unsigned n = 0; n < STREAM_LIMIT; n++) {
        uint8_t b;
        if (!dma_get(sw, &b))
            return false;
        stream_note(p, b);
        if (b == 0x99) {
            if (!dma_get(sw, &b))
                return false;
            if (b == 0x08)
                return true; // end data
            prev2 = prev = 0;
            continue;
        }
        if (prev2 == 0xD5 && prev == 0xAA) {
            // The third mark byte tells the two fields apart.  Accept both
            // spellings of it: the driver feeds the converter the UNencoded
            // value ($00 address / $0B data) and lets the hardware table
            // turn it into $96 / $AD, but the same byte written as a
            // high-bit literal is the other legal way to put that pattern
            // on the disk, and nothing downstream can tell them apart.
            if ((b == 0x00 || b == 0x96) && p->format) {
                // Address field: five encoded header bytes follow
                // (track, sector, side, format, checksum).
                uint8_t h[5];
                for (int i = 0; i < 5; i++)
                    if (!dma_get(sw, &h[i]))
                        return false;
                hdr_sector = h[1] & 0x3F;
                prev2 = prev = 0;
                continue;
            }
            if (b == 0x0B || b == 0xAD) {
                // Data field: the sector byte then 703 six-bit values.
                uint8_t s;
                if (!dma_get(sw, &s))
                    return false;
                for (int i = 0; i < GCR_STREAM_BYTES - 1; i++)
                    if (!dma_get(sw, &nib[i]))
                        return false;
                if (!gcr_denibblize(nib, tag, data))
                    sw->error |= SWIM3_E_CRC_DATA;
                else
                    swim3_deposit(sw, p, p->format ? (int)s : hdr_sector, data, tag);
                prev2 = prev = 0;
                continue;
            }
        }
        prev2 = prev;
        prev = b;
    }
    LOG(2, "stream: no end-of-data command within %u bytes", STREAM_LIMIT);
    return false;
}

static bool swim3_parse_stream(swim3_t *sw, swim3_parse_t *p) {
    p->bytes = 0;
    p->head[0] = 0;
    bool ok = p->m->mfm ? swim3_parse_mfm(sw, p) : swim3_parse_gcr(sw, p);
    LOG(6, "%s stream: %u bytes, %d field(s), %s | %s", p->m->mfm ? "mfm" : "gcr", p->bytes, p->sectors_written,
        ok ? "ended on 99 08" : "channel closed first", p->head);
    return ok;
}

// === Raw / copy-protect capture =============================================
//
// With CopyProtMode set the chip streams two DMA bytes per disk byte — a
// flag ($00, or $80 when the byte is a mark byte) then the data byte —
// starting at the next mark byte and running until GO is cleared or, as
// the driver arranges it, until the DMA count exhausts.  What we put on
// the wire is the track image the sector model implies; real silicon also
// emits flag values other than $00/$80 ("SWIM3 bug"), which software skips,
// so emitting only the two documented values is safe.

static bool raw_pair(swim3_t *sw, uint8_t flag, uint8_t data) {
    return dma_put(sw, flag) && dma_put(sw, data);
}

// Reconstruct one track's byte stream, oldest field first, into the DMA
// window.  Stops as soon as the channel closes (terminal count).
static void swim3_raw_track(swim3_t *sw, const swim3_media_t *m, int track, int side) {
    int spt = swim3_spt(m, track);
    uint8_t data[SECTOR_BYTES], tag[TAG_BYTES];

    for (int s = 0; s < spt; s++) {
        if (!swim3_read_sector(m, track, side, s, data, tag))
            return;
        if (m->mfm) {
            for (int i = 0; i < 12; i++) // sync
                if (!raw_pair(sw, 0x00, 0x00))
                    return;
            for (int i = 0; i < 3; i++) // address mark
                if (!raw_pair(sw, 0x80, 0xA1))
                    return;
            if (!raw_pair(sw, 0x80, 0xFE))
                return;
            uint8_t hdr[4] = {(uint8_t)track, (uint8_t)side, (uint8_t)(s + 1), 0x02};
            for (int i = 0; i < 4; i++)
                if (!raw_pair(sw, 0x00, hdr[i]))
                    return;
            for (int i = 0; i < 22; i++) // gap 2
                if (!raw_pair(sw, 0x00, 0x4E))
                    return;
            for (int i = 0; i < 12; i++)
                if (!raw_pair(sw, 0x00, 0x00))
                    return;
            for (int i = 0; i < 3; i++) // data mark
                if (!raw_pair(sw, 0x80, 0xA1))
                    return;
            if (!raw_pair(sw, 0x80, 0xFB))
                return;
            for (int i = 0; i < SECTOR_BYTES; i++)
                if (!raw_pair(sw, 0x00, data[i]))
                    return;
            for (int i = 0; i < 54; i++) // gap 3
                if (!raw_pair(sw, 0x00, 0x4E))
                    return;
        } else {
            // The 6-to-8 GCR codeword table.  The shared floppy module has
            // the same 64 bytes, but only behind its private header, and
            // this is the one place a machine model needs the ENCODED form
            // (raw capture is the only path that sees disk bytes rather
            // than the values either side of the chip's converter).
            static const uint8_t gcr6[64] = {
                0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6, 0xA7, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB2, 0xB3,
                0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3,
                0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC,
                0xED, 0xEE, 0xEF, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF};
            uint8_t side_enc = (uint8_t)((side << 5) | ((track >> 6) & 0x1F));
            uint8_t hdr[5] = {(uint8_t)track, (uint8_t)s, side_enc, m->fmt_byte,
                              (uint8_t)(track ^ s ^ side_enc ^ m->fmt_byte)};
            for (int i = 0; i < 5; i++) // self-sync leader
                if (!raw_pair(sw, 0x00, 0xFF))
                    return;
            if (!raw_pair(sw, 0x80, 0xD5) || !raw_pair(sw, 0x80, 0xAA) || !raw_pair(sw, 0x80, 0x96))
                return;
            for (int i = 0; i < 5; i++)
                if (!raw_pair(sw, 0x00, gcr6[hdr[i] & 0x3F]))
                    return;
            if (!raw_pair(sw, 0x00, 0xDE) || !raw_pair(sw, 0x00, 0xAA) || !raw_pair(sw, 0x00, 0xFF))
                return;
            for (int i = 0; i < 5; i++)
                if (!raw_pair(sw, 0x00, 0xFF))
                    return;
            if (!raw_pair(sw, 0x80, 0xD5) || !raw_pair(sw, 0x80, 0xAA) || !raw_pair(sw, 0x80, 0xAD))
                return;
            uint8_t nib[GCR_STREAM_BYTES - 1];
            gcr_nibblize(tag, data, nib);
            if (!raw_pair(sw, 0x00, gcr6[s & 0x3F]))
                return;
            for (int i = 0; i < GCR_STREAM_BYTES - 1; i++)
                if (!raw_pair(sw, 0x00, gcr6[nib[i] & 0x3F]))
                    return;
            if (!raw_pair(sw, 0x00, 0xDE) || !raw_pair(sw, 0x00, 0xAA) || !raw_pair(sw, 0x00, 0xFF))
                return;
        }
    }
}

// === The engine =============================================================

static void swim3_engine_event(void *source, uint64_t data);

// Arm the engine's next service slot.
static void swim3_arm(swim3_t *sw, double delay_ns) {
    remove_event(sw->sched, swim3_engine_event, sw);
    if (delay_ns < 1000.0)
        delay_ns = 1000.0; // one service slot is never shorter than a us
    sw->engine_running = 1;
    // Round UP: a slot aimed at the next header boundary must not land a
    // fraction of a nanosecond before it, or swim3_next_header computes the
    // same header again and a continuous transfer delivers one sector
    // twice (seen as duplicate sectors in Open Firmware's track reads).
    scheduler_new_cpu_event(sw->sched, swim3_engine_event, sw, 0, 0, (uint64_t)ceil(delay_ns));
}

static void swim3_stop(swim3_t *sw) {
    sw->xfer_any = 0; // GO dropped: the next GO starts at FirstSector again
    if (!sw->engine_running)
        return;
    remove_event(sw->sched, swim3_engine_event, sw);
    sw->engine_running = 0;
}

// Does the header the driver asked for match the one under the head?
// Bit 7 is the reset value's "match no sector"; bit 6 is the wildcard.
static bool sector_match(uint8_t first, uint8_t header) {
    if (first & 0x80u)
        return false;
    if (first & 0x40u)
        return true;
    return (uint8_t)(first & 0x3Fu) == (uint8_t)(header & 0x3Fu);
}

// Is the chip framing the encoding this disk actually carries?  A mismatch
// means the head finds nothing it recognises — which is exactly how the
// driver's format-detection walk (MFM1440K, MFM720K, GCR800K, GCR400K,
// GCRonHD in that order) rejects the wrong modes: no idIntNum arrives and
// its 300 ms timeout fires.
static bool encoding_matches(const swim3_t *sw, const swim3_media_t *m) {
    bool gcr_framing = (sw->setup & SWIM3_S_GCR) != 0;
    return gcr_framing != m->mfm;
}

// One read-mode service slot: the next address header passes under the
// head.  While GO is set the position registers update and idIntNum fires
// on every header (ERS §36); a header that matches FirstSector with
// SectorsToXfer non-zero additionally streams its data field.
static void swim3_read_slot(swim3_t *sw, const swim3_media_t *m) {
    int track = floppy_drive_track(sw->fd, FD);
    int side = sw->xfer_side;
    double delay = 0;
    int idx = swim3_next_header(sw, m, track, &delay);

    if (side >= m->sides) {
        swim3_arm(sw, delay); // head 1 of a single-sided disk: no fields
        return;
    }

    uint8_t hdr_sect = m->mfm ? (uint8_t)(idx + 1) : (uint8_t)idx;
    sw->ctrack = (uint8_t)((track & 0x7F) | (side ? 0x80 : 0));
    sw->csect = (uint8_t)(hdr_sect | 0x80); // bit 7 = Last_ID_valid
    sw->fmt_byte = m->fmt_byte;
    swim3_raise(sw, SWIM3_INT_ID);

    // FirstSector picks where the transfer starts; the remaining
    // SectorsToXfer are "accessed continuously" (ERS reg $E) — the next
    // headers that pass, whatever their numbers.  Open Firmware and Linux
    // read a track's tail in one go this way (FirstSector = n, SectorsToXfer
    // = spt - n + 1); the Mac OS driver's one-sector transfers never see
    // the difference.
    if (sw->nsect > 0 && (sw->xfer_any || sector_match(sw->sector, hdr_sect)) && sw->be.dma_running(sw->be.ctx)) {
        LOG(4, "read track %d side %d sector %d", track, side, idx);
        swim3_stream_read(sw, m, track, side, idx);
        sw->nsect--; // hardware decrements per completed sector (§3.7)
        sw->xfer_any = sw->nsect > 0;
        if (sw->nsect == 0)
            swim3_raise(sw, SWIM3_INT_DONE);
    }
    swim3_arm(sw, delay);
}

// One write-mode service slot: wait for the matching header, then let the
// stream parser lay down its data field.
static void swim3_write_slot(swim3_t *sw, const swim3_media_t *m) {
    int track = floppy_drive_track(sw->fd, FD);
    int side = sw->xfer_side;
    double delay = 0;
    int idx = swim3_next_header(sw, m, track, &delay);

    if (side >= m->sides) {
        swim3_arm(sw, delay);
        return;
    }
    uint8_t hdr_sect = m->mfm ? (uint8_t)(idx + 1) : (uint8_t)idx;
    sw->ctrack = (uint8_t)((track & 0x7F) | (side ? 0x80 : 0));
    sw->csect = (uint8_t)(hdr_sect | 0x80);
    sw->fmt_byte = m->fmt_byte;

    if (sw->nsect == 0 || !(sw->xfer_any || sector_match(sw->sector, hdr_sect)) || !sw->be.dma_running(sw->be.ctx)) {
        swim3_arm(sw, delay);
        return;
    }
    LOG(4, "write track %d side %d sector %d", track, side, idx);
    swim3_parse_t p = {.m = m, .track = track, .side = side, .sector = idx, .format = false};
    swim3_parse_stream(sw, &p);
    sw->nsect--;
    sw->xfer_any = sw->nsect > 0;
    if (sw->nsect == 0)
        swim3_raise(sw, SWIM3_INT_DONE);
    swim3_arm(sw, delay);
}

// Format: at the index, the whole track image the driver built is written
// out.  Its "99 08" ends the operation, and what we keep from it is the
// layout it declares plus each data field it carries.
static void swim3_format_slot(swim3_t *sw, const swim3_media_t *m) {
    int track = floppy_drive_track(sw->fd, FD);
    int side = sw->xfer_side < m->sides ? sw->xfer_side : 0;
    swim3_parse_t p = {.m = m, .track = track, .side = side, .sector = -1, .format = true};
    swim3_parse_stream(sw, &p);
    sw->fmt_sectors = (uint32_t)p.sectors_written;
    LOG(3, "format track %d side %d: %d sectors", track, side, p.sectors_written);
    swim3_raise(sw, SWIM3_INT_DONE);
    swim3_stop(sw);
}

// Raw capture ends when the DMA count exhausts (which drops RUN and raises
// the AMIC DMA interrupt) or when the driver clears GO.
static void swim3_raw_slot(swim3_t *sw, const swim3_media_t *m) {
    int track = floppy_drive_track(sw->fd, FD);
    int side = sw->xfer_side < m->sides ? sw->xfer_side : 0;
    LOG(3, "raw capture track %d side %d", track, side);
    swim3_raw_track(sw, m, track, side);
    swim3_stop(sw);
}

static void swim3_engine_event(void *source, uint64_t data) {
    (void)data;
    swim3_t *sw = (swim3_t *)source;
    sw->engine_running = 0;

    // Seek: the pulses have been paced, so the head lands and stepIntNum
    // fires.  GoStep and GO are never set together by the driver.
    if (sw->mode & SWIM3_M_GOSTEP) {
        floppy_swim3_step(sw->fd, FD, sw->step_dir != 0, sw->step);
        sw->step = 0;
        swim3_raise(sw, SWIM3_INT_STEP);
        return;
    }
    if (!(sw->mode & SWIM3_M_ACTION))
        return;

    // No media, a stopped spindle or the wrong framing: the head sees
    // nothing.  Keep the slot alive so the operation resumes if the disk
    // arrives, and let the driver's own timeout decide.
    swim3_media_t m;
    if (!swim3_media(sw, &m) || !floppy_drive_motor_on(sw->fd, FD) || !encoding_matches(sw, &m)) {
        swim3_arm(sw, 5.0e6);
        return;
    }
    floppy_swim3_set_side(sw->fd, FD, sw->xfer_side);

    if (sw->setup & SWIM3_S_COPYPROT)
        swim3_raw_slot(sw, &m);
    else if (sw->mode & SWIM3_M_FORMAT)
        swim3_format_slot(sw, &m);
    else if (sw->mode & SWIM3_M_WRITE)
        swim3_write_slot(sw, &m);
    else
        swim3_read_slot(sw, &m);
}

void swim3_engine_update(swim3_t *sw) {
    bool want = (sw->mode & (SWIM3_M_ACTION | SWIM3_M_GOSTEP)) != 0;
    if (!want) {
        swim3_stop(sw);
        return;
    }
    if (sw->engine_running)
        return;

    if (sw->mode & SWIM3_M_GOSTEP) {
        // Step counts down to zero as the pulses go out and stays there;
        // re-asserting GoStep with an exhausted counter does nothing (and
        // must not double-step, since the driver leaves the bit set until
        // after it has handled stepIntNum).
        if (sw->step)
            swim3_arm(sw, STEP_PULSE_NS * (double)sw->step);
        return;
    }
    // A whole-track operation starts at the index; a sector operation at
    // the next header.  Both are one service slot away at most.
    swim3_media_t m;
    if (!swim3_media(sw, &m)) {
        swim3_arm(sw, 5.0e6);
        return;
    }
    int track = floppy_drive_track(sw->fd, FD);
    if ((sw->mode & SWIM3_M_FORMAT) || (sw->setup & SWIM3_S_COPYPROT)) {
        swim3_arm(sw, swim3_rev_ns(&m, track) * TRACK_OP_REVS);
        return;
    }
    double delay = 0;
    swim3_next_header(sw, &m, track, &delay);
    swim3_arm(sw, delay);
}

void swim3_xfer_register_events(swim3_t *sw) {
    scheduler_new_event_type(sw->sched, "swim3", sw, "engine", swim3_engine_event);
}
