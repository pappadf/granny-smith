// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mkv_read.h
// A reader for the Matroska subset mkv_mux.c writes: the tracks' codecs
// and parameters, then every block as a packet (track, timestamp,
// keyframe, bytes), in file order.
//
// It reads front to back, a cluster at a time, and needs neither the
// SeekHead nor the Cues, so a file whose writer never closed it (segment
// size unknown, no index) reads to its last complete cluster: a cluster
// that runs past the end of the file is not returned, and the reader
// says the file was cut.  It is for the recorder's tests and for reading
// a recording back (the MP4 export); it is not a general Matroska demuxer
// (no lacing, no BlockGroups' extras, no unknown-size clusters).

#ifndef MKV_READ_H
#define MKV_READ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// === Constants ===

#define MKV_TRACK_VIDEO 1 // TrackType values
#define MKV_TRACK_AUDIO 2

#define MKV_READ_MAX_TRACKS 8

// === Type definitions ===

// One track as its TrackEntry describes it.
typedef struct {
    uint64_t number; // the blocks' track number
    int type; // MKV_TRACK_VIDEO, MKV_TRACK_AUDIO, or another TrackType
    char codec_id[32];
    uint8_t *codec_private; // NULL when absent
    size_t codec_private_len;
    uint64_t default_duration_ns; // 0 when absent
    uint64_t pixel_width, pixel_height, display_width, display_height;
    double sampling_frequency;
    uint64_t channels, bit_depth;
} mkv_track_t;

// One block.  `data` stays valid until the next mkv_read_packet.
typedef struct {
    uint64_t track; // a track number
    int64_t timestamp; // in TimestampScale ticks: the cluster's plus the block's
    bool keyframe;
    const uint8_t *data;
    size_t len;
} mkv_packet_t;

// A CuePoint (after the packets are read, when the file has Cues).
typedef struct {
    uint64_t time; // ticks
    uint64_t track;
    uint64_t cluster_pos; // from the start of the segment's data
} mkv_read_cue_t;

typedef struct mkv_reader mkv_reader_t;

// === Operations ===

// Open `path` and read everything before the first cluster.  NULL with
// *err set to a negative errno: -EINVAL for a file that is not Matroska,
// -ENOTSUP for a feature outside the subset, or the open's.
mkv_reader_t *mkv_read_open(const char *path, int *err);

// The tracks: their count, and the i-th.
int mkv_read_track_count(const mkv_reader_t *r);
const mkv_track_t *mkv_read_track(const mkv_reader_t *r, int i);

// The Info: nanoseconds per tick, and the Duration in ticks (0 when the
// file has none, as an unclosed one does not).
uint64_t mkv_read_timestamp_scale(const mkv_reader_t *r);
double mkv_read_duration(const mkv_reader_t *r);

// The next block: 1 with *p filled, 0 at the end (the end of the segment,
// or of the last complete cluster), or a negative errno.
int mkv_read_packet(mkv_reader_t *r, mkv_packet_t *p);

// After the end: whether the file stopped inside a cluster (or an element
// after the clusters), whether the Segment's size was stated, the Cues,
// and the offsets (from the segment's data) of the clusters read.
bool mkv_read_truncated(const mkv_reader_t *r);
bool mkv_read_sized(const mkv_reader_t *r);
size_t mkv_read_cues(const mkv_reader_t *r, const mkv_read_cue_t **cues);
size_t mkv_read_clusters(const mkv_reader_t *r, const uint64_t **positions);

void mkv_read_close(mkv_reader_t *r);

#endif // MKV_READ_H
