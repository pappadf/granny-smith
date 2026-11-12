// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// codec.c
// Decoding by codec id, codec names and result-code text (see codec.h).

#include "codec_internal.h"

#include <string.h>

const char *codec_strerror(int64_t rc) {
    if (rc >= 0)
        return "ok";
    switch (rc) {
    case CODEC_ERR_DATA:
        return "malformed compressed data";
    case CODEC_ERR_TRUNCATED:
        return "truncated compressed data";
    case CODEC_ERR_OVERFLOW:
        return "decodes to more than expected";
    case CODEC_ERR_NOMEM:
        return "out of memory";
    case CODEC_ERR_UNSUPPORTED:
        return "unsupported compression feature";
    case CODEC_ERR_CHECKSUM:
        return "checksum mismatch";
    case CODEC_ERR_ARG:
        return "invalid argument";
    default:
        return "decode error";
    }
}

// A stored payload: exactly the bytes, refusing more than fit.
static int64_t copy_decode(const uint8_t *src, size_t len, uint8_t *dst, size_t cap) {
    if (len > cap)
        return CODEC_ERR_OVERFLOW;
    if (len && (!src || !dst))
        return CODEC_ERR_ARG;
    if (len)
        memcpy(dst, src, len);
    return (int64_t)len;
}

int64_t codec_decode(codec_id_t id, const uint8_t *src, size_t len, uint8_t *dst, size_t cap) {
    switch (id) {
    case CODEC_COPY:
        return copy_decode(src, len, dst, cap);
    case CODEC_DEFLATE:
        return codec_inflate(src, len, dst, cap);
    case CODEC_ZLIB:
        return codec_zlib_inflate(src, len, dst, cap);
    case CODEC_ADC:
        return codec_adc_decompress(src, len, dst, cap);
    case CODEC_BZIP2:
        return codec_bzip2_decompress(src, len, dst, cap);
    case CODEC_LZFSE:
        return codec_lzfse_decompress(src, len, dst, cap);
    case CODEC_XZ:
        return codec_xz_decompress(src, len, dst, cap);
    case CODEC_LZMA_ALONE:
        return codec_lzma_alone_decompress(src, len, dst, cap);
    default:
        return CODEC_ERR_ARG;
    }
}

const char *codec_name(codec_id_t id) {
    static const char *const names[CODEC_COUNT] = {
        [CODEC_COPY] = "copy",   [CODEC_DEFLATE] = "deflate", [CODEC_ZLIB] = "zlib", [CODEC_ADC] = "adc",
        [CODEC_BZIP2] = "bzip2", [CODEC_LZFSE] = "lzfse",     [CODEC_XZ] = "xz",     [CODEC_LZMA_ALONE] = "lzma",
    };
    return (unsigned)id < CODEC_COUNT ? names[id] : "?";
}
