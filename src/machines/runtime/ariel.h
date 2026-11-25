// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ariel.h
// Apple's Ariel CLUT/DAC ($50F24000) as the V8 family (v8/v8_video.c, the
// LC II, Color Classic and Sonora boards) and the PDM Power Macs
// (pdm/ariel.c) both carry it: the four-register file -- address, data,
// control, key colour -- over a 256-entry R/G/B table, the reduced-depth
// palette window the hardware feeds the screen, the Sonora video-control
// depth codes and the Sonora monitor codes' rasters.
//
// The palette window: the Ariel has eight index lines; at a reduced depth the
// pixel drives the HIGH lines and the unused low ones are pulled high, so
// pixel v of a d-bit screen reads entry (v << (8-d)) | (2^(8-d)-1) -- 1 bpp
// white/black at $7F/$FF (Apple, *Macintosh LC Computer* Developer Note
// (1990), "Video").
//
// What stays per family is outside: where the table is checkpointed, which
// gun a one-gun monitor shows, and the depth/mode registers' side effects.

#ifndef GS_MACHINES_RUNTIME_ARIEL_H
#define GS_MACHINES_RUNTIME_ARIEL_H

#include "display.h"
#include "display_ramdac.h"

#include <stdbool.h>
#include <stdint.h>

// The register file.  Plain bytes, no padding: an owner checkpoints it as part
// of its scalar block (pdm_amic_t does, in this field order).
typedef struct ariel {
    ramdac_seq_t seq; // the address register and R/G/B phase (one shared index)
    uint8_t ctrl; // control (low 3 bits mirror the depth code)
    uint8_t key; // key colour
    uint8_t clut[256][3]; // the colour table, R/G/B
} ariel_t;
_Static_assert(sizeof(ariel_t) == sizeof(ramdac_seq_t) + 2 + 256 * 3, "ariel_t is unpadded bytes");

// ariel_refresh_view's `mono_gun` for a colour monitor.
#define ARIEL_COLOUR (-1)

// A register read at $50F24000 + off; a data read steps the R/G/B phase
// unless `peek`.  The control register reads back through `ctrl_mask`.
uint8_t ariel_read(ariel_t *a, uint32_t off, bool peek, uint8_t ctrl_mask);

// A register write.  Returns true when it committed a colour-table entry (the
// palette view is then stale).  An address write sets the one index the
// sequencer keeps for reads and writes alike.
bool ariel_write(ariel_t *a, uint32_t off, uint8_t value);

// Rebuild the palette `d` presents at its current format into `view` (256
// entries): the reduced-depth window, or no palette above 8 bpp.  A one-gun
// monitor shows the table's `mono_gun` component (0 R, 1 G, 2 B) on all
// three channels; ARIEL_COLOUR shows the triple.
void ariel_refresh_view(const ariel_t *a, display_t *d, rgba8_t *view, int mono_gun);

// The Sonora video-control depth code (0..4 = 1/2/4/8/16 bpp, as the drivers
// write it; also the V8 MonP depth field) as a pixel format; codes above 4
// map to `above`.
pixel_format_t ariel_depth_format(uint8_t code, pixel_format_t above);

// The raster a Sonora monitor code (the video mode register's timing-set
// number, as the Sonora video driver programs it) scans.  False for a code
// with no timing set.
bool ariel_mode_geometry(uint8_t code, uint32_t *width, uint32_t *height);

#endif // GS_MACHINES_RUNTIME_ARIEL_H
