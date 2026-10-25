// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// watch.h
// The machine watch: a periodic status line on the console that says what
// the machine is doing right now, and detectors that raise an alert
// (log.h, LOG_ALERT) when it looks stuck -- a Mac OS system error, the
// guest's tick counter frozen while the CPU runs, the CPU not advancing.
// The platform calls watch_poll from its loop; everything else is here.

#ifndef WATCH_H
#define WATCH_H

#include <stdint.h>

// Polls at host wall-clock time `now_ms` (any rate; the work is done at
// most once a second, the status line every watch_get_interval seconds).
void watch_poll(double now_ms);

// Prints the status line now (the `log.status` method).
void watch_report_now(void);

// Seconds between two status lines; 0 turns the line off (the detectors
// stay on).  The `log.status_every` attribute.
uint32_t watch_get_interval(void);
void watch_set_interval(uint32_t seconds);

#endif // WATCH_H
