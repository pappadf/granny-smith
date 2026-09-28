// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// drive_activity.h
// The status bar's HD / FD / CD lights, derived from the per-image I/O
// counters disk_read_data / disk_write_data keep (image.h reads, writes).
// The hot path does one increment per call and nothing else; a host samples
// the per-kind sums once per tick, and this turns them into a light state
// that stays on for a minimum visible time and changes only on an edge --
// so the page hears about a light when it changes, not per block.

#ifndef DRIVE_ACTIVITY_H
#define DRIVE_ACTIVITY_H

#include <stdbool.h>
#include <stdint.h>

// Which light.  Values cross to JS (Module.onDriveActivity) as ints.
typedef enum { DRIVE_KIND_HD = 0, DRIVE_KIND_FD = 1, DRIVE_KIND_CD = 2, DRIVE_KIND_COUNT } drive_kind_t;

typedef enum {
    DRIVE_LIGHT_IDLE = 0,
    DRIVE_LIGHT_READ = 1,
    DRIVE_LIGHT_WRITE = 2,
} drive_light_t;

// A light stays on at least this long after the last transfer, so a single
// block is visible (and a stream of them is one steady light).
#define DRIVE_LIGHT_MIN_MS 100.0

typedef struct drive_activity {
    bool primed; // a first sample has set the baselines
    uint64_t reads[DRIVE_KIND_COUNT]; // the sums at the last sample
    uint64_t writes[DRIVE_KIND_COUNT];
    drive_light_t light[DRIVE_KIND_COUNT];
    double lit_until_ms[DRIVE_KIND_COUNT]; // on until then (any transfer)
    double write_until_ms[DRIVE_KIND_COUNT]; // "write" until then
} drive_activity_t;

// Advance the lights to `now_ms` given the current per-kind counter sums.
// Returns a bitmask (1 << kind) of the lights that changed.  The first call
// only sets the baselines; a sum that went DOWN (an image was closed) is a
// new baseline, not activity.  Writing outranks reading while both happen.
unsigned drive_activity_update(drive_activity_t *a, const uint64_t reads[DRIVE_KIND_COUNT],
                               const uint64_t writes[DRIVE_KIND_COUNT], double now_ms);

#endif // DRIVE_ACTIVITY_H
