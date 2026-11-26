// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_moof.h
// Applesauce MOOF floppy images (MOOF Disk Image Reference 1.0): reader and
// writer.
//
// A MOOF file is a bit-level capture of a Macintosh 3.5" floppy: per track
// and side, the raw bitstream the drive head saw (or, rarely, its flux
// transitions).  It is opened two ways at once.
//
// The sector view: every track is framed as the IWM frames it in steady
// state (gcr_frame_revolution) and decoded to its Apple 6-and-2 GCR sectors
// -- the 512 data bytes and the 12 tag bytes of each -- and the result is
// presented as a DiskCopy 4.2 image in memory.  The format registry's dc42
// wrapper then takes it like any other DiskCopy file, so the tags reach the
// image and a MOOF mounts, boots and browses (VFS) the same as a .image.
// The delta, the VFS, probes, block exports and the sector-level floppy
// controllers see only this.
//
// The capture: the format registry also hands the MOOF source itself to the
// image (format_unwrapped_t.capture), which keeps it and parses it on
// demand (image_capture.h).  The IWM / SWIM GCR read path serves each
// track's captured nibbles from it while the image's sectors still hold
// what the capture decodes to, so what lies between and around the sectors
// -- non-standard marks, fields that are not sectors, exact sync counts --
// reaches the guest (floppy_media_track, floppy_gcr.c).  Weak bits -- long
// runs with no flux transition, which a drive reads as noise -- read as
// fresh, reproducible noise on every revolution on that path
// (image_capture_weak_runs).  What depends on when the bits arrive (bit
// timing) is not reproduced.  400K and
// 800K GCR disks decode; 1.44 MB MFM and Twiggy MOOFs are refused
// (-ENOTSUP).
//
// The writer goes the other way: sectors (and tags) are laid down as a
// Mac-formatted GCR track -- 2:1 interleave, self-sync gaps -- and stored
// as a bitstream MOOF; an export of a captured image copies the captured
// bits of every track whose sectors are unchanged instead
// (moof_encode_with).  See docs/reference/formats/moof.md.

#ifndef GS_IMAGE_MOOF_H
#define GS_IMAGE_MOOF_H

#include "source.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MOOF_HEADER_SIZE 12 // "MOOF" FF 0A 0D 0A + CRC32
#define MOOF_TAG_BYTES   12 // tag bytes per GCR sector

// MOOF disk types (INFO +1).
#define MOOF_DISK_SSDD_GCR 1 // 400K
#define MOOF_DISK_DSDD_GCR 2 // 800K
#define MOOF_DISK_DSHD_MFM 3 // 1.44M
#define MOOF_DISK_TWIGGY   4

// Largest MOOF file read (a 160-track capture is ~1.3 MB; flux ones more).
#define MOOF_MAX_FILE (32u * 1024u * 1024u)

// A decoded MOOF: its sectors in DiskCopy order (track, then side, then
// sector), each sector's tag, and what the decode found.
typedef struct {
    uint8_t disk_type; // MOOF_DISK_*
    uint8_t format_byte; // the address fields' format nibble ($22 Mac 800K, $02/$12 400K)
    bool write_protected; // INFO's write-protect flag
    uint32_t sectors; // 800 (400K) or 1600 (800K)
    uint8_t *data; // sectors * 512 bytes
    uint8_t *tags; // sectors * MOOF_TAG_BYTES bytes
    uint32_t missing; // sectors no track held (zero-filled)
    uint32_t bad_checksum; // sectors whose data checksum failed (kept as read)
    uint32_t flux_tracks; // tracks decoded from flux rather than a bitstream
    bool crc_mismatch; // the header CRC was given and did not match
} moof_disk_t;

// Sectors on the longest (outermost) GCR track.
#define MOOF_MAX_SECTORS 12

// Tracks (80) times sides (2): the TMAP's entries.
#define MOOF_TRACK_SLOTS 160

// A MOOF file's chunks and fields, located (moof_index).  Pointers are into
// the file, which must outlive the index.
typedef struct {
    const uint8_t *file;
    size_t len;
    uint8_t disk_type; // MOOF_DISK_*
    bool write_protected; // INFO's write-protect flag
    uint8_t timing; // bit cell, in 125 ns ticks (16 = 2 µs)
    int sides; // 1 (400K) or 2 (800K)
    uint32_t largest_track_blocks; // INFO's Largest Track, in 512-byte blocks
    bool crc_mismatch; // the header CRC was given and did not match
    const uint8_t *info; // INFO chunk data (60 bytes)
    const uint8_t *tmap; // TMAP (160 bytes)
    const uint8_t *trks; // TRKS: the 160 TRK records, then track data
    const uint8_t *flux_map; // FLUX map, or NULL when the file has none
    const uint8_t *meta; // META text, or NULL
    size_t meta_len;
} moof_index_t;

