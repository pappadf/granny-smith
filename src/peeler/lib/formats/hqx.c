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
//      (hqx.md § 3)  Found here.
//
//   2. 6-bit ASCII encoding — 64-character alphabet converting 8-bit
//      bytes to printable ASCII.  (hqx.md § 4)  The codec library's
//      CODEC_BINHEX, over the text after the opening colon.
//
//   3. Run-length encoding (RLE90) — 0x90 marker byte compresses
//      repeated-byte sequences.  (hqx.md § 5)  CODEC_RLE90_BINHEX,
//      stacked on layer 2 (codec_dec_stack).
//
// The decoded stream is a variable-length header, the data fork, and the
// resource fork, each followed by a CRC-16/XMODEM.  (hqx.md § 6)  The
// header is read here; each fork is the same stacked decoder with the bytes
// before it skipped and its CRC checked as the stream states it
// (codec_dec_opts_t.skip, .check_stored), so a fork decodes as far as it is
// read, like any other archive's.

#include "internal.h"

// ============================================================================
// Constants and Macros
// ============================================================================

// hqx.md § 3.1 — mandatory identification string that precedes the payload.
#define HQX_PREAMBLE "(This file must be converted with BinHex"

// hqx.md § 6.3 — maximum filename length in the header.
#define HQX_NAME_MAX 63

// hqx.md § 8.2 / Appendix B — Finder flag bits to clear on decode.
// isInvisible (bit 14), hasBeenInited (bit 7), OnDesk (bit 2).
#define FINDER_CLEAR_MASK 0x4084u

// ============================================================================
// Type Definitions (Private)
// ============================================================================

// Where everything is: the text's offset and the decoded header.
// hqx.md § 6.3 — variable-length header structure.
typedef struct {
    uint64_t payload; // offset of the text after the opening colon
    uint32_t header_len; // decoded bytes of header, its CRC included
    char name[HQX_NAME_MAX + 1];
    uint8_t name_len;
    uint32_t mac_type;
    uint32_t mac_creator;
    uint16_t finder_flags;
    uint32_t data_len;
    uint32_t rsrc_len;
} hqx_layout_t;

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
static size_t hqx_find_start_colon(const uint8_t *src, size_t len, size_t from) {
    for (size_t i = from; i < len; i++) {
        if (src[i] == ':') {
            return i + 1;
        }
    }
    return (size_t)-1;
}

// ============================================================================
// Static Helpers — The decoded stream
// ============================================================================

// The decoded stream over its text (memory, or a range of a source): the
// 6-bit layer with the run-length layer on it, `opts` the outer's.
static codec_dec_t *hqx_stream(const uint8_t *text, size_t text_len, peel_source_t *src, uint64_t off, uint64_t len,
                               const codec_dec_opts_t *opts, peel_err_t **err) {
    codec_dec_t *inner;
    if (src) {
        codec_dec_opts_t o = {.out_len = CODEC_LEN_UNKNOWN};
        inner = peel_fork_decoder(src, off, len, CODEC_BINHEX, &o, "BinHex", err);
        if (!inner)
            return NULL;
    } else {
        codec_dec_opts_t o = {
            .in = {.src = text, .len = text_len},
              .out_len = CODEC_LEN_UNKNOWN
        };
        inner = codec_dec_new(CODEC_BINHEX, &o, NULL);
    }
    int rc = 0;
    codec_dec_t *d = codec_dec_stack(inner, CODEC_RLE90_BINHEX, opts, &rc);
    if (!d && !*err)
        *err = make_err("BinHex: %s", codec_strerror(rc));
    return d;
}

// Exactly `n` decoded bytes into `buf`.  0, or -1 with *err set.
static int hqx_read_exact(codec_dec_t *d, uint8_t *buf, size_t n, peel_err_t **err) {
    size_t got = 0;
    int rc = codec_dec_run(d, buf, n, &got);
    if (got == n)
        return 0;
    if (rc < 0)
        *err = make_err("BinHex: %s", codec_dec_message(d));
    else
        *err = make_err("BinHex: premature end of stream (needed %zu more bytes)", n - got);
    return -1;
}

