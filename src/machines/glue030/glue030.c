// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// glue030.c
// Family-shared lifecycle helpers for the SE/30 / IIx / IIcx triplet.  See
// glue030.h for the contract; this file holds the actual implementations.

#include "glue030.h"
#include "system_config.h" // full config_t (image list, scheduler, etc.)

#include "log.h"

LOG_USE_CATEGORY_NAME("glue030");

// Step-2 stub.  Subsequent steps move the SE/30 init body here and replace
// the per-machine init with a thin "fill spec, call glue030_init" wrapper.
// (The image-list checkpoint helpers that used to live here moved to
// runtime/checkpoint_images.c — they were never GLUE-specific.)
int glue030_init(const glue030_init_t *spec) {
    if (!spec || !spec->cfg)
        return -1;
    return 0;
}

// Step-2 stub.  The full body lands alongside glue030_init.
void glue030_teardown(config_t *cfg) {
    (void)cfg;
}