// Locate a MOOF file's chunks.  0, -EINVAL for a file that is not a
// well-formed MOOF, -ENOTSUP for a disk type other than 400K / 800K GCR
// (ix->disk_type is still set).
int moof_index(const uint8_t *file, size_t len, moof_index_t *ix);

// The captured bits of (track, side), MSB first, looping at the returned
// count; 0 for a track the file does not hold (a blank TMAP entry, a side
// the disk does not have, a record outside the file).  *bits points into the
// file, or for a flux track into *owned, malloc'd here and the caller's to
// free.
uint32_t moof_track_bits(const moof_index_t *ix, int track, int side, const uint8_t **bits, uint8_t **owned);

// True when (track, side) is a flux track (the FLUX map names it).
bool moof_track_is_flux(const moof_index_t *ix, int track, int side);

// One steady-state revolution of a captured track's disk bytes
// (gcr_frame_revolution), malloc'd into *out.  The count, 0 (and *out NULL)
// for no bits or no memory.
size_t moof_frame_track(const uint8_t *bits, uint32_t nbits, uint8_t **out);

// What a captured track holds for one sector.
#define MOOF_SECTOR_OK      0 // a data field that decodes, checksum good
#define MOOF_SECTOR_MISSING 1 // no data field that decodes (none, or not GCR)
#define MOOF_SECTOR_BAD     2 // a data field whose checksum fails (kept as read)
#define MOOF_NO_FIELD       UINT32_MAX // no data mark after the address field

// One sector's fields on a captured revolution.  Positions are nibble
// indexes into the revolution; data_at and data_end can run past its end,
// where the field wraps round the index.
typedef struct {
    uint8_t state; // MOOF_SECTOR_*
    bool addr; // an address field for this sector was found
    uint32_t addr_at; // its D5 AA 96, in [0, n)
    uint32_t data_at; // the D5 AA AD after it, or MOOF_NO_FIELD
    uint32_t data_end; // one past the DE AA closing the data field, or MOOF_NO_FIELD
    uint8_t tag[MOOF_TAG_BYTES]; // decoded (as read when BAD; zeros when MISSING)
    uint8_t data[512];
} moof_sector_t;

// Find the sectors of (track, side) on a revolution `rev` of `n` disk bytes
// that loops (a field may straddle the index).  For each sector, the first
// copy that decodes wins, else the first with a bad checksum, else the
// first address field seen.  The address fields' format byte goes to
// *format_byte when that is still 0 (may be NULL).  A data field ends at
// the DE AA closing it, and never past the next field's mark.  The one
// sector scan the importer (moof_decode), the capture store, the IWM's track
// builder and its eject flush share, so they agree on what every sector is.
void moof_scan_track(const uint8_t *rev, size_t n, int track, int side, moof_sector_t sec[MOOF_MAX_SECTORS],
                     uint8_t *format_byte);

// True when `head` starts with the MOOF signature.
bool moof_detect(const uint8_t *head, size_t len);

// Decode a whole MOOF file.  0 and *out filled (free with moof_disk_free),
// or a negative errno: -EINVAL for a file that is not a well-formed MOOF or
// holds no readable sector, -ENOTSUP for a disk type this does not decode.
int moof_decode(const uint8_t *file, size_t len, moof_disk_t *out);
void moof_disk_free(moof_disk_t *d);

// Encode `sectors` sectors (800 or 1600) of `data` and their tags (NULL for
// zeros) as a bitstream MOOF.  `format_byte` 0 picks the Mac's ($02 for
// 400K, $22 for 800K).  0 and a malloc'd file in *out / *out_len, or a
// negative errno.
int moof_encode(const uint8_t *data, uint32_t sectors, const uint8_t *tags, uint8_t format_byte, uint8_t **out,
                size_t *out_len);

// moof_encode, but every (track, side) whose bit is set in `keep` (index
// track * 2 + side) is copied from the capture `src` as it is -- its bits,
// a flux track's converted to a bitstream; a blank entry stays blank -- and
// the rest are laid down from the sectors.  INFO is `src`'s (write protect,
// cross-track sync, bit timing) with this writer as creator, the largest
// track recomputed and no FLUX; `src`'s META follows the tracks.  With
// `src` NULL it is moof_encode.
int moof_encode_with(const uint8_t *data, uint32_t sectors, const uint8_t *tags, uint8_t format_byte,
                     const moof_index_t *src, const uint8_t keep[MOOF_TRACK_SLOTS / 8], uint8_t **out, size_t *out_len);

// The format registry's adapter: `file` decoded and served as a DiskCopy
// 4.2 image in memory (data section, then the tag section).  A new
// reference, or NULL with *err (a negative errno; may be NULL).
source_t *moof_source_open(source_t *file, int *err);

// True when the MOOF `file`'s INFO sets the write-protect flag: the
// original disk's tab was closed.  Reads only the first chunk (INFO is
// always first); false for anything that is not a MOOF.
bool moof_source_write_protected(source_t *file);

#endif // GS_IMAGE_MOOF_H
