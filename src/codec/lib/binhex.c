// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// binhex.c
// BinHex 4.0's 6-bit text encoding (see codec.h).  Format notes:
// src/peeler/docs/hqx.md §§ 3, 4.
//
// The input is the text after the payload's opening colon.  Each character
// of a 64-character alphabet stands for 6 bits, most significant first;
// every 8 of them make a byte.  Line breaks, tabs and spaces between
// characters carry nothing.  The stream ends at the closing colon (or with
// its input), any bits short of a whole byte dropped.  Under it is BinHex's
// run-length coding (CODEC_RLE90_BINHEX, stacked on this one), and under
// that the header and forks the container reads.

#include "codec_internal.h"

#include <stdio.h>

#define BH_BURST 1 // one character makes at most one byte

static const char k_alphabet[] = "!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr";

typedef struct {
    uint8_t value[256]; // a character's 6 bits, or 0xFF
    uint32_t acc; // bits not yet made into a byte
    int nbits;
} binhex_t;

static int bh_init(codec_dec_t *d) {
    binhex_t *s = d->st;
    memset(s->value, 0xFF, sizeof(s->value));
    for (unsigned i = 0; i < 64; i++)
        s->value[(unsigned char)k_alphabet[i]] = (uint8_t)i;
    return 0;
}

static int bh_fill(codec_dec_t *d) {
    binhex_t *s = d->st;
    codec_in_t *in = &d->in;
    while (codec_room_for(d, BH_BURST)) {
        int c = codec_in_byte(in);
        if (c < 0 || c == ':')
            return CODEC_END;
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ')
            continue;
        uint8_t v = s->value[c];
        if (v > 63) {
            char msg[64];
            snprintf(msg, sizeof(msg), "binhex: invalid character '%c' (0x%02X)", c >= 0x20 && c < 0x7F ? c : '?', c);
            return codec_fail(d, CODEC_ERR_DATA, msg);
        }
        s->acc = (s->acc << 6 | v) & 0xFFFF;
        s->nbits += 6;
        if (s->nbits >= 8) {
            s->nbits -= 8;
            ring_put(&d->ring, (uint8_t)(s->acc >> s->nbits));
        }
    }
    return CODEC_MORE;
}

const codec_dec_ops_t codec_binhex_dec = {
    .state_size = sizeof(binhex_t), .burst = BH_BURST, .init = bh_init, .fill = bh_fill};
