// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// drive_catalog.h
// Single source of truth for known SCSI hard disk drive models.
// Used by the shell (hd create/validate/models), SCSI attachment,
// and queried by the web frontend via `hd models`.

#pragma once

#ifndef DRIVE_CATALOG_H
#define DRIVE_CATALOG_H

#include <stddef.h>

// a known SCSI hard disk model
struct drive_model {
    const char *label; // human-friendly name, e.g. "HD20SC"
    const char *vendor; // SCSI vendor string, e.g. " SEAGATE"
    const char *product; // SCSI product string, e.g. "ST225N"
    size_t size; // exact image size in bytes
};

// return the number of known drive models
int drive_catalog_count(void);

// return the drive model at index i (0-based)
const struct drive_model *drive_catalog_get(int i);

// find the closest model whose size >= the given size.
// falls back to the largest model if size exceeds all entries.
const struct drive_model *drive_catalog_find_closest(size_t size);

// parse a human-friendly size string into exact model bytes.
//   "20mb" / "40mb"     -> closest model with size >= N megabytes
//   "HD20SC" / "hd20sc" -> exact model lookup by label
//   "20M" / "512K"      -> exact binary power (1M = 1048576)
//   "21411840"          -> raw byte count (pass-through)
// returns the resolved size in bytes, or 0 on parse error.
size_t drive_catalog_parse_size(const char *str);

#endif // DRIVE_CATALOG_H
