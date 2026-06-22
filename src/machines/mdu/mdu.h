// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mdu.h
// The one MDU+RBV-family substrate (proposal §4.2.2 / per-family adjustment):
// the IIci and IIsi both bind `mdu_substrate`.  Their per-machine deltas live
// in a mac030_mdu_board_t (named via hw_profile_t.board) — the board's data
// descriptor, its VIA1 callbacks, and one build_devices hook that does the
// machine-specific device construction (straps, ADB/Egret companion, SCSI,
// ASC, SWIM, RBV, MMU, NuBus video, I/O bind, memory layout).  The shared
// substrate supplies the common spine (core/RTC/SCC/VIA1, then build_devices,
// then finish) and the reset/teardown/checkpoint/VBL lifecycle.

#ifndef GS_MACHINES_MDU_MDU_H
#define GS_MACHINES_MDU_MDU_H

#include "checkpoint.h"
#include "machine_profile.h" // machine_substrate_t
#include "system_config.h"

struct mac030_board_desc;

// An MDU machine = its board descriptor (data) + VIA1 callbacks + a single
// device-construction hook.  (Unlike GLUE there is no VIA2; the RBV replaces
// it, and the companion — Egret on IIsi — plus the 2-bank RAM live inside
// build_devices, which is the machine-specific body.)
typedef struct mac030_mdu_board {
    const struct mac030_board_desc *desc; // rom window, io map, slots, bus-error

    void (*via1_output)(void *context, uint8_t port, uint8_t value);
    void (*via1_shift_out)(void *context, uint8_t byte);

    // Build all machine-specific devices (everything after VIA1, before finish):
    // straps, ADB/Egret, SCSI, ASC, SWIM, RBV, MMU (+ any 2-bank), NuBus video,
    // mdu_io_bind, bus-error range, memory layout, and the checkpoint restore.
    void (*build_devices)(config_t *cfg, checkpoint_t *cp);
} mac030_mdu_board_t;

// The shared MDU init: allocate the unified state, build the II-family core +
// RTC/SCC/VIA1, run the board's build_devices, then finish.  An MDU machine's
// substrate is just &mdu_substrate; this is what its init resolves to.
void mac030_mdu_init(config_t *cfg, checkpoint_t *cp, const mac030_mdu_board_t *board);

// The one MDU+RBV-family substrate (IIci + IIsi).
extern const machine_substrate_t mdu_substrate;

#endif // GS_MACHINES_MDU_MDU_H