// hqx.md §§ 3, 6.3 — find the payload in `text` and read the header from
// it, verifying its CRC (hqx.md § 7).  0, or -1 with *err set.
static int hqx_layout(const uint8_t *text, size_t len, hqx_layout_t *L, peel_err_t **err) {
    memset(L, 0, sizeof(*L));
    size_t after = hqx_find_preamble(text, len);
    if (after == (size_t)-1) {
        *err = make_err("BinHex: preamble not found");
        return -1;
    }
    size_t start = hqx_find_start_colon(text, len, after);
    if (start == (size_t)-1) {
        *err = make_err("BinHex: no starting colon found");
        return -1;
    }
    L->payload = start;
    codec_dec_opts_t o = {.out_len = CODEC_LEN_UNKNOWN};
    codec_dec_t *d = hqx_stream(text + start, len - start, NULL, 0, 0, &o, err);
    if (!d)
        return -1;
    // First byte: the filename length, 1..63 (hqx.md § 9).  Then the name,
    // a NUL, type, creator, flags, the two fork lengths (name_len + 19
    // bytes) and the header's CRC.
    uint8_t buf[1 + HQX_NAME_MAX + 19 + 2];
    int rc = hqx_read_exact(d, buf, 1, err);
    if (rc == 0 && (buf[0] == 0 || buf[0] > HQX_NAME_MAX)) {
        *err = make_err("BinHex: invalid filename length %u", buf[0]);
        rc = -1;
    }
    size_t n = buf[0], total = 1 + n + 19 + 2;
    if (rc == 0)
        rc = hqx_read_exact(d, buf + 1, total - 1, err);
    codec_dec_free(d);
    if (rc != 0)
        return -1;
    // hqx.md § 7.2 — the self-checking property: the CRC over the content
    // and the stored CRC is zero.
    if (codec_crc16_xmodem(0, buf, total) != 0) {
        *err = make_err("BinHex: header CRC mismatch");
        return -1;
    }
    L->header_len = (uint32_t)total;
    L->name_len = (uint8_t)n;
    memcpy(L->name, buf + 1, n);
    L->name[n] = '\0';
    // hqx.md § 6.3 — type at 2+n, creator at 6+n, flags at 10+n, data
    // length at 12+n, resource length at 16+n.
    L->mac_type = rd32be(buf + 2 + n);
    L->mac_creator = rd32be(buf + 6 + n);
    L->finder_flags = rd16be(buf + 10 + n);
    L->data_len = rd32be(buf + 12 + n);
    L->rsrc_len = rd32be(buf + 16 + n);
    return 0;
}

// One fork's decoder: the decoded stream from after the header (and, for
// the resource fork, the data fork and its CRC), exactly the fork's length,
// its CRC the two bytes that follow it (hqx.md § 7.2).  Over the text in
// memory, or over a source holding it.
static codec_dec_t *hqx_fork_decoder(const hqx_layout_t *L, int fork, const uint8_t *text, size_t len,
                                     peel_source_t *src, peel_err_t **err) {
    uint32_t fork_len = fork == PEEL_FORK_RSRC ? L->rsrc_len : L->data_len;
    if (fork_len > PEEL_MAX_FORK) {
        *err = make_err("BinHex: %s fork declares %u bytes, over the %u MiB limit",
                        fork == PEEL_FORK_RSRC ? "resource" : "data", fork_len, (unsigned)(PEEL_MAX_FORK >> 20));
        return NULL;
    }
    codec_dec_opts_t o = {.out_len = fork_len,
                          .exact = 1,
                          .skip = L->header_len + (fork == PEEL_FORK_RSRC ? (uint64_t)L->data_len + 2 : 0),
                          .check = CODEC_CHECK_CRC16_XMODEM,
                          .check_stored = 2};
    if (src)
        return hqx_stream(NULL, 0, src, L->payload, peel_source_size(src) - L->payload, &o, err);
    return hqx_stream(text + L->payload, len - L->payload, NULL, 0, 0, &o, err);
}

