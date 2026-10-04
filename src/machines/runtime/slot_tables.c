// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// slot_tables.c
// See slot_tables.h for why these live in one place.

#include "slot_tables.h"
#include "scsi.h"

const struct floppy_slot mac_floppy_slots_2hd[] = {
    {.label = "Internal FD0", .kind = FLOPPY_HD},
    {.label = "External FD1", .kind = FLOPPY_HD},
    {0},
};

const struct floppy_slot mac_floppy_slots_1hd[] = {
    {.label = "Internal FD0", .kind = FLOPPY_HD},
    {0},
};

const struct scsi_slot mac_scsi_slots_hd01[] = {
    {.label = "SCSI HD0", .id = 0},
    {.label = "SCSI HD1", .id = 1},
    {0},
};

const struct scsi_slot mac_scsi_slots_ext01[] = {
    {.label = "External SCSI 0", .id = 0},
    {.label = "External SCSI 1", .id = 1},
    {0},
};

const struct scsi_cd_drive mac_cdrom_drive_applecd = {
    .vendor = "SONY",
    .product = "CD-ROM CDU-8002",
    .revision = "1.8g",
    .block_size = 2048,
};
