// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pram_defaults.h
// Each ROM family's power-up PRAM (rtc.h pram_defaults_t): what a machine's
// PRAM holds at construction, before any code runs.  One table per family,
// from measurement -- each ROM booted from all-zero PRAM with no media, and
// the bytes its own cold init wrote read back (pram.md §4, "Measured").

#ifndef PRAM_DEFAULTS_H
#define PRAM_DEFAULTS_H

#include "rtc.h"

extern const pram_defaults_t pram_defaults_plus; // Plus
extern const pram_defaults_t pram_defaults_mac_ii; // II / IIx / IIcx / SE/30 / IIfx / Q700 / Q900 / Q950
extern const pram_defaults_t pram_defaults_iici; // IIci / IIsi: default video device $81 = $80
extern const pram_defaults_t pram_defaults_av; // Q840AV / Q660AV
extern const pram_defaults_t pram_defaults_pdm; // 6100 / 7100 / 8100

#endif // PRAM_DEFAULTS_H
