// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// slot_irq.h
// The /SLOTIRQ aggregate of the 68030/040 boards with a VIA2 (GLUE: SE/30,
// IIcx, IIx; MCU: Quadra 700/900/950).  Each request line -- the NuBus slots'
// /NMRQ, built-in video, Ethernet -- lands on a VIA2 port-A bit (active-low),
// and the chipset ORs them into the active-low /SLOTIRQ on VIA2 CA1.
//
// The chipset keeps the OR, not the bus: a bus that cannot see every
// contributor (DAFB on PA6, SONIC on PA0, the SE/30's built-in video on PA5)
// cannot compute the edge.  /SLOTIRQ is a LEVEL: "routed through an OR gate
// ... connected to the CA1 input of VIA2" (Quadra 700 and 900 developer
// notes).  The family owns the mask; these functions keep it and the VIA2
// lines in step.

#ifndef GS_MACHINES_RUNTIME_SLOT_IRQ_H
#define GS_MACHINES_RUNTIME_SLOT_IRQ_H

#include <stdbool.h>
#include <stdint.h>

struct via;

// The VIA2 port-A bits that can carry a /SLOTIRQ source (PA0-PA6)
#define SLOT_IRQ_SOURCES 0x7Fu

// One source of the aggregate changes state: update `*mask`, drive the
// source's port-A line (active-low) and CA1 from the OR of every source.
// A `pa_bit` outside PA0-PA6 is ignored.
void slot_irq_source(struct via *via2, uint8_t *mask, int pa_bit, bool active);

// Drive every source line and CA1 from `mask` -- a restore that read the
// mask back re-drives the VIA2 lines from it.
void slot_irq_redrive(struct via *via2, uint8_t mask);

// The aggregate mask the restored VIA2 port-A input levels imply: a source is
// active while its line is low.  A family that does not save its mask
// derives it on a restore (the pins are part of the VIA2 checkpoint).
uint8_t slot_irq_mask_from_via(const struct via *via2);

#endif // GS_MACHINES_RUNTIME_SLOT_IRQ_H
