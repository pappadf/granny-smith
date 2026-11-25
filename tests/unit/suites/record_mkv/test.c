// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The recorder's Matroska muxer (mkv_mux.c) against its reader
// (mkv_read.c): known packets on a video and an audio track come back
// identically, interleaved in time order, at the timestamps the frame
// period gives, in clusters that open on keyframes and reach no further
// than a second or a megabyte, with Cues pointing at those clusters, the
// segment size and duration patched; a video-only file; a file cut in
// the middle of a cluster, and one never closed, still read up to their
// last complete cluster.

#include "mkv_mux.h"
#include "mkv_read.h"
#include "test_assert.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TEST_OUT_DIR
#define TEST_OUT_DIR "."
#endif

#define FRAME_NS  16625103ull // one VBL period: 1203/20 fps
#define RATE      22255u
#define AUDIO_BLK 4096u
#define MAX_PKTS  2048

// A packet as written, to compare with what is read.
typedef struct {
    int track; // MUX_TRACK_*
    uint64_t pts_ns;
    bool key;
    uint8_t *data;
    size_t len;
} sent_t;

static sent_t s_sent[MAX_PKTS];
static int s_nsent;

static uint32_t s_rng = 0x9E3779B9u;

// A small deterministic generator (xorshift32).
static uint32_t rnd(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

// A path under the build directory.
static void out_path(char *buf, size_t n, const char *name) {
    snprintf(buf, n, "%s/record_mkv_%s", TEST_OUT_DIR, name);
}

// Record and send one packet of `len` random bytes.
static void send(muxer_t *m, int track, uint64_t pts, uint64_t dur, bool key, size_t len) {
    ASSERT_TRUE(s_nsent < MAX_PKTS);
    sent_t *s = &s_sent[s_nsent++];
    s->track = track;
    s->pts_ns = pts;
    s->key = key;
    s->len = len;
    s->data = malloc(len ? len : 1);
    ASSERT_TRUE(s->data != NULL);
    for (size_t i = 0; i < len; i++)
        s->data[i] = (uint8_t)rnd();
    ASSERT_EQ_INT(0, mux_packet(m, track, pts, dur, key, s->data, len));
}

static void free_sent(void) {
    for (int i = 0; i < s_nsent; i++)
        free(s_sent[i].data);
    s_nsent = 0;
}

// The milliseconds a pts becomes: rounded to the nearest.
static int64_t ms_of(uint64_t ns) {
    return (int64_t)((ns + 500000) / 1000000);
}

// The video track's parameters for the tests.
static mux_video_t video_params(void) {
    return (mux_video_t){.width = 640,
                         .height = 480,
                         .display_width = 640,
                         .display_height = 480,
                         .frame_period_ns = FRAME_NS,
                         .bits_per_pixel = 8};
}

// The audio track's parameters (a STREAMINFO of recognisable bytes).
static mux_audio_t audio_params(void) {
    mux_audio_t a = {.sample_rate = RATE, .channels = 1, .bits_per_sample = 16};
    for (int i = 0; i < MUX_STREAMINFO_LEN; i++)
        a.streaminfo[i] = (uint8_t)(i * 7 + 1);
    return a;
}

// Write `frames` video frames (keyframe every 120, one of 1.5 MB at
// frame 200 when `big`) and, with `audio`, the audio blocks covering
// them, pushed as a recorder would: each audio block once its time has
// passed.  The muxer is left open.
static muxer_t *write_file(const char *path, int frames, bool audio, bool big) {
    mux_video_t v = video_params();
    mux_audio_t a = audio_params();
    int err = 0;
    muxer_t *m = mkv_mux_open(path, &v, audio ? &a : NULL, &err);
    ASSERT_EQ_INT(0, err);
    ASSERT_TRUE(m != NULL);
    uint64_t next_audio = 0; // the next audio block's first sample
    for (int k = 0; k < frames; k++) {
        uint64_t pts = (uint64_t)k * FRAME_NS;
        bool key = k % 120 == 0;
        size_t len = key ? 3000 + rnd() % 2000 : (k % 3 ? 12 : 200 + rnd() % 3000);
        if (big && k == 200)
            len = 1500000;
        send(m, MUX_TRACK_VIDEO, pts, 0, key, len);
        // Audio blocks whose span has ended by the end of this frame.
        while (audio && (next_audio + AUDIO_BLK) * 1000000000ull / RATE <= pts + FRAME_NS) {
            uint64_t apts = next_audio * 1000000000ull / RATE;
            uint64_t adur = (uint64_t)AUDIO_BLK * 1000000000ull / RATE;
            send(m, MUX_TRACK_AUDIO, apts, adur, true, 14 + rnd() % 900);
            next_audio += AUDIO_BLK;
        }
    }
    return m;
}

// Read `path` back: every packet must be the next one sent on its track,
// in non-decreasing time across tracks.  Returns the packets read.
static int read_back(const char *path, bool audio, int *per_track) {
    int err = 0;
    mkv_reader_t *r = mkv_read_open(path, &err);
    if (!r)
        fprintf(stderr, "       open %s: %d\n", path, err);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT(audio ? 2 : 1, mkv_read_track_count(r));
    ASSERT_TRUE(mkv_read_timestamp_scale(r) == 1000000);
    const mkv_track_t *vt = mkv_read_track(r, 0);
    ASSERT_TRUE(vt->number == 1 && vt->type == MKV_TRACK_VIDEO);
    ASSERT_TRUE(strcmp(vt->codec_id, "V_MS/VFW/FOURCC") == 0);
    ASSERT_TRUE(vt->default_duration_ns == FRAME_NS);
    ASSERT_TRUE(vt->pixel_width == 640 && vt->pixel_height == 480 && vt->display_width == 640);
    // The BITMAPINFOHEADER: size 40, the dimensions, 8 bits, FourCC ZMBV.
    ASSERT_TRUE(vt->codec_private_len == 40 && vt->codec_private[0] == 40 && vt->codec_private[4] == 0x80 &&
                vt->codec_private[5] == 2 && vt->codec_private[14] == 8 && !memcmp(vt->codec_private + 16, "ZMBV", 4));
    if (audio) {
        const mkv_track_t *at = mkv_read_track(r, 1);
        mux_audio_t a = audio_params();
        ASSERT_TRUE(at->number == 2 && at->type == MKV_TRACK_AUDIO && strcmp(at->codec_id, "A_FLAC") == 0);
        ASSERT_TRUE(at->sampling_frequency == RATE && at->channels == 1 && at->bit_depth == 16);
        ASSERT_TRUE(at->codec_private_len == 42 && !memcmp(at->codec_private, "fLaC\x80\x00\x00\x22", 8) &&
                    !memcmp(at->codec_private + 8, a.streaminfo, MUX_STREAMINFO_LEN));
    }
    int next[2] = {0, 0}, total = 0;
    int64_t last_ts = -1;
    mkv_packet_t p;
    int rc;
    while ((rc = mkv_read_packet(r, &p)) == 1) {
        int track = (int)p.track - 1;
        ASSERT_TRUE(track == 0 || (audio && track == 1));
        // The next packet sent on that track.
        int i = next[track];
        while (i < s_nsent && s_sent[i].track != track)
            i++;
        ASSERT_TRUE(i < s_nsent);
        next[track] = i + 1;
        ASSERT_EQ_INT((int)s_sent[i].len, (int)p.len);
        ASSERT_TRUE(memcmp(s_sent[i].data, p.data, p.len) == 0);
        ASSERT_EQ_INT(s_sent[i].key, p.keyframe);
        ASSERT_TRUE(p.timestamp == ms_of(s_sent[i].pts_ns));
        // Time order across the tracks.
        ASSERT_TRUE(p.timestamp >= last_ts);
        last_ts = p.timestamp;
        per_track[track]++;
        total++;
    }
    ASSERT_EQ_INT(0, rc);
    mkv_read_close(r);
    return total;
}

// A finished file: everything round-trips, and its index and sizes are right.
TEST(mkv_round_trip) {
    char path[512];
    out_path(path, sizeof(path), "full.mkv");
    muxer_t *m = write_file(path, 600, true, true);
    ASSERT_TRUE(mux_bytes(m) > 1500000);
    ASSERT_EQ_INT(0, mux_close(m));
    int per[2] = {0, 0};
    int n = read_back(path, true, per);
    ASSERT_EQ_INT(s_nsent, n);
    ASSERT_EQ_INT(600, per[0]);
    fprintf(stderr, "       %d video, %d audio packets\n", per[0], per[1]);
    // Frame k's timestamp is round(k * P / 1e6): frame 1 at 17, 3 at 50, 599 at 9958.
    ASSERT_EQ_INT(17, (int)ms_of(FRAME_NS));
    ASSERT_EQ_INT(50, (int)ms_of(3 * FRAME_NS));
    ASSERT_EQ_INT(9958, (int)ms_of(599 * FRAME_NS));

    int err = 0;
    mkv_reader_t *r = mkv_read_open(path, &err);
    ASSERT_TRUE(r != NULL);
    mkv_packet_t p;
    // Clusters: note where each block's cluster starts by watching the
    // positions grow, then check each cluster's span.
    size_t seen = 0;
    int64_t first_ts = 0, last_ts = 0;
    while (mkv_read_packet(r, &p) == 1) {
        const uint64_t *pos;
        size_t nc = mkv_read_clusters(r, &pos);
        if (nc != seen) {
            if (seen)
                ASSERT_TRUE(last_ts - first_ts < 1000); // a cluster spans under a second
            seen = nc;
            first_ts = p.timestamp;
        } else {
            // Every video keyframe opens a cluster.
            ASSERT_TRUE(!(p.track == 1 && p.keyframe));
        }
        last_ts = p.timestamp;
    }
    ASSERT_TRUE(!mkv_read_truncated(r));
    ASSERT_TRUE(mkv_read_sized(r));
    // Duration: the last frame's end, 600 frames, in ms.
    double want = 600.0 * FRAME_NS / 1e6;
    ASSERT_TRUE(mkv_read_duration(r) >= want - 0.001 && mkv_read_duration(r) < want + 200);
    // Cues: one per keyframe (every 120 frames), each at a cluster.
    const mkv_read_cue_t *cues;
    const uint64_t *pos;
    size_t ncues = mkv_read_cues(r, &cues), nclusters = mkv_read_clusters(r, &pos);
    ASSERT_EQ_INT(5, (int)ncues);
    fprintf(stderr, "       %d clusters, %d cues\n", (int)nclusters, (int)ncues);
    ASSERT_TRUE(nclusters >= 10);
    for (size_t i = 0; i < ncues; i++) {
        ASSERT_TRUE(cues[i].track == 1);
        ASSERT_TRUE(cues[i].time == (uint64_t)ms_of(i * 120 * FRAME_NS));
        bool found = false;
        for (size_t c = 0; c < nclusters; c++)
            found |= pos[c] == cues[i].cluster_pos;
        ASSERT_TRUE(found);
    }
    mkv_read_close(r);

    // The 1.5 MB frame sits in a cluster of its own beyond the 1 MB mark:
    // the packets after it still come back (checked by read_back above).
    free_sent();
}

// Video only: one track, the same guarantees.
TEST(mkv_video_only) {
    char path[512];
    out_path(path, sizeof(path), "video.mkv");
    muxer_t *m = write_file(path, 250, false, false);
    ASSERT_EQ_INT(0, mux_close(m));
    int per[2] = {0, 0};
    ASSERT_EQ_INT(250, read_back(path, false, per));
    free_sent();
}

// Copy the first `n` bytes of `from` to `to`.
static void copy_prefix(const char *from, const char *to, long n) {
    FILE *a = fopen(from, "rb"), *b = fopen(to, "wb");
    ASSERT_TRUE(a && b);
    for (long i = 0; i < n; i++) {
        int c = fgetc(a);
        if (c == EOF)
            break;
        fputc(c, b);
    }
    fclose(a);
    fclose(b);
}

// The size of a file.
static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    ASSERT_TRUE(f != NULL);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

// Cut in the middle of a cluster: every complete cluster's packets, then
// a clean end that says the file was cut.
TEST(mkv_truncated_mid_cluster) {
    char path[512], cut[512];
    out_path(path, sizeof(path), "trunc.mkv");
    out_path(cut, sizeof(cut), "trunc-cut.mkv");
    muxer_t *m = write_file(path, 400, true, false);
    ASSERT_EQ_INT(0, mux_close(m));
    // Find the clusters, and cut halfway into the fourth.
    int err = 0;
    mkv_reader_t *r = mkv_read_open(path, &err);
    ASSERT_TRUE(r != NULL);
    mkv_packet_t p;
    int in_first3 = 0;
    while (mkv_read_packet(r, &p) == 1)
        if (mkv_read_clusters(r, NULL) <= 3)
            in_first3++;
    const uint64_t *pos;
    size_t nc = mkv_read_clusters(r, &pos);
    ASSERT_TRUE(nc >= 5);
    // Positions are from the segment's data; the cluster after the fourth
    // marks its end.
    uint64_t seg_data = 0;
    {
        // The segment's data starts where the reader's first cluster
        // offset says, less that offset: recover it from the file.
        FILE *f = fopen(path, "rb");
        uint8_t h[64];
        ASSERT_TRUE(fread(h, 1, sizeof(h), f) == sizeof(h));
        fclose(f);
        // EBML header: 4-byte ID, 8-byte size; then the Segment's 4 + 8.
        uint64_t ebml = 0;
        for (int i = 5; i < 12; i++)
            ebml = ebml << 8 | h[i];
        seg_data = 12 + ebml + 12;
    }
    long mid = (long)(seg_data + (pos[3] + pos[4]) / 2);
    mkv_read_close(r);
    copy_prefix(path, cut, mid);
    r = mkv_read_open(cut, &err);
    ASSERT_TRUE(r != NULL);
    int got = 0, rc;
    while ((rc = mkv_read_packet(r, &p)) == 1)
        got++;
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_INT(in_first3, got);
    ASSERT_TRUE(mkv_read_truncated(r));
    mkv_read_close(r);
    // Cut inside the header: the file does not open, or opens and has
    // nothing; either way no crash.
    for (long n = 1; n < 400; n += 37) {
        copy_prefix(path, cut, n);
        r = mkv_read_open(cut, &err);
        if (r) {
            while (mkv_read_packet(r, &p) == 1)
                ;
            mkv_read_close(r);
        }
    }
    free_sent();
}

// A muxer never closed: what is on disk is the header and the complete
// clusters, and they read back as written.
TEST(mkv_never_closed) {
    char path[512], snap[512];
    out_path(path, sizeof(path), "open.mkv");
    out_path(snap, sizeof(snap), "open-snap.mkv");
    muxer_t *m = write_file(path, 300, true, false);
    // The file as a crash would leave it.
    copy_prefix(path, snap, file_size(path));
    int err = 0;
    mkv_reader_t *r = mkv_read_open(snap, &err);
    ASSERT_TRUE(r != NULL);
    ASSERT_TRUE(!mkv_read_sized(r));
    ASSERT_TRUE(mkv_read_duration(r) == 0);
    mkv_packet_t p;
    int got = 0, rc, k = 0;
    while ((rc = mkv_read_packet(r, &p)) == 1) {
        got++;
        if (p.track == 1)
            ASSERT_TRUE(p.timestamp == ms_of((uint64_t)k++ * FRAME_NS));
    }
    ASSERT_EQ_INT(0, rc);
    ASSERT_TRUE(got > 200 && got < s_nsent);
    ASSERT_EQ_INT(0, (int)mkv_read_cues(r, NULL));
    mkv_read_close(r);
    ASSERT_EQ_INT(0, mux_close(m));
    int per[2] = {0, 0};
    ASSERT_EQ_INT(s_nsent, read_back(path, true, per));
    free_sent();
}

// Bad parameters are refused, and a file that is not Matroska is not read.
TEST(mkv_refusals) {
    char path[512];
    out_path(path, sizeof(path), "bad.mkv");
    mux_video_t v = video_params();
    int err = 0;
    v.bits_per_pixel = 12;
    ASSERT_TRUE(mkv_mux_open(path, &v, NULL, &err) == NULL && err == -EINVAL);
    v = video_params();
    v.frame_period_ns = 0;
    ASSERT_TRUE(mkv_mux_open(path, &v, NULL, &err) == NULL && err == -EINVAL);
    FILE *f = fopen(path, "wb");
    fputs("RIFF....AVI LIST", f);
    fclose(f);
    ASSERT_TRUE(mkv_read_open(path, &err) == NULL && err == -EINVAL);
    v = video_params();
    muxer_t *m = mkv_mux_open(path, &v, NULL, &err);
    ASSERT_TRUE(m != NULL);
    uint8_t b = 0;
    ASSERT_EQ_INT(-EINVAL, mux_packet(m, MUX_TRACK_AUDIO, 0, 0, true, &b, 1));
    ASSERT_EQ_INT(0, mux_close(m));
}

// A hand-made file whose cluster timestamp is INT64_MAX and whose block
// adds one to it: the sum wraps (as the file's arithmetic does), it is not
// undefined behaviour (the sanitizers stop on that).
TEST(mkv_timestamp_wraps) {
    static const uint8_t file[] = {
        0x1A, 0x45, 0xDF, 0xA3, 0x8B, 0x42, 0x82, 0x88, 'm',  'a',  't',  'r',  'o',  's', 'k', 'a', // EBML
        0x18, 0x53, 0x80, 0x67, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Segment, size unknown
        0x16, 0x54, 0xAE, 0x6B, 0x88, 0xAE, 0x86, 0xD7, 0x81, 0x01, 0x83, 0x81, 0x01, // Tracks: 1, video
        0x1F, 0x43, 0xB6, 0x75, 0x90, // Cluster
        0xE7, 0x88, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Timestamp INT64_MAX
        0xA3, 0x84, 0x81, 0x00, 0x01, 0x80, // SimpleBlock: track 1, +1, key, empty
    };
    char path[512];
    out_path(path, sizeof(path), "ts-wrap.mkv");
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f && fwrite(file, 1, sizeof(file), f) == sizeof(file));
    fclose(f);
    int err = 0;
    mkv_reader_t *r = mkv_read_open(path, &err);
    ASSERT_TRUE(r != NULL);
    mkv_packet_t p;
    ASSERT_EQ_INT(1, mkv_read_packet(r, &p));
    ASSERT_TRUE(p.track == 1 && p.len == 0 && p.keyframe);
    ASSERT_TRUE(p.timestamp == INT64_MIN);
    ASSERT_EQ_INT(0, mkv_read_packet(r, &p));
    mkv_read_close(r);
}

int main(void) {
    RUN(mkv_round_trip);
    RUN(mkv_video_only);
    RUN(mkv_truncated_mid_cluster);
    RUN(mkv_never_closed);
    RUN(mkv_refusals);
    RUN(mkv_timestamp_wraps);
    fprintf(stderr, "record_mkv: all tests passed\n");
    return 0;
}
