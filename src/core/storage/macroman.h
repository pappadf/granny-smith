// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// macroman.h
// MacRoman <-> UTF-8 transcoder.  Lives in its own translation unit so
// any future consumers (resource-fork parser, HFS+ catalog walker, ...)
// can share one table without forcing image_hfs.c to expose its
// internals.  The table covers 0x80..0xFF; bytes < 0x80 round-trip as
// plain ASCII.

#pragma once

#ifndef GS_MACROMAN_H
#define GS_MACROMAN_H

#include <stddef.h>
#include <stdint.h>

// Transcode `src_len` bytes of MacRoman text into UTF-8 written to `dst`
// of capacity `dst_cap`.  Always NUL-terminates (when dst_cap > 0).
// Unsupported bytes never appear because the table covers all 256 values.
void macroman_to_utf8(const uint8_t *src, size_t src_len, char *dst, size_t dst_cap);

// The inverse: transcode the NUL-terminated UTF-8 string `utf8` into
// MacRoman bytes in `dst` (no NUL added).  Returns the number of bytes
// written, or -EINVAL if `utf8` is not valid UTF-8 (1-3 byte sequences), has
// a character MacRoman cannot represent, or needs more than `dst_cap` bytes.
int macroman_from_utf8(const char *utf8, uint8_t *dst, size_t dst_cap);

#endif // GS_MACROMAN_H
