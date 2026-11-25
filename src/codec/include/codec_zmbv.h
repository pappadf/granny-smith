// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// codec_zmbv.h
// ZMBV (Zip Motion Blocks Video), the DOSBox capture codec: an encoder and
// a decoder for the bitstream DOSBox writes (src/libs/zmbv/zmbv.cpp) and
// FFmpeg decodes (libavcodec/zmbv.c).
//
// A frame is a flag byte (bit 0 keyframe, bit 1 palette delta), then:
//
//   keyframe  a six-byte header -- version 0.1, compression 1 (zlib), the
//             pixel format, the block width and height -- and the start of
//             a new zlib stream holding the palette (768 bytes RGB, 8 bpp
//             only) and the whole frame, top row first;
//   inter     the continuation of that zlib stream: the XOR of the palette
//             against the previous one (8 bpp, when bit 1 is set), two
//             bytes per block (dx << 1 | "has delta", dy << 1) padded to a
//             multiple of four, then for each block with a delta the XOR of
//             its pixels against the previous frame at the vector's offset.
//
// Every frame's zlib data ends in a sync flush, so a frame decodes as soon
// as it and the frames since its keyframe have.  Blocks are block_w x
// block_h pixels, clipped at the right and bottom edges.  Pixels are bytes
// (8 bpp, palette indices), little-endian 16-bit words (15 bpp x-5-5-5,
// 16 bpp 5-6-5) or little-endian 32-bit words (0x00RRGGBB: bytes B, G, R, 0).
//
// This encoder never searches for motion: every vector is zero.  It keeps
// the previous frame itself and takes either the new frame or the XOR of
// the changed rows, with the band of rows that may differ, so untouched
// blocks cost nothing to skip.  A frame whose pixels and palette did not
// change is an inter frame with an all-zero block table: a dozen bytes.
// Which frames are keyframes is the caller's choice (the first always is).

#ifndef CODEC_ZMBV_H
#define CODEC_ZMBV_H

#include "codec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// === Constants ===

// Pixel formats, as the keyframe header numbers them (DOSBox's
// zmbv_format_t; 1, 2, 3 and 7 -- 1, 2, 4 and 24 bpp -- are not written
// or read here).
#define CODEC_ZMBV_FMT_8BPP  4
#define CODEC_ZMBV_FMT_15BPP 5
#define CODEC_ZMBV_FMT_16BPP 6
#define CODEC_ZMBV_FMT_32BPP 8

#define CODEC_ZMBV_PALETTE    768 // 256 RGB triplets
#define CODEC_ZMBV_BLOCK      64 // the default block width and height
#define CODEC_ZMBV_HEADER_KEY 7 // flag byte and the keyframe header

// Frame flag bits (the first byte of every frame).
#define CODEC_ZMBV_FLAG_KEY      0x01
#define CODEC_ZMBV_FLAG_DELTAPAL 0x02

// What an encoded frame turned out to be.
typedef enum {
    CODEC_ZMBV_KEYFRAME = 0, // a fresh zlib stream with the whole frame
    CODEC_ZMBV_INTER, // XOR deltas for the blocks (or the palette) that changed
    CODEC_ZMBV_UNCHANGED, // an inter frame with nothing in it
} codec_zmbv_kind_t;

// === Encoder ===

typedef struct codec_zmbv_enc codec_zmbv_enc_t;

// One frame for the encoder.  Rows [row_first, row_end) are the only ones
// that may differ from the previous frame; the rest are taken as
// unchanged without being looked at (0 and the height: look at all of
// them).  `rows` points at row row_first, `stride` bytes apart (0: the
// width times the bytes per pixel):
//   is_xor false  the new frame's rows;
//   is_xor true   those rows XORed with the previous frame's (a capture
//                 that keeps a shadow copy has them already).
// An empty band (row_first == row_end) is a frame whose pixels did not
// change; `rows` may then be NULL.
typedef struct {
    const uint8_t *rows;
    size_t stride;
    bool is_xor;
    int row_first, row_end;
    const uint8_t *palette; // 8 bpp: this frame's CODEC_ZMBV_PALETTE bytes, NULL if unchanged
    bool keyframe; // code a keyframe (whatever changed)
} codec_zmbv_frame_t;

