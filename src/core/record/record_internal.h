// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// record_internal.h
// What the recorder's two halves share: the records the emulator thread
// writes into the ring (record.c), and the coder and worker that turn them
// into a file (record_thread.c).
//
// The ring is the mailbox record ring (mailbox_ring.h): records {kind, len}
// + payload, 8-aligned, never wrapping.  Every record's payload starts with
// a fixed header struct below; both ends are one process and one build, so
// the structs are copied as they are.
//
//   SEGMENT  open a file: its path, canvas, pixel class, frame divisor and
//            audio stream.  The file before it, if any, is finalised first.
//   FRAME    one output frame: the guest's mode, the row band that changed
//            (XOR against the previous frame, or whole rows after a resync
//            or a mode change), and the CLUT and response tables when they
//            changed (always after a resync).
//   REPEAT   `count` output frames identical to the one before.
//   AUDIO    one push: its rate, channels, volume and samples.
//   CLOSE    finalise the file; the recording carries on, and the next
//            SEGMENT opens the next file (recorder_suspend).
//   STOP     finalise the file and end.
//
// The coder (rec_coder_t) is the recorder thread's whole job and depends on
// nothing but libc and the codecs, so the unit tier drives it record by
// record.  The worker (rec_worker_t) is the ring, the thread around the
// coder, and the backlog policy; inline, the same coder runs on the
// emulator thread as each record is published.

#ifndef RECORD_INTERNAL_H
#define RECORD_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// === Constants ===

// Record kinds (0 is the ring's PAD).
#define REC_K_SEGMENT 1
#define REC_K_FRAME   2
#define REC_K_REPEAT  3
#define REC_K_AUDIO   4
#define REC_K_STOP    5
#define REC_K_CLOSE   6

// FRAME flags.
#define REC_FR_WHOLE  0x01u // the band is whole rows, not XOR: code a keyframe
#define REC_FR_MODE   0x02u // the mode (size, format) is new: start the picture afresh
#define REC_FR_CLUT   0x04u // a 768-byte CLUT (clut_len entries used) follows the header
#define REC_FR_TABLES 0x08u // the response tables' state follows (REC_FR_DAC / _CRT say which exist)
#define REC_FR_DAC    0x10u // with _TABLES: a 768-byte DAC table follows
#define REC_FR_CRT    0x20u // with _TABLES: a 768-byte monitor curve follows (after the DAC's)

#define REC_TABLE_BYTES 768 // 3 x 256

#define REC_KEY_INTERVAL     600 // changed frames between keyframes
#define REC_RESERVE_ROOM_MAX (64u << 10) // the most a reserve keeps free besides its record (a frame's room for audio)

#define REC_LEVEL_NORMAL 3 // DEFLATE effort with no backlog
#define REC_LEVEL_FAST   1 // past half the ring
#define REC_LEVEL_STORED 0 // past three quarters (CODEC_LEVEL_STORED)

// === Record payloads ===

// SEGMENT: then path_len bytes of path (no terminator).
typedef struct rec_seg_hdr {
    uint32_t canvas_w, canvas_h; // the stored frame size
    uint32_t display_w, display_h; // the shape to show it at (0: square pixels)
    uint32_t cls; // REC_CLASS_*
    uint32_t div; // frame-units per output frame (1, 2, 4)
    uint32_t audio_rate; // 0: no audio track
    uint32_t audio_channels;
    uint32_t index; // 1 for the first file, 2 for "-2", ...
    uint32_t path_len;
} rec_seg_hdr_t;

// FRAME: then the CLUT, the tables and the band, as the flags say.
typedef struct rec_frame_hdr {
    uint32_t flags;
    uint32_t width, height, format; // the guest's mode (REC_PIX_*)
    uint32_t row_bytes; // bytes of one raw row (the band's stride)
    uint32_t row_first, row_end; // the band [first, end)
    uint32_t clut_len; // with REC_FR_CLUT: entries in use
} rec_frame_hdr_t;

