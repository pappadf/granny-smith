// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mdu_io.h
// The MDU+RBV family's I/O dispatch table, shared by the IIci and IIsi.  These
// two decode the canonical $50Fxxxxx island identically (VIA1 / SCC / SCSI
// {DRQ,REG,BLIND} / ASC / SWIM / VDAC / RBV, 18-bit $40000 mirror) — the IIsi
// dispatcher was a 0-substantive-diff prefix-rename of the IIci's (proposal
// §1.1).
//
// The decode runs on the shared mac030 I/O engine (mac030_glue_io.h): this file
// just supplies the MDU window table + mirror + device set via mdu_io_bind.
// Unlike GLUE there is no VIA2 window — the RBV ($26000) replaces it — and a
// Bt450 VDAC ($24000) fronts the built-in RBV video card.

#ifndef GS_MACHINES_MDU_IO_H
#define GS_MACHINES_MDU_IO_H

#include "mac030_glue_io.h" // the shared engine (mac030_io_t + mac030_io_*)
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

// The MDU dispatch context is the shared engine's context; this file installs
// the MDU table + mirror + devices into it.  (The IIci/IIsi state struct names
// its field's type mdu_io_t.)
typedef mac030_io_t mdu_io_t;

struct mac030_board_desc;

// Cache the MDU device interfaces and install the board's window table + mirror
// + unmapped-read value.
void mdu_io_bind(mdu_io_t *io, config_t *cfg, const struct mac030_board_desc *desc, void *asc, void *floppy, void *rbv,
                 struct nubus_card *video_card);

// The MDU family's ordered window table (sentinel-terminated).  Exposed for the
// address-map unit test (§6.1) and for MDU machines' board descriptors.
extern const mac030_io_range_t mdu_io_ranges_tbl[];
const mac030_io_range_t *mdu_io_ranges(void);

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
