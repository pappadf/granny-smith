// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lisa_profile.h
// Apple ProFile parallel hard disk attached to the Lisa's built-in parallel
// port (VIA2).  NOT a SCSI disk: a daisy-wheel handshake clocks one byte at a
// time over VIA2 port A while VIA2 port B carries the CMD//BSY//OCD control
// lines.  Modeled behaviourally, driven only by the real VIA pins, following
// Apple's own driver frame (boot ROM RM248.B PROINIT/STRTRD/READIT and OS
// SOURCE-PROFILEASM).  See docs/reference/machines/lisa/lisa.md §14 and docs/internals/machines/lisa/profile.md.
//
// On-the-wire block = 532 bytes: a 20-byte tag/header followed by 512 data
// bytes.  A read streams 4 status bytes then the 532-byte block; a write
// streams the 532-byte block then (after a handshake) 4 status bytes.  Reading
// the special block $FFFFFF returns the controller's synthesized device-info
// block (drive type + capacity), which the OS driver uses to size the volume.

#ifndef LISA_PROFILE_H
#define LISA_PROFILE_H

#include "checkpoint.h"
#include "common.h"
#include "image.h"

#include <stdbool.h>
#include <stdint.h>

struct scheduler;
struct image;
typedef struct image image_t;

typedef struct lisa_profile lisa_profile_t;

// Drives the BSY handshake line, which the machine wires to VIA2 PB1 (level,
// polled by the boot ROM) and CA1 (edge, used by the OS interrupt path):
//   busy = true  -> line low  (controller acknowledged CMD//processing)
//   busy = false -> line high (controller ready / not busy)
typedef void (*lisa_profile_bsy_fn)(void *ctx, bool busy);

// === Lifecycle =============================================================

// A restore (`cp`) reattaches the image it saved, resolved in `images` (the
// machine's restored image list).
lisa_profile_t *lisa_profile_init(struct scheduler *scheduler, lisa_profile_bsy_fn bsy_cb, void *bsy_ctx,
                                  checkpoint_t *cp, const image_list_t *images);
void lisa_profile_delete(lisa_profile_t *pf);
void lisa_profile_checkpoint(lisa_profile_t *pf, checkpoint_t *cp);

// === Media ==================================================================

// Standard 5 MB ProFile: 9728 logical blocks (the canonical device the Lisa
// Office System and boot ROM were written for): a blank disk's size.  A
// pre-existing image's size overrides it.
#define LISA_PROFILE_DEFAULT_BLOCKS 9728u

// Disconnect the attached image and hand it back (NULL when none): its owner,
// the machine's image table, releases it.
image_t *lisa_profile_detach(lisa_profile_t *pf);
bool lisa_profile_attached(const lisa_profile_t *pf);

// Attach an open 532-bytes/block image (image_open_kind as IMAGE_KIND_PROFILE,
// or image_create_blank_kind): geometry is re-derived from the handle.  False,
// leaving the handle with the caller, on bad arguments or when a disk is
// already attached.
bool lisa_profile_attach_image(lisa_profile_t *pf, image_t *img);
// The attached image (NULL when none), for an export off the emulator thread.
image_t *lisa_profile_image(const lisa_profile_t *pf);

// True when a disk is attached (the machine drives OCD/ low → "connected").
bool lisa_profile_connected(const lisa_profile_t *pf);

// === VIA2 wiring ============================================================

// VIA2 port-B output changed (CMD/ on PB4, DRW on PB3).  Edge-driven: the device
// reacts to CMD/ assert/deassert.
void lisa_profile_portb(lisa_profile_t *pf, uint8_t portb);

// VIA2 port-A data byte.  `handshake` = true for the CA2/PSTRB-strobed register
// (data/command/status bytes), false for the no-handshake register (the state
// byte read / the reply byte written during a handshake).
uint8_t lisa_profile_porta_read(lisa_profile_t *pf, bool handshake);
void lisa_profile_porta_write(lisa_profile_t *pf, uint8_t value, bool handshake);

// Controller reset (VIA1 PB7 / CRES/ pulsed low): abort any transfer → idle.
void lisa_profile_reset(lisa_profile_t *pf);

#endif // LISA_PROFILE_H
