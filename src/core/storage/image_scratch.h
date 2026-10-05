// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_scratch.h
// The scratch root: where the emulator keeps files nobody asked for by name
// -- the ghost delta and journal of a read-only mount or a blank disk, the
// chunk cache's spill files, and the decode sinks of large archive members.
// Decoded images are never written here: an NDIF, UDIF or archived image is
// read through its source (source.h).

#ifndef IMAGE_SCRATCH_H
#define IMAGE_SCRATCH_H

// Root of every scratch file: GS_STORAGE_CACHE when set (the integration
// runner points it at a per-test directory so no test writes beside shared
// media and tests can run in parallel), else /tmp/gs-image-ro.
const char *image_scratch_dir(void);

#endif // IMAGE_SCRATCH_H
