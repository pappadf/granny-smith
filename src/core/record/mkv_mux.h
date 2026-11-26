// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mkv_mux.h
// The recorder's container writer: a small muxer interface, and the
// Matroska (RFC 9559) muxer behind it.
//
// A muxer takes the encoded packets of one video track (ZMBV) and an
// optional audio track (FLAC), each with its presentation time in
// nanoseconds, and writes them to a file in time order.  The interface is
// a table of operations, so a second container is an addition beside
// mkv_mux.c, not a change to its callers.
//
// The Matroska file is written front to back and is a valid stream at
// every cluster boundary: the segment's size is "unknown" until close
// patches it, so a file whose writer never closed it (a crash, a killed
// tab) plays to its last complete cluster.  Close adds the Cues, a
// SeekHead (into a Void reserved for it) and the Duration.

#ifndef MKV_MUX_H
#define MKV_MUX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// === Constants ===

#define MUX_TRACK_VIDEO 0 // the video track's index
#define MUX_TRACK_AUDIO 1 // the audio track's index (when there is one)

#define MUX_STREAMINFO_LEN 34 // a FLAC STREAMINFO body

// === Type definitions ===

// The video track: ZMBV frames of a fixed size at a constant rate.
typedef struct {
    int width, height; // the stored frame, in pixels
    int display_width, display_height; // the shape to show it at (0: the stored size)
    uint64_t frame_period_ns; // one frame's duration: the track's constant rate
    int bits_per_pixel; // 8, 16 or 32 (the codec header's bit count)
} mux_video_t;

// The audio track: FLAC, 16-bit, its STREAMINFO known up front.
typedef struct {
    uint32_t sample_rate;
    int channels;
    int bits_per_sample; // 16
    uint8_t streaminfo[MUX_STREAMINFO_LEN];
} mux_audio_t;

typedef struct muxer muxer_t;

// A container's operations.  Every one returns 0 or a negative errno.
typedef struct {
    // Queue one packet of `track`, presented at pts_ns for dur_ns (0: the
    // track's default, a video frame period), a keyframe or not.  Packets
    // of one track come in time order; the muxer interleaves the tracks.
    int (*packet)(muxer_t *m, int track, uint64_t pts_ns, uint64_t dur_ns, bool keyframe, const uint8_t *data,
                  size_t len);
    // Write everything still queued and finalise the file (index, sizes,
    // duration), then close it and free the muxer, whatever the result.
    int (*close)(muxer_t *m);
    // Bytes in the file so far, written or waiting in the muxer's buffer.
    uint64_t (*bytes)(const muxer_t *m);
} muxer_ops_t;

// Every muxer starts with its operations.
struct muxer {
    const muxer_ops_t *ops;
};

// === Operations ===

// Queue a packet (muxer_ops_t.packet).
static inline int mux_packet(muxer_t *m, int track, uint64_t pts_ns, uint64_t dur_ns, bool keyframe,
                             const uint8_t *data, size_t len) {
    return m->ops->packet(m, track, pts_ns, dur_ns, keyframe, data, len);
}

// Finalise, close and free (muxer_ops_t.close).
static inline int mux_close(muxer_t *m) {
    return m->ops->close(m);
}

// Bytes so far (muxer_ops_t.bytes).
static inline uint64_t mux_bytes(const muxer_t *m) {
    return m->ops->bytes(m);
}

// Create `path` and write a Matroska header for one ZMBV video track and,
// when `audio` is set, one FLAC audio track.  NULL with *err set to a
// negative errno (-EINVAL for bad parameters, or the open's).
muxer_t *mkv_mux_open(const char *path, const mux_video_t *video, const mux_audio_t *audio, int *err);

#endif // MKV_MUX_H
