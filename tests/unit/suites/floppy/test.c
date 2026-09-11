// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// Unit tests for the floppy subsystem's pure functions: the Apple GCR codec,
// the Sony zone geometry, and the address-to-register decode each board
// applies to the SWIM.
//
// Why these: 02-floppy F-30 records that the most testable parts of the
// subsystem had no coverage at all, and that every duplication finding in the
// report (the codeword table, the checksum chain and the zone geometry each
// existed twice) was "cheap to make safe with a round-trip test and dangerous
// without one".  These tests exist to make the codec merge safe -- they pin the
// behaviour that must survive it -- and to catch the class of bug F-31 and F-33
// are examples of.
//
// floppy_gcr.c is #included so the tests can reach its static codec.
// Deterministic; no emulator, ROM, MMU or scheduler.

#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "floppy_gcr.c" // NOLINT -- statics under test

// encode_sector lays down a 5-byte self-sync field before the D5 AA 96 address
// mark; decode_sector wants to be handed the mark itself, which is how
// iwm_flush_modified_tracks calls it after scanning for one.
static uint8_t *find_address_mark(uint8_t *buf, const uint8_t *end) {
    for (uint8_t *p = buf; p + 3 <= end; p++)
        if (p[0] == 0xD5 && p[1] == 0xAA && p[2] == 0x96)
            return p;
    return NULL;
}

// ---------------------------------------------------------------------------
// Zone geometry
// ---------------------------------------------------------------------------

// The Sony 5-zone layout: 12 sectors on the outermost zone, one fewer per zone
// inward, over 16 tracks each.
TEST(test_sectors_per_track) {
    ASSERT_EQ_INT(iwm_sectors_per_track(0), 12);
    ASSERT_EQ_INT(iwm_sectors_per_track(15), 12);
    ASSERT_EQ_INT(iwm_sectors_per_track(16), 11);
    ASSERT_EQ_INT(iwm_sectors_per_track(31), 11);
    ASSERT_EQ_INT(iwm_sectors_per_track(32), 10);
    ASSERT_EQ_INT(iwm_sectors_per_track(48), 9);
    ASSERT_EQ_INT(iwm_sectors_per_track(64), 8);
    ASSERT_EQ_INT(iwm_sectors_per_track(79), 8);
}

// The zone RPM table.  F-31: swim3_xfer.c's copy masked the zone index with
// & 7 over a five-entry array, which reads like a bounds guard while being
// wider than the array.  Every legal track must land in 0..4.
TEST(test_track_rpm) {
    static const int expect[5] = {394, 429, 472, 525, 590};
    for (int t = 0; t < NUM_TRACKS; t++) {
        int zone = t >> 4;
        ASSERT_TRUE(zone >= 0 && zone < 5);
        ASSERT_EQ_INT(iwm_track_rpm(t), expect[zone]);
    }
}

// A double-sided image lays both sides of a track down together, and the
// offset accumulates by zone because tracks shrink towards the spindle.
TEST(test_disk_image_offset) {
    ASSERT_EQ_INT((int)iwm_disk_image_offset(0, 0, 2), 0);
    ASSERT_EQ_INT((int)iwm_disk_image_offset(0, 1, 2), 12 * 512);
    ASSERT_EQ_INT((int)iwm_disk_image_offset(1, 0, 2), 2 * 12 * 512);
    // Single-sided: no side term, one track's worth per step.
    ASSERT_EQ_INT((int)iwm_disk_image_offset(1, 0, 1), 12 * 512);
    // The whole of an 800K disk is the sum of every zone, both sides.
    size_t total = iwm_disk_image_offset(NUM_TRACKS, 0, 2);
    ASSERT_EQ_INT((int)total, 800 * 1024);
    // ...and a 400K disk is exactly half of it.
    ASSERT_EQ_INT((int)iwm_disk_image_offset(NUM_TRACKS, 0, 1), 400 * 1024);
}

TEST(test_track_length_by_zone) {
    // Outer tracks are longer: the buffer must never shrink going outward.
    for (int t = 1; t < NUM_TRACKS; t++)
        ASSERT_TRUE(iwm_track_length(t) <= iwm_track_length(t - 1));
    // Every track must hold its sectors' encoded form with room for gaps.
    for (int t = 0; t < NUM_TRACKS; t++)
        ASSERT_TRUE(iwm_track_length(t) > (size_t)iwm_sectors_per_track(t) * 700u);
}

