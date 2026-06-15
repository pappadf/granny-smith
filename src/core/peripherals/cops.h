// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cops.h
// Apple Lisa COPS microcontroller (National COP421-class) — keyboard, mouse,
// real-time clock, and soft-power, reached through VIA1 port A.  See
// docs/lisa.md §11 and proposal-machine-lisa-xl.md §4.6.
//
// Host interface (verified against the rev-H boot ROM, RM248.K.TEXT):
//  * Command path (COPSCMD): the host writes a command byte to VIA1 port A,
//    polls VIA1 PB6 = CRDY for a ready→took-data toggle, and drives DDRA=$FF to
//    jam the byte.  The COPS holds CRDY low (ready) at idle and raises it when
//    it accepts the jammed byte.
//  * Response path (GETDATA): the COPS drives port A with a response byte and
//    pulses CA1 (sets VIA1 IFR bit 1); the host polls IFR bit 1 then reads ORA.
//  * Reset: the host pulses VIA1 PB0 low (RSTKBD) then high (CLRRST); the
//    low→high edge makes the COPS emit its $80-lead-in reset/id codes.

#ifndef COPS_H
#define COPS_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>

struct via;
struct scheduler;
typedef struct cops cops_t;

// === Lifecycle =============================================================

// Create the COPS attached to VIA1.  Drives CRDY (PB6) low (ready) immediately.
cops_t *cops_init(struct via *via1, struct scheduler *scheduler, checkpoint_t *cp);
void cops_delete(cops_t *cops);
void cops_checkpoint(cops_t *cops, checkpoint_t *cp);

// === VIA1 hookup ============================================================

// Called from VIA1's output callback on every port change.  port 0 = A
// (command jam, gated on DDRA), port 1 = B (PB0 reset line).  value is the
// driven output (ORx & DDRx).
void cops_via_output(cops_t *cops, uint8_t port, uint8_t value);

#endif // COPS_H
