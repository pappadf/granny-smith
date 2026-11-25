// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mdu.h
// The one MDU+RBV-family substrate:
// the IIci and IIsi both bind `mdu_substrate`.  Their per-machine deltas live
// in a mdu_board_t (named via hw_profile_t.board) — the board's data
// descriptor, its VIA1 callbacks, and one build_devices hook that does the
// machine-specific device construction (straps, ADB/Egret companion, SCSI,
// ASC, SWIM, RBV, MMU, NuBus video, I/O bind, memory layout).  The shared
// substrate supplies the common spine (core/RTC/SCC/VIA1, then build_devices,
// then finish) and the reset/teardown/checkpoint/VBL lifecycle.

#ifndef GS_MACHINES_MDU_MDU_H
#define GS_MACHINES_MDU_MDU_H

#include "checkpoint.h"
#include "machine_profile.h" // machine_substrate_t
#include "system_internal.h"

struct m68k_board_desc;

// An MDU machine = its board descriptor (data) + VIA1 callbacks + a single
// device-construction hook.  (Unlike GLUE there is no VIA2; the RBV replaces
// it, and the companion — Egret on IIsi — plus the 2-bank RAM live inside
// build_devices, which is the machine-specific body.)
typedef struct mdu_board {
    const struct m68k_board_desc *desc; // rom window, io map, slots, bus-error

    void (*via1_output)(void *context, uint8_t port, uint8_t value);
    void (*via1_shift_out)(void *context, uint8_t byte);

    // Build all machine-specific devices (everything after VIA1, before finish):
    // straps, ADB/Egret, SCSI, ASC, SWIM, RBV, MMU (+ any 2-bank), NuBus video,
    // mdu_io_bind, bus-error range, memory layout, and the checkpoint restore.
    int (*build_devices)(config_t *cfg, checkpoint_t *cp);
} mdu_board_t;

// The shared MDU init: allocate the unified state, build the II-family core +
// RTC/SCC/VIA1, run the board's build_devices, then finish.  An MDU machine's
// substrate is just &mdu_substrate; this is what its init resolves to.
int mdu_init(config_t *cfg, checkpoint_t *cp, const mdu_board_t *board);

// The one MDU+RBV-family substrate (IIci + IIsi).
extern const machine_substrate_t mdu_substrate;
// The RBV built-in video as a display device (IIci and IIsi).
extern const builtin_video_desc_t mdu_builtin_video_iici;
extern const builtin_video_desc_t mdu_builtin_video_iisi;

// IRQ source bits in cfg->rt.irq, routed by the GLUE table
// (m68k_board_update_ipl): the RBV at level 2 replaces the GLUE VIA2 and
// carries the combined slot/SCSI/sound IFR.  The same values as
// M68K_BOARD_IRQ_*; the IIsi's Egret rides VIA1's shift-register
// interrupt, so it needs no source of its own.
#define MDU_IRQ_VIA1 (1 << 0)
#define MDU_IRQ_RBV  (1 << 1)
#define MDU_IRQ_SCC  (1 << 2)
#define MDU_IRQ_NMI  (1 << 3)

// Wire the RBV and the ASC to the machine (both MDU boards, once st->rbv
// and st->asc are built): the RBV's combined interrupt to IPL 2, its soft
// power-off, its depth and blank outputs to the built-in video card, and the
// ASC's interrupt to the RBV sound flag.
void mdu_wire_rbv(config_t *cfg);

// The 5380's IRQ/DRQ, routed through the RBV flags (RvSCSIRQ / RvSCSIDRQ)
// rather than a VIA2: a scsi_set_irq_callback handler, context cfg.
void mdu_scsi_irq(void *context, bool irq, bool drq);

// Soft power-off: stop the scheduler so the headless run exits cleanly.
// The RBV's RvPowerOff falling edge, and on the IIsi Egret's PwrDown too.
void mdu_power_off(void *context);

// The RBV's checkpoint part-save function (machine_parts.h).
void part_save_rbv(void *obj, checkpoint_t *cp);

#endif // GS_MACHINES_MDU_MDU_H
