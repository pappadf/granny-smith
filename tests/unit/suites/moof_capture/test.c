// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// A MOOF's capture on the GCR read path: see Makefile.

#include "floppy_gcr.c" // NOLINT -- statics under test (track_splice)

#include "codec.h"
#include "common.h"
#include "gcr.h"
#include "image_capture.h"
#include "image_internal.h"
#include "image_moof.h"
#include "test_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// === The drive side floppy_gcr.c reaches (floppy.c's) ======================

struct floppy;
image_t *floppy_drive_image(const struct floppy *floppy, unsigned drive) {
    (void)floppy;
    (void)drive;
    return NULL;
}
int floppy_drive_format(const struct floppy *floppy, unsigned drive) {
    (void)floppy;
    (void)drive;
    return -1;
}
double scheduler_time_ns(struct scheduler *restrict scheduler) {
    (void)scheduler;
    return 0.0;
}

// === Building captures ======================================================

#define SECTORS_800K 1600
#define SECTORS_400K 800

static char g_dir[64];

// Deterministic sector and tag contents.
static void fill(uint8_t *data, uint8_t *tags, uint32_t sectors, uint32_t seed) {
    uint32_t x = seed;
    for (size_t i = 0; i < (size_t)sectors * 512; i++) {
        x = x * 1103515245u + 12345u;
        data[i] = (uint8_t)(x >> 16);
    }
    for (size_t i = 0; tags && i < (size_t)sectors * MOOF_TAG_BYTES; i++)
        tags[i] = (uint8_t)(i * 7 + seed);
}

// A writer-made MOOF of `sectors` sectors with known contents.
typedef struct {
    uint8_t *file;
    size_t len;
    uint8_t *data;
    uint8_t *tags;
    uint32_t sectors;
} moof_t;

static void moof_make(moof_t *m, uint32_t sectors, uint32_t seed) {
    m->sectors = sectors;
    m->data = malloc((size_t)sectors * 512);
    m->tags = malloc((size_t)sectors * MOOF_TAG_BYTES);
    fill(m->data, m->tags, sectors, seed);
    ASSERT_EQ_INT(0, moof_encode(m->data, sectors, m->tags, 0, &m->file, &m->len));
}

static void moof_free(moof_t *m) {
    free(m->file);
    free(m->data);
    free(m->tags);
}

// The bits of (track, side) and their count.
static uint8_t *track_bits(moof_t *m, int t, int s, uint32_t *n) {
    uint8_t idx = m->file[88 + t * 2 + s];
    ASSERT_TRUE(idx != 0xFF);
    const uint8_t *rec = m->file + 256 + 8 * idx;
    *n = RD_LE32(rec + 4);
    return m->file + (size_t)RD_LE16(rec) * 512;
}

static int bit_get(const uint8_t *b, uint32_t i) {
    return (b[i >> 3] >> (7 - (i & 7))) & 1;
}

static void bit_put(uint8_t *b, uint32_t i, int v) {
    uint8_t mask = (uint8_t)(0x80 >> (i & 7));
    b[i >> 3] = (uint8_t)(v ? b[i >> 3] | mask : b[i >> 3] & ~mask);
}

// Overwrite `n` cells at `at` with the low `n` bits of `v`, MSB first.
static void bits_put(uint8_t *b, uint32_t at, uint32_t v, int n) {
    for (int k = 0; k < n; k++)
        bit_put(b, at + (uint32_t)k, (v >> (n - 1 - k)) & 1);
}

// The 24 cells at `at` as a number.
static uint32_t bits24(const uint8_t *b, uint32_t at) {
    uint32_t v = 0;
    for (int k = 0; k < 24; k++)
        v = v << 1 | (uint32_t)bit_get(b, at + (uint32_t)k);
    return v;
}

// The cell where sector `sec`'s address mark (D5 AA 96) starts on a
// writer-made track: the address field whose sector value is its fifth byte.
static uint32_t addr_mark_of(const uint8_t *b, uint32_t n, int sec) {
    for (uint32_t i = 0; i + 48 < n; i++) {
        if (bits24(b, i) != 0xD5AA96)
            continue;
        uint32_t s = 0;
        for (int k = 0; k < 8; k++)
            s = s << 1 | (uint32_t)bit_get(b, i + 32 + (uint32_t)k);
        if (gcr_decode6[s] == sec)
            return i;
    }
    ASSERT_TRUE(0);
    return 0;
}

// The cell where sector `sec`'s data mark (D5 AA AD) starts on a writer-made
// track: after its address field.
static uint32_t data_mark_of(const uint8_t *b, uint32_t n, int sec) {
    for (uint32_t j = addr_mark_of(b, n, sec) + 24; j + 24 < n; j++)
        if (bits24(b, j) == 0xD5AAAD)
            return j;
    ASSERT_TRUE(0);
    return 0;
}

// Turn (t, s)'s revolution so it starts `off` cells later: the same track
// captured with the index somewhere else on it.
static void rotate_track(moof_t *m, int t, int s, uint32_t off) {
    uint32_t n;
    uint8_t *b = track_bits(m, t, s, &n);
    size_t bytes = (n + 7) / 8;
    uint8_t *turned = calloc(1, bytes);
    ASSERT_TRUE(turned != NULL);
    for (uint32_t i = 0; i < n; i++)
        bit_put(turned, i, bit_get(b, (i + off) % n));
    memcpy(b, turned, bytes);
    free(turned);
}

