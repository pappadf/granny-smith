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
struct scsi;

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

// Attach the bus/target model (scsi.c): select sequences reach real
// targets, transfer commands move data through the shared buffer, and
// the pseudo-DMA aperture below carries the payload.
void scsi_53c96_attach_bus(scsi_53c96_t *c, struct scsi *bus);

// TurboSCSI pseudo-DMA aperture: the CPU moves 16-bit words (or bytes)
// through this port while a DMA Transfer Information command is active.
// Not bus-mastering (Trap 1) — the CPU is the mover; DAFB only supplies
// the DRQ-gated acknowledge timing, which this functional model always
// satisfies immediately (the data is buffer-backed).
uint16_t scsi_53c96_pdma_read16(scsi_53c96_t *c);
void scsi_53c96_pdma_write16(scsi_53c96_t *c, uint16_t value);
uint8_t scsi_53c96_pdma_read8(scsi_53c96_t *c);
void scsi_53c96_pdma_write8(scsi_53c96_t *c, uint8_t value);

// Live DRQ output (for the TurboSCSI DRQ-status bit).
bool scsi_53c96_dreq(scsi_53c96_t *c);

// The target left the data phase with a DMA read still armed — a short
// transfer.  For a bus master (the PDM's AMIC pump) the phase change is
// visible before the chip is asked for another byte, so the master calls
// this to let the chip terminate the command the way real hardware does.
void scsi_53c96_dma_short_transfer(scsi_53c96_t *c);

#endif // SCSI_53C96_H
