// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_config_decl.h
// The declaration types a machine profile describes its configuration space
// with: scalar options, floppy positions, storage buses and their bays, the
// default devices, and the built-in display.  catalog.profile publishes them
// as the machine-description tree; machine.boot's configuration document is
// checked against them.  Data only -- the profile owns every instance.

#ifndef GS_CORE_MACHINE_CONFIG_DECL_H
#define GS_CORE_MACHINE_CONFIG_DECL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// === Scalar options ========================================================

// One value an option may take.  `id` is what the document carries; the
// array ends at the entry whose id is NULL.
typedef struct config_value_decl {
    const char *id; // "active"
    const char *label; // "Active"
    const char *detail; // optional secondary text
} config_value_decl_t;

// One scalar machine option (memory, AppleTalk, power supplies, keyswitch, …).
// `requires_*` is the one cross-option constraint the space needs today: the
// value `requires_value` of this option is valid only while option
// `requires_option` is not `requires_not`.
typedef struct config_option_decl {
    const char *id; // "appletalk"; NULL ends an array
    const char *label; // "AppleTalk"
    const char *detail;
    const config_value_decl_t *values;
    const char *default_value; // one of values[].id
    const char *requires_value;
    const char *requires_option;
    const char *requires_not;
} config_option_decl_t;

// === Storage ===============================================================

// What a storage bus is: SCSI (narrow 0-7 or wide 0-15), ATA (master/slave)
// or the Lisa's parallel ProFile port (one unit).
typedef enum storage_kind {
    STORAGE_KIND_SCSI = 0,
    STORAGE_KIND_ATA,
    STORAGE_KIND_PROFILE,
} storage_kind_t;

// The device types a bus accepts (a bit mask) and a device has (one bit).
#define STORAGE_DEV_HD 0x1u
#define STORAGE_DEV_CD 0x2u

// A unit that is a physical internal position, with Apple's name for it.  The
// array ends at the entry whose label is NULL.
typedef struct storage_bay_decl {
    int unit;
    const char *label; // "Internal hard disk bay", "Front bay 2", "CD-ROM bay"
} storage_bay_decl_t;

// One storage bus.  Devices are not here: the document says which exist, the
// bus says where they may go.  The array ends at the entry whose id is NULL.
typedef struct storage_bus_decl {
    const char *id; // object name: "scsi", "scsi2", "ata0", "ata1", "profile"
    const char *label; // "SCSI", "Internal SCSI", "Primary ATA bus", "ProFile port"
    const char *detail; // "Symbios 53C825A", "MESH" (optional)
    storage_kind_t kind;
    bool wide; // SCSI: 16 IDs instead of 8
    // The media bus the attach verbs reach this bus through
    // (media_bus_t, machine_profile.h), and the offset its units have there:
    // the beige G3's ATA buses are one media bus, cell * 2 + device.
    int media_bus;
    int media_unit_base;
    uint32_t units; // bit n: unit n exists
    uint32_t reserved; // bit n: unit n is the initiator / a controller
    const char *shares_units_with; // another bus id in the same ID space, or NULL
    bool external_connector; // units that are not bays read "External"
    const storage_bay_decl_t *bays; // NULL: none
    unsigned accepts; // STORAGE_DEV_* mask
    // The bus can name its devices in the family's startup-device record
    // (validation V9).  False: `startup` cannot point at a device here.
    bool startup_ok;
} storage_bus_decl_t;

// One default storage device: type at (bus, unit).  The array ends at the
// entry whose bus is NULL.  The first hard disk is the default startup device.
typedef struct storage_device_decl {
    const char *bus;
    int unit;
    unsigned type; // STORAGE_DEV_HD or STORAGE_DEV_CD
} storage_device_decl_t;

#endif // GS_CORE_MACHINE_CONFIG_DECL_H
