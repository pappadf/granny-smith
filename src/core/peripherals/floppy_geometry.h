// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// floppy_geometry.h
// The Sony/SuperDrive media geometry, as one public answer.
//
// Four different rules for "what disk is this" used to live across the
// subsystem (02-floppy F-04/F-17): image_t::type in floppy_gcr.c and floppy.c,
// `disk_size(img) > 1000000` in three places in floppy_swim.c, an exact-size
// switch in swim3_xfer.c, and a format-bitmask derivation in iop_swim.c.  They
// disagreed -- only the third handled 720K -- and adding a geometry meant
// editing five places.
//
// This header is PUBLIC on purpose.  lisa_fdc.c already reached into
// floppy_internal.h for the zone helpers in violation of that header's own
// rule (F-29), which was third-consumer evidence that the geometry belongs in
// an API rather than behind the controller's private types.

#ifndef FLOPPY_GEOMETRY_H
#define FLOPPY_GEOMETRY_H

#include "image.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Sony GCR zones: five speed groups of sixteen tracks, shrinking towards the
// spindle.  These describe the DRIVE, not the medium, so they take a track
// number and nothing else.
#define FLOPPY_NUM_TRACKS   80
#define FLOPPY_NUM_SIDES    2
#define FLOPPY_ZONE_TRACKS  16
#define FLOPPY_SECTOR_BYTES 512

// Sectors on a GCR track: 12 on the outermost zone, one fewer per zone inward.
int floppy_zone_sectors_per_track(int track);
// Spindle speed for a GCR track, in RPM.
int floppy_zone_rpm(int track);
// Approximate encoded length of a GCR track, in bytes.
size_t floppy_zone_track_length(int track);
// Byte offset of a (track, side) in a GCR image laid out by zone.
size_t floppy_zone_image_offset(int track, int side, int num_sides);

// What the medium in the drive is.  Derived once, from the image.
typedef struct floppy_media {
    image_t *img; // the medium itself; NULL when the drive is empty
    bool valid; // false = empty drive, or a size this driver cannot present
    bool mfm; // MFM framing rather than Apple GCR
    bool hd; // high-density media (1440K)
    int sides; // 1 for a 400K disk, 2 otherwise
    int mfm_spt; // sectors per track on MFM media; 0 for GCR (zoned)
    uint8_t fmt_byte; // the sector header's format byte
} floppy_media_t;

// Fills *out from the image.  Returns false (and a zeroed *out) for an empty
// drive or a size that is not a floppy geometry.
bool floppy_media_from_image(image_t *img, floppy_media_t *out);

// Sectors per track for this medium: uniform on MFM, zoned on GCR.
int floppy_media_spt(const floppy_media_t *m, int track);

// Byte offset of one sector in the image.
size_t floppy_media_sector_offset(const floppy_media_t *m, int track, int side, int sector);

// decode_gcr() returns this for a byte that is not one of the 64 legal GCR
// codewords.  It is out of the 6-bit range, so a caller that ignores it still
// cannot silently fold a corrupt nibble into valid data.
#define GCR_BAD_CODEWORD 0xFFu

// The 64-entry 6-to-8 GCR codeword table, and the Apple rotate-add-xor
// checksum chain.  Both existed twice -- the table byte-identically, the chain
// once as dst-capturing macros and once as these functions (02-floppy F-18).
// The chain is subtle (the carry feed between ca/cb/cc, and the final
// pair-not-triplet case), so one implementation with a unit test beats two
// that happen to agree.
extern const uint8_t gcr_codewords[];

// Three bytes -> four six-bit values, advancing the checksum registers.
void gcr_encode_triplet(const uint8_t *src, uint16_t *ca, uint16_t *cb, uint16_t *cc, uint8_t *dst);
// Four six-bit values -> three bytes, advancing the checksum registers.
void gcr_decode_triplet(const uint8_t *src, uint16_t *ca, uint16_t *cb, uint16_t *cc, uint8_t *dst);

// Converts a GCR codeword to its 6-bit value, or GCR_BAD_CODEWORD.
uint8_t decode_gcr(uint8_t gcr_codeword);

// === MFM sector layout ======================================================
//
// One description of the IBM System-34 sector the SuperDrive lays down, for
// every controller.  It was written twice (02-floppy F-20): once in
// floppy_swim.c filling a byte buffer with a parallel mark array, once in
// swim3_xfer.c emitting (clock, data) pairs into a DMA stream.  Identical field
// order and values; they differed only in output SINK -- and, unexplained, in
// gap-3 length and whether CRC bytes were emitted at all.
//
// Those two remain parameters rather than being unified to one value: gap 3 is
// format-dependent and nothing in the SWIM or SWIM3 specs in
// local/gs-docs/library/floppy pins the numbers these two chose, so preserving
// each caller's behaviour is the honest option until a source settles it.
// Recorded as an open question in proposal-floppy-controller-unification.

// Emits one byte of the layout.  `is_mark` marks the $A1/$C2 bytes written
// with a missing clock transition.
typedef void (*floppy_mfm_emit_fn)(void *ctx, uint8_t byte, bool is_mark);

// Lays down one complete MFM sector: sync, address mark, C/H/S/N, CRC, gap 2,
// sync, data mark, 512 data bytes, CRC, gap 3.  `sector` is 1-based, as it
// appears in the header.  `emit_crc` false omits both CRC fields.
void floppy_mfm_emit_sector(floppy_mfm_emit_fn emit, void *ctx, int track, int side, int sector, const uint8_t *data,
                            int gap3_len, bool emit_crc);

#endif // FLOPPY_GEOMETRY_H