// An encoder for `width` x `height` frames in `format` (CODEC_ZMBV_FMT_*)
// with blocks of block_w x block_h pixels (1..255; 0: CODEC_ZMBV_BLOCK),
// DEFLATE at `level` (CODEC_LEVEL_STORED or 1..9).  The previous frame and
// palette start all zero.  NULL with *err set (CODEC_ERR_ARG, _NOMEM).
codec_zmbv_enc_t *codec_zmbv_enc_new(int width, int height, int format, int block_w, int block_h, int level, int *err);

// Change the DEFLATE level from the next frame on.
void codec_zmbv_enc_set_level(codec_zmbv_enc_t *e, int level);

// Make the next frame a keyframe, whatever its codec_zmbv_frame_t says.
void codec_zmbv_enc_force_keyframe(codec_zmbv_enc_t *e);

// Bytes of output that always suffice for one frame.
size_t codec_zmbv_enc_bound(const codec_zmbv_enc_t *e);

// Encode one frame into `out`: the frame's bytes, or CODEC_ERR_OVERFLOW
// (cap too small: nothing changed, the frame can be retried) or
// CODEC_ERR_ARG.  *kind, when set, says what the frame became.
int64_t codec_zmbv_enc_frame(codec_zmbv_enc_t *e, const codec_zmbv_frame_t *f, uint8_t *out, size_t cap,
                             codec_zmbv_kind_t *kind);

// The encoder's copy of the last frame it coded (what a decoder now
// holds): height rows of width x bytes-per-pixel.
const uint8_t *codec_zmbv_enc_pixels(const codec_zmbv_enc_t *e);

void codec_zmbv_enc_free(codec_zmbv_enc_t *e);

// === Decoder ===

typedef struct codec_zmbv_dec codec_zmbv_dec_t;

// A decoder for a `width` x `height` stream (the container states the
// size; ZMBV does not).  NULL with *err set.
codec_zmbv_dec_t *codec_zmbv_dec_new(int width, int height, int *err);

// Decode one frame.  An empty packet repeats the previous frame.  Returns
// 0, or a CODEC_ERR_* code: _DATA for a malformed frame, _UNSUPPORTED for
// a format or version not handled, _DATA for an inter frame before any
// keyframe.  A failed frame leaves the picture undefined until the next
// keyframe.
int codec_zmbv_dec_frame(codec_zmbv_dec_t *d, const uint8_t *pkt, size_t len);

// The pixel format of the last keyframe (CODEC_ZMBV_FMT_*), 0 before one.
int codec_zmbv_dec_format(const codec_zmbv_dec_t *d);

// The bytes per pixel of that format (1, 2 or 4), 0 before a keyframe.
int codec_zmbv_dec_bytes_per_pixel(const codec_zmbv_dec_t *d);

// The current frame: height rows of width x bytes-per-pixel bytes.
const uint8_t *codec_zmbv_dec_pixels(const codec_zmbv_dec_t *d);

// The current palette (CODEC_ZMBV_PALETTE bytes; 8 bpp only).
const uint8_t *codec_zmbv_dec_palette(const codec_zmbv_dec_t *d);

// The current frame as RGB, three bytes a pixel, into `out` (width x
// height x 3 bytes): the palette applied, 5-bit and 6-bit channels widened
// by bit replication.  0, or CODEC_ERR_DATA before a keyframe.
int codec_zmbv_dec_rgb24(const codec_zmbv_dec_t *d, uint8_t *out);

// Why the last frame failed, in a few words, or "".
const char *codec_zmbv_dec_message(const codec_zmbv_dec_t *d);

void codec_zmbv_dec_free(codec_zmbv_dec_t *d);

#ifdef __cplusplus
}
#endif

#endif // CODEC_ZMBV_H
