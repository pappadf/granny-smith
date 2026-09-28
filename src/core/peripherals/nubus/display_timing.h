// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// display_timing.h
// The Apple monitor sense codes and the raster each one means.
//
// Every Apple framebuffer from RBV onward decodes a 3-bit code from the
// DB-15 connector's three sense pins to identify the attached monitor
// (docs/core/peripherals/video.md §5.3), and that code means the same thing on
// every part.  It was nevertheless spelled out in four places -- a per-card
// nubus_monitor_t catalogue keyed by sense code, a chip-local switch in Ariel,
// a literal in Civic, and a derivation in DAFB -- so "sense 2 is 512x384" had
// four chances to disagree.  This is the one table.
//
// SCOPE: the PASSIVE 3-bit space only, codes 0..7.  It deliberately does NOT
// cover the extended (tie-matrix) monitors, because the parts that support
// those number them differently: DAFB indexes its extended set from 8 with
// 9 = PAL (dafb.h), while Ariel's Sonora timing-set codes run past 7 in their
// own numbering with 9 = GoldFish (ariel.c).  Those two encodings are not the
// same space, and flattening them into one table would invent an agreement
// that does not exist in the hardware.  Extended codes stay per-part.
//
// Parts that derive geometry from programmed CRTC registers (DAFB, Control,
// Mach64, Cirrus) keep deriving -- that is correct for a programmable timing
// generator.  They can still use this to NAME what they derived.

#ifndef DISPLAY_TIMING_H
#define DISPLAY_TIMING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct display_timing {
    uint8_t sense_code; // 0..7, as read from the three sense pins
    uint32_t width, height; // active raster
    // Vertical refresh in millihertz (66670 = 66.67 Hz).  Integer so the
    // table stays exactly comparable; a producer that paces off the machine's
    // own retrace instead wants scheduler.h's MAC_VBL_PERIOD_NS.
    uint32_t refresh_mhz;
    const char *name;
    // Several sense codes share a raster: NTSC and the 13"/14" RGB both drive
    // 640x480, the two 21" Workstations both 1152x870, the two 15" Portraits
    // both 640x870.  display_timing_name() looks up by SIZE, so exactly one
    // row per geometry may answer for it -- the one a reader would expect to
    // see in a log line.  Marking the others keeps the table in sense-code
    // order instead of reordering it to make the lookup come out right.
    bool name_alias;
} display_timing_t;

// Sense code 7 is "nothing connected", which is also what a monitor that
// answers the extended tie-matrix probe reads as before the probe runs -- so
// it has no geometry here and lookup fails for it, deliberately.
static const display_timing_t display_timings[] = {
    {0x0, 1152, 870, 75000, "21\" RGB Workstation"},
    {0x1, 640, 870, 75000, "15\" Portrait B&W"},
    {0x2, 512, 384, 60150, "12\" RGB"},
    {0x3, 1152, 870, 75000, "21\" B&W Workstation", .name_alias = true},
    // NTSC underscan; the raster Apple's parts actually drive for it is the
    // 640x480 timing set, which is what every producer here has always used.
    {0x4, 640, 480, 59940, "NTSC", .name_alias = true},
    {0x5, 640, 870, 75000, "15\" Portrait RGB", .name_alias = true},
    {0x6, 640, 480, 66670, "13\"/14\" RGB"},
};

// Geometry for a passive sense code.  Returns NULL for 7 (no monitor) and for
// anything outside the 3-bit space.
static inline const display_timing_t *display_timing_for_sense(uint8_t sense) {
    for (size_t i = 0; i < sizeof display_timings / sizeof display_timings[0]; i++)
        if (display_timings[i].sense_code == (sense & 0x7u))
            return &display_timings[i];
    return NULL;
}

// The name of a raster, for log lines on the parts that derive their geometry
// from CRTC registers rather than from a sense code.  Returns NULL when no
// standard timing has that size.
static inline const char *display_timing_name(uint32_t width, uint32_t height) {
    for (size_t i = 0; i < sizeof display_timings / sizeof display_timings[0]; i++)
        if (!display_timings[i].name_alias && display_timings[i].width == width && display_timings[i].height == height)
            return display_timings[i].name;
    return NULL;
}

#endif // DISPLAY_TIMING_H
