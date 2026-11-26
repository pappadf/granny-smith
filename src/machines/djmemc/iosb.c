// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iosb.c
// The IOSB's own configuration registers and its SCSI-DREQ wire into VIA2
// -- see iosb.h.

#include "iosb.h"

#include "checkpoint.h"
#include "log.h"
#include "system.h"

#include <string.h>

LOG_USE_CATEGORY_NAME("board");

// The revision the chip reports in bits 7-5 of $300.  The ROM's own uses of
// the field only distinguish revision 0 from later parts; a shipping board
// carries a later part.
#define IOSB_REVISION 1u

void iosb_init(iosb_t *iosb, bool bus_33mhz) {
    memset(iosb->regs, 0, sizeof iosb->regs);
    iosb->regs[IOSB_REG_CONFIG][0] = bus_33mhz ? 0x00u : 0x01u;
    iosb->regs[IOSB_REG_REV][0] = (uint8_t)(IOSB_REVISION << 5);
}

void iosb_set_dreq(iosb_t *iosb, iosb_dreq_fn fn, void *context) {
    iosb->dreq = fn;
    iosb->dreq_ctx = context;
}

uint8_t iosb_reg_read(iosb_t *iosb, uint32_t win_off) {
    uint32_t reg = (win_off >> 8) & (IOSB_REG_COUNT - 1);
    uint8_t v = iosb->regs[reg][win_off & 1u];
    // The revision bits are the part's, not the register's.
    if (reg == IOSB_REG_REV && (win_off & 1u) == 0)
        v = (uint8_t)((v & 0x1Fu) | (IOSB_REVISION << 5));
    return v;
}

void iosb_reg_write(iosb_t *iosb, uint32_t win_off, uint8_t value) {
    uint32_t reg = (win_off >> 8) & (IOSB_REG_COUNT - 1);
    iosb->regs[reg][win_off & 1u] = value;
    LOG(2, "IOSB $%03X = $%02X", win_off & 0x7FFu, value);
}

uint8_t iosb_via2_ifr(const iosb_t *iosb, uint8_t ifr) {
    bool dreq = iosb->dreq && iosb->dreq(iosb->dreq_ctx);
    return (uint8_t)((ifr & ~0x01u) | (dreq ? 0x01u : 0x00u));
}

void iosb_checkpoint(const iosb_t *iosb, checkpoint_t *cp) {
    system_write_checkpoint_data(cp, iosb->regs, sizeof iosb->regs, "iosb.regs");
}

void iosb_restore(iosb_t *iosb, checkpoint_t *cp) {
    system_read_checkpoint_data(cp, iosb->regs, sizeof iosb->regs, "iosb.regs");
}
