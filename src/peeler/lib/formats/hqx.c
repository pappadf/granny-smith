// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// hqx.c — BinHex 4.0 (.hqx) format peeler.
//
// Format spec: hqx.md
//
// BinHex 4.0 wraps a single Macintosh file (both forks + metadata) in
// three processing layers (hqx.md § 2.1 "The Big Picture"):
//
//   1. Text envelope — preamble string, colon delimiters, line breaks.
//      (hqx.md § 3)
//
//   2. 6-bit ASCII encoding — 64-character alphabet converting 8-bit
//      bytes to printable ASCII.  (hqx.md § 4)
//
//   3. Run-length encoding (RLE90) — 0x90 marker byte compresses
//      repeated-byte sequences.  (hqx.md § 5)
//
// After decoding all three layers, a binary stream remains containing a
// variable-length header, data fork, and resource fork, each followed
// by a CRC-16-CCITT checksum.  (hqx.md § 6)

#include "internal.h"

// ============================================================================
// Constants and Macros
// ============================================================================

// hqx.md § 3.1 — mandatory identification string that precedes the payload.
#define HQX_PREAMBLE "(This file must be converted with BinHex"

// hqx.md § 5.1 — the marker byte for run-length encoding.
#define RLE_MARKER 0x90

// hqx.md § 6.3 — maximum filename length in the header.
#define HQX_NAME_MAX 63

// hqx.md § 8.2 / Appendix B — Finder flag bits to clear on decode.
// isInvisible (bit 14), hasBeenInited (bit 7), OnDesk (bit 2).
#define FINDER_CLEAR_MASK 0x4084u

// ============================================================================
// Type Definitions (Private)
// ============================================================================

// hqx.md § 4.1 — the 64-character BinHex alphabet, index 0–63.
static const char hqx_alphabet[] =
    "!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr";

// State for the three-layer pull-based decoder pipeline.
// hqx.md § 10.1 describes the layered architecture.
typedef struct {
    // Source input buffer (borrowed, not owned)
    const uint8_t *src;
    size_t src_len;
    size_t src_pos;

    // hqx.md § 4.1 / Appendix A.2 — reverse alphabet lookup table.
    uint8_t rev[256];

    // hqx.md § 10.2 — six-to-eight converter accumulator.
    unsigned accum;
    unsigned accum_bits;

    // hqx.md § 5.2 — RLE expander state.
    bool rle_marker_seen;
    uint8_t rle_prev;
    int rle_pending;

    // Abort context for error reporting
    decode_ctx_t *ctx;
} hqx_decoder_t;

// Parsed fields from the BinHex header.
// hqx.md § 6.3 — variable-length header structure.
typedef struct {
    char name[HQX_NAME_MAX + 1];
    uint8_t name_len;
    uint32_t mac_type;
    uint32_t mac_creator;
    uint16_t finder_flags;
    uint32_t data_len;
    uint32_t rsrc_len;
} hqx_header_t;

// ============================================================================
// Static Helpers — Text Envelope
// ============================================================================

// hqx.md § 3.1 — scan the input for the preamble identification string.
// Returns the offset just past the preamble line, or (size_t)-1 if not found.
static size_t hqx_find_preamble(const uint8_t *src, size_t len) {
    size_t preamble_len = strlen(HQX_PREAMBLE);
    if (len < preamble_len) {
        return (size_t)-1;
    }

    // Search for the preamble substring in the input
    for (size_t i = 0; i + preamble_len <= len; i++) {
        if (memcmp(src + i, HQX_PREAMBLE, preamble_len) == 0) {
            // Skip past the rest of this line
            size_t j = i + preamble_len;
            while (j < len && src[j] != '\n' && src[j] != '\r') {
                j++;
            }
            // Skip the line ending
            while (j < len && (src[j] == '\n' || src[j] == '\r')) {
                j++;
            }
            return j;
        }
    }
    return (size_t)-1;
}

// hqx.md § 3.2 — find the starting colon that begins the encoded payload.
// Returns the offset of the byte immediately after the colon, or (size_t)-1.
static size_t hqx_find_start_colon(const uint8_t *src, size_t len,
                                   size_t from) {
    for (size_t i = from; i < len; i++) {
        if (src[i] == ':') {
            return i + 1;
        }
    }
    return (size_t)-1;
}

// ============================================================================
// Static Helpers — Decoder Pipeline
// ============================================================================

