// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// slot_tables.c
// See slot_tables.h for why these live in one place.

#include "slot_tables.h"

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
