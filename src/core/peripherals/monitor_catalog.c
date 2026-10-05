// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// monitor_catalog.c
// The shared monitor catalogue (see monitor_catalog.h).  Names follow Apple's
// product names; geometry is the display's native raster.

#include "monitor_catalog.h"

#include <stdio.h>
#include <string.h>

// "″" (U+2033 DOUBLE PRIME), spelled once.
#define IN "\xe2\x80\xb3"

static const monitor_catalog_entry_t s_catalog[] = {
    {"12in_rgb",            "12" IN " Macintosh RGB Display",          512,  384, false},
    {"12in_mono",           "12" IN " Macintosh Monochrome Display",   640,  480, true },
    {"13in_rgb",            "13" IN " AppleColor High-Resolution RGB", 640,  480, false},
    {"15in_portrait",       "15" IN " Macintosh Portrait Display",     640,  870, true },
    {"15in_portrait_color", "15" IN " Color Portrait Display",         640,  870, false},
    {"16in_rgb",            "16" IN " Macintosh Color Display",        832,  624, false},
    {"19in_rgb",            "19" IN " RGB Display",                    1024, 768, false},
    {"21in_rgb",            "21" IN " Macintosh Color Display",        1152, 870, false},
    {"21in_mono",           "21" IN " Two-Page Monochrome Display",    1152, 870, true },
    {"audiovision_14",      "14" IN " AudioVision 14 Display",         640,  480, false},
    {"multiscan_15",        "15" IN " Apple Multiple Scan Display",    832,  624, false},
    {"multiscan_17",        "17" IN " Apple Multiple Scan Display",    1024, 768, false},
    {"multiscan_20",        "20" IN " Apple Multiple Scan Display",    1152, 870, false},
    {"vga",                 "VGA monitor",                             640,  480, false},
    {"svga",                "SVGA monitor",                            800,  600, false},
    {"ntsc",                "NTSC television",                         640,  480, false},
    {"pal",                 "PAL television",                          768,  576, false},
    {"compact_9in",         "Built-in 9" IN " screen",                 512,  342, true },
    {"lisa_12in",           "Built-in 12" IN " screen",                720,  364, true },
    {MONITOR_NONE,          "No monitor",                              0,    0,   false},
    {NULL,                  NULL,                                      0,    0,   false},
};

const monitor_catalog_entry_t *monitor_catalog_find(const char *id) {
    if (!id)
        return NULL;
    for (const monitor_catalog_entry_t *m = s_catalog; m->id; m++) {
        if (strcmp(m->id, id) == 0)
            return m;
    }
    return NULL;
}

const monitor_catalog_entry_t *monitor_catalog_all(void) {
    return s_catalog;
}

void monitor_mode_label(const monitor_catalog_entry_t *m, uint32_t width, uint32_t height, int depth, char *buf,
                        size_t len) {
    bool grays = m && m->grays;
    const char *words;
    switch (depth) {
    case 1:
        words = "Black & white";
        break;
    case 2:
        words = grays ? "4 grays" : "4 colors";
        break;
    case 4:
        words = grays ? "16 grays" : "16 colors";
        break;
    case 8:
        words = grays ? "256 grays" : "256 colors";
        break;
    case 15:
    case 16:
        words = "Thousands of colors";
        break;
    case 24:
    case 32:
        words = "Millions of colors";
        break;
    default:
        words = NULL;
        break;
    }
    if (words)
        snprintf(buf, len, "%u \xc3\x97 %u, %s", width, height, words);
    else
        snprintf(buf, len, "%u \xc3\x97 %u, %d bits", width, height, depth);
}
