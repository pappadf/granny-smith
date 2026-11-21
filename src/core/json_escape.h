// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// json_escape.h
// The one JSON string escape set (RFC 8259): quote, backslash, the short
// forms \b \f \n \r \t, every other control byte as \u00XX, everything else
// (UTF-8 included) as itself.  Header-only and dependency-free so each
// writer builds on it whatever it links: value_format's vbuf form
// (vbuf_append_json_string / vbuf_append_json_escaped), storage_util's
// bounded-buffer and FILE* forms (json_escape, json_escape_dup, json_fputs),
// the job output records and the event text records.

#ifndef JSON_ESCAPE_H
#define JSON_ESCAPE_H

#include <stddef.h>

// Longest escape of one byte (\u00XX), without a terminator.
#define JSON_ESCAPE_MAX 6

// The escape JSON needs for byte `c` inside a string literal, written to
// `out` (not terminated).  Returns its length, or 0 when `c` stands for
// itself.
static inline size_t json_escape_char(unsigned char c, char out[JSON_ESCAPE_MAX]) {
    static const char hex[] = "0123456789abcdef";
    char short_form = 0;
    switch (c) {
    case '"':
    case '\\':
        short_form = (char)c;
        break;
    case '\b':
        short_form = 'b';
        break;
    case '\f':
        short_form = 'f';
        break;
    case '\n':
        short_form = 'n';
        break;
    case '\r':
        short_form = 'r';
        break;
    case '\t':
        short_form = 't';
        break;
    default:
        if (c >= 0x20)
            return 0;
        // The remaining control bytes have no short form
        out[0] = '\\';
        out[1] = 'u';
        out[2] = '0';
        out[3] = '0';
        out[4] = hex[c >> 4];
        out[5] = hex[c & 0xF];
        return 6;
    }
    out[0] = '\\';
    out[1] = short_form;
    return 2;
}

// The escaped length of byte `c` inside a JSON string (1 when it stands for
// itself): what a writer that sizes its output first counts.
static inline size_t json_escaped_len(unsigned char c) {
    char tmp[JSON_ESCAPE_MAX];
    size_t n = json_escape_char(c, tmp);
    return n ? n : 1;
}

#endif // JSON_ESCAPE_H
