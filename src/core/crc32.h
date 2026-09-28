// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// crc32.h
// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320): the one zlib, PNG and
// UDIF use.  One table-driven implementation for the whole tree -- the disk
// image decoders, the PNG writer, the PCI option-ROM identity and the AFP
// append logs all fold through it.

#ifndef GS_CRC32_H
#define GS_CRC32_H

#include <stddef.h>
#include <stdint.h>

// Fold `len` bytes into a running CRC.  Start from 0; a result seeds the next
// call, so gs_crc32(gs_crc32(0, a, n), b, m) is the CRC of a followed by b.
uint32_t gs_crc32(uint32_t crc, const void *data, size_t len);

#endif // GS_CRC32_H
