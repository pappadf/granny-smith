// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// macroman.h
// MacRoman -> UTF-8 transcoder.  Lives in its own translation unit so
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

#endif // GS_MACROMAN_H
