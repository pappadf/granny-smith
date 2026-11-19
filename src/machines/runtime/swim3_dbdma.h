// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// swim3_dbdma.h
// The SWIM3-over-DBDMA data path shared by the two MacRISC I/O controllers
// that feed the floppy from a DBDMA channel: Grand Central (TNT, channel 1)
// and Heathrow (Gossamer, channel 1).  PDM's AMIC moves floppy bytes its own
// way (pdm/swim3.c) and does not use this.
//
// The engine and the DMA engine meet in the middle.  The SWIM3 engine
// PUSHES: as a sector passes under the head it offers the decoded bytes
// one at a time, and a byte the channel cannot take is an overrun.  DBDMA
// PULLS: an INPUT command asks the device port for up to N bytes, and a
// port that returns fewer stalls the channel until the device kicks it.
// A byte ring between them turns the one into the other: reads fill the
// ring and kick the channel, which drains it into the descriptor's
// buffer; writes go the other way, the channel filling the ring from
// memory and the engine consuming it.  The ring is sized for the largest
// thing the engine streams in one service slot (a GCR sector is 704
// bytes; a whole-track format or raw capture streams a track, which the
// engine hands over in slot-sized pieces), with room to spare.
//
// What differs per board is data: the DBDMA engine and channel, and the
// interrupt sink for the chip's IRQ pin.

#ifndef GS_MACHINES_RUNTIME_SWIM3_DBDMA_H
#define GS_MACHINES_RUNTIME_SWIM3_DBDMA_H

#include "swim3.h"

#include <stdbool.h>
#include <stdint.h>

struct dbdma;
struct floppy;
struct scheduler;

// === Constants ==============================================================

#define SWIM3_DBDMA_RING_SIZE 4096u

// === Type Definitions =======================================================

// The byte ring between the chip and the channel.  Plain data, checkpointed
// whole by the family (sizeof(ring)).
typedef struct swim3_dbdma_ring {
    uint8_t buf[SWIM3_DBDMA_RING_SIZE];
    uint32_t head, tail; // free-running; count = tail - head
} swim3_dbdma_ring_t;

// One SWIM3's DBDMA port: the ring (checkpointed) and its wiring (rebuilt by
// swim3_dbdma_init; not checkpointed).  Its address is the callbacks'
// context, so it lives in the family state for the machine's lifetime.
typedef struct swim3_dbdma {
    swim3_dbdma_ring_t ring;
    // --- wiring: not checkpointed ---
    struct dbdma *dbdma; // the I/O controller's DBDMA engine
    int chan; // the floppy channel
    void (*set_irq)(void *ctx, bool level); // the chip's IRQ pin
    void *irq_ctx;
} swim3_dbdma_t;

// === Lifecycle ==============================================================

// Wire the port to its channel and IRQ sink, empty the ring and register the
// DBDMA device port {out, in, stop}.  Once, at construction (a restore then
// overwrites the ring).
void swim3_dbdma_init(swim3_dbdma_t *p, struct dbdma *dbdma, int chan, void (*set_irq)(void *ctx, bool level),
                      void *irq_ctx);

// Bind the shared SWIM3 model to this port (its DMA backend) and to the
// floppy and scheduler.  At construction and again after a restore, which
// overwrites the chip's pointer tail.
void swim3_dbdma_bind(swim3_dbdma_t *p, swim3_t *chip, struct floppy *fd, struct scheduler *sched);

#endif // GS_MACHINES_RUNTIME_SWIM3_DBDMA_H