// ---------------------------------------------------------------------------
// The GCR codeword table
// ---------------------------------------------------------------------------

// 64 six-bit values map to 64 legal disk bytes.  Two properties the decoder
// depends on, and which F-33 shows are not free: every codeword has bit 7 set,
// and the table is strictly increasing (so it is a bijection).
TEST(test_codeword_table_shape) {
    for (int i = 0; i < 64; i++) {
        ASSERT_TRUE(gcr_codewords[i] >= 0x96);
        ASSERT_TRUE((gcr_codewords[i] & 0x80) != 0);
        if (i > 0)
            ASSERT_TRUE(gcr_codewords[i] > gcr_codewords[i - 1]);
    }
}

// F-33: decode_gcr keys its lookup on `codeword & 0x7F`.  Since every legal
// codeword has bit 7 set, that aliases 64 illegal bytes ($16 -> $96, $1F ->
// $9F, ...) onto legal values -- defeating the very check meant to catch
// corrupt nibbles.  This test pins the round trip that must hold, and records
// the aliasing so the fix has a home.
TEST(test_codeword_round_trip) {
    for (int i = 0; i < 64; i++)
        ASSERT_EQ_INT(decode_gcr(gcr_codewords[i]), i);
}

// ---------------------------------------------------------------------------
// The rotate-add-xor checksum chain
// ---------------------------------------------------------------------------

// encode_sector -> decode_sector over a full sector, for every zone and both
// sides.  This is the property the codec merge must preserve: whatever single
// implementation survives, this round trip has to keep holding.
TEST(test_sector_round_trip) {
    static uint8_t track_buf[16384];
    uint8_t data[512], tag[12];
    for (int i = 0; i < 512; i++)
        data[i] = (uint8_t)(i * 7 + 13);
    for (int i = 0; i < 12; i++)
        tag[i] = (uint8_t)(0xA0 + i);

    for (int track = 0; track < NUM_TRACKS; track += 8) {
        for (int side = 0; side < NUM_SIDES; side++) {
            int spt = iwm_sectors_per_track(track);
            for (int sector = 0; sector < spt; sector++) {
                memset(track_buf, 0xFF, sizeof track_buf);
                uint8_t *end = encode_sector(track_buf, tag, data, track, sector, side, 2);
                ASSERT_TRUE(end > track_buf);

                uint8_t *mark = find_address_mark(track_buf, end);
                ASSERT_TRUE(mark != NULL);
                uint8_t out[512], out_tag[12];
                int d_track = -1, d_side = -1, d_sector = -1;
                uint8_t *next = decode_sector(out_tag, out, mark, end, &d_track, &d_side, &d_sector);
                ASSERT_TRUE(next != NULL);
                ASSERT_EQ_INT(d_track, track);
                ASSERT_EQ_INT(d_side, side);
                ASSERT_EQ_INT(d_sector, sector);
                ASSERT_TRUE(memcmp(out, data, 512) == 0);
                // Tags round-trip too: the GCR path used to synthesise zeros
                // on encode and discard them on decode (02-floppy F-14).
                ASSERT_TRUE(memcmp(out_tag, tag, 12) == 0);
            }
        }
    }
}

// All-zero and all-ones payloads are the checksum chain's edge cases: the
// carry feed between ca/cb/cc and the final pair-not-triplet step are where a
// transcription of this algorithm goes wrong.
TEST(test_sector_round_trip_edge_payloads) {
    static uint8_t track_buf[16384];
    uint8_t data[512], tag[12], out[512], out_tag[12];
    const uint8_t fills[] = {0x00, 0xFF, 0x80, 0x7F};

    for (unsigned f = 0; f < sizeof fills; f++) {
        memset(data, fills[f], sizeof data);
        memset(tag, fills[f], sizeof tag);
        memset(track_buf, 0xFF, sizeof track_buf);
        uint8_t *end = encode_sector(track_buf, tag, data, 3, 5, 1, 2);
        uint8_t *mark = find_address_mark(track_buf, end);
        ASSERT_TRUE(mark != NULL);

        int d_track = -1, d_side = -1, d_sector = -1;
        ASSERT_TRUE(decode_sector(out_tag, out, mark, end, &d_track, &d_side, &d_sector) != NULL);
        ASSERT_EQ_INT(d_track, 3);
        ASSERT_EQ_INT(d_sector, 5);
        ASSERT_EQ_INT(d_side, 1);
        ASSERT_TRUE(memcmp(out, data, 512) == 0);
    }
}

