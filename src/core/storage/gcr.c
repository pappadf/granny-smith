// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gcr.c
// Apple 6-and-2 GCR: codeword tables, the sector nibblizer with its
// rotate-add-xor checksum chain, and the address field.  See gcr.h.

#include "gcr.h"

// The 64 disk bytes the 6-bit values 0x00-0x3F encode to.
const uint8_t gcr_encode6[64] = {0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6, 0xA7, 0xAB, 0xAC, 0xAD, 0xAE,
                                 0xAF, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE,
                                 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3, 0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD,
                                 0xDE, 0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xF2,
                                 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF};

// Disk byte -> 6-bit value, 0xFF for a byte that is no codeword.  A constant
// table keyed on the whole byte: a lazily built one was a data race between
// the I/O worker (MOOF) and the emulator thread (IWM), and one keyed on
// `byte & 0x7F` aliased 64 illegal bytes onto legal values.
const uint8_t gcr_decode6[256] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x01,
    0xFF, 0xFF, 0x02, 0x03, 0xFF, 0x04, 0x05, 0x06, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x08, 0xFF, 0xFF, 0xFF,
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0xFF, 0xFF, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0xFF, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1A, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x1B, 0xFF, 0x1C, 0x1D, 0x1E, 0xFF,
    0xFF, 0xFF, 0x1F, 0xFF, 0xFF, 0x20, 0x21, 0xFF, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0x29, 0x2A, 0x2B, 0xFF, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0xFF, 0xFF, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0xFF, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
};

// ============================================================================
// The checksum chain
// ============================================================================
//
// Three registers: cc rotates left one bit per triplet, and each byte is
// added into the next register (with the previous register's carry) and
// XORed with the register before it.  Every value written is six bits; a
// triplet's high bits go first, in a value of their own.

// Encode three bytes at `src` into four six-bit values at `dst`.
static void encode_triplet(const uint8_t *src, uint16_t *ca, uint16_t *cb, uint16_t *cc, uint8_t *dst) {
    *cc = (uint16_t)((*cc << 1) | ((*cc >> 7) & 1)); // rotate cc left
    *ca &= 0xFF;
    *ca = (uint16_t)(*ca + src[0] + (*cc & 1)); // add with the rotate's carry
    uint8_t ba = (uint8_t)(src[0] ^ *cc);
    *cb &= 0xFF;
    *cb = (uint16_t)(*cb + src[1] + ((*ca >> 8) & 1)); // ...and with ca's carry
    uint8_t bb = (uint8_t)(src[1] ^ *ca);
    *cc &= 0xFF;
    *cc = (uint16_t)(*cc + src[2] + ((*cb >> 8) & 1)); // ...and with cb's carry
    uint8_t bc = (uint8_t)(src[2] ^ *cb);
    dst[0] = (uint8_t)(((ba >> 2) & 0x30) | ((bb >> 4) & 0x0C) | ((bc >> 6) & 0x03));
    dst[1] = (uint8_t)(ba & 0x3F);
    dst[2] = (uint8_t)(bb & 0x3F);
    dst[3] = (uint8_t)(bc & 0x3F);
}

// Decode four six-bit values at `src` into three bytes at `dst`:
// encode_triplet's inverse.
static void decode_triplet(const uint8_t *src, uint16_t *ca, uint16_t *cb, uint16_t *cc, uint8_t *dst) {
    uint8_t ba = (uint8_t)(((src[0] << 2) & 0xC0) | (src[1] & 0x3F));
    uint8_t bb = (uint8_t)(((src[0] << 4) & 0xC0) | (src[2] & 0x3F));
    uint8_t bc = (uint8_t)(((src[0] << 6) & 0xC0) | (src[3] & 0x3F));
    *cc = (uint16_t)((*cc << 1) | ((*cc >> 7) & 1));
    dst[0] = (uint8_t)(ba ^ *cc);
    *ca &= 0xFF;
    *ca = (uint16_t)(*ca + dst[0] + (*cc & 1));
    dst[1] = (uint8_t)(bb ^ *ca);
    *cb &= 0xFF;
    *cb = (uint16_t)(*cb + dst[1] + ((*ca >> 8) & 1));
    dst[2] = (uint8_t)(bc ^ *cb);
    *cc &= 0xFF;
    *cc = (uint16_t)(*cc + dst[2] + ((*cb >> 8) & 1));
}

// ============================================================================
// Sectors
// ============================================================================

void gcr_sector_nibblize(const uint8_t tag[GCR_TAG_BYTES], const uint8_t data[GCR_DATA_BYTES], bool bad_cksum,
                         uint8_t out6[GCR_SECTOR_VALUES]) {
    uint16_t ca = 0, cb = 0, cc = 0;
    uint8_t *dst = out6;
    // 4 tag triplets, then 170 data triplets.
    for (int i = 0; i < GCR_TAG_BYTES; i += 3, dst += 4)
        encode_triplet(tag + i, &ca, &cb, &cc, dst);
    for (int i = 0; i < GCR_DATA_BYTES - 2; i += 3, dst += 4)
        encode_triplet(data + i, &ca, &cb, &cc, dst);
    // The last two data bytes make a pair, not a triplet.
    cc = (uint16_t)((cc << 1) | ((cc >> 7) & 1));
    ca &= 0xFF;
    ca = (uint16_t)(ca + data[510] + (cc & 1));
    uint8_t ba = (uint8_t)(data[510] ^ cc);
    cb &= 0xFF;
    cb = (uint16_t)(cb + data[511] + ((ca >> 8) & 1));
    uint8_t bb = (uint8_t)(data[511] ^ ca);
    *dst++ = (uint8_t)(((ba >> 2) & 0x30) | ((bb >> 4) & 0x0C));
    *dst++ = (uint8_t)(ba & 0x3F);
    *dst++ = (uint8_t)(bb & 0x3F);
    // A bad checksum differs from the right one in every bit of ca.
    if (bad_cksum)
        ca ^= 0xFF;
    // ...then the three checksum registers, themselves encoded as a triplet.
    uint8_t c[3] = {(uint8_t)ca, (uint8_t)cb, (uint8_t)cc};
    *dst++ = (uint8_t)(((c[0] >> 2) & 0x30) | ((c[1] >> 4) & 0x0C) | ((c[2] >> 6) & 0x03));
    *dst++ = (uint8_t)(c[0] & 0x3F);
    *dst++ = (uint8_t)(c[1] & 0x3F);
    *dst++ = (uint8_t)(c[2] & 0x3F);
}

