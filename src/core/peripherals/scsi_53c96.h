// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scsi_53c96.h
// NCR 53C96 Advanced SCSI Controller — the Quadra generation's SCSI chip
// (proposal-machine-quadra-700-900-950.md §10), machine-independent so the
// later 68040/early-PowerPC machines can reuse it.
//
// Phase C scope: the chip register file with data-manual-faithful reset and
// interrupt semantics (NCR 53C94/95/96 Data Manual ch. 4/5) — enough for the
// boot ROM's controller probe and bus scan: chip reset, NOP, flush FIFO,
// SCSI bus reset, enable/disable selection, and the select sequences ending
// in a selection time-out interrupt when no target responds.  Phase E
// attaches the existing bus/target/CD-ROM object model and the TurboSCSI
// pseudo-DMA path.

#ifndef SCSI_53C96_H
#define SCSI_53C96_H

#include "checkpoint.h"

#include <stdbool.h>
#include <stdint.h>

struct scheduler;

typedef struct scsi_53c96 scsi_53c96_t;

// INT output callback (level; wired to VIA2 CB2 on the Quadras).
typedef void (*scsi_53c96_irq_cb)(void *context, bool active);

// Create a controller instance.  `clock_hz` scales the selection time-out
// (register value × 8192 × clock-conversion / clock).
scsi_53c96_t *scsi_53c96_init(struct scheduler *sched, uint32_t clock_hz, checkpoint_t *cp);
void scsi_53c96_delete(scsi_53c96_t *c);
void scsi_53c96_checkpoint(scsi_53c96_t *c, checkpoint_t *cp);

void scsi_53c96_set_irq_callback(scsi_53c96_t *c, scsi_53c96_irq_cb cb, void *context);

// Register file access (reg = A3..A0, i.e. the byte offset already divided
// by the board's 16-byte spacing).
uint8_t scsi_53c96_read(scsi_53c96_t *c, uint32_t reg);
void scsi_53c96_write(scsi_53c96_t *c, uint32_t reg, uint8_t value);

// Hardware reset (power-on / RESET line).
void scsi_53c96_reset(scsi_53c96_t *c);

#endif // SCSI_53C96_H
