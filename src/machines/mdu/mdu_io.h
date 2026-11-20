// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mdu_io.h
// The MDU+RBV family's I/O dispatch table, shared by the IIci and IIsi.  These
// two decode the canonical $50Fxxxxx island identically (VIA1 / SCC / SCSI
// {DRQ,REG,BLIND} / ASC / SWIM / VDAC / RBV, 18-bit $40000 mirror) — the IIsi
// dispatcher was a 0-substantive-diff prefix-rename of the IIci's.
//
// The decode runs on the shared board_io engine (m68k_board_io.h): this file
// just supplies the MDU window table + mirror + device set via mdu_io_bind.
// Unlike GLUE there is no VIA2 window — the RBV ($26000) replaces it — and a
// Bt450 VDAC ($24000) fronts the built-in RBV video card.

#ifndef GS_MACHINES_MDU_IO_H
#define GS_MACHINES_MDU_IO_H

#include "m68k_board_io.h" // the shared engine (board_io_t + board_io_*)
#include "m68k_board_overlay.h"
#include "memory.h"
#include "system_internal.h"

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
typedef board_io_t mdu_io_t;

struct m68k_board_desc;

// Cache the MDU device interfaces and install the board's window table + mirror
// + unmapped-read value.
void mdu_io_bind(mdu_io_t *io, config_t *cfg, const struct m68k_board_desc *desc, void *asc, void *floppy, void *rbv,
                 struct nubus_card *video_card);

// The MDU family's ordered window table (sentinel-terminated).  Exposed for the
// address-map unit test and for MDU machines' board descriptors.
extern const board_io_range_t mdu_io_ranges_tbl[];
const board_io_range_t *mdu_io_ranges(void);

// Unified MDU+RBV machine state — the single struct shared by the IIci and
// IIsi (mirrors the GLUE-family unification).  Superset: the IIsi uses egret;
// the IIci leaves egret NULL.
typedef struct mdu_state {
    struct asc *asc; // (the adb and floppy are cfg->adb / cfg->floppy)
    struct egret *egret; // IIsi companion (ADB/RTC/PRAM/power); NULL on IIci
    struct rbv *rbv; // RBV chip (VIA2 replacement + video control)
    struct nubus_card *video_card; // built-in RBV video pseudo-card

    m68k_board_overlay_t overlay; // ROM at $00000000, following VIA1 PA4 (line policy)
    struct mmu_state *mmu;

    mdu_io_t mdu_io; // device handles for the shared MDU dispatcher

    memory_interface_t io_interface; // registered at the $50000000 I/O region
} mdu_state_t;

#endif // GS_MACHINES_MDU_IO_H
