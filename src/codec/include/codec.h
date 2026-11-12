// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// codec.h
// The one place compression, decompression and their checksums live.
//
// A small C99 library with no dependency beyond libc.  Every part of the
// tree that meets a compressed stream goes through it: peeler's archives
// and wrappers (zip, gzip, StuffIt, Compact Pro, BinHex), the disk-image
// decoders (UDIF, NDIF, QCOW2), System 7 compressed resources, the
// checkpoint files, the UDIF writer, and the PNG and PDF writers.
//
// Every decoder has the same shape (codec_dec_*): created for one stream,
// it is run with output buffers of any size and stops wherever one fills,
// continuing from exactly there on the next run -- a reader decodes as far
// as it reads and never more.  Input comes from memory or is pulled through
// a callback as the decoder needs it.  codec_decode() is the one-shot form
// over the same decoders.  Every encoder has one shape too (codec_enc_*):
// a whole input to one complete stream, with state reused across calls.
//
//   id                  decode  encode  stream
//   CODEC_COPY            x             stored bytes
//   CODEC_DEFLATE         x             raw DEFLATE (RFC 1951)
//   CODEC_ZLIB            x       x     zlib (RFC 1950)
//   CODEC_ADC             x             Apple Data Compression (NDIF, UDCO)
//   CODEC_BZIP2           x             bzip2 (UDBZ)
//   CODEC_LZFSE           x             LZFSE with LZVN and stored blocks (ULFO)
//   CODEC_XZ              x       x     xz, LZMA2 inside (ULMO)
//   CODEC_LZMA_ALONE      x             legacy .lzma
//   CODEC_RLE90           x             0x90-escape run lengths (StuffIt 1, BinHex)
//   CODEC_SIT_LZW         x             StuffIt method 2 (LZW, 14-bit)
//   CODEC_SIT_HUFFMAN     x             StuffIt method 3 (static Huffman)
//   CODEC_SIT13           x             StuffIt method 13 (LZSS + Huffman)
//   CODEC_SIT_ARSENIC     x             StuffIt method 15 (Arsenic: BWT + arithmetic)
//   CODEC_CPT_RLE         x             Compact Pro's run lengths
//   CODEC_CPT_LZH         x             Compact Pro's LZSS + Huffman
//   CODEC_DCMP            x             System 7 compressed resource (dcmp 0, 2, 3)
//   CODEC_GSRLE           x       x     the checkpoint file's run lengths
//
// The decoders take untrusted input: every length, distance, table and
// state is bounds-checked, and a malformed stream is an error, never
// undefined behaviour or a read outside the buffers.

#ifndef CODEC_H
#define CODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// === Result codes ===

#define CODEC_ERR_DATA        (-1) // malformed or corrupt stream
#define CODEC_ERR_TRUNCATED   (-2) // the input ends before the stream does
#define CODEC_ERR_OVERFLOW    (-3) // the stream decodes to more than allowed
#define CODEC_ERR_NOMEM       (-4) // out of memory
#define CODEC_ERR_UNSUPPORTED (-5) // a valid stream using a feature not implemented
#define CODEC_ERR_CHECKSUM    (-6) // the stream's own checksum does not match
#define CODEC_ERR_ARG         (-7) // invalid arguments (NULL buffer, unknown codec)

// A short static description of a CODEC_ERR_* code.
const char *codec_strerror(int64_t rc);

// === Codecs ===

typedef enum {
    CODEC_COPY = 0,
    CODEC_DEFLATE,
    CODEC_ZLIB,
    CODEC_ADC,
    CODEC_BZIP2,
    CODEC_LZFSE,
    CODEC_XZ,
    CODEC_LZMA_ALONE,
    CODEC_RLE90,
    CODEC_SIT_LZW,
    CODEC_SIT_HUFFMAN,
    CODEC_SIT13,
    CODEC_SIT_ARSENIC,
    CODEC_CPT_RLE,
    CODEC_CPT_LZH,
    CODEC_DCMP,
    CODEC_GSRLE,
    CODEC_COUNT
} codec_id_t;

// The codec's short name ("zlib", "bzip2", ...), or "?" for an unknown id.
const char *codec_name(codec_id_t id);

// === Decoding ===

#define CODEC_LEN_UNKNOWN UINT64_MAX

// Supplies compressed input: up to `cap` bytes into `buf`.  Returns the byte
// count, 0 at the end of the input, or a negative code.
typedef int64_t (*codec_pull_fn)(void *ctx, uint8_t *buf, size_t cap);

// CODEC_RLE90 flags.
#define CODEC_RLE90_BINHEX                                                                                             \
    0x1u // BinHex's rules: an escaped 0x90 is the byte repeated after it; a count of 1 is an error

// CODEC_DCMP: the signature every System 7 compressed resource starts with
// (0xA89F, the _Unimplemented trap, then 'er'), and the largest decoded
// size accepted (a resource fork's data offsets are 24 bits).
#define CODEC_DCMP_MAGIC    0xA89F6572u
#define CODEC_DCMP_MAX_SIZE (16u * 1024u * 1024u)

// True when `len` bytes at `p` begin with the compressed-resource signature
// (safe on any length, and on NULL).
static inline int codec_dcmp_is_compressed(const uint8_t *p, size_t len) {
    return p && len >= 4 &&
           ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]) == CODEC_DCMP_MAGIC;
}

