// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_scratch.c
// The scratch root.  See image_scratch.h.

#include "image_scratch.h"

#include <stdlib.h>

#define IMAGE_SCRATCH_DEFAULT_DIR "/tmp/gs-image-ro"

const char *image_scratch_dir(void) {
    const char *cache = getenv("GS_STORAGE_CACHE");
    return (cache && *cache) ? cache : IMAGE_SCRATCH_DEFAULT_DIR;
}