// Initialise a decoder from the source buffer positioned at the start of
// the encoded payload (just past the opening colon).
static void hqx_decoder_init(hqx_decoder_t *dec, const uint8_t *src,
                              size_t len, size_t payload_start,
                              decode_ctx_t *ctx) {
    memset(dec, 0, sizeof(*dec));
    dec->src = src;
    dec->src_len = len;
    dec->src_pos = payload_start;
    dec->ctx = ctx;

    // hqx.md § Appendix A.2 — build reverse lookup table.
    memset(dec->rev, 0xFF, sizeof(dec->rev));
    for (unsigned i = 0; i < 64; i++) {
        dec->rev[(unsigned char)hqx_alphabet[i]] = (uint8_t)i;
    }
}

// hqx.md § 3.4 — fetch the next encoded character, skipping whitespace.
// Returns the character, or -1 at the terminating colon or EOF.
static int hqx_next_char(hqx_decoder_t *dec) {
    while (dec->src_pos < dec->src_len) {
        uint8_t ch = dec->src[dec->src_pos++];
        // hqx.md § 3.2 — terminating colon marks end of payload
        if (ch == ':') {
            return -1;
        }
        // hqx.md § 3.4 — skip whitespace: CR, LF, TAB, SP
        if (ch == '\r' || ch == '\n' || ch == '\t' || ch == ' ') {
            continue;
        }
        return (int)ch;
    }
    return -1;
}

// hqx.md § 10.2 / § 4.2 — decode one raw byte from the 6-bit stream.
// Accumulates 6-bit values until 8 bits are available, then extracts one byte.
// Returns 0..255 on success, or -1 on EOF.
static int hqx_raw_byte(hqx_decoder_t *dec) {
    // Feed 6-bit symbols until we have at least 8 bits
    while (dec->accum_bits < 8) {
        int ch = hqx_next_char(dec);
        if (ch < 0) {
            return -1;
        }
        uint8_t val = dec->rev[(unsigned char)ch];
        if (val > 63) {
            // hqx.md § 9 — invalid encoding character is a fatal error
            decode_abort(dec->ctx, "BinHex: invalid character '%c' (0x%02X)",
                         ch, ch);
        }
        dec->accum = (dec->accum << 6) | val;
        dec->accum_bits += 6;
    }
    dec->accum_bits -= 8;
    return (int)((dec->accum >> dec->accum_bits) & 0xFF);
}

// hqx.md § 10.3 / § 5.2 — produce the next decompressed byte after
// RLE expansion.  Returns 0..255, or -1 on EOF.
static int hqx_decoded_byte(hqx_decoder_t *dec) {
    // Step 1: drain any pending repeat copies
    if (dec->rle_pending > 0) {
        dec->rle_pending--;
        return dec->rle_prev;
    }

    for (;;) {
        int raw = hqx_raw_byte(dec);
        if (raw < 0) {
            return -1;
        }

        if (dec->rle_marker_seen) {
            dec->rle_marker_seen = false;
            if (raw == 0x00) {
                // hqx.md § 5.3 — literal 0x90 escape: emit 0x90 and set
                // prev so subsequent markers can repeat it.
                dec->rle_prev = RLE_MARKER;
                return RLE_MARKER;
            }
            if (raw == 0x01) {
                // hqx.md § 5.3 — a count of 1 is illegal
                decode_abort(dec->ctx, "BinHex: illegal RLE count of 1");
            }
            // hqx.md § 5.2 step 3 — repeat prev byte `raw` times total.
            // One copy was already emitted before the marker; emit one more
            // now and queue the remainder.
            dec->rle_pending = raw - 2;
            return dec->rle_prev;
        }

        if ((uint8_t)raw == RLE_MARKER) {
            // hqx.md § 5.2 step 4 — marker produces no output; loop for count
            dec->rle_marker_seen = true;
            continue;
        }

        // hqx.md § 5.2 step 5 — normal byte
        dec->rle_prev = (uint8_t)raw;
        return raw;
    }
}

// Read exactly `n` decoded bytes into `buf`.
// Aborts via ctx on premature end of stream.
static void hqx_read_bytes(hqx_decoder_t *dec, uint8_t *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int b = hqx_decoded_byte(dec);
        if (b < 0) {
            // hqx.md § 9 — premature end of stream
            decode_abort(dec->ctx, "BinHex: premature end of stream "
                         "(needed %zu more bytes)", n - i);
        }
        buf[i] = (uint8_t)b;
    }
}

