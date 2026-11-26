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
#include <stddef.h>
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

// Frame a looped bitstream (MSB first, `nbits` cells, the track looping at
// the end) into disk bytes the way the IWM does in steady state: shift bits
// in, a byte completes once its top bit is set, and the shift register
// carries across the index.  One revolution is pre-rolled and the second is
// kept, so the bytes are what the head delivers on every pass once the
// framer has synced -- framing cold from the first cell can differ.  A byte
// that straddles the index is the revolution's first (it completes there).
// Returns the byte count; at most `cap`, which nbits / 8 + 2 always covers
// (0 for an empty track or no room).
size_t gcr_frame_revolution(const uint8_t *bits, uint32_t nbits, uint8_t *out, size_t cap);

// ============================================================================
// Weak bits
// ============================================================================
//
// A long run of cells with no flux transition reads differently on every
// revolution of a real drive: the read amplifier's gain climbs while it
// hears nothing, and once a few cells have passed in silence it starts
// turning noise into transitions -- at random, so the IWM's framing after
// the run slips differently each time (a nibble copier writes the run back
// as real data, which reads the same every time).  The model: inside a run, a cell more than GCR_WEAK_GRACE
// cells after the last transition (captured or invented) becomes a 1 with
// probability GCR_WEAK_P_NUM / GCR_WEAK_P_DEN.

// Silent cells the amplifier lets pass before it invents a transition: a
// run of up to three zeros is legal GCR.
#define GCR_WEAK_GRACE 3
// The chance per cell after that, as a fraction (30 %).
#define GCR_WEAK_P_NUM 3
#define GCR_WEAK_P_DEN 10

// A run of cells with no flux transition on a looping track: `len` cells
// from cell `start`.  A run that crosses the index has start + len > nbits
// and continues at cell 0.
typedef struct {
    uint32_t start;
    uint32_t len;
} gcr_weak_run_t;

// The zero runs of at least `min_len` cells on a looping bitstream, in
// order of `start`, the run across the index (if any) found once.  Writes
// at most `cap` (runs may be NULL with cap 0) and returns how many there
// are, which may be more.
size_t gcr_zero_runs(const uint8_t *bits, uint32_t nbits, uint32_t min_len, gcr_weak_run_t *runs, size_t cap);

// gcr_frame_revolution over the bits with every run in `runs` filled with
// noise as the model above describes, drawn from `seed`: the same seed
// gives the same bytes.  The pre-rolled revolution is filled from
// `pre_seed` -- the revolution the head passed over just before -- so the
// framer reaches the index in the state that revolution left it in, and
// a read that runs on across the index sees one unbroken stream.  With no
// runs it is gcr_frame_revolution.  Returns the byte count, 0 for an
// empty track, no room or no memory.
size_t gcr_frame_revolution_noisy(const uint8_t *bits, uint32_t nbits, const gcr_weak_run_t *runs, size_t nruns,
                                  uint64_t pre_seed, uint64_t seed, uint8_t *out, size_t cap);

// The bits of a revolution with the runs filled from `seed`, as
// gcr_frame_revolution_noisy frames them: `out` holds (nbits + 7) / 8
// bytes.  For tests and inspection.
void gcr_weak_fill(const uint8_t *bits, uint32_t nbits, const gcr_weak_run_t *runs, size_t nruns, uint64_t seed,
                   uint8_t *out);

#endif // GCR_H