bool gcr_sector_denibblize(const uint8_t in6[GCR_SECTOR_VALUES], uint8_t tag[GCR_TAG_BYTES],
                           uint8_t data[GCR_DATA_BYTES]) {
    uint16_t ca = 0, cb = 0, cc = 0;
    const uint8_t *src = in6;
    for (int i = 0; i < GCR_TAG_BYTES; i += 3, src += 4)
        decode_triplet(src, &ca, &cb, &cc, tag + i);
    for (int i = 0; i < GCR_DATA_BYTES - 2; i += 3, src += 4)
        decode_triplet(src, &ca, &cb, &cc, data + i);
    // The final pair.
    uint8_t ba = (uint8_t)(((src[0] << 2) & 0xC0) | (src[1] & 0x3F));
    uint8_t bb = (uint8_t)(((src[0] << 4) & 0xC0) | (src[2] & 0x3F));
    cc = (uint16_t)((cc << 1) | ((cc >> 7) & 1));
    data[510] = (uint8_t)(ba ^ cc);
    ca &= 0xFF;
    ca = (uint16_t)(ca + data[510] + (cc & 1));
    data[511] = (uint8_t)(bb ^ ca);
    cb &= 0xFF;
    cb = (uint16_t)(cb + data[511] + ((ca >> 8) & 1));
    src += 3;
    // The checksum the stream carries, against the one recomputed.
    uint8_t got[3] = {(uint8_t)(((src[0] << 2) & 0xC0) | (src[1] & 0x3F)),
                      (uint8_t)(((src[0] << 4) & 0xC0) | (src[2] & 0x3F)),
                      (uint8_t)(((src[0] << 6) & 0xC0) | (src[3] & 0x3F))};
    return got[0] == (uint8_t)ca && got[1] == (uint8_t)cb && got[2] == (uint8_t)cc;
}

int gcr_sector_from_disk(const uint8_t disk[GCR_SECTOR_VALUES], uint8_t tag[GCR_TAG_BYTES],
                         uint8_t data[GCR_DATA_BYTES]) {
    uint8_t v[GCR_SECTOR_VALUES];
    // Every byte must be a codeword: an illegal one can leave the checksum
    // intact, so the checksum alone does not catch it.
    for (int i = 0; i < GCR_SECTOR_VALUES; i++) {
        v[i] = gcr_decode6[disk[i]];
        if (v[i] == GCR_BAD_CODEWORD)
            return -1;
    }
    return gcr_sector_denibblize(v, tag, data) ? 1 : 0;
}

// ============================================================================
// Address field
// ============================================================================

void gcr_addr_encode(int track, int sector, int side, uint8_t format, uint8_t out6[GCR_ADDR_VALUES]) {
    // Side in bit 5; track bit 6 (tracks 64-79) below it.
    uint8_t side_enc = (uint8_t)(((side ? 1 : 0) << 5) | ((track >> 6) & 0x1F));
    out6[0] = (uint8_t)(track & 0x3F);
    out6[1] = (uint8_t)(sector & 0x3F);
    out6[2] = (uint8_t)(side_enc & 0x3F);
    out6[3] = (uint8_t)(format & 0x3F);
    out6[4] = (uint8_t)((track ^ sector ^ side_enc ^ format) & 0x3F);
}

bool gcr_addr_from_disk(const uint8_t disk[GCR_ADDR_VALUES], int *track, int *sector, int *side, uint8_t *format) {
    uint8_t h[GCR_ADDR_VALUES];
    for (int i = 0; i < GCR_ADDR_VALUES; i++) {
        h[i] = gcr_decode6[disk[i]];
        if (h[i] == GCR_BAD_CODEWORD)
            return false;
    }
    if ((h[0] ^ h[1] ^ h[2] ^ h[3]) != h[4])
        return false;
    if (track)
        *track = h[0] | ((h[2] & 0x01) << 6);
    if (sector)
        *sector = h[1];
    if (side)
        *side = (h[2] >> 5) & 1;
    if (format)
        *format = h[3];
    return true;
}

// ============================================================================
// Framing
// ============================================================================

size_t gcr_frame_revolution(const uint8_t *bits, uint32_t nbits, uint8_t *out, size_t cap) {
    if (!bits || !out || nbits == 0)
        return 0;
    uint32_t reg = 0;
    size_t n = 0;
    // Revolution 1 only syncs the shift register; revolution 2 is kept.
    for (int rev = 0; rev < 2; rev++) {
        for (uint32_t k = 0; k < nbits; k++) {
            reg = ((reg << 1) | ((bits[k >> 3] >> (7 - (k & 7))) & 1)) & 0xFF; // shift one cell in
            if (!(reg & 0x80))
                continue; // the byte is not complete yet
            if (rev == 1) {
                if (n == cap)
                    return 0; // the caller's buffer is too small
                out[n++] = (uint8_t)reg;
            }
            reg = 0; // the latch takes the byte and the register restarts
        }
    }
    return n;
}
