// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lisa_profile.h
// Apple ProFile parallel hard disk attached to the Lisa's built-in parallel
// port (VIA2).  NOT a SCSI disk: a daisy-wheel handshake clocks one byte at a
// time over VIA2 port A while VIA2 port B carries the CMD//BSY//OCD control
// lines.  Modeled behaviourally, driven only by the real VIA pins, following
// Apple's own driver frame (boot ROM RM248.B PROINIT/STRTRD/READIT and OS
// SOURCE-PROFILEASM).  See docs/machines/lisa/lisa.md §14 and docs/machines/lisa/profile.md.
//
// On-the-wire block = 532 bytes: a 20-byte tag/header followed by 512 data
// bytes.  A read streams 4 status bytes then the 532-byte block; a write
// streams the 532-byte block then (after a handshake) 4 status bytes.  Reading
// the special block $FFFFFF returns the controller's synthesized device-info
// block (drive type + capacity), which the OS driver uses to size the volume.

#ifndef LISA_PROFILE_H
#define LISA_PROFILE_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>

struct scheduler;

typedef struct lisa_profile lisa_profile_t;

// Drives the BSY handshake line, which the machine wires to VIA2 PB1 (level,
// polled by the boot ROM) and CA1 (edge, used by the OS interrupt path):
//   busy = true  -> line low  (controller acknowledged CMD//processing)
//   busy = false -> line high (controller ready / not busy)
typedef void (*lisa_profile_bsy_fn)(void *ctx, bool busy);

// === Lifecycle =============================================================

lisa_profile_t *lisa_profile_init(struct scheduler *scheduler, lisa_profile_bsy_fn bsy_cb, void *bsy_ctx,
                                  checkpoint_t *cp);
void lisa_profile_delete(lisa_profile_t *pf);
void lisa_profile_checkpoint(lisa_profile_t *pf, checkpoint_t *cp);

// === Media ==================================================================

// Attach a 532-bytes/block image through the shared base+delta subsystem.  A
// NULL path makes a blank (all-zero) disk of the canonical 5 MB geometry; a
// real path opens that file as an immutable base, all writes going to a
// per-instance delta (the base is never modified).  Geometry (block count) is
// taken from the file size, or PRO_DEFAULT_BLOCKS for a blank disk.  `writable`
// selects a persistent delta (true) or an ephemeral scratch one (false).
// Returns false on I/O error.
bool lisa_profile_attach(lisa_profile_t *pf, const char *path, bool writable);
void lisa_profile_detach(lisa_profile_t *pf); // close the image (no base writeback)
bool lisa_profile_attached(const lisa_profile_t *pf);

// Write the current contents (base merged with the delta) to a new,
// self-contained single-file 532-bytes/block image at `path`.  Refuses to
// overwrite an existing file.  Returns false on I/O error.
bool lisa_profile_save_as(const lisa_profile_t *pf, const char *path);

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
