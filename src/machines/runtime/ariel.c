// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ariel.c
// The Ariel CLUT/DAC register file and the Sonora video codes -- see ariel.h.

#include "ariel.h"

#include <string.h>

uint8_t ariel_read(ariel_t *a, uint32_t off, bool peek, uint8_t ctrl_mask) {
    switch (off & 3) {
    case 0:
        return ramdac_seq_addr(&a->seq);
    case 1: { // data reads auto-advance the R/G/B phase / address
        uint8_t comp;
        uint8_t idx = ramdac_seq_read(&a->seq, peek, &comp);
        return a->clut[idx][comp];
    }
    case 2:
        return (uint8_t)(a->ctrl & ctrl_mask);
    default:
        return a->key;
    }
}

bool ariel_write(ariel_t *a, uint32_t off, uint8_t value) {
    switch (off & 3) {
    case 0:
        // One index for reads and writes (no RAMDAC_SPLIT_INDEX): this is
        // the read address too, and it restarts the R/G/B phase.
        ramdac_seq_set_waddr(&a->seq, value);
        return false;
    case 1: { // latched R, G, B; the entry is stored on B
        uint8_t idx;
        if (!ramdac_seq_write(&a->seq, value, &idx))
            return false;
        memcpy(a->clut[idx], a->seq.pending, 3);
        return true;
    }
    case 2:
        a->ctrl = value; // low 3 bits mirror the depth; the owner's depth register drives the scanout
        return false;
    default:
        a->key = value;
        return false;
    }
}

void ariel_refresh_view(const ariel_t *a, display_t *d, rgba8_t *view, int mono_gun) {
    uint32_t bpp = display_bpp(d->format);
    if (bpp > 8) { // direct format: the CLUT is bypassed
        d->clut = NULL;
        d->clut_len = 0;
        return;
    }
    uint32_t len = 1u << bpp;
    uint32_t skip = 256u >> bpp;
    for (uint32_t i = 0; i < len; i++) {
        const uint8_t *e = a->clut[(skip - 1) + i * skip]; // the low index lines pulled high
        if (mono_gun >= 0)
            view[i] = (rgba8_t){e[mono_gun], e[mono_gun], e[mono_gun], 255};
        else
            view[i] = (rgba8_t){e[0], e[1], e[2], 255};
    }
    d->clut = view;
    d->clut_len = len;
    d->clut_dirty = true;
}

pixel_format_t ariel_depth_format(uint8_t code, pixel_format_t above) {
    switch (code & 0x07u) {
    case 0:
        return PIXEL_1BPP_MSB;
    case 1:
        return PIXEL_2BPP_MSB;
    case 2:
        return PIXEL_4BPP_MSB;
    case 3:
        return PIXEL_8BPP;
    case 4:
        return PIXEL_16BPP_555;
    default:
        return above;
    }
}

// The Sonora timing sets (PDM Developer Note Table 3-10; the Sonora video
// driver's codes).  1, 2 and 6 are the ordinary Apple sense codes; the codes
// past 7 are Sonora's own numbering -- not sense codes, and not the same space
// as any other part's extended set (DAFB's 9 is PAL; this 9 is GoldFish).
static const struct {
    uint8_t code;
    uint16_t width, height;
} ariel_modes[] = {
    {1,  640, 870}, // Portrait, 75 Hz
    {2,  512, 384}, // 12" RGB (Rubik), 60.15 Hz
    {6,  640, 480}, // 13"/14" RGB (Hi-Res), 66.67 Hz
    {9,  832, 624}, // 16" RGB (GoldFish), 74.55 Hz
    {10, 560, 384}, // 12" RGB, 560-wide mode
    {11, 640, 480}, // VGA, 59.94 Hz
    {13, 640, 400}, // 13" RGB, 640 x 400 mode (Vail timing set)
};

bool ariel_mode_geometry(uint8_t code, uint32_t *width, uint32_t *height) {
    for (size_t i = 0; i < sizeof ariel_modes / sizeof ariel_modes[0]; i++) {
        if (ariel_modes[i].code == code) {
            *width = ariel_modes[i].width;
            *height = ariel_modes[i].height;
            return true;
        }
    }
    return false;
}
