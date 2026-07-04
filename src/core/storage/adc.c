// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// adc.c
// Apple Data Compression (ADC) decompressor — see adc.h.
//
// The stream is a sequence of tokens.  The top two bits of each tag byte
// select the token type:
//   1xxxxxxx  literal run  : len = (tag & 0x7F) + 1, then `len` literal bytes
//   01xxxxxx  long match   : len = (tag & 0x3F) + 4, offset = BE16(next two)
//   00xxxxxx  short match  : len = ((tag>>2) & 0x0F) + 3, offset = ((tag&3)<<8)|next
// For matches the copy source is (out_pos - offset - 1) and the copy is done
// one byte at a time so overlapping runs (RLE) work.

#include "adc.h"

long adc_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap) {
    size_t ip = 0;
    size_t op = 0;

    while (ip < in_len && op < out_cap) {
        uint8_t tag = in[ip];

        if (tag & 0x80) {
            // Literal run.
            size_t len = (size_t)(tag & 0x7F) + 1;
            ip++;
            if (len > in_len - ip)
                len = in_len - ip; // truncated stream: copy what's left
            if (len > out_cap - op)
                len = out_cap - op;
            for (size_t k = 0; k < len; k++)
                out[op++] = in[ip++];
        } else if (tag & 0x40) {
            // Long back-reference (3-byte token).
            size_t len = (size_t)(tag & 0x3F) + 4;
            if (in_len - ip < 3)
                break; // truncated token
            size_t off = ((size_t)in[ip + 1] << 8) | (size_t)in[ip + 2];
            ip += 3;
            if (off + 1 > op)
                return -1; // reference before start of output
            size_t src = op - off - 1;
            for (size_t k = 0; k < len && op < out_cap; k++)
                out[op++] = out[src++];
        } else {
            // Short back-reference (2-byte token).
            size_t len = (size_t)((tag >> 2) & 0x0F) + 3;
            if (in_len - ip < 2)
                break; // truncated token
            size_t off = ((size_t)(tag & 0x03) << 8) | (size_t)in[ip + 1];
            ip += 2;
            if (off + 1 > op)
                return -1;
            size_t src = op - off - 1;
            for (size_t k = 0; k < len && op < out_cap; k++)
                out[op++] = out[src++];
        }
    }
    return (long)op;
}
