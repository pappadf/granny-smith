// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// codec.h
// The one place compression, decompression and their checksums live.
//
// A small C99 library with no dependencies beyond libc.  Every part of the
// tree that meets a compressed stream goes through it: peeler's zip and
// gzip members, the disk-image decoders (UDIF, NDIF, QCOW2), the UDIF
// writer, the PNG and PDF writers, the CRCs the image layer keeps.
//
//   checksums  CRC-32 (IEEE, reflected), its MSB-first form (bzip2),
//              CRC-64 (ECMA-182, as xz uses it), Adler-32
//   DEFLATE    decode (raw RFC 1951, zlib RFC 1950; resumable or one-shot)
//              and encode (zlib)
//   ADC        Apple Data Compression, decode
//   bzip2      decode (one or more concatenated streams)
//   LZFSE      decode, including its LZVN and raw blocks
//   LZMA       decode: the xz container (LZMA2 inside), and the legacy
//              .lzma ("LZMA alone") format; encode: xz
//
// Every one-shot decoder has the same shape: it reads `len` bytes of `src`,
// writes at most `cap` bytes to `dst`, and returns the count written or a
// negative CODEC_ERR_* code.  Expanding past `cap` is an error
// (CODEC_ERR_OVERFLOW), never a silent clamp -- with one documented
// exception, ADC, whose stream has no end marker.  codec_decode() reaches
// them all by id, for callers that map a container's method field to a
// codec (UDIF chunk types, NDIF chunk types).
//
// The decoders take untrusted input: every length, distance and table is
// bounds-checked against the buffers, and a malformed stream is an error,
// not undefined behaviour.

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
#define CODEC_ERR_OVERFLOW    (-3) // the stream decodes to more than `cap`
#define CODEC_ERR_NOMEM       (-4) // out of memory
#define CODEC_ERR_UNSUPPORTED (-5) // a valid stream using a feature not implemented
#define CODEC_ERR_CHECKSUM    (-6) // the stream's own checksum does not match
#define CODEC_ERR_ARG         (-7) // invalid arguments (NULL buffer, unknown codec)

// A short static description of a CODEC_ERR_* code.
const char *codec_strerror(int64_t rc);

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

// === DEFLATE: decode ===

// Supplies compressed input: up to `cap` bytes into `buf`.  Returns the byte
// count, 0 at the end of the input, or a negative code.
typedef int64_t (*codec_pull_fn)(void *ctx, uint8_t *buf, size_t cap);

// A resumable raw-DEFLATE decoder: it stops wherever the caller's buffer
// fills and continues from exactly there on the next call (mid-block, even
// mid-match), pulling input as it needs it.  zip and gzip members decode a
// little at a time through it.
typedef struct codec_inflater codec_inflater_t;

// NULL when out of memory.
codec_inflater_t *codec_inflater_new(codec_pull_fn pull, void *ctx);
void codec_inflater_free(codec_inflater_t *z);

// Produce up to `cap` bytes into `out` (*out_n set).  Returns 1 once the
// final block has ended, 0 when more output remains, -1 on a truncated or
// malformed stream.
int codec_inflater_run(codec_inflater_t *z, uint8_t *out, size_t cap, size_t *out_n);

// Bytes produced so far, and compressed bytes consumed so far.
uint64_t codec_inflater_total_out(const codec_inflater_t *z);
uint64_t codec_inflater_consumed(const codec_inflater_t *z);

// After the final block: drop the bits up to the next byte boundary (a
// gzip trailer follows on one).
void codec_inflater_align(codec_inflater_t *z);

// A raw DEFLATE stream (RFC 1951) into `dst`.  The stream must end within
// `cap` bytes of output.
int64_t codec_inflate(const uint8_t *src, size_t len, uint8_t *dst, size_t cap);

// A zlib stream (RFC 1950: 2-byte header, DEFLATE, Adler-32) into `dst`.
// The header is checked (CM 8, FCHECK, no preset dictionary); the Adler-32
// trailer is checked when present -- a stream that stops right after its
// final block is accepted, as some writers omit the trailer.
int64_t codec_zlib_inflate(const uint8_t *src, size_t len, uint8_t *dst, size_t cap);

// A zlib stream into a freshly allocated buffer, for callers that do not
// know the decoded size up front (the PNG reader).  The output may not
// exceed `out_max` bytes.  Returns malloc'd output and sets *out_len, or
// NULL when the stream is malformed or larger than `out_max`.
uint8_t *codec_zlib_inflate_alloc(const uint8_t *src, size_t len, size_t out_max, size_t *out_len);

// === DEFLATE: encode ===

// Bytes of output buffer that always suffice for `len` bytes of input, at
// any level (fixed Huffman codes a byte in at most 9 bits; stored blocks add
// 5 bytes per 65535).
size_t codec_deflate_bound(size_t len);

// Reusable encoder state for inputs of at most `max_len` bytes, so a caller
// compressing many chunks allocates the hash chains once.
typedef struct codec_deflate_state codec_deflate_state_t;
codec_deflate_state_t *codec_deflate_state_new(size_t max_len);
void codec_deflate_state_free(codec_deflate_state_t *st);

