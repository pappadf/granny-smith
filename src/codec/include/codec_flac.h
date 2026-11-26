// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// codec_flac.h
// FLAC (RFC 9639): a streaming encoder for 16-bit mono or stereo audio and
// a decoder for the subset it writes.
//
// The encoder takes interleaved int16 samples as they come and hands back
// one complete FLAC frame per 4096 samples per channel (a fixed-blocksize
// stream; the last frame, from codec_flac_enc_flush, may be shorter).  Each
// channel of a frame is a CONSTANT subframe when every sample is the same,
// else the cheapest of VERBATIM and the FIXED predictors of order 0 to 4
// with partitioned Rice-coded residuals, the partition order and Rice
// parameters chosen per frame.  Samples whose low bits are zero throughout
// a frame (8-bit audio carried in int16) are coded without those bits (the
// wasted-bits field).  Stereo goes out as left/right, left/side, side/right
// or mid/side, whichever is smallest.  Frame headers carry the frame
// number, block size and sample rate, and end in a CRC-8; frames end in a
// CRC-16.  There is no LPC analysis: on emulated-machine audio the fixed
// predictors are within a few percent of it.
//
// The STREAMINFO block (codec_flac_enc_streaminfo) is fixed before any
// audio exists, as a container's track header needs it: block size 4096
// (minimum and maximum), and zero -- "unknown", which RFC 9639 §8.2
// permits -- for the frame sizes, the total sample count and the MD5.
//
// The decoder reads frames of that shape: CONSTANT, VERBATIM and FIXED
// subframes, wasted bits, every stereo decorrelation, 4- and 5-bit Rice
// parameters and escaped partitions, at 16 bits per sample, mono or
// stereo; LPC subframes are CODEC_ERR_UNSUPPORTED.  Both CRCs are checked.

#ifndef CODEC_FLAC_H
#define CODEC_FLAC_H

#include "codec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// === Constants ===

#define CODEC_FLAC_BLOCK      4096 // samples per channel in every frame but the last
#define CODEC_FLAC_STREAMINFO 34 // bytes of a STREAMINFO block's body
#define CODEC_FLAC_MAX_RATE   655350 // the largest sample rate STREAMINFO's 20 bits hold

// === Encoder ===

typedef struct codec_flac_enc codec_flac_enc_t;

// An encoder for 16-bit audio at `sample_rate` Hz (1..CODEC_FLAC_MAX_RATE)
// with 1 or 2 channels.  NULL with *err set (CODEC_ERR_ARG, _NOMEM).
codec_flac_enc_t *codec_flac_enc_new(uint32_t sample_rate, int channels, int *err);

// The stream's STREAMINFO body (CODEC_FLAC_STREAMINFO bytes) into `out`.
void codec_flac_enc_streaminfo(const codec_flac_enc_t *e, uint8_t *out);

// Bytes of output that always suffice for one frame.
size_t codec_flac_enc_bound(const codec_flac_enc_t *e);

// Take up to `n` interleaved sample frames (one sample per channel each)
// from `samples`: as many as fit before the current block is full.
// Returns the number taken; once a block is full, codec_flac_enc_ready is
// true and nothing more is taken until codec_flac_enc_frame has coded it.
size_t codec_flac_enc_push(codec_flac_enc_t *e, const int16_t *samples, size_t n);

// True when a whole block is waiting to be coded.
bool codec_flac_enc_ready(const codec_flac_enc_t *e);

// Code the full block waiting as one FLAC frame into `out`: its length,
// 0 when no block is full, or CODEC_ERR_OVERFLOW (cap too small; the block
// stays).  *first_sample, when set, is the index (per channel, from 0) of
// the frame's first sample.
int64_t codec_flac_enc_frame(codec_flac_enc_t *e, uint8_t *out, size_t cap, uint64_t *first_sample);

// Code the samples still pending, fewer than a block, as the stream's last
// frame: its length, or 0 when none are pending.  The stream may not
// continue after it (a fixed-blocksize stream has one short frame, last).
int64_t codec_flac_enc_flush(codec_flac_enc_t *e, uint8_t *out, size_t cap, uint64_t *first_sample);

// Sample frames taken so far, per channel.
uint64_t codec_flac_enc_samples(const codec_flac_enc_t *e);

void codec_flac_enc_free(codec_flac_enc_t *e);

// === Decoder ===

typedef struct codec_flac_dec codec_flac_dec_t;

// What a STREAMINFO block says.
typedef struct {
    uint32_t min_block, max_block, min_frame, max_frame;
    uint32_t sample_rate;
    int channels, bits;
    uint64_t total_samples;
    uint8_t md5[16];
} codec_flac_streaminfo_t;

// Parse a STREAMINFO body (CODEC_FLAC_STREAMINFO bytes).  0 or CODEC_ERR_DATA.
int codec_flac_parse_streaminfo(const uint8_t *p, size_t len, codec_flac_streaminfo_t *out);

// A decoder.  NULL with *err set.
codec_flac_dec_t *codec_flac_dec_new(int *err);

// Decode the frame at the start of `frame` (`len` bytes: at least the
// frame) into `out`, interleaved, room for `cap` sample frames.  Returns
// the sample frames decoded (per channel), or a CODEC_ERR_* code:
// _CHECKSUM for a CRC that does not match, _TRUNCATED for a frame cut
// short, _UNSUPPORTED for a feature outside the subset, _OVERFLOW when the
// frame holds more than cap.  *used, when set, is the frame's length.
int64_t codec_flac_dec_frame(codec_flac_dec_t *d, const uint8_t *frame, size_t len, int16_t *out, size_t cap,
                             size_t *used);

// The last frame's channel count, sample rate (0 when the header defers
// to STREAMINFO) and frame number.
int codec_flac_dec_channels(const codec_flac_dec_t *d);
uint32_t codec_flac_dec_sample_rate(const codec_flac_dec_t *d);
uint64_t codec_flac_dec_frame_number(const codec_flac_dec_t *d);

// Why the last frame failed, in a few words, or "".
const char *codec_flac_dec_message(const codec_flac_dec_t *d);

void codec_flac_dec_free(codec_flac_dec_t *d);

#ifdef __cplusplus
}
#endif

#endif // CODEC_FLAC_H
