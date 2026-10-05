// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// slot_tables.c
// See slot_tables.h for why these live in one place.

#include "slot_tables.h"
#include "scsi.h"

const struct floppy_slot mac_floppy_slots_ext[] = {
    {.label = "Internal floppy drive", .kind = FLOPPY_HD},
    {.label = "External floppy drive", .kind = FLOPPY_HD, .optional = true, .default_none = true},
    {0},
};

const struct floppy_slot mac_floppy_slots_2int[] = {
    {.label = "Internal floppy drive", .kind = FLOPPY_HD},
    {.label = "Second internal floppy drive", .kind = FLOPPY_HD, .optional = true, .default_none = true},
    {0},
};

const struct floppy_slot mac_floppy_slots_1hd[] = {
    {.label = "Internal floppy drive", .kind = FLOPPY_HD},
    {0},
};

const storage_bay_decl_t mac_bays_hd0[] = {
    {.unit = 0, .label = "Internal hard disk bay"},
    {0},
};

const storage_bay_decl_t mac_bays_hd0_cd3[] = {
    {.unit = 0, .label = "Internal hard disk bay"},
    {.unit = 3, .label = "CD-ROM bay"},
    {0},
};

const storage_bus_decl_t mac_storage_scsi_hd_bay[] = {
    MAC_SCSI_BUS("scsi", "SCSI", MEDIA_BUS_SCSI, mac_bays_hd0, true),
    {0},
};

const storage_bus_decl_t mac_storage_scsi_cd_bay[] = {
    MAC_SCSI_BUS("scsi", "SCSI", MEDIA_BUS_SCSI, mac_bays_hd0_cd3, true),
    {0},
};

const storage_device_decl_t mac_default_storage_hd0_cd3[] = {
    {.bus = "scsi", .unit = 0, .type = STORAGE_DEV_HD},
    {.bus = "scsi", .unit = 3, .type = STORAGE_DEV_CD},
    {0},
};

const struct scsi_cd_drive mac_cdrom_drive_applecd = {
    .vendor = "SONY",
    .product = "CD-ROM CDU-8002",
    .revision = "1.8g",
    .block_size = 2048,
};
