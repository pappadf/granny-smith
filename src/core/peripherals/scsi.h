// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scsi.h
// Public interface for SCSI controller emulation.

#ifndef SCSI_H
#define SCSI_H

// === Includes ===
#include "common.h"
#include "image.h"
#include "memory.h"
#include "via.h"

// === Type Definitions ===
struct scsi;
typedef struct scsi scsi_t;

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

scsi_t *scsi_init(memory_map_t *map, checkpoint_t *checkpoint);

void scsi_delete(scsi_t *scsi);

void scsi_checkpoint(scsi_t *restrict scsi, checkpoint_t *checkpoint);

// === Device Types ===
enum scsi_device_type;

// === Operations ===

void scsi_add_device(scsi_t *restrict scsi, int scsi_id, const char *vendor, const char *product, const char *revision,
                     image_t *image, enum scsi_device_type type, uint16_t block_size, bool read_only);

// Get the memory-mapped I/O interface for machine-level address decode
const memory_interface_t *scsi_get_memory_interface(scsi_t *scsi);

// Connect SCSI interrupt outputs (IRQ, DRQ) to VIA2 for SE/30-style machines.
// On machines without VIA2 (e.g. Plus), this is not called and SCSI is polled.
void scsi_set_via(scsi_t *scsi, via_t *via);

// Enable/disable SCSI loopback test card (passive bus terminator).
// When enabled, initiator-driven signals are reflected back through
// status registers, emulating a connected SCSI diagnostic card.
void scsi_set_loopback(scsi_t *scsi, bool enable);

// Query whether SCSI loopback mode is active
bool scsi_get_loopback(scsi_t *scsi);

// Eject the medium at the given SCSI id (0..6). Returns 1 on successful
// eject, 0 if the slot was already empty, -1 on bad arguments.
int scsi_eject_device(scsi_t *scsi, int id);

// === M7d — object-model accessors ==========================================
//
// Read-only views over the SCSI controller and its 8 device slots used
// by the `scsi` / `scsi.bus` / `scsi.devices` object classes. Phase is
// exposed as an integer with the canonical name table living in the
// object class so the proposal's V_ENUM display works without leaking
// the internal phase enum across the public header.
//
// Slot index is 0..7 (the SCSI ID). Reads on an unpopulated slot
// return type=0 (none) / 0 / NULL — callers test `type` first.

// Bus phase as a small integer matching the order:
//   0=bus_free, 1=arbitration, 2=selection, 3=reselection, 4=command,
//   5=data_in, 6=data_out, 7=status, 8=message_in, 9=message_out
int scsi_get_bus_phase(const scsi_t *scsi);
int scsi_get_bus_target(const scsi_t *scsi);
int scsi_get_bus_initiator(const scsi_t *scsi);

// Per-device queries. `which` is the SCSI ID (0..7).
//   type:           0=none, 1=hd, 2=cdrom (matches `enum scsi_device_type`)
//   read_only:      block-write inhibit
//   medium_present: relevant for CD-ROM eject/insert state
//   block_size:     512 for HD, usually 2048 for CD-ROM
//   vendor/product: NULL when slot is empty
int scsi_device_type(const scsi_t *scsi, unsigned which);
bool scsi_device_present(const scsi_t *scsi, unsigned which);
bool scsi_device_read_only(const scsi_t *scsi, unsigned which);
bool scsi_device_medium_present(const scsi_t *scsi, unsigned which);
uint16_t scsi_device_block_size(const scsi_t *scsi, unsigned which);
const char *scsi_device_vendor(const scsi_t *scsi, unsigned which);
const char *scsi_device_product(const scsi_t *scsi, unsigned which);
const char *scsi_device_revision(const scsi_t *scsi, unsigned which);

// Get the image_t* mounted at the given SCSI id, or NULL if the slot
// is empty or the device has no medium loaded.
struct image *scsi_device_image(const scsi_t *scsi, unsigned which);

#endif // SCSI_H