// Make sector `sec`'s address field on (t, s) claim another place: the
// headers a duplicator can lay down that do not name their own track.
static void readdress(moof_t *m, int t, int s, int sec, int claim_track, int claim_side, int claim_sector) {
    uint32_t n;
    uint8_t *b = track_bits(m, t, s, &n);
    uint32_t at = addr_mark_of(b, n, sec);
    uint8_t addr[GCR_ADDR_VALUES];
    gcr_addr_encode(claim_track, claim_sector, claim_side, 0x22, addr);
    for (int k = 0; k < GCR_ADDR_VALUES; k++)
        bits_put(b, at + 24 + 8 * (uint32_t)k, gcr_encode6[addr[k]], 8);
}

// Restamp the header CRC after an edit.
static void restamp(moof_t *m) {
    WR_LE32(m->file + 8, codec_crc32(0, m->file + MOOF_HEADER_SIZE, m->len - MOOF_HEADER_SIZE));
}

// The protection-like edits the suite's capture carries:
//  - track 3, side 0, sector 5: a $DE where the formatter leaves the last
//    sync cell in front of the data mark (the sector still reads);
//  - track 70, side 0, sector 3: its data field's values replaced by
//    nibbles that are no GCR codeword (a field that is not a sector).
#define SIG_TRACK  3
#define SIG_SECTOR 5
#define BAD_TRACK  70
#define BAD_SECTOR 3
static void protect(moof_t *m) {
    uint32_t n;
    uint8_t *b = track_bits(m, SIG_TRACK, 0, &n);
    uint32_t dm = data_mark_of(b, n, SIG_SECTOR);
    bits_put(b, dm - 10, 0x378, 10); // 11011110 00: $DE in a ten-cell sync slot
    b = track_bits(m, BAD_TRACK, 0, &n);
    dm = data_mark_of(b, n, BAD_SECTOR);
    for (uint32_t k = 0; k < 600; k++)
        bits_put(b, dm + 32 + k * 8, 0x80, 8); // $80: framed as a byte, no codeword
    restamp(m);
}

// Image order of (track, side, sector) on a disk of `sides`.
static uint64_t block_of(int t, int s, int sec, int sides) {
    return floppy_zone_image_offset(t, s, sides) / 512 + (uint64_t)sec;
}

// Write the capture to a file and open it as a floppy: writable (a fresh
// delta instance) or read-only, honouring the capture's write protection or not.
static image_t *open_moof(moof_t *m, const char *name, bool writable, bool honour) {
    char path[160];
    snprintf(path, sizeof path, "%s/%s", g_dir, name);
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    ASSERT_EQ_INT((int)m->len, (int)fwrite(m->file, 1, m->len, f));
    fclose(f);
    image_geometry_t g = {.gcr_tags = true, .honour_media_lock = honour};
    image_t *img = writable ? image_create_with_geometry(path, g_dir, g) : image_open_readonly_with_geometry(path, g);
    ASSERT_TRUE(img != NULL);
    return img;
}

// floppy_media_track for an image.
static uint8_t *media_track(image_t *img, int t, int s, size_t *len) {
    floppy_media_t md;
    ASSERT_TRUE(floppy_media_from_image(img, &md));
    uint8_t *out = NULL;
    ASSERT_EQ_INT(0, floppy_media_track(&md, t, s, &out, len));
    return out;
}

// The capture's own steady-state revolution of (t, s).
static uint8_t *capture_track(image_t *img, int t, int s, size_t *len, moof_sector_t *sec) {
    moof_sector_t local[MOOF_MAX_SECTORS];
    uint8_t *nib = NULL;
    *len = image_capture_track(image_capture(img), t, s, &nib, sec ? sec : local);
    return nib;
}

// Occurrences of `pat` (`n` bytes) on a looping track.
static int count_seq(const uint8_t *trk, size_t len, const uint8_t *pat, size_t n) {
    int c = 0;
    for (size_t i = 0; i < len; i++) {
        size_t k = 0;
        while (k < n && trk[(i + k) % len] == pat[k])
            k++;
        c += k == n;
    }
    return c;
}

static const uint8_t SIG[4] = {0xDE, 0xD5, 0xAA, 0xAD};

// === Tests ==================================================================

// The capture is kept and known for what each sector held; a sector it could
// not read reads as zeros in the sector view and fails a sector-level read.
TEST(capture_status_and_unreadable_sector) {
    moof_t m;
    moof_make(&m, SECTORS_800K, 11);
    protect(&m);
    image_t *img = open_moof(&m, "status.moof", true, false);
    ASSERT_TRUE(image_has_capture(img));
    ASSERT_TRUE(strcmp(image_get_format(img), "moof+dc42") == 0);
    uint64_t bad = block_of(BAD_TRACK, 0, BAD_SECTOR, 2);
    ASSERT_EQ_INT(IMAGE_SECTOR_MISSING, image_sector_status(img, bad));
    ASSERT_EQ_INT(IMAGE_SECTOR_OK, image_sector_status(img, bad - 1));
    ASSERT_EQ_INT(IMAGE_SECTOR_OK, image_sector_status(img, block_of(SIG_TRACK, 0, SIG_SECTOR, 2)));
    uint8_t buf[512];
    ASSERT_EQ_INT(512, (int)disk_read_data(img, bad * 512, buf, 512));
    for (int i = 0; i < 512; i++)
        ASSERT_EQ_INT(0, buf[i]); // the importer's zeros
    floppy_media_t md;
    ASSERT_TRUE(floppy_media_from_image(img, &md));
    ASSERT_TRUE(!floppy_media_read_sector(&md, BAD_TRACK, 0, BAD_SECTOR, buf));
    ASSERT_TRUE(floppy_media_read_sector(&md, BAD_TRACK, 0, BAD_SECTOR - 1, buf));
    ASSERT_TRUE(floppy_media_block_unreadable(img, bad));
    // A write makes it readable, as on the real disk.
    memset(buf, 0, sizeof buf);
    ASSERT_EQ_INT(512, (int)disk_write_data(img, bad * 512, buf, 512));
    ASSERT_TRUE(!floppy_media_block_unreadable(img, bad));
    ASSERT_TRUE(floppy_media_read_sector(&md, BAD_TRACK, 0, BAD_SECTOR, buf));
    image_close(img);
    moof_free(&m);
}

