// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iici_internal.h
// Private state and equates for the Macintosh IIci ("Aurora") machine.
// The IIci runs the MDU-decoded I/O island at the same canonical
// $50F0xxxx addresses as the IIcx GLUE, but replaces VIA2 with the RBV
// chip ($50F26000) and drives built-in video (framebuffer in the slot-$B
// aperture) via a Bt450 VDAC at $50F24000.  Mirrors iicx_internal.h in
// shape; a shared mdu/ helper is deferred until a second MDU machine
// (IIsi) arrives, matching the IIcx "ship inline, extract on the 2nd
// caller" stance.

#ifndef IICI_INTERNAL_H
#define IICI_INTERNAL_H

#include "common.h"
#include "memory.h"
#include "mmu.h"
#include "system_config.h" // for config_t.machine_context
#include <stdbool.h>
#include <stdint.h>

struct adb;
struct asc;
struct floppy;
struct rbv;
struct nubus_card;

// Per-machine state.
typedef struct iici_state {
    struct adb *adb;
    struct asc *asc;
    struct floppy *floppy;
    struct rbv *rbv; // RBV chip (VIA2 replacement + video control)
    struct nubus_card *video_card; // built-in RBV video pseudo-card (slot 0)

    bool rom_overlay;
    mmu_state_t *mmu;

    const memory_interface_t *via1_iface;
    const memory_interface_t *scc_iface;
    const memory_interface_t *scsi_iface;
    const memory_interface_t *asc_iface;
    const memory_interface_t *floppy_iface;
    const memory_interface_t *rbv_iface;

    uint8_t last_port_b; // ADB ST filtering on VIA1 PB

    memory_interface_t io_interface;
} iici_state_t;

static inline iici_state_t *iici_state(config_t *cfg) {
    return (iici_state_t *)cfg->machine_context;
}

// IRQ source bit assignments (RBV at level 2 replaces the IIcx VIA2).
#define IICI_IRQ_VIA1 (1 << 0)
#define IICI_IRQ_RBV  (1 << 1)
#define IICI_IRQ_SCC  (1 << 2)
#define IICI_IRQ_NMI  (1 << 3)

// I/O bus penalty equates (match the IIcx dispatcher).
#define IICI_VIA_IO_PENALTY  16
#define IICI_SCC_IO_PENALTY  2
#define IICI_SCSI_IO_PENALTY 2
#define IICI_ASC_IO_PENALTY  2
#define IICI_SWIM_IO_PENALTY 2
#define IICI_RBV_IO_PENALTY  2
#define IICI_VDAC_IO_PENALTY 2

// Address-space constants.  The IIci ROM lives at $40800000 (MDUtable;
// confirmed by the reset PC $4080002A), NOT the IIcx's $40000000.  The
// I/O island shares the IIcx base/size; the mirror mask is widened to
// $3FFFF so RBV ($26000) and VDAC ($24000) decode distinctly from the
// SCSI windows (the IIcx's $1FFFF mask would fold $26000 onto $6000).
#define IICI_ROM_START 0x40800000UL
#define IICI_ROM_END   0x41000000UL
#define IICI_IO_BASE   0x50000000UL
#define IICI_IO_SIZE   0x10000000UL
#define IICI_IO_MIRROR 0x0003FFFFUL

#endif // IICI_INTERNAL_H
