// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mdu_io.h
// The MDU+RBV-family I/O dispatcher, shared by the IIci and IIsi.  These two
// decode the canonical $50Fxxxxx island identically (VIA1 / SCC / SCSI
// {DRQ,REG,BLIND} / ASC / SWIM / VDAC / RBV, 18-bit $40000 mirror) — the IIsi
// dispatcher was a 0-substantive-diff prefix-rename of the IIci's (proposal
// §1.1).  Like the GLUE dispatcher (mac030_glue_io.h) it is decoupled from
// each machine's private state via a small mdu_io_t device-handle context.
//
// Unlike GLUE there is no VIA2 window — the RBV ($26000) replaces it — and a
// Bt450 VDAC ($24000) fronts the built-in RBV video card.

#ifndef GS_MACHINES_MDU_IO_H
#define GS_MACHINES_MDU_IO_H

#include "memory.h"
#include "system_config.h"

#include <stdbool.h>
#include <stdint.h>

struct nubus_card;
struct adb;
struct asc;
struct egret;
struct floppy;
struct rbv;
struct mmu_state;

// Device handles + cached interfaces the MDU dispatcher routes to.
typedef struct mdu_io {
    config_t *cfg; // for cfg->via1 / scc / scsi
    const memory_interface_t *via1_iface;
    const memory_interface_t *scc_iface;
    const memory_interface_t *scsi_iface;
    const memory_interface_t *asc_iface;
    const memory_interface_t *floppy_iface;
    const memory_interface_t *rbv_iface;
    void *asc; // asc_t*
    void *floppy; // floppy_t*
    void *rbv; // rbv_t*
    struct nubus_card *video_card; // built-in RBV video (VDAC window)
} mdu_io_t;

// Cache the device interfaces from the constructed peripherals.
void mdu_io_bind(mdu_io_t *io, config_t *cfg, void *asc, void *floppy, void *rbv, struct nubus_card *video_card);

// The six dispatch entry-points.  `ctx` is an mdu_io_t*.
uint8_t mdu_io_read_uint8(void *ctx, uint32_t addr);
uint16_t mdu_io_read_uint16(void *ctx, uint32_t addr);
uint32_t mdu_io_read_uint32(void *ctx, uint32_t addr);
void mdu_io_write_uint8(void *ctx, uint32_t addr, uint8_t value);
void mdu_io_write_uint16(void *ctx, uint32_t addr, uint16_t value);
void mdu_io_write_uint32(void *ctx, uint32_t addr, uint32_t value);

// Fill `iface` with the six dispatch function pointers.
void mdu_io_fill_interface(memory_interface_t *iface);

// Unified MDU+RBV machine state — the single struct shared by the IIci and
// IIsi (mirrors the GLUE-family unification).  Superset: the IIsi uses egret +
// last_port_a; the IIci uses last_port_b and leaves egret NULL.
typedef struct mac030_mdu_state {
    struct adb *adb;
    struct asc *asc;
    struct egret *egret; // IIsi companion (ADB/RTC/PRAM/power); NULL on IIci
    struct floppy *floppy;
    struct rbv *rbv; // RBV chip (VIA2 replacement + video control)
    struct nubus_card *video_card; // built-in RBV video pseudo-card

    bool rom_overlay;
    struct mmu_state *mmu;

    mdu_io_t mdu_io; // device handles for the shared MDU dispatcher

    uint8_t last_port_a; // IIsi: floppy/overlay filtering on VIA1 PA
    uint8_t last_port_b; // IIci: ADB ST filtering on VIA1 PB

    memory_interface_t io_interface; // registered at the $50000000 I/O region
} mac030_mdu_state_t;

#endif // GS_MACHINES_MDU_IO_H
