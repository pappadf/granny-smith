// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// build_id.c
// Compile-time build identification string.
// This file is force-recompiled on every build so __DATE__ and __TIME__ stay current.

#include "build_id.h"

// Build ID string set at compile time ("Mmm DD YYYY HH:MM:SS")
static const char g_build_id[] = __DATE__ " " __TIME__;

// Returns the build ID string
const char *get_build_id(void) {
    return g_build_id;
}
