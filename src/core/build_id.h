// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// build_id.h
// Build identification derived from compile-time date and time.

#ifndef BUILD_ID_H
#define BUILD_ID_H

// Fixed length of the build ID string (excluding null terminator).
// Format: "Mmm DD YYYY HH:MM:SS" — exactly 20 characters.
#define BUILD_ID_LEN 20

// Returns the build ID string (defined in build_id.c, recompiled every build)
const char *build_id_get(void);

#endif // BUILD_ID_H
