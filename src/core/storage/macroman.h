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

#include <stdbool.h>
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

// === Mac file names on a host filesystem ===================================
//
// One convention for every place a Mac name becomes a host file name and
// back: the name is UTF-8 on the host, and a Mac '/' -- legal in
// an HFS name, a separator in a host path -- is a host ':' (and a host ':',
// illegal in a Mac name, is a Mac '/').  macOS stores Finder names the same
// way, and image_hfs.c exposes HFS names through the VFS the same way.

// The host name for the Mac name `mac` (MacRoman, `len` bytes), NUL-terminated
// in `dst`.  False if the name holds a NUL or a ':' (neither is legal in a Mac
// name) or does not fit in `dst_cap`.
bool macroman_name_to_host(const uint8_t *mac, size_t len, char *dst, size_t dst_cap);

// The Mac name for host name `host`, in at most `dst_cap` bytes (no NUL
// added); its length, or -1 if it has a character MacRoman cannot hold or does
// not fit.  Accents stored decomposed (NFD, as macOS often stores names) are
// composed first.  A host name that is not valid UTF-8 is taken as raw MacRoman
// -- how names written before the server transcoded look.
int macroman_name_from_host(const char *host, uint8_t *dst, size_t dst_cap);

#endif // GS_MACROMAN_H
