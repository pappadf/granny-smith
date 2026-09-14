// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scsi_mesh.h
// Apple's MESH (Macintosh Enhanced SCSI Hardware) controller.
//
// A peer of the NCR 5380, the NCR 53C96 and the Symbios 53C825 SCRIPTS engine:
// it drives the shared bus in core/peripherals/scsi.h and nothing else.  It
// used to live under machines/tnt/ with no handle of its own, taking config_t*
// everywhere and reaching its state through tnt_st(cfg)->mesh, which meant it
// could not be constructed outside a whole Power Macintosh and so could not be
// unit-tested at all.
//
// The shape here is deliberately the 53C96's, because they are peers and one
// shape is easier to hold in your head than four.

#ifndef SCSI_MESH_H
#define SCSI_MESH_H

#include "byte_fifo.h"
#include "common.h"
#include "scsi_msgsession.h"

#include <stdbool.h>
#include <stdint.h>

struct scsi;
struct scheduler;

#define MESH_FIFO 16

// Interrupt delivery: the machine says where the line goes.  On a Power
// Macintosh that is Grand Central's MESH source.
typedef void (*mesh_irq_cb)(void *ctx, bool level);

// The data phases move through a DBDMA channel the machine owns; MESH pokes it
// when it has room or bytes.
typedef void (*mesh_dbdma_kick_cb)(void *ctx);

typedef struct mesh {
    // ---- plain data, saved as one block ----------------------------------
    // Everything up to the first pointer is written in a single
    // system_write_checkpoint_data() call, the same shape via_t, scc_t, rtc_t,
    // struct scsi, scsi_5380_t, scsi_53c96_t and sym53c8xx_t use.
    //
    // This struct had NO pointers before the move, so tnt.c could save it with
    // sizeof() and get away with it.  Adding the bus and the two callbacks is
    // exactly what makes that unsafe -- it is how F-21's host addresses got
    // into the 53C96's checkpoint -- so the bound is explicit from the start.

    BYTE_FIFO(MESH_FIFO) fifo;
    uint8_t sequence; // last written sequence-command byte
    uint8_t bus0_atn; // explicitly driven ATN (bus_status0 write)
    uint8_t exception, error; // W1C cause latches
    uint8_t intr_mask, interrupt; // W1C summary; mask gates GC line only
    uint8_t source_id, dest_id;
    uint8_t sync_params, sel_timeout;
    // Live transfer engine: the sequence command in progress and its
    // down-counter (count_lo/hi read back the live remainder).
    uint8_t active; // command nibble in progress (0 = idle)
    uint8_t active_dma; // SEQ_DMA_MODE was set on the active command
    uint32_t remaining; // bytes left on the active transfer
    uint8_t connected; // a target is selected (bus not free)
    uint8_t msgout_pending; // select-with-ATN: present MSG OUT until sent
    uint8_t resel_enabled, parity_enabled;
    // SDTR message engine (mesh.c §"Sync negotiation"): the assembled
    // message-out bytes of the current session and the virtual
    // message-in queue the target speaks through.  All of it is
    // per-connection state.
    // The IDENTIFY/SDTR conversation, shared with the 53C8xx SCRIPTS engine.
    // MESH's capability limits live beside the code that applies them.
    scsi_msgsession_t msg;
    uint8_t msgin_taken; // the bus message byte was delivered (MESSAGE IN
                         // lingers in the bus model until release, but the
                         // target no longer REQs — busfree must succeed)

    // ---- runtime pointers: re-bound by the machine after a restore --------
    struct scsi *bus;
    struct scheduler *sched;
    mesh_irq_cb irq_cb;
    void *irq_ctx;
    mesh_dbdma_kick_cb dbdma_kick;
    void *dbdma_ctx;
} mesh_t;

mesh_t *mesh_init(struct scheduler *sched, checkpoint_t *cp);
void mesh_delete(mesh_t *m);
void mesh_checkpoint(mesh_t *m, checkpoint_t *cp);

void mesh_attach_bus(mesh_t *m, struct scsi *bus);
void mesh_set_irq_callback(mesh_t *m, mesh_irq_cb cb, void *ctx);
void mesh_set_dbdma_kick(mesh_t *m, mesh_dbdma_kick_cb cb, void *ctx);

uint8_t mesh_read(mesh_t *m, uint32_t offset);
void mesh_write(mesh_t *m, uint32_t offset, uint8_t value);
void mesh_reset(mesh_t *m);

// The DBDMA channel-10 device port: the machine registers these with its own
// DBDMA engine.
int mesh_port_in(void *ctx, uint8_t *buf, int len);
int mesh_port_out(void *ctx, const uint8_t *buf, int len);

#endif // SCSI_MESH_H
