// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_moof.h
// Applesauce MOOF floppy images (MOOF Disk Image Reference 1.0): reader and
// writer.
//
// A MOOF file is a bit-level capture of a Macintosh 3.5" floppy: per track
// and side, the raw bitstream the drive head saw (or, rarely, its flux
// transitions).  The emulator's floppy controllers work on sectors, so a
// MOOF is opened by decoding every track to its Apple 6-and-2 GCR sectors
// -- the 512 data bytes and the 12 tag bytes of each -- and presenting the
// result as a DiskCopy 4.2 image in memory.  The format registry's dc42
// wrapper then takes it like any other DiskCopy file, so the tags reach the
// image and a MOOF mounts, boots and browses (VFS) the same as a .image.
//
// Only what decodes to sectors survives: a copy-protection scheme that
// relies on non-standard nibbles, bit timing or weak bits between the
// sectors is not reproduced.  400K and 800K GCR disks decode; 1.44 MB MFM
// and Twiggy MOOFs are refused (-ENOTSUP).
//
// The writer goes the other way: sectors (and tags) are laid down as a
// Mac-formatted GCR track -- 2:1 interleave, self-sync gaps -- and stored
// as a bitstream MOOF.  See docs/reference/formats/moof.md.

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

// The format registry's adapter: `file` decoded and served as a DiskCopy
// 4.2 image in memory (data section, then the tag section).  A new
// reference, or NULL with *err (a negative errno; may be NULL).
source_t *moof_source_open(source_t *file, int *err);

#endif // GS_IMAGE_MOOF_H
