// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// decoders.h
// Per-resource-type decoders for the `re` orchestrator.  Each decoder
// has the same shape: take a raw resource buffer and write decoded
// JSON and/or plain text to host files.  Dispatch happens through
// re_decode_dispatch(), which picks the right decoder based on the
// 4-CC type and falls back to "no decoder" cleanly.

#pragma once

#ifndef GS_RE_DECODERS_H
#define GS_RE_DECODERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// One decoder.  Writes JSON to `json_fp` (always required) and an
// optional plain-text summary to `txt_fp` (NULL = skip).  Returns 0 on
// success, negative on failure (caller logs).  Each decoder is
// expected to write a complete top-level JSON object terminated with
// a newline — the manifest writer wraps these in array context.
typedef int (*re_decoder_fn)(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp);

// Dispatch a resource to its decoder.  Returns the decoder name (e.g.
// "vers") on hit, NULL if no decoder is registered for `type`.  When
// no decoder is registered the caller can fall back to the "raw bytes
// + minimal stub" path.
const char *re_decode_dispatch_name(const uint8_t type[4]);

// Run the dispatched decoder.  Returns 1 if a decoder ran (success),
// 0 if no decoder for the type, negative on decoder failure.
int re_decode_dispatch(const uint8_t type[4], const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp);

// Convenience: write a JSON string with proper escaping of \" \\ \n.
// Used by decoders for short string fields.
void re_json_write_string(FILE *fp, const char *s);

// Read a Pascal string at `off` from `bytes`, transcode MacRoman to
// UTF-8 into `out`, and return the number of bytes consumed (1 length
// byte + payload).  Returns 0 on out-of-range.
size_t re_pstring_to_utf8(const uint8_t *bytes, size_t len, size_t off, char *out, size_t cap);

// Individual decoder entry points — exposed for the dispatcher and
// for unit tests.
int re_decode_vers(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp);
int re_decode_str_single(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp); // "STR "
int re_decode_strlist(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp); // "STR#"
int re_decode_text(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp);
int re_decode_menu(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp);
int re_decode_mbar(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp);
int re_decode_dlog(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp);
int re_decode_alrt(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp);
int re_decode_ditl(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp);
int re_decode_size(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp);
int re_decode_bndl(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp);
int re_decode_fref(const uint8_t *bytes, size_t len, FILE *json_fp, FILE *txt_fp);

#endif // GS_RE_DECODERS_H