// Decode a fork whole through its decoder.  hqx.md § 6.6 — an empty fork
// still has its CRC checked.
static peel_buf_t hqx_read_fork(codec_dec_t *d, const hqx_layout_t *L, int fork, peel_err_t **err) {
    const char *name = fork == PEEL_FORK_RSRC ? "resource" : "data";
    size_t len = fork == PEEL_FORK_RSRC ? L->rsrc_len : L->data_len;
    uint8_t *out = malloc(len ? len : 1);
    int64_t n = out ? codec_dec_read(d, out, len) : CODEC_ERR_NOMEM;
    if (n != (int64_t)len) {
        if (n == CODEC_ERR_CHECKSUM)
            *err = make_err("BinHex: %s fork CRC mismatch", name);
        else if (n == CODEC_ERR_TRUNCATED)
            *err = make_err("BinHex: premature end of stream in the %s fork", name);
        else
            *err =
                make_err("BinHex: %s fork: %s", name, *codec_dec_message(d) ? codec_dec_message(d) : codec_strerror(n));
        codec_dec_free(d);
        free(out);
        return (peel_buf_t){0};
    }
    codec_dec_free(d);
    if (len == 0) {
        free(out);
        return (peel_buf_t){0};
    }
    return (peel_buf_t){.data = out, .size = len, .owned = true};
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

// Decode a BinHex 4.0 file and return both forks plus metadata.
// hqx.md § 2.1 — the full decoding pipeline is reversed: strip text
// envelope, decode 6-bit ASCII, expand RLE, parse binary stream.
peel_file_t peel_hqx_file(const uint8_t *src, size_t len, peel_err_t **err) {
    *err = NULL;
    hqx_layout_t L;
    if (hqx_layout(src, len, &L, err) != 0)
        return (peel_file_t){0};
    peel_file_t file = {0};
    for (int fork = PEEL_FORK_DATA; fork <= PEEL_FORK_RSRC; fork++) {
        codec_dec_t *d = hqx_fork_decoder(&L, fork, src, len, NULL, err);
        peel_buf_t b = d ? hqx_read_fork(d, &L, fork, err) : (peel_buf_t){0};
        if (*err) {
            peel_free(&file.data_fork);
            return (peel_file_t){0};
        }
        if (fork == PEEL_FORK_RSRC)
            file.resource_fork = b;
        else
            file.data_fork = b;
    }
    size_t np = 0; // one sanitised component (peel_append_segment)
    peel_append_segment(file.meta.name, sizeof(file.meta.name), &np, (const uint8_t *)L.name, L.name_len);
    file.meta.mac_type = L.mac_type;
    file.meta.mac_creator = L.mac_creator;
    // hqx.md § 8.2 — clear Finder flag bits that should not persist on decode
    file.meta.finder_flags = L.finder_flags & (uint16_t)~FINDER_CLEAR_MASK;
    return file;
}

// Decode a BinHex 4.0 file and return the data fork as a flat buffer.
peel_buf_t peel_hqx(const uint8_t *src, size_t len, peel_err_t **err) {
    peel_file_t f = peel_hqx_file(src, len, err);
    peel_free(&f.resource_fork);
    return f.data_fork;
}

// ============================================================================
// Structure-first access (peeler.h: peel_open)
// ============================================================================
//
// Listing decodes only the header, from the probe's head.  A fork is not a
// contiguous range of the file (it is 6-bit text, run-length coded), so it
// opens as a decode-through source over its decoder: a read decodes the
// text as far as it reaches.

static bool hqx_detect_probe(const peel_probe_t *p) {
    return hqx_detect(p->head, p->head_len);
}

static int hqx_open(peel_archive_t *a, const peel_probe_t *p, peel_err_t **err) {
    hqx_layout_t *L = calloc(1, sizeof(*L));
    if (!L) {
        *err = make_err("BinHex: out of memory");
        return -1;
    }
    if (hqx_layout(p->head, p->head_len, L, err) != 0) {
        free(L);
        return -1;
    }
    peel_entry_t *e = peel_archive_add(a);
    if (!e) {
        free(L);
        *err = make_err("BinHex: out of memory");
        return -1;
    }
    a->priv = L;
    size_t np = 0;
    peel_append_segment(e->path, sizeof(e->path), &np, (const uint8_t *)L->name, L->name_len);
    e->mac_type = L->mac_type;
    e->mac_creator = L->mac_creator;
    e->finder_flags = L->finder_flags & (uint16_t)~FINDER_CLEAR_MASK;
    e->data_len = L->data_len;
    e->rsrc_len = L->rsrc_len;
    e->data_packed = e->rsrc_packed = UINT64_MAX; // interleaved in the text
    e->data_tier = e->rsrc_tier = PEEL_TIER_EARNED;
    return 0;
}

static codec_dec_t *hqx_decoder(peel_archive_t *a, int i, int fork, peel_err_t **err) {
    (void)i;
    return hqx_fork_decoder(a->priv, fork, NULL, 0, a->src, err);
}

static peel_buf_t hqx_decode_fork(peel_archive_t *a, int i, int fork, peel_err_t **err) {
    codec_dec_t *d = hqx_decoder(a, i, fork, err);
    return d ? hqx_read_fork(d, a->priv, fork, err) : (peel_buf_t){0};
}

static void hqx_close(peel_archive_t *a) {
    free(a->priv);
}

const peel_fmt_t peel_fmt_hqx = {
    .desc = {"hqx", true, hqx_detect_probe},
    .open = hqx_open,
    .decode = hqx_decode_fork,
    .decoder = hqx_decoder,
    .close = hqx_close,
};
