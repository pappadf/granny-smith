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

// === Type Definitions ===
struct scsi;
typedef struct scsi scsi_t;

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

scsi_t *scsi_init(memory_map_t *map, checkpoint_t *checkpoint);

void scsi_delete(scsi_t *scsi);

void scsi_checkpoint(scsi_t *restrict scsi, checkpoint_t *checkpoint);

// === Operations ===

void scsi_add_device(scsi_t *restrict scsi, int scsi_id, const char *vendor, const char *product, image_t *image);

#endif // SCSI_H
