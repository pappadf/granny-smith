// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// build_id.c
// Compile-time build identification string.
// This file is force-recompiled on every build so __DATE__ and __TIME__ stay current.

#include "build_id.h"

#include <assert.h>

// Build ID string set at compile time ("Mmm DD YYYY HH:MM:SS")
static const char g_build_id[] = __DATE__ " " __TIME__;

// Pin format invariants: 20-character payload + 1 NUL terminator. checkpoint.c reads/writes exactly BUILD_ID_LEN bytes.
_Static_assert(sizeof(g_build_id) == BUILD_ID_LEN + 1, "build id payload must be exactly BUILD_ID_LEN bytes");
_Static_assert(sizeof(g_build_id) - 1 == BUILD_ID_LEN, "BUILD_ID_LEN must match __DATE__ \" \" __TIME__ length");

// Returns the build ID string
const char *get_build_id(void) {
    return g_build_id;
}