// ============================================================================
// Static Helpers — Header Parsing
// ============================================================================

// hqx.md § 6.3 — parse the variable-length header from the decoded stream.
// Also verifies the header CRC (hqx.md § 7).
static hqx_header_t hqx_parse_header(hqx_decoder_t *dec) {
    hqx_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));

    // First byte: filename length
    uint8_t name_len_byte;
    hqx_read_bytes(dec, &name_len_byte, 1);

    // hqx.md § 9 — filename length must be 1..63
    if (name_len_byte == 0 || name_len_byte > HQX_NAME_MAX) {
        decode_abort(dec->ctx, "BinHex: invalid filename length %u",
                     name_len_byte);
    }
    hdr.name_len = name_len_byte;

    // hqx.md § 6.3 — remaining header fields after the name-length byte:
    //   name_len bytes (name) + 1 (NUL) + 4 (type) + 4 (creator) +
    //   2 (flags) + 4 (data_len) + 4 (rsrc_len) = name_len + 19 bytes,
    //   followed by 2 bytes of header CRC.
    size_t payload_len = (size_t)name_len_byte + 19;
    size_t total_len = 1 + payload_len + 2; // name_len_byte + payload + CRC
    uint8_t buf[256 + 22];
    buf[0] = name_len_byte;
    hqx_read_bytes(dec, buf + 1, payload_len + 2);

    // hqx.md § 7.2 — verify header CRC using the self-checking property:
    // CRC over (content + stored CRC) should yield zero.
    uint16_t crc = crc16_ccitt(buf, total_len);
    if (crc != 0) {
        // hqx.md § 9 — header CRC mismatch
        decode_abort(dec->ctx, "BinHex: header CRC mismatch");
    }

    // Extract filename
    size_t copy_len = hdr.name_len;
    if (copy_len > HQX_NAME_MAX) {
        copy_len = HQX_NAME_MAX;
    }
    memcpy(hdr.name, buf + 1, copy_len);
    hdr.name[copy_len] = '\0';

    // hqx.md § 6.3 — field offsets relative to start of header:
    // type at offset 2+n, creator at 6+n, flags at 10+n,
    // data_len at 12+n, rsrc_len at 16+n
    size_t n = hdr.name_len;
    const uint8_t *tp = buf + 2 + n;   // type
    const uint8_t *cp = buf + 6 + n;   // creator
    const uint8_t *fp = buf + 10 + n;  // flags
    const uint8_t *dl = buf + 12 + n;  // data fork length
    const uint8_t *rl = buf + 16 + n;  // resource fork length

    hdr.mac_type    = rd32be(tp);
    hdr.mac_creator = rd32be(cp);
    hdr.finder_flags = rd16be(fp);
    hdr.data_len    = rd32be(dl);
    hdr.rsrc_len    = rd32be(rl);

    return hdr;
}

// ============================================================================
// Static Helpers — Fork Reading with CRC
// ============================================================================

// hqx.md § 6.4 / § 6.5 — read a fork of `fork_len` bytes from the decoded
// stream, verify the trailing 2-byte CRC, and return the data.
// hqx.md § 7.2 — uses the CRC placeholder rule for verification.
static peel_buf_t hqx_read_fork(hqx_decoder_t *dec, uint32_t fork_len,
                                const char *fork_name) {
    if (fork_len == 0) {
        // hqx.md § 6.6 — zero-length fork: still must read and verify CRC
        uint8_t crc_bytes[2];
        hqx_read_bytes(dec, crc_bytes, 2);
        uint16_t stored_crc = rd16be(crc_bytes);
        if (stored_crc != 0x0000) {
            decode_abort(dec->ctx, "BinHex: %s fork CRC mismatch "
                         "(empty fork, expected 0x0000)", fork_name);
        }
        return (peel_buf_t){0};
    }

    // Allocate and read fork content
    grow_buf_t gbuf;
    grow_init(&gbuf, fork_len, dec->ctx);

    uint8_t chunk[4096];
    uint32_t remaining = fork_len;
    while (remaining > 0) {
        size_t batch = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        hqx_read_bytes(dec, chunk, batch);
        grow_append(&gbuf, chunk, batch, dec->ctx);
        remaining -= (uint32_t)batch;
    }

    // hqx.md § 7.2 — verify using self-checking property:
    // CRC(content + stored_crc) should yield zero.
    uint8_t crc_bytes[2];
    hqx_read_bytes(dec, crc_bytes, 2);

    // Compute CRC over fork content then over the stored CRC bytes
    uint16_t crc = crc16_ccitt(gbuf.data, gbuf.len);
    crc = crc16_ccitt_update(crc, crc_bytes, 2);
    if (crc != 0) {
        grow_free(&gbuf);
        decode_abort(dec->ctx, "BinHex: %s fork CRC mismatch", fork_name);
    }

    return grow_finish(&gbuf);
}

