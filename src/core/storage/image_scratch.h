// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_scratch.h
// The cache of derived images: a UDIF or NDIF image decoded to a raw file,
// or a disk image nested inside a mounted volume copied out to one.  Each is
// cached as a scratch file so repeated opens reuse the decode, and each
// cached file is reused only if it provably came from the same source.
//
// A source is described by an identity string that changes whenever the
// source does -- its canonical path, size and mtime, say.  The scratch file
// is named from a 64-bit hash of that identity, and a sidecar
// "<scratch>.id" holding the identity itself is written only once the file
// is complete.  Reuse compares the whole identity, so a name collision can
// only cost a re-decode, and an interrupted decode (which may have left a
// full-size file of zeros) is never mistaken for a finished one
// (09-storage F-33).
//
// Use:
//   image_scratch_path(tag, identity, path, sizeof(path));
//   if (image_scratch_valid(path, identity, size)) -> reuse path
//   image_scratch_prepare(path);  write path;  image_scratch_seal(path, identity)
//   (on a failed write: remove(path))

#ifndef IMAGE_SCRATCH_H
#define IMAGE_SCRATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Root of every scratch sidecar: GS_STORAGE_CACHE when set (the integration
// runner points it at a per-test directory so no test writes beside shared
// media and tests can run in parallel), else /tmp/gs-image-ro.
const char *image_scratch_dir(void);

// "<image_scratch_dir()>/<tag>-<16 hex>.img" for `identity`.  `tag` may
// contain a '/' to place the file in a subdirectory (e.g. "nested/img").
// Returns false if it does not fit in `cap`.
bool image_scratch_path(const char *tag, const char *identity, char *out, size_t cap);

// True if `scratch` is a complete decode of `identity`: its sidecar holds
// exactly `identity`, and the file is `size` bytes (any size when 0).
bool image_scratch_valid(const char *scratch, const char *identity, uint64_t size);

// Before (re)writing `scratch`: create its directory and drop any sidecar,
// so the file is not trusted until sealed again.  0 or a negative errno.
int image_scratch_prepare(const char *scratch);

// Mark `scratch` as a complete decode of `identity`.  0 or a negative errno.
int image_scratch_seal(const char *scratch, const char *identity);

#endif // IMAGE_SCRATCH_H
