// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rbv.h
// Public interface for the RBV ("RAM-Based Video") chip used by the
// Macintosh IIci (and, in its V8 variant, the IIsi/LC family).  RBV lives
// at physical $50F26000 and subsumes the VIA2 role of the IIcx-family
// machines: it aggregates slot / SCSI / sound interrupts into a single
// 68030 IPL-2 assertion, owns the soft-power-off and cache-control bits,
// and exposes the built-in-video monitor-sense + depth register.  The
// framebuffer itself is owned by the builtin_rbv_video NuBus card; RBV
// only carries the depth/monitor register and the slot-0 video IRQ.
//
// Register names are taken verbatim from Apple's HardwarePrivateEqu.a
// (RBV block, lines 816-916).

#ifndef RBV_H
#define RBV_H

#include "common.h"
#include "memory.h"

#include <stdbool.h>
#include <stdint.h>

struct rbv;
typedef struct rbv rbv_t;

// RBV silicon variant.  Only the IIci's RBV is modelled in v1; the IIsi's
// V8 superset is a future addition selected here.
typedef enum rbv_variant {
    RBV_VARIANT_IICI = 0,
} rbv_variant_t;

// === Lifecycle ==============================================================

// Create an RBV instance, optionally restoring plain-data state from a
// checkpoint.  Does not register itself in the memory map — the machine's
// I/O dispatcher forwards the $50F26000 window to rbv_get_memory_interface().
rbv_t *rbv_init(rbv_variant_t variant, checkpoint_t *cp);
void rbv_delete(rbv_t *rbv);
void rbv_checkpoint(rbv_t *rbv, checkpoint_t *cp);

// === Wiring =================================================================

// Memory-mapped register interface (offsets relative to the RBV base).
const memory_interface_t *rbv_get_memory_interface(rbv_t *rbv);

// Combined-interrupt callback — fires whenever RBV's aggregated interrupt
// state changes.  The machine routes this to its IPL-2 source.
void rbv_set_irq_callback(rbv_t *rbv, void (*cb)(void *ctx, bool active), void *ctx);

// Soft-power-off callback — fires on the RvPowerOff (RvDataB bit 2) 1→0 edge.
void rbv_set_power_off_callback(rbv_t *rbv, void (*cb)(void *ctx), void *ctx);

// Video depth-change callback — fires when the RvMonP depth field (bits 0-2)
// changes.  `depth_code`: 0 = 1 bpp, 1 = 2 bpp, 2 = 4 bpp, 3 = 8 bpp.
void rbv_set_mode_callback(rbv_t *rbv, void (*cb)(void *ctx, int depth_code), void *ctx);

// === Interrupt sources ======================================================

// Slot interrupt assert / clear.  slot 0 = built-in video (RvIRQ0), slots
// 1-6 = NuBus.  Gated by RvSEnb before contributing to RvAnySlot / IPL.
void rbv_assert_slot_irq(rbv_t *rbv, int slot);
void rbv_clear_slot_irq(rbv_t *rbv, int slot);

// SCSI IRQ (RvSCSIRQ) and DRQ (RvSCSIDRQ) level inputs from the 5380 model.
void rbv_set_scsi_irq(rbv_t *rbv, bool active);
void rbv_set_scsi_drq(rbv_t *rbv, bool active);

// Apple Sound Chip IRQ (RvSndIRQ) level input.  Unwired on the IIci in v1
// (matches the IIfx, whose ASC has no IRQ callback), kept for completeness.
void rbv_set_snd_irq(rbv_t *rbv, bool active);

// === Configuration ==========================================================

// Set the 3-bit monitor-sense code reported in RvMonP bits 3-5 (RvMonID1-3).
// 6 (binary 110) = Macintosh II 13" RGB — the v1 default.
void rbv_set_monitor_sense(rbv_t *rbv, uint8_t sense3);

// Current video depth code (0..3) latched in RvMonP bits 0-2.
int rbv_current_depth(rbv_t *rbv);

#endif // RBV_H
