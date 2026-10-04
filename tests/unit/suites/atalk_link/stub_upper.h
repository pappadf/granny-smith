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

// The printer's hooks (appletalk_printer.c), counted: registered once with the
// network, its timers put on each machine plugged in, its session dropped
// with each machine unplugged.
extern int g_printer_registers;
extern int g_printer_timer_registrations;
extern int g_printer_unplugs;

#endif // STUB_UPPER_H