// An image holding what the capture decodes to gets the captured track:
// the signature in front of a data mark included, which a track laid down
// from sectors cannot have.
TEST(unchanged_track_is_the_capture) {
    moof_t m;
    moof_make(&m, SECTORS_800K, 12);
    protect(&m);
    image_t *img = open_moof(&m, "serve.moof", true, false);
    for (int t = 0; t < 80; t += 7) {
        size_t a, b;
        uint8_t *got = media_track(img, t, 1, &a);
        uint8_t *cap = capture_track(img, t, 1, &b, NULL);
        ASSERT_EQ_INT((int)b, (int)a);
        ASSERT_EQ_INT(0, memcmp(got, cap, a));
        free(got);
        free(cap);
    }
    size_t len;
    uint8_t *trk = media_track(img, SIG_TRACK, 0, &len);
    ASSERT_EQ_INT(1, count_seq(trk, len, SIG, 4));
    free(trk);
    // The undecodable field too, while nothing has written its sector.
    size_t a, b;
    uint8_t *got = media_track(img, BAD_TRACK, 0, &a);
    uint8_t *cap = capture_track(img, BAD_TRACK, 0, &b, NULL);
    ASSERT_TRUE(a == b && memcmp(got, cap, a) == 0);
    free(got);
    free(cap);
    image_close(img);
    moof_free(&m);
}

// A changed sector's data field is re-encoded into the capture: the rest of
// the track -- the signature on another sector included -- is untouched, the
// length is the capture's, and the new field reads back.
TEST(changed_sector_is_spliced) {
    moof_t m;
    moof_make(&m, SECTORS_800K, 13);
    protect(&m);
    image_t *img = open_moof(&m, "splice.moof", true, false);
    size_t clen;
    moof_sector_t before[MOOF_MAX_SECTORS];
    uint8_t *cap = capture_track(img, SIG_TRACK, 0, &clen, before);
    uint8_t buf[512];
    for (int i = 0; i < 512; i++)
        buf[i] = (uint8_t)(i ^ 0x5A);
    uint64_t blk = block_of(SIG_TRACK, 0, 1, 2);
    ASSERT_EQ_INT(512, (int)disk_write_data(img, blk * 512, buf, 512));
    size_t len;
    uint8_t *trk = media_track(img, SIG_TRACK, 0, &len);
    ASSERT_EQ_INT((int)clen, (int)len);
    ASSERT_EQ_INT(1, count_seq(trk, len, SIG, 4));
    moof_sector_t sec[MOOF_MAX_SECTORS];
    moof_scan_track(trk, len, SIG_TRACK, 0, sec, NULL);
    ASSERT_EQ_INT(MOOF_SECTOR_OK, sec[1].state);
    ASSERT_EQ_INT(0, memcmp(sec[1].data, buf, 512));
    ASSERT_EQ_INT(0, memcmp(sec[1].tag, m.tags + blk * MOOF_TAG_BYTES, MOOF_TAG_BYTES)); // the tag kept
    // Outside the replaced field, byte for byte the capture.
    size_t from = before[1].addr_at + 10, to = before[1].data_end;
    ASSERT_EQ_INT(0, memcmp(trk, cap, from));
    ASSERT_EQ_INT(0, memcmp(trk + to, cap + to, clen - to));
    for (int k = 0; k < 12; k++)
        if (k != 1)
            ASSERT_EQ_INT(0, memcmp(sec[k].data, m.data + (blk - 1 + (uint64_t)k) * 512, 512));
    free(trk);
    // Rewriting the signed sector itself loses the signature, as the
    // drive's sync over the gap would.
    uint64_t sblk = block_of(SIG_TRACK, 0, SIG_SECTOR, 2);
    ASSERT_EQ_INT(512, (int)disk_write_data(img, sblk * 512, buf, 512));
    trk = media_track(img, SIG_TRACK, 0, &len);
    ASSERT_EQ_INT((int)clen, (int)len);
    ASSERT_EQ_INT(0, count_seq(trk, len, SIG, 4));
    free(trk);
    free(cap);
    image_close(img);
    moof_free(&m);
}

