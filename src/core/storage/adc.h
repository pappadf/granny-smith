// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// adc.h
// Apple Data Compression (ADC) decompressor.  ADC is the byte-aligned LZ77
// variant Disk Copy 6.x uses for NDIF (0x83) chunks and that UDIF later
// reused (chunk type 0x80000004).  Reverse-engineered; matches dmg2img/adc.c
// and Aaru.  Decode-only (creating ADC streams is out of scope).

#ifndef GS_ADC_H
#define GS_ADC_H

#include <stddef.h>
#include <stdint.h>

// Decompress an ADC stream.  Reads up to `in_len` bytes from `in`, writes up
// to `out_cap` bytes to `out`, stopping when the input is exhausted or the
// output buffer is full.  Returns the number of output bytes written, or -1
// on a malformed stream (a back-reference pointing before the start of the
// decoded output).
long adc_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap);

#endif // GS_ADC_H
