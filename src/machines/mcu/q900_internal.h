// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// q900_internal.h
// Shared tower (Quadra 900/950) board hooks, defined in q900.c and reused
// by q950.c — the Q950 is the same Eclipse board architecture at 33 MHz
// with the DAFB 3 / AC842a video revision and its own ROM + model sense.
// Per-machine facts flow through mcu_board_desc_t (via1_pa_model,
// dafb_version, has_ac842a) and hw_profile_t.freq (the VIA2 PB5 speed
// sense strap: 0 = 25 MHz, 1 = 33 MHz — HardwarePrivateEqu.a v2Speed).

#ifndef GS_MACHINES_MCU_Q900_INTERNAL_H
#define GS_MACHINES_MCU_Q900_INTERNAL_H

#include "mcu.h"

// VIA hooks (Caboose handshake on VIA1 PB + SR; sound-input selects on VIA2).
void q900_via1_output(void *context, uint8_t port, uint8_t output);
void q900_via1_shift_out(void *context, uint8_t byte);
void q900_via2_output(void *context, uint8_t port, uint8_t output);

// SCC chip INT (ORs with the SCC IOP host INT onto the level-4 source).
void q900_scc_irq(void *context, bool active);

// The full tower device build (Caboose, IOPs, dual 53C96, DAFB, SONIC, …).
int q900_build_devices(config_t *cfg, checkpoint_t *cp);

#endif // GS_MACHINES_MCU_Q900_INTERNAL_H
