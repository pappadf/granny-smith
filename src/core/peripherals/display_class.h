// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// display_class.h
// The one `framebuffer` object node every display source exposes.
//
// `machine.screen.source` is a reference edge precisely so a script can read
// the active framebuffer without knowing which bus it is on.  That only works
// if the node on the far end is the SAME node.  It was not: the NuBus node
// called its byte count `raw_size` and the PCI one called it `size`, NuBus
// typed width/height/depth as V_INT and PCI as V_UINT, and only NuBus had
// `format` -- so a script reading `.raw_size` worked on a IIcx and errored on
// a pm9500, and one reading `.format` worked on NuBus only.
// The built-in video chips had no framebuffer node at all, which is why the
// headless-debug workflow on a Quadra or an 8100 was strictly poorer than on
// a card-based machine.
//
// A source attaches this class with a display_fb_node_t describing how to
// reach its live display_t.  Nothing is copied: every read goes to the
// producer's descriptor at the moment of the read.

#ifndef DISPLAY_CLASS_H
#define DISPLAY_CLASS_H

#include "display.h"
#include "object.h"

#include <stdint.h>

// How one source's framebuffer node reaches its state.  The struct must
// outlive the node; sources keep it beside the rest of their node storage.
typedef struct display_fb_node {
    void *owner; // producer-defined (the card, the chip state, ...)
    // The live descriptor, or NULL when the source currently has none (a
    // refused scanout, an empty socket).  Every attribute reads 0 / "" then.
    display_t *(*resolve)(void *owner);
    // Where the framebuffer sits, in whatever address space the source's
    // guest reaches it through: NuBus slot space, a byte offset into VRAM, a
    // physical address.  Optional -- `base` reads 0 without it, because "this
    // source does not have one address" is a real answer.
    uint64_t (*base)(void *owner);
} display_fb_node_t;

// The shared class.  instance_data must be a display_fb_node_t *.
extern const class_desc_t display_fb_class;

// Attach `machine.video` with a `framebuffer` child, for a machine whose
// display comes from a soldered-down chip rather than from a card -- DAFB,
// Civic, Ariel, Control, RBV, the SE/30's built-in video.  Those machines had
// no framebuffer node at all, so on a Quadra or an 8100 there was no way to
// read the stride, the pixel format or the scan base from the shell, and the
// headless-debug workflow was strictly poorer than on a card-based machine.
//
// `node` must outlive the machine (the chip's own state is the natural home).
// Returns the `video` node, or NULL; the caller owns detaching it at teardown.
struct object *display_attach_video_node(display_fb_node_t *node, const char *label);

// Tear down what display_attach_video_node built.
void display_detach_video_node(struct object *video);

#endif // DISPLAY_CLASS_H
