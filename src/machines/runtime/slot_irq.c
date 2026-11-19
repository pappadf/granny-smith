// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// slot_irq.c
// The VIA2 /SLOTIRQ aggregate shared by the GLUE and MCU families.  See the
// header.

#include "slot_irq.h"

#include "via.h"

void slot_irq_source(via_t *via2, uint8_t *mask, int pa_bit, bool active) {
    if (pa_bit < 0 || pa_bit > 6)
        return;
    uint8_t bit = (uint8_t)(1u << pa_bit);
    *mask = active ? (*mask | bit) : (*mask & (uint8_t)~bit);
    via_input(via2, /*port A*/ 0, pa_bit, active ? 0 : 1); // active-low line
    via_input_c(via2, /*CA1*/ 0, 0, *mask ? 0 : 1); // /SLOTIRQ = OR of sources
}

void slot_irq_redrive(via_t *via2, uint8_t mask) {
    // Sources 0-6; the build parked them at the inactive level
    for (int bit = 0; bit <= 6; bit++)
        via_input(via2, 0, bit, (mask >> bit) & 1 ? 0 : 1);
    via_input_c(via2, 0, 0, mask ? 0 : 1);
}

uint8_t slot_irq_mask_from_via(const via_t *via2) {
    // Active-low lines: a low pin is a requesting source
    return (uint8_t)(~via_port_input(via2, 0) & SLOT_IRQ_SOURCES);
}