// REPEAT.
typedef struct rec_repeat_hdr {
    uint32_t count;
    uint32_t pad;
} rec_repeat_hdr_t;

// AUDIO: then frames * channels int16 samples.
typedef struct rec_audio_hdr {
    uint32_t rate;
    uint16_t channels;
    uint16_t vol;
    uint32_t frames;
    uint32_t pad;
} rec_audio_hdr_t;

// === The coder ===

typedef struct rec_coder rec_coder_t;

// What the coder reports back: written by the coder's thread, read by
// anyone (atomics).
typedef struct rec_stats {
    uint64_t bytes; // file bytes, every segment so far
    uint64_t video_bytes, audio_bytes; // payload bytes, every segment
    uint64_t frames; // output frames coded
    uint64_t keyframes;
    int err; // 0, or the negative errno of the first failure
} rec_stats_t;

// A coder with no file open.  NULL when out of memory.
rec_coder_t *rec_coder_new(void);

// Code one record (REC_K_*, its payload).  `level` is the DEFLATE effort
// for a frame (REC_LEVEL_*).  Returns 0, or a negative errno: the file is
// then finalised as far as it got, and every later record is ignored.
int rec_coder_record(rec_coder_t *c, uint32_t kind, const uint8_t *p, size_t len, int level);

// The coder's counters so far (any thread).
void rec_coder_stats(const rec_coder_t *c, rec_stats_t *out);

// Why the coder failed, in a few words, or "" (after rec_coder_stats says err).
const char *rec_coder_message(const rec_coder_t *c);

// Finalise whatever is open (as STOP would) and free the coder.
void rec_coder_free(rec_coder_t *c);

// === The worker ===

typedef struct rec_worker rec_worker_t;

// A worker with a ring of `ring_bytes` (a power of two) feeding `coder`
// (which it then owns), on a thread of its own or, `inline`, on the
// caller's.  `block`: a full ring waits for room instead of refusing, and
// the DEFLATE effort never drops (the output is then a function of the
// records alone).  NULL when the thread or memory cannot be had.
rec_worker_t *rec_worker_new(rec_coder_t *coder, uint32_t ring_bytes, bool threaded, bool block);

// Reserve a record of `kind` with a payload of `len` bytes: a pointer to
// the payload, or NULL when it does not fit now (never with block, or with
// `must`, which waits whatever the policy: the records that end things),
// or when it would not fit even the empty ring (rec_worker_fit first).
// `reserve_room` keeps that many bytes free besides (frames leave room for
// the small audio records).  Publish with rec_worker_commit.
uint8_t *rec_worker_reserve(rec_worker_t *w, uint32_t kind, size_t len, size_t reserve_room, bool must);

// Publish the reserved record; inline, code it now.
void rec_worker_commit(rec_worker_t *w);

// Make sure a record of `len` bytes can ever fit, with up to
// REC_RESERVE_ROOM_MAX kept free besides (the ring grows, once the
// recorder thread has caught up).  0, or -1 when out of memory.
int rec_worker_fit(rec_worker_t *w, size_t len);

// Change the full-ring policy.
void rec_worker_set_block(rec_worker_t *w, bool block);

// Wait until the coder has taken every record published so far (inline:
// at once).  After a CLOSE, the file is then closed.
void rec_worker_sync(rec_worker_t *w);

// The coder's counters (any time).
void rec_worker_stats(rec_worker_t *w, rec_stats_t *out);

// The coder's failure message (once its err is set).
const char *rec_worker_message(rec_worker_t *w);

// After a STOP record was committed: wait for the coder to finish it, and
// free the worker and the coder.  `final` (may be NULL) receives the
// coder's last counters and `msg` (msg_cap bytes, may be NULL) its failure
// message.
void rec_worker_free(rec_worker_t *w, rec_stats_t *final, char *msg, size_t msg_cap);

#endif // RECORD_INTERNAL_H