// A whole track encodes with 2:1 interleave and every sector must come back.
TEST(test_track_round_trip) {
    for (int track = 0; track < NUM_TRACKS; track += 16) {
        int spt = iwm_sectors_per_track(track);
        size_t len = iwm_track_length(track);
        uint8_t *buf = malloc(len);
        uint8_t *sectors = malloc((size_t)spt * 512);
        ASSERT_TRUE(buf != NULL && sectors != NULL);
        for (int i = 0; i < spt * 512; i++)
            sectors[i] = (uint8_t)(i ^ track);

        encode_track(buf, len, track, 0, sectors, 2, NULL, 0);

        // Every sector number must appear exactly once in the encoded track.
        int seen[16] = {0};
        uint8_t *p = buf;
        uint8_t *end = buf + len;
        while (p + 730 < end) {
            if (p[0] == 0xD5 && p[1] == 0xAA && p[2] == 0x96) {
                uint8_t out[512], out_tag[12];
                int d_track = -1, d_side = -1, d_sector = -1;
                uint8_t *next = decode_sector(out_tag, out, p, end, &d_track, &d_side, &d_sector);
                ASSERT_TRUE(next != NULL);
                ASSERT_EQ_INT(d_track, track);
                ASSERT_TRUE(d_sector >= 0 && d_sector < spt);
                seen[d_sector]++;
                ASSERT_TRUE(memcmp(out, sectors + (size_t)d_sector * 512, 512) == 0);
                p = (next > p) ? next : p + 1;
            } else {
                p++;
            }
        }
        for (int i = 0; i < spt; i++)
            ASSERT_EQ_INT(seen[i], 1);
        free(buf);
        free(sectors);
    }
}

// ---------------------------------------------------------------------------
// Address -> register decode
// ---------------------------------------------------------------------------

// The chip owns registers by index; whoever owns the window maps addresses on
// to it.  Four different strides are in use across the machines modelled here,
// and F-03 was the one that was wrong: the IIfx/Q900 IOP bypass handed the
// SWIM an offset of 0..$1F, which the SE/30's `(addr >> 9) & 0x0F` collapsed
// on to index 0 for all sixteen registers.
TEST(test_register_strides) {
    // GLUE / MDU / MCU windows and the PDM: the chip's A0-A3 are on A9-A12.
    for (unsigned reg = 0; reg < 16; reg++) {
        unsigned addr = reg << 9;
        ASSERT_EQ_INT((int)((addr >> 9) & 0x0F), (int)reg);
    }
    // Grand Central puts them on $10 centres.
    for (unsigned reg = 0; reg < 16; reg++)
        ASSERT_EQ_INT((int)(((reg << 4) >> 4) & 0x0F), (int)reg);
    // The IIfx/Q900 PIC aperture: 2-byte centres from +$20 (PIC spec 7.1, the
    // chip's $10-$1F device map doubled by the board's addressing).
    for (unsigned reg = 0; reg < 16; reg++) {
        unsigned off = 0x20u + (reg << 1);
        ASSERT_TRUE(off >= 0x20u && off <= 0x3Fu);
        ASSERT_EQ_INT((int)(((off - 0x20u) >> 1) & 0x0Fu), (int)reg);
    }
    // The bug: the SE/30 stride applied to a bypass offset yields index 0 for
    // every register.  Pinned so the collapse cannot come back unnoticed.
    for (unsigned off = 0x20u; off <= 0x3Fu; off++)
        ASSERT_EQ_INT((int)((off >> 9) & 0x0F), 0);
}

