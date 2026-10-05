// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// slot_tables.h
// The floppy and storage declarations that more than one machine declares
// identically.
//
// These were 27 byte-identical `static const` copies across 15 files, which is
// not merely repetition: a new machine got its tables by hand-copying them from
// a neighbour, and two known bugs -- the TNT declared no floppy table at all,
// and the ANS's table contradicted its own comment -- are that copy going
// wrong.  Referencing a table cannot go wrong the same way.
//
// A shared table must be TRUE on every machine that uses it: a label that is
// right on one machine and wrong on another (the IIx's second drive is not
// "External") means the second machine needs its own table.  A machine whose
// hardware genuinely differs keeps its own: the Plus (no internal bay), the
// AV, PDM and TNT families (CD-ROM bays), the Network Servers (backplanes).

#ifndef GS_MACHINES_RUNTIME_SLOT_TABLES_H
#define GS_MACHINES_RUNTIME_SLOT_TABLES_H

#include "machine_profile.h"

// Internal + External floppy drive, both SuperDrive; the external port ships
// without a drive.  The IIcx, IIci and IIsi.
extern const struct floppy_slot mac_floppy_slots_ext[];

// Internal + Second internal floppy drive, both SuperDrive; the second bay
// ships empty.  The IIx and IIfx, which have two internal bays and no
// external port.
extern const struct floppy_slot mac_floppy_slots_2int[];

// Internal floppy drive only, SuperDrive.  Machines with no external floppy
// port: the Quadras, the PDM, TNT and G3 Power Macs, the Network Servers.
extern const struct floppy_slot mac_floppy_slots_1hd[];

// A narrow SCSI bus with the one bay every 68k Mac desktop names, the
// internal hard disk bay at ID 0, and an external connector: the SE/30, the
// Macintosh II family and the Quadra 700.
extern const storage_bus_decl_t mac_storage_scsi_hd_bay[];

// The same with a CD-ROM bay at ID 3: the AV Quadras and the Power Macintosh
// 6100 (whose CD-ROM drive sits in the front 5.25" bay).
extern const storage_bus_decl_t mac_storage_scsi_cd_bay[];

// The default devices of such a machine: a hard disk at ID 0 and an external
// CD-ROM drive at ID 3.
extern const storage_device_decl_t mac_default_storage_hd0_cd3[];

// The bay names Apple's documentation gives, shared by several tables.
extern const storage_bay_decl_t mac_bays_hd0[];
extern const storage_bay_decl_t mac_bays_hd0_cd3[];

// The CD-ROM drive every Macintosh profile takes: Apple's AppleCD SC, a SONY
// CD-ROM CDU-8002 presenting 2048-byte blocks (Mode 1 sectors).
extern const struct scsi_cd_drive mac_cdrom_drive_applecd;

// A narrow SCSI bus declaration: units 0-6, the Macintosh at 7, hard disks and
// CD-ROM drives, able to hold the startup device.
#define MAC_SCSI_UNITS    0x7Fu
#define MAC_SCSI_RESERVED 0x80u
#define MAC_SCSI_BUS(id_, label_, media_bus_, bays_, external_)                                                        \
    {.id = (id_),                                                                                                      \
     .label = (label_),                                                                                                \
     .kind = STORAGE_KIND_SCSI,                                                                                        \
     .media_bus = (media_bus_),                                                                                        \
     .units = MAC_SCSI_UNITS,                                                                                          \
     .reserved = MAC_SCSI_RESERVED,                                                                                    \
     .external_connector = (external_),                                                                                \
     .bays = (bays_),                                                                                                  \
     .accepts = STORAGE_DEV_HD | STORAGE_DEV_CD,                                                                       \
     .startup_ok = true}

#endif // GS_MACHINES_RUNTIME_SLOT_TABLES_H