// A sector the capture could not read stays the capture while the image
// holds the importer's zeros and nothing wrote it -- the eject flush's
// rewrite of identical data on readable sectors does not count -- and is
// spliced once the guest has written it, zeros included.
TEST(unreadable_sector_and_the_delta) {
    moof_t m;
    moof_make(&m, SECTORS_800K, 14);
    protect(&m);
    image_t *img = open_moof(&m, "delta.moof", true, false);
    // An identical rewrite of a readable sector (what an eject flush does).
    uint64_t ok = block_of(BAD_TRACK, 0, 0, 2);
    uint8_t buf[512];
    ASSERT_EQ_INT(512, (int)disk_read_data(img, ok * 512, buf, 512));
    ASSERT_EQ_INT(512, (int)disk_write_data(img, ok * 512, buf, 512));
    size_t a, b;
    uint8_t *got = media_track(img, BAD_TRACK, 0, &a);
    uint8_t *cap = capture_track(img, BAD_TRACK, 0, &b, NULL);
    ASSERT_TRUE(a == b && memcmp(got, cap, a) == 0);
    free(got);
    // The guest writes zeros over the unreadable sector: the content is
    // unchanged, the medium is not.
    uint64_t bad = block_of(BAD_TRACK, 0, BAD_SECTOR, 2);
    memset(buf, 0, sizeof buf);
    ASSERT_EQ_INT(512, (int)disk_write_data(img, bad * 512, buf, 512));
    got = media_track(img, BAD_TRACK, 0, &a);
    ASSERT_EQ_INT((int)b, (int)a); // a standard-length field replaced: same length
    ASSERT_TRUE(memcmp(got, cap, a) != 0);
    moof_sector_t sec[MOOF_MAX_SECTORS];
    moof_scan_track(got, a, BAD_TRACK, 0, sec, NULL);
    ASSERT_EQ_INT(MOOF_SECTOR_OK, sec[BAD_SECTOR].state);
    free(got);
    free(cap);
    image_close(img);
    moof_free(&m);
}

// Every sector changed (a reformat): laid down from the sectors, at the
// zone's length.  And a track the image cannot be spliced into likewise.
TEST(all_changed_is_synthesised) {
    moof_t m;
    moof_make(&m, SECTORS_800K, 15);
    image_t *img = open_moof(&m, "reformat.moof", true, false);
    uint8_t buf[512];
    for (int k = 0; k < 12; k++) {
        memset(buf, 0xE5 + k, sizeof buf);
        ASSERT_EQ_INT(512, (int)disk_write_data(img, block_of(10, 1, k, 2) * 512, buf, 512));
    }
    size_t len;
    uint8_t *trk = media_track(img, 10, 1, &len);
    ASSERT_EQ_INT((int)iwm_track_length(10), (int)len);
    moof_sector_t sec[MOOF_MAX_SECTORS];
    moof_scan_track(trk, len, 10, 1, sec, NULL);
    for (int k = 0; k < 12; k++) {
        ASSERT_EQ_INT(MOOF_SECTOR_OK, sec[k].state);
        ASSERT_EQ_INT(0xE5 + k, sec[k].data[100]);
    }
    free(trk);
    image_close(img);
    moof_free(&m);
}

// A 400K capture has no side 1: that side falls back to the track laid down
// from side 0's sectors, as for any 400K image.
TEST(single_sided_capture_side_1) {
    moof_t m;
    moof_make(&m, SECTORS_400K, 16);
    image_t *img = open_moof(&m, "ss.moof", true, false);
    size_t a, b;
    uint8_t *got = media_track(img, 20, 0, &a);
    uint8_t *cap = capture_track(img, 20, 0, &b, NULL);
    ASSERT_TRUE(a == b && memcmp(got, cap, a) == 0);
    free(got);
    free(cap);
    got = media_track(img, 20, 1, &a);
    ASSERT_EQ_INT((int)iwm_track_length(20), (int)a);
    free(got);
    image_close(img);
    moof_free(&m);
}

// INFO's write-protect flag: honoured when asked (an insert with no explicit
// writable=), overridden otherwise.
TEST(write_protect_flag) {
    moof_t m;
    moof_make(&m, SECTORS_400K, 17);
    m.file[12 + 8 + 2] = 1; // INFO: write protected
    restamp(&m);
    image_t *img = open_moof(&m, "wp1.moof", true, true);
    ASSERT_TRUE(!image_is_writable(img));
    ASSERT_TRUE(image_path(img) == NULL); // no delta instance was made
    image_close(img);
    img = open_moof(&m, "wp2.moof", true, false);
    ASSERT_TRUE(image_is_writable(img));
    image_close(img);
    m.file[12 + 8 + 2] = 0;
    restamp(&m);
    img = open_moof(&m, "wp3.moof", true, true);
    ASSERT_TRUE(image_is_writable(img));
    image_close(img);
    moof_free(&m);
}

