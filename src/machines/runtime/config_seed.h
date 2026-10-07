// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// config_seed.h
// The seeding step's record writers (machine_substrate_t.seed): the
// parameter-memory records that follow from a configuration, written once
// when a new machine is built -- never on a restore, a reset or a power
// cycle, after which the store is the guest's.
//
// Two stores carry the same 256-byte Mac OS PRAM image: the RTC's (68k Macs,
// the PDM Power Macs) and the XPRAM partition of an Open Firmware machine's
// NVRAM.  The records are the same bytes in both; only where they are written
// differs.

#ifndef GS_MACHINES_RUNTIME_CONFIG_SEED_H
#define GS_MACHINES_RUNTIME_CONFIG_SEED_H

#include <stdbool.h>
#include <stdint.h>

#include "rtc.h"

struct config;

// The seed hook of every machine whose Mac OS PRAM is the RTC's: the default
// startup device (Start Manager record, SCSI driver refnum), SysParam (valid,
// with AppleTalk's on/off state as port B use and the mouse tracking slowed
// to no acceleration), the addressing mode (MMFlags bit 0) and each NuBus
// card's startup video mode (its slot record).
void mac_seed_rtc_pram(struct config *cfg);

// Write the same SysParam (AppleTalk record, mouse tracking) into a 256-byte
// XPRAM image (an Open Firmware machine's NVRAM partition), around the
// SysParam of `pram` (the family's measured defaults).  The startup device
// there is the family's own of_nvram_set_startup_scsi.
void mac_seed_xpram_sysparam(uint8_t xpram[256], const struct config *cfg, const pram_defaults_t *pram);

// The startup SCSI ID the configuration names on bus `bus_id`, -1 for "no
// default" (or a device on another bus), -2 when the configuration says
// nothing (keep the factory record).
int mac_seed_startup_scsi_id(const struct config *cfg, const char *bus_id);

#endif // GS_MACHINES_RUNTIME_CONFIG_SEED_H
