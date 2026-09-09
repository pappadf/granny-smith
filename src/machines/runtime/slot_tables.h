// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// slot_tables.h
// The floppy and SCSI slot tables that more than one machine family declares
// identically.
//
// These were 27 byte-identical `static const` copies across 15 files (the
// 2026-09-03 review's F-31), which is not merely repetition: a new machine got
// its tables by hand-copying them from a neighbour, and two review findings --
// F-01 (the TNT declared no floppy table at all) and F-17 (the ANS's table
// contradicted its own comment) -- are that copy going wrong.  Referencing a
// table cannot go wrong the same way.
//
// A machine whose slots genuinely differ keeps its own: the Plus and Lisa
// (800K drives), the TNT family (its own "Internal HD0/HD1" pair in tnt.h,
// alongside tnt_floppy_slots), and the Network Servers (backplane bays).
// Sharing is for tables that are the same because the hardware is the same,
// not for making unlike machines look alike.

#ifndef GS_MACHINES_RUNTIME_SLOT_TABLES_H
#define GS_MACHINES_RUNTIME_SLOT_TABLES_H

#include "machine_profile.h"

// Internal FD0 + External FD1, both SuperDrive.  The desktop II-family shape:
// SE/30, IIcx, IIx, IIci, IIsi, IIfx.
extern const struct floppy_slot mac_floppy_slots_2hd[];

// Internal FD0 only, SuperDrive.  Machines with no external floppy port:
// the Quadras and the PDM Power Macs.
extern const struct floppy_slot mac_floppy_slots_1hd[];

// SCSI HD0 (id 0) + HD1 (id 1) -- the generic two-target internal bus every
// 68k Mac and the PDM family present.
extern const struct scsi_slot mac_scsi_slots_hd01[];

#endif // GS_MACHINES_RUNTIME_SLOT_TABLES_H