// The splice's limits on a hand-made revolution: a field shorter than a
// sector's grows the track, accepted within 2% of the capture's length and
// refused past it; a field running across the index moves the seam.
TEST(splice_growth_and_wrap) {
    track_sectors_t ts;
    memset(&ts, 0, sizeof ts);
    ts.nsec = 8;
    ts.num_sides = 1;
    for (int i = 0; i < 512; i++)
        ts.data[3 * 512 + i] = (uint8_t)i;
    // A revolution of sync with sector 3's fields at `at`: an address field,
    // six sync, a 553-byte data field closed by DE AA.
    for (size_t n = 5000; n <= 9000; n += 4000) {
        uint8_t *nib = malloc(n);
        memset(nib, 0xFF, n);
        size_t at = 1000;
        moof_sector_t sec[MOOF_MAX_SECTORS];
        memset(sec, 0, sizeof sec);
        sec[3].addr = true;
        sec[3].addr_at = (uint32_t)at;
        sec[3].data_at = (uint32_t)(at + 16);
        sec[3].data_end = (uint32_t)(at + 16 + 553);
        uint8_t *out = NULL;
        size_t len = 0;
        bool ok = track_splice(nib, n, sec, 1u << 3, 64, 0, &ts, &out, &len);
        // 709 - 553 = 156 more bytes: 3.1% of 5000, 1.7% of 9000.
        ASSERT_EQ_INT(n == 9000, ok);
        if (ok) {
            ASSERT_EQ_INT((int)(n + 156), (int)len);
            moof_sector_t got[MOOF_MAX_SECTORS];
            moof_scan_track(out, len, 64, 0, got, NULL);
            (void)got; // no address field was laid: check the data field directly
            ASSERT_TRUE(out[at + 16] == 0xD5 && out[at + 17] == 0xAA && out[at + 18] == 0xAD);
            uint8_t tag[12], data[512];
            ASSERT_EQ_INT(1, gcr_sector_from_disk(out + at + 20, tag, data));
            ASSERT_EQ_INT(0, memcmp(data, ts.data + 3 * 512, 512));
        }
        free(out);
        // The same field across the index: the seam moves to its end.
        sec[3].addr_at = (uint32_t)(n - 100);
        sec[3].data_at = (uint32_t)(n - 84);
        sec[3].data_end = (uint32_t)(n - 84 + 709);
        ok = track_splice(nib, n, sec, 1u << 3, 64, 0, &ts, &out, &len);
        ASSERT_TRUE(ok);
        ASSERT_EQ_INT((int)n, (int)len);
        // The output starts where the old field ended, so the new field
        // ends exactly at the end of the buffer.
        ASSERT_TRUE(out[len - 2] == 0xDE && out[len - 1] == 0xAA);
        free(out);
        free(nib);
    }
}

// A track's length is its buffer's once built; a stale checkpointed size
// with no buffer reads as the zone's.
TEST(track_length_is_the_buffers) {
    static floppy_drive_t d;
    memset(&d, 0, sizeof d);
    d.tracks[0][5].size = 12345; // stale: no buffer behind it
    ASSERT_EQ_INT((int)iwm_track_length(5), (int)floppy_track_len(&d, 0, 5));
    uint8_t buf[16];
    d.tracks[1][70].data = buf;
    d.tracks[1][70].size = 6243;
    ASSERT_EQ_INT(6243, (int)floppy_track_len(&d, 1, 70));
}

// A .moof export copies every track whose sectors are unchanged bit for bit,
// re-encodes the rest, and writes a valid CRC.
TEST(export_copies_unchanged_tracks) {
    moof_t m;
    moof_make(&m, SECTORS_800K, 18);
    protect(&m);
    image_t *img = open_moof(&m, "exp.moof", true, false);
    uint8_t buf[512];
    memset(buf, 0x77, sizeof buf);
    uint64_t blk = block_of(SIG_TRACK, 1, 2, 2);
    ASSERT_EQ_INT(512, (int)disk_write_data(img, blk * 512, buf, 512));
    char out[160];
    snprintf(out, sizeof out, "%s/out.moof", g_dir);
    ASSERT_EQ_INT(0, image_export_to(img, out));
    FILE *f = fopen(out, "rb");
    ASSERT_TRUE(f != NULL);
    fseek(f, 0, SEEK_END);
    size_t len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(len);
    ASSERT_EQ_INT((int)len, (int)fread(file, 1, len, f));
    fclose(f);
    moof_index_t ix, src;
    ASSERT_EQ_INT(0, moof_index(file, len, &ix));
    ASSERT_TRUE(!ix.crc_mismatch);
    ASSERT_EQ_INT(0, moof_index(m.file, m.len, &src));
    int kept = 0;
    for (int t = 0; t < 80; t++)
        for (int s = 0; s < 2; s++) {
            const uint8_t *a, *b;
            uint8_t *oa = NULL, *ob = NULL;
            uint32_t na = moof_track_bits(&ix, t, s, &a, &oa), nb = moof_track_bits(&src, t, s, &b, &ob);
            bool same = na == nb && memcmp(a, b, (na + 7) / 8) == 0;
            if (t == SIG_TRACK && s == 1)
                ASSERT_TRUE(!same); // the changed track, laid down from sectors
            else
                ASSERT_TRUE(same);
            kept += same;
        }
    ASSERT_EQ_INT(159, kept);
    // And it reads back as the image: the signature and the unreadable
    // sector included.
    moof_disk_t d;
    ASSERT_EQ_INT(0, moof_decode(file, len, &d));
    ASSERT_EQ_INT(1, (int)d.missing);
    ASSERT_EQ_INT(0, memcmp(d.data + blk * 512, buf, 512));
    moof_disk_free(&d);
    free(file);
    image_close(img);
    moof_free(&m);
}

// === Splicing a sector into a capture ======================================

// Sector data no capture in the suite holds.
static void pattern(uint8_t *buf, uint8_t seed) {
    for (int i = 0; i < 512; i++)
        buf[i] = (uint8_t)(i * 3 + seed);
}

