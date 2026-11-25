// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_capture.h
// The bit-level capture behind an image: a MOOF's tracks, kept beside the
// sector view the image is for everyone else.
//
// A MOOF opens as a sector image (image_moof.h): the delta, the VFS, probes,
// block exports and the sector-level controllers see 512-byte sectors and
// their tags.  The format registry also hands the MOOF source itself to the
// image (format_unwrapped_t.capture), which holds the reference for its
// lifetime and parses it here on first use -- the IWM / SWIM track builder
// asking for a track, a controller asking whether a sector was readable, a
// .moof export.  A VFS mount, a probe or an image that never reaches a GCR
// drive holds only the reference and pays nothing.
//
// Once built the store is immutable: the file's bytes, its index, every
// flux track already converted to bits, and what the capture held for each
// sector (moof_scan_track: decoded, missing, or a bad data checksum).  It is
// built on the emulator thread (image_capture); a .moof export takes a
// reference there and reads it on the I/O worker.

#ifndef GS_IMAGE_CAPTURE_H
#define GS_IMAGE_CAPTURE_H

#include "image.h"
#include "image_moof.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct image_capture image_capture_t;

// What the capture held for a sector.
typedef enum {
    IMAGE_SECTOR_OK = MOOF_SECTOR_OK, // decoded, checksum good (or the image has no capture)
    IMAGE_SECTOR_MISSING = MOOF_SECTOR_MISSING, // no data field that decodes: the sector view holds zeros
    IMAGE_SECTOR_BAD = MOOF_SECTOR_BAD, // the data checksum failed: the sector view holds it as read
} image_sector_status_t;

// True when the image was opened from a bit-level capture (and the capture
// has not failed to parse).  Builds nothing.
bool image_has_capture(const image_t *img);

// The image's capture store, built on the first call; NULL for an image
// with none (or out of memory, logged once, after which the image is served
// as a plain sector image).  Emulator thread only.
image_capture_t *image_capture(image_t *img);

// The captured bits of (track, side): MSB first, looping at *nbits.  False
// for a track the capture does not hold (a blank TMAP entry, side 1 of a
// 400K disk) and for any image with no capture.
bool image_track_bits(image_t *img, int track, int side, const uint8_t **bits, uint32_t *nbits);

// What the capture held for a block (a sector, in image order).
// IMAGE_SECTOR_OK for any image with no capture.
image_sector_status_t image_sector_status(image_t *img, uint64_t block);

// The same over a store.
image_sector_status_t image_capture_sector_status(const image_capture_t *cap, uint64_t block);
bool image_capture_bits(const image_capture_t *cap, int track, int side, const uint8_t **bits, uint32_t *nbits);

// Sides the captured disk has: 1 (400K) or 2 (800K).
int image_capture_sides(const image_capture_t *cap);

// The capture's index (its file's chunks), for an export that copies
// tracks through.  Valid while the store is.
const moof_index_t *image_capture_index(const image_capture_t *cap);

// A fingerprint of the capture file: its header CRC-32 (MOOF bytes 8-11),
// or the CRC-32 computed over the file when the header leaves it 0.  What a
// checkpoint records, to know it reopened the same capture.
uint32_t image_capture_crc(const image_capture_t *cap);

// The most disk bytes a captured track can frame to: INFO's Largest Track
// (in 512-byte blocks of bits) as bytes, or the longest track's when INFO
// leaves it 0.  One disk byte takes at least eight cells, so a revolution
// never frames to more.
size_t image_capture_max_track_bytes(const image_capture_t *cap);

// One steady-state revolution of (track, side), malloc'd into *nib, and the
// sector fields on it (moof_scan_track).  The byte count; 0 with *nib NULL
// for a track the capture does not hold (every sector then MISSING, with no
// address field).
size_t image_capture_track(const image_capture_t *cap, int track, int side, uint8_t **nib,
                           moof_sector_t sec[MOOF_MAX_SECTORS]);

// Which of a captured track's `nsec` sectors the image no longer holds as
// the capture does: bit k set for sector k.  `data` / `tags` are the
// image's sectors of the track (nsec x 512 and nsec x 12 bytes), `in_delta`
// has bit k set for each sector in the image's delta.  A sector that
// decoded differs when its data or tag does; one the capture could not
// read (missing, bad) differs when the image no longer holds what the
// importer gave it (zeros; the bytes as read) or the guest has written it:
// on the real disk such a write leaves a readable sector, whatever its
// contents.
uint32_t image_capture_track_diff(const moof_sector_t sec[MOOF_MAX_SECTORS], int nsec, const uint8_t *data,
                                  const uint8_t *tags, uint32_t in_delta);

// A second reference to a store (an export reading it off the emulator
// thread), and its release.  The image holds the first.
image_capture_t *image_capture_retain(image_capture_t *cap);
void image_capture_release(image_capture_t *cap);

#endif // GS_IMAGE_CAPTURE_H
