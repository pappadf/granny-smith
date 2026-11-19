// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gcr.h
// Apple's 6-and-2 GCR, the encoding of a 400K / 800K Macintosh floppy: the
// 64 disk-byte codewords, a sector's 12 tag + 512 data bytes as 703 six-bit
// values (699 data values and the 24-bit rotate-add-xor checksum), and the
// address field.  The one codec under the MOOF reader and writer
// (image_moof.c), the IWM / SWIM track encoder and decoder (floppy_gcr.c)
// and SWIM3's DMA stream (swim3_xfer.c), which used to carry three copies.

#ifndef GCR_H
#define GCR_H

#include <stdbool.h>
#include <stdint.h>

#define GCR_TAG_BYTES     12
#define GCR_DATA_BYTES    512
#define GCR_SECTOR_VALUES 703 // 699 data + 4 checksum six-bit values
#define GCR_ADDR_VALUES   5 // track, sector, side, format, checksum

// gcr_decode6 holds this for a byte that is not one of the 64 codewords.
#define GCR_BAD_CODEWORD 0xFFu

// Six-bit value -> disk byte.
extern const uint8_t gcr_encode6[64];

// Disk byte -> six-bit value, or GCR_BAD_CODEWORD.
extern const uint8_t gcr_decode6[256];

// A sector's tag and data as 703 six-bit values.  `bad_cksum` lays down a
// checksum that does not match the data: what an unreadable sector on a
// real disk gives the controller (the Sony driver's badDCksum).
void gcr_sector_nibblize(const uint8_t tag[GCR_TAG_BYTES], const uint8_t data[GCR_DATA_BYTES], bool bad_cksum,
                         uint8_t out6[GCR_SECTOR_VALUES]);

// 703 six-bit values -> tag and data; true when the checksum the values
// carry matches.  Bits above the low six of each value are ignored.
bool gcr_sector_denibblize(const uint8_t in6[GCR_SECTOR_VALUES], uint8_t tag[GCR_TAG_BYTES],
                           uint8_t data[GCR_DATA_BYTES]);

// 703 disk bytes -> tag and data: -1 when a byte is no codeword (nothing
// is written), 0 when the checksum does not match, 1 when it does.
int gcr_sector_from_disk(const uint8_t disk[GCR_SECTOR_VALUES], uint8_t tag[GCR_TAG_BYTES],
                         uint8_t data[GCR_DATA_BYTES]);

// The address field's five six-bit values: track (low six bits), sector,
// side in bit 5 with track bit 6 below it, format byte, and their XOR.
void gcr_addr_encode(int track, int sector, int side, uint8_t format, uint8_t out6[GCR_ADDR_VALUES]);

// Five disk bytes of an address field -> its fields; false when a byte is
// no codeword or the checksum does not match.  Any output may be NULL.
bool gcr_addr_from_disk(const uint8_t disk[GCR_ADDR_VALUES], int *track, int *sector, int *side, uint8_t *format);

#endif // GCR_H