// A data field with no DE AA after its values (a bit-slip protection, a
// field the duplicator garbled): the scan must not run on to the DE AA that
// closes the next sector's address field, or a write to the sector replaces
// that address field too and the next sector vanishes from the track.
#define SLIP_TRACK  5
#define SLIP_SECTOR 3
#define SLIP_NEXT   9 // the sector after it in the 2:1 interleave
TEST(epilogue_less_field_keeps_its_neighbour) {
    moof_t m;
    moof_make(&m, SECTORS_800K, 23);
    uint32_t n;
    uint8_t *b = track_bits(&m, SLIP_TRACK, 0, &n);
    bits_put(b, data_mark_of(b, n, SLIP_SECTOR) + (3 + 1 + GCR_SECTOR_VALUES + 1) * 8, 0xAB, 8); // DE AB
    restamp(&m);
    image_t *img = open_moof(&m, "slip.moof", true, false);
    size_t clen;
    moof_sector_t sec[MOOF_MAX_SECTORS];
    uint8_t *cap = capture_track(img, SLIP_TRACK, 0, &clen, sec);
    ASSERT_EQ_INT(MOOF_SECTOR_OK, sec[SLIP_SECTOR].state); // its values still read
    ASSERT_TRUE(sec[SLIP_SECTOR].data_end <= sec[SLIP_NEXT].addr_at);
    uint64_t blk = block_of(SLIP_TRACK, 0, SLIP_SECTOR, 2);
    uint8_t buf[512];
    pattern(buf, 0x5C);
    ASSERT_EQ_INT(512, (int)disk_write_data(img, blk * 512, buf, 512));
    size_t len;
    uint8_t *trk = media_track(img, SLIP_TRACK, 0, &len);
    static const uint8_t ADDR_MARK[3] = {0xD5, 0xAA, 0x96};
    ASSERT_EQ_INT(12, count_seq(trk, len, ADDR_MARK, 3));
    moof_scan_track(trk, len, SLIP_TRACK, 0, sec, NULL);
    uint64_t first = block_of(SLIP_TRACK, 0, 0, 2);
    for (int k = 0; k < 12; k++) {
        ASSERT_EQ_INT(MOOF_SECTOR_OK, sec[k].state);
        ASSERT_EQ_INT(0, memcmp(sec[k].data, k == SLIP_SECTOR ? buf : m.data + (first + (uint64_t)k) * 512, 512));
    }
    free(trk);
    free(cap);
    image_close(img);
    moof_free(&m);
}

// The splice itself refuses a region with another address field in it,
// whatever the scan said: the track is laid down from the sectors instead.
TEST(splice_refuses_to_cover_an_address_field) {
    track_sectors_t ts;
    memset(&ts, 0, sizeof ts);
    ts.nsec = 8;
    ts.num_sides = 1;
    size_t n = 6000;
    uint8_t *nib = malloc(n);
    ASSERT_TRUE(nib != NULL);
    memset(nib, 0xFF, n);
    moof_sector_t sec[MOOF_MAX_SECTORS];
    memset(sec, 0, sizeof sec);
    sec[3].addr = true;
    sec[3].addr_at = 1000;
    sec[3].data_at = 1016;
    sec[3].data_end = 1016 + 709;
    uint8_t *out = NULL;
    size_t len = 0;
    ASSERT_TRUE(track_splice(nib, n, sec, 1u << 3, 64, 0, &ts, &out, &len));
    free(out);
    // Another sector's address field inside the region.
    nib[1600] = 0xD5, nib[1601] = 0xAA, nib[1602] = 0x96;
    out = NULL;
    ASSERT_TRUE(!track_splice(nib, n, sec, 1u << 3, 64, 0, &ts, &out, &len));
    ASSERT_TRUE(out == NULL);
    free(nib);
}

// === The drive: a track buffer written as the guest writes, then ejected ===

static floppy_drive_t g_drive;

// Build (t, s) into the drive's buffer, as the guest's first touch does.
static floppy_track_t *drive_load(image_t *img, int t, int s) {
    floppy_track_t *trk = &g_drive.tracks[s][t];
    trk->data = media_track(img, t, s, &trk->size);
    trk->modified = false;
    g_drive.track = t;
    g_drive.write_hdr_start = -1;
    return trk;
}

// The eject: flush what the guest wrote, drop the buffers.
static void drive_eject(image_t *img) {
    iwm_flush_modified_tracks(&g_drive, img, 0);
    for (int s = 0; s < NUM_SIDES; s++)
        for (int t = 0; t < NUM_TRACKS; t++)
            free(g_drive.tracks[s][t].data);
    memset(&g_drive, 0, sizeof g_drive);
}

// What the Sony driver writes over a sector once it has found its address
// field: the data field, mark through bit-slip marks, at `at` on a looping
// track of `n` bytes.  It marks the buffer written, as the data register does.
static void guest_write_field(floppy_track_t *trk, size_t at, int sec, const uint8_t *tag, const uint8_t *data) {
    uint8_t values[GCR_SECTOR_VALUES];
    gcr_sector_nibblize(tag, data, false, values);
    uint8_t field[DATA_FIELD_NIBS];
    size_t o = 0;
    field[o++] = 0xD5, field[o++] = 0xAA, field[o++] = 0xAD;
    field[o++] = gcr_encode6[sec];
    for (int v = 0; v < GCR_SECTOR_VALUES; v++)
        field[o++] = gcr_encode6[values[v]];
    field[o++] = 0xDE, field[o++] = 0xAA;
    for (size_t k = 0; k < o; k++)
        trk->data[(at + k) % trk->size] = field[k];
    trk->modified = true;
}

