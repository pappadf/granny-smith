// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// adc.c
// Apple Data Compression (ADC) decoder (see codec.h): the byte-aligned
// LZ77 variant Disk Copy 6 uses for NDIF chunks and UDIF for UDCO chunks.
// Reverse-engineered; matches dmg2img's adc.c and Aaru.
//
// The stream is a sequence of tokens.  The top two bits of each tag byte
// select the token type:
//   1xxxxxxx  literal run  : len = (tag & 0x7F) + 1, then `len` literal bytes
//   01xxxxxx  long match   : len = (tag & 0x3F) + 4, offset = BE16(next two)
//   00xxxxxx  short match  : len = ((tag>>2) & 0x0F) + 3, offset = ((tag&3)<<8)|next
// A match copies from offset + 1 bytes back, a byte at a time, so it may
// overlap what it writes (a run).  The stream has no end marker: it is as
// long as the container says (out_len), and a final token reaching past
// that is cut there.

#include "codec_internal.h"

#include <stdlib.h>

// ============================================================================
// Constants
// ============================================================================

#define ADC_WINDOW 65536 // the farthest a match reaches (offset 65535 + 1)
#define ADC_BURST  128 // the most one token writes (a literal run)

// ============================================================================
// The decoder
// ============================================================================

static int adc_fill(codec_dec_t *d) {
    codec_in_t *in = &d->in;
    codec_ring_t *r = &d->ring;
    while (codec_left(d) > 0) {
        if (ring_room(r) < ADC_BURST)
            return CODEC_MORE;
        int tag = codec_in_byte(in);
        if (tag < 0)
            return codec_fail(d, CODEC_ERR_TRUNCATED, "adc: the stream ends short of its size");
        size_t len, off;
        if (tag & 0x80) {
            // A literal run.
            len = (size_t)(tag & 0x7F) + 1;
            if (len > codec_left(d))
                len = (size_t)codec_left(d);
            for (size_t i = 0; i < len; i++) {
                int b = codec_in_byte(in);
                if (b < 0)
                    return codec_fail(d, CODEC_ERR_TRUNCATED, "adc: truncated literal run");
                ring_put(r, (uint8_t)b);
            }
            continue;
        }
        int b1 = codec_in_byte(in);
        if (b1 < 0)
            return codec_fail(d, CODEC_ERR_TRUNCATED, "adc: truncated match");
        if (tag & 0x40) {
            // A long match: a 16-bit offset.
            int b2 = codec_in_byte(in);
            if (b2 < 0)
                return codec_fail(d, CODEC_ERR_TRUNCATED, "adc: truncated match");
            len = (size_t)(tag & 0x3F) + 4;
            off = (size_t)b1 << 8 | (size_t)b2;
        } else {
            // A short match: a 10-bit offset.
            len = (size_t)((tag >> 2) & 0x0F) + 3;
            off = (size_t)(tag & 0x03) << 8 | (size_t)b1;
        }
        if (off + 1 > r->total)
            return codec_fail(d, CODEC_ERR_DATA, "adc: reference before the start of the output");
        if (len > codec_left(d))
            len = (size_t)codec_left(d);
        ring_copy(r, off + 1, len);
    }
    return CODEC_END;
}

static int adc_init(codec_dec_t *d) {
    d->st = NULL;
    return codec_ring_alloc(d, ADC_WINDOW, ADC_BURST);
}

static void adc_free(codec_dec_t *d) {
    (void)d;
}

const codec_dec_ops_t codec_adc_dec = {adc_init, adc_fill, adc_free, true};
