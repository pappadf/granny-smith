// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// stub_upper.h
// Recorders exposed by stub_upper.c.

#ifndef STUB_UPPER_H
#define STUB_UPPER_H

#include <stdint.h>

extern int g_afp_calls;
extern uint8_t g_afp_last_opcode;
extern uint16_t g_afp_last_session;
extern int g_asp_closes;
extern int g_adsp_in_calls;
extern int g_adsp_in_last_len;
extern int g_aevt_set_calls;

// The printer (laserwriter_job.c), modelled: the id in service (0 = none)
// and the ids freed so far, in order.  stub_printer_use puts one in service
// the way a job does.
extern uint32_t g_printer_current;
extern uint32_t g_printer_freed[16];
extern int g_printer_nfreed;
uint32_t stub_printer_use(void);
void stub_printer_reset(void);

#endif // STUB_UPPER_H
