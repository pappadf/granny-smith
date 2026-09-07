// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_teardown.h
// The one config_t-owned teardown chain, shared by every machine family.
//
// Before this existed, seven families each carried a hand-written copy of it
// (the 2026-09-03 review's F-27).  Measured with comments and whitespace
// stripped, four of the five 68k copies differed from the GLUE one only in
// whether they had a VIA2 -- which the NULL guard below already covers -- and
// the MCU's was byte-identical.  A change to the chain had to be made seven
// times or it was made inconsistently.

#ifndef GS_MACHINES_RUNTIME_MACHINE_TEARDOWN_H
#define GS_MACHINES_RUNTIME_MACHINE_TEARDOWN_H

struct config;

// Free every config_t-owned device, in the one canonical order:
//
//   scsi -> via2 -> via1 -> appletalk -> scc -> rtc -> scheduler ->
//   cpu | ppc -> mem_map -> debugger
//
// Any NULL handle is skipped, so a machine with one VIA or no SCSI passes
// straight through those steps, and the 68k/PowerPC split falls out of which
// of cfg->cpu / cfg->ppc the family built.
//
// Two orderings in here are load-bearing, and are why this is one function
// rather than a per-family list: the AppleTalk stack holds the SCC pointer it
// was given at init, so it goes before scc_delete; and every device that holds
// a VIA must already be gone by the time the VIAs are freed.
//
// Call it AFTER freeing whatever lives in the machine's private state, and
// free that state afterwards.  Every family's teardown is therefore:
//
//   scheduler_stop -> the family's own devices -> this -> free(st)
//
// Note that scheduler_stop() comes first, so no device callback can fire
// while the chain runs; the only hazard ordering guards against is one
// delete reading through a pointer into an already-freed object.
void machine_teardown_config_devices(struct config *cfg);

#endif // GS_MACHINES_RUNTIME_MACHINE_TEARDOWN_H