// A capture's index can fall anywhere on a track: here inside sector 4's
// data field, which then runs across the end of the buffer.  A guest write
// to that sector (a data field only, as the Sony driver writes) reaches the
// image at the eject, and the track rebuilt afterwards is the capture with
// that field spliced in.
#define WRAP_TRACK  10
#define WRAP_SECTOR 4
static void wrapped_capture(moof_t *m, uint32_t seed) {
    moof_make(m, SECTORS_800K, seed);
    uint32_t n;
    uint8_t *b = track_bits(m, WRAP_TRACK, 0, &n);
    rotate_track(m, WRAP_TRACK, 0, data_mark_of(b, n, WRAP_SECTOR) + 300 * 8);
    restamp(m);
}

TEST(straddling_sector_write_survives_eject) {
    moof_t m;
    wrapped_capture(&m, 21);
    image_t *img = open_moof(&m, "wrap.moof", true, false);
    floppy_track_t *trk = drive_load(img, WRAP_TRACK, 0);
    size_t clen = trk->size;
    moof_sector_t sec[MOOF_MAX_SECTORS];
    moof_scan_track(trk->data, clen, WRAP_TRACK, 0, sec, NULL);
    for (int k = 0; k < 12; k++)
        ASSERT_EQ_INT(MOOF_SECTOR_OK, sec[k].state);
    ASSERT_TRUE(sec[WRAP_SECTOR].data_at < clen && sec[WRAP_SECTOR].data_end > clen); // across the index
    uint64_t blk = block_of(WRAP_TRACK, 0, WRAP_SECTOR, 2);
    uint8_t buf[512], got[512];
    pattern(buf, 0x31);
    guest_write_field(trk, sec[WRAP_SECTOR].data_at, WRAP_SECTOR, m.tags + blk * MOOF_TAG_BYTES, buf);
    drive_eject(img);
    ASSERT_EQ_INT(512, (int)disk_read_data(img, blk * 512, got, 512));
    ASSERT_EQ_INT(0, memcmp(got, buf, 512));
    // Reinserted: the capture's length, every sector there, the one written new.
    size_t len;
    uint8_t *again = media_track(img, WRAP_TRACK, 0, &len);
    ASSERT_EQ_INT((int)clen, (int)len);
    moof_scan_track(again, len, WRAP_TRACK, 0, sec, NULL);
    uint64_t first = block_of(WRAP_TRACK, 0, 0, 2);
    for (int k = 0; k < 12; k++) {
        ASSERT_EQ_INT(MOOF_SECTOR_OK, sec[k].state);
        ASSERT_EQ_INT(0, memcmp(sec[k].data, k == WRAP_SECTOR ? buf : m.data + (first + (uint64_t)k) * 512, 512));
    }
    free(again);
    image_close(img);
    moof_free(&m);
}

// The same sector written whole (address field, then data field), as a
// format does, with the head crossing the index between the two: the
// write-through still finds the sector it started.
TEST(write_through_across_the_index) {
    moof_t m;
    wrapped_capture(&m, 22);
    image_t *img = open_moof(&m, "wrapwt.moof", true, false);
    floppy_track_t *trk = drive_load(img, WRAP_TRACK, 0);
    moof_sector_t sec[MOOF_MAX_SECTORS];
    moof_scan_track(trk->data, trk->size, WRAP_TRACK, 0, sec, NULL);
    uint64_t blk = block_of(WRAP_TRACK, 0, WRAP_SECTOR, 2);
    uint8_t buf[512], got[512];
    pattern(buf, 0x47);
    guest_write_field(trk, sec[WRAP_SECTOR].data_at, WRAP_SECTOR, m.tags + blk * MOOF_TAG_BYTES, buf);
    // The address field went by before the index; the DE AA closing the
    // data field has just been written after it.
    g_drive.write_hdr_start = (int)sec[WRAP_SECTOR].addr_at;
    g_drive.offset = (int)(sec[WRAP_SECTOR].data_end - trk->size);
    iwm_write_through(&g_drive, img, 0, 0);
    ASSERT_EQ_INT(-1, g_drive.write_hdr_start);
    ASSERT_EQ_INT(512, (int)disk_read_data(img, blk * 512, got, 512));
    ASSERT_EQ_INT(0, memcmp(got, buf, 512));
    drive_eject(img);
    image_close(img);
    moof_free(&m);
}

