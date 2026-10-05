// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// monitor_catalog.h
// The one catalogue of monitors every display device names its monitors from.
//
// Each device used to name monitors its own way -- "13in_rgb", "hires",
// "rgb_640x480", "gc_640x480", "14in_rgb" for what is one Apple display --
// with labels in seven formats.  A device's own table keeps its private
// token (the sense strap, the sister sResource) and points at a catalogue id
// here; everything a user sees comes from this table, and the ids are shared
// so a configuration can move a monitor from one device to another.

#ifndef GS_PERIPHERALS_MONITOR_CATALOG_H
#define GS_PERIPHERALS_MONITOR_CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One catalogue entry.  The array ends at the entry whose id is NULL.
typedef struct monitor_catalog_entry {
    const char *id; // "13in_rgb"
    const char *label; // "13″ AppleColor High-Resolution RGB"
    uint32_t width; // native geometry; 0 for a multiple-scan display or a TV
    uint32_t height;
    bool grays; // a monochrome display: depths read "grays", not "colors"
} monitor_catalog_entry_t;

// The id every display port reports when nothing is plugged in.  It is never
// offered in a monitor picker: an unplugged monitor is expressed by
// connecting it elsewhere.
#define MONITOR_NONE "none"

// The entry for `id`, or NULL.
const monitor_catalog_entry_t *monitor_catalog_find(const char *id);

// Every entry, in catalogue order.
const monitor_catalog_entry_t *monitor_catalog_all(void);

// Write a video mode's label -- "832 × 624, Thousands of colors" (Apple's
// depth words; grays on a monochrome monitor) -- into `buf`.
void monitor_mode_label(const monitor_catalog_entry_t *m, uint32_t width, uint32_t height, int depth, char *buf,
                        size_t len);

#endif // GS_PERIPHERALS_MONITOR_CATALOG_H