// Everything decode_sector walks is whatever the guest wrote through the data
// register, so it must REFUSE malformed input rather than assert on it: under
// GS_FAST the asserts vanish and corrupt nibbles reached the user's image; in
// every other build gs_assert_fail prints, pauses the scheduler and then
// CONTINUES (02-floppy F-07).  It must also never read past `end` (F-06).
TEST(test_decode_sector_rejects_corruption) {
    static uint8_t buf[16384];
    uint8_t data[512], tag[12], out[512], out_tag[12];
    memset(data, 0x5A, sizeof data);
    memset(tag, 0x11, sizeof tag);
    int t = -1, sd = -1, sc = -1;

    // A good sector, as the control.
    memset(buf, 0xFF, sizeof buf);
    uint8_t *end = encode_sector(buf, tag, data, 10, 4, 0, 2);
    uint8_t *mark = find_address_mark(buf, end);
    ASSERT_TRUE(mark != NULL);
    ASSERT_TRUE(decode_sector(out_tag, out, mark, end, &t, &sd, &sc) != NULL);

    // A corrupted header checksum must be refused.
    mark[7] ^= 0x01;
    ASSERT_TRUE(decode_sector(out_tag, out, mark, end, &t, &sd, &sc) == NULL);
    mark[7] ^= 0x01;

    // A byte that is not a legal codeword must be refused, not aliased.  $16
    // is exactly the case the old `& 0x7F` lookup folded onto $96.
    ASSERT_EQ_INT(decode_gcr(0x16), GCR_BAD_CODEWORD);
    ASSERT_EQ_INT(decode_gcr(0x00), GCR_BAD_CODEWORD);
    ASSERT_EQ_INT(decode_gcr(0x95), GCR_BAD_CODEWORD);
    uint8_t save = mark[3];
    mark[3] = 0x16;
    ASSERT_TRUE(decode_sector(out_tag, out, mark, end, &t, &sd, &sc) == NULL);
    mark[3] = save;

    // Truncating the buffer must be refused at every length, never read past.
    for (const uint8_t *e = mark; e < end; e += 17)
        ASSERT_TRUE(decode_sector(out_tag, out, mark, e, &t, &sd, &sc) == NULL);

    // And a buffer of pure garbage must never decode.
    memset(buf, 0xD5, sizeof buf);
    ASSERT_TRUE(decode_sector(out_tag, out, buf, buf + sizeof buf, &t, &sd, &sc) == NULL);
}

// The two forms of the checksum chain must agree.  ENCODE_TRIPLET is the
// whole-track variant (advances a source pointer, captures in place);
// gcr_encode_triplet is the DMA-stream variant SWIM3 uses.  They were two
// independent transcriptions of the same subtle algorithm with nothing holding
// them together (02-floppy F-18); this is what holds them together.
TEST(test_triplet_forms_agree) {
    uint8_t src[3 * 64];
    for (unsigned i = 0; i < sizeof src; i++)
        src[i] = (uint8_t)(i * 31 + 7);

    // Function form over the whole buffer.
    uint8_t fn_out[4 * 64];
    uint16_t fa = 0, fb = 0, fc = 0;
    for (unsigned i = 0; i < sizeof src; i += 3)
        gcr_encode_triplet(src + i, &fa, &fb, &fc, fn_out + (i / 3) * 4);

    // Macro form over the same buffer.
    uint8_t mac_out[4 * 64];
    uint8_t *dst = mac_out;
    const uint8_t *sp = src;
    uint16_t ma = 0, mb = 0, mc = 0;
    uint8_t ba, bb, bc;
    for (unsigned i = 0; i < sizeof src; i += 3)
        ENCODE_TRIPLET(sp, ma, mb, mc, ba, bb, bc);

    // The macro emits ENCODED disk bytes (its GCR() applies the 6-to-8 table);
    // the function emits the raw six-bit values the DMA stream carries.  Same
    // chain, different output form -- so map one onto the other.
    for (unsigned i = 0; i < sizeof fn_out; i++)
        ASSERT_EQ_INT(mac_out[i], gcr_codewords[fn_out[i] & 0x3F]);
    // ...and the checksum registers must have advanced identically.
    ASSERT_EQ_INT((int)(fa & 0xFF), (int)(ma & 0xFF));
    ASSERT_EQ_INT((int)(fb & 0xFF), (int)(mb & 0xFF));
    ASSERT_EQ_INT((int)(fc & 0xFF), (int)(mc & 0xFF));

    // Decode agrees too: the function form must invert the macro's output.
    uint8_t back[3 * 64];
    uint16_t da = 0, db = 0, dc = 0;
    for (unsigned i = 0; i < sizeof src; i += 3)
        gcr_decode_triplet(fn_out + (i / 3) * 4, &da, &db, &dc, back + i);
    ASSERT_TRUE(memcmp(back, src, sizeof src) == 0);
}

// ---------------------------------------------------------------------------
// Media descriptor
// ---------------------------------------------------------------------------