// An eject writes back the sectors of the track it flushes, each once: not
// one whose header names another track or side, and of two headers naming
// the same sector, the copy the capture's scan takes (the first that reads),
// which is what the image already holds.
#define DUP_TRACK 5
TEST(flush_writes_only_its_own_track_once) {
    moof_t m;
    moof_make(&m, SECTORS_800K, 24);
    uint32_t n;
    uint8_t *b = track_bits(&m, DUP_TRACK, 0, &n);
    readdress(&m, DUP_TRACK, 0, 2, DUP_TRACK + 1, 0, 2); // sector 2 claims track 6
    readdress(&m, DUP_TRACK, 0, 10, DUP_TRACK, 1, 10); // sector 10 claims side 1
    // Sector 8, address and data field, claims to be sector 3: a decoy
    // ahead of the real one (the interleave runs ... 2 8 3 9 ...).
    bits_put(b, data_mark_of(b, n, 8) + 24, gcr_encode6[3], 8);
    readdress(&m, DUP_TRACK, 0, 8, DUP_TRACK, 0, 3);
    restamp(&m);
    image_t *img = open_moof(&m, "dup.moof", true, false);
    uint64_t s3 = block_of(DUP_TRACK, 0, 3, 2), s8 = block_of(DUP_TRACK, 0, 8, 2);
    uint64_t foreign_track = block_of(DUP_TRACK + 1, 0, 2, 2), foreign_side = block_of(DUP_TRACK, 1, 10, 2);
    ASSERT_EQ_INT(IMAGE_SECTOR_MISSING, image_sector_status(img, block_of(DUP_TRACK, 0, 2, 2)));
    uint8_t got[512], buf[512];
    ASSERT_EQ_INT(512, (int)disk_read_data(img, s3 * 512, got, 512));
    ASSERT_EQ_INT(0, memcmp(got, m.data + s8 * 512, 512)); // the decoy is the capture's sector 3
    // The guest writes sector 0; the eject flushes the whole track.
    floppy_track_t *trk = drive_load(img, DUP_TRACK, 0);
    moof_sector_t sec[MOOF_MAX_SECTORS];
    moof_scan_track(trk->data, trk->size, DUP_TRACK, 0, sec, NULL);
    uint64_t s0 = block_of(DUP_TRACK, 0, 0, 2);
    pattern(buf, 0x6D);
    guest_write_field(trk, sec[0].data_at, 0, m.tags + s0 * MOOF_TAG_BYTES, buf);
    drive_eject(img);
    ASSERT_EQ_INT(512, (int)disk_read_data(img, s0 * 512, got, 512));
    ASSERT_EQ_INT(0, memcmp(got, buf, 512));
    ASSERT_EQ_INT(512, (int)disk_read_data(img, foreign_track * 512, got, 512));
    ASSERT_EQ_INT(0, memcmp(got, m.data + foreign_track * 512, 512)); // track 6 untouched
    ASSERT_EQ_INT(512, (int)disk_read_data(img, foreign_side * 512, got, 512));
    ASSERT_EQ_INT(0, memcmp(got, m.data + foreign_side * 512, 512)); // side 1 untouched
    ASSERT_EQ_INT(512, (int)disk_read_data(img, s3 * 512, got, 512));
    ASSERT_EQ_INT(0, memcmp(got, m.data + s8 * 512, 512)); // still the decoy
    ASSERT_TRUE(!image_block_in_delta(img, foreign_track));
    image_close(img);
    moof_free(&m);
}

// An eject's flush of a captured track the guest wrote keeps the capture:
// rewriting a sector with what it holds leaves the track the capture byte
// for byte, and a real change splices just that sector, the signature on
// another sector kept.
TEST(eject_flush_keeps_the_capture) {
    moof_t m;
    moof_make(&m, SECTORS_800K, 25);
    protect(&m);
    image_t *img = open_moof(&m, "flushcap.moof", true, false);
    size_t clen;
    moof_sector_t sec[MOOF_MAX_SECTORS];
    uint8_t *cap = capture_track(img, SIG_TRACK, 0, &clen, sec);
    uint64_t blk = block_of(SIG_TRACK, 0, 1, 2);
    floppy_track_t *trk = drive_load(img, SIG_TRACK, 0);
    guest_write_field(trk, sec[1].data_at, 1, m.tags + blk * MOOF_TAG_BYTES, m.data + blk * 512);
    drive_eject(img);
    size_t len;
    uint8_t *got = media_track(img, SIG_TRACK, 0, &len);
    ASSERT_TRUE(len == clen && memcmp(got, cap, len) == 0);
    free(got);
    uint8_t buf[512];
    pattern(buf, 0x7E);
    trk = drive_load(img, SIG_TRACK, 0);
    guest_write_field(trk, sec[1].data_at, 1, m.tags + blk * MOOF_TAG_BYTES, buf);
    drive_eject(img);
    got = media_track(img, SIG_TRACK, 0, &len);
    ASSERT_EQ_INT((int)clen, (int)len);
    ASSERT_EQ_INT(1, count_seq(got, len, SIG, 4));
    moof_scan_track(got, len, SIG_TRACK, 0, sec, NULL);
    ASSERT_EQ_INT(MOOF_SECTOR_OK, sec[1].state);
    ASSERT_EQ_INT(0, memcmp(sec[1].data, buf, 512));
    free(got);
    free(cap);
    image_close(img);
    moof_free(&m);
}

int main(void) {
    strcpy(g_dir, "/tmp/gs-moof-capture-XXXXXX");
    ASSERT_TRUE(mkdtemp(g_dir) != NULL);
    setenv("GS_STORAGE_CACHE", g_dir, 1);
    RUN(capture_status_and_unreadable_sector);
    RUN(unchanged_track_is_the_capture);
    RUN(changed_sector_is_spliced);
    RUN(unreadable_sector_and_the_delta);
    RUN(all_changed_is_synthesised);
    RUN(single_sided_capture_side_1);
    RUN(write_protect_flag);
    RUN(splice_growth_and_wrap);
    RUN(track_length_is_the_buffers);
    RUN(export_copies_unchanged_tracks);
    RUN(epilogue_less_field_keeps_its_neighbour);
    RUN(splice_refuses_to_cover_an_address_field);
    RUN(straddling_sector_write_survives_eject);
    RUN(write_through_across_the_index);
    RUN(flush_writes_only_its_own_track_once);
    RUN(eject_flush_keeps_the_capture);
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_dir);
    if (system(cmd) != 0)
        return 1;
    return 0;
}
