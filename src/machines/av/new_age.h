// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// new_age.h
// "New Age" floppy controller stub (NEC µPD72070 in Apple mode) — the
// documented quickest no-floppy path: the PIO command/result handshakes are
// modelled exactly (they hang the '.NewAge' driver otherwise), every drive
// reports ST3 = $FF ("no drive"), and no media path exists.  Contract:
// the AV New Age hardware notes §3/§5 — including the <SM23>
// silicon deviation the driver depends on (Command Busy transiently SET
// after an interrupt, so SenseInterrupt's wait-for-CB-set terminates).
//
// Register surface (island $2A000): $101 = MSR read / DRR write,
// $141 = FIFO (command bytes in, result bytes out).  Interrupts latch
// PSC-VIA2 IFR bit 5 after every interrupting command completion.

#ifndef GS_MACHINES_AV_NEW_AGE_H
#define GS_MACHINES_AV_NEW_AGE_H

#include "system_config.h"

#include <stdint.h>

struct av_new_age;
typedef struct av_new_age av_new_age_t;

// === Lifecycle ==============================================================

av_new_age_t *av_new_age_init(config_t *cfg, checkpoint_t *cp);
void av_new_age_delete(av_new_age_t *fdc);
void av_new_age_checkpoint(av_new_age_t *fdc, checkpoint_t *cp);

// === I/O island handlers ====================================================

uint8_t av_new_age_read(config_t *cfg, uint32_t addr);
void av_new_age_write(config_t *cfg, uint32_t addr, uint8_t value);

#endif // GS_MACHINES_AV_NEW_AGE_H