// ============================================================================
// Static Helpers — Full Decode Pipeline
// ============================================================================

// Decode a BinHex 4.0 file into a peel_file_t with both forks and metadata.
// This is the shared implementation for both peel_hqx and peel_hqx_file.
static peel_file_t hqx_decode(const uint8_t *src, size_t len,
                               decode_ctx_t *ctx) {
    // hqx.md § 3.1 — locate the preamble identification string
    size_t after_preamble = hqx_find_preamble(src, len);
    if (after_preamble == (size_t)-1) {
        decode_abort(ctx, "BinHex: preamble not found");
    }

    // hqx.md § 3.2 — find the starting colon
    size_t payload_start = hqx_find_start_colon(src, len, after_preamble);
    if (payload_start == (size_t)-1) {
        decode_abort(ctx, "BinHex: no starting colon found");
    }

    // Initialise the three-layer decoder pipeline
    hqx_decoder_t dec;
    hqx_decoder_init(&dec, src, len, payload_start, ctx);

    // hqx.md § 6.3 — parse the header
    hqx_header_t hdr = hqx_parse_header(&dec);

    // hqx.md § 6.4 — read the data fork and verify its CRC
    peel_buf_t data_fork = hqx_read_fork(&dec, hdr.data_len, "data");

    // hqx.md § 6.5 — read the resource fork and verify its CRC
    peel_buf_t rsrc_fork = hqx_read_fork(&dec, hdr.rsrc_len, "resource");

    // Assemble the result
    peel_file_t file;
    memset(&file, 0, sizeof(file));

    // Populate metadata from the parsed header
    size_t nl = hdr.name_len;
    if (nl > sizeof(file.meta.name) - 1) {
        nl = sizeof(file.meta.name) - 1;
    }
    memcpy(file.meta.name, hdr.name, nl);
    file.meta.name[nl] = '\0';
    file.meta.mac_type    = hdr.mac_type;
    file.meta.mac_creator = hdr.mac_creator;

    // hqx.md § 8.2 — clear Finder flag bits that should not persist on decode
    file.meta.finder_flags = hdr.finder_flags & (uint16_t)~FINDER_CLEAR_MASK;

    file.data_fork = data_fork;
    file.resource_fork = rsrc_fork;

    return file;
}

// ============================================================================
// Operations (Public API) — Detection
// ============================================================================

// hqx.md § 3.1 — probe input for the BinHex 4.0 identification string.
bool hqx_detect(const uint8_t *src, size_t len) {
    return hqx_find_preamble(src, len) != (size_t)-1;
}

// ============================================================================
// Operations (Public API) — Wrapper Peel
// ============================================================================

// Decode a BinHex 4.0 file and return the data fork as a flat buffer.
// hqx.md § 2.1 — the full decoding pipeline is reversed: strip text
// envelope, decode 6-bit ASCII, expand RLE, parse binary stream.
peel_buf_t peel_hqx(const uint8_t *src, size_t len, peel_err_t **err) {
    *err = NULL;

    // Use setjmp/longjmp for deep-error abort throughout the decode pipeline
    decode_ctx_t ctx;
    if (setjmp(ctx.jmp) != 0) {
        *err = make_err("%s", ctx.errmsg);
        return (peel_buf_t){0};
    }

    peel_file_t file = hqx_decode(src, len, &ctx);

    // Return the data fork; free the resource fork
    peel_buf_t result = file.data_fork;
    peel_free(&file.resource_fork);
    return result;
}

// Decode a BinHex 4.0 file and return both forks plus metadata.
peel_file_t peel_hqx_file(const uint8_t *src, size_t len, peel_err_t **err) {
    *err = NULL;

    decode_ctx_t ctx;
    if (setjmp(ctx.jmp) != 0) {
        *err = make_err("%s", ctx.errmsg);
        return (peel_file_t){0};
    }

    return hqx_decode(src, len, &ctx);
}