// Compress `in` into a complete zlib stream in `out` (`cap` bytes, at least
// codec_deflate_bound(len) to be sure).  `level` 0 emits stored blocks;
// 1..9 pick the hash-chain depth (6 is what the PNG writer has always used).
// `st` may be NULL (allocated for the call) and must otherwise have been
// made for at least `len` bytes.  Returns the bytes written, or -1 when
// `cap` is too small or memory runs out.
//
// The LZ77 pass is greedy over a 32 KB window; each block is written as
// dynamic Huffman, fixed Huffman or stored, whichever is smallest.  The
// dynamic codes are length-limited and complete, so strict decoders
// (zlib's, and every tool that reads the images and screenshots) accept them.
long codec_zlib_deflate(codec_deflate_state_t *st, const uint8_t *in, size_t len, uint8_t *out, size_t cap, int level);

// === ADC (Apple Data Compression) ===

// Decode an ADC stream: the byte-aligned LZ77 variant Disk Copy 6 uses for
// NDIF chunks (0x83) and UDIF for UDCO chunks (0x80000004).  The stream has
// no end marker, so decoding stops when the input is exhausted or `cap`
// bytes are written, whichever comes first; callers compare the result with
// the size they expect.  CODEC_ERR_DATA on a back-reference before the start
// of the output.
int64_t codec_adc_decompress(const uint8_t *src, size_t len, uint8_t *dst, size_t cap);

// === bzip2 ===

// Decode a bzip2 stream ("BZh1".."BZh9"), or several concatenated, as the
// bzip2 tool writes them.  Every block CRC and each stream's combined CRC is
// checked.  Blocks in the "randomised" form (written by bzip2 before 0.9.5
// and by nothing since) are CODEC_ERR_UNSUPPORTED.
int64_t codec_bzip2_decompress(const uint8_t *src, size_t len, uint8_t *dst, size_t cap);

// === LZFSE ===

// Decode an LZFSE stream: a sequence of blocks ending in "bvx$", each an
// LZFSE block (v1 "bvx1" or the packed-header v2 "bvx2"), an LZVN block
// ("bvxn") or stored bytes ("bvx-").  This is what Apple's libcompression
// writes for COMPRESSION_LZFSE (UDIF ULFO chunks, 0x80000007).
int64_t codec_lzfse_decompress(const uint8_t *src, size_t len, uint8_t *dst, size_t cap);

// === LZMA ===

// Decode an xz stream, or several concatenated with optional stream padding:
// the .xz container with LZMA2 as its only filter, as Apple's
// libcompression writes it for COMPRESSION_LZMA (UDIF ULMO chunks,
// 0x80000008).  CRC-32 and CRC-64 checks are verified; SHA-256 and the
// reserved check types are skipped.  Other filters (BCJ, delta) are
// CODEC_ERR_UNSUPPORTED.
int64_t codec_xz_decompress(const uint8_t *src, size_t len, uint8_t *dst, size_t cap);

// === LZMA: encode ===

// Bytes of output buffer that always suffice for `len` bytes of input.
size_t codec_xz_bound(size_t len);

// Reusable encoder state (hash chains, the model) for inputs of at most
// `max_len` bytes, so a caller compressing many chunks allocates once.
typedef struct codec_xz_state codec_xz_state_t;
codec_xz_state_t *codec_xz_state_new(size_t max_len);
void codec_xz_state_free(codec_xz_state_t *st);

// Compress `in` into one complete xz stream in `out` (`cap` bytes, at least
// codec_xz_bound(len) to be sure): one block, LZMA2 (lc 3, lp 0, pb 2) with
// a dictionary just covering the input, a CRC-64 check -- what
// codec_xz_decompress, xz-utils and Apple's libcompression read.  `level`
// 1..9 trades speed for size (hash-chain depth, lazy matching).  `st` may
// be NULL (allocated for the call) and must otherwise have been made for at
// least `len` bytes.  Returns the bytes written, CODEC_ERR_OVERFLOW when
// `cap` is too small, or CODEC_ERR_NOMEM.
//
// The parse is heuristic (hash chains, the repeat distances, one byte of
// lazy matching), not xz-utils' optimal one: its output is a little larger
// than `xz` writes at the same effort.
int64_t codec_xz_compress(codec_xz_state_t *st, const uint8_t *in, size_t len, uint8_t *out, size_t cap, int level);

// Decode a legacy .lzma stream ("LZMA alone": a 13-byte header of
// properties, dictionary size and decoded size, then raw LZMA).  An unknown
// decoded size (all ones) requires the end marker.
int64_t codec_lzma_alone_decompress(const uint8_t *src, size_t len, uint8_t *dst, size_t cap);

// === Dispatch by id ===

typedef enum {
    CODEC_COPY = 0, // stored: the bytes as they are
    CODEC_DEFLATE, // raw DEFLATE (RFC 1951)
    CODEC_ZLIB, // zlib (RFC 1950)
    CODEC_ADC, // Apple Data Compression
    CODEC_BZIP2, // bzip2
    CODEC_LZFSE, // LZFSE (with LZVN and raw blocks)
    CODEC_XZ, // xz (LZMA2)
    CODEC_LZMA_ALONE, // legacy .lzma
    CODEC_COUNT
} codec_id_t;

// Decode `len` bytes of `src` in codec `id` into `dst` (at most `cap`
// bytes).  Returns the count written or a negative CODEC_ERR_* code.
int64_t codec_decode(codec_id_t id, const uint8_t *src, size_t len, uint8_t *dst, size_t cap);

// The codec's short name ("zlib", "bzip2", ...), or "?" for an unknown id.
const char *codec_name(codec_id_t id);

#ifdef __cplusplus
}
#endif

#endif // CODEC_H