typedef struct {
    // The compressed input: `len` bytes at `src`, or, when `pull` is set,
    // pulled through it as the decoder needs them.
    const uint8_t *src;
    size_t len;
    codec_pull_fn pull;
    void *pull_ctx;
    // The decoded size, when the container states it.  Streams with no end
    // of their own (ADC, RLE90, the StuffIt and Compact Pro methods, GSRLE)
    // end there and require it; for the rest it is a limit, and decoding
    // past it is CODEC_ERR_OVERFLOW.  CODEC_LEN_UNKNOWN: no limit.
    uint64_t out_len;
    uint32_t flags; // codec-specific (CODEC_RLE90_BINHEX)
} codec_dec_opts_t;

typedef struct codec_dec codec_dec_t;

// A decoder for one stream.  NULL with *err set (CODEC_ERR_*) on bad
// arguments, a codec that needs out_len without one, or no memory; a bad
// stream header is reported by the first run, not here.
codec_dec_t *codec_dec_new(codec_id_t id, const codec_dec_opts_t *opts, int *err);

// Produce up to `cap` bytes into `out` (*n set).  Returns 1 once the stream
// has ended and all of it has been produced, 0 when more remains, or a
// CODEC_ERR_* code (after the bytes before the fault have been produced).
int codec_dec_run(codec_dec_t *d, uint8_t *out, size_t cap, size_t *n);

// Bytes produced so far.
uint64_t codec_dec_total_out(const codec_dec_t *d);

// Compressed bytes the stream occupied: exact once it has ended (a gzip
// trailer or the next member follows there).
uint64_t codec_dec_consumed(const codec_dec_t *d);

// Why the decoder failed, in a few words ("bzip2: block CRC mismatch"), or
// "" while it has not.
const char *codec_dec_message(const codec_dec_t *d);

void codec_dec_free(codec_dec_t *d);

// A codec_pull_fn reading a decoder's output (ctx: the codec_dec_t), for
// chaining one stream into another (Compact Pro's LZH, then its RLE).
int64_t codec_dec_pull(void *ctx, uint8_t *buf, size_t cap);

// One-shot: decode `len` bytes of `src` into `dst`.  The stream must end
// within `cap` bytes (a stream with no end of its own is `cap` bytes long).
// Returns the count written or a CODEC_ERR_* code.
int64_t codec_decode(codec_id_t id, const uint8_t *src, size_t len, uint8_t *dst, size_t cap);

// codec_decode with the decoder's flags.
int64_t codec_decode_flags(codec_id_t id, uint32_t flags, const uint8_t *src, size_t len, uint8_t *dst, size_t cap);

// One-shot into a fresh buffer, for a stream of unknown decoded size (a PNG,
// a compressed resource): at most `max` bytes.  Returns the malloc'd output
// (*out_len set), or NULL with *err set.
uint8_t *codec_decode_alloc(codec_id_t id, const uint8_t *src, size_t len, size_t max, size_t *out_len, int *err);

// === Encoding ===

typedef struct codec_enc codec_enc_t;

// An encoder for inputs of at most `max_len` bytes, at `level` 1..9 (speed
// to size; < 0: the codec's default).  Its state is reused by every run, so
// a caller compressing many chunks allocates once.  NULL with *err set for
// a codec that does not encode, or no memory.
codec_enc_t *codec_enc_new(codec_id_t id, size_t max_len, int level, int *err);

// Bytes of output that always suffice for `len` bytes of input.
size_t codec_enc_bound(codec_id_t id, size_t len);

// Compress `in` (at most max_len bytes) into one complete stream in `out`.
// Returns the bytes written, CODEC_ERR_OVERFLOW when `cap` is too small, or
// another CODEC_ERR_* code.
int64_t codec_enc_run(codec_enc_t *e, const uint8_t *in, size_t len, uint8_t *out, size_t cap);

void codec_enc_free(codec_enc_t *e);

// One-shot: codec_enc_new, run, free.
int64_t codec_encode(codec_id_t id, int level, const uint8_t *in, size_t len, uint8_t *out, size_t cap);

// === Checksums ===

// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320) -- zlib, gzip, zip,
// PNG, UDIF, xz.  Start from 0; a result seeds the next call, so
// codec_crc32(codec_crc32(0, a, n), b, m) is the CRC of a followed by b.
uint32_t codec_crc32(uint32_t crc, const void *data, size_t len);

// The CRC-32 continued over `len` zero bytes, in O(log len): a 2 GB empty
// disk image is checksummed without feeding 2 GB of zeros through the table.
uint32_t codec_crc32_zeros(uint32_t crc, uint64_t len);

// CRC-32 in its MSB-first (non-reflected) form, polynomial 0x04C11DB7, as
// bzip2 uses it.  Start from 0; chains like codec_crc32.
uint32_t codec_crc32_msb(uint32_t crc, const void *data, size_t len);

// CRC-64 (ECMA-182, reflected polynomial 0xC96C5795D7870F42), as xz uses
// it.  Start from 0; chains like codec_crc32.
uint64_t codec_crc64(uint64_t crc, const void *data, size_t len);

// Adler-32 (RFC 1950 §8), continuing from `adler` (start from 1).
uint32_t codec_adler32(uint32_t adler, const void *data, size_t len);

// CRC-16/XMODEM (polynomial 0x1021, MSB first, initial 0, no final XOR), as
// BinHex and MacBinary use it.  Chains: a result seeds the next call.
uint16_t codec_crc16_xmodem(uint16_t crc, const void *data, size_t len);

// CRC-16/ARC (reflected polynomial 0xA001, initial 0, no final XOR), as
// StuffIt checks a fork with it.  Chains like codec_crc16_xmodem.
uint16_t codec_crc16_arc(uint16_t crc, const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif // CODEC_H
