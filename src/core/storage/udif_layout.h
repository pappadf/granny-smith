// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// udif_layout.h
// The on-disk layout of a UDIF image's 'koly' trailer and 'mish' block
// table: every field offset, once, for the reader (image_udif.c) and the
// writer (udif_writer.c).  Private to the storage layer.  All fields are
// big-endian.

#ifndef GS_UDIF_LAYOUT_H
#define GS_UDIF_LAYOUT_H

#include "image_udif.h"

// 'koly' trailer field offsets.  The 128-byte checksum blob at 0x58 is why
// XMLOffset lands at 0xD8 and not, as a naive count of the struct members
// suggests, earlier.
#define KOLY_OFF_SIGNATURE     0x00
#define KOLY_OFF_VERSION       0x04
#define KOLY_OFF_HEADER_SIZE   0x08
#define KOLY_OFF_FLAGS         0x0C
#define KOLY_OFF_RUNNING       0x10
#define KOLY_OFF_DATA_OFFSET   0x18
#define KOLY_OFF_DATA_LENGTH   0x20
#define KOLY_OFF_RSRC_OFFSET   0x28
#define KOLY_OFF_RSRC_LENGTH   0x30
#define KOLY_OFF_SEGMENT_NUM   0x38
#define KOLY_OFF_SEGMENT_COUNT 0x3C
#define KOLY_OFF_SEGMENT_ID    0x40 // 16-byte UUID
#define KOLY_OFF_CHECKSUM_TYPE 0x50 // data-fork checksum
#define KOLY_OFF_CHECKSUM_BITS 0x54
#define KOLY_OFF_CHECKSUM      0x58
#define KOLY_OFF_XML_OFFSET    0xD8
#define KOLY_OFF_XML_LENGTH    0xE0
#define KOLY_OFF_MASTER_TYPE   0x160 // master checksum over the block tables'
#define KOLY_OFF_MASTER_BITS   0x164
#define KOLY_OFF_MASTER        0x168
#define KOLY_OFF_VARIANT       0x1E8
#define KOLY_OFF_SECTORS       0x1EC

// The trailer is exactly 512 bytes, so a mistyped offset is a read past its
// end — catch that at compile time instead of at decode time.
_Static_assert(KOLY_OFF_XML_LENGTH + 8 <= UDIF_TRAILER_SIZE, "koly offset outside the trailer");
_Static_assert(KOLY_OFF_SECTORS + 8 <= UDIF_TRAILER_SIZE, "koly offset outside the trailer");

// 'mish' block-table field offsets.
#define MISH_OFF_SIGNATURE     0x00
#define MISH_OFF_VERSION       0x04
#define MISH_OFF_SECTOR        0x08
#define MISH_OFF_SECTOR_COUNT  0x10
#define MISH_OFF_DATA_OFFSET   0x18
#define MISH_OFF_BUFFERS       0x20 // sectors of decode buffer a reader needs
#define MISH_OFF_DESCRIPTOR    0x24 // the blkx resource ID
#define MISH_OFF_CHECKSUM_TYPE 0x40
#define MISH_OFF_CHECKSUM_BITS 0x44
#define MISH_OFF_CHECKSUM      0x48
#define MISH_OFF_CHUNK_COUNT   0xC8
#define MISH_HEADER_SIZE       0xCC // chunk entries start here
#define MISH_ENTRY_SIZE        40

// Field offsets within one 40-byte chunk entry.
#define MISH_ENTRY_OFF_TYPE   0x00
#define MISH_ENTRY_OFF_SECTOR 0x08
#define MISH_ENTRY_OFF_COUNT  0x10
#define MISH_ENTRY_OFF_OFFSET 0x18
#define MISH_ENTRY_OFF_LENGTH 0x20

// Only version 4 trailers and version 1 block tables were ever written.
#define KOLY_VERSION 4
#define MISH_VERSION 1

#define KOLY_SIGNATURE_STR "koly"
#define MISH_SIGNATURE     0x6D697368u // 'mish'

#endif // GS_UDIF_LAYOUT_H