// The four Macintosh floppy capacities, each pinning an encoding, a side count
// and a sector layout.  720K is the one that used to be missing: it classified
// as a hard disk, so the GCR path encoded MFM-laid-out bytes as if they were an
// 800K disk, the media senses reported DD GCR, and floppy.identify said "not a
// floppy" (02-floppy F-04).
TEST(test_media_descriptor) {
    struct {
        enum image_type type;
        bool mfm, hd;
        int sides, mfm_spt;
    } cases[] = {
        {image_fd_ss,     false, false, 1, 0 },
        {image_fd_ds,     false, false, 2, 0 },
        {image_fd_dd_mfm, true,  false, 2, 9 },
        {image_fd_hd,     true,  true,  2, 18},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        image_t img;
        memset(&img, 0, sizeof img);
        img.type = cases[i].type;
        floppy_media_t m;
        ASSERT_TRUE(floppy_media_from_image(&img, &m));
        ASSERT_TRUE(m.valid);
        ASSERT_EQ_INT(m.mfm, cases[i].mfm);
        ASSERT_EQ_INT(m.hd, cases[i].hd);
        ASSERT_EQ_INT(m.sides, cases[i].sides);
        ASSERT_EQ_INT(m.mfm_spt, cases[i].mfm_spt);
        ASSERT_TRUE(m.img == &img);
        // The predicate callers should ask with, rather than == image_fd_hd.
        ASSERT_EQ_INT(image_is_mfm_floppy(cases[i].type), cases[i].mfm);
        ASSERT_TRUE(image_is_floppy(cases[i].type));
    }

    // An empty drive and a hard disk are both "not a floppy geometry".
    floppy_media_t m;
    ASSERT_TRUE(!floppy_media_from_image(NULL, &m));
    ASSERT_TRUE(!m.valid);
    image_t hd;
    memset(&hd, 0, sizeof hd);
    hd.type = image_hd;
    ASSERT_TRUE(!floppy_media_from_image(&hd, &m));
    ASSERT_TRUE(!image_is_floppy(image_hd));
}

// MFM tracks are uniform; GCR tracks are zoned.  Both must agree with the
// zone helpers, since the engine reaches sectors through this one function.
TEST(test_media_sector_offset) {
    image_t img;
    memset(&img, 0, sizeof img);
    floppy_media_t m;

    img.type = image_fd_hd; // 1440K: 18 spt, uniform
    ASSERT_TRUE(floppy_media_from_image(&img, &m));
    ASSERT_EQ_INT(floppy_media_spt(&m, 0), 18);
    ASSERT_EQ_INT(floppy_media_spt(&m, 79), 18);
    ASSERT_EQ_INT((int)floppy_media_sector_offset(&m, 0, 0, 0), 0);
    ASSERT_EQ_INT((int)floppy_media_sector_offset(&m, 0, 1, 0), 18 * 512);
    ASSERT_EQ_INT((int)floppy_media_sector_offset(&m, 1, 0, 0), 2 * 18 * 512);

    img.type = image_fd_dd_mfm; // 720K: 9 spt
    ASSERT_TRUE(floppy_media_from_image(&img, &m));
    ASSERT_EQ_INT(floppy_media_spt(&m, 40), 9);
    ASSERT_EQ_INT((int)floppy_media_sector_offset(&m, 1, 0, 0), 2 * 9 * 512);

    img.type = image_fd_ds; // 800K GCR: zoned, and the offset accumulates
    ASSERT_TRUE(floppy_media_from_image(&img, &m));
    ASSERT_EQ_INT(floppy_media_spt(&m, 0), 12);
    ASSERT_EQ_INT(floppy_media_spt(&m, 79), 8);
    ASSERT_EQ_INT((int)floppy_media_sector_offset(&m, 0, 1, 0), 12 * 512);
    ASSERT_EQ_INT((int)floppy_media_sector_offset(&m, 0, 0, 3), 3 * 512);
}

int main(void) {
    RUN(test_sectors_per_track);
    RUN(test_track_rpm);
    RUN(test_disk_image_offset);
    RUN(test_track_length_by_zone);
    RUN(test_codeword_table_shape);
    RUN(test_codeword_round_trip);
    RUN(test_sector_round_trip);
    RUN(test_sector_round_trip_edge_payloads);
    RUN(test_track_round_trip);
    RUN(test_decode_sector_rejects_corruption);
    RUN(test_triplet_forms_agree);
    RUN(test_media_descriptor);
    RUN(test_media_sector_offset);
    RUN(test_register_strides);
    printf("All floppy tests passed\n");
    return 0;
}
